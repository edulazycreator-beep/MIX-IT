// C ABI: graphics inputs (API 0.6).
#include <cstddef>
#include <string>

#include "abi/abi_common.hpp"
#include "abi/session_handles.hpp"
#include "core/last_error.hpp"
#include "media/graphics_source.hpp"
#include "mixit/mixit_engine.h"

static_assert(sizeof(mixit_graphics_source_config) == 24);
static_assert(sizeof(mixit_graphics_info) == 288);
static_assert(offsetof(mixit_graphics_info, frames_rendered) == 24);

namespace {

using mixit::Status;
using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::struct_size_ok;
using mixit::abi::write_out;
using mixit::media::GraphicsSource;
using mixit::media::GraphicsTimerMode;

constexpr const char* kInvalidSession = "invalid session handle";

}  // namespace

extern "C" {

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_graphics_source(mixit_session session,
                                                                     const mixit_graphics_source_config* config,
                                                                     uint32_t* out_input_id) {
    constexpr const char* fn = "mixit_session_add_graphics_source";
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (out_input_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_input_id is NULL");
    if (config->scene_json == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "scene_json is NULL");
    if (config->reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
#if !MIXIT_HAVE_GRAPHICS
        return fail(fn, MIXIT_E_NOT_SUPPORTED, "this engine was built without the graphics renderer");
#else
        std::unique_ptr<GraphicsSource> g;
        if (Status st = GraphicsSource::create(config->width, config->height, config->scene_json, g); !st.is_ok()) {
            return report(fn, st);
        }
        return report(fn, s->add_source(std::move(g), *out_input_id));
#endif
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_graphics_scene(mixit_session session, uint32_t input_id,
                                                                    const char* scene_json) {
    constexpr const char* fn = "mixit_session_set_graphics_scene";
    if (scene_json == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "scene_json is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        const std::string json = scene_json;
        return report(fn, s->with_graphics(input_id, [&](GraphicsSource& g) { return g.set_scene(json); }));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_graphics_field(mixit_session session, uint32_t input_id,
                                                                    const char* name, const char* value) {
    constexpr const char* fn = "mixit_session_set_graphics_field";
    if (name == nullptr || value == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "name and value are required");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        const std::string n = name, v = value;
        return report(fn, s->with_graphics(input_id, [&](GraphicsSource& g) { return g.set_field(n, v); }));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_graphics_timer(mixit_session session, uint32_t input_id,
                                                                    int32_t mode, int64_t value_ms, int32_t running) {
    constexpr const char* fn = "mixit_session_set_graphics_timer";
    if (mode != MIXIT_GRAPHICS_TIMER_STOPWATCH && mode != MIXIT_GRAPHICS_TIMER_COUNTDOWN) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown timer mode");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->with_graphics(input_id, [&](GraphicsSource& g) {
            return g.set_timer(static_cast<GraphicsTimerMode>(mode), value_ms, running != 0);
        }));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_graphics_info(mixit_session session, uint32_t input_id,
                                                                   mixit_graphics_info* out_info) {
    constexpr const char* fn = "mixit_session_get_graphics_info";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        GraphicsSource::Info info;
        if (Status st = s->with_graphics(input_id, [&](GraphicsSource& g) {
                info = g.info();
                return Status::ok();
            });
            !st.is_ok()) {
            return report(fn, st);
        }
        mixit_graphics_info c{};
        c.struct_size = sizeof(c);
        c.timer_mode = static_cast<int32_t>(info.timer_mode);
        c.timer_ms = info.timer_ms;
        c.timer_running = info.timer_running ? 1 : 0;
        c.frames_rendered = info.frames_rendered;
        mixit::copy_utf8_truncated(c.warnings, sizeof(c.warnings), info.warnings);
        write_out(out_info, c);
        return MIXIT_OK;
    });
}

}  // extern "C"
