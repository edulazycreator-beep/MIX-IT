#include "audio/dsp.hpp"

#include <algorithm>
#include <numbers>

namespace mixit::audio {

float time_coefficient(float ms, float sample_rate) noexcept {
    if (ms <= 0.0f) return 0.0f;
    return std::exp(-1.0f / (ms * 0.001f * sample_rate));
}

// ---- Biquad -------------------------------------------------------------------

BiquadCoefficients BiquadCoefficients::design(FilterType type, float sample_rate, float frequency, float gain_db,
                                              float q) noexcept {
    const double fs = sample_rate;
    const double f = std::clamp(static_cast<double>(frequency), 10.0, fs * 0.49);
    const double qq = std::max(0.05, static_cast<double>(q));
    const double w0 = 2.0 * std::numbers::pi * f / fs;
    const double cw = std::cos(w0), sw = std::sin(w0);
    const double alpha = sw / (2.0 * qq);
    const double A = std::pow(10.0, gain_db / 40.0);
    double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
    switch (type) {
        case FilterType::Peaking:
            b0 = 1 + alpha * A;
            b1 = -2 * cw;
            b2 = 1 - alpha * A;
            a0 = 1 + alpha / A;
            a1 = -2 * cw;
            a2 = 1 - alpha / A;
            break;
        case FilterType::LowShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) - (A - 1) * cw + s);
            b1 = 2 * A * ((A - 1) - (A + 1) * cw);
            b2 = A * ((A + 1) - (A - 1) * cw - s);
            a0 = (A + 1) + (A - 1) * cw + s;
            a1 = -2 * ((A - 1) + (A + 1) * cw);
            a2 = (A + 1) + (A - 1) * cw - s;
            break;
        }
        case FilterType::HighShelf: {
            const double s = 2 * std::sqrt(A) * alpha;
            b0 = A * ((A + 1) + (A - 1) * cw + s);
            b1 = -2 * A * ((A - 1) + (A + 1) * cw);
            b2 = A * ((A + 1) + (A - 1) * cw - s);
            a0 = (A + 1) - (A - 1) * cw + s;
            a1 = 2 * ((A - 1) - (A + 1) * cw);
            a2 = (A + 1) - (A - 1) * cw - s;
            break;
        }
        case FilterType::HighPass:
            b0 = (1 + cw) / 2;
            b1 = -(1 + cw);
            b2 = (1 + cw) / 2;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
        case FilterType::LowPass:
            b0 = (1 - cw) / 2;
            b1 = 1 - cw;
            b2 = (1 - cw) / 2;
            a0 = 1 + alpha;
            a1 = -2 * cw;
            a2 = 1 - alpha;
            break;
    }
    BiquadCoefficients c;
    c.b0 = static_cast<float>(b0 / a0);
    c.b1 = static_cast<float>(b1 / a0);
    c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0);
    c.a2 = static_cast<float>(a2 / a0);
    return c;
}

float BiquadCoefficients::magnitude_db(float frequency, float sample_rate) const noexcept {
    const double w = 2.0 * std::numbers::pi * frequency / sample_rate;
    const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
    const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    const double mag2 = (nr * nr + ni * ni) / (dr * dr + di * di);
    return static_cast<float>(10.0 * std::log10(std::max(mag2, 1e-30)));
}

// ---- Dynamics -----------------------------------------------------------------

float compressor_gain_db(const CompressorSettings& s, float level_db) noexcept {
    const float ratio = std::max(1.0f, s.ratio);
    const float over = level_db - s.threshold_db;
    const float knee = std::max(0.0f, s.knee_db);
    float out_over;
    if (knee > 0.0f && 2.0f * over > -knee && 2.0f * over < knee) {
        const float x = over + knee / 2.0f;
        out_over = over + (1.0f / ratio - 1.0f) * x * x / (2.0f * knee);
    } else if (over <= 0.0f) {
        out_over = over;
    } else {
        out_over = over / ratio;
    }
    return out_over - over;  // <= 0
}

void Dynamics::reset() noexcept {
    comp_env_db_ = 0.0f;
    gate_gain_db_ = 0.0f;
    gate_hold_ = 0.0f;
    gate_open_ = true;
    limit_gain_ = 1.0f;
}

namespace {

float frame_peak(const float* f, std::uint32_t channels) noexcept {
    float p = 0.0f;
    for (std::uint32_t c = 0; c < channels; ++c) p = std::max(p, std::abs(f[c]));
    return p;
}

}  // namespace

float Dynamics::process_compressor(const CompressorSettings& s, float* x, std::uint32_t frames,
                                   std::uint32_t channels) noexcept {
    if (!s.enabled) {
        comp_env_db_ = 0.0f;
        return 0.0f;
    }
    const float att = time_coefficient(s.attack_ms, sample_rate_);
    const float rel = time_coefficient(s.release_ms, sample_rate_);
    const float makeup = db_to_gain(s.makeup_db);
    float deepest = 0.0f;
    for (std::uint32_t i = 0; i < frames; ++i, x += channels) {
        const float target = compressor_gain_db(s, gain_to_db(frame_peak(x, channels)));
        const float coeff = target < comp_env_db_ ? att : rel;  // falling gain = attack
        comp_env_db_ = target + coeff * (comp_env_db_ - target);
        const float g = db_to_gain(comp_env_db_) * makeup;
        for (std::uint32_t c = 0; c < channels; ++c) x[c] *= g;
        deepest = std::min(deepest, comp_env_db_);
    }
    return deepest;
}

float Dynamics::process_gate(const GateSettings& s, float* x, std::uint32_t frames, std::uint32_t channels) noexcept {
    if (!s.enabled) {
        gate_gain_db_ = 0.0f;
        gate_open_ = true;
        return 0.0f;
    }
    const float att = time_coefficient(s.attack_ms, sample_rate_);
    const float rel = time_coefficient(s.release_ms, sample_rate_);
    const float hold_samples = s.hold_ms * 0.001f * sample_rate_;
    const float range = std::min(0.0f, s.range_db);
    float deepest = 0.0f;
    for (std::uint32_t i = 0; i < frames; ++i, x += channels) {
        const float level = gain_to_db(frame_peak(x, channels));
        float target;
        if (level >= s.threshold_db) {
            target = 0.0f;
            gate_hold_ = hold_samples;
        } else if (gate_hold_ > 0.0f) {
            target = 0.0f;
            gate_hold_ -= 1.0f;
        } else if (s.ratio > 1.0f) {  // downward expander
            target = std::max(range, (level - s.threshold_db) * (s.ratio - 1.0f));
        } else {
            target = range;
        }
        const float coeff = target > gate_gain_db_ ? att : rel;  // opening = attack
        gate_gain_db_ = target + coeff * (gate_gain_db_ - target);
        const float g = db_to_gain(gate_gain_db_);
        for (std::uint32_t c = 0; c < channels; ++c) x[c] *= g;
        deepest = std::min(deepest, gate_gain_db_);
    }
    gate_open_ = gate_gain_db_ > -3.0f;
    return deepest;
}

float Dynamics::process_limiter(const LimiterSettings& s, float* x, std::uint32_t frames,
                                std::uint32_t channels) noexcept {
    if (!s.enabled) {
        limit_gain_ = 1.0f;
        return 0.0f;
    }
    const float ceiling = db_to_gain(std::min(0.0f, s.ceiling_db));
    const float rel = time_coefficient(s.release_ms, sample_rate_);
    float lowest = 1.0f;
    for (std::uint32_t i = 0; i < frames; ++i, x += channels) {
        const float peak = frame_peak(x, channels);
        const float needed = peak > ceiling ? ceiling / peak : 1.0f;
        // Instant attack, release towards unity.
        limit_gain_ = needed < limit_gain_ ? needed : std::min(needed, 1.0f + rel * (limit_gain_ - 1.0f));
        for (std::uint32_t c = 0; c < channels; ++c) x[c] *= limit_gain_;
        lowest = std::min(lowest, limit_gain_);
    }
    return gain_to_db(lowest);
}

// ---- Metering -----------------------------------------------------------------

void LevelMeter::prepare(float sample_rate, std::uint32_t channels) noexcept {
    sample_rate_ = sample_rate;
    channels_ = std::min(channels, kMaxMeterChannels);
    rms_coeff_ = time_coefficient(300.0f / 2.2f, sample_rate);  // 300 ms integration (10–90 %)
    fall_per_sample_db_ = 20.0f / (1.5f * sample_rate);
    reset();
}

void LevelMeter::reset() noexcept {
    peak_.fill(kMinDb);
    mean_square_.fill(0.0f);
}

void LevelMeter::process(const float* x, std::uint32_t frames) noexcept {
    const std::uint32_t stride = channels_;
    for (std::uint32_t c = 0; c < channels_; ++c) {
        float peak = 0.0f;
        float ms = mean_square_[c];
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float v = x[static_cast<std::size_t>(i) * stride + c];
            peak = std::max(peak, std::abs(v));
            ms = v * v + rms_coeff_ * (ms - v * v);
        }
        mean_square_[c] = ms;
        const float fallen = peak_[c] - fall_per_sample_db_ * static_cast<float>(frames);
        peak_[c] = std::max(gain_to_db(peak), std::max(kMinDb, fallen));
    }
}

MeterReading LevelMeter::reading() const noexcept {
    MeterReading r;
    r.peak_db.fill(kMinDb);
    r.rms_db.fill(kMinDb);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        r.peak_db[c] = peak_[c];
        r.rms_db[c] = gain_to_db(std::sqrt(std::max(0.0f, mean_square_[c])));
    }
    return r;
}

void LoudnessMeter::prepare(float sample_rate, std::uint32_t channels) {
    sample_rate_ = sample_rate;
    channels_ = channels;
    // K-weighting per BS.1770: stage 1 high shelf (+4 dB above ~1.7 kHz) and
    // stage 2 RLB high-pass (~38 Hz), from their analogue prototypes via the
    // bilinear transform so that any sample rate matches the 48 kHz reference
    // coefficients of the standard.
    {
        const double f0 = 1681.974450955533, gain_db = 3.999843853973347, q = 0.7071752369554196;
        const double k = std::tan(std::numbers::pi * f0 / sample_rate);
        const double vh = std::pow(10.0, gain_db / 20.0);
        const double vb = std::pow(vh, 0.4996667741545416);
        const double a0 = 1.0 + k / q + k * k;
        shelf_.b0 = static_cast<float>((vh + vb * k / q + k * k) / a0);
        shelf_.b1 = static_cast<float>(2.0 * (k * k - vh) / a0);
        shelf_.b2 = static_cast<float>((vh - vb * k / q + k * k) / a0);
        shelf_.a1 = static_cast<float>(2.0 * (k * k - 1.0) / a0);
        shelf_.a2 = static_cast<float>((1.0 - k / q + k * k) / a0);
    }
    {
        const double f0 = 38.13547087602444, q = 0.5003270373238773;
        const double k = std::tan(std::numbers::pi * f0 / sample_rate);
        const double a0 = 1.0 + k / q + k * k;
        highpass_.b0 = 1.0f;
        highpass_.b1 = -2.0f;
        highpass_.b2 = 1.0f;
        highpass_.a1 = static_cast<float>(2.0 * (k * k - 1.0) / a0);
        highpass_.a2 = static_cast<float>((1.0 - k / q + k * k) / a0);
    }
    shelf_state_.assign(channels, BiquadState{});
    hp_state_.assign(channels, BiquadState{});
    subblock_frames_ = static_cast<std::uint32_t>(std::lround(sample_rate * 0.1f));
    histogram_.assign(kBins, 0);
    histogram_energy_.assign(kBins, 0.0);
    reset();
}

void LoudnessMeter::reset() noexcept {
    for (auto& s : shelf_state_) s.reset();
    for (auto& s : hp_state_) s.reset();
    subblock_pos_ = 0;
    subblock_energy_ = 0.0;
    history_.fill(0.0);
    history_count_ = 0;
    history_pos_ = 0;
    momentary_ = short_term_ = kMinDb;
    std::fill(histogram_.begin(), histogram_.end(), 0u);
    std::fill(histogram_energy_.begin(), histogram_energy_.end(), 0.0);
}

namespace {

float energy_to_lufs(double mean_square) noexcept {
    return mean_square <= 1e-20 ? kMinDb : static_cast<float>(-0.691 + 10.0 * std::log10(mean_square));
}

}  // namespace

void LoudnessMeter::process(const float* x, std::uint32_t frames) noexcept {
    for (std::uint32_t i = 0; i < frames; ++i) {
        double sum = 0.0;
        // Channel weights: 1.0 for the first three channels (L, R, C); surround
        // weights (1.41) apply to channels 4/5 in a 5.1 layout (ch 3 = LFE excluded).
        for (std::uint32_t c = 0; c < channels_; ++c) {
            if (channels_ == 6 && c == 3) continue;
            const float k = hp_state_[c].process(highpass_, shelf_state_[c].process(shelf_, x[c]));
            const double w = (channels_ == 6 && c >= 4) ? 1.41 : 1.0;
            sum += w * static_cast<double>(k) * k;
        }
        subblock_energy_ += sum;
        x += channels_;
        if (++subblock_pos_ == subblock_frames_) finish_subblock();
    }
}

void LoudnessMeter::finish_subblock() noexcept {
    history_[history_pos_] = subblock_energy_ / static_cast<double>(subblock_frames_);
    history_pos_ = (history_pos_ + 1) % static_cast<std::uint32_t>(history_.size());
    history_count_ = std::min<std::uint32_t>(history_count_ + 1, static_cast<std::uint32_t>(history_.size()));
    subblock_pos_ = 0;
    subblock_energy_ = 0.0;

    auto mean_of_last = [&](std::uint32_t n) {
        n = std::min(n, history_count_);
        double e = 0.0;
        for (std::uint32_t k = 1; k <= n; ++k) e += history_[(history_pos_ + history_.size() - k) % history_.size()];
        return n == 0 ? 0.0 : e / static_cast<double>(n);
    };
    if (history_count_ >= 4) {
        const double block = mean_of_last(4);  // 400 ms gating block, 75 % overlap
        momentary_ = energy_to_lufs(block);
        if (momentary_ >= -70.0f) {  // absolute gate
            const int bin = std::clamp(static_cast<int>(std::lround((momentary_ + 70.0f) * 10.0f)), 0, kBins - 1);
            ++histogram_[static_cast<std::size_t>(bin)];
            histogram_energy_[static_cast<std::size_t>(bin)] += block;
        }
    }
    if (history_count_ >= 30) short_term_ = energy_to_lufs(mean_of_last(30));
}

float LoudnessMeter::integrated_lufs() const noexcept {
    double energy = 0.0;
    std::uint64_t count = 0;
    for (int b = 0; b < kBins; ++b) {
        energy += histogram_energy_[static_cast<std::size_t>(b)];
        count += histogram_[static_cast<std::size_t>(b)];
    }
    if (count == 0) return kMinDb;
    const float relative_gate = energy_to_lufs(energy / static_cast<double>(count)) - 10.0f;
    energy = 0.0;
    count = 0;
    for (int b = 0; b < kBins; ++b) {
        const float lufs = -70.0f + static_cast<float>(b) * 0.1f;
        if (lufs < relative_gate) continue;
        energy += histogram_energy_[static_cast<std::size_t>(b)];
        count += histogram_[static_cast<std::size_t>(b)];
    }
    return count == 0 ? kMinDb : energy_to_lufs(energy / static_cast<double>(count));
}

}  // namespace mixit::audio
