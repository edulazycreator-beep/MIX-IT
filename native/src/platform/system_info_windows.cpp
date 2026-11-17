// Windows implementation of capability probing (the product platform).
// APIs used are all publicly documented Win32 / DXGI APIs:
//   GetActiveProcessorCount, GetLogicalProcessorInformationEx,
//   GlobalMemoryStatusEx, RtlGetVersion (ntdll), CreateDXGIFactory1,
//   IDXGIFactory1::EnumAdapters1, IDXGIAdapter1::GetDesc1.
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>

#include <memory>
#include <string>
#include <vector>

#include "logging/logger.hpp"
#include "mixit/mixit_engine.h"
#include "platform/system_info.hpp"
#include "platform/win_strings.hpp"

namespace mixit {
namespace {

// Minimal owning pointer for COM interfaces (avoids a WRL/ATL dependency).
template <typename T>
class ComRef {
public:
    ComRef() = default;
    ~ComRef() { reset(); }
    ComRef(const ComRef&) = delete;
    ComRef& operator=(const ComRef&) = delete;
    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T** put() noexcept { reset(); return &ptr_; }
    void reset() noexcept {
        if (ptr_ != nullptr) { ptr_->Release(); ptr_ = nullptr; }
    }
private:
    T* ptr_ = nullptr;
};

std::uint32_t count_physical_cores(Logger& logger) {
    DWORD length = 0;
    ::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &length);
    if (::GetLastError() != ERROR_INSUFFICIENT_BUFFER || length == 0) {
        logger.logf(MIXIT_LOG_WARNING, "caps", "GetLogicalProcessorInformationEx size query failed ({})",
                    ::GetLastError());
        return 0;
    }
    std::vector<unsigned char> buffer(length);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data());
    if (!::GetLogicalProcessorInformationEx(RelationProcessorCore, info, &length)) {
        logger.logf(MIXIT_LOG_WARNING, "caps", "GetLogicalProcessorInformationEx failed ({})", ::GetLastError());
        return 0;
    }
    std::uint32_t cores = 0;
    for (DWORD offset = 0; offset < length;) {
        const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
        if (entry->Relationship == RelationProcessorCore) ++cores;
        if (entry->Size == 0) break;
        offset += entry->Size;
    }
    return cores;
}

std::string describe_windows_version(Logger& logger) {
    // RtlGetVersion reports the true version regardless of the application
    // manifest (GetVersionEx is subject to compatibility shims).
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    auto fn = ntdll == nullptr ? nullptr
                               : reinterpret_cast<RtlGetVersionFn>(
                                     reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (fn == nullptr) {
        logger.log(MIXIT_LOG_WARNING, "caps", "RtlGetVersion unavailable");
        return {};
    }
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (fn(&v) != 0) {
        logger.log(MIXIT_LOG_WARNING, "caps", "RtlGetVersion failed");
        return {};
    }
    // Windows 11 still reports major 10; it is distinguished by build >= 22000.
    const char* family = (v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000) ? "Windows 11" : "Windows";
    return std::string(family) + " " + std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) +
           " (build " + std::to_string(v.dwBuildNumber) + ")";
}

bool enumerate_gpu_adapters(Logger& logger, std::vector<GpuAdapter>& out) {
    ComRef<IDXGIFactory1> factory;
    const HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory.put()));
    if (FAILED(hr)) {
        logger.logf(MIXIT_LOG_WARNING, "caps", "CreateDXGIFactory1 failed (hr=0x{:08X})",
                    static_cast<unsigned long>(hr));
        return false;
    }
    for (UINT i = 0;; ++i) {
        ComRef<IDXGIAdapter1> adapter;
        const HRESULT ehr = factory->EnumAdapters1(i, adapter.put());
        if (ehr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(ehr)) {
            logger.logf(MIXIT_LOG_WARNING, "caps", "EnumAdapters1({}) failed (hr=0x{:08X})", i,
                        static_cast<unsigned long>(ehr));
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            logger.logf(MIXIT_LOG_WARNING, "caps", "GetDesc1 failed for adapter {}", i);
            continue;
        }
        GpuAdapter a;
        a.vendor_id = desc.VendorId;
        a.device_id = desc.DeviceId;
        a.flags = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? MIXIT_GPU_ADAPTER_FLAG_SOFTWARE : 0u;
        a.dedicated_video_memory_bytes = static_cast<std::uint64_t>(desc.DedicatedVideoMemory);
        a.shared_system_memory_bytes = static_cast<std::uint64_t>(desc.SharedSystemMemory);
        a.description = os::wide_to_utf8(desc.Description);
        out.push_back(std::move(a));
    }
    return true;
}

}  // namespace

SystemInfo probe_system_info(Logger& logger) {
    SystemInfo info;

    const CpuIdInfo cpu = probe_cpuid();
    info.cpu_features = cpu.features;
    info.cpu_brand = cpu.brand;
    info.logical_processor_count = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    info.physical_core_count = count_physical_cores(logger);
    if (info.logical_processor_count > 0) info.detection_flags |= MIXIT_DETECTED_CPU;

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    if (::GlobalMemoryStatusEx(&mem)) {
        info.total_physical_memory_bytes = mem.ullTotalPhys;
        info.available_physical_memory_bytes = mem.ullAvailPhys;
        info.detection_flags |= MIXIT_DETECTED_MEMORY;
    } else {
        logger.logf(MIXIT_LOG_WARNING, "caps", "GlobalMemoryStatusEx failed ({})", ::GetLastError());
    }

    info.os_description = describe_windows_version(logger);
    if (!info.os_description.empty()) info.detection_flags |= MIXIT_DETECTED_OS;

    if (enumerate_gpu_adapters(logger, info.gpu_adapters)) {
        info.detection_flags |= MIXIT_DETECTED_GPU_ADAPTERS;
    }
    return info;
}

}  // namespace mixit

#endif
