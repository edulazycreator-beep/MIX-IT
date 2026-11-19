// C ABI: VST3 plug-in scanning and mixer inserts (API 0.4, EXPERIMENTAL).
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "abi/abi_common.hpp"
#include "abi/session_handles.hpp"
#include "audio/vst3_host.hpp"
#include "core/last_error.hpp"
#include "mixit/mixit_engine.h"

static_assert(sizeof(mixit_vst3_plugin_info) == 1008);
static_assert(sizeof(mixit_audio_insert_config) == 40);
static_assert(sizeof(mixit_audio_insert_info) == 320);
static_assert(sizeof(mixit_audio_insert_parameter) == 256);
static_assert(offsetof(mixit_audio_insert_parameter, default_value) == 16);
static_assert(MIXIT_AUDIO_INSERT_SLOTS == mixit::media::Session::kInsertSlots);

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

MIXIT_API mixit_result MIXIT_CALL mixit_scan_vst3(mixit_engine engine, const char* path,
                                                  mixit_vst3_plugin_info* plugins, uint32_t capacity,
                                                  uint32_t* out_count) {
    constexpr const char* fn = "mixit_scan_vst3";
    if (out_count == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_count is NULL");
    *out_count = 0;
    if (capacity > 0 && plugins == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "plugins is NULL");
    for (uint32_t i = 0; i < capacity; ++i) {
        if (plugins[i].struct_size < sizeof(mixit_vst3_plugin_info)) {
            return fail(fn, MIXIT_E_INVALID_ARGUMENT, "plugins[i].struct_size too small");
        }
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto e = mixit::abi::resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        std::vector<std::string> roots;
        if (path != nullptr && *path != '\0') {
            roots.emplace_back(path);
        } else {
            roots = mixit::audio::vst3_default_folders();
        }
        std::vector<mixit::audio::Vst3PluginInfo> all;
        for (const std::string& root : roots) {
            std::vector<mixit::audio::Vst3PluginInfo> found;
            std::vector<std::string> errors;
            const Status st = mixit::audio::vst3_scan(root, found, errors);
            if (!st.is_ok()) {
                if (path != nullptr && *path != '\0') return report(fn, st);
                e->logger().logf(MIXIT_LOG_WARNING, "vst3", "scan of {} failed: {}", root, st.message());
            }
            for (const std::string& err : errors) e->logger().logf(MIXIT_LOG_WARNING, "vst3", "skipped {}", err);
            all.insert(all.end(), found.begin(), found.end());
        }
        uint32_t written = 0;
        for (const auto& p : all) {
            if (p.path.size() >= sizeof(mixit_vst3_plugin_info::path)) {
                e->logger().logf(MIXIT_LOG_WARNING, "vst3", "skipped {} (path too long)", p.path);
                continue;
            }
            if (written < capacity) {
                mixit_vst3_plugin_info info{};
                info.struct_size = sizeof(info);
                put(info.class_id, p.class_id);
                put(info.name, p.name);
                put(info.vendor, p.vendor);
                put(info.version, p.version);
                put(info.subcategories, p.subcategories);
                std::memcpy(info.path, p.path.c_str(), p.path.size() + 1);
                plugins[written] = info;
            }
            ++written;
        }
        *out_count = written;
        return written > capacity && capacity > 0
                   ? fail(fn, MIXIT_E_BUFFER_TOO_SMALL, "more plug-ins than capacity; see out_count")
                   : MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_insert(mixit_session session, uint32_t input_id,
                                                                  const mixit_audio_insert_config* config) {
    constexpr const char* fn = "mixit_session_set_audio_insert";
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (config->reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
    if (config->path == nullptr || config->class_id == nullptr) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "path and class_id are required");
    }
    if (config->state_size > 0 && config->state == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "state is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        std::vector<std::uint8_t> state;
        if (config->state_size > 0) state.assign(config->state, config->state + config->state_size);
        return report(fn, s->set_audio_insert(input_id, config->slot, config->path, config->class_id, state));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_remove_audio_insert(mixit_session session, uint32_t input_id,
                                                                     uint32_t slot) {
    constexpr const char* fn = "mixit_session_remove_audio_insert";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->remove_audio_insert(input_id, slot));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_insert_bypass(mixit_session session, uint32_t input_id,
                                                                         uint32_t slot, int32_t bypass) {
    constexpr const char* fn = "mixit_session_set_audio_insert_bypass";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_audio_insert_bypass(input_id, slot, bypass != 0));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_insert(mixit_session session, uint32_t input_id,
                                                                  uint32_t slot, mixit_audio_insert_info* out_info) {
    constexpr const char* fn = "mixit_session_get_audio_insert";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        mixit::media::Session::InsertInfo info;
        if (Status st = s->get_audio_insert(input_id, slot, info); !st.is_ok()) return report(fn, st);
        mixit_audio_insert_info c{};
        c.struct_size = sizeof(c);
        c.flags = info.bypass ? MIXIT_AUDIO_INSERT_FLAG_BYPASS : 0u;
        c.parameter_count = info.parameter_count;
        c.latency_samples = info.latency_samples;
        c.process_errors = info.process_errors;
        put(c.class_id, info.plugin.class_id);
        put(c.name, info.plugin.name);
        put(c.vendor, info.plugin.vendor);
        write_out(out_info, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_insert_parameter(mixit_session session, uint32_t input_id,
                                                                            uint32_t slot, uint32_t index,
                                                                            mixit_audio_insert_parameter* out_parameter) {
    constexpr const char* fn = "mixit_session_get_audio_insert_parameter";
    if (!struct_size_ok(out_parameter)) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_parameter NULL or struct_size too small");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        mixit::media::Session::InsertParameter p;
        if (Status st = s->get_audio_insert_parameter(input_id, slot, index, p); !st.is_ok()) return report(fn, st);
        mixit_audio_insert_parameter c{};
        c.struct_size = sizeof(c);
        c.id = p.info.id;
        c.flags = (p.info.read_only ? MIXIT_VST3_PARAM_READ_ONLY : 0u) | (p.info.is_bypass ? MIXIT_VST3_PARAM_BYPASS : 0u);
        c.step_count = p.info.step_count;
        c.default_value = p.info.default_normalized;
        c.value = p.value;
        put(c.title, p.info.title);
        put(c.units, p.info.units);
        put(c.display, p.display);
        write_out(out_parameter, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_insert_parameter(mixit_session session, uint32_t input_id,
                                                                            uint32_t slot, uint32_t parameter_id,
                                                                            double value) {
    constexpr const char* fn = "mixit_session_set_audio_insert_parameter";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_audio_insert_parameter(input_id, slot, parameter_id, value));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_insert_state(mixit_session session, uint32_t input_id,
                                                                        uint32_t slot, uint8_t* buffer,
                                                                        uint32_t capacity, uint32_t* out_size) {
    constexpr const char* fn = "mixit_session_get_audio_insert_state";
    if (out_size == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_size is NULL");
    *out_size = 0;
    if (capacity > 0 && buffer == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "buffer is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        std::vector<std::uint8_t> state;
        if (Status st = s->get_audio_insert_state(input_id, slot, state); !st.is_ok()) return report(fn, st);
        if (state.size() > UINT32_MAX) return fail(fn, MIXIT_E_INTERNAL, "state larger than 4 GiB");
        *out_size = static_cast<uint32_t>(state.size());
        if (capacity == 0) return MIXIT_OK;
        if (capacity < state.size()) return fail(fn, MIXIT_E_BUFFER_TOO_SMALL, "buffer too small; see out_size");
        std::memcpy(buffer, state.data(), state.size());
        return MIXIT_OK;
    });
}

}  // extern "C"
