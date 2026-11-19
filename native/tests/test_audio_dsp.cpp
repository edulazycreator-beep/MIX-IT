// Audio DSP building blocks against analytic expectations and published
// reference values (EBU Tech 3341 loudness, RBJ filter responses).
#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "audio/channel_strip.hpp"
#include "audio/dsp.hpp"
#include "audio/sample_fifo.hpp"
#include "util/triple_buffer.hpp"

using namespace mixit::audio;

namespace {

constexpr float kSr = 48000.0f;

std::vector<float> sine(float freq, float amplitude, std::uint32_t frames, std::uint32_t channels = 1) {
    std::vector<float> v(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t i = 0; i < frames; ++i) {
        const auto s = static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * freq * i / kSr));
        for (std::uint32_t c = 0; c < channels; ++c) v[static_cast<std::size_t>(i) * channels + c] = s;
    }
    return v;
}

float peak_of(const std::vector<float>& v, std::size_t from) {
    float p = 0;
    for (std::size_t i = from; i < v.size(); ++i) p = std::max(p, std::abs(v[i]));
    return p;
}

}  // namespace

TEST(AudioDsp, DecibelConversions) {
    EXPECT_FLOAT_EQ(db_to_gain(0.0f), 1.0f);
    EXPECT_NEAR(db_to_gain(-6.0206f), 0.5f, 1e-5);
    EXPECT_EQ(db_to_gain(kMinDb), 0.0f);
    EXPECT_NEAR(gain_to_db(0.5f), -6.0206f, 1e-3);
    EXPECT_EQ(gain_to_db(0.0f), kMinDb);
}

TEST(AudioDsp, BiquadResponsesMatchTheirDesign) {
    const auto peak = BiquadCoefficients::design(FilterType::Peaking, kSr, 1000, 6, 1);
    EXPECT_NEAR(peak.magnitude_db(1000, kSr), 6.0f, 0.01f);
    EXPECT_NEAR(peak.magnitude_db(50, kSr), 0.0f, 0.1f);
    EXPECT_NEAR(peak.magnitude_db(15000, kSr), 0.0f, 0.2f);

    const auto hp = BiquadCoefficients::design(FilterType::HighPass, kSr, 100, 0, 0.707f);
    EXPECT_NEAR(hp.magnitude_db(100, kSr), -3.0f, 0.1f);
    EXPECT_LT(hp.magnitude_db(20, kSr), -25.0f);  // 12 dB/oct: ~-28 dB at 20 Hz
    EXPECT_NEAR(hp.magnitude_db(2000, kSr), 0.0f, 0.05f);

    const auto low_shelf = BiquadCoefficients::design(FilterType::LowShelf, kSr, 200, -9, 0.707f);
    EXPECT_NEAR(low_shelf.magnitude_db(20, kSr), -9.0f, 0.3f);
    EXPECT_NEAR(low_shelf.magnitude_db(5000, kSr), 0.0f, 0.1f);

    // Time domain agrees with the analytic response: +6 dB at 1 kHz = x2.
    std::vector<float> s = sine(1000, 0.25f, 9600);
    BiquadState st;
    for (float& v : s) v = st.process(peak, v);
    EXPECT_NEAR(peak_of(s, 4800), 0.5f, 0.005f);
}

TEST(AudioDsp, CompressorStaticCurve) {
    CompressorSettings c;
    c.threshold_db = -20;
    c.ratio = 4;
    c.knee_db = 0;
    EXPECT_FLOAT_EQ(compressor_gain_db(c, -30), 0.0f);
    EXPECT_FLOAT_EQ(compressor_gain_db(c, -10), -7.5f);  // 10 dB over → 2.5 dB over
    c.knee_db = 6;
    EXPECT_NEAR(compressor_gain_db(c, -20), -0.5625f, 1e-4);  // middle of the soft knee
    EXPECT_FLOAT_EQ(compressor_gain_db(c, -24), 0.0f);        // below the knee
    EXPECT_FLOAT_EQ(compressor_gain_db(c, 0), -15.0f);        // above the knee
}

TEST(AudioDsp, CompressorSettlesToTheStaticCurve) {
    Dynamics d;
    d.prepare(kSr);
    CompressorSettings c;
    c.enabled = true;
    c.threshold_db = -20;
    c.ratio = 4;
    c.knee_db = 0;
    c.attack_ms = 5;
    c.makeup_db = 2;
    std::vector<float> x(4800 * 2, db_to_gain(-10.0f));  // constant level, stereo
    const float gr = d.process_compressor(c, x.data(), 4800, 2);
    EXPECT_NEAR(gr, -7.5f, 0.05f);
    EXPECT_NEAR(gain_to_db(x.back()), -10.0f - 7.5f + 2.0f, 0.05f);
}

TEST(AudioDsp, GateAndExpander) {
    Dynamics d;
    d.prepare(kSr);
    GateSettings g;
    g.enabled = true;
    g.threshold_db = -50;
    g.range_db = -40;
    g.hold_ms = 10;
    g.release_ms = 20;
    std::vector<float> quiet(48000, db_to_gain(-60.0f));
    d.process_gate(g, quiet.data(), 48000, 1);
    EXPECT_NEAR(gain_to_db(quiet.back()), -100.0f, 0.1f);  // -60 + range -40
    EXPECT_FALSE(d.gate_open());
    std::vector<float> loud(4800, db_to_gain(-20.0f));
    g.attack_ms = 0.5f;
    d.process_gate(g, loud.data(), 4800, 1);
    EXPECT_NEAR(gain_to_db(loud.back()), -20.0f, 0.01f);
    EXPECT_TRUE(d.gate_open());

    Dynamics e;  // 2:1 expander, 10 dB under threshold → 10 dB extra attenuation
    e.prepare(kSr);
    g.ratio = 2;
    g.hold_ms = 0;
    std::vector<float> under(48000, db_to_gain(-60.0f));
    e.process_gate(g, under.data(), 48000, 1);
    EXPECT_NEAR(gain_to_db(under.back()), -70.0f, 0.1f);
}

TEST(AudioDsp, LimiterNeverExceedsItsCeiling) {
    Dynamics d;
    d.prepare(kSr);
    LimiterSettings l;
    l.enabled = true;
    l.ceiling_db = -1;
    std::vector<float> x = sine(440, 2.0f, 9600, 2);  // +6 dBFS
    for (std::size_t i = 0; i < x.size(); i += 997) x[i] = 3.5f;  // spikes
    const float gr = d.process_limiter(l, x.data(), 9600, 2);
    EXPECT_LE(peak_of(x, 0), db_to_gain(-1.0f) + 1e-6f);
    EXPECT_LT(gr, -6.0f);
    LimiterSettings off;
    std::vector<float> y(10, 2.0f);
    d.process_limiter(off, y.data(), 10, 1);
    EXPECT_EQ(y[5], 2.0f);
}

TEST(AudioDsp, LevelMeterPeakAndRms) {
    LevelMeter m;
    m.prepare(kSr, 2);
    std::vector<float> x = sine(1000, 0.5f, 48000, 2);
    m.process(x.data(), 48000);
    const MeterReading r = m.reading();
    EXPECT_NEAR(r.peak_db[0], -6.02f, 0.05f);
    EXPECT_NEAR(r.rms_db[1], -9.03f, 0.1f);
    std::vector<float> silence(48000 * 2, 0.0f);
    m.process(silence.data(), 48000);
    EXPECT_NEAR(m.reading().peak_db[0], -6.02f - 20.0f / 1.5f, 0.1f);  // 20 dB / 1.5 s fall
}

TEST(AudioDsp, LoudnessMatchesEbuReferenceSignals) {
    // EBU Tech 3341 case: stereo 997 Hz sine at -23 dBFS → -23.0 LUFS (±0.1).
    LoudnessMeter l;
    l.prepare(kSr, 2);
    std::vector<float> x = sine(997, db_to_gain(-23.0f), 48000 * 20, 2);
    l.process(x.data(), 48000 * 20);
    EXPECT_NEAR(l.momentary_lufs(), -23.0f, 0.1f);
    EXPECT_NEAR(l.short_term_lufs(), -23.0f, 0.1f);
    EXPECT_NEAR(l.integrated_lufs(), -23.0f, 0.1f);

    // Silence does not drag the integrated value down (absolute gate); only
    // the three overlapping blocks at the transition count.
    std::vector<float> silence(48000 * 2 * 3, 0.0f);
    l.process(silence.data(), 48000 * 3);
    EXPECT_NEAR(l.integrated_lufs(), -23.0f, 0.1f);
    EXPECT_LT(l.momentary_lufs(), -100.0f);

    // Mono full-scale 997 Hz: BS.1770 gives -3.01 LKFS for one channel.
    LoudnessMeter mono;
    mono.prepare(kSr, 1);
    std::vector<float> m = sine(997, 1.0f, 48000);
    mono.process(m.data(), 48000);
    EXPECT_NEAR(mono.momentary_lufs(), -3.01f, 0.1f);

    // Other sample rates use matching K-weighting.
    LoudnessMeter cd;
    cd.prepare(44100.0f, 2);
    std::vector<float> c(44100 * 2 * 2);
    for (std::uint32_t i = 0; i < 44100 * 2; ++i) {
        const auto v = static_cast<float>(db_to_gain(-23.0f) * std::sin(2.0 * std::numbers::pi * 997.0 * i / 44100.0));
        c[2 * i] = c[2 * i + 1] = v;
    }
    cd.process(c.data(), 44100 * 2);
    EXPECT_NEAR(cd.momentary_lufs(), -23.0f, 0.1f);
}

TEST(AudioDsp, BalanceLaw) {
    EXPECT_EQ(balance_gains(0.0f), (std::array<float, 2>{1.0f, 1.0f}));
    EXPECT_NEAR(balance_gains(1.0f)[0], 0.0f, 1e-6);
    EXPECT_EQ(balance_gains(1.0f)[1], 1.0f);
    EXPECT_NEAR(balance_gains(-0.5f)[1], 0.7071f, 1e-4);
    EXPECT_EQ(balance_gains(-0.5f)[0], 1.0f);
}

TEST(AudioDsp, ChannelStripChainAndRamps) {
    ChannelStrip strip(kSr, 2, 480);
    StripParams p;
    p.audio_follows_video = false;
    p.fader_db = -6.0206f;
    std::vector<float> block(480 * 2);
    for (int b = 0; b < 10; ++b) {  // first block ramps up from silence
        std::fill(block.begin(), block.end(), 0.5f);
        strip.process(p, 1.0f, block.data(), 480);
    }
    EXPECT_NEAR(block[0], 0.25f, 1e-4);
    EXPECT_NEAR(strip.meters().level.peak_db[0], -12.04f, 0.05f);

    p.mute = true;  // ramps to silence within one block, no step
    std::fill(block.begin(), block.end(), 0.5f);
    strip.process(p, 1.0f, block.data(), 480);
    EXPECT_GT(block[0], 0.2f);
    EXPECT_EQ(block[479 * 2], 0.0f);
    EXPECT_EQ(strip.meters().applied_gain_db, kMinDb);

    p.mute = false;
    p.audio_follows_video = true;  // AFV gain 0 (input not on air) silences it
    for (int b = 0; b < 2; ++b) {
        std::fill(block.begin(), block.end(), 0.5f);
        strip.process(p, 0.0f, block.data(), 480);
    }
    EXPECT_EQ(block[100], 0.0f);

    p.balance = 1.0f;  // hard right
    for (int b = 0; b < 3; ++b) {
        std::fill(block.begin(), block.end(), 0.5f);
        strip.process(p, 1.0f, block.data(), 480);
    }
    EXPECT_NEAR(block[200], 0.0f, 1e-6);       // left
    EXPECT_NEAR(block[201], 0.25f, 1e-4);      // right
}

TEST(AudioDsp, SampleFifoTimeline) {
    SampleFifo f(2, 1000);
    std::vector<float> a(100 * 2, 1.0f), out(300 * 2, -1.0f);
    ASSERT_TRUE(f.write(0, a.data(), 100));
    ASSERT_TRUE(f.write(200, a.data(), 100));  // gap 100..199 = silence
    EXPECT_EQ(f.head(), 300);
    EXPECT_EQ(f.read(0, out.data(), 300), 0u);
    EXPECT_EQ(out[0], 1.0f);
    EXPECT_EQ(out[150 * 2], 0.0f);
    EXPECT_EQ(out[250 * 2 + 1], 1.0f);
    EXPECT_EQ(f.read(250, out.data(), 100), 50u);  // 300..349 not delivered yet
    EXPECT_EQ(f.read(-50, out.data(), 60), 0u);    // before the timeline: silence, not underrun
    EXPECT_EQ(out[0], 0.0f);
    std::vector<float> big(1200 * 2, 2.0f);
    ASSERT_TRUE(f.write(300, big.data(), 1200));
    EXPECT_FALSE(f.write(0, a.data(), 100));  // older than the capacity
    EXPECT_EQ(f.read(1400, out.data(), 100), 0u);
    EXPECT_EQ(out[10], 2.0f);
}

TEST(AudioDsp, TripleBufferHandsOverLatestValueWithoutTearing) {
    struct Pair {
        std::uint64_t a = 0, b = 0;
    };
    mixit::TripleBuffer<Pair> tb;
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (std::uint64_t i = 1; i <= 200000; ++i) tb.publish(Pair{i, i * 3});
        done = true;
    });
    std::uint64_t last = 0;
    bool torn = false, monotonic = true;
    while (!done.load()) {
        const Pair& p = tb.read();
        torn = torn || p.b != p.a * 3;
        monotonic = monotonic && p.a >= last;
        last = p.a;
    }
    writer.join();
    EXPECT_FALSE(torn);
    EXPECT_TRUE(monotonic);
    EXPECT_EQ(tb.read().a, 200000u);
}
