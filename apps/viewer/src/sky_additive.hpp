#pragma once

// The MeshAdditive sky policy, shared by every route that draws a MeshAdditive
// surface as a sky layer: the space environment (space_environment_*.cpp) and
// the land sky (land_look.cpp, #6811). One shader text, one set of declared
// inputs and bindings, one program choice per classified scene route; the
// callers add only their own placement (instance transform, or the land dome's
// camera-centred vertex stage).
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include "eawr/presentation/space/space.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend::sky_additive {

// Clean-room modern_spatial adapter for eawr-space-sky-meshadditive-t0-v1: the
// MeshAdditive t0/t0_p0 arithmetic of docs/behaviour/meshadditive-sun-billboard.md
// A-02..A-10 for a non-billboard surface, fed with the surface's authored Color
// and UVScrollRate and the declared TIME and LIGHT_SCALE inputs under names no
// engine clock or scene light writes (Godot's TIME is not read). Declared
// state policy space-sky-meshadditive-state-v1: ONE/ONE additive rgb
// (blend_add with fragment alpha kept at 1), no depth write, the engine depth
// test, fog disabled, the sky preview's cull_disabled. Texels are sampled
// without sRGB decode and the product is written as a stored value (the
// stored-value output of docs/rendering.md; the Compatibility fallback
// compensates its ALBEDO round trip), so the arithmetic and the blend act on
// stored values.
constexpr std::string_view meshadditive_sky_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never, blend_add;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Color;
uniform vec4 UVScrollRate;
uniform float eawr_sky_time;
uniform vec4 eawr_sky_light_scale;
varying vec2 eawr_scrolled_uv;
varying vec3 eawr_vertex_color;

void vertex() {
    // Unwrapped; the sampler's repeat addressing wraps it per sample.
    eawr_scrolled_uv = UV + eawr_sky_time * UVScrollRate.xy;
    eawr_vertex_color = clamp(Color.rgb * eawr_sky_light_scale.rgb * eawr_sky_light_scale.a, 0.0, 1.0);
}

// ALBEDO under the stored-value output (docs/rendering.md, colour policy);
// the Compatibility fallback compiles compatibility_albedo_writer instead.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void fragment() {
    vec3 stored_rgb = eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

// CEM-03: double the sampled product before the legacy fragment saturation.
// Keep the stock adapter byte-identical for its existing pixel controls.
[[nodiscard]] inline const std::string& meshadditive2x_sky_shader() {
    static const std::string source = [] {
        std::string text(meshadditive_sky_shader);
        constexpr std::string_view from = "eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb";
        constexpr std::string_view to = "clamp(2.0 * eawr_vertex_color * texture(BaseTexture, eawr_scrolled_uv).rgb, 0.0, 1.0)";
        const std::size_t at = text.find(from);
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    }();
    return source;
}

// MeshAdditiveVColor.fx: the MeshAdditive adapter with the authored vertex
// colour as a further factor (the effect has no Color field; Color is bound
// to (1, 1, 1, 1)). The same stored-value policy as the MeshAdditive route.
[[nodiscard]] inline const std::string& meshadditive_vcolor_sky_shader() {
    static const std::string source = [] {
        std::string text(meshadditive_sky_shader);
        constexpr std::string_view from = "clamp(Color.rgb * eawr_sky_light_scale.rgb";
        constexpr std::string_view to = "clamp(COLOR.rgb * Color.rgb * eawr_sky_light_scale.rgb";
        const std::size_t at = text.find(from);
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    }();
    return source;
}

// The program of a MeshAdditive-family scene route (meshadditive,
// meshadditive2x, meshadditive_vcolor); empty for any other route.
[[nodiscard]] inline std::string additive_sky_program(const space::SceneRoute route) {
    switch (route) {
    case space::SceneRoute::meshadditive: return std::string(meshadditive_sky_shader);
    case space::SceneRoute::meshadditive2x: return meshadditive2x_sky_shader();
    case space::SceneRoute::meshadditive_vcolor: return meshadditive_vcolor_sky_shader();
    default: return {};
    }
}

[[nodiscard]] inline bool is_additive_sky_route(const space::SceneRoute route) noexcept {
    return route == space::SceneRoute::meshadditive || route == space::SceneRoute::meshadditive2x
        || route == space::SceneRoute::meshadditive_vcolor;
}

// The material bindings of the program: the surface's authored Color and
// UVScrollRate plus the declared TIME and LIGHT_SCALE inputs. The base texture
// is the uploaded asset's own texture (any binding value that is not
// "shared:<name>").
[[nodiscard]] inline std::vector<MaterialBinding> meshadditive_bindings(
    const space::MeshAdditiveMaterial& material, const space::MeshAdditiveInputs& inputs) {
    return {
        {"BaseTexture", std::string{"space-sky-base-texture"}},
        {"Color", material.color},
        {"UVScrollRate", material.uv_scroll_rate},
        {"eawr_sky_time", inputs.time},
        {"eawr_sky_light_scale", inputs.light_scale},
    };
}

} // namespace eawr::presentation::godot_backend::sky_additive
