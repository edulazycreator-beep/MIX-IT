#!/usr/bin/env bash
# Builds the native test suite for Windows with MinGW-w64 and runs it under
# Wine. This executes the real Windows code paths (Win32 / DXGI) from a
# Linux host. It does not replace testing on Windows with MSVC:
#   - Wine's DXGI may report no adapters (headless); tests degrade accordingly.
#   - MinGW is a different compiler/CRT than the MSVC release toolchain.
# Requires: g++-mingw-w64-x86-64-posix, wine64, network access to GitHub
# (GoogleTest is fetched for the cross build).
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD=build/mingw-wine-tests
cmake -S . -B "$BUILD" -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake \
  -DCMAKE_BUILD_TYPE=Debug -DMIXIT_BUILD_TESTS=ON
cmake --build "$BUILD"

export WINEDEBUG=-all
export WINEPREFIX="${WINEPREFIX:-$PWD/build/wineprefix}"
GCC_DIR="$(dirname "$(x86_64-w64-mingw32-g++-posix -print-libgcc-file-name)")"
export WINEPATH="$GCC_DIR;/usr/x86_64-w64-mingw32/lib"

cd "$BUILD/bin"
wine mixit_native_tests.exe "$@"
