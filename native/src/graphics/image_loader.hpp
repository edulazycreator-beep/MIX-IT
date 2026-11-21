// Still images (PNG, JPEG, BMP, ...) decoded to BGRA with straight alpha,
// through FFmpeg (M6). Files only; the size is bounded (16384² max).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/status.hpp"

namespace mixit::graphics {

struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> pixels;  // BGRA, width*4 per row
};

Status load_image_bgra(const std::string& path, Image& out);

}  // namespace mixit::graphics
