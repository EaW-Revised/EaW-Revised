#pragma once

#include <optional>

namespace eawr::presentation::godot_backend::detail {

// What scene lighting writes onto a material is apply_lighting_params (renderer.cpp): the irradiance matrices, the fill
// matrices, ambient, the sun's diffuse and specular colours, the direction toward the light and the shadow floor. Every
// material takes the current lighting when it is configured, so a set_lighting that repeats those values changes nothing
// on any material and the pass over all of them can be left out. The light itself (shadow distance, atlas, layout, bias)
// is not a material parameter and is always updated. Keep this list equal to the fields apply_lighting_params reads.
template <class State>
[[nodiscard]] constexpr bool same_material_lighting(const State& a, const State& b) {
    return a.sph == b.sph && a.sph_fill == b.sph_fill && a.ambient == b.ambient && a.sun_diffuse == b.sun_diffuse
        && a.toward_light == b.toward_light && a.specular == b.specular && a.shadow_floor == b.shadow_floor;
}

// True when `next` has to be pushed onto every existing material: there was no lighting before (materials hold the
// frozen constants), or a value a material reads differs. A NaN never compares equal, so it always pushes.
template <class State>
[[nodiscard]] constexpr bool materials_need_lighting(const std::optional<State>& current, const State& next) {
    return !current || !same_material_lighting(*current, next);
}

} // namespace eawr::presentation::godot_backend::detail
