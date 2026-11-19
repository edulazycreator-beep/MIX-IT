// Production session (lifecycle steps 3–8): inputs → frame sync → buses, and
// inputs → audio mixer → audio buses (M4, session_audio.cpp).
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

#include <array>
#include <atomic>
#include <string>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/channel_strip.hpp"
#include "core/status.hpp"
#include "media/compositor.hpp"
#include "media/media_queue.hpp"
#include "media/media_source.hpp"
#include "media/media_types.hpp"
#include "media/test_signal.hpp"
#include "media/time_base.hpp"
#include "util/triple_buffer.hpp"

namespace mixit {
class Logger;
}

namespace mixit::media {

// Which compositor a session uses. Auto = CPU in this release: frames are
// CPU-resident until the GPU decode/display paths land, and the GPU path then
// pays upload + readback on every frame (see ADR-0005).
enum class CompositorPreference : std::int32_t { Auto = 0, Cpu = 1, D3D11 = 2, D3D11Software = 3 };

struct SessionConfig {
    std::uint32_t width = 1920;
    std::uint32_t height = 1080;
    Rate frame_rate{30, 1};
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t program_pool_frames = 0;  // 0 = default (8)
    CompositorPreference compositor = CompositorPreference::Auto;
};

enum class SessionState : std::int32_t { Configuring = 1, Running = 2, Stopped = 3, Closed = 4 };

enum class Bus : std::int32_t { Program = 0, Preview = 1 };

struct LayerConfig {
    std::uint32_t input_id = 0;  // 0 = layer disabled
    NormRect crop;               // source region
    NormRect dest;               // placement on the bus output
    float opacity = 1.0f;
};

// What a bus shows: a base input (fitted with letterbox/pillarbox) and up to
// kMaxLayersPerScene overlay layers (picture-in-picture, logos, ...).
struct BusScene {
    std::uint32_t base_input = 0;  // 0 = black
    std::array<LayerConfig, kMaxLayersPerScene> layers{};
}; 

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
    std::uint64_t decode_errors = 0;       // file/capture sources
    std::uint64_t loops = 0;               // file sources: times the file restarted
    std::uint64_t audio_underrun_frames = 0;  // mix needed samples the source had not delivered (silence)
    std::uint64_t audio_late_blocks = 0;      // blocks that arrived after their mix time (discarded)
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
    std::uint32_t preview_input_id = 0;
    std::int32_t transition_active = -1;       // TransitionKind while running, -1 otherwise
    float transition_progress = 0.0f;
    CompositorKind compositor = CompositorKind::Cpu;
    std::uint64_t compositor_fallbacks = 0;    // GPU failures that switched the session to the CPU compositor
    std::uint64_t transitions_completed = 0;
    std::uint64_t composited_frames = 0;       // bus frames rendered by the compositor (not zero-copy)
    std::uint64_t audio_blocks = 0;            // mix blocks processed
    std::uint64_t audio_late_blocks = 0;       // mix blocks started more than one block late
    std::uint64_t audio_skipped_blocks = 0;    // mix blocks skipped to recover from a stall (silence)
    std::int64_t audio_max_lateness_ns = 0;
};

// Audio bus indices: 0 master, 1..4 aux A..D, 5 monitor (listen, read-only).
inline constexpr std::uint32_t kAudioBusCount = audio::kMixBuses + 1;

struct AudioBusFrameInfo {
    std::uint32_t frames = 0;
    std::int64_t pts_ns = 0;        // time of the first sample on the master clock
    std::int64_t first_sample = 0;  // session sample index of the first sample
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
    // Adds any source (file, capture). Only while not running.
    Status add_source(std::unique_ptr<MediaSource> source, std::uint32_t& out_input_id);
    Status remove_input(std::uint32_t input_id);
    Status set_program_input(std::uint32_t input_id);  // 0 = black; immediate (cancels a running transition)
    Status set_preview_input(std::uint32_t input_id);  // 0 = black
    Status cut();                                      // swap preview and program scenes now
    // Transition program -> preview over `frames` output frames; at the end
    // the scenes swap (the old program becomes preview). INVALID_STATE if a
    // transition is already running; Cut or 0 frames is an immediate cut.
    Status start_transition(TransitionKind kind, std::uint32_t frames);
    Status set_layer(Bus bus, std::uint32_t index, const LayerConfig& layer);
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
                              ProgramFrameInfo& info) {
        return copy_bus_frame(Bus::Program, dest, dest_size, dest_stride, info);
    }
    // Preview frames are rendered only while someone reads them: the first
    // request after a pause returns NOT_FOUND until the next output tick.
    Status copy_bus_frame(Bus bus, std::uint8_t* dest, std::size_t dest_size, std::uint32_t dest_stride,
                          ProgramFrameInfo& info);

    // ---- audio mixer (M4) --------------------------------------------------
    Status set_audio_strip(std::uint32_t input_id, const audio::StripParams& params);
    Status get_audio_strip(std::uint32_t input_id, audio::StripParams& out) const;
    Status set_audio_bus(std::uint32_t bus, const audio::BusParams& params);  // 0..4
    Status get_audio_bus(std::uint32_t bus, audio::BusParams& out) const;
    // Bus the monitor follows when nothing is soloed (0..4).
    Status set_monitor_source(std::uint32_t bus);
    [[nodiscard]] std::uint32_t monitor_source() const noexcept { return monitor_source_.load(std::memory_order_relaxed); }
    Status input_meters(std::uint32_t input_id, audio::StripMeters& out);
    Status bus_meters(std::uint32_t bus, audio::BusMeters& out);  // 0..5
    Status reset_loudness(std::uint32_t bus);
    // Pops whole mixed blocks of a bus (interleaved float, session channels)
    // into `dest` (capacity in sample frames). NOT_FOUND if none is waiting.
    // One consumer per bus (diagnostics, tests); outputs get their own taps (M5).
    Status read_bus_audio(std::uint32_t bus, float* dest, std::uint32_t capacity_frames, AudioBusFrameInfo& info);

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
    void audio_loop() noexcept;
    void drain_audio(Input& input, std::int64_t next_block_first) noexcept;
    void init_audio();
    void reset_audio_runtime() noexcept;
    void mix_block(std::int64_t block_index) noexcept;
    void publish_afv(const BusScene& program, const BusScene* next, float progress) noexcept;
    void match_av_events(Input& input) noexcept;
    void stop_threads() noexcept;
    struct Transition {
        bool active = false;
        TransitionKind kind = TransitionKind::Cut;
        std::uint32_t total = 0;
        std::uint32_t elapsed = 0;
    };
    SceneDraw build_draw(const BusScene& scene) const noexcept;
    bool render_bus(const BusScene& a, const BusScene* b, TransitionKind kind, float progress, std::int64_t target,
                    std::uint64_t tick, ProgramFrame& out) noexcept;
    Input* find_input(std::uint32_t id) const noexcept;
    Status add_input_locked(std::unique_ptr<MediaSource> source, std::uint32_t& out_input_id);

    SessionConfig config_;
    Logger& logger_;
    std::int64_t latency_ns_ = 0;  // frame-sync look-behind: one output period + margin

    mutable std::mutex control_mutex_;
    std::atomic<SessionState> state_{SessionState::Configuring};
    std::vector<std::unique_ptr<Input>> inputs_;  // changed only while not running
    std::uint32_t next_input_id_ = 1;

    std::unique_ptr<VideoFramePool> program_pool_;
    // GPU compositor (optional) with the CPU compositor as permanent fallback.
    // The active pointer is switched only by the output thread (or while no
    // output thread runs); stats read the atomic kind.
    std::unique_ptr<Compositor> cpu_compositor_;
    std::unique_ptr<Compositor> gpu_compositor_;
    Compositor* compositor_ = nullptr;
    std::atomic<std::int32_t> compositor_kind_{0};
    std::atomic<std::uint64_t> compositor_fallbacks_{0};
    VideoRef black_frame_;
    MediaQueue<ProgramFrame> program_out_{2, OverflowPolicy::DropOldest};
    MediaQueue<ProgramFrame> preview_out_{2, OverflowPolicy::DropOldest};

    // Scenes and transition: written by control calls, read/advanced by the
    // output thread at tick start (short critical sections only).
    mutable std::mutex scene_mutex_;
    BusScene program_scene_;
    BusScene preview_scene_;
    Transition transition_;
    std::atomic<std::int64_t> preview_requested_ns_{-1'000'000'000'000LL};

    std::mutex consumer_mutex_;
    ProgramFrame program_cache_;  // latest frames handed to readers (guarded by consumer_mutex_)
    ProgramFrame preview_cache_;

    std::atomic<bool> stop_requested_{false};
    std::thread output_thread_;
    std::int64_t origin_ns_ = 0;

    // Output-thread counters (atomics: read by stats()).
    std::atomic<std::uint64_t> ticks_{0}, late_ticks_{0}, skipped_ticks_{0};
    std::atomic<std::int64_t> max_tick_lateness_ns_{0};
    std::atomic<std::uint64_t> program_frames_{0}, program_black_frames_{0}, program_pool_exhausted_{0};
    std::atomic<std::int64_t> video_latency_last_ns_{0}, video_latency_max_ns_{0};
    std::atomic<std::uint64_t> transitions_completed_{0}, composited_frames_{0};

    // ---- audio ---------------------------------------------------------------
    // Which inputs are on air for audio-follows-video, published by the output
    // thread every tick and read by the audio thread.
    struct AfvState {
        std::array<std::uint32_t, kMaxLayersPerScene + 1> on_program{};
        std::array<std::uint32_t, kMaxLayersPerScene + 1> incoming{};  // next scene during a transition
        float progress = 0.0f;                                          // 0 = program only
        bool transitioning = false;
    };
    struct AudioBus;
    std::uint32_t audio_block_frames_ = 480;
    std::int64_t audio_latency_ns_ = 30'000'000;
    TripleBuffer<AfvState> afv_;
    std::vector<std::unique_ptr<AudioBus>> audio_buses_;  // kAudioBusCount, created with the session
    std::atomic<std::uint32_t> monitor_source_{0};
    std::vector<float> mix_scratch_;                      // one block, session channels
    std::vector<std::vector<float>> bus_accumulators_;    // kMixBuses + solo accumulator
    std::thread audio_thread_;
    std::atomic<std::uint64_t> audio_blocks_{0}, audio_late_blocks_{0}, audio_skipped_blocks_{0};
    std::atomic<std::int64_t> audio_max_lateness_ns_{0};
};

}  // namespace mixit::media
