// Session audio mixer (M4): one "mixit-audio" thread mixes fixed blocks
// (10 ms) on the session sample timeline.
//
// Block k covers session samples [k·B, (k+1)·B). It is mixed when it is due
// plus the audio latency, after every input's newly delivered blocks were
// written into its timeline buffer (SampleFifo). Each strip reads its range
// shifted by its delay, processes it (channel_strip.hpp) and adds it to the
// buses in its mask; buses are processed and their blocks are offered on
// per-bus taps (drop-oldest queues) stamped with the session timeline, so
// program audio and program video share one clock.
//
// Real-time rules on this thread: no locks, no allocation, no logging.
// Parameters arrive through TripleBuffers; meters leave the same way.
#include <algorithm>
#include <cmath>
#include <cstring>

#include "media/session.hpp"
#include "media/session_internal.hpp"
#include "platform/os.hpp"
#include "util/clock.hpp"

namespace mixit::media {

namespace {

bool finite(float v) noexcept { return std::isfinite(v); }

bool in_range(float v, float lo, float hi) noexcept { return finite(v) && v >= lo && v <= hi; }

Status validate(const audio::StripParams& p) {
    auto bad = [](const char* what) { return Status{MIXIT_E_INVALID_ARGUMENT, std::string("audio strip: ") + what}; };
    if (!in_range(p.trim_db, -24.0f, 24.0f)) return bad("trim must be -24..+24 dB");
    if (!finite(p.fader_db) || p.fader_db > 12.0f) return bad("fader must be <= +12 dB");
    if (!in_range(p.balance, -1.0f, 1.0f)) return bad("balance must be -1..1");
    if (p.bus_mask >= (1u << audio::kMixBuses)) return bad("bus mask has bits beyond the aux buses");
    if (!in_range(p.delay_ms, 0.0f, audio::kMaxDelayMs)) return bad("delay must be 0..1000 ms");
    if (!in_range(p.highpass_hz, 20.0f, 1000.0f)) return bad("high-pass frequency must be 20..1000 Hz");
    for (const audio::EqBand& b : p.eq) {
        if (static_cast<std::int32_t>(b.type) < 0 || static_cast<std::int32_t>(b.type) > 4) return bad("unknown EQ type");
        if (!in_range(b.frequency, 20.0f, 20000.0f)) return bad("EQ frequency must be 20..20000 Hz");
        if (!in_range(b.gain_db, -24.0f, 24.0f)) return bad("EQ gain must be -24..+24 dB");
        if (!in_range(b.q, 0.1f, 10.0f)) return bad("EQ Q must be 0.1..10");
    }
    const audio::GateSettings& g = p.gate;
    if (!in_range(g.threshold_db, -100.0f, 0.0f)) return bad("gate threshold must be -100..0 dB");
    if (!(g.ratio == 0.0f || in_range(g.ratio, 1.0f, 20.0f))) return bad("gate ratio must be 0 (gate) or 1..20 (expander)");
    if (!in_range(g.range_db, -100.0f, 0.0f)) return bad("gate range must be -100..0 dB");
    if (!in_range(g.attack_ms, 0.0f, 100.0f) || !in_range(g.hold_ms, 0.0f, 2000.0f) ||
        !in_range(g.release_ms, 1.0f, 5000.0f)) {
        return bad("gate times out of range (attack 0..100, hold 0..2000, release 1..5000 ms)");
    }
    const audio::CompressorSettings& c = p.compressor;
    if (!in_range(c.threshold_db, -60.0f, 0.0f)) return bad("compressor threshold must be -60..0 dB");
    if (!in_range(c.ratio, 1.0f, 30.0f)) return bad("compressor ratio must be 1..30");
    if (!in_range(c.knee_db, 0.0f, 24.0f)) return bad("compressor knee must be 0..24 dB");
    if (!in_range(c.attack_ms, 0.0f, 500.0f) || !in_range(c.release_ms, 1.0f, 5000.0f)) {
        return bad("compressor times out of range (attack 0..500, release 1..5000 ms)");
    }
    if (!in_range(c.makeup_db, 0.0f, 24.0f)) return bad("compressor make-up must be 0..24 dB");
    return Status::ok();
}

Status validate(const audio::BusParams& p) {
    if (!finite(p.fader_db) || p.fader_db > 12.0f) return {MIXIT_E_INVALID_ARGUMENT, "bus fader must be <= +12 dB"};
    if (!in_range(p.limiter.ceiling_db, -30.0f, 0.0f)) {
        return {MIXIT_E_INVALID_ARGUMENT, "limiter ceiling must be -30..0 dB"};
    }
    if (!in_range(p.limiter.release_ms, 1.0f, 5000.0f)) {
        return {MIXIT_E_INVALID_ARGUMENT, "limiter release must be 1..5000 ms"};
    }
    return Status::ok();
}

void add_into(std::vector<float>& acc, const float* src) noexcept {
    for (std::size_t i = 0; i < acc.size(); ++i) acc[i] += src[i];
}

bool contains(const std::array<std::uint32_t, kMaxLayersPerScene + 1>& ids, std::uint32_t id) noexcept {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

void Session::init_audio() {
    audio_block_frames_ = config_.sample_rate / 100;
    // Mix late enough for sources that push a block at its end time, and close
    // to the video look-behind so audio-follows-video switches with the picture
    // (within one video frame).
    audio_latency_ns_ = std::max<std::int64_t>(25'000'000, latency_ns_ - 10'000'000);
    for (std::uint32_t i = 0; i < kAudioBusCount; ++i) {
        audio_buses_.push_back(std::make_unique<AudioBus>(i, config_, audio_block_frames_));
    }
    const std::size_t block_floats = static_cast<std::size_t>(audio_block_frames_) * config_.channels;
    mix_scratch_.assign(block_floats, 0.0f);
    bus_accumulators_.assign(audio::kMixBuses + 1, std::vector<float>(block_floats, 0.0f));
}

void Session::reset_audio_runtime() noexcept {
    for (auto& bus : audio_buses_) {
        bus->processor.reset();
        bus->tap.clear();
        std::lock_guard reader(bus->reader_mutex);
        bus->held.reset();
    }
    afv_.publish(AfvState{});
}

// --------------------------------------------------------------------------- control

Status Session::set_audio_strip(std::uint32_t input_id, const audio::StripParams& params) {
    if (Status st = validate(params); !st.is_ok()) return st;
    std::lock_guard lock(control_mutex_);
    Input* in = find_input(input_id);
    if (in == nullptr) return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    in->strip_shadow = params;
    in->strip_params.publish(params);
    return Status::ok();
}

Status Session::get_audio_strip(std::uint32_t input_id, audio::StripParams& out) const {
    std::lock_guard lock(control_mutex_);
    const Input* in = find_input(input_id);
    if (in == nullptr) return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    out = in->strip_shadow;
    return Status::ok();
}

Status Session::set_audio_bus(std::uint32_t bus, const audio::BusParams& params) {
    if (bus >= audio::kMixBuses) return {MIXIT_E_INVALID_ARGUMENT, "bus must be 0 (master) .. 4 (aux D)"};
    if (Status st = validate(params); !st.is_ok()) return st;
    std::lock_guard lock(control_mutex_);
    audio_buses_[bus]->shadow = params;
    audio_buses_[bus]->params.publish(params);
    return Status::ok();
}

Status Session::get_audio_bus(std::uint32_t bus, audio::BusParams& out) const {
    if (bus >= audio::kMixBuses) return {MIXIT_E_INVALID_ARGUMENT, "bus must be 0 (master) .. 4 (aux D)"};
    std::lock_guard lock(control_mutex_);
    out = audio_buses_[bus]->shadow;
    return Status::ok();
}

Status Session::set_monitor_source(std::uint32_t bus) {
    if (bus >= audio::kMixBuses) return {MIXIT_E_INVALID_ARGUMENT, "monitor source must be bus 0..4"};
    monitor_source_.store(bus, std::memory_order_relaxed);
    return Status::ok();
}

Status Session::input_meters(std::uint32_t input_id, audio::StripMeters& out) {
    std::lock_guard lock(control_mutex_);  // single meter reader at a time
    Input* in = find_input(input_id);
    if (in == nullptr) return {MIXIT_E_NOT_FOUND, "no input with id " + std::to_string(input_id)};
    out = in->strip_meters.read();
    return Status::ok();
}

Status Session::bus_meters(std::uint32_t bus, audio::BusMeters& out) {
    if (bus >= kAudioBusCount) return {MIXIT_E_INVALID_ARGUMENT, "bus must be 0..5"};
    std::lock_guard lock(control_mutex_);
    out = audio_buses_[bus]->meters.read();
    return Status::ok();
}

Status Session::reset_loudness(std::uint32_t bus) {
    if (bus >= audio::kMixBuses) return {MIXIT_E_INVALID_ARGUMENT, "loudness is measured on buses 0..4"};
    audio_buses_[bus]->reset_loudness.store(true, std::memory_order_release);
    return Status::ok();
}

Status Session::read_bus_audio(std::uint32_t bus, float* dest, std::uint32_t capacity_frames, AudioBusFrameInfo& info) {
    if (bus >= kAudioBusCount) return {MIXIT_E_INVALID_ARGUMENT, "bus must be 0..5"};
    if (dest == nullptr && capacity_frames > 0) return {MIXIT_E_INVALID_ARGUMENT, "dest is NULL"};
    info = AudioBusFrameInfo{};
    if (capacity_frames < audio_block_frames_) {
        info.frames = audio_block_frames_;
        return {MIXIT_E_BUFFER_TOO_SMALL, "capacity must hold at least one block (" +
                                              std::to_string(audio_block_frames_) + " frames)"};
    }
    AudioBus& b = *audio_buses_[bus];
    std::lock_guard reader(b.reader_mutex);
    const std::uint32_t ch = config_.channels;
    for (;;) {
        AudioRef blk = std::move(b.held);
        b.held.reset();
        if (!blk && !b.tap.pop(blk)) break;
        const bool contiguous = info.frames == 0 || blk->first_sample == info.first_sample + info.frames;
        if (!contiguous || info.frames + blk->frames > capacity_frames) {
            b.held = std::move(blk);  // returned by the next call
            break;
        }
        if (info.frames == 0) {
            info.pts_ns = blk->pts_ns;
            info.first_sample = blk->first_sample;
        }
        std::memcpy(dest + static_cast<std::size_t>(info.frames) * ch, blk->samples,
                    static_cast<std::size_t>(blk->frames) * ch * sizeof(float));
        info.frames += blk->frames;
    }
    if (info.frames == 0) return {MIXIT_E_NOT_FOUND, "no audio waiting on this bus"};
    return Status::ok();
}

// --------------------------------------------------------------------------- audio thread

void Session::publish_afv(const BusScene& program, const BusScene* next, float progress) noexcept {
    AfvState s;
    auto fill = [](const BusScene& scene, std::array<std::uint32_t, kMaxLayersPerScene + 1>& ids) {
        std::size_t n = 0;
        if (scene.base_input != 0) ids[n++] = scene.base_input;
        for (const LayerConfig& l : scene.layers) {
            if (l.input_id != 0 && l.opacity > 0.0f) ids[n++] = l.input_id;
        }
    };
    fill(program, s.on_program);
    if (next != nullptr) {
        fill(*next, s.incoming);
        s.transitioning = true;
        s.progress = progress;
    }
    afv_.publish(s);
}

void Session::match_av_events(Input& in) noexcept {
    // Called by the output thread after frame sync: pairs the newest click the
    // audio thread saw with the newest flash frame.
    if (!in.flash_valid) return;
    const std::uint64_t seq = in.click_seq.load(std::memory_order_acquire);
    if (seq == in.click_seq_seen) return;
    in.click_seq_seen = seq;
    const std::int64_t click_ns = in.last_click_ns.load(std::memory_order_relaxed);
    if (std::llabs(click_ns - in.flash_pts) > internal::kAvMatchWindowNs) return;
    const std::int64_t offset = click_ns - in.flash_pts;
    in.av_offset_last_ns.store(offset, std::memory_order_relaxed);
    internal::atomic_max(in.av_offset_max_abs_ns, std::llabs(offset));
    in.av_sync_events.fetch_add(1, std::memory_order_relaxed);
    in.flash_valid = false;
}

void Session::drain_audio(Input& in, std::int64_t next_block_first) noexcept {
    AudioRef blk;
    const Rate sr{config_.sample_rate, 1};
    while (in.audio_queue.pop(blk)) {
        in.audio_blocks_consumed.fetch_add(1, std::memory_order_relaxed);
        const AudioBlock& b = *blk;
        if (b.first_sample + static_cast<std::int64_t>(b.frames) <= next_block_first) {
            in.audio_late_blocks.fetch_add(1, std::memory_order_relaxed);
        }
        if (b.channels == config_.channels) (void)in.fifo.write(b.first_sample, b.samples, b.frames);
        // Sync-marker detection (test signal click) for A/V measurement.
        for (std::uint32_t i = 0; i < b.frames; ++i) {
            const bool above = std::abs(b.samples[static_cast<std::size_t>(i) * b.channels]) > kClickThreshold;
            if (above && !in.click_active) {
                in.last_click_ns.store(origin_ns_ + time_of_index(b.first_sample + i, sr), std::memory_order_relaxed);
                in.click_seq.fetch_add(1, std::memory_order_release);
            }
            in.click_active = above;
        }
        blk.reset();
    }
}

void Session::mix_block(std::int64_t k) noexcept {
    const std::uint32_t frames = audio_block_frames_;
    const std::uint32_t ch = config_.channels;
    const std::int64_t first = k * frames;
    const float sr = static_cast<float>(config_.sample_rate);
    for (auto& acc : bus_accumulators_) std::fill(acc.begin(), acc.end(), 0.0f);
    const AfvState& afv = afv_.read();

    bool any_solo = false;
    for (auto& in : inputs_) {
        const audio::StripParams& p = in->strip_params.read();
        any_solo = any_solo || (p.solo && in->has_audio);
    }
    for (auto& in : inputs_) {
        if (!in->has_audio) continue;
        const audio::StripParams& p = in->strip_params.read();
        const auto delay = static_cast<std::int64_t>(std::lround(p.delay_ms * sr / 1000.0f));
        const std::uint32_t missing = in->fifo.read(first - delay, mix_scratch_.data(), frames);
        if (missing > 0 && in->fifo.head() > 0) {
            in->audio_underrun_frames.fetch_add(missing, std::memory_order_relaxed);
        }
        float afv_gain = 1.0f;
        if (p.audio_follows_video) {
            const bool on_a = contains(afv.on_program, in->id);
            const bool on_b = afv.transitioning && contains(afv.incoming, in->id);
            if (!afv.transitioning) {
                afv_gain = on_a ? 1.0f : 0.0f;
            } else if (on_a && on_b) {
                afv_gain = 1.0f;
            } else {
                afv_gain = on_a ? 1.0f - afv.progress : (on_b ? afv.progress : 0.0f);
            }
        }
        in->strip.process(p, afv_gain, mix_scratch_.data(), frames);
        in->strip_meters.publish(in->strip.meters());
        for (std::uint32_t b = 0; b < audio::kMixBuses; ++b) {
            if ((p.bus_mask & (1u << b)) != 0) add_into(bus_accumulators_[b], mix_scratch_.data());
        }
        if (p.solo) add_into(bus_accumulators_[audio::kMixBuses], mix_scratch_.data());
    }

    const std::int64_t pts = origin_ns_ + time_of_index(first, Rate{config_.sample_rate, 1});
    auto emit = [&](AudioBus& bus, const float* samples) {
        AudioRef out = bus.pool.acquire();
        if (!out) return;  // a slow reader holds every block: drop (tap is drop-oldest anyway)
        std::memcpy(out->samples, samples, static_cast<std::size_t>(frames) * ch * sizeof(float));
        out->frames = frames;
        out->first_sample = first;
        out->pts_ns = pts;
        out->source_id = bus.index;
        (void)bus.tap.push(std::move(out));
    };
    for (std::uint32_t b = 0; b < audio::kMixBuses; ++b) {
        AudioBus& bus = *audio_buses_[b];
        if (bus.reset_loudness.exchange(false, std::memory_order_acq_rel)) bus.processor.reset_loudness();
        bus.processor.process(bus.params.read(), bus_accumulators_[b].data(), frames);
        bus.meters.publish(bus.processor.meters());
        emit(bus, bus_accumulators_[b].data());
    }
    // Monitor (listen) bus: soloed strips (after fader) if any, else a copy of
    // the selected bus. Never feeds program.
    std::vector<float>& monitor = bus_accumulators_[audio::kMixBuses];
    if (!any_solo) {
        const std::uint32_t src = std::min(monitor_source_.load(std::memory_order_relaxed), audio::kMixBuses - 1);
        std::copy(bus_accumulators_[src].begin(), bus_accumulators_[src].end(), monitor.begin());
    }
    AudioBus& mon = *audio_buses_[audio::kMonitorBus];
    mon.processor.process(audio::BusParams{}, monitor.data(), frames);
    mon.meters.publish(mon.processor.meters());
    emit(mon, monitor.data());
}

void Session::audio_loop() noexcept {
    os::set_current_thread_name("mixit-audio");
    const Rate sr{config_.sample_rate, 1};
    const std::int64_t block_ns = duration_of_index(0, Rate{config_.sample_rate, audio_block_frames_});
    std::int64_t k = 0;
    while (!stop_requested_.load(std::memory_order_acquire)) {
        const std::int64_t due = origin_ns_ + time_of_index((k + 1) * audio_block_frames_, sr);
        const std::int64_t wake = due + audio_latency_ns_;
        os::sleep_until_monotonic_ns(static_cast<std::uint64_t>(wake));
        if (stop_requested_.load(std::memory_order_acquire)) break;
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        const std::int64_t lateness = now - wake;
        internal::atomic_max(audio_max_lateness_ns_, lateness);
        if (lateness > block_ns) audio_late_blocks_.fetch_add(1, std::memory_order_relaxed);

        const std::int64_t first = k * audio_block_frames_;
        for (auto& in : inputs_) drain_audio(*in, first);
        mix_block(k);
        audio_blocks_.fetch_add(1, std::memory_order_relaxed);
        ++k;
        // After a long stall, resynchronize instead of mixing a burst of stale blocks.
        if (lateness > 20 * block_ns) {
            const std::int64_t resume = index_at_time(now - origin_ns_ - audio_latency_ns_, sr) / audio_block_frames_;
            if (resume > k) {
                audio_skipped_blocks_.fetch_add(static_cast<std::uint64_t>(resume - k), std::memory_order_relaxed);
                k = resume;
            }
        }
    }
}

}  // namespace mixit::media
