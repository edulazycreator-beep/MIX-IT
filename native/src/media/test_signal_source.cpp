#include "media/test_signal_source.hpp"

#include <algorithm>
#include <string>

#include "platform/os.hpp"
#include "util/clock.hpp"

namespace mixit::media {

bool SourceIo::sleep_until(std::int64_t target_ns) const noexcept {
    constexpr std::int64_t kStopCheckNs = 20'000'000;
    for (;;) {
        if (stop_requested()) return false;
        const auto now = static_cast<std::int64_t>(monotonic_ns());
        if (now >= target_ns) return true;
        os::sleep_until_monotonic_ns(static_cast<std::uint64_t>(std::min(target_ns, now + kStopCheckNs)));
    }
}

TestSignalSource::TestSignalSource(const TestSignalConfig& config) : signal_(config) {
    format_.has_video = true;
    format_.has_audio = true;
    format_.width = config.width;
    format_.height = config.height;
    format_.nominal_frame_rate = config.frame_rate;
    format_.description = "test signal " + std::to_string(config.width) + "x" + std::to_string(config.height);
}

void TestSignalSource::run(SourceIo& io) noexcept {
    const TestSignalConfig& cfg = signal_.config();
    const Rate sample_rate{io.sample_rate(), 1};
    const std::uint32_t block = io.audio_block_frames();
    const std::int64_t origin = io.origin_ns();
    std::int64_t next_frame = 0;
    std::int64_t next_sample = 0;

    while (!io.stop_requested()) {
        // A frame is "captured" at its start time; an audio block once its last sample is.
        const std::int64_t video_due = origin + time_of_index(next_frame, cfg.frame_rate);
        const std::int64_t audio_due = origin + time_of_index(next_sample + block, sample_rate);
        if (!io.sleep_until(std::min(video_due, audio_due))) break;
        const auto now = static_cast<std::int64_t>(monotonic_ns());

        if (video_due <= now) {
            // Fell more than two frames behind: jump to the present (a real
            // capture device would have lost those frames too).
            const std::int64_t current_index = index_at_time(now - origin, cfg.frame_rate);
            if (current_index - next_frame > 2) {
                io.counters().producer_late_skips.fetch_add(static_cast<std::uint64_t>(current_index - next_frame),
                                                            std::memory_order_relaxed);
                next_frame = current_index;
            }
            if (VideoRef frame = io.acquire_video()) {
                signal_.render_video(*frame, next_frame, origin);
                io.push_video(std::move(frame));
            }
            ++next_frame;
        }
        if (audio_due <= now) {
            if (AudioRef blk = io.acquire_audio()) {
                signal_.render_audio(*blk, next_sample, block, origin);
                io.push_audio(std::move(blk));
            }
            next_sample += block;  // audio timeline never skips: gaps would break sync
        }
    }
}

}  // namespace mixit::media
