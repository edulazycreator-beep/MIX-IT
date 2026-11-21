# MIX IT — Native Engine API Contract (v0.5.0)

Header: [`native/include/mixit/mixit_engine.h`](../native/include/mixit/mixit_engine.h).
Library: `mixit_engine.dll` (Windows x64), `libmixit_engine.so` (Linux dev host).
Exported symbols: exactly the 60 functions below (verified with `nm`/`objdump`).

| Version | Added |
|---|---|
| 0.1.0 (M1) | engine lifecycle, logging, diagnostics, capabilities |
| 0.2.0 (M2) | sessions (lifecycle steps 3–8), test-signal inputs, program frame access, media library (FFmpeg) queries |
| 0.3.0 (M3) | media-file inputs, preview bus, cut, transitions, layers, bus frame access, compositor preference, capture devices (experimental); appended fields in `session_config`, `session_stats`, `input_stats` |
| 0.5.0 (M5) | outputs: recording and streaming (add/remove/start/stop, stats), encoder query |
| 0.4.0 (M4) | audio mixer (strips, buses, monitor, meters, loudness, bus audio read), audio devices (monitor output, audio inputs; experimental), VST3 inserts (experimental); appended audio counters in `session_stats`, `input_stats` |

API 0.x requires an **exact minor match** at `mixit_engine_create`.

## General rules

| Rule | Detail |
|---|---|
| Calling convention | `__cdecl` on Windows (`MIXIT_CALL`); platform default elsewhere. |
| Types | Fixed-width integers only; enum values passed as `int32_t`; no `bool`, `size_t`, `long`. |
| Handles | `mixit_engine` is an opaque 64-bit id, never reused. NULL, destroyed or foreign handles → `MIXIT_E_INVALID_HANDLE`. |
| Struct versioning | Every extensible struct starts with `uint32_t struct_size` set by the caller to `sizeof` as compiled. Smaller than the v0.1 size → `MIXIT_E_INVALID_ARGUMENT`. Larger (newer caller) → accepted; the engine reads/writes only the fields it knows and preserves the caller's `struct_size`. |
| Strings | UTF-8, NUL-terminated. Output strings go into caller-owned buffers. Static strings (`mixit_get_build_info`, `mixit_result_name`) live as long as the library is loaded. The engine never returns memory the caller must free. |
| Ownership | The caller owns handles from `mixit_engine_create` and must call `mixit_engine_destroy`. Input pointers are only read during the call (e.g. `log_file_path` is copied). |
| Errors | Every fallible function returns `mixit_result`. On failure the calling thread's last-error message is set; read it with `mixit_get_last_error_message` before making another call on that thread. Successful calls do not clear it. |
| Exceptions | Never cross the boundary. |
| Thread safety | All functions are thread-safe. Destroy may race with other calls on the same handle (in-flight calls complete; later calls get `MIXIT_E_INVALID_HANDLE`). |
| Callbacks | Invoked only on engine-owned threads (currently the `mixit-log` thread). A callback must not call `mixit_engine_shutdown`/`_destroy` for its engine (returns `MIXIT_E_INVALID_STATE`). No callback is invoked after `shutdown`/`destroy` returns. |
| Unloading | Destroy all engines before unloading the library. The library runs no code at load/unload. |
| Versioning | `MIXIT_API_VERSION` = `major<<24 | minor<<12 | patch`. While major = 0 a minor change may break the ABI; hosts require an exact major.minor match. From 1.0: same major, caller minor ≤ library minor. |

## Result codes

| Code | Value | Meaning |
|---|---|---|
| `MIXIT_OK` | 0 | success |
| `MIXIT_E_INVALID_ARGUMENT` | -1 | NULL pointer, out-of-range value, bad `struct_size` |
| `MIXIT_E_INVALID_HANDLE` | -2 | handle NULL, unknown or destroyed |
| `MIXIT_E_INVALID_STATE` | -3 | not allowed in the current state (e.g. after shutdown, or from a callback) |
| `MIXIT_E_OUT_OF_MEMORY` | -4 | allocation failed |
| `MIXIT_E_NOT_SUPPORTED` | -5 | feature unavailable on this platform/build |
| `MIXIT_E_BUFFER_TOO_SMALL` | -6 | output buffer too small; required size reported |
| `MIXIT_E_VERSION_MISMATCH` | -7 | caller compiled against an incompatible API version |
| `MIXIT_E_NOT_FOUND` | -8 | index/id out of range |
| `MIXIT_E_PLATFORM` | -9 | an OS/driver call failed |
| `MIXIT_E_IO` | -10 | file-system error |
| `MIXIT_E_INTERNAL` | -99 | unexpected internal failure (a bug) |

## Functions

### Global (no engine required)

**`uint32_t mixit_get_api_version(void)`** — packed API version of the library. Infallible.

**`const char* mixit_get_build_info(void)`** — static description: version, compiler, configuration, platform. Infallible.

**`const char* mixit_result_name(mixit_result result)`** — static symbolic name, `"MIXIT_E_UNKNOWN"` for unknown values. Infallible.

**`uint64_t mixit_get_monotonic_time_ns(void)`** — engine master clock (monotonic, ns). All media timestamps use this clock. Infallible.

**`mixit_result mixit_get_last_error_message(char* buffer, uint32_t capacity, uint32_t* out_required)`**
Copies the calling thread's last error. `out_required` (optional) receives the length including NUL. `capacity == 0` is a size query (returns `MIXIT_E_BUFFER_TOO_SMALL`). `buffer == NULL` with `capacity > 0` → `MIXIT_E_INVALID_ARGUMENT`. Too small → truncated on a UTF-8 boundary, NUL-terminated, `MIXIT_E_BUFFER_TOO_SMALL`.

### Engine lifecycle

**`mixit_result mixit_engine_create(const mixit_engine_config* config, mixit_engine* out_engine)`**
Creates and initializes an engine (lifecycle step 1). `*out_engine` is set to NULL on any failure.
Errors: `INVALID_ARGUMENT` (NULLs, `struct_size`, `min_log_level` out of range, queue capacity > 2³⁰), `VERSION_MISMATCH`, `IO` (log file cannot be opened), `PLATFORM` (thread creation), `OUT_OF_MEMORY`.

| `mixit_engine_config` field | Meaning |
|---|---|
| `struct_size` | `sizeof(mixit_engine_config)` (40 on x64) |
| `api_version` | `MIXIT_API_VERSION` the caller compiled with |
| `min_log_level` | `MIXIT_LOG_TRACE`…`MIXIT_LOG_OFF` |
| `log_queue_capacity` | records; 0 = 4096; rounded up to a power of two |
| `log_file_path` | UTF-8 path of a JSON-lines log (appended), or NULL |
| `log_callback`, `log_user_data` | optional; see Callbacks |

**`mixit_result mixit_engine_shutdown(mixit_engine engine)`** — lifecycle step 9. Stops activity, drains and closes the log. Idempotent. The handle stays valid for diagnostics. Errors: `INVALID_HANDLE`, `INVALID_STATE` (called from a callback).

**`mixit_result mixit_engine_destroy(mixit_engine engine)`** — shuts down (if needed) and invalidates the handle. Errors: `INVALID_HANDLE` (incl. double destroy), `INVALID_STATE` (called from a callback; handle remains valid).

**`mixit_result mixit_engine_get_diagnostics(mixit_engine engine, mixit_engine_diagnostics* out)`** — valid in any state until destroy.

| `mixit_engine_diagnostics` field | Meaning |
|---|---|
| `state` | `MIXIT_ENGINE_STATE_INITIALIZED` (1) / `_SHUT_DOWN` (2) |
| `uptime_ns` | monotonic time since create |
| `log_records_accepted` / `_dropped` / `_delivered` | logging counters (dropped = queue full) |
| `log_file_write_errors` | failed writes/flushes |

### Logging

**`mixit_result mixit_engine_set_log_level(mixit_engine engine, int32_t level)`** — lock-free runtime change. Errors: `INVALID_ARGUMENT`, `INVALID_HANDLE`, `INVALID_STATE` (after shutdown).

**`mixit_result mixit_engine_log(mixit_engine engine, int32_t level, const char* category, const char* message)`** — host record into the engine's ordered stream. Never blocks; a full queue drops and counts (still `MIXIT_OK`). `category` NULL → `"host"`. Errors: `INVALID_ARGUMENT` (NULL message, level outside TRACE…CRITICAL), `INVALID_HANDLE`, `INVALID_STATE`.

**Callback** `void (*mixit_log_callback)(const mixit_log_record* record, void* user_data)` — `record` and its strings are valid only during the call. Fields: `level`, `unix_time_ns`, `thread_id` (producer's OS thread), `sequence` (per-engine, unique), `category`, `message`.

### Capabilities

**`mixit_result mixit_engine_detect_capabilities(mixit_engine engine, mixit_system_capabilities* out)`** — lifecycle step 2; probes and caches. Errors: `INVALID_ARGUMENT`, `INVALID_HANDLE`, `INVALID_STATE` (after shutdown).

| Field | Meaning |
|---|---|
| `detection_flags` | `MIXIT_DETECTED_CPU/MEMORY/OS/GPU_ADAPTERS` — a clear bit means *unknown*, not *absent* |
| `logical_processor_count`, `physical_core_count` | 0 = unknown |
| `cpu_features` | `MIXIT_CPU_FEATURE_*`; AVX-family only if the OS saves the register state |
| `gpu_adapter_count` | adapters available via `mixit_engine_get_gpu_adapter` |
| `total_/available_physical_memory_bytes` | |
| `cpu_brand[64]`, `os_description[128]` | UTF-8, NUL-terminated |

**`mixit_result mixit_engine_get_gpu_adapter(mixit_engine engine, uint32_t index, mixit_gpu_adapter_info* out)`** — from the cached detection. Errors: `INVALID_STATE` (not detected yet), `NOT_FOUND` (index ≥ count), `INVALID_ARGUMENT`, `INVALID_HANDLE`.

| Field | Meaning |
|---|---|
| `vendor_id`, `device_id` | PCI ids (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel, 0x1414 Microsoft) |
| `flags` | `MIXIT_GPU_ADAPTER_FLAG_SOFTWARE` (e.g. Microsoft Basic Render Driver) |
| `dedicated_video_memory_bytes`, `shared_system_memory_bytes` | as reported by DXGI |
| `description[128]` | UTF-8 adapter name |

### Sessions (API 0.2)

A session is one production: a program format, its inputs, frame
synchronization and the program bus. It belongs to an engine; engine
shutdown closes it (`MIXIT_SESSION_STATE_CLOSED`) but the caller must still
destroy the session handle. A session handle keeps the engine *object* alive
(not running) after the engine handle is destroyed.

| State | Value | Allowed |
|---|---|---|
| `CONFIGURING` | 1 | add/remove inputs, set program, start |
| `RUNNING` | 2 | set program (cut), stats, copy frame, stop |
| `STOPPED` | 3 | as CONFIGURING (restart allowed) |
| `CLOSED` | 4 | stats, destroy |

**`mixit_session_create(engine, config, out_session)`** — step 3. Errors: `INVALID_ARGUMENT` (NULLs, `struct_size`, size not even/16..8192, channels 1..8, pool 5..64), `NOT_SUPPORTED` (frame rate not in {24000/1001, 24, 25, 30000/1001, 30, 50, 60000/1001, 60}; sample rate not 48000/44100), `INVALID_HANDLE`, `INVALID_STATE` (engine shut down).

| `mixit_session_config` | |
|---|---|
| `width`, `height` | program size |
| `frame_rate_num/den` | exact rational program rate |
| `audio_sample_rate`, `audio_channels` | session audio format (file inputs are resampled to it) |
| `program_pool_frames` | 0 = 8 |

**`mixit_session_destroy(session)`** — stops (joins threads) and invalidates the handle.

**`mixit_session_add_test_source(session, config, out_input_id)`** — step 4; only when not RUNNING. Input ids are 1..255, unique per session. Test source: even size 64..8192, any supported frame rate (may differ from the session), `tone_hz` < Nyquist (0 = none), `flash_interval_frames` (0 = none), `reserved` = 0.

**`mixit_session_remove_input(session, input_id)`** — only when not RUNNING; `NOT_FOUND` for unknown ids. If it was the program input, program becomes black.

**`mixit_session_set_program_input(session, input_id)`** — 0 = black. Allowed while RUNNING (a cut at the next tick). `NOT_FOUND` for unknown ids.

**`mixit_session_start` / `mixit_session_stop`** — steps 6/7; idempotent. Stop joins all session threads and returns every pooled frame/audio block.

**`mixit_session_get_stats` / `mixit_session_get_input_stats`** — counters (see header); `NOT_FOUND` for unknown input ids.

**`mixit_session_copy_program_frame(session, buffer, buffer_size, stride, out_info)`** — copies the latest program frame (BGRA8). `NOT_FOUND` if none (also after stop). `buffer = NULL, size = 0` is a size query: returns `BUFFER_TOO_SMALL` with `out_info` filled. `stride < width*4` → `INVALID_ARGUMENT`. Diagnostic/test path only; not for real-time display.

### Sessions — API 0.3 additions (M3)

Appended fields keep older callers working: a 0.2-sized `mixit_session_config`
(32 bytes) means compositor AUTO; 0.2-sized `mixit_session_stats` (96) and
`mixit_input_stats` (112) are filled up to the caller's size.

**`mixit_session_config.compositor`** — `MIXIT_COMPOSITOR_PREFER_AUTO` (0, = CPU in 0.3), `_CPU` (1), `_D3D11` (2), `_D3D11_WARP` (3); `reserved` must be 0. A GPU preference that cannot be honoured falls back to CPU with a logged warning; `stats.compositor` (`MIXIT_COMPOSITOR_CPU` / `_D3D11`) reports what runs and `stats.compositor_fallbacks` counts GPU failures that switched a running session to CPU (ADR-0005).

**`mixit_session_add_file_source(session, config, out_input_id)`** — only when not RUNNING. `path` = UTF-8 local file (the `file` protocol only); flag `MIXIT_FILE_SOURCE_LOOP`. Video is converted to BGRA at the file's size, audio resampled to the session format; paced by the file's timestamps. `NOT_FOUND` missing file; `NOT_SUPPORTED` FFmpeg unusable/no decodable stream; `IO` cannot open/parse. Input stats: `decode_errors`, `loops`.

**`mixit_session_set_preview_input(session, input_id)`** — any time; 0 = black.

**`mixit_session_cut(session)`** — swaps preview and program at the next tick; completes a running transition. Counts in `transitions_completed`.

**`mixit_session_start_transition(session, kind, duration_frames)`** — `MIXIT_TRANSITION_MIX` / `_FADE` / `_WIPE` (`_CUT` = cut), 1..600 output frames. Advances with output time, then swaps the buses. While stopped: swaps immediately. `INVALID_STATE` if one is running; `INVALID_ARGUMENT` for other kinds/durations. Stats: `transition_active` (kind or −1), `transition_progress` 0..1.

**`mixit_session_set_layer(session, bus, index, layer)`** — overlay layer 0..`MIXIT_MAX_LAYERS`−1 (8) on `MIXIT_BUS_PROGRAM` / `_PREVIEW`, drawn over the bus's base input in index order. `mixit_layer_config`: `input_id` (0 = disabled), `crop_*` (normalized source rect, must lie within 0..1 and be non-empty), `dest_*` (normalized output rect; may extend beyond the frame, within −4..5), `opacity` 0..1, `reserved` = 0. Floats are allowed here (geometry); they never carry time. `NOT_FOUND` unknown input.

**`mixit_session_copy_bus_frame(session, bus, buffer, size, stride, out_info)`** — as `copy_program_frame` for either bus. Preview is rendered only while it is read (within the last second), so the first preview read after a pause returns `NOT_FOUND` until the next tick.

### Capture devices — API 0.3 (EXPERIMENTAL, Windows)

**`mixit_enumerate_capture_devices(engine, devices, capacity, out_count)`** — Media Foundation video capture sources. Caller sets `struct_size` of each entry; `capacity` 0 queries the count. `id` is never truncated (entries whose id exceeds 511 bytes are skipped). `NOT_SUPPORTED` on non-Windows hosts and Windows without Media Foundation.

**`mixit_session_add_capture_source(session, config, out_input_id)`** — only when not RUNNING. `device_id` from enumeration; `width`/`height` both 0 (= largest mode up to 1920×1080) or both set; `frame_rate_num` 0 = highest up to 60 fps. Video only; frames stamped on arrival. Device loss: last frame held, `decode_errors` counted, reopened every second (ADR-0007). `NOT_FOUND` no such device; `NOT_SUPPORTED` no usable mode / no backend.

### Outputs: recording and streaming — API 0.5 (M5, ADR-0010)

An output encodes the program — or one input (ISO: the input's own size and
its clean pre-strip audio) — as H.264 + AAC and writes it to a file or a
network destination. Outputs with identical source and encoder settings share
one encoder. Each destination has its own thread and bounded queue; a slow or
broken destination drops its own packets up to the next keyframe (counted) or
reconnects, and never blocks the session or other outputs.

**`mixit_session_add_output(session, config, out_id)`** — any time; validates
the url/format (`INVALID_ARGUMENT`), bitrates (video 100–200000 kb/s, audio
32–512, keyframe 250–10000 ms), segments (files only, ≥ 2 s) and the source
input (`NOT_FOUND`; ISO needs video). Formats from the url: `rtmp(s)://` → FLV,
`srt://`/`udp://`/`rtp://`/`tcp://` → MPEG-TS, `.m3u8` → HLS, `.mp4`, `.mov`,
`.mkv`, `.ts`, `.flv`. MP4/MOV are fragmented unless
`MIXIT_OUTPUT_FLAG_NOT_FRAGMENTED`. Files: an existing name is never
overwritten (`name (2).ext`), missing folders are created, segments are
`name_0001.ext`, …

**`mixit_session_start_output(session, id)`** — session RUNNING
(`INVALID_STATE` otherwise or when already started). Opens the encoder here
(automatic choice: NVENC, Quick Sync, AMF, Media Foundation, OpenH264, x264;
or the named one — `NOT_FOUND` if unknown, `NOT_SUPPORTED` if it cannot open).
The destination connects in the background.

**`mixit_session_stop_output(session, id)`** — asynchronous: STOPPING, then
IDLE once the file is finalized. **`mixit_session_stop` / destroy** stop every
output and return only after files are finalized.
**`mixit_session_remove_output`** — `INVALID_STATE` while started.

**`mixit_session_get_output_stats(session, id, stats)`** — state
(IDLE/STARTING/RUNNING/RECONNECTING/STOPPING/FAILED), encoder and whether it is
hardware, bytes, packets, dropped packets (this destination), encoder dropped
frames (shared encoder), reconnects, segments, bitrate (last ~2 s), queue ms,
media duration, free bytes on the recording drive, data-lost flag, current
file, last error. Urls in errors and logs are redacted (stream keys,
passwords and query values become `***`).

**`mixit_query_video_encoders(encoders, capacity, out_count)`** — global; each
H.264 encoder of the build, open-tested at 1280×720 (slow, not on a UI
thread). `NOT_SUPPORTED` without FFmpeg.

### Audio mixer — API 0.4 (M4)

The session mixes on its own audio thread in 10 ms blocks on the session sample
timeline (ENGINE_DESIGN §10). Every input with audio has a **strip**:
[VST3 inserts] → trim → high-pass → gate/expander → 4-band EQ → compressor →
fader × mute × audio-follows-video (ramped) → balance → bus sends.
Buses: master + aux A–D (`MIXIT_AUDIO_BUS_MASTER` 0, `_AUX_A..D` 1–4), each with
fader, mute and limiter (master limiter on by default at −1 dBFS), plus the
read-only **monitor** bus (5): after-fader solo if any strip is soloed, else a
copy of the monitor source bus. Monitor never feeds program.

**`mixit_session_set_audio_strip / get_audio_strip(session, input_id, strip)`** — any time. Ranges in the header; `INVALID_ARGUMENT` outside them or for unknown flags. Defaults: fader 0 dB, AFV on, master send only, EQ bands at 100 Hz shelf / 500 Hz / 2.5 kHz / 8 kHz shelf (flat). AFV: the input is heard while it is on program (base or a visible layer); transitions crossfade it. `NOT_FOUND` unknown input; `INVALID_ARGUMENT` input without audio.

**`mixit_session_set_audio_bus / get_audio_bus(session, bus, config)`** — buses 0–4.

**`mixit_session_set_monitor_source(session, bus)`** — bus 0–4 copied to monitor when nothing is soloed.

**`mixit_session_get_input_meter(session, input_id, meter)`, `mixit_session_get_bus_meter(session, bus, meter)`** — per channel (up to 8): ballistic peak (20 dB/1.5 s fall), 300 ms RMS, clip flag, gain reduction (strip dynamics / bus limiter); buses 0–4 also momentary, short-term and gated integrated loudness (BS.1770-4 / EBU R128, LUFS; −144 = no signal). **`mixit_session_reset_loudness(session, bus)`** restarts integration.

**`mixit_session_read_bus_audio(session, bus, dest, capacity_frames, info)`** — interleaved float32 of any bus (0–5), up to 50 blocks buffered per bus (drop-oldest). `NOT_FOUND` when nothing new; `info` has `first_sample`, `frames`, `pts_ns` on the master clock. One reader per bus.

### Audio devices — API 0.4 (EXPERIMENTAL, Windows WASAPI)

**`mixit_enumerate_audio_devices(engine, direction, devices, capacity, out_count)`** — active render/capture endpoints (shared-mode mix format). `NOT_SUPPORTED` where no backend exists (Linux dev host). The id `"mixit:null[:ppm]"` is a built-in **test device** on every platform (not enumerated): it consumes/produces audio on its own clock, optionally offset by ±ppm.

**`mixit_session_set_audio_output(session, config)`** — plays a bus (normally monitor) on a device; any time; NULL turns it off. Shared mode, float32 at the session format (Windows converts). A drift-compensating resampler bridges the mixer clock and the device clock (±0.5 % range). A failing/disappearing device is retried every second. **`mixit_session_get_audio_output_stats`**: state OFF/RUNNING/RETRYING, device name, buffered ms, underruns, dropped frames, device errors, `drift_ratio`.

**`mixit_session_add_audio_capture_source(session, config, out_input_id)`** — only when not RUNNING; an audio-only input from a capture endpoint, resampled onto the session clock with the same drift bridge. AFV is off by default for such inputs.

### VST3 inserts — API 0.4 (EXPERIMENTAL, ADR-0008)

Plug-ins run **inside the engine process**: a faulty plug-in can crash the engine. Mono and stereo sessions; no editors, sidechains, MIDI or latency compensation.

**`mixit_scan_vst3(engine, path, plugins, capacity, out_count)`** — a `.vst3` bundle, a folder (recursive), or the default folders when `path` is NULL/"". Loads every binary (slow, not on a UI thread). Failing plug-ins are logged and skipped; entries with paths ≥ 512 bytes are skipped. `BUFFER_TOO_SMALL` with `out_count` when capacity is short.

**`mixit_session_set_audio_insert(session, input_id, config)`** — slot 0..1 (`MIXIT_AUDIO_INSERT_SLOTS`), any time incl. RUNNING. Loads the class (`path`, 32-hex `class_id`) and optional `state`, then swaps it in; a replaced effect is destroyed after the audio thread has finished with it. `NOT_FOUND` unknown class/input; `NOT_SUPPORTED` plug-in refuses the bus layout/32-bit float, or no VST3 support in the build.

**`mixit_session_remove_audio_insert`**, **`mixit_session_set_audio_insert_bypass`**, **`mixit_session_get_audio_insert`** (`NOT_FOUND` = empty slot; reports parameter count, latency, failed blocks), **`mixit_session_get_audio_insert_parameter(…, index, out)`** (normalized value, default, steps, title, units, plug-in display text), **`mixit_session_set_audio_insert_parameter(…, parameter_id, value 0..1)`**, **`mixit_session_get_audio_insert_state(…, buffer, capacity, out_size)`** (opaque blob; capacity 0 queries the size).

### Media library (API 0.2, global)

**`mixit_get_media_library_info(out_info)`** — FFmpeg presence, versions (`major<<16|minor<<8|micro`), licence string and flags `COMPILED_IN`, `VERSIONS_MATCH`, `LICENSE_LGPL`, `LGPL_REQUIRED`, `USABLE` (= compiled ∧ versions match ∧ (LGPL ∨ ¬required)). See ADR-0004.

**`mixit_query_codec(name, out_info)`** — `MIXIT_CODEC_DECODER/ENCODER/HARDWARE` + long name. `NOT_FOUND`: not in the build; `NOT_SUPPORTED`: FFmpeg absent or rejected. Presence does not prove the hardware can run the codec.

## Struct layout (x64, asserted on both sides)

| Struct | Size | Key offsets |
|---|---|---|
| `mixit_engine_config` | 40 | `log_file_path` 16, `log_callback` 24, `log_user_data` 32 |
| `mixit_log_record` | 48 | `unix_time_ns` 8, `message` 40 |
| `mixit_engine_diagnostics` | 48 | |
| `mixit_system_capabilities` | 232 | `total_physical_memory_bytes` 24, `cpu_brand` 40, `os_description` 104 |
| `mixit_gpu_adapter_info` | 160 | `description` 32 |
| `mixit_session_config` | 40 | `compositor` 32 (0.2 size 32 accepted) |
| `mixit_test_source_config` | 32 | |
| `mixit_session_stats` | 168 | `ticks` 16, `video_latency_max_ns` 88, `preview_input_id` 96, `transitions_completed` 112, `compositor_fallbacks` 128, `audio_blocks` 136 (0.2/0.3 sizes 96/136 accepted) |
| `mixit_input_stats` | 144 | `frames_produced` 8, `av_offset_max_abs_ns` 104, `decode_errors` 112, `audio_underrun_frames` 128 (0.2/0.3 sizes 112/128 accepted) |
| `mixit_file_source_config` | 16 | `path` 8 |
| `mixit_layer_config` | 48 | `opacity` 40 |
| `mixit_capture_device_info` | 648 | `name` 8, `id` 136 |
| `mixit_capture_source_config` | 32 | `device_id` 8 |
| `mixit_video_frame_info` | 56 | `tick_pts_ns` 24, `tick_index` 48 |
| `mixit_media_library_info` | 152 | |
| `mixit_eq_band` | 20 | |
| `mixit_audio_strip` | 160 | `eq` 32, `gate_threshold_db` 112, `comp_threshold_db` 136 |
| `mixit_audio_bus_config` | 24 | |
| `mixit_audio_meter` | 104 | `peak_db` 16, `gain_reduction_db` 80 |
| `mixit_audio_block_info` | 24 | |
| `mixit_audio_device_info` | 408 | `name` 24, `id` 152 |
| `mixit_audio_output_config` | 24 | `device_id` 8 |
| `mixit_audio_output_stats` | 176 | `drift_ratio` 40 |
| `mixit_audio_capture_config` | 16 | |
| `mixit_vst3_plugin_info` | 1008 | `class_id` 8, `path` 496 |
| `mixit_audio_insert_config` | 40 | `path` 8, `state` 24 |
| `mixit_audio_insert_info` | 320 | `process_errors` 16, `class_id` 24 |
| `mixit_audio_insert_parameter` | 256 | `default_value` 16, `title` 32 |
| `mixit_output_config` | 56 | `url` 8, `video_kbps` 32 |
| `mixit_output_stats` | 888 | `duration_ns` 72, `encoder` 88, `current_file` 120, `last_error` 632 |
| `mixit_video_encoder_info` | 360 | `name` 8 |
| `mixit_codec_info` | 128 | |

Changing any of these requires updating the `static_assert`s in `src/abi/*.cpp`, `MixIt.Interop/NativeStructs.cs`, the layout tests, and bumping `MIXIT_API_VERSION`.
