#include "engine/engine.h"
#include "engine/super_block.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <cstring>

namespace cabe {

    Engine::~Engine() {
        if (opened_) {
            CABE_LOG_WARN("Engine 析构时仍处于 Opened 状态，自动 Close");
            Close();
        }
    }

    Status Engine::Open(const Options& opts) {
        if (opened_) return Status::Error(err::kEngineAlreadyOpen);
        if (opts.devices.empty()) return Status::Error(err::kEngineInvalidOpts);
        if (opts.devices.size() > 1) return Status::Error(err::kEngineInvalidOpts);

        options_ = opts;   // P5M3：常驻；组件持 &options_ 现读 wal_level

        for (std::size_t i = 0; i < opts.devices.size(); ++i) {
            const auto& cfg = opts.devices[i];
            DeviceContext dc;

            // 超级块：create 写双份 / recover 读校验（含双向配对 + 设备编号）
            int32_t rc = opts.create
                ? CreateDeviceGroup(cfg, i, &dc.super_block)
                : RecoverDeviceGroup(cfg, i, &dc.super_block);
            if (rc != err::kSuccess) {
                AbortOpen(dc);
                return Status::Error(rc);
            }

            // 打开数据设备的 IoBackend
            rc = dc.io.Open(cfg.data_path, &options_);
            if (rc != err::kSuccess) {
                AbortOpen(dc);
                return Status::Error(rc);
            }

            // 兜底：超级块持久 block_count 不应超过 IoBackend 实测可寻址块数（recover 已核对，
            // 此处再防御任何绕过 RecoverDeviceGroup 的路径或现算/持久值漂移）。
            if (dc.super_block.block_count > dc.io.BlockCount()) {
                CABE_LOG_ERROR("block_count 不一致: 超级块=%llu > 设备=%llu",
                               static_cast<unsigned long long>(dc.super_block.block_count),
                               static_cast<unsigned long long>(dc.io.BlockCount()));
                AbortOpen(dc);
                return Status::Error(err::kSuperBlockSizeMismatch);
            }

            dc.pool = BufferPool(kDefaultPoolBlocks);
            // 用超级块记录的 block_count（数据区块数，权威值）；逻辑 block 从 0，
            // 物理偏移由 IoBackend 加 kDataRegionOffset
            // TODO(P7/多设备): BlockId 的 device 位此处硬编码为 0，而 super_block.device_id=i；
            //   多设备启用后应改为 static_cast<DeviceId>(i) 并与 RouteKey 路由对齐。
            dc.block_allocator.Init(0, dc.super_block.block_count);

            // P5M2/M4：仅 create 模式打开 WAL + 快照设备（D9）；recover 的重放/加载在 M6。
            if (opts.create) {
                rc = dc.wal.Open(cfg.wal_path, &options_);
                if (rc != err::kSuccess) {
                    AbortOpen(dc);
                    return Status::Error(rc);
                }

                // P5M4：打开快照设备——内含 A/B 布局计算 + 部署期快照设备容量校验
                //   + 清空两槽、next_gen=1；容量不足返回 kDeviceTooSmall。
                //   注：WAL 设备容量校验推迟 M5（M4 测试设备小、WAL 线性不回收，过早校验会
                //   拒绝小 loop 设备；见 P5M4 §11 备注）。
                rc = dc.snapshot.Open(cfg.snapshot_path, &options_, dc.super_block);
                if (rc != err::kSuccess) {
                    AbortOpen(dc);
                    return Status::Error(rc);
                }
            }

            devices_.push_back(std::move(dc));
        }

        opened_ = true;
        CABE_LOG_INFO("Engine::Open 成功: %zu 个设备", opts.devices.size());
        return Status::Ok();
    }

    Status Engine::Close() {
        if (!opened_) return Status::Error(err::kEngineNotOpen);

        // 关闭每个设备：先 wal（含攒批档收尾 Flush）再 io；记下第一个错误并向上传播，
        // 但仍关完所有设备（避免 fd 泄漏）。级别 2/4 收尾刷盘失败必须让调用方知道。
        int32_t first_err = err::kSuccess;
        for (auto& dc : devices_) {
            int32_t src = dc.snapshot.Close();
            int32_t wrc = dc.wal.Close();
            int32_t irc = dc.io.Close();
            if (first_err == err::kSuccess)
                first_err = (src != err::kSuccess) ? src : (wrc != err::kSuccess) ? wrc : irc;
        }
        devices_.clear();
        opened_ = false;
        CABE_LOG_INFO("Engine::Close 完成");
        return first_err != err::kSuccess ? Status::Error(first_err) : Status::Ok();
    }

    Status Engine::Put(std::string_view key, DataView value) {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        if (key.empty()) return Status::Error(err::kMemEmptyKey);
        if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);

        if (key.size() > kWalKeyMax) return Status::Error(err::kWalKeyTooLong);

        auto& dc = devices_[RouteKey(key)];

        // 申请新块（不先回收旧块——旧块在提交成功后才回收，覆盖安全；见 P5M2 §7.3）
        BlockId block_id{};
        int32_t rc = dc.block_allocator.Acquire(&block_id);
        if (rc != err::kSuccess) return Status::Error(rc);

        std::byte* buf = dc.pool.Allocate();
        if (!buf) {
            dc.block_allocator.Recycle(block_id);
            return Status::Error(err::kEnginePoolExhausted);
        }
        std::memcpy(buf, value.data(), kValueSize);
        const std::uint32_t value_crc = util::CRC32(value);
        const std::uint64_t now = util::GetWallTimeNs();

        // 按级别（io.Write 现读 wal_level）：1/2 value FUA、3/4 异步；写在 WAL 之前
        rc = dc.io.Write(block_id.block_idx(), buf);
        dc.pool.Free(buf);
        if (rc != err::kSuccess) {
            dc.block_allocator.Recycle(block_id);
            return Status::Error(rc);
        }

        // 按级别（wal.WriteWal 现读 wal_level）：1/3 同步落盘、2/4 攒批；预写日志，写在内存索引之前
        rc = dc.wal.WriteWal(WalEntry{WalEntryType::Put, key, block_id, value_crc, now});
        if (rc != err::kSuccess) {
            dc.block_allocator.Recycle(block_id);
            return Status::Error(rc);
        }

        // 提交到内存索引（WAL 落盘成功之后）
        ValueMeta old_meta{};
        const bool had_old = (dc.meta_index.Lookup(key, &old_meta) == err::kSuccess);
        ValueMeta meta{};
        meta.block     = block_id;
        meta.timestamp = now;
        meta.crc       = value_crc;
        meta.state     = ValueState::Active;
        dc.meta_index.Insert(key, meta);

        // 提交成功后回收旧块（覆盖写时的前一份 value）
        if (had_old) {
            dc.block_allocator.Recycle(old_meta.block);
            TrimDeviceBlock(dc, old_meta.block);
        }

        // P5M4：写已提交，查大小阈值，到了就（自动、fire-and-forget）触发一份快照。
        MaybeRequestSnapshot(dc);
        return Status::Ok();
    }

    Status Engine::Get(std::string_view key, DataBuffer value) {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        if (key.empty()) return Status::Error(err::kMemEmptyKey);
        if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);

        auto& dc = devices_[RouteKey(key)];

        ValueMeta meta{};
        int32_t rc = dc.meta_index.Lookup(key, &meta);
        if (rc != err::kSuccess) return Status::Error(rc);

        std::byte* buf = dc.pool.Allocate();
        if (!buf) return Status::Error(err::kEnginePoolExhausted);

        rc = dc.io.Read(meta.block.block_idx(), buf);
        if (rc != err::kSuccess) {
            dc.pool.Free(buf);
            return Status::Error(rc);
        }

        std::uint32_t crc_check = util::CRC32(DataView{buf, kValueSize});
        if (crc_check != meta.crc) {
            CABE_LOG_ERROR("CRC32 不匹配: 存储=0x%08X 读出=0x%08X", meta.crc, crc_check);
            dc.pool.Free(buf);
            return Status::Error(err::kEngineDataCorrupted);
        }

        std::memcpy(value.data(), buf, kValueSize);
        dc.pool.Free(buf);
        return Status::Ok();
    }

    Status Engine::Delete(std::string_view key) {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        if (key.empty()) return Status::Error(err::kMemEmptyKey);

        auto& dc = devices_[RouteKey(key)];

        ValueMeta meta{};
        int32_t rc = dc.meta_index.Lookup(key, &meta);
        if (rc != err::kSuccess) return Status::Error(rc);   // 不存在 → 不写 WAL

        // 按级别：1/3 同步落盘、2/4 攒批；墓碑帧写在内存改动之前（预写日志）
        rc = dc.wal.WriteWal(WalEntry{WalEntryType::Delete, key, BlockId{}, 0, util::GetWallTimeNs()});
        if (rc != err::kSuccess) return Status::Error(rc);   // WAL 失败 → 不动内存

        dc.meta_index.Delete(key);
        dc.block_allocator.Recycle(meta.block);
        TrimDeviceBlock(dc, meta.block);

        // P5M4：墓碑写已提交，查大小阈值（Delete 也让 WAL 增长）。
        MaybeRequestSnapshot(dc);
        return Status::Ok();
    }

    Status Engine::SetWalLevel(WalLevel new_level) {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        if (!IsValidWalLevel(new_level)) return Status::Error(err::kEngineInvalidOpts);
        // 注：recover 模式 M3 不开 WAL（仅 create 开），此入口对 WAL 落盘无实际作用——
        //   改级别只动 options_，而 recover 下 WAL 通路本就不写（重放/续写留 M6）。
        const WalLevel old = options_.wal_level;
        // 收紧 WAL（攒批档 2/4 → 同步档 1/3）：先把各设备攒批缓冲刷净，新保证才从此刻成立。
        const bool tighten_wal = !IsWalSyncLevel(old) && IsWalSyncLevel(new_level);
        if (tighten_wal) {
            for (auto& dc : devices_) {
                int32_t rc = dc.wal.Flush();
                if (rc != err::kSuccess) return Status::Error(rc);
            }
        }
        options_.wal_level = new_level;   // 组件下次操作自然现读到
        return Status::Ok();
    }

    bool Engine::is_open() const noexcept { return opened_; }

    std::size_t Engine::RouteKey(std::string_view key) const noexcept {
        (void)key;
        return 0;
    }

    void Engine::TrimDeviceBlock(DeviceContext& dc, BlockId id) {
        // TODO(P7): 通过待 TRIM 队列异步批量发送 BLKDISCARD
        (void)dc;
        (void)id;
    }

    void Engine::AbortOpen(DeviceContext& dc) {
        // Open 失败路径统一清理：先关当前还未入列的局部 dc（已打开的句柄不留给析构兜底），
        // 再关已入列设备、清空列表。各 Close 对未打开句柄都是安全空操作；
        // 错误码不在此收集——Open 向上返回的是原始失败码。
        dc.snapshot.Close();
        dc.wal.Close();
        dc.io.Close();
        for (auto& d : devices_) { d.snapshot.Close(); d.wal.Close(); d.io.Close(); }
        devices_.clear();
    }

    // ---- P5M4：快照触发链 ----

    Status Engine::Snapshot() {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        // 手动触发：同步执行、逐设备各做一份、返回第一个错误。
        int32_t first_err = err::kSuccess;
        for (auto& dc : devices_) {
            // M4 仅 create 模式打开快照设备；recover 模式的快照/恢复编排在 M6——
            // 明确报"未实现"，而不是落进 Write 的未打开守卫、报误导性的"写快照失败"。
            // （自动路径 MaybeRequestSnapshot 有同款守卫，两条路保持对称。）
            if (!dc.snapshot.is_open()) {
                if (first_err == err::kSuccess) first_err = err::kEngineNotImplemented;
                continue;
            }
            int32_t rc = DoSnapshot(dc);
            if (first_err == err::kSuccess) first_err = rc;
        }
        return first_err != err::kSuccess ? Status::Error(first_err) : Status::Ok();
    }

    int32_t Engine::DoSnapshot(DeviceContext& dc) {
        // 一次快照尝试从这里开始：先推进退避基准（成败都算，设计 §10.3）。记账放在尝试的
        // 起点而非 Snapshot::Write 内，保证"还没到 Write 就失败"的路径（如下面刷 WAL 失败）
        // 也被记账——否则攒批档（2/4）+ WAL 设备故障时，每次写都会重试一遍注定失败的刷盘。
        dc.snapshot.NoteTriggerAttempt(dc.wal.last_seq());
        // 先刷 WAL（默认级别 3 是空操作）→ 取 covered_seq → 驱动遍历写快照。
        int32_t rc = dc.wal.Flush();
        if (rc != err::kSuccess) return rc;
        const std::uint64_t covered_seq = dc.wal.last_seq();
        return dc.snapshot.Write(covered_seq, [&](const MetaIndexVisitor& v) {
            return dc.meta_index.ForEach(v);   // 回调驱动一致扫描；可中止错误一路传出
        });
    }

    void Engine::RequestSnapshot(DeviceContext& dc) {
        // 自动触发汇总入口：fire-and-forget。M4 同步执行；P7 改为唤醒后台快照线程。
        // 快照失败不连累本次写，只记日志（退避基准已在 DoSnapshot 入口推进）。
        int32_t rc = DoSnapshot(dc);
        if (rc != err::kSuccess) {
            CABE_LOG_ERROR("自动触发的快照失败: rc=%d（不影响本次写，后续按退避重试）", rc);
        }
    }

    void Engine::MaybeRequestSnapshot(DeviceContext& dc) {
        if (!dc.snapshot.is_open()) return;   // M4 仅 create 模式开了快照设备
        // 距上次"尝试"以来的 WAL 增长（序号差 × 帧大小）达阈值即触发（退避：基准是 last_trigger_seq）。
        const std::uint64_t grown =
            (dc.wal.last_seq() - dc.snapshot.last_trigger_seq()) * kWalFrameSize;
        if (grown >= options_.snapshot_threshold_bytes) {
            RequestSnapshot(dc);
        }
    }

} // namespace cabe
