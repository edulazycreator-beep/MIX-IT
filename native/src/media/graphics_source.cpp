#include "media/graphics_source.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

#include "platform/os.hpp"
#include "util/clock.hpp"

namespace mixit::media {

namespace {

std::string local_time_text() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace

Status GraphicsSource::create(std::uint32_t width, std::uint32_t height, const std::string& scene_json,
                              std::unique_ptr<GraphicsSource>& out) {
    if (width < 16 || height < 16 || width > 8192 || height > 8192 || (width % 2) != 0 || (height % 2) != 0) {
        return {MIXIT_E_INVALID_ARGUMENT, "graphics size must be even and within 16..8192"};
    }
    std::unique_ptr<GraphicsSource> s(new GraphicsSource());
    if (Status st = graphics::parse_scene(scene_json, s->scene_); !st.is_ok()) return st;
    s->format_.has_video = true;
    s->format_.has_audio = false;
    s->format_.width = width;
    s->format_.height = height;
    s->format_.nominal_frame_rate = Rate{1, 1};
    s->format_.description = "graphics " + std::to_string(width) + "x" + std::to_string(height) + ", " +
                             std::to_string(s->scene_.elements.size()) + " elements";
    out = std::move(s);
    return Status::ok();
}

Status GraphicsSource::set_scene(const std::string& json) {
    graphics::Scene scene;
    if (Status st = graphics::parse_scene(json, scene); !st.is_ok()) return st;
    std::lock_guard lock(mutex_);
    scene_ = std::move(scene);
    generation_.fetch_add(1);
    return Status::ok();
}

Status GraphicsSource::set_field(const std::string& name, const std::string& value) {
    if (name.empty() || name.size() > 64) return {MIXIT_E_INVALID_ARGUMENT, "field name must be 1..64 characters"};
    if (name == "time" || name == "timer") return {MIXIT_E_INVALID_ARGUMENT, "{time} and {timer} are built in"};
    if (value.size() > 4096) return {MIXIT_E_INVALID_ARGUMENT, "field value longer than 4096 bytes"};
    std::lock_guard lock(mutex_);
    if (fields_.size() >= 256 && fields_.find(name) == fields_.end()) return {MIXIT_E_INVALID_ARGUMENT, "too many fields"};
    fields_[name] = value;
    generation_.fetch_add(1);
    return Status::ok();
}

std::int64_t GraphicsSource::timer_value_ms_locked(std::int64_t now_ns) const noexcept {
    const std::int64_t elapsed = timer_running_ ? (now_ns - timer_started_ns_) / 1'000'000 : 0;
    return timer_mode_ == GraphicsTimerMode::Countdown ? std::max<std::int64_t>(0, timer_base_ms_ - elapsed)
                                                       : timer_base_ms_ + elapsed;
}

Status GraphicsSource::set_timer(GraphicsTimerMode mode, std::int64_t value_ms, bool running) {
    if (mode != GraphicsTimerMode::Stopwatch && mode != GraphicsTimerMode::Countdown) {
        return {MIXIT_E_INVALID_ARGUMENT, "unknown timer mode"};
    }
    if (value_ms > 100LL * 3600 * 1000) return {MIXIT_E_INVALID_ARGUMENT, "timer value above 100 hours"};
    const auto now = static_cast<std::int64_t>(monotonic_ns());
    std::lock_guard lock(mutex_);
    const std::int64_t current = timer_value_ms_locked(now);
    timer_base_ms_ = value_ms >= 0 ? value_ms : current;
    timer_mode_ = mode;
    timer_running_ = running;
    timer_started_ns_ = now;
    generation_.fetch_add(1);
    return Status::ok();
}

GraphicsSource::Info GraphicsSource::info() const {
    std::lock_guard lock(mutex_);
    Info i;
    i.timer_ms = timer_value_ms_locked(static_cast<std::int64_t>(monotonic_ns()));
    i.timer_running = timer_running_;
    i.timer_mode = timer_mode_;
    i.frames_rendered = frames_rendered_.load();
    i.warnings = warnings_;
    return i;
}

void GraphicsSource::run(SourceIo& io) noexcept {
    std::unique_ptr<graphics::Renderer> renderer;
    if (Status st = graphics::Renderer::create(renderer); !st.is_ok()) {
        std::lock_guard lock(mutex_);
        warnings_ = st.message();
        return;
    }
    std::uint64_t rendered_generation = 0;
    std::string rendered_dynamic;
    while (!io.stop_requested()) {
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        graphics::Scene scene;
        graphics::FieldMap fields;
        graphics::DynamicText dynamic;
        std::uint64_t generation = 0;
        bool time_based = false;
        {
            std::lock_guard lock(mutex_);
            generation = generation_.load();
            time_based = scene_.uses_time || scene_.uses_timer;
            if (time_based) {
                dynamic.time = local_time_text();
                dynamic.timer = graphics::format_timer((timer_value_ms_locked(now) + (timer_mode_ == GraphicsTimerMode::Countdown ? 999 : 0)) / 1000);
            }
            if (generation != rendered_generation || dynamic.time + dynamic.timer != rendered_dynamic) {
                scene = scene_;
                fields = fields_;
            }
        }
        if (generation != rendered_generation || dynamic.time + dynamic.timer != rendered_dynamic) {
            if (VideoRef frame = io.acquire_video()) {
                std::vector<std::string> warnings;
                (void)renderer->render(scene, fields, dynamic, *frame, warnings);
                frame->pts_ns = now;
                frame->duration_ns = 0;
                frame->flags = 0;
                frame->sequence = static_cast<std::int64_t>(frames_rendered_.fetch_add(1));
                io.push_video(std::move(frame));
                rendered_generation = generation;
                rendered_dynamic = dynamic.time + dynamic.timer;
                std::string joined;
                for (const auto& w : warnings) joined += (joined.empty() ? "" : "; ") + w;
                std::lock_guard lock(mutex_);
                warnings_ = std::move(joined);
            }
        }
        // Changes are picked up within 10 ms; clocks tick on the second.
        if (!io.sleep_until(static_cast<std::int64_t>(monotonic_ns()) + 10'000'000)) break;
    }
}

}  // namespace mixit::media
