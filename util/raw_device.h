#ifndef CABE_RAW_DEVICE_H
#define CABE_RAW_DEVICE_H

#include "common/error_code.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace cabe {

    // 通用裸设备 I/O 工具（P5M1-D5）。
    // 封装：打开裸块设备（O_DIRECT）+ 设备大小查询 + 任意偏移/长度的对齐读写 + 对齐缓冲分配。
    // 服务于超级块（4K）、WAL（4K 帧）、快照（流式）——与 IoBackend 的 1M 块语义分离。
    // offset / len / buf 均要求 4K 对齐（O_DIRECT 硬约束）。
    class RawDevice {
    public:
        static constexpr std::size_t kAlignment = 4096;

        RawDevice() noexcept = default;
        ~RawDevice();

        RawDevice(const RawDevice&) = delete;
        RawDevice& operator=(const RawDevice&) = delete;
        RawDevice(RawDevice&& other) noexcept;
        RawDevice& operator=(RawDevice&& other) noexcept;

        int32_t Open(const std::string& path);
        int32_t Close();
        int32_t Sync();   // fdatasync：把已写数据刷出设备易失写缓存（O_DIRECT 不保证刷设备缓存）
        std::uint64_t SizeBytes() const noexcept;
        int32_t ReadAt(std::uint64_t offset, std::byte* buf, std::size_t len);
        int32_t WriteAt(std::uint64_t offset, const std::byte* buf, std::size_t len);
        bool is_open() const noexcept;

        // 4K 对齐缓冲分配（posix_memalign）；用完以 FreeAligned 释放。
        static std::byte* AllocAligned(std::size_t size);
        static void FreeAligned(std::byte* p) noexcept;

    private:
        int fd_ = -1;
        std::uint64_t size_bytes_ = 0;
    };

} // namespace cabe

#endif // CABE_RAW_DEVICE_H
