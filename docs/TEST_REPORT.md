# MIX IT — Test Report

## Run: Milestones 7–9 (control, desktop, release groundwork) — 2026-11-21

### GitHub Actions

| Run (commit) | Result |
|---|---|
| 44870966128 (6138bfd, M7) | ❌ Windows: `IOException` without `using System.IO` in the WPF project. |
| 44872051800 (158ddf7) | ❌ Windows: self-test did not finish (async-only services could not be disposed by the DI container; the UI error dialog blocked the unattended run). Fixed in f9bced6. |
| f9bced6 | ❌ Windows: NU1510 — DPAPI is part of the .NET 10 Windows Desktop framework (package reference removed). |
| 44873736430 (bfb2d9d, M7+M8) | ✅ Windows: native 154/154, xUnit **56/56**, self-test PASS incl. macro, remote API (401 without token, state, command), tally, show save/open, shortcut, dashboard. Linux superseded by the next push. |
| 1bb5e7a / later (M9) | see TEST_REPORT updates below / the run page |

### Local

| Check | Result |
|---|---|
| Managed (local runner, real native engine) | ✅ 59/59 |
| `mixit-license` keygen → issue → verify; refuses to overwrite a key | ✅ |

### New tests

ControlTests (9): macro text round trip; commands drive the production and report errors (switching, transitions, PiP, audio, tally); macros in order / stop on error / nesting / cancellation / through commands; automation armed-only, no catch-up, daily/once/interval/start; TSL 5.0 packet bytes received over UDP; VISCA bytes (Sony framing and raw) received over UDP; remote API (token, state, commands, 404/400, WebSocket events and commands, macro run); extensions (commands, isolated failures, detach); control settings round trip and broken-file handling.
ShowFileTests (1): full production round trip incl. protected stream key, key-less save warning, missing file, invalid file.
LicensingTests (3): signature/tamper/expiry/PC binding/no key; service install/keep/remove/persist; Free limits at configuration time only.

### Not verified (no hardware / services in CI)

Tally receivers, PTZ cameras, LAN remote clients and control surfaces, extra
displays, real extensions, installer on a clean consumer PC, SmartScreen
behaviour of the unsigned installer.

## Run: Milestone 6 (graphics and production tools) — 2026-11-21

### GitHub Actions

| Run (commit) | Result |
|---|---|
| 44851831773 (2c9e865, M6b) | ❌ Windows self-test: "overlay 1 not on air" — a WPF ComboBox pushed a null selection into the overlay channel when the input list was rebuilt, emptying the channel. Fixed in 8ab3c64. |
| 44854020148 (8ab3c64, M6c) | ❌ Windows: `test_buses.cpp` missed `#include <array>` (MSVC). |
| 44854704962 (d315f03) | ✅ Windows: native **154/154** (D3D11 keyer comparison on WARP passed), WPF 0 warnings, xUnit **46/46**, self-test PASS (recording, lower third on an overlay, multiview monitor, replay at 50 %) · ❌ Linux TSan: data race in the replay encoder (read the frame size after releasing the frame; a buffer kept by a replay input could still hold frames of a removed input's pool). Fixed in e41d496. |
| 44856343384 (e41d496) | ✅ Windows: native 154/154, WPF, xUnit 46/46, self-test PASS · ✅ Linux: Debug, ASan+UBSan, TSan, Release, MinGW cross-check |

### Local (2-core Linux)

| Check | Result |
|---|---|
| Native against the pinned LGPL FFmpeg 9.0.2 | ✅ 153/153 + 1 skipped (D3D11, Windows only) |
| ASan+UBSan (system FFmpeg 6.1) | ✅ 153/153 |
| TSan (`-j2`) | ✅ replay/outputs/buses/graphics suites, replay test ×3 after the fix · full run: 152/153 — `Session.ScalesInputsToProgramSizeAndCutsBetweenInputs` failed once on timing under load and passed 3/3 when rerun (M3 test; known flake on this host) |
| MinGW-w64 cross-build | ✅ 75 exports |
| Managed (local runner) | ✅ 46/46 |

### New tests (M6)

Native (13): JSON parser (valid, limits, errors), scene parsing and field expansion, rendering of rectangles, text (FreeType, alignment, fit) and images; chroma and luma key pixel tests and the D3D11 keyer comparison; keyed layer with the older 48-byte layer struct; overlay channel across cuts and fades; lower third on an overlay with live fields and timer; multiview layout with tally; multiview recording (counts skipped ticks and encoder drops so slow sanitizer builds stay honest); **replay**: validation, buffer statistics, playback from the oldest frame at 50 % recorded to a file and decoded (flash every 0.5 s live → every 1.0 s ± 0.15 s), pause holds the position, the recorded input removed while its replay stays. Managed (2): graphics on an overlay across a rebuild; replay + rename + multiview recording across a rebuild, replay removed with its input.

### Limitations

- No real green-screen camera footage keyed; the GPU keyer ran on WARP only.
- Replay soak (10 min buffer at 1080p, memory) not run; replay is video only.
- HTML graphics not implemented (renderer decision pending).

## Run: Milestone 5 (streaming and recording) — 2026-11-21

### GitHub Actions

| Run (commit) | Result |
|---|---|
| 44846054396 (778f0ec, M5b) | ❌ Windows: recorded A/V offset 85 ms with `h264_mf` (2 tests); isolation test too impatient for Windows' ~2 s refused-connection time. Linux: SRT test failed under TSan — lock-order report inside libsrt (third party). |
| 8ac9b09 | ❌ Windows: offset now 64 ms — diagnosis printed by the test: video timestamps exact, audio 3073 samples early → audio dropped before the first (late) video packet. |
| cc2cc38 | ✅ Windows: native **141/141** (Media Foundation H.264 chosen automatically), WPF 0 warnings, xUnit **44/44**, self-test PASS incl. a 2.5 s recording · Linux: see the run page |

### Local (2-core Linux)

| Check | Result |
|---|---|
| Native Debug / Release / ASan+UBSan / TSan (`-j2`) | ✅ 141/141 each |
| Output tests against the pinned LGPL FFmpeg 9.0.2 (OpenH264 chosen) | ✅ 10/10, A/V 0.021 ms |
| A/V regression check with a look-ahead encoder (x264 `tune=film`, local only) | 94.6 ms before the fix, 0.021 ms after |
| MinGW-w64 cross-build | ✅ 60 exports |
| Managed (local runner) | ✅ 44/44 |

### New tests (M5)

Native (10), all verifying by **decoding what was written**: encoder query; program → MP4 with frame count, codec, size, channels, start at 0, audio coverage, flash/click A/V offset (< 40 ms; measured 0.021 ms) and a second take not overwriting the first; shared encoder → MKV + 2 s segmented TS (every segment decodes from its start); RTMP to a local FFmpeg listener; SRT to a local listener; broken RTMP destination does not disturb a recording or the session clock and stops promptly; HLS playlist + segments; ISO at the input's own size with A/V check; validation; url redaction. Managed (2): production outputs + ISO across a session rebuild, finalized files, ISO removed with its input; url redaction.

### Limitations

- No hardware encoder ran (no GPU in CI); no real streaming platform; RTMPS never connected to a TLS server.
- 1 h recording/stream soak not run.

## Run: Milestone 4 (audio engine) — 2026-11-19

### GitHub Actions

| Run (commit) | Result |
|---|---|
| 44474551238 (6eb6096, M4b) | ✅ Windows + Linux; WPF builds; self-test incl. audio checks PASS |
| 44476905944 (e560175, M4c) | ✅ Windows + Linux |
| 44480253897 (44ba2b3, M4d) | ❌ Windows: 3 VST3 tests — the test hard-coded the class id; on Windows VST3 lays out `INLINE_UID` in COM GUID byte order. The plug-in loaded fine. Fixed in 337520a. |
| 44481086098 (337520a) | ✅ Windows: native **131/131** (incl. VST3 host loading a real `.vst3` DLL with `LoadLibraryExW`), solution incl. WPF **0 warnings**, xUnit **42/42**, probe soak PASS, **`MixIt.exe --self-test production` PASS** (80/79 monitor frames, 0 late ticks) · Linux: see below |

### Local (2-core Linux dev host)

| Check | Result |
|---|---|
| Native Debug / Release / ASan+UBSan | ✅ 131/131 each |
| Native TSan, `-j2` | ✅ 131/131, no TSan reports |
| Native TSan, `-j6` (3× oversubscribed) | ⚠️ 2–3 *timing* tests fail, a different set each run (`Session.ScalesInputs…`, `Buses.AutoTransition…`, `AudioDevices.OutputCompensates…`); they pass on re-run and at `-j2`. Wall-clock assertions under heavy TSan overload, not data races (no TSan report). CI uses the default parallelism. |
| MinGW-w64 cross-build | ✅ 0 warnings; exactly the 54 API functions exported; imports unchanged (d3d11, dxgi, kernel32, msvcrt, ole32; MF and avrt loaded at run time) |
| Managed (xUnit-compatible local runner) | ✅ 42/42 |

### New tests (M4)

- Native (28): DSP (12: biquad responses, shelves, high-pass, compressor curve and timing, gate/expander, limiter ceiling, peak ballistics, RMS, K-weighting response, momentary/short-term loudness, gated integrated loudness of a −23 LUFS tone, gain ramps), mixer through the ABI (7: AFV with program switches and transitions, fader/mute/trim/bus fader, routing/solo/monitor, delay, meters/loudness, A/V alignment of the mixed program audio, **VST3 insert while running**), drift bridge (3: transparency, ±ppm tracking over a 400 s simulation, start-up/stall resync), devices with the null device (3: output compensating a clock offset, capture input level/frequency/no dropouts, device loss and retry), VST3 host (3: scan, process/parameters/bypass/state mono+stereo, zero-frame parameter flush).
- Managed (5): audio strips/buses/meters/bus audio read, audio settings surviving a format change, monitor output + audio input on the test device, VST3 scan/insert/parameters/bypass/state, plug-in inserts and their settings surviving a session rebuild; layout checks for every new struct.

### Defects found and fixed

1. Mute ramp ended at −1.6e-6 instead of 0 (float accumulation) → per-frame interpolation, exact last frame.
2. Integrated loudness read −23.17 LUFS after silence: the transition blocks are legitimately included by the gating; test changed to a 20 s tone.
3. Drift resampler oscillated and the expected sign in the test was wrong (a fast device needs ratio ≈ 1 − ppm) → retuned PI controller, ~1 s fill smoothing, averaged assertion.
4. Device tests flaky under parallel load: start-up excess fill raised the ratio and shifted pitch → start-up/stall resync discards excess audio.
5. `ProductionService.GetAudioOutputStatistics()` returned null after a format change → the session is rebuilt immediately.
6. A plug-in parameter changed while the session was stopped was missing from the saved state → zero-length process call delivers it.
7. VST3 class id byte order differs on Windows (see CI table).

### Limitations

- **No audio hardware** anywhere: WASAPI output/capture paths ran only as far as "no device"/enumeration on CI; the drift bridge is verified with the simulated null device.
- **Only the test plug-in** has been hosted; commercial VST3 plug-ins are untested (ADR-0008).
- The WPF Audio Mixer was exercised by the self-test, not by a person.

## Run: Milestone 3 (video pipeline) — 2026-11-17

### GitHub Actions — run 44041552714 (commit 1389f21)

| Job | Result |
|---|---|
| Windows (MSVC 19.44, .NET 10, LGPL FFmpeg 9.0.2, no GPU) | ✅ native **103/103** — incl. `Compositor.D3D11MatchesTheCpuReference` (WARP), `Buses.GpuCompositorPreferenceRendersOrFallsBack` (D3D11 on WARP), `Capture.EnumerationAndValidation` · solution incl. WPF **0 warnings** · xUnit **37/37** |
| Windows packaging | ✅ `mixit-probe --session-test 10` PASS (PiP, watched preview, mix + cut, 0 late ticks) · `MixIt.exe` opens its window, engine log OK · **`MixIt.exe --self-test production` PASS** (monitors showed 73/72 distinct frames in ~3 s, 2 transitions, tally correct, composited frames, clean stop) |
| Linux (GCC 13.3) | ✅ native 103/103 in Debug, ASan+UBSan, TSan (D3D11 test skipped: Windows only) · MinGW cross-check · xUnit 37/37 |

Artifact: `MixIt-win-x64-1389f21…` (132 MB, includes the LGPL FFmpeg DLLs).

### Local (2-core Linux dev host)

| Check | Result |
|---|---|
| Native, Debug / Release / ASan+UBSan / TSan (`-j2`, twice for TSan) | ✅ 103/103 each |
| Full native suite against **FFmpeg 9.0.2** (BtbN Linux LGPL build of the same release as Windows CI) | ✅ 99/99 at commit bdb718a (before the capture tests were added) |
| MinGW-w64 cross-build incl. D3D11 compositor and Media Foundation capture | ✅ 0 warnings; DLL imports only d3d11/dxgi/kernel32/msvcrt/ole32 (MF resolved at run time) |
| Managed (xUnit-compatible local runner; nuget.org blocked) | ✅ 37/37 |
| CPU compositor benchmark, Release, 640×360 → 1920×1080 | 4.85 ms/frame (was 20.2 ms before the separable + banded rewrite); two-scene mix 10.1 ms |
| `mixit-probe --session-test 10 --format 1920x1080@60` (Release native) | ✅ PASS: 599/600 ticks, 0 late, PiP every tick + preview + mix + cut, 1139 composited frames |
| same at 1080p29.97 | ✅ PASS: 0 late, max lateness 0.30 ms |
| same with the Debug native build at 1080p60 | ❌ FAIL (269 late ticks) — expected for an unoptimised build; product builds are Release |
| `--file` H.264 720p29.97 / 44.1 kHz into a 30 fps session (M3a) | ✅ PASS: 2 loops, 0 decode errors |

### New tests

- Native (28): compositor (fit, black, exact copy, placement/scale/blend, source alpha, crop, transitions, bilinear downscale, **D3D11 vs CPU**), buses (preview/program + cut, auto transition timing and swap, cut completing a transition, transition while stopped, PiP decoded from pixels, layer validation, aspect bars, preview rendered only while watched, GPU preference/fallback, 0.2 config/stats compatibility), file source (7), capture (enumeration/validation), ABI compatibility.
- Managed (7): production service (full production run with monitors/PiP/transitions, engine restart + format change, missing file, capture listing), preview/cut/transition/layers, GPU preference, file-source error mapping (plus layout checks for every new struct).

### Defects found and fixed

1. **FFmpeg 9 dropped the last MP4 frame** (Windows CI: 44/45). Root cause: the test writer left the last packet's duration at 0, so the MP4 muxer wrote a zero-length last sample excluded by the edit list. Reproduced locally with the same FFmpeg build; the reader was correct.
2. **Transitions ran long under load**: progress advanced per rendered tick, so skipped ticks stretched the transition. Now advances by elapsed tick indices.
3. **Null dereference** possible in the zero-copy path when an input vanished (GCC `-Wnull-dereference`, Release only).
4. Compositor too slow for TSan timing tests (20 ms/frame at 1080p) → separable filter with row caches + band parallelism (4× faster).
5. D3D11 vs CPU comparison failed (max Δ 20) on test content with wrap-around discontinuities, where sub-texel filter precision differs between rasterizers; test content made smooth and the worst pixel is now reported.
6. xUnit analyzer (xUnit2013) error in CI — not visible with the local runner.

### Limitations

- No GPU, no capture hardware and no interactive desktop session were available; GPU results are WARP-only, capture is validated only for "no device" paths, and the WPF app was driven by its self-test, not by a person.
- Monitors poll at 25 Hz (ADR-0006); they skip frames at 50/60 fps by design.
- The 1080p60 *file* exit criterion needs a Windows workstation run.

## Run: GitHub Actions — 2026-11-17 (run 44031124025, commit 5516a44)

First execution on real Windows with MSVC and on GitHub-hosted Linux.

| Job | Environment | Result |
|---|---|---|
| Windows (MSVC + .NET) | windows-2022 runner (Windows Server 2022 build 20348, AMD EPYC, no GPU), MSVC 19.44, .NET 10 | ✅ native **75/75** (1 skipped: codec queries, no FFmpeg on Windows CI) · solution incl. **WPF app built with 0 warnings / 0 errors** · xUnit **30/30** · `mixit-probe` OK, DXGI reported 2 Microsoft Basic Render Driver adapters (one flagged software) |
| Linux dev host | ubuntu-24.04 runner (kernel 7.0 azure), GCC 13.3, system FFmpeg | ✅ native **75/75** in Debug, ASan+UBSan and TSan · MinGW cross-check · xUnit **30/30** · probe |

Follow-up runs 44031857039 / 44032464409 added packaging and a smoke test of
the published app on the Windows runner: `MixIt.exe` (self-contained, Release)
**starts, opens its window and the engine writes its log**; `mixit-probe`
session test (10 s) PASS. Artifact: `MixIt-win-x64-<sha>` (65 MB).

Defect found by that smoke test and fixed: the engine opened its log with
`_wfopen_s`, which locks the file on Windows, so the log could not be read
while MIX IT was running. Now opened with `_wfsopen(..., _SH_DENYWR)`;
`EngineAbi.LogFileIsCreated` reads the live log (Wine did not reproduce the
lock — only real Windows did).

This closes the M1 items "MSVC build", "WPF compiles", "WPF app starts", "real xUnit" and
"NuGet versions resolve". Still open: hands-on use of the WPF app, real-GPU and
long-duration timing on Windows hardware, Windows FFmpeg.

---

## Run: Milestone 2 (media pipeline foundation) — 2026-11-17

**Environment:** same Linux development host as the M1 run (2 vCPU Xeon with
AVX-512, 8 GiB, no GPU), plus Ubuntu FFmpeg 6.1.1 (a **GPL** build — used to
exercise the licence guard; never distributable). Still no Windows/MSVC, WPF
targeting pack or nuget.org.

### Summary

| Suite | Result | Notes |
|---|---|---|
| Native GoogleTest, Linux Debug | ✅ **75/75** | 0 warnings (`-Werror`) |
| Native, ASan + UBSan | ✅ **75/75** | |
| Native, ThreadSanitizer | ✅ **75/75**, also 3× with `ctest -j4` (CPU contention on 2 vCPUs) | real-time session tests included |
| Session tests repeated | ✅ 5/5 runs | flakiness check |
| Windows build (MinGW) under Wine | ✅ **74 passed, 1 skipped** | skipped = codec queries (cross build has no FFmpeg); session pacing uses the Windows high-resolution waitable timer |
| LGPL policy build (`MIXIT_REQUIRE_LGPL_FFMPEG=ON`) against GPL FFmpeg | ✅ rejected | `USABLE` clear, `mixit_query_codec` → `NOT_SUPPORTED`, error logged |
| Managed tests (30) | ✅ **30/30** | local xUnit-compatible runner (nuget.org blocked) |
| `mixit-probe --session-test 60` (720p29.97, release) | ✅ PASS | 1798/~1799 ticks, 0 late, 0 dropped, max tick lateness 1.63 ms, A/V max offset **0 ns** over 52 events |
| `mixit-probe --session-test 20 --format 1920x1080@60000/1001` | ✅ PASS | 1198/~1199 ticks, 0 late, max lateness 1.97 ms, A/V 0 ns over 17 events |
| MSVC / WPF / real xUnit / CI | ⚠️ not run | unchanged from M1 |

### New native tests (35)

| Suite | Tests | Covers |
|---|---|---|
| TimeBase | 9 | 128-bit muldiv vs reference (100k random cases, 3 roundings), full-width intermediates, overflow/zero detection, signed floor semantics, NTSC frame times, **no drift at frame 3·10⁸**, one-hour 59.94 vs 48 kHz alignment, durations sum exactly (5 rates × 100k), `index_at_time`/`nearest_index` inverse properties (80k random), supported-rate list |
| Pool / VideoFramePool / MediaQueue | 6 | exhaustion without growth, ref-count copy/move/release-once, 4-thread churn accounting, 64-byte alignment, DropNewest keeps item with caller, DropOldest keeps newest + counters + releases dropped items |
| TestSignal | 6 | frame code round-trip incl. >32-bit indices, determinism, flash flag/band, uniform images (black/grey/white) never decode, tone below click threshold, click on the nearest sample to each flash frame at 29.97/25/59.94 |
| Session (ABI) | 10 | config validation, lifecycle + input rules while running, black program before selection, same-rate frame-for-frame presentation with decoded frame code, **25→30 repeats / 60→30 skips**, scaling 640→1280 + zero-copy + **cut while running**, size query/too-small/bad stride, **A/V alignment ≤ ½ sample**, engine shutdown closes sessions, session handle outliving engine handle, repeated start/stop releases program frames |
| SessionInternal | 1 | **no pooled buffer leaks** after 3 start/cut/stop cycles |
| MediaLibrary | 3 | licence-string policy, flags consistency, codec queries (skipped without usable FFmpeg) |

### New managed tests (5) + layout additions

`SessionTests`: test signal to program with frame copy and statistics, native
error mapping (`NotSupported`, `NotFound`, `InvalidState`, `BufferTooSmall`),
engine dispose closes sessions, media-library policy consistency, `MediaRate`
formatting. `InteropLayoutTests` now also asserts the 7 new struct sizes and
key offsets.

### Defects found and fixed during this run

1. **Black frames decoded as "frame 0 of source 0"** — an all-black code row is a valid all-zero code. Found by `Session.ProgramIsBlackUntilAnInputIsSelected`. Fixed with a white/black sentinel block pair; uniform images are now rejected (tested).
2. `__int128` rejected under `-Wpedantic`; replaced by a portable 64×64→128 multiply (also removes MSVC-specific intrinsics).
3. Quick Sync codecs were not reported as hardware (FFmpeg flags them `HYBRID`); now counted as hardware.
4. Test expectation for black-frame `source_sequence` corrected to the documented `-1`.

### Limitations

Same as M1 (managed tests via local runner; Windows paths via MinGW + Wine;
no GPU). Additionally: soak tests ran on a Linux VM — real Windows timer and
scheduling behaviour still has to be measured on hardware.

---

## Run: Milestone 1 — 2026-11-17

**Environment:** Linux x86-64 development host (Ubuntu 24.04, 2 vCPU Xeon @ 2.1 GHz
with AVX-512, 8 GiB RAM, no GPU, no display). GCC 13.3.0, CMake 3.28.3,
Ninja, .NET SDK 10.0.115, MinGW-w64 GCC 13 (posix), Wine 9.0.
**No Windows machine, MSVC, WPF targeting pack or nuget.org access was available.**

### Summary

| Suite | Build | Result | Notes |
|---|---|---|---|
| Native GoogleTest, Linux Debug | ✅ 0 warnings (`-Werror`) | ✅ **40/40** | |
| Native GoogleTest, ASan + UBSan | ✅ | ✅ **40/40** | no reports |
| Native GoogleTest, ThreadSanitizer | ✅ | ✅ **40/40** | no reports |
| Native engine, MinGW-w64 Windows cross-build | ✅ 0 warnings (`-Werror`) | — | DLL exports exactly 13 `mixit_*` symbols; imports only `dxgi.dll`, `KERNEL32.dll`, `msvcrt.dll` |
| Native GoogleTest, Windows build under Wine | ✅ | ✅ **40/40** (after 2 fixes, below) | executes Win32/DXGI code paths |
| Managed tests (25) against real engine | ✅ 0 warnings | ✅ **25/25**, 5 repeated runs | run with a local xUnit-compatible shim — see "Limitations" |
| Managed tests against ASan+UBSan engine | ✅ | ✅ **25/25** | native memory errors from managed calls would abort |
| `mixit-probe` end-to-end | ✅ | ✅ | text, `--json`, `--log-file` (valid JSON lines), exit 1 on engine error, exit 2 on usage error |
| Desktop view models (type-check with WPF stub) | ✅ 0 warnings | — | XAML not compiled |
| MSVC build | ⚠️ not run | ⚠️ | first task of next session |
| WPF application | ⚠️ not compiled | ⚠️ | needs WPF targeting pack (nuget.org) |
| xUnit via `dotnet test` | ⚠️ not run | ⚠️ | needs nuget.org |
| GitHub Actions CI | ⚠️ not run | ⚠️ | repository not pushed |

### Native tests (40)

| Suite | Tests | Covers |
|---|---|---|
| BoundedMpmcQueue | 6 | capacity rounding/limits, FIFO, full-queue rejection, wrap-around (10k laps), in-place writer/reader, 4-producer/3-consumer stress (200k items, each seen exactly once) |
| Json | 3 | escaping, UTF-8 pass-through, ISO-8601 timestamps incl. leap day |
| LastError | 4 | round-trip + size query, truncation, invalid args, per-thread isolation |
| Utf8Truncation | 1 | never splits a multi-byte sequence |
| Logger | 9 | in-order delivery, level filter + runtime change, UTF-8-safe truncation with marker, **non-blocking drop policy under a stalled sink** (1000 records in < 500 ms, accepted ≤ 9 at capacity 8), drain on stop + idempotence + no callbacks after stop, 8-thread concurrent producers, JSON-lines file content, unopenable file → `IO`, invalid level |
| EngineAbi | 15 | version/build info/result names/clock; argument validation; API version policy (0.x exact minor, patch-compatible); larger structs from newer callers (no over-write); full lifecycle incl. double destroy and use-after-destroy; NULL/foreign handles; host + engine records via callback; runtime log level; **destroy from inside the log callback refused without deadlock**; log file creation/failure; capability detection + GPU index errors; **20 rounds of 4 threads calling while the handle is destroyed**; 8 coexisting engines |
| SystemInfo | 2 | CPUID baseline/consistency; probe fills CPU/memory/OS; GPU flag honest per platform (Linux: not set; Windows: set, or under Wine: degrade cleanly) |

### Managed tests (25)

| Class | Tests | Covers |
|---|---|---|
| InteropLayoutTests | 3 | struct sizes, field offsets, enum values vs C header |
| NativeEngineTests | 11 | version check, clock, lifecycle + `ObjectDisposedException` after dispose, `InvalidState` with native message after shutdown, `Io` error on bad log path, 100 records in order with non-ASCII text, handler exceptions contained and counted, dispose from handler refused, capability plausibility, **finalizer releases an abandoned engine**, concurrent `TryLog` while disposing (10 rounds × 4 threads) |
| EngineHostTests | 8 | state transitions + events + idempotence, restart, capability caching, start failure → `Faulted`, log stream content, bounded stream drops oldest, `ILogger` → engine forwarding (level, formatting, exceptions), level mapping |
| CapabilityFormattingTests | 3 | byte/feature formatting, "unavailable" vs "no adapters" wording |

### Defects found and fixed during this run

1. **Windows-only test bug** (found only by running the Windows build under Wine): `Logger.WritesJsonLinesFile` deleted the log file while its `ifstream` was still open — legal on Linux, a sharing violation on Windows. Fixed by closing the stream first.
2. **DXGI unavailable in headless Wine** (`CreateDXGIFactory1` → `0x887A0004 DXGI_ERROR_UNSUPPORTED`). The engine already degraded correctly (flag cleared, warning logged); the test now asserts the degraded behaviour under Wine and still requires DXGI success on real Windows.
3. **Public header not clean under strict C++ warnings**: version macros used C-style casts (`-Wold-style-cast`). Rewritten cast-free.
4. **Linux shared library leaked libstdc++ template symbols** into its export table. Fixed with a linker version script; now exactly 13 exports.
5. **MinGW named the DLL `libmixit_engine.dll`**, which .NET on Windows does not probe. Fixed (`PREFIX ""` on Windows).
6. **Pre-emptive MSVC fix:** C4324 (intentional `alignas` padding) would fail `/W4 /WX`; disabled with justification.
7. **WPF code review** found three compile errors before they shipped (duplicate `Style`, int/string ternary, a `System` property shadowing the namespace); fixed, and the view models now type-check.

### Limitations of this run (read before relying on it)

- The **managed tests were executed by a minimal local runner** that implements the subset of the xUnit API the tests use (`[Fact]`, `Assert.*`), compiled with the unmodified test sources against the real Core/Interop projects and native library. It is not part of the repository. The tests must be re-run with real xUnit (`dotnet test`) on a machine with nuget.org access.
- Managed code was compiled in **offline verification mode**, which takes `Microsoft.Extensions.Logging.Abstractions` from the locally installed ASP.NET Core shared framework instead of NuGet.
- The Windows code paths ran under **Wine with MinGW**, not on Windows with MSVC. DXGI enumeration of real GPUs is untested.

## How to reproduce

See [BUILD_GUIDE.md](BUILD_GUIDE.md). Linux: `scripts/verify-linux.sh --mingw`
(add `--offline` without nuget.org); Windows build under Wine:
`scripts/run-windows-tests-under-wine.sh`; Windows: `scripts/build-windows.ps1`.
