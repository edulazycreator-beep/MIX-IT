# MIX IT — Roadmap

Status legend (CLAUDE.md §16.I): PLANNED · IN_PROGRESS · EXPERIMENTAL · IMPLEMENTED · TESTED · PRODUCTION_READY.
A milestone is closed only when its exit criteria are met **on Windows**.

| # | Milestone | Status | Exit criteria |
|---|---|---|---|
| 1 | Foundation | IMPLEMENTED (Linux/Wine-tested; Windows/MSVC verification pending) | MSVC build + native/managed tests green on Windows; WPF shell runs and shows real capability data |
| 2 | Media pipeline foundation | IMPLEMENTED on dev host (input abstraction + 1 h Windows soak pending) | synthetic A/V session runs 1 h with < 1 ms drift; no steady-state allocation; queues bounded under overload |
| 3 | First working video pipeline / video engine | PLANNED | file → GPU → preview/program → cut, 1080p60 without drops on reference HW; native monitors in WPF |
| 4 | Audio engine | PLANNED | WASAPI (+ASIO) capture, mixer, buses, DSP, meters/LUFS, sync; no underruns in 1 h soak; VST3 host design (out-of-process) |
| 5 | Streaming and recording | PLANNED | RTMP/RTMPS/SRT/HLS multi-destination with isolation; NVENC/QSV/AMF detection; program + ISO recording with recovery |
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
| Media input abstraction | IN_PROGRESS — concrete test-signal input; interface extracted in M3 |
| Synthetic video + audio source (test signal with sync markers) | TESTED |
| Frame/audio buffer pools, rational time base, timestamps on the master clock | TESTED |
| Bounded media queues with stage policies, diagnostics counters | TESTED |
| Basic frame processing (zero-copy / nearest scale / black) | TESTED |
| Media diagnostics (session + per-input stats, A/V offset) | TESTED |
| Long-run drift | arithmetic proven; 60 s soak PASS; 1 h on Windows pending |
| Benchmarks | PLANNED |

## Milestone 3 — First working video pipeline (planned scope)

File decode (D3D11VA where available) → GPU conversion → compositor → preview & program buses → cut transition → native swap-chain monitors hosted in WPF; frame-accurate switching and sync verification.
