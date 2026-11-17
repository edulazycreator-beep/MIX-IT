// Linux implementation. Linux is a DEVELOPMENT / CI HOST ONLY for Mix It:
// it lets the engine core be built and tested without Windows. It is not a
// supported product platform. GPU adapter enumeration is intentionally not
// implemented here (the product uses DXGI on Windows).
#if defined(__linux__)

#include <sys/utsname.h>
#include <unistd.h>

#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <utility>

#include "logging/logger.hpp"
#include "mixit/mixit_engine.h"
#include "platform/system_info.hpp"

namespace mixit {
namespace {

std::uint64_t meminfo_kib(const std::string& key) {
    std::ifstream in("/proc/meminfo");
    std::string name;
    std::uint64_t value = 0;
    std::string unit;
    while (in >> name >> value) {
        std::getline(in, unit);
        if (name == key + ":") return value;
    }
    return 0;
}

std::uint32_t count_physical_cores() {
    std::ifstream in("/proc/cpuinfo");
    std::set<std::pair<int, int>> cores;
    int physical_id = 0;
    std::string line;
    while (std::getline(in, line)) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string key = line.substr(0, line.find_last_not_of(" \t", colon - 1) + 1);
        const std::string value = line.substr(colon + 1);
        try {
            if (key == "physical id") physical_id = std::stoi(value);
            else if (key == "core id") cores.emplace(physical_id, std::stoi(value));
        } catch (...) {
            // malformed line: ignore
        }
    }
    return static_cast<std::uint32_t>(cores.size());
}

}  // namespace

SystemInfo probe_system_info(Logger& logger) {
    SystemInfo info;

    const CpuIdInfo cpu = probe_cpuid();
    info.cpu_features = cpu.features;
    info.cpu_brand = cpu.brand;
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    info.logical_processor_count = online > 0 ? static_cast<std::uint32_t>(online) : 0;
    info.physical_core_count = count_physical_cores();
    if (info.logical_processor_count > 0) {
        info.detection_flags |= MIXIT_DETECTED_CPU;
    } else {
        logger.log(MIXIT_LOG_WARNING, "caps", "could not determine processor count");
    }

    const std::uint64_t total = meminfo_kib("MemTotal");
    const std::uint64_t available = meminfo_kib("MemAvailable");
    if (total > 0) {
        info.total_physical_memory_bytes = total * 1024;
        info.available_physical_memory_bytes = available * 1024;
        info.detection_flags |= MIXIT_DETECTED_MEMORY;
    } else {
        logger.log(MIXIT_LOG_WARNING, "caps", "could not read /proc/meminfo");
    }

    utsname u{};
    if (::uname(&u) == 0) {
        std::ostringstream os;
        os << u.sysname << ' ' << u.release << ' ' << u.machine << " (development host)";
        info.os_description = os.str();
        info.detection_flags |= MIXIT_DETECTED_OS;
    }

    logger.log(MIXIT_LOG_INFO, "caps",
               "GPU adapter enumeration is not implemented on Linux (development host); "
               "MIXIT_DETECTED_GPU_ADAPTERS is not set");
    return info;
}

}  // namespace mixit

#endif
