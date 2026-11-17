#include <cstring>

#include "mixit/mixit_engine.h"
#include "platform/system_info.hpp"

#if defined(__x86_64__) || defined(_M_X64)
#  define MIXIT_X86_64 1
#  if defined(_MSC_VER) && !defined(__clang__)
#    include <immintrin.h>
#    include <intrin.h>
#  else
#    include <cpuid.h>
#  endif
#endif

namespace mixit {

#if defined(MIXIT_X86_64)
namespace {

struct Regs {
    std::uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
};

Regs cpuid(std::uint32_t leaf, std::uint32_t subleaf) noexcept {
    Regs r;
#if defined(_MSC_VER) && !defined(__clang__)
    int out[4] = {};
    __cpuidex(out, static_cast<int>(leaf), static_cast<int>(subleaf));
    r.eax = static_cast<std::uint32_t>(out[0]);
    r.ebx = static_cast<std::uint32_t>(out[1]);
    r.ecx = static_cast<std::uint32_t>(out[2]);
    r.edx = static_cast<std::uint32_t>(out[3]);
#else
    __cpuid_count(leaf, subleaf, r.eax, r.ebx, r.ecx, r.edx);
#endif
    return r;
}

// XCR0: which register states the OS saves on context switch.
std::uint64_t read_xcr0() noexcept {
#if defined(_MSC_VER) && !defined(__clang__)
    return _xgetbv(0);
#else
    std::uint32_t eax = 0, edx = 0;
    __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
    return (static_cast<std::uint64_t>(edx) << 32) | eax;
#endif
}

constexpr bool bit(std::uint32_t value, int n) noexcept { return ((value >> n) & 1u) != 0; }

}  // namespace

CpuIdInfo probe_cpuid() noexcept {
    CpuIdInfo info;
    const std::uint32_t max_leaf = cpuid(0, 0).eax;
    if (max_leaf >= 1) {
        const Regs l1 = cpuid(1, 0);
        if (bit(l1.edx, 26)) info.features |= MIXIT_CPU_FEATURE_SSE2;
        if (bit(l1.ecx, 19)) info.features |= MIXIT_CPU_FEATURE_SSE4_1;
        if (bit(l1.ecx, 20)) info.features |= MIXIT_CPU_FEATURE_SSE4_2;

        const bool osxsave = bit(l1.ecx, 27);
        const std::uint64_t xcr0 = osxsave ? read_xcr0() : 0;
        const bool os_saves_ymm = (xcr0 & 0x6u) == 0x6u;     // XMM | YMM
        const bool os_saves_zmm = (xcr0 & 0xE6u) == 0xE6u;   // + opmask, ZMM_Hi256, Hi16_ZMM

        if (os_saves_ymm) {
            if (bit(l1.ecx, 28)) info.features |= MIXIT_CPU_FEATURE_AVX;
            if (bit(l1.ecx, 12)) info.features |= MIXIT_CPU_FEATURE_FMA3;
            if (max_leaf >= 7) {
                const Regs l7 = cpuid(7, 0);
                if (bit(l7.ebx, 5)) info.features |= MIXIT_CPU_FEATURE_AVX2;
                if (os_saves_zmm && bit(l7.ebx, 16)) info.features |= MIXIT_CPU_FEATURE_AVX512F;
            }
        }
    }

    if (cpuid(0x80000000u, 0).eax >= 0x80000004u) {
        char brand[49] = {};
        for (std::uint32_t i = 0; i < 3; ++i) {
            const Regs r = cpuid(0x80000002u + i, 0);
            std::memcpy(brand + i * 16 + 0, &r.eax, 4);
            std::memcpy(brand + i * 16 + 4, &r.ebx, 4);
            std::memcpy(brand + i * 16 + 8, &r.ecx, 4);
            std::memcpy(brand + i * 16 + 12, &r.edx, 4);
        }
        try {
            const std::string s(brand);
            const auto first = s.find_first_not_of(' ');
            const auto last = s.find_last_not_of(' ');
            if (first != std::string::npos) info.brand = s.substr(first, last - first + 1);
        } catch (...) {
            info.brand.clear();  // allocation failure: brand is optional
        }
    }
    return info;
}

#else

CpuIdInfo probe_cpuid() noexcept { return {}; }

#endif

}  // namespace mixit
