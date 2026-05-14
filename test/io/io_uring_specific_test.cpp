/*
 * Project: Cabe
 * Created Time: 2026-04-29
 * Created by: CodeFarmerPK
 *
 * io_uring 后端专属测试。仅在 CABE_IO_BACKEND=io_uring 编译时挂入
 * cabe_test 可执行(CMakeLists.txt 条件 list(APPEND TEST_SOURCES ...))。
 *
 * P4 阶段会持续在此文件追加 io_uring 独有行为测试:
 *   M4(本文件初版):
 *     - RegisterBuffersFailsWhenPoolTooLarge:RLIMIT_MEMLOCK 撞限 → Open 失败 (D15)
 *   M6 计划补:
 *     - CloseDrainsInflightSubmissions
 *     - RegisterBufferIndexMatchesSlot
 *     - OpenRejectsSqDepthLessThanPoolCount(M6 引入 Options.io_uring_sq_depth 后)
 *     - WriteBlockEAGAINRetriesOnce
 *
 * 与 io_backend_contract_test.cpp 的分工:
 *   - contract_test:跨 backend 共契约,sync 与 io_uring 都跑
 *   - 本文件:io_uring 独有路径(register_buffers / queue_init / IOSQE_*)的行为
 */

#include "io/io_backend.h"

#include "common/error_code.h"
#include "common/structs.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <set>          // M6 RegisterBufferIndexMatchesSlot 用
#include <string>
#include <vector>       // M6 RegisterBufferIndexMatchesSlot 用

#include <sys/resource.h>
#include <unistd.h>

namespace cabe::io {
namespace {

std::string GetTestDevice() {
    const char* env = std::getenv("CABE_TEST_DEVICE");
    if (env == nullptr || *env == '\0') return {};
    return env;
}

// ⚠ root / CAP_IPC_LOCK 限制(2026-04-29 实测):
//
//   Linux 内核 io_uring/rsrc.c::__io_account_mem 入口先做 capable(CAP_IPC_LOCK)
//   检查,持有 CAP_IPC_LOCK 直接 return 0 跳过 RLIMIT 校验。EUID=0 默认包含
//   CAP_IPC_LOCK,因此本测试在 sudo run-tests.sh 的典型工作流下无法触发
//   register_buffers 失败 —— SetUp 直接 SKIP。
//
//   想真正跑 D15 验证,需以 non-root 身份跑(且该用户对 CABE_TEST_DEVICE
//   有读写权限),典型命令:
//
//     # 1) 让 cabe 用户能访问 loop device
//     sudo chown $USER:$USER /dev/loop0   # 或合适的组,或加 udev 规则
//     # 2) 以 non-root 跑这一条 filter
//     CABE_TEST_DEVICE=/dev/loop0 ./scripts/run-tests.sh --backend=io_uring --filter RegisterBuffers
//
//   或直接用 setpriv 在 sudo 之内降权:
//     sudo setpriv --reuid=$(id -u $USER) --regid=$(id -g $USER) --clear-groups env CABE_TEST_DEVICE=/dev/loop0 ./scripts/run-tests.sh --backend=io_uring --filter RegisterBuffers
// =====================================================================
class RegisterBuffersFailureTest : public ::testing::Test {
protected:
    rlimit       saved_memlock_{};
    bool         saved_  = false;
    std::string  devicePath_;

    void SetUp() override {
        devicePath_ = GetTestDevice();
        if (devicePath_.empty()) {
            GTEST_SKIP() << "CABE_TEST_DEVICE not set; "
                            "use scripts/mkloop.sh to create a loop device";
        }

        // Root / CAP_IPC_LOCK 会让 io_uring_register_buffers 跳过 RLIMIT 检查。
        // 见 fixture 顶部注释。
        if (::geteuid() == 0) {
            GTEST_SKIP()
                << "Running as root: io_uring_register_buffers bypasses "
                << "RLIMIT_MEMLOCK via CAP_IPC_LOCK. Cannot trigger D15 "
                << "register_buffers failure path. To exercise this test, "
                << "drop privileges first, e.g.:\n"
                << "  sudo chown $USER:$USER $CABE_TEST_DEVICE\n"
                << "  ./scripts/run-tests.sh --backend=io_uring --filter RegisterBuffers";
        }

        ASSERT_EQ(0, ::getrlimit(RLIMIT_MEMLOCK, &saved_memlock_))
            << "getrlimit(RLIMIT_MEMLOCK) failed, errno=" << errno
            << " (" << ::strerror(errno) << ")";
        saved_ = true;

        // 把 soft 压到 64 KiB(若 hard 比 64 KiB 还小,以 hard 为准)。
        // hard 不动 → TearDown 升回 soft 不需要特权。
        rlimit tight = saved_memlock_;
        tight.rlim_cur = 64UL * 1024UL;
        if (tight.rlim_max != RLIM_INFINITY && tight.rlim_max < tight.rlim_cur) {
            tight.rlim_cur = tight.rlim_max;
        }
        ASSERT_EQ(0, ::setrlimit(RLIMIT_MEMLOCK, &tight))
            << "setrlimit(RLIMIT_MEMLOCK soft=64KiB) failed, errno=" << errno
            << " (" << ::strerror(errno) << ")";
    }

    void TearDown() override {
        if (saved_) {
            // 还原:soft 升回原值(不超过 hard,无需特权)
            ::setrlimit(RLIMIT_MEMLOCK, &saved_memlock_);
        }
    }
};

TEST_F(RegisterBuffersFailureTest, RegisterBuffersFailsWhenPoolTooLarge) {
    // pool = 16 × 1 MiB = 16 MiB ≫ 64 KiB ulimit
    // → io_uring_register_buffers 失败(典型 -ENOMEM)
    // → D15:Open 整体失败,不 fallback;backend 保持未 open 状态
    IoBackend backend;
    const int32_t result = backend.Open(devicePath_, /*bufferPoolCount=*/16);

    EXPECT_NE(SUCCESS, result)
        << "Open should fail when register_buffers is blocked by RLIMIT_MEMLOCK; "
        << "got rc=" << result;
    EXPECT_FALSE(backend.IsOpen())
        << "Backend should remain in not-opened state on Open failure";
    EXPECT_FALSE(backend.is_closed())
        << "Open failure must not flip the backend into terminal-Closed state "
        << "(允许丢弃实例新构造再 Open)";
}

// =====================================================================
// M6 — 4 个 io_uring 专属测试(对应设计稿 §13 M6 计划)
//
// 通用 fixture:需要 CABE_TEST_DEVICE。下面 3 个 case 直接复用,1 个用
// 独立 fixture(SkipWhenInfrastructureMissing)。
// =====================================================================
class IoUringSpecificTest : public ::testing::Test {
protected:
    std::string devicePath_;

    void SetUp() override {
        devicePath_ = GetTestDevice();
        if (devicePath_.empty()) {
            GTEST_SKIP() << "CABE_TEST_DEVICE not set; "
                            "use scripts/mkloop.sh to create a loop device";
        }
    }
};

// ---------------------------------------------------------------------
// M6 / D7 / R12:Open 校验 sq_depth >= pool_count 与 2 的幂
// ---------------------------------------------------------------------
TEST_F(IoUringSpecificTest, OpenRejectsSqDepthLessThanPoolCount) {
    IoBackend backend;
    // sq_depth=8 < pool_count=16 → R12 校验失败
    const int32_t rc = backend.Open(devicePath_, /*pool=*/16, /*sq_depth=*/8);
    EXPECT_EQ(POOL_INVALID_PARAMS, rc)
        << "Open should reject sq_depth < pool_count (R12); got rc=" << rc;
    EXPECT_FALSE(backend.IsOpen());
    EXPECT_FALSE(backend.is_closed());
}

TEST_F(IoUringSpecificTest, OpenRejectsSqDepthNotPowerOfTwo) {
    IoBackend backend;
    // sq_depth=100 不是 2 的幂 → D7 校验失败
    const int32_t rc = backend.Open(devicePath_, /*pool=*/8, /*sq_depth=*/100);
    EXPECT_EQ(POOL_INVALID_PARAMS, rc)
        << "Open should reject sq_depth not power-of-two (D7); got rc=" << rc;
    EXPECT_FALSE(backend.IsOpen());
}

TEST_F(IoUringSpecificTest, OpenAcceptsPowerOfTwoSqDepthGEPool) {
    IoBackend backend;
    // sq_depth=32 是 2 的幂 且 >= pool_count=16 → 通过
    const int32_t rc = backend.Open(devicePath_, /*pool=*/16, /*sq_depth=*/32);
    EXPECT_EQ(SUCCESS, rc) << "Open should succeed with valid sq_depth; got rc=" << rc;
    EXPECT_TRUE(backend.IsOpen());
}

// ---------------------------------------------------------------------
// M6:RegisterBufferIndexMatchesSlot — 验证 fixed_buf_index == slot_index
// 的一一对应,以及不同 slot 对应独立内存且 *_FIXED ops 命中正确块
// ---------------------------------------------------------------------
TEST_F(IoUringSpecificTest, RegisterBufferIndexMatchesSlot) {
    IoBackend backend;
    constexpr std::uint32_t kPool = 8;
    ASSERT_EQ(SUCCESS, backend.Open(devicePath_, kPool));

    // 1) 拿全 pool,验证 8 个 handle 互不指向同一片内存
    std::vector<BufferHandle> handles;
    handles.reserve(kPool);
    for (std::uint32_t i = 0; i < kPool; ++i) {
        handles.push_back(backend.AcquireBuffer());
        ASSERT_TRUE(handles.back().valid()) << "AcquireBuffer slot " << i << " failed";
    }
    std::set<const char*> distinct_ptrs;
    for (const auto& h : handles) {
        EXPECT_TRUE(distinct_ptrs.insert(h.data()).second)
            << "duplicate pointer across slots → register_buffers index 不独立";
    }
    EXPECT_EQ(distinct_ptrs.size(), kPool);

    // 2) 给每个 slot 写不同 pattern 到不同 block,经 *_FIXED ops 提交
    for (std::uint32_t i = 0; i < kPool; ++i) {
        for (std::size_t j = 0; j < CABE_VALUE_DATA_SIZE; ++j) {
            handles[i].data()[j] = static_cast<char>((i * 37 + j) & 0xFF);
        }
        EXPECT_EQ(SUCCESS, backend.WriteBlock(i, handles[i]));
    }
    handles.clear();    // 全部归还

    // 3) 重新拿 handle,从每个 block 读回,验证 pattern 与 step 2 写入的对应
    //    一致 → 证明 buf_index 与 fd 的映射在 *_FIXED 链路上正确
    for (std::uint32_t i = 0; i < kPool; ++i) {
        BufferHandle h = backend.AcquireBuffer();
        ASSERT_TRUE(h.valid());
        std::memset(h.data(), 0xCC, CABE_VALUE_DATA_SIZE);
        ASSERT_EQ(SUCCESS, backend.ReadBlock(i, h));
        for (std::size_t j = 0; j < CABE_VALUE_DATA_SIZE; ++j) {
            ASSERT_EQ(static_cast<char>((i * 37 + j) & 0xFF), h.data()[j])
                << "block " << i << " byte " << j << " mismatch — buf_index 路由错位";
        }
    }
}

// ---------------------------------------------------------------------
// M6:CloseDrainsInflightSubmissions — 大量串行 W/R 后 Close 干净退出
//
// 注意:Model A 1:1 串行下,WriteBlock 退出 io_mutex_ 时 in_flight_count_
// 必归 0,所以 Close 进入 drain 循环时是 no-op。真正的"drain 等多个 in-flight
// cqe"测试需要 M8 Model B(reaper 线程)的多并发提交基础设施。本测试退而验证:
//   - Open + 反复 W/R + Close 的状态机干净
//   - drain 代码路径在多 op 之后被 exercise(即便每次都 no-op)
//   - Close 后 IsOpen=false 且 is_closed=true(进入 terminal)
// ---------------------------------------------------------------------
TEST_F(IoUringSpecificTest, CloseDrainsInflightSubmissions) {
    IoBackend backend;
    ASSERT_EQ(SUCCESS, backend.Open(devicePath_, /*pool=*/8));
    ASSERT_GE(backend.BlockCount(), 50u);

    for (int i = 0; i < 50; ++i) {
        BufferHandle h = backend.AcquireBuffer();
        ASSERT_TRUE(h.valid());
        std::memset(h.data(), static_cast<int>(i & 0xFF), CABE_VALUE_DATA_SIZE);
        ASSERT_EQ(SUCCESS, backend.WriteBlock(i % 32, h));
        // h dtor 归还
    }

    EXPECT_EQ(SUCCESS, backend.Close());
    EXPECT_FALSE(backend.IsOpen());
    EXPECT_TRUE(backend.is_closed());
}

// ---------------------------------------------------------------------
// M6:WriteBlockEAGAINRetriesOnce — Model A 1:1 串行下 SQ 永不会满,
// EAGAIN 几乎不会发生。无法在不引入 mock kernel 的情况下稳定触发。
// 留 SKIP 占位,等 M7 batch / M8 reaper 上线 + 注入式测试基础设施后再实测。
// ---------------------------------------------------------------------
TEST_F(IoUringSpecificTest, WriteBlockEAGAINRetriesOnce) {
    GTEST_SKIP() << "Model A 1:1 串行下 SQ depth 64 ≫ 1,EAGAIN 不可稳定触发。"
                    "M7 batch / M8 reaper 引入并发提交后,可设 sq_depth=2 + 跑 4+ "
                    "并发 op 制造 EAGAIN;此 case 占位等待 M7+。";
}

} // namespace
} // namespace cabe::io
