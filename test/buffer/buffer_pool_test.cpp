/*
 * Project: Cabe
 * Created Time: 2026-04-16 20:18
 * Created by: CodeFarmerPK
 */

#include "buffer/buffer_pool.h"
#include "common/error_code.h"
#include "common/structs.h"
#include <gtest/gtest.h>
#include <cstring>
#include <set>

// ============================================================
// 测试常量
// ============================================================
constexpr size_t TEST_BUFFER_SIZE = 4096;       // 4KB，测试用小缓冲区
constexpr uint32_t TEST_BUFFER_COUNT = 4;

// ============================================================
// 1. Init / Destroy 生命周期
// ============================================================
TEST(BufferPoolTest, InitSuccess) {
    BufferPool pool;
    EXPECT_FALSE(pool.IsInitialized());

    int32_t ret = pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);
    EXPECT_EQ(ret, SUCCESS);
    EXPECT_TRUE(pool.IsInitialized());
    EXPECT_EQ(pool.TotalCount(), TEST_BUFFER_COUNT);
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT);
    EXPECT_EQ(pool.UsedCount(), 0u);
    EXPECT_EQ(pool.BufferSize(), TEST_BUFFER_SIZE);
}

TEST(BufferPoolTest, DestroySuccess) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    int32_t ret = pool.Destroy();
    EXPECT_EQ(ret, SUCCESS);
    EXPECT_FALSE(pool.IsInitialized());
    EXPECT_EQ(pool.TotalCount(), 0u);
    EXPECT_EQ(pool.FreeCount(), 0u);
    EXPECT_EQ(pool.BufferSize(), 0u);
}

TEST(BufferPoolTest, DoubleInitFails) {
    BufferPool pool;
    EXPECT_EQ(pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT), SUCCESS);
    EXPECT_EQ(pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT), POOL_ALREADY_INITIALIZED);
}

TEST(BufferPoolTest, DestroyWithoutInitFails) {
    BufferPool pool;
    EXPECT_EQ(pool.Destroy(), POOL_NOT_INITIALIZED);
}

TEST(BufferPoolTest, ReInitAfterDestroy) {
    BufferPool pool;
    EXPECT_EQ(pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT), SUCCESS);
    EXPECT_EQ(pool.Destroy(), SUCCESS);

    // 销毁后可以重新初始化
    EXPECT_EQ(pool.Init(TEST_BUFFER_SIZE * 2, TEST_BUFFER_COUNT), SUCCESS);
    EXPECT_EQ(pool.BufferSize(), TEST_BUFFER_SIZE * 2);
}

// ============================================================
// 2. 参数验证
// ============================================================
TEST(BufferPoolTest, InitZeroBufferSizeFails) {
    BufferPool pool;
    EXPECT_EQ(pool.Init(0, TEST_BUFFER_COUNT), POOL_INVALID_PARAMS);
    EXPECT_FALSE(pool.IsInitialized());
}

TEST(BufferPoolTest, InitZeroBufferCountFails) {
    BufferPool pool;
    EXPECT_EQ(pool.Init(TEST_BUFFER_SIZE, 0), POOL_INVALID_PARAMS);
    EXPECT_FALSE(pool.IsInitialized());
}

TEST(BufferPoolTest, InitBothZeroFails) {
    BufferPool pool;
    EXPECT_EQ(pool.Init(0, 0), POOL_INVALID_PARAMS);
}

// ============================================================
// 3. Acquire 基本功能
// ============================================================
TEST(BufferPoolTest, AcquireReturnsNonNull) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    char* buf = pool.Acquire();
    EXPECT_NE(buf, nullptr);
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT - 1);
    EXPECT_EQ(pool.UsedCount(), 1u);

    pool.Release(buf);
}

TEST(BufferPoolTest, AcquireReturnsZeroedBuffer) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    char* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);

    // 验证缓冲区内容全为 0
    for (size_t i = 0; i < TEST_BUFFER_SIZE; ++i) {
        EXPECT_EQ(buf[i], 0) << "Non-zero byte at offset " << i;
    }

    pool.Release(buf);
}

TEST(BufferPoolTest, AcquireReturnsAlignedPointer) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    for (uint32_t i = 0; i < TEST_BUFFER_COUNT; ++i) {
        char* buf = pool.Acquire();
        ASSERT_NE(buf, nullptr);

        // mmap 返回页对齐地址，每个 buffer 偏移是 bufferSize 的整数倍
        // 验证 512 字节对齐（O_DIRECT 要求）
        auto addr = reinterpret_cast<uintptr_t>(buf);
        EXPECT_EQ(addr % 512, 0u) << "Buffer " << i << " not 512-aligned";
    }
}

TEST(BufferPoolTest, AcquireReturnsUniquePointers) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    std::set<char*> ptrs;
    for (uint32_t i = 0; i < TEST_BUFFER_COUNT; ++i) {
        char* buf = pool.Acquire();
        ASSERT_NE(buf, nullptr);
        // 每个指针必须唯一
        EXPECT_TRUE(ptrs.insert(buf).second) << "Duplicate pointer at index " << i;
    }

    // 归还所有
    for (char* p : ptrs) {
        pool.Release(p);
    }
}

TEST(BufferPoolTest, AcquireExhaustedReturnsNull) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    // 取光所有缓冲区
    char* buffers[TEST_BUFFER_COUNT];
    for (uint32_t i = 0; i < TEST_BUFFER_COUNT; ++i) {
        buffers[i] = pool.Acquire();
        ASSERT_NE(buffers[i], nullptr);
    }

    // 池已耗尽
    EXPECT_EQ(pool.Acquire(), nullptr);
    EXPECT_EQ(pool.FreeCount(), 0u);

    // 归还所有
    for (auto& buf : buffers) {
        pool.Release(buf);
    }
}

TEST(BufferPoolTest, AcquireWithoutInitReturnsNull) {
    BufferPool pool;
    EXPECT_EQ(pool.Acquire(), nullptr);
}

// ============================================================
// 4. Release 验证
// ============================================================
TEST(BufferPoolTest, ReleaseSuccess) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    char* buf = pool.Acquire();
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT - 1);

    int32_t ret = pool.Release(buf);
    EXPECT_EQ(ret, SUCCESS);
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT);
}

TEST(BufferPoolTest, ReleaseNullFails) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);
    EXPECT_EQ(pool.Release(nullptr), POOL_INVALID_POINTER);
}

TEST(BufferPoolTest, ReleaseWithoutInitFails) {
    BufferPool pool;
    char dummy = 'x';
    EXPECT_EQ(pool.Release(&dummy), POOL_NOT_INITIALIZED);
}

TEST(BufferPoolTest, ReleaseOutOfRangeFails) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    // 栈上地址，不在池范围内
    char outsideBuf[16];
    EXPECT_EQ(pool.Release(outsideBuf), POOL_INVALID_POINTER);
}

TEST(BufferPoolTest, ReleaseMisalignedFails) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    // 获取一个合法指针，然后偏移 1 字节使其不对齐
    char* buf = pool.Acquire();
    ASSERT_NE(buf, nullptr);

    EXPECT_EQ(pool.Release(buf + 1), POOL_INVALID_POINTER);

    // 用正确指针归还
    pool.Release(buf);
}

// ============================================================
// 5. Acquire-Release 循环复用
// ============================================================
TEST(BufferPoolTest, AcquireReleaseReuse) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    // 第一轮：取光再全部归还
    char* buffers[TEST_BUFFER_COUNT];
    for (uint32_t i = 0; i < TEST_BUFFER_COUNT; ++i) {
        buffers[i] = pool.Acquire();
        ASSERT_NE(buffers[i], nullptr);
    }
    EXPECT_EQ(pool.FreeCount(), 0u);

    for (auto& buf : buffers) {
        pool.Release(buf);
    }
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT);

    // 第二轮：再次取光，验证复用正常
    for (uint32_t i = 0; i < TEST_BUFFER_COUNT; ++i) {
        buffers[i] = pool.Acquire();
        ASSERT_NE(buffers[i], nullptr);
    }
    EXPECT_EQ(pool.FreeCount(), 0u);

    for (auto& buf : buffers) {
        pool.Release(buf);
    }
}

TEST(BufferPoolTest, AcquireReleaseSingleReuse) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, 1);

    // 只有 1 个缓冲区，反复获取-归还
    for (int round = 0; round < 100; ++round) {
        char* buf = pool.Acquire();
        ASSERT_NE(buf, nullptr) << "Failed at round " << round;
        EXPECT_EQ(pool.FreeCount(), 0u);

        // 写入数据
        std::memset(buf, 0xAB, TEST_BUFFER_SIZE);

        pool.Release(buf);
        EXPECT_EQ(pool.FreeCount(), 1u);
    }
}

// ============================================================
// 6. 实际 1MB 缓冲区大小测试（模拟真实使用场景）
// ============================================================
TEST(BufferPoolTest, RealSizeBuffer) {
    BufferPool pool;
    constexpr size_t REAL_SIZE = CABE_VALUE_DATA_SIZE;  // 1MB
    constexpr uint32_t REAL_COUNT = 2;

    int32_t ret = pool.Init(REAL_SIZE, REAL_COUNT);
    EXPECT_EQ(ret, SUCCESS);
    EXPECT_EQ(pool.BufferSize(), REAL_SIZE);

    char* buf1 = pool.Acquire();
    char* buf2 = pool.Acquire();
    ASSERT_NE(buf1, nullptr);
    ASSERT_NE(buf2, nullptr);

    // 验证 512 对齐
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf1) % 512, 0u);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(buf2) % 512, 0u);

    // 验证两个缓冲区不重叠（间距应为 1MB）
    auto diff = static_cast<size_t>(std::abs(buf2 - buf1));
    EXPECT_EQ(diff, REAL_SIZE);

    // 写满 1MB 验证不越界
    std::memset(buf1, 0xFF, REAL_SIZE);
    std::memset(buf2, 0xAA, REAL_SIZE);

    // 验证内容互不影响
    EXPECT_EQ(static_cast<unsigned char>(buf1[0]), 0xFF);
    EXPECT_EQ(static_cast<unsigned char>(buf2[0]), 0xAA);

    pool.Release(buf1);
    pool.Release(buf2);
}

// ============================================================
// 7. RAII — 析构函数自动清理
// ============================================================
TEST(BufferPoolTest, DestructorCleansUp) {
    // 作用域结束后，析构函数应自动调用 Destroy()
    // 不崩溃即为通过
    {
        BufferPool pool;
        pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);
        pool.Acquire();  // 故意不归还
    }
    SUCCEED();
}

TEST(BufferPoolTest, DestructorOnUninitializedPool) {
    // 未初始化的池，析构也不应崩溃
    {
        BufferPool pool;
    }
    SUCCEED();
}

// ============================================================
// 8. 计数器一致性
// ============================================================
TEST(BufferPoolTest, CountersConsistency) {
    BufferPool pool;
    pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT);

    EXPECT_EQ(pool.TotalCount(), TEST_BUFFER_COUNT);
    EXPECT_EQ(pool.FreeCount() + pool.UsedCount(), pool.TotalCount());

    char* buf1 = pool.Acquire();
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT - 1);
    EXPECT_EQ(pool.UsedCount(), 1u);
    EXPECT_EQ(pool.FreeCount() + pool.UsedCount(), pool.TotalCount());

    char* buf2 = pool.Acquire();
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT - 2);
    EXPECT_EQ(pool.UsedCount(), 2u);
    EXPECT_EQ(pool.FreeCount() + pool.UsedCount(), pool.TotalCount());

    pool.Release(buf1);
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT - 1);
    EXPECT_EQ(pool.UsedCount(), 1u);

    pool.Release(buf2);
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT);
    EXPECT_EQ(pool.UsedCount(), 0u);
}

// Double-release 检测：同一 buffer 释放两次必须被拒绝，
// 否则后续两个 Acquire 会拿到同一块内存 → 静默数据损坏
TEST(BufferPoolTest, DoubleReleaseFails) {
    BufferPool pool;
    ASSERT_EQ(SUCCESS, pool.Init(TEST_BUFFER_SIZE, TEST_BUFFER_COUNT));

    char* p = pool.Acquire();
    ASSERT_NE(nullptr, p);
    EXPECT_EQ(pool.UsedCount(), 1u);

    EXPECT_EQ(SUCCESS, pool.Release(p));
    EXPECT_EQ(pool.UsedCount(), 0u);
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT);

    // 第二次释放同一指针应当被拒绝
    EXPECT_EQ(POOL_INVALID_POINTER, pool.Release(p));

    // freeStack 不应因为 double-release 膨胀
    EXPECT_EQ(pool.FreeCount(), TEST_BUFFER_COUNT);
    EXPECT_EQ(pool.UsedCount(), 0u);

    // 验证被拒绝后池依然可用：连续 Acquire 四次应得到四个不同指针
    std::set<char*> seen;
    for (uint32_t i = 0; i < TEST_BUFFER_COUNT; ++i) {
        char* q = pool.Acquire();
        ASSERT_NE(nullptr, q);
        EXPECT_TRUE(seen.insert(q).second) << "double-handout after double-release";
    }
    EXPECT_EQ(pool.UsedCount(), TEST_BUFFER_COUNT);
}