#include "engine/put_path.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>

namespace {

struct FreeDeleter {
    void operator()(std::byte* ptr) const noexcept {
        std::free(ptr);
    }
};

std::unique_ptr<std::byte, FreeDeleter> MakeAligned(std::size_t alignment, std::size_t size) {
    void* raw = nullptr;
    if (posix_memalign(&raw, alignment, size) != 0) return {};
    return std::unique_ptr<std::byte, FreeDeleter>(static_cast<std::byte*>(raw));
}

cabe::DataView ViewOf(const std::unique_ptr<std::byte, FreeDeleter>& mem) {
    return cabe::DataView{mem.get(), cabe::kValueSize};
}

} // namespace

TEST(PutPath, TargetKeyValueBufferDirect) {
    auto mem = MakeAligned(4096, cabe::kValueSize);
    ASSERT_NE(mem, nullptr);

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        ViewOf(mem),
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::TargetKeyValueBuffer},
        cabe::BackendDirectWriteCaps{});

    EXPECT_EQ(plan.path, cabe::PutWritePath::Direct);
}

TEST(PutPath, OtherKeyValueBufferFallback) {
    auto mem = MakeAligned(4096, cabe::kValueSize);
    ASSERT_NE(mem, nullptr);

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        ViewOf(mem),
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::OtherKeyValueBuffer},
        cabe::BackendDirectWriteCaps{});

    EXPECT_EQ(plan.path, cabe::PutWritePath::CopyFallback);
}

TEST(PutPath, OtherDeviceValueBufferFallback) {
    auto mem = MakeAligned(4096, cabe::kValueSize);
    ASSERT_NE(mem, nullptr);

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        ViewOf(mem),
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::OtherDeviceValueBuffer},
        cabe::BackendDirectWriteCaps{});

    EXPECT_EQ(plan.path, cabe::PutWritePath::CopyFallback);
}

TEST(PutPath, PoolAddressNotAllocatedFallback) {
    auto mem = MakeAligned(4096, cabe::kValueSize);
    ASSERT_NE(mem, nullptr);

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        ViewOf(mem),
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::PoolAddressNotAllocated},
        cabe::BackendDirectWriteCaps{});

    EXPECT_EQ(plan.path, cabe::PutWritePath::CopyFallback);
}

TEST(PutPath, ExternalAlignedDirect) {
    auto mem = MakeAligned(4096, cabe::kValueSize);
    ASSERT_NE(mem, nullptr);

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        ViewOf(mem),
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::ExternalValueMemory},
        cabe::BackendDirectWriteCaps{.allow_external_value_memory = true,
                                     .external_memory_alignment = 4096});

    EXPECT_EQ(plan.path, cabe::PutWritePath::Direct);
}

TEST(PutPath, ExternalUnalignedFallback) {
    auto mem = MakeAligned(4096, cabe::kValueSize + 1);
    ASSERT_NE(mem, nullptr);
    const cabe::DataView unaligned{mem.get() + 1, cabe::kValueSize};

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        unaligned,
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::ExternalValueMemory},
        cabe::BackendDirectWriteCaps{.allow_external_value_memory = true,
                                     .external_memory_alignment = 4096});

    EXPECT_EQ(plan.path, cabe::PutWritePath::CopyFallback);
}

TEST(PutPath, ExternalDisabledFallback) {
    auto mem = MakeAligned(4096, cabe::kValueSize);
    ASSERT_NE(mem, nullptr);

    const cabe::PutWritePlan plan = cabe::BuildPutWritePlan(
        ViewOf(mem),
        cabe::PutValueSource{.kind = cabe::PutValueSourceKind::ExternalValueMemory},
        cabe::BackendDirectWriteCaps{.allow_external_value_memory = false,
                                     .external_memory_alignment = 4096});

    EXPECT_EQ(plan.path, cabe::PutWritePath::CopyFallback);
}
