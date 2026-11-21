// MediaSource rendering a graphics scene (titles, lower thirds, clock,
// scoreboard; see graphics/graphics.hpp) with live fields and a timer (M6).
// Frames are rendered only when something changes (or each second when the
// scene shows {time}/{timer}); the session repeats the last frame between.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "graphics/graphics.hpp"
#include "media/media_source.hpp"

namespace mixit::media {

enum class GraphicsTimerMode : std::int32_t { Stopwatch = 1, Countdown = 2 };  // = MIXIT_GRAPHICS_TIMER_*

class GraphicsSource final : public MediaSource {
public:
    static Status create(std::uint32_t width, std::uint32_t height, const std::string& scene_json,
                         std::unique_ptr<GraphicsSource>& out);

    [[nodiscard]] const SourceFormat& format() const noexcept override { return format_; }
    void run(SourceIo& io) noexcept override;

    // ---- control (any thread) ----
    Status set_scene(const std::string& json);  // validated; the old scene stays on error
    Status set_field(const std::string& name, const std::string& value);
    // value_ms < 0 keeps the current value (pause/resume).
    Status set_timer(GraphicsTimerMode mode, std::int64_t value_ms, bool running);
    struct Info {
        std::int64_t timer_ms = 0;
        bool timer_running = false;
        GraphicsTimerMode timer_mode = GraphicsTimerMode::Stopwatch;
        std::uint64_t frames_rendered = 0;
        std::string warnings;  // last render (missing font/image), empty = fine
    };
    [[nodiscard]] Info info() const;

private:
    GraphicsSource() = default;
    [[nodiscard]] std::int64_t timer_value_ms_locked(std::int64_t now_ns) const noexcept;

    SourceFormat format_;
    mutable std::mutex mutex_;
    graphics::Scene scene_;
    graphics::FieldMap fields_;
    GraphicsTimerMode timer_mode_ = GraphicsTimerMode::Stopwatch;
    std::int64_t timer_base_ms_ = 0;     // value when (re)started or paused
    std::int64_t timer_started_ns_ = 0;  // monotonic, when running
    bool timer_running_ = false;
    std::string warnings_;
    std::atomic<std::uint64_t> generation_{1};  // bumped on every change
    std::atomic<std::uint64_t> frames_rendered_{0};
};

}  // namespace mixit::media
