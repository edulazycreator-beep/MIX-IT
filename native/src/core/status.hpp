// Status: explicit, exception-free error value used by engine internals.
// The ABI layer converts a failed Status into a mixit_result plus the
// calling thread's last-error message.
#pragma once

#include <string>
#include <utility>

#include "mixit/mixit_engine.h"

namespace mixit {

class [[nodiscard]] Status {
public:
    Status() = default;  // OK
    Status(mixit_result code, std::string message) : code_(code), message_(std::move(message)) {}

    static Status ok() { return {}; }

    [[nodiscard]] bool is_ok() const noexcept { return code_ == MIXIT_OK; }
    [[nodiscard]] mixit_result code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    mixit_result code_ = MIXIT_OK;
    std::string message_;
};

}  // namespace mixit
