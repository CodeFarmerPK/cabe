#include "engine/status.h"

#include <gtest/gtest.h>

TEST(Status, OkDefault) {
    const cabe::Status s{};
    EXPECT_TRUE(s.ok());
    EXPECT_TRUE(static_cast<bool>(s));
    EXPECT_EQ(s.code, cabe::err::kSuccess);
}

TEST(Status, OkFactory) {
    const auto s = cabe::Status::Ok();
    EXPECT_TRUE(s.ok());
    EXPECT_EQ(s, cabe::Status{});
}

TEST(Status, ErrorFactory) {
    const auto s = cabe::Status::Error(cabe::err::kMemEmptyKey);
    EXPECT_FALSE(s.ok());
    EXPECT_FALSE(static_cast<bool>(s));
    EXPECT_EQ(s.code, cabe::err::kMemEmptyKey);
}

TEST(Status, Comparison) {
    EXPECT_EQ(cabe::Status::Ok(), cabe::Status::Ok());
    EXPECT_NE(cabe::Status::Ok(), cabe::Status::Error(cabe::err::kMemEmptyKey));
    EXPECT_NE(cabe::Status::Error(cabe::err::kMemEmptyKey),
              cabe::Status::Error(cabe::err::kMemEmptyValue));
}

TEST(Status, TriviallyCopyable) {
    static_assert(std::is_trivially_copyable_v<cabe::Status>);
    static_assert(sizeof(cabe::Status) == sizeof(int));
}
