<#
.SYNOPSIS
  Downloads a pinned LGPL shared FFmpeg build for Windows x64 and prepares FFMPEG_ROOT.

.DESCRIPTION
  Source: BtbN/FFmpeg-Builds GitHub release <Tag> (an immutable autobuild
  release). From that release the newest *release-branch* asset matching
  ffmpeg-n<major>.<minor>...-win64-lgpl-shared-<major>.<minor>.zip is chosen,
  so the selection is deterministic for a given tag. If -Sha256 is given the
  archive must match it (pinning); the hash is always printed so it can be
  recorded in docs/THIRD_PARTY_LICENSES.md.

  The engine enforces the licence at runtime as well (ADR-0004): a non-LGPL
  FFmpeg is rejected on Windows builds.

  Requires the GitHub CLI with a token (GH_TOKEN) — available in CI.

.EXAMPLE
  ./scripts/fetch-ffmpeg-windows.ps1 -Tag autobuild-2026-11-16-15-29 -Destination build/ffmpeg
#>
param(
    [Parameter(Mandatory)] [string]$Tag,
    [string]$Destination = 'build/ffmpeg',
    [string]$Sha256 = ''
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$assets = gh release view $Tag -R BtbN/FFmpeg-Builds --json assets -q '.assets[].name'
if ($LASTEXITCODE -ne 0) { throw "release $Tag of BtbN/FFmpeg-Builds not found (pruned?) — pick a newer tag" }

$candidates = $assets | Where-Object { $_ -match '^ffmpeg-n(\d+)\.(\d+).*-win64-lgpl-shared-\d+\.\d+\.zip$' } |
    ForEach-Object { [pscustomobject]@{ Name = $_; Version = [version]"$($Matches[1]).$($Matches[2])" } } |
    Sort-Object Version -Descending
if (-not $candidates) { throw "no win64-lgpl-shared release-branch asset in $Tag" }
$asset = $candidates[0].Name
Write-Host "FFmpeg asset: $asset (release $Tag)"

$download = Join-Path $root 'build/ffmpeg-download'
New-Item -ItemType Directory -Force $download | Out-Null
gh release download $Tag -R BtbN/FFmpeg-Builds -p $asset -D $download --clobber
if ($LASTEXITCODE -ne 0) { throw "download of $asset failed" }
$zip = Join-Path $download $asset

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Host "SHA-256: $hash"
if ($Sha256 -and $hash -ne $Sha256.ToLowerInvariant()) { throw "SHA-256 mismatch for $asset (expected $Sha256)" }

$dest = Join-Path $root $Destination
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
Expand-Archive $zip -DestinationPath $download -Force
$extracted = Get-ChildItem $download -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'include/libavcodec/avcodec.h') } |
    Select-Object -First 1
if (-not $extracted) { throw "unexpected archive layout in $asset" }
Move-Item $extracted.FullName $dest

foreach ($lib in 'avcodec', 'avformat', 'avutil', 'swscale', 'swresample') {
    if (-not (Test-Path (Join-Path $dest "lib/$lib.lib"))) { throw "import library $lib.lib missing in $asset" }
}
Get-ChildItem (Join-Path $dest 'bin') -Filter '*.dll' | ForEach-Object { Write-Host "  $($_.Name)" }

if ($env:GITHUB_ENV) {
    "FFMPEG_ROOT=$dest" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
    "MIXIT_FFMPEG_ASSET=$asset" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}
Write-Host "FFMPEG_ROOT=$dest"
