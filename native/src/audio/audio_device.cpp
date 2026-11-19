// Platform-independent parts of the audio device layer: the capture pacing
// loop shared by every capture backend, and the Null test device.
#include "audio/audio_device.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <numbers>
#include <thread>

#include "audio/capture_pacer.hpp"
#include "platform/os.hpp"
#include "util/clock.hpp"

namespace mixit::audio {

bool parse_null_device(const std::string& id, NullDeviceSpec& spec) {
    static constexpr std::string_view kPrefix = "mixit:null";
    if (id.compare(0, kPrefix.size(), kPrefix) != 0) return false;
    spec = NullDeviceSpec{};
    if (id.size() == kPrefix.size()) return true;
    if (id[kPrefix.size()] != ':') return false;
    const char* first = id.data() + kPrefix.size() + 1;
    const char* last = id.data() + id.size();
    double ppm = 0.0;
    const auto [ptr, ec] = std::from_chars(first, last, ppm);
    if (ec != std::errc{} || ptr != last || std::abs(ppm) > 5000.0) return false;
    spec.ppm = ppm;
    return true;
}

// ---- capture pacing -------------------------------------------------------------

void CapturePacer::run(media::SourceIo& io) noexcept {
    os::set_current_thread_name("mixit-acap");
    on_thread_start();
    const std::uint32_t sr = io.sample_rate();
    const std::uint32_t ch = io.channels();
    const std::uint32_t block = io.audio_block_frames();
    const media::Rate rate{sr, 1};
    const std::int64_t block_ns = media::duration_of_index(0, media::Rate{sr, block});
    SampleRing ring(ch, sr);                    // 1 s
    DriftResampler resampler(ch, sr * 3 / 100);  // 30 ms target fill
    bool open = false;
    bool reported = false;
    std::int64_t next_retry = 0;

    auto now_ns = [] { return static_cast<std::int64_t>(monotonic_ns()); };
    std::int64_t k = std::max<std::int64_t>(0, (now_ns() - io.origin_ns()) / block_ns);
    while (!io.stop_requested()) {
        const std::int64_t due = io.origin_ns() + media::time_of_index((k + 1) * block, rate) + kLatencyNs;
        io.sleep_until(due);
        if (io.stop_requested()) break;
        const std::int64_t now = now_ns();

        if (!open && now >= next_retry) {
            const Status st = open_device(sr, ch);
            if (st.is_ok()) {
                open = true;
                ring.clear();
                resampler.reset();
                if (reported) log_restored();
                reported = false;
            } else {
                io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
                if (!reported) log_failure(st);
                reported = true;
                next_retry = now + 1'000'000'000;
            }
        }
        if (open && !pump(ring)) {
            close_device();
            open = false;
            io.counters().decode_errors.fetch_add(1, std::memory_order_relaxed);
            if (!reported) log_failure(Status{MIXIT_E_IO, "capture device stopped delivering audio"});
            reported = true;
            next_retry = now + 1'000'000'000;
        }

        if (media::AudioRef b = io.acquire_audio()) {
            if (open) {
                resampler.pull(ring, b->samples, block);
            } else {
                std::memset(b->samples, 0, static_cast<std::size_t>(block) * ch * sizeof(float));
            }
            b->frames = block;
            b->first_sample = k * block;
            b->pts_ns = io.origin_ns() + media::time_of_index(b->first_sample, rate);
            io.push_audio(std::move(b));
        }
        ++k;
        if (now - due > 200'000'000) k = std::max(k, (now - io.origin_ns() - kLatencyNs) / block_ns);  // after a stall
    }
    if (open) close_device();
    on_thread_stop();
    underruns_.store(resampler.stats().underruns, std::memory_order_relaxed);
}

// ---- null test device ------------------------------------------------------------

namespace {

class NullOutput final : public AudioOutput {
public:
    NullOutput(const AudioOutputConfig& cfg, const NullDeviceSpec& spec, SampleRing& ring)
        : cfg_(cfg), spec_(spec), ring_(ring),
          resampler_(cfg.channels, std::max<std::uint32_t>(1, cfg.sample_rate * cfg.latency_ms / 1000)) {
        thread_ = std::thread([this] { loop(); });
    }
    ~NullOutput() override {
        stop_.store(true);
        if (thread_.joinable()) thread_.join();
    }
    [[nodiscard]] AudioOutputStats stats() const override {
        AudioOutputStats s;
        s.state = OutputState::Running;
        s.underruns = underruns_.load();
        s.ratio = ratio_.load();
        s.buffered_frames = buffered_.load();
        s.device_name = "Null test device";
        return s;
    }

private:
    void loop() noexcept {
        os::set_current_thread_name("mixit-aout");
        std::vector<float> buf(static_cast<std::size_t>(cfg_.channels) * 4800);
        const auto start = static_cast<std::int64_t>(monotonic_ns());
        const double rate = cfg_.sample_rate * (1.0 + spec_.ppm * 1e-6);
        std::uint64_t consumed = 0;
        while (!stop_.load(std::memory_order_acquire)) {
            os::sleep_until_monotonic_ns(monotonic_ns() + 10'000'000);
            const double elapsed = static_cast<double>(static_cast<std::int64_t>(monotonic_ns()) - start) * 1e-9;
            auto due = static_cast<std::uint64_t>(elapsed * rate);
            while (consumed < due) {
                const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(due - consumed, 4800));
                resampler_.pull(ring_, buf.data(), n);
                consumed += n;
            }
            underruns_.store(resampler_.stats().underruns, std::memory_order_relaxed);
            ratio_.store(resampler_.stats().ratio, std::memory_order_relaxed);
            buffered_.store(ring_.available(), std::memory_order_relaxed);
        }
    }

    AudioOutputConfig cfg_;
    NullDeviceSpec spec_;
    SampleRing& ring_;
    DriftResampler resampler_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<double> ratio_{1.0};
    std::atomic<std::uint32_t> buffered_{0};
    std::thread thread_;
};

// Generates a -20 dBFS 440 Hz tone at the device clock (session rate ± ppm).
class NullCapture final : public CapturePacer {
public:
    explicit NullCapture(const NullDeviceSpec& spec) : spec_(spec) {
        format_.has_audio = true;
        format_.has_video = false;
        format_.description = "null capture device (test)";
    }
    [[nodiscard]] const media::SourceFormat& format() const noexcept override { return format_; }

private:
    Status open_device(std::uint32_t sample_rate, std::uint32_t channels) noexcept override {
        sample_rate_ = sample_rate;
        channels_ = channels;
        start_ = static_cast<std::int64_t>(monotonic_ns());
        produced_ = 0;
        try {
            buf_.assign(static_cast<std::size_t>(channels) * 4800, 0.0f);
        } catch (...) {
            return {MIXIT_E_OUT_OF_MEMORY, "null capture buffer"};
        }
        return Status::ok();
    }
    bool pump(SampleRing& ring) noexcept override {
        const double rate = sample_rate_ * (1.0 + spec_.ppm * 1e-6);
        const double elapsed = static_cast<double>(static_cast<std::int64_t>(monotonic_ns()) - start_) * 1e-9;
        const auto due = static_cast<std::uint64_t>(elapsed * rate);
        while (produced_ < due) {
            const auto n = static_cast<std::uint32_t>(std::min<std::uint64_t>(due - produced_, 4800));
            for (std::uint32_t i = 0; i < n; ++i) {
                const auto v = static_cast<float>(
                    0.1 * std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(produced_ + i) / sample_rate_));
                for (std::uint32_t c = 0; c < channels_; ++c) buf_[static_cast<std::size_t>(i) * channels_ + c] = v;
            }
            ring.write(buf_.data(), n);
            produced_ += n;
        }
        return true;
    }
    void close_device() noexcept override {}
    void log_failure(const Status&) noexcept override {}
    void log_restored() noexcept override {}

    NullDeviceSpec spec_;
    media::SourceFormat format_;
    std::uint32_t sample_rate_ = 48000;
    std::uint32_t channels_ = 2;
    std::int64_t start_ = 0;
    std::uint64_t produced_ = 0;
    std::vector<float> buf_;
};

}  // namespace

Status open_null_output(const AudioOutputConfig& config, const NullDeviceSpec& spec, SampleRing& ring,
                        std::unique_ptr<AudioOutput>& out) {
    try {
        out = std::make_unique<NullOutput>(config, spec, ring);
    } catch (const std::system_error& e) {
        return {MIXIT_E_PLATFORM, std::string("cannot start the output thread: ") + e.what()};
    }
    return Status::ok();
}

Status open_null_capture_source(const NullDeviceSpec& spec, std::unique_ptr<media::MediaSource>& out) {
    out = std::make_unique<NullCapture>(spec);
    return Status::ok();
}

}  // namespace mixit::audio
