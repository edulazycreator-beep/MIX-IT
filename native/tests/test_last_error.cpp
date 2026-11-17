#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>

#include "core/last_error.hpp"

using namespace mixit;

TEST(LastError, RoundTripsAndReportsRequiredSize) {
    set_last_error("fn", "boom");
    std::uint32_t required = 0;
    EXPECT_EQ(copy_last_error(nullptr, 0, &required), MIXIT_E_BUFFER_TOO_SMALL);
    EXPECT_EQ(required, std::strlen("fn: boom") + 1);

    char buf[64];
    EXPECT_EQ(copy_last_error(buf, sizeof(buf), &required), MIXIT_OK);
    EXPECT_STREQ(buf, "fn: boom");
}

TEST(LastError, TruncatesIntoSmallBuffer) {
    set_last_error("fn", "a long message");
    char buf[5];
    EXPECT_EQ(copy_last_error(buf, sizeof(buf), nullptr), MIXIT_E_BUFFER_TOO_SMALL);
    EXPECT_STREQ(buf, "fn: ");
}

TEST(LastError, NullBufferWithCapacityIsInvalid) {
    EXPECT_EQ(copy_last_error(nullptr, 10, nullptr), MIXIT_E_INVALID_ARGUMENT);
}

TEST(LastError, IsPerThread) {
    set_last_error("main", "main-thread error");
    std::string other;
    std::thread([&] {
        set_last_error("worker", "worker error");
        char buf[64];
        (void)copy_last_error(buf, sizeof(buf), nullptr);
        other = buf;
    }).join();
    char buf[64];
    ASSERT_EQ(copy_last_error(buf, sizeof(buf), nullptr), MIXIT_OK);
    EXPECT_STREQ(buf, "main: main-thread error");
    EXPECT_EQ(other, "worker: worker error");
}

TEST(Utf8Truncation, NeverSplitsMultibyteSequence) {
    // "ab" + U+20AC (3 bytes: E2 82 AC)
    const std::string text = "ab\xE2\x82\xAC";
    char buf[5];  // room for 4 bytes: would cut the euro sign after 2 of its 3 bytes
    EXPECT_EQ(copy_utf8_truncated(buf, sizeof(buf), text), 2u);
    EXPECT_STREQ(buf, "ab");

    char exact[6];
    EXPECT_EQ(copy_utf8_truncated(exact, sizeof(exact), text), 5u);
    EXPECT_EQ(std::string(exact), text);
}
