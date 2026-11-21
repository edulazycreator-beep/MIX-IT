<#
.SYNOPSIS
  Builds the MIX IT installer (Inno Setup 6) from the package folder made by
  package-windows.ps1. Not code-signed until the owner provides a certificate.
#>
param(
    [string]$Package = 'artifacts/MixIt-win-x64',
    [string]$OutputDir = 'artifacts'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) {
    Write-Host '==> Installing Inno Setup (chocolatey)' -ForegroundColor Cyan
    choco install innosetup -y --no-progress | Out-Host
    $iscc = "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe"
    if (-not (Test-Path $iscc)) { throw 'Inno Setup 6 (ISCC.exe) is not available' }
}
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()  # product version (the engine ABI has its own)
if (-not (Test-Path (Join-Path $Package 'MixIt.exe'))) { throw "No package at $Package (run package-windows.ps1 first)" }
$source = (Resolve-Path $Package).Path
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$out = (Resolve-Path $OutputDir).Path
Write-Host "==> Building installer $version" -ForegroundColor Cyan
& $iscc "/DAppVersion=$version" "/DSourceDir=$source" "/DOutputDir=$out" (Join-Path $root 'installer/MixIt.iss') | Out-Host
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
$setup = Join-Path $out "MixIt-Setup-$version-x64.exe"
if (-not (Test-Path $setup)) { throw "Installer not found: $setup" }

# Silent install/uninstall check on the CI machine.
Write-Host '==> Installer test: silent install, run probe, uninstall' -ForegroundColor Cyan
$dir = Join-Path $env:TEMP 'MixItInstallTest'
$p = Start-Process $setup -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=$dir" -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "silent install failed ($($p.ExitCode))" }
& (Join-Path $dir 'mixit-probe.exe') --codecs | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'installed mixit-probe failed' }
$u = Start-Process (Join-Path $dir 'unins000.exe') -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
if ($u.ExitCode -ne 0) { throw "silent uninstall failed ($($u.ExitCode))" }
# The uninstaller re-launches itself from %TEMP%: wait for the files to go.
$deadline = (Get-Date).AddSeconds(60)
while ((Test-Path (Join-Path $dir 'MixIt.exe')) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
if (Test-Path (Join-Path $dir 'MixIt.exe')) { throw 'uninstall left MixIt.exe behind' }
Write-Host "Installer ready: $setup" -ForegroundColor Green
