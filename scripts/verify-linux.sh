#!/usr/bin/env bash
# Development-host verification (Linux). Linux is NOT a product platform;
# this script exists so the engine core can be built and tested without
# Windows, including under sanitizers and as a MinGW cross-build.
#
#   scripts/verify-linux.sh            # native: debug + ASan/UBSan + TSan, probe
#   scripts/verify-linux.sh --mingw    # + MinGW cross-compile of Windows paths
#   scripts/verify-linux.sh --offline  # build managed code without nuget.org
#                                        (MixItOfflineVerification mode)
#   scripts/verify-linux.sh --soak N   # + N-second session soak test (mixit-probe)
set -euo pipefail
cd "$(dirname "$0")/.."

MINGW=0
SOAK=0
DOTNET_ARGS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --mingw) MINGW=1 ;;
    --offline) DOTNET_ARGS+=("-p:MixItOfflineVerification=true") ;;
    --soak) SOAK="${2:?--soak needs a number of seconds}"; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
  shift
done

step() { printf '\n==> %s\n' "$*"; }

for preset in linux-debug linux-asan linux-tsan; do
  step "native $preset"
  cmake --preset "$preset" >/dev/null
  cmake --build --preset "$preset"
  ctest --preset "$preset"
done

if [[ $MINGW -eq 1 ]]; then
  step "MinGW cross-compile check (Windows code paths)"
  cmake --preset mingw-crosscheck >/dev/null
  cmake --build --preset mingw-crosscheck
fi

if command -v dotnet >/dev/null; then
  step "managed: Interop + Core + Probe"
  dotnet build managed/MixIt.Probe/MixIt.Probe.csproj -c Debug "${DOTNET_ARGS[@]}"
  step "probe smoke test"
  dotnet managed/MixIt.Probe/bin/Debug/net10.0/mixit-probe.dll --codecs
  if [[ $SOAK -gt 0 ]]; then
    step "session soak test (${SOAK} s)"
    dotnet managed/MixIt.Probe/bin/Debug/net10.0/mixit-probe.dll --session-test "$SOAK"
  fi
  if [[ ${#DOTNET_ARGS[@]} -eq 0 ]]; then
    step "managed tests (xUnit)"
    dotnet test managed/tests/MixIt.Tests/MixIt.Tests.csproj -c Debug
  else
    echo "offline mode: skipping xUnit tests (packages unavailable)"
  fi
fi

step "all requested checks passed"
