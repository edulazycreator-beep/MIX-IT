/*
 * MIX IT — Native Engine public C ABI
 *
 * This header is the ONLY supported boundary between the native engine
 * (mixit_engine.dll / libmixit_engine.so) and any host (the C# desktop
 * application, tests, future SDK consumers).
 *
 * ABI rules (see docs/API_CONTRACT.md for the full contract):
 *   - Plain C only. No C++ types cross this boundary.
 *   - Fixed-width integer types only. No `bool`, `long`, `size_t` or enums
 *     in structs or signatures; enum *values* are passed as int32_t.
 *   - Engines are opaque handles. Handles are validated on every call;
 *     a destroyed or foreign handle yields MIXIT_E_INVALID_HANDLE instead of
 *     undefined behaviour.
 *   - Every extensible struct begins with `uint32_t struct_size`, which the
 *     caller sets to sizeof(struct) as it was compiled. The engine reads and
 *     writes at most that many bytes, which lets new fields be appended in
 *     later minor versions without breaking older callers.
 *   - Strings are UTF-8, NUL-terminated. Output strings are written into
 *     caller-owned buffers; the engine never returns heap memory that the
 *     caller must free.
 *   - No C++ exception ever crosses the boundary. Every function returns a
 *     mixit_result (or a plain value documented as infallible).
 *   - On failure, a human-readable message for the CALLING THREAD is
 *     available from mixit_get_last_error_message().
 *
 * Versioning: during 0.x, a minor version change MAY break the ABI.
 * Hosts must require an exact major.minor match while major == 0.
 */
#ifndef MIXIT_ENGINE_H
#define MIXIT_ENGINE_H

#include <stdint.h>

#if defined(_WIN32)
#  define MIXIT_CALL __cdecl
#  if defined(MIXIT_ENGINE_BUILDING)
#    define MIXIT_API __declspec(dllexport)
#  elif defined(MIXIT_ENGINE_STATIC)
#    define MIXIT_API
#  else
#    define MIXIT_API __declspec(dllimport)
#  endif
#else
#  define MIXIT_CALL
#  if defined(MIXIT_ENGINE_BUILDING)
#    define MIXIT_API __attribute__((visibility("default")))
#  else
#    define MIXIT_API
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------ */
/* Versioning                                                               */
/* ------------------------------------------------------------------------ */

/* Cast-free so the header compiles cleanly under strict C++ warnings
 * (-Wold-style-cast); the unsigned masks make the arithmetic unsigned. */
#define MIXIT_MAKE_VERSION(major, minor, patch) \
    ((((major) & 0xFFu) << 24) | (((minor) & 0xFFFu) << 12) | ((patch) & 0xFFFu))
#define MIXIT_VERSION_MAJOR(v) (((v) >> 24) & 0xFFu)
#define MIXIT_VERSION_MINOR(v) (((v) >> 12) & 0xFFFu)
#define MIXIT_VERSION_PATCH(v) ((v) & 0xFFFu)

#define MIXIT_API_VERSION_MAJOR 0
#define MIXIT_API_VERSION_MINOR 4
#define MIXIT_API_VERSION_PATCH 0
#define MIXIT_API_VERSION \
    MIXIT_MAKE_VERSION(MIXIT_API_VERSION_MAJOR, MIXIT_API_VERSION_MINOR, MIXIT_API_VERSION_PATCH)

/* ------------------------------------------------------------------------ */
/* Result codes                                                             */
/* ------------------------------------------------------------------------ */

typedef int32_t mixit_result;

#define MIXIT_OK                       0
#define MIXIT_E_INVALID_ARGUMENT      -1  /* NULL pointer, bad enum value, bad struct_size */
#define MIXIT_E_INVALID_HANDLE        -2  /* handle unknown, destroyed, or NULL           */
#define MIXIT_E_INVALID_STATE         -3  /* call not allowed in current engine state     */
#define MIXIT_E_OUT_OF_MEMORY         -4
#define MIXIT_E_NOT_SUPPORTED         -5  /* feature unavailable on this platform/build   */
#define MIXIT_E_BUFFER_TOO_SMALL      -6  /* output buffer too small; see out_required    */
#define MIXIT_E_VERSION_MISMATCH      -7  /* caller compiled against incompatible ABI     */
#define MIXIT_E_NOT_FOUND             -8  /* index or id out of range                     */
#define MIXIT_E_PLATFORM              -9  /* an OS / driver call failed                   */
#define MIXIT_E_IO                   -10  /* file system error                            */
#define MIXIT_E_INTERNAL             -99  /* unexpected internal failure (bug)            */

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

#define MIXIT_LOG_TRACE    0
#define MIXIT_LOG_DEBUG    1
#define MIXIT_LOG_INFO     2
#define MIXIT_LOG_WARNING  3
#define MIXIT_LOG_ERROR    4
#define MIXIT_LOG_CRITICAL 5
#define MIXIT_LOG_OFF      6

typedef struct mixit_log_record {
    uint32_t    struct_size;
    int32_t     level;              /* MIXIT_LOG_*                                   */
    int64_t     unix_time_ns;       /* wall-clock time the record was produced       */
    uint64_t    thread_id;          /* OS thread id of the producing thread          */
    uint64_t    sequence;           /* per-engine monotonically increasing number    */
    const char* category;           /* UTF-8, valid only for the callback's duration */
    const char* message;            /* UTF-8, valid only for the callback's duration */
} mixit_log_record;

/*
 * Log callback. Invoked on the engine-owned logging thread ("mixit-log"),
 * NEVER on the thread that produced the record and NEVER on a real-time
 * media thread. The callback must not call mixit_engine_shutdown or
 * mixit_engine_destroy for the same engine (that would self-deadlock; the
 * engine detects this and returns MIXIT_E_INVALID_STATE).
 */
typedef void (MIXIT_CALL *mixit_log_callback)(const mixit_log_record* record, void* user_data);

/* ------------------------------------------------------------------------ */
/* Engine lifecycle                                                         */
/* ------------------------------------------------------------------------ */

typedef struct mixit_engine_s* mixit_engine;   /* opaque */

#define MIXIT_ENGINE_STATE_INITIALIZED 1
#define MIXIT_ENGINE_STATE_SHUT_DOWN   2

typedef struct mixit_engine_config {
    uint32_t           struct_size;        /* = sizeof(mixit_engine_config)              */
    uint32_t           api_version;        /* = MIXIT_API_VERSION the caller compiled with*/
    int32_t            min_log_level;      /* MIXIT_LOG_*; records below are discarded   */
    uint32_t           log_queue_capacity; /* records; 0 = default (4096). Rounded up to
                                              a power of two. Bounded: when full, records
                                              are dropped and counted, never blocking.   */
    const char*        log_file_path;      /* UTF-8 path for JSON-lines log, or NULL     */
    mixit_log_callback log_callback;       /* optional, may be NULL                      */
    void*              log_user_data;      /* passed through to log_callback             */
} mixit_engine_config;

typedef struct mixit_engine_diagnostics {
    uint32_t struct_size;
    int32_t  state;                     /* MIXIT_ENGINE_STATE_*                          */
    uint64_t uptime_ns;                 /* monotonic time since create                   */
    uint64_t log_records_accepted;      /* enqueued for the log thread                   */
    uint64_t log_records_dropped;       /* rejected because the bounded queue was full   */
    uint64_t log_records_delivered;     /* fully processed by all sinks                  */
    uint64_t log_file_write_errors;
} mixit_engine_diagnostics;

/* ------------------------------------------------------------------------ */
/* Capabilities                                                             */
/* ------------------------------------------------------------------------ */

/* mixit_system_capabilities.cpu_features bits */
#define MIXIT_CPU_FEATURE_SSE2     (1u << 0)
#define MIXIT_CPU_FEATURE_SSE4_1   (1u << 1)
#define MIXIT_CPU_FEATURE_SSE4_2   (1u << 2)
#define MIXIT_CPU_FEATURE_AVX      (1u << 3)
#define MIXIT_CPU_FEATURE_AVX2     (1u << 4)
#define MIXIT_CPU_FEATURE_FMA3     (1u << 5)
#define MIXIT_CPU_FEATURE_AVX512F  (1u << 6)

/* mixit_system_capabilities.detection_flags bits: what was actually probed */
#define MIXIT_DETECTED_CPU            (1u << 0)
#define MIXIT_DETECTED_MEMORY         (1u << 1)
#define MIXIT_DETECTED_OS             (1u << 2)
#define MIXIT_DETECTED_GPU_ADAPTERS   (1u << 3)  /* unset => GPU enumeration unavailable */

typedef struct mixit_system_capabilities {
    uint32_t struct_size;
    uint32_t detection_flags;               /* MIXIT_DETECTED_*                        */
    uint32_t logical_processor_count;
    uint32_t physical_core_count;           /* 0 if unknown                            */
    uint32_t cpu_features;                  /* MIXIT_CPU_FEATURE_* (OS support checked) */
    uint32_t gpu_adapter_count;
    uint64_t total_physical_memory_bytes;
    uint64_t available_physical_memory_bytes;
    char     cpu_brand[64];                 /* UTF-8, NUL-terminated                   */
    char     os_description[128];           /* UTF-8, NUL-terminated                   */
} mixit_system_capabilities;

#define MIXIT_GPU_ADAPTER_FLAG_SOFTWARE (1u << 0)  /* e.g. Microsoft Basic Render Driver */

typedef struct mixit_gpu_adapter_info {
    uint32_t struct_size;
    uint32_t vendor_id;                     /* PCI vendor id (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel) */
    uint32_t device_id;
    uint32_t flags;                         /* MIXIT_GPU_ADAPTER_FLAG_*                */
    uint64_t dedicated_video_memory_bytes;
    uint64_t shared_system_memory_bytes;
    char     description[128];              /* UTF-8, NUL-terminated                   */
} mixit_gpu_adapter_info;

/* ------------------------------------------------------------------------ */
/* Global functions (no engine required, always thread-safe)                */
/* ------------------------------------------------------------------------ */

/* Packed MIXIT_API_VERSION the library was built with. Infallible. */
MIXIT_API uint32_t MIXIT_CALL mixit_get_api_version(void);

/* Static, NUL-terminated build description (version, compiler, config).
 * The pointer is valid for the lifetime of the loaded library. Infallible. */
MIXIT_API const char* MIXIT_CALL mixit_get_build_info(void);

/* Static name for a result code, e.g. "MIXIT_E_INVALID_HANDLE". Infallible. */
MIXIT_API const char* MIXIT_CALL mixit_result_name(mixit_result result);

/* Monotonic engine clock in nanoseconds. All media timestamps produced by
 * the engine are expressed on this clock. Infallible. */
MIXIT_API uint64_t MIXIT_CALL mixit_get_monotonic_time_ns(void);

/*
 * Copies the calling thread's last error message (set by the most recent
 * failing mixit_* call on this thread) into `buffer`.
 *   - buffer may be NULL when capacity == 0 (size query).
 *   - *out_required (optional) receives the length INCLUDING the NUL.
 *   - Returns MIXIT_E_BUFFER_TOO_SMALL if it did not fit (the buffer then
 *     holds a truncated, NUL-terminated prefix when capacity > 0).
 * Successful calls do not clear the message.
 */
MIXIT_API mixit_result MIXIT_CALL mixit_get_last_error_message(char* buffer,
                                                                uint32_t capacity,
                                                                uint32_t* out_required);

/* ------------------------------------------------------------------------ */
/* Engine functions                                                         */
/* ------------------------------------------------------------------------ */

/*
 * Lifecycle step 1 — Initialize.
 * Creates and initializes an engine. On success *out_engine receives a
 * handle in state MIXIT_ENGINE_STATE_INITIALIZED. On failure *out_engine is
 * set to NULL. Ownership: the caller owns the handle and must release it
 * with mixit_engine_destroy.
 */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_create(const mixit_engine_config* config,
                                                       mixit_engine* out_engine);

/*
 * Lifecycle step 9 — Shutdown.
 * Stops all engine activity, flushes and closes the log (the log callback
 * will not be invoked after this returns). Idempotent: shutting down an
 * already shut-down engine returns MIXIT_OK. The handle remains valid for
 * mixit_engine_get_diagnostics until destroyed.
 */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_shutdown(mixit_engine engine);

/*
 * Releases the handle. Implies mixit_engine_shutdown. After this call the
 * handle value is invalid; any further use returns MIXIT_E_INVALID_HANDLE.
 * Must not be called concurrently with other calls on the same handle.
 */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_destroy(mixit_engine engine);

MIXIT_API mixit_result MIXIT_CALL mixit_engine_get_diagnostics(mixit_engine engine,
                                                                mixit_engine_diagnostics* out_diagnostics);

/* Changes the minimum log level at runtime. Thread-safe, lock-free. */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_set_log_level(mixit_engine engine, int32_t level);

/*
 * Submits a log record from the host so managed and native logs share one
 * ordered stream. Never blocks: if the bounded log queue is full the record
 * is dropped and counted in diagnostics (still returns MIXIT_OK).
 * Messages longer than the engine's record capacity are truncated.
 */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_log(mixit_engine engine,
                                                    int32_t level,
                                                    const char* category,
                                                    const char* message);

/*
 * Lifecycle step 2 — Detect capabilities.
 * Probes CPU, memory, OS and (where supported) GPU adapters. Fields that
 * could not be probed are zero and their MIXIT_DETECTED_* bit is clear.
 * The result is cached; adapters are then queried by index with
 * mixit_engine_get_gpu_adapter.
 */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_detect_capabilities(mixit_engine engine,
                                                                    mixit_system_capabilities* out_capabilities);

/* Returns MIXIT_E_INVALID_STATE if capabilities were never detected and
 * MIXIT_E_NOT_FOUND if index >= gpu_adapter_count. */
MIXIT_API mixit_result MIXIT_CALL mixit_engine_get_gpu_adapter(mixit_engine engine,
                                                                uint32_t index,
                                                                mixit_gpu_adapter_info* out_adapter);

/* ------------------------------------------------------------------------ */
/* Sessions (lifecycle steps 3-8) — API 0.2                                 */
/* ------------------------------------------------------------------------ */

typedef struct mixit_session_s* mixit_session;   /* opaque */

#define MIXIT_SESSION_STATE_CONFIGURING 1   /* created, never started          */
#define MIXIT_SESSION_STATE_RUNNING     2
#define MIXIT_SESSION_STATE_STOPPED     3   /* can be started again            */
#define MIXIT_SESSION_STATE_CLOSED      4   /* engine shut down; only stats/destroy */

#define MIXIT_PIXEL_FORMAT_BGRA8 1          /* B,G,R,A bytes; DXGI_FORMAT_B8G8R8A8_UNORM */

#define MIXIT_FRAME_FLAG_SYNC_FLASH (1u << 0) /* test signal flash frame (A/V sync marker) */

typedef struct mixit_session_config {
    uint32_t struct_size;          /* = sizeof(mixit_session_config)                   */
    uint32_t width;                /* program size: even, 16..8192                     */
    uint32_t height;
    uint32_t frame_rate_num;       /* 24000/1001, 24, 25, 30000/1001, 30, 50, 60000/1001, 60 */
    uint32_t frame_rate_den;
    uint32_t audio_sample_rate;    /* 48000 or 44100                                   */
    uint32_t audio_channels;       /* 1..8                                             */
    uint32_t program_pool_frames;  /* 0 = default (8); 5..64                           */
    /* API 0.3 — appended; a 0.2-sized struct (32 bytes) means AUTO */
    int32_t  compositor;           /* MIXIT_COMPOSITOR_PREFER_*                        */
    uint32_t reserved;             /* must be 0                                        */
} mixit_session_config;

/* Compositor preference (mixit_session_config.compositor). AUTO selects the
 * CPU compositor in API 0.3 (frames are CPU-resident; see ARCHITECTURE).
 * D3D11 / D3D11_WARP fall back to the CPU compositor with a logged warning if
 * Direct3D 11 is unavailable; mixit_session_stats.compositor reports the
 * compositor actually in use. WARP is Microsoft's software rasterizer
 * (testing, machines without a GPU). */
#define MIXIT_COMPOSITOR_PREFER_AUTO       0
#define MIXIT_COMPOSITOR_PREFER_CPU        1
#define MIXIT_COMPOSITOR_PREFER_D3D11      2
#define MIXIT_COMPOSITOR_PREFER_D3D11_WARP 3

/* Synthetic test-signal input: colour bars, moving stripe, machine-readable
 * frame code, optional flash frames with matching audio clicks, sine tone.
 * Audio uses the session's sample rate and channel count. */
typedef struct mixit_test_source_config {
    uint32_t struct_size;
    uint32_t width;                  /* even, 64..8192 */
    uint32_t height;
    uint32_t frame_rate_num;         /* same set as sessions; may differ from the session rate */
    uint32_t frame_rate_den;
    uint32_t tone_hz;                /* 0 = no tone */
    uint32_t flash_interval_frames;  /* 0 = no flash/click */
    uint32_t reserved;               /* must be 0 */
} mixit_test_source_config;

typedef struct mixit_session_stats {
    uint32_t struct_size;
    int32_t  state;                  /* MIXIT_SESSION_STATE_* */
    uint32_t input_count;
    uint32_t program_input_id;       /* 0 = black */
    uint64_t ticks;                  /* output frames clocked */
    uint64_t late_ticks;             /* started > half a period late */
    uint64_t skipped_ticks;          /* skipped to recover from a stall */
    int64_t  max_tick_lateness_ns;
    uint64_t program_frames;
    uint64_t program_black_frames;   /* no input frame available */
    uint64_t program_frames_dropped; /* program output queue overflow */
    uint64_t program_pool_exhausted;
    int64_t  video_latency_last_ns;  /* tick time - presented source frame time */
    int64_t  video_latency_max_ns;
    /* API 0.3 — appended; filled only if the caller's struct_size covers them */
    uint32_t preview_input_id;
    int32_t  transition_active;      /* MIXIT_TRANSITION_* while running, -1 otherwise */
    float    transition_progress;    /* 0..1 while a transition runs */
    int32_t  compositor;             /* MIXIT_COMPOSITOR_* */
    uint64_t transitions_completed;  /* cuts + finished transitions */
    uint64_t composited_frames;      /* bus frames rendered by the compositor (others are zero-copy) */
    uint64_t compositor_fallbacks;   /* GPU failures that moved the session to the CPU compositor */
    /* API 0.4 — appended */
    uint64_t audio_blocks;           /* mix blocks processed (10 ms each) */
    uint64_t audio_late_blocks;      /* mix blocks started more than one block late */
    uint64_t audio_skipped_blocks;   /* blocks skipped after a stall (silence on every bus) */
    int64_t  audio_max_lateness_ns;
} mixit_session_stats;

typedef struct mixit_input_stats {
    uint32_t struct_size;
    uint32_t input_id;
    uint64_t frames_produced;
    uint64_t frames_dropped;         /* input queue overflow (oldest dropped) */
    uint64_t frames_pool_exhausted;
    uint64_t frames_presented;       /* distinct frames used by frame sync */
    uint64_t frames_repeated;        /* ticks that repeated the previous frame */
    uint64_t frames_skipped;         /* frames superseded before being used */
    uint64_t producer_late_skips;
    uint64_t audio_blocks_produced;
    uint64_t audio_blocks_dropped;
    uint64_t audio_blocks_consumed;
    uint64_t av_sync_events;         /* flash/click pairs measured */
    int64_t  av_offset_last_ns;      /* click time - flash frame time */
    int64_t  av_offset_max_abs_ns;
    /* API 0.3 — appended; filled only if the caller's struct_size covers them */
    uint64_t decode_errors;          /* file/capture sources */
    uint64_t loops;                  /* file sources: completed loops */
    /* API 0.4 — appended */
    uint64_t audio_underrun_frames;  /* mix needed samples the source had not delivered (played as silence) */
    uint64_t audio_late_blocks;      /* blocks that arrived after their mix time */
} mixit_input_stats;

typedef struct mixit_video_frame_info {
    uint32_t struct_size;
    uint32_t width;
    uint32_t height;
    int32_t  pixel_format;           /* MIXIT_PIXEL_FORMAT_* */
    uint32_t input_id;               /* 0 = black frame */
    uint32_t flags;                  /* MIXIT_FRAME_FLAG_* */
    int64_t  tick_pts_ns;            /* output tick time (master clock) */
    int64_t  source_pts_ns;          /* presented source frame time */
    int64_t  source_sequence;        /* source frame index, -1 for black */
    uint64_t tick_index;
} mixit_video_frame_info;

/* Lifecycle step 3. The session belongs to `engine`; engine shutdown closes
 * it (state CLOSED). The caller must still destroy the session handle. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_create(mixit_engine engine,
                                                        const mixit_session_config* config,
                                                        mixit_session* out_session);

/* Stops the session if running and releases the handle. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_destroy(mixit_session session);

/* Lifecycle step 4. Only while not RUNNING. Input ids are unique per session (1..255). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_add_test_source(mixit_session session,
                                                                 const mixit_test_source_config* config,
                                                                 uint32_t* out_input_id);

#define MIXIT_FILE_SOURCE_LOOP (1u << 0)

typedef struct mixit_file_source_config {
    uint32_t    struct_size;
    uint32_t    flags;           /* MIXIT_FILE_SOURCE_* */
    const char* path;            /* UTF-8 path of a local media file (no URLs) */
} mixit_file_source_config;

/* API 0.3. Adds a media-file input decoded with FFmpeg (video converted to
 * BGRA at the file's size, audio resampled to the session format), paced by
 * the file's timestamps. Only while not RUNNING.
 * MIXIT_E_NOT_FOUND: file missing. MIXIT_E_NOT_SUPPORTED: FFmpeg unusable or
 * no decodable streams. MIXIT_E_IO: cannot open/parse. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_add_file_source(mixit_session session,
                                                                 const mixit_file_source_config* config,
                                                                 uint32_t* out_input_id);

/* ---- Video capture devices (API 0.3, EXPERIMENTAL) -------------------------
 * Windows Media Foundation video capture sources: webcams and USB/PCIe capture
 * devices exposing a UVC / Media Foundation interface. Video only (device
 * audio is not captured yet); frames are stamped with their arrival time.
 * Other platforms: MIXIT_E_NOT_SUPPORTED. */
typedef struct mixit_capture_device_info {
    uint32_t struct_size;
    uint32_t flags;              /* reserved, 0 */
    char     name[128];          /* UTF-8 friendly name (truncated on a character boundary) */
    char     id[512];            /* UTF-8 device id; pass to mixit_capture_source_config.device_id */
} mixit_capture_device_info;

/* Lists connected capture devices. Writes up to `capacity` entries (each with
 * struct_size set by the caller) and the total count to `out_count`; call with
 * capacity 0 to query the count. MIXIT_E_NOT_SUPPORTED: no capture backend
 * (non-Windows, or Windows without Media Foundation). */
MIXIT_API mixit_result MIXIT_CALL mixit_enumerate_capture_devices(mixit_engine engine, mixit_capture_device_info* devices,
                                                                   uint32_t capacity, uint32_t* out_count);

typedef struct mixit_capture_source_config {
    uint32_t    struct_size;
    uint32_t    flags;           /* reserved, 0 */
    const char* device_id;       /* from mixit_capture_device_info.id */
    uint32_t    width;           /* 0 with height 0 = largest mode up to 1920x1080 */
    uint32_t    height;
    uint32_t    frame_rate_num;  /* 0 = highest rate up to 60 fps (else closest not above) */
    uint32_t    frame_rate_den;
} mixit_capture_source_config;

/* Adds a capture-device input (only while not RUNNING). The device mode is
 * chosen now; capture runs while the session runs. If the device fails or is
 * unplugged, the input holds its last frame, input stats count decode_errors
 * and the engine retries every second. MIXIT_E_NOT_FOUND: no such device;
 * MIXIT_E_NOT_SUPPORTED: no usable mode / no capture backend. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_add_capture_source(mixit_session session,
                                                                    const mixit_capture_source_config* config,
                                                                    uint32_t* out_input_id);

/* Only while not RUNNING. If it was the program input, program becomes black (0). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_remove_input(mixit_session session, uint32_t input_id);

/* Selects the program input (0 = black). Allowed while RUNNING; takes
 * effect at the next output tick (a cut). Cancels a running transition. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_program_input(mixit_session session, uint32_t input_id);

/* ---- Buses, transitions and layers (API 0.3) ---------------------------- */

#define MIXIT_BUS_PROGRAM 0
#define MIXIT_BUS_PREVIEW 1

#define MIXIT_TRANSITION_CUT  0
#define MIXIT_TRANSITION_MIX  1   /* cross-dissolve */
#define MIXIT_TRANSITION_FADE 2   /* through black */
#define MIXIT_TRANSITION_WIPE 3   /* left to right */

#define MIXIT_COMPOSITOR_CPU   0
#define MIXIT_COMPOSITOR_D3D11 1

#define MIXIT_MAX_LAYERS 8

/* Geometry is normalized: 0..1 of the source (crop) or of the bus output
 * (dest). Floats are 32-bit IEEE-754. A layer is drawn above the bus's base
 * input; layers draw in index order. */
typedef struct mixit_layer_config {
    uint32_t struct_size;
    uint32_t input_id;            /* 0 = disable the layer */
    float    crop_x, crop_y, crop_w, crop_h;   /* source region, within 0..1 */
    float    dest_x, dest_y, dest_w, dest_h;   /* placement on the bus */
    float    opacity;             /* 0..1, multiplied with the source alpha */
    uint32_t reserved;            /* must be 0 */
} mixit_layer_config;

/* Selects the preview input (0 = black). Any time. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_preview_input(mixit_session session, uint32_t input_id);

/* Swaps preview and program now (completes a running transition). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_cut(mixit_session session);

/* Transitions program -> preview over `duration_frames` output frames
 * (<= 600); at the end preview and program swap. MIXIT_TRANSITION_CUT or 0
 * frames is an immediate cut. MIXIT_E_INVALID_STATE if one is running. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_start_transition(mixit_session session, int32_t kind,
                                                                  uint32_t duration_frames);

/* Sets layer `index` (0..MIXIT_MAX_LAYERS-1) of a bus. Any time. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_layer(mixit_session session, int32_t bus, uint32_t index,
                                                           const mixit_layer_config* layer);

/* Like mixit_session_copy_program_frame for any bus. The preview bus is only
 * rendered while it is being read: after a pause the first call returns
 * MIXIT_E_NOT_FOUND until the next output tick. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_copy_bus_frame(mixit_session session, int32_t bus, uint8_t* buffer,
                                                                uint32_t buffer_size, uint32_t stride,
                                                                mixit_video_frame_info* out_info);

/* Lifecycle step 6 / 7. Both are idempotent. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_start(mixit_session session);
MIXIT_API mixit_result MIXIT_CALL mixit_session_stop(mixit_session session);

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_stats(mixit_session session, mixit_session_stats* out_stats);

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_input_stats(mixit_session session, uint32_t input_id,
                                                                 mixit_input_stats* out_stats);

/*
 * Copies the most recent program frame (BGRA8, rows of width*4 bytes at
 * `stride` byte intervals) into `buffer` and describes it in *out_info.
 *   - MIXIT_E_NOT_FOUND: no program frame yet (or the session is stopped).
 *   - MIXIT_E_BUFFER_TOO_SMALL: *out_info is still filled (size query:
 *     buffer = NULL, buffer_size = 0); required = stride*(height-1) + width*4.
 * Intended for diagnostics and tests, not for real-time display (M3 renders
 * monitors natively).
 */
MIXIT_API mixit_result MIXIT_CALL mixit_session_copy_program_frame(mixit_session session, uint8_t* buffer,
                                                                    uint32_t buffer_size, uint32_t stride,
                                                                    mixit_video_frame_info* out_info);

/* ------------------------------------------------------------------------ */
/* Media library (FFmpeg) — API 0.2. Global: no engine required.            */
/* ------------------------------------------------------------------------ */

#define MIXIT_MEDIA_LIBRARY_COMPILED_IN    (1u << 0)  /* engine built with FFmpeg            */
#define MIXIT_MEDIA_LIBRARY_VERSIONS_MATCH (1u << 1)  /* runtime majors == build headers     */
#define MIXIT_MEDIA_LIBRARY_LICENSE_LGPL   (1u << 2)  /* runtime libraries report LGPL       */
#define MIXIT_MEDIA_LIBRARY_LGPL_REQUIRED  (1u << 3)  /* this build enforces the LGPL policy */
#define MIXIT_MEDIA_LIBRARY_USABLE         (1u << 4)  /* the engine will use FFmpeg          */

typedef struct mixit_media_library_info {
    uint32_t struct_size;
    uint32_t flags;               /* MIXIT_MEDIA_LIBRARY_* */
    uint32_t avcodec_version;     /* major<<16 | minor<<8 | micro, 0 if absent */
    uint32_t avformat_version;
    uint32_t avutil_version;
    uint32_t reserved;
    char     version[64];         /* FFmpeg version string, UTF-8 */
    char     license[64];         /* licence reported by libavcodec */
} mixit_media_library_info;

#define MIXIT_CODEC_DECODER  (1u << 0)
#define MIXIT_CODEC_ENCODER  (1u << 1)
#define MIXIT_CODEC_HARDWARE (1u << 2)  /* backed by dedicated hardware (NVENC, QSV, AMF, ...) */

typedef struct mixit_codec_info {
    uint32_t struct_size;
    uint32_t flags;               /* MIXIT_CODEC_* */
    char     long_name[120];      /* UTF-8 */
} mixit_codec_info;

MIXIT_API mixit_result MIXIT_CALL mixit_get_media_library_info(mixit_media_library_info* out_info);

/* Whether codec `name` (FFmpeg codec name, e.g. "h264", "h264_nvenc") is in
 * the loaded build. Presence in the build does NOT prove the GPU/driver can
 * run it (encoders are open-tested in M5).
 * MIXIT_E_NOT_FOUND: not in the build. MIXIT_E_NOT_SUPPORTED: FFmpeg absent
 * or not usable. */
MIXIT_API mixit_result MIXIT_CALL mixit_query_codec(const char* name, mixit_codec_info* out_info);

/* ---- Audio mixer (API 0.4) -------------------------------------------------
 * Every input has a channel strip; audio is mixed in 10 ms blocks on the
 * session sample timeline (sample 0 = session start, same master clock as
 * video). Strip flow: trim → high-pass → gate/expander → 4-band EQ →
 * compressor → fader × mute × audio-follows-video → balance → buses.
 * Buses: 0 master, 1..4 aux A..D (fader, mute, limiter, meters, loudness);
 * 5 monitor = soloed strips (after fader) if any strip is soloed, else a copy
 * of the monitor-source bus. Solo never changes master or aux. */
#define MIXIT_AUDIO_BUS_MASTER   0
#define MIXIT_AUDIO_BUS_AUX_A    1
#define MIXIT_AUDIO_BUS_AUX_B    2
#define MIXIT_AUDIO_BUS_AUX_C    3
#define MIXIT_AUDIO_BUS_AUX_D    4
#define MIXIT_AUDIO_BUS_MONITOR  5
#define MIXIT_AUDIO_MIX_BUSES    5   /* master + aux; valid bits of bus_mask: 0x1F */
#define MIXIT_AUDIO_EQ_BANDS     4
#define MIXIT_AUDIO_MAX_METER_CHANNELS 8
#define MIXIT_AUDIO_FADER_OFF_DB (-96.0f)  /* at or below: silence */

#define MIXIT_EQ_PEAKING     0
#define MIXIT_EQ_LOW_SHELF   1
#define MIXIT_EQ_HIGH_SHELF  2
#define MIXIT_EQ_HIGH_PASS   3
#define MIXIT_EQ_LOW_PASS    4

#define MIXIT_STRIP_MUTE               0x1u
#define MIXIT_STRIP_SOLO               0x2u  /* monitor bus only (AFL) */
#define MIXIT_STRIP_AUDIO_FOLLOWS_VIDEO 0x4u /* audible while the input is on program (default on) */
#define MIXIT_STRIP_HIGH_PASS          0x8u
#define MIXIT_STRIP_GATE               0x10u
#define MIXIT_STRIP_COMPRESSOR         0x20u

typedef struct mixit_eq_band {
    int32_t enabled;       /* 0 / 1 */
    int32_t type;          /* MIXIT_EQ_* */
    float   frequency_hz;  /* 20..20000 */
    float   gain_db;       /* -24..+24 (peaking/shelves) */
    float   q;             /* 0.1..10 */
} mixit_eq_band;

typedef struct mixit_audio_strip {
    uint32_t      struct_size;
    uint32_t      flags;               /* MIXIT_STRIP_* */
    float         trim_db;             /* -24..+24 */
    float         fader_db;            /* <= +12; <= MIXIT_AUDIO_FADER_OFF_DB = off */
    float         balance;             /* -1 left .. +1 right (stereo sessions) */
    float         delay_ms;            /* 0..1000, lip-sync compensation */
    uint32_t      bus_mask;            /* bit 0 master, bits 1..4 aux A..D */
    float         high_pass_hz;        /* 20..1000 */
    mixit_eq_band eq[MIXIT_AUDIO_EQ_BANDS];
    float         gate_threshold_db;   /* -100..0 */
    float         gate_ratio;          /* 0 = gate, 1..20 = downward expander */
    float         gate_range_db;       /* -100..0 (maximum attenuation) */
    float         gate_attack_ms;      /* 0..100 */
    float         gate_hold_ms;        /* 0..2000 */
    float         gate_release_ms;     /* 1..5000 */
    float         comp_threshold_db;   /* -60..0 */
    float         comp_ratio;          /* 1..30 */
    float         comp_knee_db;        /* 0..24 */
    float         comp_attack_ms;      /* 0..500 */
    float         comp_release_ms;     /* 1..5000 */
    float         comp_makeup_db;      /* 0..24 */
} mixit_audio_strip;

#define MIXIT_AUDIO_BUS_FLAG_MUTE    0x1u
#define MIXIT_AUDIO_BUS_FLAG_LIMITER 0x2u  /* brick-wall limiter (master: on by default, -1 dBFS) */

typedef struct mixit_audio_bus_config {
    uint32_t struct_size;
    uint32_t flags;                /* MIXIT_AUDIO_BUS_FLAG_* */
    float    fader_db;             /* <= +12 */
    float    limiter_ceiling_db;   /* -30..0 */
    float    limiter_release_ms;   /* 1..5000 */
    uint32_t reserved;             /* 0 */
} mixit_audio_bus_config;

typedef struct mixit_audio_meter {
    uint32_t struct_size;
    uint32_t id;                   /* input id, or bus index */
    uint32_t channels;
    uint32_t flags;                /* bit 0: gate open (strips) */
    float    peak_db[MIXIT_AUDIO_MAX_METER_CHANNELS];  /* ballistic peak dBFS (-144 = silence) */
    float    rms_db[MIXIT_AUDIO_MAX_METER_CHANNELS];   /* 300 ms RMS dBFS */
    float    gain_reduction_db;    /* strips: compressor; buses: limiter (<= 0) */
    float    gate_reduction_db;    /* strips only (<= 0) */
    float    applied_gain_db;      /* strips: fader × mute × AFV now (-144 = silent) */
    float    momentary_lufs;       /* buses 0..4: EBU R128 (-144 = no signal) */
    float    short_term_lufs;
    float    integrated_lufs;      /* gated, since session start or reset */
} mixit_audio_meter;

typedef struct mixit_audio_block_info {
    uint32_t struct_size;
    uint32_t frames;               /* sample frames written (interleaved, session channels) */
    int64_t  pts_ns;               /* master-clock time of the first frame */
    int64_t  first_sample;         /* session sample index of the first frame */
} mixit_audio_block_info;

/* Strip settings of an input (any time; applied from the next 10 ms block). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_strip(mixit_session session, uint32_t input_id,
                                                                 const mixit_audio_strip* strip);
MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_strip(mixit_session session, uint32_t input_id,
                                                                 mixit_audio_strip* out_strip);
/* Master / aux bus settings (bus 0..4). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_bus(mixit_session session, uint32_t bus,
                                                               const mixit_audio_bus_config* config);
MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_bus(mixit_session session, uint32_t bus,
                                                               mixit_audio_bus_config* out_config);
/* Bus the monitor plays when nothing is soloed (0..4; default master). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_monitor_source(mixit_session session, uint32_t bus);
/* Meters: one input strip, or one bus (0..5). Values are updated every block. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_get_input_meter(mixit_session session, uint32_t input_id,
                                                                 mixit_audio_meter* out_meter);
MIXIT_API mixit_result MIXIT_CALL mixit_session_get_bus_meter(mixit_session session, uint32_t bus,
                                                               mixit_audio_meter* out_meter);
/* Restarts integrated loudness of bus 0..4. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_reset_loudness(mixit_session session, uint32_t bus);
/* Pops mixed audio of a bus (0..5): whole contiguous 10 ms blocks, interleaved
 * float, session channel count. `capacity_frames` must hold at least one
 * block (else BUFFER_TOO_SMALL with info.frames = block size). NOT_FOUND if
 * nothing is waiting. Each bus keeps 500 ms (older blocks are dropped); one
 * reader per bus (diagnostics, tests, simple recorders). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_read_bus_audio(mixit_session session, uint32_t bus, float* dest,
                                                                uint32_t capacity_frames,
                                                                mixit_audio_block_info* out_info);

/* ---- Audio devices (API 0.4, EXPERIMENTAL on Windows) -----------------------
 * WASAPI shared mode. Streams use the session's rate and channel count; Windows
 * converts to the device format and the engine compensates the device clock
 * against the master clock (adaptive resampling, max ±0.5 %).
 * Test device (all platforms, not enumerated): id "mixit:null" or
 * "mixit:null:<ppm>" — paced by the monotonic clock with a clock offset;
 * capture from it is a -20 dBFS 440 Hz tone. */
#define MIXIT_AUDIO_DEVICE_RENDER  0
#define MIXIT_AUDIO_DEVICE_CAPTURE 1
#define MIXIT_AUDIO_DEVICE_FLAG_DEFAULT 0x1u

typedef struct mixit_audio_device_info {
    uint32_t struct_size;
    int32_t  direction;      /* MIXIT_AUDIO_DEVICE_* */
    uint32_t flags;          /* MIXIT_AUDIO_DEVICE_FLAG_* */
    uint32_t channels;       /* device mix format */
    uint32_t sample_rate;
    uint32_t reserved;
    char     name[128];      /* UTF-8, truncated on a character boundary */
    char     id[256];        /* UTF-8, never truncated (longer ids are skipped) */
} mixit_audio_device_info;

/* Lists active devices of one direction (caller sets each struct_size;
 * capacity 0 queries the count). NOT_SUPPORTED where no backend exists. */
MIXIT_API mixit_result MIXIT_CALL mixit_enumerate_audio_devices(mixit_engine engine, int32_t direction,
                                                                 mixit_audio_device_info* devices, uint32_t capacity,
                                                                 uint32_t* out_count);

typedef struct mixit_audio_output_config {
    uint32_t    struct_size;
    uint32_t    bus;          /* bus to play, normally MIXIT_AUDIO_BUS_MONITOR */
    const char* device_id;    /* NULL or "" = default render device */
    uint32_t    latency_ms;   /* 0 = 40; 10..500 (buffer between mixer and device) */
    uint32_t    reserved;     /* 0 */
} mixit_audio_output_config;

/* Plays a bus on a device (headphones / monitor speakers). Any time; replaces
 * the previous output. NULL config turns the output off. A device that fails
 * or disappears is retried every second (stats.state = RETRYING). */
MIXIT_API mixit_result MIXIT_CALL mixit_session_set_audio_output(mixit_session session,
                                                                  const mixit_audio_output_config* config);

#define MIXIT_AUDIO_OUTPUT_OFF      0
#define MIXIT_AUDIO_OUTPUT_RUNNING  1
#define MIXIT_AUDIO_OUTPUT_RETRYING 2

typedef struct mixit_audio_output_stats {
    uint32_t struct_size;
    int32_t  state;              /* MIXIT_AUDIO_OUTPUT_* */
    uint32_t bus;
    uint32_t buffered_ms;
    uint64_t underruns;          /* device needed audio the mixer had not delivered */
    uint64_t device_errors;
    uint64_t dropped_frames;     /* mixer output the device buffer could not take */
    double   drift_ratio;        /* playback ratio (1.0 = clocks agree) */
    char     device_name[128];
} mixit_audio_output_stats;

MIXIT_API mixit_result MIXIT_CALL mixit_session_get_audio_output_stats(mixit_session session,
                                                                        mixit_audio_output_stats* out_stats);

typedef struct mixit_audio_capture_config {
    uint32_t    struct_size;
    uint32_t    flags;        /* reserved, 0 */
    const char* device_id;    /* NULL or "" = default capture device */
} mixit_audio_capture_config;

/* Adds an audio-only input from a capture device (microphone, line in, the
 * audio of a USB/HDMI device). Only while not RUNNING. Audio is placed on the
 * session timeline on arrival (≈30 ms latency); device loss gives silence,
 * counted in input decode_errors, and a retry every second. */
MIXIT_API mixit_result MIXIT_CALL mixit_session_add_audio_capture_source(mixit_session session,
                                                                          const mixit_audio_capture_config* config,
                                                                          uint32_t* out_input_id);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MIXIT_ENGINE_H */
