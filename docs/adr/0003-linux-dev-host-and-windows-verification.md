# ADR-0003: Linux as a development host; Windows is the only product platform

- Status: Accepted (M1)
- Date: 2026-11-17

## Context

Development sessions (Claude Code) may run in Linux environments without
MSVC, WPF or unrestricted network access. The product targets Windows 10/11 only.

## Decision

- The engine core is written portably where it costs nothing (standard C++20,
  `std::thread`, `steady_clock`). Platform code lives in `src/platform/*_windows.cpp`
  and `*_linux.cpp`; Linux implementations exist only to let the core build and
  be tested, and are labelled "development host" in all output.
- Linux CI runs debug, ASan+UBSan and TSan builds — sanitizers are far easier
  there than with MSVC.
- Windows code is compile-checked with MinGW-w64 (`mingw-crosscheck` preset)
  and its tests can be executed under Wine. Neither replaces MSVC builds and
  testing on real Windows hardware, which remain required to close a milestone.
- Features with no Linux implementation (e.g. GPU enumeration) report
  "not detected" rather than inventing values.

## Consequences

- + Sanitizer coverage, faster iteration, two compilers catching more bugs (Wine already found a Windows-only file-sharing bug).
- − Some duplication in platform probes; discipline needed so Linux-only behaviour never leaks into product decisions.
