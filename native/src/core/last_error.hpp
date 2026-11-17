// Per-thread last-error message, mirroring the errno / GetLastError model.
// Storage is a fixed thread_local buffer: setting an error never allocates.
#pragma once

#include <cstdint>
#include <string_view>

#include "mixit/mixit_engine.h"

namespace mixit {

inline constexpr std::size_t kLastErrorCapacity = 1024;

// Stores "<function>: <message>" for the calling thread (truncated to fit).
void set_last_error(std::string_view function, std::string_view message) noexcept;

// Implements mixit_get_last_error_message for the calling thread.
mixit_result copy_last_error(char* buffer, std::uint32_t capacity, std::uint32_t* out_required) noexcept;

// Copies at most capacity-1 bytes of `text` into `dest`, never splitting a
// UTF-8 sequence, and NUL-terminates. Returns the number of bytes copied.
// Shared by every component that writes strings into fixed ABI buffers.
std::size_t copy_utf8_truncated(char* dest, std::size_t capacity, std::string_view text) noexcept;

}  // namespace mixit
