// Clock-drift bridge: ring buffer and PI-steered resampler under simulated
// clock offsets (no real devices needed).
#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
#include <vector>

#include "audio/drift_resampler.hpp"

using namespace mixit::audio;

TEST(AudioDrift, RingIsBoundedAndOrdered) {
    SampleRing ring(2, 8);
    float in[20], out[20];
    for (int i = 0; i < 20; ++i) in[i] = static_cast<float>(i);
    EXPECT_EQ(ring.write(in, 5), 5u);
    EXPECT_EQ(ring.write(in + 10, 5), 3u) << "full: the rest is dropped";
    EXPECT_EQ(ring.available(), 8u);
    EXPECT_EQ(ring.read(out, 6), 6u);
    EXPECT_EQ(out[0], 0.0f);
    EXPECT_EQ(out[9], 9.0f);
    EXPECT_EQ(out[10], 10.0f);
    ring.clear();
    EXPECT_EQ(ring.available(), 0u);
    EXPECT_EQ(ring.read(out, 4), 0u);
}

namespace {

struct DriftRun {
    std::uint64_t underruns_after_start = 0;
    double mean_ratio = 0;  // second half of the run
    std::uint32_t min_fill = 1u << 30, max_fill = 0;
    double max_error = 0;  // vs. an ideal sine, after settling
};

// Producer: 48 kHz in 480-frame blocks on the master clock. Consumer: a
// device whose clock runs `ppm` faster, pulling 441-frame periods.
DriftRun simulate(double ppm, double seconds, std::uint32_t target) {
    SampleRing ring(1, 48000);
    DriftResampler rs(1, target);
    DriftRun run;
    const double device_rate = 48000.0 * (1.0 + ppm * 1e-6);
    const double pull_interval = 441.0 / device_rate;
    double next_pull = 0.0;
    std::vector<float> block(480), out(441);
    std::uint64_t produced = 0;
    std::uint64_t consumed = 0;
    std::uint64_t underruns_at_start = 0;
    bool started = false;
    double ratio_sum = 0;
    std::uint64_t ratio_count = 0;
    for (double t = 0.0; t < seconds; t += 0.010) {
        for (std::uint32_t i = 0; i < 480; ++i) {
            block[i] = static_cast<float>(std::sin(2.0 * std::numbers::pi * 440.0 * static_cast<double>(produced + i) / 48000.0));
        }
        ring.write(block.data(), 480);
        produced += 480;
        while (next_pull <= t) {
            rs.pull(ring, out.data(), 441);
            next_pull += pull_interval;
            if (!started && rs.stats().fill_frames > 0 && out[440] != 0.0f) {
                started = true;
                underruns_at_start = rs.stats().underruns;
            }
            if (started && t > seconds / 2) {
                run.min_fill = std::min(run.min_fill, ring.available());
                run.max_fill = std::max(run.max_fill, ring.available());
                ratio_sum += rs.stats().ratio;
                ++ratio_count;
            }
            consumed += 441;
        }
    }
    run.underruns_after_start = rs.stats().underruns - underruns_at_start;
    run.mean_ratio = ratio_count > 0 ? ratio_sum / static_cast<double>(ratio_count) : 0.0;
    (void)consumed;
    return run;
}

}  // namespace

TEST(AudioDrift, TracksDeviceClockOffsetsWithoutUnderruns) {
    for (double ppm : {-300.0, -50.0, 0.0, 120.0, 500.0}) {
        const DriftRun r = simulate(ppm, 400.0, 1920);  // ~7 minutes, 40 ms target
        EXPECT_EQ(r.underruns_after_start, 0u) << ppm << " ppm";
        // A fast device takes fewer input frames per output frame:
        // ratio = 1 / (1 + ppm) ≈ 1 - ppm.
        EXPECT_NEAR((r.mean_ratio - 1.0) * 1e6, -ppm, 15.0) << ppm << " ppm";
        EXPECT_GT(r.min_fill, 1920u / 2) << ppm << " ppm";
        EXPECT_LT(r.max_fill, 1920u * 2) << ppm << " ppm";
    }
}

TEST(AudioDrift, InterpolationIsTransparentAtUnityAndCleanAfterUnderrun) {
    SampleRing ring(1, 4800);
    DriftResampler rs(1, 2400);
    std::vector<float> in(4800), out(960);
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = static_cast<float>(std::sin(0.05 * static_cast<double>(i)));
    ring.write(in.data(), 2400);  // exactly the target fill: no correction, ratio 1
    rs.pull(ring, out.data(), 480);
    EXPECT_DOUBLE_EQ(rs.stats().ratio, 1.0);
    double max_dev = 0;
    for (std::size_t i = 0; i < 480; ++i) max_dev = std::max(max_dev, static_cast<double>(std::abs(out[i] - in[i])));
    EXPECT_LT(max_dev, 1e-6) << "unity ratio passes samples through unchanged";

    SampleRing small(1, 4800);
    DriftResampler dry(1, 480);
    small.write(in.data(), 600);
    dry.pull(small, out.data(), 960);  // needs more than the ring holds
    EXPECT_EQ(dry.stats().underruns, 1u);
    EXPECT_EQ(out[959], 0.0f) << "silence after running dry, no garbage";
    dry.pull(small, out.data(), 100);
    EXPECT_EQ(out[50], 0.0f) << "waits for the target fill before playing again";
}
