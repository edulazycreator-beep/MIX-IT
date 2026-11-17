#if !defined(_WIN32)

#include "platform/os.hpp"

#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <thread>

#include "util/clock.hpp"
#include <cstring>

namespace mixit::os {

std::uint64_t current_thread_id() noexcept {
    thread_local const std::uint64_t id = static_cast<std::uint64_t>(::syscall(SYS_gettid));
    return id;
}

void set_current_thread_name(const char* name) noexcept {
    if (name == nullptr) return;
    char truncated[16] = {};  // Linux limit: 15 chars + NUL
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    (void)::pthread_setname_np(::pthread_self(), truncated);
}

void sleep_until_monotonic_ns(std::uint64_t target_ns) noexcept {
    // steady_clock is CLOCK_MONOTONIC on Linux; sleep_until uses an absolute
    // clock_nanosleep, so wake-up time does not drift with loop overhead.
    const std::chrono::steady_clock::time_point target{std::chrono::nanoseconds(static_cast<std::int64_t>(target_ns))};
    std::this_thread::sleep_until(target);
}

std::FILE* open_append_utf8(const std::string& path_utf8, std::string& error) {
    std::FILE* f = std::fopen(path_utf8.c_str(), "ab");
    if (f == nullptr) {
        error = std::strerror(errno);
    }
    return f;
}

}  // namespace mixit::os

#endif
