// Engine logger.
//
// Threading model
//   * Any thread may call log()/logf(). The producer path takes no locks,
//     performs no allocation (log) and no system calls after the first call
//     on a thread: it formats into a pre-allocated slot of a bounded
//     lock-free queue. This keeps it usable from real-time threads.
//   * If the queue is full the record is DROPPED and counted. Logging never
//     applies backpressure to the caller.
//   * One logger-owned thread ("mixit-log") drains the queue and delivers
//     records to the sinks: an optional JSON-lines file and an optional
//     host callback. Sinks therefore never run on producer threads.
//   * The logger thread wakes on a short timer (it is not signalled by
//     producers, which would require a syscall on the hot path); stop() and
//     flush() wake it immediately.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "core/status.hpp"
#include "mixit/mixit_engine.h"
#include "util/bounded_mpmc_queue.hpp"

namespace mixit {

struct LogSlot {
    static constexpr std::size_t kCategoryCapacity = 32;
    static constexpr std::size_t kMessageCapacity = 480;

    std::int32_t level = 0;
    std::int64_t unix_time_ns = 0;
    std::uint64_t thread_id = 0;
    std::uint64_t sequence = 0;
    std::array<char, kCategoryCapacity> category{};
    std::array<char, kMessageCapacity> message{};
};

struct LoggerOptions {
    std::int32_t min_level = MIXIT_LOG_INFO;
    std::uint32_t queue_capacity = 4096;
    std::string file_path;                  // empty = no file sink
    mixit_log_callback callback = nullptr;  // nullptr = no callback sink
    void* user_data = nullptr;
    std::uint32_t poll_interval_ms = 10;    // logger thread idle wake-up period
};

struct LoggerStats {
    std::uint64_t accepted = 0;
    std::uint64_t dropped = 0;
    std::uint64_t delivered = 0;
    std::uint64_t file_write_errors = 0;
};

class Logger {
public:
    // Opens sinks and starts the logger thread.
    static Status create(const LoggerOptions& options, std::unique_ptr<Logger>& out);

    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    [[nodiscard]] bool is_enabled(std::int32_t level) const noexcept {
        return level >= min_level_.load(std::memory_order_relaxed) && level < MIXIT_LOG_OFF;
    }
    void set_min_level(std::int32_t level) noexcept { min_level_.store(level, std::memory_order_relaxed); }
    [[nodiscard]] std::int32_t min_level() const noexcept { return min_level_.load(std::memory_order_relaxed); }

    // Enqueues a record. Never blocks. Overlong text is truncated on a UTF-8
    // boundary and marked with "...". Returns false if dropped or filtered.
    bool log(std::int32_t level, std::string_view category, std::string_view message) noexcept;

    // std::format-style convenience for control-plane code (formats into a
    // stack buffer; not intended for real-time threads).
    template <typename... Args>
    bool logf(std::int32_t level, std::string_view category, std::format_string<Args...> fmt,
              Args&&... args) noexcept {
        if (!is_enabled(level)) return false;
        char buf[LogSlot::kMessageCapacity + 4];
        try {
            const auto res = std::format_to_n(buf, sizeof(buf), fmt, std::forward<Args>(args)...);
            const auto len = static_cast<std::size_t>(res.out - buf);
            return log(level, category, std::string_view(buf, len < sizeof(buf) ? len : sizeof(buf)));
        } catch (...) {
            return log(level, category, "<log formatting failed>");
        }
    }

    // Blocks until every record accepted before the call has been delivered
    // (or the logger is stopped). Must not be called from the logger thread.
    // Returns false on timeout or if called from the logger thread.
    bool flush(std::uint32_t timeout_ms = 5000) noexcept;

    // Drains remaining records, stops the thread, closes the file.
    // Idempotent. After stop() no callback is invoked and log() drops.
    void stop() noexcept;

    [[nodiscard]] bool is_logger_thread() const noexcept;
    [[nodiscard]] LoggerStats stats() const noexcept;
    [[nodiscard]] std::size_t queue_capacity() const noexcept { return queue_.capacity(); }

private:
    explicit Logger(const LoggerOptions& options);
    void run() noexcept;
    std::size_t drain() noexcept;
    void deliver(const LogSlot& slot) noexcept;

    BoundedMpmcQueue<LogSlot> queue_;
    std::atomic<std::int32_t> min_level_;
    std::atomic<std::uint64_t> next_sequence_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> delivered_{0};
    std::atomic<std::uint64_t> file_write_errors_{0};
    std::atomic<bool> accepting_{true};

    std::FILE* file_ = nullptr;
    mixit_log_callback callback_ = nullptr;
    void* user_data_ = nullptr;
    std::uint32_t poll_interval_ms_;
    std::string line_buffer_;  // logger thread only

    std::mutex wake_mutex_;
    std::condition_variable wake_cv_;      // stop / flush requests -> logger thread
    std::condition_variable delivered_cv_; // logger thread -> flush() waiters
    bool stop_requested_ = false;          // guarded by wake_mutex_
    bool flush_requested_ = false;         // guarded by wake_mutex_

    std::once_flag stop_once_;
    std::thread thread_;
    std::atomic<std::thread::id> thread_id_{};
};

const char* log_level_name(std::int32_t level) noexcept;

}  // namespace mixit
