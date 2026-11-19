// Device output and capture through the C ABI, using the null test device
// (clock offset in ppm) so drift compensation runs on every platform.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <numbers>
#include <thread>
#include <vector>

#include "mixit/mixit_engine.h"

namespace {

std::string last_error() {
    char buf[1024];
    (void)mixit_get_last_error_message(buf, sizeof(buf), nullptr);
    return buf;
}

struct Rig {
    mixit_engine engine = nullptr;
    mixit_session session = nullptr;
    Rig() {
        mixit_engine_config ec{};
        ec.struct_size = sizeof(ec);
        ec.api_version = MIXIT_API_VERSION;
        ec.min_log_level = MIXIT_LOG_WARNING;
        EXPECT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK);
        mixit_session_config sc{};
        sc.struct_size = sizeof(sc);
        sc.width = 320;
        sc.height = 180;
        sc.frame_rate_num = 25;
        sc.frame_rate_den = 1;
        sc.audio_sample_rate = 48000;
        sc.audio_channels = 2;
        EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_OK);
    }
    ~Rig() {
        EXPECT_EQ(mixit_session_destroy(session), MIXIT_OK);
        EXPECT_EQ(mixit_engine_destroy(engine), MIXIT_OK);
    }
    mixit_audio_output_stats output() const {
        mixit_audio_output_stats s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_audio_output_stats(session, &s), MIXIT_OK);
        return s;
    }
};

double tone_level(const std::vector<float>& stereo, double freq) {
    const size_t n = stereo.size() / 2;
    const double coeff = 2.0 * std::cos(2.0 * std::numbers::pi * freq / 48000.0);
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
        const double s = stereo[2 * i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coeff * s1 * s2)) / static_cast<double>(n);
}

}  // namespace

TEST(AudioDevices, OutputCompensatesADeviceClockOffset) {
    Rig r;
    mixit_test_source_config c{};
    c.struct_size = sizeof(c);
    c.width = 320;
    c.height = 180;
    c.frame_rate_num = 25;
    c.frame_rate_den = 1;
    c.tone_hz = 1000;
    uint32_t id = 0;
    ASSERT_EQ(mixit_session_add_test_source(r.session, &c, &id), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_program_input(r.session, id), MIXIT_OK);
    EXPECT_EQ(r.output().state, MIXIT_AUDIO_OUTPUT_OFF);

    mixit_audio_output_config oc{};
    oc.struct_size = sizeof(oc);
    oc.bus = MIXIT_AUDIO_BUS_MONITOR;
    oc.device_id = "mixit:null:400";  // device clock 400 ppm fast
    ASSERT_EQ(mixit_session_set_audio_output(r.session, &oc), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_start(r.session), MIXIT_OK);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    mixit_audio_output_stats s = r.output();
    EXPECT_EQ(s.state, MIXIT_AUDIO_OUTPUT_RUNNING);
    EXPECT_EQ(s.bus, static_cast<uint32_t>(MIXIT_AUDIO_BUS_MONITOR));
    EXPECT_EQ(s.underruns, 0u) << "the device never ran dry";
    EXPECT_EQ(s.dropped_frames, 0u);
    EXPECT_GT(s.buffered_ms, 15u);
    EXPECT_LT(s.buffered_ms, 90u);
    EXPECT_LT(s.drift_ratio, 1.0) << "a fast device must play the buffer slower than 1:1";
    EXPECT_GT(s.drift_ratio, 0.995);
    EXPECT_STREQ(s.device_name, "Null test device");

    // Switch while running, then off.
    oc.device_id = "mixit:null";
    oc.bus = MIXIT_AUDIO_BUS_MASTER;
    oc.latency_ms = 80;
    ASSERT_EQ(mixit_session_set_audio_output(r.session, &oc), MIXIT_OK);
    EXPECT_EQ(r.output().bus, 0u);
    ASSERT_EQ(mixit_session_set_audio_output(r.session, nullptr), MIXIT_OK);
    EXPECT_EQ(r.output().state, MIXIT_AUDIO_OUTPUT_OFF);

    oc.latency_ms = 5;
    EXPECT_EQ(mixit_session_set_audio_output(r.session, &oc), MIXIT_E_INVALID_ARGUMENT);
    oc.latency_ms = 0;
    oc.bus = 6;
    EXPECT_EQ(mixit_session_set_audio_output(r.session, &oc), MIXIT_E_INVALID_ARGUMENT);
}

TEST(AudioDevices, CaptureInputIsPlacedOnTheSessionTimeline) {
    Rig r;
    mixit_audio_capture_config cc{};
    cc.struct_size = sizeof(cc);
    cc.device_id = "mixit:null:-250";  // device clock 250 ppm slow
    uint32_t mic = 0;
    ASSERT_EQ(mixit_session_add_audio_capture_source(r.session, &cc, &mic), MIXIT_OK) << last_error();
    mixit_audio_strip strip{};
    strip.struct_size = sizeof(strip);
    ASSERT_EQ(mixit_session_get_audio_strip(r.session, mic, &strip), MIXIT_OK);
    strip.flags &= ~MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO;  // a microphone is live regardless of video
    ASSERT_EQ(mixit_session_set_audio_strip(r.session, mic, &strip), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(r.session), MIXIT_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(800));

    std::vector<float> buf(480 * 2 * 64), all;
    mixit_audio_block_info info{};
    info.struct_size = sizeof(info);
    while (mixit_session_read_bus_audio(r.session, MIXIT_AUDIO_BUS_MASTER, buf.data(), 480 * 64, &info) == MIXIT_OK) {
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (all.size() < 48000 * 2 && std::chrono::steady_clock::now() < deadline) {
        if (mixit_session_read_bus_audio(r.session, MIXIT_AUDIO_BUS_MASTER, buf.data(), 480 * 64, &info) == MIXIT_OK) {
            all.insert(all.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(info.frames) * 2);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ASSERT_GE(all.size(), 48000u * 2);
    // A -20 dBFS sine: RMS -23 dBFS, continuous (no gaps from the drift bridge).
    double sum = 0;
    for (float v : all) sum += static_cast<double>(v) * v;
    EXPECT_NEAR(10.0 * std::log10(sum / static_cast<double>(all.size())), -23.0, 0.5);
    size_t silent_run = 0, longest = 0;
    for (size_t i = 0; i < all.size() / 2; ++i) {
        silent_run = std::abs(all[2 * i]) < 1e-6f ? silent_run + 1 : 0;
        longest = std::max(longest, silent_run);
    }
    EXPECT_LT(longest, 5u) << "no dropouts";
    double best_f = 0, best = 0;
    for (double f = 430; f <= 450; f += 0.25) {
        const double l = tone_level(all, f);
        if (l > best) {
            best = l;
            best_f = f;
        }
    }
    EXPECT_GT(best, 0.08) << "tone at " << best_f << " Hz";
    EXPECT_NEAR(best_f, 440.0, 1.0);

    mixit_input_stats is{};
    is.struct_size = sizeof(is);
    ASSERT_EQ(mixit_session_get_input_stats(r.session, mic, &is), MIXIT_OK);
    EXPECT_EQ(is.decode_errors, 0u);
    EXPECT_LT(is.audio_underrun_frames, 480u * 3);
    EXPECT_EQ(is.frames_produced, 0u) << "audio-only input";
}

TEST(AudioDevices, EnumerationAndPlatformSupport) {
    Rig r;
    uint32_t count = 77;
#if defined(_WIN32)
    const mixit_result e = mixit_enumerate_audio_devices(r.engine, MIXIT_AUDIO_DEVICE_RENDER, nullptr, 0, &count);
    EXPECT_TRUE(e == MIXIT_OK || e == MIXIT_E_NOT_SUPPORTED) << last_error();
    if (e == MIXIT_OK && count > 0) {
        std::vector<mixit_audio_device_info> d(count);
        for (auto& x : d) x.struct_size = sizeof(x);
        ASSERT_EQ(mixit_enumerate_audio_devices(r.engine, MIXIT_AUDIO_DEVICE_RENDER, d.data(), count, &count), MIXIT_OK);
        EXPECT_NE(d[0].id[0], '\0');
    }
#else
    EXPECT_EQ(mixit_enumerate_audio_devices(r.engine, MIXIT_AUDIO_DEVICE_CAPTURE, nullptr, 0, &count),
              MIXIT_E_NOT_SUPPORTED);
    mixit_audio_output_config oc{};
    oc.struct_size = sizeof(oc);
    EXPECT_EQ(mixit_session_set_audio_output(r.session, &oc), MIXIT_E_NOT_SUPPORTED);
#endif
    EXPECT_EQ(mixit_enumerate_audio_devices(r.engine, 5, nullptr, 0, &count), MIXIT_E_INVALID_ARGUMENT);
}
