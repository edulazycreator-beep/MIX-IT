# ADR-0007: Video capture through Media Foundation (first backend)

- Status: Accepted (M3, EXPERIMENTAL)
- Date: 2026-11-17

## Context

M3 requires a capture abstraction. Webcams and most USB/PCIe HDMI/SDI capture
devices expose a UVC / Media Foundation video capture source on Windows 10/11.
Vendor SDKs (Blackmagic DeckLink, NDI) are separate, optional integrations
(ARCHITECTURE §3). There is no capture hardware on the development host or in
CI.

## Decision

1. Capture is a `MediaSource` like the test signal and file inputs; the
   session, pools, frame sync and compositor are unchanged.
2. First backend: **Media Foundation source reader** in synchronous mode on the
   input's own thread (MTA), with `MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING` so
   NV12/YUY2/MJPG device formats are converted to RGB32. Device enumeration and
   mode probing run on a short-lived MTA helper thread, because callers (the
   WPF UI) are STA threads.
3. The mode is chosen when the input is added (requested size/rate, otherwise
   the largest mode up to 1080p at the highest rate up to 60 fps); the session
   sizes its pools from it.
4. Frames are stamped with their **arrival time** on the master clock. Device
   timestamps are not mapped yet; capture audio is not captured yet.
5. Media Foundation entry points are resolved at run time
   (`LoadLibraryEx` from System32): Windows N editions without the Media
   Feature Pack still load the engine, and capture reports NOT_SUPPORTED.
6. Device loss: the input keeps its last frame (frame sync repeats it), the
   loss is counted in `decode_errors` and logged once, and the device is
   reopened every second until the session stops.

## Consequences

- Stop latency is bounded by the device frame interval (a blocking
  `ReadSample`), typically ≤ 40 ms; a device that stalls without returning
  could delay stop. An asynchronous reader is the follow-up if this shows up.
- Arrival-time stamping adds the device's internal latency as a constant
  offset and its jitter to the timeline; A/V alignment with capture audio (M4)
  will need device timestamps mapped to the master clock.
- Real-hardware verification is outstanding (`mixit-probe --list-capture`,
  `--session-test 60 --capture 0`).
- DeckLink and NDI are added later as further `MediaSource` backends.
