// Preview/program buses, cuts, transitions and layers through the C ABI.
#include <gtest/gtest.h>

#include <chrono>
#include <optional>
#include <thread>
#include <vector>

#include "media/test_signal.hpp"
#include "mixit/mixit_engine.h"

namespace {

constexpr uint32_t W = 640, H = 360;

std::string last_error() {
    char buf[1024];
    (void)mixit_get_last_error_message(buf, sizeof(buf), nullptr);
    return buf;
}

struct Studio {
    mixit_engine engine = nullptr;
    mixit_session session = nullptr;
    uint32_t a = 0, b = 0;

    Studio() {
        mixit_engine_config ec{};
        ec.struct_size = sizeof(ec);
        ec.api_version = MIXIT_API_VERSION;
        ec.min_log_level = MIXIT_LOG_WARNING;
        EXPECT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK);
        mixit_session_config sc{};
        sc.struct_size = sizeof(sc);
        sc.width = W;
        sc.height = H;
        sc.frame_rate_num = 30;
        sc.frame_rate_den = 1;
        sc.audio_sample_rate = 48000;
        sc.audio_channels = 2;
        EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_OK);
        a = add(W, H);
        b = add(W, H);
    }
    ~Studio() {
        if (session != nullptr) {
            EXPECT_EQ(mixit_session_destroy(session), MIXIT_OK);
        }
        if (engine != nullptr) {
            EXPECT_EQ(mixit_engine_destroy(engine), MIXIT_OK);
        }
    }
    uint32_t add(uint32_t w, uint32_t h) {
        mixit_test_source_config c{};
        c.struct_size = sizeof(c);
        c.width = w;
        c.height = h;
        c.frame_rate_num = 30;
        c.frame_rate_den = 1;
        uint32_t id = 0;
        EXPECT_EQ(mixit_session_add_test_source(session, &c, &id), MIXIT_OK);
        return id;
    }
    mixit_session_stats stats() const {
        mixit_session_stats s{};
        s.struct_size = sizeof(s);
        EXPECT_EQ(mixit_session_get_stats(session, &s), MIXIT_OK);
        return s;
    }
    // Copies a bus frame; retries briefly (preview renders on demand).
    std::optional<std::vector<uint8_t>> frame(int32_t bus, mixit_video_frame_info* out = nullptr) const {
        std::vector<uint8_t> buf(W * H * 4);
        mixit_video_frame_info info{};
        info.struct_size = sizeof(info);
        for (int i = 0; i < 20; ++i) {
            const mixit_result r =
                mixit_session_copy_bus_frame(session, bus, buf.data(), static_cast<uint32_t>(buf.size()), W * 4, &info);
            if (r == MIXIT_OK) {
                if (out != nullptr) *out = info;
                return buf;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }
        return std::nullopt;
    }
    std::optional<uint8_t> source_on(int32_t bus) const {
        auto buf = frame(bus);
        if (!buf) return std::nullopt;
        auto code = mixit::media::decode_frame_code(buf->data(), W * 4, W, H);
        if (!code) return std::nullopt;
        return code->source_id;
    }
};

void wait_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST(Buses, PreviewAndProgramShowTheirInputsAndCutSwapsThem) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(s.session, s.b), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(300);
    EXPECT_EQ(s.source_on(MIXIT_BUS_PROGRAM), s.a);
    EXPECT_EQ(s.source_on(MIXIT_BUS_PREVIEW), s.b);

    ASSERT_EQ(mixit_session_cut(s.session), MIXIT_OK);
    wait_ms(150);
    EXPECT_EQ(s.source_on(MIXIT_BUS_PROGRAM), s.b);
    EXPECT_EQ(s.source_on(MIXIT_BUS_PREVIEW), s.a);
    const mixit_session_stats st = s.stats();
    EXPECT_EQ(st.program_input_id, s.b);
    EXPECT_EQ(st.preview_input_id, s.a);
    EXPECT_EQ(st.transitions_completed, 1u);
    EXPECT_EQ(st.compositor, MIXIT_COMPOSITOR_CPU);
}

TEST(Buses, AutoTransitionRunsOverItsDurationThenSwaps) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(s.session, s.b), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(200);
    ASSERT_EQ(mixit_session_start_transition(s.session, MIXIT_TRANSITION_MIX, 30), MIXIT_OK);  // 1 s
    EXPECT_EQ(mixit_session_start_transition(s.session, MIXIT_TRANSITION_MIX, 30), MIXIT_E_INVALID_STATE);
    wait_ms(500);
    mixit_session_stats st = s.stats();
    EXPECT_EQ(st.transition_active, MIXIT_TRANSITION_MIX);
    EXPECT_GT(st.transition_progress, 0.2f);
    EXPECT_LT(st.transition_progress, 0.9f);
    EXPECT_FALSE(s.source_on(MIXIT_BUS_PROGRAM).has_value()) << "mid-dissolve the frame code is a blend";
    for (int i = 0; i < 60 && s.stats().transition_active != -1; ++i) wait_ms(50);  // due at ~1 s
    st = s.stats();
    EXPECT_EQ(st.transition_active, -1);
    EXPECT_EQ(st.program_input_id, s.b);
    EXPECT_EQ(st.preview_input_id, s.a);
    EXPECT_EQ(s.source_on(MIXIT_BUS_PROGRAM), s.b);
    EXPECT_GE(st.composited_frames, 10u) << "the dissolve itself is composited";
}

TEST(Buses, CutCompletesARunningTransition) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(s.session, s.b), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(150);
    ASSERT_EQ(mixit_session_start_transition(s.session, MIXIT_TRANSITION_WIPE, 90), MIXIT_OK);
    wait_ms(200);
    ASSERT_EQ(mixit_session_cut(s.session), MIXIT_OK);
    wait_ms(150);
    const mixit_session_stats st = s.stats();
    EXPECT_EQ(st.transition_active, -1);
    EXPECT_EQ(st.program_input_id, s.b);
    EXPECT_EQ(s.source_on(MIXIT_BUS_PROGRAM), s.b);
}

TEST(Buses, TransitionWhileStoppedLandsOnTheEndState) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(s.session, s.b), MIXIT_OK);
    ASSERT_EQ(mixit_session_start_transition(s.session, MIXIT_TRANSITION_FADE, 20), MIXIT_OK);
    const mixit_session_stats st = s.stats();
    EXPECT_EQ(st.program_input_id, s.b);
    EXPECT_EQ(st.preview_input_id, s.a);
    EXPECT_EQ(mixit_session_start_transition(s.session, 7, 20), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_session_start_transition(s.session, MIXIT_TRANSITION_MIX, 601), MIXIT_E_INVALID_ARGUMENT);
}

TEST(Buses, PictureInPictureLayerShowsTheSecondInput) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    mixit_layer_config l{};
    l.struct_size = sizeof(l);
    l.input_id = s.b;
    l.crop_w = l.crop_h = 1.0f;
    l.dest_x = l.dest_y = 0.5f;
    l.dest_w = l.dest_h = 0.5f;
    l.opacity = 1.0f;
    ASSERT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_OK) << last_error();
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(300);
    auto buf = s.frame(MIXIT_BUS_PROGRAM);
    ASSERT_TRUE(buf.has_value());
    const auto base = mixit::media::decode_frame_code(buf->data(), W * 4, W, H);
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base->source_id, s.a);
    // The PiP quadrant holds input B at half size: decode it in place.
    const uint8_t* quadrant = buf->data() + (H / 2) * W * 4 + (W / 2) * 4;
    const auto pip = mixit::media::decode_frame_code(quadrant, W * 4, W / 2, H / 2);
    ASSERT_TRUE(pip.has_value());
    EXPECT_EQ(pip->source_id, s.b);

    // Disabling the layer returns to the zero-copy path.
    l.input_id = 0;
    ASSERT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_OK);
    wait_ms(150);
    const uint64_t composited = s.stats().composited_frames;
    wait_ms(300);
    EXPECT_EQ(s.stats().composited_frames, composited);
}

TEST(Buses, LayerValidation) {
    Studio s;
    mixit_layer_config l{};
    l.struct_size = sizeof(l);
    l.input_id = s.b;
    l.crop_w = l.crop_h = 1.0f;
    l.dest_w = l.dest_h = 0.5f;
    l.opacity = 1.0f;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, MIXIT_MAX_LAYERS, &l), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_session_set_layer(s.session, 5, 0, &l), MIXIT_E_INVALID_ARGUMENT);
    l.opacity = 1.5f;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_E_INVALID_ARGUMENT);
    l.opacity = 1.0f;
    l.crop_x = 0.8f;  // crop beyond the source
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_E_INVALID_ARGUMENT);
    l.crop_x = 0.0f;
    l.input_id = 77;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_E_NOT_FOUND);
    l.input_id = s.b;
    l.reserved = 1;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_E_INVALID_ARGUMENT);
}

TEST(Buses, AspectRatioIsPreservedWithBars) {
    Studio s;
    const uint32_t square = s.add(360, 360);  // 1:1 into 16:9 -> pillarbox
    ASSERT_EQ(mixit_session_set_program_input(s.session, square), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(300);
    auto buf = s.frame(MIXIT_BUS_PROGRAM);
    ASSERT_TRUE(buf.has_value());
    const uint8_t* left = buf->data() + (H / 2) * W * 4 + 10 * 4;  // inside the left bar
    EXPECT_EQ(left[0] + left[1] + left[2], 0);
    const uint8_t* centre = buf->data() + (H / 2) * W * 4 + (W / 2) * 4;
    EXPECT_GT(centre[0] + centre[1] + centre[2], 0);
}

TEST(Buses, PreviewIsOnlyRenderedWhileWatched) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    const uint32_t odd = s.add(320, 180);  // needs scaling: composited, not zero-copy
    ASSERT_EQ(mixit_session_set_preview_input(s.session, odd), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(500);
    EXPECT_EQ(s.stats().composited_frames, 0u) << "nobody watched preview";
    ASSERT_TRUE(s.frame(MIXIT_BUS_PREVIEW).has_value());
    wait_ms(300);
    EXPECT_GT(s.stats().composited_frames, 0u);
}
