/*
 * Project: Cabe
 * Created Time: 2026-04-28
 * Created by: CodeFarmerPK
 *
 * P4 M1 io_uring 后端骨架冒烟测试(对应 sync_io_backend_skeleton_test.cpp)。
 *
 * 仅验证:
 *   1) IoBackend 类型别名解析(由 CABE_IO_BACKEND_IO_URING=1 选定为 IoUringIoBackend)
 *   2) IoBackendTraits concept 编译期满足
 *   3) IoBackend 不可 copy / move
 *   4) IsOpen / BlockCount / is_closed 默认值符合契约
 *   5) AcquireBuffer 在未 Open 时返回 invalid handle(M1 stub:等同 Q3 池耗尽)
 *   6) BufferHandle move 语义(Q1 RAII 不漏泄)
 *   7) Close 在未 Open 上幂等 SUCCESS,不进入 terminal
 *   8) WriteBlock/ReadBlock 在未 Open 上返回 IO_BACKEND_NOT_OPEN
 *
 * 不测真实 I/O —— 那是 M2/M3 之后契约测试(io_backend_contract_test.cpp)的工作。
 *
 * 由 CMakeLists.txt 按 CABE_IO_BACKEND 条件追加:
 *   - CABE_IO_BACKEND=sync     → 编译 sync_io_backend_skeleton_test.cpp
 *   - CABE_IO_BACKEND=io_uring → 编译本文件
 * 同时编译两份会因 static_assert(IoBackend == 具体后端) 互相冲突。
 *
 * M1 验收:
 *   ./scripts/run-tests.sh --backend=io_uring --filter 'IoUringSkeleton'
 *   预期 8/8 全绿。同 build 下 io_backend_contract_test 的 Engine 集成用例
 *   会因 stub 实现(Open 返回 NOT_OPEN)而 fail —— 这是 M1 期预期行为,
 *   M2 / M3 落地后转绿。
 */

#include "io/io_backend.h"

#include <gtest/gtest.h>

#include <type_traits>
#include <utility>

namespace cabe::io {
namespace {

TEST(IoUringSkeleton, TypeAliasResolves) {
    // 单纯靠 "能编译过" 就证明 IoBackend 别名指向了 IoUringIoBackend。
    // SUCCEED() 让 gtest 把它列出来,否则会被认为没有 ASSERT 而误报。
    static_assert(std::is_same_v<IoBackend, IoUringIoBackend>,
                  "P4 M1:CABE_IO_BACKEND=io_uring 时 IoBackend 别名应指向 IoUringIoBackend");
    SUCCEED();
}

TEST(IoUringSkeleton, ConceptIsSatisfied) {
    static_assert(IoBackendTraits<IoBackend>);
    static_assert(!std::is_copy_constructible_v<IoBackend>);
    static_assert(!std::is_move_constructible_v<IoBackend>);
    SUCCEED();
}

TEST(IoUringSkeleton, ConstructionAndDefaults) {
    IoBackend backend;
    EXPECT_FALSE(backend.IsOpen());
    EXPECT_EQ(backend.BlockCount(), 0u);
    EXPECT_FALSE(backend.is_closed());
}

TEST(IoUringSkeleton, AcquireBufferReturnsInvalidWhenNotOpen) {
    IoBackend backend;
    BufferHandle h = backend.AcquireBuffer();
    EXPECT_FALSE(h.valid());
    EXPECT_EQ(h.size(), 0u);
    EXPECT_EQ(h.data(), nullptr);
}

TEST(IoUringSkeleton, BufferHandleDefaultIsInvalid) {
    BufferHandle h;
    EXPECT_FALSE(h.valid());
    EXPECT_EQ(h.size(), 0u);
    EXPECT_EQ(h.data(), nullptr);

    const BufferHandle& cref = h;
    EXPECT_EQ(cref.data(), nullptr);
}

TEST(IoUringSkeleton, BufferHandleMoveSemantics) {
    BufferHandle a;
    BufferHandle b{std::move(a)};
    EXPECT_FALSE(a.valid());     // a 被 move 走,仍 invalid(原本就 invalid)
    EXPECT_FALSE(b.valid());

    BufferHandle c;
    c = std::move(b);
    EXPECT_FALSE(b.valid());
    EXPECT_FALSE(c.valid());
}

TEST(IoUringSkeleton, CloseWithoutOpenIsSafe) {
    IoBackend backend;
    // M1 stub:Close 在未 Open 上幂等 SUCCESS,不进入 terminal:
    //   - 返回 SUCCESS
    //   - is_closed() 仍为 false(允许 M2 起在同实例上首次 Open 成功)
    //   - IsOpen 仍为 false
    // 与 sync 后端的同名测试语义完全一致(同一份 IoBackend 契约)。
    EXPECT_EQ(backend.Close(), SUCCESS);
    EXPECT_FALSE(backend.is_closed());
    EXPECT_FALSE(backend.IsOpen());
}

TEST(IoUringSkeleton, ReadWriteBeforeOpenReturnsNotOpen) {
    IoBackend backend;
    BufferHandle h;
    EXPECT_EQ(backend.WriteBlock(0, h), IO_BACKEND_NOT_OPEN);
    EXPECT_EQ(backend.ReadBlock(0, h),  IO_BACKEND_NOT_OPEN);
}

} // namespace
} // namespace cabe::io
