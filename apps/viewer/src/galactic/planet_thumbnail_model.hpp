#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/lighting/lighting.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend::galactic_detail {

// PS-08: one planet model decoded for the search pictures with the galaxy
// upload's own material, texture-chain and idle-clip rules, overview surfaces
// only (alternate 0 and untagged, as the map draws a discovered planet away
// from the camera target). Pure CPU work, safe on a worker thread; the GPU
// upload stays on the main thread.
struct PreparedPlanetSurface final {
    assets::Model geometry;
    MaterialDescription material;
    assets::Texture primary;
    std::vector<GodotRenderer::BindingTexture> extra;
    bool effect_clock{};
    std::optional<animation::Player> idle;
};
struct PreparedPlanetModel final {
    std::string path;
    std::string cause; // not empty: nothing of the model can be drawn, and why
    std::vector<PreparedPlanetSurface> surfaces;
    // Asset-space bind-pose bounds of the drawn surfaces (min xyz, max xyz).
    std::optional<std::array<float, 6>> bounds;
};

[[nodiscard]] PreparedPlanetModel prepare_planet_thumbnail_model(const vfs::Vfs& filesystem, const std::string& logical_path,
    const presentation::lighting::Environment& environment);

} // namespace eawr::presentation::godot_backend::galactic_detail
