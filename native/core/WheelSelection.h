#pragma once

#include "core/ClipboardEntry.h"
#include "core/Settings.h"

#include <variant>

namespace smk::core {

using WheelSelection = std::variant<std::monostate, ClipboardEntry, ExtendedWheelActionSlot>;

} // namespace smk::core
