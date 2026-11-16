#pragma once

#include "eawr/assets/asset_name_resolver.hpp"
#include "eawr/vfs/vfs.hpp"

#include <string>
#include <string_view>

// The two places the loading screens turn an authored picture or texture name into a logical path. Both go through the
// shared resolver (docs/behaviour/asset-name-resolution.md, ANR-01); a name that is not in the data logs one
// EAWR-ASSET-NAME-0001 line there. No Godot types, so the tests link this header without the engine.
namespace eawr::presentation::godot_backend {

inline constexpr std::string_view loading_picture_site = "loading screen picture";
inline constexpr std::string_view loading_planet_site = "loading planet texture";

// A loading-screen picture: a name the data authors (`load_weather_sun.tga`) or a rooted path the screen model built
// (`data/art/textures/<map>.dds`). Empty when the data has no such file.
[[nodiscard]] inline std::string loading_picture_path(const vfs::Vfs& filesystem, const std::string_view name) {
    return assets::resolve_texture(filesystem, name, loading_picture_site);
}

// A texture the loading planet's model declares. Empty when the data has no such file.
[[nodiscard]] inline std::string loading_planet_texture_path(const vfs::Vfs& filesystem, const std::string_view name) {
    return assets::resolve_texture(filesystem, name, loading_planet_site);
}

} // namespace eawr::presentation::godot_backend
