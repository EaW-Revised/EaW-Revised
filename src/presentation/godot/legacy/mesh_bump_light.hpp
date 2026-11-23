#pragma once

#include "bindings.hpp"
#include "bump_colorize.hpp"
#include "family.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// MeshBumpLight.fx sph_t2/sph_t2_p0 (#8037; CEM-20). A mod-authored effect, not a retail one: following the owner's
// decision (#3834) its compiled form was read locally on a mod host (tools/shaders/d3d9_bytecode.py) to learn what it does,
// and the behaviour is described here in our own words; nothing is copied. Observed in the compiled programs: the pass is the bump colorize lighting with two differences: the surface is
// base.rgb x (1 + 5 base.a) instead of the Colorization mix, and the specular term is per vertex. Spec variant: vs_1_1 with Specular and the eye position, ps_1_1):
//
// Vertex stage. D = Diffuse x SPH_LIGHT_FILL irradiance(world normal) x LIGHT_SCALE.rgb + Emissive, alpha
// LIGHT_SCALE.a, a saturated colour output. The sun's toward-light vector is projected on the authored tangent
// frame (v.T, v.B, v.N), normalized and interpolated (no half vector). S = Specular x light-0 specular x
// max(N.H, 0)^16 per vertex, H = normalize(normalize(eye - P) + L), saturated. Both texture coordinates are
// UV + UVOffset.xy.
//
// Pixel stage (ps_1_1). n = 2 (normal.rgb - 0.5) and the interpolated light vector are not renormalized;
// RGB = 2 (saturate(n.L) Diffuse x light-0 diffuse x LIGHT_SCALE.rgb + D.rgb) x base.rgb x (1 + 5 base.a)
// + normal.a x S, alpha D.a. Declared states: ZWriteEnable TRUE, ZFunc LESSEQUAL, AlphaBlendEnable FALSE, so
// the pass is opaque. Colorization is declared and unread. Output is the stored value.
namespace eawr::presentation::godot_backend::legacy::mesh_bump_light {

inline constexpr std::string_view shader_source = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D NormalTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform vec2 eawr_uv_offset = vec2(0.0);
uniform mat4 eawr_sph_fill_r;
uniform mat4 eawr_sph_fill_g;
uniform mat4 eawr_sph_fill_b;
uniform vec4 eawr_light_scale = vec4(1.0);
// The unit's own light scale (GodotRenderer::set_light_scale), e.g. the shield flash.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec3 eawr_vertex_diffuse;
varying vec3 eawr_vertex_specular;
varying vec3 eawr_tangent_light;

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void vertex() {
    mat3 model_basis = mat3(MODEL_MATRIX);
    vec3 position_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    vec3 normal_model = normalize(NORMAL);
    vec3 tangent_model = normalize(TANGENT);
    vec3 binormal_model = CUSTOM0.x * tangent_model + CUSTOM0.y * normal_model
        + CUSTOM0.z * cross(normal_model, tangent_model);
    vec3 normal_world = normalize(model_basis * normal_model);
    vec3 frame_tangent = model_basis * tangent_model;
    vec3 frame_binormal = model_basis * binormal_model;
    vec3 frame_normal = model_basis * normal_model;
    vec3 toward_light = eawr_light_direction;
    eawr_tangent_light = normalize(vec3(dot(toward_light, frame_tangent),
        dot(toward_light, frame_binormal), dot(toward_light, frame_normal)));
    vec3 half_vector = normalize(normalize(CAMERA_POSITION_WORLD - position_world) + toward_light);
    float highlight = pow(max(dot(normal_world, half_vector), 0.0), 16.0);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 fill = vec3(
        dot(normal_h, eawr_sph_fill_r * normal_h),
        dot(normal_h, eawr_sph_fill_g * normal_h),
        dot(normal_h, eawr_sph_fill_b * normal_h));
    eawr_vertex_diffuse = clamp(eawr_diffuse * fill * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive,
        0.0, 1.0);
    eawr_vertex_specular = clamp(eawr_specular * eawr_light_specular * highlight, 0.0, 1.0);
    UV += eawr_uv_offset;
}

void fragment() {
    vec4 base = texture(BaseTexture, UV);
    vec4 normal_texel = texture(NormalTexture, UV);
    vec3 surface = base.rgb * (1.0 + 5.0 * base.a);
    vec3 normal_vector = 2.0 * (normal_texel.rgb - 0.5);
    float n_dot_l = clamp(dot(normal_vector, eawr_tangent_light), 0.0, 1.0);
    vec3 lit = n_dot_l * eawr_diffuse * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale
        + eawr_vertex_diffuse;
    vec3 stored_rgb = 2.0 * lit * surface + normal_texel.a * eawr_vertex_specular;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

inline constexpr std::array<float, 4> default_emissive{0.0F, 0.0F, 0.0F, 0.0F};
inline constexpr std::array<float, 4> default_diffuse{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_specular{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_uv_offset{0.0F, 0.0F, 0.0F, 0.0F};

[[nodiscard]] inline std::string_view shader(const RenderPass pass) noexcept {
    return pass == RenderPass::opaque ? shader_source : std::string_view{};
}

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Emissive"), std::string_view("Diffuse"),
             std::string_view("Specular"), std::string_view("UVOffset")}) {
        if (auto problem = bindings::vector_problem(material, name)) return problem;
    }
    const std::size_t normal_count = bindings::count(material, "NormalTexture");
    if (normal_count == 0) {
        return std::string("binds no NormalTexture; an unbound normal sampler's result is not established");
    }
    if (normal_count > 1 || bindings::count(material, "BaseTexture") > 1) {
        return std::string("binds NormalTexture or BaseTexture more than once");
    }
    if (!std::holds_alternative<std::string>(bindings::find(material, "NormalTexture")->value)) {
        return std::string("binds NormalTexture as a value that is not a texture");
    }
    return std::nullopt;
}

[[nodiscard]] inline std::vector<Uniform> uniforms(const MaterialDescription& material) {
    const auto rgb = [&material](const std::string_view name, const std::array<float, 4>& fallback) {
        const auto value = bindings::vector_or(material, name, fallback);
        return std::array<float, 3>{value[0], value[1], value[2]};
    };
    const auto offset = bindings::vector_or(material, "UVOffset", default_uv_offset);
    return {
        {"eawr_emissive", rgb("Emissive", default_emissive)},
        {"eawr_diffuse", rgb("Diffuse", default_diffuse)},
        {"eawr_specular", rgb("Specular", default_specular)},
        {"eawr_uv_offset", std::array<float, 2>{offset[0], offset[1]}},
    };
}

// Engine-free reference for the pixel stage on stored values.
struct PixelInputs final {
    std::array<float, 4> base{};
    std::array<float, 4> normal{};
    std::array<float, 3> diffuse{};
    std::array<float, 3> light_diffuse{};
    std::array<float, 3> light_scale{1.0F, 1.0F, 1.0F};
    std::array<float, 3> vertex_diffuse{};
    std::array<float, 3> vertex_specular{};
    std::array<float, 3> tangent_light{};
};

[[nodiscard]] inline std::array<float, 3> reference_pixel(const PixelInputs& in) noexcept {
    float n_dot_l = 0.0F;
    for (std::size_t axis = 0; axis < 3; ++axis) n_dot_l += 2.0F * (in.normal[axis] - 0.5F) * in.tangent_light[axis];
    n_dot_l = bindings::saturate(n_dot_l);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float surface = in.base[channel] * (1.0F + 5.0F * in.base[3]);
        const float lit = n_dot_l * in.diffuse[channel] * in.light_diffuse[channel] * in.light_scale[channel]
            + in.vertex_diffuse[channel];
        result[channel] = 2.0F * lit * surface + in.normal[3] * in.vertex_specular[channel];
    }
    return result;
}

inline constexpr Family family{
    .program = "MeshBumpLight.fx",
    .technique = "sph_t2",
    .pass_name = "sph_t2_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .binding_textures = bump_colorize::sampled_textures,
    .authored_binormals = true,
    .shader = &shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::mesh_bump_light
