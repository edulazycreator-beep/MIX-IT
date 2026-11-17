// Media file input (M3): demux + decode with FFmpeg, converted to the
// session's formats (BGRA8 video at the file's size, float audio at the
// session's rate/channels), paced by presentation timestamps.
//
// Security: only the "file" protocol is allowed, so a crafted file (e.g. a
// playlist) cannot make the engine open network or device URLs.
//
// Timeline: media time 0 = the container start time. Each loop restarts the
// file with an offset equal to the loop length, where the loop length is the
// longer of the video and audio durations; the shorter stream is padded
// (audio with silence, video by holding the last frame) so audio and video
// stay locked across loops.
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "core/status.hpp"
#include "media/media_source.hpp"

namespace mixit {
class Logger;
}

namespace mixit::media {

struct FileSourceOptions {
    bool loop = false;
};

class FileSource;

// Opens and probes `path_utf8`. Returns NOT_SUPPORTED if FFmpeg is absent or
// rejected by the licence policy, NOT_FOUND if the file does not exist,
// INVALID_ARGUMENT/NOT_SUPPORTED for files without usable streams.
Status open_file_source(const std::string& path_utf8, const FileSourceOptions& options, Logger& logger,
                        std::unique_ptr<MediaSource>& out);

}  // namespace mixit::media
