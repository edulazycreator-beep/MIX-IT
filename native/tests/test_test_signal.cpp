#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "media/media_types.hpp"
#include "media/test_signal.hpp"

using namespace mixit::media;

namespace {
TestSignalConfig cfg(std::uint32_t w = 640, std::uint32_t h = 360, Rate fps = {30, 1}, std::uint32_t flash = 30) {
    TestSignalConfig c;
    c.width = w;
    c.height = h;
    c.frame_rate = fps;
    c.flash_interval_frames = flash;
    c.source_id = 7;
    return c;
}
}  // namespace

TEST(TestSignal, FrameCodeRoundTrips) {
    TestSignal sig(cfg());
    VideoFramePool pool(640, 360, 1);
    auto f = pool.acquire();
    for (std::int64_t idx : {0LL, 1LL, 12345LL, 0xFFFFFFFFLL, 0x1'0000'0002LL}) {
        sig.render_video(*f, idx, 1000);
        const auto code = decode_frame_code(f->data, f->stride, f->width, f->height);
        ASSERT_TRUE(code.has_value()) << idx;
        EXPECT_EQ(code->sequence_low32, static_cast<std::uint32_t>(idx & 0xFFFFFFFF));
        EXPECT_EQ(code->source_id, 7);
        EXPECT_EQ(f->sequence, idx);
        EXPECT_EQ(f->pts_ns, 1000 + time_of_index(idx, Rate{30, 1}));
    }
}

TEST(TestSignal, RenderingIsDeterministic) {
    TestSignal sig(cfg());
    VideoFramePool pool(640, 360, 2);
    auto a = pool.acquire();
    auto b = pool.acquire();
    sig.render_video(*a, 77, 0);
    sig.render_video(*b, 76, 0);
    sig.render_video(*b, 77, 0);
    for (std::uint32_t y = 0; y < 360; ++y) {
        ASSERT_EQ(std::memcmp(a->data + y * a->stride, b->data + y * b->stride, 640 * 4), 0) << "row " << y;
    }
}

TEST(TestSignal, FlashFramesAreFlaggedAndWhite) {
    TestSignal sig(cfg());
    VideoFramePool pool(640, 360, 1);
    auto f = pool.acquire();
    sig.render_video(*f, 30, 0);
    EXPECT_TRUE(f->flags & kFrameFlagSyncFlash);
    const std::uint8_t* px = f->data + (360 - 2) * f->stride + 100 * 4;
    EXPECT_EQ(px[0], 255);
    EXPECT_EQ(px[1], 255);
    EXPECT_EQ(px[2], 255);
    sig.render_video(*f, 31, 0);
    EXPECT_FALSE(f->flags & kFrameFlagSyncFlash);
}

TEST(TestSignal, NonTestImagesDoNotDecode) {
    for (std::uint8_t level : {std::uint8_t{0}, std::uint8_t{128}, std::uint8_t{255}}) {
        std::vector<std::uint8_t> image(640 * 360 * 4, level);
        EXPECT_FALSE(decode_frame_code(image.data(), 640 * 4, 640, 360).has_value()) << int{level};
    }
}

TEST(TestSignal, ToneStaysBelowClickThreshold) {
    TestSignalConfig c = cfg();
    c.flash_interval_frames = 0;
    TestSignal sig(c);
    AudioBlockPool pool(2, 480, 48000, 1);
    auto b = pool.acquire();
    float peak = 0;
    for (std::int64_t s = 0; s < 48000; s += 480) {
        sig.render_audio(*b, s, 480, 0);
        for (std::uint32_t i = 0; i < 480 * 2; ++i) peak = std::max(peak, std::abs(b->samples[i]));
    }
    EXPECT_NEAR(peak, kToneAmplitude, 0.001);
    EXPECT_LT(peak, kClickThreshold);
}

// The click must start on the sample nearest to the flash frame's start
// time — this is what makes the test signal a valid A/V sync reference.
TEST(TestSignal, ClickAlignsWithFlashFrameAcrossRates) {
    for (const Rate fps : {Rate{30000, 1001}, Rate{25, 1}, Rate{60000, 1001}}) {
        TestSignal sig(cfg(640, 360, fps, 7));
        AudioBlockPool pool(2, 480, 48000, 1);
        auto b = pool.acquire();
        std::vector<std::int64_t> onsets;
        bool active = false;
        for (std::int64_t s = 0; s < 48000 * 3; s += 480) {
            sig.render_audio(*b, s, 480, 0);
            for (std::uint32_t i = 0; i < 480; ++i) {
                const bool above = std::abs(b->samples[i * 2]) > kClickThreshold;
                if (above && !active) onsets.push_back(s + i);
                active = above;
            }
        }
        ASSERT_GE(onsets.size(), 5u) << fps.num;
        for (std::size_t k = 0; k < onsets.size(); ++k) {
            const auto frame = static_cast<std::int64_t>(k) * 7;
            EXPECT_EQ(onsets[k], sig.click_start_sample(frame));
            const std::int64_t click_ns = time_of_index(onsets[k], Rate{48000, 1});
            EXPECT_LE(std::llabs(click_ns - time_of_index(frame, fps)), 10'417) << "more than half a sample off";
        }
    }
}
