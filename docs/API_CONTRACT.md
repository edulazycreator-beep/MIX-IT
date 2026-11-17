# MIX IT — Native Engine API Contract (v0.2.0)

Header: [`native/include/mixit/mixit_engine.h`](../native/include/mixit/mixit_engine.h).
Library: `mixit_engine.dll` (Windows x64), `libmixit_engine.so` (Linux dev host).
Exported symbols: exactly the 25 functions below (verified with `nm`/`objdump`).

| Version | Added |
|---|---|
| 0.1.0 (M1) | engine lifecycle, logging, diagnostics, capabilities |
| 0.2.0 (M2) | sessions (lifecycle steps 3–8), test-signal inputs, program frame access, media library (FFmpeg) queries |

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
| `audio_sample_rate`, `audio_channels` | session audio format (inputs use it; no SRC before M4) |
| `program_pool_frames` | 0 = 8 |

**`mixit_session_destroy(session)`** — stops (joins threads) and invalidates the handle.

**`mixit_session_add_test_source(session, config, out_input_id)`** — step 4; only when not RUNNING. Input ids are 1..255, unique per session. Test source: even size 64..8192, any supported frame rate (may differ from the session), `tone_hz` < Nyquist (0 = none), `flash_interval_frames` (0 = none), `reserved` = 0.

**`mixit_session_remove_input(session, input_id)`** — only when not RUNNING; `NOT_FOUND` for unknown ids. If it was the program input, program becomes black.

**`mixit_session_set_program_input(session, input_id)`** — 0 = black. Allowed while RUNNING (a cut at the next tick). `NOT_FOUND` for unknown ids.

**`mixit_session_start` / `mixit_session_stop`** — steps 6/7; idempotent. Stop joins all session threads and returns every pooled frame/audio block.

**`mixit_session_get_stats` / `mixit_session_get_input_stats`** — counters (see header); `NOT_FOUND` for unknown input ids.

**`mixit_session_copy_program_frame(session, buffer, buffer_size, stride, out_info)`** — copies the latest program frame (BGRA8). `NOT_FOUND` if none (also after stop). `buffer = NULL, size = 0` is a size query: returns `BUFFER_TOO_SMALL` with `out_info` filled. `stride < width*4` → `INVALID_ARGUMENT`. Diagnostic/test path only; not for real-time display.

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
| `mixit_session_config` | 32 | |
| `mixit_test_source_config` | 32 | |
| `mixit_session_stats` | 96 | `ticks` 16, `video_latency_max_ns` 88 |
| `mixit_input_stats` | 112 | `frames_produced` 8, `av_offset_max_abs_ns` 104 |
| `mixit_video_frame_info` | 56 | `tick_pts_ns` 24, `tick_index` 48 |
| `mixit_media_library_info` | 152 | |
| `mixit_codec_info` | 128 | |

Changing any of these requires updating the `static_assert`s in `src/abi/*.cpp`, `MixIt.Interop/NativeStructs.cs`, the layout tests, and bumping `MIXIT_API_VERSION`.
