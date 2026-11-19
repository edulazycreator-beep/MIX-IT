// Session handle resolution shared by the ABI translation units.
#pragma once

#include <memory>

#include "media/session.hpp"
#include "mixit/mixit_engine.h"

namespace mixit::abi {

// The session behind a handle, or nullptr if the handle is invalid/destroyed.
std::shared_ptr<media::Session> resolve_session(mixit_session handle);

}  // namespace mixit::abi
