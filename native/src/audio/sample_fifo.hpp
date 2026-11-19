// Per-input audio timeline buffer: interleaved samples addressed by absolute
// sample index on the session timeline (index 0 = session origin).
//
// The mixer writes every block a source delivers at its index and reads fixed
// ranges for each mix block (shifted by the strip's delay). Gaps in the
// source's timeline read as silence; ranges not yet written read as silence
// and are reported as underrun frames. Fixed capacity, no allocation after
// construction, single thread (the audio thread).
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace mixit::audio {

class SampleFifo {
public:
    SampleFifo(std::uint32_t channels, std::uint32_t capacity_frames)
        : channels_(channels), capacity_(capacity_frames), data_(static_cast<std::size_t>(channels) * capacity_frames) {}

    void reset() noexcept {
        head_ = 0;
        std::fill(data_.begin(), data_.end(), 0.0f);
    }

    [[nodiscard]] std::int64_t head() const noexcept { return head_; }

    // Writes `frames` at absolute index `first`. Data older than the buffer
    // capacity behind the head is ignored (returns false).
    bool write(std::int64_t first, const float* samples, std::uint32_t frames) noexcept {
        if (first + frames <= head_ - static_cast<std::int64_t>(capacity_)) return false;
        if (first > head_) zero(head_, std::min<std::int64_t>(first - head_, capacity_));  // gap = silence
        for (std::uint32_t i = 0; i < frames; ++i) {
            const std::int64_t idx = first + i;
            if (idx < head_ - static_cast<std::int64_t>(capacity_)) continue;
            std::memcpy(slot(idx), samples + static_cast<std::size_t>(i) * channels_, channels_ * sizeof(float));
        }
        head_ = std::max(head_, first + static_cast<std::int64_t>(frames));
        return true;
    }

    // Reads [first, first + frames) into `out`; returns the number of frames
    // that were not available (written as silence).
    std::uint32_t read(std::int64_t first, float* out, std::uint32_t frames) const noexcept {
        std::uint32_t missing = 0;
        for (std::uint32_t i = 0; i < frames; ++i) {
            const std::int64_t idx = first + i;
            float* o = out + static_cast<std::size_t>(i) * channels_;
            if (idx < 0 || idx >= head_ || idx < head_ - static_cast<std::int64_t>(capacity_)) {
                std::memset(o, 0, channels_ * sizeof(float));
                if (idx >= head_) ++missing;
                continue;
            }
            std::memcpy(o, slot(idx), channels_ * sizeof(float));
        }
        return missing;
    }

private:
    float* slot(std::int64_t idx) noexcept {
        return data_.data() + static_cast<std::size_t>(idx % static_cast<std::int64_t>(capacity_)) * channels_;
    }
    const float* slot(std::int64_t idx) const noexcept {
        return data_.data() + static_cast<std::size_t>(idx % static_cast<std::int64_t>(capacity_)) * channels_;
    }
    void zero(std::int64_t from, std::int64_t count) noexcept {
        for (std::int64_t i = 0; i < count; ++i) std::memset(slot(from + i), 0, channels_ * sizeof(float));
    }

    std::uint32_t channels_;
    std::uint32_t capacity_;
    std::vector<float> data_;
    std::int64_t head_ = 0;
};

}  // namespace mixit::audio
