#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Grass.fx sph_t0/sph_t0_p0 for placed ground cover (#32), from the
// clean-room note docs/behaviour/vegetation-effects.md G-01..G-09. Vertex
// stage: the normal is the object's +Z (source up) through the world matrix;
// wave a = 0.5 + 0.5 x sin(6.28 x frac(ts x TIME + (x + y) / 20)) over the
// source-basis mesh position, with w = BendScale x wind speed / 10 and ts =
// 0.125 + 0.875 x w; the world position moves by (1 - v) x (10w + (10 +
// 10.5w) x a) along the unit wind direction (#147, G-05, W-06/W-07); colour D
// = saturate(irradiance x lerp(Diffuse.rgb, Diffuse1.rgb, a) x
// LIGHT_SCALE.rgb + Emissive), alpha Diffuse.a x LIGHT_SCALE.a x distance
// fade. Pixel stage: RGB = 2 x texel.rgb x fog-of-war.rgb x D.rgb, alpha =
// texel.a x D.a, alpha test GREATER 8/255, SRCALPHA/INVSRCALPHA blending with
// depth writes and LESSEQUAL, transparent render pass. The renderer binds the
// scene wind and clock (eawr_wind, eawr_scene_time) and the populated map the
// surface's mesh frame (eawr_mesh_x/y: rows taking the render-basis model
// VERTEX to source-basis mesh space); unbound, there is no wind and TIME is
// 0 s. Project policy (not retail facts): the distance-fade and fog-of-war
// inputs have no presentation source, so they are identity. The engine's grass
// vertex processor billboards each clump about its centre; that centre is not
// stored in the model and its rule is not established, so the cards draw as
// authored with culling off. The base sampler is point-filtered within a level
// and linear between levels; the colour arithmetic follows the linear policy
// of the lit adapters.
namespace eawr::presentation::godot_backend::legacy::grass {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_mix, depth_draw_always, depth_test_default, cull_disabled;

uniform sampler2D BaseTexture : filter_nearest_mipmap, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec4 eawr_diffuse = vec4(1.0);
uniform vec3 eawr_diffuse1 = vec3(1.0);
uniform float eawr_bend_scale = 1.0;
uniform vec3 eawr_wind = vec3(0.0);
uniform float eawr_scene_time = 0.0;
uniform vec4 eawr_mesh_x = vec4(1.0, 0.0, 0.0, 0.0);
uniform vec4 eawr_mesh_y = vec4(0.0, 0.0, -1.0, 0.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
varying vec4 eawr_vertex_colour;

vec3 eawr_linear_to_srgb(vec3 linear_rgb) {
    vec3 nonnegative = max(linear_rgb, vec3(0.0));
    vec3 low = 12.92 * nonnegative;
    vec3 high = 1.055 * pow(nonnegative, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.0031308)));
}

vec3 eawr_srgb_to_linear(vec3 encoded_rgb) {
    vec3 nonnegative = max(encoded_rgb, vec3(0.0));
    vec3 low = nonnegative / 12.92;
    vec3 high = pow((nonnegative + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.04045)));
}

void vertex() {
    vec4 model_position = vec4(VERTEX, 1.0);
    float wind_speed = length(eawr_wind);
    float normalized_speed = eawr_bend_scale * wind_speed / 10.0;
    float time_scale = 0.125 + 0.875 * normalized_speed;
    float wave = 0.5 + 0.5 * sin(6.28 * fract(time_scale * eawr_scene_time
        + dot(eawr_mesh_x, model_position) / 20.0 + dot(eawr_mesh_y, model_position) / 20.0));
    if (wind_speed > 0.0) {
        float bend = 10.0 * normalized_speed + (10.0 + 10.5 * normalized_speed) * wave;
        VERTEX += inverse(mat3(MODEL_MATRIX)) * ((1.0 - UV.y) * bend / wind_speed * eawr_wind);
    }
    vec3 up_world = normalize(mat3(MODEL_MATRIX) * vec3(0.0, 1.0, 0.0));
    vec4 normal_h = vec4(up_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    vec3 material = mix(eawr_diffuse.rgb, eawr_diffuse1, wave) * eawr_light_scale.rgb;
    eawr_vertex_colour = clamp(vec4(irradiance * material + eawr_emissive,
        eawr_diffuse.a * eawr_light_scale.a), 0.0, 1.0);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    float alpha = base_sample.a * eawr_vertex_colour.a;
    // AlphaFunc GREATER with AlphaRef 8.
    if (alpha <= 8.0 / 255.0) {
        discard;
    }
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_colour.rgb * base_linear_rgb;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
    ALPHA = alpha;
}
)GODOT";

inline constexpr std::array<float, 4> default_emissive{0.0F, 0.0F, 0.0F, 0.0F};
inline constexpr std::array<float, 4> default_diffuse{1.0F, 1.0F, 1.0F, 1.0F};
inline constexpr std::array<float, 4> default_diffuse1{1.0F, 1.0F, 1.0F, 1.0F};
inline constexpr float default_bend_scale = 1.0F;

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::transparent ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Emissive"), std::string_view("Diffuse"),
             std::string_view("Diffuse1")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    return bindings::scalar_problem(material, "BendScale");
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto emissive = bindings::vector_or(material, "Emissive", default_emissive);
    const auto diffuse1 = bindings::vector_or(material, "Diffuse1", default_diffuse1);
    return {
        {"eawr_emissive", std::array<float, 3>{emissive[0], emissive[1], emissive[2]}},
        {"eawr_diffuse", bindings::vector_or(material, "Diffuse", default_diffuse)},
        {"eawr_diffuse1", std::array<float, 3>{diffuse1[0], diffuse1[1], diffuse1[2]}},
        {"eawr_bend_scale", bindings::scalar_or(material, "BendScale", default_bend_scale)},
    };
}

struct ReferenceOutput final {
    std::array<float, 3> rgb{};
    float alpha{};
    bool drawn{};
};

// Engine-free reference for the pixel stage in linear light, given the
// interpolated (already saturated) vertex colour and the texel.
[[nodiscard]] constexpr ReferenceOutput reference_pixel(
    const std::array<float, 4>& texel_linear, const std::array<float, 4>& colour) noexcept {
    ReferenceOutput result;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result.rgb[channel] = 2.0F * colour[channel] * texel_linear[channel];
    }
    result.alpha = texel_linear[3] * colour[3];
    result.drawn = result.alpha > 8.0F / 255.0F;
    return result;
}

inline constexpr Family family{
    .program = "Grass.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = false,
    .transparent = true,
    .receives_shadows = true,
    .reads_wind = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::grass
