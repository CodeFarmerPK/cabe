#ifndef CABE_IO_H
#define CABE_IO_H

#include "engine/status.h"
#include "common/structs.h"

#include <cstddef>
#include <cstdint>

namespace cabe {

    Status WriteBlock(int fd, std::uint64_t block_idx, const std::byte* buf);
    Status ReadBlock(int fd, std::uint64_t block_idx, std::byte* buf);

} // namespace cabe

#endif // CABE_IO_H
