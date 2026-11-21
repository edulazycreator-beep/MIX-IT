# MIX IT — Architecture

Status: **Milestone 3 (first working video pipeline) implemented; M1–M3 tested on Linux and Windows CI; hands-on and real-hardware checks pending**. This document is the architecture plan
required before implementation (master prompt §28). It distinguishes what
exists (✅ implemented and verified), what exists but is unverified (⚠️), and
what is planned (◻). Detailed engine internals live in
[ENGINE_DESIGN.md](ENGINE_DESIGN.md); the exact native interface in
[API_CONTRACT.md](API_CONTRACT.md).

---

## 1. Architecture overview

MIX IT is a single Windows process with two halves joined by one versioned C ABI:

```
┌──────────────────────────── managed (.NET 10) ─────────────────────────────┐
│  MixIt.Desktop (WPF, MVVM)      mixit-probe (CLI)      [M7] Remote API     │
│            │                          │                       │            │
│            └──────────────┬───────────┴───────────────────────┘            │
│                    MixIt.Core  — application services                      │
│        EngineHost · EngineLogStream · NativeEngineLoggerProvider           │
│        ProductionService (M3) · [M5+] Output services · [M7] Automation    │
│        [M9] Licensing · Project store (SQLite + JSON)                      │
│                           │                                                │
│                    MixIt.Interop — the ONLY P/Invoke code                  │
│        LibraryImport · SafeHandle · blittable structs · callback bridge    │
└───────────────────────────┬────────────────────────────────────────────────┘
                            │  C ABI  (mixit_engine.h, versioned, opaque handles)
┌───────────────────────────┴──────────── native (C++20) ────────────────────┐
│  ABI layer: validation · handle registry · exception barrier · last error  │
│  Engine (root object, lifecycle owner)                                     │
│   ├─ Logger (lock-free bounded queue → log thread → JSONL file / callback) │
│   ├─ Capability probes (CPU · memory · OS · GPU/DXGI)  [M2+: encoders,     │
│   │                                       audio/capture devices, storage]  │
│   ├─ Sessions (M2): time base · pools · media queues · test signal ·       │
│   │   frame sync · program bus · A/V measurement · FFmpeg licence guard   │
│   ├─ Video engine (M3): MediaSources (test · file · capture*) → frame      │
│   │   sync → Compositor (CPU · D3D11*) → preview/program buses, layers,    │
│   │   cut/transitions                                                      │
│   ├─ [M4] Audio engine: inputs → mixer/DSP → buses (real-time thread)      │
│   ├─ [M5] Encoders · Streaming outputs · Recording outputs                 │
│   ├─ [M6] Graphics · Replay · Multiview · Virtual studio                   │
│   └─ [M7] Command dispatcher used by automation & remote control           │
└────────────────────────────────────────────────────────────────────────────┘
```

### Responsibility split

| Engine (master prompt §5) | Lives in | Why |
|---|---|---|
| A. Application layer | managed (Desktop + Core) | UI, workspace, projects, preferences, licensing are not latency-critical; C# is productive and safe here. |
| B. Video engine | native | Frame-accurate timing, GPU (D3D11) resources, zero-copy paths. |
| C. Audio engine | native | Hard real-time callback; no GC pauses allowed. |
| D. Graphics engine | native renderer + managed authoring | Rendering composites into the GPU pipeline; template editing is UI. |
| E. Streaming engine | native | Encoders and muxers (FFmpeg) operate on native frames. |
| F. Recording engine | native | Same encoders/muxers; disk I/O on dedicated threads. |
| G. Output engine | native | Swap chains, NDI/DeckLink output, frame pacing. |
| H. Automation engine | managed, issuing native commands | Macros/schedules are not real-time; they call the same command API as the UI and remote control. |

### Rules the architecture enforces

1. **One boundary.** Only `MixIt.Interop` calls native code; only `mixit_engine.h` is exported (verified: the Linux `.so` and the Windows DLL export exactly the 33 `mixit_*` functions of API 0.3).
2. **The engine never depends on the UI thread.** Native work runs on engine-owned threads; UI stalls cannot stall media (master prompt §K).
3. **Bounded everything.** Every queue has a fixed capacity and an explicit overload policy (drop newest, drop oldest, or signal degraded) — see ENGINE_DESIGN §4.
4. **Honest status.** Capabilities report what was *probed*, not assumed (`MIXIT_DETECTED_*` bits); UI sections without a backend say so.

### Process model (decision)

In-process engine for M1–M8. The C ABI makes moving the engine into a separate
service process possible later (an IPC shim implementing the same header),
which would isolate the engine from UI crashes. VST3 plug-ins will need
out-of-process hosting for crash isolation (M4 ADR).

---

## 2. Repository structure

```
mixit/
├─ CLAUDE.md                  project rules (read first every session)
├─ DEVELOPMENT_STATUS.md      verified state, build/test results, next task
├─ ROADMAP.md                 milestones and status
├─ CHANGELOG.md
├─ README.md
├─ CMakeLists.txt             native build entry  (+ CMakePresets.json)
├─ cmake/                     compiler options, toolchains (MinGW cross-check)
├─ native/                    MixIt.Native — the engine
│  ├─ include/mixit/mixit_engine.h      the public C ABI (only exported surface)
│  ├─ src/abi/                ABI implementation, handle registry, export map
│  ├─ src/core/               Engine, Status, last-error
│  ├─ src/logging/            Logger, JSON-lines formatting
│  ├─ src/media/              time base, pools, queues, sources (test/file/capture), session, compositors, FFmpeg
│  ├─ src/platform/           OS helpers, capability probes (Windows / Linux dev host)
│  ├─ src/util/               bounded MPMC queue, clocks
│  └─ tests/                  GoogleTest suite
├─ managed/                   .NET 10 projects
│  ├─ Directory.Build.props / .targets / Directory.Packages.props
│  ├─ MixIt.Interop/          P/Invoke layer (BCL only)
│  ├─ MixIt.Core/             application services (engine host, production service, Control/: commands, macros, automation, tally, PTZ, extensions)
│  ├─ MixIt.Protocol/         remote-control and extension contracts (no dependencies)
│  ├─ MixIt.SDK/              extension interfaces (depends on Protocol only)
│  ├─ MixIt.Remote/           remote-control server (Kestrel HTTP + WebSocket)
│  ├─ MixIt.Licensing/        editions, signed offline licences (no dependencies)
│  ├─ MixIt.LicenseTool/      mixit-license: publisher key + licence issuing (not shipped)
│  ├─ MixIt.Probe/            mixit-probe diagnostics CLI
│  ├─ MixIt.Desktop/          WPF application
│  └─ tests/MixIt.Tests/      xUnit tests (use the real native engine)
├─ MixIt.sln / MixIt.Engine.slnf   full solution / non-WPF subset
├─ scripts/                   build-windows.ps1, verify-linux.sh, Wine runner
├─ docs/                      this document, design, API, build, tests, licenses, ADRs
└─ .github/workflows/ci.yml   Windows + Linux CI (build, tests, package, self-test)
```

`MixIt.Protocol` and `MixIt.SDK` were created in M7 (ADR-0012) and
`MixIt.Licensing` in M9 (ADR-0013), each when it had real content. Dependency direction: Desktop → Remote → Core →
(Interop, SDK → Protocol); clients and extensions need only Protocol/SDK.

---

## 3. Dependency and licensing assessment

Full inventory with versions: [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
I am not a lawyer; items marked ⚖️ need review by qualified counsel before
commercial release.

| Dependency | Use | License | Assessment |
|---|---|---|---|
| GoogleTest 1.14 / 1.15.2 | native tests | BSD-3 | Test-only, not redistributed. ✅ |
| xUnit 2.9.3, Test SDK | managed tests | Apache-2.0 / MIT | Test-only. ✅ |
| Microsoft.Extensions.* 10.0 | logging, DI | MIT | Redistributable. ✅ |
| **FFmpeg** (M2) | decode/encode/mux | LGPL-2.1+ *or* GPL | Must be an **LGPL build** (no `--enable-gpl`, no `--enable-nonfree`), **dynamically linked** (DLLs), with source offer + notices. That excludes libx264/libx265 (GPL) and fdk-aac (non-free). Software H.264 options: OpenH264 (BSD-2; Cisco's binary carries its patent licence) or a commercial x264 licence; hardware NVENC/QSV/AMF via FFmpeg are LGPL-compatible. ⚖️ |
| Codec patents (H.264, HEVC, AAC) | encode/decode | patent pools | Copyright licences do not cover patents. Distribution of encoders may require pool licences. ⚖️ |
| **NDI SDK** (M3/M5) | NDI in/out | NDI SDK licence (royalty-free) | Requires "NDI® is a registered trademark of Vizrt NDI AB" attribution, a link to ndi.video near NDI UI, DLLs shipped in the app folder. Optional at build time (runtime-loaded). NDI Advanced SDK is a separate commercial licence. |
| **Blackmagic DeckLink SDK** (M3/M5) | SDI/HDMI I/O | Blackmagic SDK EULA | Uses the COM API installed with Desktop Video drivers; headers only at build time. Review EULA for header redistribution. ⚖️ |
| **ASIO SDK** (M4) | low-latency audio | Steinberg proprietary **or** GPLv3 (since Oct 2025) | A closed-source commercial product must use the **proprietary licence agreement**; the GPLv3 option would force GPL on MIX IT. ⚖️ |
| **VST3 SDK ≥ 3.8** (M4) | plug-in hosting | MIT (since 3.8, Oct 2025) | Commercial use allowed with copyright/licence notice retained; VST trademark/logo rules apply separately. |
| SRT (libsrt) (M5) | SRT transport | MPL-2.0 | File-level copyleft; fine when dynamically linked and modifications to its files are published. |
| SQLite (M2/M8) | project store | Public domain | ✅ (via Microsoft.Data.Sqlite, MIT + SQLitePCLRaw, Apache-2.0). |
| OpenSSL / Schannel (M5) | RTMPS TLS | Apache-2.0 / OS | Prefer the OS TLS stack (Schannel) where FFmpeg supports it, to inherit OS certificate validation. |
| HTML graphics engine (after M6) | browser sources | CEF (BSD) or WebView2 (MS licence) | Deferred (ADR-0011): off-screen GPU rendering support is the deciding factor. M6 graphics use FreeType (FTL) + embedded DejaVu fonts. |

---

## 4. Native engine design (summary)

Detailed in [ENGINE_DESIGN.md](ENGINE_DESIGN.md). Key points:

- **Root object:** `mixit::Engine` owns every subsystem; `shutdown()` stops them in reverse start order. Destroying the handle always shuts down first.
- **Threads:** control plane (ABI callers, may lock), engine-owned worker threads (media, I/O, logging) that never wait on the control plane or the UI. Real-time threads: no locks, no allocation, no syscalls beyond what the device API requires.
- **Errors:** internals return `Status` (code + message); the ABI layer converts to `mixit_result` + per-thread last-error text. No exception crosses the ABI (catch-all barrier).
- **Time base:** one monotonic nanosecond master clock (`mixit_get_monotonic_time_ns`); all media timestamps are on it. Video output ticks at a rational frame rate; audio drift is corrected by adaptive resampling, never by dropping samples silently.
- **Memory:** fixed-capacity pools (frames, audio blocks, log records) allocated up front; steady-state media processing does not allocate.

## 5. C# ↔ C++ interoperability design

Implemented in M1 (✅ verified by tests and the probe):

| Concern | Design |
|---|---|
| Binding | `[LibraryImport]` source-generated P/Invoke (no runtime marshalling stubs; trimming/AOT-safe). |
| Library resolution | `"mixit_engine"` → `mixit_engine.dll` (Windows), `libmixit_engine.so` (Linux dev host). |
| Handles | Opaque 64-bit ids (never reused) validated in a registry; `EngineHandle : SafeHandle` ref-counts every call, so a handle cannot be freed mid-call; finalizer releases forgotten engines (tested). |
| Structs | Blittable, `struct_size`-prefixed for forward compatibility; sizes/offsets asserted on **both** sides (`static_assert` + managed layout tests). |
| Strings | UTF-8 in, UTF-8 out into caller buffers; native never returns heap memory to free. |
| Errors | `mixit_result` → `MixItNativeException(Result, Function, NativeMessage)` using the per-thread last error. |
| Callbacks | `[UnmanagedCallersOnly]` static function pointer + `GCHandle` user data. Invoked only on the engine's log thread; exceptions are caught and counted; disposing from inside a callback is refused (would self-join). The `GCHandle` is freed only after native destroy has joined the thread. |
| Versioning | Library and binding both check API version; while major = 0, minor must match exactly. |

High-rate data: video is copied across the boundary only for monitoring — the
engine writes a bus frame straight into a WPF `WriteableBitmap` back buffer at
25 Hz while the Production Studio is visible (ADR-0006). Native DXGI swap
chains hosted via `HwndHost` replace this once the GPU path is the default
(M8). Meters (M4) will be fixed-size snapshot structs polled at UI rate;
engine events will use a bounded native event queue drained on an engine
event thread.

## 6. Build system configuration

- **Native:** CMake ≥ 3.25 with presets. `windows-msvc` (VS 2022, x64, multi-config) is the release toolchain. Linux presets (`linux-debug/-release/-asan/-tsan`) and `mingw-crosscheck` exist for development hosts and CI. Warnings are errors (`/W4 /WX`, `-Wall -Wextra -Wpedantic -Wconversion ... -Werror`). The engine core is an OBJECT library linked into both the DLL and the test executable; Linux exports are restricted by a version script, Windows by `__declspec(dllexport)` on ABI functions only.
- **Managed:** `MixIt.sln` (all projects) and `MixIt.Engine.slnf` (no WPF). Central package management with pinned versions; `TreatWarningsAsErrors`; nullable enabled. The native binary is copied next to apps/tests from `MixItNativeDir` (defaults per OS, overridable).
- **Offline verification mode** (`-p:MixItOfflineVerification=true`): only for hosts without nuget.org; maps the logging abstractions to the locally installed shared framework. Never for releases.

Step-by-step instructions: [BUILD_GUIDE.md](BUILD_GUIDE.md).

## 7. Milestone implementation plan

See [../ROADMAP.md](../ROADMAP.md) for the full list. Near term:

| Milestone | Scope | Exit criteria |
|---|---|---|
| **M1 Foundation** | repo, CMake, .NET solution, native lib, C ABI, interop, init/shutdown, logging, errors, capability interface, tests, docs | Native + managed tests green on Windows CI ✅; hands-on Windows use pending (see DEVELOPMENT_STATUS). |
| **M2 Media pipeline foundation** (tested on Linux + Windows CI) | FFmpeg (LGPL, dynamic) init + capability detection of codecs/HW encoders; `MediaSource` abstraction; synthetic video (test pattern + frame counter + timecode burn-in) and audio (tone + clicks synced to frames); frame & audio buffer pools; rational time bases; bounded media queues with policies; diagnostics counters | Synthetic A/V runs for 1 h with measured drift < 1 ms; zero steady-state allocation (instrumented); queues never exceed bounds under overload tests. |
| **M3 First working video pipeline** (implemented; see ROADMAP for deviations) | as built: file decode (FFmpeg, software) and capture (Media Foundation, experimental) behind one `MediaSource` interface → frame sync → compositor (CPU reference, D3D11 experimental) → preview/program buses with layers, cut and transitions → WPF monitors (WriteableBitmap). Deferred: D3D11VA decode, GPU-resident frames, native swap-chain monitors. | 1080p60 file plays to program with no dropped frames on reference hardware; cut is frame-accurate. (Frame-accurate cut and 1080p60 synthetic soak verified; reference-hardware file run pending.) |

## 8. Initial testing strategy

| Layer | Tooling | Status (M3) |
|---|---|---|
| Native units | GoogleTest | ✅ 103 tests (M3 adds file source, compositor incl. D3D11 vs CPU, buses/transitions/layers, capture); M1's 40: queue (incl. MPMC stress), JSON, last-error/UTF-8, logger (ordering, drop policy, truncation, file sink), ABI (lifecycle, validation, version, struct-size negotiation, concurrency vs destroy, callback re-entrancy), capability probes |
| Memory/thread safety | ASan + UBSan, TSan | ✅ all native tests clean under both (Linux CI) |
| Windows code paths | MinGW cross-build (+ Wine in M1–M2) | ✅ cross-build on every push, incl. D3D11 and Media Foundation code |
| Windows/MSVC | `build-windows.ps1`, CI | ✅ every push (GitHub Actions) |
| Managed interop | xUnit (real native engine) | ✅ 37 tests under real xUnit on Windows and Linux CI |
| End-to-end | `mixit-probe`, `MixIt.exe --self-test production` | ✅ probe soaks PASS (incl. 1080p60 with PiP + transitions); production self-test PASS on Windows CI |
| Later | synthetic sources, golden-frame hashing, long-run A/V drift, Google Benchmark, soak tests, hardware lab matrix | M2+ |

Details and raw results: [TEST_REPORT.md](TEST_REPORT.md).

## 9. Risks, technical limitations and architectural conflicts

### Conflicts identified before writing code (and their resolution)

1. **"Unlimited inputs" vs real hardware.** Resolved: no hardcoded limits in data structures, but admission control based on detected capabilities and configured resource limits; the engine refuses or degrades explicitly rather than silently dropping.
2. **GPL contamination.** FFmpeg must be LGPL/dynamic (no x264/x265/fdk-aac); ASIO must use Steinberg's proprietary licence. Both are hard constraints on M2/M4.
3. **WPF and live video.** WPF's composition is not designed for several 60 fps video surfaces. Interim (M3): 25 Hz WriteableBitmap confidence monitors (ADR-0006). Target (M8): native DXGI swap chains in `HwndHost` regions (accepting WPF airspace limits on monitor areas).
4. **"No hardcoded secrets" vs offline licence validation.** Resolved in design: licences are signed server-side (asymmetric, e.g. Ed25519); the app embeds only the public key. Client-side checks can be bypassed on any desktop app; the goal is honest-user enforcement, not DRM.
5. **Module list in CLAUDE.md vs "no unnecessary abstractions".** Resolved: modules are created when they have content (see §2).
6. **Development environment vs target platform.** This repository was developed in a Linux environment without MSVC, WPF or nuget.org. Mitigated by Linux dev-host builds, sanitizers, MinGW cross-compilation and Wine execution — and, since M2, by Windows CI (MSVC, WPF, packaging, self-test). Still unverified: hands-on use, real GPUs and capture hardware.

### Ongoing risks

| Risk | Impact | Mitigation |
|---|---|---|
| Scope: a vMix-class product is many engineer-years | Schedule | Strict milestone gating; engine first; feature status matrix (PRODUCT_SPEC). |
| MSVC-only compile errors/warnings | build breaks | Every push builds with MSVC `/W4 /WX` on Windows CI. |
| Real-time behaviour on Windows (timer resolution, MMCSS, GPU scheduling, power plans) | Dropped frames/audio glitches | M2/M4 pacing design with MMCSS, waitable timers, instrumentation of late frames/underruns. |
| Single-process crash domain | A UI or plug-in crash ends the show | UI-exception containment (done in Desktop); out-of-process VST3 host (M4); optional out-of-process engine later. |
| Vendor SDK EULAs (NDI, DeckLink, ASIO) | Distribution | Optional, runtime-detected integrations; attribution and EULA checklist in THIRD_PARTY_LICENSES. |
| Codec patent licensing | Commercial release | Legal review before M9. |
| Hardware coverage | Field failures | Hardware lab matrix (NVIDIA/AMD/Intel GPUs, DeckLink, USB/HDMI capture) before M9. |

## Architecture decision records

- [ADR-0001](adr/0001-hybrid-native-engine-with-c-abi.md) Hybrid C# + C++ with a single versioned C ABI
- [ADR-0002](adr/0002-bounded-lock-free-logging.md) Bounded lock-free logging with an engine-owned log thread
- [ADR-0003](adr/0003-linux-dev-host-and-windows-verification.md) Linux as development host; Windows remains the only product platform
- [ADR-0004](adr/0004-ffmpeg-integration-and-licensing.md) FFmpeg integration and the LGPL licence guard
- [ADR-0005](adr/0005-compositor-cpu-reference-and-d3d11.md) Compositor: CPU reference, Direct3D 11 path, CPU-resident frames
- [ADR-0006](adr/0006-wpf-monitors-writeablebitmap.md) Preview/program monitors via WriteableBitmap (swap chains deferred)
- [ADR-0007](adr/0007-capture-media-foundation.md) Video capture through Media Foundation
- [ADR-0008](adr/0008-vst3-in-process-host.md) VST3 effects hosted in the engine process (first version; out-of-process host planned)
- [ADR-0009](adr/0009-audio-engine-timeline-and-device-clock-bridge.md) Audio engine on the session sample timeline, drift bridge to device clocks
- [ADR-0010](adr/0010-outputs-shared-encoders-isolated-destinations.md) Outputs: shared encoders, isolated destinations, timeline timestamps
- [ADR-0013](adr/0013-licensing-and-release.md) Offline signed licences, edition policy enforced at configuration time, Inno Setup installer
- [ADR-0012](adr/0012-command-layer-remote-control.md) One command layer for UI/macros/automation/remote/extensions; Kestrel remote control; TSL tally; VISCA PTZ
- [ADR-0011](adr/0011-graphics-keys-multiview-replay.md) Graphics as inputs (FreeType, JSON scenes), layer keyers, overlay channels, multiview bus, in-memory JPEG replay
