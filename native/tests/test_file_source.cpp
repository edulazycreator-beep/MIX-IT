// File inputs (M3) end to end through the C ABI, using media files written
// by the tests themselves (media_file_writer). Real-time runs: tolerant bounds.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "media/test_signal.hpp"
#include "media_file_writer.hpp"
#include "mixit/mixit_engine.h"

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

std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("mixit_test_" + name);
}

struct Fixture {
    mixit_engine engine = nullptr;
    mixit_session session = nullptr;
    uint32_t width, height;

    Fixture(uint32_t w, uint32_t h, uint32_t fps_num, uint32_t fps_den, uint32_t sample_rate = 48000)
        : width(w), height(h) {
        mixit_engine_config ec{};
        ec.struct_size = sizeof(ec);
        ec.api_version = MIXIT_API_VERSION;
        ec.min_log_level = MIXIT_LOG_WARNING;
        EXPECT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK);
        mixit_session_config sc{};
        sc.struct_size = sizeof(sc);
        sc.width = w;
        sc.height = h;
        sc.frame_rate_num = fps_num;
        sc.frame_rate_den = fps_den;
        sc.audio_sample_rate = sample_rate;
        sc.audio_channels = 2;
        EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_OK) << last_error();
    }
    ~Fixture() {
        if (session != nullptr) {
            EXPECT_EQ(mixit_session_destroy(session), MIXIT_OK);
        }
        if (engine != nullptr) {
            EXPECT_EQ(mixit_engine_destroy(engine), MIXIT_OK);
        }
    }

    mixit_result add_file(const std::string& path, bool loop, uint32_t& id) {
        mixit_file_source_config c{};
        c.struct_size = sizeof(c);
        c.flags = loop ? MIXIT_FILE_SOURCE_LOOP : 0;
        c.path = path.c_str();
        return mixit_session_add_file_source(session, &c, &id);
    }

    mixit_input_stats input(uint32_t id) const {
        mixit_input_stats s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_input_stats(session, id, &s), MIXIT_OK);
        return s;
    }

    std::optional<mixit::media::FrameCode> program_code(mixit_video_frame_info* out_info = nullptr) const {
        std::vector<uint8_t> buf(static_cast<std::size_t>(width) * height * 4);
        mixit_video_frame_info info{};
        info.struct_size = sizeof(info);
        if (mixit_session_copy_program_frame(session, buf.data(), static_cast<uint32_t>(buf.size()), width * 4, &info) !=
            MIXIT_OK) {
            return std::nullopt;
        }
        if (out_info != nullptr) *out_info = info;
        return mixit::media::decode_frame_code(buf.data(), width * 4, width, height);
    }
};

#define REQUIRE_FFMPEG()                                                    \
    do {                                                                    \
        if (!ffmpeg_usable()) GTEST_SKIP() << "FFmpeg not usable in this build"; \
    } while (0)

#if MIXIT_HAVE_FFMPEG
std::string make_file(const std::string& name, mixit::testing::TestMediaSpec spec) {
    spec.path = temp_path(name).string();
    std::filesystem::remove(spec.path);
    const std::string err = mixit::testing::write_test_media(spec);
    EXPECT_EQ(err, "") << "writing " << spec.path;
    return spec.path;
}
#endif

}  // namespace

TEST(FileSource, RejectsMissingFilesUrlsAndBadArguments) {
    Fixture fx(320, 180, 30, 1);
    uint32_t id = 99;
    if (!ffmpeg_usable()) {
        EXPECT_EQ(fx.add_file("anything.mkv", false, id), MIXIT_E_NOT_SUPPORTED);
        return;
    }
    EXPECT_EQ(fx.add_file(temp_path("does_not_exist.mkv").string(), false, id), MIXIT_E_NOT_FOUND);
    EXPECT_EQ(id, 0u);

    const std::string text = temp_path("not_media.txt").string();
    std::ofstream(text) << "this is not a media file";
    const mixit_result r = fx.add_file(text, false, id);
    EXPECT_TRUE(r == MIXIT_E_IO || r == MIXIT_E_NOT_SUPPORTED) << mixit_result_name(r);
    std::filesystem::remove(text);

    // Only local files: network/other protocols are refused before any I/O.
    EXPECT_NE(fx.add_file("http://127.0.0.1:9/stream.ts", false, id), MIXIT_OK);

    mixit_file_source_config c{};
    c.struct_size = sizeof(c);
    c.path = nullptr;
    EXPECT_EQ(mixit_session_add_file_source(fx.session, &c, &id), MIXIT_E_INVALID_ARGUMENT);
    c.path = "x";
    c.flags = 0x80;
    EXPECT_EQ(mixit_session_add_file_source(fx.session, &c, &id), MIXIT_E_INVALID_ARGUMENT);
}

#if MIXIT_HAVE_FFMPEG

TEST(FileSource, PlaysFramesInOrderWithAudioVideoSync) {
    REQUIRE_FFMPEG();
    mixit::testing::TestMediaSpec spec;
    spec.frames = 60;  // 2 s
    spec.flash_interval_frames = 15;
    const std::string path = make_file("play.nut", spec);

    Fixture fx(320, 180, 30, 1);
    uint32_t id = 0;
    ASSERT_EQ(fx.add_file(path, false, id), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_set_program_input(fx.session, id), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);

    std::vector<uint32_t> seen;
    for (int i = 0; i < 26; ++i) {  // 2.6 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (auto code = fx.program_code()) seen.push_back(code->sequence_low32);
    }
    ASSERT_GE(seen.size(), 15u);
    for (std::size_t i = 1; i < seen.size(); ++i) EXPECT_GE(seen[i], seen[i - 1]) << "frames out of order";
    EXPECT_EQ(seen.back(), 59u) << "after the end the last frame is held";

    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    const mixit_input_stats in = fx.input(id);
    EXPECT_EQ(in.frames_produced, 60u);
    EXPECT_EQ(in.decode_errors, 0u);
    EXPECT_EQ(in.frames_dropped, 0u);
    EXPECT_EQ(in.frames_pool_exhausted, 0u);
    EXPECT_EQ(in.audio_blocks_dropped, 0u);
    EXPECT_GE(in.av_sync_events, 3u);
    EXPECT_LE(in.av_offset_max_abs_ns, 10'417) << "file A/V sync worse than half a sample";
    std::filesystem::remove(path);
}

// Long-run lock across loops: the loop offset is derived from the audio
// sample index, so flash/click alignment must not degrade with each pass.
TEST(FileSource, LoopsKeepAudioAndVideoLocked) {
    REQUIRE_FFMPEG();
    mixit::testing::TestMediaSpec spec;
    spec.frames = 15;  // 0.5 s per pass
    spec.flash_interval_frames = 5;
    const std::string path = make_file("loop.nut", spec);

    Fixture fx(320, 180, 30, 1);
    uint32_t id = 0;
    ASSERT_EQ(fx.add_file(path, true, id), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_set_program_input(fx.session, id), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(3200));
    const auto code = fx.program_code();
    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);

    const mixit_input_stats in = fx.input(id);
    EXPECT_GE(in.loops, 5u);
    EXPECT_GE(in.frames_produced, 85u);
    EXPECT_GE(in.av_sync_events, 15u);
    EXPECT_LE(in.av_offset_max_abs_ns, 10'417) << "A/V drifted across loops";
    EXPECT_EQ(in.frames_dropped, 0u);
    ASSERT_TRUE(code.has_value());
    EXPECT_LT(code->sequence_low32, 15u) << "frame codes repeat each loop";
    std::filesystem::remove(path);
}

TEST(FileSource, ResamplesAudioToTheSessionRate) {
    REQUIRE_FFMPEG();
    mixit::testing::TestMediaSpec spec;  // 48 kHz file
    spec.frames = 60;
    const std::string path = make_file("resample.nut", spec);

    Fixture fx(320, 180, 30, 1, 44100);  // 44.1 kHz session
    uint32_t id = 0;
    ASSERT_EQ(fx.add_file(path, false, id), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(2400));
    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    const mixit_input_stats in = fx.input(id);
    // 2 s of audio in 10 ms blocks at the session rate (+ the padded tail block).
    EXPECT_GE(in.audio_blocks_produced, 199u);
    EXPECT_LE(in.audio_blocks_produced, 201u);
    EXPECT_EQ(in.audio_blocks_dropped, 0u);
    std::filesystem::remove(path);
}

TEST(FileSource, DecodesCompressedMp4WithColourConversion) {
    REQUIRE_FFMPEG();
    mixit::testing::TestMediaSpec spec;
    spec.container = "mp4";
    spec.compressed = true;  // MPEG-4 Part 2 (yuv420p, BT.709) + AAC
    spec.width = 640;
    spec.height = 360;
    spec.frames = 45;
    const std::string path = make_file("compressed.mp4", spec);

    Fixture fx(640, 360, 30, 1);
    uint32_t id = 0;
    ASSERT_EQ(fx.add_file(path, false, id), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_set_program_input(fx.session, id), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    int decoded = 0;
    for (int i = 0; i < 18; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (fx.program_code().has_value()) ++decoded;
    }
    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    const mixit_input_stats in = fx.input(id);
    EXPECT_EQ(in.frames_produced, 45u);
    EXPECT_EQ(in.decode_errors, 0u);
    EXPECT_GE(decoded, 10) << "frame codes must survive YUV conversion and lossy coding";
    EXPECT_GT(in.audio_blocks_produced, 140u);
    std::filesystem::remove(path);
}

TEST(FileSource, AudioOnlyAndVideoOnlyFiles) {
    REQUIRE_FFMPEG();
    mixit::testing::TestMediaSpec audio_only;
    audio_only.video = false;
    const std::string a = make_file("audio_only.nut", audio_only);
    mixit::testing::TestMediaSpec video_only;
    video_only.audio = false;
    const std::string v = make_file("video_only.nut", video_only);

    Fixture fx(320, 180, 30, 1);
    uint32_t ida = 0, idv = 0;
    ASSERT_EQ(fx.add_file(a, false, ida), MIXIT_OK) << last_error();
    ASSERT_EQ(fx.add_file(v, false, idv), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_set_program_input(fx.session, ida), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    std::this_thread::sleep_for(std::chrono::milliseconds(1400));
    mixit_video_frame_info info{};
    (void)fx.program_code(&info);
    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    EXPECT_EQ(info.input_id, 0u) << "an audio-only program input shows black";
    EXPECT_EQ(fx.input(ida).frames_produced, 0u);
    EXPECT_GE(fx.input(ida).audio_blocks_produced, 99u);
    EXPECT_EQ(fx.input(idv).frames_produced, 30u);
    EXPECT_EQ(fx.input(idv).audio_blocks_produced, 0u);
    std::filesystem::remove(a);
    std::filesystem::remove(v);
}

TEST(FileSource, RestartAfterStopPlaysFromTheBeginning) {
    REQUIRE_FFMPEG();
    mixit::testing::TestMediaSpec spec;
    spec.frames = 90;  // 3 s; we stop early
    const std::string path = make_file("restart.nut", spec);
    Fixture fx(320, 180, 30, 1);
    uint32_t id = 0;
    ASSERT_EQ(fx.add_file(path, false, id), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_program_input(fx.session, id), MIXIT_OK);
    for (int run = 0; run < 2; ++run) {
        ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const auto code = fx.program_code();
        ASSERT_TRUE(code.has_value()) << "run " << run;
        EXPECT_LT(code->sequence_low32, 20u) << "run " << run << " must restart at the beginning";
        ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    }
    std::filesystem::remove(path);
}

#endif  // MIXIT_HAVE_FFMPEG
