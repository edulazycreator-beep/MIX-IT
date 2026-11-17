// CPU compositor reference behaviour (pixel exact where defined). The GPU
// compositor is checked against these same expectations.
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#include "media/compositor.hpp"
#include "logging/logger.hpp"
#include "media/media_types.hpp"

using namespace mixit::media;
using mixit::LoggerOptions;
using mixit::Status;

namespace {

struct Px {
    int b, g, r, a;
};

Px at(const VideoFrame& f, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = f.data + static_cast<std::size_t>(y) * f.stride + static_cast<std::size_t>(x) * 4;
    return {p[0], p[1], p[2], p[3]};
}

void fill(VideoFrame& f, std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a = 255) {
    for (std::uint32_t y = 0; y < f.height; ++y) {
        for (std::uint32_t x = 0; x < f.width; ++x) {
            std::uint8_t* p = f.data + static_cast<std::size_t>(y) * f.stride + static_cast<std::size_t>(x) * 4;
            p[0] = b;
            p[1] = g;
            p[2] = r;
            p[3] = a;
        }
    }
}

SceneDraw scene(std::initializer_list<LayerDraw> layers) {
    SceneDraw s;
    for (const LayerDraw& l : layers) s.layers[s.count++] = l;
    return s;
}

struct Frames {
    VideoFramePool pool{64, 36, 6};
    VideoRef dst = pool.acquire();
    VideoRef white = pool.acquire();
    VideoRef red = pool.acquire();
    VideoRef black = pool.acquire();
    Frames() {
        fill(*white, 255, 255, 255);
        fill(*red, 0, 0, 255);
        fill(*black, 0, 0, 0);
    }
};

}  // namespace

TEST(Compositor, FitRectLetterboxesAndPillarboxes) {
    EXPECT_EQ(fit_rect(1920, 1080, 1280, 720), NormRect{});
    const NormRect lb = fit_rect(1920, 800, 1920, 1080);  // wider: bars top/bottom
    EXPECT_FLOAT_EQ(lb.x, 0.0f);
    EXPECT_NEAR(lb.h, 800.0f / 1080.0f, 1e-6);
    EXPECT_NEAR(lb.y, (1.0f - lb.h) / 2.0f, 1e-6);
    const NormRect pb = fit_rect(1440, 1080, 1920, 1080);  // 4:3 into 16:9: bars left/right
    EXPECT_NEAR(pb.w, 0.75f, 1e-6);
    EXPECT_NEAR(pb.x, 0.125f, 1e-6);
}

TEST(Compositor, EmptySceneIsOpaqueBlack) {
    Frames f;
    fill(*f.dst, 9, 9, 9, 9);
    auto c = make_cpu_compositor();
    ASSERT_TRUE(c->render(SceneDraw{}, nullptr, TransitionKind::Cut, 0, *f.dst).is_ok());
    const Px p = at(*f.dst, 10, 10);
    EXPECT_EQ(p.b + p.g + p.r, 0);
    EXPECT_EQ(p.a, 255);
}

TEST(Compositor, FullFrameLayerIsAnExactCopy) {
    Frames f;
    for (std::uint32_t y = 0; y < 36; ++y)
        for (std::uint32_t x = 0; x < 64; ++x) f.red->data[y * f.red->stride + x * 4 + 1] = static_cast<std::uint8_t>(x * 4 + y);
    auto c = make_cpu_compositor();
    LayerDraw l;
    l.frame = &*f.red;
    ASSERT_TRUE(c->render(scene({l}), nullptr, TransitionKind::Cut, 0, *f.dst).is_ok());
    for (std::uint32_t y = 0; y < 36; ++y) {
        ASSERT_EQ(std::memcmp(f.dst->data + y * f.dst->stride, f.red->data + y * f.red->stride, 64 * 4), 0) << y;
    }
}

TEST(Compositor, LayersPlaceScaleAndBlend) {
    Frames f;
    auto c = make_cpu_compositor();
    LayerDraw base;
    base.frame = &*f.black;
    LayerDraw pip;
    pip.frame = &*f.red;
    pip.dest = {0.5f, 0.5f, 0.5f, 0.5f};  // bottom-right quadrant
    LayerDraw ghost;
    ghost.frame = &*f.white;
    ghost.dest = {0.0f, 0.0f, 0.25f, 0.25f};
    ghost.opacity = 0.5f;
    ASSERT_TRUE(c->render(scene({base, pip, ghost}), nullptr, TransitionKind::Cut, 0, *f.dst).is_ok());
    EXPECT_EQ(at(*f.dst, 48, 27).r, 255);  // inside PiP
    EXPECT_EQ(at(*f.dst, 48, 27).b, 0);
    EXPECT_EQ(at(*f.dst, 20, 27).r, 0);    // outside PiP
    const Px g = at(*f.dst, 4, 4);         // 50% white over black
    EXPECT_NEAR(g.r, 128, 1);
    EXPECT_NEAR(g.g, 128, 1);
    EXPECT_EQ(g.a, 255);
    EXPECT_EQ(at(*f.dst, 20, 4).r, 0) << "ghost limited to its quarter";
}

TEST(Compositor, SourceAlphaIsRespected) {
    Frames f;
    fill(*f.white, 255, 255, 255, 64);  // 25% alpha
    auto c = make_cpu_compositor();
    LayerDraw base;
    base.frame = &*f.black;
    LayerDraw over;
    over.frame = &*f.white;
    over.dest = {0, 0, 1, 1};
    over.crop = {0, 0, 1, 1};
    over.opacity = 1.0f;
    over.dest.w = 0.999f;  // force the blending path rather than the copy fast path
    ASSERT_TRUE(c->render(scene({base, over}), nullptr, TransitionKind::Cut, 0, *f.dst).is_ok());
    EXPECT_NEAR(at(*f.dst, 10, 10).r, 64, 1);
}

TEST(Compositor, CropSelectsTheSourceRegion) {
    Frames f;
    // Left half red, right half white.
    for (std::uint32_t y = 0; y < 36; ++y)
        for (std::uint32_t x = 32; x < 64; ++x) std::memset(f.red->data + y * f.red->stride + x * 4, 255, 4);
    auto c = make_cpu_compositor();
    LayerDraw l;
    l.frame = &*f.red;
    l.crop = {0.5f, 0.0f, 0.5f, 1.0f};  // right half only, stretched to the full frame
    l.dest = {0, 0, 1, 1};
    ASSERT_TRUE(c->render(scene({l}), nullptr, TransitionKind::Cut, 0, *f.dst).is_ok());
    EXPECT_EQ(at(*f.dst, 1, 10).b, 255) << "left edge must come from the white (right) half";
    EXPECT_EQ(at(*f.dst, 62, 10).b, 255);
}

TEST(Compositor, TransitionsBlendAsSpecified) {
    Frames f;
    auto c = make_cpu_compositor();
    LayerDraw la;
    la.frame = &*f.white;
    LayerDraw lb;
    lb.frame = &*f.red;
    const SceneDraw a = scene({la});
    const SceneDraw b = scene({lb});

    ASSERT_TRUE(c->render(a, &b, TransitionKind::Mix, 0.5f, *f.dst).is_ok());
    Px p = at(*f.dst, 10, 10);
    EXPECT_NEAR(p.r, 255, 1);
    EXPECT_NEAR(p.b, 128, 1);  // white.b 255 -> red.b 0, half way

    ASSERT_TRUE(c->render(a, &b, TransitionKind::Fade, 0.25f, *f.dst).is_ok());
    p = at(*f.dst, 10, 10);
    EXPECT_NEAR(p.b, 128, 1) << "A at 50% towards black";
    ASSERT_TRUE(c->render(a, &b, TransitionKind::Fade, 0.5f, *f.dst).is_ok());
    p = at(*f.dst, 10, 10);
    EXPECT_EQ(p.r + p.g + p.b, 0) << "black at the midpoint";
    ASSERT_TRUE(c->render(a, &b, TransitionKind::Fade, 1.0f, *f.dst).is_ok());
    EXPECT_EQ(at(*f.dst, 10, 10).r, 255);
    EXPECT_EQ(at(*f.dst, 10, 10).b, 0);

    ASSERT_TRUE(c->render(a, &b, TransitionKind::Wipe, 0.5f, *f.dst).is_ok());
    EXPECT_EQ(at(*f.dst, 10, 10).b, 0) << "left half already B (red)";
    EXPECT_EQ(at(*f.dst, 50, 10).b, 255) << "right half still A (white)";

    ASSERT_TRUE(c->render(a, &b, TransitionKind::Mix, 1.0f, *f.dst).is_ok());
    EXPECT_EQ(at(*f.dst, 10, 10).b, 0);
}

TEST(Compositor, DownscaleAveragesBilinearly) {
    Frames f;
    // Vertical stripes 1 px wide alternating black/white; 2:1 downscale
    // lands between texels, producing mid grey.
    for (std::uint32_t y = 0; y < 36; ++y)
        for (std::uint32_t x = 0; x < 64; x += 2) {
            std::memset(f.red->data + y * f.red->stride + x * 4, 0, 3);  // opaque black
            f.red->data[y * f.red->stride + x * 4 + 3] = 255;
        }
    for (std::uint32_t y = 0; y < 36; ++y)
        for (std::uint32_t x = 1; x < 64; x += 2) std::memset(f.red->data + y * f.red->stride + x * 4, 255, 4);
    auto c = make_cpu_compositor();
    LayerDraw l;
    l.frame = &*f.red;
    l.dest = {0, 0, 0.5f, 0.5f};
    ASSERT_TRUE(c->render(scene({l}), nullptr, TransitionKind::Cut, 0, *f.dst).is_ok());
    EXPECT_NEAR(at(*f.dst, 10, 5).g, 128, 2);
}

// ---- D3D11 compositor against the CPU reference (Windows; WARP on CI) ------

namespace {

void gradient(VideoFrame& f, int salt) {
    for (std::uint32_t y = 0; y < f.height; ++y) {
        for (std::uint32_t x = 0; x < f.width; ++x) {
            std::uint8_t* p = f.data + static_cast<std::size_t>(y) * f.stride + static_cast<std::size_t>(x) * 4;
            p[0] = static_cast<std::uint8_t>(x * 255 / (f.width - 1));
            p[1] = static_cast<std::uint8_t>(y * 255 / (f.height - 1));
            p[2] = static_cast<std::uint8_t>((x * 7 + y * 13 + static_cast<std::uint32_t>(salt)) & 0xFF);
            p[3] = 255;
        }
    }
}

}  // namespace

TEST(Compositor, D3D11MatchesTheCpuReference) {
    LoggerOptions lo;
    lo.min_level = MIXIT_LOG_OFF;
    std::unique_ptr<mixit::Logger> logger;
    ASSERT_TRUE(mixit::Logger::create(lo, logger).is_ok());
    std::unique_ptr<Compositor> gpu;
    GpuCompositorOptions options;
    options.software = true;  // WARP: deterministic and present on every Windows 10/11 install
    const Status st = make_d3d11_compositor(options, *logger, gpu);
#if defined(_WIN32)
    ASSERT_TRUE(st.is_ok()) << st.message();
#else
    ASSERT_EQ(st.code(), MIXIT_E_NOT_SUPPORTED);
    GTEST_SKIP() << "Direct3D 11 is Windows only";
#endif
    ASSERT_EQ(gpu->kind(), CompositorKind::D3D11);
    auto cpu = make_cpu_compositor();

    VideoFramePool small(40, 24, 2);  // odd scale factors against the 64x36 output
    VideoRef s1 = small.acquire();
    VideoRef s2 = small.acquire();
    gradient(*s1, 1);
    gradient(*s2, 99);
    for (std::uint32_t y = 0; y < 24; ++y)
        for (std::uint32_t x = 0; x < 40; ++x) s2->data[y * s2->stride + x * 4 + 3] = static_cast<std::uint8_t>(x * 6);

    struct Case {
        const char* name = "";
        SceneDraw a, b;
        bool has_b = false;
        TransitionKind kind = TransitionKind::Cut;
        float progress = 0;
    };
    Frames f;
    gradient(*f.red, 7);
    std::vector<Case> cases;
    {
        Case c;
        c.name = "copy";
        LayerDraw l;
        l.frame = &*f.red;
        c.a = scene({l});
        cases.push_back(c);
    }
    {
        Case c;
        c.name = "upscale+pip+alpha+crop";
        LayerDraw base;
        base.frame = &*s1;
        LayerDraw pip;
        pip.frame = &*s2;
        pip.crop = {0.1f, 0.2f, 0.7f, 0.6f};
        pip.dest = {0.4f, 0.3f, 0.5f, 0.6f};
        pip.opacity = 0.7f;
        c.a = scene({base, pip});
        cases.push_back(c);
    }
    for (TransitionKind k : {TransitionKind::Mix, TransitionKind::Fade, TransitionKind::Wipe}) {
        for (float p : {0.3f, 0.75f}) {
            Case c;
            c.name = "transition";
            LayerDraw la;
            la.frame = &*f.red;
            LayerDraw lb;
            lb.frame = &*s1;
            c.a = scene({la});
            c.b = scene({lb});
            c.has_b = true;
            c.kind = k;
            c.progress = p;
            cases.push_back(c);
        }
    }

    VideoRef out_gpu = f.pool.acquire();
    for (const Case& c : cases) {
        ASSERT_TRUE(cpu->render(c.a, c.has_b ? &c.b : nullptr, c.kind, c.progress, *f.dst).is_ok());
        const Status g = gpu->render(c.a, c.has_b ? &c.b : nullptr, c.kind, c.progress, *out_gpu);
        ASSERT_TRUE(g.is_ok()) << c.name << ": " << g.message();
        int max_diff = 0;
        double sum = 0;
        for (std::uint32_t y = 0; y < 36; ++y) {
            for (std::uint32_t x = 0; x < 64 * 4; ++x) {
                const int d = std::abs(f.dst->data[y * f.dst->stride + x] - out_gpu->data[y * out_gpu->stride + x]);
                max_diff = std::max(max_diff, d);
                sum += d;
            }
        }
        const double mean = sum / (64.0 * 36 * 4);
        // Filtering precision differs (GPU sub-texel weights vs 8-bit fixed point).
        EXPECT_LE(max_diff, 4) << c.name << " kind " << static_cast<int>(c.kind) << " p " << c.progress;
        EXPECT_LT(mean, 1.0) << c.name;
    }
}
