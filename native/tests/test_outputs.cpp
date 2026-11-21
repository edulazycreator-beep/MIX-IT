// Outputs (M5): recording and streaming, verified by decoding what was written.
#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "media_file_reader.hpp"
#include "mixit/mixit_engine.h"

namespace fs = std::filesystem;
using mixit::test::DecodedMedia;
using mixit::test::decode_media;

namespace {

std::string last_error() {
    char buf[1024];
    (void)mixit_get_last_error_message(buf, sizeof(buf), nullptr);
    return buf;
}

bool ffmpeg_usable() {
    mixit_media_library_info info{};
    info.struct_size = sizeof(info);
    return mixit_get_media_library_info(&info) == MIXIT_OK && (info.flags & MIXIT_MEDIA_LIBRARY_USABLE) != 0;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

fs::path temp_dir(const std::string& name) {
    const fs::path p = fs::temp_directory_path() / ("mixit_out_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

std::string str(const fs::path& p) { return p.string(); }

struct Studio {
    mixit_engine engine = nullptr;
    mixit_session session = nullptr;
    uint32_t a = 0;

    explicit Studio(uint32_t w = 640, uint32_t h = 360) {
        mixit_engine_config ec{};
        ec.struct_size = sizeof(ec);
        ec.api_version = MIXIT_API_VERSION;
        ec.min_log_level = MIXIT_LOG_WARNING;
        EXPECT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK);
        mixit_session_config sc{};
        sc.struct_size = sizeof(sc);
        sc.width = w;
        sc.height = h;
        sc.frame_rate_num = 30;
        sc.frame_rate_den = 1;
        sc.audio_sample_rate = 48000;
        sc.audio_channels = 2;
        EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_OK) << last_error();
        a = add_source(w, h);
        EXPECT_EQ(mixit_session_set_program_input(session, a), MIXIT_OK);
    }
    ~Studio() {
        if (session != nullptr) {
            EXPECT_EQ(mixit_session_destroy(session), MIXIT_OK);
        }
        if (engine != nullptr) {
            EXPECT_EQ(mixit_engine_destroy(engine), MIXIT_OK);
        }
    }
    uint32_t add_source(uint32_t w, uint32_t h) {
        mixit_test_source_config tc{};
        tc.struct_size = sizeof(tc);
        tc.width = w;
        tc.height = h;
        tc.frame_rate_num = 30;
        tc.frame_rate_den = 1;
        tc.tone_hz = 1000;
        tc.flash_interval_frames = 15;  // flash + click every 0.5 s
        uint32_t id = 0;
        EXPECT_EQ(mixit_session_add_test_source(session, &tc, &id), MIXIT_OK) << last_error();
        return id;
    }
    uint32_t add_output(const std::string& url, uint32_t source = 0, uint32_t segment_seconds = 0, uint32_t flags = 0,
                        const char* format = nullptr) {
        mixit_output_config oc{};
        oc.struct_size = sizeof(oc);
        oc.url = url.c_str();
        oc.format = format;
        oc.source_input = source;
        oc.video_kbps = 1500;
        oc.keyframe_ms = 1000;
        oc.segment_seconds = segment_seconds;
        oc.flags = flags;
        uint32_t id = 0;
        EXPECT_EQ(mixit_session_add_output(session, &oc, &id), MIXIT_OK) << last_error();
        return id;
    }
    mixit_output_stats stats(uint32_t id) const {
        mixit_output_stats s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_output_stats(session, id, &s), MIXIT_OK) << last_error();
        return s;
    }
    // Waits until the output reached `state` (or timeout); returns the stats.
    mixit_output_stats wait_for(uint32_t id, int32_t state, int timeout_ms = 15000) const {
        mixit_output_stats s = stats(id);
        for (int t = 0; t < timeout_ms && s.state != state; t += 50) {
            sleep_ms(50);
            s = stats(id);
        }
        return s;
    }
};

// Times (s) of flash frames and of click onsets in decoded media.
std::vector<double> flash_times(const DecodedMedia& m) {
    std::vector<double> out;
    for (std::size_t i = 0; i < m.bottom_luma.size(); ++i) {
        const bool on = m.bottom_luma[i] > 180.0;
        const bool before = i > 0 && m.bottom_luma[i - 1] > 180.0;
        if (on && !before) out.push_back(m.frame_times[i]);
    }
    return out;
}
std::vector<double> click_times(const DecodedMedia& m) {
    std::vector<double> out;
    std::size_t quiet = 0;
    for (std::size_t i = 0; i < m.audio.size(); ++i) {
        // The click alternates at the Nyquist rate; AAC keeps only part of it,
        // still well above the -20 dBFS tone (0.1).
        if (std::fabs(m.audio[i]) > 0.2f) {
            if (quiet > static_cast<std::size_t>(m.sample_rate / 10)) {
                out.push_back(m.audio_start + static_cast<double>(i) / m.sample_rate);
            }
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    return out;
}

// Largest |flash − nearest click| (s); NaN when nothing matched.
double max_av_offset(const DecodedMedia& m, int* matched) {
    const std::vector<double> flashes = flash_times(m);
    const std::vector<double> clicks = click_times(m);
    double worst = 0.0;
    *matched = 0;
    for (const double f : flashes) {
        double best = 1e9;
        for (const double c : clicks) best = std::min(best, std::fabs(c - f));
        if (best < 0.2) {
            worst = std::max(worst, best);
            ++*matched;
        }
    }
    return worst;
}

}  // namespace

TEST(Outputs, QueryEncoders) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    mixit_video_encoder_info list[16];
    for (auto& e : list) e.struct_size = sizeof(e);
    uint32_t count = 0;
    ASSERT_EQ(mixit_query_video_encoders(list, 16, &count), MIXIT_OK) << last_error();
    ASSERT_GT(count, 0u);
    bool usable = false;
    for (uint32_t i = 0; i < count; ++i) {
        std::printf("encoder %-12s %s %s %s\n", list[i].name, (list[i].flags & MIXIT_ENCODER_HARDWARE) ? "hw" : "sw",
                    (list[i].flags & MIXIT_ENCODER_USABLE) ? "usable" : "-", list[i].error);
        usable = usable || (list[i].flags & MIXIT_ENCODER_USABLE) != 0;
    }
    EXPECT_TRUE(usable) << "no usable H.264 encoder in this FFmpeg build";
}

TEST(Outputs, RecordsTheProgramToMp4InSync) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const fs::path file = temp_dir("mp4") / "program.mp4";
    const uint32_t o = s.add_output(str(file));
    EXPECT_EQ(mixit_session_start_output(s.session, o), MIXIT_E_INVALID_STATE) << "session not running";
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    EXPECT_EQ(mixit_session_remove_output(s.session, o), MIXIT_E_INVALID_STATE) << "running";
    EXPECT_EQ(s.wait_for(o, MIXIT_OUTPUT_RUNNING, 5000).state, MIXIT_OUTPUT_RUNNING);
    sleep_ms(4000);
    ASSERT_EQ(mixit_session_stop_output(s.session, o), MIXIT_OK);
    const mixit_output_stats st = s.wait_for(o, MIXIT_OUTPUT_IDLE);
    ASSERT_EQ(st.state, MIXIT_OUTPUT_IDLE) << st.last_error;
    EXPECT_EQ(st.dropped_packets, 0u);
    EXPECT_EQ(st.flags & MIXIT_OUTPUT_STATS_DATA_LOST, 0u);
    EXPECT_GT(st.video_packets, 100u);
    EXPECT_GT(st.audio_packets, 150u);
    EXPECT_STREQ(st.current_file, str(file).c_str());
    std::printf("encoder %s, %llu bytes, %.2f s\n", st.encoder, static_cast<unsigned long long>(st.bytes_written),
                static_cast<double>(st.duration_ns) / 1e9);

    const DecodedMedia m = decode_media(str(file));
    ASSERT_TRUE(m.error.empty()) << m.error;
    EXPECT_EQ(m.video_codec, "h264");
    EXPECT_EQ(m.audio_codec, "aac");
    EXPECT_EQ(m.width, 640);
    EXPECT_EQ(m.height, 360);
    EXPECT_EQ(m.channels, 2);
    EXPECT_EQ(m.decode_errors, 0);
    ASSERT_GT(m.frame_times.size(), 100u);
    const double span = m.frame_times.back() - m.frame_times.front();
    // Constant 30 fps: every tick is in the file unless the encoder could
    // not keep up (then it is counted, never silent).
    std::printf("frames %zu, encoder drops %llu\n", m.frame_times.size(),
                static_cast<unsigned long long>(st.encoder_dropped_frames));
    EXPECT_NEAR(static_cast<double>(m.frame_times.size() + st.encoder_dropped_frames), span * 30.0 + 1.0, 2.0);
    EXPECT_LE(st.encoder_dropped_frames, 6u);
    EXPECT_NEAR(m.frame_times.front(), 0.0, 0.001) << "starts at 0";
    EXPECT_NEAR(static_cast<double>(m.audio.size()) / 48000.0, span, 0.25) << "audio covers the video";
    int matched = 0;
    const double offset = max_av_offset(m, &matched);
    std::printf("A/V: %d flashes matched, worst offset %.3f ms\nfirst packets: %s\nflashes:", matched, offset * 1000.0,
                m.first_packets.c_str());
    for (double t : flash_times(m)) std::printf(" %.4f", t);
    std::printf("\nclicks:");
    for (double t : click_times(m)) std::printf(" %.4f", t);
    std::printf("\naudio start %.4f\n", m.audio_start);
    EXPECT_GE(matched, 6);
    EXPECT_LT(offset, 0.040) << "flash vs click in the recording";
    // Recording again to the same name never overwrites the first file.
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    sleep_ms(1500);
    ASSERT_EQ(mixit_session_stop_output(s.session, o), MIXIT_OK);
    const mixit_output_stats again = s.wait_for(o, MIXIT_OUTPUT_IDLE);
    EXPECT_STREQ(again.current_file, str(file.parent_path() / "program (2).mp4").c_str());
    EXPECT_TRUE(decode_media(str(file)).error.empty()) << "the first recording is intact";
    EXPECT_EQ(mixit_session_remove_output(s.session, o), MIXIT_OK);
    EXPECT_EQ(mixit_session_get_output_stats(s.session, o, nullptr), MIXIT_E_INVALID_ARGUMENT);
}

TEST(Outputs, SharedEncoderFeedsSegmentedFilesAndMkv) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const fs::path dir = temp_dir("shared");
    const uint32_t mkv = s.add_output(str(dir / "program.mkv"));
    const uint32_t ts = s.add_output(str(dir / "program.ts"), 0, 2);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, mkv), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_start_output(s.session, ts), MIXIT_OK) << last_error();
    sleep_ms(5500);
    // Stopping the session stops (and finalizes) every output.
    ASSERT_EQ(mixit_session_stop(s.session), MIXIT_OK);
    const mixit_output_stats a = s.stats(mkv);
    const mixit_output_stats b = s.stats(ts);
    EXPECT_EQ(a.state, MIXIT_OUTPUT_IDLE) << a.last_error;
    EXPECT_EQ(b.state, MIXIT_OUTPUT_IDLE) << b.last_error;
    EXPECT_STREQ(a.encoder, b.encoder) << "one shared encoder";
    EXPECT_GE(b.segments, 2u);

    const DecodedMedia m = decode_media(str(dir / "program.mkv"));
    ASSERT_TRUE(m.error.empty()) << m.error;
    EXPECT_EQ(m.decode_errors, 0);
    EXPECT_GT(m.frame_times.size(), 140u);
    for (uint32_t i = 1; i <= b.segments; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "program_%04u.ts", i);
        const DecodedMedia seg = decode_media(str(dir / name));
        ASSERT_TRUE(seg.error.empty()) << name << ": " << seg.error;
        EXPECT_EQ(seg.video_codec, "h264") << name;
        EXPECT_EQ(seg.decode_errors, 0) << name << ": every segment decodes from its start";
        EXPECT_GT(seg.frame_times.size(), 20u) << name;
        EXPECT_GT(seg.audio.size(), 20000u) << name;
    }
}

TEST(Outputs, StreamsRtmpToALocalServer) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const std::string url = "rtmp://127.0.0.1:19361/live/mixit";
    DecodedMedia received;
    std::thread server([&] { received = decode_media(url, "listen=1;timeout=15", 3.0, 20.0); });
    sleep_ms(500);
    const uint32_t o = s.add_output(url);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    server.join();
    const mixit_output_stats st = s.stats(o);
    ASSERT_EQ(mixit_session_stop_output(s.session, o), MIXIT_OK);
    ASSERT_TRUE(received.error.empty()) << received.error << " / output: " << st.last_error;
    EXPECT_EQ(received.format, "flv");
    EXPECT_EQ(received.video_codec, "h264");
    EXPECT_EQ(received.audio_codec, "aac");
    EXPECT_EQ(received.decode_errors, 0);
    EXPECT_GT(received.frame_times.size(), 80u);
    EXPECT_GT(received.audio.size(), 100000u);
}

TEST(Outputs, StreamsSrtToALocalListener) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    DecodedMedia received;
    std::thread server([&] { received = decode_media("srt://127.0.0.1:19362?mode=listener", "", 3.0, 20.0); });
    sleep_ms(500);
    const uint32_t o = s.add_output("srt://127.0.0.1:19362?mode=caller");
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    server.join();
    const mixit_output_stats st = s.stats(o);
    ASSERT_EQ(mixit_session_stop_output(s.session, o), MIXIT_OK);
    if (received.error.find("Protocol not found") != std::string::npos) GTEST_SKIP() << "FFmpeg built without SRT";
    ASSERT_TRUE(received.error.empty()) << received.error << " / output: " << st.last_error;
    EXPECT_EQ(received.format, "mpegts");
    EXPECT_EQ(received.video_codec, "h264");
    EXPECT_EQ(received.audio_codec, "aac");
    EXPECT_EQ(received.decode_errors, 0) << "decodable from the first packet received (SPS/PPS in band)";
    EXPECT_GT(received.frame_times.size(), 60u);
}

TEST(Outputs, ABrokenDestinationDoesNotDisturbTheOthers) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const fs::path file = temp_dir("isolation") / "program.mkv";
    const uint32_t bad = s.add_output("rtmp://127.0.0.1:1/live/nothing-listens-here");
    const uint32_t good = s.add_output(str(file));
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, bad), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_start_output(s.session, good), MIXIT_OK) << last_error();
    // A refused connection takes ~2 s to fail on Windows (SYN retries).
    const auto t0 = std::chrono::steady_clock::now();
    const mixit_output_stats b = s.wait_for(bad, MIXIT_OUTPUT_RECONNECTING, 15000);
    std::this_thread::sleep_until(t0 + std::chrono::seconds(4));
    EXPECT_EQ(b.state, MIXIT_OUTPUT_RECONNECTING);
    EXPECT_GE(b.reconnects, 1u);
    EXPECT_NE(std::string(b.last_error), "");
    ASSERT_EQ(mixit_session_stop_output(s.session, good), MIXIT_OK);
    const mixit_output_stats g = s.wait_for(good, MIXIT_OUTPUT_IDLE);
    EXPECT_EQ(g.state, MIXIT_OUTPUT_IDLE) << g.last_error;
    EXPECT_EQ(g.dropped_packets, 0u);
    ASSERT_EQ(mixit_session_stop_output(s.session, bad), MIXIT_OK);
    EXPECT_EQ(s.wait_for(bad, MIXIT_OUTPUT_IDLE, 5000).state, MIXIT_OUTPUT_IDLE) << "a retrying output stops promptly";
    const DecodedMedia m = decode_media(str(file));
    ASSERT_TRUE(m.error.empty()) << m.error;
    EXPECT_GT(m.frame_times.size(), 100u);
    EXPECT_EQ(m.decode_errors, 0);
    mixit_session_stats ss{};
    ss.struct_size = sizeof(ss);
    ASSERT_EQ(mixit_session_get_stats(s.session, &ss), MIXIT_OK);
    EXPECT_LT(ss.late_ticks, 10u) << "the session kept its clock";
}

TEST(Outputs, HlsToAFolder) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const fs::path dir = temp_dir("hls");
    const uint32_t o = s.add_output(str(dir / "live.m3u8"));
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    sleep_ms(4500);
    ASSERT_TRUE(fs::exists(dir / "live.m3u8"));
    int segments = 0;
    for (const auto& e : fs::directory_iterator(dir)) segments += e.path().extension() == ".ts" ? 1 : 0;
    EXPECT_GE(segments, 2);
    ASSERT_EQ(mixit_session_stop(s.session), MIXIT_OK);
    const DecodedMedia m = decode_media(str(dir / "live.m3u8"));
    ASSERT_TRUE(m.error.empty()) << m.error;
    EXPECT_EQ(m.decode_errors, 0);
    EXPECT_GT(m.frame_times.size(), 60u);
}

TEST(Outputs, IsoRecordsAnInputAtItsOwnSize) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const uint32_t b = s.add_source(320, 180);
    const fs::path file = temp_dir("iso") / "camera2.mp4";
    const uint32_t o = s.add_output(str(file), b);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    sleep_ms(3000);
    ASSERT_EQ(mixit_session_stop(s.session), MIXIT_OK);
    const DecodedMedia m = decode_media(str(file));
    ASSERT_TRUE(m.error.empty()) << m.error;
    EXPECT_EQ(m.width, 320) << "the input's own size, not the program's";
    EXPECT_EQ(m.height, 180);
    EXPECT_EQ(m.decode_errors, 0);
    EXPECT_GT(m.frame_times.size(), 60u);
    int matched = 0;
    const double offset = max_av_offset(m, &matched);
    EXPECT_GE(matched, 3);
    EXPECT_LT(offset, 0.040);
}

TEST(Outputs, Validation) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    mixit_output_config oc{};
    oc.struct_size = sizeof(oc);
    uint32_t id = 0;
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_INVALID_ARGUMENT) << "no url";
    oc.url = "recording.unknown";
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_INVALID_ARGUMENT) << "format unknown";
    oc.format = "avi";
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_INVALID_ARGUMENT);
    oc.format = nullptr;
    oc.url = "rtmp://example.invalid/live";
    oc.segment_seconds = 10;
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_INVALID_ARGUMENT) << "segments are for files";
    oc.segment_seconds = 0;
    oc.video_kbps = 50;
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_INVALID_ARGUMENT);
    oc.video_kbps = 0;
    oc.source_input = 99;
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_NOT_FOUND);
    oc.source_input = 0;
    oc.flags = 0x80;
    EXPECT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_E_INVALID_ARGUMENT);
    oc.flags = 0;
    const std::string path = str(temp_dir("validation") / "x.mp4");
    oc.url = path.c_str();
    oc.video_encoder = "no_such_encoder";
    ASSERT_EQ(mixit_session_add_output(s.session, &oc, &id), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    EXPECT_EQ(mixit_session_start_output(s.session, id), MIXIT_E_NOT_FOUND);
    EXPECT_EQ(s.stats(id).state, MIXIT_OUTPUT_FAILED);
    EXPECT_EQ(mixit_session_start_output(s.session, 4242), MIXIT_E_NOT_FOUND);
    EXPECT_EQ(mixit_session_remove_output(s.session, id), MIXIT_OK);
}

#include "output/output.hpp"

TEST(Outputs, RedactsStreamKeysAndPasswords) {
    using mixit::output::redact_url;
    EXPECT_EQ(redact_url("rtmp://a.rtmp.youtube.com/live2/abcd-efgh-ijkl"), "rtmp://a.rtmp.youtube.com/live2/***");
    EXPECT_EQ(redact_url("rtmps://live-api-s.facebook.com:443/rtmp/FB-123?s_bl=1&a=xyz"),
              "rtmps://live-api-s.facebook.com:443/rtmp/***?s_bl=***&a=***");
    EXPECT_EQ(redact_url("srt://host:9000?mode=caller&passphrase=secret123&streamid=key"),
              "srt://host:9000?mode=***&passphrase=***&streamid=***");
    EXPECT_EQ(redact_url("rtmp://user:pass@host/app/key"), "rtmp://***@host/app/***");
    EXPECT_EQ(redact_url("C:\\Recordings\\show.mp4"), "C:\\Recordings\\show.mp4");
}

TEST(Outputs, RecordsTheMultiview) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    const fs::path file = temp_dir("multiview") / "multiview.mkv";
    const uint32_t o = s.add_output(str(file), MIXIT_OUTPUT_SOURCE_MULTIVIEW);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    sleep_ms(2500);
    const mixit_output_stats st = s.stats(o);
    mixit_session_stats ss{};
    ss.struct_size = sizeof(ss);
    ASSERT_EQ(mixit_session_get_stats(s.session, &ss), MIXIT_OK);
    ASSERT_EQ(mixit_session_stop(s.session), MIXIT_OK);
    const DecodedMedia m = decode_media(str(file));
    ASSERT_TRUE(m.error.empty()) << m.error;
    EXPECT_EQ(m.width, 640);
    EXPECT_EQ(m.decode_errors, 0);
    // Slow (sanitizer) builds cannot render the multiview at 30 fps: the session
    // skips ticks (counted) and the encoder may drop frames (counted).
    EXPECT_GT(m.frame_times.size() + st.encoder_dropped_frames + ss.skipped_ticks, 50u)
        << "skipped ticks " << ss.skipped_ticks << ", late " << ss.late_ticks;
    EXPECT_GT(m.frame_times.size(), 10u);
    EXPECT_GT(m.audio.size(), 48000u) << "with program audio";
}

// Instant replay (M6): record an input, play it back in slow motion on the
// program and verify the flash cadence (every 0.5 s live) doubles to 1 s.
TEST(Replay, SlowMotionPlaybackOfARecordedInput) {
    if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not available";
    Studio s;
    mixit_replay_buffer_info bi{};
    bi.struct_size = sizeof(bi);
    uint32_t r = 0;
    EXPECT_EQ(mixit_session_add_replay_source(s.session, s.a, &r), MIXIT_E_INVALID_STATE) << "replay not enabled yet";
    EXPECT_EQ(mixit_session_get_replay_buffer(s.session, s.a, &bi), MIXIT_E_INVALID_STATE);
    EXPECT_EQ(mixit_session_enable_replay(s.session, s.a, 2, 4), MIXIT_E_INVALID_ARGUMENT) << "too short";
    EXPECT_EQ(mixit_session_enable_replay(s.session, s.a, 30, 40), MIXIT_E_INVALID_ARGUMENT) << "bad quality";
    EXPECT_EQ(mixit_session_enable_replay(s.session, 999, 30, 4), MIXIT_E_NOT_FOUND);
    ASSERT_EQ(mixit_session_enable_replay(s.session, s.a, 30, 4), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_add_replay_source(s.session, s.a, &r), MIXIT_OK) << last_error();
    EXPECT_EQ(mixit_session_enable_replay(s.session, r, 30, 4), MIXIT_E_INVALID_ARGUMENT) << "replay of a replay";
    EXPECT_EQ(mixit_session_replay_play(s.session, s.a, 0, 1.0f, 0), MIXIT_E_INVALID_ARGUMENT) << "not a replay input";
    EXPECT_EQ(mixit_session_replay_play(s.session, r, 0, 3.0f, 0), MIXIT_E_INVALID_ARGUMENT) << "speed out of range";

    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    sleep_ms(3000);
    ASSERT_EQ(mixit_session_get_replay_buffer(s.session, s.a, &bi), MIXIT_OK) << last_error();
    EXPECT_EQ(bi.width, 640u);
    EXPECT_GT(bi.frames, 60u);
    EXPECT_GT(bi.bytes, 0u);
    EXPECT_GT(bi.newest_ns - bi.oldest_ns, 2'000'000'000);
    uint32_t late = 0;
    EXPECT_EQ(mixit_session_add_replay_source(s.session, s.a, &late), MIXIT_E_INVALID_STATE) << "only while stopped";

    const fs::path file = temp_dir("replay") / "replay.mkv";
    const uint32_t o = s.add_output(str(file));
    ASSERT_EQ(mixit_session_replay_play(s.session, r, bi.oldest_ns, 0.5f, 0), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_set_program_input(s.session, r), MIXIT_OK);
    sleep_ms(200);
    ASSERT_EQ(mixit_session_start_output(s.session, o), MIXIT_OK) << last_error();
    sleep_ms(3500);

    mixit_replay_state st{};
    st.struct_size = sizeof(st);
    ASSERT_EQ(mixit_session_get_replay_state(s.session, r, &st), MIXIT_OK) << last_error();
    EXPECT_EQ(st.active, 1);
    EXPECT_FLOAT_EQ(st.speed, 0.5f);
    const double advanced = static_cast<double>(st.position_ns - bi.oldest_ns) / 1e9;
    EXPECT_NEAR(advanced, 0.5 * 3.7, 0.3) << "half speed";

    ASSERT_EQ(mixit_session_replay_control(s.session, r, 0.0f, 1), MIXIT_OK);  // pause, keep speed
    ASSERT_EQ(mixit_session_get_replay_state(s.session, r, &st), MIXIT_OK);
    const int64_t held = st.position_ns;
    sleep_ms(300);
    ASSERT_EQ(mixit_session_get_replay_state(s.session, r, &st), MIXIT_OK);
    EXPECT_EQ(st.paused, 1);
    EXPECT_EQ(st.position_ns, held);
    EXPECT_FLOAT_EQ(st.speed, 0.5f);
    ASSERT_EQ(mixit_session_stop(s.session), MIXIT_OK);
    // The recorded input can go while its replay input stays (sanitizers check
    // that the buffer released every frame of the removed input's pool).
    ASSERT_EQ(mixit_session_remove_input(s.session, s.a), MIXIT_OK) << last_error();
    EXPECT_EQ(mixit_session_get_replay_state(s.session, r, &st), MIXIT_OK);

    const DecodedMedia m = decode_media(str(file));
    ASSERT_TRUE(m.error.empty()) << m.error;
    const std::vector<double> flashes = flash_times(m);
    ASSERT_GE(flashes.size(), 3u) << "replayed flashes";
    for (std::size_t i = 1; i + 1 < flashes.size(); ++i) {  // the last may be cut by the pause
        EXPECT_NEAR(flashes[i] - flashes[i - 1], 1.0, 0.15) << "flash " << i;
    }
}
