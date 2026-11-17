// Minimal JSON helpers for the structured (JSON-lines) log sink.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mixit::json {

// Appends `text` to `out` as JSON string content (without quotes), escaping
// quotes, backslashes and control characters. Bytes >= 0x80 are copied
// verbatim (the engine only produces valid UTF-8).
void append_escaped(std::string& out, std::string_view text);

// Appends an ISO-8601 UTC timestamp with millisecond precision,
// e.g. 2026-11-17T04:05:55.947Z
void append_iso8601_utc(std::string& out, std::int64_t unix_time_ns);

}  // namespace mixit::json
