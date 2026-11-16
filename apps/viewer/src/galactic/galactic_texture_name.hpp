#pragma once

#include "eawr/assets/asset_name_resolver.hpp"

#include <optional>
#include <string>
#include <string_view>

// #6771 row 4 (T10): the one rule the galaxy view uses to turn an authored texture name (planet and family textures,
// the route band texture, the corruption particle textures) into a logical path. It is the shared resolver of
// docs/behaviour/asset-name-resolution.md (ANR-01): the authored name, then, only on a miss, the name cut at its first
// dot plus `.dds`; a `.tga`/`.dds` name also tries the other of the two (the tolerant deviation DEV-A). A miss logs one
// EAWR-ASSET-NAME-0001 line per name. No Godot types, so a plain test can run it.
namespace eawr::viewer::galactic {

inline constexpr std::string_view texture_name_site = "galactic_scene";

[[nodiscard]] inline std::optional<std::string> scene_texture_path(const vfs::Vfs& filesystem, const std::string_view name) {
    std::string path = assets::resolve_texture(filesystem, name, texture_name_site);
    if (path.empty()) return std::nullopt;
    return path;
}

} // namespace eawr::viewer::galactic
