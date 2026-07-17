#ifndef CABE_SPDK_IO_BACKEND_PLACEHOLDER_H
#define CABE_SPDK_IO_BACKEND_PLACEHOLDER_H

#include "common/error_code.h"
#include "engine/options.h"
#include "io/io_backend.h"

#include <cstdint>
#include <span>
#include <string>

namespace cabe {

    // P9M1 只建立可编译、可链接的 SPDK 后端边界。真正的 runtime、控制器探测和
    // I/O 提交从后续里程碑逐步进入；任何调用都必须响亮地返回尚未实现。
    class SpdkIoBackendPlaceholder {
    public:
        int32_t Open(const std::string&, const Options* = nullptr) noexcept {
            return err::kEngineNotImplemented;
        }
        void RebindOptions(const Options*) noexcept {}
        int32_t Close() noexcept { return err::kSuccess; }
        std::uint64_t BlockCount() const noexcept { return 0; }
        int32_t RegisterWriteBuffers(std::span<const ValueBufferSlotView>) noexcept {
            return err::kEngineNotImplemented;
        }
        int32_t Write(std::uint64_t, const IoWriteBuffer&) noexcept {
            return err::kEngineNotImplemented;
        }
        int32_t Read(std::uint64_t, std::byte*) noexcept {
            return err::kEngineNotImplemented;
        }
        bool is_open() const noexcept { return false; }
    };

    static_assert(IoBackend<SpdkIoBackendPlaceholder>);

} // namespace cabe

#endif // CABE_SPDK_IO_BACKEND_PLACEHOLDER_H
