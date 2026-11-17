// Helpers shared by every C ABI translation unit: failure reporting, the
// exception barrier, struct-size negotiation and handle conversion.
// No engine logic lives here.
#pragma once

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>

#include "abi/handle_registry.hpp"
#include "core/engine.hpp"
#include "core/last_error.hpp"
#include "core/status.hpp"
#include "mixit/mixit_engine.h"

namespace mixit::abi {

inline mixit_result fail(const char* function, mixit_result code, std::string_view message) noexcept {
    set_last_error(function, message);
    return code;
}

inline mixit_result report(const char* function, const Status& status) noexcept {
    return status.is_ok() ? MIXIT_OK : fail(function, status.code(), status.message());
}

// Exception barrier: nothing thrown inside may cross the C boundary.
template <typename Fn>
mixit_result guarded(const char* function, Fn&& fn) noexcept {
    try {
        return fn();
    } catch (const std::bad_alloc&) {
        return fail(function, MIXIT_E_OUT_OF_MEMORY, "out of memory");
    } catch (const std::exception& e) {
        return fail(function, MIXIT_E_INTERNAL, e.what());
    } catch (...) {
        return fail(function, MIXIT_E_INTERNAL, "unknown exception");
    }
}

// The v0.x size of each extensible struct is its current sizeof; callers
// compiled against a newer header pass a larger struct_size.
template <typename T>
bool struct_size_ok(const T* p) noexcept {
    return p != nullptr && p->struct_size >= sizeof(T);
}

// Copies a fully populated struct into the caller's, preserving the caller's
// struct_size and never writing past what this library knows.
template <typename T>
void write_out(T* dest, const T& value) noexcept {
    static_assert(std::is_trivially_copyable_v<T>);
    const std::uint32_t caller_size = dest->struct_size;
    std::memcpy(dest, &value, sizeof(T));
    dest->struct_size = caller_size;
}

template <typename Handle>
std::uint64_t to_id(Handle handle) noexcept {
    return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
}

template <typename Handle>
Handle to_handle(std::uint64_t id) noexcept {
    return reinterpret_cast<Handle>(static_cast<std::uintptr_t>(id));
}

// Live engines. Intentionally leaked (see engine_abi.cpp).
HandleRegistry<Engine>& engine_registry();

inline std::shared_ptr<Engine> resolve_engine(mixit_engine handle) {
    return handle == nullptr ? nullptr : engine_registry().find(to_id(handle));
}

}  // namespace mixit::abi
