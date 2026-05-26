#ifndef CABE_SYNC_IO_BACKEND_H
#define CABE_SYNC_IO_BACKEND_H

#include "io/io_backend.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <cstdint>
#include <string>

namespace cabe {

    class SyncIoBackend {
    public:
        SyncIoBackend() noexcept = default;
        ~SyncIoBackend();

        SyncIoBackend(const SyncIoBackend&) = delete;
        SyncIoBackend& operator=(const SyncIoBackend&) = delete;
        SyncIoBackend(SyncIoBackend&& other) noexcept;
        SyncIoBackend& operator=(SyncIoBackend&& other) noexcept;

        int32_t Open(const std::string& path);
        int32_t Close();
        std::uint64_t BlockCount() const noexcept;
        int32_t Write(std::uint64_t block_idx, const std::byte* buf);
        int32_t Read(std::uint64_t block_idx, std::byte* buf);

        bool is_open() const noexcept;

    private:
        int fd_ = -1;
        std::uint64_t block_count_ = 0;
    };

    static_assert(IoBackend<SyncIoBackend>);

} // namespace cabe

#endif // CABE_SYNC_IO_BACKEND_H
