// Media source abstraction (M3).
//
// A MediaSource produces timestamped video frames and/or audio blocks for one
// session input. The session owns everything around it — buffer pools,
// bounded queues, counters and the input's thread — and hands the source a
// SourceIo for the duration of MediaSource::run().
//
// Contract for implementations:
//   * run() executes on the input's dedicated thread from session start until
//     io.stop_requested() becomes true; it must notice stop within ~50 ms
//     (sleep with io.sleep_until(), which wakes early on stop).
//   * Timestamps are master-clock ns: io.origin_ns() + media time.
//   * Frames go out in presentation order, pushed no earlier than their pts
//     (a source behaves like capture hardware: content appears when "live").
//   * Audio is delivered in blocks of exactly io.audio_block_frames() frames
//     at the session's sample rate and channel count, on a gap-free sample
//     timeline (block pts = origin + time of its first sample).
//   * Pools may be exhausted: acquire_*() returns an empty ref; the source
//     counts the loss and continues. Never block on the session.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "media/media_queue.hpp"
#include "media/media_types.hpp"
#include "media/time_base.hpp"

namespace mixit::media {

struct SourceFormat {
    bool has_video = false;
    bool has_audio = false;
    std::uint32_t width = 0;   // video size (frames from this source)
    std::uint32_t height = 0;
    Rate nominal_frame_rate{30, 1};  // for buffer sizing and display only; timing uses pts
    std::string description;
};

// Per-input counters written by the source thread, read by stats().
struct SourceCounters {
    std::atomic<std::uint64_t> frames_produced{0};
    std::atomic<std::uint64_t> frames_pool_exhausted{0};
    std::atomic<std::uint64_t> producer_late_skips{0};
    std::atomic<std::uint64_t> audio_blocks_produced{0};
    std::atomic<std::uint64_t> audio_pool_exhausted{0};
    std::atomic<std::uint64_t> decode_errors{0};
    std::atomic<std::uint64_t> loops{0};
};

class SourceIo {
public:
    SourceIo(VideoFramePool& video_pool, AudioBlockPool& audio_pool, MediaQueue<VideoRef>& video_queue,
             MediaQueue<AudioRef>& audio_queue, SourceCounters& counters, const std::atomic<bool>& stop,
             std::int64_t origin_ns, std::uint32_t sample_rate, std::uint32_t channels,
             std::uint32_t audio_block_frames) noexcept
        : video_pool_(video_pool), audio_pool_(audio_pool), video_queue_(video_queue), audio_queue_(audio_queue),
          counters_(counters), stop_(stop), origin_ns_(origin_ns), sample_rate_(sample_rate), channels_(channels),
          audio_block_frames_(audio_block_frames) {}

    [[nodiscard]] bool stop_requested() const noexcept { return stop_.load(std::memory_order_acquire); }
    [[nodiscard]] std::int64_t origin_ns() const noexcept { return origin_ns_; }
    [[nodiscard]] std::uint32_t sample_rate() const noexcept { return sample_rate_; }
    [[nodiscard]] std::uint32_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::uint32_t audio_block_frames() const noexcept { return audio_block_frames_; }
    SourceCounters& counters() noexcept { return counters_; }

    // Sleeps until master-clock time `target_ns`, waking at least every 20 ms
    // to observe stop. Returns false if stop was requested.
    bool sleep_until(std::int64_t target_ns) const noexcept;

    VideoRef acquire_video() noexcept {
        VideoRef f = video_pool_.acquire();
        if (!f) counters_.frames_pool_exhausted.fetch_add(1, std::memory_order_relaxed);
        return f;
    }
    AudioRef acquire_audio() noexcept {
        AudioRef b = audio_pool_.acquire();
        if (!b) counters_.audio_pool_exhausted.fetch_add(1, std::memory_order_relaxed);
        return b;
    }
    void push_video(VideoRef&& frame) noexcept {
        counters_.frames_produced.fetch_add(1, std::memory_order_relaxed);
        (void)video_queue_.push(std::move(frame));  // DropOldest: counted by the queue
    }
    void push_audio(AudioRef&& block) noexcept {
        counters_.audio_blocks_produced.fetch_add(1, std::memory_order_relaxed);
        (void)audio_queue_.push(std::move(block));  // DropNewest: counted by the queue
    }

private:
    VideoFramePool& video_pool_;
    AudioBlockPool& audio_pool_;
    MediaQueue<VideoRef>& video_queue_;
    MediaQueue<AudioRef>& audio_queue_;
    SourceCounters& counters_;
    const std::atomic<bool>& stop_;
    std::int64_t origin_ns_;
    std::uint32_t sample_rate_;
    std::uint32_t channels_;
    std::uint32_t audio_block_frames_;
};

class MediaSource {
public:
    virtual ~MediaSource() = default;
    [[nodiscard]] virtual const SourceFormat& format() const noexcept = 0;
    // Called once per session run on the input's thread. May be called again
    // after a stop (restart): implementations reset their timeline each time.
    virtual void run(SourceIo& io) noexcept = 0;
};

}  // namespace mixit::media
