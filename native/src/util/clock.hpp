// Engine clocks.
//
// monotonic_ns(): the engine master clock. All media timestamps will be
// expressed on this clock (see docs/ENGINE_DESIGN.md, "Time base"). It is
// steady (never jumps), so it is safe for durations, pacing and A/V sync.
// On Windows, std::chrono::steady_clock is backed by QueryPerformanceCounter;
// on Linux by CLOCK_MONOTONIC.
//
// unix_time_ns(): wall-clock time, only for human-facing timestamps (logs,
// file names). Never use it for media timing: it can jump (NTP, DST, user).
#pragma once

#include <chrono>
#include <cstdint>

namespace mixit {

inline std::uint64_t monotonic_ns() noexcept {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

inline std::int64_t unix_time_ns() noexcept {
    using namespace std::chrono;
    return static_cast<std::int64_t>(
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count());
}

}  // namespace mixit
