#include "io/io_write_buffer.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <type_traits>

TEST(IoWriteBuffer, DefaultsArePlainExternalMemory) {
    const cabe::IoWriteBuffer buffer{};
    EXPECT_EQ(buffer.data, nullptr);
    EXPECT_EQ(buffer.size, 0u);
    EXPECT_EQ(buffer.kind, cabe::IoWriteBufferKind::ExternalMemory);
    EXPECT_EQ(buffer.device_id, 0u);
    EXPECT_EQ(buffer.pool_id, 0u);
    EXPECT_EQ(buffer.slot_index, 0u);
    EXPECT_EQ(buffer.slot_generation, 0u);
}

TEST(IoWriteBuffer, AggregatesSlotIdentity) {
    const std::byte data[1]{};
    const cabe::IoWriteBuffer buffer{
        .data = data,
        .size = cabe::kValueSize,
        .kind = cabe::IoWriteBufferKind::ValueBufferSlot,
        .device_id = 3,
        .pool_id = 5,
        .slot_index = 7,
        .slot_generation = 11,
    };

    EXPECT_EQ(buffer.data, data);
    EXPECT_EQ(buffer.size, cabe::kValueSize);
    EXPECT_EQ(buffer.kind, cabe::IoWriteBufferKind::ValueBufferSlot);
    EXPECT_EQ(buffer.device_id, 3u);
    EXPECT_EQ(buffer.pool_id, 5u);
    EXPECT_EQ(buffer.slot_index, 7u);
    EXPECT_EQ(buffer.slot_generation, 11u);
}

TEST(ValueBufferSlotView, IsSimpleBackendNeutralView) {
    static_assert(std::is_trivially_copyable_v<cabe::ValueBufferSlotView>);
    const std::byte data[1]{};
    const cabe::ValueBufferSlotView view{
        .data = data,
        .size = cabe::kValueSize,
        .slot_index = 4,
    };

    EXPECT_EQ(view.data, data);
    EXPECT_EQ(view.size, cabe::kValueSize);
    EXPECT_EQ(view.slot_index, 4u);
}
