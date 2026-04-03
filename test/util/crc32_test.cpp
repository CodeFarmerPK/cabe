/*
 * Project: Cabe
 * Created Time: 2026-04-03
 * Created by: CodeFarmerPK
 *
 * CRC32 单元测试
 */

#include <gtest/gtest.h>
#include "util/crc32.h"
#include <vector>

TEST(CRC32Test, EmptyDataDeterministic) {
    std::vector<char> empty;
    uint32_t crc1 = cabe::util::CRC32({empty.data(), 0});
    uint32_t crc2 = cabe::util::CRC32({empty.data(), 0});
    EXPECT_EQ(crc1, crc2);
}

TEST(CRC32Test, SameDataSameCRC) {
    const char data[] = "hello cabe storage engine";
    uint32_t crc1 = cabe::util::CRC32({data, sizeof(data) - 1});
    uint32_t crc2 = cabe::util::CRC32({data, sizeof(data) - 1});
    EXPECT_EQ(crc1, crc2);
}

TEST(CRC32Test, DifferentDataDifferentCRC) {
    const char data1[] = "hello";
    const char data2[] = "world";
    uint32_t crc1 = cabe::util::CRC32({data1, 5});
    uint32_t crc2 = cabe::util::CRC32({data2, 5});
    EXPECT_NE(crc1, crc2);
}

TEST(CRC32Test, SingleByteDifferenceChangesCRC) {
    std::vector<char> data1(64, 'A');
    std::vector<char> data2(data1);
    data2[32] = 'B';

    uint32_t crc1 = cabe::util::CRC32({data1.data(), data1.size()});
    uint32_t crc2 = cabe::util::CRC32({data2.data(), data2.size()});
    EXPECT_NE(crc1, crc2);
}

TEST(CRC32Test, LargeBlockConsistency) {
    constexpr size_t size = CABE_VALUE_DATA_SIZE;  // 1MB
    std::vector<char> data(size, '\x42');

    uint32_t crc1 = cabe::util::CRC32({data.data(), data.size()});
    uint32_t crc2 = cabe::util::CRC32({data.data(), data.size()});
    EXPECT_EQ(crc1, crc2);
    EXPECT_NE(0u, crc1);
}

TEST(CRC32Test, AllZerosNonZeroCRC) {
    std::vector<char> zeros(CABE_VALUE_DATA_SIZE, 0);
    uint32_t crc = cabe::util::CRC32({zeros.data(), zeros.size()});
    EXPECT_NE(0u, crc);
}
