// End-to-end session tests through the public C ABI. These run the real
// paced pipeline for short periods, so timing assertions are tolerant
// (sanitizer builds run several times slower).
#include <gtest/gtest.h>

#include <chrono>
#include <thread>
#include <vector>

#include "media/test_signal.hpp"
#include "mixit/mixit_engine.h"

namespace {

std::string last_error() {
    char buf[1024];
    (void)mixit_get_last_error_message(buf, sizeof(buf), nullptr);
    return buf;
}

struct Fixture {
    mixit_engine engine = nullptr;
    mixit_session session = nullptr;

    explicit Fixture(uint32_t w = 640, uint32_t h = 360, uint32_t fps_num = 30, uint32_t fps_den = 1) {
        mixit_engine_config ec{};
        ec.struct_size = sizeof(ec);
        ec.api_version = MIXIT_API_VERSION;
        ec.min_log_level = MIXIT_LOG_WARNING;
        EXPECT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK) << last_error();
        mixit_session_config sc{};
        sc.struct_size = sizeof(sc);
        sc.width = w;
        sc.height = h;
        sc.frame_rate_num = fps_num;
        sc.frame_rate_den = fps_den;
        sc.audio_sample_rate = 48000;
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

    uint32_t add_source(uint32_t w, uint32_t h, uint32_t num, uint32_t den, uint32_t flash = 0) {
        mixit_test_source_config c{};
        c.struct_size = sizeof(c);
        c.width = w;
        c.height = h;
        c.frame_rate_num = num;
        c.frame_rate_den = den;
        c.tone_hz = 1000;
        c.flash_interval_frames = flash;
        uint32_t id = 0;
        EXPECT_EQ(mixit_session_add_test_source(session, &c, &id), MIXIT_OK) << last_error();
        return id;
    }

    mixit_session_stats stats() const {
        mixit_session_stats s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_stats(session, &s), MIXIT_OK);
        return s;
    }

    mixit_input_stats input(uint32_t id) const {
        mixit_input_stats s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_input_stats(session, id, &s), MIXIT_OK);
        return s;
    }

    // Copies the program frame and decodes its test-signal frame code.
    std::optional<mixit::media::FrameCode> program_code(mixit_video_frame_info& info) const {
        info = {};
        info.struct_size = sizeof(info);
        std::vector<uint8_t> buf(640 * 360 * 4);
        const mixit_result r =
            mixit_session_copy_program_frame(session, buf.data(), static_cast<uint32_t>(buf.size()), 640 * 4, &info);
        if (r != MIXIT_OK) return std::nullopt;
        return mixit::media::decode_frame_code(buf.data(), 640 * 4, info.width, info.height);
    }
};

void run_for(std::chrono::milliseconds d) { std::this_thread::sleep_for(d); }

}  // namespace

TEST(Session, ValidatesConfiguration) {
    mixit_engine_config ec{};
    ec.struct_size = sizeof(ec);
    ec.api_version = MIXIT_API_VERSION;
    mixit_engine e = nullptr;
    ASSERT_EQ(mixit_engine_create(&ec, &e), MIXIT_OK);

    mixit_session_config sc{};
    sc.struct_size = sizeof(sc);
    sc.width = 1920;
    sc.height = 1080;
    sc.frame_rate_num = 29;
    sc.frame_rate_den = 1;
    sc.audio_sample_rate = 48000;
    sc.audio_channels = 2;
    mixit_session s = reinterpret_cast<mixit_session>(0x1);
    EXPECT_EQ(mixit_session_create(e, &sc, &s), MIXIT_E_NOT_SUPPORTED);
    EXPECT_EQ(s, nullptr);
    EXPECT_NE(last_error().find("29"), std::string::npos) << last_error();
    sc.frame_rate_num = 30000;
    sc.frame_rate_den = 1001;
    sc.width = 1919;
    EXPECT_EQ(mixit_session_create(e, &sc, &s), MIXIT_E_INVALID_ARGUMENT);
    sc.width = 1920;
    sc.audio_sample_rate = 32000;
    EXPECT_EQ(mixit_session_create(e, &sc, &s), MIXIT_E_NOT_SUPPORTED);
    sc.audio_sample_rate = 48000;
    EXPECT_EQ(mixit_session_create(nullptr, &sc, &s), MIXIT_E_INVALID_HANDLE);
    sc.struct_size = 8;
    EXPECT_EQ(mixit_session_create(e, &sc, &s), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_engine_destroy(e), MIXIT_OK);
}

TEST(Session, LifecycleAndInputRulesWhileRunning) {
    Fixture fx;
    const uint32_t a = fx.add_source(640, 360, 30, 1);
    EXPECT_EQ(a, 1u);
    mixit_session_stats s = fx.stats();
    EXPECT_EQ(s.state, MIXIT_SESSION_STATE_CONFIGURING);
    EXPECT_EQ(s.input_count, 1u);

    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK) << last_error();
    EXPECT_EQ(mixit_session_start(fx.session), MIXIT_OK) << "idempotent";
    mixit_test_source_config c{};
    c.struct_size = sizeof(c);
    c.width = 640;
    c.height = 360;
    c.frame_rate_num = 30;
    c.frame_rate_den = 1;
    uint32_t id = 0;
    EXPECT_EQ(mixit_session_add_test_source(fx.session, &c, &id), MIXIT_E_INVALID_STATE);
    EXPECT_EQ(mixit_session_remove_input(fx.session, a), MIXIT_E_INVALID_STATE);
    EXPECT_EQ(mixit_session_set_program_input(fx.session, 99), MIXIT_E_NOT_FOUND);

    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    EXPECT_EQ(fx.stats().state, MIXIT_SESSION_STATE_STOPPED);
    EXPECT_EQ(mixit_session_remove_input(fx.session, a), MIXIT_OK);
    EXPECT_EQ(mixit_session_remove_input(fx.session, a), MIXIT_E_NOT_FOUND);
    EXPECT_EQ(mixit_session_start(fx.session), MIXIT_OK) << "a stopped session can restart";
}

TEST(Session, ProgramIsBlackUntilAnInputIsSelected) {
    Fixture fx;
    fx.add_source(640, 360, 30, 1);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    run_for(std::chrono::milliseconds(300));
    mixit_video_frame_info info{};
    EXPECT_FALSE(fx.program_code(info).has_value());
    EXPECT_EQ(info.input_id, 0u);
    EXPECT_EQ(info.source_sequence, -1) << "black frames carry no source";
    const mixit_session_stats s = fx.stats();
    EXPECT_GT(s.ticks, 0u);
    EXPECT_EQ(s.program_black_frames, s.program_frames);
}

TEST(Session, SameRateInputIsPresentedFrameForFrame) {
    Fixture fx;
    const uint32_t a = fx.add_source(640, 360, 30, 1);
    ASSERT_EQ(mixit_session_set_program_input(fx.session, a), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    run_for(std::chrono::milliseconds(1500));

    mixit_video_frame_info info{};
    const auto code = fx.program_code(info);
    ASSERT_TRUE(code.has_value()) << "program frame must carry the input's frame code";
    EXPECT_EQ(code->source_id, a);
    EXPECT_EQ(static_cast<int64_t>(code->sequence_low32), info.source_sequence);
    EXPECT_EQ(info.input_id, a);
    EXPECT_LE(info.source_pts_ns, info.tick_pts_ns) << "never shows a frame from the future";

    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    const mixit_session_stats s = fx.stats();
    const mixit_input_stats in = fx.input(a);
    EXPECT_GE(s.ticks, 30u);
    EXPECT_LE(s.ticks, 48u);
    EXPECT_EQ(in.frames_dropped, 0u);
    EXPECT_EQ(in.frames_pool_exhausted, 0u);
    // Equal rates: (almost) every tick presents a new frame.
    EXPECT_LE(in.frames_repeated + in.frames_skipped, s.ticks / 10 + 2);
    EXPECT_LT(s.video_latency_max_ns, 200'000'000);
}

TEST(Session, FrameSyncRepeatsSlowInputsAndSkipsFastInputs) {
    Fixture fx;  // 30 fps output
    const uint32_t slow = fx.add_source(640, 360, 25, 1);
    const uint32_t fast = fx.add_source(640, 360, 60, 1);
    ASSERT_EQ(mixit_session_set_program_input(fx.session, slow), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    run_for(std::chrono::milliseconds(2000));
    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);

    const mixit_session_stats s = fx.stats();
    const mixit_input_stats sl = fx.input(slow);
    const mixit_input_stats fa = fx.input(fast);
    // 25 -> 30: about 1 repeat per 6 ticks.
    EXPECT_GT(sl.frames_repeated, s.ticks / 12);
    EXPECT_LT(sl.frames_repeated, s.ticks / 3);
    // 60 -> 30: about every other frame is skipped.
    EXPECT_GT(fa.frames_skipped, fa.frames_produced / 3);
    EXPECT_LT(fa.frames_skipped, fa.frames_produced * 2 / 3 + 2);
}

TEST(Session, ScalesInputsToProgramSizeAndCutsBetweenInputs) {
    Fixture fx(1280, 720);
    const uint32_t a = fx.add_source(640, 360, 30, 1);    // upscaled x2
    const uint32_t b = fx.add_source(1280, 720, 30, 1);   // zero copy
    ASSERT_EQ(mixit_session_set_program_input(fx.session, a), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    run_for(std::chrono::milliseconds(500));

    std::vector<uint8_t> buf(1280 * 720 * 4);
    mixit_video_frame_info info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(mixit_session_copy_program_frame(fx.session, buf.data(), static_cast<uint32_t>(buf.size()), 1280 * 4, &info),
              MIXIT_OK);
    auto code = mixit::media::decode_frame_code(buf.data(), 1280 * 4, 1280, 720);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code->source_id, a);

    ASSERT_EQ(mixit_session_set_program_input(fx.session, b), MIXIT_OK);  // cut while running
    run_for(std::chrono::milliseconds(300));
    ASSERT_EQ(mixit_session_copy_program_frame(fx.session, buf.data(), static_cast<uint32_t>(buf.size()), 1280 * 4, &info),
              MIXIT_OK);
    code = mixit::media::decode_frame_code(buf.data(), 1280 * 4, 1280, 720);
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(code->source_id, b);
    EXPECT_EQ(info.input_id, b);

    // Size query and too-small buffers.
    EXPECT_EQ(mixit_session_copy_program_frame(fx.session, nullptr, 0, 0, &info), MIXIT_E_BUFFER_TOO_SMALL);
    EXPECT_EQ(info.width, 1280u);
    EXPECT_EQ(mixit_session_copy_program_frame(fx.session, buf.data(), 100, 1280 * 4, &info), MIXIT_E_BUFFER_TOO_SMALL);
    EXPECT_EQ(mixit_session_copy_program_frame(fx.session, buf.data(), static_cast<uint32_t>(buf.size()), 16, &info),
              MIXIT_E_INVALID_ARGUMENT);
}

// A/V alignment end to end: flash frames and clicks travel through separate
// queues and threads; the session pairs them by timestamp.
TEST(Session, MeasuresAudioVideoAlignment) {
    Fixture fx(640, 360, 30000, 1001);
    const uint32_t a = fx.add_source(640, 360, 30000, 1001, /*flash every*/ 10);
    ASSERT_EQ(mixit_session_set_program_input(fx.session, a), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    run_for(std::chrono::milliseconds(1500));
    ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    const mixit_input_stats in = fx.input(a);
    EXPECT_GE(in.av_sync_events, 3u);
    EXPECT_LE(in.av_offset_max_abs_ns, 10'417) << "flash/click misaligned by more than half a 48 kHz sample";
    EXPECT_GT(in.audio_blocks_consumed, 100u);
    EXPECT_EQ(in.audio_blocks_dropped, 0u);
}

TEST(Session, EngineShutdownClosesSessions) {
    Fixture fx;
    fx.add_source(640, 360, 30, 1);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    run_for(std::chrono::milliseconds(100));
    ASSERT_EQ(mixit_engine_shutdown(fx.engine), MIXIT_OK);
    EXPECT_EQ(fx.stats().state, MIXIT_SESSION_STATE_CLOSED);
    EXPECT_EQ(mixit_session_start(fx.session), MIXIT_E_INVALID_STATE);

    mixit_session_config sc{};
    sc.struct_size = sizeof(sc);
    sc.width = 640;
    sc.height = 360;
    sc.frame_rate_num = 30;
    sc.frame_rate_den = 1;
    sc.audio_sample_rate = 48000;
    sc.audio_channels = 2;
    mixit_session other = nullptr;
    EXPECT_EQ(mixit_session_create(fx.engine, &sc, &other), MIXIT_E_INVALID_STATE);
}

TEST(Session, SessionHandleOutlivesEngineHandle) {
    Fixture fx;
    fx.add_source(640, 360, 30, 1);
    ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
    ASSERT_EQ(mixit_engine_destroy(fx.engine), MIXIT_OK);
    fx.engine = nullptr;
    EXPECT_EQ(fx.stats().state, MIXIT_SESSION_STATE_CLOSED);
    EXPECT_EQ(mixit_session_destroy(fx.session), MIXIT_OK);
    fx.session = nullptr;
    mixit_session_stats s{};
    s.struct_size = sizeof(s);
    EXPECT_EQ(mixit_session_get_stats(reinterpret_cast<mixit_session>(0x77), &s), MIXIT_E_INVALID_HANDLE);
}

TEST(Session, RepeatedStartStopReturnsAllPooledBuffers) {
    Fixture fx;
    const uint32_t a = fx.add_source(640, 360, 60, 1, 5);
    fx.add_source(640, 360, 25, 1);
    ASSERT_EQ(mixit_session_set_program_input(fx.session, a), MIXIT_OK);
    for (int i = 0; i < 5; ++i) {
        ASSERT_EQ(mixit_session_start(fx.session), MIXIT_OK);
        run_for(std::chrono::milliseconds(120));
        mixit_video_frame_info info{};
        (void)fx.program_code(info);
        ASSERT_EQ(mixit_session_stop(fx.session), MIXIT_OK);
    }
    mixit_video_frame_info info{};
    info.struct_size = sizeof(info);
    uint8_t byte = 0;
    EXPECT_EQ(mixit_session_copy_program_frame(fx.session, &byte, 1, 4, &info), MIXIT_E_NOT_FOUND)
        << "stopped sessions release program frames";
}

// Internal API: verify no pooled frame/audio block leaks across runs.
#include "logging/logger.hpp"
#include "media/session.hpp"

TEST(SessionInternal, PoolsAreFullyReturnedAfterStop) {
    std::unique_ptr<mixit::Logger> logger;
    ASSERT_TRUE(mixit::Logger::create(mixit::LoggerOptions{}, logger).is_ok());
    mixit::media::SessionConfig sc;
    sc.width = 1280;
    sc.height = 720;
    sc.frame_rate = {30, 1};
    std::unique_ptr<mixit::media::Session> session;
    ASSERT_TRUE(mixit::media::Session::create(sc, *logger, session).is_ok());
    mixit::media::TestSignalConfig tc;
    tc.width = 640;
    tc.height = 360;
    tc.frame_rate = {60, 1};
    tc.flash_interval_frames = 10;
    std::uint32_t a = 0, b = 0;
    ASSERT_TRUE(session->add_test_source(tc, a).is_ok());
    tc.width = 1280;
    tc.height = 720;
    tc.frame_rate = {25, 1};
    ASSERT_TRUE(session->add_test_source(tc, b).is_ok());
    ASSERT_TRUE(session->set_program_input(a).is_ok());
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(session->start().is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        EXPECT_GT(session->pooled_items_in_use(), 0u);
        ASSERT_TRUE(session->set_program_input(i % 2 == 0 ? b : a).is_ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ASSERT_TRUE(session->stop().is_ok());
        EXPECT_EQ(session->pooled_items_in_use(), 0u) << "run " << i;
    }
    session->close();
    EXPECT_EQ(session->state(), mixit::media::SessionState::Closed);
}

// API 0.3 appended fields to mixit_input_stats. A caller compiled against
// 0.2 passes the old size: it must still work, and nothing may be written
// past the size it declared.
TEST(SessionAbiCompat, InputStatsAcceptsTheApi02StructSize) {
    Fixture fx;
    const uint32_t id = fx.add_source(640, 360, 30, 1);
    mixit_input_stats s{};
    s.struct_size = 112;  // sizeof(mixit_input_stats) in API 0.2
    s.decode_errors = 0xDEADBEEF;
    s.loops = 0xFEEDFACE;
    ASSERT_EQ(mixit_session_get_input_stats(fx.session, id, &s), MIXIT_OK) << last_error();
    EXPECT_EQ(s.input_id, id);
    EXPECT_EQ(s.struct_size, 112u);
    EXPECT_EQ(s.decode_errors, 0xDEADBEEFu) << "wrote past the caller's struct_size";
    EXPECT_EQ(s.loops, 0xFEEDFACEu);
    s.struct_size = 104;
    EXPECT_EQ(mixit_session_get_input_stats(fx.session, id, &s), MIXIT_E_INVALID_ARGUMENT);
}
