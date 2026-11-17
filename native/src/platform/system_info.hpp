// Hardware / OS capability probing.
//
// Milestone 1 scope: CPU, memory, OS, GPU adapters (Windows/DXGI).
// Later milestones extend this with encoders (M5), audio devices (M4),
// capture devices (M3), network and storage — each as its own probe so a
// failing probe never hides the others. See docs/ENGINE_DESIGN.md.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mixit {

class Logger;

struct GpuAdapter {
    std::uint32_t vendor_id = 0;
    std::uint32_t device_id = 0;
    std::uint32_t flags = 0;  // MIXIT_GPU_ADAPTER_FLAG_*
    std::uint64_t dedicated_video_memory_bytes = 0;
    std::uint64_t shared_system_memory_bytes = 0;
    std::string description;
};

struct SystemInfo {
    std::uint32_t detection_flags = 0;  // MIXIT_DETECTED_*
    std::uint32_t logical_processor_count = 0;
    std::uint32_t physical_core_count = 0;
    std::uint32_t cpu_features = 0;     // MIXIT_CPU_FEATURE_*
    std::uint64_t total_physical_memory_bytes = 0;
    std::uint64_t available_physical_memory_bytes = 0;
    std::string cpu_brand;
    std::string os_description;
    std::vector<GpuAdapter> gpu_adapters;
};

struct CpuIdInfo {
    std::uint32_t features = 0;
    std::string brand;
};

// Portable x86-64 CPUID probe (returns empty info on other architectures).
// AVX-family bits are only reported when the OS saves the YMM/ZMM state.
CpuIdInfo probe_cpuid() noexcept;

// Platform-specific full probe. Never throws; probes that fail leave their
// MIXIT_DETECTED_* bit clear and log a warning.
SystemInfo probe_system_info(Logger& logger);

}  // namespace mixit
