#include "logging/logger.hpp"

#include <chrono>
#include <cstring>
#include <system_error>

#include "core/last_error.hpp"
#include "logging/json.hpp"
#include "platform/os.hpp"
#include "util/clock.hpp"

namespace mixit {

const char* log_level_name(std::int32_t level) noexcept {
    switch (level) {
        case MIXIT_LOG_TRACE: return "trace";
        case MIXIT_LOG_DEBUG: return "debug";
        case MIXIT_LOG_INFO: return "info";
        case MIXIT_LOG_WARNING: return "warning";
        case MIXIT_LOG_ERROR: return "error";
        case MIXIT_LOG_CRITICAL: return "critical";
        default: return "unknown";
    }
}

namespace {

constexpr std::string_view kTruncationMarker = "...";

template <std::size_t N>
void copy_with_marker(std::array<char, N>& dest, std::string_view text) noexcept {
    if (text.size() < N) {
        copy_utf8_truncated(dest.data(), N, text);
        return;
    }
    const std::size_t room = N - kTruncationMarker.size();  // includes the NUL slot
    const std::size_t n = copy_utf8_truncated(dest.data(), room, text);
    std::memcpy(dest.data() + n, kTruncationMarker.data(), kTruncationMarker.size());
    dest[n + kTruncationMarker.size()] = '\0';
}

}  // namespace

Logger::Logger(const LoggerOptions& options)
    : queue_(options.queue_capacity == 0 ? 4096 : options.queue_capacity),
      min_level_(options.min_level),
      callback_(options.callback),
      user_data_(options.user_data),
      poll_interval_ms_(options.poll_interval_ms == 0 ? 10 : options.poll_interval_ms) {
    line_buffer_.reserve(1024);
}

Status Logger::create(const LoggerOptions& options, std::unique_ptr<Logger>& out) {
    if (options.min_level < MIXIT_LOG_TRACE || options.min_level > MIXIT_LOG_OFF) {
        return {MIXIT_E_INVALID_ARGUMENT, "min_log_level out of range"};
    }
    if (options.queue_capacity > BoundedMpmcQueue<LogSlot>::kMaxCapacity) {
        return {MIXIT_E_INVALID_ARGUMENT, "log_queue_capacity too large"};
    }

    std::unique_ptr<Logger> logger(new Logger(options));

    if (!options.file_path.empty()) {
        std::string error;
        logger->file_ = os::open_append_utf8(options.file_path, error);
        if (logger->file_ == nullptr) {
            return {MIXIT_E_IO, "cannot open log file '" + options.file_path + "': " + error};
        }
    }

    try {
        logger->thread_ = std::thread([raw = logger.get()] { raw->run(); });
    } catch (const std::system_error& e) {
        if (logger->file_ != nullptr) {
            std::fclose(logger->file_);
            logger->file_ = nullptr;
        }
        return {MIXIT_E_PLATFORM, std::string("cannot start logger thread: ") + e.what()};
    }

    out = std::move(logger);
    return Status::ok();
}

Logger::~Logger() { stop(); }

bool Logger::log(std::int32_t level, std::string_view category, std::string_view message) noexcept {
    if (!is_enabled(level) || !accepting_.load(std::memory_order_acquire)) {
        return false;
    }
    const std::int64_t now = unix_time_ns();
    const std::uint64_t tid = os::current_thread_id();
    const bool pushed = queue_.try_push_with([&](LogSlot& slot) noexcept {
        slot.level = level;
        slot.unix_time_ns = now;
        slot.thread_id = tid;
        slot.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed);
        copy_with_marker(slot.category, category);
        copy_with_marker(slot.message, message);
    });
    if (pushed) {
        accepted_.fetch_add(1, std::memory_order_release);
    } else {
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }
    return pushed;
}

bool Logger::flush(std::uint32_t timeout_ms) noexcept {
    if (is_logger_thread()) return false;
    const std::uint64_t target = accepted_.load(std::memory_order_acquire);
    std::unique_lock lock(wake_mutex_);
    if (stop_requested_) return delivered_.load() >= target;
    flush_requested_ = true;
    wake_cv_.notify_all();
    return delivered_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
        return delivered_.load(std::memory_order_acquire) >= target || stop_requested_;
    }) && delivered_.load() >= target;
}

void Logger::stop() noexcept {
    // Joining from the logger thread itself would deadlock; the owner will
    // call stop() again (the destructor always does).
    if (is_logger_thread()) return;
    std::call_once(stop_once_, [this]() noexcept {
        accepting_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(wake_mutex_);
            stop_requested_ = true;
        }
        wake_cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
        delivered_cv_.notify_all();
    });
}

bool Logger::is_logger_thread() const noexcept {
    return thread_id_.load(std::memory_order_acquire) == std::this_thread::get_id();
}

LoggerStats Logger::stats() const noexcept {
    LoggerStats s;
    s.accepted = accepted_.load(std::memory_order_relaxed);
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.delivered = delivered_.load(std::memory_order_relaxed);
    s.file_write_errors = file_write_errors_.load(std::memory_order_relaxed);
    return s;
}

void Logger::run() noexcept {
    thread_id_.store(std::this_thread::get_id(), std::memory_order_release);
    os::set_current_thread_name("mixit-log");

    for (;;) {
        drain();
        std::unique_lock lock(wake_mutex_);
        delivered_cv_.notify_all();
        if (stop_requested_) break;
        if (!flush_requested_) {
            wake_cv_.wait_for(lock, std::chrono::milliseconds(poll_interval_ms_),
                              [this] { return stop_requested_ || flush_requested_; });
        }
        flush_requested_ = false;
    }
    // Final drain: everything accepted before accepting_ was cleared.
    drain();
    std::lock_guard lock(wake_mutex_);
    delivered_cv_.notify_all();
}

std::size_t Logger::drain() noexcept {
    std::size_t count = 0;
    while (queue_.try_pop_with([this](LogSlot& slot) noexcept { deliver(slot); })) {
        ++count;
    }
    if (count > 0 && file_ != nullptr && std::fflush(file_) != 0) {
        file_write_errors_.fetch_add(1, std::memory_order_relaxed);
    }
    return count;
}

void Logger::deliver(const LogSlot& slot) noexcept {
    if (file_ != nullptr) {
        try {
            line_buffer_.clear();
            line_buffer_ += "{\"ts\":\"";
            json::append_iso8601_utc(line_buffer_, slot.unix_time_ns);
            line_buffer_ += "\",\"level\":\"";
            line_buffer_ += log_level_name(slot.level);
            line_buffer_ += "\",\"seq\":";
            line_buffer_ += std::to_string(slot.sequence);
            line_buffer_ += ",\"thread\":";
            line_buffer_ += std::to_string(slot.thread_id);
            line_buffer_ += ",\"category\":\"";
            json::append_escaped(line_buffer_, slot.category.data());
            line_buffer_ += "\",\"msg\":\"";
            json::append_escaped(line_buffer_, slot.message.data());
            line_buffer_ += "\"}\n";
            if (std::fwrite(line_buffer_.data(), 1, line_buffer_.size(), file_) != line_buffer_.size()) {
                file_write_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            file_write_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (callback_ != nullptr) {
        mixit_log_record record{};
        record.struct_size = sizeof(record);
        record.level = slot.level;
        record.unix_time_ns = slot.unix_time_ns;
        record.thread_id = slot.thread_id;
        record.sequence = slot.sequence;
        record.category = slot.category.data();
        record.message = slot.message.data();
        callback_(&record, user_data_);
    }

    delivered_.fetch_add(1, std::memory_order_release);
}

}  // namespace mixit
