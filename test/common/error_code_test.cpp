#include "common/error_code.h"

#include <gtest/gtest.h>

namespace e = cabe::err;

TEST(ErrorCode, MemorySegmentValues) {
    EXPECT_EQ(e::kSuccess, 0);
    EXPECT_EQ(e::kMemNullPointer, -100000);
    EXPECT_EQ(e::kMemEmptyKey, -100001);
    EXPECT_EQ(e::kMemEmptyValue, -100002);
    EXPECT_EQ(e::kMemInsertFail, -100003);
}

TEST(ErrorCode, SegmentsContiguousAndNonOverlapping) {
    EXPECT_EQ(e::kMemoryBase - e::kSegmentSize, e::kIoBase);
    EXPECT_EQ(e::kIoBase - e::kSegmentSize, e::kIndexBase);
    EXPECT_EQ(e::kIndexBase - e::kSegmentSize, e::kWalBase);
    EXPECT_EQ(e::kWalBase - e::kSegmentSize, e::kEngineBase);
    EXPECT_EQ(e::kEngineBase - e::kSegmentSize, e::kWalRecoveryBase);
    EXPECT_EQ(e::kWalRecoveryBase - e::kSegmentSize, e::kSnapshotBase);
    EXPECT_EQ(e::kSnapshotBase - e::kSegmentSize, e::kSpdkBase);
}

TEST(ErrorCode, MemoryCodesWithinSegment) {
    EXPECT_LE(e::kMemInsertFail, e::kMemoryBase);
    EXPECT_GT(e::kMemInsertFail, e::kMemoryBase - e::kSegmentSize);
}

TEST(ErrorCode, SpdkSegmentValues) {
    EXPECT_EQ(e::kSpdkBase, -107000);
    EXPECT_EQ(e::kSpdkInvalidConfig, -107000);
    EXPECT_EQ(e::kSpdkNamespaceOverlap, -107001);
    EXPECT_LE(e::kSpdkNamespaceOverlap, e::kSpdkBase);
    EXPECT_GT(e::kSpdkNamespaceOverlap, e::kSpdkBase - e::kSegmentSize);
}
