# MIX IT — Roadmap

Status legend (CLAUDE.md §16.I): PLANNED · IN_PROGRESS · EXPERIMENTAL · IMPLEMENTED · TESTED · PRODUCTION_READY.
A milestone is closed only when its exit criteria are met **on Windows**.

| # | Milestone | Status | Exit criteria |
|---|---|---|---|
| 1 | Foundation | TESTED (Linux + Windows CI: MSVC, WPF build/start, xUnit); hands-on and real-GPU checks pending | MSVC build + native/managed tests green on Windows; WPF shell runs and shows real capability data |
| 2 | Media pipeline foundation | TESTED (Linux + Windows CI); 1 h soak on Windows hardware pending | synthetic A/V session runs 1 h with < 1 ms drift; no steady-state allocation; queues bounded under overload |
| 3 | First working video pipeline / video engine | IMPLEMENTED — tested on Linux + Windows CI; reference-hardware run, real capture devices and GPU measurements pending (details below) | file → GPU → preview/program → cut, 1080p60 without drops on reference HW; native monitors in WPF |
| 4 | Audio engine | IMPLEMENTED — tested on Linux + Windows CI; real audio devices, ASIO (licence decision) and commercial plug-ins pending (details below) | WASAPI (+ASIO) capture, mixer, buses, DSP, meters/LUFS, sync; no underruns in 1 h soak; VST3 host design (out-of-process) |
| 5 | Streaming and recording | IN_PROGRESS | RTMP/RTMPS/SRT/HLS multi-destination with isolation; NVENC/QSV/AMF detection; program + ISO recording with recovery |
| 6 | Graphics and production | PLANNED | titles/overlays/chroma/luma key, multiview, replay, virtual sets (2D); HTML renderer ADR |
| 7 | Advanced features | PLANNED | command API, macros/automation, remote control (documented protocol), PTZ, tally, plugin SDK |
| 8 | Desktop experience | PLANNED | full operator UI, workspace customization, profiles, diagnostics dashboard, performance presets |
| 9 | Commercial release | PLANNED | Free/Professional licensing (signed licences, offline grace), installer, compatibility lab, legal review, docs |

## Milestone 1 — Foundation (detail)

| Deliverable | Status |
|---|---|
| Repository structure | TESTED |
| CMake configuration (presets, strict warnings, sanitizers, cross-check) | TESTED (GCC, MinGW) · MSVC pending |
| .NET solution (`MixIt.sln`, `MixIt.Engine.slnf`, central packages) | IMPLEMENTED |
| Native engine library + C ABI | TESTED |
| C# interop layer | TESTED (local runner) |
| Engine initialization and shutdown | TESTED |
| Structured logging | TESTED |
| Basic error handling | TESTED |
| Capability detection interface (CPU/memory/OS/GPU) | TESTED (Linux), EXPERIMENTAL (Windows: Wine only) |
| Unit tests (native 40, managed 25) | TESTED |
| Build documentation | DONE |
| WPF shell with Hardware Diagnostics | IMPLEMENTED, unverified |

## Milestone 2 — Media pipeline foundation (detail)

| Deliverable | Status |
|---|---|
| ADR-0004 FFmpeg sourcing + licence policy | DONE (pinned Windows build still to be produced) |
| FFmpeg initialization, version + LGPL licence guard, codec availability | TESTED |
| Session API (`mixit_session_*`, lifecycle steps 3–8) | TESTED |
| Media input abstraction | TESTED (extracted in M3: `MediaSource` / `SourceIo`) |
| Synthetic video + audio source (test signal with sync markers) | TESTED |
| Frame/audio buffer pools, rational time base, timestamps on the master clock | TESTED |
| Bounded media queues with stage policies, diagnostics counters | TESTED |
| Basic frame processing (zero-copy / nearest scale / black) | TESTED |
| Media diagnostics (session + per-input stats, A/V offset) | TESTED |
| Long-run drift | arithmetic proven; 60 s soak PASS; 1 h on Windows pending |
| Benchmarks | PLANNED |

## Milestone 3 — First working video pipeline (detail)

| Deliverable | Status | Evidence / note |
|---|---|---|
| Media source abstraction (`MediaSource`, `SourceIo`) | TESTED | test signal, file and capture sources share it |
| Video decoding: FFmpeg file input (demux, decode, BGRA conversion, audio resample, pts pacing, A/V-locked looping) | TESTED | 7 file-source tests (raw NUT bit-exact, MP4 MPEG-4/AAC), Linux FFmpeg 6.1 + Windows/Linux FFmpeg 9.0.2 LGPL; probe soak with H.264 file |
| Hardware decode (D3D11VA) | PLANNED | deferred (ADR-0005); software decode only |
| Video capture abstraction + Media Foundation backend | EXPERIMENTAL | enumeration/validation tested on Windows CI (no devices there); **not run with real hardware** (ADR-0007) |
| Compositor: CPU reference (scale, crop, alpha, opacity, letter/pillarbox) | TESTED | 9 pixel-exact tests; 1080p60 soak with PiP + preview + transitions: 0 late ticks (2-core host) |
| GPU rendering: Direct3D 11 compositor | EXPERIMENTAL | matches CPU reference on WARP in CI; no real-GPU measurement yet; opt-in (ADR-0005) |
| Preview and program buses, 8 layers per bus | TESTED | bus tests incl. PiP decoded from pixels |
| Switching: cut, auto transitions (mix, fade, wipe), frame-accurate | TESTED | cut/transition tests, transition duration under load |
| Desktop: Production Studio (monitors, tally, CUT/AUTO, PiP, inputs, formats) | TESTED (automated) | `MixIt.exe --self-test production` PASS on Windows CI; hands-on use pending |
| Native swap-chain monitors | PLANNED (M8) | WriteableBitmap monitors for now (ADR-0006) |
| Exit criterion: 1080p60 file to program without drops on reference HW | PENDING | needs a Windows workstation run: `mixit-probe --session-test 600 --format 1920x1080@60 --file clip.mp4` |

## Milestone 4 — Audio engine (detail)

| Deliverable | Status | Evidence / note |
|---|---|---|
| Audio thread on the session timeline, timeline buffers per input (ADR-0009) | TESTED | CI |
| Mixer: strips (trim, HPF, gate/expander, EQ, compressor, fader, mute, solo, balance, delay, sends), AFV with crossfades | TESTED | DSP + mixer tests |
| Buses: master + 4 aux with limiter, monitor bus | TESTED | |
| Meters: peak/RMS/clip/GR, loudness M/S/I | TESTED | |
| Device clock bridge (drift resampler) | TESTED | simulated null device, 400 s |
| WASAPI output + capture | EXPERIMENTAL | no real device exercised |
| ASIO | PLANNED — blocked on licence decision | Steinberg proprietary SDK agreement needed |
| VST3 inserts (scan, load, parameters, bypass, state) | EXPERIMENTAL | in-process (ADR-0008); test plug-in only |
| VST3 out-of-process host, editors, latency compensation | PLANNED | before M9 |
| Desktop Audio Mixer | TESTED (automated) | self-test |
| Exit: 1 h without underruns on hardware | PENDING | Windows workstation |
