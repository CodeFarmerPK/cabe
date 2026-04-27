/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * P3 M1 IoBackend 骨架冒烟测试。
 *
 * 仅验证:
 *   1) IoBackend 类型别名解析(由 CABE_IO_BACKEND_SYNC=1 选定为 SyncIoBackend)
 *   2) IoBackendTraits concept 编译期满足
 *   3) IoBackend 不可 copy / move
 *   4) IsOpen / BlockCount / is_closed 默认值符合契约
 *   5) AcquireBuffer 在未 Open 时返回 invalid handle(Q3)
 *   6) BufferHandle move 语义(Q1 RAII 不漏泄)
 *
 * 不测真实 I/O —— 那是 M2 之后契约测试(io_backend_contract_test.cpp)的工作。
 */

#include "io/io_backend.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

namespace cabe::io {
namespace {

TEST(SyncIoBackendSkeleton, TypeAliasResolves) {
    // 这个测试单纯靠"能编译过"就证明 IoBackend 别名指向了 SyncIoBackend。
    // 加 SUCCEED() 让 gtest 把它列出来,否则会被认为没有 ASSERT 而误报。
    static_assert(std::is_same_v<IoBackend, SyncIoBackend>,
                  "P3 M1 默认应选 sync 后端");
    SUCCEED();
}

TEST(SyncIoBackendSkeleton, ConceptIsSatisfied) {
    static_assert(IoBackendTraits<IoBackend>);
    static_assert(!std::is_copy_constructible_v<IoBackend>);
    static_assert(!std::is_move_constructible_v<IoBackend>);
    SUCCEED();
}

TEST(SyncIoBackendSkeleton, ConstructionAndDefaults) {
    IoBackend backend;
    EXPECT_FALSE(backend.IsOpen());
    EXPECT_EQ(backend.BlockCount(), 0u);
    EXPECT_FALSE(backend.is_closed());
}

TEST(SyncIoBackendSkeleton, AcquireBufferReturnsInvalidWhenNotOpen) {
    IoBackend backend;
    BufferHandle h = backend.AcquireBuffer();
    EXPECT_FALSE(h.valid());
    EXPECT_EQ(h.size(), 0u);
    EXPECT_EQ(h.data(), nullptr);
}

TEST(SyncIoBackendSkeleton, BufferHandleDefaultIsInvalid) {
    BufferHandle h;
    EXPECT_FALSE(h.valid());
    EXPECT_EQ(h.size(), 0u);
    EXPECT_EQ(h.data(), nullptr);

    const BufferHandle& cref = h;
    EXPECT_EQ(cref.data(), nullptr);
}

TEST(SyncIoBackendSkeleton, BufferHandleMoveSemantics) {
    BufferHandle a;
    BufferHandle b{std::move(a)};
    EXPECT_FALSE(a.valid());     // a 被 move 走,仍 invalid(原本就 invalid)
    EXPECT_FALSE(b.valid());

    BufferHandle c;
    c = std::move(b);
    EXPECT_FALSE(b.valid());
    EXPECT_FALSE(c.valid());
}

TEST(SyncIoBackendSkeleton, CloseWithoutOpenIsSafe) {
    IoBackend backend;
    // M2 起 Close 在 (opened=false) 状态下幂等 no-op,不进入 terminal:
    //   - 返回 SUCCESS
    //   - is_closed() 仍为 false(允许后续 Open,与 Engine usedOnce_ 行为分离)
    //   - IsOpen 仍为 false
    EXPECT_EQ(backend.Close(), SUCCESS);
    EXPECT_FALSE(backend.is_closed());
    EXPECT_FALSE(backend.IsOpen());
}

TEST(SyncIoBackendSkeleton, ReadWriteBeforeOpenReturnsNotOpen) {
    IoBackend backend;
    BufferHandle h;
    EXPECT_EQ(backend.WriteBlock(0, h), IO_BACKEND_NOT_OPEN);
    EXPECT_EQ(backend.ReadBlock(0, h),  IO_BACKEND_NOT_OPEN);
}

} // namespace
} // namespace cabe::io
