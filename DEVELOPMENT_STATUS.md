# MIX IT — Development Status

_Last updated: 2026-11-17 (first GitHub Actions run)_

**Current milestone:** M2 — Media Pipeline Foundation → **IMPLEMENTED on the dev host** (gaps listed below)
**Previous milestone:** M1 — Foundation → implemented; **MSVC build, WPF compile and xUnit verified on GitHub Actions** (run 44031124025); interactive WPF run and real-GPU checks pending
**Deviation from plan:** M2 was started before M1's Windows checks, at the user's request ("lanjut"), because this environment has no Windows/MSVC/nuget.org. Both milestones close only after the Windows checks below pass.

## Completed and verified this session (M2)

| Item | Status | Evidence |
|---|---|---|
| Exact time base (rational rates, 128-bit muldiv, no-drift frame/sample times) | TESTED | 9 TimeBase tests incl. frame 3·10⁸ exactness |
| Frame & audio buffer pools (fixed, aligned, ref-counted, no runtime allocation) | TESTED | Pool/VideoFramePool tests, TSan |
| Bounded media queues with DropNewest / DropOldest + counters | TESTED | MediaQueue tests |
| Synthetic test signal: bars, motion, machine-readable frame code, flash + click sync markers, tone | TESTED | 6 TestSignal tests |
| Session API (lifecycle steps 3–8), per-input paced producers, output clock, frame sync (repeat/skip), program bus with cut, scaling, black fallback | TESTED | 11 Session tests, 60 s + 1080p59.94 soaks |
| A/V alignment measurement end to end | TESTED | 0 ns measured (limit 10.4 µs) |
| Precise pacing (absolute sleeps; Windows high-res waitable timer) | TESTED on Linux; Windows path run under Wine | |
| FFmpeg integration: optional build, version check, **LGPL licence guard**, codec availability | TESTED | policy ON rejects GPL FFmpeg; ADR-0004 |
| C ABI 0.2.0 (25 exports), managed `NativeSession`, media library API | TESTED | 30 managed tests (local runner) |
| `mixit-probe --codecs`, `--session-test N [--format]` (PASS/FAIL soak) | TESTED | |

## Gaps in M2 (honest list)

| Item | Status | Note |
|---|---|---|
| Generic media input abstraction | IN_PROGRESS | Inputs are the concrete test signal inside `Session::Input`. The source interface will be extracted in M3 when the file/capture sources give it a second, different implementation (avoids a speculative abstraction). |
| 1-hour drift test | PARTIAL | Drift-free arithmetic is proven by tests; real-time soaks ran 60 s (720p) and 20 s (1080p59.94). Run `mixit-probe --session-test 3600` on Windows hardware. |
| Google Benchmark harness | PLANNED | not added (no measurements needed yet for CPU reference path) |
| FFmpeg log routing into the engine log | PLANNED (M3) | needed once decoding starts |
| Pinned Windows LGPL FFmpeg build (version, configure line, hash) | PLANNED | ADR-0004 step 5 |

## Verified on GitHub Actions (run 44031124025, commit 5516a44)

| Item | Result |
|---|---|
| MSVC 19.44 build of engine + tests (`/W4 /WX`), Windows Server 2022 runner | ✅ 75/75 native tests (1 skipped: no FFmpeg) |
| .NET solution incl. **MixIt.Desktop (WPF)** | ✅ 0 warnings, 0 errors |
| Managed tests under **real xUnit** (`dotnet test`) | ✅ 30/30 on Windows and Linux |
| NuGet versions in `Directory.Packages.props` | ✅ resolved |
| DXGI enumeration on real Windows | ✅ 2 adapters (Microsoft Basic Render Driver; no GPU on runner) |
| `mixit-probe` on Windows | ✅ |
| Packaged self-contained `MixIt.exe` starts, opens its window, engine logs (smoke test) | ✅ artifact `MixIt-win-x64-<sha>` per CI run |
| Linux: Debug, ASan+UBSan, TSan, MinGW cross-check, xUnit | ✅ |

## Still unverified

| Item | Action |
|---|---|
| Hands-on use of the WPF app (layout, DPI, interaction) | download the CI artifact and run `MixIt.exe` |
| DXGI / timing on real GPU hardware | `mixit-probe --codecs` and `--session-test 3600 --format 1920x1080@60000/1001` on a workstation |
| FFmpeg on Windows (LGPL build) | ADR-0004 step 5, then CI with `FFMPEG_ROOT` |

## Build status (this session)

| Target | Result |
|---|---|
| Linux Debug / Release / ASan+UBSan / TSan | ✅ 0 warnings |
| LGPL-policy variant (`-DMIXIT_REQUIRE_LGPL_FFMPEG=ON`) | ✅ |
| MinGW-w64 cross-build | ✅ 0 warnings (without FFmpeg) |
| .NET Interop, Core, Probe (offline mode) | ✅ 0 warnings |
| MSVC (Windows Server 2022, GitHub Actions) | ✅ 0 warnings |
| WPF (`MixIt.Desktop`) | ✅ compiles, 0 warnings |

## Test results (this session)

Native **75/75** (Debug, ASan+UBSan, TSan incl. parallel under load) · Windows build under Wine **74 passed + 1 skipped** · Managed **30/30** (local runner) · soaks PASS. Details: [docs/TEST_REPORT.md](docs/TEST_REPORT.md).

## Known issues

1. `MixIt.Desktop` compiles in CI but has not been run interactively; it does not yet show sessions.
2. CI runners have no GPU and no FFmpeg on Windows, so GPU and media-library paths are only partly covered there.
3. Dev-host FFmpeg is GPL; it is tolerated only because the dev build disables the licence policy.
4. Inputs cannot be added/removed while a session runs (hot-plug is M3).
5. Session audio is consumed for measurement only; there is no mixer or audio output yet (M4).

## Next recommended tasks

1. **On a Windows workstation:** run the WPF app, `mixit-probe --codecs` and `mixit-probe --session-test 3600 --format 1920x1080@60000/1001`; record results.
2. Provide the pinned LGPL FFmpeg for Windows (ADR-0004 step 5).
3. Start M3: extract the media source interface; FFmpeg file source (decode, timestamps → master clock); D3D11 device + GPU upload/convert; native swap-chain monitors in WPF; GPU program path.
