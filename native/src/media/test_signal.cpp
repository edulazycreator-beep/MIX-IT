#include "media/test_signal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace mixit::media {
namespace {

constexpr int kCodeBits = 40;  // 32 bits sequence + 8 bits source id
// Blocks kCodeBits and kCodeBits+1 are a fixed white/black sentinel, so a
// uniform image (e.g. a black program frame) never decodes as "frame 0".
constexpr int kSentinelWhite = kCodeBits;
constexpr int kSentinelBlack = kCodeBits + 1;

struct Bgra {
    std::uint8_t b, g, r, a;
};

// 75% colour bars: white, yellow, cyan, green, magenta, red, blue, black.
constexpr Bgra kBars[8] = {{191, 191, 191, 255}, {0, 191, 191, 255}, {191, 191, 0, 255}, {0, 191, 0, 255},
                           {191, 0, 191, 255},   {0, 0, 191, 255},   {191, 0, 0, 255},   {16, 16, 16, 255}};

void fill_rect(VideoFrame& f, std::uint32_t x0, std::uint32_t y0, std::uint32_t w, std::uint32_t h, Bgra c) noexcept {
    const std::uint32_t x1 = std::min(f.width, x0 + w);
    const std::uint32_t y1 = std::min(f.height, y0 + h);
    for (std::uint32_t y = y0; y < y1; ++y) {
        std::uint8_t* row = f.data + static_cast<std::size_t>(y) * f.stride;
        for (std::uint32_t x = x0; x < x1; ++x) {
            std::memcpy(row + static_cast<std::size_t>(x) * 4, &c, 4);
        }
    }
}

}  // namespace

std::uint32_t TestSignal::code_block_size(std::uint32_t width) noexcept { return std::max<std::uint32_t>(1, width / 64); }

TestSignal::TestSignal(const TestSignalConfig& config) : config_(config), bars_row_(static_cast<std::size_t>(config.width) * 4) {
    for (std::uint32_t x = 0; x < config.width; ++x) {
        const Bgra c = kBars[(static_cast<std::uint64_t>(x) * 8) / config.width];
        std::memcpy(bars_row_.data() + static_cast<std::size_t>(x) * 4, &c, 4);
    }
}

bool TestSignal::is_flash_frame(std::int64_t index) const noexcept {
    const auto interval = static_cast<std::int64_t>(config_.flash_interval_frames);
    return interval > 0 && index >= 0 && index % interval == 0;
}

std::int64_t TestSignal::click_start_sample(std::int64_t flash_frame_index) const noexcept {
    const std::int64_t t = time_of_index(flash_frame_index, config_.frame_rate);
    // Sample nearest to the frame's start time.
    return muldiv_i64(t, config_.sample_rate, kNanosPerSecond, Rounding::Nearest).value_or(0);
}

void TestSignal::render_video(VideoFrame& f, std::int64_t index, std::int64_t origin_ns) const noexcept {
    for (std::uint32_t y = 0; y < f.height; ++y) {
        std::memcpy(f.data + static_cast<std::size_t>(y) * f.stride, bars_row_.data(), bars_row_.size());
    }

    const std::uint32_t block = code_block_size(f.width);

    // Moving stripe: crosses the frame in ~4 seconds at any frame rate.
    const std::uint64_t travel = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(config_.frame_rate.num) * 4 /
                                                                config_.frame_rate.den);
    const auto x = static_cast<std::uint32_t>((static_cast<std::uint64_t>(index) % travel) * f.width / travel);
    fill_rect(f, x, 3 * block, std::max<std::uint32_t>(2, block * 2), f.height - 3 * block, {128, 128, 128, 255});

    // Frame code row (y = block): black background, white = 1.
    fill_rect(f, 0, block, f.width, block, {0, 0, 0, 255});
    const std::uint64_t code = (static_cast<std::uint64_t>(index) & 0xFFFF'FFFFu) |
                               (static_cast<std::uint64_t>(config_.source_id & 0xFFu) << 32);
    for (int bit = 0; bit < kCodeBits; ++bit) {
        if ((code >> bit) & 1u) {
            fill_rect(f, static_cast<std::uint32_t>(bit) * block, block, block, block, {255, 255, 255, 255});
        }
    }
    fill_rect(f, static_cast<std::uint32_t>(kSentinelWhite) * block, block, block, block, {255, 255, 255, 255});

    f.flags = 0;
    if (is_flash_frame(index)) {
        fill_rect(f, 0, f.height - f.height / 8, f.width, f.height / 8, {255, 255, 255, 255});
        f.flags |= kFrameFlagSyncFlash;
    }

    f.sequence = index;
    f.source_id = config_.source_id;
    f.pts_ns = origin_ns + time_of_index(index, config_.frame_rate);
    f.duration_ns = duration_of_index(index, config_.frame_rate);
}

void TestSignal::render_audio(AudioBlock& b, std::int64_t first_sample, std::uint32_t frames,
                              std::int64_t origin_ns) const noexcept {
    const std::uint32_t n = std::min(frames, b.capacity_frames);
    const std::uint32_t channels = b.channels;
    const std::uint32_t sr = config_.sample_rate;
    const Rate sample_rate{sr, 1};

    // Tone: phase from the exact integer (tone_hz * sample) mod sample_rate.
    for (std::uint32_t i = 0; i < n; ++i) {
        float v = 0.0f;
        if (config_.tone_hz > 0) {
            const std::uint64_t s = static_cast<std::uint64_t>(first_sample + i);
            const std::uint64_t phase = (static_cast<std::uint64_t>(config_.tone_hz) % sr) * (s % sr) % sr;
            v = kToneAmplitude *
                static_cast<float>(std::sin(2.0 * std::numbers::pi * static_cast<double>(phase) / static_cast<double>(sr)));
        }
        for (std::uint32_t c = 0; c < channels; ++c) b.samples[static_cast<std::size_t>(i) * channels + c] = v;
    }

    // Clicks for flash frames overlapping this block.
    if (config_.flash_interval_frames > 0) {
        const std::int64_t block_start_ns = time_of_index(first_sample, sample_rate);
        const auto interval = static_cast<std::int64_t>(config_.flash_interval_frames);
        const std::int64_t frame_at_start = index_at_time(block_start_ns, config_.frame_rate);
        std::int64_t k = frame_at_start >= 0 ? (frame_at_start / interval) * interval : 0;
        for (int candidates = 0; candidates < 3; ++candidates, k += interval) {
            const std::int64_t click0 = click_start_sample(k);
            const std::int64_t click1 = click0 + click_length_samples();
            const std::int64_t lo = std::max<std::int64_t>(click0, first_sample);
            const std::int64_t hi = std::min<std::int64_t>(click1, first_sample + n);
            for (std::int64_t s = lo; s < hi; ++s) {
                const float v = ((s - click0) % 2 == 0) ? kClickAmplitude : -kClickAmplitude;
                const auto i = static_cast<std::size_t>(s - first_sample);
                for (std::uint32_t c = 0; c < channels; ++c) b.samples[i * channels + c] = v;
            }
        }
    }

    b.frames = n;
    b.first_sample = first_sample;
    b.sample_rate = sr;
    b.source_id = config_.source_id;
    b.pts_ns = origin_ns + time_of_index(first_sample, sample_rate);
}

std::optional<FrameCode> decode_frame_code(const std::uint8_t* data, std::uint32_t stride, std::uint32_t width,
                                           std::uint32_t height) noexcept {
    const std::uint32_t block = TestSignal::code_block_size(width);
    if (data == nullptr || width < static_cast<std::uint32_t>(kSentinelBlack + 1) * block || height < 3 * block) {
        return std::nullopt;
    }
    const std::uint32_t y = block + block / 2;
    // -1 = neither clearly white nor clearly black.
    auto sample = [&](int bit) {
        const std::uint32_t x = static_cast<std::uint32_t>(bit) * block + block / 2;
        const std::uint8_t* px = data + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * 4;
        if (px[0] > 200 && px[1] > 200 && px[2] > 200) return 1;
        if (px[0] < 55 && px[1] < 55 && px[2] < 55) return 0;
        return -1;
    };
    if (sample(kSentinelWhite) != 1 || sample(kSentinelBlack) != 0) return std::nullopt;
    std::uint64_t code = 0;
    for (int bit = 0; bit < kCodeBits; ++bit) {
        const int v = sample(bit);
        if (v < 0) return std::nullopt;  // not a test-signal frame
        if (v == 1) code |= std::uint64_t{1} << bit;
    }
    return FrameCode{static_cast<std::uint32_t>(code & 0xFFFF'FFFFu), static_cast<std::uint8_t>(code >> 32)};
}

}  // namespace mixit::media
