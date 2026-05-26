#ifndef CABE_IO_BACKEND_H
#define CABE_IO_BACKEND_H

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>

namespace cabe {

    template<typename T>
    concept IoBackend = requires(T& io, const std::string& path,
                                 std::uint64_t block_idx,
                                 const std::byte* wbuf, std::byte* rbuf) {
        { io.Open(path) } -> std::same_as<int32_t>;
        { io.Close() } -> std::same_as<int32_t>;
        { io.BlockCount() } -> std::convertible_to<std::uint64_t>;
        { io.Write(block_idx, wbuf) } -> std::same_as<int32_t>;
        { io.Read(block_idx, rbuf) } -> std::same_as<int32_t>;
    };

} // namespace cabe

#endif // CABE_IO_BACKEND_H
