<#
.SYNOPSIS
  Builds a runnable Windows package of MIX IT (Release, self-contained x64).

.DESCRIPTION
  1. Builds the native engine in Release (CMake preset windows-msvc).
  2. Publishes MixIt.Desktop (MixIt.exe) and mixit-probe self-contained for
     win-x64 into one folder, with mixit_engine.dll next to them.
  3. Smoke-tests the package: runs mixit-probe, then launches MixIt.exe,
     verifies it stays up, opens a window and that the engine wrote its log.

  The package does not need an installed .NET runtime.

.EXAMPLE
  ./scripts/package-windows.ps1 -Output artifacts/MixIt-win-x64
#>
param(
    [string]$Output = 'artifacts/MixIt-win-x64',
    [switch]$SkipSmokeTest
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Invoke-Step([string]$Name, [scriptblock]$Command) {
    Write-Host "==> $Name" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

$nativeDir = Join-Path $root 'build/windows-msvc/bin/Release'
$out = Join-Path $root $Output
if (Test-Path $out) { Remove-Item -Recurse -Force $out }

Invoke-Step 'Configure native engine' { cmake --preset windows-msvc }
Invoke-Step 'Build native engine (Release)' { cmake --build --preset windows-release --target mixit_engine }

$publishArgs = @('-c', 'Release', '-r', 'win-x64', '--self-contained', 'true', "-p:MixItNativeDir=$nativeDir", '-o', $out)
Invoke-Step 'Publish MixIt.Desktop' { dotnet publish managed/MixIt.Desktop/MixIt.Desktop.csproj @publishArgs }
Invoke-Step 'Publish mixit-probe' { dotnet publish managed/MixIt.Probe/MixIt.Probe.csproj @publishArgs }

# The engine (and FFmpeg DLLs when built with FFmpeg) must sit next to the executables.
Copy-Item (Join-Path $nativeDir 'mixit_engine.dll') $out -Force
Get-ChildItem $nativeDir -Include 'av*.dll', 'sw*.dll' -Recurse -ErrorAction SilentlyContinue | Copy-Item -Destination $out -Force
if ($env:FFMPEG_ROOT -and (Test-Path (Join-Path $env:FFMPEG_ROOT 'LICENSE.txt'))) {
    # LGPL: ship the licence text with the FFmpeg DLLs (see THIRD_PARTY_LICENSES.md).
    Copy-Item (Join-Path $env:FFMPEG_ROOT 'LICENSE.txt') (Join-Path $out 'FFMPEG_LICENSE.txt') -Force
}
Copy-Item (Join-Path $root 'docs/THIRD_PARTY_LICENSES.md') $out -Force
Copy-Item (Join-Path $root 'docs/REMOTE_API.md') $out -Force
Copy-Item (Join-Path $root 'docs/USER_GUIDE.md') $out -Force
# MIT: the VST3 interface code compiled into mixit_engine.dll needs its licence text.
$vst3Licence = Join-Path $root 'build/windows-msvc/_deps/vst3/pluginterfaces/LICENSE.txt'
if (Test-Path $vst3Licence) { Copy-Item $vst3Licence (Join-Path $out 'VST3_PLUGINTERFACES_LICENSE.txt') -Force }
# FreeType (FTL, attribution required) and the embedded DejaVu fonts (licence text must accompany copies).
$ftLicence = Join-Path $root 'build/windows-msvc/_deps/freetype-src/docs/FTL.TXT'
if (Test-Path $ftLicence) { Copy-Item $ftLicence (Join-Path $out 'FREETYPE_LICENSE.txt') -Force }
$dejavuLicence = Join-Path $root 'build/windows-msvc/_deps/dejavu/dejavu-fonts-ttf-2.37/LICENSE'
if (Test-Path $dejavuLicence) { Copy-Item $dejavuLicence (Join-Path $out 'DEJAVU_FONTS_LICENSE.txt') -Force }

$version = (Select-String -Path (Join-Path $root 'native/include/mixit/mixit_engine.h') -Pattern 'MIXIT_API_VERSION_(MAJOR|MINOR|PATCH) (\d+)').Matches |
    ForEach-Object { $_.Groups[2].Value }
$commit = (git rev-parse --short HEAD 2>$null)
$productVersion = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
@"
MIX IT $productVersion (engine API $($version -join '.')) — Live Production & Broadcast Studio (preview, commit $commit)
=====================================================================================

Start MixIt.exe. Sections (left): Production Studio, Input Manager, Audio Mixer,
Streaming & Recording, Graphics Studio, Replay & Multiview, Automation & Control,
Hardware Diagnostics, Settings & Workspace, License. Each section shows its status.

Tested automatically (Windows CI, software rendering, no hardware attached):
  switching (cut/mix/fade/wipe, PiP), test signals and media files, audio mixer
  with DSP and loudness, recording (MP4/MOV/MKV, ISO), streaming to local
  RTMP/SRT/HLS servers, titles and lower thirds, chroma/luma keys, overlay
  channels, multiview, instant replay, macros, the remote-control API, show files.

EXPERIMENTAL / NOT YET VERIFIED ON REAL HARDWARE — test before a live event:
  capture cards and webcams, the GPU (Direct3D 11) compositor on real GPUs,
  hardware encoders (NVENC/QSV/AMF), streaming to real platforms (RTMPS),
  WASAPI audio devices, commercial VST3 plug-ins, TSL tally, VISCA PTZ,
  full-screen outputs on extra displays, extensions.

Editions: without a licence MIX IT runs as the Free edition (up to 4 video
inputs, 1 stream destination; see License). Licences are checked offline.

Command-line diagnostics:  mixit-probe.exe --verbose | --codecs | --session-test 30
Remote control API: docs in REMOTE_API.md (enable in Automation & Control).

FFmpeg: the FFmpeg DLLs here are an LGPL build (FFMPEG_LICENSE.txt). Third-party
notices: THIRD_PARTY_LICENSES.md and the *_LICENSE.txt files.
Requirements: Windows 10/11 x64. No .NET installation needed. Not code-signed yet:
Windows SmartScreen may warn on first start.
Logs: %LOCALAPPDATA%\MixIt\logs\   Settings: %APPDATA%\MIX IT\
"@ | Set-Content -Encoding UTF8 (Join-Path $out 'README.txt')

if ($SkipSmokeTest) { return }

Invoke-Step 'Smoke test: mixit-probe' { & (Join-Path $out 'mixit-probe.exe') --codecs }
Invoke-Step 'Smoke test: mixit-probe session (10 s)' { & (Join-Path $out 'mixit-probe.exe') --session-test 10 }

Write-Host '==> Smoke test: MixIt.exe' -ForegroundColor Cyan
$logDir = Join-Path $env:LOCALAPPDATA 'MixIt\logs'
$startedAt = Get-Date
$app = Start-Process -FilePath (Join-Path $out 'MixIt.exe') -PassThru
try {
    $deadline = (Get-Date).AddSeconds(30)
    $window = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        $app.Refresh()
        if ($app.HasExited) { throw "MixIt.exe exited during start-up with code $($app.ExitCode)" }
        if ($app.MainWindowHandle -ne 0) { $window = $true; break }
    }
    if (-not $window) { throw 'MixIt.exe did not open a window within 30 s' }
    Write-Host "  window opened: '$($app.MainWindowTitle)'"
    Start-Sleep -Seconds 5  # let the engine start and detect hardware
    $app.Refresh()
    if ($app.HasExited) { throw "MixIt.exe exited after opening its window (code $($app.ExitCode))" }

    $log = Get-ChildItem $logDir -Filter 'mixit-*.jsonl' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $startedAt } | Sort-Object LastWriteTime | Select-Object -Last 1
    if (-not $log) { throw "MixIt.exe wrote no engine log to $logDir" }
    $text = Get-Content $log.FullName -Raw
    foreach ($needle in @('engine initialized', '"category":"caps"')) {
        if ($text -notmatch [regex]::Escape($needle)) { throw "engine log $($log.Name) lacks '$needle'" }
    }
    Write-Host "  engine log OK: $($log.FullName)"
}
finally {
    if (-not $app.HasExited) {
        $null = $app.CloseMainWindow()
        if (-not $app.WaitForExit(10000)) { Stop-Process -Id $app.Id -Force }
    }
}
Write-Host '==> Self-test: MixIt.exe --self-test production' -ForegroundColor Cyan
# Drives the Production Studio view model like an operator (two test inputs,
# start, monitors, CUT, AUTO, PiP, stop) and exits 0 = PASS, 1 = FAIL.
$startedAt = Get-Date
$selfTest = Start-Process -FilePath (Join-Path $out 'MixIt.exe') -ArgumentList '--self-test', 'production' -PassThru
if (-not $selfTest.WaitForExit(180000)) {
    Stop-Process -Id $selfTest.Id -Force
    throw 'MixIt.exe --self-test production did not finish within 180 s'
}
$log = Get-ChildItem $logDir -Filter 'mixit-*.jsonl' -ErrorAction SilentlyContinue |
    Where-Object { $_.LastWriteTime -ge $startedAt } | Sort-Object LastWriteTime | Select-Object -Last 1
$verdict = if ($log) { Select-String -Path $log.FullName -Pattern 'Self-test production' | ForEach-Object { $_.Line } } else { @() }
$verdict | ForEach-Object { Write-Host "  $_" }
if ($selfTest.ExitCode -ne 0) { throw "Production self-test failed (exit code $($selfTest.ExitCode))" }
if (-not ($verdict -match 'Self-test production: PASS')) { throw 'Production self-test wrote no PASS line to the log' }

Write-Host "Package ready: $out" -ForegroundColor Green
