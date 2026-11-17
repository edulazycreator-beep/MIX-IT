#include "logging/json.hpp"

#include <chrono>
#include <cstdio>

namespace mixit::json {

void append_escaped(std::string& out, std::string_view text) {
    static constexpr char kHex[] = "0123456789abcdef";
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    out += "\\u00";
                    out += kHex[c >> 4];
                    out += kHex[c & 0xF];
                } else {
                    out += ch;
                }
        }
    }
}

void append_iso8601_utc(std::string& out, std::int64_t unix_time_ns) {
    using namespace std::chrono;
    const sys_time<nanoseconds> tp{nanoseconds{unix_time_ns}};
    const auto day = floor<days>(tp);
    const year_month_day ymd{day};
    const hh_mm_ss<milliseconds> tod{floor<milliseconds>(tp - day)};

    char buf[40];
    const int n = std::snprintf(buf, sizeof(buf), "%04d-%02u-%02uT%02d:%02d:%02d.%03dZ",
                                static_cast<int>(ymd.year()), static_cast<unsigned>(ymd.month()),
                                static_cast<unsigned>(ymd.day()), static_cast<int>(tod.hours().count()),
                                static_cast<int>(tod.minutes().count()),
                                static_cast<int>(tod.seconds().count()),
                                static_cast<int>(tod.subseconds().count()));
    if (n > 0) out.append(buf, static_cast<std::size_t>(n));
}

}  // namespace mixit::json
