// Clock-domain bridge between the session's master clock and an audio
// device's own clock (M4).
//
// A producer writes interleaved frames into a lock-free single-producer /
// single-consumer ring (SampleRing); the consumer pulls frames through a
// DriftResampler, which plays the ring at a ratio steered by a PI controller
// so the ring's fill level stays at its target. Two clocks that differ by a
// few hundred ppm therefore neither underrun nor overflow, and the pitch
// change is inaudible (ratio limited to ±0.5 %).
//
// Interpolation: 4-point cubic Hermite. Allocation happens only at
// construction; produce()/pull() are real-time safe.
#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace mixit::audio {

class SampleRing {
public:
    SampleRing(std::uint32_t channels, std::uint32_t capacity_frames);

    // Producer: writes up to `frames`; returns frames written (the rest is dropped).
    std::uint32_t write(const float* interleaved, std::uint32_t frames) noexcept;
    // Consumer: reads up to `frames`; returns frames read.
    std::uint32_t read(float* interleaved, std::uint32_t frames) noexcept;
    // Consumer: discards everything readable / the oldest `frames`.
    void clear() noexcept;
    void discard(std::uint32_t frames) noexcept;
    [[nodiscard]] std::uint32_t available() const noexcept;  // readable frames
    [[nodiscard]] std::uint32_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }

private:
    std::uint32_t channels_;
    std::uint32_t capacity_;  // frames; one slot kept empty
    std::vector<float> data_;
    alignas(64) std::atomic<std::uint64_t> write_{0};
    alignas(64) std::atomic<std::uint64_t> read_{0};
};

struct DriftStats {
    std::uint64_t underruns = 0;   // pulls that found the ring empty (silence played)
    std::uint64_t resyncs = 0;     // excess buffered audio skipped (start-up burst, producer stall recovery)
    double ratio = 1.0;            // current playback ratio (input frames per output frame)
    std::uint32_t fill_frames = 0; // smoothed ring fill
};

class DriftResampler {
public:
    // `nominal_ratio` = source rate / destination rate (1.0 for equal rates).
    DriftResampler(std::uint32_t channels, std::uint32_t target_fill_frames, double nominal_ratio = 1.0);

    // Produces `frames` output frames from `ring`. Before the ring first
    // reaches its target fill, and after an underrun until it refills, the
    // output is silence (a clean gap instead of a stutter).
    void pull(SampleRing& ring, float* out, std::uint32_t frames) noexcept;
    void reset() noexcept;
    [[nodiscard]] const DriftStats& stats() const noexcept { return stats_; }

private:
    bool next_input_frame(SampleRing& ring) noexcept;

    std::uint32_t channels_;
    std::uint32_t target_;
    double nominal_;
    bool primed_ = false;
    double phase_ = 0.0;            // position between history[1] and history[2]
    std::vector<float> history_;    // 4 frames for cubic interpolation
    double integral_ = 0.0;
    double smoothed_fill_ = 0.0;
    DriftStats stats_;
};

}  // namespace mixit::audio
