#ifndef CABE_IO_RETRY_H
#define CABE_IO_RETRY_H

// 裸 pread/pwrite 的健壮封装：循环重试 EINTR、累加部分传输，直到读/写满 len 或遇错/EOF。
// 头文件内联，无需链接——RawDevice 与各 IoBackend 可各自包含调用（尊重 D5：不让 IoBackend
// 依赖 RawDevice，只共享 syscall 层的重试机制）。io_uring 因 API 不同，自带 EINTR 循环。
//
// 设计依据：P5 review（环境异常加固——EINTR / 部分传输）。

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <unistd.h>

namespace cabe::io_util {

    // 完整读：成功读满 len 返回 true；遇真错误或提前 EOF 返回 false。EINTR 自动重试。
    inline bool ReadExact(int fd, void* buf, std::size_t len, std::uint64_t offset) {
        auto* p = static_cast<unsigned char*>(buf);
        std::size_t done = 0;
        while (done < len) {
            ssize_t n = ::pread(fd, p + done, len - done,
                                static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;  // 信号中断 → 重试
                return false;
            }
            if (n == 0) return false;          // 提前 EOF（读到的比 len 少）
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

    // 完整写：成功写满 len 返回 true；遇真错误返回 false。EINTR 自动重试。
    inline bool WriteExact(int fd, const void* buf, std::size_t len, std::uint64_t offset) {
        const auto* p = static_cast<const unsigned char*>(buf);
        std::size_t done = 0;
        while (done < len) {
            ssize_t n = ::pwrite(fd, p + done, len - done,
                                 static_cast<off_t>(offset + done));
            if (n < 0) {
                if (errno == EINTR) continue;  // 信号中断 → 重试
                return false;
            }
            if (n == 0) return false;          // 防御：可写 fd 上 pwrite 不应返回 0，兜底防死循环
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

} // namespace cabe::io_util

#endif // CABE_IO_RETRY_H
