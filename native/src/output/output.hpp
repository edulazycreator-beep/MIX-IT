// Outputs (M5): encoding the program (or one input, ISO) and sending it to
// files and network destinations.
//
//   session threads ──feed (lock-free)──▶ EncoderPipeline ──packets──▶ Sink ×N
//   (tick: video refs,                      (thread "mixit-enc":       (thread "mixit-out" each:
//    audio: mixed blocks)                    H.264 + AAC, shared by     muxer, own bounded queue,
//                                            outputs with identical     reconnect / segments /
//                                            settings)                  storage checks)
//
// Isolation (CLAUDE.md §16.K): feeding never blocks the session; a slow
// encoder drops frames (counted); a slow or broken destination drops its own
// packets up to the next keyframe (counted, reported) or reconnects, without
// affecting other destinations or the session. Files are finalized on stop.
//
// Timestamps: video pts = tick index, audio pts = sample index, both
// relative to the first encoded tick, on the session master timeline — the
// muxers interleave by those, so A/V sync is a property of the timeline.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "media/media_types.hpp"
#include "media/time_base.hpp"

namespace mixit {
class Logger;
}

namespace mixit::output {

enum class OutputState : std::int32_t {
    Idle = 0,          // added or stopped
    Starting = 1,      // connecting / opening the file
    Running = 2,
    Reconnecting = 3,  // network destination lost; retrying with back-off
    Stopping = 4,      // draining and finalizing
    Failed = 5,        // stopped by an error (see last_error)
};

struct OutputConfig {
    std::string url;                // file path, rtmp(s)://, srt://, udp://, or a .m3u8 path (HLS)
    std::string format;             // "" = from the url; else mp4, mov, matroska, mpegts, flv, hls
    std::uint32_t source_input = 0; // 0 = program; otherwise an input id (ISO recording)
    std::string video_encoder;      // "" = automatic (hardware first); else an FFmpeg encoder name
    std::uint32_t video_kbps = 6000;
    std::uint32_t keyframe_ms = 2000;
    std::uint32_t audio_kbps = 160;
    bool fragmented = true;          // MP4/MOV: fragmented, playable after a crash
    std::uint32_t segment_seconds = 0;  // files: start a new file at the first keyframe after this
    bool reconnect = true;           // network: retry with back-off after errors
};

struct OutputStats {
    OutputState state = OutputState::Idle;
    std::string encoder;              // FFmpeg encoder in use
    bool hardware_encoder = false;
    std::string current_file;         // files: the file being written
    std::string last_error;
    std::uint64_t bytes_written = 0;
    std::uint64_t video_packets = 0;
    std::uint64_t audio_packets = 0;
    std::uint64_t dropped_packets = 0;   // this destination fell behind (dropped to the next keyframe)
    std::uint64_t encoder_dropped_frames = 0;  // frames the encoder could not take (shared encoder)
    std::uint64_t reconnects = 0;
    std::uint32_t segments = 0;          // files completed or in progress
    std::uint32_t bitrate_kbps = 0;      // written, last ~2 s
    std::uint32_t queue_ms = 0;          // buffered in this destination
    std::int64_t duration_ns = 0;        // media time written
    std::int64_t free_bytes = -1;        // files: free space on the volume (-1 unknown)
    bool data_lost = false;              // files: packets were dropped (disk too slow / full)
};

struct EncoderInfo {
    std::string name;
    std::string long_name;
    bool hardware = false;
    bool usable = false;  // opened successfully on this machine
    std::string error;
};

// Video encoders this build knows (H.264 candidates, preferred first),
// open-tested at 1280×720 30 fps. NOT_SUPPORTED without FFmpeg.
Status query_video_encoders(std::vector<EncoderInfo>& out);

struct Timeline {
    media::Rate frame_rate;
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t program_width = 0;
    std::uint32_t program_height = 0;
};

class EncoderPipeline;
class Sink;

class OutputManager {
public:
    // `wait_quiescent` returns once the session's feeding threads can no
    // longer hold a pointer published before the call (or are not running).
    OutputManager(const Timeline& timeline, Logger& logger, std::function<void()> wait_quiescent);
    ~OutputManager();
    OutputManager(const OutputManager&) = delete;
    OutputManager& operator=(const OutputManager&) = delete;

    // ---- control thread (callers serialize) ----
    Status add(const OutputConfig& config, std::uint32_t& out_id);
    Status remove(std::uint32_t id);
    // `source_size` = the source's frame size (program size or the input's).
    Status start(std::uint32_t id, std::uint32_t source_width, std::uint32_t source_height, bool source_has_audio);
    Status stop(std::uint32_t id);  // asynchronous: state goes Stopping → Idle
    void stop_all() noexcept;      // synchronous: every file finalized on return
    Status stats(std::uint32_t id, OutputStats& out) const;
    Status config(std::uint32_t id, OutputConfig& out) const;
    [[nodiscard]] std::vector<std::uint32_t> ids() const;
    [[nodiscard]] bool has_source(std::uint32_t source) const;

    // ---- feeding (output thread / audio thread); never blocks ----
    void feed_video(std::uint32_t source, const media::VideoRef& frame, std::uint64_t tick) noexcept;
    void feed_audio(std::uint32_t source, const float* samples, std::uint32_t frames, std::int64_t first_sample) noexcept;
    [[nodiscard]] bool wants(std::uint32_t source) const noexcept {
        return (source_mask_.load(std::memory_order_relaxed) & mask_bit(source)) != 0;
    }

    static constexpr std::size_t kMaxPipelines = 16;  // distinct encodes at once (hardware bound anyway)

private:
    struct Output;
    static std::uint64_t mask_bit(std::uint32_t source) noexcept { return 1ull << (source % 64); }
    void detach_locked(Output& o) noexcept;
    void release_pipeline_locked(Output& o) noexcept;
    void refresh_mask() noexcept;
    void reap_locked() noexcept;

    Timeline timeline_;
    Logger& logger_;
    std::function<void()> wait_quiescent_;
    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<Output>> outputs_;
    std::vector<std::shared_ptr<EncoderPipeline>> pipelines_;  // owned; slots below point into them
    std::array<std::atomic<EncoderPipeline*>, kMaxPipelines> slots_{};
    std::atomic<std::uint64_t> source_mask_{0};
    std::uint32_t next_id_ = 1;
};

}  // namespace mixit::output
