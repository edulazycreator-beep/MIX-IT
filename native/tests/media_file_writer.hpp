// Test helper: writes media files containing the MIX IT test signal using
// FFmpeg's own (LGPL) encoders, so file-input tests need no external assets.
#pragma once

#if MIXIT_HAVE_FFMPEG

#include <cstdint>
#include <string>

#include "media/time_base.hpp"

namespace mixit::testing {

struct TestMediaSpec {
    std::string path;
    std::string container = "nut";         // "nut" (raw, bit exact) or "mp4" (compressed)
    bool compressed = false;               // false: rawvideo BGRA + pcm_f32le; true: mpeg4 + aac
    bool video = true;
    bool audio = true;
    std::uint32_t width = 320;
    std::uint32_t height = 180;
    media::Rate frame_rate{30, 1};
    std::int64_t frames = 30;
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t flash_interval_frames = 10;
};

// Returns an empty string on success, otherwise an error description.
std::string write_test_media(const TestMediaSpec& spec);

}  // namespace mixit::testing

#endif
