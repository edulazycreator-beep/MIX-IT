// Exact media time arithmetic.
//
// Rules (docs/ENGINE_DESIGN.md §3):
//   * Media time is int64 nanoseconds on the engine master clock.
//   * Rates (frame rates, sample rates) are exact rationals, e.g. 60000/1001.
//   * The time of item n is ALWAYS computed from n (n * den * 1e9 / num) with
//     a 128-bit intermediate, never by accumulating a rounded period. So the
//     error at frame 10^9 is the same (< 1 ns) as at frame 1: no drift.
#pragma once

#include <cstdint>
#include <optional>

namespace mixit::media {

enum class Rounding { Down, Nearest, Up };

// a * b / c for unsigned 64-bit values with an exact 128-bit intermediate.
// Returns nullopt if c == 0 or the result does not fit in 64 bits.
std::optional<std::uint64_t> muldiv_u64(std::uint64_t a, std::uint64_t b, std::uint64_t c,
                                        Rounding rounding = Rounding::Down) noexcept;

// Signed variant: a may be negative; b > 0, c > 0. Rounding::Down means
// toward negative infinity (floor).
std::optional<std::int64_t> muldiv_i64(std::int64_t a, std::int64_t b, std::int64_t c,
                                       Rounding rounding = Rounding::Down) noexcept;

inline constexpr std::int64_t kNanosPerSecond = 1'000'000'000;

// An exact positive rational rate in units per second (frames/s, samples/s).
struct Rate {
    std::uint32_t num = 0;
    std::uint32_t den = 1;

    [[nodiscard]] constexpr bool valid() const noexcept { return num > 0 && den > 0; }
    [[nodiscard]] double to_double() const noexcept { return static_cast<double>(num) / den; }
    friend constexpr bool operator==(Rate, Rate) = default;
};

// Time (ns) at which item `index` starts, relative to the stream origin.
std::int64_t time_of_index(std::int64_t index, Rate rate) noexcept;

// Index of the item whose interval [t(i), t(i+1)) contains `time_ns`
// (floor). For negative times returns negative indices.
std::int64_t index_at_time(std::int64_t time_ns, Rate rate) noexcept;

// Index of the item whose start time is nearest to `time_ns`.
std::int64_t nearest_index(std::int64_t time_ns, Rate rate) noexcept;

// Duration (ns) of item `index` (varies by ±1 ns for non-integer periods,
// so that durations always sum exactly to time_of_index differences).
std::int64_t duration_of_index(std::int64_t index, Rate rate) noexcept;

// Frame rates MIX IT supports for production formats (master prompt §6).
// Returns false for anything else.
bool is_supported_frame_rate(Rate rate) noexcept;

}  // namespace mixit::media
