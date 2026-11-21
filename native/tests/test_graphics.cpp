// Graphics (M6): JSON reader, scene validation, placeholder expansion, and
// rendering (shapes, text, images) checked on pixels.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "graphics/graphics.hpp"
#include "graphics/json.hpp"
#include "media/pool.hpp"
#include "mixit/mixit_engine.h"

using namespace mixit::graphics;
using mixit::media::VideoFrame;
using mixit::media::VideoFramePool;
using mixit::media::VideoRef;

namespace {

struct Px {
    int b, g, r, a;
};
Px at(const VideoFrame& f, int x, int y) {
    const std::uint8_t* p = f.data + static_cast<std::size_t>(y) * f.stride + static_cast<std::size_t>(x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

// Opaque pixels in [x0,x1)x[y0,y1) and their mean x.
struct Ink {
    int count = 0;
    double mean_x = 0;
};
Ink ink(const VideoFrame& f, int x0, int y0, int x1, int y1) {
    Ink r;
    double sx = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            if (at(f, x, y).a > 128) {
                ++r.count;
                sx += x;
            }
    r.mean_x = r.count > 0 ? sx / r.count : 0;
    return r;
}

std::string str_scene(const std::string& elements) { return std::string("{\"elements\":[") + elements + "]}"; }

}  // namespace

TEST(Json, ParsesValuesEscapesAndRejectsGarbage) {
    JsonValue v;
    ASSERT_TRUE(parse_json(R"({"a":[1,-2.5e2,true,false,null],"s":"x\n\u00e9\ud83d\ude00\"","o":{}})", v).is_ok());
    ASSERT_TRUE(v.is_object());
    const JsonValue* a = v.find("a");
    ASSERT_NE(a, nullptr);
    ASSERT_EQ(a->array.size(), 5u);
    EXPECT_DOUBLE_EQ(a->array[1].number, -250.0);
    EXPECT_TRUE(a->array[2].boolean);
    EXPECT_EQ(v.string_or("s", ""), "x\n\xC3\xA9\xF0\x9F\x98\x80\"");
    for (const char* bad : {"", "{", "[1,]", "{\"a\":1,}", "\"\\x\"", "01", "1.", "\"\\ud800\"", "{} x", "[\"a\nb\"]"}) {
        EXPECT_FALSE(parse_json(bad, v).is_ok()) << bad;
    }
    std::string deep(40, '[');
    deep += std::string(40, ']');
    EXPECT_FALSE(parse_json(deep, v).is_ok()) << "depth bounded";
}

TEST(GraphicsScene, ValidatesAndCollectsFields) {
    Scene s;
    ASSERT_TRUE(parse_scene(str_scene(R"({"type":"text","text":"{name} – {title} {time} {timer} {{x}"})"), s).is_ok());
    EXPECT_EQ(s.fields, (std::set<std::string>{"name", "title"}));
    EXPECT_TRUE(s.uses_time);
    EXPECT_TRUE(s.uses_timer);
    EXPECT_FALSE(parse_scene("{}", s).is_ok());
    EXPECT_FALSE(parse_scene(str_scene(R"({"type":"circle"})"), s).is_ok());
    EXPECT_FALSE(parse_scene(str_scene(R"({"type":"rect","color":"red"})"), s).is_ok());
    EXPECT_FALSE(parse_scene(str_scene(R"({"type":"rect","x":3})"), s).is_ok());
    EXPECT_FALSE(parse_scene(str_scene(R"({"type":"text","align":"justify"})"), s).is_ok());
    EXPECT_FALSE(parse_scene(str_scene(R"({"type":"image"})"), s).is_ok()) << "image needs a path";

    FieldMap fields{{"name", "Budi"}};
    EXPECT_EQ(expand("Hi {name}, {missing}! {{literal} {time}", fields, DynamicText{"12:00:00", "0:05"}),
              "Hi Budi, ! {literal} 12:00:00");
    EXPECT_EQ(format_timer(65), "1:05");
    EXPECT_EQ(format_timer(3725), "1:02:05");
    EXPECT_EQ(format_timer(-3), "0:00");
}

#if MIXIT_HAVE_GRAPHICS

TEST(GraphicsRender, RoundedRectanglesAndTransparency) {
    std::unique_ptr<Renderer> r;
    ASSERT_TRUE(Renderer::create(r).is_ok());
    VideoFramePool pool(320, 180, 1);
    VideoRef f = pool.acquire();
    Scene s;
    ASSERT_TRUE(parse_scene(str_scene(R"({"type":"rect","x":0.25,"y":0.25,"w":0.5,"h":0.5,"color":"#FF2040C0","radius":0.1})"), s).is_ok());
    std::vector<std::string> warnings;
    ASSERT_TRUE(r->render(s, {}, {}, *f, warnings).is_ok());
    EXPECT_TRUE(warnings.empty());
    EXPECT_EQ(at(*f, 5, 5).a, 0) << "transparent outside";
    const Px c = at(*f, 160, 90);
    EXPECT_EQ(c.a, 255);
    EXPECT_EQ(c.r, 0x20);
    EXPECT_EQ(c.g, 0x40);
    EXPECT_EQ(c.b, 0xC0);
    EXPECT_EQ(at(*f, 81, 46).a, 0) << "rounded corner is cut";
    EXPECT_EQ(at(*f, 160, 46).a, 255) << "straight edge is filled";
}

TEST(GraphicsRender, TextAlignmentFittingKerningAndBold) {
    std::unique_ptr<Renderer> r;
    ASSERT_TRUE(Renderer::create(r).is_ok());
    EXPECT_GT(r->measure("Hello", 40, true), r->measure("Hello", 40, false)) << "bold is wider";
    EXPECT_LT(r->measure("AV", 60, false), r->measure("A", 60, false) + r->measure("V", 60, false)) << "kerning";
    VideoFramePool pool(640, 360, 1);
    VideoRef f = pool.acquire();
    std::vector<std::string> warnings;
    auto render_text = [&](const char* align, const char* text) {
        Scene s;
        const std::string e = std::string(R"({"type":"text","x":0.1,"y":0.4,"w":0.8,"h":0.2,"size":0.1,"align":")") + align +
                              R"(","text":")" + text + R"("})";
        EXPECT_TRUE(parse_scene(str_scene(e), s).is_ok());
        EXPECT_TRUE(r->render(s, {}, {}, *f, warnings).is_ok());
    };
    render_text("left", "MIX");
    const Ink left = ink(*f, 0, 0, 640, 360);
    render_text("center", "MIX");
    const Ink center = ink(*f, 0, 0, 640, 360);
    render_text("right", "MIX");
    const Ink right = ink(*f, 0, 0, 640, 360);
    EXPECT_GT(left.count, 300) << "text drawn";
    EXPECT_NEAR(center.mean_x, 320, 6) << "centered";
    EXPECT_LT(left.mean_x, center.mean_x - 100);
    EXPECT_GT(right.mean_x, center.mean_x + 100);
    EXPECT_EQ(ink(*f, 0, 0, 640, 140).count, 0) << "nothing above the box";
    EXPECT_EQ(ink(*f, 0, 220, 640, 360).count, 0) << "nothing below the box";

    // A long line is shrunk to the box width.
    render_text("left", "The quick brown fox jumps over the lazy dog, twice over");
    EXPECT_EQ(ink(*f, 0, 0, 62, 360).count, 0) << "starts at the box";
    EXPECT_EQ(ink(*f, 580, 0, 640, 360).count, 0) << "fits the box width";
    EXPECT_GT(ink(*f, 64, 0, 576, 360).count, 500);

    // Missing custom font: falls back, with a warning.
    Scene s;
    ASSERT_TRUE(parse_scene(str_scene(R"({"type":"text","text":"x","font":"/no/such/font.ttf"})"), s).is_ok());
    warnings.clear();
    ASSERT_TRUE(r->render(s, {}, {}, *f, warnings).is_ok());
    ASSERT_EQ(warnings.size(), 1u);
    EXPECT_NE(warnings[0].find("font not readable"), std::string::npos);
}

TEST(GraphicsRender, ImagesFromFiles) {
    // A 4x2 24-bit BMP: top row red, bottom row blue.
    const auto path = std::filesystem::temp_directory_path() / "mixit_graphics_test.bmp";
    {
        std::ofstream out(path, std::ios::binary);
        const std::uint8_t header[54] = {'B', 'M', 54 + 24, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 4, 0, 0, 0, 2, 0,
                                         0, 0, 1, 0, 24, 0, 0, 0, 0, 0, 24, 0, 0, 0, 0x13, 0x0B, 0, 0, 0x13, 0x0B, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 0};
        out.write(reinterpret_cast<const char*>(header), 54);
        for (int row = 0; row < 2; ++row) {  // bottom-up: blue row first
            for (int x = 0; x < 4; ++x) {
                const std::uint8_t bgr[3] = {static_cast<std::uint8_t>(row == 0 ? 255 : 0), 0,
                                             static_cast<std::uint8_t>(row == 0 ? 0 : 255)};
                out.write(reinterpret_cast<const char*>(bgr), 3);
            }
        }
    }
    std::unique_ptr<Renderer> r;
    ASSERT_TRUE(Renderer::create(r).is_ok());
    VideoFramePool pool(160, 90, 1);
    VideoRef f = pool.acquire();
    Scene s;
    const std::string p = path.string();
    std::string escaped;
    for (char c : p) escaped += c == '\\' ? std::string("\\\\") : std::string(1, c);
    ASSERT_TRUE(parse_scene(str_scene(R"({"type":"image","x":0,"y":0,"w":1,"h":1,"fit":"stretch","path":")" + escaped + R"("})"), s).is_ok());
    std::vector<std::string> warnings;
    ASSERT_TRUE(r->render(s, {}, {}, *f, warnings).is_ok());
    if (!warnings.empty() && warnings[0].find("FFmpeg") != std::string::npos) GTEST_SKIP() << "images need FFmpeg";
    ASSERT_TRUE(warnings.empty()) << warnings[0];
    EXPECT_GT(at(*f, 80, 5).r, 240) << "top: red";
    EXPECT_GT(at(*f, 80, 85).b, 240) << "bottom: blue";
    EXPECT_EQ(at(*f, 80, 85).a, 255) << "opaque image";
    ASSERT_TRUE(parse_scene(str_scene(R"({"type":"image","path":"/no/such.png"})"), s).is_ok());
    warnings.clear();
    ASSERT_TRUE(r->render(s, {}, {}, *f, warnings).is_ok());
    EXPECT_EQ(warnings.size(), 1u);
}

#endif
