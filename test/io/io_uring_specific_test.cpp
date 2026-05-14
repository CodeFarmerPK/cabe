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
#include <string>

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

} // namespace
} // namespace cabe::io
