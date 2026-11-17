#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <random>

#include "media/time_base.hpp"

using namespace mixit::media;

TEST(TimeBase, MulDivSmallValuesMatchPlainArithmetic) {
    std::mt19937_64 rng(42);
    for (int i = 0; i < 100000; ++i) {
        const std::uint64_t a = rng() % 4'000'000'000ull;
        const std::uint64_t b = rng() % 4'000'000'000ull;
        const std::uint64_t c = rng() % 1'000'000ull + 1;
        const std::uint64_t p = a * b;  // < 1.6e19 fits in uint64
        ASSERT_EQ(muldiv_u64(a, b, c, Rounding::Down), p / c);
        ASSERT_EQ(muldiv_u64(a, b, c, Rounding::Up), p / c + (p % c != 0 ? 1 : 0));
        ASSERT_EQ(muldiv_u64(a, b, c, Rounding::Nearest), p / c + (p % c >= c - p % c ? 1 : 0));
    }
}

TEST(TimeBase, MulDivUsesFull128BitIntermediate) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(muldiv_u64(kMax, kMax, kMax), kMax);
    EXPECT_EQ(muldiv_u64(std::uint64_t{1} << 63, 2, 4), std::uint64_t{1} << 62);
    EXPECT_EQ(muldiv_u64(kMax, 3, 6), kMax / 2);  // product needs 66 bits
    EXPECT_EQ(muldiv_u64(1'000'000'007ull * 1'000'000'009ull, 1'000'000'007ull, 1'000'000'007ull),
              1'000'000'007ull * 1'000'000'009ull);
}

TEST(TimeBase, MulDivReportsOverflowAndDivisionByZero) {
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_FALSE(muldiv_u64(kMax, 2, 1).has_value());
    EXPECT_FALSE(muldiv_u64(1, 1, 0).has_value());
    EXPECT_EQ(muldiv_u64(kMax, 1, 1, Rounding::Up), kMax);  // exact: no rounding needed
}

TEST(TimeBase, SignedMulDivFloorsTowardNegativeInfinity) {
    EXPECT_EQ(muldiv_i64(-7, 1, 2, Rounding::Down), -4);
    EXPECT_EQ(muldiv_i64(-7, 1, 2, Rounding::Up), -3);
    EXPECT_EQ(muldiv_i64(-6, 1, 2, Rounding::Down), -3);
    EXPECT_EQ(muldiv_i64(7, 1, 2, Rounding::Nearest), 4);
    EXPECT_EQ(muldiv_i64(std::numeric_limits<std::int64_t>::min(), 1, 1), std::numeric_limits<std::int64_t>::min());
    EXPECT_FALSE(muldiv_i64(1, 0, 1).has_value());
}

TEST(TimeBase, FrameTimesForNtscRates) {
    const Rate r{30000, 1001};
    EXPECT_EQ(time_of_index(0, r), 0);
    EXPECT_EQ(time_of_index(1, r), 33'366'667);           // 33366666.67 ns, nearest
    EXPECT_EQ(time_of_index(30000, r), 1'001'000'000'000);  // exactly 1001 s
    EXPECT_EQ(time_of_index(-1, r), -33'366'667);
}

// The core no-drift guarantee: frame n's time is computed from n, so the
// value at n = 10^9 (over a year at 29.97 fps) is still exact to the ns.
TEST(TimeBase, NoDriftOverVeryLongRuns) {
    const Rate ntsc{30000, 1001};
    EXPECT_EQ(time_of_index(300'000'000, ntsc), 10'010'000'000'000'000);  // 10,010,000 s exactly
    const Rate r5994{60000, 1001};
    // One hour of 59.94 output vs 48 kHz audio: both land on exact times.
    const std::int64_t hour_frames = 215'784;  // floor(3600 * 60000/1001)
    const std::int64_t t = time_of_index(hour_frames, r5994);
    EXPECT_LE(t, 3'600'000'000'000);
    EXPECT_GT(time_of_index(hour_frames + 1, r5994), 3'600'000'000'000);
    EXPECT_EQ(time_of_index(48'000 * 3600, Rate{48000, 1}), 3'600'000'000'000);
}

TEST(TimeBase, DurationsSumExactly) {
    for (const Rate r : {Rate{30000, 1001}, Rate{60000, 1001}, Rate{24000, 1001}, Rate{25, 1}, Rate{44100, 1}}) {
        std::int64_t sum = 0;
        for (std::int64_t i = 0; i < 100'000; ++i) {
            const std::int64_t d = duration_of_index(i, r);
            ASSERT_GE(d, time_of_index(1, r) - 1);
            ASSERT_LE(d, time_of_index(1, r) + 1);
            sum += d;
        }
        EXPECT_EQ(sum, time_of_index(100'000, r)) << r.num << "/" << r.den;
    }
}

TEST(TimeBase, IndexAtTimeInvertsTimeOfIndex) {
    std::mt19937_64 rng(7);
    for (const Rate r : {Rate{30000, 1001}, Rate{60000, 1001}, Rate{25, 1}, Rate{48000, 1}}) {
        for (int i = 0; i < 20000; ++i) {
            const auto t = static_cast<std::int64_t>(rng() % 100'000'000'000'000ull) - 1'000'000'000'000;
            const std::int64_t idx = index_at_time(t, r);
            ASSERT_LE(time_of_index(idx, r), t);
            ASSERT_GT(time_of_index(idx + 1, r), t);
            const std::int64_t near = nearest_index(t, r);
            ASSERT_TRUE(near == idx || near == idx + 1);
            ASSERT_LE(std::llabs(time_of_index(near, r) - t), duration_of_index(idx, r) / 2 + 1);
        }
    }
}

TEST(TimeBase, SupportedFrameRates) {
    for (const Rate r : {Rate{24000, 1001}, Rate{24, 1}, Rate{25, 1}, Rate{30000, 1001}, Rate{30, 1}, Rate{50, 1},
                         Rate{60000, 1001}, Rate{60, 1}, Rate{120, 2}}) {
        EXPECT_TRUE(is_supported_frame_rate(r)) << r.num << "/" << r.den;
    }
    for (const Rate r : {Rate{29, 1}, Rate{0, 1}, Rate{30, 0}, Rate{120, 1}, Rate{2997, 100}}) {
        EXPECT_FALSE(is_supported_frame_rate(r)) << r.num << "/" << r.den;
    }
}
