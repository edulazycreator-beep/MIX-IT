#include <gtest/gtest.h>

#include <string>

#include "logging/json.hpp"

using namespace mixit::json;

TEST(Json, EscapesSpecialCharacters) {
    std::string out;
    append_escaped(out, "a\"b\\c\nd\te\r\x01");
    EXPECT_EQ(out, "a\\\"b\\\\c\\nd\\te\\r\\u0001");
}

TEST(Json, PassesUtf8Through) {
    std::string out;
    append_escaped(out, "Kamera 1 \xE2\x80\x94 \xE6\x91\x84\xE5\x83\x8F");  // em dash + CJK
    EXPECT_EQ(out, "Kamera 1 \xE2\x80\x94 \xE6\x91\x84\xE5\x83\x8F");
}

TEST(Json, FormatsIso8601Utc) {
    std::string out;
    append_iso8601_utc(out, 0);
    EXPECT_EQ(out, "1970-01-01T00:00:00.000Z");

    out.clear();
    // 2026-11-17T04:05:55.947Z (unix 1794888355.947)
    append_iso8601_utc(out, 1'794'888'355'947'000'000LL);
    EXPECT_EQ(out, "2026-11-17T04:05:55.947Z");

    out.clear();
    append_iso8601_utc(out, 951'782'400'000'000'000LL);  // leap day 2000-02-29
    EXPECT_EQ(out, "2000-02-29T00:00:00.000Z");
}
