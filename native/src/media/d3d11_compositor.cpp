// Direct3D 11 compositor (Windows). Same scene model and pixel conventions as
// the CPU reference (cpu_compositor.cpp); tests compare the two.
//
// Frames are CPU-resident in this milestone: each render uploads the layer
// frames (UpdateSubresource), draws them as textured quads with alpha
// blending, blends two scenes for transitions, and reads the result back
// through a staging texture. This is a correct GPU path that the later GPU
// decode / encode / display paths build on; it is not yet a zero-copy path.
//
// Public, documented APIs only: D3D11CreateDevice, ID3D11Device /
// ID3D11DeviceContext, IDXGIDevice::GetAdapter, and D3DCompile from
// d3dcompiler_47.dll (part of Windows 10/11), loaded at run time.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

#include "logging/logger.hpp"
#include "media/compositor.hpp"
#include "platform/com_ref.hpp"
#include "platform/win_strings.hpp"

namespace mixit::media {
namespace {

// One vertex shader draws a quad from SV_VertexID (triangle strip, 4 vertices).
// ps_layer: bilinear sample clamped to the crop's texel centres, straight
// alpha multiplied by the layer opacity (blend state does the "over").
// ps_blend: per-pixel transition between two same-size scene targets.
constexpr char kShaderSource[] = R"(
cbuffer Params : register(b0) {
    float4 dest;      // NDC x0, y0, x1, y1
    float4 uv;        // texture coordinates at the quad corners (u0, v0, u1, v1)
    float4 uv_clamp;  // min_u, min_v, max_u, max_v (texel centres, normalized)
    float4 params;    // opacity, transition kind, progress, wipe edge in pixels
    float4 key0;      // key type (0 none, 1 chroma, 2 luma), key Cb, key Cr, dominant channel (0 b, 1 g, 2 r)
    float4 key1;      // similarity, 1/smoothness, spill, luma threshold
    float4 key2;      // 1/softness, invert, -, -
};
Texture2D tex0 : register(t0);
Texture2D tex1 : register(t1);
SamplerState linear_clamp : register(s0);

struct VsOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VsOut vs_main(uint id : SV_VertexID) {
    float2 t = float2((id & 1) ? 1.0 : 0.0, (id & 2) ? 1.0 : 0.0);
    VsOut o;
    o.pos = float4(lerp(dest.x, dest.z, t.x), lerp(dest.y, dest.w, t.y), 0.0, 1.0);
    o.uv = float2(lerp(uv.x, uv.z, t.x), lerp(uv.y, uv.w, t.y));
    return o;
}

// Same keyer as the CPU compositor (compositor.hpp, KeySettings).
float key_alpha(inout float3 rgb) {
    float y = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    if (key0.x > 1.5) {
        float a = saturate((y - key1.w) * key2.x);
        return key2.y > 0.5 ? 1.0 - a : a;
    }
    float2 cbcr = float2((rgb.b - y) / 1.8556, (rgb.r - y) / 1.5748);
    float a = saturate((length(cbcr - key0.yz) - key1.x) * key1.y);
    if (key1.z > 0.0) {
        // Channels in BGRA order as on the CPU: 0 = b, 1 = g, 2 = r.
        float3 bgr = rgb.bgr * 255.0;
        int c = (int)key0.w;
        float cap = c == 0 ? max(bgr.y, bgr.z) : (c == 1 ? max(bgr.x, bgr.z) : max(bgr.x, bgr.y));
        float v = c == 0 ? bgr.x : (c == 1 ? bgr.y : bgr.z);
        if (v > cap) v = round(v - key1.z * (v - cap));
        if (c == 0) bgr.x = v; else if (c == 1) bgr.y = v; else bgr.z = v;
        rgb = bgr.zyx / 255.0;
    }
    return a;
}

float4 ps_layer(VsOut i) : SV_Target {
    float4 s = tex0.SampleLevel(linear_clamp, clamp(i.uv, uv_clamp.xy, uv_clamp.zw), 0);
    float a = s.a * params.x;
    float3 c = s.rgb;
    if (key0.x > 0.5 && a > 0.0) a *= key_alpha(c);
    return float4(c, a);
}

float4 ps_blend(VsOut i) : SV_Target {
    int3 p = int3(int2(i.pos.xy), 0);
    float3 a = tex0.Load(p).rgb;
    float3 b = tex1.Load(p).rgb;
    int kind = (int)params.y;
    float t = params.z;
    float3 c;
    if (kind == 3) {
        c = i.pos.x < params.w ? b : a;
    } else if (kind == 2) {
        c = t < 0.5 ? a * (1.0 - 2.0 * t) : b * (2.0 * t - 1.0);
    } else {
        c = lerp(a, b, t);
    }
    return float4(c, 1.0);
}
)";

struct alignas(16) Constants {
    float dest[4];
    float uv[4];
    float uv_clamp[4];
    float params[4];
    float key0[4];
    float key1[4];
    float key2[4];
};
static_assert(sizeof(Constants) == 112);

std::string hr_text(HRESULT hr) { return std::format("0x{:08X}", static_cast<unsigned>(hr)); }

Status hr_status(const char* what, HRESULT hr) {
    return {MIXIT_E_PLATFORM, std::format("D3D11 {} failed ({})", what, hr_text(hr))};
}

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

// A render target that can also be sampled (scene targets feed the blend pass).
struct Target {
    std::uint32_t width = 0, height = 0;
    ComRef<ID3D11Texture2D> texture;
    ComRef<ID3D11RenderTargetView> rtv;
    ComRef<ID3D11ShaderResourceView> srv;
};

// Upload texture, reused across renders for frames of the same size.
struct Upload {
    std::uint32_t width = 0, height = 0;
    ComRef<ID3D11Texture2D> texture;
    ComRef<ID3D11ShaderResourceView> srv;
    std::uint64_t generation = 0;  // render() call that last used it
    const VideoFrame* frame = nullptr;
};

constexpr std::size_t kMaxUploads = 2 * (kMaxLayersPerScene + 1) + 2;

class D3D11Compositor final : public Compositor {
public:
    [[nodiscard]] std::string name() const override { return "d3d11"; }
    [[nodiscard]] CompositorKind kind() const noexcept override { return CompositorKind::D3D11; }

    Status init(const GpuCompositorOptions& options, Logger& logger) {
        Status st = create_device(options, logger);
        if (!st.is_ok()) return st;
        if (st = create_pipeline(); !st.is_ok()) return st;
        uploads_.reserve(kMaxUploads);
        return Status::ok();
    }

    using Compositor::render;
    Status render(const SceneDraw& a, const SceneDraw* b, TransitionKind kind, float progress, VideoFrame& dst,
                  const SceneDraw* overlay) noexcept override {
        try {
            ++generation_;
            Status st = ensure_target(target_a_, dst.width, dst.height);
            if (!st.is_ok()) return st;
            if (st = draw_scene(a, target_a_); !st.is_ok()) return st;
            Target* result = &target_a_;
            if (b != nullptr && kind != TransitionKind::Cut) {
                if (st = ensure_target(target_b_, dst.width, dst.height); !st.is_ok()) return st;
                if (st = ensure_target(target_out_, dst.width, dst.height); !st.is_ok()) return st;
                if (st = draw_scene(*b, target_b_); !st.is_ok()) return st;
                if (st = blend(kind, std::clamp(progress, 0.0f, 1.0f)); !st.is_ok()) return st;
                result = &target_out_;
            }
            if (overlay != nullptr && overlay->count > 0) {
                if (st = draw_layers(*overlay, *result); !st.is_ok()) return st;
            }
            return readback(result->texture.get(), dst);
        } catch (const std::exception& e) {
            return {MIXIT_E_INTERNAL, std::string("D3D11 compositor: ") + e.what()};
        }
    }

private:
    Status create_device(const GpuCompositorOptions& options, Logger& logger) {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0};
        const D3D_DRIVER_TYPE driver = options.software ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        D3D_FEATURE_LEVEL got{};
        HRESULT hr = ::D3D11CreateDevice(nullptr, driver, nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                         device_.put(), &got, context_.put());
        if (hr == E_INVALIDARG) {  // runtimes without 11.1 reject the list containing it
            hr = ::D3D11CreateDevice(nullptr, driver, nullptr, 0, levels + 1, ARRAYSIZE(levels) - 1,
                                     D3D11_SDK_VERSION, device_.put(), &got, context_.put());
        }
        if (FAILED(hr)) return hr_status(options.software ? "WARP device creation" : "device creation", hr);

        std::string adapter = options.software ? "WARP (software)" : "unknown adapter";
        ComRef<IDXGIDevice> dxgi;
        if (SUCCEEDED(device_->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(dxgi.put())))) {
            ComRef<IDXGIAdapter> a;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(dxgi->GetAdapter(a.put())) && SUCCEEDED(a->GetDesc(&desc))) {
                adapter = os::wide_to_utf8(desc.Description);
            }
        }
        logger.logf(MIXIT_LOG_INFO, "compositor", "D3D11 compositor on '{}' (feature level {:x})", adapter,
                    static_cast<unsigned>(got));
        return Status::ok();
    }

    Status create_pipeline() {
        HMODULE compiler = ::LoadLibraryW(L"d3dcompiler_47.dll");
        if (compiler == nullptr) return {MIXIT_E_NOT_SUPPORTED, "d3dcompiler_47.dll is not available"};
        const auto compile = reinterpret_cast<pD3DCompile>(reinterpret_cast<void*>(::GetProcAddress(compiler, "D3DCompile")));
        Status st = compile == nullptr ? Status{MIXIT_E_NOT_SUPPORTED, "D3DCompile not found"} : Status::ok();
        if (st.is_ok()) st = compile_shaders(compile);
        ::FreeLibrary(compiler);  // all blobs from the compiler are released by now
        if (!st.is_ok()) return st;

        D3D11_BUFFER_DESC cb{};
        cb.ByteWidth = sizeof(Constants);
        cb.Usage = D3D11_USAGE_DYNAMIC;
        cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (HRESULT hr = device_->CreateBuffer(&cb, nullptr, constants_.put()); FAILED(hr)) return hr_status("CreateBuffer", hr);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        if (HRESULT hr = device_->CreateSamplerState(&sd, sampler_.put()); FAILED(hr)) return hr_status("CreateSamplerState", hr);

        // "Over" for colour; destination alpha stays 1 (targets are cleared opaque).
        D3D11_BLEND_DESC bd{};
        auto& rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D11_BLEND_ZERO;
        rt.DestBlendAlpha = D3D11_BLEND_ONE;
        rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (HRESULT hr = device_->CreateBlendState(&bd, blend_over_.put()); FAILED(hr)) return hr_status("CreateBlendState", hr);

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        if (HRESULT hr = device_->CreateRasterizerState(&rd, raster_.put()); FAILED(hr)) return hr_status("CreateRasterizerState", hr);
        return Status::ok();
    }

    Status compile_shaders(pD3DCompile compile) {
        auto build = [&](const char* entry, const char* profile, ComRef<ID3DBlob>& code) -> Status {
            ComRef<ID3DBlob> errors;
            const HRESULT hr = compile(kShaderSource, sizeof(kShaderSource) - 1, "mixit_compositor.hlsl", nullptr,
                                       nullptr, entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.put(),
                                       errors.put());
            if (FAILED(hr)) {
                std::string detail;
                if (errors) detail.assign(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
                return {MIXIT_E_INTERNAL, std::format("shader {} failed to compile ({}): {}", entry, hr_text(hr), detail)};
            }
            return Status::ok();
        };
        ComRef<ID3DBlob> vs, ps_layer, ps_blend;
        Status st = build("vs_main", "vs_4_0", vs);
        if (st.is_ok()) st = build("ps_layer", "ps_4_0", ps_layer);
        if (st.is_ok()) st = build("ps_blend", "ps_4_0", ps_blend);
        if (!st.is_ok()) return st;
        if (HRESULT hr = device_->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, vs_.put());
            FAILED(hr)) {
            return hr_status("CreateVertexShader", hr);
        }
        if (HRESULT hr = device_->CreatePixelShader(ps_layer->GetBufferPointer(), ps_layer->GetBufferSize(), nullptr,
                                                    ps_layer_.put());
            FAILED(hr)) {
            return hr_status("CreatePixelShader(layer)", hr);
        }
        if (HRESULT hr = device_->CreatePixelShader(ps_blend->GetBufferPointer(), ps_blend->GetBufferSize(), nullptr,
                                                    ps_blend_.put());
            FAILED(hr)) {
            return hr_status("CreatePixelShader(blend)", hr);
        }
        return Status::ok();
    }

    Status ensure_target(Target& t, std::uint32_t w, std::uint32_t h) {
        if (t.texture && t.width == w && t.height == h) return Status::ok();
        t = Target{};
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w;
        d.Height = h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (HRESULT hr = device_->CreateTexture2D(&d, nullptr, t.texture.put()); FAILED(hr)) return hr_status("CreateTexture2D(target)", hr);
        if (HRESULT hr = device_->CreateRenderTargetView(t.texture.get(), nullptr, t.rtv.put()); FAILED(hr)) {
            return hr_status("CreateRenderTargetView", hr);
        }
        if (HRESULT hr = device_->CreateShaderResourceView(t.texture.get(), nullptr, t.srv.put()); FAILED(hr)) {
            return hr_status("CreateShaderResourceView(target)", hr);
        }
        t.width = w;
        t.height = h;
        return Status::ok();
    }

    // Uploads a frame once per render() call (the same input may appear on
    // both scenes) into a size-matched reusable texture.
    Status upload(const VideoFrame& f, ID3D11ShaderResourceView*& out) {
        for (Upload& u : uploads_) {
            if (u.generation == generation_ && u.frame == &f) {
                out = u.srv.get();
                return Status::ok();
            }
        }
        Upload* slot = nullptr;
        for (Upload& u : uploads_) {
            if (u.generation != generation_ && u.width == f.width && u.height == f.height) {
                slot = &u;
                break;
            }
        }
        if (slot == nullptr) {
            if (uploads_.size() >= kMaxUploads) {
                // Recycle the least recently used texture of another size.
                auto lru = std::min_element(uploads_.begin(), uploads_.end(),
                                            [](const Upload& x, const Upload& y) { return x.generation < y.generation; });
                if (lru == uploads_.end() || lru->generation == generation_) {
                    return {MIXIT_E_INTERNAL, "D3D11 compositor: too many distinct layer frames"};
                }
                *lru = Upload{};
                slot = &*lru;
            } else {
                slot = &uploads_.emplace_back();
            }
            D3D11_TEXTURE2D_DESC d{};
            d.Width = f.width;
            d.Height = f.height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_DEFAULT;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (HRESULT hr = device_->CreateTexture2D(&d, nullptr, slot->texture.put()); FAILED(hr)) {
                return hr_status("CreateTexture2D(upload)", hr);
            }
            if (HRESULT hr = device_->CreateShaderResourceView(slot->texture.get(), nullptr, slot->srv.put()); FAILED(hr)) {
                return hr_status("CreateShaderResourceView(upload)", hr);
            }
            slot->width = f.width;
            slot->height = f.height;
        }
        context_->UpdateSubresource(slot->texture.get(), 0, nullptr, f.data, f.stride, 0);
        slot->generation = generation_;
        slot->frame = &f;
        out = slot->srv.get();
        return Status::ok();
    }

    Status set_constants(const Constants& c) {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (HRESULT hr = context_->Map(constants_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m); FAILED(hr)) {
            return hr_status("Map(constants)", hr);
        }
        std::memcpy(m.pData, &c, sizeof(c));
        context_->Unmap(constants_.get(), 0);
        return Status::ok();
    }

    void bind_common(const Target& t) {
        ID3D11RenderTargetView* rtv = t.rtv.get();
        context_->OMSetRenderTargets(1, &rtv, nullptr);
        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(t.width), static_cast<float>(t.height), 0.0f, 1.0f};
        context_->RSSetViewports(1, &vp);
        context_->RSSetState(raster_.get());
        context_->IASetInputLayout(nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context_->VSSetShader(vs_.get(), nullptr, 0);
        ID3D11Buffer* cb = constants_.get();
        context_->VSSetConstantBuffers(0, 1, &cb);
        context_->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState* s = sampler_.get();
        context_->PSSetSamplers(0, 1, &s);
    }

    Status draw_scene(const SceneDraw& scene, Target& t) {
        const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        context_->ClearRenderTargetView(t.rtv.get(), black);
        return draw_layers(scene, t);
    }

    // Draws layers "over" what the target already holds.
    Status draw_layers(const SceneDraw& scene, Target& t) {
        bind_common(t);
        context_->PSSetShader(ps_layer_.get(), nullptr, 0);
        const float factor[4] = {0, 0, 0, 0};
        context_->OMSetBlendState(blend_over_.get(), factor, 0xFFFFFFFFu);
        const std::uint32_t n = std::min<std::uint32_t>(scene.count, static_cast<std::uint32_t>(scene.layers.size()));
        for (std::uint32_t i = 0; i < n; ++i) {
            if (scene.layers[i].frame == nullptr) continue;
            if (Status st = draw_layer(scene.layers[i], t); !st.is_ok()) return st;
        }
        return Status::ok();
    }

    Status draw_layer(const LayerDraw& l, const Target& t) {
        const VideoFrame& src = *l.frame;
        const PixelRect d = to_pixels(l.dest, t.width, t.height);
        const float opacity = std::clamp(l.opacity, 0.0f, 1.0f);
        if (d.x1 <= d.x0 || d.y1 <= d.y0 || src.width == 0 || src.height == 0 || opacity <= 0.0f) return Status::ok();

        const auto sw = static_cast<float>(src.width), sh = static_cast<float>(src.height);
        const float cx0 = l.crop.x * sw, cy0 = l.crop.y * sh, cw = l.crop.w * sw, ch = l.crop.h * sh;
        const float min_u = std::max(0.0f, cx0), max_u = std::min(sw - 1.0f, cx0 + cw - 1.0f);
        const float min_v = std::max(0.0f, cy0), max_v = std::min(sh - 1.0f, cy0 + ch - 1.0f);
        const auto W = static_cast<float>(t.width), H = static_cast<float>(t.height);

        Constants c{};
        c.dest[0] = static_cast<float>(d.x0) / W * 2.0f - 1.0f;
        c.dest[1] = 1.0f - static_cast<float>(d.y0) / H * 2.0f;
        c.dest[2] = static_cast<float>(d.x1) / W * 2.0f - 1.0f;
        c.dest[3] = 1.0f - static_cast<float>(d.y1) / H * 2.0f;
        c.uv[0] = cx0 / sw;
        c.uv[1] = cy0 / sh;
        c.uv[2] = (cx0 + cw) / sw;
        c.uv[3] = (cy0 + ch) / sh;
        c.uv_clamp[0] = (min_u + 0.5f) / sw;
        c.uv_clamp[1] = (min_v + 0.5f) / sh;
        c.uv_clamp[2] = (std::max(min_u, max_u) + 0.5f) / sw;
        c.uv_clamp[3] = (std::max(min_v, max_v) + 0.5f) / sh;
        c.params[0] = opacity;
        if (l.key.type != KeyType::None) {
            const KeyColour kc = key_colour(l.key.color);
            c.key0[0] = static_cast<float>(static_cast<int>(l.key.type));
            c.key0[1] = kc.cb;
            c.key0[2] = kc.cr;
            c.key0[3] = static_cast<float>(kc.dominant);
            c.key1[0] = l.key.similarity;
            c.key1[1] = 1.0f / std::max(l.key.smoothness, 1e-4f);
            c.key1[2] = l.key.spill;
            c.key1[3] = l.key.threshold;
            c.key2[0] = 1.0f / std::max(l.key.softness, 1e-4f);
            c.key2[1] = l.key.invert ? 1.0f : 0.0f;
        }

        ID3D11ShaderResourceView* srv = nullptr;
        if (Status st = upload(src, srv); !st.is_ok()) return st;
        if (Status st = set_constants(c); !st.is_ok()) return st;
        context_->PSSetShaderResources(0, 1, &srv);
        context_->Draw(4, 0);
        return Status::ok();
    }

    Status blend(TransitionKind kind, float progress) {
        bind_common(target_out_);
        context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
        context_->PSSetShader(ps_blend_.get(), nullptr, 0);
        Constants c{};
        c.dest[0] = -1.0f;
        c.dest[1] = 1.0f;
        c.dest[2] = 1.0f;
        c.dest[3] = -1.0f;
        c.params[1] = static_cast<float>(static_cast<int>(kind));
        c.params[2] = progress;
        c.params[3] = static_cast<float>(std::lround(progress * static_cast<float>(target_out_.width)));
        if (Status st = set_constants(c); !st.is_ok()) return st;
        ID3D11ShaderResourceView* views[2] = {target_a_.srv.get(), target_b_.srv.get()};
        context_->PSSetShaderResources(0, 2, views);
        context_->Draw(4, 0);
        ID3D11ShaderResourceView* none[2] = {nullptr, nullptr};
        context_->PSSetShaderResources(0, 2, none);  // targets are rendered to again next frame
        return Status::ok();
    }

    Status readback(ID3D11Texture2D* result, VideoFrame& dst) {
        ID3D11RenderTargetView* none = nullptr;
        context_->OMSetRenderTargets(1, &none, nullptr);
        if (!staging_ || staging_width_ != dst.width || staging_height_ != dst.height) {
            staging_.reset();
            D3D11_TEXTURE2D_DESC d{};
            d.Width = dst.width;
            d.Height = dst.height;
            d.MipLevels = 1;
            d.ArraySize = 1;
            d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (HRESULT hr = device_->CreateTexture2D(&d, nullptr, staging_.put()); FAILED(hr)) {
                return hr_status("CreateTexture2D(staging)", hr);
            }
            staging_width_ = dst.width;
            staging_height_ = dst.height;
        }
        context_->CopyResource(staging_.get(), result);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (HRESULT hr = context_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &m); FAILED(hr)) {
            const HRESULT reason = device_->GetDeviceRemovedReason();
            return {MIXIT_E_PLATFORM, std::format("D3D11 readback failed ({}, device status {})", hr_text(hr), hr_text(reason))};
        }
        const auto row_bytes = static_cast<std::size_t>(dst.width) * 4;
        for (std::uint32_t y = 0; y < dst.height; ++y) {
            std::memcpy(dst.data + static_cast<std::size_t>(y) * dst.stride,
                        static_cast<const std::uint8_t*>(m.pData) + static_cast<std::size_t>(y) * m.RowPitch, row_bytes);
        }
        context_->Unmap(staging_.get(), 0);
        return Status::ok();
    }

    ComRef<ID3D11Device> device_;
    ComRef<ID3D11DeviceContext> context_;
    ComRef<ID3D11VertexShader> vs_;
    ComRef<ID3D11PixelShader> ps_layer_, ps_blend_;
    ComRef<ID3D11Buffer> constants_;
    ComRef<ID3D11SamplerState> sampler_;
    ComRef<ID3D11BlendState> blend_over_;
    ComRef<ID3D11RasterizerState> raster_;
    Target target_a_, target_b_, target_out_;
    ComRef<ID3D11Texture2D> staging_;
    std::uint32_t staging_width_ = 0, staging_height_ = 0;
    std::vector<Upload> uploads_;
    std::uint64_t generation_ = 0;
};

}  // namespace

Status make_d3d11_compositor(const GpuCompositorOptions& options, Logger& logger, std::unique_ptr<Compositor>& out) {
    try {
        auto c = std::make_unique<D3D11Compositor>();
        if (Status st = c->init(options, logger); !st.is_ok()) return st;
        out = std::move(c);
        return Status::ok();
    } catch (const std::bad_alloc&) {
        return {MIXIT_E_OUT_OF_MEMORY, "D3D11 compositor"};
    }
}

}  // namespace mixit::media

#else  // !_WIN32

#include "media/compositor.hpp"

namespace mixit::media {

Status make_d3d11_compositor(const GpuCompositorOptions&, Logger&, std::unique_ptr<Compositor>&) {
    return {MIXIT_E_NOT_SUPPORTED, "Direct3D 11 is only available on Windows"};
}

}  // namespace mixit::media

#endif
