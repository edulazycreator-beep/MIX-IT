# MIX IT — Development Status

_Last updated: 2026-11-21 (Milestone 5)_

**Current milestone:** M5 — Streaming and recording → **IMPLEMENTED**, tested on Linux and Windows CI (commit cc2cc38). Hardware encoders and real streaming platforms not yet exercised.
**Previous milestones:** M4 IMPLEMENTED (real audio devices / commercial plug-ins pending); M3 IMPLEMENTED (reference-hardware checks pending); M1/M2 TESTED.
**Next milestone:** M6 — Graphics and production tools.
**Owner decision in force (2026-11-19):** no downloadable package/ZIP until M9. CI still builds, smoke-tests and self-tests the Release package on every push; upload only with repository variable `MIXIT_PUBLISH_PACKAGE=true`.

## Completed in M5

| Item | Status | Evidence |
|---|---|---|
| Encoder pipeline: H.264 + AAC, shared by outputs with equal settings, timeline timestamps (ADR-0010) | TESTED | recorded MP4 flash/click offset 0.02 ms (Linux x264, LGPL OpenH264); Windows CI with Media Foundation H.264 within the 40 ms limit after the fix below |
| Automatic encoder choice by open-test (NVENC, Quick Sync, AMF, Media Foundation, OpenH264, x264) | TESTED (software encoders) / EXPERIMENTAL (hardware) | Windows CI picked `h264_mf`; no GPU encoder available in CI |
| Program recording: fragmented MP4/MOV, MKV, TS, FLV; segments; never overwrite; folder creation; free-space stop | TESTED | decoded back in tests |
| ISO recording (input size, clean audio) | TESTED | `Outputs.IsoRecordsAnInputAtItsOwnSize` |
| RTMP streaming | TESTED | local FFmpeg RTMP listener |
| SRT streaming (caller) | TESTED | local SRT listener |
| HLS to a folder | TESTED | playlist decoded |
| RTMPS | EXPERIMENTAL | TLS from FFmpeg; never run against a TLS server |
| Destination isolation, drop-to-keyframe, reconnect with back-off, stop cancels connecting | TESTED | broken destination test (Linux + Windows) |
| Stream keys/passwords redacted in logs, errors, UI | TESTED | native + managed tests |
| C ABI 0.5.0 (60 exports) + managed bindings + `ProductionService` outputs | TESTED | xUnit 44/44 |
| WPF Streaming & Recording section | TESTED (automated) | self-test records 2.5 s and checks the finalized file |

## Defects found during M5 (fixed)

1. **Windows/Media Foundation: audio 64–85 ms early in recordings.** Audio packets that left the encoder before the first (delayed) video packet were dropped by the destination, so audio started later than the file's timeline. Destinations now hold that audio and keep what belongs after the first keyframe. Reproduced locally with a look-ahead x264: 94.6 ms before, 0.02 ms after.
2. A stop while a destination was connecting reported FAILED; a stop interrupted the file trailer (broken MKV). Stop now cancels only connection attempts; trailers are never interrupted.
3. Packet buffers were shared between encoder and destination threads (TSan reports through uninstrumented FFmpeg); each destination now gets its own copy.
4. Codec parameters leaked once per encoder (ASan).

## Deviations from the M5 plan (honest list)

| Planned | As built | Why / next |
|---|---|---|
| Hardware encoders verified | detected and open-tested; only software encoders ran in CI | needs a PC with NVIDIA/Intel/AMD GPU |
| Real platforms (YouTube, Facebook, …) | local RTMP/SRT servers only | needs stream keys; operator test |
| HLS over HTTP | folder only | later (needs an upload target) |
| Audio-only and multi-track recording | not built | M6+ |
| DeckLink / NDI | not started | optional vendor SDKs (owner) |

## Still unverified (needs a Windows workstation)

| Item | How to check |
|---|---|
| Hardware encoder (NVENC / Quick Sync / AMF) chosen and stable for 1 h | Streaming & Recording → Test (encoder list) → record 1 h; watch "frames not encoded" |
| Streaming to a real platform (RTMPS) incl. reconnect after unplugging the network | add RTMPS destination with key, start, pull the cable for 30 s |
| M4 items: audio devices, commercial VST3 plug-ins | see M4 section |
| M3 items: capture devices, GPU compositor, 1080p60 file, 1 h soak | see M3 section |

## Build status

| Target | Result |
|---|---|
| Windows: MSVC `/W4 /WX`, .NET 10 incl. WPF | ✅ 0 warnings (CI) |
| Linux Debug / Release / ASan+UBSan / TSan | ✅ 0 warnings |
| MinGW-w64 cross-build | ✅ 0 warnings |
| Windows package (self-contained) | ✅ built + self-tested in CI; **not uploaded** (owner decision until M9) |

## Test results

Native **141/141** (Windows CI; Linux Debug/Release/ASan/TSan) · managed **44/44** (Windows CI xUnit; local runner) · production self-test PASS (incl. a recording). Details: [docs/TEST_REPORT.md](docs/TEST_REPORT.md).

## Known issues

1. **Patent/licence decision before release:** H.264/AAC encoding and the OpenH264 copy inside the LGPL FFmpeg build (THIRD_PARTY_LICENSES ⚖️).
2. Encoding converts BGRA → YUV on the CPU; at 1080p60 with several distinct encodes this costs CPU (frames are not GPU-resident yet, ADR-0005).
3. FFmpeg's own messages go to stderr (urls in them are redacted the same way), not into the MIX IT log files.
4. M4 known issues (in-process VST3, no plug-in crossfade) and M3 known issues still apply.

## Next recommended tasks

1. **M6 — Graphics and production tools** (now): titles/lower thirds/clock/scoreboard with live text, image overlays, chroma and luma key per layer, multiview, instant replay with slow motion, virtual set presets.
2. **Owner:** H.264/AAC licensing route; ASIO licence; code-signing certificate; licence activation server.
3. On a Windows workstation: the "Still unverified" checks.

---

# Milestone 4 status (kept for reference)

## Completed in M4

| Item | Status | Evidence |
|---|---|---|
| Audio thread on the session sample timeline, 10 ms blocks, per-input timeline buffers, underrun/lateness counters (ADR-0009) | TESTED | mixer + A/V tests (Windows + Linux CI) |
| Channel strip: trim, high-pass, gate/expander, 4-band EQ, compressor, fader, mute, solo, balance, sends, delay | TESTED | 12 DSP tests + mixer tests |
| Audio-follows-video incl. transition crossfades | TESTED | `AudioMixer.AudioFollowsVideoSwitchesWithTheProgram` |
| Master + aux A–D (fader, mute, limiter), monitor bus (solo / source) | TESTED | routing/solo/monitor tests |
| Meters: peak (ballistic), RMS, clip, gain reduction; loudness M/S/I (BS.1770-4) | TESTED | −23 LUFS reference tone within ±0.1 LU |
| Drift-compensating bridge to device clocks; null test device | TESTED | 400 s ±ppm simulation; device tests on the null device |
| WASAPI shared-mode monitor output and audio capture inputs, device-loss retry, MMCSS | EXPERIMENTAL | compiles and enumerates on Windows CI; **no real device exercised** |
| VST3 inserts (2 per strip): scan, load while running, parameters, bypass, state (ADR-0008) | EXPERIMENTAL | real `.vst3` test plug-in loaded and processed on Windows and Linux CI; commercial plug-ins untested; in-process |
| C ABI 0.4.0 (54 exports) + managed bindings | TESTED | layout tests, xUnit 42/42 |
| `ProductionService` audio API (state survives rebuilds; async plug-in load) | TESTED | xUnit |
| WPF Audio Mixer (strips, buses, monitor, meters, loudness, detail editor, devices, plug-in slots) | TESTED (automated) | self-test PASS on Windows CI |

## Deviations from the M4 plan (honest list)

| Planned | As built | Why / next |
|---|---|---|
| ASIO capture/output | not built; adapter point is the `AudioOutput`/capture interface | ASIO SDK needs Steinberg's **proprietary** licence (GPLv3 option unusable) — **owner decision needed** |
| VST3 host out-of-process | in-process, with exception barriers and safe hand-over | out-of-process host keeps the same API; planned before release (ADR-0008) |
| Multichannel (>2) routing | mono/stereo mixing; meters up to 8 ch | later, with output channel mapping |
| Audio from video capture devices | not yet (MF capture is video only) | use a WASAPI audio input for now |
| 1 h audio soak without underruns on hardware | not run | needs a Windows workstation with an audio device |

## Still unverified (needs a Windows workstation)

| Item | How to check |
|---|---|
| Monitor output on headphones/speakers; dropouts; drift over 1 h | Audio Mixer → Headphones → device; play the test tone; watch "dropouts" and the drift ratio |
| Audio inputs (USB mic, line-in, capture card audio endpoint) | Audio Mixer → Audio input → + Add |
| Commercial VST3 effects (EQ, compressor, reverb) | Audio Mixer → Scan → pick → Load on a strip |
| Everything listed for M3 (capture, GPU, 1080p60 file, 1 h soak) | see the M3 section below |

## Build status

| Target | Result |
|---|---|
| Windows: MSVC `/W4 /WX`, .NET 10 incl. WPF | ✅ 0 warnings (CI) |
| Linux Debug / Release / ASan+UBSan / TSan | ✅ 0 warnings |
| MinGW-w64 cross-build | ✅ 0 warnings |
| Windows package (self-contained) | ✅ built + self-tested in CI; **not uploaded** (owner decision until M9) |

## Test results

Native **131/131** (Windows CI; Linux Debug/Release/ASan; TSan at `-j2`) · managed **42/42** (Windows CI; local runner) · production self-test PASS. Details: [docs/TEST_REPORT.md](docs/TEST_REPORT.md).

## Known issues

1. VST3 plug-ins run in the engine process: a crashing plug-in stops the engine. Plug-in latency is reported, not compensated; no plug-in editor windows yet.
2. Loading, removing or bypassing a plug-in switches at a block boundary without a crossfade, so a large level change is not smoothed.
3. Under heavy CPU oversubscription (TSan at 3× cores) some wall-clock tests fail intermittently; no data races reported.
4. Items 1–4 and 6 of the M3 list below still apply (CPU compositor cost, 25 Hz monitors, capture timestamps, inputs editable only while stopped, rebuild on format change).

## Next recommended tasks

1. **M5 — Streaming and recording** (now): H.264/AAC encoding through FFmpeg with hardware encoder detection (NVENC/QSV/AMF/MF, OpenH264 fallback), program recording (MP4 fragmented/MOV/MKV) with recovery and segmentation, RTMP/RTMPS/SRT/HLS destinations each isolated with reconnect, ISO recording, storage monitoring.
2. **Owner:** decide on the ASIO SDK licence (Steinberg proprietary agreement), code-signing certificate and the licence activation server (needed by M9).
3. On a Windows workstation: the "Still unverified" checks.

---

# Milestone 3 status (kept for reference)

## Completed in M3 (CI run 44041552714)

| Item | Status | Evidence |
|---|---|---|
| `MediaSource` / `SourceIo` abstraction (test signal, file, capture behind one interface) | TESTED | all session and file tests run through it |
| FFmpeg file input: decode, BGRA (BT.709/601), resample, pts pacing, A/V-locked looping, `file` protocol only | TESTED | 7 file tests; Windows CI with pinned LGPL FFmpeg 9.0.2; local FFmpeg 6.1 and 9.0.2 |
| Compositor interface + CPU reference (bilinear, crop, alpha, opacity, letter/pillarbox, mix/fade/wipe), band-parallel | TESTED | 8 pixel-exact tests; 640×360→1080p in 4.9 ms (Release, 2 cores) |
| Direct3D 11 compositor (runtime shaders, WARP, CPU fallback on GPU failure) | EXPERIMENTAL | equals CPU reference within ±4 on WARP (CI); no real GPU measured |
| Preview/program buses, 8 layers per bus, cut, auto transitions, preview rendered only while watched | TESTED | 10 bus tests incl. PiP decoded from pixels, transition timing under load |
| Video capture abstraction + Media Foundation backend (enumerate, mode choice, RGB32, reconnect) | EXPERIMENTAL | Windows CI: enumeration + "no device" paths only |
| C ABI 0.3.0 (33 exports), backwards-compatible struct growth | TESTED | compat tests with 0.2 struct sizes |
| Managed bindings + `ProductionService` (MixIt.Core) | TESTED | 37 xUnit tests (CI) |
| WPF Production Studio: monitors, tally, input tiles, CUT/AUTO, transition type/duration, PiP, formats, capture list, GPU toggle | TESTED (automated) | `MixIt.exe --self-test production` PASS on Windows CI |
| `mixit-probe`: `--file`, `--compositor`, `--list-capture`, `--capture N`; soak exercises PiP/preview/mix/cut | TESTED | 1080p60 soak PASS (Release, 0 late ticks) |
| ADR-0005/0006/0007, API contract 0.3, engine design §8–9, docs | DONE | |

## Deviations from the M3 plan (honest list)

| Planned | As built | Why / next |
|---|---|---|
| Decode with D3D11VA, frames stay on GPU | software decode, CPU-resident frames | no GPU on the dev host or CI to develop/verify against; ADR-0005 lists the steps |
| GPU compositor as the program path | CPU compositor is default; D3D11 opt-in, EXPERIMENTAL | with CPU-resident frames the GPU path pays upload + readback; no GPU data yet to justify switching (ADR-0005) |
| Native swap-chain monitors in WPF | WriteableBitmap monitors, 25 Hz | ADR-0006; swap chains in M8 |
| DeckLink / NDI inputs in M3 | not started | optional vendor SDKs; planned as further `MediaSource` backends (M5) |

## Still unverified (needs a Windows workstation)

| Item | How to check |
|---|---|
| Hands-on use of the WPF app (layout, DPI, interaction, monitor smoothness) | run `MixIt.exe` (package publishing paused until M9: build locally with `scripts/package-windows.ps1`) |
| M3 exit criterion: 1080p60 file to program without drops | `mixit-probe --session-test 600 --format 1920x1080@60 --file clip.mp4` |
| GPU compositor on real hardware, CPU vs GPU cost | `mixit-probe --session-test 60 --format 1920x1080@60 --compositor d3d11` vs `cpu` |
| Capture with real webcams / capture cards (incl. a 4-input card) | `mixit-probe --list-capture`, then `--session-test 60 --capture 0`; in the app: ↻, select device, + Capture |
| 1-hour drift soak (M2) | `mixit-probe --session-test 3600 --format 1920x1080@60000/1001` |

## Known issues

1. The CPU compositor's cost grows with resolution × layers; a Debug native build cannot hold 1080p60 with compositing (Release can, even on 2 cores).
2. Monitors poll at 25 Hz and skip frames at 50/60 fps by design (confidence monitors, ADR-0006).
3. Capture: frames are stamped on arrival (device latency appears as a constant offset); no capture audio; a device that stalls without returning could delay stopping the session (ADR-0007).
4. Inputs can be added/removed only while production is stopped.
5. ~~Session audio is used for A/V measurement only~~ — resolved in M4.
6. Changing format or toggling the GPU compositor rebuilds the session (allowed only while stopped).

