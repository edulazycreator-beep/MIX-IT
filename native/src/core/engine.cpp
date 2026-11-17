#include "core/engine.hpp"

#include <string>

#include "media/ffmpeg_library.hpp"
#include "util/clock.hpp"

namespace mixit {

Engine::Engine(std::unique_ptr<Logger> logger, std::uint64_t created_ns)
    : logger_(std::move(logger)), created_ns_(created_ns) {}

Status Engine::create(const mixit_engine_config& config, std::unique_ptr<Engine>& out) {
    const std::uint64_t created_ns = monotonic_ns();

    LoggerOptions options;
    options.min_level = config.min_log_level;
    options.queue_capacity = config.log_queue_capacity;
    options.file_path = config.log_file_path != nullptr ? std::string(config.log_file_path) : std::string();
    options.callback = config.log_callback;
    options.user_data = config.log_user_data;

    std::unique_ptr<Logger> logger;
    if (Status s = Logger::create(options, logger); !s.is_ok()) {
        return s;
    }

    std::unique_ptr<Engine> engine(new Engine(std::move(logger), created_ns));
    engine->logger_->logf(MIXIT_LOG_INFO, "engine", "engine initialized (api {}.{}.{}, log queue {} records)",
                          MIXIT_API_VERSION_MAJOR, MIXIT_API_VERSION_MINOR, MIXIT_API_VERSION_PATCH,
                          engine->logger_->queue_capacity());
    out = std::move(engine);
    return Status::ok();
}

Engine::~Engine() {
    // Destruction always implies shutdown; errors cannot be reported here.
    (void)shutdown();
}

Status Engine::shutdown() {
    if (logger_->is_logger_thread()) {
        return {MIXIT_E_INVALID_STATE, "shutdown/destroy must not be called from the log callback"};
    }
    std::lock_guard lock(control_mutex_);
    if (state_.load(std::memory_order_acquire) == MIXIT_ENGINE_STATE_SHUT_DOWN) {
        return Status::ok();  // idempotent
    }
    // Stop sub-systems in reverse start order. Sessions own inputs, frame
    // sync and the program bus; later milestones add encoders/outputs/GPU.
    for (auto& weak : sessions_) {
        if (auto session = weak.lock()) session->close();
    }
    sessions_.clear();
    const double uptime_s = static_cast<double>(monotonic_ns() - created_ns_) / 1e9;
    logger_->logf(MIXIT_LOG_INFO, "engine", "engine shutting down after {:.3f} s", uptime_s);
    state_.store(MIXIT_ENGINE_STATE_SHUT_DOWN, std::memory_order_release);
    logger_->stop();  // drains every accepted record, then closes sinks
    return Status::ok();
}

void Engine::fill_diagnostics(mixit_engine_diagnostics& out) const noexcept {
    const LoggerStats stats = logger_->stats();
    out.state = state();
    out.uptime_ns = monotonic_ns() - created_ns_;
    out.log_records_accepted = stats.accepted;
    out.log_records_dropped = stats.dropped;
    out.log_records_delivered = stats.delivered;
    out.log_file_write_errors = stats.file_write_errors;
}

Status Engine::require_initialized(const char* operation) const {
    if (state() != MIXIT_ENGINE_STATE_INITIALIZED) {
        return {MIXIT_E_INVALID_STATE, std::string(operation) + " requires an initialized (not shut down) engine"};
    }
    return Status::ok();
}

Status Engine::set_log_level(std::int32_t level) {
    if (level < MIXIT_LOG_TRACE || level > MIXIT_LOG_OFF) {
        return {MIXIT_E_INVALID_ARGUMENT, "log level out of range"};
    }
    if (Status s = require_initialized("set_log_level"); !s.is_ok()) return s;
    logger_->set_min_level(level);
    return Status::ok();
}

Status Engine::log(std::int32_t level, std::string_view category, std::string_view message) {
    if (level < MIXIT_LOG_TRACE || level >= MIXIT_LOG_OFF) {
        return {MIXIT_E_INVALID_ARGUMENT, "log level out of range"};
    }
    if (Status s = require_initialized("log"); !s.is_ok()) return s;
    (void)logger_->log(level, category, message);  // drop is counted, not an error
    return Status::ok();
}

Status Engine::detect_capabilities(SystemInfo& out) {
    if (Status s = require_initialized("detect_capabilities"); !s.is_ok()) return s;
    SystemInfo info = probe_system_info(*logger_);  // slow-ish; done outside the lock
    logger_->logf(MIXIT_LOG_INFO, "caps",
                  "detected: cpu='{}' logical={} physical={} features=0x{:X} ram={} MiB gpus={} os='{}' flags=0x{:X}",
                  info.cpu_brand, info.logical_processor_count, info.physical_core_count, info.cpu_features,
                  info.total_physical_memory_bytes / (1024 * 1024), info.gpu_adapters.size(),
                  info.os_description, info.detection_flags);
    for (std::size_t i = 0; i < info.gpu_adapters.size(); ++i) {
        const GpuAdapter& a = info.gpu_adapters[i];
        logger_->logf(MIXIT_LOG_INFO, "caps", "gpu[{}]: '{}' vendor=0x{:04X} device=0x{:04X} vram={} MiB{}", i,
                      a.description, a.vendor_id, a.device_id, a.dedicated_video_memory_bytes / (1024 * 1024),
                      (a.flags & MIXIT_GPU_ADAPTER_FLAG_SOFTWARE) ? " (software)" : "");
    }
    media::FfmpegLibrary::log_info(*logger_);
    std::lock_guard lock(control_mutex_);
    capabilities_ = info;
    out = std::move(info);
    return Status::ok();
}

Status Engine::gpu_adapter(std::uint32_t index, GpuAdapter& out) const {
    std::lock_guard lock(control_mutex_);
    if (!capabilities_) {
        return {MIXIT_E_INVALID_STATE, "capabilities have not been detected yet"};
    }
    if (index >= capabilities_->gpu_adapters.size()) {
        return {MIXIT_E_NOT_FOUND, "GPU adapter index " + std::to_string(index) + " out of range (count " +
                                       std::to_string(capabilities_->gpu_adapters.size()) + ")"};
    }
    out = capabilities_->gpu_adapters[index];
    return Status::ok();
}

}  // namespace mixit

namespace mixit {

Status Engine::create_session(const media::SessionConfig& config, std::shared_ptr<media::Session>& out) {
    if (Status s = require_initialized("create_session"); !s.is_ok()) return s;
    std::unique_ptr<media::Session> session;
    if (Status s = media::Session::create(config, *logger_, session); !s.is_ok()) return s;
    std::shared_ptr<media::Session> shared(std::move(session));
    std::lock_guard lock(control_mutex_);
    if (state() != MIXIT_ENGINE_STATE_INITIALIZED) {
        return {MIXIT_E_INVALID_STATE, "engine shut down while the session was being created"};
    }
    // Drop entries of sessions that were already destroyed.
    std::erase_if(sessions_, [](const std::weak_ptr<media::Session>& w) { return w.expired(); });
    sessions_.push_back(shared);
    out = std::move(shared);
    return Status::ok();
}

}  // namespace mixit
