#include "graphics/graphics.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "graphics/image_loader.hpp"
#include "graphics/json.hpp"

#if MIXIT_HAVE_GRAPHICS
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

namespace mixit::graphics {

#if MIXIT_HAVE_GRAPHICS
extern const std::uint8_t kFont_regular[];
extern const std::size_t kFont_regular_size;
extern const std::uint8_t kFont_bold[];
extern const std::size_t kFont_bold_size;
#endif

namespace {

constexpr std::size_t kMaxElements = 256;

bool parse_color(const std::string& s, Color& out) {
    if (s.size() != 7 && s.size() != 9) return false;
    if (s[0] != '#') return false;
    std::uint32_t v = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
    }
    const std::uint32_t a = s.size() == 9 ? (v >> 24) & 0xFF : 0xFF;
    out.a = static_cast<float>(a) / 255.0f;
    out.r = static_cast<float>((v >> 16) & 0xFF) / 255.0f;
    out.g = static_cast<float>((v >> 8) & 0xFF) / 255.0f;
    out.b = static_cast<float>(v & 0xFF) / 255.0f;
    return true;
}

void collect_fields(const std::string& text, Scene& scene) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '{') continue;
        if (i + 1 < text.size() && text[i + 1] == '{') {
            ++i;
            continue;
        }
        const std::size_t end = text.find('}', i);
        if (end == std::string::npos) return;
        const std::string name = text.substr(i + 1, end - i - 1);
        if (name == "time") scene.uses_time = true;
        else if (name == "timer") scene.uses_timer = true;
        else if (!name.empty()) scene.fields.insert(name);
        i = end;
    }
}

// Decodes UTF-8 (invalid bytes become U+FFFD).
std::vector<std::uint32_t> decode_utf8(std::string_view s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 0x6 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            len = 2;
        } else if ((c >> 4) == 0xE && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            len = 3;
        } else if ((c >> 3) == 0x1E && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            len = 4;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

// Straight-alpha "over": src colour with coverage-scaled alpha onto dst.
inline void blend_over(std::uint8_t* d, const Color& c, float coverage) noexcept {
    const float sa = c.a * coverage;
    if (sa <= 0.0f) return;
    const float da = static_cast<float>(d[3]) / 255.0f;
    const float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0f) return;
    const float k = da * (1.0f - sa);
    auto ch = [&](float src, std::uint8_t dst) {
        return static_cast<std::uint8_t>(std::lround(std::clamp((src * sa + static_cast<float>(dst) / 255.0f * k) / oa, 0.0f, 1.0f) * 255.0f));
    };
    d[0] = ch(c.b, d[0]);
    d[1] = ch(c.g, d[1]);
    d[2] = ch(c.r, d[2]);
    d[3] = static_cast<std::uint8_t>(std::lround(oa * 255.0f));
}

void draw_rect(media::VideoFrame& f, const Element& e) {
    const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
    const float x0 = e.x * W, y0 = e.y * H, x1 = (e.x + e.w) * W, y1 = (e.y + e.h) * H;
    const float r = std::min({e.radius * H, (x1 - x0) / 2.0f, (y1 - y0) / 2.0f});
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0))), iy0 = std::max(0, static_cast<int>(std::floor(y0)));
    const int ix1 = std::min(static_cast<int>(f.width), static_cast<int>(std::ceil(x1)));
    const int iy1 = std::min(static_cast<int>(f.height), static_cast<int>(std::ceil(y1)));
    for (int y = iy0; y < iy1; ++y) {
        std::uint8_t* row = f.data + static_cast<std::size_t>(y) * f.stride;
        const float py = static_cast<float>(y) + 0.5f;
        for (int x = ix0; x < ix1; ++x) {
            const float px = static_cast<float>(x) + 0.5f;
            // Signed distance to the rounded rectangle; coverage = 0.5 - d.
            const float qx = std::max(std::fabs(px - (x0 + x1) / 2.0f) - ((x1 - x0) / 2.0f - r), 0.0f);
            const float qy = std::max(std::fabs(py - (y0 + y1) / 2.0f) - ((y1 - y0) / 2.0f - r), 0.0f);
            const float outside = std::sqrt(qx * qx + qy * qy) - r;
            const float inside = std::min(std::max(std::fabs(px - (x0 + x1) / 2.0f) - (x1 - x0) / 2.0f,
                                                   std::fabs(py - (y0 + y1) / 2.0f) - (y1 - y0) / 2.0f), 0.0f);
            const float d = (qx > 0.0f || qy > 0.0f) ? outside : inside;
            const float cov = std::clamp(0.5f - d, 0.0f, 1.0f);
            if (cov > 0.0f) blend_over(row + static_cast<std::size_t>(x) * 4, e.color, cov);
        }
    }
}

}  // namespace

Status parse_scene(std::string_view json, Scene& out) {
    out = Scene{};
    JsonValue root;
    if (Status st = parse_json(json, root); !st.is_ok()) return st;
    const JsonValue* elements = root.find("elements");
    if (elements == nullptr || !elements->is_array()) return {MIXIT_E_INVALID_ARGUMENT, "scene: \"elements\" array missing"};
    if (elements->array.size() > kMaxElements) return {MIXIT_E_INVALID_ARGUMENT, "scene: at most 256 elements"};
    for (std::size_t i = 0; i < elements->array.size(); ++i) {
        const JsonValue& v = elements->array[i];
        const std::string where = "scene element " + std::to_string(i) + ": ";
        if (!v.is_object()) return {MIXIT_E_INVALID_ARGUMENT, where + "not an object"};
        Element e;
        const std::string type = v.string_or("type", "");
        if (type == "rect") e.type = ElementType::Rect;
        else if (type == "text") e.type = ElementType::Text;
        else if (type == "image") e.type = ElementType::Image;
        else return {MIXIT_E_INVALID_ARGUMENT, where + "type must be rect, text or image"};
        e.x = static_cast<float>(v.number_or("x", 0));
        e.y = static_cast<float>(v.number_or("y", 0));
        e.w = static_cast<float>(v.number_or("w", 1));
        e.h = static_cast<float>(v.number_or("h", 1));
        if (!(e.w > 0 && e.h > 0 && e.x >= -1 && e.y >= -1 && e.x + e.w <= 2 && e.y + e.h <= 2)) {
            return {MIXIT_E_INVALID_ARGUMENT, where + "rectangle out of range"};
        }
        const std::string color = v.string_or("color", type == "rect" ? "#80000000" : "#FFFFFF");
        if (!parse_color(color, e.color)) return {MIXIT_E_INVALID_ARGUMENT, where + "color must be #RRGGBB or #AARRGGBB"};
        e.radius = static_cast<float>(v.number_or("radius", 0));
        if (e.radius < 0 || e.radius > 1) return {MIXIT_E_INVALID_ARGUMENT, where + "radius must be 0..1"};
        if (e.type == ElementType::Text) {
            e.text = v.string_or("text", "");
            e.size = static_cast<float>(v.number_or("size", 0.05));
            if (!(e.size > 0.002f && e.size <= 1.0f)) return {MIXIT_E_INVALID_ARGUMENT, where + "size must be 0.002..1"};
            e.bold = v.bool_or("bold", false);
            const std::string align = v.string_or("align", "left");
            if (align == "left") e.align = HAlign::Left;
            else if (align == "center") e.align = HAlign::Center;
            else if (align == "right") e.align = HAlign::Right;
            else return {MIXIT_E_INVALID_ARGUMENT, where + "align must be left, center or right"};
            const std::string valign = v.string_or("valign", "middle");
            if (valign == "top") e.valign = VAlign::Top;
            else if (valign == "middle") e.valign = VAlign::Middle;
            else if (valign == "bottom") e.valign = VAlign::Bottom;
            else return {MIXIT_E_INVALID_ARGUMENT, where + "valign must be top, middle or bottom"};
            e.shadow = v.bool_or("shadow", false);
            e.fit = v.bool_or("fit", true);
            e.font = v.string_or("font", "");
            collect_fields(e.text, out);
        }
        if (e.type == ElementType::Image) {
            e.path = v.string_or("path", "");
            if (e.path.empty()) return {MIXIT_E_INVALID_ARGUMENT, where + "image needs a path"};
            const std::string fit = v.string_or("fit", "contain");
            if (fit != "contain" && fit != "stretch") return {MIXIT_E_INVALID_ARGUMENT, where + "fit must be contain or stretch"};
            e.contain = fit == "contain";
        }
        out.elements.push_back(std::move(e));
    }
    return Status::ok();
}

std::string expand(std::string_view text, const FieldMap& fields, const DynamicText& dynamic) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c != '{') {
            out += c;
            continue;
        }
        if (i + 1 < text.size() && text[i + 1] == '{') {
            out += '{';
            ++i;
            continue;
        }
        const std::size_t end = text.find('}', i);
        if (end == std::string_view::npos) {
            out.append(text.substr(i));
            break;
        }
        const std::string_view name = text.substr(i + 1, end - i - 1);
        if (name == "time") {
            out += dynamic.time;
        } else if (name == "timer") {
            out += dynamic.timer;
        } else if (const auto it = fields.find(name); it != fields.end()) {
            out += it->second;
        }
        i = end;
    }
    return out;
}

std::string format_timer(std::int64_t seconds) {
    seconds = std::max<std::int64_t>(0, seconds);
    char buf[32];
    if (seconds >= 3600) {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", static_cast<long long>(seconds / 3600),
                      static_cast<long long>((seconds / 60) % 60), static_cast<long long>(seconds % 60));
    } else {
        std::snprintf(buf, sizeof(buf), "%lld:%02lld", static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
    }
    return buf;
}

#if MIXIT_HAVE_GRAPHICS

struct Renderer::Impl {
    FT_Library lib = nullptr;
    FT_Face regular = nullptr;
    FT_Face bold = nullptr;
    std::unordered_map<std::string, FT_Face> files;     // font files, opened once
    std::unordered_map<std::string, Image> images;      // decoded once per path
    std::unordered_map<std::string, std::string> image_errors;

    ~Impl() {
        for (auto& [_, f] : files) FT_Done_Face(f);
        if (regular != nullptr) FT_Done_Face(regular);
        if (bold != nullptr) FT_Done_Face(bold);
        if (lib != nullptr) FT_Done_FreeType(lib);
    }

    FT_Face face_for(const Element& e, std::string& warning) {
        if (!e.font.empty()) {
            if (const auto it = files.find(e.font); it != files.end()) return it->second;
            FT_Face f = nullptr;
            if (FT_New_Face(lib, e.font.c_str(), 0, &f) == 0) {
                files.emplace(e.font, f);
                return f;
            }
            warning = "font not readable: " + e.font + " (using the built-in font)";
        }
        return e.bold ? bold : regular;
    }

    struct Line {
        std::vector<std::uint32_t> cps;
        int width = 0;  // pixels (26.6 rounded)
    };

    static int line_width(FT_Face face, const std::vector<std::uint32_t>& cps) {
        FT_Pos pen = 0;
        FT_UInt prev = 0;
        for (std::uint32_t cp : cps) {
            const FT_UInt gi = FT_Get_Char_Index(face, cp);
            if (prev != 0 && gi != 0 && FT_HAS_KERNING(face)) {
                FT_Vector k{};
                if (FT_Get_Kerning(face, prev, gi, FT_KERNING_DEFAULT, &k) == 0) pen += k.x;
            }
            if (FT_Load_Glyph(face, gi, FT_LOAD_DEFAULT) == 0) pen += face->glyph->advance.x;
            prev = gi;
        }
        return static_cast<int>((pen + 32) >> 6);
    }

    void draw_line(media::VideoFrame& f, FT_Face face, const std::vector<std::uint32_t>& cps, int x, int baseline,
                   const Color& color) {
        FT_Pos pen = static_cast<FT_Pos>(x) << 6;
        FT_UInt prev = 0;
        for (std::uint32_t cp : cps) {
            const FT_UInt gi = FT_Get_Char_Index(face, cp);
            if (prev != 0 && gi != 0 && FT_HAS_KERNING(face)) {
                FT_Vector k{};
                if (FT_Get_Kerning(face, prev, gi, FT_KERNING_DEFAULT, &k) == 0) pen += k.x;
            }
            prev = gi;
            if (FT_Load_Glyph(face, gi, FT_LOAD_RENDER) != 0) continue;
            const FT_GlyphSlot g = face->glyph;
            const FT_Bitmap& bm = g->bitmap;
            const int gx = static_cast<int>((pen + 32) >> 6) + g->bitmap_left;
            const int gy = baseline - g->bitmap_top;
            for (unsigned row = 0; row < bm.rows; ++row) {
                const int y = gy + static_cast<int>(row);
                if (y < 0 || y >= static_cast<int>(f.height)) continue;
                const unsigned char* src = bm.buffer + static_cast<std::ptrdiff_t>(row) * bm.pitch;
                std::uint8_t* dst = f.data + static_cast<std::size_t>(y) * f.stride;
                for (unsigned col = 0; col < bm.width; ++col) {
                    const int px = gx + static_cast<int>(col);
                    if (px < 0 || px >= static_cast<int>(f.width) || src[col] == 0) continue;
                    blend_over(dst + static_cast<std::size_t>(px) * 4, color, static_cast<float>(src[col]) / 255.0f);
                }
            }
            pen += g->advance.x;
        }
    }

    void draw_text(media::VideoFrame& f, const Element& e, const std::string& text, std::vector<std::string>& warnings) {
        std::string warning;
        FT_Face face = face_for(e, warning);
        if (!warning.empty()) warnings.push_back(warning);
        const float H = static_cast<float>(f.height), W = static_cast<float>(f.width);
        const int box_x = static_cast<int>(std::lround(e.x * W)), box_y = static_cast<int>(std::lround(e.y * H));
        const int box_w = static_cast<int>(std::lround(e.w * W)), box_h = static_cast<int>(std::lround(e.h * H));
        int px = std::max(4, static_cast<int>(std::lround(e.size * H)));
        std::vector<Line> lines;
        std::size_t start = 0;
        for (;;) {
            const std::size_t nl = text.find('\n', start);
            lines.push_back(Line{decode_utf8(std::string_view(text).substr(start, nl == std::string::npos ? std::string::npos : nl - start)), 0});
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
        auto layout = [&](int size) {
            FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(size));
            int widest = 0;
            for (Line& l : lines) {
                l.width = line_width(face, l.cps);
                widest = std::max(widest, l.width);
            }
            return widest;
        };
        int widest = layout(px);
        if (e.fit && widest > box_w && widest > 0) {
            px = std::max(4, px * box_w / widest);
            widest = layout(px);
        }
        const int line_h = static_cast<int>((face->size->metrics.height + 32) >> 6);
        const int ascender = static_cast<int>((face->size->metrics.ascender + 32) >> 6);
        const int block_h = line_h * static_cast<int>(lines.size());
        int top = box_y;
        if (e.valign == VAlign::Middle) top = box_y + (box_h - block_h) / 2;
        if (e.valign == VAlign::Bottom) top = box_y + box_h - block_h;
        const Color shadow{0, 0, 0, 0.6f * e.color.a};
        const int offset = std::max(1, px / 16);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            int x = box_x;
            if (e.align == HAlign::Center) x = box_x + (box_w - lines[i].width) / 2;
            if (e.align == HAlign::Right) x = box_x + box_w - lines[i].width;
            const int baseline = top + ascender + static_cast<int>(i) * line_h;
            if (e.shadow) draw_line(f, face, lines[i].cps, x + offset, baseline + offset, shadow);
            draw_line(f, face, lines[i].cps, x, baseline, e.color);
        }
    }

    void draw_image(media::VideoFrame& f, const Element& e, std::vector<std::string>& warnings) {
        auto it = images.find(e.path);
        if (it == images.end()) {
            if (const auto err = image_errors.find(e.path); err != image_errors.end()) {
                warnings.push_back(err->second);
                return;
            }
            Image img;
            const Status st = load_image_bgra(e.path, img);
            if (!st.is_ok()) {
                image_errors.emplace(e.path, "image " + e.path + ": " + st.message());
                warnings.push_back(image_errors[e.path]);
                return;
            }
            it = images.emplace(e.path, std::move(img)).first;
        }
        const Image& img = it->second;
        if (img.width == 0 || img.height == 0) return;
        const float W = static_cast<float>(f.width), H = static_cast<float>(f.height);
        float dx = e.x * W, dy = e.y * H, dw = e.w * W, dh = e.h * H;
        if (e.contain) {
            const float s = std::min(dw / static_cast<float>(img.width), dh / static_cast<float>(img.height));
            const float nw = static_cast<float>(img.width) * s, nh = static_cast<float>(img.height) * s;
            dx += (dw - nw) / 2;
            dy += (dh - nh) / 2;
            dw = nw;
            dh = nh;
        }
        const int x0 = std::max(0, static_cast<int>(std::floor(dx))), y0 = std::max(0, static_cast<int>(std::floor(dy)));
        const int x1 = std::min(static_cast<int>(f.width), static_cast<int>(std::ceil(dx + dw)));
        const int y1 = std::min(static_cast<int>(f.height), static_cast<int>(std::ceil(dy + dh)));
        const float sx = static_cast<float>(img.width) / dw, sy = static_cast<float>(img.height) / dh;
        for (int y = y0; y < y1; ++y) {
            const float v = std::clamp((static_cast<float>(y) + 0.5f - dy) * sy - 0.5f, 0.0f, static_cast<float>(img.height - 1));
            const int v0 = static_cast<int>(v), v1 = std::min(v0 + 1, static_cast<int>(img.height) - 1);
            const float fy = v - static_cast<float>(v0);
            std::uint8_t* row = f.data + static_cast<std::size_t>(y) * f.stride;
            for (int x = x0; x < x1; ++x) {
                const float u = std::clamp((static_cast<float>(x) + 0.5f - dx) * sx - 0.5f, 0.0f, static_cast<float>(img.width - 1));
                const int u0 = static_cast<int>(u), u1 = std::min(u0 + 1, static_cast<int>(img.width) - 1);
                const float fx = u - static_cast<float>(u0);
                float px[4];
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int uu, int vv) {
                        return static_cast<float>(img.pixels[(static_cast<std::size_t>(vv) * img.width + static_cast<std::size_t>(uu)) * 4 + static_cast<std::size_t>(c)]);
                    };
                    px[c] = (at(u0, v0) * (1 - fx) + at(u1, v0) * fx) * (1 - fy) + (at(u0, v1) * (1 - fx) + at(u1, v1) * fx) * fy;
                }
                const Color col{px[2] / 255.0f, px[1] / 255.0f, px[0] / 255.0f, px[3] / 255.0f * e.color.a};
                blend_over(row + static_cast<std::size_t>(x) * 4, col, 1.0f);
            }
        }
    }
};

Renderer::Renderer() : impl_(std::make_unique<Impl>()) {}
Renderer::~Renderer() = default;

Status Renderer::create(std::unique_ptr<Renderer>& out) {
    std::unique_ptr<Renderer> r(new Renderer());
    Impl& d = *r->impl_;
    if (FT_Init_FreeType(&d.lib) != 0) return {MIXIT_E_INTERNAL, "FreeType initialisation failed"};
    if (FT_New_Memory_Face(d.lib, kFont_regular, static_cast<FT_Long>(kFont_regular_size), 0, &d.regular) != 0 ||
        FT_New_Memory_Face(d.lib, kFont_bold, static_cast<FT_Long>(kFont_bold_size), 0, &d.bold) != 0) {
        return {MIXIT_E_INTERNAL, "embedded font could not be loaded"};
    }
    out = std::move(r);
    return Status::ok();
}

Status Renderer::render(const Scene& scene, const FieldMap& fields, const DynamicText& dynamic, media::VideoFrame& dst,
                        std::vector<std::string>& warnings) {
    for (std::uint32_t y = 0; y < dst.height; ++y) std::memset(dst.data + static_cast<std::size_t>(y) * dst.stride, 0, static_cast<std::size_t>(dst.width) * 4);
    for (const Element& e : scene.elements) {
        switch (e.type) {
        case ElementType::Rect: draw_rect(dst, e); break;
        case ElementType::Text: impl_->draw_text(dst, e, expand(e.text, fields, dynamic), warnings); break;
        case ElementType::Image: impl_->draw_image(dst, e, warnings); break;
        }
    }
    return Status::ok();
}

int Renderer::measure(std::string_view utf8, int pixel_size, bool bold) {
    FT_Face face = bold ? impl_->bold : impl_->regular;
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixel_size));
    return Impl::line_width(face, decode_utf8(utf8));
}

#else  // built without FreeType

struct Renderer::Impl {};
Renderer::Renderer() = default;
Renderer::~Renderer() = default;
Status Renderer::create(std::unique_ptr<Renderer>&) {
    return {MIXIT_E_NOT_SUPPORTED, "this engine was built without the graphics renderer"};
}
Status Renderer::render(const Scene&, const FieldMap&, const DynamicText&, media::VideoFrame&, std::vector<std::string>&) {
    return {MIXIT_E_NOT_SUPPORTED, "no graphics renderer"};
}
int Renderer::measure(std::string_view, int, bool) { return 0; }

#endif

}  // namespace mixit::graphics
