// Test helper: decodes a recorded file / stream completely (FFmpeg).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mixit::test {

struct DecodedMedia {
    std::string error;            // empty = success
    std::string format;           // demuxer name
    std::string video_codec, audio_codec;
    int width = 0, height = 0;
    int video_streams = 0, audio_streams = 0;
    std::vector<double> frame_times;     // seconds, from the container
    std::vector<double> bottom_luma;     // mean Y of the bottom 1/8 of each frame (flash detector)
    int decode_errors = 0;
    int sample_rate = 0, channels = 0;
    double audio_start = 0.0;            // seconds, first decoded sample
    std::vector<float> audio;            // first channel, decoded
};

// `options`: demuxer options "key=value;key=value" (e.g. "listen=1"); a
// non-zero `max_seconds` stops after that much media time (live inputs).
DecodedMedia decode_media(const std::string& url, const std::string& options = {}, double max_seconds = 0.0,
                          double timeout_seconds = 20.0);

}  // namespace mixit::test
