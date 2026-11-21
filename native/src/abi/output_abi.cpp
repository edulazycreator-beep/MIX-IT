// C ABI: outputs — recording and streaming (API 0.5).
#include <cstddef>
#include <string>
#include <vector>

#include "abi/abi_common.hpp"
#include "abi/session_handles.hpp"
#include "core/last_error.hpp"
#include "mixit/mixit_engine.h"
#include "output/output.hpp"

static_assert(sizeof(mixit_output_config) == 56);
static_assert(offsetof(mixit_output_config, video_kbps) == 32);
static_assert(sizeof(mixit_output_stats) == 888);
static_assert(offsetof(mixit_output_stats, duration_ns) == 72);
static_assert(offsetof(mixit_output_stats, encoder) == 88);
static_assert(sizeof(mixit_video_encoder_info) == 360);
static_assert(MIXIT_OUTPUT_FAILED == static_cast<int>(mixit::output::OutputState::Failed));
static_assert(MIXIT_OUTPUT_SOURCE_MULTIVIEW == mixit::media::Session::kMultiviewSource);

namespace {

using mixit::Status;
using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::struct_size_ok;
using mixit::abi::write_out;

constexpr const char* kInvalidSession = "invalid session handle";

template <std::size_t N>
void put(char (&dest)[N], const std::string& text) {
    mixit::copy_utf8_truncated(dest, N, text);
}

}  // namespace

extern "C" {

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_output(mixit_session session, const mixit_output_config* config,
                                                            uint32_t* out_output_id) {
    constexpr const char* fn = "mixit_session_add_output";
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (out_output_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_output_id is NULL");
    if (config->url == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "url is NULL");
    if (config->reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
    if ((config->flags & ~(MIXIT_OUTPUT_FLAG_NOT_FRAGMENTED | MIXIT_OUTPUT_FLAG_NO_RECONNECT)) != 0) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown flags");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        mixit::output::OutputConfig c;
        c.url = config->url;
        c.format = config->format != nullptr ? config->format : "";
        c.video_encoder = config->video_encoder != nullptr ? config->video_encoder : "";
        c.source_input = config->source_input;
        if (config->video_kbps != 0) c.video_kbps = config->video_kbps;
        if (config->keyframe_ms != 0) c.keyframe_ms = config->keyframe_ms;
        if (config->audio_kbps != 0) c.audio_kbps = config->audio_kbps;
        c.fragmented = (config->flags & MIXIT_OUTPUT_FLAG_NOT_FRAGMENTED) == 0;
        c.reconnect = (config->flags & MIXIT_OUTPUT_FLAG_NO_RECONNECT) == 0;
        c.segment_seconds = config->segment_seconds;
        return report(fn, s->add_output(c, *out_output_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_remove_output(mixit_session session, uint32_t output_id) {
    constexpr const char* fn = "mixit_session_remove_output";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->remove_output(output_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_start_output(mixit_session session, uint32_t output_id) {
    constexpr const char* fn = "mixit_session_start_output";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->start_output(output_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_stop_output(mixit_session session, uint32_t output_id) {
    constexpr const char* fn = "mixit_session_stop_output";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->stop_output(output_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_output_stats(mixit_session session, uint32_t output_id,
                                                                  mixit_output_stats* out_stats) {
    constexpr const char* fn = "mixit_session_get_output_stats";
    if (!struct_size_ok(out_stats)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_stats NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        mixit::output::OutputStats st;
        if (Status r = s->output_stats(output_id, st); !r.is_ok()) return report(fn, r);
        mixit_output_stats c{};
        c.struct_size = sizeof(c);
        c.state = static_cast<int32_t>(st.state);
        c.flags = (st.hardware_encoder ? MIXIT_OUTPUT_STATS_HARDWARE_ENCODER : 0u) |
                  (st.data_lost ? MIXIT_OUTPUT_STATS_DATA_LOST : 0u);
        c.segments = st.segments;
        c.bytes_written = st.bytes_written;
        c.video_packets = st.video_packets;
        c.audio_packets = st.audio_packets;
        c.dropped_packets = st.dropped_packets;
        c.encoder_dropped_frames = st.encoder_dropped_frames;
        c.reconnects = st.reconnects;
        c.bitrate_kbps = st.bitrate_kbps;
        c.queue_ms = st.queue_ms;
        c.duration_ns = st.duration_ns;
        c.free_bytes = st.free_bytes;
        put(c.encoder, st.encoder);
        put(c.current_file, st.current_file);
        put(c.last_error, st.last_error);
        write_out(out_stats, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_query_video_encoders(mixit_video_encoder_info* encoders, uint32_t capacity,
                                                              uint32_t* out_count) {
    constexpr const char* fn = "mixit_query_video_encoders";
    if (out_count == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_count is NULL");
    *out_count = 0;
    if (capacity > 0 && encoders == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "encoders is NULL");
    for (uint32_t i = 0; i < capacity; ++i) {
        if (encoders[i].struct_size < sizeof(mixit_video_encoder_info)) {
            return fail(fn, MIXIT_E_INVALID_ARGUMENT, "encoders[i].struct_size too small");
        }
    }
    return guarded(fn, [&]() -> mixit_result {
        std::vector<mixit::output::EncoderInfo> list;
        if (Status st = mixit::output::query_video_encoders(list); !st.is_ok()) return report(fn, st);
        *out_count = static_cast<uint32_t>(list.size());
        for (uint32_t i = 0; i < capacity && i < list.size(); ++i) {
            mixit_video_encoder_info e{};
            e.struct_size = sizeof(e);
            e.flags = (list[i].hardware ? MIXIT_ENCODER_HARDWARE : 0u) | (list[i].usable ? MIXIT_ENCODER_USABLE : 0u);
            put(e.name, list[i].name);
            put(e.long_name, list[i].long_name);
            put(e.error, list[i].error);
            encoders[i] = e;
        }
        return capacity > 0 && list.size() > capacity
                   ? fail(fn, MIXIT_E_BUFFER_TOO_SMALL, "more encoders than capacity; see out_count")
                   : MIXIT_OK;
    });
}

}  // extern "C"
