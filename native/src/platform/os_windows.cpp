#if defined(_WIN32)

#include "platform/os.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <share.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include "util/clock.hpp"

#include "platform/win_strings.hpp"

namespace mixit::os {

std::uint64_t current_thread_id() noexcept {
    thread_local const std::uint64_t id = static_cast<std::uint64_t>(::GetCurrentThreadId());
    return id;
}

void set_current_thread_name(const char* name) noexcept {
    if (name == nullptr) return;
    // SetThreadDescription exists from Windows 10 1607. Resolve it at runtime
    // so the engine still loads on older Windows 10 builds.
    using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    HMODULE kernel = ::GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr) return;
    auto fn = reinterpret_cast<SetThreadDescriptionFn>(
        reinterpret_cast<void*>(::GetProcAddress(kernel, "SetThreadDescription")));
    if (fn == nullptr) return;
    try {
        const std::wstring wide = utf8_to_wide(name);
        (void)fn(::GetCurrentThread(), wide.c_str());
    } catch (...) {
        // best effort only
    }
}

namespace {

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// One high-resolution timer per thread, created lazily, closed at thread exit.
struct ThreadTimer {
    HANDLE handle = nullptr;
    bool tried = false;
    ~ThreadTimer() {
        if (handle != nullptr) ::CloseHandle(handle);
    }
    HANDLE get() noexcept {
        if (!tried) {
            tried = true;
            handle = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        }
        return handle;
    }
};

thread_local ThreadTimer t_timer;

}  // namespace

void sleep_until_monotonic_ns(std::uint64_t target_ns) noexcept {
    const std::uint64_t now = monotonic_ns();
    if (target_ns <= now) return;
    const std::uint64_t remaining_100ns = (target_ns - now) / 100;
    HANDLE timer = t_timer.get();
    if (timer != nullptr && remaining_100ns > 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -static_cast<LONGLONG>(remaining_100ns);  // negative = relative
        if (::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
            ::WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    const std::chrono::steady_clock::time_point target{std::chrono::nanoseconds(static_cast<std::int64_t>(target_ns))};
    std::this_thread::sleep_until(target);
}

std::FILE* open_append_utf8(const std::string& path_utf8, std::string& error) {
    std::wstring wide;
    try {
        wide = utf8_to_wide(path_utf8);
    } catch (const std::exception& e) {
        error = e.what();
        return nullptr;
    }
    // _wfsopen with _SH_DENYWR: other processes may READ the file while the
    // engine runs (operators and support tail live logs), but not write it.
    // _wfopen_s would open it unshared and lock everyone else out.
    std::FILE* f = ::_wfsopen(wide.c_str(), L"ab", _SH_DENYWR);
    if (f == nullptr) {
        char buf[128] = {};
        ::strerror_s(buf, sizeof(buf), errno);
        error = buf;
        return nullptr;
    }
    return f;
}

}  // namespace mixit::os

#endif
