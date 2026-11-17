# ADR-0006: Preview/program monitors in WPF via WriteableBitmap (swap chains deferred)

- Status: Accepted (M3); revisit in M8
- Date: 2026-11-17

## Context

The M3 plan called for DXGI flip-model swap chains owned by native code and
hosted in WPF through `HwndHost`. That gives the lowest latency and no CPU
copy, but it requires the GPU compositor to be the production path (it is
EXPERIMENTAL, see ADR-0005), adds airspace limitations (no WPF content can
overlap the monitors), window-lifetime coupling between UI and engine, and
cannot be exercised on the development host or tested in CI without a GPU.

## Decision

The Production Studio monitors are `WriteableBitmap`s (`Bgr32`) that the view
model refreshes at 25 Hz while the section is visible: under `Lock()`, the
engine copies the newest bus frame **directly into the bitmap's back buffer**
(`mixit_session_copy_bus_frame`, one copy, no managed array), and the dirty
rect is only added when the tick index changed. Polling stops when the section
is hidden or production stops.

## Consequences

- Cost: one frame copy per monitor per refresh on the UI thread (≈ 8 MB for
  1080p; two monitors at 25 Hz ≈ 400 MB/s memcpy) plus WPF's own upload.
  Acceptable for an operator monitor; not for program output.
- Monitors are confidence monitors, not frame-accurate outputs: they show the
  newest frame at refresh time (25 Hz), so some frames are skipped at 50/60 fps.
- No airspace issues; works with any WPF layout, DPI and remote desktop.
- M8 replaces this with native swap chains once the GPU path is default; the
  view model's `BusMonitor` is the only place to change.
