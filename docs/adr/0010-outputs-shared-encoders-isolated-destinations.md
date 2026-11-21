# ADR-0010: Outputs — shared encoders, isolated destinations, timeline timestamps

- Status: Accepted (M5)
- Date: 2026-11-21

## Context

A live production records and streams to several places at once (a program
recording, one or more platforms, ISO recordings). Encoding is the expensive
part; destinations fail independently (a platform drops the connection, a
drive is slow). CLAUDE.md §16.K: one destination must never block the
pipeline or the others; recordings must not lose data silently.

## Decision

1. **FFmpeg (libavcodec/libavformat) for encoding and muxing**, already the
   media dependency (LGPL build, ADR-0003). H.264 + AAC. The encoder is chosen
   by open-testing, hardware first: NVENC, Quick Sync, AMF, Media Foundation,
   then OpenH264 (LGPL build) or x264 (dev builds only, GPL). Live settings:
   CBR-like rate control, no B-frames, fixed GOP (keyframe interval), BT.709
   limited range, global headers.
2. **One encoder per distinct (source, encoder, bitrates, GOP)**, shared by
   every output with those settings (`EncoderPipeline`, own thread). The
   session feeds it lock-free: the tick thread hands over frame references
   (2-frame queue, drop + count when full), the audio thread copies mixed
   10 ms blocks into a 2.56 s ring. Pipelines are unpublished and flushed with
   the same epoch hand-over as plug-ins (ADR-0008).
3. **Timestamps come from the session timeline**: video pts = tick index,
   audio pts = sample index, both relative to the first encoded tick. Muxers
   interleave by them; A/V sync needs no per-output correction (measured
   0.02 ms flash/click offset in a recorded MP4).
4. **One `Sink` per destination**: own thread, own bounded queue (4 s network,
   20 s files). A sink that falls behind drops its queue and resumes at the
   next keyframe (counted; files flag data loss and log an error). Network
   sinks reconnect with back-off (1–30 s), every connection/file starts at a
   keyframe (forced on the encoder) with timestamps from 0. A stop while
   connecting cancels the attempt; a stop while running drains and finalizes.
5. **Container choices**: fragmented MP4/MOV by default (playable after a
   crash), MKV, MPEG-TS and HLS (SPS/PPS repeated before keyframes), FLV for
   RTMP(S), MPEG-TS for SRT/UDP. Segmented files start at keyframes. Free space
   is checked every 2 s; below 256 MB the recording is finalized and stopped.
   Existing files are never overwritten ("name (2).ext").
6. **ISO recording** = an output whose source is an input: its frames of every
   tick at the input's own size, and its clean (pre-strip, delay-aligned)
   audio.
7. **Credentials**: urls are redacted ("***") in logs, errors and the UI; the
   desktop takes the stream key in a password box.

## Consequences

- N destinations with the same settings cost one encode.
- Frames are converted BGRA → YUV on the encoder thread (CPU); with GPU-
  resident frames (ADR-0005 follow-up) hardware encoders could take D3D11
  textures directly.
- H.264/AAC patent licensing and the OpenH264 build in the LGPL FFmpeg are
  commercial questions for the owner (THIRD_PARTY_LICENSES ⚖️) before M9.
- Not yet: HEVC/AV1, per-destination bitrate adaptation, RTMPS certificate
  pinning, SRT listener mode as a server for remote callers, audio-only
  recording, multi-track audio.
