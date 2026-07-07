#include "io/uring/io_uring_backend.h"
#include "engine/buffer_pool.h"
#include "engine/value_buffer_pool.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace {

std::string GetTestDevice() {
    const char* dev = std::getenv("CABE_TEST_DEVICE");
    return dev ? std::string(dev) : "";
}

cabe::IoWriteBuffer MakeWriteBuffer(const std::byte* data,
                                     cabe::IoWriteBufferKind kind = cabe::IoWriteBufferKind::ExternalMemory) {
    return cabe::IoWriteBuffer{
        .data = data,
        .size = cabe::kValueSize,
        .kind = kind,
    };
}

std::vector<cabe::ValueBufferSlotView> ExportViews(const std::shared_ptr<cabe::ValueBufferPool>& pool) {
    std::vector<cabe::ValueBufferSlotView> views(pool->slot_count());
    const std::size_t exported = pool->ExportSlotViews(views);
    views.resize(exported);
    return views;
}

} // namespace

class IoUringBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = GetTestDevice();
        if (device_.empty()) GTEST_SKIP() << "CABE_TEST_DEVICE 未设置";
    }
    void TearDown() override {
        backend_.Close();
    }
    cabe::IoUringIoBackend backend_;
    std::string device_;
};

TEST_F(IoUringBackendTest, OpenCloseNormal) {
    EXPECT_FALSE(backend_.is_open());
    EXPECT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_TRUE(backend_.is_open());
    EXPECT_EQ(backend_.Close(), cabe::err::kSuccess);
    EXPECT_FALSE(backend_.is_open());
}

TEST_F(IoUringBackendTest, DoubleOpenFails) {
    EXPECT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_NE(backend_.Open(device_), cabe::err::kSuccess);
}

TEST_F(IoUringBackendTest, CloseIdempotent) {
    EXPECT_EQ(backend_.Close(), cabe::err::kSuccess);
}

TEST_F(IoUringBackendTest, BlockCountCorrect) {
    EXPECT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_GT(backend_.BlockCount(), 0u);
}

TEST_F(IoUringBackendTest, WriteReadRoundTrip) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);

    cabe::BufferPool pool(2);
    auto* wbuf = pool.Allocate();
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0xAB, cabe::kValueSize);

    EXPECT_EQ(backend_.Write(0, MakeWriteBuffer(wbuf)), cabe::err::kSuccess);

    auto* rbuf = pool.Allocate();
    ASSERT_NE(rbuf, nullptr);
    std::memset(rbuf, 0, cabe::kValueSize);

    EXPECT_EQ(backend_.Read(0, rbuf), cabe::err::kSuccess);
    EXPECT_EQ(std::memcmp(wbuf, rbuf, cabe::kValueSize), 0);
    pool.Free(wbuf);
    pool.Free(rbuf);
}

TEST_F(IoUringBackendTest, WriteReadMultipleBlocks) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    cabe::BufferPool pool(4);

    for (std::uint64_t i = 0; i < 4; ++i) {
        auto* wbuf = pool.Allocate();
        ASSERT_NE(wbuf, nullptr);
        std::memset(wbuf, static_cast<int>(0x10 + i), cabe::kValueSize);
        EXPECT_EQ(backend_.Write(i, MakeWriteBuffer(wbuf)), cabe::err::kSuccess);
        pool.Free(wbuf);
    }

    for (std::uint64_t i = 0; i < 4; ++i) {
        auto* rbuf = pool.Allocate();
        ASSERT_NE(rbuf, nullptr);
        EXPECT_EQ(backend_.Read(i, rbuf), cabe::err::kSuccess);
        EXPECT_EQ(static_cast<unsigned char>(rbuf[0]), 0x10 + i);
        pool.Free(rbuf);
    }
}

TEST_F(IoUringBackendTest, DestructorAutoCloses) {
    {
        cabe::IoUringIoBackend tmp;
        EXPECT_EQ(tmp.Open(device_), cabe::err::kSuccess);
    }
}

TEST_F(IoUringBackendTest, MoveConstruct) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    auto count = backend_.BlockCount();

    cabe::IoUringIoBackend moved(std::move(backend_));
    EXPECT_TRUE(moved.is_open());
    EXPECT_EQ(moved.BlockCount(), count);
    EXPECT_FALSE(backend_.is_open());
    moved.Close();
}

TEST_F(IoUringBackendTest, ConceptSatisfied) {
    static_assert(cabe::IoBackend<cabe::IoUringIoBackend>);
}

TEST_F(IoUringBackendTest, RegisterWriteBuffersZeroCapacity) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);
    EXPECT_EQ(backend_.RegisterWriteBuffers(std::span<const cabe::ValueBufferSlotView>{}),
              cabe::err::kSuccess);
}

TEST_F(IoUringBackendTest, RegisterWriteBuffersRejectsRepeat) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);

    auto created = cabe::ValueBufferPool::Create(0, 7, 1);
    ASSERT_TRUE(created.ok()) << created.status.code;
    auto views = ExportViews(created.pool);
    ASSERT_EQ(views.size(), 1u);

    ASSERT_EQ(backend_.RegisterWriteBuffers(std::span<const cabe::ValueBufferSlotView>{views}),
              cabe::err::kSuccess);
    EXPECT_EQ(backend_.RegisterWriteBuffers(std::span<const cabe::ValueBufferSlotView>{views}),
              cabe::err::kIoBase);
}

TEST_F(IoUringBackendTest, FixedValueBufferWriteReadRoundTrip) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);

    auto created = cabe::ValueBufferPool::Create(0, 9, 2);
    ASSERT_TRUE(created.ok()) << created.status.code;
    auto views = ExportViews(created.pool);
    ASSERT_EQ(views.size(), 2u);
    ASSERT_EQ(backend_.RegisterWriteBuffers(std::span<const cabe::ValueBufferSlotView>{views}),
              cabe::err::kSuccess);

    auto value = created.pool->Allocate("fixed");
    ASSERT_TRUE(value.ok()) << value.status.code;
    std::memset(value.buffer.data().data(), 0xEE, cabe::kValueSize);
    const auto info = created.pool->Identify(value.buffer.view());
    ASSERT_TRUE(info.matched);

    cabe::IoWriteBuffer fixed{
        .data = value.buffer.view().data(),
        .size = cabe::kValueSize,
        .kind = cabe::IoWriteBufferKind::ValueBufferSlot,
        .device_id = info.device_id,
        .pool_id = info.pool_id,
        .slot_index = info.slot_index,
        .slot_generation = info.slot_generation,
    };
    ASSERT_EQ(backend_.Write(0, fixed), cabe::err::kSuccess);

    cabe::BufferPool pool(1);
    auto* rbuf = pool.Allocate();
    ASSERT_NE(rbuf, nullptr);
    ASSERT_EQ(backend_.Read(0, rbuf), cabe::err::kSuccess);
    EXPECT_EQ(rbuf[0], std::byte{0xEE});
    EXPECT_EQ(rbuf[cabe::kValueSize - 1], std::byte{0xEE});
    pool.Free(rbuf);
}

TEST_F(IoUringBackendTest, CopyFallbackBufferStaysPlainWriteWithRegisteredBuffers) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);

    auto created = cabe::ValueBufferPool::Create(0, 10, 1);
    ASSERT_TRUE(created.ok()) << created.status.code;
    auto views = ExportViews(created.pool);
    ASSERT_EQ(backend_.RegisterWriteBuffers(std::span<const cabe::ValueBufferSlotView>{views}),
              cabe::err::kSuccess);

    cabe::BufferPool pool(2);
    auto* wbuf = pool.Allocate();
    ASSERT_NE(wbuf, nullptr);
    std::memset(wbuf, 0x5A, cabe::kValueSize);
    ASSERT_EQ(backend_.Write(0, MakeWriteBuffer(wbuf, cabe::IoWriteBufferKind::CopyFallbackBuffer)),
              cabe::err::kSuccess);

    auto* rbuf = pool.Allocate();
    ASSERT_NE(rbuf, nullptr);
    ASSERT_EQ(backend_.Read(0, rbuf), cabe::err::kSuccess);
    EXPECT_EQ(rbuf[0], std::byte{0x5A});
    EXPECT_EQ(rbuf[cabe::kValueSize - 1], std::byte{0x5A});
    pool.Free(wbuf);
    pool.Free(rbuf);
}

TEST_F(IoUringBackendTest, MoveConstructWithRegisteredBuffers) {
    ASSERT_EQ(backend_.Open(device_), cabe::err::kSuccess);

    auto created = cabe::ValueBufferPool::Create(0, 11, 1);
    ASSERT_TRUE(created.ok()) << created.status.code;
    auto views = ExportViews(created.pool);
    ASSERT_EQ(backend_.RegisterWriteBuffers(std::span<const cabe::ValueBufferSlotView>{views}),
              cabe::err::kSuccess);

    cabe::IoUringIoBackend moved(std::move(backend_));
    EXPECT_TRUE(moved.is_open());
    EXPECT_FALSE(backend_.is_open());

    auto value = created.pool->Allocate("moved");
    ASSERT_TRUE(value.ok()) << value.status.code;
    std::memset(value.buffer.data().data(), 0x6B, cabe::kValueSize);
    const auto info = created.pool->Identify(value.buffer.view());
    ASSERT_TRUE(info.matched);
    cabe::IoWriteBuffer fixed{
        .data = value.buffer.view().data(),
        .size = cabe::kValueSize,
        .kind = cabe::IoWriteBufferKind::ValueBufferSlot,
        .device_id = info.device_id,
        .pool_id = info.pool_id,
        .slot_index = info.slot_index,
        .slot_generation = info.slot_generation,
    };
    EXPECT_EQ(moved.Write(0, fixed), cabe::err::kSuccess);
    moved.Close();
}

TEST(IoUringBackendNoDevice, OpenBadPath) {
    cabe::IoUringIoBackend backend;
    EXPECT_NE(backend.Open("/no/such/device"), cabe::err::kSuccess);
}

TEST(IoUringBackendNoDevice, RejectsInvalidWriteBuffer) {
    cabe::IoUringIoBackend backend;
    cabe::IoWriteBuffer invalid{};
    EXPECT_EQ(backend.Write(0, invalid), cabe::err::kIoBase);
}
