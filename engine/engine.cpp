#include "engine/engine.h"
#include "common/logger.h"

#include <fcntl.h>
#include <unistd.h>

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
            int fd = ::open(cfg.path.c_str(), O_RDWR | O_DIRECT, 0);
            if (fd < 0) {
                CABE_LOG_ERROR("open(\"%s\") O_DIRECT 失败: fd=%d", cfg.path.c_str(), fd);
                for (auto& dc : devices_) {
                    if (dc.fd >= 0) ::close(dc.fd);
                }
                devices_.clear();
                return Status::Error(err::kIoBase);
            }
            DeviceContext dc;
            dc.fd = fd;
            dc.pool = BufferPool(kDefaultPoolBlocks);
            devices_.push_back(std::move(dc));
        }

        opened_ = true;
        CABE_LOG_INFO("Engine::Open 成功: %zu 个设备", opts.devices.size());
        return Status::Ok();
    }

    Status Engine::Close() {
        if (!opened_) return Status::Error(err::kEngineNotOpen);

        for (auto& dc : devices_) {
            if (dc.fd >= 0) {
                ::close(dc.fd);
                dc.fd = -1;
            }
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
        return Status::Error(err::kEngineNotImplemented);
    }

    Status Engine::Get(std::string_view key, DataBuffer value) {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        if (key.empty()) return Status::Error(err::kMemEmptyKey);
        if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);
        return Status::Error(err::kEngineNotImplemented);
    }

    Status Engine::Delete(std::string_view key) {
        if (!opened_) return Status::Error(err::kEngineNotOpen);
        if (key.empty()) return Status::Error(err::kMemEmptyKey);
        return Status::Error(err::kEngineNotImplemented);
    }

    bool Engine::is_open() const noexcept { return opened_; }

} // namespace cabe
