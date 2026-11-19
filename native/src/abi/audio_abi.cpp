// C ABI: audio mixer (API 0.4). Converts the flat C structs to the engine's
// parameter types and back; validation of ranges lives in the session.
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include "abi/abi_common.hpp"
#include "abi/session_handles.hpp"
#include "core/last_error.hpp"
#include "mixit/mixit_engine.h"

static_assert(sizeof(mixit_eq_band) == 20);
static_assert(sizeof(mixit_audio_strip) == 160);
static_assert(offsetof(mixit_audio_strip, eq) == 32);
static_assert(offsetof(mixit_audio_strip, gate_threshold_db) == 112);
static_assert(offsetof(mixit_audio_strip, comp_threshold_db) == 136);
static_assert(sizeof(mixit_audio_bus_config) == 24);
static_assert(sizeof(mixit_audio_meter) == 104);
static_assert(offsetof(mixit_audio_meter, peak_db) == 16);
static_assert(offsetof(mixit_audio_meter, gain_reduction_db) == 80);
static_assert(sizeof(mixit_audio_block_info) == 24);
static_assert(sizeof(mixit_audio_device_info) == 408);
static_assert(sizeof(mixit_audio_output_config) == 24);
static_assert(sizeof(mixit_audio_output_stats) == 176);
static_assert(offsetof(mixit_audio_output_stats, drift_ratio) == 40);
static_assert(sizeof(mixit_audio_capture_config) == 16);
static_assert(MIXIT_AUDIO_EQ_BANDS == mixit::audio::kEqBands);
static_assert(MIXIT_AUDIO_MIX_BUSES == mixit::audio::kMixBuses);
static_assert(MIXIT_AUDIO_MAX_METER_CHANNELS == mixit::audio::kMaxMeterChannels);

namespace {

using mixit::Status;
using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::struct_size_ok;
using mixit::abi::write_out;
namespace audio = mixit::audio;

constexpr const char* kInvalidSession = "invalid session handle";
constexpr std::uint32_t kKnownStripFlags = MIXIT_STRIP_MUTE | MIXIT_STRIP_SOLO | MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO |
                                           MIXIT_STRIP_HIGH_PASS | MIXIT_STRIP_GATE | MIXIT_STRIP_COMPRESSOR;

audio::StripParams to_params(const mixit_audio_strip& c) {
    audio::StripParams p;
    p.trim_db = c.trim_db;
    p.fader_db = c.fader_db;
    p.balance = c.balance;
    p.delay_ms = c.delay_ms;
    p.bus_mask = c.bus_mask;
    p.mute = (c.flags & MIXIT_STRIP_MUTE) != 0;
    p.solo = (c.flags & MIXIT_STRIP_SOLO) != 0;
    p.audio_follows_video = (c.flags & MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO) != 0;
    p.highpass = (c.flags & MIXIT_STRIP_HIGH_PASS) != 0;
    p.highpass_hz = c.high_pass_hz;
    for (std::uint32_t b = 0; b < audio::kEqBands; ++b) {
        p.eq[b].enabled = c.eq[b].enabled != 0;
        p.eq[b].type = static_cast<audio::FilterType>(c.eq[b].type);
        p.eq[b].frequency = c.eq[b].frequency_hz;
        p.eq[b].gain_db = c.eq[b].gain_db;
        p.eq[b].q = c.eq[b].q;
    }
    p.gate.enabled = (c.flags & MIXIT_STRIP_GATE) != 0;
    p.gate.threshold_db = c.gate_threshold_db;
    p.gate.ratio = c.gate_ratio;
    p.gate.range_db = c.gate_range_db;
    p.gate.attack_ms = c.gate_attack_ms;
    p.gate.hold_ms = c.gate_hold_ms;
    p.gate.release_ms = c.gate_release_ms;
    p.compressor.enabled = (c.flags & MIXIT_STRIP_COMPRESSOR) != 0;
    p.compressor.threshold_db = c.comp_threshold_db;
    p.compressor.ratio = c.comp_ratio;
    p.compressor.knee_db = c.comp_knee_db;
    p.compressor.attack_ms = c.comp_attack_ms;
    p.compressor.release_ms = c.comp_release_ms;
    p.compressor.makeup_db = c.comp_makeup_db;
    return p;
}

mixit_audio_strip to_c(const audio::StripParams& p) {
    mixit_audio_strip c{};
    c.struct_size = sizeof(c);
    c.flags = (p.mute ? MIXIT_STRIP_MUTE : 0u) | (p.solo ? MIXIT_STRIP_SOLO : 0u) |
              (p.audio_follows_video ? MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO : 0u) | (p.highpass ? MIXIT_STRIP_HIGH_PASS : 0u) |
              (p.gate.enabled ? MIXIT_STRIP_GATE : 0u) | (p.compressor.enabled ? MIXIT_STRIP_COMPRESSOR : 0u);
    c.trim_db = p.trim_db;
    c.fader_db = p.fader_db;
    c.balance = p.balance;
    c.delay_ms = p.delay_ms;
    c.bus_mask = p.bus_mask;
    c.high_pass_hz = p.highpass_hz;
    for (std::uint32_t b = 0; b < audio::kEqBands; ++b) {
        c.eq[b].enabled = p.eq[b].enabled ? 1 : 0;
        c.eq[b].type = static_cast<int32_t>(p.eq[b].type);
        c.eq[b].frequency_hz = p.eq[b].frequency;
        c.eq[b].gain_db = p.eq[b].gain_db;
        c.eq[b].q = p.eq[b].q;
    }
    c.gate_threshold_db = p.gate.threshold_db;
    c.gate_ratio = p.gate.ratio;
    c.gate_range_db = p.gate.range_db;
    c.gate_attack_ms = p.gate.attack_ms;
    c.gate_hold_ms = p.gate.hold_ms;
    c.gate_release_ms = p.gate.release_ms;
    c.comp_threshold_db = p.compressor.threshold_db;
    c.comp_ratio = p.compressor.ratio;
    c.comp_knee_db = p.compressor.knee_db;
    c.comp_attack_ms = p.compressor.attack_ms;
    c.comp_release_ms = p.compressor.release_ms;
    c.comp_makeup_db = p.compressor.makeup_db;
    return c;
}

void fill_levels(mixit_audio_meter& m, const audio::MeterReading& r, std::uint32_t channels) {
    m.channels = std::min(channels, audio::kMaxMeterChannels);
    for (std::uint32_t c = 0; c < audio::kMaxMeterChannels; ++c) {
        m.peak_db[c] = r.peak_db[c];
        m.rms_db[c] = r.rms_db[c];
    }
}

}  // namespace

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_strip(mixit_session session, uint32_t input_id,
                                                                 const mixit_audio_strip* strip) {
    constexpr const char* fn = "mixit_session_set_audio_strip";
    if (!struct_size_ok(strip)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "strip NULL or struct_size too small");
    if ((strip->flags & ~kKnownStripFlags) != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown strip flags");
    for (const mixit_eq_band& b : strip->eq) {
        if (b.enabled != 0 && b.enabled != 1) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "eq.enabled must be 0 or 1");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_audio_strip(input_id, to_params(*strip)));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_strip(mixit_session session, uint32_t input_id,
                                                                 mixit_audio_strip* out_strip) {
    constexpr const char* fn = "mixit_session_get_audio_strip";
    if (!struct_size_ok(out_strip)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_strip NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        audio::StripParams p;
        if (Status st = s->get_audio_strip(input_id, p); !st.is_ok()) return report(fn, st);
        write_out(out_strip, to_c(p));
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_bus(mixit_session session, uint32_t bus,
                                                               const mixit_audio_bus_config* config) {
    constexpr const char* fn = "mixit_session_set_audio_bus";
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if ((config->flags & ~(MIXIT_AUDIO_BUS_FLAG_MUTE | MIXIT_AUDIO_BUS_FLAG_LIMITER)) != 0 || config->reserved != 0) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown flags or reserved != 0");
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        audio::BusParams p;
        p.fader_db = config->fader_db;
        p.mute = (config->flags & MIXIT_AUDIO_BUS_FLAG_MUTE) != 0;
        p.limiter.enabled = (config->flags & MIXIT_AUDIO_BUS_FLAG_LIMITER) != 0;
        p.limiter.ceiling_db = config->limiter_ceiling_db;
        p.limiter.release_ms = config->limiter_release_ms;
        return report(fn, s->set_audio_bus(bus, p));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_bus(mixit_session session, uint32_t bus,
                                                               mixit_audio_bus_config* out_config) {
    constexpr const char* fn = "mixit_session_get_audio_bus";
    if (!struct_size_ok(out_config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_config NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        audio::BusParams p;
        if (Status st = s->get_audio_bus(bus, p); !st.is_ok()) return report(fn, st);
        mixit_audio_bus_config c{};
        c.struct_size = sizeof(c);
        c.flags = (p.mute ? MIXIT_AUDIO_BUS_FLAG_MUTE : 0u) | (p.limiter.enabled ? MIXIT_AUDIO_BUS_FLAG_LIMITER : 0u);
        c.fader_db = p.fader_db;
        c.limiter_ceiling_db = p.limiter.ceiling_db;
        c.limiter_release_ms = p.limiter.release_ms;
        write_out(out_config, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_monitor_source(mixit_session session, uint32_t bus) {
    constexpr const char* fn = "mixit_session_set_monitor_source";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->set_monitor_source(bus));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_input_meter(mixit_session session, uint32_t input_id,
                                                                 mixit_audio_meter* out_meter) {
    constexpr const char* fn = "mixit_session_get_input_meter";
    if (!struct_size_ok(out_meter)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_meter NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        audio::StripMeters sm;
        if (Status st = s->input_meters(input_id, sm); !st.is_ok()) return report(fn, st);
        mixit_audio_meter m{};
        m.struct_size = sizeof(m);
        m.id = input_id;
        fill_levels(m, sm.level, s->config().channels);
        m.flags = sm.gate_open ? 1u : 0u;
        m.gain_reduction_db = sm.compressor_reduction_db;
        m.gate_reduction_db = sm.gate_reduction_db;
        m.applied_gain_db = sm.applied_gain_db;
        m.momentary_lufs = m.short_term_lufs = m.integrated_lufs = audio::kMinDb;
        write_out(out_meter, m);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_bus_meter(mixit_session session, uint32_t bus,
                                                               mixit_audio_meter* out_meter) {
    constexpr const char* fn = "mixit_session_get_bus_meter";
    if (!struct_size_ok(out_meter)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_meter NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        audio::BusMeters bm;
        if (Status st = s->bus_meters(bus, bm); !st.is_ok()) return report(fn, st);
        mixit_audio_meter m{};
        m.struct_size = sizeof(m);
        m.id = bus;
        fill_levels(m, bm.level, s->config().channels);
        m.gain_reduction_db = bm.limiter_reduction_db;
        m.applied_gain_db = 0.0f;
        m.momentary_lufs = bm.momentary_lufs;
        m.short_term_lufs = bm.short_term_lufs;
        m.integrated_lufs = bm.integrated_lufs;
        write_out(out_meter, m);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_reset_loudness(mixit_session session, uint32_t bus) {
    constexpr const char* fn = "mixit_session_reset_loudness";
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        return report(fn, s->reset_loudness(bus));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_read_bus_audio(mixit_session session, uint32_t bus, float* dest,
                                                                uint32_t capacity_frames,
                                                                mixit_audio_block_info* out_info) {
    constexpr const char* fn = "mixit_session_read_bus_audio";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        mixit::media::AudioBusFrameInfo info;
        const Status st = s->read_bus_audio(bus, dest, capacity_frames, info);
        mixit_audio_block_info o{};
        o.struct_size = sizeof(o);
        o.frames = info.frames;
        o.pts_ns = info.pts_ns;
        o.first_sample = info.first_sample;
        write_out(out_info, o);
        return st.is_ok() ? MIXIT_OK : report(fn, st);
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_enumerate_audio_devices(mixit_engine engine, int32_t direction,
                                                                 mixit_audio_device_info* devices, uint32_t capacity,
                                                                 uint32_t* out_count) {
    constexpr const char* fn = "mixit_enumerate_audio_devices";
    if (out_count == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_count is NULL");
    *out_count = 0;
    if (direction != MIXIT_AUDIO_DEVICE_RENDER && direction != MIXIT_AUDIO_DEVICE_CAPTURE) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "direction must be RENDER or CAPTURE");
    }
    if (capacity > 0 && devices == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "devices is NULL");
    for (uint32_t i = 0; i < capacity; ++i) {
        if (devices[i].struct_size < sizeof(mixit_audio_device_info)) {
            return fail(fn, MIXIT_E_INVALID_ARGUMENT, "devices[i].struct_size too small");
        }
    }
    return guarded(fn, [&]() -> mixit_result {
        const auto e = mixit::abi::resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        std::vector<audio::AudioDeviceInfo> found;
        const auto dir = direction == MIXIT_AUDIO_DEVICE_RENDER ? audio::DeviceDirection::Render
                                                                : audio::DeviceDirection::Capture;
        if (Status st = audio::enumerate_audio_devices(dir, e->logger(), found); !st.is_ok()) return report(fn, st);
        uint32_t written = 0;
        for (const audio::AudioDeviceInfo& d : found) {
            if (d.id.size() >= sizeof(mixit_audio_device_info::id)) continue;  // never hand out a truncated id
            if (written < capacity) {
                mixit_audio_device_info info{};
                info.struct_size = sizeof(info);
                info.direction = direction;
                info.flags = d.is_default ? MIXIT_AUDIO_DEVICE_FLAG_DEFAULT : 0u;
                info.channels = d.channels;
                info.sample_rate = d.sample_rate;
                mixit::copy_utf8_truncated(info.name, sizeof(info.name), d.name);
                std::memcpy(info.id, d.id.c_str(), d.id.size() + 1);
                devices[written] = info;
            }
            ++written;
        }
        *out_count = written;
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_output(mixit_session session,
                                                                  const mixit_audio_output_config* config) {
    constexpr const char* fn = "mixit_session_set_audio_output";
    if (config != nullptr && !struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "struct_size too small");
    if (config != nullptr && config->reserved != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "reserved must be 0");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        if (config == nullptr) {
            s->disable_audio_output();
            return MIXIT_OK;
        }
        const std::string id = config->device_id == nullptr ? std::string() : std::string(config->device_id);
        return report(fn, s->set_audio_output(config->bus, id, config->latency_ms));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_output_stats(mixit_session session,
                                                                        mixit_audio_output_stats* out_stats) {
    constexpr const char* fn = "mixit_session_get_audio_output_stats";
    if (!struct_size_ok(out_stats)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_stats NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        const auto info = s->audio_output_info();
        mixit_audio_output_stats o{};
        o.struct_size = sizeof(o);
        o.state = info.enabled ? static_cast<int32_t>(info.device.state) : MIXIT_AUDIO_OUTPUT_OFF;
        o.bus = info.bus;
        o.buffered_ms = static_cast<uint32_t>(static_cast<std::uint64_t>(info.device.buffered_frames) * 1000 /
                                              s->config().sample_rate);
        o.underruns = info.device.underruns;
        o.device_errors = info.device.device_errors;
        o.dropped_frames = info.dropped_frames;
        o.drift_ratio = info.device.ratio;
        mixit::copy_utf8_truncated(o.device_name, sizeof(o.device_name), info.device.device_name);
        write_out(out_stats, o);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_session_add_audio_capture_source(mixit_session session,
                                                                          const mixit_audio_capture_config* config,
                                                                          uint32_t* out_input_id) {
    constexpr const char* fn = "mixit_session_add_audio_capture_source";
    if (out_input_id == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_input_id is NULL");
    *out_input_id = 0;
    if (!struct_size_ok(config)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config NULL or struct_size too small");
    if (config->flags != 0) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "unknown flags");
    return guarded(fn, [&]() -> mixit_result {
        const auto s = mixit::abi::resolve_session(session);
        if (!s) return fail(fn, MIXIT_E_INVALID_HANDLE, kInvalidSession);
        const std::string id = config->device_id == nullptr ? std::string() : std::string(config->device_id);
        return report(fn, s->add_audio_capture_source(id, *out_input_id));
    });
}
