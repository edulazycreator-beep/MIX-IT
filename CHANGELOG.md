# Changelog

All notable changes to MIX IT. Format: Keep a Changelog; versions follow the
native API version until the first product release.

## [0.6.0] — 2026-11-21 — Milestone 6: Graphics and production tools

### Added
- C ABI 0.6.0 (75 exports): layer keyers, overlay channels, graphics inputs, multiview bus and output source, input labels, instant replay. `mixit_layer_config` grows to 80 bytes (the 0.3 size 48 is still accepted, without key).
- Chroma key (BT.709 CbCr distance, soft edge, spill suppression) and luma key (threshold, softness, invert) per layer, identical in the CPU and D3D11 compositors.
- Four overlay channels (downstream keys) drawn after transitions, with fades; they stay on air across cuts and count for audio-follows-video.
- Graphics inputs (ADR-0011): JSON scenes of rectangles, text and images; FreeType 2.14.3 text with embedded DejaVu Sans/Bold 2.37 (kerning, alignment, fit-to-box, shadow); live fields, local clock and stopwatch/countdown; rendered only on change. Own bounded JSON parser.
- Multiview bus: preview, program and inputs 1–8 with labels and tally borders, rendered only while shown or recorded; outputs can record/stream it with program audio.
- Instant replay: per-input MJPEG buffer of the last 5–600 s (non-blocking feed, drops counted), replay inputs that play from any moment at 0.1–2× with pause.
- Managed: keys, overlays, graphics, replay and multiview bindings; `GraphicsTemplates` (lower third, title, clock, countdown, scoreboard, logo); `ProductionService` graphics, overlays (restored after rebuilds), replay (mark in, play from mark or the last N seconds, speed, pause), `RenameInput` (multiview label), multiview outputs.
- Desktop: **Graphics Studio** (templates, live field editing, timers, overlay channels with key, placement presets for a basic 2D virtual set, fade IN/OUT/CUT) and **Replay & Multiview** (multiview monitor, replay inputs with MARK IN / PLAY MARK / LAST 5 s / speed / pause / to preview); multiview choice in Streaming & Recording. The self-test also checks a lower third on an overlay, the multiview monitor and a replay at 50 %.
- ADR-0011; API contract §M6 incl. the scene format; engine design §12; FreeType and DejaVu in the licence inventory (licence texts packaged).
- 13 native tests (154 total), 2 managed tests (46 total).

### Changed
- Program pool 16 → 18 frames; input pools +3 (replay queue).

### Fixed
- An overlay channel was emptied when the input list was rebuilt (the WPF ComboBox pushed a null selection into the view model) — found by the self-test.

## [0.5.0] — 2026-11-21 — Milestone 5: Streaming and recording

### Added
- C ABI 0.5.0 (60 exports): outputs (add/remove/start/stop, statistics) and `mixit_query_video_encoders`.
- Output engine (ADR-0010): H.264 + AAC via FFmpeg; encoder chosen by open-testing, hardware first (NVENC, Quick Sync, AMF, Media Foundation, then OpenH264/x264); one encoder shared by outputs with the same settings; timestamps from the session timeline (recorded MP4: flash/click offset 0.02 ms).
- Destinations, each with its own thread and bounded queue: files (fragmented MP4/MOV, MKV, MPEG-TS, FLV), RTMP/RTMPS, SRT, UDP/RTP/TCP (MPEG-TS), HLS to a folder. Reconnect with back-off; drop-to-keyframe when a destination falls behind (counted; data loss flagged for files); segmentation; free-space stop; recordings never overwrite files; folders created as needed.
- ISO recording of any input at its own size with its clean audio.
- Stream keys, passwords and query values redacted ("***") in logs, errors and the UI.
- Managed: `OutputSettings`, `OutputStatistics`, `VideoEncoderInfo`, `UrlRedaction`, `NativeSession` output calls; `ProductionService` outputs with stable ids across session rebuilds (ISO outputs follow their input).
- Desktop: **Streaming & Recording** section — add file/RTMP(S)/SRT/HLS destinations for the program or an input, bitrate, encoder test, segment length, stream key in a password box; live state, bitrate, buffer, drops, errors; Start/Stop/Remove. The self-test records 2.5 s and checks the finalized file.
- ADR-0010; API contract §outputs; engine design §11.
- 10 native tests (141 total) that decode what was written; 2 managed tests (44 total).

### Changed
- Program pool 10 → 16 frames and input pools +3 (encoder queues).
- `AsyncCommand.ExecuteAsync()` for callers that must wait.

## [0.4.0] — 2026-11-19 — Milestone 4: Audio engine

### Added
- C ABI 0.4.0 (54 exports): audio mixer (strips, buses, monitor source, meters, loudness reset, bus audio read), audio devices (enumeration, monitor output, audio capture inputs — EXPERIMENTAL), VST3 scan and inserts (EXPERIMENTAL). Appended audio counters to `session_stats` / `input_stats`; 0.2/0.3 struct sizes still accepted.
- Audio mixer thread on the session sample timeline (10 ms blocks), per-input timeline buffers with zero-filled gaps and underrun counting, per-input delay (lip-sync), lock-free parameter/meter exchange, ramped gains (ADR-0009).
- Channel strip: trim, high-pass, gate/expander, 4-band parametric EQ, compressor with soft knee, fader, mute, solo (after-fader listen), balance, bus sends; audio-follows-video with transition crossfades.
- Buses: master + aux A–D with fader, mute, limiter (master −1 dBFS by default); monitor bus (solo or selected source).
- Metering: ballistic peak, RMS, clip, gain reduction; BS.1770-4 / EBU R128 momentary, short-term and gated integrated loudness.
- Device clock bridge: SPSC ring + drift-compensating resampler (PI controller, ±0.5 %); WASAPI shared-mode output (event-driven, MMCSS) and capture inputs with device-loss retry; built-in `mixit:null[:ppm]` test device.
- VST3 host on the pinned MIT `vst3_pluginterfaces` (ADR-0008): scan, load, mono/stereo processing, parameters, bypass, state; 2 insert slots per strip, swappable while running. A VST3 test plug-in is built with the tests.
- Managed: audio types and bindings, `ProductionService` audio API (settings, inserts and plug-in state survive session rebuilds; plug-ins load on a worker thread).
- Desktop: **Audio Mixer** — strips with meters, fader, mute/solo/AFV, bus sends; master/aux/monitor buses with loudness; detail editor (trim, delay, high-pass, balance, EQ, gate, compressor); monitor/headphone output and audio-input devices; VST3 insert slots with a generic parameter editor. The self-test checks master level, a fader move, the plug-in scan and the insert slots.
- ADR-0008 (VST3 in-process host), ADR-0009 (audio timeline and clock bridge).
- 28 native tests (131 total), 5 managed tests (42 total).

### Changed
- CI no longer uploads the Windows package unless the repository variable `MIXIT_PUBLISH_PACKAGE` is `true` (owner decision: no downloadable build before M9). The Release build, smoke test and self-test still run on every push.
- A/V click detection moved to the audio thread (sample-exact); the program audio is now the mixed master.

### Fixed
- Mute ramps ended a few millionths above zero (accumulated float error); the last frame of a ramp is now exact.
- The drift resampler oscillated after start-up with excess buffered audio; start-up/stall resync discards the excess.
- A plug-in parameter changed while the session was stopped was missing from the saved plug-in state.

## [0.3.0] — 2026-11-17 — Milestone 3: First working video pipeline

### Added
- C ABI 0.3.0 (33 exports): media-file inputs, preview bus, cut, auto transitions (mix, fade through black, wipe), 8 overlay layers per bus, bus frame copy, compositor preference, capture device enumeration and capture inputs (experimental). Appended struct fields; 0.2 struct sizes still accepted.
- `MediaSource` / `SourceIo` abstraction; test signal moved behind it.
- FFmpeg file source: demux/decode, BGRA conversion (BT.709/601), resampling to the session format, pts pacing, gap-free A/V-locked looping, `file` protocol only.
- Compositor interface with a CPU reference implementation (separable bilinear, crop, straight alpha, opacity, band-parallel) and an EXPERIMENTAL Direct3D 11 implementation (runtime-compiled shaders, WARP support, automatic CPU fallback on GPU failure).
- Preview/program buses, aspect-preserving fit, zero-copy fast path, preview rendered only while watched; transitions advance with output time.
- Media Foundation capture source (EXPERIMENTAL): device enumeration, mode selection, RGB32 conversion, reconnect on device loss; MF resolved at run time (engine loads on Windows N).
- Managed: `NativeSession` bus/transition/layer/capture API, `CompositorPreference`, `ProductionService` (MixIt.Core).
- Desktop: **Production Studio** — preview/program monitors with tally, input tiles, CUT, AUTO, transition type and duration, PiP, format selection, test-signal/media-file/capture inputs, GPU compositor toggle (experimental). `MixIt.exe --self-test production`.
- `mixit-probe`: `--file`, `--compositor auto|cpu|d3d11|warp`, `--list-capture`, `--capture N`; the session soak now exercises PiP, preview, a transition and a cut.
- Windows CI uses a pinned LGPL FFmpeg 9.0.2 (BtbN build, SHA-256 checked) and ships its DLLs and licence in the package; the package step runs the production self-test.
- ADR-0005 (compositor), ADR-0006 (WPF monitors), ADR-0007 (capture).
- 28 native tests (103 total), 7 managed tests (37 total).

### Fixed
- Possible null dereference in the zero-copy program path (found by GCC `-Wnull-dereference` at -O2).
- Transitions lasted longer than requested when output ticks were skipped under load.
- Test media writer left the last MP4 frame without a duration, so FFmpeg 9 dropped it (Windows CI 44/45 frames); reproduced locally with the same FFmpeg build.
- Windows: test files deleted while the session still had them open.
- TSan false positives inside FFmpeg's threads suppressed narrowly (`libavutil`/`libavcodec` only).

### Changed
- `mixit_session_config` 32 → 40 bytes, `mixit_session_stats` 96 → 136, `mixit_input_stats` 112 → 128 (older sizes accepted).
- Program scaling uses the compositor (bilinear) instead of nearest-neighbour.

## [0.2.0] — 2026-11-17 — Milestone 2: Media pipeline foundation

### Added
- C ABI 0.2.0 (25 exports): sessions (`mixit_session_*`: create/destroy, test-signal inputs, program selection, start/stop, statistics, program frame copy) and media library queries (`mixit_get_media_library_info`, `mixit_query_codec`).
- Exact media time base (rational rates, portable 128-bit muldiv; frame/sample times computed from indices — no drift).
- Fixed-capacity, ref-counted frame and audio pools; bounded media queues with DropNewest/DropOldest policies and counters.
- Deterministic test signal: colour bars, moving stripe, machine-readable frame code with sentinel, flash frames with sample-aligned clicks, tone.
- Session pipeline: paced producer per input, output clock, frame synchronization (repeat/skip), program bus (zero-copy, nearest scaling, black), cuts while running, A/V alignment measurement, stall recovery.
- Precise pacing: absolute `clock_nanosleep` (Linux), high-resolution waitable timers (Windows).
- Optional FFmpeg integration with runtime LGPL licence guard and version check (ADR-0004); CMake `MIXIT_WITH_FFMPEG`, `MIXIT_REQUIRE_LGPL_FFMPEG`, `FFMPEG_ROOT`.
- Managed `NativeSession`, `MediaRate`, statistics records, media library API; `EngineHost.CreateSession`.
- `mixit-probe --codecs` and `--session-test N [--format WxH@rate]` soak test with PASS/FAIL criteria; `verify-linux.sh --soak N`.
- 35 native tests (75 total) and 5 managed tests (30 total).

### Fixed
- Uniform (e.g. black) images decoded as a valid frame code — sentinel added.
- Quick Sync codecs not reported as hardware-backed.

## [0.1.0] — 2026-11-17 — Milestone 1: Foundation

### Added
- Native engine `mixit_engine` (C++20) with a versioned C ABI (`mixit_engine.h`, API 0.1.0): engine create/shutdown/destroy, diagnostics, logging, capability detection, GPU adapter queries, monotonic clock, per-thread last error.
- Opaque, never-reused engine handles validated through a registry; safe against concurrent destroy.
- Lock-free bounded MPMC queue; engine logger with JSON-lines file sink and host callback; drop-and-count overload policy.
- Capability probes: CPUID (SSE2…AVX-512F with OS state check), cores, memory, OS version, DXGI GPU adapters (Windows).
- CMake presets for MSVC, Linux (debug/release/ASan+UBSan/TSan) and a MinGW-w64 Windows cross-check; strict warnings as errors.
- GoogleTest suite (40 tests).
- .NET 10: `MixIt.Interop` (LibraryImport, SafeHandle, callbacks), `MixIt.Core` (EngineHost, EngineLogStream, ILogger→engine forwarding), `mixit-probe` CLI, `MixIt.Desktop` WPF shell (unverified), xUnit tests (25).
- Documentation: architecture, engine design, API contract, build guide, test report, licence inventory, roadmap, status, 3 ADRs.

### Fixed (during M1 verification)
- Windows-only test bug (deleting an open file), DXGI-unavailable handling under Wine, cast-free public header macros, Linux export list, MinGW DLL name, MSVC C4324, three WPF compile errors.
