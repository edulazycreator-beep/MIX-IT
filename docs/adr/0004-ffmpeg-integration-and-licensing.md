# ADR-0004: FFmpeg integration and the LGPL licence guard

- Status: Accepted (M2)
- Date: 2026-11-17

## Context

FFmpeg provides decoding, encoding and muxing for M3–M5. It can be built as
LGPL-2.1+ or, when GPL components (libx264, libx265, …) are enabled, as GPL;
"non-free" builds are not redistributable at all. MIX IT is a closed-source
commercial product, so a distributed build must use an LGPL FFmpeg, linked
dynamically (ARCHITECTURE §3). Development hosts, however, often only have
their distribution's FFmpeg — Ubuntu's is a GPL build.

## Decision

1. **Optional at build time.** `MIXIT_WITH_FFMPEG=AUTO|ON|OFF`. On Windows the
   engine links a prebuilt **LGPL shared** FFmpeg from `FFMPEG_ROOT`
   (`include/`, `lib/` import libraries, `bin/` DLLs, copied next to
   `mixit_engine.dll`). Dev hosts use pkg-config. Cross builds never use
   host pkg-config.
2. **Runtime licence guard.** At startup the engine reads the licence strings
   of libavcodec, libavformat and libavutil. If `MIXIT_REQUIRE_LGPL_FFMPEG` is ON
   (default on Windows) and any of them is not `LGPL…`, FFmpeg is **rejected**:
   flagged not usable, logged as an error, and every FFmpeg-backed feature
   returns `MIXIT_E_NOT_SUPPORTED`. With the policy OFF (dev hosts) a warning
   states that the build must never be distributed.
3. **ABI-mismatch guard.** Runtime major versions must equal the build
   headers' majors; otherwise FFmpeg is rejected (protects against a wrong DLL
   on the path).
4. **Availability ≠ capability.** `mixit_query_codec` reports whether a codec
   exists in the build (decoder/encoder/hardware). Hardware encoders are
   open-tested against the actual GPU/driver in M5 before being offered.
5. **Binary source (to be completed before the first Windows FFmpeg build):**
   a pinned LGPL shared build (exact version, `configure` line, SHA-256 and the
   corresponding source archive) recorded in THIRD_PARTY_LICENSES.md. Building
   FFmpeg ourselves with a scripted, reviewed `configure` line is preferred over
   third-party binaries so the licence configuration is provable.

## Consequences

- + A GPL or non-free FFmpeg can never silently ship in a product build (verified: with the policy ON, Ubuntu's GPL FFmpeg is rejected and codec queries fail).
- + Engine and tests build without FFmpeg (MinGW cross-check, minimal CI).
- − Software H.264/HEVC encoding is unavailable in LGPL builds (no x264/x265); options are hardware encoders, OpenH264 or a commercial x264 licence (decision in M5).
- − Windows developers must provide `FFMPEG_ROOT` to get media features.
