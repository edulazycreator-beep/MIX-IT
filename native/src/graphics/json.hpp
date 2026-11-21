// Minimal JSON reader for graphics scenes (M6). Strict RFC 8259 subset:
// objects, arrays, strings (with \uXXXX incl. surrogate pairs), numbers,
// true/false/null. Depth and size are bounded; errors carry the offset.
#pragma once

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"

namespace mixit::graphics {

class JsonValue {
public:
    enum class Kind { Null, Bool, Number, String, Array, Object };

    Kind kind = Kind::Null;
    bool boolean = false;
    double number = 0.0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue, std::less<>> object;

    [[nodiscard]] bool is_object() const noexcept { return kind == Kind::Object; }
    [[nodiscard]] bool is_array() const noexcept { return kind == Kind::Array; }
    // Member lookup; null when absent or not an object.
    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    [[nodiscard]] double number_or(std::string_view key, double fallback) const noexcept;
    [[nodiscard]] std::string string_or(std::string_view key, std::string fallback) const;
    [[nodiscard]] bool bool_or(std::string_view key, bool fallback) const noexcept;
};

inline constexpr std::size_t kMaxJsonBytes = 1u << 20;  // 1 MiB scene description
inline constexpr int kMaxJsonDepth = 32;

Status parse_json(std::string_view text, JsonValue& out);

}  // namespace mixit::graphics
