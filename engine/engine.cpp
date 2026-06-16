#include "engine/engine.h"
#include "engine/super_block.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <cstring>
#include <vector>

namespace cabe {

    namespace {
        // P5M6：⑨ 步 value CRC 全检的逐键日志上限（百万条坏 value 刷屏等于没有日志；
        // 上限 + 收尾汇总兼顾可读与全貌，设计 D16）。
        constexpr std::uint64_t kRecoveryCrcLogCap = 100;
    } // namespace

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

            // 超级块：create 写双份 / recover 读校验（含双向配对 + 设备编号）——流水线 ①
            int32_t rc = opts.create
                ? CreateDeviceGroup(cfg, i, &dc.super_block)
                : RecoverDeviceGroup(cfg, i, &dc.super_block);
            if (rc != err::kSuccess) {
                AbortOpen(dc);
                return Status::Error(rc);
            }

            // 打开数据设备的 IoBackend——流水线 ②
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

            if (opts.create) {
                // create：开 WAL + 快照设备（空环 / 清槽），索引从空开始。
                rc = dc.wal.Open(cfg.wal_path, &options_);
                if (rc != err::kSuccess) {
                    AbortOpen(dc);
                    return Status::Error(rc);
                }

                rc = dc.snapshot.Open(cfg.snapshot_path, &options_, dc.super_block);
                if (rc != err::kSuccess) {
                    AbortOpen(dc);
                    return Status::Error(rc);
                }
            } else {
                // P5M6 recover：完整恢复链（流水线 ③~⑨）。要么完整恢复要么干净失败（D3）——
                // 任一环错走 AbortOpen + 原始码上抛，绝不交付半份索引。
                rc = RecoverDevice(cfg, dc);
                if (rc != err::kSuccess) {
                    AbortOpen(dc);
                    return Status::Error(rc);
                }
            }

            devices_.push_back(std::move(dc));
        }

        opened_ = true;
        CABE_LOG_INFO("Engine::Open 成功: %zu 个设备 (%s)", opts.devices.size(),
                      opts.create ? "create" : "recover");
        return Status::Ok();
        // 终态契约（P5M6-D4）：自此引擎不再记得自己怎么打开的——后续一切路径零 create/recover 分支。
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

        // 按级别（wal.WriteWal 现读 wal_level）：1/3 同步落盘、2/4 攒批；预写日志，写在内存索引之前。
        // P5M5：经撞墙救援包装——环满时强制快照腾空间再重试一次。
        rc = WriteWalRescuing(dc, WalEntry{WalEntryType::Put, key, block_id, value_crc, now});
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

        // P5M4：写已提交，查大小阈值，到了就（自动、发后不管）触发一份快照。
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

        // 按级别：1/3 同步落盘、2/4 攒批；墓碑帧写在内存改动之前（预写日志）。
        // P5M5：经撞墙救援包装（同 Put）。
        rc = WriteWalRescuing(dc, WalEntry{WalEntryType::Delete, key, BlockId{}, 0, util::GetWallTimeNs()});
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

    // ---- P5M6：恢复编排（流水线 ③~⑨，设计稿 §4）----

    int32_t Engine::RecoverDevice(const DeviceConfig& cfg, DeviceContext& dc) {
        // ③ 快照设备：开 + 双槽裁决 + 状态重建（last_trigger_seq = covered 也在其内落位）。
        int32_t rc = dc.snapshot.Open(cfg.snapshot_path, &options_, dc.super_block);
        if (rc != err::kSuccess) return rc;

        // ④ 快照记录 → 索引（基底）。接收器 = 共享校验 + Insert。
        std::uint64_t loaded = 0;
        rc = dc.snapshot.Load([&](std::string_view k, const ValueMeta& m) -> int32_t {
            ++loaded;
            return ApplyRecoveredEntry(dc, k, m);
        });
        if (rc != err::kSuccess) return rc;

        // ⑤⑥⑦ WAL：开（几何 + recover 容量校验）→ 扫描重放（增量压基底）→ 运行态重建。
        rc = dc.wal.Open(cfg.wal_path, &options_);
        if (rc != err::kSuccess) return rc;
        std::uint64_t replayed = 0;
        rc = dc.wal.Recover(dc.snapshot.last_covered_seq(),
                            [&](const WalEntry& e, std::uint64_t seq) -> int32_t {
                                ++replayed;
                                return ApplyWalEntry(dc, e, seq);
                            });
        if (rc != err::kSuccess) return rc;

        // ⑧ 终态索引 → 分配器（空闲 = 终态补集；必须在重放完成之后——活块集合才完整，D1）。
        rc = RebuildAllocator(dc);
        if (rc != err::kSuccess) return rc;

        // ⑨ 可选诊断（默认关）：CRC 不符不算失败；读 I/O 错拒开（回顾修正 #2：返回值必须检查）。
        if (options_.verify_value_crc_on_recovery) {
            rc = VerifyValuesCrc(dc);
            if (rc != err::kSuccess) return rc;
        }

        CABE_LOG_INFO("恢复完成: 快照 %llu 条 + 重放 %llu 帧 (covered_seq=%llu), 活块 %zu/%llu",
                      static_cast<unsigned long long>(loaded),
                      static_cast<unsigned long long>(replayed),
                      static_cast<unsigned long long>(dc.snapshot.last_covered_seq()),
                      dc.meta_index.Size(),
                      static_cast<unsigned long long>(dc.super_block.block_count));
        return err::kSuccess;
    }

    bool Engine::ValidateRecoveredMeta(const DeviceContext& dc, std::string_view key, BlockId block,
                                       bool has_block, const char* origin, std::uint64_t seq) const {
        // 统一校验表（4.3，两条恢复来路同一份代码）：只查"不查会咬人"的字段——
        // CRC 已保证内容位级忠实，这里防的是"写入方当年就写错了"。违例 = 证据矛盾，调用方拒开。
        const char* why = nullptr;
        if (key.empty() || key.size() > kWalKeyMax) {
            why = "key 长度非法";                          // 空 key 是写路径门口就拒的（幽灵键）
        } else if (has_block && block.dev() != 0) {
            why = "BlockId device 位非零";                 // 指向不存在的设备，污染分配器重建（P7 随路由改）
        } else if (has_block && block.block_idx() >= dc.super_block.block_count) {
            why = "块号越出数据区";                        // 越界读 / RebuildFromActive 被喂越界块
        }
        if (why != nullptr) {
            CABE_LOG_ERROR("恢复条目校验不过 [%s]: %s — seq/序=%llu key_len=%zu block_idx=%llu/%llu",
                           origin, why,
                           static_cast<unsigned long long>(seq),
                           key.size(),
                           static_cast<unsigned long long>(block.block_idx()),
                           static_cast<unsigned long long>(dc.super_block.block_count));
            return false;
        }
        return true;
    }

    int32_t Engine::ApplyRecoveredEntry(DeviceContext& dc, std::string_view key, const ValueMeta& meta) {
        if (!ValidateRecoveredMeta(dc, key, meta.block, /*has_block=*/true,
                                   "快照记录", dc.meta_index.Size())) {
            return err::kSnapshotCorrupted;
        }
        return dc.meta_index.Insert(key, meta);
    }

    int32_t Engine::ApplyWalEntry(DeviceContext& dc, const WalEntry& e, std::uint64_t seq) {
        // 语义解释收口（D15）：与正常写路径的索引段逐字对仗；按 seq 序重放后同 key 自然留最后结果。
        switch (e.type) {
        case WalEntryType::Put: {
            if (!ValidateRecoveredMeta(dc, e.key, e.block, /*has_block=*/true, "WAL帧", seq)) {
                return err::kWalRecoveryCorrupted;
            }
            ValueMeta meta{};
            meta.block     = e.block;
            meta.timestamp = e.timestamp;   // 还原"当年"，不用恢复时刻（TTL 前提，4.2）
            meta.crc       = e.value_crc;
            meta.state     = ValueState::Active;
            return dc.meta_index.Insert(e.key, meta);
        }
        case WalEntryType::Delete: {
            if (!ValidateRecoveredMeta(dc, e.key, BlockId{}, /*has_block=*/false, "WAL帧", seq)) {
                return err::kWalRecoveryCorrupted;
            }
            const int32_t rc = dc.meta_index.Delete(e.key);
            if (rc == err::kIndexKeyNotFound) {
                // 写侧按构造保证墓碑只在键存在时产生；精确复演下必命中——不命中 = 快照、
                // 帧序、索引三者必有一个在说谎（4.1 两步论证），拒开。
                CABE_LOG_ERROR("重放遇到\"删不存在的键\"（证据矛盾）: seq=%llu key_len=%zu",
                               static_cast<unsigned long long>(seq), e.key.size());
                return err::kWalRecoveryCorrupted;
            }
            return rc;
        }
        default:
            // VerifyFrame 不查类型；版本 1 的写入方只产出 Put/Delete——未知类型 = 矛盾（4.3 查①）。
            CABE_LOG_ERROR("重放遇到未知帧类型（版本 1 只产出 Put/Delete）: type=%u seq=%llu",
                           static_cast<unsigned>(e.type),
                           static_cast<unsigned long long>(seq));
            return err::kWalRecoveryCorrupted;
        }
    }

    int32_t Engine::RebuildAllocator(DeviceContext& dc) {
        std::vector<BlockId> active;
        active.reserve(dc.meta_index.Size());
        int32_t rc = dc.meta_index.ForEach([&](std::string_view, const ValueMeta& m) -> int32_t {
            active.push_back(m.block);
            return err::kSuccess;
        });
        if (rc != err::kSuccess) return rc;
        rc = dc.block_allocator.RebuildFromActive(0, dc.super_block.block_count, active);
        if (rc != err::kSuccess) {
            // 越界/重复活块（P5M6-D18 增补检测）——穿透了上游校验的恢复一致性矛盾，拒开。
            CABE_LOG_ERROR("空闲块重建失败（活块清单矛盾）: rc=%d, 活块数=%zu", rc, active.size());
            return rc;
        }
        return err::kSuccess;
    }

    int32_t Engine::VerifyValuesCrc(DeviceContext& dc) {
        std::byte* buf = dc.pool.Allocate();
        if (buf == nullptr) return err::kEnginePoolExhausted;
        std::uint64_t bad = 0;
        int32_t rc = dc.meta_index.ForEach([&](std::string_view key, const ValueMeta& m) -> int32_t {
            const int32_t rrc = dc.io.Read(m.block.block_idx(), buf);
            if (rrc != err::kSuccess) return rrc;   // 读 I/O 错 = 证据不可得 → 拒开（io 段原始码）
            if (util::CRC32(DataView{buf, kValueSize}) != m.crc) {
                ++bad;
                if (bad <= kRecoveryCrcLogCap) {
                    CABE_LOG_WARN("恢复期 value CRC 不符: key=%.*s... block_idx=%llu"
                                  "（条目保留，Get 时如实报 kEngineDataCorrupted）",
                                  static_cast<int>(std::min<std::size_t>(key.size(), 32)), key.data(),
                                  static_cast<unsigned long long>(m.block.block_idx()));
                }
            }
            return err::kSuccess;
        });
        dc.pool.Free(buf);
        if (rc != err::kSuccess) return rc;
        if (bad > 0) {
            CABE_LOG_WARN("恢复期 value CRC 全检: %llu 个键的 value 损坏（级别 3 契约内的正常崩溃"
                          "形态；条目全部保留，删条目是假装写入没发生过——P5M6-D16）",
                          static_cast<unsigned long long>(bad));
        }
        return err::kSuccess;
    }

    // ---- P5M4：快照触发链 ----

    Status Engine::Snapshot() {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        // 手动触发：同步执行、逐设备各做一份、返回第一个错误。
        // P5M6：引擎开着 ⇒ 三设备必然全开着（recover 守卫已拆，D20）。
        int32_t first_err = err::kSuccess;
        for (auto& dc : devices_) {
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
        // 先刷 WAL（默认级别 3 是空操作）→ 定格时刻成对捕获 → 驱动遍历写快照。
        int32_t rc = dc.wal.Flush();
        if (rc != err::kSuccess) return rc;

        // P5M5：与 covered_seq 同刻捕获回收边界（covered_seq 的物理孪生，设计 §6.2）。
        // 必须在 Flush 之后——变体 Y 下此时的窗口起点才是"已持久且永不再被重写"的边界。
        const std::uint64_t covered_seq = dc.wal.last_seq();
        const std::uint64_t boundary    = dc.wal.reclaim_boundary();

        rc = dc.snapshot.Write(covered_seq, [&](const MetaIndexVisitor& v) {
            return dc.meta_index.ForEach(v);   // 回调驱动一致扫描；可中止错误一路传出
        });
        if (rc != err::kSuccess) return rc;    // 快照失败 → 不回收，boundary 随栈丢弃（铁律）

        // P5M5：快照已落地 → 回收（head 跳到边界，铁律的落点）。回收失败只记日志、不上抛——
        // 快照确实持久了（对调用方报失败是说谎）；失败是保守方向（空间暂不复用、正确性零损），
        // 且下次快照会捕获新边界自然再收（自愈）。Wal 侧已记 FATAL。
        const int32_t rrc = dc.wal.ReclaimUpTo(boundary);
        if (rrc != err::kSuccess) {
            CABE_LOG_ERROR("WAL 回收失败（快照本身已成功，空间暂不复用）: rc=%d", rrc);
        }
        return err::kSuccess;                  // 快照成败 = Write 成败（D11）
    }

    int32_t Engine::WriteWalRescuing(DeviceContext& dc, const WalEntry& e) {
        int32_t rc = dc.wal.WriteWal(e);
        if (rc != err::kWalFull) return rc;    // 正常路径零开销：就一个比较

        // 撞墙救援（P5M5 §8.2）：环满 → 强制快照腾空间。直调 DoSnapshot、绕过增长闸门——
        // 撞墙后写入失败、WAL 不再增长，闸门若拦它会卡死且设备恢复后无法自愈。
        // 语义自洽：本次 key 尚未入索引 → 快照不含本次写；被拒帧 seq 未分配 → covered_seq
        // 不含它；重试的新帧 seq > covered_seq → 活帧。双故障（环满 + 快照坏）下每次写会做
        // 一次注定失败的尝试——系统本就在持续报错，吵闹换自愈（设计 D13 如实认账）。
        CABE_LOG_WARN("WAL 环已满，强制快照腾空间");
        rc = DoSnapshot(dc);
        if (rc != err::kSuccess) return err::kWalFull;   // 救不了 → 对外就是"满"（运维信号）

        return dc.wal.WriteWal(e);   // 重试恰一次：快照成功 + ring ≥ 缓冲+4K（Open 校验）⇒ 必成
    }

    void Engine::RequestSnapshot(DeviceContext& dc) {
        // 自动触发汇总入口：发后不管。M4 同步执行；P7 改为唤醒后台快照线程。
        // 快照失败不连累本次写，只记日志（退避基准已在 DoSnapshot 入口推进）。
        int32_t rc = DoSnapshot(dc);
        if (rc != err::kSuccess) {
            CABE_LOG_ERROR("自动触发的快照失败: rc=%d（不影响本次写，后续按退避重试）", rc);
        }
    }

    void Engine::MaybeRequestSnapshot(DeviceContext& dc) {
        // 距上次"尝试"以来的 WAL 增长（序号差 × 帧大小）达阈值即触发（退避：基准是 last_trigger_seq）。
        // P5M6：recover 后 last_trigger = covered（盘上真实痕迹）——肥 WAL 恢复后首写自然触发（D4 自愈）。
        const std::uint64_t grown =
            (dc.wal.last_seq() - dc.snapshot.last_trigger_seq()) * kWalFrameSize;
        if (grown >= options_.snapshot_threshold_bytes) {
            RequestSnapshot(dc);
        }
    }

} // namespace cabe
