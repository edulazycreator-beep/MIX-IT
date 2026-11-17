// Deterministic synthetic test signal (video + audio).
//
// Used to exercise the pipeline without capture hardware (master prompt §23)
// and to measure synchronization:
//   * Video: colour bars, a moving stripe (motion is visible), a binary
//     "frame code" row encoding the frame index and source id (machine
//     readable from any output), and a white flash band on sync frames.
//   * Audio: a sine tone at -20 dBFS plus a 2 ms ±0.9 click whose first
//     sample is the sample nearest to each flash frame's start time.
// Every pixel/sample is a pure function of (frame index | sample index), so
// output is reproducible and long runs cannot drift.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "media/media_types.hpp"
#include "media/time_base.hpp"

namespace mixit::media {

struct TestSignalConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    Rate frame_rate{30, 1};
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t tone_hz = 1000;            // 0 = silence between clicks
    std::uint32_t flash_interval_frames = 0;  // 0 = no flash/click
    std::uint32_t source_id = 0;
};

inline constexpr float kToneAmplitude = 0.1f;   // -20 dBFS
inline constexpr float kClickAmplitude = 0.9f;
inline constexpr float kClickThreshold = 0.5f;  // detector threshold (tone never reaches it)

struct FrameCode {
    std::uint32_t sequence_low32 = 0;
    std::uint8_t source_id = 0;
};

class TestSignal {
public:
    explicit TestSignal(const TestSignalConfig& config);

    [[nodiscard]] const TestSignalConfig& config() const noexcept { return config_; }

    // Renders frame `index` into `frame` (which must match width/height) and
    // sets pts/duration/sequence/flags relative to `origin_ns`.
    void render_video(VideoFrame& frame, std::int64_t index, std::int64_t origin_ns) const noexcept;

    // Fills `block` with `frames` sample frames starting at absolute sample
    // index `first_sample`, timestamps relative to `origin_ns`.
    void render_audio(AudioBlock& block, std::int64_t first_sample, std::uint32_t frames,
                      std::int64_t origin_ns) const noexcept;

    [[nodiscard]] bool is_flash_frame(std::int64_t index) const noexcept;

    // Absolute sample index at which the click for flash frame `index` starts.
    [[nodiscard]] std::int64_t click_start_sample(std::int64_t flash_frame_index) const noexcept;
    [[nodiscard]] std::uint32_t click_length_samples() const noexcept { return config_.sample_rate / 500; }

    // Size of one code block in pixels for a given frame width.
    static std::uint32_t code_block_size(std::uint32_t width) noexcept;

private:
    TestSignalConfig config_;
    std::vector<std::uint8_t> bars_row_;  // one BGRA row of colour bars
};

// Reads the frame code written by TestSignal::render_video from any BGRA
// image of the same layout (e.g. a program output frame at source size).
std::optional<FrameCode> decode_frame_code(const std::uint8_t* data, std::uint32_t stride, std::uint32_t width,
                                           std::uint32_t height) noexcept;

}  // namespace mixit::media
