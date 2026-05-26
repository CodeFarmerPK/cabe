#include "engine/engine.h"
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

        for (const auto& cfg : opts.devices) {
            DeviceContext dc;

            int32_t rc = dc.io.Open(cfg.path);
            if (rc != err::kSuccess) {
                for (auto& d : devices_) d.io.Close();
                devices_.clear();
                return Status::Error(rc);
            }

            dc.pool = BufferPool(kDefaultPoolBlocks);
            dc.free_list.Init(0, dc.io.BlockCount());
            devices_.push_back(std::move(dc));
        }

        opened_ = true;
        CABE_LOG_INFO("Engine::Open 成功: %zu 个设备", opts.devices.size());
        return Status::Ok();
    }

    Status Engine::Close() {
        if (!opened_) return Status::Error(err::kEngineNotOpen);

        for (auto& dc : devices_) {
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

        auto& dc = devices_[RouteKey(key)];

        ValueMeta old_meta{};
        if (dc.meta_index.Lookup(key, &old_meta) == err::kSuccess) {
            dc.free_list.Free(old_meta.block);
        }

        BlockId block_id{};
        int32_t rc = dc.free_list.Allocate(&block_id);
        if (rc != err::kSuccess) return Status::Error(rc);

        std::byte* buf = dc.pool.Allocate();
        if (!buf) {
            dc.free_list.Free(block_id);
            return Status::Error(err::kEnginePoolExhausted);
        }
        std::memcpy(buf, value.data(), kValueSize);

        rc = dc.io.Write(block_id.block_idx(), buf);
        dc.pool.Free(buf);
        if (rc != err::kSuccess) {
            dc.free_list.Free(block_id);
            return Status::Error(rc);
        }

        ValueMeta meta{};
        meta.block     = block_id;
        meta.timestamp = util::GetWallTimeNs();
        meta.crc       = util::CRC32(value);
        meta.state     = ValueState::Active;
        dc.meta_index.Insert(key, meta);

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
        if (rc != err::kSuccess) return Status::Error(rc);

        dc.free_list.Free(meta.block);
        dc.meta_index.Delete(key);

        return Status::Ok();
    }

    bool Engine::is_open() const noexcept { return opened_; }

    std::size_t Engine::RouteKey(std::string_view key) const noexcept {
        (void)key;
        return 0;
    }

} // namespace cabe
