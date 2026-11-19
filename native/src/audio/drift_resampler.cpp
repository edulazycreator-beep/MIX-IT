#include "audio/drift_resampler.hpp"

#include <algorithm>
#include <cstring>

namespace mixit::audio {

SampleRing::SampleRing(std::uint32_t channels, std::uint32_t capacity_frames)
    : channels_(channels), capacity_(capacity_frames), data_(static_cast<std::size_t>(channels) * capacity_frames) {}

std::uint32_t SampleRing::available() const noexcept {
    return static_cast<std::uint32_t>(write_.load(std::memory_order_acquire) - read_.load(std::memory_order_acquire));
}

std::uint32_t SampleRing::write(const float* in, std::uint32_t frames) noexcept {
    const std::uint64_t w = write_.load(std::memory_order_relaxed);
    const std::uint64_t r = read_.load(std::memory_order_acquire);
    const auto free_frames = static_cast<std::uint32_t>(capacity_ - (w - r));
    const std::uint32_t n = std::min(frames, free_frames);
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t slot = static_cast<std::size_t>((w + i) % capacity_) * channels_;
        std::memcpy(&data_[slot], in + static_cast<std::size_t>(i) * channels_, channels_ * sizeof(float));
    }
    write_.store(w + n, std::memory_order_release);
    return n;
}

std::uint32_t SampleRing::read(float* out, std::uint32_t frames) noexcept {
    const std::uint64_t r = read_.load(std::memory_order_relaxed);
    const std::uint64_t w = write_.load(std::memory_order_acquire);
    const std::uint32_t n = std::min(frames, static_cast<std::uint32_t>(w - r));
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::size_t slot = static_cast<std::size_t>((r + i) % capacity_) * channels_;
        std::memcpy(out + static_cast<std::size_t>(i) * channels_, &data_[slot], channels_ * sizeof(float));
    }
    read_.store(r + n, std::memory_order_release);
    return n;
}

void SampleRing::clear() noexcept { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

void SampleRing::discard(std::uint32_t frames) noexcept {
    const std::uint64_t r = read_.load(std::memory_order_relaxed);
    const std::uint64_t w = write_.load(std::memory_order_acquire);
    read_.store(r + std::min<std::uint64_t>(frames, w - r), std::memory_order_release);
}

DriftResampler::DriftResampler(std::uint32_t channels, std::uint32_t target_fill_frames, double nominal_ratio)
    : channels_(channels), target_(std::max<std::uint32_t>(target_fill_frames, 1)), nominal_(nominal_ratio),
      history_(static_cast<std::size_t>(channels) * 4, 0.0f) {
    reset();
}

void DriftResampler::reset() noexcept {
    primed_ = false;
    phase_ = 0.0;
    integral_ = 0.0;
    smoothed_fill_ = static_cast<double>(target_);
    std::fill(history_.begin(), history_.end(), 0.0f);
    stats_.ratio = nominal_;
    stats_.fill_frames = 0;
}

bool DriftResampler::next_input_frame(SampleRing& ring) noexcept {
    // Shift history left by one frame and append the next input frame.
    std::memmove(history_.data(), history_.data() + channels_, static_cast<std::size_t>(channels_) * 3 * sizeof(float));
    return ring.read(history_.data() + static_cast<std::size_t>(channels_) * 3, 1) == 1;
}

void DriftResampler::pull(SampleRing& ring, float* out, std::uint32_t frames) noexcept {
    std::uint32_t fill = ring.available();
    // Far too much buffered (start-up burst, or the consumer stalled): jump
    // back to the target instead of steering the pitch for minutes.
    if (fill > 3 * target_ || (!primed_ && fill > target_ + target_ / 2)) {
        ring.discard(fill - target_);
        if (primed_) ++stats_.resyncs;
        fill = target_;
        smoothed_fill_ = static_cast<double>(fill);
        integral_ = 0.0;
    }
    // ~1 s smoothing (25 × a 40 ms target): removes the beat between the
    // producer's and the device's block sizes from the control signal.
    const double alpha = std::min(1.0, static_cast<double>(frames) / (25.0 * static_cast<double>(target_)));
    smoothed_fill_ += alpha * (static_cast<double>(fill) - smoothed_fill_);
    stats_.fill_frames = static_cast<std::uint32_t>(smoothed_fill_);
    if (!primed_) {
        if (fill < target_) {
            std::memset(out, 0, static_cast<std::size_t>(frames) * channels_ * sizeof(float));
            return;
        }
        primed_ = true;
        smoothed_fill_ = static_cast<double>(fill);
        integral_ = 0.0;
        for (int i = 0; i < 3; ++i) (void)next_input_frame(ring);  // prime the interpolator
    }

    // PI control of the playback ratio on the relative fill error. The plant
    // integrates: d(error)/dt = -adjust · rate / target (≈ -25·adjust per
    // second for a 40 ms target). Gains give a critically damped loop with a
    // ~60 s time constant: slow enough to be inaudible and to ignore the
    // block-size sawtooth of the fill level, fast enough for crystal drift.
    const double error = (smoothed_fill_ - static_cast<double>(target_)) / static_cast<double>(target_);
    integral_ = std::clamp(integral_ + error * static_cast<double>(frames) * 1e-8, -0.004, 0.004);
    const double adjust = std::clamp(0.008 * error + integral_, -0.005, 0.005);
    const double ratio = nominal_ * (1.0 + adjust);
    stats_.ratio = ratio;

    for (std::uint32_t i = 0; i < frames; ++i) {
        while (phase_ >= 1.0) {
            if (!next_input_frame(ring)) {
                // Ran dry: silence for the rest of this pull, then re-prime.
                ++stats_.underruns;
                std::memset(out + static_cast<std::size_t>(i) * channels_, 0,
                            static_cast<std::size_t>(frames - i) * channels_ * sizeof(float));
                primed_ = false;
                phase_ = 0.0;
                return;
            }
            phase_ -= 1.0;
        }
        const auto t = static_cast<float>(phase_);
        for (std::uint32_t c = 0; c < channels_; ++c) {
            const float y0 = history_[c], y1 = history_[channels_ + c], y2 = history_[2 * channels_ + c],
                        y3 = history_[3 * channels_ + c];
            // Catmull-Rom (cubic Hermite) between y1 and y2.
            const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
            const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
            const float cc = -0.5f * y0 + 0.5f * y2;
            out[static_cast<std::size_t>(i) * channels_ + c] = ((a * t + b) * t + cc) * t + y1;
        }
        phase_ += ratio;
    }
}

}  // namespace mixit::audio
