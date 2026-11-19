// Audio DSP building blocks (M4). All processors are allocation-free after
// construction/prepare() and safe to run on the real-time audio thread.
//
// Conventions: float samples, 0 dBFS = 1.0; levels in dB are dBFS; time
// constants in milliseconds. Processors are per-channel unless stated.
#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mixit::audio {

inline constexpr float kMinDb = -144.0f;

inline float db_to_gain(float db) noexcept { return db <= kMinDb ? 0.0f : std::pow(10.0f, db / 20.0f); }
inline float gain_to_db(float g) noexcept { return g <= 1e-7f ? kMinDb : 20.0f * std::log10(g); }

// One-pole smoothing coefficient for a time constant (63 % in `ms`).
float time_coefficient(float ms, float sample_rate) noexcept;

// ---- Biquad (RBJ Audio EQ Cookbook), transposed direct form II ----------

enum class FilterType : std::int32_t {
    Peaking = 0,
    LowShelf = 1,
    HighShelf = 2,
    HighPass = 3,  // 12 dB/oct
    LowPass = 4,   // 12 dB/oct
};

struct BiquadCoefficients {
    float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;  // a0 normalized to 1
    static BiquadCoefficients design(FilterType type, float sample_rate, float frequency, float gain_db, float q) noexcept;
    // Magnitude response in dB at `frequency` (for tests and UI curves).
    [[nodiscard]] float magnitude_db(float frequency, float sample_rate) const noexcept;
};

struct BiquadState {
    float z1 = 0, z2 = 0;
    float process(const BiquadCoefficients& c, float x) noexcept {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = 0; }
};

// ---- Dynamics ---------------------------------------------------------------

struct CompressorSettings {
    bool enabled = false;
    float threshold_db = -18.0f;
    float ratio = 3.0f;        // >= 1
    float knee_db = 6.0f;      // soft knee width
    float attack_ms = 10.0f;
    float release_ms = 150.0f;
    float makeup_db = 0.0f;
};

// Static gain curve of the compressor (dB of gain change for an input level).
float compressor_gain_db(const CompressorSettings& s, float level_db) noexcept;

struct GateSettings {
    bool enabled = false;
    float threshold_db = -50.0f;
    float ratio = 0.0f;        // 0 = gate (attenuate by range); > 1 = downward expander ratio
    float range_db = -60.0f;   // maximum attenuation (<= 0)
    float attack_ms = 1.0f;
    float hold_ms = 50.0f;
    float release_ms = 200.0f;
};

struct LimiterSettings {
    bool enabled = false;
    float ceiling_db = -1.0f;
    float release_ms = 100.0f;
};

// Linked (all channels share one gain) feed-forward dynamics for one block of
// interleaved samples. Detection is peak-based across channels.
class Dynamics {
public:
    void prepare(float sample_rate) noexcept { sample_rate_ = sample_rate; }
    void reset() noexcept;
    // Processes in place; returns the deepest gain reduction in the block (dB, <= 0).
    float process_compressor(const CompressorSettings& s, float* interleaved, std::uint32_t frames,
                             std::uint32_t channels) noexcept;
    float process_gate(const GateSettings& s, float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept;
    // Instant attack (no overshoot above the ceiling), exponential release.
    float process_limiter(const LimiterSettings& s, float* interleaved, std::uint32_t frames,
                          std::uint32_t channels) noexcept;
    [[nodiscard]] bool gate_open() const noexcept { return gate_open_; }

private:
    float sample_rate_ = 48000.0f;
    float comp_env_db_ = 0.0f;   // smoothed gain (dB)
    float gate_gain_db_ = 0.0f;
    float gate_hold_ = 0.0f;     // remaining hold samples
    bool gate_open_ = true;
    float limit_gain_ = 1.0f;
};

// ---- Metering -----------------------------------------------------------------

inline constexpr std::uint32_t kMaxMeterChannels = 8;

struct MeterReading {
    std::array<float, kMaxMeterChannels> peak_db{};  // ballistic peak (instant rise, 20 dB/1.5 s fall)
    std::array<float, kMaxMeterChannels> rms_db{};   // 300 ms RMS
};

class LevelMeter {
public:
    void prepare(float sample_rate, std::uint32_t channels) noexcept;
    void reset() noexcept;
    void process(const float* interleaved, std::uint32_t frames) noexcept;
    [[nodiscard]] MeterReading reading() const noexcept;

private:
    float sample_rate_ = 48000.0f;
    std::uint32_t channels_ = 2;
    std::array<float, kMaxMeterChannels> peak_{};
    std::array<float, kMaxMeterChannels> mean_square_{};
    float rms_coeff_ = 0.0f;
    float fall_per_sample_db_ = 0.0f;
};

// ITU-R BS.1770-4 / EBU R128 loudness: K-weighting, momentary (400 ms),
// short-term (3 s) and gated integrated loudness. Fixed memory: integrated
// loudness uses a histogram of 400 ms block loudness (0.1 LU bins).
class LoudnessMeter {
public:
    void prepare(float sample_rate, std::uint32_t channels);  // allocates (not real-time)
    void reset() noexcept;
    void process(const float* interleaved, std::uint32_t frames) noexcept;
    [[nodiscard]] float momentary_lufs() const noexcept { return momentary_; }
    [[nodiscard]] float short_term_lufs() const noexcept { return short_term_; }
    [[nodiscard]] float integrated_lufs() const noexcept;

private:
    void finish_subblock() noexcept;

    float sample_rate_ = 48000.0f;
    std::uint32_t channels_ = 2;
    BiquadCoefficients shelf_, highpass_;
    std::vector<BiquadState> shelf_state_, hp_state_;
    // 100 ms sub-blocks: mean square sum over channels (weights 1.0 for L/R/C).
    std::uint32_t subblock_frames_ = 4800;
    std::uint32_t subblock_pos_ = 0;
    double subblock_energy_ = 0.0;
    std::array<double, 30> history_{};  // last 30 sub-blocks (3 s)
    std::uint32_t history_count_ = 0;
    std::uint32_t history_pos_ = 0;
    float momentary_ = kMinDb;
    float short_term_ = kMinDb;
    // Gated integration: histogram of block loudness from -70 to +5 LUFS.
    static constexpr int kBins = 751;
    std::vector<std::uint32_t> histogram_;
    std::vector<double> histogram_energy_;
};

}  // namespace mixit::audio
