#pragma once

#include <string>

namespace eawr::presentation::godot_backend {

// The read-ahead reader count rule (texture and model read-ahead alike): more than one reader only when every volume the game
// data comes from (the game folder and, when a mod is loaded, the mod folder) reported no seek penalty. Paths are UTF-8; an
// empty mod root means no mod.
[[nodiscard]] bool game_data_volumes_solid_state(const std::string& game_root, const std::string& mod_root);

} // namespace eawr::presentation::godot_backend
