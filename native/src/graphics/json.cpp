#include "graphics/json.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace mixit::graphics {

namespace {

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    Status parse(JsonValue& out) {
        skip_ws();
        if (Status st = value(out, 0); !st.is_ok()) return st;
        skip_ws();
        if (pos_ != t_.size()) return error("unexpected trailing characters");
        return Status::ok();
    }

private:
    Status error(const char* what) const {
        return {MIXIT_E_INVALID_ARGUMENT, std::string("JSON: ") + what + " at offset " + std::to_string(pos_)};
    }
    void skip_ws() noexcept {
        while (pos_ < t_.size() && (t_[pos_] == ' ' || t_[pos_] == '\t' || t_[pos_] == '\n' || t_[pos_] == '\r')) ++pos_;
    }
    bool consume(std::string_view word) noexcept {
        if (t_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    Status value(JsonValue& v, int depth) {
        if (depth > kMaxJsonDepth) return error("nesting too deep");
        if (pos_ >= t_.size()) return error("unexpected end");
        const char c = t_[pos_];
        if (c == '{') return object(v, depth);
        if (c == '[') return array(v, depth);
        if (c == '"') {
            v.kind = JsonValue::Kind::String;
            return string(v.string);
        }
        if (consume("true")) {
            v.kind = JsonValue::Kind::Bool;
            v.boolean = true;
            return Status::ok();
        }
        if (consume("false")) {
            v.kind = JsonValue::Kind::Bool;
            v.boolean = false;
            return Status::ok();
        }
        if (consume("null")) {
            v.kind = JsonValue::Kind::Null;
            return Status::ok();
        }
        if (c == '-' || (c >= '0' && c <= '9')) return number(v);
        return error("unexpected character");
    }

    Status number(JsonValue& v) {
        const std::size_t start = pos_;
        if (t_[pos_] == '-') ++pos_;
        if (pos_ >= t_.size() || t_[pos_] < '0' || t_[pos_] > '9') return error("invalid number");
        if (t_[pos_] == '0') {
            ++pos_;
        } else {
            while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
        }
        if (pos_ < t_.size() && t_[pos_] == '.') {
            ++pos_;
            if (pos_ >= t_.size() || t_[pos_] < '0' || t_[pos_] > '9') return error("invalid number");
            while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
        }
        if (pos_ < t_.size() && (t_[pos_] == 'e' || t_[pos_] == 'E')) {
            ++pos_;
            if (pos_ < t_.size() && (t_[pos_] == '+' || t_[pos_] == '-')) ++pos_;
            if (pos_ >= t_.size() || t_[pos_] < '0' || t_[pos_] > '9') return error("invalid number");
            while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') ++pos_;
        }
        const std::string text(t_.substr(start, pos_ - start));
        char* end = nullptr;
        errno = 0;
        v.number = std::strtod(text.c_str(), &end);  // the grammar above is locale independent
        if (end != text.c_str() + text.size() || errno == ERANGE) return error("number out of range");
        v.kind = JsonValue::Kind::Number;
        return Status::ok();
    }

    static void append_utf8(std::string& out, std::uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(std::uint32_t& out) noexcept {
        if (pos_ + 4 > t_.size()) return false;
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[pos_++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    Status string(std::string& out) {
        ++pos_;  // opening quote
        out.clear();
        while (pos_ < t_.size()) {
            const char c = t_[pos_++];
            if (c == '"') return Status::ok();
            if (static_cast<unsigned char>(c) < 0x20) return error("control character in string");
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= t_.size()) break;
            const char e = t_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint32_t cp = 0;
                if (!hex4(cp)) return error("invalid \\u escape");
                if (cp >= 0xD800 && cp < 0xDC00) {
                    std::uint32_t lo = 0;
                    if (!consume("\\u") || !hex4(lo) || lo < 0xDC00 || lo >= 0xE000) return error("invalid surrogate pair");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return error("unpaired surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default: return error("invalid escape");
            }
        }
        return error("unterminated string");
    }

    Status array(JsonValue& v, int depth) {
        ++pos_;
        v.kind = JsonValue::Kind::Array;
        skip_ws();
        if (pos_ < t_.size() && t_[pos_] == ']') {
            ++pos_;
            return Status::ok();
        }
        for (;;) {
            skip_ws();
            JsonValue item;
            if (Status st = value(item, depth + 1); !st.is_ok()) return st;
            v.array.push_back(std::move(item));
            skip_ws();
            if (pos_ >= t_.size()) return error("unterminated array");
            if (t_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (t_[pos_] == ']') {
                ++pos_;
                return Status::ok();
            }
            return error("expected , or ]");
        }
    }

    Status object(JsonValue& v, int depth) {
        ++pos_;
        v.kind = JsonValue::Kind::Object;
        skip_ws();
        if (pos_ < t_.size() && t_[pos_] == '}') {
            ++pos_;
            return Status::ok();
        }
        for (;;) {
            skip_ws();
            if (pos_ >= t_.size() || t_[pos_] != '"') return error("expected a member name");
            std::string key;
            if (Status st = string(key); !st.is_ok()) return st;
            skip_ws();
            if (pos_ >= t_.size() || t_[pos_] != ':') return error("expected :");
            ++pos_;
            skip_ws();
            JsonValue item;
            if (Status st = value(item, depth + 1); !st.is_ok()) return st;
            v.object[std::move(key)] = std::move(item);
            skip_ws();
            if (pos_ >= t_.size()) return error("unterminated object");
            if (t_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (t_[pos_] == '}') {
                ++pos_;
                return Status::ok();
            }
            return error("expected , or }");
        }
    }

    std::string_view t_;
    std::size_t pos_ = 0;
};

}  // namespace

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (kind != Kind::Object) return nullptr;
    const auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

double JsonValue::number_or(std::string_view key, double fallback) const noexcept {
    const JsonValue* v = find(key);
    return v != nullptr && v->kind == Kind::Number ? v->number : fallback;
}

std::string JsonValue::string_or(std::string_view key, std::string fallback) const {
    const JsonValue* v = find(key);
    return v != nullptr && v->kind == Kind::String ? v->string : fallback;
}

bool JsonValue::bool_or(std::string_view key, bool fallback) const noexcept {
    const JsonValue* v = find(key);
    return v != nullptr && v->kind == Kind::Bool ? v->boolean : fallback;
}

Status parse_json(std::string_view text, JsonValue& out) {
    if (text.size() > kMaxJsonBytes) return {MIXIT_E_INVALID_ARGUMENT, "JSON larger than 1 MiB"};
    out = JsonValue{};
    return Parser(text).parse(out);
}

}  // namespace mixit::graphics
