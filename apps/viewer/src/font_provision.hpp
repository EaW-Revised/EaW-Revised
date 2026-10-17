#pragma once

#include <filesystem>
#include <string>

namespace eawr::presentation::godot_backend {

// #3963: the player's EmpireAtWar fonts, extracted natively from their own corruption/StarWarsG.exe
// into Godot user data (`<user data>/fonts`), so the install folders can be read-only and no Python
// is needed. Does nothing when `--eawr-font-cache` or EAWR_FONT_CACHE already names a cache. A valid
// cache is reused; otherwise it is extracted from `game_root`. On success the process environment's
// EAWR_FONT_CACHE names the cache, which every screen already reads. Returns an empty string, or the
// readable reason the fonts are missing when `required` (a player entry point); other runs only warn
// and keep the checkout's out/fonts fallback.
[[nodiscard]] std::string provision_fonts(const std::filesystem::path& game_root, bool allow_unknown_build,
                                          bool required);

} // namespace eawr::presentation::godot_backend
