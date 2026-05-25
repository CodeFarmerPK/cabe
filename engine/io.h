#ifndef CABE_IO_H
#define CABE_IO_H

#include "common/error_code.h"
#include "common/structs.h"

#include <cstddef>
#include <cstdint>

namespace cabe {

    // 内部函数——返回 int32_t 错误码（不是 Status）。
    // 调用方（Engine）在公开方法体内转 Status::Error(rc)。
    int32_t WriteBlock(int fd, std::uint64_t block_idx, const std::byte* buf);
    int32_t ReadBlock(int fd, std::uint64_t block_idx, std::byte* buf);

} // namespace cabe

#endif // CABE_IO_H
