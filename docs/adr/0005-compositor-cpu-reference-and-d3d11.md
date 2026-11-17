# ADR-0005: Compositor — CPU reference implementation, Direct3D 11 path, CPU-resident frames

- Status: Accepted (M3)
- Date: 2026-11-17

## Context

M3 needs one engine component that draws a bus: a base input fitted to the
program format, up to 8 overlay layers (crop, placement, opacity) and, during
a transition, two scenes blended together. The original M3 plan (ARCHITECTURE
§7) put this on the GPU from the start, with decoded frames staying in GPU
memory (D3D11VA) and native swap-chain monitors.

In M3 every frame source is CPU-resident: the test signal renders in memory,
the FFmpeg file source decodes in software and converts with swscale, and the
Media Foundation capture source converts to RGB32 in system memory. Outputs
(the diagnostic frame copy and the WPF monitors) also consume CPU memory. The
development host and CI runners have no GPU (CI has only WARP).

## Decision

1. **One `Compositor` interface, two implementations** sharing a written pixel
   convention: pixel centres at +0.5, bilinear sampling clamped to the crop's
   texel centres, straight (non-premultiplied) alpha multiplied by layer
   opacity, destination rectangles rounded to whole pixels, output alpha 255.
   Transitions: Mix (cross-dissolve), Fade (through black, A until 50 %, then
   B), Wipe (left to right).
2. **CPU compositor = reference and default.** Fixed-point (8-bit fraction)
   separable bilinear filter with per-lane row caches, split over horizontal
   bands on a small helper pool (≤ 8 lanes). Results do not depend on the
   number of bands. Exact pixel tests define its behaviour.
3. **Direct3D 11 compositor = EXPERIMENTAL GPU path.** Textured quads with
   alpha blending, a blend pass for transitions, readback through a staging
   texture. Shaders are compiled at run time with `d3dcompiler_47.dll`
   (part of Windows 10/11); WARP is supported for tests. A CI test compares it
   with the CPU reference (max |Δ| ≤ 4, mean < 1 per channel on smooth
   content; on hard edges the sub-texel filter precision differs).
4. **Selection is explicit.** `mixit_session_config.compositor`:
   `AUTO` (= CPU in API 0.3), `CPU`, `D3D11`, `D3D11_WARP`. A GPU preference
   that cannot be honoured falls back to CPU with a warning; a GPU failure
   while running (device removed, driver reset) switches the session to the
   CPU path for the rest of its life instead of outputting black
   (`stats.compositor_fallbacks`). `stats.compositor` always reports what runs.
5. **Zero-copy stays first.** A bus showing one full-size input with no
   layers and no transition is not composited at all (shared frame ref).
   Preview is rendered only while someone reads it.

## Why AUTO is CPU for now

With CPU-resident frames the GPU path pays an upload per layer and a
synchronous readback per bus per tick. Measured on the 2-core dev host, the
CPU path renders a 640×360 → 1080p upscale in ~4.9 ms and a two-scene mix in
~10 ms (Release), and passes a 1080p60 soak with a PiP layer, a watched
preview bus and transitions with zero late ticks. No GPU measurements exist
yet (no hardware here; CI has WARP only). Switching the default without data
would contradict "profile before optimizing". `mixit-probe --session-test N
--compositor d3d11` exists to collect that data on real machines.

## Consequences

- The GPU path becomes the default once frames stay on the GPU end to end
  (hardware decode, GPU capture paths, swap-chain monitors, hardware encode —
  M3 follow-up / M5 / M8). The interface does not change for that; the
  `SceneDraw` layers will then reference GPU textures as well.
- CPU cost grows with resolution × layers. On many-core workstations the band
  pool scales; on small machines operators should prefer zero-copy layouts.
- Two implementations must be kept in step; the equivalence test runs on every
  Windows CI build.
