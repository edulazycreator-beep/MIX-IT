// Instant replay (M6).
//
// A ReplayBuffer keeps the last N seconds of one input as JPEG frames in
// memory (FFmpeg MJPEG, own encoder thread), stamped with the session tick
// time at which each frame was on air. A ReplaySource is an input that plays
// a buffer back from any point, at 0.1x..2x speed (slow motion repeats
// frames; no interpolation), paused or running. Video only: replays are
// silent (the program audio continues from the mixer).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/status.hpp"
#include "media/media_source.hpp"
#include "media/media_types.hpp"

namespace mixit {
class Logger;
}

namespace mixit::replay {

struct ReplayFrame {
    std::int64_t pts_ns = 0;  // session master time when the frame was on air
    std::shared_ptr<const std::vector<std::uint8_t>> jpeg;
};

struct BufferInfo {
    std::int64_t oldest_ns = 0;
    std::int64_t newest_ns = 0;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t dropped = 0;  // frames not encoded (encoder behind)
    std::uint32_t width = 0, height = 0;
};

class ReplayBuffer {
public:
    // seconds: 5..600; quality: JPEG qscale 2 (best) .. 31.
    static Status create(std::uint32_t seconds, std::uint32_t quality, Logger& logger, std::shared_ptr<ReplayBuffer>& out);
    ~ReplayBuffer();
    ReplayBuffer(const ReplayBuffer&) = delete;
    ReplayBuffer& operator=(const ReplayBuffer&) = delete;

    // Output thread; never blocks (drops and counts when the encoder is behind).
    void feed(const media::VideoRef& frame, std::int64_t pts_ns) noexcept;

    // Readers (any thread).
    [[nodiscard]] BufferInfo info() const;
    // Newest frame with pts <= at_ns (false if none).
    bool frame_at(std::int64_t at_ns, ReplayFrame& out) const;

private:
    ReplayBuffer(std::uint32_t seconds, std::uint32_t quality, Logger& logger);
    void run() noexcept;
    struct Encoder;

    const std::int64_t span_ns_;
    const std::uint32_t quality_;
    Logger& logger_;
    std::unique_ptr<Encoder> encoder_;  // encoder thread only

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::pair<media::VideoRef, std::int64_t>> queue_;  // bounded (3)
    bool stop_ = false;
    std::atomic<std::uint64_t> dropped_{0};

    mutable std::mutex frames_mutex_;
    std::deque<ReplayFrame> frames_;
    std::uint64_t bytes_ = 0;
    std::uint32_t width_ = 0, height_ = 0;

    std::thread thread_;
};

// Playback input. Control is thread-safe; frames are produced on the input thread.
class ReplaySource final : public media::MediaSource {
public:
    ReplaySource(std::shared_ptr<ReplayBuffer> buffer, std::uint32_t width, std::uint32_t height, std::uint32_t source_input);
    ~ReplaySource() override;

    [[nodiscard]] const media::SourceFormat& format() const noexcept override { return format_; }
    void run(media::SourceIo& io) noexcept override;

    // Plays from `from_ns` (buffer time) at `speed` (0.1..2); paused = hold the frame at from_ns.
    Status play(std::int64_t from_ns, float speed, bool paused);
    Status set_speed(float speed);
    Status set_paused(bool paused);
    struct State {
        bool active = false;   // play() called
        bool paused = false;
        float speed = 1.0f;
        std::int64_t position_ns = 0;  // buffer time shown
    };
    [[nodiscard]] State state() const;
    [[nodiscard]] std::uint32_t source_input() const noexcept { return source_input_; }

private:
    [[nodiscard]] std::int64_t position_locked(std::int64_t now) const noexcept;
    struct Decoder;

    std::shared_ptr<ReplayBuffer> buffer_;
    media::SourceFormat format_;
    std::uint32_t source_input_;
    mutable std::mutex mutex_;
    bool active_ = false;
    bool paused_ = false;
    float speed_ = 1.0f;
    std::int64_t anchor_pos_ns_ = 0;   // buffer time at anchor_now_ns_
    std::int64_t anchor_now_ns_ = 0;   // monotonic
    std::atomic<std::uint64_t> generation_{0};
};

}  // namespace mixit::replay
