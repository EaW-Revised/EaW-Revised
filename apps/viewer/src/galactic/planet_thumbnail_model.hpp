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
// upload's own material, texture-chain and idle-clip rules, with the requested
// close-up, overview or hidden-shell surfaces. Pure CPU work, safe on a worker thread; the GPU
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

// `alternate` is the surface alternate the map draws for the object (WGUI-05: 2 for an undiscovered planet's
// grid shell, 1 for the pictured system's close-up, 0 for decorations); untagged surfaces always draw.
[[nodiscard]] PreparedPlanetModel prepare_planet_thumbnail_model(const vfs::Vfs& filesystem, const std::string& logical_path,
    int alternate, const presentation::lighting::Environment& environment);

} // namespace eawr::presentation::godot_backend::galactic_detail
