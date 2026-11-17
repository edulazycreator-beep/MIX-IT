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

$version = (Select-String -Path (Join-Path $root 'native/include/mixit/mixit_engine.h') -Pattern 'MIXIT_API_VERSION_(MAJOR|MINOR|PATCH) (\d+)').Matches |
    ForEach-Object { $_.Groups[2].Value }
$commit = (git rev-parse --short HEAD 2>$null)
@"
MIX IT $($version -join '.') (development build, commit $commit)
=====================================================================

This is an EARLY DEVELOPMENT BUILD (Milestones 1-2), not a production tool.

What works:
  MixIt.exe        Opens the operator shell. Only "Hardware Diagnostics" is
                   functional: engine status, CPU/RAM/OS/GPU detection and the
                   live engine log. All other sections are labelled "Planned".
  mixit-probe.exe  Command-line diagnostics:
                     mixit-probe.exe --verbose
                     mixit-probe.exe --codecs
                     mixit-probe.exe --session-test 60 --format 1920x1080@60000/1001
                   The session test runs the synthetic production pipeline
                   (test signal, frame sync, program bus, A/V sync check) and
                   prints PASS/FAIL.

                   Add a media file as the second input:
                     mixit-probe.exe --session-test 30 --file C:\path\clip.mp4

Not yet available in the app UI: inputs, GPU rendering, audio mixing,
streaming, recording, graphics.

FFmpeg: the FFmpeg DLLs in this folder are an LGPL build (see FFMPEG_LICENSE.txt);
the engine refuses to use non-LGPL FFmpeg builds.

Requirements: Windows 10/11 x64. No .NET installation needed.
Logs: %LOCALAPPDATA%\MixIt\logs\
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
Write-Host "Package ready: $out" -ForegroundColor Green
