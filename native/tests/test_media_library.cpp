#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "media/ffmpeg_library.hpp"
#include "mixit/mixit_engine.h"

using mixit::media::FfmpegLibrary;

TEST(MediaLibrary, LicencePolicyRecognisesOnlyLgpl) {
    EXPECT_TRUE(FfmpegLibrary::is_lgpl_license("LGPL version 2.1 or later"));
    EXPECT_TRUE(FfmpegLibrary::is_lgpl_license("LGPL version 3 or later"));
    EXPECT_FALSE(FfmpegLibrary::is_lgpl_license("GPL version 2 or later"));
    EXPECT_FALSE(FfmpegLibrary::is_lgpl_license("GPL version 3 or later"));
    EXPECT_FALSE(FfmpegLibrary::is_lgpl_license("nonfree and unredistributable"));
    EXPECT_FALSE(FfmpegLibrary::is_lgpl_license(""));
}

TEST(MediaLibrary, InfoIsConsistentWithFlags) {
    mixit_media_library_info info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(mixit_get_media_library_info(&info), MIXIT_OK);
    const bool compiled = info.flags & MIXIT_MEDIA_LIBRARY_COMPILED_IN;
    const bool usable = info.flags & MIXIT_MEDIA_LIBRARY_USABLE;
    const bool lgpl = info.flags & MIXIT_MEDIA_LIBRARY_LICENSE_LGPL;
    const bool required = info.flags & MIXIT_MEDIA_LIBRARY_LGPL_REQUIRED;
    const bool versions = info.flags & MIXIT_MEDIA_LIBRARY_VERSIONS_MATCH;
    EXPECT_EQ(usable, compiled && versions && (lgpl || !required)) << "usable must follow the policy";
    if (compiled) {
        EXPECT_GT(info.avcodec_version, 0u);
        EXPECT_GT(std::strlen(info.license), 0u);
        EXPECT_EQ(lgpl, FfmpegLibrary::is_lgpl_license(info.license));
    } else {
        EXPECT_FALSE(usable);
        EXPECT_EQ(info.avcodec_version, 0u);
    }
#if defined(_WIN32)
    EXPECT_TRUE(required) << "product builds must enforce the LGPL policy";
#endif
}

TEST(MediaLibrary, CodecQueries) {
    mixit_codec_info c{};
    c.struct_size = sizeof(c);
    EXPECT_EQ(mixit_query_codec(nullptr, &c), MIXIT_E_INVALID_ARGUMENT);
    EXPECT_EQ(mixit_query_codec("", &c), MIXIT_E_INVALID_ARGUMENT);

    mixit_media_library_info info{};
    info.struct_size = sizeof(info);
    ASSERT_EQ(mixit_get_media_library_info(&info), MIXIT_OK);
    if (!(info.flags & MIXIT_MEDIA_LIBRARY_USABLE)) {
        EXPECT_EQ(mixit_query_codec("h264", &c), MIXIT_E_NOT_SUPPORTED);
        GTEST_SKIP() << "FFmpeg not usable in this build";
    }
    ASSERT_EQ(mixit_query_codec("h264", &c), MIXIT_OK);
    EXPECT_TRUE(c.flags & MIXIT_CODEC_DECODER);
    EXPECT_GT(std::strlen(c.long_name), 0u);
    ASSERT_EQ(mixit_query_codec("aac", &c), MIXIT_OK);
    EXPECT_TRUE(c.flags & MIXIT_CODEC_ENCODER) << "FFmpeg's native AAC encoder is always built";
    EXPECT_EQ(mixit_query_codec("definitely_not_a_codec", &c), MIXIT_E_NOT_FOUND);
}
