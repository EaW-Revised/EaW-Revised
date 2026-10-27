#pragma once
// The path helpers moved to src/mods/steam (shared with the mod core); the launcher keeps its names.
#include "eawr/mods/steam/paths.hpp"

namespace eawr::launcher {

namespace fs = std::filesystem;

using mods::steam::find_child_ci;
using mods::steam::lower_ascii;
using mods::steam::path_from_utf8;
using mods::steam::path_to_utf8;
using mods::steam::same_location;
using mods::steam::widen_ascii;

} // namespace eawr::launcher
