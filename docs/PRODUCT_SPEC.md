# MIX IT — Product Specification and Feature Status

This is the authoritative feature-status matrix. A feature is listed as
anything other than PLANNED only with evidence in TEST_REPORT.md.

Status: PLANNED · IN_PROGRESS · EXPERIMENTAL · IMPLEMENTED · TESTED · PRODUCTION_READY

## Platform foundation

| Feature | Status | Milestone |
|---|---|---|
| Native engine lifecycle (initialize / shutdown) | TESTED | M1 |
| Versioned C ABI + .NET interop | TESTED | M1 |
| Structured logging (engine + app, JSON lines) | TESTED | M1 |
| Hardware capability detection — CPU, memory, OS | TESTED | M1 |
| Hardware capability detection — GPU adapters (DXGI) | TESTED (Windows CI: software adapters only) | M1 |
| Codec availability in the FFmpeg build (incl. hardware encoders present) | TESTED | M2 |
| Capture device enumeration (Media Foundation) | EXPERIMENTAL (no real device tested) | M3 |
| Hardware encoder open-test (M5), audio devices (M4), storage free space per recording (M5); network interfaces | IMPLEMENTED / PLANNED (network) | M4–M5 |
| Pipeline monitoring (ticks, lateness, drops, repeats, skips, A/V offset) | TESTED | M2 |
| Runtime monitoring (CPU/GPU load, audio underruns, disk) | PLANNED | M3–M5 |
| Performance presets (Low Resource → Professional) | PLANNED | M8 |
| Hardware Diagnostics screen | IMPLEMENTED (starts in CI; hands-on use pending) | M1/M8 |

## Video

| Feature | Status | Milestone |
|---|---|---|
| Synthetic test sources (video + audio, sync markers) | TESTED | M2 |
| Video files (FFmpeg software decode, looping, A/V locked) | TESTED | M3 |
| Hardware video decode (D3D11VA) | PLANNED | M3 follow-up |
| Still images | PLANNED | M6 |
| Webcams / USB / HDMI capture (Media Foundation; video only) | EXPERIMENTAL (not tested with hardware) | M3 |
| DirectShow capture fallback, capture audio | PLANNED | M4/M5 |
| Blackmagic DeckLink | PLANNED | M5 (optional SDK) |
| NDI input | PLANNED | M5 (optional SDK) |
| RTSP / SRT network inputs | PLANNED | M3/M5 |
| Desktop / screen capture | PLANNED | M6 |
| Program bus + cut | TESTED | M2/M3 |
| Preview bus (rendered only while watched), CUT swaps buses | TESTED | M3 |
| GPU compositor (Direct3D 11) | EXPERIMENTAL (matches CPU reference on WARP; no GPU measurements) | M3 |
| Mix (dissolve), fade through black, wipe; 1–600 frames | TESTED | M3 |
| Custom / stinger transitions | PLANNED | M6 |
| Multi-layer compositing (8 layers per bus), PiP, crop, position, scale, opacity, aspect fit | TESTED | M3 |
| Layer rotation, borders, shadows | PLANNED | M6 |
| Colour correction, chroma key, luma key | PLANNED | M6 |
| Frame synchronization (repeat/skip across rates) | TESTED | M2 |
| Video delay (per input) | PLANNED | M4 (with audio sync) |
| Formats up to 8192², 23.976/24/25/29.97/30/50/59.94/60 fps (session API) | TESTED (CPU path; 1080p60 soak with PiP + transitions on a 2-core host) | M2/M3 |
| Production Studio screen (monitors, tally, inputs, CUT/AUTO, PiP) | TESTED (automated self-test on Windows CI; hands-on use pending) | M3 |

## Audio

| Feature | Status | Milestone |
|---|---|---|
| WASAPI shared-mode monitor output and audio inputs (drift-compensated) | EXPERIMENTAL (no real device tested) | M4 |
| ASIO / WASAPI exclusive | PLANNED — ASIO needs the Steinberg proprietary SDK licence (owner decision) | M4+ |
| Audio from media files and test signals into the mixer | TESTED | M4 |
| Trim, fader, balance, mute, solo (after-fader listen), audio-follows-video with crossfade | TESTED | M4 |
| Mono/stereo sessions; meters up to 8 channels | TESTED (mono/stereo); multichannel routing PLANNED | M4 |
| Peak / RMS / LUFS (momentary, short-term, integrated, BS.1770-4) metering | TESTED | M4 |
| Master + 4 aux buses, routing, per-input delay, monitor bus and source | TESTED | M4 |
| High-pass, 4-band parametric EQ (shelves/peaks), gate/expander, compressor, bus limiters | TESTED | M4 |
| VST3 inserts: scan, load, bypass, parameters, state (2 slots per strip) | EXPERIMENTAL — in-process (ADR-0008); tested with a test plug-in only | M4 |
| VST3 isolation (out-of-process), editors, latency compensation | PLANNED | M8–M9 |

## Streaming and recording

| Feature | Status | Milestone |
|---|---|---|
| RTMP (tested against a local server) / RTMPS (TLS by FFmpeg; not yet tested against a TLS server) | TESTED / EXPERIMENTAL | M5 |
| SRT caller (tested against a local listener); listener/rendezvous via URL options | TESTED (caller) | M5 |
| HLS to a folder (playlist + segments); HTTP delivery | TESTED (folder) / PLANNED (HTTP) | M5 |
| Multiple isolated destinations, shared encoders, reconnection with back-off, statistics | TESTED | M5 |
| Encoders: NVENC, Quick Sync, AMF, Media Foundation, OpenH264 (open-tested, hardware first) | TESTED (software); hardware EXPERIMENTAL (no GPU in CI) | M5 |
| Program and ISO recording, MP4/MOV (fragmented)/MKV/TS, segmentation, no overwrite, free-space stop | TESTED | M5 |
| Audio-only and multi-track audio recording | PLANNED | M6+ |

## Graphics, production, automation, outputs

| Feature | Status | Milestone |
|---|---|---|
| Text, PNG overlays, lower thirds, titles, clocks, countdowns, scoreboards | PLANNED | M6 |
| Animated and HTML graphics, template editor | PLANNED | M6 |
| Virtual studio (2D; 3D architecture only) | PLANNED | M6 |
| Instant replay, slow motion, playlists | PLANNED | M6 |
| Multiview, external displays, clean feed, NDI output | PLANNED | M6 |
| Macros, scheduled/triggered automation, remote control API | PLANNED | M7 |
| PTZ, tally | PLANNED | M7 |
| Projects (SQLite + JSON import/export, schema migration) | PLANNED | M2/M8 |
| Free / Professional licensing | PLANNED | M9 |
