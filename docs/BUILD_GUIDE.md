# MIX IT — Build Guide

## Windows 10/11 x64 (product platform)

### Prerequisites

| Tool | Version | Notes |
|---|---|---|
| Visual Studio 2022 | 17.10 or newer | Workload "Desktop development with C++" (MSVC v143, Windows 10/11 SDK) and ".NET desktop development" |
| CMake | ≥ 3.25 | bundled with VS or from cmake.org |
| .NET SDK | 10.0.100 or newer 10.0.x feature band | pinned by `global.json` |
| Git | any | |
| Internet access | nuget.org, github.com | NuGet packages; GoogleTest is fetched if not installed (or install via vcpkg) |

### One command

```powershell
./scripts/build-windows.ps1                 # Debug
./scripts/build-windows.ps1 -Configuration Release
```

This configures and builds the engine (MSVC), runs the native tests, builds the
.NET solution, runs the managed tests and the `mixit-probe` smoke test.

### Step by step

```powershell
cmake --preset windows-msvc                 # generates build/windows-msvc (VS solution)
cmake --build --preset windows-debug        # -> build/windows-msvc/bin/Debug/mixit_engine.dll
ctest --preset windows-debug                # native tests

dotnet build MixIt.sln -c Debug             # copies mixit_engine.dll next to apps/tests
dotnet test managed/tests/MixIt.Tests/MixIt.Tests.csproj -c Debug
dotnet run --project managed/MixIt.Probe -- --verbose
dotnet run --project managed/MixIt.Desktop  # the WPF app
```

### FFmpeg (media features, from M2)

The engine builds without FFmpeg, but media features need it. On Windows,
point CMake at a **LGPL shared** FFmpeg build (see ADR-0004):

```powershell
# The same pinned LGPL build CI uses (BtbN FFmpeg-Builds, SHA-256 checked; needs the gh CLI):
./scripts/fetch-ffmpeg-windows.ps1 -Tag autobuild-2026-11-16-15-29 `
    -Sha256 bc661777e3aeb29cbd9e68f8b29a4ea1b54cc0db0373ff447085e5cb4ef307b3   # sets FFMPEG_ROOT
# or any LGPL shared build:
$env:FFMPEG_ROOT = "C:\sdk\ffmpeg-lgpl-shared"   # contains include\, lib\ (*.lib), bin\ (*.dll)
cmake --preset windows-msvc                         # prints "MIX IT: FFmpeg TRUE"
```

Required libraries: avcodec, avformat, avutil, swscale, swresample.

The DLLs are copied next to `mixit_engine.dll`. Windows builds enforce the
licence policy (`MIXIT_REQUIRE_LGPL_FFMPEG=ON`): a GPL or non-free FFmpeg is
rejected at runtime. Check with `mixit-probe --codecs`.

Platform libraries (Windows SDK, nothing to install): d3d11, dxgi, ole32,
mfuuid (GUIDs only). `d3dcompiler_47.dll` and Media Foundation are loaded at
run time; without them the GPU compositor / capture report NOT_SUPPORTED and
the engine keeps working.

### Diagnostics

```powershell
dotnet run --project managed/MixIt.Probe -- --codecs                       # FFmpeg + codec availability
dotnet run --project managed/MixIt.Probe -- --session-test 3600 --format 1920x1080@60000/1001   # 1 h soak, exit 0 = PASS
dotnet run --project managed/MixIt.Probe -- --session-test 600 --format 1920x1080@60 --file C:\clips\test.mp4   # file input
dotnet run --project managed/MixIt.Probe -- --session-test 60 --compositor d3d11     # GPU compositor (experimental)
dotnet run --project managed/MixIt.Probe -- --list-capture                           # webcams / capture cards
dotnet run --project managed/MixIt.Probe -- --session-test 60 --capture 0            # capture input (experimental)
MixIt.exe --self-test production    # drives the Production Studio; exit 0 = PASS, verdict in the log
```

The managed build looks for the engine in `build/windows-msvc/bin/<Configuration>`.
Use another location with `-p:MixItNativeDir=<folder>`. If the engine is missing
the build warns and the app/tests fail at runtime with a message pointing here.

Logs: the desktop app writes JSON-lines logs to
`%LOCALAPPDATA%\MixIt\logs\mixit-<timestamp>.jsonl`.

## Linux (development host only)

Linux is not a product platform. It is supported for building and testing the
engine core, running sanitizers, and cross-checking Windows code.

```bash
sudo apt-get install -y cmake ninja-build g++ libgtest-dev libgmock-dev dotnet-sdk-10.0 \
     pkg-config libavcodec-dev libavformat-dev libavutil-dev   # FFmpeg optional (distro builds are GPL: dev only)
# optional: Windows cross-check and Wine execution
sudo apt-get install -y g++-mingw-w64-x86-64-posix wine64

scripts/verify-linux.sh                 # debug + ASan/UBSan + TSan, probe, xUnit
scripts/verify-linux.sh --mingw         # + MinGW cross-compile of Windows paths
scripts/verify-linux.sh --offline       # no nuget.org: skips xUnit, uses offline mode
scripts/verify-linux.sh --soak 30       # + 30 s session soak test
scripts/run-windows-tests-under-wine.sh # Windows test binary under Wine
```

Individual presets: `linux-debug`, `linux-release`, `linux-asan`, `linux-tsan`,
`mingw-crosscheck` (`cmake --preset <p> && cmake --build --preset <p> && ctest --preset <p>`).

The WPF project (`MixIt.Desktop`) can be compiled on Linux only with nuget.org
access (it needs the Windows Desktop targeting pack; `EnableWindowsTargeting`
is set). It cannot run on Linux. `MixIt.Engine.slnf` contains every project
except the WPF app.

### Offline verification mode

`-p:MixItOfflineVerification=true` lets Interop, Core and the probe build without
nuget.org by taking `Microsoft.Extensions.Logging.Abstractions` from the
locally installed ASP.NET Core shared framework, and disables NuGet auditing.
It exists only for restricted development environments — never use it for a
release build.

## CMake options

| Option | Default | Meaning |
|---|---|---|
| `MIXIT_BUILD_TESTS` | ON | build GoogleTest suite |
| `MIXIT_WARNINGS_AS_ERRORS` | ON | `/WX` / `-Werror` |
| `MIXIT_ENABLE_SANITIZERS` | OFF | ASan + UBSan (GCC/Clang) |
| `MIXIT_ENABLE_TSAN` | OFF | ThreadSanitizer (GCC/Clang) |
| `MIXIT_WITH_FFMPEG` | AUTO | `AUTO` / `ON` (fail if missing) / `OFF` |
| `MIXIT_REQUIRE_LGPL_FFMPEG` | ON on Windows, OFF elsewhere | reject non-LGPL FFmpeg at runtime |
| `FFMPEG_ROOT` | env `FFMPEG_ROOT` | FFmpeg shared build (include/, lib/, bin/) |
| `MIXIT_ENABLE_VST3` | ON | VST3 plug-in host (ADR-0008). Fetches the pinned `vst3_pluginterfaces` (MIT) with git at configure time; OFF builds without plug-ins |
| `MIXIT_ENABLE_GRAPHICS` | ON | graphics inputs and multiview labels (ADR-0011). Fetches the pinned FreeType (git) and the DejaVu fonts zip (hash-checked) at configure time; OFF builds without graphics (`NOT_SUPPORTED`) |
| `MIXIT_FREETYPE_DIR` | empty | offline: path of an existing FreeType checkout of the pinned commit (no download) |
| `MIXIT_DEJAVU_ZIP` | empty | offline: path of `dejavu-fonts-ttf-2.37.zip` (hash still checked) |
| `MIXIT_VST3_INTERFACES_DIR` | empty | offline: path of an existing checkout of the pinned commit; the folder itself must be named `pluginterfaces` (no download) |

With tests enabled, the build also produces a small VST3 test plug-in at
`build/<preset>/vst3/MixItTestGain.vst3` (used by the native and managed tests;
not part of the product).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `MixItNativeException: The MIX IT native engine (mixit_engine) could not be loaded` | `mixit_engine.dll` not next to the executable — build the engine first or set `MixItNativeDir`. |
| `VersionMismatch` | Managed binding and native library from different API versions — rebuild both. |
| `NU1301` / `NU1900` errors | No nuget.org access; use offline mode for Interop/Core/Probe only. |
| `gtest_discover_tests` timeout when cross-compiling | discovery runs at `ctest` time (`PRE_TEST`); run the `.exe` directly under Wine instead. |
