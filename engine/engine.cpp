#include "engine/engine.h"
#include "engine/super_block.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <cstring>
#include <memory>
#include <vector>

namespace cabe {

    namespace {
        // P5M6：⑨ 步 value CRC 全检的逐键日志上限（百万条坏 value 刷屏等于没有日志；
        // 上限 + 收尾汇总兼顾可读与全貌，设计 D16）。
        constexpr std::uint64_t kRecoveryCrcLogCap = 100;
    } // namespace

    Engine::~Engine() {
        if (opened_.load(std::memory_order_acquire)) {
            CABE_LOG_WARN("Engine 析构时仍处于 Opened 状态，自动 Close");
            Close();
        }
    }

    Status Engine::Open(const Options& opts) {
        if (opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineAlreadyOpen);
        if (opts.devices.empty()) return Status::Error(err::kEngineInvalidOpts);
        if (opts.devices.size() > 1) return Status::Error(err::kEngineInvalidOpts);

        options_ = opts;   // P5M3：常驻；组件持 &options_ 现读 wal_level（M1 只读）

        // ---- 阶段一：逐设备 create/recover 进临时 holder（reactor 线程尚未起，单线程，P7-D11）----
        std::vector<DeviceContext> recovered;
        recovered.reserve(opts.devices.size());

        auto fail_phase1 = [&](DeviceContext& dc, int32_t rc) -> Status {
            AbortOpen(dc);
            for (auto& d : recovered) AbortOpen(d);
            recovered.clear();
            return Status::Error(rc);
        };

        for (std::size_t i = 0; i < opts.devices.size(); ++i) {
            const auto& cfg = opts.devices[i];
            DeviceContext dc;

            // 超级块：create 写双份 / recover 读校验（含双向配对 + 设备编号）——流水线 ①
            int32_t rc = opts.create
                ? CreateDeviceGroup(cfg, i, &dc.super_block)
                : RecoverDeviceGroup(cfg, i, &dc.super_block);
            if (rc != err::kSuccess) return fail_phase1(dc, rc);

            // 打开数据设备的 IoBackend——流水线 ②
            rc = dc.io.Open(cfg.data_path, &options_);
            if (rc != err::kSuccess) return fail_phase1(dc, rc);

            // 兜底：超级块持久 block_count 不应超过 IoBackend 实测可寻址块数。
            if (dc.super_block.block_count > dc.io.BlockCount()) {
                CABE_LOG_ERROR("block_count 不一致: 超级块=%llu > 设备=%llu",
                               static_cast<unsigned long long>(dc.super_block.block_count),
                               static_cast<unsigned long long>(dc.io.BlockCount()));
                return fail_phase1(dc, err::kSuperBlockSizeMismatch);
            }

            dc.pool = BufferPool(kDefaultPoolBlocks);
            // TODO(P7/多设备 M4): BlockId device 位此处仍硬编码 0；M4 改 static_cast<DeviceId>(i)
            //   并与 RouteKey 路由对齐。M1 是 N=1，device 位无歧义。
            dc.block_allocator.Init(0, dc.super_block.block_count);

            if (opts.create) {
                // create：开 WAL + 快照设备（空环 / 清槽），索引从空开始。
                rc = dc.wal.Open(cfg.wal_path, &options_);
                if (rc != err::kSuccess) return fail_phase1(dc, rc);
                rc = dc.snapshot.Open(cfg.snapshot_path, &options_, dc.super_block);
                if (rc != err::kSuccess) return fail_phase1(dc, rc);
            } else {
                // P5M6 recover：完整恢复链（流水线 ③~⑨）。要么完整恢复要么干净失败（D3）。
                rc = RecoverDevice(cfg, dc);
                if (rc != err::kSuccess) return fail_phase1(dc, rc);
            }

            recovered.push_back(std::move(dc));
        }

        // ---- 阶段二：全部 recover 成功 → 逐个 move 进 Reactor + Start（P7-D4/D11）----
        for (std::size_t i = 0; i < recovered.size(); ++i) {
            auto r = std::make_unique<Reactor>(std::move(recovered[i]));
            int32_t rc = r->Start();
            if (rc != err::kSuccess) {
                // Start 失败（明显故障：线程/资源创建）：停掉已起的 reactor；
                // recovered[i] 的 dc 已 move 进 r，r 析构会关它；recovered[i+1..] 未 wrap，AbortOpen
                // 逐个关（已 move 出的是安全空操作）。
                CABE_LOG_ERROR("reactor 启动失败: rc=%d", rc);
                reactors_.clear();
                for (auto& d : recovered) AbortOpen(d);
                return Status::Error(rc);
            }
            reactors_.push_back(std::move(r));
        }

        opened_.store(true, std::memory_order_release);
        CABE_LOG_INFO("Engine::Open 成功: %zu 个设备 (%s)", opts.devices.size(),
                      opts.create ? "create" : "recover");
        return Status::Ok();
        // 终态契约（P5M6-D4）：自此引擎不再记得自己怎么打开的——后续一切路径零 create/recover 分支。
    }

    Status Engine::Close() {
        if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
        opened_.store(false, std::memory_order_release);   // 尽力挡新调用；竞态由 reactor 的 drain-then-close 兜

        // 逐 reactor：投 Stop op + join（drain-then-close，关 snapshot→wal→io）+ 取关闭首错。
        int32_t first_err = err::kSuccess;
        for (auto& r : reactors_) {
            int32_t rc = r->Stop();
            if (first_err == err::kSuccess) first_err = rc;
        }
        reactors_.clear();   // 线程已 join、dc 已关；析构是干净的
        CABE_LOG_INFO("Engine::Close 完成");
        return first_err != err::kSuccess ? Status::Error(first_err) : Status::Ok();
    }

    // ---- P7M1：写路径与运营口暂返 not-implemented（M2 入 reactor）。参数不命名以避 -Wunused-parameter ----

    Status Engine::Put(std::string_view, DataView) {
        if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
        return Status::Error(err::kEngineNotImplemented);   // P7M2：Put 写路径入 reactor
    }

    Status Engine::Get(std::string_view key, DataBuffer value) {
        if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
        if (key.empty()) return Status::Error(err::kMemEmptyKey);
        if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);

        // 算路由 → 栈上建 op（零堆分配）→ 投递 reactor 并挂起 → 醒来取结果码 → 翻译 Status。
        OpNode op{OpType::Get, key, value};   // result/next/wake 用默认初始化
        const int32_t rc = SubmitAndWait(*reactors_[RouteKey(key)], op);
        return rc == err::kSuccess ? Status::Ok() : Status::Error(rc);
    }

    Status Engine::Delete(std::string_view) {
        if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
        return Status::Error(err::kEngineNotImplemented);   // P7M2：Delete 写路径入 reactor
    }

    Status Engine::SetWalLevel(WalLevel) {
        if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
        return Status::Error(err::kEngineNotImplemented);   // P7M2：运营口入 reactor（wal_level 转 per-reactor）
    }

    Status Engine::Snapshot() {
        if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
        return Status::Error(err::kEngineNotImplemented);   // P7M2：运营口 fan-out 入 reactor
    }

    bool Engine::is_open() const noexcept { return opened_.load(std::memory_order_acquire); }

    std::size_t Engine::RouteKey(std::string_view key) const noexcept {
        (void)key;
        return 0;   // P7M1：N=1，单 reactor；M4 改 hash(key)%N（P7-D7）
    }

    void Engine::TrimDeviceBlock(DeviceContext& dc, BlockId id) {
        // TODO(P7): 通过待 TRIM 队列异步批量发送 BLKDISCARD（P7 首轮不做，留性能轮）
        (void)dc;
        (void)id;
    }

    void Engine::AbortOpen(DeviceContext& dc) {
        // 关一个裸 dc 的三设备句柄。各 Close 对未打开 / moved-from 句柄都是安全空操作。
        dc.snapshot.Close();
        dc.wal.Close();
        dc.io.Close();
    }

    // ---- P5M6：恢复编排（流水线 ③~⑨，设计稿 §4；Open 阶段一在裸 dc 上跑）----

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

    // ---- P5M4/M5：快照触发链 + 撞墙救援（P7M1 暂不调用，dead；M2 写路径入 reactor 时启用/迁入）----

    int32_t Engine::DoSnapshot(DeviceContext& dc) {
        // 一次快照尝试从这里开始：先推进退避基准（成败都算，设计 §10.3）。
        dc.snapshot.NoteTriggerAttempt(dc.wal.last_seq());
        // 先刷 WAL（默认级别 3 是空操作）→ 定格时刻成对捕获 → 驱动遍历写快照。
        int32_t rc = dc.wal.Flush();
        if (rc != err::kSuccess) return rc;

        // P5M5：与 covered_seq 同刻捕获回收边界（covered_seq 的物理孪生，设计 §6.2）。
        const std::uint64_t covered_seq = dc.wal.last_seq();
        const std::uint64_t boundary    = dc.wal.reclaim_boundary();

        rc = dc.snapshot.Write(covered_seq, [&](const MetaIndexVisitor& v) {
            return dc.meta_index.ForEach(v);   // 回调驱动一致扫描；可中止错误一路传出
        });
        if (rc != err::kSuccess) return rc;    // 快照失败 → 不回收，boundary 随栈丢弃（铁律）

        // P5M5：快照已落地 → 回收（head 跳到边界）。回收失败只记日志、不上抛（自愈）。
        const int32_t rrc = dc.wal.ReclaimUpTo(boundary);
        if (rrc != err::kSuccess) {
            CABE_LOG_ERROR("WAL 回收失败（快照本身已成功，空间暂不复用）: rc=%d", rrc);
        }
        return err::kSuccess;                  // 快照成败 = Write 成败（D11）
    }

    int32_t Engine::WriteWalRescuing(DeviceContext& dc, const WalEntry& e) {
        int32_t rc = dc.wal.WriteWal(e);
        if (rc != err::kWalFull) return rc;    // 正常路径零开销：就一个比较

        // 撞墙救援（P5M5 §8.2）：环满 → 强制快照腾空间。直调 DoSnapshot、绕过增长闸门。
        CABE_LOG_WARN("WAL 环已满，强制快照腾空间");
        rc = DoSnapshot(dc);
        if (rc != err::kSuccess) return err::kWalFull;   // 救不了 → 对外就是"满"（运维信号）

        return dc.wal.WriteWal(e);   // 重试恰一次：快照成功 + ring ≥ 缓冲+4K（Open 校验）⇒ 必成
    }

    void Engine::RequestSnapshot(DeviceContext& dc) {
        // 自动触发汇总入口：发后不管。快照失败不连累本次写，只记日志。
        int32_t rc = DoSnapshot(dc);
        if (rc != err::kSuccess) {
            CABE_LOG_ERROR("自动触发的快照失败: rc=%d（不影响本次写，后续按退避重试）", rc);
        }
    }

    void Engine::MaybeRequestSnapshot(DeviceContext& dc) {
        // 距上次"尝试"以来的 WAL 增长（序号差 × 帧大小）达阈值即触发（退避：基准是 last_trigger_seq）。
        const std::uint64_t grown =
            (dc.wal.last_seq() - dc.snapshot.last_trigger_seq()) * kWalFrameSize;
        if (grown >= options_.snapshot_threshold_bytes) {
            RequestSnapshot(dc);
        }
    }

} // namespace cabe
