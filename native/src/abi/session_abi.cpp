// C ABI for sessions (API 0.2): validation, handle resolution, struct
// conversion. Session behaviour lives in media/session.cpp.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "abi/abi_common.hpp"
#include "abi/session_handles.hpp"
#include "core/last_error.hpp"
#include "media/capture_source.hpp"
#include "media/file_source.hpp"
#include "media/session.hpp"

static_assert(sizeof(mixit_session_config) == 40);
constexpr std::uint32_t kSessionConfigV02Size = 32;
constexpr std::uint32_t kLayerConfigV03Size = 48;
static_assert(sizeof(mixit_layer_config) == 80);
static_assert(sizeof(mixit_overlay_info) == 16);
static_assert(sizeof(mixit_test_source_config) == 32);
static_assert(sizeof(mixit_session_stats) == 168);
static_assert(offsetof(mixit_session_stats, preview_input_id) == 96);

// Size of mixit_session_stats as defined by API 0.2 (before appended fields).
constexpr std::uint32_t kSessionStatsV02Size = 96;
static_assert(offsetof(mixit_session_stats, ticks) == 16);
static_assert(offsetof(mixit_session_stats, video_latency_max_ns) == 88);
static_assert(sizeof(mixit_input_stats) == 144);
static_assert(offsetof(mixit_input_stats, decode_errors) == 112);
static_assert(sizeof(mixit_file_source_config) == 16);
static_assert(sizeof(mixit_capture_device_info) == 648);
static_assert(sizeof(mixit_capture_source_config) == 32);

// Size of mixit_input_stats as defined by API 0.2 (before appended fields).
constexpr std::uint32_t kInputStatsV02Size = 112;
static_assert(offsetof(mixit_input_stats, frames_produced) == 8);
static_assert(offsetof(mixit_input_stats, av_offset_max_abs_ns) == 104);
static_assert(sizeof(mixit_video_frame_info) == 56);
static_assert(offsetof(mixit_video_frame_info, tick_pts_ns) == 24);
static_assert(offsetof(mixit_video_frame_info, tick_index) == 48);

namespace {

using mixit::Status;
using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::struct_size_ok;
using mixit::abi::to_handle;
using mixit::abi::to_id;
using mixit::abi::write_out;
namespace media = mixit::media;

// A session handle keeps its engine object alive (the logger it writes to),
// even after the engine handle was destroyed; the engine closes the session
// on shutdown, so a session never outlives a running engine.
struct SessionHandle {
    std::shared_ptr<mixit::Engine> engine;
    std::shared_ptr<media::Session> session;
};

mixit::HandleRegistry<SessionHandle>& session_registry() {
    static auto* instance = new mixit::HandleRegistry<SessionHandle>();  // leaked on purpose, see engine_abi.cpp
    return *instance;
}

std::shared_ptr<media::Session> resolve(mixit_session handle) {
    if (handle == nullptr) return nullptr;
    const auto h = session_registry().find(to_id(handle));
    return h ? h->session : nullptr;
}

constexpr const char* kInvalidSession = "invalid session handle";

}  // namespace

std::shared_ptr<mixit::media::Session> mixit::abi::resolve_session(mixit_session handle) { return resolve(handle); }

namespace {

mixit_result copy_frame(const char* fn, mixit_session session, media::Bus bus, uint8_t* buffer, uint32_t buffer_size,
                        uint32_t stride, mixit_video_frame_info* out_info) noexcept {
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    if (buffer == nullptr && buffer_size != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "buffer is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::ProgramFrameInfo info;
        const Status r = s->copy_bus_frame(bus, buffer, buffer_size, stride, info);
        if (r.is_ok() || r.code() == MIXIT_E_BUFFER_TOO_SMALL) {
            mixit_video_frame_info o{};
            o.struct_size = sizeof(o);
            o.width = info.width;
            o.height = info.height;
            o.pixel_format = MIXIT_PIXEL_FORMAT_BGRA8;
            o.input_id = info.input_id;
            o.flags = info.flags;
            o.tick_pts_ns = info.tick_pts_ns;
            o.source_pts_ns = info.source_pts_ns;
            o.source_sequence = info.source_sequence;
            o.tick_index = info.tick_index;
            write_out(out_info, o);
        }
        return report(fn, r);
    });
}

// Converts a caller's layer struct (0.3 size or newer) into the engine form.
mixit_result to_layer_config(const char* fn, const mixit_layer_config* layer, media::LayerConfig& cfg) {
    if (!mixit::abi::struct_size_at_least(layer, kLayerConfigV03Size)) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "layer NULL or struct_size too small");
    }
    mixit_layer_config c{};
    std::memcpy(&c, layer, std::min<std::size_t>(layer->struct_size, sizeof(c)));  // shorter = older ABI
    if (layer->struct_size < sizeof(mixit_layer_config)) c.key_type = MIXIT_KEY_NONE;
    if (c.reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
    if ((c.key_flags & ~MIXIT_KEY_FLAG_INVERT) != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown key flags");
    cfg.input_id = c.input_id;
    cfg.crop = {c.crop_x, c.crop_y, c.crop_w, c.crop_h};
    cfg.dest = {c.dest_x, c.dest_y, c.dest_w, c.dest_h};
    cfg.opacity = c.opacity;
    cfg.key.type = static_cast<media::KeyType>(c.key_type);
    cfg.key.color = c.key_color;
    cfg.key.similarity = c.key_similarity;
    cfg.key.smoothness = c.key_smoothness;
    cfg.key.spill = c.key_spill;
    cfg.key.threshold = c.luma_threshold;
    cfg.key.softness = c.luma_softness;
    cfg.key.invert = (c.key_flags & MIXIT_KEY_FLAG_INVERT) != 0;
    if (cfg.key.type == media::KeyType::None) cfg.key = media::KeySettings{};
    return MIXIT_OK;
}

}  // namespace

extern "C" {

MIXIT_API mixit_result MIXIT_CALL mixit_session_create(mixit_engine engine, const mixit_session_config* config,
                                                        mixit_session* out_session) {
    constexpr const char* fn = "mixit_session_create";
    if (out_session == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_session is NULL");
    *out_session = nullptr;
    if (!mixit::abi::struct_size_at_least(config, kSessionConfigV02Size)) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    }
    return guarded(fn, [&]() -> mixit_result {
        mixit_session_config c{};
        std::memcpy(&c, config, std::min<std::size_t>(config->struct_size, sizeof(c)));  // shorter = older ABI
        config = &c;
        if (c.compositor < MIXIT_COMPOSITOR_PREFER_AUTO || c.compositor > MIXIT_COMPOSITOR_PREFER_D3D11_WARP) {
            return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown compositor preference");
        }
        if (c.reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
        const auto e = mixit::abi::resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        media::SessionConfig cfg;
        cfg.width = config->width;
        cfg.height = config->height;
        cfg.frame_rate = {config->frame_rate_num, config->frame_rate_den};
        cfg.sample_rate = config->audio_sample_rate;
        cfg.channels = config->audio_channels;
        cfg.program_pool_frames = config->program_pool_frames;
        cfg.compositor = static_cast<media::CompositorPreference>(config->compositor);
        std::shared_ptr<media::Session> session;
        if (Status s = e->create_session(cfg, session); !s.is_ok()) return report(fn, s);
        auto handle = std::make_shared<SessionHandle>(SessionHandle{e, std::move(session)});
        *out_session = to_handle<mixit_session>(session_registry().add(std::move(handle)));
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_destroy(mixit_session session) {
    constexpr const char* fn = "mixit_session_destroy";
    return guarded(fn, [&]() -> mixit_result {
        const auto h = session == nullptr ? nullptr : session_registry().remove(to_id(session));
        if (!h) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        h->session->close();  // joins session threads before returning
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_test_source(mixit_session session,
                                                                 const mixit_test_source_config* config,
                                                                 uint32_t* out_input_id) {
    constexpr const char* fn = "mixit_session_add_test_source";
    if (out_input_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_input_id is NULL");
    *out_input_id = 0;
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (config->reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::TestSignalConfig cfg;
        cfg.width = config->width;
        cfg.height = config->height;
        cfg.frame_rate = {config->frame_rate_num, config->frame_rate_den};
        cfg.tone_hz = config->tone_hz;
        cfg.flash_interval_frames = config->flash_interval_frames;
        return report(fn, s->add_test_source(cfg, *out_input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_file_source(mixit_session session,
                                                                 const mixit_file_source_config* config,
                                                                 uint32_t* out_input_id) {
    constexpr const char* fn = "mixit_session_add_file_source";
    if (out_input_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_input_id is NULL");
    *out_input_id = 0;
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (config->path == nullptr || config->path[0] == '\0') return fail(fn, MIXIT_E_INVALID_ARGUMENT, "path is NULL or empty");
    if ((config->flags & ~MIXIT_FILE_SOURCE_LOOP) != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown flags");
    return guarded(fn, [&]() -> mixit_result {
        const auto h = session == nullptr ? nullptr : session_registry().find(to_id(session));
        if (!h) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::FileSourceOptions options;
        options.loop = (config->flags & MIXIT_FILE_SOURCE_LOOP) != 0;
        std::unique_ptr<media::MediaSource> source;
        if (Status st = media::open_file_source(config->path, options, h->engine->logger(), source); !st.is_ok()) {
            return report(fn, st);
        }
        return report(fn, h->session->add_source(std::move(source), *out_input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_enumerate_capture_devices(mixit_engine engine, mixit_capture_device_info* devices,
                                                                   uint32_t capacity, uint32_t* out_count) {
    constexpr const char* fn = "mixit_enumerate_capture_devices";
    if (out_count == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_count is NULL");
    *out_count = 0;
    if (capacity > 0 && devices == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "devices is NULL");
    for (uint32_t i = 0; i < capacity; ++i) {
        if (devices[i].struct_size < sizeof(mixit_capture_device_info)) {
            return fail(fn, MIXIT_E_INVALID_ARGUMENT, "devices[i].struct_size too small");
        }
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto e = mixit::abi::resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        std::vector<media::CaptureDeviceInfo> found;
        if (Status st = media::enumerate_capture_devices(e->logger(), found); !st.is_ok()) return report(fn, st);
        *out_count = static_cast<uint32_t>(found.size());
        for (uint32_t i = 0; i < capacity && i < found.size(); ++i) {
            mixit_capture_device_info info{};
            info.struct_size = sizeof(info);
            mixit::copy_utf8_truncated(info.name, sizeof(info.name), found[i].name);
            if (found[i].id.size() >= sizeof(info.id)) continue;  // an id must never be truncated
            std::memcpy(info.id, found[i].id.c_str(), found[i].id.size() + 1);
            devices[i] = info;
        }
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_capture_source(mixit_session session,
                                                                    const mixit_capture_source_config* config,
                                                                    uint32_t* out_input_id) {
    constexpr const char* fn = "mixit_session_add_capture_source";
    if (out_input_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_input_id is NULL");
    *out_input_id = 0;
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (config->device_id == nullptr || config->device_id[0] == '\0') {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "device_id is NULL or empty");
    }
    if (config->flags != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown flags");
    if (config->frame_rate_num != 0 && config->frame_rate_den == 0) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "frame_rate_den is 0");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto h = session == nullptr ? nullptr : session_registry().find(to_id(session));
        if (!h) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::CaptureOptions options;
        options.device_id = config->device_id;
        options.width = config->width;
        options.height = config->height;
        options.frame_rate = {config->frame_rate_num, config->frame_rate_num == 0 ? 1u : config->frame_rate_den};
        std::unique_ptr<media::MediaSource> source;
        if (Status st = media::open_capture_source(options, h->engine->logger(), source); !st.is_ok()) {
            return report(fn, st);
        }
        return report(fn, h->session->add_source(std::move(source), *out_input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_remove_input(mixit_session session, uint32_t input_id) {
    constexpr const char* fn = "mixit_session_remove_input";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->remove_input(input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_program_input(mixit_session session, uint32_t input_id) {
    constexpr const char* fn = "mixit_session_set_program_input";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_program_input(input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_start(mixit_session session) {
    constexpr const char* fn = "mixit_session_start";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->start());
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_stop(mixit_session session) {
    constexpr const char* fn = "mixit_session_stop";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->stop());
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_stats(mixit_session session, mixit_session_stats* out_stats) {
    constexpr const char* fn = "mixit_session_get_stats";
    if (!mixit::abi::struct_size_at_least(out_stats, kSessionStatsV02Size)) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_stats NULL or struct_size too small");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        const media::SessionStats st = s->stats();
        mixit_session_stats o{};
        o.struct_size = sizeof(o);
        o.state = static_cast<int32_t>(st.state);
        o.input_count = st.input_count;
        o.program_input_id = st.program_input_id;
        o.ticks = st.ticks;
        o.late_ticks = st.late_ticks;
        o.skipped_ticks = st.skipped_ticks;
        o.max_tick_lateness_ns = st.max_tick_lateness_ns;
        o.program_frames = st.program_frames;
        o.program_black_frames = st.program_black_frames;
        o.program_frames_dropped = st.program_frames_dropped;
        o.program_pool_exhausted = st.program_pool_exhausted;
        o.video_latency_last_ns = st.video_latency_last_ns;
        o.video_latency_max_ns = st.video_latency_max_ns;
        o.preview_input_id = st.preview_input_id;
        o.transition_active = st.transition_active;
        o.transition_progress = st.transition_progress;
        o.compositor = static_cast<int32_t>(st.compositor);
        o.transitions_completed = st.transitions_completed;
        o.composited_frames = st.composited_frames;
        o.compositor_fallbacks = st.compositor_fallbacks;
        o.audio_blocks = st.audio_blocks;
        o.audio_late_blocks = st.audio_late_blocks;
        o.audio_skipped_blocks = st.audio_skipped_blocks;
        o.audio_max_lateness_ns = st.audio_max_lateness_ns;
        mixit::abi::write_out_prefix(out_stats, o);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_input_stats(mixit_session session, uint32_t input_id,
                                                                 mixit_input_stats* out_stats) {
    constexpr const char* fn = "mixit_session_get_input_stats";
    // Callers built against API 0.2 pass the shorter struct; the appended
    // fields are simply not written for them.
    if (out_stats == nullptr || out_stats->struct_size < kInputStatsV02Size) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_stats NULL or struct_size too small");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::InputStats st;
        if (Status r = s->input_stats(input_id, st); !r.is_ok()) return report(fn, r);
        mixit_input_stats o{};
        o.struct_size = sizeof(o);
        o.input_id = st.input_id;
        o.frames_produced = st.frames_produced;
        o.frames_dropped = st.frames_dropped;
        o.frames_pool_exhausted = st.frames_pool_exhausted;
        o.frames_presented = st.frames_presented;
        o.frames_repeated = st.frames_repeated;
        o.frames_skipped = st.frames_skipped;
        o.producer_late_skips = st.producer_late_skips;
        o.audio_blocks_produced = st.audio_blocks_produced;
        o.audio_blocks_dropped = st.audio_blocks_dropped;
        o.audio_blocks_consumed = st.audio_blocks_consumed;
        o.av_sync_events = st.av_sync_events;
        o.av_offset_last_ns = st.av_offset_last_ns;
        o.av_offset_max_abs_ns = st.av_offset_max_abs_ns;
        o.decode_errors = st.decode_errors;
        o.loops = st.loops;
        o.audio_underrun_frames = st.audio_underrun_frames;
        o.audio_late_blocks = st.audio_late_blocks;
        const std::uint32_t caller_size = out_stats->struct_size;
        std::memcpy(out_stats, &o, std::min<std::size_t>(caller_size, sizeof(o)));
        out_stats->struct_size = caller_size;
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_preview_input(mixit_session session, uint32_t input_id) {
    constexpr const char* fn = "mixit_session_set_preview_input";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_preview_input(input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_cut(mixit_session session) {
    constexpr const char* fn = "mixit_session_cut";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->cut());
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_start_transition(mixit_session session, int32_t kind,
                                                                  uint32_t duration_frames) {
    constexpr const char* fn = "mixit_session_start_transition";
    if (kind < MIXIT_TRANSITION_CUT || kind > MIXIT_TRANSITION_WIPE) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown transition kind");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->start_transition(static_cast<media::TransitionKind>(kind), duration_frames));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_layer(mixit_session session, int32_t bus, uint32_t index,
                                                           const mixit_layer_config* layer) {
    constexpr const char* fn = "mixit_session_set_layer";
    media::LayerConfig cfg;
    if (mixit_result r = to_layer_config(fn, layer, cfg); r != MIXIT_OK) return r;
    if (bus != MIXIT_BUS_PROGRAM && bus != MIXIT_BUS_PREVIEW) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown bus");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_layer(static_cast<media::Bus>(bus), index, cfg));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_overlay(mixit_session session, uint32_t channel,
                                                             const mixit_layer_config* layer) {
    constexpr const char* fn = "mixit_session_set_overlay";
    media::LayerConfig cfg;
    if (mixit_result r = to_layer_config(fn, layer, cfg); r != MIXIT_OK) return r;
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_overlay(channel, cfg));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_show_overlay(mixit_session session, uint32_t channel, int32_t visible,
                                                              uint32_t fade_frames) {
    constexpr const char* fn = "mixit_session_show_overlay";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->show_overlay(channel, visible != 0, fade_frames));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_overlay(mixit_session session, uint32_t channel,
                                                             mixit_overlay_info* out_info) {
    constexpr const char* fn = "mixit_session_get_overlay";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::Session::OverlayInfo info;
        if (Status st = s->overlay_info(channel, info); !st.is_ok()) return report(fn, st);
        mixit_overlay_info c{};
        c.struct_size = sizeof(c);
        c.input_id = info.layer.input_id;
        c.visible = info.visible ? 1 : 0;
        c.level = info.level;
        write_out(out_info, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_copy_bus_frame(mixit_session session, int32_t bus, uint8_t* buffer,
                                                                uint32_t buffer_size, uint32_t stride,
                                                                mixit_video_frame_info* out_info) {
    constexpr const char* fn = "mixit_session_copy_bus_frame";
    if (bus != MIXIT_BUS_PROGRAM && bus != MIXIT_BUS_PREVIEW) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown bus");
    return copy_frame(fn, session, static_cast<media::Bus>(bus), buffer, buffer_size, stride, out_info);
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_copy_program_frame(mixit_session session, uint8_t* buffer,
                                                                    uint32_t buffer_size, uint32_t stride,
                                                                    mixit_video_frame_info* out_info) {
    return copy_frame("mixit_session_copy_program_frame", session, media::Bus::Program, buffer, buffer_size, stride,
                      out_info);
}

}  // extern "C"
