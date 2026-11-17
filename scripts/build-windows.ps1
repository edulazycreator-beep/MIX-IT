<#
.SYNOPSIS
  Builds and tests MIX IT on Windows (the product platform).

.DESCRIPTION
  1. Configures and builds the native engine with MSVC (CMake preset windows-msvc).
  2. Runs the native GoogleTest suite (ctest).
  3. Builds the .NET solution (Interop, Core, Probe, Desktop, Tests) against that engine.
  4. Runs the managed tests and the mixit-probe smoke test.

.EXAMPLE
  ./scripts/build-windows.ps1 -Configuration Release
#>
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Invoke-Step([string]$Name, [scriptblock]$Command) {
    Write-Host "==> $Name" -ForegroundColor Cyan
    & $Command
    if ($LASTEXITCODE -ne 0) { throw "$Name failed with exit code $LASTEXITCODE" }
}

$preset = if ($Configuration -eq 'Debug') { 'windows-debug' } else { 'windows-release' }

Invoke-Step 'Configure native engine' { cmake --preset windows-msvc }
Invoke-Step 'Build native engine'     { cmake --build --preset $preset }
Invoke-Step 'Native tests'            { ctest --preset $preset }

$nativeDir = Join-Path $root "build/windows-msvc/bin/$Configuration"
Invoke-Step 'Build .NET solution'     { dotnet build MixIt.sln -c $Configuration "-p:MixItNativeDir=$nativeDir" }
Invoke-Step 'Managed tests'           { dotnet test managed/tests/MixIt.Tests/MixIt.Tests.csproj -c $Configuration --no-build "-p:MixItNativeDir=$nativeDir" }
Invoke-Step 'Probe smoke test'        { dotnet "managed/MixIt.Probe/bin/$Configuration/net10.0/mixit-probe.dll" }

Write-Host "All steps passed." -ForegroundColor Green
