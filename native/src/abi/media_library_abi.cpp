// C ABI for media library (FFmpeg) information.
#include <string>

#include "abi/abi_common.hpp"
#include "media/ffmpeg_library.hpp"

static_assert(sizeof(mixit_media_library_info) == 152);
static_assert(sizeof(mixit_codec_info) == 128);

using mixit::abi::fail;
using mixit::abi::guarded;
using mixit::abi::report;
using mixit::abi::struct_size_ok;
using mixit::abi::write_out;
using mixit::media::FfmpegLibrary;

extern "C" {

MIXIT_API mixit_result MIXIT_CALL mixit_get_media_library_info(mixit_media_library_info* out_info) {
    constexpr const char* fn = "mixit_get_media_library_info";
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        const mixit::media::FfmpegInfo& i = FfmpegLibrary::info();
        mixit_media_library_info o{};
        o.struct_size = sizeof(o);
        if (i.compiled_in) o.flags |= MIXIT_MEDIA_LIBRARY_COMPILED_IN;
        if (i.versions_match) o.flags |= MIXIT_MEDIA_LIBRARY_VERSIONS_MATCH;
        if (i.license_ok) o.flags |= MIXIT_MEDIA_LIBRARY_LICENSE_LGPL;
        if (i.lgpl_required) o.flags |= MIXIT_MEDIA_LIBRARY_LGPL_REQUIRED;
        if (i.usable()) o.flags |= MIXIT_MEDIA_LIBRARY_USABLE;
        o.avcodec_version = i.avcodec_version;
        o.avformat_version = i.avformat_version;
        o.avutil_version = i.avutil_version;
        mixit::copy_utf8_truncated(o.version, sizeof(o.version), i.version_string);
        mixit::copy_utf8_truncated(o.license, sizeof(o.license), i.license);
        write_out(out_info, o);
        return MIXIT_OK;
    });
}

MIXIT_API mixit_result MIXIT_CALL mixit_query_codec(const char* name, mixit_codec_info* out_info) {
    constexpr const char* fn = "mixit_query_codec";
    if (name == nullptr || name[0] == '\0') return fail(fn, MIXIT_E_INVALID_ARGUMENT, "name is NULL or empty");
    if (!struct_size_ok(out_info)) return fail(fn, MIXIT_E_INVALID_ARGUMENT, "out_info NULL or struct_size too small");
    return guarded(fn, [&]() -> mixit_result {
        mixit::media::CodecAvailability c;
        if (mixit::Status s = FfmpegLibrary::query_codec(name, c); !s.is_ok()) return report(fn, s);
        mixit_codec_info o{};
        o.struct_size = sizeof(o);
        if (c.decoder) o.flags |= MIXIT_CODEC_DECODER;
        if (c.encoder) o.flags |= MIXIT_CODEC_ENCODER;
        if (c.hardware) o.flags |= MIXIT_CODEC_HARDWARE;
        mixit::copy_utf8_truncated(o.long_name, sizeof(o.long_name), c.long_name);
        write_out(out_info, o);
        return MIXIT_OK;
    });
}

}  // extern "C"
