// Graphics (M6): titles, lower thirds, clocks, scoreboards — described as a
// JSON scene of elements and rendered on the CPU into a BGRA frame with
// straight alpha (transparent background), so it can be placed on a layer or
// an overlay channel.
//
// Scene format (all coordinates normalized to the frame, 0..1):
//   {"elements": [
//     {"type": "rect",  "x":0.05,"y":0.78,"w":0.5,"h":0.12, "color":"#D0101820", "radius":0.01},
//     {"type": "text",  "x":0.07,"y":0.79,"w":0.46,"h":0.06, "text":"{name}",
//      "size":0.05, "color":"#FFFFFF", "bold":true, "align":"left|center|right",
//      "valign":"top|middle|bottom", "shadow":true, "fit":true, "font":"C:/Windows/Fonts/arial.ttf"},
//     {"type": "image", "x":0.85,"y":0.05,"w":0.1,"h":0.1, "path":"C:/logo.png", "fit":"contain|stretch"}
//   ]}
// Colours: "#RRGGBB" or "#AARRGGBB". Text size: fraction of the frame
// height. "{field}" placeholders are replaced by field values set at run
// time; "{time}" is the local time (HH:MM:SS) and "{timer}" the source's
// countdown/stopwatch (M:SS, H:MM:SS from one hour).
//
// Text: FreeType with the embedded DejaVu Sans / Bold (or a font file),
// grayscale anti-aliasing, kerning, explicit line breaks; no complex-script
// shaping (Arabic, Indic, ...) yet.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"
#include "media/media_types.hpp"

namespace mixit::graphics {

struct Color {
    float r = 1, g = 1, b = 1, a = 1;  // 0..1, straight alpha
};

enum class ElementType { Rect, Text, Image };
enum class HAlign { Left, Center, Right };
enum class VAlign { Top, Middle, Bottom };

struct Element {
    ElementType type = ElementType::Rect;
    float x = 0, y = 0, w = 0, h = 0;
    Color color;
    float radius = 0;      // rect: corner radius (fraction of frame height)
    std::string text;      // text: with {placeholders}
    float size = 0.05f;    // text: fraction of frame height
    bool bold = false;
    HAlign align = HAlign::Left;
    VAlign valign = VAlign::Middle;
    bool shadow = false;
    bool fit = true;       // text: shrink to the box width if too long
    std::string font;      // text: font file (empty = embedded DejaVu)
    std::string path;      // image file
    bool contain = true;   // image: keep aspect ratio inside the box
};

struct Scene {
    std::vector<Element> elements;
    bool uses_time = false;   // {time}
    bool uses_timer = false;  // {timer}
    std::set<std::string> fields;  // other placeholders, for the operator UI
};

// Parses and validates a scene description (size, element count ≤ 256,
// coordinates within -1..2, known types/values).
Status parse_scene(std::string_view json, Scene& out);

using FieldMap = std::map<std::string, std::string, std::less<>>;

struct DynamicText {
    std::string time;   // "HH:MM:SS"
    std::string timer;  // "M:SS"
};

// Replaces {name} placeholders. Unknown fields become empty; "{{" is a
// literal "{".
std::string expand(std::string_view text, const FieldMap& fields, const DynamicText& dynamic);

// Formats a timer value (seconds, ≥ 0): "M:SS", or "H:MM:SS" from 1 hour.
std::string format_timer(std::int64_t seconds);

// Renders scenes. Not thread-safe: one per rendering thread (FreeType).
class Renderer {
public:
    static Status create(std::unique_ptr<Renderer>& out);
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Clears `dst` to transparent and draws the scene. Element errors (a
    // missing font or image) are reported in `warnings`, the rest is drawn.
    Status render(const Scene& scene, const FieldMap& fields, const DynamicText& dynamic, media::VideoFrame& dst,
                  std::vector<std::string>& warnings);

    // Width in pixels of one line of text at `pixel_size` (tests, layout).
    int measure(std::string_view utf8, int pixel_size, bool bold);

private:
    Renderer();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mixit::graphics
