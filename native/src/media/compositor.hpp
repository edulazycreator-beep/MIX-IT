// Video compositor (M3): renders a bus scene (base input + overlay layers)
// and, during a transition, blends two scenes into one BGRA output frame.
//
// Two implementations share this interface:
//   * CpuCompositor  — portable reference (bilinear scaling, straight alpha);
//                      also the fallback when no GPU device is available.
//   * D3D11Compositor — GPU path on Windows (d3d11_compositor.cpp), verified
//                      against the CPU reference in tests.
// Implementations are used by one thread at a time (the session's output
// thread); render() must not allocate per call after warm-up.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

#include "core/status.hpp"
#include "logging/logger.hpp"
#include "media/media_types.hpp"

namespace mixit::media {

// Normalized rectangle (0..1) relative to the frame it applies to.
struct NormRect {
    float x = 0.0f;
    float y = 0.0f;
    float w = 1.0f;
    float h = 1.0f;
    friend bool operator==(const NormRect&, const NormRect&) = default;
};

inline constexpr std::uint32_t kMaxLayersPerScene = 8;  // overlay layers on top of the base

struct LayerDraw {
    const VideoFrame* frame = nullptr;  // null = skip
    NormRect crop;                      // source region
    NormRect dest;                      // destination region of the output
    float opacity = 1.0f;               // multiplied with the source alpha
};

// Draw list for one scene, bottom to top. Layer 0 is normally the base input.
struct SceneDraw {
    std::array<LayerDraw, kMaxLayersPerScene + 1> layers{};
    std::uint32_t count = 0;
};

enum class TransitionKind : std::int32_t {
    Cut = 0,
    Mix = 1,   // cross-dissolve
    Fade = 2,  // A fades to black, then B fades up
    Wipe = 3,  // B wipes in from left to right
};

enum class CompositorKind : std::int32_t { Cpu = 0, D3D11 = 1 };  // = MIXIT_COMPOSITOR_*

class Compositor {
public:
    virtual ~Compositor() = default;
    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual CompositorKind kind() const noexcept = 0;

    // Renders `a` into `dst` (cleared to black first). If `b` is non-null,
    // renders it too and blends A→B with `kind` at `progress` in [0, 1].
    virtual Status render(const SceneDraw& a, const SceneDraw* b, TransitionKind kind, float progress,
                          VideoFrame& dst) noexcept = 0;
};

std::unique_ptr<Compositor> make_cpu_compositor();

struct GpuCompositorOptions {
    bool software = false;  // WARP rasterizer (tests, machines without a GPU)
};

// Windows: Direct3D 11 compositor. Elsewhere returns NOT_SUPPORTED.
Status make_d3d11_compositor(const GpuCompositorOptions& options, Logger& logger, std::unique_ptr<Compositor>& out);

// Destination rectangle that fits a src_w x src_h picture into the output
// while preserving its aspect ratio (letterbox / pillarbox), centred.
NormRect fit_rect(std::uint32_t src_w, std::uint32_t src_h, std::uint32_t dst_w, std::uint32_t dst_h) noexcept;

}  // namespace mixit::media
