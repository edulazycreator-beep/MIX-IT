#include "audio/channel_strip.hpp"

#include <algorithm>
#include <numbers>

namespace mixit::audio {

std::array<float, 2> balance_gains(float balance) noexcept {
    const float b = std::clamp(balance, -1.0f, 1.0f);
    const float half_pi = std::numbers::pi_v<float> / 2.0f;
    return {b > 0.0f ? std::cos(b * half_pi) : 1.0f, b < 0.0f ? std::cos(-b * half_pi) : 1.0f};
}

namespace {

// Linear gain ramp across the block (avoids zipper noise on fader moves).
void apply_ramp(float* io, std::uint32_t frames, std::uint32_t channels, float from, float to) noexcept {
    if (from == to) {
        if (to == 1.0f) return;
        const std::size_t n = static_cast<std::size_t>(frames) * channels;
        for (std::size_t i = 0; i < n; ++i) io[i] *= to;
        return;
    }
    const float n = static_cast<float>(std::max<std::uint32_t>(frames, 1));
    for (std::uint32_t i = 0; i < frames; ++i, io += channels) {
        // Exactly `to` on the last frame (no accumulated rounding: mute is silent).
        const float g = i + 1 == frames ? to : from + (to - from) * static_cast<float>(i + 1) / n;
        for (std::uint32_t c = 0; c < channels; ++c) io[c] *= g;
    }
}

}  // namespace

ChannelStrip::ChannelStrip(float sample_rate, std::uint32_t channels, std::uint32_t /*max_frames*/)
    : sample_rate_(sample_rate),
      channels_(channels),
      hp_state_(channels),
      eq_state_(static_cast<std::size_t>(kEqBands) * channels) {
    dynamics_.prepare(sample_rate);
    meter_.prepare(sample_rate, channels);
}

void ChannelStrip::reset() noexcept {
    for (auto& s : hp_state_) s.reset();
    for (auto& s : eq_state_) s.reset();
    dynamics_.reset();
    meter_.reset();
    last_gain_ = 0.0f;
    last_balance_ = {1.0f, 1.0f};
    meters_ = StripMeters{};
    meters_.level = meter_.reading();
}

void ChannelStrip::update_filters(const StripParams& p) noexcept {
    if (p.highpass && (!hp_valid_ || hp_hz_ != p.highpass_hz)) {
        hp_coeff_ = BiquadCoefficients::design(FilterType::HighPass, sample_rate_, p.highpass_hz, 0.0f, 0.707f);
        hp_hz_ = p.highpass_hz;
        hp_valid_ = true;
    }
    for (std::uint32_t b = 0; b < kEqBands; ++b) {
        if (p.eq[b].enabled && (!eq_valid_[b] || !(eq_designed_[b] == p.eq[b]))) {
            const EqBand& e = p.eq[b];
            eq_coeff_[b] = BiquadCoefficients::design(e.type, sample_rate_, e.frequency, e.gain_db, e.q);
            eq_designed_[b] = e;
            eq_valid_[b] = true;
        }
    }
}

void ChannelStrip::process(const StripParams& p, float afv_gain, float* io, std::uint32_t frames) noexcept {
    update_filters(p);
    const std::size_t n = static_cast<std::size_t>(frames) * channels_;

    if (p.trim_db != 0.0f) {
        const float t = db_to_gain(p.trim_db);
        for (std::size_t i = 0; i < n; ++i) io[i] *= t;
    }
    if (p.highpass) {
        for (std::uint32_t c = 0; c < channels_; ++c) {
            BiquadState& s = hp_state_[c];
            for (std::uint32_t i = 0; i < frames; ++i) {
                float& v = io[static_cast<std::size_t>(i) * channels_ + c];
                v = s.process(hp_coeff_, v);
            }
        }
    }
    meters_.gate_reduction_db = dynamics_.process_gate(p.gate, io, frames, channels_);
    meters_.gate_open = dynamics_.gate_open();
    for (std::uint32_t b = 0; b < kEqBands; ++b) {
        if (!p.eq[b].enabled || (p.eq[b].gain_db == 0.0f && p.eq[b].type != FilterType::HighPass &&
                                     p.eq[b].type != FilterType::LowPass)) {
            continue;
        }
        for (std::uint32_t c = 0; c < channels_; ++c) {
            BiquadState& s = eq_state_[static_cast<std::size_t>(b) * channels_ + c];
            for (std::uint32_t i = 0; i < frames; ++i) {
                float& v = io[static_cast<std::size_t>(i) * channels_ + c];
                v = s.process(eq_coeff_[b], v);
            }
        }
    }
    meters_.compressor_reduction_db = dynamics_.process_compressor(p.compressor, io, frames, channels_);

    float gain = (p.mute || p.fader_db <= kFaderOffDb) ? 0.0f : db_to_gain(p.fader_db);
    if (p.audio_follows_video) gain *= std::clamp(afv_gain, 0.0f, 1.0f);
    apply_ramp(io, frames, channels_, last_gain_, gain);
    last_gain_ = gain;
    meters_.applied_gain_db = gain_to_db(gain);

    if (channels_ == 2) {
        const std::array<float, 2> bal = balance_gains(p.balance);
        if (bal != last_balance_ || bal[0] != 1.0f || bal[1] != 1.0f) {
            const float count = static_cast<float>(std::max<std::uint32_t>(frames, 1));
            for (std::uint32_t i = 0; i < frames; ++i) {
                const float t = static_cast<float>(i + 1) / count;
                const bool last = i + 1 == frames;
                io[2 * i] *= last ? bal[0] : last_balance_[0] + (bal[0] - last_balance_[0]) * t;
                io[2 * i + 1] *= last ? bal[1] : last_balance_[1] + (bal[1] - last_balance_[1]) * t;
            }
        }
        last_balance_ = bal;
    }
    meter_.process(io, frames);
    meters_.level = meter_.reading();
}

BusProcessor::BusProcessor(float sample_rate, std::uint32_t channels, bool loudness)
    : channels_(channels), has_loudness_(loudness) {
    dynamics_.prepare(sample_rate);
    meter_.prepare(sample_rate, channels);
    if (has_loudness_) loudness_.prepare(sample_rate, channels);
}

void BusProcessor::reset() noexcept {
    dynamics_.reset();
    meter_.reset();
    if (has_loudness_) loudness_.reset();
    last_gain_ = 1.0f;
    meters_ = BusMeters{};
    meters_.level = meter_.reading();
}

void BusProcessor::reset_loudness() noexcept {
    if (has_loudness_) loudness_.reset();
}

void BusProcessor::process(const BusParams& p, float* io, std::uint32_t frames) noexcept {
    const float gain = (p.mute || p.fader_db <= kFaderOffDb) ? 0.0f : db_to_gain(p.fader_db);
    apply_ramp(io, frames, channels_, last_gain_, gain);
    last_gain_ = gain;
    meters_.limiter_reduction_db = dynamics_.process_limiter(p.limiter, io, frames, channels_);
    meter_.process(io, frames);
    meters_.level = meter_.reading();
    if (has_loudness_) {
        loudness_.process(io, frames);
        meters_.momentary_lufs = loudness_.momentary_lufs();
        meters_.short_term_lufs = loudness_.short_term_lufs();
        meters_.integrated_lufs = loudness_.integrated_lufs();
    }
}

}  // namespace mixit::audio
