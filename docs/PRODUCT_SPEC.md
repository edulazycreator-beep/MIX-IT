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
| Hardware capability detection — GPU adapters (DXGI) | EXPERIMENTAL (Wine only) | M1 |
| Codec availability in the FFmpeg build (incl. hardware encoders present) | TESTED | M2 |
| Hardware encoder open-test, audio/capture devices, network, storage | PLANNED | M3–M5 |
| Pipeline monitoring (ticks, lateness, drops, repeats, skips, A/V offset) | TESTED | M2 |
| Runtime monitoring (CPU/GPU load, audio underruns, disk) | PLANNED | M3–M5 |
| Performance presets (Low Resource → Professional) | PLANNED | M8 |
| Hardware Diagnostics screen | IMPLEMENTED (unverified) | M1/M8 |

## Video

| Feature | Status | Milestone |
|---|---|---|
| Synthetic test sources (video + audio, sync markers) | TESTED | M2 |
| Video files, images | PLANNED | M3 |
| Webcams / USB / HDMI capture (Media Foundation, DirectShow fallback) | PLANNED | M3 |
| Blackmagic DeckLink | PLANNED | M3 |
| NDI input | PLANNED | M3 |
| RTSP / SRT network inputs | PLANNED | M3/M5 |
| Desktop / screen capture | PLANNED | M3 |
| Program bus + cut (CPU reference path, test sources) | TESTED | M2 |
| Preview bus, GPU program path | PLANNED | M3 |
| Dissolve, fade, wipe, custom transitions | PLANNED | M3/M6 |
| Multi-layer compositing, PiP, crop/position/scale/rotate | PLANNED | M3/M6 |
| Colour correction, chroma key, luma key | PLANNED | M6 |
| Frame synchronization (repeat/skip across rates) | TESTED | M2 |
| Video delay (per input) | PLANNED | M3 |
| Formats up to 8192², 23.976/24/25/29.97/30/50/59.94/60 fps (session API) | TESTED (CPU path; soaks at 720p29.97 and 1080p59.94) | M2 |

## Audio

| Feature | Status | Milestone |
|---|---|---|
| WASAPI / ASIO capture & output | PLANNED | M4 |
| Gain, fader, pan, mute, solo, mono/stereo/multichannel | PLANNED | M4 |
| Peak / RMS / LUFS metering | PLANNED | M4 |
| Master + aux buses, routing, delay, monitoring | PLANNED | M4 |
| EQ (parametric, 3-band), compressor, limiter, gate, expander, filters | PLANNED | M4 |
| VST3 hosting (scan, load, bypass, parameters, state, isolation) | PLANNED | M4 |

## Streaming and recording

| Feature | Status | Milestone |
|---|---|---|
| RTMP / RTMPS (TLS validated) | PLANNED | M5 |
| SRT caller / listener / rendezvous | PLANNED | M5 |
| HLS with secured HTTP delivery | PLANNED | M5 |
| Multiple isolated destinations, reconnection, statistics | PLANNED | M5 |
| Encoders: software, NVENC, Quick Sync, AMF (detected) | PLANNED | M5 |
| Program / ISO / audio recording, MP4/MOV/MKV, segmentation, recovery | PLANNED | M5 |

## Graphics, production, automation, outputs

| Feature | Status | Milestone |
|---|---|---|
| Text, PNG overlays, lower thirds, titles, clocks, countdowns, scoreboards | PLANNED | M6 |
| Animated and HTML graphics, template editor | PLANNED | M6 |
| Virtual studio (2D; 3D architecture only) | PLANNED | M6 |
| Instant replay, slow motion, playlists | PLANNED | M6 |
| Multiview, external displays, clean feed, NDI output | PLANNED | M3/M6 |
| Macros, scheduled/triggered automation, remote control API | PLANNED | M7 |
| PTZ, tally | PLANNED | M7 |
| Projects (SQLite + JSON import/export, schema migration) | PLANNED | M2/M8 |
| Free / Professional licensing | PLANNED | M9 |
