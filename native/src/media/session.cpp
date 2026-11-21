#include "media/session.hpp"
#include "media/graphics_source.hpp"
#include "media/session_internal.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <system_error>

#include "logging/logger.hpp"
#include "media/test_signal_source.hpp"
#include "platform/os.hpp"
#include "util/clock.hpp"

namespace mixit::media {
using namespace internal;
namespace {

bool valid_dimensions(std::uint32_t w, std::uint32_t h, std::uint32_t min) {
    return w >= min && h >= min && w <= 8192 && h <= 8192 && w % 2 == 0 && h % 2 == 0;
}

std::string rate_text(Rate r) {
    return r.den == 1 ? std::to_string(r.num) : std::to_string(r.num) + "/" + std::to_string(r.den);
}

// Recognises the test signal's flash band from pixels, so A/V alignment can
// be measured for any source carrying the test signal (files, or a camera /
// capture card pointed at a screen showing it), not only synthetic inputs.
// Samples 8 points across the band; normal pictures rarely have a uniformly
// near-white bottom edge, and a false flash only expires unmatched.
bool looks_like_flash(const VideoFrame& f) noexcept {
    if (f.width < 16 || f.height < 16) return false;
    const std::uint32_t y = f.height - f.height / 16;
    const std::uint8_t* row = f.data + static_cast<std::size_t>(y) * f.stride;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::uint8_t* px = row + static_cast<std::size_t>((f.width * (2 * i + 1)) / 16) * 4;
        if (px[0] < 230 || px[1] < 230 || px[2] < 230) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------

Session::Session(const SessionConfig& config, Logger& logger) : config_(config), logger_(logger) {}

Status Session::create(const SessionConfig& config, Logger& logger, std::unique_ptr<Session>& out) {
    if (!valid_dimensions(config.width, config.height, 16)) {
        return {MIXIT_E_INVALID_ARGUMENT, "session size must be even and within 16..8192"};
    }
    if (!is_supported_frame_rate(config.frame_rate)) {
        return {MIXIT_E_NOT_SUPPORTED, "unsupported session frame rate " + rate_text(config.frame_rate) +
                                           " (supported: 24000/1001, 24, 25, 30000/1001, 30, 50, 60000/1001, 60)"};
    }
    if (config.sample_rate != 48000 && config.sample_rate != 44100) {
        return {MIXIT_E_NOT_SUPPORTED, "session sample rate must be 48000 or 44100"};
    }
    if (config.channels < 1 || config.channels > 8) {
        return {MIXIT_E_INVALID_ARGUMENT, "session channel count must be 1..8"};
    }
    std::unique_ptr<Session> s(new Session(config, logger));
    const std::uint32_t pool = config.program_pool_frames == 0 ? kDefaultProgramPool : config.program_pool_frames;
    if (pool < 5 || pool > 64) return {MIXIT_E_INVALID_ARGUMENT, "program_pool_frames must be 5..64"};
    s->program_pool_ = std::make_unique<VideoFramePool>(config.width, config.height, pool);
    s->cpu_compositor_ = make_cpu_compositor();
    s->compositor_ = s->cpu_compositor_.get();
    if (config.compositor == CompositorPreference::D3D11 || config.compositor == CompositorPreference::D3D11Software) {
        GpuCompositorOptions gpu;
        gpu.software = config.compositor == CompositorPreference::D3D11Software;
        if (Status g = make_d3d11_compositor(gpu, logger, s->gpu_compositor_); g.is_ok()) {
            s->compositor_ = s->gpu_compositor_.get();
        } else {
            // A production must still start: fall back to the CPU path, loudly.
            logger.logf(MIXIT_LOG_WARNING, "session", "GPU compositor unavailable, using CPU: {}", g.message());
        }
    }
    s->compositor_kind_.store(static_cast<std::int32_t>(s->compositor_->kind()), std::memory_order_relaxed);
    // Latency of frame sync: one output period plus 2 ms scheduling margin.
    s->latency_ns_ = duration_of_index(0, config.frame_rate) + 2'000'000;
    s->init_audio();
    {
        output::Timeline tl;
        tl.frame_rate = config.frame_rate;
        tl.sample_rate = config.sample_rate;
        tl.channels = config.channels;
        tl.program_width = config.width;
        tl.program_height = config.height;
        Session* raw = s.get();
        s->outputs_ = std::make_unique<output::OutputManager>(tl, logger, [raw] { raw->wait_feed_quiescent(); });
    }
    logger.logf(MIXIT_LOG_INFO, "session", "session created: {}x{} @ {} fps, {} Hz x{} ch", config.width,
                config.height, rate_text(config.frame_rate), config.sample_rate, config.channels);
    out = std::move(s);
    return Status::ok();
}

Session::~Session() { close(); }

Session::Input* Session::find_input(std::uint32_t id) const noexcept {
    for (const auto& in : inputs_) {
        if (in->id == id) return in.get();
    }
    return nullptr;
}

Status Session::add_test_source(TestSignalConfig cfg, std::uint32_t& out_input_id) {
    std::lock_guard lock(control_mutex_);
    const SessionState st = state();
    if (st == SessionState::Running || st == SessionState::Closed) {
        return {MIXIT_E_INVALID_STATE, "inputs can only be added while the session is not running"};
    }
    if (!valid_dimensions(cfg.width, cfg.height, 64)) {
        return {MIXIT_E_INVALID_ARGUMENT, "test source size must be even and within 64..8192"};
    }
    if (!is_supported_frame_rate(cfg.frame_rate)) {
        return {MIXIT_E_NOT_SUPPORTED, "unsupported test source frame rate " + rate_text(cfg.frame_rate)};
    }
    cfg.sample_rate = config_.sample_rate;  // generated directly at the session format
    cfg.channels = config_.channels;
    if (cfg.tone_hz >= cfg.sample_rate / 2) return {MIXIT_E_INVALID_ARGUMENT, "tone frequency must be below Nyquist"};
    if (cfg.flash_interval_frames > 100000) return {MIXIT_E_INVALID_ARGUMENT, "flash interval too large"};

    const std::uint32_t id = next_input_id_;
    cfg.source_id = id;
    auto source = std::make_unique<TestSignalSource>(cfg);
    if (Status st2 = add_input_locked(std::move(source), out_input_id); !st2.is_ok()) return st2;
    logger_.logf(MIXIT_LOG_INFO, "session", "input {} added: test signal {}x{} @ {} fps, tone {} Hz, flash every {} frames",
                 id, cfg.width, cfg.height, rate_text(cfg.frame_rate), cfg.tone_hz, cfg.flash_interval_frames);
    return Status::ok();
}

Status Session::add_source(std::unique_ptr<MediaSource> source, std::uint32_t& out_input_id) {
    if (!source) return {MIXIT_E_INVALID_ARGUMENT, "source is null"};
    std::lock_guard lock(control_mutex_);
    const SessionState st = state();
    if (st == SessionState::Running || st == SessionState::Closed) {
        return {MIXIT_E_INVALID_STATE, "inputs can only be added while the session is not running"};
    }
    const SourceFormat& f = source->format();
    if (!f.has_video && !f.has_audio) return {MIXIT_E_INVALID_ARGUMENT, "source has neither video nor audio"};
    if (f.has_video && (f.width == 0 || f.height == 0 || f.width > 8192 || f.height > 8192)) {
        return {MIXIT_E_NOT_SUPPORTED, "source video size outside 1..8192"};
    }
    const std::string description = f.description;
    if (Status st2 = add_input_locked(std::move(source), out_input_id); !st2.is_ok()) return st2;
    logger_.logf(MIXIT_LOG_INFO, "session", "input {} added: {}", out_input_id, description);
    return Status::ok();
}

Status Session::with_graphics(std::uint32_t input_id, const std::function<Status(GraphicsSource&)>& fn) const {
    std::lock_guard lock(control_mutex_);
    const Input* in = find_input(input_id);
    if (in == nullptr) return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    auto* g = dynamic_cast<GraphicsSource*>(in->source.get());
    if (g == nullptr) return {MIXIT_E_INVALID_ARGUMENT, "input " + std::to_string(input_id) + " is not a graphics input"};
    return fn(*g);
}

Status Session::add_input_locked(std::unique_ptr<MediaSource> source, std::uint32_t& out_input_id) {
    if (next_input_id_ > 255) return {MIXIT_E_NOT_SUPPORTED, "input id space exhausted for this session"};
    const std::uint32_t id = next_input_id_++;
    inputs_.push_back(std::make_unique<Input>(id, std::move(source), config_));
    out_input_id = id;
    return Status::ok();
}

Status Session::remove_input(std::uint32_t input_id) {
    std::lock_guard lock(control_mutex_);
    const SessionState st = state();
    if (st == SessionState::Running || st == SessionState::Closed) {
        return {MIXIT_E_INVALID_STATE, "inputs can only be removed while the session is not running"};
    }
    const auto it = std::find_if(inputs_.begin(), inputs_.end(), [&](const auto& in) { return in->id == input_id; });
    if (it == inputs_.end()) return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    // Release any bus frames that may reference this input's pool.
    program_out_.clear();
    preview_out_.clear();
    reset_audio_runtime();
    {
        std::lock_guard consumer(consumer_mutex_);
        program_cache_ = ProgramFrame{};
        preview_cache_ = ProgramFrame{};
    }
    inputs_.erase(it);
    {
        std::lock_guard scenes(scene_mutex_);
        for (BusScene* scene : {&program_scene_, &preview_scene_}) {
            if (scene->base_input == input_id) scene->base_input = 0;
            for (LayerConfig& l : scene->layers) {
                if (l.input_id == input_id) l = LayerConfig{};
            }
        }
    }
    logger_.logf(MIXIT_LOG_INFO, "session", "input {} removed", input_id);
    return Status::ok();
}

Status Session::set_program_input(std::uint32_t input_id) {
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    if (input_id != 0 && find_input(input_id) == nullptr) {
        return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    }
    std::lock_guard scenes(scene_mutex_);
    program_scene_.base_input = input_id;
    transition_.active = false;  // a direct selection always wins
    return Status::ok();
}

Status Session::set_preview_input(std::uint32_t input_id) {
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    if (input_id != 0 && find_input(input_id) == nullptr) {
        return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    }
    std::lock_guard scenes(scene_mutex_);
    preview_scene_.base_input = input_id;
    return Status::ok();
}

Status Session::cut() {
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    std::lock_guard scenes(scene_mutex_);
    if (transition_.active) {
        // CUT during a transition completes it immediately (operators expect
        // CUT to always act).
        transition_.active = false;
    }
    std::swap(program_scene_, preview_scene_);
    transitions_completed_.fetch_add(1, std::memory_order_relaxed);
    return Status::ok();
}

Status Session::start_transition(TransitionKind kind, std::uint32_t frames) {
    if (kind != TransitionKind::Cut && kind != TransitionKind::Mix && kind != TransitionKind::Fade &&
        kind != TransitionKind::Wipe) {
        return {MIXIT_E_INVALID_ARGUMENT, "unknown transition kind"};
    }
    if (frames > 600) return {MIXIT_E_INVALID_ARGUMENT, "transition duration must be at most 600 frames"};
    if (kind == TransitionKind::Cut || frames == 0) return cut();
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    std::lock_guard scenes(scene_mutex_);
    if (transition_.active) return {MIXIT_E_INVALID_STATE, "a transition is already running"};
    if (state() != SessionState::Running) {
        // Nothing is being rendered: the end state is the same as a cut.
        std::swap(program_scene_, preview_scene_);
        transitions_completed_.fetch_add(1, std::memory_order_relaxed);
        return Status::ok();
    }
    transition_ = Transition{true, kind, frames, 0};
    return Status::ok();
}

Status Session::validate_layer(const LayerConfig& layer) const {
    auto valid_rect = [](const NormRect& r) {
        return r.w > 0.0f && r.h > 0.0f && r.x >= -4.0f && r.y >= -4.0f && r.x + r.w <= 5.0f && r.y + r.h <= 5.0f;
    };
    auto valid_crop = [](const NormRect& r) {
        return r.w > 0.0f && r.h > 0.0f && r.x >= 0.0f && r.y >= 0.0f && r.x + r.w <= 1.0001f && r.y + r.h <= 1.0001f;
    };
    if (layer.input_id == 0) return Status::ok();
    if (!valid_rect(layer.dest)) return {MIXIT_E_INVALID_ARGUMENT, "layer destination rectangle out of range"};
    if (!valid_crop(layer.crop)) return {MIXIT_E_INVALID_ARGUMENT, "layer crop must lie within the source (0..1)"};
    if (!(layer.opacity >= 0.0f && layer.opacity <= 1.0f)) return {MIXIT_E_INVALID_ARGUMENT, "opacity must be 0..1"};
    const KeySettings& k = layer.key;
    if (k.type != KeyType::None && k.type != KeyType::Chroma && k.type != KeyType::Luma) {
        return {MIXIT_E_INVALID_ARGUMENT, "unknown key type"};
    }
    auto unit = [](float v) { return v >= 0.0f && v <= 1.0f; };
    if (k.type == KeyType::Chroma && (!unit(k.similarity) || !unit(k.spill) ||
                                      !(k.smoothness > 0.0f && k.smoothness <= 1.0f) || k.color > 0xFFFFFFu)) {
        return {MIXIT_E_INVALID_ARGUMENT, "chroma key: similarity/spill 0..1, smoothness > 0..1, colour 0xRRGGBB"};
    }
    if (k.type == KeyType::Luma && (!unit(k.threshold) || !(k.softness > 0.0f && k.softness <= 1.0f))) {
        return {MIXIT_E_INVALID_ARGUMENT, "luma key: threshold 0..1, softness > 0..1"};
    }
    return Status::ok();
}

Status Session::set_layer(Bus bus, std::uint32_t index, const LayerConfig& layer) {
    if (bus != Bus::Program && bus != Bus::Preview) return {MIXIT_E_INVALID_ARGUMENT, "unknown bus"};
    if (index >= kMaxLayersPerScene) {
        return {MIXIT_E_INVALID_ARGUMENT, "layer index must be below " + std::to_string(kMaxLayersPerScene)};
    }
    if (Status st = validate_layer(layer); !st.is_ok()) return st;
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    if (layer.input_id != 0 && find_input(layer.input_id) == nullptr) {
        return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(layer.input_id)};
    }
    std::lock_guard scenes(scene_mutex_);
    BusScene& scene = bus == Bus::Program ? program_scene_ : preview_scene_;
    scene.layers[index] = layer.input_id == 0 ? LayerConfig{} : layer;
    return Status::ok();
}

Status Session::set_overlay(std::uint32_t channel, const LayerConfig& layer) {
    if (channel >= kOverlayChannels) return {MIXIT_E_INVALID_ARGUMENT, "overlay channel must be below " + std::to_string(kOverlayChannels)};
    if (Status st = validate_layer(layer); !st.is_ok()) return st;
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    if (layer.input_id != 0 && find_input(layer.input_id) == nullptr) {
        return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(layer.input_id)};
    }
    std::lock_guard scenes(scene_mutex_);
    Overlay& o = overlays_[channel];
    o.layer = layer.input_id == 0 ? LayerConfig{} : layer;
    if (layer.input_id == 0) {
        o.visible = false;
        o.level = 0.0f;
    }
    return Status::ok();
}

Status Session::show_overlay(std::uint32_t channel, bool visible, std::uint32_t fade_frames) {
    if (channel >= kOverlayChannels) return {MIXIT_E_INVALID_ARGUMENT, "overlay channel must be below " + std::to_string(kOverlayChannels)};
    if (fade_frames > 600) return {MIXIT_E_INVALID_ARGUMENT, "fade must be at most 600 frames"};
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    std::lock_guard scenes(scene_mutex_);
    Overlay& o = overlays_[channel];
    if (visible && o.layer.input_id == 0) return {MIXIT_E_INVALID_STATE, "the overlay channel has no input"};
    o.visible = visible;
    o.step = fade_frames == 0 ? 1.0f : 1.0f / static_cast<float>(fade_frames);
    // While stopped (or with no fade) the change is immediate.
    if (fade_frames == 0 || state() != SessionState::Running) o.level = visible ? 1.0f : 0.0f;
    return Status::ok();
}

Status Session::overlay_info(std::uint32_t channel, OverlayInfo& out) const {
    if (channel >= kOverlayChannels) return {MIXIT_E_INVALID_ARGUMENT, "overlay channel must be below " + std::to_string(kOverlayChannels)};
    std::lock_guard scenes(scene_mutex_);
    const Overlay& o = overlays_[channel];
    out.layer = o.layer;
    out.visible = o.visible;
    out.level = o.level;
    return Status::ok();
}

Status Session::start() {
    std::lock_guard lock(control_mutex_);
    const SessionState st = state();
    if (st == SessionState::Running) return Status::ok();
    if (st == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};

    if (!black_frame_) {
        black_frame_ = program_pool_->acquire();
        if (!black_frame_) return {MIXIT_E_OUT_OF_MEMORY, "program pool exhausted"};
        VideoFrame& f = *black_frame_;
        for (std::uint32_t y = 0; y < f.height; ++y) {
            auto* row = reinterpret_cast<std::uint32_t*>(f.data + static_cast<std::size_t>(y) * f.stride);
            std::fill(row, row + f.width, 0xFF000000u);  // B=G=R=0, A=255
        }
    }

    stop_requested_.store(false);
    // Origin slightly in the future so every thread starts on schedule.
    origin_ns_ = static_cast<std::int64_t>(monotonic_ns()) + 5'000'000;
    try {
        for (auto& in : inputs_) {
            in->reset_runtime_state();
            Input* raw = in.get();
            in->producer = std::thread([this, raw] { producer_loop(*raw); });
        }
        reset_audio_runtime();
        output_thread_ = std::thread([this] { output_loop(); });
        audio_thread_ = std::thread([this] { audio_loop(); });
    } catch (const std::system_error& e) {
        stop_threads();
        return {MIXIT_E_PLATFORM, std::string("cannot start session threads: ") + e.what()};
    }
    state_.store(SessionState::Running, std::memory_order_release);
    std::uint32_t program_input = 0;
    {
        std::lock_guard scenes(scene_mutex_);
        program_input = program_scene_.base_input;
    }
    logger_.logf(MIXIT_LOG_INFO, "session", "session started with {} input(s), program input {}, compositor {}",
                 inputs_.size(), program_input, compositor_kind_.load() == 0 ? "cpu" : "d3d11");
    return Status::ok();
}

void Session::stop_threads() noexcept {
    stop_requested_.store(true, std::memory_order_release);
    if (output_thread_.joinable()) output_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
    for (auto& in : inputs_) {
        if (in->producer.joinable()) in->producer.join();
    }
    // Return every pooled item: queues, frame-sync state, program output.
    for (auto& in : inputs_) in->reset_runtime_state();
    program_out_.clear();
    preview_out_.clear();
    {
        std::lock_guard scenes(scene_mutex_);
        if (transition_.active) {  // stopping mid-transition: land on the end state
            std::swap(program_scene_, preview_scene_);
            transition_.active = false;
            transitions_completed_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    std::lock_guard consumer(consumer_mutex_);
    program_cache_ = ProgramFrame{};
    preview_cache_ = ProgramFrame{};
}

Status Session::stop() {
    std::lock_guard lock(control_mutex_);
    const SessionState st = state();
    if (st == SessionState::Closed) return {MIXIT_E_INVALID_STATE, "session is closed"};
    if (st != SessionState::Running) return Status::ok();
    outputs_->stop_all();  // finalize recordings while frames still flow
    stop_threads();
    state_.store(SessionState::Stopped, std::memory_order_release);
    logger_.logf(MIXIT_LOG_INFO, "session", "session stopped after {} ticks ({} late, {} skipped)", ticks_.load(),
                 late_ticks_.load(), skipped_ticks_.load());
    return Status::ok();
}

void Session::close() noexcept {
    std::lock_guard lock(control_mutex_);
    if (state() == SessionState::Closed) return;
    if (outputs_) outputs_->stop_all();
    stop_threads();
    output_bus_.store(-1, std::memory_order_release);
    output_.reset();
    black_frame_.reset();
    state_.store(SessionState::Closed, std::memory_order_release);
}

// --------------------------------------------------------------------------- media threads

void Session::producer_loop(Input& in) noexcept {
    os::set_current_thread_name("mixit-src");
    SourceIo io(in.video_pool, in.audio_pool, in.video_queue, in.audio_queue, in.counters, stop_requested_, origin_ns_,
                config_.sample_rate, config_.channels, config_.sample_rate / 100);
    in.source->run(io);
}

void Session::sync_input(Input& in, std::int64_t target_ns) noexcept {
    bool advanced = false;
    for (;;) {
        if (!in.pending && !in.video_queue.pop(in.pending)) break;
        if (in.pending->pts_ns > target_ns) break;  // too new: keep for a later tick
        if ((in.pending->flags & kFrameFlagSyncFlash) || looks_like_flash(*in.pending)) {
            in.flash_valid = true;
            in.flash_pts = in.pending->pts_ns;
        }
        if (in.current && !in.current_presented) in.frames_skipped.fetch_add(1, std::memory_order_relaxed);
        in.current = std::move(in.pending);
        in.pending.reset();
        in.current_presented = false;
        advanced = true;
    }
    if (!in.current) return;
    if (advanced || !in.current_presented) {
        in.frames_presented.fetch_add(1, std::memory_order_relaxed);
        in.current_presented = true;
    } else {
        in.frames_repeated.fetch_add(1, std::memory_order_relaxed);
    }
}

SceneDraw Session::build_draw(const BusScene& scene) const noexcept {
    SceneDraw d;
    auto current_of = [this](std::uint32_t id) -> const VideoFrame* {
        if (id == 0) return nullptr;
        const Input* in = find_input(id);
        return (in != nullptr && in->current) ? &*in->current : nullptr;
    };
    if (const VideoFrame* base = current_of(scene.base_input)) {
        LayerDraw& l = d.layers[d.count++];
        l.frame = base;
        l.dest = fit_rect(base->width, base->height, config_.width, config_.height);
    }
    for (const LayerConfig& cfg : scene.layers) {
        const VideoFrame* f = current_of(cfg.input_id);
        if (f == nullptr) continue;
        LayerDraw& l = d.layers[d.count++];
        l.frame = f;
        l.crop = cfg.crop;
        l.dest = cfg.dest;
        l.opacity = cfg.opacity;
        l.key = cfg.key;
    }
    return d;
}

bool Session::render_bus(const BusScene& a, const BusScene* b, TransitionKind kind, float progress, std::int64_t target,
                         std::uint64_t tick, ProgramFrame& out, const SceneDraw* overlay) noexcept {
    out.info.tick_pts_ns = target;
    out.info.tick_index = tick;
    out.info.width = config_.width;
    out.info.height = config_.height;

    const SceneDraw da = build_draw(a);
    if (da.count >= 1 && a.base_input != 0 && da.layers[0].frame != nullptr) {
        const VideoFrame& base = *da.layers[0].frame;
        out.info.input_id = a.base_input;
        out.info.flags = base.flags;
        out.info.source_pts_ns = base.pts_ns;
        out.info.source_sequence = base.sequence;
    }
    bool has_layers = false;
    for (const LayerConfig& l : a.layers) has_layers = has_layers || l.input_id != 0;

    if (overlay != nullptr) has_layers = true;
    // Zero copy: a single full-size base input and nothing else to draw.
    if (b == nullptr && !has_layers && out.info.input_id != 0) {
        const Input* in = find_input(a.base_input);
        if (in != nullptr && in->current && in->current->width == config_.width &&
            in->current->height == config_.height) {
            out.frame = in->current;
            return true;
        }
    }
    if (b == nullptr && da.count == 0 && overlay == nullptr) {
        out.frame = black_frame_;
        return true;
    }
    out.frame = program_pool_->acquire();
    if (!out.frame) {
        program_pool_exhausted_.fetch_add(1, std::memory_order_relaxed);
        out.frame = black_frame_;
        return false;
    }
    SceneDraw db;
    if (b != nullptr) db = build_draw(*b);
    Status st = compositor_->render(da, b != nullptr ? &db : nullptr, kind, progress, *out.frame, overlay);
    if (!st.is_ok() && compositor_ != cpu_compositor_.get()) {
        // GPU failure (device removed, driver reset): continue on the CPU
        // path for the rest of the session rather than putting black on air.
        logger_.logf(MIXIT_LOG_ERROR, "session", "GPU compositor failed, switching to CPU: {}", st.message());
        compositor_ = cpu_compositor_.get();
        compositor_kind_.store(static_cast<std::int32_t>(CompositorKind::Cpu), std::memory_order_relaxed);
        compositor_fallbacks_.fetch_add(1, std::memory_order_relaxed);
        st = compositor_->render(da, b != nullptr ? &db : nullptr, kind, progress, *out.frame, overlay);
    }
    if (!st.is_ok()) {
        out.frame = black_frame_;
        return false;
    }
    composited_frames_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Session::output_loop() noexcept {
    os::set_current_thread_name("mixit-tick");
    video_running_.store(true, std::memory_order_seq_cst);
    struct Done {
        std::atomic<bool>& flag;
        ~Done() { flag.store(false, std::memory_order_seq_cst); }
    } done{video_running_};
    const Rate rate = config_.frame_rate;
    const std::int64_t period = duration_of_index(0, rate);
    std::int64_t n = 0;
    std::int64_t previous_n = -1;

    while (!stop_requested_.load(std::memory_order_acquire)) {
        const std::int64_t target = origin_ns_ + time_of_index(n, rate);
        const std::int64_t wake = target + latency_ns_;
        os::sleep_until_monotonic_ns(static_cast<std::uint64_t>(wake));
        if (stop_requested_.load(std::memory_order_acquire)) break;
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        const std::int64_t lateness = now - wake;
        ticks_.fetch_add(1, std::memory_order_relaxed);
        if (lateness > period / 2) late_ticks_.fetch_add(1, std::memory_order_relaxed);
        atomic_max(max_tick_lateness_ns_, lateness);

        for (auto& in : inputs_) {
            sync_input(*in, target);
            match_av_events(*in);
            if (in->flash_valid && target - in->flash_pts > kFlashExpiryNs) in->flash_valid = false;
        }

        // Snapshot scenes and advance a running transition.
        BusScene program_scene;
        BusScene preview_scene;
        Transition tr;
        std::array<LayerConfig, kOverlayChannels> overlay_layers{};
        {
            std::lock_guard scenes(scene_mutex_);
            program_scene = program_scene_;
            preview_scene = preview_scene_;
            const std::int64_t ticks_elapsed = previous_n < 0 ? 1 : std::max<std::int64_t>(1, n - previous_n);
            for (std::uint32_t c = 0; c < kOverlayChannels; ++c) {
                Overlay& o = overlays_[c];
                const float target_level = o.visible ? 1.0f : 0.0f;
                const float delta = o.step * static_cast<float>(ticks_elapsed);
                o.level = o.level < target_level ? std::min(target_level, o.level + delta) : std::max(target_level, o.level - delta);
                if (o.level > 0.0f && o.layer.input_id != 0) {
                    overlay_layers[c] = o.layer;
                    overlay_layers[c].opacity = o.layer.opacity * o.level;
                }
            }
            if (transition_.active) {
                // Transitions advance with output time: ticks skipped after a
                // stall count too, so the duration holds under load.
                const std::int64_t step = previous_n < 0 ? 1 : std::max<std::int64_t>(1, n - previous_n);
                transition_.elapsed = static_cast<std::uint32_t>(
                    std::min<std::int64_t>(transition_.total, static_cast<std::int64_t>(transition_.elapsed) + step));
                tr = transition_;
                if (transition_.elapsed >= transition_.total) {  // last frame shows B fully, then swap
                    std::swap(program_scene_, preview_scene_);
                    transition_.active = false;
                    transitions_completed_.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }

        ProgramFrame out;
        const float progress = tr.active ? static_cast<float>(tr.elapsed) / static_cast<float>(tr.total) : 0.0f;
        SceneDraw overlay_draw;
        for (const LayerConfig& cfg : overlay_layers) {
            if (cfg.input_id == 0) continue;
            const Input* in = find_input(cfg.input_id);
            if (in == nullptr || !in->current) continue;
            LayerDraw& l = overlay_draw.layers[overlay_draw.count++];
            l.frame = &*in->current;
            l.crop = cfg.crop;
            l.dest = cfg.dest;
            l.opacity = cfg.opacity;
            l.key = cfg.key;
        }
        publish_afv(program_scene, tr.active ? &preview_scene : nullptr, progress, overlay_layers);
        render_bus(program_scene, tr.active ? &preview_scene : nullptr, tr.kind, progress, target,
                   static_cast<std::uint64_t>(n), out, overlay_draw.count > 0 ? &overlay_draw : nullptr);
        if (out.info.input_id != 0) {
            const std::int64_t latency = target - out.info.source_pts_ns;
            video_latency_last_ns_.store(latency, std::memory_order_relaxed);
            atomic_max(video_latency_max_ns_, latency);
        }
        if (out.info.input_id == 0 && !tr.active) program_black_frames_.fetch_add(1, std::memory_order_relaxed);
        program_frames_.fetch_add(1, std::memory_order_relaxed);
        if (out.frame && outputs_->wants(0)) outputs_->feed_video(0, out.frame, static_cast<std::uint64_t>(n));
        for (auto& in : inputs_) {  // ISO recordings take each input's frame of this tick
            if (in->current && outputs_->wants(in->id)) outputs_->feed_video(in->id, in->current, static_cast<std::uint64_t>(n));
        }
        (void)program_out_.push(std::move(out));

        // Preview is rendered only while someone is watching it.
        if (now - preview_requested_ns_.load(std::memory_order_relaxed) < 1'000'000'000) {
            ProgramFrame pv;
            render_bus(preview_scene, nullptr, TransitionKind::Cut, 0.0f, target, static_cast<std::uint64_t>(n), pv);
            (void)preview_out_.push(std::move(pv));
        }

        previous_n = n;
        ++n;
        // After a long stall (debugger, sleep), resynchronize instead of
        // replaying a burst of stale ticks.
        if (lateness > 2 * period) {
            const std::int64_t resume = index_at_time(now - origin_ns_ - latency_ns_, rate) + 1;
            if (resume > n) {
                skipped_ticks_.fetch_add(static_cast<std::uint64_t>(resume - n), std::memory_order_relaxed);
                n = resume;
            }
        }
    }
}

// --------------------------------------------------------------------------- readers

SessionStats Session::stats() const {
    SessionStats s;
    s.state = state();
    {
        std::lock_guard lock(control_mutex_);
        s.input_count = static_cast<std::uint32_t>(inputs_.size());
    }
    {
        std::lock_guard scenes(scene_mutex_);
        s.program_input_id = program_scene_.base_input;
        s.preview_input_id = preview_scene_.base_input;
        s.transition_active = transition_.active ? static_cast<std::int32_t>(transition_.kind) : -1;
        s.transition_progress =
            transition_.active ? static_cast<float>(transition_.elapsed) / static_cast<float>(transition_.total) : 0.0f;
    }
    s.compositor = static_cast<CompositorKind>(compositor_kind_.load(std::memory_order_relaxed));
    s.compositor_fallbacks = compositor_fallbacks_.load(std::memory_order_relaxed);
    s.transitions_completed = transitions_completed_.load();
    s.composited_frames = composited_frames_.load();
    s.audio_blocks = audio_blocks_.load();
    s.audio_late_blocks = audio_late_blocks_.load();
    s.audio_skipped_blocks = audio_skipped_blocks_.load();
    s.audio_max_lateness_ns = audio_max_lateness_ns_.load();
    s.ticks = ticks_.load();
    s.late_ticks = late_ticks_.load();
    s.skipped_ticks = skipped_ticks_.load();
    s.max_tick_lateness_ns = max_tick_lateness_ns_.load();
    s.program_frames = program_frames_.load();
    s.program_black_frames = program_black_frames_.load();
    s.program_frames_dropped = program_out_.stats().dropped;
    s.program_pool_exhausted = program_pool_exhausted_.load();
    s.video_latency_last_ns = video_latency_last_ns_.load();
    s.video_latency_max_ns = video_latency_max_ns_.load();
    return s;
}

Status Session::input_stats(std::uint32_t input_id, InputStats& out) const {
    std::lock_guard lock(control_mutex_);
    const Input* in = find_input(input_id);
    if (in == nullptr) return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    out.input_id = in->id;
    out.frames_produced = in->counters.frames_produced.load();
    out.frames_dropped = in->video_queue.stats().dropped;
    out.frames_pool_exhausted = in->counters.frames_pool_exhausted.load();
    out.frames_presented = in->frames_presented.load();
    out.frames_repeated = in->frames_repeated.load();
    out.frames_skipped = in->frames_skipped.load();
    out.producer_late_skips = in->counters.producer_late_skips.load();
    out.audio_blocks_produced = in->counters.audio_blocks_produced.load();
    out.decode_errors = in->counters.decode_errors.load();
    out.loops = in->counters.loops.load();
    out.audio_blocks_dropped = in->audio_queue.stats().dropped;
    out.audio_blocks_consumed = in->audio_blocks_consumed.load();
    out.av_sync_events = in->av_sync_events.load();
    out.av_offset_last_ns = in->av_offset_last_ns.load();
    out.av_offset_max_abs_ns = in->av_offset_max_abs_ns.load();
    out.audio_underrun_frames = in->audio_underrun_frames.load();
    out.audio_late_blocks = in->audio_late_blocks.load();
    return Status::ok();
}

Status Session::copy_bus_frame(Bus bus, std::uint8_t* dest, std::size_t dest_size, std::uint32_t dest_stride,
                               ProgramFrameInfo& info) {
    if (bus != Bus::Program && bus != Bus::Preview) return {MIXIT_E_INVALID_ARGUMENT, "unknown bus"};
    if (bus == Bus::Preview) {
        preview_requested_ns_.store(static_cast<std::int64_t>(monotonic_ns()), std::memory_order_relaxed);
    }
    std::lock_guard consumer(consumer_mutex_);
    MediaQueue<ProgramFrame>& queue = bus == Bus::Program ? program_out_ : preview_out_;
    ProgramFrame& cache = bus == Bus::Program ? program_cache_ : preview_cache_;
    ProgramFrame latest;
    while (queue.pop(latest)) cache = std::move(latest);
    if (!cache.frame) {
        return {MIXIT_E_NOT_FOUND, bus == Bus::Program ? "no program frame has been produced yet"
                                                       : "no preview frame yet (preview renders while it is being read)"};
    }

    const VideoFrame& f = *cache.frame;
    info = cache.info;
    const std::size_t row_bytes = static_cast<std::size_t>(f.width) * 4;
    if (dest == nullptr) {  // size query: report the tightly packed size
        return {MIXIT_E_BUFFER_TOO_SMALL, "program frame needs " + std::to_string(row_bytes * f.height) + " bytes"};
    }
    if (dest_stride < row_bytes) return {MIXIT_E_INVALID_ARGUMENT, "stride is smaller than width * 4"};
    const std::size_t needed = static_cast<std::size_t>(dest_stride) * (f.height - 1) + row_bytes;
    if (dest_size < needed) {
        return {MIXIT_E_BUFFER_TOO_SMALL, "program frame needs " + std::to_string(needed) + " bytes"};
    }
    for (std::uint32_t y = 0; y < f.height; ++y) {
        std::memcpy(dest + static_cast<std::size_t>(y) * dest_stride, f.data + static_cast<std::size_t>(y) * f.stride,
                    row_bytes);
    }
    return Status::ok();
}

std::uint32_t Session::pooled_items_in_use() const {
    std::lock_guard lock(control_mutex_);
    std::uint32_t total = program_pool_ ? program_pool_->pool().in_use() : 0;
    if (black_frame_) --total;  // the black frame is a permanent session resource
    for (const auto& in : inputs_) total += in->video_pool.pool().in_use() + in->audio_pool.pool().in_use();
    return total;
}

}  // namespace mixit::media
