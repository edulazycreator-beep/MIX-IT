// Internals shared by session.cpp (video, lifecycle) and session_audio.cpp
// (audio mixer). Not part of any public interface.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include "audio/channel_strip.hpp"
#include "audio/sample_fifo.hpp"
#include "media/media_source.hpp"
#include "media/session.hpp"
#include "replay/replay.hpp"
#include "util/triple_buffer.hpp"

namespace mixit::media {

namespace internal {

constexpr std::uint32_t kInputVideoQueue = 4;
// queue + pending + current + program queue (2) + reader cache + margin
constexpr std::uint32_t kInputVideoPool = kInputVideoQueue + 8 + 3 + 3;  // + ISO encoder queue + replay queue
constexpr std::uint32_t kInputAudioQueue = 32;  // 320 ms of 10 ms blocks
// queue + file read-ahead (24) + margin
constexpr std::uint32_t kInputAudioPool = kInputAudioQueue + 32;
// program out (2) + reader cache (1) + preview out (2) + cache (1) + black (1) + in flight (1) + margin
constexpr std::uint32_t kDefaultProgramPool = 18;  // 10 + encoder queues (M5) + multiview (M6)
constexpr std::int64_t kAvMatchWindowNs = 100'000'000;
constexpr std::int64_t kFlashExpiryNs = 500'000'000;
constexpr std::uint32_t kTapBlocks = 50;  // 500 ms per audio bus tap

inline audio::BusParams default_bus(std::uint32_t index) {
    audio::BusParams p;
    if (index == 0) {  // master: protect the output by default
        p.limiter.enabled = true;
        p.limiter.ceiling_db = -1.0f;
    }
    return p;
}


inline void atomic_max(std::atomic<std::int64_t>& target, std::int64_t value) noexcept {
    std::int64_t cur = target.load(std::memory_order_relaxed);
    while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed)) {
    }
}

}  // namespace internal

struct Session::Input {
    std::uint32_t id = 0;
    std::unique_ptr<MediaSource> source;

    // Pools first: destroyed last (queues and refs below hold their items).
    VideoFramePool video_pool;
    AudioBlockPool audio_pool;
    MediaQueue<VideoRef> video_queue{internal::kInputVideoQueue, OverflowPolicy::DropOldest};
    MediaQueue<AudioRef> audio_queue{internal::kInputAudioQueue, OverflowPolicy::DropNewest};

    std::thread producer;
    SourceCounters counters;  // written by the source thread

    // Output-thread state (only touched by the output thread while running).
    VideoRef pending;
    VideoRef current;
    bool current_presented = false;
    bool flash_valid = false;
    std::int64_t flash_pts = 0;
    std::uint64_t click_seq_seen = 0;

    // Audio (M4). Strip parameters: control → audio thread; meters: audio → control.
    bool has_audio;
    TripleBuffer<audio::StripParams> strip_params;
    audio::StripParams strip_shadow;  // last published value (control mutex)
    TripleBuffer<audio::StripMeters> strip_meters;
    audio::SampleFifo fifo;           // audio thread only
    audio::ChannelStrip strip;        // audio thread only
    // VST3 inserts: the audio thread reads `inserts`; `insert_owned` keeps
    // the effects alive and is changed under the session control mutex.
    std::array<std::atomic<audio::Vst3Effect*>, Session::kInsertSlots> inserts{};
    std::array<std::unique_ptr<audio::Vst3Effect>, Session::kInsertSlots> insert_owned;
    // Instant replay recording of this input (M6): the output thread feeds
    // `replay`; `replay_owned` (control) keeps it alive, shared with replay inputs.
    std::atomic<replay::ReplayBuffer*> replay{nullptr};
    std::shared_ptr<replay::ReplayBuffer> replay_owned;
    bool click_active = false;        // audio thread only
    std::atomic<std::int64_t> last_click_ns{0};
    std::atomic<std::uint64_t> click_seq{0};
    std::atomic<std::uint64_t> audio_underrun_frames{0}, audio_late_blocks{0};
    std::atomic<std::uint64_t> frames_presented{0}, frames_repeated{0}, frames_skipped{0};
    std::atomic<std::uint64_t> audio_blocks_consumed{0}, av_sync_events{0};
    std::atomic<std::int64_t> av_offset_last_ns{0}, av_offset_max_abs_ns{0};

    Input(std::uint32_t input_id, std::unique_ptr<MediaSource> src, const SessionConfig& session)
        : id(input_id),
          source(std::move(src)),
          video_pool(std::max<std::uint32_t>(2, source->format().width), std::max<std::uint32_t>(2, source->format().height),
                     internal::kInputVideoPool),
          audio_pool(session.channels, session.sample_rate / 100, session.sample_rate, internal::kInputAudioPool),
          has_audio(source->format().has_audio),
          // Delay range + 500 ms of look-ahead/latency headroom.
          fifo(session.channels, session.sample_rate * 3 / 2),
          strip(static_cast<float>(session.sample_rate), session.channels, session.sample_rate / 100) {}

    void reset_runtime_state() noexcept {
        pending.reset();
        current.reset();
        current_presented = false;
        click_active = false;
        flash_valid = false;
        click_seq_seen = click_seq.load(std::memory_order_relaxed);
        video_queue.clear();
        audio_queue.clear();
        fifo.reset();
        strip.reset();
    }
};


// One audio bus: parameters, processing state, meters and the read tap.
struct Session::AudioBus {
    std::uint32_t index;
    TripleBuffer<audio::BusParams> params;
    audio::BusParams shadow;
    audio::BusProcessor processor;
    TripleBuffer<audio::BusMeters> meters;
    std::atomic<bool> reset_loudness{false};
    AudioBlockPool pool;
    MediaQueue<AudioRef> tap{internal::kTapBlocks, OverflowPolicy::DropOldest};
    std::mutex reader_mutex;
    AudioRef held;  // block popped by a reader but not yet returned (guarded by reader_mutex)

    AudioBus(std::uint32_t i, const SessionConfig& cfg, std::uint32_t block)
        : index(i),
          params(internal::default_bus(i)),
          shadow(internal::default_bus(i)),
          processor(static_cast<float>(cfg.sample_rate), cfg.channels, i < audio::kMixBuses),
          pool(cfg.channels, block, cfg.sample_rate, internal::kTapBlocks + 4) {}
};

}  // namespace mixit::media
