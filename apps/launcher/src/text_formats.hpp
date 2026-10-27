#pragma once
// The JSON and KeyValues readers moved to src/mods/steam (shared with the mod core); the launcher keeps its names.
#include "eawr/mods/steam/text_formats.hpp"

namespace eawr::launcher {

using mods::steam::json_quote;
using mods::steam::JsonValue;
using mods::steam::parse_json;
using mods::steam::parse_vdf;
using mods::steam::VdfNode;

} // namespace eawr::launcher
