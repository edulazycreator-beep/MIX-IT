# MIX IT — Test Report

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
