// C ABI: instant replay (API 0.6).
#include <cstddef>

#include "abi/abi_common.hpp"
#include "abi/session_handles.hpp"
#include "mixit/mixit_engine.h"
#include "replay/replay.hpp"

static_assert(sizeof(mixit_replay_buffer_info) == 56);
static_assert(sizeof(mixit_replay_state) == 24);

namespace {

using mixit::Status;
using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::struct_size_ok;
using mixit::abi::write_out;
using mixit::replay::ReplaySource;

constexpr const char* kInvalidSession = "invalid session handle";

}  // namespace

extern "C" {

MIXIT_API mixit_result MIXIT_CALL mixit_session_enable_replay(mixit_session session, uint32_t input_id, uint32_t seconds,
                                                               uint32_t quality) {
    constexpr const char* fn = "mixit_session_enable_replay";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->enable_replay(input_id, seconds, quality));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_replay_buffer(mixit_session session, uint32_t input_id,
                                                                   mixit_replay_buffer_info* out_info) {
    constexpr const char* fn = "mixit_session_get_replay_buffer";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        mixit::replay::BufferInfo i;
        if (Status st = s->replay_buffer_info(input_id, i); !st.is_ok()) return report(fn, st);
        mixit_replay_buffer_info c{};
        c.struct_size = sizeof(c);
        c.width = i.width;
        c.height = i.height;
        c.oldest_ns = i.oldest_ns;
        c.newest_ns = i.newest_ns;
        c.frames = i.frames;
        c.bytes = i.bytes;
        c.dropped = i.dropped;
        write_out(out_info, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_replay_source(mixit_session session, uint32_t source_input_id,
                                                                   uint32_t* out_input_id) {
    constexpr const char* fn = "mixit_session_add_replay_source";
    if (out_input_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_input_id is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->add_replay_source(source_input_id, *out_input_id));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_replay_play(mixit_session session, uint32_t replay_input_id,
                                                             int64_t from_ns, float speed, int32_t paused) {
    constexpr const char* fn = "mixit_session_replay_play";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->with_replay(replay_input_id, [&](ReplaySource& r) { return r.play(from_ns, speed, paused != 0); }));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_replay_control(mixit_session session, uint32_t replay_input_id,
                                                                float speed, int32_t paused) {
    constexpr const char* fn = "mixit_session_replay_control";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->with_replay(replay_input_id, [&](ReplaySource& r) {
            if (speed > 0.0f) {
                if (Status st = r.set_speed(speed); !st.is_ok()) return st;
            }
            if (paused >= 0) return r.set_paused(paused != 0);
            return Status::ok();
        }));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_replay_state(mixit_session session, uint32_t replay_input_id,
                                                                  mixit_replay_state* out_state) {
    constexpr const char* fn = "mixit_session_get_replay_state";
    if (!struct_size_ok(out_state)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_state NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        ReplaySource::State st;
        if (Status r = s->with_replay(replay_input_id, [&](ReplaySource& src) {
                st = src.state();
                return Status::ok();
            });
            !r.is_ok()) {
            return report(fn, r);
        }
        mixit_replay_state c{};
        c.struct_size = sizeof(c);
        c.active = st.active ? 1 : 0;
        c.paused = st.paused ? 1 : 0;
        c.speed = st.speed;
        c.position_ns = st.position_ns;
        write_out(out_state, c);
        return MIXIT_OK;
    });
}

}  // extern "C"
