// Thin OS helpers used across the engine. Implemented per platform in
// os_windows.cpp / os_posix.cpp.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace mixit::os {

// OS thread id of the calling thread (cached per thread; no syscall after
// the first call on a thread).
std::uint64_t current_thread_id() noexcept;

// Best-effort thread name for debuggers / profilers. Failure is ignored.
void set_current_thread_name(const char* name) noexcept;

// Sleeps until the engine master clock (mixit::monotonic_ns) reaches
// target_ns. Returns immediately if it already has. On Windows this uses a
// high-resolution waitable timer (Windows 10 1803+) so pacing is not limited
// to the 15.6 ms default timer tick; older systems fall back to sleep_until.
void sleep_until_monotonic_ns(std::uint64_t target_ns) noexcept;

// Opens a file for appending in binary mode from a UTF-8 path.
// Returns nullptr on failure and stores a description in `error`.
std::FILE* open_append_utf8(const std::string& path_utf8, std::string& error);

}  // namespace mixit::os
