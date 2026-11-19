// Audio mixer through the C ABI: audio-follows-video, faders, routing, solo,
// delay, meters, loudness, and A/V alignment of the mixed program audio.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "mixit/mixit_engine.h"

namespace {

constexpr uint32_t kSr = 48000;
constexpr uint32_t kBlock = 480;

std::string last_error() {
    char buf[1024];
    (void)mixit_get_last_error_message(buf, sizeof(buf), nullptr);
    return buf;
}

struct Mix {
    mixit_engine engine = nullptr;
    mixit_session session = nullptr;

    Mix() {
        mixit_engine_config ec{};
        ec.struct_size = sizeof(ec);
        ec.api_version = MIXIT_API_VERSION;
        ec.min_log_level = MIXIT_LOG_WARNING;
        EXPECT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK);
        mixit_session_config sc{};
        sc.struct_size = sizeof(sc);
        sc.width = 320;
        sc.height = 180;
        sc.frame_rate_num = 30;
        sc.frame_rate_den = 1;
        sc.audio_sample_rate = kSr;
        sc.audio_channels = 2;
        EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_OK);
    }
    ~Mix() {
        EXPECT_EQ(mixit_session_destroy(session), MIXIT_OK);
        EXPECT_EQ(mixit_engine_destroy(engine), MIXIT_OK);
    }
    uint32_t add(uint32_t tone_hz, uint32_t flash = 0) {
        mixit_test_source_config c{};
        c.struct_size = sizeof(c);
        c.width = 320;
        c.height = 180;
        c.frame_rate_num = 30;
        c.frame_rate_den = 1;
        c.tone_hz = tone_hz;
        c.flash_interval_frames = flash;
        uint32_t id = 0;
        EXPECT_EQ(mixit_session_add_test_source(session, &c, &id), MIXIT_OK);
        return id;
    }
    mixit_audio_strip strip(uint32_t id) const {
        mixit_audio_strip s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_audio_strip(session, id, &s), MIXIT_OK);
        return s;
    }
    void set(uint32_t id, const mixit_audio_strip& s) const {
        ASSERT_EQ(mixit_session_set_audio_strip(session, id, &s), MIXIT_OK) << last_error();
    }
    void drain(uint32_t bus) const {
        std::vector<float> buf(kBlock * 2 * 64);
        mixit_audio_block_info info{};
        info.struct_size = sizeof(info);
        while (mixit_session_read_bus_audio(session, bus, buf.data(), kBlock * 64, &info) == MIXIT_OK) {
        }
    }
    // Collects `frames` contiguous frames from a bus (first sample index in *first).
    std::vector<float> collect(uint32_t bus, uint32_t frames, int64_t* first = nullptr, int64_t* pts = nullptr) const {
        std::vector<float> out;
        std::vector<float> buf(kBlock * 2 * 64);
        mixit_audio_block_info info{};
        info.struct_size = sizeof(info);
        int64_t next = -1;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (out.size() < static_cast<size_t>(frames) * 2 && std::chrono::steady_clock::now() < deadline) {
            if (mixit_session_read_bus_audio(session, bus, buf.data(), kBlock * 64, &info) != MIXIT_OK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (next >= 0) {
                EXPECT_EQ(info.first_sample, next) << "bus audio must be contiguous";
            }
            if (next < 0) {
                if (first != nullptr) *first = info.first_sample;
                if (pts != nullptr) *pts = info.pts_ns;
            }
            next = info.first_sample + info.frames;
            out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(info.frames) * 2);
        }
        out.resize(std::min(out.size(), static_cast<size_t>(frames) * 2));
        return out;
    }
};

void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Amplitude of `freq` in the left channel (Goertzel).
double tone_level(const std::vector<float>& stereo, double freq) {
    const size_t n = stereo.size() / 2;
    const double w = 2.0 * std::numbers::pi * freq / kSr;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s = stereo[2 * i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
    return 2.0 * std::sqrt(std::max(power, 0.0)) / static_cast<double>(n);
}

double db(double v) { return 20.0 * std::log10(std::max(v, 1e-9)); }

}  // namespace

TEST(AudioMixer, AudioFollowsVideoSwitchesWithTheProgram) {
    Mix m;
    const uint32_t a = m.add(1000);
    const uint32_t b = m.add(3000);
    ASSERT_EQ(mixit_session_set_program_input(m.session, a), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(m.session, b), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(m.session), MIXIT_OK);
    wait_ms(200);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    std::vector<float> x = m.collect(MIXIT_AUDIO_BUS_MASTER, 9600);
    EXPECT_NEAR(db(tone_level(x, 1000)), -20.0, 0.3) << "program input at unity";
    EXPECT_LT(db(tone_level(x, 3000)), -80.0) << "preview input is not on air";

    ASSERT_EQ(mixit_session_cut(m.session), MIXIT_OK);
    wait_ms(300);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    x = m.collect(MIXIT_AUDIO_BUS_MASTER, 9600);
    EXPECT_LT(db(tone_level(x, 1000)), -80.0);
    EXPECT_NEAR(db(tone_level(x, 3000)), -20.0, 0.3);

    // AFV off: the input is heard regardless of video.
    mixit_audio_strip s = m.strip(a);
    s.flags &= ~MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO;
    m.set(a, s);
    wait_ms(100);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    x = m.collect(MIXIT_AUDIO_BUS_MASTER, 9600);
    EXPECT_NEAR(db(tone_level(x, 1000)), -20.0, 0.3);
    EXPECT_NEAR(db(tone_level(x, 3000)), -20.0, 0.3);

    mixit_session_stats st{};
    st.struct_size = sizeof(st);
    ASSERT_EQ(mixit_session_get_stats(m.session, &st), MIXIT_OK);
    EXPECT_GT(st.audio_blocks, 50u);
    EXPECT_EQ(st.audio_skipped_blocks, 0u);
}

TEST(AudioMixer, FaderMuteTrimAndBusFader) {
    Mix m;
    const uint32_t a = m.add(1000);
    ASSERT_EQ(mixit_session_set_program_input(m.session, a), MIXIT_OK);
    mixit_audio_strip s = m.strip(a);
    s.fader_db = -6.0f;
    s.trim_db = 3.0f;
    m.set(a, s);
    ASSERT_EQ(mixit_session_start(m.session), MIXIT_OK);
    wait_ms(200);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    EXPECT_NEAR(db(tone_level(m.collect(MIXIT_AUDIO_BUS_MASTER, 9600), 1000)), -23.0, 0.3);

    mixit_audio_bus_config bus{};
    bus.struct_size = sizeof(bus);
    ASSERT_EQ(mixit_session_get_audio_bus(m.session, MIXIT_AUDIO_BUS_MASTER, &bus), MIXIT_OK);
    EXPECT_EQ(bus.flags & MIXIT_AUDIO_BUS_FLAG_LIMITER, MIXIT_AUDIO_BUS_FLAG_LIMITER) << "master limiter on by default";
    EXPECT_FLOAT_EQ(bus.limiter_ceiling_db, -1.0f);
    bus.fader_db = -10.0f;
    ASSERT_EQ(mixit_session_set_audio_bus(m.session, MIXIT_AUDIO_BUS_MASTER, &bus), MIXIT_OK);
    wait_ms(100);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    EXPECT_NEAR(db(tone_level(m.collect(MIXIT_AUDIO_BUS_MASTER, 9600), 1000)), -33.0, 0.3);

    s.flags |= MIXIT_STRIP_MUTE;
    m.set(a, s);
    wait_ms(100);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    EXPECT_LT(db(tone_level(m.collect(MIXIT_AUDIO_BUS_MASTER, 4800), 1000)), -100.0);
}

TEST(AudioMixer, RoutingSoloAndMonitor) {
    Mix m;
    const uint32_t a = m.add(1000);
    const uint32_t b = m.add(3000);
    for (uint32_t id : {a, b}) {
        mixit_audio_strip s = m.strip(id);
        s.flags &= ~MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO;
        if (id == b) s.bus_mask = 1u << MIXIT_AUDIO_BUS_AUX_A;  // B only to aux A
        m.set(id, s);
    }
    ASSERT_EQ(mixit_session_start(m.session), MIXIT_OK);
    wait_ms(200);
    for (uint32_t bus = 0; bus <= MIXIT_AUDIO_BUS_MONITOR; ++bus) m.drain(bus);
    std::vector<float> master = m.collect(MIXIT_AUDIO_BUS_MASTER, 9600);
    std::vector<float> aux = m.collect(MIXIT_AUDIO_BUS_AUX_A, 9600);
    std::vector<float> monitor = m.collect(MIXIT_AUDIO_BUS_MONITOR, 9600);
    EXPECT_NEAR(db(tone_level(master, 1000)), -20.0, 0.3);
    EXPECT_LT(db(tone_level(master, 3000)), -80.0);
    EXPECT_NEAR(db(tone_level(aux, 3000)), -20.0, 0.3);
    EXPECT_LT(db(tone_level(aux, 1000)), -80.0);
    EXPECT_NEAR(db(tone_level(monitor, 1000)), -20.0, 0.3) << "monitor follows master by default";

    ASSERT_EQ(mixit_session_set_monitor_source(m.session, MIXIT_AUDIO_BUS_AUX_A), MIXIT_OK);
    wait_ms(100);
    m.drain(MIXIT_AUDIO_BUS_MONITOR);
    monitor = m.collect(MIXIT_AUDIO_BUS_MONITOR, 9600);
    EXPECT_NEAR(db(tone_level(monitor, 3000)), -20.0, 0.3);

    // Solo A: the monitor plays A only; master is unchanged.
    mixit_audio_strip s = m.strip(a);
    s.flags |= MIXIT_STRIP_SOLO;
    m.set(a, s);
    wait_ms(100);
    m.drain(MIXIT_AUDIO_BUS_MONITOR);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    monitor = m.collect(MIXIT_AUDIO_BUS_MONITOR, 9600);
    master = m.collect(MIXIT_AUDIO_BUS_MASTER, 9600);
    EXPECT_NEAR(db(tone_level(monitor, 1000)), -20.0, 0.3);
    EXPECT_LT(db(tone_level(monitor, 3000)), -80.0);
    EXPECT_NEAR(db(tone_level(master, 1000)), -20.0, 0.3);
}

TEST(AudioMixer, MixedAudioIsAlignedWithVideoAndDelayShiftsIt) {
    Mix m;
    const uint32_t a = m.add(0, 30);  // click on every 30th frame (1 s), no tone
    ASSERT_EQ(mixit_session_set_program_input(m.session, a), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(m.session), MIXIT_OK);

    auto find_click = [&](int64_t* click_sample) {
        int64_t first = 0;
        std::vector<float> x = m.collect(MIXIT_AUDIO_BUS_MASTER, kSr * 3 / 2, &first);
        for (size_t i = 0; i < x.size() / 2; ++i) {
            if (std::abs(x[2 * i]) > 0.5f) {
                *click_sample = first + static_cast<int64_t>(i);
                return true;
            }
        }
        return false;
    };
    int64_t click = -1;
    ASSERT_TRUE(find_click(&click));
    // Flash frame k·30 starts at k seconds = sample k·48000: the click in the
    // mix sits on exactly the sample of its video frame (same master clock).
    EXPECT_EQ(click % kSr, 0) << "click at sample " << click;

    mixit_audio_strip s = m.strip(a);
    s.delay_ms = 100.0f;
    m.set(a, s);
    wait_ms(100);
    m.drain(MIXIT_AUDIO_BUS_MASTER);
    ASSERT_TRUE(find_click(&click));
    EXPECT_EQ(click % kSr, 4800) << "100 ms delay = 4800 samples later";

    mixit_input_stats is{};
    is.struct_size = sizeof(is);
    ASSERT_EQ(mixit_session_get_input_stats(m.session, a, &is), MIXIT_OK);
    EXPECT_LT(is.audio_underrun_frames, kBlock) << "steady state must not run dry";
    EXPECT_GE(is.av_sync_events, 1u) << "A/V measurement still pairs flash and click";
}

TEST(AudioMixer, MetersAndLoudness) {
    Mix m;
    const uint32_t a = m.add(1000);
    ASSERT_EQ(mixit_session_set_program_input(m.session, a), MIXIT_OK);
    mixit_audio_strip s = m.strip(a);
    s.flags |= MIXIT_STRIP_COMPRESSOR;
    s.comp_threshold_db = -30.0f;
    s.comp_ratio = 2.0f;
    s.comp_knee_db = 0.0f;
    m.set(a, s);
    ASSERT_EQ(mixit_session_start(m.session), MIXIT_OK);
    wait_ms(3500);  // short-term loudness needs 3 s
    mixit_audio_meter in{};
    in.struct_size = sizeof(in);
    ASSERT_EQ(mixit_session_get_input_meter(m.session, a, &in), MIXIT_OK);
    EXPECT_EQ(in.channels, 2u);
    // -20 dBFS tone, 10 dB over a 2:1 threshold → about -5 dB of reduction.
    EXPECT_NEAR(in.gain_reduction_db, -5.0f, 0.6f);
    EXPECT_NEAR(in.peak_db[0], -25.0f, 0.6f);
    EXPECT_FLOAT_EQ(in.applied_gain_db, 0.0f);

    mixit_audio_meter master{};
    master.struct_size = sizeof(master);
    ASSERT_EQ(mixit_session_get_bus_meter(m.session, MIXIT_AUDIO_BUS_MASTER, &master), MIXIT_OK);
    EXPECT_NEAR(master.peak_db[1], -25.0f, 0.6f);
    // Stereo 1 kHz sine at peak -25 dBFS ≈ -25 LUFS (BS.1770, see dsp test).
    EXPECT_NEAR(master.momentary_lufs, -25.0f, 0.7f);
    EXPECT_NEAR(master.short_term_lufs, -25.0f, 0.7f);
    EXPECT_NEAR(master.integrated_lufs, -25.0f, 0.7f);
    ASSERT_EQ(mixit_session_reset_loudness(m.session, MIXIT_AUDIO_BUS_MASTER), MIXIT_OK);
    wait_ms(50);
    ASSERT_EQ(mixit_session_get_bus_meter(m.session, MIXIT_AUDIO_BUS_MASTER, &master), MIXIT_OK);
    EXPECT_LT(master.short_term_lufs, -100.0f) << "reset clears the loudness history";
}

TEST(AudioMixer, Validation) {
    Mix m;
    const uint32_t a = m.add(1000);
    mixit_audio_strip s = m.strip(a);
    EXPECT_EQ(s.flags & MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO, MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO) << "AFV is the default";
    EXPECT_EQ(s.bus_mask, 1u);
    mixit_audio_strip bad = s;
    bad.fader_db = 20.0f;
    EXPECT_EQ(mixit_session_set_audio_strip(m.session, a, &bad), MIXIT_E_INVALID_ARGUMENT);
    bad = s;
    bad.bus_mask = 0x40;
    EXPECT_EQ(mixit_session_set_audio_strip(m.session, a, &bad), MIXIT_E_INVALID_ARGUMENT);
    bad = s;
    bad.eq[1].q = 0.0f;
    bad.eq[1].enabled = 1;
    EXPECT_EQ(mixit_session_set_audio_strip(m.session, a, &bad), MIXIT_E_INVALID_ARGUMENT);
    bad = s;
    bad.delay_ms = NAN;
    EXPECT_EQ(mixit_session_set_audio_strip(m.session, a, &bad), MIXIT_E_INVALID_ARGUMENT);
    bad = s;
    bad.flags |= 0x100;
    EXPECT_EQ(mixit_session_set_audio_strip(m.session, a, &bad), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_session_set_audio_strip(m.session, 99, &s), MIXIT_E_NOT_FOUND);

    s.eq[2].enabled = 1;
    s.eq[2].gain_db = 4.5f;
    s.delay_ms = 40.0f;
    m.set(a, s);
    const mixit_audio_strip back = m.strip(a);
    EXPECT_EQ(back.eq[2].enabled, 1);
    EXPECT_FLOAT_EQ(back.eq[2].gain_db, 4.5f);
    EXPECT_FLOAT_EQ(back.delay_ms, 40.0f);

    mixit_audio_bus_config bus{};
    bus.struct_size = sizeof(bus);
    bus.limiter_ceiling_db = -1.0f;
    bus.limiter_release_ms = 100.0f;
    EXPECT_EQ(mixit_session_set_audio_bus(m.session, MIXIT_AUDIO_BUS_MONITOR, &bus), MIXIT_E_INVALID_ARGUMENT);
    bus.limiter_ceiling_db = 3.0f;
    EXPECT_EQ(mixit_session_set_audio_bus(m.session, 0, &bus), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_session_set_monitor_source(m.session, MIXIT_AUDIO_BUS_MONITOR), MIXIT_E_INVALID_ARGUMENT);

    float small[16];
    mixit_audio_block_info info{};
    info.struct_size = sizeof(info);
    EXPECT_EQ(mixit_session_read_bus_audio(m.session, 0, small, 8, &info), MIXIT_E_BUFFER_TOO_SMALL);
    EXPECT_EQ(info.frames, kBlock);
    std::vector<float> buf(kBlock * 2);
    EXPECT_EQ(mixit_session_read_bus_audio(m.session, 0, buf.data(), kBlock, &info), MIXIT_E_NOT_FOUND)
        << "nothing mixed while stopped";
    EXPECT_EQ(mixit_session_read_bus_audio(m.session, 6, buf.data(), kBlock, &info), MIXIT_E_INVALID_ARGUMENT);
}
