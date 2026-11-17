# MIX IT

**Professional Live Production & Broadcast Studio for Windows 10/11 x64.**
Hybrid architecture: a native C++20 media engine behind a versioned C ABI, and a
.NET 10 / WPF operator application.

> **Status: Milestone 2 (Media pipeline foundation).** Engine foundation, C ABI
> 0.2, interop, logging, capability detection, and a CPU reference media
> pipeline (exact time base, pooled buffers, bounded queues, synthetic A/V test
> signal, frame synchronization, program bus with cuts, A/V alignment
> measurement, FFmpeg with an LGPL licence guard) are implemented and tested on
> a Linux development host, incl. sanitizers and the Windows build under Wine.
> **Windows/MSVC verification is pending.** No camera/file inputs, GPU
> rendering, audio mixing, streaming or recording yet — see
> [docs/PRODUCT_SPEC.md](docs/PRODUCT_SPEC.md).

## Quick start (Windows)

```powershell
./scripts/build-windows.ps1
dotnet run --project managed/MixIt.Probe -- --verbose   # hardware report
dotnet run --project managed/MixIt.Desktop              # operator app
```

Prerequisites and Linux instructions: [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md).

## Repository map

| Path | What |
|---|---|
| `native/` | engine (`mixit_engine.dll`), public header `native/include/mixit/mixit_engine.h`, GoogleTest suite |
| `managed/MixIt.Interop` | the only P/Invoke layer |
| `managed/MixIt.Core` | application services (engine host, logging bridge) |
| `managed/MixIt.Probe` | `mixit-probe` hardware diagnostics CLI |
| `managed/MixIt.Desktop` | WPF application |
| `managed/tests` | xUnit tests against the real engine |
| `docs/` | architecture, engine design, API contract, build, tests, licences, ADRs |

## Documentation

[CLAUDE.md](CLAUDE.md) (project rules) ·
[DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md) ·
[ROADMAP.md](ROADMAP.md) ·
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) ·
[docs/ENGINE_DESIGN.md](docs/ENGINE_DESIGN.md) ·
[docs/API_CONTRACT.md](docs/API_CONTRACT.md) ·
[docs/TEST_REPORT.md](docs/TEST_REPORT.md) ·
[docs/THIRD_PARTY_LICENSES.md](docs/THIRD_PARTY_LICENSES.md) ·
[CHANGELOG.md](CHANGELOG.md)

All source code is original. MIX IT does not contain code from vMix or any other
commercial production software.
