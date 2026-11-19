// Audio mixer channel strip and bus processing (M4).
//
// Strip signal flow (per input, session channel count, interleaved float):
//   trim → high-pass → gate/expander → EQ (4 bands) → compressor →
//   fader × mute × audio-follows-video gain (ramped per block) → balance
//   → post-fader meter → sends to master / aux buses (bus mask).
// Bus flow: sum → fader × mute (ramped) → limiter → meter (+ loudness).
//
// Parameters are plain values handed over with a TripleBuffer; processing
// never allocates, locks or blocks.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "audio/dsp.hpp"

namespace mixit::audio {

inline constexpr std::uint32_t kEqBands = 4;
inline constexpr std::uint32_t kAuxBuses = 4;
inline constexpr std::uint32_t kMixBuses = 1 + kAuxBuses;  // master + A..D
inline constexpr std::uint32_t kMonitorBus = kMixBuses;     // listen bus (solo / selected bus)
inline constexpr float kFaderOffDb = -96.0f;                // at or below = silence
inline constexpr float kMaxDelayMs = 1000.0f;

struct EqBand {
    bool enabled = false;
    FilterType type = FilterType::Peaking;
    float frequency = 1000.0f;
    float gain_db = 0.0f;
    float q = 0.707f;

    friend bool operator==(const EqBand&, const EqBand&) = default;
};

struct StripParams {
    float trim_db = 0.0f;        // -24..+24
    float fader_db = 0.0f;       // kFaderOffDb..+12
    float balance = 0.0f;        // -1 (left) .. +1 (right); stereo only
    bool mute = false;
    bool solo = false;           // affects the monitor bus only (AFL)
    bool audio_follows_video = true;
    std::uint32_t bus_mask = 1;  // bit 0 master, bits 1..4 aux A..D
    float delay_ms = 0.0f;       // 0..kMaxDelayMs, lip-sync compensation
    bool highpass = false;
    float highpass_hz = 80.0f;
    std::array<EqBand, kEqBands> eq{{
        {false, FilterType::LowShelf, 100.0f, 0.0f, 0.707f},
        {false, FilterType::Peaking, 500.0f, 0.0f, 1.0f},
        {false, FilterType::Peaking, 2500.0f, 0.0f, 1.0f},
        {false, FilterType::HighShelf, 8000.0f, 0.0f, 0.707f},
    }};
    GateSettings gate;
    CompressorSettings compressor;
};

struct BusParams {
    float fader_db = 0.0f;
    bool mute = false;
    LimiterSettings limiter;
};

// What the strip reports to meters (written by the audio thread).
struct StripMeters {
    MeterReading level;
    float gate_reduction_db = 0.0f;
    float compressor_reduction_db = 0.0f;
    bool gate_open = true;
    float applied_gain_db = kMinDb;  // fader × mute × AFV at the end of the block
};

struct BusMeters {
    MeterReading level;
    float limiter_reduction_db = 0.0f;
    float momentary_lufs = kMinDb;
    float short_term_lufs = kMinDb;
    float integrated_lufs = kMinDb;
};

class ChannelStrip {
public:
    ChannelStrip(float sample_rate, std::uint32_t channels, std::uint32_t max_frames);

    void reset() noexcept;

    // Processes `frames` of `io` in place up to and including balance;
    // `afv_gain` (0..1) is the audio-follows-video target for this block.
    // Returns the per-block linear gain ramp end value applied.
    void process(const StripParams& p, float afv_gain, float* io, std::uint32_t frames) noexcept;

    [[nodiscard]] const StripMeters& meters() const noexcept { return meters_; }

private:
    void update_filters(const StripParams& p) noexcept;

    float sample_rate_;
    std::uint32_t channels_;
    // Filters: coefficients are redesigned only when their parameters change.
    bool hp_valid_ = false;
    float hp_hz_ = 0.0f;
    BiquadCoefficients hp_coeff_;
    std::vector<BiquadState> hp_state_;
    std::array<EqBand, kEqBands> eq_designed_{};
    std::array<bool, kEqBands> eq_valid_{};
    std::array<BiquadCoefficients, kEqBands> eq_coeff_{};
    std::vector<BiquadState> eq_state_;  // kEqBands × channels
    Dynamics dynamics_;
    LevelMeter meter_;
    float last_gain_ = 0.0f;
    std::array<float, 2> last_balance_{1.0f, 1.0f};
    StripMeters meters_;
};

class BusProcessor {
public:
    BusProcessor(float sample_rate, std::uint32_t channels, bool loudness);
    void reset() noexcept;
    void process(const BusParams& p, float* io, std::uint32_t frames) noexcept;
    void reset_loudness() noexcept;
    [[nodiscard]] const BusMeters& meters() const noexcept { return meters_; }

private:
    std::uint32_t channels_;
    Dynamics dynamics_;
    LevelMeter meter_;
    bool has_loudness_;
    LoudnessMeter loudness_;
    float last_gain_ = 1.0f;
    BusMeters meters_;
};

// Balance law: centre = unity on both sides; turning towards one side
// attenuates the other with a cosine curve (-3 dB at ±0.5, silent at ±1).
std::array<float, 2> balance_gains(float balance) noexcept;

}  // namespace mixit::audio
