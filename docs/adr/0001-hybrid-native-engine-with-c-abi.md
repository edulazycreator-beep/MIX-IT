# ADR-0001: Hybrid C# + C++ with a single versioned C ABI

- Status: Accepted (M1)
- Date: 2026-11-17

## Context

MIX IT needs hard real-time audio, frame-accurate GPU video and many vendor SDKs
(FFmpeg, DeckLink, NDI, ASIO, VST3) that are C/C++ APIs, plus a large, evolving
operator UI. A garbage-collected runtime cannot guarantee real-time deadlines;
C++ is costly for UI and application logic.

## Decision

- All media processing lives in one native C++20 library (`mixit_engine`).
- The UI and application services are C# (.NET 10, WPF, MVVM).
- They communicate **only** through a plain-C ABI in `mixit_engine.h`: opaque
  handles, fixed-width types, `struct_size`-versioned structs, error codes and a
  per-thread last-error message; no C++ types and no exceptions cross it.
- Exactly one managed assembly (`MixIt.Interop`) contains P/Invoke code,
  generated with `LibraryImport`.
- Handles are registry-validated 64-bit ids, never raw pointers, so misuse
  yields `MIXIT_E_INVALID_HANDLE` instead of memory corruption.

## Consequences

- + Real-time code is isolated from GC; UI hangs cannot stall media threads.
- + The ABI is testable from C++ directly (most M1 tests do), and stable enough to support an out-of-process engine or third-party SDK later.
- + Struct layouts are asserted on both sides, catching drift at build/test time.
- − Every feature needs ABI plumbing; high-rate data (video, meters) must use dedicated paths (swap chains, snapshots) rather than per-item calls.
- − Two toolchains (MSVC + .NET) in the build.

## Alternatives considered

- **C++/CLI bridge:** Windows/MSVC-only, couples managed and native lifetimes, harder to test, cannot be used from other languages. Rejected.
- **COM interfaces:** heavier, registration/marshalling complexity, no benefit for an in-process private engine. Rejected.
- **All-C++ (Qt UI):** strong option but contradicts the mandated C#/WPF stack and adds an LGPL/commercial Qt licence decision. Rejected for this product.
