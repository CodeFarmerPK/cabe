#include "engine/engine.h"
#include "common/logger.h"

#include <fcntl.h>
#include <linux/fs.h>    // BLKGETSIZE64
#include <sys/ioctl.h>
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
            // 查询设备大小 → 计算可用块数（尾部不足 kValueSize 丢弃）
            std::uint64_t dev_bytes = 0;
            if (::ioctl(fd, BLKGETSIZE64, &dev_bytes) < 0) {
                CABE_LOG_ERROR("ioctl BLKGETSIZE64 失败: fd=%d", fd);
                ::close(fd);
                for (auto& d : devices_) {
                    if (d.fd >= 0) ::close(d.fd);
                }
                devices_.clear();
                return Status::Error(err::kIoBase);
            }
            std::uint64_t block_count = dev_bytes / kValueSize;
            if (block_count == 0) {
                CABE_LOG_ERROR("设备太小: %llu 字节 < 1 MiB",
                               static_cast<unsigned long long>(dev_bytes));
                ::close(fd);
                for (auto& d : devices_) {
                    if (d.fd >= 0) ::close(d.fd);
                }
                devices_.clear();
                return Status::Error(err::kEngineInvalidOpts);
            }

            DeviceContext dc;
            dc.fd = fd;
            dc.pool = BufferPool(kDefaultPoolBlocks);
            dc.free_list.Init(0, block_count);  // device_id = 0（P1 单设备）
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

    std::size_t Engine::RouteKey(std::string_view key) const noexcept {
        (void)key;
        return 0;
    }

} // namespace cabe
