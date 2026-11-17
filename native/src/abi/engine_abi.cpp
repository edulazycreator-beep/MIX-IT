// C ABI implementation: argument validation, handle resolution, struct-size
// negotiation and the exception barrier. No engine logic lives here.
#include <cstddef>
#include <cstring>
#include <exception>
#include <new>
#include <string>
#include <type_traits>

#include "abi/abi_common.hpp"
#include "mixit/mixit_engine.h"
#include "util/clock.hpp"

#ifndef MIXIT_BUILD_CONFIG
#define MIXIT_BUILD_CONFIG "unknown"
#endif

// ABI layout contract (x64). The managed binding asserts the same numbers
// (MixIt.Interop NativeStructSizes); changing a struct requires changing both
// and bumping MIXIT_API_VERSION.
static_assert(sizeof(void*) == 8, "MIX IT ABI is defined for 64-bit targets");
static_assert(sizeof(mixit_engine_config) == 40);
static_assert(offsetof(mixit_engine_config, log_file_path) == 16);
static_assert(offsetof(mixit_engine_config, log_callback) == 24);
static_assert(offsetof(mixit_engine_config, log_user_data) == 32);
static_assert(sizeof(mixit_log_record) == 48);
static_assert(offsetof(mixit_log_record, unix_time_ns) == 8);
static_assert(offsetof(mixit_log_record, message) == 40);
static_assert(sizeof(mixit_engine_diagnostics) == 48);
static_assert(sizeof(mixit_system_capabilities) == 232);
static_assert(offsetof(mixit_system_capabilities, total_physical_memory_bytes) == 24);
static_assert(offsetof(mixit_system_capabilities, cpu_brand) == 40);
static_assert(offsetof(mixit_system_capabilities, os_description) == 104);
static_assert(sizeof(mixit_gpu_adapter_info) == 160);
static_assert(offsetof(mixit_gpu_adapter_info, description) == 32);

namespace {

using mixit::Engine;
using mixit::Status;
using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::resolve_engine;
using mixit::abi::struct_size_ok;
using mixit::abi::to_handle;
using mixit::abi::to_id;
using mixit::abi::write_out;

}  // namespace

namespace mixit::abi {

// Intentionally leaked: no destructor may run during library unload (on
// Windows that happens under the loader lock, where joining engine threads
// would deadlock). Hosts must destroy their engines before unloading.
HandleRegistry<Engine>& engine_registry() {
    static auto* instance = new HandleRegistry<Engine>();
    return *instance;
}

}  // namespace mixit::abi

extern "C" {

MIXIT_API uint32_t MIXIT_CALL mixit_get_api_version(void) { return MIXIT_API_VERSION; }

MIXIT_API const char* MIXIT_CALL mixit_get_build_info(void) {
#define MIXIT_STR2(x) #x
#define MIXIT_STR(x) MIXIT_STR2(x)
#if defined(_MSC_VER) && !defined(__clang__)
#define MIXIT_COMPILER "MSVC " MIXIT_STR(_MSC_FULL_VER)
#elif defined(__clang__)
#define MIXIT_COMPILER "Clang " __clang_version__
#elif defined(__GNUC__)
#define MIXIT_COMPILER "GCC " __VERSION__
#else
#define MIXIT_COMPILER "unknown compiler"
#endif
#if defined(_WIN32)
#define MIXIT_PLATFORM "windows-x64"
#elif defined(__linux__)
#define MIXIT_PLATFORM "linux-x64 (development host)"
#else
#define MIXIT_PLATFORM "unknown"
#endif
    return "MIX IT native engine " MIXIT_STR(MIXIT_API_VERSION_MAJOR) "." MIXIT_STR(MIXIT_API_VERSION_MINOR) "." MIXIT_STR(
        MIXIT_API_VERSION_PATCH) " | " MIXIT_COMPILER " | " MIXIT_BUILD_CONFIG " | " MIXIT_PLATFORM;
}

MIXIT_API const char* MIXIT_CALL mixit_result_name(mixit_result result) {
    switch (result) {
        case MIXIT_OK: return "MIXIT_OK";
        case MIXIT_E_INVALID_ARGUMENT: return "MIXIT_E_INVALID_ARGUMENT";
        case MIXIT_E_INVALID_HANDLE: return "MIXIT_E_INVALID_HANDLE";
        case MIXIT_E_INVALID_STATE: return "MIXIT_E_INVALID_STATE";
        case MIXIT_E_OUT_OF_MEMORY: return "MIXIT_E_OUT_OF_MEMORY";
        case MIXIT_E_NOT_SUPPORTED: return "MIXIT_E_NOT_SUPPORTED";
        case MIXIT_E_BUFFER_TOO_SMALL: return "MIXIT_E_BUFFER_TOO_SMALL";
        case MIXIT_E_VERSION_MISMATCH: return "MIXIT_E_VERSION_MISMATCH";
        case MIXIT_E_NOT_FOUND: return "MIXIT_E_NOT_FOUND";
        case MIXIT_E_PLATFORM: return "MIXIT_E_PLATFORM";
        case MIXIT_E_IO: return "MIXIT_E_IO";
        case MIXIT_E_INTERNAL: return "MIXIT_E_INTERNAL";
        default: return "MIXIT_E_UNKNOWN";
    }
}

MIXIT_API uint64_t MIXIT_CALL mixit_get_monotonic_time_ns(void) { return mixit::monotonic_ns(); }

MIXIT_API mixit_result MIXIT_CALL mixit_get_last_error_message(char* buffer, uint32_t capacity,
                                                                uint32_t* out_required) {
    return mixit::copy_last_error(buffer, capacity, out_required);
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_create(const mixit_engine_config* config, mixit_engine* out_engine) {
    constexpr const char* fn = "mixit_engine_create";
    if (out_engine == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_engine is NULL");
    *out_engine = nullptr;
    if (config == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config is NULL");
    if (config->struct_size < sizeof(mixit_engine_config)) {
        return fail(fn, MIXIT_E_INVALID_ARGUMENT, "config->struct_size is smaller than mixit_engine_config v0.1");
    }
    return guarded(fn, [&]() -> mixit_result {
        const uint32_t v = config->api_version;
        // 0.x: major and minor must both match. 1.x+: major match, caller minor <= ours.
        const bool compatible = MIXIT_VERSION_MAJOR(v) == MIXIT_API_VERSION_MAJOR &&
                                (MIXIT_API_VERSION_MAJOR == 0 ? MIXIT_VERSION_MINOR(v) == MIXIT_API_VERSION_MINOR
                                                              : MIXIT_VERSION_MINOR(v) <= MIXIT_API_VERSION_MINOR);
        if (!compatible) {
            return fail(fn, MIXIT_E_VERSION_MISMATCH,
                        "caller API " + std::to_string(MIXIT_VERSION_MAJOR(v)) + "." +
                            std::to_string(MIXIT_VERSION_MINOR(v)) + " is incompatible with engine API " +
                            std::to_string(MIXIT_API_VERSION_MAJOR) + "." + std::to_string(MIXIT_API_VERSION_MINOR));
        }

        std::unique_ptr<Engine> engine;
        if (Status s = Engine::create(*config, engine); !s.is_ok()) return report(fn, s);
        const std::uint64_t id = mixit::abi::engine_registry().add(std::shared_ptr<Engine>(std::move(engine)));
        *out_engine = to_handle<mixit_engine>(id);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_shutdown(mixit_engine engine) {
    constexpr const char* fn = "mixit_engine_shutdown";
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        return report(fn, e->shutdown());
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_destroy(mixit_engine engine) {
    constexpr const char* fn = "mixit_engine_destroy";
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        if (e->is_logger_thread()) {
            return fail(fn, MIXIT_E_INVALID_STATE, "destroy must not be called from the log callback");
        }
        if (!mixit::abi::engine_registry().remove(to_id(engine))) {
            return fail(fn, MIXIT_E_INVALID_HANDLE, "engine handle was destroyed concurrently");
        }
        // Shut down now (joins engine threads) so the host can rely on "no
        // callbacks after destroy returns", even if another thread still
        // holds a reference for an in-flight call.
        return report(fn, e->shutdown());
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_get_diagnostics(mixit_engine engine,
                                                                mixit_engine_diagnostics* out_diagnostics) {
    constexpr const char* fn = "mixit_engine_get_diagnostics";
    if (!struct_size_ok(out_diagnostics)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_diagnostics NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        mixit_engine_diagnostics d{};
        d.struct_size = sizeof(d);
        e->fill_diagnostics(d);
        write_out(out_diagnostics, d);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_set_log_level(mixit_engine engine, int32_t level) {
    constexpr const char* fn = "mixit_engine_set_log_level";
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        return report(fn, e->set_log_level(level));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_log(mixit_engine engine, int32_t level, const char* category,
                                                    const char* message) {
    constexpr const char* fn = "mixit_engine_log";
    if (message == nullptr) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "message is NULL");
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        return report(fn, e->log(level, category != nullptr ? category : "host", message));
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_detect_capabilities(mixit_engine engine,
                                                                    mixit_system_capabilities* out_capabilities) {
    constexpr const char* fn = "mixit_engine_detect_capabilities";
    if (!struct_size_ok(out_capabilities)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_capabilities NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        mixit::SystemInfo info;
        if (Status s = e->detect_capabilities(info); !s.is_ok()) return report(fn, s);

        mixit_system_capabilities c{};
        c.struct_size = sizeof(c);
        c.detection_flags = info.detection_flags;
        c.logical_processor_count = info.logical_processor_count;
        c.physical_core_count = info.physical_core_count;
        c.cpu_features = info.cpu_features;
        c.gpu_adapter_count = static_cast<uint32_t>(info.gpu_adapters.size());
        c.total_physical_memory_bytes = info.total_physical_memory_bytes;
        c.available_physical_memory_bytes = info.available_physical_memory_bytes;
        mixit::copy_utf8_truncated(c.cpu_brand, sizeof(c.cpu_brand), info.cpu_brand);
        mixit::copy_utf8_truncated(c.os_description, sizeof(c.os_description), info.os_description);
        write_out(out_capabilities, c);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_engine_get_gpu_adapter(mixit_engine engine, uint32_t index,
                                                                mixit_gpu_adapter_info* out_adapter) {
    constexpr const char* fn = "mixit_engine_get_gpu_adapter";
    if (!struct_size_ok(out_adapter)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_adapter NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const auto e = resolve_engine(engine);
        if (!e) return fail(fn, MIXIT_E_INVALID_HANDLE, "invalid engine handle");
        mixit::GpuAdapter adapter;
        if (Status s = e->gpu_adapter(index, adapter); !s.is_ok()) return report(fn, s);

        mixit_gpu_adapter_info a{};
        a.struct_size = sizeof(a);
        a.vendor_id = adapter.vendor_id;
        a.device_id = adapter.device_id;
        a.flags = adapter.flags;
        a.dedicated_video_memory_bytes = adapter.dedicated_video_memory_bytes;
        a.shared_system_memory_bytes = adapter.shared_system_memory_bytes;
        mixit::copy_utf8_truncated(a.description, sizeof(a.description), adapter.description);
        write_out(out_adapter, a);
        return MIXIT_OK;
    });
}

}  // extern "C"
