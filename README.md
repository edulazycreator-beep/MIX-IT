# MIX IT

**Professional Live Production & Broadcast Studio for Windows 10/11 x64.**
Hybrid architecture: a native C++20 media engine behind a versioned C ABI, and a
.NET 10 / WPF operator application.

> **Status: Milestone 3 (first working video pipeline) implemented.** The
> Production Studio runs a real video pipeline: test-signal, media-file
> (FFmpeg) and — experimentally — webcam/capture-card inputs, preview/program
> monitors with tally, CUT, AUTO (mix, fade, wipe), picture-in-picture and 8
> layers per bus, on a CPU compositor (Direct3D 11 compositor experimental).
> Built and tested on Linux and on Windows CI (MSVC, WPF, LGPL FFmpeg); not yet
> run by hand on Windows hardware or with real capture devices. **No audio
> mixing, streaming or recording yet** — nothing is broadcast. See
> [docs/PRODUCT_SPEC.md](docs/PRODUCT_SPEC.md) and
> [DEVELOPMENT_STATUS.md](DEVELOPMENT_STATUS.md).

## Quick start (Windows)

```powershell
./scripts/build-windows.ps1
dotnet run --project managed/MixIt.Probe -- --verbose   # hardware report
dotnet run --project managed/MixIt.Desktop              # operator app (Production Studio)
```

Prerequisites and Linux instructions: [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md).

## Repository map

| Path | What |
|---|---|
| `native/` | engine (`mixit_engine.dll`), public header `native/include/mixit/mixit_engine.h`, GoogleTest suite |
| `managed/MixIt.Interop` | the only P/Invoke layer |
| `managed/MixIt.Core` | application services (engine host, logging bridge, production service) |
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
