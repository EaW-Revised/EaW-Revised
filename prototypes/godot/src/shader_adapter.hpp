#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <string_view>

namespace eawr::godot_prototype {

// Clean-room Godot-language adapter for MeshGloss sph_t0/sph_t0_p0. The
// arithmetic follows docs/behaviour/meshgloss-programmable.md; it is not a
// stock/PBR approximation and does not ingest private shader text.
inline constexpr std::string_view meshgloss_shader_opaque = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : source_color, filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform vec4 Specular = vec4(1.0);
uniform float Shininess = 32.0;
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
uniform vec3 eawr_eye_position = vec3(0.0, 420.0, 1050.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec4 eawr_vertex_diffuse;
varying vec3 eawr_vertex_specular;

// Godot Compatibility treats spatial ALBEDO as an sRGB value, then decodes it
// before its linear tonemap/output stage.  MeshGloss arithmetic is linear, so
// cross that API boundary explicitly instead of feeding linear RGB to ALBEDO.
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
    // Preserve the selected effect's world-upper-3x3 normal transform.
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
        Diffuse.rgb * irradiance * eawr_light_scale.rgb + Emissive.rgb,
        eawr_light_scale.a);
    eawr_vertex_specular = Specular.rgb * (specular_factor * eawr_light_specular);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    // Runtime-created BC3 textures use a non-sRGB GL internal format in the
    // Compatibility backend. OUTPUT_IS_SRGB identifies that path; decode the
    // common scene's declared sRGB texels before the linear material equation.
    vec3 base_linear_rgb = OUTPUT_IS_SRGB
        ? eawr_srgb_to_linear(base_sample.rgb) : base_sample.rgb;
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb
        + eawr_vertex_specular * base_sample.a;
    ALBEDO = OUTPUT_IS_SRGB
        ? eawr_linear_to_srgb(eawr_linear_rgb) : eawr_linear_rgb;
}
)GODOT";

inline constexpr std::string_view meshgloss_shader_alpha = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_mix, depth_draw_always, depth_test_default, cull_back;

uniform sampler2D BaseTexture : source_color, filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform vec4 Specular = vec4(1.0);
uniform float Shininess = 32.0;
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0, 1.0, 1.0, 0.5);
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
        Diffuse.rgb * irradiance * eawr_light_scale.rgb + Emissive.rgb,
        eawr_light_scale.a);
    eawr_vertex_specular = Specular.rgb * (specular_factor * eawr_light_specular);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = OUTPUT_IS_SRGB
        ? eawr_srgb_to_linear(base_sample.rgb) : base_sample.rgb;
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb
        + eawr_vertex_specular * base_sample.a;
    ALBEDO = OUTPUT_IS_SRGB
        ? eawr_linear_to_srgb(eawr_linear_rgb) : eawr_linear_rgb;
    ALPHA = eawr_vertex_diffuse.a;
}
)GODOT";

struct SphChannelMatrix final {
    // Column-major logical 4x4 matrix, matching Godot's mat4 representation.
    std::array<std::array<float, 4>, 4> columns{};
};

[[nodiscard]] inline SphChannelMatrix meshgloss_hemisphere_matrix(
    const float ambient, const float directional, const std::array<float, 3>& light) noexcept {
    SphChannelMatrix result;
    result.columns[0][3] = directional * light[0] * 0.25F;
    result.columns[1][3] = directional * light[1] * 0.25F;
    result.columns[2][3] = directional * light[2] * 0.25F;
    result.columns[3][0] = result.columns[0][3];
    result.columns[3][1] = result.columns[1][3];
    result.columns[3][2] = result.columns[2][3];
    result.columns[3][3] = ambient + directional * 0.5F;
    return result;
}

[[nodiscard]] inline float evaluate_quadratic(
    const SphChannelMatrix& matrix, const std::array<float, 4>& vector) noexcept {
    float result = 0.0F;
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            result += vector[row] * matrix.columns[column][row] * vector[column];
        }
    }
    return result;
}

[[nodiscard]] constexpr bool meshgloss_uses_alpha_blend(const float light_scale_alpha) noexcept {
    return light_scale_alpha < 1.0F;
}

[[nodiscard]] inline float linear_to_srgb_component(const float linear) noexcept {
    const float nonnegative = linear < 0.0F ? 0.0F : linear;
    return nonnegative <= 0.0031308F
        ? 12.92F * nonnegative
        : 1.055F * std::pow(nonnegative, 1.0F / 2.4F) - 0.055F;
}


[[nodiscard]] inline float srgb_to_linear_component(const float encoded) noexcept {
    const float nonnegative = encoded < 0.0F ? 0.0F : encoded;
    return nonnegative <= 0.04045F
        ? nonnegative / 12.92F
        : std::pow((nonnegative + 0.055F) / 1.055F, 2.4F);
}

} // namespace eawr::godot_prototype
