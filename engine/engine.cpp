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

        for (std::size_t i = 0; i < opts.devices.size(); ++i) {
            const auto& cfg = opts.devices[i];
            DeviceContext dc;

            // 超级块：create 写双份 / recover 读校验（含双向配对 + 设备编号）
            int32_t rc = opts.create
                ? CreateDeviceGroup(cfg, i, &dc.super_block)
                : RecoverDeviceGroup(cfg, i, &dc.super_block);
            if (rc != err::kSuccess) {
                for (auto& d : devices_) { d.wal.Close(); d.io.Close(); }
                devices_.clear();
                return Status::Error(rc);
            }

            // 打开数据设备的 IoBackend
            rc = dc.io.Open(cfg.data_path);
            if (rc != err::kSuccess) {
                for (auto& d : devices_) { d.wal.Close(); d.io.Close(); }
                devices_.clear();
                return Status::Error(rc);
            }

            // 兜底：超级块持久 block_count 不应超过 IoBackend 实测可寻址块数（recover 已核对，
            // 此处再防御任何绕过 RecoverDeviceGroup 的路径或现算/持久值漂移）。
            if (dc.super_block.block_count > dc.io.BlockCount()) {
                CABE_LOG_ERROR("block_count 不一致: 超级块=%llu > 设备=%llu",
                               static_cast<unsigned long long>(dc.super_block.block_count),
                               static_cast<unsigned long long>(dc.io.BlockCount()));
                for (auto& d : devices_) { d.wal.Close(); d.io.Close(); }
                devices_.clear();
                return Status::Error(err::kSuperBlockSizeMismatch);
            }

            dc.pool = BufferPool(kDefaultPoolBlocks);
            // 用超级块记录的 block_count（数据区块数，权威值）；逻辑 block 从 0，
            // 物理偏移由 IoBackend 加 kDataRegionOffset
            // TODO(P7/多设备): BlockId 的 device 位此处硬编码为 0，而 super_block.device_id=i；
            //   多设备启用后应改为 static_cast<DeviceId>(i) 并与 RouteKey 路由对齐。
            dc.block_allocator.Init(0, dc.super_block.block_count);

            // P5M2：仅 create 模式打开 WAL（D9）；recover 的 WAL 重放 + 续写在 M5。
            if (opts.create) {
                rc = dc.wal.Open(cfg.wal_path, opts.wal_level);
                if (rc != err::kSuccess) {
                    for (auto& d : devices_) { d.wal.Close(); d.io.Close(); }
                    devices_.clear();
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

        for (auto& dc : devices_) {
            dc.wal.Close();
            dc.io.Close();
        }
        devices_.clear();
        opened_ = false;
        CABE_LOG_INFO("Engine::Close 完成");
        return Status::Ok();
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

        // 级别 1：value FUA 持久（io.Write 内部 fdatasync），写在 WAL 之前
        rc = dc.io.Write(block_id.block_idx(), buf);
        dc.pool.Free(buf);
        if (rc != err::kSuccess) {
            dc.block_allocator.Recycle(block_id);
            return Status::Error(rc);
        }

        // 级别 1：WAL 帧同步落盘（预写日志：写在内存索引之前）
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

        // 级别 1：墓碑帧同步落盘（写在内存改动之前；预写日志）
        rc = dc.wal.WriteWal(WalEntry{WalEntryType::Delete, key, BlockId{}, 0, util::GetWallTimeNs()});
        if (rc != err::kSuccess) return Status::Error(rc);   // WAL 失败 → 不动内存

        dc.meta_index.Delete(key);
        dc.block_allocator.Recycle(meta.block);
        TrimDeviceBlock(dc, meta.block);

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

} // namespace cabe
