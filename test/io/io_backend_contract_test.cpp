/*
 * Project: Cabe
 * Created Time: 2026-04-27
 * Created by: CodeFarmerPK
 *
 * IoBackend 契约测试(P3 M2 验收)。
 *
 * 这是**编译期 dispatch 后所有 backend 都必须通过**的黑盒契约测试。
 * 当前 P3 M2 阶段只有 SyncIoBackend 一种实现 → 测的是 cabe::io::IoBackend
 * 类型别名(由 CABE_IO_BACKEND_SYNC=1 选定)。
 *
 * P4 接入 io_uring 后,本文件不改,只在 CMake 切到 -DCABE_IO_BACKEND=io_uring
 * 重新编译,同一组测试针对 IoUringIoBackend 跑一次 —— 这是契约测试的设计
 * 意图:接口契约不依赖具体实现。
 *
 * 与 SyncIoBackendSkeleton 的分工:
 *   - skeleton_test 只测**不依赖设备**的契约(类型别名 / concept / move 语义 /
 *     未 Open 状态的默认值),无 CABE_TEST_DEVICE 也能跑
 *   - contract_test 测**真实 I/O 路径**(Open 真实块设备 / 读写往返 / 池耗尽 /
 *     越界保护 / Q7 闭环),必须 CABE_TEST_DEVICE 设到一个块设备节点
 *
 * 数据隔离:每个 test 都新建 IoBackend 实例,但所有 test 共用一个真实设备。
 * SyncIoBackend 的 Open 是终态 Close 的(Q5 实例级状态机),所以每个 test
 * 的 backend 实例独立 —— 不会复用前一个 test 的状态。设备本身的 block 0..N
 * 会被覆盖写,但每个 test 的 blockId 选择避开冲突或不依赖跨 test 数据。
 */

#include "io/io_backend.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace cabe::io {
namespace {

constexpr std::uint32_t kDefaultPoolCount = 8;

std::string GetTestDevice() {
    const char* env = std::getenv("CABE_TEST_DEVICE");
    if (env == nullptr || *env == '\0') return {};
    return env;
}

// 用确定性 pattern 填充 buffer,便于 ReadBlock 比对
void FillPattern(char* buf, std::size_t size, std::uint8_t seed) {
    for (std::size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<char>((i * 31 + seed) & 0xFF);
    }
}

bool MatchesPattern(const char* buf, std::size_t size, std::uint8_t seed) {
    for (std::size_t i = 0; i < size; ++i) {
        if (buf[i] != static_cast<char>((i * 31 + seed) & 0xFF)) return false;
    }
    return true;
}

// ============================================================
// Fixture
// ============================================================
class IoBackendContractTest : public ::testing::Test {
protected:
    IoBackend   backend_;
    std::string devicePath_;

    void SetUp() override {
        devicePath_ = GetTestDevice();
        if (devicePath_.empty()) {
            GTEST_SKIP() << "CABE_TEST_DEVICE not set; "
                            "use scripts/mkloop.sh to create a loop device "
                            "and `export CABE_TEST_DEVICE=/dev/loopX`";
        }
    }

    // backend_ 析构时若 opened_ 仍 true,RAII 会调 Close,无需 TearDown 显式处理
};

// ============================================================
// Open / Close
// ============================================================

TEST_F(IoBackendContractTest, OpenAndClose) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));
    EXPECT_TRUE(backend_.IsOpen());
    EXPECT_GT(backend_.BlockCount(), 0u);

    ASSERT_EQ(SUCCESS, backend_.Close());
    EXPECT_FALSE(backend_.IsOpen());
    EXPECT_EQ(backend_.BlockCount(), 0u);
    EXPECT_TRUE(backend_.is_closed());
}

TEST_F(IoBackendContractTest, OpenEmptyPathFails) {
    EXPECT_EQ(DEVICE_FAILED_TO_OPEN_DEVICE,
              backend_.Open(std::string{}, kDefaultPoolCount));
    EXPECT_FALSE(backend_.IsOpen());
}

TEST_F(IoBackendContractTest, OpenZeroPoolCountFails) {
    EXPECT_EQ(POOL_INVALID_PARAMS, backend_.Open(devicePath_, 0u));
    EXPECT_FALSE(backend_.IsOpen());
}

TEST_F(IoBackendContractTest, OpenNonBlockDeviceRejected) {
    // /dev/null 是字符设备,不是块设备,必须被 S_ISBLK 拦住
    EXPECT_EQ(DEVICE_NOT_BLOCK_DEVICE,
              backend_.Open("/dev/null", kDefaultPoolCount));
    EXPECT_FALSE(backend_.IsOpen());
}

TEST_F(IoBackendContractTest, OpenNonExistentPathFails) {
    EXPECT_EQ(DEVICE_FAILED_TO_OPEN_DEVICE,
              backend_.Open("/dev/this-does-not-exist-cabe-test", kDefaultPoolCount));
    EXPECT_FALSE(backend_.IsOpen());
}

TEST_F(IoBackendContractTest, DoubleOpenRejected) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));
    EXPECT_EQ(IO_BACKEND_ALREADY_OPEN,
              backend_.Open(devicePath_, kDefaultPoolCount));
    EXPECT_TRUE(backend_.IsOpen());     // 仍以首次配置打开
}

// 终态 Close:Open → Close → Open 必须被拒绝(实例已用过)
TEST_F(IoBackendContractTest, OpenAfterCloseRejected) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));
    ASSERT_EQ(SUCCESS, backend_.Close());
    EXPECT_TRUE(backend_.is_closed());

    EXPECT_EQ(IO_BACKEND_ALREADY_OPEN,
              backend_.Open(devicePath_, kDefaultPoolCount));
}

// 重复 Close 幂等(opened=true → Close → opened=false → Close no-op)
TEST_F(IoBackendContractTest, DoubleCloseIdempotent) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));
    ASSERT_EQ(SUCCESS, backend_.Close());
    EXPECT_EQ(SUCCESS, backend_.Close());     // 第二次 close 是 no-op
}

// ============================================================
// AcquireBuffer / 归还
// ============================================================

TEST_F(IoBackendContractTest, AcquireBufferReturnsValidWhenOpen) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle h = backend_.AcquireBuffer();
    EXPECT_TRUE(h.valid());
    EXPECT_EQ(h.size(), CABE_VALUE_DATA_SIZE);
    EXPECT_NE(h.data(), nullptr);
}

TEST_F(IoBackendContractTest, AcquireBufferExhaustionReturnsInvalid) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    std::vector<BufferHandle> handles;
    for (std::uint32_t i = 0; i < kDefaultPoolCount; ++i) {
        BufferHandle h = backend_.AcquireBuffer();
        ASSERT_TRUE(h.valid()) << "slot " << i << " 不应耗尽";
        handles.push_back(std::move(h));
    }

    // Q3:第 N+1 次必须立刻返回 invalid,不阻塞
    BufferHandle exhausted = backend_.AcquireBuffer();
    EXPECT_FALSE(exhausted.valid());
}

// 归还后 slot 可被再次 Acquire(同一指针 — LIFO,刚归还的最先被取)
TEST_F(IoBackendContractTest, BufferAutoReturnedOnDestruction) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    char* firstPtr = nullptr;
    {
        BufferHandle h = backend_.AcquireBuffer();
        ASSERT_TRUE(h.valid());
        firstPtr = h.data();
    }   // h 析构 → 归还

    BufferHandle h2 = backend_.AcquireBuffer();
    ASSERT_TRUE(h2.valid());
    // LIFO:刚归还的 slot 是栈顶,下次 Acquire 拿到同一指针
    EXPECT_EQ(h2.data(), firstPtr);
}

// move:source 失效,target 接管;原来的 slot 不会被多次归还
TEST_F(IoBackendContractTest, BufferHandleMovePreservesSingleOwnership) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle a = backend_.AcquireBuffer();
    ASSERT_TRUE(a.valid());
    char* aPtr = a.data();

    BufferHandle b{std::move(a)};
    EXPECT_FALSE(a.valid());
    EXPECT_TRUE(b.valid());
    EXPECT_EQ(b.data(), aPtr);
    // a 此时 invalid,析构不会归还;只有 b 析构归还一次
}

// ============================================================
// ReadBlock / WriteBlock 往返
// ============================================================

TEST_F(IoBackendContractTest, WriteThenReadRoundTrip) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    // 写入
    {
        BufferHandle wbuf = backend_.AcquireBuffer();
        ASSERT_TRUE(wbuf.valid());
        FillPattern(wbuf.data(), wbuf.size(), 0x37);
        EXPECT_EQ(SUCCESS, backend_.WriteBlock(0, wbuf));
    }

    // 读取并比对
    {
        BufferHandle rbuf = backend_.AcquireBuffer();
        ASSERT_TRUE(rbuf.valid());
        // Q2:Acquire 不 memset,buffer 内容未定义。先写无关 pattern 再读,
        // 验证 ReadBlock 确实覆盖了整块。
        FillPattern(rbuf.data(), rbuf.size(), 0xAB);
        EXPECT_EQ(SUCCESS, backend_.ReadBlock(0, rbuf));
        EXPECT_TRUE(MatchesPattern(rbuf.data(), rbuf.size(), 0x37))
            << "ReadBlock 没有正确覆盖 buffer 全部内容";
    }
}

TEST_F(IoBackendContractTest, WriteBlockOutOfRangeRejected) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle wbuf = backend_.AcquireBuffer();
    ASSERT_TRUE(wbuf.valid());
    FillPattern(wbuf.data(), wbuf.size(), 0x42);

    // 越界 blockId
    const BlockId outOfRange = backend_.BlockCount();
    EXPECT_EQ(DEVICE_NO_SPACE, backend_.WriteBlock(outOfRange, wbuf));
}

TEST_F(IoBackendContractTest, ReadBlockOutOfRangeRejected) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle rbuf = backend_.AcquireBuffer();
    ASSERT_TRUE(rbuf.valid());

    const BlockId outOfRange = backend_.BlockCount();
    EXPECT_EQ(DEVICE_NO_SPACE, backend_.ReadBlock(outOfRange, rbuf));
}

TEST_F(IoBackendContractTest, WriteBlockInvalidHandleRejected) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle invalid;       // default 构造 → invalid
    EXPECT_EQ(IO_BACKEND_INVALID_HANDLE, backend_.WriteBlock(0, invalid));
}

TEST_F(IoBackendContractTest, ReadBlockInvalidHandleRejected) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle invalid;
    EXPECT_EQ(IO_BACKEND_INVALID_HANDLE, backend_.ReadBlock(0, invalid));
}

TEST_F(IoBackendContractTest, WriteBlockBeforeOpenFails) {
    BufferHandle h;
    EXPECT_EQ(IO_BACKEND_NOT_OPEN, backend_.WriteBlock(0, h));
}

TEST_F(IoBackendContractTest, ReadBlockBeforeOpenFails) {
    BufferHandle h;
    EXPECT_EQ(IO_BACKEND_NOT_OPEN, backend_.ReadBlock(0, h));
}

// 跨多个 block 读写,确认 offset 计算正确
TEST_F(IoBackendContractTest, MultiBlockWriteReadIsolated) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    constexpr std::size_t kBlocks = 4;
    ASSERT_GE(backend_.BlockCount(), kBlocks);

    // 写 4 个不同 pattern 的 block
    for (BlockId id = 0; id < kBlocks; ++id) {
        BufferHandle wbuf = backend_.AcquireBuffer();
        ASSERT_TRUE(wbuf.valid());
        FillPattern(wbuf.data(), wbuf.size(), static_cast<std::uint8_t>(0x10 + id));
        EXPECT_EQ(SUCCESS, backend_.WriteBlock(id, wbuf));
    }

    // 倒序读回,验证每个 block 的 pattern 独立
    for (BlockId id = kBlocks; id-- > 0;) {
        BufferHandle rbuf = backend_.AcquireBuffer();
        ASSERT_TRUE(rbuf.valid());
        std::memset(rbuf.data(), 0, rbuf.size());
        EXPECT_EQ(SUCCESS, backend_.ReadBlock(id, rbuf));
        EXPECT_TRUE(MatchesPattern(rbuf.data(), rbuf.size(),
                                   static_cast<std::uint8_t>(0x10 + id)))
            << "block " << id << " pattern 不匹配";
    }
}

// 写入 close 后再开新实例读出,验证数据真的落盘(O_SYNC)
TEST_F(IoBackendContractTest, DataPersistsAcrossInstances) {
    constexpr std::uint8_t kSeed = 0x73;
    constexpr BlockId      kTargetBlock = 7;

    {
        IoBackend writer;
        ASSERT_EQ(SUCCESS, writer.Open(devicePath_, kDefaultPoolCount));
        ASSERT_GT(writer.BlockCount(), kTargetBlock);

        BufferHandle wbuf = writer.AcquireBuffer();
        ASSERT_TRUE(wbuf.valid());
        FillPattern(wbuf.data(), wbuf.size(), kSeed);
        ASSERT_EQ(SUCCESS, writer.WriteBlock(kTargetBlock, wbuf));
        // wbuf 析构归还 → writer.Close() 析构调用 → 终态
    }

    {
        IoBackend reader;
        ASSERT_EQ(SUCCESS, reader.Open(devicePath_, kDefaultPoolCount));

        BufferHandle rbuf = reader.AcquireBuffer();
        ASSERT_TRUE(rbuf.valid());
        std::memset(rbuf.data(), 0, rbuf.size());
        ASSERT_EQ(SUCCESS, reader.ReadBlock(kTargetBlock, rbuf));
        EXPECT_TRUE(MatchesPattern(rbuf.data(), rbuf.size(), kSeed))
            << "数据没有跨实例持久化(O_SYNC + O_DIRECT 应保证落盘)";
    }
}

// ============================================================
// Q7:Close 后 handle 析构静默 no-op(Release 模式;Debug 此 case 会 abort)
// ============================================================
//
// 这条 case 在 Debug 构建下会触发 std::abort —— 这是 Q7 的设计意图(让开发期
// 立刻发现"忘归还 handle"的 bug)。所以仅在 Release 构建运行,Debug 跳过。
//
// 实际很难在测试里制造一个稳定可观察的 Release-only abort 行为不出错的 case,
// 这里反过来:在 Release 下验证"force-release 后 handle 析构不 crash"。
#ifdef NDEBUG
TEST_F(IoBackendContractTest, ForceReleaseCloseDoesNotCrashOnHandleDtor) {
    ASSERT_EQ(SUCCESS, backend_.Open(devicePath_, kDefaultPoolCount));

    BufferHandle leaked = backend_.AcquireBuffer();
    ASSERT_TRUE(leaked.valid());

    // 故意在 handle 还活着时 Close —— Release 模式 warn + force-release
    EXPECT_EQ(SUCCESS, backend_.Close());
    EXPECT_TRUE(backend_.is_closed());

    // leaked 走出作用域析构 → BufferHandleImpl::~ 调 ReturnBuffer_Internal
    //   → 看到 closed_=true,fast-path 只 dec count,不触碰已 munmap 的 pool
    //   → 不应 crash
    // 此 case 跑过即视为"force-release 路径不 UAF"
}
#endif

} // namespace
} // namespace cabe::io
