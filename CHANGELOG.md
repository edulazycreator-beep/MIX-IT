# Changelog

All notable changes to MIX IT. Format: Keep a Changelog; versions follow the
native API version until the first product release.

## [0.2.0] — 2026-11-17 — Milestone 2: Media pipeline foundation

### Added
- C ABI 0.2.0 (25 exports): sessions (`mixit_session_*`: create/destroy, test-signal inputs, program selection, start/stop, statistics, program frame copy) and media library queries (`mixit_get_media_library_info`, `mixit_query_codec`).
- Exact media time base (rational rates, portable 128-bit muldiv; frame/sample times computed from indices — no drift).
- Fixed-capacity, ref-counted frame and audio pools; bounded media queues with DropNewest/DropOldest policies and counters.
- Deterministic test signal: colour bars, moving stripe, machine-readable frame code with sentinel, flash frames with sample-aligned clicks, tone.
- Session pipeline: paced producer per input, output clock, frame synchronization (repeat/skip), program bus (zero-copy, nearest scaling, black), cuts while running, A/V alignment measurement, stall recovery.
- Precise pacing: absolute `clock_nanosleep` (Linux), high-resolution waitable timers (Windows).
- Optional FFmpeg integration with runtime LGPL licence guard and version check (ADR-0004); CMake `MIXIT_WITH_FFMPEG`, `MIXIT_REQUIRE_LGPL_FFMPEG`, `FFMPEG_ROOT`.
- Managed `NativeSession`, `MediaRate`, statistics records, media library API; `EngineHost.CreateSession`.
- `mixit-probe --codecs` and `--session-test N [--format WxH@rate]` soak test with PASS/FAIL criteria; `verify-linux.sh --soak N`.
- 35 native tests (75 total) and 5 managed tests (30 total).

### Fixed
- Uniform (e.g. black) images decoded as a valid frame code — sentinel added.
- Quick Sync codecs not reported as hardware-backed.

## [0.1.0] — 2026-11-17 — Milestone 1: Foundation

### Added
- Native engine `mixit_engine` (C++20) with a versioned C ABI (`mixit_engine.h`, API 0.1.0): engine create/shutdown/destroy, diagnostics, logging, capability detection, GPU adapter queries, monotonic clock, per-thread last error.
- Opaque, never-reused engine handles validated through a registry; safe against concurrent destroy.
- Lock-free bounded MPMC queue; engine logger with JSON-lines file sink and host callback; drop-and-count overload policy.
- Capability probes: CPUID (SSE2…AVX-512F with OS state check), cores, memory, OS version, DXGI GPU adapters (Windows).
- CMake presets for MSVC, Linux (debug/release/ASan+UBSan/TSan) and a MinGW-w64 Windows cross-check; strict warnings as errors.
- GoogleTest suite (40 tests).
- .NET 10: `MixIt.Interop` (LibraryImport, SafeHandle, callbacks), `MixIt.Core` (EngineHost, EngineLogStream, ILogger→engine forwarding), `mixit-probe` CLI, `MixIt.Desktop` WPF shell (unverified), xUnit tests (25).
- Documentation: architecture, engine design, API contract, build guide, test report, licence inventory, roadmap, status, 3 ADRs.

### Fixed (during M1 verification)
- Windows-only test bug (deleting an open file), DXGI-unavailable handling under Wine, cast-free public header macros, Linux export list, MinGW DLL name, MSVC C4324, three WPF compile errors.
