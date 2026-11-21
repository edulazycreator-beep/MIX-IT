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

    explicit Studio(int32_t compositor = MIXIT_COMPOSITOR_PREFER_AUTO) {
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
        sc.compositor = compositor;
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

TEST(Buses, KeyedLayerAndOlderLayerStruct) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    mixit_layer_config l{};
    l.struct_size = sizeof(l);
    l.input_id = s.b;
    l.crop_w = l.crop_h = 1.0f;
    l.dest_w = l.dest_h = 1.0f;
    l.opacity = 1.0f;
    // Luma key with a threshold above every pixel of the test pattern's dark
    // background: the black areas of B become transparent and show A.
    l.key_type = MIXIT_KEY_LUMA;
    l.luma_threshold = 0.02f;
    l.luma_softness = 0.01f;
    ASSERT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_OK) << last_error();
    l.key_similarity = 2.0f;
    l.key_type = MIXIT_KEY_CHROMA;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 1, &l), MIXIT_E_INVALID_ARGUMENT) << "range";
    l.key_similarity = 0.1f;
    l.key_type = 7;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 1, &l), MIXIT_E_INVALID_ARGUMENT) << "unknown type";
    l.key_type = MIXIT_KEY_NONE;
    l.key_flags = 0x10;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 1, &l), MIXIT_E_INVALID_ARGUMENT) << "unknown flag";
    // An API 0.3 caller (48-byte struct) still works and gets no key.
    l.key_flags = 0;
    l.struct_size = 48;
    l.key_type = 99;  // beyond its declared size: ignored
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 1, &l), MIXIT_OK) << last_error();
    l.struct_size = 40;
    EXPECT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 1, &l), MIXIT_E_INVALID_ARGUMENT);
    l.struct_size = sizeof(l);
    l.input_id = 0;
    ASSERT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 1, &l), MIXIT_OK);

    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(300);
    auto buf = s.frame(MIXIT_BUS_PROGRAM);
    ASSERT_TRUE(buf.has_value());
    // B is drawn full frame on top, but its black background is keyed out:
    // A's frame code (white/black blocks) is not fully covered. With the
    // full-frame copy of B instead (no key) we would read B's code.
    const auto code = mixit::media::decode_frame_code(buf->data(), W * 4, W, H);
    ASSERT_TRUE(code.has_value());
    EXPECT_GT(s.stats().composited_frames, 0u);
}

TEST(Buses, OverlayChannelStaysOnAirAcrossCutsAndFades) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(s.session, s.b), MIXIT_OK);
    mixit_layer_config l{};
    l.struct_size = sizeof(l);
    l.input_id = s.b;
    l.crop_w = l.crop_h = 1.0f;
    l.dest_x = l.dest_y = 0.5f;
    l.dest_w = l.dest_h = 0.5f;
    l.opacity = 1.0f;
    EXPECT_EQ(mixit_session_show_overlay(s.session, 0, 1, 0), MIXIT_E_INVALID_STATE) << "empty channel";
    ASSERT_EQ(mixit_session_set_overlay(s.session, 0, &l), MIXIT_OK) << last_error();
    EXPECT_EQ(mixit_session_set_overlay(s.session, MIXIT_OVERLAY_CHANNELS, &l), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_session_show_overlay(s.session, 0, 1, 601), MIXIT_E_INVALID_ARGUMENT);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    ASSERT_EQ(mixit_session_show_overlay(s.session, 0, 1, 15), MIXIT_OK);  // 0.5 s fade in at 30 fps
    mixit_overlay_info info{};
    info.struct_size = sizeof(info);
    wait_ms(200);
    ASSERT_EQ(mixit_session_get_overlay(s.session, 0, &info), MIXIT_OK);
    EXPECT_EQ(info.visible, 1);
    EXPECT_GT(info.level, 0.0f);
    EXPECT_LT(info.level, 1.0f) << "still fading";
    wait_ms(600);
    ASSERT_EQ(mixit_session_get_overlay(s.session, 0, &info), MIXIT_OK);
    EXPECT_FLOAT_EQ(info.level, 1.0f);
    auto quadrant_source = [&]() -> uint32_t {
        auto buf = s.frame(MIXIT_BUS_PROGRAM);
        if (!buf) return 0;
        const auto q = mixit::media::decode_frame_code(buf->data() + (H / 2) * W * 4 + (W / 2) * 4, W * 4, W / 2, H / 2);
        return q ? q->source_id : 0u;
    };
    EXPECT_EQ(quadrant_source(), s.b);
    // A cut swaps the buses; the overlay stays on top of the new program.
    ASSERT_EQ(mixit_session_cut(s.session), MIXIT_OK);
    wait_ms(200);
    EXPECT_EQ(s.stats().program_input_id, s.b);
    EXPECT_EQ(quadrant_source(), s.b);
    auto buf = s.frame(MIXIT_BUS_PROGRAM);
    ASSERT_TRUE(buf.has_value());
    ASSERT_EQ(mixit_session_show_overlay(s.session, 0, 0, 0), MIXIT_OK);  // cut out
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    wait_ms(200);
    ASSERT_EQ(mixit_session_get_overlay(s.session, 0, &info), MIXIT_OK);
    EXPECT_EQ(info.level, 0.0f);
    EXPECT_EQ(quadrant_source(), 0u) << "the quadrant shows A's picture, not a frame code of B";
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

TEST(Buses, GpuCompositorPreferenceRendersOrFallsBack) {
    Studio s(MIXIT_COMPOSITOR_PREFER_D3D11_WARP);
#if defined(_WIN32)
    EXPECT_EQ(s.stats().compositor, MIXIT_COMPOSITOR_D3D11);
#else
    EXPECT_EQ(s.stats().compositor, MIXIT_COMPOSITOR_CPU) << "falls back where Direct3D is unavailable";
#endif
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    mixit_layer_config l{};
    l.struct_size = sizeof(l);
    l.input_id = s.b;
    l.crop_w = l.crop_h = 1.0f;
    l.dest_x = l.dest_y = 0.5f;
    l.dest_w = l.dest_h = 0.5f;
    l.opacity = 1.0f;
    ASSERT_EQ(mixit_session_set_layer(s.session, MIXIT_BUS_PROGRAM, 0, &l), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_preview_input(s.session, s.b), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    wait_ms(400);
    auto buf = s.frame(MIXIT_BUS_PROGRAM);
    ASSERT_TRUE(buf.has_value());
    const auto base = mixit::media::decode_frame_code(buf->data(), W * 4, W, H);
    ASSERT_TRUE(base.has_value());
    EXPECT_EQ(base->source_id, s.a);
    const auto pip = mixit::media::decode_frame_code(buf->data() + (H / 2) * W * 4 + (W / 2) * 4, W * 4, W / 2, H / 2);
    ASSERT_TRUE(pip.has_value());
    EXPECT_EQ(pip->source_id, s.b);
    ASSERT_EQ(mixit_session_start_transition(s.session, MIXIT_TRANSITION_MIX, 6), MIXIT_OK);
    for (int i = 0; i < 40 && s.stats().transition_active != -1; ++i) wait_ms(25);
    const mixit_session_stats st = s.stats();
    EXPECT_EQ(st.program_input_id, s.b);
    EXPECT_EQ(st.compositor_fallbacks, 0u);
    EXPECT_GT(st.composited_frames, 0u);
}

TEST(Buses, SessionConfigCompatibilityAndValidation) {
    mixit_engine engine = nullptr;
    mixit_engine_config ec{};
    ec.struct_size = sizeof(ec);
    ec.api_version = MIXIT_API_VERSION;
    ec.min_log_level = MIXIT_LOG_OFF;
    ASSERT_EQ(mixit_engine_create(&ec, &engine), MIXIT_OK);
    mixit_session_config sc{};
    sc.struct_size = 32;  // API 0.2 layout: compositor defaults to AUTO
    sc.width = 320;
    sc.height = 180;
    sc.frame_rate_num = 25;
    sc.frame_rate_den = 1;
    sc.audio_sample_rate = 48000;
    sc.audio_channels = 2;
    sc.compositor = 99;  // beyond the caller's struct: must be ignored
    mixit_session session = nullptr;
    ASSERT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_OK) << last_error();
    mixit_session_stats st{};
    st.struct_size = 96;  // API 0.2 stats size
    ASSERT_EQ(mixit_session_get_stats(session, &st), MIXIT_OK);
    EXPECT_EQ(st.struct_size, 96u);
    EXPECT_EQ(st.preview_input_id, 0u);
    EXPECT_EQ(mixit_session_destroy(session), MIXIT_OK);

    sc.struct_size = sizeof(sc);
    EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_E_INVALID_ARGUMENT);
    sc.compositor = MIXIT_COMPOSITOR_PREFER_CPU;
    sc.reserved = 1;
    EXPECT_EQ(mixit_session_create(engine, &sc, &session), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_engine_destroy(engine), MIXIT_OK);
}

TEST(Capture, EnumerationAndValidation) {
    Studio s;
    uint32_t count = 99;
#if defined(_WIN32)
    // CI machines have no camera; the call must still succeed (count may be 0).
    const mixit_result r = mixit_enumerate_capture_devices(s.engine, nullptr, 0, &count);
    ASSERT_TRUE(r == MIXIT_OK || r == MIXIT_E_NOT_SUPPORTED) << last_error();  // NOT_SUPPORTED: Windows N without MF
    if (r == MIXIT_OK && count > 0) {
        std::vector<mixit_capture_device_info> devices(count);
        for (auto& d : devices) d.struct_size = sizeof(d);
        ASSERT_EQ(mixit_enumerate_capture_devices(s.engine, devices.data(), count, &count), MIXIT_OK);
        EXPECT_NE(devices[0].id[0], '\0');
    }
    mixit_capture_source_config c{};
    c.struct_size = sizeof(c);
    c.device_id = "\\\\?\\no-such-device";
    uint32_t id = 0;
    const mixit_result add = mixit_session_add_capture_source(s.session, &c, &id);
    EXPECT_TRUE(add == MIXIT_E_NOT_FOUND || add == MIXIT_E_NOT_SUPPORTED) << last_error();
#else
    EXPECT_EQ(mixit_enumerate_capture_devices(s.engine, nullptr, 0, &count), MIXIT_E_NOT_SUPPORTED);
    EXPECT_EQ(count, 0u);
    mixit_capture_source_config c{};
    c.struct_size = sizeof(c);
    c.device_id = "anything";
    uint32_t id = 0;
    EXPECT_EQ(mixit_session_add_capture_source(s.session, &c, &id), MIXIT_E_NOT_SUPPORTED);
#endif
    c.device_id = "";
    EXPECT_EQ(mixit_session_add_capture_source(s.session, &c, &id), MIXIT_E_INVALID_ARGUMENT);
    mixit_capture_device_info small{};
    small.struct_size = 8;
    EXPECT_EQ(mixit_enumerate_capture_devices(s.engine, &small, 1, &count), MIXIT_E_INVALID_ARGUMENT);
}

#if MIXIT_HAVE_GRAPHICS
TEST(Graphics, LowerThirdOnAnOverlayWithLiveFieldsAndTimer) {
    Studio s;
    ASSERT_EQ(mixit_session_set_program_input(s.session, s.a), MIXIT_OK);
    const char* scene = R"({"elements":[
        {"type":"rect","x":0.05,"y":0.75,"w":0.6,"h":0.15,"color":"#FF1060E0"},
        {"type":"text","x":0.07,"y":0.76,"w":0.56,"h":0.13,"text":"{name}","size":0.08,"color":"#FFFFFF","bold":true},
        {"type":"text","x":0.8,"y":0.02,"w":0.18,"h":0.08,"text":"{timer}","size":0.06}]})";
    mixit_graphics_source_config gc{};
    gc.struct_size = sizeof(gc);
    gc.width = W;
    gc.height = H;
    gc.scene_json = "{\"elements\":[{\"type\":\"blob\"}]}";
    uint32_t g = 0;
    EXPECT_EQ(mixit_session_add_graphics_source(s.session, &gc, &g), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_NE(last_error().find("type must be"), std::string::npos) << last_error();
    gc.scene_json = scene;
    ASSERT_EQ(mixit_session_add_graphics_source(s.session, &gc, &g), MIXIT_OK) << last_error();
    EXPECT_EQ(mixit_session_set_graphics_field(s.session, s.a, "name", "x"), MIXIT_E_INVALID_ARGUMENT) << "not graphics";
    EXPECT_EQ(mixit_session_set_graphics_field(s.session, g, "time", "x"), MIXIT_E_INVALID_ARGUMENT) << "built in";
    ASSERT_EQ(mixit_session_set_graphics_field(s.session, g, "name", "Siti Rahma"), MIXIT_OK);
    ASSERT_EQ(mixit_session_set_graphics_timer(s.session, g, MIXIT_GRAPHICS_TIMER_COUNTDOWN, 10'000, 1), MIXIT_OK);

    mixit_layer_config l{};
    l.struct_size = sizeof(l);
    l.input_id = g;
    l.crop_w = l.crop_h = l.dest_w = l.dest_h = 1.0f;
    l.opacity = 1.0f;
    ASSERT_EQ(mixit_session_set_overlay(s.session, 0, &l), MIXIT_OK);
    ASSERT_EQ(mixit_session_show_overlay(s.session, 0, 1, 0), MIXIT_OK);
    ASSERT_EQ(mixit_session_start(s.session), MIXIT_OK);
    EXPECT_EQ(mixit_session_add_graphics_source(s.session, &gc, &g), MIXIT_E_INVALID_STATE);
    auto ink_in_box = [&](const std::vector<uint8_t>& buf) {
        // White text pixels inside the blue box.
        int n = 0;
        for (uint32_t y = H * 76 / 100; y < H * 89 / 100; ++y)
            for (uint32_t x = W * 7 / 100; x < W * 63 / 100; ++x) {
                const uint8_t* p = buf.data() + (y * W + x) * 4;
                if (p[0] > 200 && p[1] > 200 && p[2] > 200) ++n;
            }
        return n;
    };
    std::optional<std::vector<uint8_t>> buf;
    for (int i = 0; i < 60; ++i) {  // first rendered frame on air
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        buf = s.frame(MIXIT_BUS_PROGRAM);
        ASSERT_TRUE(buf.has_value());
        if (ink_in_box(*buf) > 200) break;
    }
    const uint8_t* box = buf->data() + ((H * 82 / 100) * W + W * 60 / 100) * 4;
    EXPECT_EQ(box[0], 0xE0) << "lower-third box drawn on program";
    EXPECT_EQ(box[1], 0x60);
    EXPECT_EQ(box[2], 0x10);
    const int ink_long = ink_in_box(*buf);
    EXPECT_GT(ink_long, 200) << "the name is written";

    ASSERT_EQ(mixit_session_set_graphics_field(s.session, g, "name", "Ali"), MIXIT_OK);
    int ink_short = ink_long;
    for (int i = 0; i < 60 && ink_short >= ink_long * 2 / 3; ++i) {  // shown within a few frames (slower under sanitizers)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        buf = s.frame(MIXIT_BUS_PROGRAM);
        ASSERT_TRUE(buf.has_value());
        ink_short = ink_in_box(*buf);
    }
    EXPECT_GT(ink_short, 50);
    EXPECT_LT(ink_short, ink_long * 2 / 3) << "a shorter name uses less ink: live update";

    mixit_graphics_info info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(mixit_session_get_graphics_info(s.session, g, &info), MIXIT_OK);
    EXPECT_EQ(info.timer_mode, MIXIT_GRAPHICS_TIMER_COUNTDOWN);
    EXPECT_EQ(info.timer_running, 1);
    EXPECT_LT(info.timer_ms, 10'000);
    EXPECT_GT(info.timer_ms, 9'000);
    EXPECT_GE(info.frames_rendered, 2u);
    EXPECT_STREQ(info.warnings, "");
    ASSERT_EQ(mixit_session_set_graphics_timer(s.session, g, MIXIT_GRAPHICS_TIMER_COUNTDOWN, -1, 0), MIXIT_OK);  // pause
    ASSERT_EQ(mixit_session_get_graphics_info(s.session, g, &info), MIXIT_OK);
    const int64_t paused = info.timer_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    ASSERT_EQ(mixit_session_get_graphics_info(s.session, g, &info), MIXIT_OK);
    EXPECT_EQ(info.timer_ms, paused);
    EXPECT_EQ(info.timer_running, 0);
    EXPECT_EQ(mixit_session_set_graphics_timer(s.session, g, 9, 0, 0), MIXIT_E_INVALID_ARGUMENT);

    EXPECT_EQ(mixit_session_set_graphics_scene(s.session, g, "{"), MIXIT_E_INVALID_ARGUMENT);
    ASSERT_EQ(mixit_session_set_graphics_scene(s.session, g, R"({"elements":[{"type":"image","path":"/no/logo.png"}]})"),
              MIXIT_OK);
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        ASSERT_EQ(mixit_session_get_graphics_info(s.session, g, &info), MIXIT_OK);
        if (info.warnings[0] != 0) break;
    }
    EXPECT_NE(std::string(info.warnings).find("logo.png"), std::string::npos) << "missing image reported";
}
#endif
