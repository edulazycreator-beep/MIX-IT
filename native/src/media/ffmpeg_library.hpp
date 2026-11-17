// FFmpeg integration point (M2): detection, version/licence checks and
// codec availability. Decoding/encoding arrive in M3/M5 and must call
// FfmpegLibrary::usable() before touching FFmpeg.
//
// Licence policy (ADR-0004): a commercial MIX IT build must load an LGPL
// FFmpeg. When MIXIT_REQUIRE_LGPL_FFMPEG is enabled (default on Windows),
// any library reporting a non-LGPL licence (GPL, nonfree) is REJECTED:
// reported, logged as an error, and never used.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/status.hpp"

namespace mixit {
class Logger;
}

namespace mixit::media {

struct FfmpegInfo {
    bool compiled_in = false;      // built with FFmpeg headers/libraries
    bool versions_match = false;   // runtime major versions == build-time major versions
    bool license_ok = false;       // runtime libraries report an LGPL licence
    bool lgpl_required = false;    // this build enforces the LGPL policy
    std::uint32_t avcodec_version = 0;   // AV_VERSION_INT packing (major<<16 | minor<<8 | micro)
    std::uint32_t avformat_version = 0;
    std::uint32_t avutil_version = 0;
    std::string version_string;    // av_version_info()
    std::string license;           // avcodec_license()

    [[nodiscard]] bool usable() const noexcept {
        return compiled_in && versions_match && (license_ok || !lgpl_required);
    }
};

struct CodecAvailability {
    bool decoder = false;
    bool encoder = false;
    bool hardware = false;  // codec is backed by dedicated hardware (NVENC, QSV, AMF, ...)
    std::string long_name;
};

class FfmpegLibrary {
public:
    // Probes once per process (thread-safe); the result never changes.
    static const FfmpegInfo& info();

    // Writes the probe result (versions, licence, policy verdict) to `logger`.
    static void log_info(Logger& logger);

    // Whether `name` exists in the loaded build as a decoder and/or encoder.
    // Availability in the build does NOT prove the hardware/driver can run
    // it; encoders are open-tested in M5. NOT_SUPPORTED without FFmpeg.
    static Status query_codec(std::string_view name, CodecAvailability& out);

    // Licence string check used by the policy (exposed for tests).
    static bool is_lgpl_license(std::string_view license) noexcept;
};

}  // namespace mixit::media
