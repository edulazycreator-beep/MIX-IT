#include "core/last_error.hpp"

#include <cstring>

namespace mixit {
namespace {

struct LastError {
    char text[kLastErrorCapacity] = {};
    std::size_t length = 0;
};

thread_local LastError t_last_error;

}  // namespace

std::size_t copy_utf8_truncated(char* dest, std::size_t capacity, std::string_view text) noexcept {
    if (dest == nullptr || capacity == 0) {
        return 0;
    }
    std::size_t n = text.size();
    if (n > capacity - 1) {
        n = capacity - 1;
        // Back up so we never cut a multi-byte sequence: continuation bytes
        // look like 10xxxxxx, so step back until text[n] starts a character.
        while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0u) == 0x80u) {
            --n;
        }
    }
    if (n > 0) {
        std::memcpy(dest, text.data(), n);
    }
    dest[n] = '\0';
    return n;
}

void set_last_error(std::string_view function, std::string_view message) noexcept {
    LastError& e = t_last_error;
    std::size_t used = copy_utf8_truncated(e.text, sizeof(e.text), function);
    if (used + 2 < sizeof(e.text)) {
        e.text[used++] = ':';
        e.text[used++] = ' ';
        used += copy_utf8_truncated(e.text + used, sizeof(e.text) - used, message);
    }
    e.length = used;
}

mixit_result copy_last_error(char* buffer, std::uint32_t capacity, std::uint32_t* out_required) noexcept {
    const LastError& e = t_last_error;
    const auto required = static_cast<std::uint32_t>(e.length + 1);
    if (out_required != nullptr) {
        *out_required = required;
    }
    if (buffer == nullptr && capacity != 0) {
        return MIXIT_E_INVALID_ARGUMENT;
    }
    if (capacity == 0) {
        return MIXIT_E_BUFFER_TOO_SMALL;  // pure size query
    }
    copy_utf8_truncated(buffer, capacity, std::string_view(e.text, e.length));
    return capacity >= required ? MIXIT_OK : MIXIT_E_BUFFER_TOO_SMALL;
}

}  // namespace mixit
