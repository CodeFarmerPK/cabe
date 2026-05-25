#include "engine/engine.h"
#include "engine/io.h"
#include "common/logger.h"
#include "util/crc32.h"
#include "util/util.h"

#include <cstring>
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

        auto& dc = devices_[RouteKey(key)];

        // 覆盖写：key 已存在则先释放旧块
        ValueMeta old_meta{};
        if (dc.meta_index.Lookup(key, &old_meta) == err::kSuccess) {
            dc.free_list.Free(old_meta.block);
        }

        // 分配新块
        BlockId block_id{};
        int32_t rc = dc.free_list.Allocate(&block_id);
        if (rc != err::kSuccess) return Status::Error(rc);

        // 分配对齐 buffer + 填 value + 写设备
        std::byte* buf = dc.pool.Allocate();
        if (!buf) {
            dc.free_list.Free(block_id);
            return Status::Error(err::kEnginePoolExhausted);
        }
        std::memcpy(buf, value.data(), kValueSize);

        rc = WriteBlock(dc.fd, block_id.block_idx(), buf);
        dc.pool.Free(buf);
        if (rc != err::kSuccess) {
            dc.free_list.Free(block_id);
            return Status::Error(rc);
        }

        // 更新索引
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

        rc = ReadBlock(dc.fd, meta.block.block_idx(), buf);
        if (rc != err::kSuccess) {
            dc.pool.Free(buf);
            return Status::Error(rc);
        }

        // CRC32 校验
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

        // 标记删除 + 立即回收（不做 I/O、不发 TRIM）
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
