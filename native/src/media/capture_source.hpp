// Live video capture input (M3, EXPERIMENTAL): webcams, USB/PCIe HDMI-SDI
// capture devices that expose a Windows Media Foundation video capture
// source (UVC devices and most capture cards do).
//
// Windows implementation: capture_source_windows.cpp (Media Foundation
// source reader, frames converted to BGRA by the reader's video processor).
// Other platforms: NOT_SUPPORTED.
//
// Timing: capture frames are stamped with their arrival time on the master
// clock. Device timestamps are not yet mapped to the master clock, and audio
// from capture devices is not yet captured (video only).
//
// Disconnection: if the device fails or disappears while running, the input
// holds its last frame (frame sync repeats it), the failure is counted as a
// decode error and logged, and the source retries opening the device every
// second until the session stops.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/status.hpp"
#include "media/media_source.hpp"

namespace mixit {
class Logger;
}

namespace mixit::media {

struct CaptureDeviceInfo {
    std::string id;    // opaque, stable while the device stays connected (symbolic link)
    std::string name;  // friendly name for the operator
};

struct CaptureOptions {
    std::string device_id;
    std::uint32_t width = 0;   // 0 = choose (largest mode up to 1920x1080)
    std::uint32_t height = 0;
    Rate frame_rate{0, 1};     // 0 = choose (highest rate up to 60 fps)
};

Status enumerate_capture_devices(Logger& logger, std::vector<CaptureDeviceInfo>& out);

// Opens the device briefly to choose its mode; capture starts with the session.
// NOT_FOUND: no such device; NOT_SUPPORTED: no matching mode / platform.
Status open_capture_source(const CaptureOptions& options, Logger& logger, std::unique_ptr<MediaSource>& out);

}  // namespace mixit::media
