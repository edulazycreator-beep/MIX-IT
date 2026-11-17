# MIX IT — Native Engine Design

Scope: the design rules every native subsystem follows. Sections marked
**(M1, implemented)** describe code that exists and is tested; everything
else is the agreed design that later milestones implement.

---

## 1. Lifecycle (implemented: steps 1–9; M2 inputs are synthetic)

The ABI lifecycle from the master prompt §20 maps to engine states:

| Step | ABI call | State after | Milestone |
|---|---|---|---|
| 1 Initialize | `mixit_engine_create` | `INITIALIZED` | M1 ✅ |
| 2 Detect capabilities | `mixit_engine_detect_capabilities` | unchanged (cached result) | M1 ✅ (CPU, memory, OS, GPU) |
| 3 Create session | `mixit_session_create` | session `CONFIGURING` | M2 ✅ |
| 4 Register inputs | `mixit_session_add_test_source` | | M2 ✅ (synthetic), M3 (real) |
| 5 Configure outputs | program bus is implicit; `mixit_session_add_output` | | M2 program bus ✅, M3/M5 outputs |
| 6 Start production | `mixit_session_start` | session `RUNNING` | M2 ✅ |
| 7 Stop production | `mixit_session_stop` | session `STOPPED` | M2 ✅ |
| 8 Release resources | `mixit_session_destroy` | | M2 ✅ |
| 9 Shutdown | `mixit_engine_shutdown` / `_destroy` | `SHUT_DOWN` | M1 ✅ |

Rules:
- Shutdown is idempotent and always drains the log before returning; once it returns, no callback will fire.
- `destroy` implies `shutdown`. Destroy racing with in-flight calls on other threads is safe (registry hands out `shared_ptr`s; in-flight calls finish, later calls get `MIXIT_E_INVALID_HANDLE`) — tested.
- Calling shutdown/destroy from inside an engine callback is refused with `MIXIT_E_INVALID_STATE` instead of deadlocking — tested.
- Sub-systems start in dependency order (GPU device → clocks → inputs → mixers/compositor → encoders → outputs) and stop in reverse.

## 2. Threading model

| Thread class | Examples | May block? | May allocate? | May lock? |
|---|---|---|---|---|
| Control plane | any thread calling the ABI (UI, automation) | yes | yes | yes (control mutexes) |
| Real-time audio | WASAPI/ASIO callback (M4) | **no** | **no** | **no** (try-lock at most) |
| Media workers | compositor tick, frame sync, decode (M2/M3) | only on their own work queue | pools only | short, never shared with control plane |
| I/O workers | encoder, muxer, network sender, disk writer (M5) | yes (own queue) | yes | yes |
| Log thread (M1) | `mixit-log` | waits on timer/condvar | yes | its own mutex only |

Cross-class communication uses bounded queues (§4). The control plane changes
media configuration by publishing immutable configuration snapshots that media
threads pick up at frame boundaries (no locks on the media path).

Windows specifics (M2+): media and audio threads register with MMCSS
(`AvSetMmThreadCharacteristicsW`: "Pro Audio" / "Playback"); frame pacing uses
high-resolution waitable timers; threads are named (`SetThreadDescription`) for
profilers — the logger thread already is.

## 3. Time base and A/V synchronization

- **Master clock:** `monotonic_ns()` — `std::chrono::steady_clock` (QueryPerformanceCounter on Windows). Exposed as `mixit_get_monotonic_time_ns`. Wall-clock time is used only for human-facing timestamps (logs, file names).
- **Media timestamps:** every frame and audio block carries a capture/presentation time in master-clock nanoseconds plus its native rational time base (e.g. 1/48000, 1001/60000). Conversions use 64-bit rational arithmetic with explicit rounding; no floating-point accumulation.
- **Output clock:** the program output ticks at a rational frame rate (e.g. 60000/1001). Tick *n* is scheduled at `t0 + n·den/num` computed from *n* (not by adding a rounded period), so error does not accumulate.
- **Input frame synchronization:** each input has a small bounded frame buffer; at each output tick the frame whose presentation time is nearest (minus the input's configured delay) is selected. Faster inputs drop frames, slower inputs repeat frames; both are counted per input.
- **Audio:** the audio engine runs on the audio device clock. Each input's drift relative to the master clock is measured and corrected with adaptive resampling (small ratio adjustments), never by silently inserting/dropping samples. Sample-rate conversion happens at the input.
- **Lip-sync:** per-input audio and video delay settings; measured end-to-end with the synthetic source (frame flash + audio click) in long-run tests (target: < 1 ms drift over 1 h).

### Implementation status (M2)

- `media/time_base` implements the rules above: exact 64×64→128-bit
  multiply/divide (portable, identical on all compilers), `time_of_index`,
  `index_at_time`, `nearest_index`, `duration_of_index`. Tests prove durations
  sum exactly and that frame 3·10⁸ at 29.97 fps lands on the exact nanosecond.
- Frame synchronization runs per output tick with a one-frame look-behind
  (`latency = one output period + 2 ms`): the newest frame with
  `pts ≤ tick time` is presented; superseded frames are counted as *skipped*,
  re-used frames as *repeated*. Verified: 25→30 fps repeats ≈ 1/6 of ticks,
  60→30 fps skips ≈ 1/2 of frames.
- A/V alignment is measured end to end with the test signal: flash frames and
  2 ms clicks travel through separate queues and threads; the session pairs
  them by timestamp. Measured offset in tests and 60 s soaks: **0 ns** (limit
  10.4 µs = half a 48 kHz sample).
- After a stall longer than two periods the output clock resynchronizes
  (counted as skipped ticks) instead of replaying a burst of stale ticks;
  producers likewise jump ahead (counted as late skips).

## 4. Memory and queue policy

- **Pools, not heaps, on media paths.** Frames (CPU and GPU textures), audio blocks and log records come from fixed pools allocated at session start (sized from configuration and capabilities). Steady-state processing does not call `new`/`malloc`.
- **Bounded queues only.** `BoundedMpmcQueue<T>` (M1, implemented): capacity fixed at construction (power of two), lock-free, no syscalls on push/pop, items written/read in place. A full queue *rejects* the item; the caller applies the stage's policy:

| Stage | Policy when full |
|---|---|
| Logging (M1) | drop newest, count (`log_records_dropped`) |
| Preview / multiview frames | drop oldest (show the latest) |
| Program → encoder | drop newest with a frame-drop counter and operator warning; never block the compositor |
| Per streaming destination | each destination has its own queue; a slow destination drops/reconnects alone and never back-pressures program or other destinations |
| Recording | larger queue; on persistent overflow raise a critical event (never silently lose recorded data) |

- **Degradation is visible.** Every drop is counted in diagnostics and surfaced to the operator.

### Implemented in M2

| Component | Detail |
|---|---|
| `Pool<T>` | fixed slots, lock-free free-list (`BoundedMpmcQueue<uint32_t>`), intrusive atomic ref-count; `acquire()` returns empty on exhaustion (counted) |
| `VideoFramePool` / `AudioBlockPool` | one 64-byte-aligned allocation per pool; BGRA8 rows 64-byte aligned; float32 interleaved audio |
| `MediaQueue<T>` | `DropNewest` / `DropOldest` with pushed/dropped/popped/high-water counters |
| Session pools | per input: video queue 4 (drop oldest) + pool 12; audio queue 32 × 10 ms (drop newest) + pool 36; program pool 8 (configurable) |
| Leak check | `Session::pooled_items_in_use()` returns 0 after every stop (tested over repeated start/stop cycles) |

## 5. Logging (M1, implemented)

- Producers (any thread) format into a fixed-size slot (category ≤ 31 bytes, message ≤ 479 bytes; longer text truncated on a UTF-8 boundary and marked `...`) and push into a bounded lock-free queue. No locks or allocation on this path; a thread-local caches the OS thread id.
- The engine-owned `mixit-log` thread drains the queue every 10 ms (or immediately on flush/stop) and delivers to: an optional JSON-lines file (one object per line: `ts`, `level`, `seq`, `thread`, `category`, `msg`), and an optional host callback.
- The managed host forwards its own `ILogger` output into the same stream (`NativeEngineLoggerProvider`), so the JSON-lines file is one ordered log for the whole application. Native records are never fed back into `ILogger` (no loops).
- Counters: accepted, dropped, delivered, file write errors (`mixit_engine_get_diagnostics`).

## 6. Error handling (M1, implemented)

- Internals return `mixit::Status` (code + message). Exceptions are only used for genuinely exceptional failures and are caught at the ABI barrier (`std::bad_alloc` → `MIXIT_E_OUT_OF_MEMORY`, others → `MIXIT_E_INTERNAL`).
- Every failure sets a per-thread last-error message: `"<function>: <reason>"` (fixed 1 KiB thread-local buffer, never allocates).
- Probes degrade rather than fail: a failed sub-probe clears its `MIXIT_DETECTED_*` bit and logs a warning (verified under Wine, where DXGI returns `DXGI_ERROR_UNSUPPORTED`).

## 7. Capability detection (M1 partial)

| Probe | Windows (product) | Linux (dev host) | Milestone |
|---|---|---|---|
| CPU brand, SSE2…AVX-512F (OS-enabled state via XGETBV) | CPUID ✅ | CPUID ✅ | M1 |
| Logical / physical cores | `GetActiveProcessorCount`, `GetLogicalProcessorInformationEx` ⚠️ | sysconf, /proc/cpuinfo ✅ | M1 |
| Memory | `GlobalMemoryStatusEx` ⚠️ | /proc/meminfo ✅ | M1 |
| OS version | `RtlGetVersion` (Win 11 = build ≥ 22000) ⚠️ | uname ✅ | M1 |
| GPU adapters, VRAM | DXGI `EnumAdapters1` ⚠️ | not implemented (flag clear) | M1 |
| Hardware encoders (NVENC/QSV/AMF) | FFmpeg encoder open-test | — | M2 |
| Audio devices | WASAPI / ASIO enumeration | — | M4 |
| Capture devices | Media Foundation, DeckLink, NDI discovery | — | M3 |
| Network interfaces, storage throughput | `GetAdaptersAddresses`, write benchmark | — | M5 |
| Live monitoring (CPU/GPU load, frame drops, underruns, disk) | PDH / DXGI / counters | — | M2–M5 |

⚠️ = compiled and executed under Wine; still to be verified on real Windows hardware.

Performance presets (Low Resource / Balanced / High Performance / Professional)
will be derived from these probes **after** benchmark data exists (M3/M5);
thresholds are not guessed. Presets are recommendations and are never applied
automatically during an active production.

## 8. Session pipeline (M2, implemented — CPU reference path)

```
 input thread "mixit-src" (per input)            output thread "mixit-tick"
 ───────────────────────────────────            ──────────────────────────────────────
 paced on source rate (absolute deadlines)       paced on session rate, wake = tick + latency
 render test frame → pool → video queue ──────▶  frame sync (per input) → current frame
 render 10 ms audio → pool → audio queue ─────▶  consume audio, detect clicks, A/V pairing
                                                 program = selected input's current frame
                                                   same size → zero copy (shared ref)
                                                   other size → nearest-neighbour scale
                                                   none     → pre-filled black frame
                                                 → program queue (2, drop oldest) → readers
```

- Control calls (add/remove/start/stop) take the session control mutex; media
  threads never do. Inputs change only while stopped; program selection is an
  atomic, so cuts happen at tick boundaries.
- Pacing uses `os::sleep_until_monotonic_ns`: absolute `clock_nanosleep` on
  Linux, a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`,
  Windows 10 1803+) on Windows with a `sleep_until` fallback. Executed under
  Wine; real-hardware timing on Windows is still to be measured.
- The test signal (`media/test_signal`) is a pure function of frame/sample
  index: colour bars, moving stripe, a 40-bit frame code (frame index + source
  id) with a white/black sentinel, flash band on sync frames, −20 dBFS tone and
  ±0.9 clicks starting on the sample nearest to each flash frame.

## 9. GPU pipeline (M3 design)

- One D3D11 device per engine (D3D11.1+, BGRA support, video support flags), created on the adapter chosen from the capability list; D3D12 considered later for specific workloads.
- Decoded frames stay on the GPU where the decoder supports it (D3D11VA); conversions (NV12/P010 → RGBA, scaling, colour) are shader passes.
- The compositor renders layers into pooled render targets per bus (program, preview, clean feed, aux); outputs read from those targets. CPU readback only where an output requires it (software encoders, NDI send), through pooled staging textures with fences.
- Preview/program monitors are DXGI flip-model swap chains owned by native code inside WPF `HwndHost` windows.
