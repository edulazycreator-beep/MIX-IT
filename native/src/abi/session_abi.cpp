// C ABI for sessions (API 0.2): validation, handle resolution, struct
// conversion. Session behaviour lives in media/session.cpp.
#include <cstddef>
#include <memory>
#include <string>

#include "abi/abi_common.hpp"
#include "media/session.hpp"

static_assert(sizeof(mixit_session_config) == 32);
static_assert(sizeof(mixit_test_source_config) == 32);
static_assert(sizeof(mixit_session_stats) == 96);
static_assert(offsetof(mixit_session_stats, ticks) == 16);
static_assert(offsetof(mixit_session_stats, video_latency_max_ns) == 88);
static_assert(sizeof(mixit_input_stats) == 112);
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

extern "C" {

MIXIT_API mixit_result MIXIT_CALL mixit_session_create(mixit_engine engine, const mixit_session_config* config,
                                                        mixit_session* out_session) {
    constexpr const char* fn = "mixit_session_create";
    if (out_session == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_session is NULL");
    *out_session = nullptr;
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto e = mixit::abi::resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        media::SessionConfig cfg;
        cfg.width = config->width;
        cfg.height = config->height;
        cfg.frame_rate = {config->frame_rate_num, config->frame_rate_den};
        cfg.sample_rate = config->audio_sample_rate;
        cfg.channels = config->audio_channels;
        cfg.program_pool_frames = config->program_pool_frames;
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
    if (!struct_size_ok(out_stats)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_stats NULL or struct_size too small");
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
        write_out(out_stats, o);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_input_stats(mixit_session session, uint32_t input_id,
                                                                 mixit_input_stats* out_stats) {
    constexpr const char* fn = "mixit_session_get_input_stats";
    if (!struct_size_ok(out_stats)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_stats NULL or struct_size too small");
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
        write_out(out_stats, o);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_copy_program_frame(mixit_session session, uint8_t* buffer,
                                                                    uint32_t buffer_size, uint32_t stride,
                                                                    mixit_video_frame_info* out_info) {
    constexpr const char* fn = "mixit_session_copy_program_frame";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    if (buffer == nullptr && buffer_size != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "buffer is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = resolve(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        media::ProgramFrameInfo info;
        const Status r = s->copy_program_frame(buffer, buffer_size, stride, info);
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

}  // extern "C"
