// Engine: the root object behind a mixit_engine handle.
//
// Milestone 1 owns: configuration validation, the logger, lifecycle state
// and cached capability detection. Later milestones attach the session,
// media graph, audio engine, outputs, etc. as owned sub-systems, each with
// its own start/stop ordering inside Engine::shutdown().
//
// Thread-safety: all public methods are safe to call concurrently. State
// transitions and capability data are guarded by a control-plane mutex
// (never taken on media threads).
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "core/status.hpp"
#include "logging/logger.hpp"
#include "mixit/mixit_engine.h"
#include "media/session.hpp"
#include "platform/system_info.hpp"

namespace mixit {

class Engine {
public:
    static Status create(const mixit_engine_config& config, std::unique_ptr<Engine>& out);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    Status shutdown();
    [[nodiscard]] std::int32_t state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] bool is_logger_thread() const noexcept { return logger_->is_logger_thread(); }

    void fill_diagnostics(mixit_engine_diagnostics& out) const noexcept;
    Status set_log_level(std::int32_t level);
    Status log(std::int32_t level, std::string_view category, std::string_view message);

    Status detect_capabilities(SystemInfo& out);
    Status gpu_adapter(std::uint32_t index, GpuAdapter& out) const;

    // Lifecycle step 3. The engine tracks sessions and closes them on shutdown.
    Status create_session(const media::SessionConfig& config, std::shared_ptr<media::Session>& out);

    Logger& logger() noexcept { return *logger_; }

private:
    Engine(std::unique_ptr<Logger> logger, std::uint64_t created_ns);
    Status require_initialized(const char* operation) const;

    std::unique_ptr<Logger> logger_;
    std::uint64_t created_ns_;
    std::atomic<std::int32_t> state_{MIXIT_ENGINE_STATE_INITIALIZED};
    mutable std::mutex control_mutex_;
    std::optional<SystemInfo> capabilities_;  // guarded by control_mutex_
    std::vector<std::weak_ptr<media::Session>> sessions_;  // guarded by control_mutex_
};

}  // namespace mixit
