#pragma once

#include "bindings.hpp"
#include "family.hpp"

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// MeshBumpColorize.fx and RSkinBumpColorize.fx sph_t2/sph_t2_p0 (#199), the
// technique retail selects at the Highest shader detail (DX9, docs/
// graphics-features.md), opaque pass only. Facts from the FoC shader source:
//
// Vertex stage. The fill lights and ambient are per vertex: D = Diffuse x
// SPH_LIGHT_FILL irradiance(world normal) x LIGHT_SCALE.rgb + Emissive, alpha
// LIGHT_SCALE.a, written to a vs_1_1 colour output and so clamped to [0, 1].
// The sun (light 0) is per pixel: the toward-light vector L and the half
// vector H = normalize(normalize(eye - P) + L) are projected on the authored
// tangent frame, (v.T, v.B, v.N), normalized and interpolated. MeshBumpColorize
// works in object space with the raw T, B, N; RSkinBumpColorize works in world
// space after the skin transform with the raw T and B and the normalized N.
// Both texture coordinates are UV + UVOffset.xy.
//
// Pixel stage. surface = lerp(base.rgb, Colorization.rgb x base.rgb, base.a);
// n = 2 (normal.rgb - 0.5); the interpolated L and H are not renormalized.
// RGB = 2 surface (saturate(n.L) Diffuse x light-0 diffuse x LIGHT_SCALE.rgb
// + D.rgb) + light-0 specular x Specular x saturate(n.H)^16 x normal.a,
// alpha D.a. Shininess is authored but unread. Declared states match the
// other opaque hull effects (ZWriteEnable TRUE, ZFunc LESSEQUAL, blend only
// when LIGHT_SCALE.a < 1); the opaque pass binds LIGHT_SCALE.a = 1, and no
// transparent LIGHT_SCALE source exists, so the transparent pass is not
// admitted.
//
// Colour policy (docs/rendering.md): the arithmetic runs on stored texel and
// constant values, as the retail ps_2_0 does, and ALBEDO is the stored result.
// Colorization therefore binds as a stored value (scene::colorization_binding
// for a stored-value selector). Godot's vertex stage sees TANGENT and NORMAL
// after skinning and in model space; world space is the same frame for the
// mesh program up to the object's uniform scale, which the final normalize
// removes.
namespace eawr::presentation::godot_backend::legacy::bump_colorize {

// The frame line is the only difference between the two programs.
inline constexpr std::string_view mesh_frame_normal = "    vec3 frame_normal = model_basis * normal_model;\n";
inline constexpr std::string_view rskin_frame_normal = "    vec3 frame_normal = normal_world;\n";

inline constexpr std::string_view shader_head = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D NormalTexture : filter_linear_mipmap, repeat_enable;
uniform vec3 eawr_emissive = vec3(0.0);
uniform vec3 eawr_diffuse = vec3(1.0);
uniform vec3 eawr_specular = vec3(1.0);
uniform vec3 eawr_colorization = vec3(0.0, 1.0, 0.0);
uniform vec2 eawr_uv_offset = vec2(0.0);
uniform mat4 eawr_sph_fill_r;
uniform mat4 eawr_sph_fill_g;
uniform mat4 eawr_sph_fill_b;
uniform vec4 eawr_light_scale = vec4(1.0);
// The unit's own light scale (GodotRenderer::set_light_scale), e.g. the shield flash.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
// The unit's own opacity (GodotRenderer::set_unit_opacity, #535): a screen-door fade of the opaque pass.
instance uniform float eawr_unit_opacity = 1.0;
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec3 eawr_vertex_diffuse;
varying vec3 eawr_tangent_light;
varying vec3 eawr_tangent_half;

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
)GODOT";

inline constexpr std::string_view shader_tail = R"GODOT(    vec3 frame_tangent = model_basis * tangent_model;
    vec3 frame_binormal = model_basis * binormal_model;
    vec3 toward_light = eawr_light_direction;
    vec3 half_vector = normalize(normalize(CAMERA_POSITION_WORLD - position_world) + toward_light);
    eawr_tangent_light = normalize(vec3(dot(toward_light, frame_tangent),
        dot(toward_light, frame_binormal), dot(toward_light, frame_normal)));
    eawr_tangent_half = normalize(vec3(dot(half_vector, frame_tangent),
        dot(half_vector, frame_binormal), dot(half_vector, frame_normal)));
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 fill = vec3(
        dot(normal_h, eawr_sph_fill_r * normal_h),
        dot(normal_h, eawr_sph_fill_g * normal_h),
        dot(normal_h, eawr_sph_fill_b * normal_h));
    eawr_vertex_diffuse = clamp(eawr_diffuse * fill * eawr_light_scale.rgb * eawr_unit_light_scale + eawr_emissive,
        0.0, 1.0);
    UV += eawr_uv_offset;
}

void fragment() {
    if (eawr_unit_opacity < 1.0) {
        float eawr_dither = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
        if (eawr_dither >= eawr_unit_opacity) discard;
    }
    vec4 base = texture(BaseTexture, UV);
    vec4 normal_texel = texture(NormalTexture, UV);
    vec3 surface = mix(base.rgb, eawr_colorization * base.rgb, base.a);
    vec3 normal_vector = 2.0 * (normal_texel.rgb - 0.5);
    float n_dot_l = clamp(dot(normal_vector, eawr_tangent_light), 0.0, 1.0);
    float n_dot_h = clamp(dot(normal_vector, eawr_tangent_half), 0.0, 1.0);
    vec3 diffuse = 2.0 * surface
        * (n_dot_l * eawr_diffuse * eawr_light_diffuse * eawr_light_scale.rgb * eawr_unit_light_scale
            + eawr_vertex_diffuse);
    vec3 specular = eawr_light_specular * eawr_specular * pow(n_dot_h, 16.0) * normal_texel.a;
    vec3 stored_rgb = diffuse + specular;
    ALBEDO = eawr_stored_albedo(stored_rgb);
}
)GODOT";

[[nodiscard]] inline std::string_view mesh_shader(const RenderPass pass) noexcept {
    static const std::string source
        = std::string(shader_head) + std::string(mesh_frame_normal) + std::string(shader_tail);
    return pass == RenderPass::opaque ? std::string_view(source) : std::string_view{};
}

[[nodiscard]] inline std::string_view rskin_shader(const RenderPass pass) noexcept {
    static const std::string source
        = std::string(shader_head) + std::string(rskin_frame_normal) + std::string(shader_tail);
    return pass == RenderPass::opaque ? std::string_view(source) : std::string_view{};
}

inline constexpr std::array<float, 4> default_emissive{0.0F, 0.0F, 0.0F, 0.0F};
inline constexpr std::array<float, 4> default_diffuse{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_specular{1.0F, 1.0F, 1.0F, 0.0F};
inline constexpr std::array<float, 4> default_colorization{0.0F, 1.0F, 0.0F, 1.0F};
inline constexpr std::array<float, 4> default_uv_offset{0.0F, 0.0F, 0.0F, 0.0F};

inline constexpr TextureBinding sampled_textures[]{{"NormalTexture", TexturePlaceholder::flat_normal}};

[[nodiscard]] inline std::optional<std::string> binding_problem(const MaterialDescription& material) {
    for (const std::string_view name : {std::string_view("Emissive"), std::string_view("Diffuse"),
             std::string_view("Specular"), std::string_view("Colorization"), std::string_view("UVOffset")}) {
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
        {"eawr_colorization", rgb("Colorization", default_colorization)},
        {"eawr_uv_offset", std::array<float, 2>{offset[0], offset[1]}},
    };
}

// Engine-free reference for the pixel stage on stored values, given the
// two texture samples, the clamped vertex fill colour and the interpolated
// tangent-space light and half vectors.
struct PixelInputs final {
    std::array<float, 4> base{};
    std::array<float, 4> normal{};
    std::array<float, 3> colorization{};
    std::array<float, 3> diffuse{};
    std::array<float, 3> specular{};
    std::array<float, 3> light_diffuse{};
    std::array<float, 3> light_specular{};
    std::array<float, 3> light_scale{1.0F, 1.0F, 1.0F};
    std::array<float, 3> vertex_diffuse{};
    std::array<float, 3> tangent_light{};
    std::array<float, 3> tangent_half{};
};

[[nodiscard]] inline std::array<float, 3> reference_pixel(const PixelInputs& in) noexcept {
    std::array<float, 3> n{};
    for (std::size_t axis = 0; axis < 3; ++axis) n[axis] = 2.0F * (in.normal[axis] - 0.5F);
    const auto dot = [&n](const std::array<float, 3>& v) { return n[0] * v[0] + n[1] * v[1] + n[2] * v[2]; };
    const float n_dot_l = bindings::saturate(dot(in.tangent_light));
    const float n_dot_h = bindings::saturate(dot(in.tangent_half));
    const float highlight = std::pow(n_dot_h, 16.0F);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        const float base = in.base[channel];
        const float surface = base + (in.colorization[channel] * base - base) * in.base[3];
        const float light = n_dot_l * in.diffuse[channel] * in.light_diffuse[channel] * in.light_scale[channel]
            + in.vertex_diffuse[channel];
        result[channel] = 2.0F * surface * light
            + in.light_specular[channel] * in.specular[channel] * highlight * in.normal[3];
    }
    return result;
}

inline constexpr Family mesh_family{
    .program = "MeshBumpColorize.fx",
    .technique = "sph_t2",
    .pass_name = "sph_t2_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .binding_textures = sampled_textures,
    .authored_binormals = true,
    .shader = &mesh_shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

inline constexpr Family rskin_family{
    .program = "RSkinBumpColorize.fx",
    .technique = "sph_t2",
    .pass_name = "sph_t2_p0",
    .opaque = true,
    .transparent = false,
    .receives_shadows = true,
    .binding_textures = sampled_textures,
    .authored_binormals = true,
    .shader = &rskin_shader,
    .binding_problem = &binding_problem,
    .uniforms = &uniforms,
};

} // namespace eawr::presentation::godot_backend::legacy::bump_colorize
