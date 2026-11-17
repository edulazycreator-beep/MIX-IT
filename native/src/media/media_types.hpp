// Media payload types carried through the pipeline.
//
// M2 frames are CPU BGRA8 (the reference path). M3 adds GPU texture frames
// behind the same pooled-handle model.
#pragma once

#include <cstdint>
#include <memory>
#include <new>

#include "media/pool.hpp"

namespace mixit::media {

enum class PixelFormat : std::int32_t {
    Bgra8 = 1,  // 8-bit B,G,R,A in memory order; matches DXGI_FORMAT_B8G8R8A8_UNORM
};

// VideoFrame::flags
inline constexpr std::uint32_t kFrameFlagSyncFlash = 1u << 0;  // test signal: flash frame (A/V sync marker)

struct VideoFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t stride = 0;  // bytes per row (64-byte aligned)
    PixelFormat format = PixelFormat::Bgra8;
    std::uint8_t* data = nullptr;  // owned by the pool's storage
    std::int64_t pts_ns = 0;       // presentation time on the engine master clock
    std::int64_t duration_ns = 0;
    std::int64_t sequence = 0;     // source frame index
    std::uint32_t source_id = 0;
    std::uint32_t flags = 0;
};

struct AudioBlock {
    std::uint32_t channels = 0;
    std::uint32_t capacity_frames = 0;  // sample frames the storage can hold
    std::uint32_t frames = 0;           // valid sample frames
    std::uint32_t sample_rate = 0;
    float* samples = nullptr;           // interleaved float32, owned by the pool's storage
    std::int64_t pts_ns = 0;            // time of the first sample frame
    std::int64_t first_sample = 0;      // absolute sample index of the first frame
    std::uint32_t source_id = 0;
};

using VideoPool = Pool<VideoFrame>;
using AudioPool = Pool<AudioBlock>;
using VideoRef = VideoPool::Ref;
using AudioRef = AudioPool::Ref;

// 64-byte aligned storage block shared by all payloads of one pool.
struct AlignedStorage {
    static constexpr std::size_t kAlignment = 64;
    struct Deleter {
        void operator()(std::uint8_t* p) const noexcept { ::operator delete(p, std::align_val_t{kAlignment}); }
    };
    std::unique_ptr<std::uint8_t, Deleter> bytes;

    explicit AlignedStorage(std::size_t size)
        : bytes(static_cast<std::uint8_t*>(::operator new(size, std::align_val_t{kAlignment}))) {}
};

inline std::uint32_t aligned_stride(std::uint32_t width, std::uint32_t bytes_per_pixel) noexcept {
    const std::uint32_t raw = width * bytes_per_pixel;
    return (raw + 63u) & ~63u;
}

// A pool of BGRA frames of one size, with its pixel storage.
class VideoFramePool {
public:
    VideoFramePool(std::uint32_t width, std::uint32_t height, std::uint32_t count)
        : stride_(aligned_stride(width, 4)),
          frame_bytes_(static_cast<std::size_t>(stride_) * height),
          storage_(frame_bytes_ * count),
          pool_(count, [this, width, height](VideoFrame& f, std::uint32_t i) {
              f.width = width;
              f.height = height;
              f.stride = stride_;
              f.format = PixelFormat::Bgra8;
              f.data = storage_.bytes.get() + frame_bytes_ * i;
          }) {}

    VideoRef acquire() noexcept { return pool_.acquire(); }
    [[nodiscard]] const VideoPool& pool() const noexcept { return pool_; }

private:
    std::uint32_t stride_;
    std::size_t frame_bytes_;
    AlignedStorage storage_;
    VideoPool pool_;
};

// A pool of interleaved float audio blocks.
class AudioBlockPool {
public:
    AudioBlockPool(std::uint32_t channels, std::uint32_t frames_per_block, std::uint32_t sample_rate, std::uint32_t count)
        : block_floats_(static_cast<std::size_t>(channels) * frames_per_block),
          storage_(block_floats_ * sizeof(float) * count),
          pool_(count, [this, channels, frames_per_block, sample_rate](AudioBlock& b, std::uint32_t i) {
              b.channels = channels;
              b.capacity_frames = frames_per_block;
              b.sample_rate = sample_rate;
              b.samples = reinterpret_cast<float*>(storage_.bytes.get()) + block_floats_ * i;
          }) {}

    AudioRef acquire() noexcept { return pool_.acquire(); }
    [[nodiscard]] const AudioPool& pool() const noexcept { return pool_; }

private:
    std::size_t block_floats_;
    AlignedStorage storage_;
    AudioPool pool_;
};

}  // namespace mixit::media
