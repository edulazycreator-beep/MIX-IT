#include "media/time_base.hpp"

#include <limits>


namespace mixit::media {
namespace {

struct U128 {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
};

// Portable 64x64 -> 128-bit multiply from 32-bit halves: identical on every
// compiler (no __int128 / _umul128 differences, clean under -Wpedantic).
U128 mul_64x64(std::uint64_t a, std::uint64_t b) noexcept {
    constexpr std::uint64_t kMask = 0xFFFF'FFFFu;
    const std::uint64_t a_lo = a & kMask, a_hi = a >> 32;
    const std::uint64_t b_lo = b & kMask, b_hi = b >> 32;
    const std::uint64_t lo_lo = a_lo * b_lo;
    const std::uint64_t hi_lo = a_hi * b_lo;
    const std::uint64_t lo_hi = a_lo * b_hi;
    const std::uint64_t hi_hi = a_hi * b_hi;
    const std::uint64_t cross = (lo_lo >> 32) + (hi_lo & kMask) + lo_hi;  // cannot overflow
    U128 r;
    r.lo = (cross << 32) | (lo_lo & kMask);
    r.hi = hi_hi + (hi_lo >> 32) + (cross >> 32);
    return r;
}

// 128/64 division; caller guarantees n.hi < d (quotient fits 64 bits).
// Portable shift-subtract long division: deterministic and identical on
// every compiler (MSVC's _udiv128 faults on overflow instead of reporting).
std::uint64_t div_128_by_64(U128 n, std::uint64_t d, std::uint64_t& remainder) noexcept {
    std::uint64_t q = 0;
    std::uint64_t r = n.hi;
    for (int i = 63; i >= 0; --i) {
        const bool carry = (r >> 63) != 0;
        r = (r << 1) | ((n.lo >> i) & 1u);
        q <<= 1;
        if (carry || r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    remainder = r;
    return q;
}

}  // namespace

std::optional<std::uint64_t> muldiv_u64(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                        Rounding rounding) noexcept {
    if (c == 0) return std::nullopt;
    const U128 p = mul_64x64(a, b);
    if (p.hi >= c) return std::nullopt;  // quotient would not fit in 64 bits
    std::uint64_t rem = 0;
    std::uint64_t q = div_128_by_64(p, c, rem);
    bool round_up = false;
    switch (rounding) {
        case Rounding::Down: break;
        case Rounding::Up: round_up = rem != 0; break;
        case Rounding::Nearest: round_up = rem >= c - rem; break;  // rem >= c/2 without overflow
    }
    if (round_up) {
        if (q == std::numeric_limits<std::uint64_t>::max()) return std::nullopt;
        ++q;
    }
    return q;
}

std::optional<std::int64_t> muldiv_i64(std::int64_t a, std::int64_t b, std::int64_t c,
                                       Rounding rounding) noexcept {
    if (b <= 0 || c <= 0) return std::nullopt;
    const bool negative = a < 0;
    // |a| as unsigned without overflow for INT64_MIN.
    const std::uint64_t mag = negative ? (~static_cast<std::uint64_t>(a) + 1u) : static_cast<std::uint64_t>(a);
    // For negative values, floor on the signed axis is ceil on the magnitude.
    Rounding mag_rounding = rounding;
    if (negative) {
        if (rounding == Rounding::Down) mag_rounding = Rounding::Up;
        else if (rounding == Rounding::Up) mag_rounding = Rounding::Down;
    }
    const auto q = muldiv_u64(mag, static_cast<std::uint64_t>(b), static_cast<std::uint64_t>(c), mag_rounding);
    if (!q) return std::nullopt;
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!negative) {
        if (*q > kMax) return std::nullopt;
        return static_cast<std::int64_t>(*q);
    }
    if (*q > kMax + 1u) return std::nullopt;
    return *q == kMax + 1u ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(*q);
}

std::int64_t time_of_index(std::int64_t index, Rate rate) noexcept {
    // t = index * den * 1e9 / num, rounded to the nearest nanosecond.
    const auto t = muldiv_i64(index, static_cast<std::int64_t>(rate.den) * kNanosPerSecond, rate.num,
                              Rounding::Nearest);
    if (t) return *t;
    return index < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
}

std::int64_t index_at_time(std::int64_t time_ns, Rate rate) noexcept {
    // Largest i with time_of_index(i) <= t. Start from the exact floor and
    // correct for the nearest-rounding used by time_of_index.
    const auto approx = muldiv_i64(time_ns, rate.num, static_cast<std::int64_t>(rate.den) * kNanosPerSecond,
                                   Rounding::Down);
    if (!approx) return time_ns < 0 ? std::numeric_limits<std::int64_t>::min() : std::numeric_limits<std::int64_t>::max();
    std::int64_t i = *approx;
    while (time_of_index(i + 1, rate) <= time_ns) ++i;
    while (time_of_index(i, rate) > time_ns) --i;
    return i;
}

std::int64_t nearest_index(std::int64_t time_ns, Rate rate) noexcept {
    const std::int64_t i = index_at_time(time_ns, rate);
    const std::int64_t before = time_ns - time_of_index(i, rate);
    const std::int64_t after = time_of_index(i + 1, rate) - time_ns;
    return after < before ? i + 1 : i;
}

std::int64_t duration_of_index(std::int64_t index, Rate rate) noexcept {
    return time_of_index(index + 1, rate) - time_of_index(index, rate);
}

bool is_supported_frame_rate(Rate rate) noexcept {
    constexpr Rate kSupported[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
                                   {50, 1},       {60000, 1001}, {60, 1}};
    for (const Rate r : kSupported) {
        // Compare as fractions so 60/1 == 120/2.
        if (static_cast<std::uint64_t>(r.num) * rate.den == static_cast<std::uint64_t>(rate.num) * r.den &&
            rate.valid()) {
            return true;
        }
    }
    return false;
}

}  // namespace mixit::media
