#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshGlossColorize.fx sph_t0/sph_t0_p0 for placed objects (WP-43), opaque
// pass only. Its vertex stage is MeshGloss sph_t0's (see shader_adapter.hpp
// meshgloss_shader_opaque): D = Diffuse x SPH_LIGHT_ALL irradiance x
// LIGHT_SCALE.rgb + Emissive with alpha LIGHT_SCALE.a, and S = Specular x
// pow(max(N.H, 0), 16) x light-0 specular. The pixel stage colorizes before
// lighting: surface = mix(base.rgb, Colorization.rgb x base.rgb, base.a),
// RGB = 2 x surface x D.rgb + S x gloss.r, alpha = D.a, where gloss is the
// GlossTexture sample at the same UV. Declared states match MeshGloss
// (ZWriteEnable TRUE, ZFunc LESSEQUAL, blend only when LIGHT_SCALE.a < 1);
// the opaque pass binds LIGHT_SCALE.a = 1, and no transparent LIGHT_SCALE
// source exists, so the transparent pass is not admitted. The colour
// arithmetic follows the MeshGloss adapter's linear policy; its ps_1_1 input
// saturation is not reproduced, exactly as for MeshGloss.
//
// GlossTexture reaches the upload as its own per-binding texture (gate
// MULTITEX-01, as MeshAlphaGloss's does since #200), so a distinct gloss map
// such as the laser pads' NV_MDS_Gloss.tga (#80) samples its own image; an
// unresolved one binds the black placeholder, which adds no specular.
namespace eawr::presentation::godot_backend::legacy::mesh_gloss_colorize {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D GlossTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform vec3 eawr_colorization = vec3(0.0, 1.0, 0.0);
instance uniform vec4 eawr_unit_colorization = vec4(0.0, 0.0, 0.0, -1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
// WR-14/37: per-object preview tint and arrival fade share the light-scale input.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
uniform vec3 eawr_eye_position = vec3(0.0, 420.0, 1050.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec4 eawr_vertex_diffuse;
varying vec3 eawr_vertex_specular;

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
    vec3 position_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    vec3 eye_direction = normalize(eawr_eye_position - position_world);
    vec3 half_direction = normalize(eye_direction + eawr_light_direction);
    float specular_factor = pow(max(dot(normal_world, half_direction), 0.0), 16.0);
    eawr_vertex_diffuse = vec4(
        eawr_diffuse * irradiance * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive,
        eawr_light_scale.a);
    eawr_vertex_specular = eawr_specular * (specular_factor * eawr_light_specular);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    // The gloss mask is data: its red channel is read as stored, like the
    // MeshGloss adapter's texture alpha.
    float gloss = texture(GlossTexture, UV).r;
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 colorization = eawr_unit_colorization.a < 0.0 ? eawr_colorization
        : eawr_srgb_to_linear(eawr_unit_colorization.rgb);
    vec3 surface = mix(base_linear_rgb, colorization * base_linear_rgb, base_sample.a);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * surface + eawr_vertex_specular * gloss;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
}
)GODOT";

inline constexpr std::array<float, 4> default_emissive{0.0F, 0.0F, 0.0F, 0.0F};
inline constexpr std::array<float, 4> default_diffuse{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_specular{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_colorization{0.0F, 1.0F, 0.0F, 1.0F};

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Emissive"), std::string_view("Diffuse"),
             std::string_view("Specular"), std::string_view("Colorization")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    const std::size_t gloss_count = bindings::count(material, "GlossTexture");
    if (gloss_count == 0) {
        return std::string("binds no GlossTexture; an unbound gloss sampler's result is not established");
    }
    if (gloss_count > 1 || bindings::count(material, "BaseTexture") > 1) {
        return std::string("binds GlossTexture or BaseTexture more than once");
    }
    if (!std::holds_alternative<std::string>(bindings::find(material, "GlossTexture")->value)) {
        return std::string("binds GlossTexture as a value that is not a texture");
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto rgb = [&material](const std::string_view name, const std::array<float, 4>& fallback) {
        const auto value = bindings::vector_or(material, name, fallback);
        return std::array<float, 3>{value[0], value[1], value[2]};
    };
    return {
        {"eawr_emissive", rgb("Emissive", default_emissive)},
        {"eawr_diffuse", rgb("Diffuse", default_diffuse)},
        {"eawr_specular", rgb("Specular", default_specular)},
        {"eawr_colorization", rgb("Colorization", default_colorization)},
    };
}

// Engine-free reference for the pixel stage in linear light, given the
// interpolated diffuse and specular and the two texture samples.
[[nodiscard]] constexpr std::array<float, 3> reference_pixel(
    const std::array<float, 4>& base_linear, const float gloss_red, const std::array<float, 3>& colorization,
    const std::array<float, 3>& diffuse, const std::array<float, 3>& specular) noexcept {
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float base = base_linear[channel];
        const float surface = base + (colorization[channel] * base - base) * base_linear[3];
        result[channel] = 2.0F * diffuse[channel] * surface + specular[channel] * gloss_red;
    }
    return result;
}

inline constexpr TextureBinding sampled_textures[]{{"GlossTexture", TexturePlaceholder::black}};

inline constexpr Family family{
    .program = "MeshGlossColorize.fx",
    .technique = "sph_t0",
    .pass_name = "sph_t0_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .binding_textures = sampled_textures,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_gloss_colorize
