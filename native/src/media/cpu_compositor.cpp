// CPU reference compositor. Conventions shared with the GPU path:
//   * pixel centres at +0.5; bilinear sampling clamped to the crop rectangle;
//   * straight (non-premultiplied) alpha: a = src.alpha/255 * opacity,
//     out.rgb = src.rgb*a + dst.rgb*(1-a), out.alpha = 255;
//   * destination rectangles are rounded to whole pixels.
// Work is split into horizontal bands over a small helper pool; results are
// identical regardless of the number of bands.
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "media/compositor.hpp"
#include "platform/os.hpp"

namespace mixit::media {
namespace {

struct PixelRect {
    int x0, y0, x1, y1;
};

PixelRect to_pixels(const NormRect& r, std::uint32_t w, std::uint32_t h) noexcept {
    auto clampi = [](long v, long lo, long hi) { return static_cast<int>(std::clamp(v, lo, hi)); };
    const long W = static_cast<long>(w), H = static_cast<long>(h);
    return {clampi(std::lround(r.x * static_cast<float>(w)), 0, W), clampi(std::lround(r.y * static_cast<float>(h)), 0, H),
            clampi(std::lround((r.x + r.w) * static_cast<float>(w)), 0, W),
            clampi(std::lround((r.y + r.h) * static_cast<float>(h)), 0, H)};
}

// Fixed set of helper threads that split a frame into horizontal bands. The
// calling (tick) thread always takes band 0, so a pool with no helpers is a
// plain loop. Work items are noexcept and never block.
class BandPool {
public:
    explicit BandPool(unsigned helpers) {
        try {
            for (unsigned i = 0; i < helpers; ++i) threads_.emplace_back([this, i] { worker(i + 1); });
        } catch (...) {
            // Fewer helpers than requested still works; lanes() reflects reality.
        }
    }
    ~BandPool() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        start_.notify_all();
        for (std::thread& t : threads_) t.join();
    }
    BandPool(const BandPool&) = delete;
    BandPool& operator=(const BandPool&) = delete;

    [[nodiscard]] unsigned lanes() const noexcept { return static_cast<unsigned>(threads_.size()) + 1; }

    // Calls fn(lane, begin, end) for disjoint bands covering [0, rows).
    template <typename Fn>
    void run(int rows, Fn&& fn) noexcept {
        const unsigned lanes_used = rows >= kMinRowsPerBand * 2 ? std::min(lanes(), static_cast<unsigned>(rows / kMinRowsPerBand)) : 1;
        if (lanes_used <= 1) {
            fn(0u, 0, rows);
            return;
        }
        {
            std::lock_guard lock(mutex_);
            job_ = [](void* ctx, unsigned lane, int b, int e) noexcept { (*static_cast<Fn*>(ctx))(lane, b, e); };
            ctx_ = &fn;
            rows_ = rows;
            lanes_used_ = lanes_used;
            pending_ = lanes_used - 1;
            ++generation_;
        }
        start_.notify_all();
        fn(0u, 0, band_end(0, rows, lanes_used));
        std::unique_lock lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
    }

private:
    static constexpr int kMinRowsPerBand = 16;

    static int band_end(unsigned lane, int rows, unsigned lanes) noexcept {
        return static_cast<int>(static_cast<long long>(rows) * (lane + 1) / lanes);
    }

    void worker(unsigned lane) noexcept {
        os::set_current_thread_name("mixit-comp");
        std::uint64_t seen = 0;
        std::unique_lock lock(mutex_);
        for (;;) {
            start_.wait(lock, [&] { return stop_ || generation_ != seen; });
            if (stop_) return;
            seen = generation_;
            if (lane >= lanes_used_) continue;
            const auto job = job_;
            void* ctx = ctx_;
            const int rows = rows_;
            const unsigned lanes_used = lanes_used_;
            lock.unlock();
            job(ctx, lane, band_end(lane - 1, rows, lanes_used), band_end(lane, rows, lanes_used));
            lock.lock();
            if (--pending_ == 0) done_.notify_one();
        }
    }

    std::mutex mutex_;
    std::condition_variable start_, done_;
    std::vector<std::thread> threads_;
    void (*job_)(void*, unsigned, int, int) noexcept = nullptr;
    void* ctx_ = nullptr;
    int rows_ = 0;
    unsigned lanes_used_ = 1;
    unsigned pending_ = 0;
    std::uint64_t generation_ = 0;
    bool stop_ = false;
};

void clear_black_rows(VideoFrame& f, int y0, int y1) noexcept {
    for (int y = y0; y < y1; ++y) {
        auto* row = reinterpret_cast<std::uint32_t*>(f.data + static_cast<std::size_t>(y) * f.stride);
        std::fill(row, row + f.width, 0xFF000000u);
    }
}

bool is_plain_copy(const LayerDraw& l, const VideoFrame& dst) noexcept {
    const VideoFrame& s = *l.frame;
    return s.width == dst.width && s.height == dst.height && l.crop == NormRect{} && l.dest == NormRect{} &&
           l.opacity >= 1.0f;
}

// Per-lane scratch: horizontal taps are shared (computed once per layer),
// two horizontally filtered source rows are cached per lane so that each
// source row is filtered once per band instead of twice per output row.
struct LaneScratch {
    std::vector<std::uint16_t> rows[2];
};

struct LayerPlan {
    const VideoFrame* src = nullptr;
    PixelRect d{};
    float cy0 = 0, sy = 0, min_v = 0, max_v = 0;
    int op = 256;
    int dw = 0;
    const int* tu0 = nullptr;
    const int* tu1 = nullptr;
    const int* tfx = nullptr;
};

void filter_row(const LayerPlan& p, int v, std::uint16_t* out) noexcept {
    const std::uint8_t* r = p.src->data + static_cast<std::size_t>(v) * p.src->stride;
    for (int i = 0; i < p.dw; ++i, out += 4) {
        const std::uint8_t* a = r + p.tu0[i];
        const std::uint8_t* b = r + p.tu1[i];
        const int fx = p.tfx[i];
        for (int c = 0; c < 4; ++c) out[c] = static_cast<std::uint16_t>(a[c] * (256 - fx) + b[c] * fx);
    }
}

void draw_band(const LayerPlan& p, VideoFrame& dst, LaneScratch& s, int y0, int y1) noexcept {
    std::uint16_t* ha = s.rows[0].data();
    std::uint16_t* hb = s.rows[1].data();
    int ia = -1, ib = -1;  // source row held by ha / hb
    for (int y = y0; y < y1; ++y) {
        const float v = std::clamp(p.cy0 + (static_cast<float>(y - p.d.y0) + 0.5f) * p.sy - 0.5f, p.min_v, p.max_v);
        const int v0 = static_cast<int>(v);
        const int v1 = std::min(v0 + 1, static_cast<int>(p.max_v));
        const auto fy = static_cast<int>((v - static_cast<float>(v0)) * 256.0f);
        if (v0 == ib) {
            std::swap(ha, hb);
            std::swap(ia, ib);
        }
        if (ia != v0) {
            filter_row(p, v0, ha);
            ia = v0;
        }
        if (ib != v1) {
            if (v1 == v0) {
                std::memcpy(hb, ha, static_cast<std::size_t>(p.dw) * 4 * sizeof(std::uint16_t));
            } else {
                filter_row(p, v1, hb);
            }
            ib = v1;
        }
        std::uint8_t* o = dst.data + static_cast<std::size_t>(y) * dst.stride + static_cast<std::size_t>(p.d.x0) * 4;
        const std::uint16_t* a = ha;
        const std::uint16_t* b = hb;
        for (int i = 0; i < p.dw; ++i, o += 4, a += 4, b += 4) {
            int px[4];
            for (int c = 0; c < 4; ++c) px[c] = (a[c] * (256 - fy) + b[c] * fy + (1 << 15)) >> 16;
            const int alpha = (px[3] * p.op + 127) / 255;  // 0..256
            if (alpha >= 256) {
                o[0] = static_cast<std::uint8_t>(px[0]);
                o[1] = static_cast<std::uint8_t>(px[1]);
                o[2] = static_cast<std::uint8_t>(px[2]);
            } else if (alpha > 0) {
                for (int c = 0; c < 3; ++c) o[c] = static_cast<std::uint8_t>((px[c] * alpha + o[c] * (256 - alpha) + 128) >> 8);
            }
            o[3] = 255;
        }
    }
}

class CpuCompositor final : public Compositor {
public:
    CpuCompositor() : pool_(helper_count()) {}

    [[nodiscard]] std::string name() const override { return "cpu"; }
    [[nodiscard]] CompositorKind kind() const noexcept override { return CompositorKind::Cpu; }

    Status render(const SceneDraw& a, const SceneDraw* b, TransitionKind kind, float progress,
                  VideoFrame& dst) noexcept override {
        if (!render_scene(a, dst)) return {MIXIT_E_OUT_OF_MEMORY, "compositor scratch rows"};
        if (b == nullptr || kind == TransitionKind::Cut) return Status::ok();
        if (!ensure_scratch(dst)) return {MIXIT_E_OUT_OF_MEMORY, "compositor scratch frame"};
        if (!render_scene(*b, scratch_)) return {MIXIT_E_OUT_OF_MEMORY, "compositor scratch rows"};
        blend(dst, scratch_, kind, std::clamp(progress, 0.0f, 1.0f));
        return Status::ok();
    }

private:
    static unsigned helper_count() noexcept {
        const unsigned hc = std::thread::hardware_concurrency();
        return hc <= 1 ? 0u : std::min(hc, 8u) - 1u;
    }

    bool render_scene(const SceneDraw& scene, VideoFrame& dst) noexcept {
        const std::uint32_t n = std::min<std::uint32_t>(scene.count, static_cast<std::uint32_t>(scene.layers.size()));
        const int rows = static_cast<int>(dst.height);
        if (n >= 1 && scene.layers[0].frame != nullptr && is_plain_copy(scene.layers[0], dst)) {
            const VideoFrame& s = *scene.layers[0].frame;
            pool_.run(rows, [&](unsigned, int y0, int y1) noexcept {
                for (int y = y0; y < y1; ++y) {
                    std::uint8_t* row = dst.data + static_cast<std::size_t>(y) * dst.stride;
                    std::memcpy(row, s.data + static_cast<std::size_t>(y) * s.stride, static_cast<std::size_t>(dst.width) * 4);
                    // A copy only stays exact if the source alpha is opaque.
                    for (std::uint32_t x = 0; x < dst.width; ++x) row[x * 4 + 3] = 255;
                }
            });
        } else {
            pool_.run(rows, [&](unsigned, int y0, int y1) noexcept { clear_black_rows(dst, y0, y1); });
            if (n >= 1 && scene.layers[0].frame != nullptr && !draw_layer(scene.layers[0], dst)) return false;
        }
        for (std::uint32_t i = 1; i < n; ++i) {
            if (scene.layers[i].frame != nullptr && !draw_layer(scene.layers[i], dst)) return false;
        }
        return true;
    }

    bool draw_layer(const LayerDraw& l, VideoFrame& dst) noexcept {
        const VideoFrame& src = *l.frame;
        LayerPlan p;
        p.src = &src;
        p.d = to_pixels(l.dest, dst.width, dst.height);
        if (p.d.x1 <= p.d.x0 || p.d.y1 <= p.d.y0 || src.width == 0 || src.height == 0) return true;
        const float opacity = std::clamp(l.opacity, 0.0f, 1.0f);
        if (opacity <= 0.0f) return true;

        // Source crop in texels; sampling is clamped inside it.
        const float cx0 = l.crop.x * static_cast<float>(src.width);
        p.cy0 = l.crop.y * static_cast<float>(src.height);
        const float cw = l.crop.w * static_cast<float>(src.width);
        const float ch = l.crop.h * static_cast<float>(src.height);
        const float min_u = std::max(0.0f, cx0);
        const float max_u = std::min(static_cast<float>(src.width) - 1.0f, cx0 + cw - 1.0f);
        p.min_v = std::max(0.0f, p.cy0);
        p.max_v = std::min(static_cast<float>(src.height) - 1.0f, p.cy0 + ch - 1.0f);
        p.dw = p.d.x1 - p.d.x0;
        const float sx = cw / static_cast<float>(p.dw);
        p.sy = ch / static_cast<float>(p.d.y1 - p.d.y0);
        p.op = static_cast<int>(std::lround(opacity * 256.0f));

        const auto dw = static_cast<std::size_t>(p.dw);
        try {
            if (u0_.size() < dw) {
                u0_.resize(dw);
                u1_.resize(dw);
                fx_.resize(dw);
            }
            lanes_.resize(pool_.lanes());
            for (LaneScratch& s : lanes_) {
                for (auto& r : s.rows) {
                    if (r.size() < dw * 4) r.resize(dw * 4);
                }
            }
        } catch (...) {
            return false;
        }
        for (std::size_t i = 0; i < dw; ++i) {
            const float u = std::clamp(cx0 + (static_cast<float>(i) + 0.5f) * sx - 0.5f, min_u, max_u);
            const int u0 = static_cast<int>(u);
            u0_[i] = u0 * 4;
            u1_[i] = std::min(u0 + 1, static_cast<int>(max_u)) * 4;
            fx_[i] = static_cast<int>((u - static_cast<float>(u0)) * 256.0f);
        }
        p.tu0 = u0_.data();
        p.tu1 = u1_.data();
        p.tfx = fx_.data();

        pool_.run(p.d.y1 - p.d.y0, [&](unsigned lane, int b, int e) noexcept {
            draw_band(p, dst, lanes_[lane], p.d.y0 + b, p.d.y0 + e);
        });
        return true;
    }

    bool ensure_scratch(const VideoFrame& like) noexcept {
        if (scratch_.width == like.width && scratch_.height == like.height && scratch_.data != nullptr) return true;
        try {
            scratch_storage_.assign(static_cast<std::size_t>(like.stride) * like.height, 0);
        } catch (...) {
            return false;
        }
        scratch_ = like;
        scratch_.data = scratch_storage_.data();
        return true;
    }

    void blend(VideoFrame& a, const VideoFrame& b, TransitionKind kind, float p) noexcept {
        const int wipe_x = std::clamp(static_cast<int>(std::lround(p * static_cast<float>(a.width))), 0, static_cast<int>(a.width));
        int wa = 256, wb = 0;  // weights for Mix / Fade (0..256)
        if (kind == TransitionKind::Mix) {
            wb = static_cast<int>(std::lround(p * 256.0f));
            wa = 256 - wb;
        } else if (kind == TransitionKind::Fade) {
            if (p < 0.5f) {
                wa = static_cast<int>(std::lround((1.0f - 2.0f * p) * 256.0f));
            } else {
                wa = 0;
                wb = static_cast<int>(std::lround((2.0f * p - 1.0f) * 256.0f));
            }
        }
        pool_.run(static_cast<int>(a.height), [&](unsigned, int y0, int y1) noexcept {
            for (int y = y0; y < y1; ++y) {
                std::uint8_t* ra = a.data + static_cast<std::size_t>(y) * a.stride;
                const std::uint8_t* rb = b.data + static_cast<std::size_t>(y) * b.stride;
                if (kind == TransitionKind::Wipe) {
                    std::memcpy(ra, rb, static_cast<std::size_t>(wipe_x) * 4);
                    continue;
                }
                // All four channels blend uniformly (vectorisable); alpha is
                // then forced opaque.
                const std::size_t n = static_cast<std::size_t>(a.width) * 4;
                for (std::size_t x = 0; x < n; ++x) {
                    ra[x] = static_cast<std::uint8_t>((ra[x] * wa + rb[x] * wb + 128) >> 8);
                }
                for (std::size_t x = 3; x < n; x += 4) ra[x] = 255;
            }
        });
    }

    BandPool pool_;
    std::vector<LaneScratch> lanes_;
    std::vector<int> u0_, u1_, fx_;
    std::vector<std::uint8_t> scratch_storage_;
    VideoFrame scratch_;
};

}  // namespace

std::unique_ptr<Compositor> make_cpu_compositor() { return std::make_unique<CpuCompositor>(); }

NormRect fit_rect(std::uint32_t src_w, std::uint32_t src_h, std::uint32_t dst_w, std::uint32_t dst_h) noexcept {
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) return {};
    const double src_aspect = static_cast<double>(src_w) / src_h;
    const double dst_aspect = static_cast<double>(dst_w) / dst_h;
    if (std::abs(src_aspect - dst_aspect) < 1e-6) return {};
    if (src_aspect > dst_aspect) {  // wider: letterbox (bars top/bottom)
        const auto h = static_cast<float>(dst_aspect / src_aspect);
        return {0.0f, (1.0f - h) / 2.0f, 1.0f, h};
    }
    const auto w = static_cast<float>(src_aspect / dst_aspect);  // narrower: pillarbox
    return {(1.0f - w) / 2.0f, 0.0f, w, 1.0f};
}

}  // namespace mixit::media
