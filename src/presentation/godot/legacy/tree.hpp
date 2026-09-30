#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tree.fx sph_t1/sph_t1_p0 for placed foliage (#32), from the clean-room note
// on the effect (docs/behaviour/vegetation-effects.md T-01..T-08). Vertex
// stage: D = saturate(Diffuse x SPH_LIGHT_ALL irradiance x LIGHT_SCALE.rgb +
// Emissive) with alpha LIGHT_SCALE.a, and S = saturate(Specular x
// pow(max(N.H, 0), 16) x light-0 specular); both are colour registers, hence
// the saturation. Pixel stage: RGB = 2 x texel.rgb x D.rgb + 2 x S x texel.a,
// alpha = texel.a x LIGHT_SCALE.a, alpha test GREATER 128/255, depth writes,
// LESSEQUAL, and blending only while LIGHT_SCALE.a < 1. The renderer binds
// LIGHT_SCALE.a = 1, so the pass is alpha-tested and unblended: it is admitted
// in the opaque render pass with an alpha scissor, although the effect
// declares the Transparent render phase. The base sampler is point-filtered
// within a level and linear between levels. The wind bend (#147, T-07 and
// W-02..W-04) moves the world position by BendScale x WIND_BEND_VECTOR.xyz x
// z^2 x WIND_BEND_VECTOR.w before lighting, z being the vertex's mesh-space
// height: the bend vector is the scene wind x 0.5 x (1 + sin(2 pi t / 3 +
// 2 pi sin(2 pi x / 1000) sin(2 pi y / 1000))) at the placed model's world
// box centre (x, y), and w = 1 / H^2 for the box height H. The renderer binds
// the scene wind and clock (eawr_wind, eawr_scene_time); the populated map
// binds the model's object-space bend box (eawr_bend_box_min/max) and the
// surface's mesh frame (eawr_mesh_z: the row taking the render-basis model
// VERTEX to the source-basis mesh-space height). Unbound, the wind is zero and the box
// empty, so nothing bends. The never-sampled NormalTexture is accepted unread.
// The colour arithmetic follows the MeshGloss adapter's linear policy.
namespace eawr::presentation::godot_backend::legacy::tree {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_nearest_mipmap, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
uniform vec3 eawr_eye_position = vec3(0.0, 420.0, 1050.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
uniform float eawr_bend_scale = 1.0;
uniform vec3 eawr_wind = vec3(0.0);
uniform float eawr_scene_time = 0.0;
uniform vec3 eawr_bend_box_min = vec3(0.0);
uniform vec3 eawr_bend_box_max = vec3(0.0);
uniform vec4 eawr_mesh_z = vec4(0.0, 1.0, 0.0, 0.0);
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
    // WIND_BEND_VECTOR from the placed model's world box: the centre's source
    // (x, y) is render (x, -z) and the height is the render-Y extent.
    vec3 box_centre = (MODEL_MATRIX * vec4(0.5 * (eawr_bend_box_min + eawr_bend_box_max), 1.0)).xyz;
    vec3 box_half = 0.5 * (eawr_bend_box_max - eawr_bend_box_min);
    float box_height = 2.0 * dot(abs(vec3(MODEL_MATRIX[0].y, MODEL_MATRIX[1].y, MODEL_MATRIX[2].y)), box_half);
    if (box_height > 0.0) {
        float phase = 6.2831855 * sin(6.2831855 * box_centre.x / 1000.0) * sin(6.2831855 * -box_centre.z / 1000.0);
        vec3 bend = eawr_wind * (0.5 * (sin(6.2831855 * fract(eawr_scene_time / 3.0) + phase) + 1.0));
        float mesh_z = dot(eawr_mesh_z, vec4(VERTEX, 1.0));
        vec3 offset = eawr_bend_scale * bend * (mesh_z * mesh_z / (box_height * box_height));
        position_world += offset;
        VERTEX += inverse(mat3(MODEL_MATRIX)) * offset;
    }
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    vec3 eye_direction = normalize(eawr_eye_position - position_world);
    vec3 half_direction = normalize(eye_direction + eawr_light_direction);
    float specular_factor = pow(max(dot(normal_world, half_direction), 0.0), 16.0);
    eawr_vertex_diffuse = clamp(vec4(
        eawr_diffuse * irradiance * eawr_light_scale.rgb + eawr_emissive,
        eawr_light_scale.a), 0.0, 1.0);
    eawr_vertex_specular = clamp(eawr_specular * (specular_factor * eawr_light_specular), 0.0, 1.0);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb
        + 2.0 * eawr_vertex_specular * base_sample.a;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
    ALPHA = base_sample.a * eawr_vertex_diffuse.a;
    // AlphaFunc GREATER with AlphaRef 128: an 8-bit alpha passes from 129.
    ALPHA_SCISSOR_THRESHOLD = 128.5 / 255.0;
}
)GODOT";

inline constexpr std::array<float, 4> default_emissive{0.0F, 0.0F, 0.0F, 0.0F};
inline constexpr std::array<float, 4> default_diffuse{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_specular{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr float default_bend_scale = 1.0F;
// The alpha scissor threshold in the adapter source.
inline constexpr float alpha_scissor = 128.5F / 255.0F;

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Emissive"), std::string_view("Diffuse"),
             std::string_view("Specular")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    return bindings::scalar_problem(material, "BendScale");
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
        {"eawr_bend_scale", bindings::scalar_or(material, "BendScale", default_bend_scale)},
    };
}

struct ReferenceOutput final {
    std::array<float, 3> rgb{};
    float alpha{};
    bool drawn{};
};

// Engine-free reference for the pixel stage in linear light, given the
// interpolated (already saturated) diffuse and specular colours, the texel and
// LIGHT_SCALE.a.
[[nodiscard]] constexpr ReferenceOutput reference_pixel(
    const std::array<float, 4>& texel_linear, const std::array<float, 3>& diffuse,
    const std::array<float, 3>& specular, const float light_scale_alpha) noexcept {
    ReferenceOutput result;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result.rgb[channel] = 2.0F * diffuse[channel] * texel_linear[channel]
            + 2.0F * specular[channel] * texel_linear[3];
    }
    result.alpha = texel_linear[3] * bindings::saturate(light_scale_alpha);
    result.drawn = result.alpha >= alpha_scissor;
    return result;
}

inline constexpr Family family{
    .program = "Tree.fx",
    .technique = "sph_t1",
    .pass_name = "sph_t1_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .reads_wind = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::tree
