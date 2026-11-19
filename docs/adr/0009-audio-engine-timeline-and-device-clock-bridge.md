# ADR-0009: Audio engine on the session sample timeline, with a drift bridge to device clocks

- Status: Accepted (M4)
- Date: 2026-11-19

## Context

Audio must stay locked to video for hours (CLAUDE.md §16.E), sound cards run on
their own crystal (typically ±50 ppm off nominal), several devices may be used
at once, and the real-time path must never block or allocate.

## Decision

1. **One master timeline.** The session's master clock (ENGINE_DESIGN §3)
   defines sample index *n* ↔ time `origin + n/sr` exactly (rational
   arithmetic, no accumulated float time). Video ticks and audio blocks are
   both addressed on it.
2. **A dedicated mixer thread** (`mixit-audio`) mixes fixed 10 ms blocks.
   Block *k* is mixed at `origin + (k+1)·B/sr + L`, where
   `L = max(25 ms, video latency − 10 ms)`, so inputs have had time to deliver.
   A stall > 20 blocks resynchronizes (counted as skipped blocks) instead of
   mixing a burst of stale audio.
3. **Per-input timeline buffers** (`SampleFifo`, 1.5 s) addressed by absolute
   sample index. Inputs write where their timestamps say; gaps are zero-filled
   and missing frames counted as underruns. Per-strip delay is a read offset.
4. **Parameters and meters cross threads lock-free** (triple buffers);
   gain changes are ramped over one block to avoid zipper noise.
5. **Device clocks are bridged, never slaved.** Between the mixer and a device
   sits an SPSC ring plus a `DriftResampler` (Catmull-Rom interpolation, PI
   controller on the smoothed ring fill, ratio limited to ±0.5 %). Output
   devices consume, capture devices produce at their own rate; the timeline
   is unaffected. Start-up and stalls are handled by priming and discarding
   excess fill.
6. **WASAPI shared mode first**, float32 at the session format with Windows'
   converter, event-driven, MMCSS "Pro Audio". Exclusive mode and ASIO are
   later backends behind the same `AudioOutput` / capture interfaces. ASIO
   needs Steinberg's proprietary SDK licence (the GPLv3 option is
   incompatible with a closed product) — waiting for the owner's decision.
7. A built-in **null test device** (`mixit:null[:ppm]`) runs the same bridge
   against a simulated clock on every platform, so drift handling is tested in
   CI without hardware.

## Consequences

- A/V alignment is a property of timestamps, not of device timing; measured by
  the existing flash/click detector (now on the mixed program audio).
- The bridge adds about 20–40 ms of buffering per device (configurable
  latency on the output, fixed 20 ms on capture inputs).
- Real WASAPI devices have not been exercised (no audio hardware in CI or on
  the dev host): EXPERIMENTAL until run on a workstation.
