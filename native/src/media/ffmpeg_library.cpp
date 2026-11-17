#include "media/ffmpeg_library.hpp"

#include "logging/logger.hpp"

#if MIXIT_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}
#endif

#ifndef MIXIT_REQUIRE_LGPL_FFMPEG
#define MIXIT_REQUIRE_LGPL_FFMPEG 0
#endif

namespace mixit::media {

bool FfmpegLibrary::is_lgpl_license(std::string_view license) noexcept {
    // FFmpeg reports e.g. "LGPL version 2.1 or later", "GPL version 2 or later",
    // "GPL version 3 or later", "nonfree and unredistributable".
    return license.starts_with("LGPL");
}

namespace {

FfmpegInfo probe() {
    FfmpegInfo info;
    info.lgpl_required = MIXIT_REQUIRE_LGPL_FFMPEG != 0;
#if MIXIT_HAVE_FFMPEG
    info.compiled_in = true;
    info.avcodec_version = avcodec_version();
    info.avformat_version = avformat_version();
    info.avutil_version = avutil_version();
    // A DLL from another major release has a different ABI: never use it.
    info.versions_match = AV_VERSION_MAJOR(info.avcodec_version) == LIBAVCODEC_VERSION_MAJOR &&
                          AV_VERSION_MAJOR(info.avformat_version) == LIBAVFORMAT_VERSION_MAJOR &&
                          AV_VERSION_MAJOR(info.avutil_version) == LIBAVUTIL_VERSION_MAJOR;
    const char* version = av_version_info();
    info.version_string = version != nullptr ? version : "";
    const char* license = avcodec_license();
    info.license = license != nullptr ? license : "";
    const char* format_license = avformat_license();
    const char* util_license = avutil_license();
    info.license_ok = FfmpegLibrary::is_lgpl_license(info.license) && format_license != nullptr &&
                      FfmpegLibrary::is_lgpl_license(format_license) &&
                      util_license != nullptr && FfmpegLibrary::is_lgpl_license(util_license);
#endif
    return info;
}

std::string version_text(std::uint32_t v) {
    return std::to_string(v >> 16) + "." + std::to_string((v >> 8) & 0xFF) + "." + std::to_string(v & 0xFF);
}

}  // namespace

const FfmpegInfo& FfmpegLibrary::info() {
    static const FfmpegInfo cached = probe();
    return cached;
}

void FfmpegLibrary::log_info(Logger& logger) {
    const FfmpegInfo& i = info();
    if (!i.compiled_in) {
        logger.log(MIXIT_LOG_INFO, "ffmpeg", "built without FFmpeg (MIXIT_WITH_FFMPEG off or not found)");
        return;
    }
    logger.logf(MIXIT_LOG_INFO, "ffmpeg", "FFmpeg {} (avcodec {}, avformat {}, avutil {}), licence '{}'", i.version_string,
                version_text(i.avcodec_version), version_text(i.avformat_version), version_text(i.avutil_version),
                i.license);
    if (!i.versions_match) {
        logger.log(MIXIT_LOG_ERROR, "ffmpeg", "FFmpeg runtime major versions differ from the build headers; FFmpeg disabled");
    }
    if (!i.license_ok) {
        logger.log(i.lgpl_required ? MIXIT_LOG_ERROR : MIXIT_LOG_WARNING, "ffmpeg",
                   i.lgpl_required ? "FFmpeg is not an LGPL build: rejected by the licence policy (ADR-0004)"
                                   : "FFmpeg is not an LGPL build: tolerated on this development build only, "
                                     "never distribute it with MIX IT");
    }
#if MIXIT_HAVE_FFMPEG
    logger.logf(MIXIT_LOG_DEBUG, "ffmpeg", "configuration: {}", avcodec_configuration());
#endif
}

Status FfmpegLibrary::query_codec(std::string_view name, CodecAvailability& out) {
    out = {};
#if MIXIT_HAVE_FFMPEG
    if (!info().usable()) return {MIXIT_E_NOT_SUPPORTED, "FFmpeg is present but not usable (version or licence check failed)"};
    const std::string n(name);
    const AVCodec* dec = avcodec_find_decoder_by_name(n.c_str());
    const AVCodec* enc = avcodec_find_encoder_by_name(n.c_str());
    if (dec == nullptr && enc == nullptr) return {MIXIT_E_NOT_FOUND, "codec '" + n + "' is not in this FFmpeg build"};
    const AVCodec* c = enc != nullptr ? enc : dec;
    out.decoder = dec != nullptr;
    out.encoder = enc != nullptr;
    // NVENC/AMF report HARDWARE; Quick Sync reports HYBRID (hardware with CPU assistance).
    out.hardware = (c->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_HYBRID)) != 0;
    out.long_name = c->long_name != nullptr ? c->long_name : "";
    return Status::ok();
#else
    (void)name;
    return {MIXIT_E_NOT_SUPPORTED, "this engine was built without FFmpeg"};
#endif
}

}  // namespace mixit::media
