# MIX IT — Development Status

_Last updated: 2026-11-17 (Milestone 3)_

**Current milestone:** M3 — First working video pipeline → **IMPLEMENTED**, tested on Linux and Windows CI (GitHub Actions run 44041552714, commit 1389f21, all green). Exit criterion on reference hardware still open (see below).
**Previous milestones:** M1 Foundation and M2 Media pipeline foundation → TESTED on Linux and Windows CI; hands-on Windows use and the 1-hour soak on hardware still pending.
**Next milestone:** M4 — Audio engine.

## Completed in M3

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
| Hands-on use of the WPF app (layout, DPI, interaction, monitor smoothness) | run `MixIt.exe` from the CI artifact |
| M3 exit criterion: 1080p60 file to program without drops | `mixit-probe --session-test 600 --format 1920x1080@60 --file clip.mp4` |
| GPU compositor on real hardware, CPU vs GPU cost | `mixit-probe --session-test 60 --format 1920x1080@60 --compositor d3d11` vs `cpu` |
| Capture with real webcams / capture cards (incl. a 4-input card) | `mixit-probe --list-capture`, then `--session-test 60 --capture 0`; in the app: ↻, select device, + Capture |
| 1-hour drift soak (M2) | `mixit-probe --session-test 3600 --format 1920x1080@60000/1001` |

## Build status

| Target | Result |
|---|---|
| Windows: MSVC 19.44 `/W4 /WX`, .NET 10 incl. WPF | ✅ 0 warnings (CI) |
| Linux Debug / Release / ASan+UBSan / TSan | ✅ 0 warnings |
| MinGW-w64 cross-build (incl. D3D11 + Media Foundation code) | ✅ 0 warnings |
| Windows package (self-contained `MixIt.exe` + LGPL FFmpeg DLLs) | ✅ artifact per CI run |

## Test results

Native **103/103** (Windows CI; Linux Debug/Release/ASan/TSan; D3D11 test runs on Windows only) · managed **37/37** (real xUnit on Windows and Linux CI) · probe soaks PASS · production self-test PASS. Details: [docs/TEST_REPORT.md](docs/TEST_REPORT.md).

## Known issues

1. The CPU compositor's cost grows with resolution × layers; a Debug native build cannot hold 1080p60 with compositing (Release can, even on 2 cores).
2. Monitors poll at 25 Hz and skip frames at 50/60 fps by design (confidence monitors, ADR-0006).
3. Capture: frames are stamped on arrival (device latency appears as a constant offset); no capture audio; a device that stalls without returning could delay stopping the session (ADR-0007).
4. Inputs can be added/removed only while production is stopped.
5. Session audio is used for A/V measurement only — no mixer, monitoring or audio output (M4).
6. Changing format or toggling the GPU compositor rebuilds the session (allowed only while stopped).

## Next recommended tasks

1. **On a Windows workstation:** the checks in "Still unverified" — especially real capture devices and the GPU vs CPU compositor numbers; report the results so defaults can be set from data.
2. Start **M4 — Audio engine**: WASAPI capture/render, mixer with per-input gain/pan/mute/solo, master + aux buses, meters, DSP chain, monitoring, and audio from file/capture inputs into the mixer; keep A/V sync with the video timeline.
3. M3 follow-ups when hardware is available: D3D11VA decode, GPU-resident frames, asynchronous capture reader with device timestamps.
