# ADR-0011: Graphics, keys, overlay channels, multiview and replay

- Status: Accepted (M6)
- Date: 2026-11-21

## Context

Milestone 6 adds the production tools an operator uses on every show: titles
and lower thirds with live text, logos, a clock/countdown/scoreboard, green or
blue screen keys, graphics that survive cuts, a multiview, and instant replay
with slow motion. CLAUDE.md requires original implementations, licences that
allow closed-source distribution, real-time safety (nothing may block the tick
or audio threads) and honest status.

## Decision

1. **Keys belong to layers.** `mixit_layer_config` grows (80 bytes, the 0.3
   size of 48 stays accepted) with a chroma or luma keyer, evaluated per pixel
   in the compositor. The CPU compositor is the reference; the D3D11 shader
   uses the same formulas and is compared against it in tests (WARP in CI).
   Chroma uses BT.709 CbCr distance with a soft edge and spill suppression —
   simple, predictable and cheap on both paths.
2. **Overlay channels** (4) are downstream layers after the transition mix,
   with per-channel fades advanced by elapsed ticks. Graphics therefore stay
   on air across cuts and transitions, as operators expect.
3. **Graphics are inputs**, not a special compositor stage: a JSON scene of
   rect/text/image elements rendered on the CPU into a transparent BGRA frame,
   only when something changed. Any graphic can go on a layer or an overlay
   channel, be recorded ISO, or be keyed. Templates (lower third, title,
   clock, countdown, scoreboard, logo) live in the managed layer
   (`GraphicsTemplates`) as ordinary scenes.
4. **Text with FreeType 2.14.3** (FTL — BSD-style with an attribution clause;
   chosen over the GPL-2 option), fetched at a pinned commit and linked
   statically with every optional dependency disabled. **DejaVu Sans / Bold
   2.37** are embedded so titles render identically on every PC (Bitstream
   Vera licence plus public-domain DejaVu changes: redistribution allowed with
   the licence text; modified fonts may not use the "Bitstream"/"Vera" names —
   we embed them unmodified). Images are decoded with
   the FFmpeg already in the engine. Own small JSON parser with size, depth and
   element limits (inputs are operator files).
5. **HTML/browser graphics are deferred**: CEF (BSD-3, ~150 MB, Chromium
   notices) and WebView2 (Microsoft runtime, offscreen capture limits) both
   need a separate decision; nothing in this design blocks adding an HTML input
   later as another input kind.
6. **Multiview is a bus**, rendered by the tick thread on the CPU only while
   someone reads it or an output records it (source `0xFFFF`, with program
   audio). One fixed layout (2 large + 8 small) for now.
7. **Instant replay records JPEG frames in memory**: per input, an MJPEG
   encoder thread fed through a non-blocking 3-deep queue, frames kept in a
   time-bounded deque stamped with master time. Intra-only JPEG makes any
   frame directly addressable (seeking, slow motion, reverse later) at about
   3 MB/s for 1080p30 — fine for the 5–600 s range on 16 GB machines. A replay
   is an ordinary input that plays a buffer at 0.1–2×, so it switches,
   transitions and records like a camera. Video only: replay audio at
   variable speed needs time-stretching (later).

## Consequences

- Graphics cost CPU only when they change; a clock costs one render per second.
- Keyed cameras on overlay channels give a usable 2D virtual set (presenter
  over a background input) without a separate engine; 3D sets and camera
  tracking remain out of scope.
- Replay memory is bounded by the configured span; recording never blocks the
  tick (drops are counted and shown).
- The multiview and graphics are CPU-rendered; with the D3D11 compositor the
  frames are uploaded like any input. Moving them to the GPU is an M8
  optimization once profiled on reference hardware.
- New third-party components (FreeType, DejaVu fonts) are recorded in
  THIRD_PARTY_LICENSES with their attribution duties.
