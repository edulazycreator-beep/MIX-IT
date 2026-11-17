// Production session (lifecycle steps 3–8): inputs → frame sync → program bus.
//
// Milestone 2 scope (CPU reference path):
//   * Inputs are synthetic test-signal sources, each with its own resolution
//     and frame rate, produced on its own paced thread into bounded queues.
//   * One output thread ticks at the session frame rate. Every tick it runs
//     frame synchronization for each input (repeat/skip to match rates),
//     selects the program input, and produces the program frame (zero-copy
//     when sizes match, nearest-neighbour scaling otherwise; black if the
//     input has nothing yet).
//   * Audio blocks flow through bounded queues; the output thread consumes
//     them and measures A/V alignment using the test signal's flash/click.
//   * Inputs can be added/removed only while the session is not running
//     (hot-plug is an M3 feature). Program selection is allowed while running.
//
// Threading: control methods may be called from any thread (control mutex);
// media threads never take that mutex.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/status.hpp"
#include "media/media_queue.hpp"
#include "media/media_types.hpp"
#include "media/test_signal.hpp"
#include "media/time_base.hpp"

namespace mixit {
class Logger;
}

namespace mixit::media {

struct SessionConfig {
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    Rate frame_rate{30, 1};
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t program_pool_frames = 0;  // 0 = default (8)
};

enum class SessionState : std::int32_t { Configuring = 1, Running = 2, Stopped = 3, Closed = 4 };

struct InputStats {
    std::uint32_t input_id = 0;
    std::uint64_t frames_produced = 0;
    std::uint64_t frames_dropped = 0;      // input queue overflow (oldest dropped)
    std::uint64_t frames_pool_exhausted = 0;
    std::uint64_t frames_presented = 0;    // distinct frames selected by frame sync
    std::uint64_t frames_repeated = 0;     // ticks that reused the previous frame
    std::uint64_t frames_skipped = 0;      // frames superseded before any tick used them
    std::uint64_t producer_late_skips = 0; // frames the producer skipped because it fell behind
    std::uint64_t audio_blocks_produced = 0;
    std::uint64_t audio_blocks_dropped = 0;
    std::uint64_t audio_blocks_consumed = 0;
    std::uint64_t av_sync_events = 0;
    std::int64_t av_offset_last_ns = 0;     // click time - flash frame time (source timestamps)
    std::int64_t av_offset_max_abs_ns = 0;
};

struct SessionStats {
    SessionState state = SessionState::Configuring;
    std::uint32_t input_count = 0;
    std::uint32_t program_input_id = 0;
    std::uint64_t ticks = 0;
    std::uint64_t late_ticks = 0;          // started more than half a period late
    std::uint64_t skipped_ticks = 0;       // ticks skipped to recover from a long stall
    std::int64_t max_tick_lateness_ns = 0;
    std::uint64_t program_frames = 0;
    std::uint64_t program_black_frames = 0;
    std::uint64_t program_frames_dropped = 0;  // program output queue overflow
    std::uint64_t program_pool_exhausted = 0;
    std::int64_t video_latency_last_ns = 0;    // tick time - presented program frame time
    std::int64_t video_latency_max_ns = 0;
};

struct ProgramFrameInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t input_id = 0;   // 0 = black (no input frame)
    std::uint32_t flags = 0;
    std::int64_t tick_pts_ns = 0;   // output tick time on the master clock
    std::int64_t source_pts_ns = 0; // presented source frame time
    std::int64_t source_sequence = -1;
    std::uint64_t tick_index = 0;
};

class Session {
public:
    static Status create(const SessionConfig& config, Logger& logger, std::unique_ptr<Session>& out);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Status add_test_source(TestSignalConfig config, std::uint32_t& out_input_id);
    Status remove_input(std::uint32_t input_id);
    Status set_program_input(std::uint32_t input_id);  // 0 = black
    Status start();
    Status stop();
    void close() noexcept;  // stop + refuse further use (engine shutdown)

    [[nodiscard]] SessionState state() const noexcept { return state_.load(std::memory_order_acquire); }
    [[nodiscard]] const SessionConfig& config() const noexcept { return config_; }
    SessionStats stats() const;
    Status input_stats(std::uint32_t input_id, InputStats& out) const;

    // Copies the most recent program frame (BGRA) into `dest`.
    // NOT_FOUND if none has been produced yet; BUFFER_TOO_SMALL if dest is too small.
    Status copy_program_frame(std::uint8_t* dest, std::size_t dest_size, std::uint32_t dest_stride,
                              ProgramFrameInfo& info);

    // Frames/blocks currently held by pools (0 after stop: no leaks).
    [[nodiscard]] std::uint32_t pooled_items_in_use() const;

private:
    struct Input;
    struct ProgramFrame {
        VideoRef frame;
        ProgramFrameInfo info;
    };

    Session(const SessionConfig& config, Logger& logger);
    void producer_loop(Input& input) noexcept;
    void output_loop() noexcept;
    void sync_input(Input& input, std::int64_t target_ns) noexcept;
    void consume_audio(Input& input) noexcept;
    void stop_threads() noexcept;
    Input* find_input(std::uint32_t id) const noexcept;

    SessionConfig config_;
    Logger& logger_;
    std::int64_t latency_ns_ = 0;  // frame-sync look-behind: one output period + margin

    mutable std::mutex control_mutex_;
    std::atomic<SessionState> state_{SessionState::Configuring};
    std::vector<std::unique_ptr<Input>> inputs_;  // changed only while not running
    std::uint32_t next_input_id_ = 1;
    std::atomic<std::uint32_t> program_input_id_{0};

    std::unique_ptr<VideoFramePool> program_pool_;
    VideoRef black_frame_;
    MediaQueue<ProgramFrame> program_out_{2, OverflowPolicy::DropOldest};

    std::mutex consumer_mutex_;
    ProgramFrame program_cache_;  // latest frame handed to readers (guarded by consumer_mutex_)

    std::atomic<bool> stop_requested_{false};
    std::thread output_thread_;
    std::int64_t origin_ns_ = 0;

    // Output-thread counters (atomics: read by stats()).
    std::atomic<std::uint64_t> ticks_{0}, late_ticks_{0}, skipped_ticks_{0};
    std::atomic<std::int64_t> max_tick_lateness_ns_{0};
    std::atomic<std::uint64_t> program_frames_{0}, program_black_frames_{0}, program_pool_exhausted_{0};
    std::atomic<std::int64_t> video_latency_last_ns_{0}, video_latency_max_ns_{0};
};

}  // namespace mixit::media
