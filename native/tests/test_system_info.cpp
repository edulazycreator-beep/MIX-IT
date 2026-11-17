#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "logging/logger.hpp"
#include "platform/system_info.hpp"

using namespace mixit;

namespace {
#if defined(_WIN32)
// Wine (used to run the Windows build from Linux CI) exports this from
// ntdll. Real Windows does not.
bool running_under_wine() {
    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    return ntdll != nullptr && ::GetProcAddress(ntdll, "wine_get_version") != nullptr;
}
#endif
}  // namespace

TEST(SystemInfo, CpuIdReportsBaselineOnX64) {
#if defined(__x86_64__) || defined(_M_X64)
    const CpuIdInfo cpu = probe_cpuid();
    EXPECT_TRUE(cpu.features & MIXIT_CPU_FEATURE_SSE2) << "SSE2 is architecturally guaranteed on x86-64";
    if (cpu.features & MIXIT_CPU_FEATURE_AVX2) {
        EXPECT_TRUE(cpu.features & MIXIT_CPU_FEATURE_AVX) << "AVX2 implies OS-enabled AVX";
    }
    if (cpu.features & MIXIT_CPU_FEATURE_AVX512F) {
        EXPECT_TRUE(cpu.features & MIXIT_CPU_FEATURE_AVX);
    }
    if (!cpu.brand.empty()) {
        EXPECT_NE(cpu.brand.front(), ' ');
        EXPECT_NE(cpu.brand.back(), ' ');
    }
#else
    GTEST_SKIP() << "x86-64 only";
#endif
}

TEST(SystemInfo, ProbeFillsCoreFields) {
    std::unique_ptr<Logger> logger;
    ASSERT_TRUE(Logger::create(LoggerOptions{}, logger).is_ok());
    const SystemInfo info = probe_system_info(*logger);

    EXPECT_TRUE(info.detection_flags & MIXIT_DETECTED_CPU);
    EXPECT_TRUE(info.detection_flags & MIXIT_DETECTED_MEMORY);
    EXPECT_TRUE(info.detection_flags & MIXIT_DETECTED_OS);
    EXPECT_GT(info.logical_processor_count, 0u);
    EXPECT_LE(info.physical_core_count, info.logical_processor_count);
    EXPECT_GT(info.total_physical_memory_bytes, 64ull * 1024 * 1024);
    EXPECT_LE(info.available_physical_memory_bytes, info.total_physical_memory_bytes);
    EXPECT_FALSE(info.os_description.empty());

#if defined(_WIN32)
    if (running_under_wine()) {
        // Headless Wine has no display adapter; DXGI factory creation may
        // fail. The engine must then degrade cleanly: flag clear, no adapters.
        if (!(info.detection_flags & MIXIT_DETECTED_GPU_ADAPTERS)) {
            EXPECT_TRUE(info.gpu_adapters.empty());
        }
    } else {
        // Real Windows always exposes at least the Microsoft Basic Render Driver.
        EXPECT_TRUE(info.detection_flags & MIXIT_DETECTED_GPU_ADAPTERS) << "DXGI enumeration should succeed on Windows";
        EXPECT_FALSE(info.gpu_adapters.empty());
    }
#else
    EXPECT_FALSE(info.detection_flags & MIXIT_DETECTED_GPU_ADAPTERS)
        << "GPU enumeration is not implemented on the Linux dev host and must not be reported as detected";
    EXPECT_TRUE(info.gpu_adapters.empty());
#endif
}
