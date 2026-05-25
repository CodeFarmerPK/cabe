#include "engine/io.h"
#include "common/logger.h"

#include <unistd.h>

namespace cabe {

    int32_t WriteBlock(int fd, std::uint64_t block_idx, const std::byte* buf) {
        const auto offset = static_cast<off_t>(block_idx * kValueSize);
        ssize_t written = ::pwrite(fd, buf, kValueSize, offset);
        if (written != static_cast<ssize_t>(kValueSize)) {
            CABE_LOG_ERROR("pwrite 失败: fd=%d block_idx=%llu written=%zd",
                           fd, static_cast<unsigned long long>(block_idx), written);
            return err::kIoBase;
        }
        return err::kSuccess;
    }

    int32_t ReadBlock(int fd, std::uint64_t block_idx, std::byte* buf) {
        const auto offset = static_cast<off_t>(block_idx * kValueSize);
        ssize_t nread = ::pread(fd, buf, kValueSize, offset);
        if (nread != static_cast<ssize_t>(kValueSize)) {
            CABE_LOG_ERROR("pread 失败: fd=%d block_idx=%llu nread=%zd",
                           fd, static_cast<unsigned long long>(block_idx), nread);
            return err::kIoBase;
        }
        return err::kSuccess;
    }

} // namespace cabe
