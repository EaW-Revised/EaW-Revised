#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend {

// Clean-room Godot-language adapter for MeshGloss sph_t0/sph_t0_p0. The
// arithmetic follows docs/behaviour/meshgloss-programmable.md; it is not a
// stock/PBR approximation and does not ingest private shader text.
inline constexpr std::string_view meshgloss_shader_opaque = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
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

// ALBEDO is a stored (sRGB-encoded) value on every backend (docs/rendering.md,
// colour policy). MeshGloss arithmetic is linear, so cross that boundary
// explicitly instead of feeding linear RGB to ALBEDO.
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
    // Texels are sampled as stored values (no source_color decode); decode the
    // common scene's declared sRGB texels before the linear material equation.
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb
        + eawr_vertex_specular * base_sample.a;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
}
)GODOT";

inline constexpr std::string_view meshgloss_shader_alpha = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_mix, depth_draw_always, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
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
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb
        + eawr_vertex_specular * base_sample.a;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
    ALPHA = eawr_vertex_diffuse.a;
}
)GODOT";

// Clean-room Godot adapter for the accepted opaque RSKIN descriptor family.
// Godot's mesh BONE/WEIGHT streams and RenderingServer skeleton palette perform
// vertex deformation; this shader preserves the common colorization contract.
inline constexpr std::string_view rskin_shader_opaque = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform vec4 Specular = vec4(1.0);
uniform float Shininess = 32.0;
uniform vec4 Colorization = vec4(0.0, 1.0, 0.0, 1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
// The unit's own light scale (GodotRenderer::set_light_scale), e.g. the shield flash.
instance uniform vec3 eawr_unit_light_scale = vec3(1.0);
// The unit's own opacity (GodotRenderer::set_unit_opacity, #535): a screen-door fade of the opaque pass.
instance uniform float eawr_unit_opacity = 1.0;
uniform vec3 eawr_eye_position = vec3(0.0, 420.0, 1050.0);
uniform vec3 eawr_light_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_light_specular = vec3(2.0, 1.88, 1.72);
varying vec3 eawr_vertex_diffuse;
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
    float power = max(Shininess, 1.0);
    float specular_factor = pow(max(dot(normal_world, half_direction), 0.0), power);
    eawr_vertex_diffuse = Diffuse.rgb * irradiance * eawr_light_scale.rgb * eawr_unit_light_scale + Emissive.rgb;
    eawr_vertex_specular = Specular.rgb * (specular_factor * eawr_light_specular);
}

void fragment() {
    if (eawr_unit_opacity < 1.0) {
        float eawr_dither = fract(52.9829189 * fract(dot(FRAGCOORD.xy, vec2(0.06711056, 0.00583715))));
        if (eawr_dither >= eawr_unit_opacity) discard;
    }
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 colorized = mix(base_linear_rgb, Colorization.rgb, base_sample.a);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse * colorized
        + eawr_vertex_specular * base_sample.a;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
}
)GODOT";

// Clean-room adapters for fixed-function MESH/BATCHMESH passes whose complete
// texture-stage cascade is carried by the P1-02 descriptor bundle
// (plan/inventories/shader-corpus.json, `source_states`). No shader text is
// ingested; the cascades are:
//   BatchMeshGloss sph_t1_p0: stage 0 rgb = 2 * D.rgb * B.rgb, a = D.a;
//     stage 1 multiplies by the fog-of-war texture, which is #28's input and
//     is the identity here (fixed_mesh_shader_opaque_fog below is the opt-in
//     synthetic fog-stub-v1 stage 1). Depth write on, source-alpha blending.
//   MeshAlpha / MeshAlphaGloss / BatchMeshAlpha sph_t1_p0:
//     rgb = 2 * D.rgb * B.rgb,
//     a = B.a * D.a. No depth write, source-alpha/inverse-source-alpha.
//   BatchMeshAlpha's stage 1 also multiplies RGB by its FOW texture. That
//   stage is the identity until its opt-in fog consumer is installed. The NU2
//   vertex format has no authored colour;
//   D.a is interpolated fixed-function material/light-scale alpha.
// D is fixed-function vertex lighting: material diffuse (Diffuse * light
// scale) against engine lights plus emissive. Engine lights are external draw
// inputs owned by #25, so D is evaluated with the renderer's placeholder
// hemisphere rig, exactly as the accepted MeshGloss adapter does.
inline constexpr std::string_view fixed_mesh_shader_opaque = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
varying vec3 eawr_vertex_diffuse;

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
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    eawr_vertex_diffuse = Diffuse.rgb * irradiance * eawr_light_scale.rgb + Emissive.rgb;
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse * base_linear_rgb;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
}
)GODOT";

// Shared synthetic fog-stub-v1 stage 1 for the opt-in BatchMeshGloss and
// BatchMeshAlpha sph_t1_p0 variants (P1-07, #28/#32). Each is its baseline
// shader plus exactly the three blocks below (fog uniforms, the world-position
// varying, one multiply); render modes, stage 0, alpha, depth and lighting
// inputs are unchanged. Source XY is the
// world (X, -Z) of the posed vertex after MODEL_MATRIX. Nearest texel-centre
// sampling, half-open bounds tested before clamping, dark outside or while
// unbound, and the attenuation multiplies linear RGB once before the output
// transfer. Harness policy, not retail fog semantics.
inline constexpr std::string_view fog_stage1_declarations = R"GODOT(uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
varying vec3 eawr_fog_world_position;

float eawr_fog_attenuation() {
    vec2 source = vec2(eawr_fog_world_position.x, -eawr_fog_world_position.z);
    vec2 uv = (source - eawr_fog_origin) / eawr_fog_extent;
    float attenuation = 0.0;
    if (eawr_fog_bound && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
        vec2 texel = min(floor(uv * eawr_fog_size), eawr_fog_size - vec2(1.0));
        attenuation = texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
    }
    return attenuation;
}
)GODOT";
inline constexpr std::string_view fog_stage1_vertex =
    "    eawr_fog_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;\n";
inline constexpr std::string_view fog_stage1_fragment =
    "    eawr_linear_rgb *= eawr_fog_attenuation();\n";

// Derived fog-stub-v1 stage for an accepted legacy adapter with no fixed fog
// variant (P1-07 #28; opt-in, harness policy, not retail fog semantics). The
// result is `source` plus: fog_stage1_declarations and two sRGB helpers after
// the render_mode statement, fog_stage1_vertex first in vertex() (a vertex()
// is added when the source has none), and one statement right after the
// single `ALBEDO = ` assignment that decodes the stored ALBEDO, multiplies the
// linear colour once and re-encodes it. Render modes, alpha, depth, lighting and
// every other statement are unchanged. nullopt when the source does not have
// exactly one render_mode, fragment() and ALBEDO assignment, already declares
// a fog uniform, or uses a render mode under which MODEL_MATRIX * VERTEX is
// not the world position.
inline constexpr std::string_view derived_fog_helpers = R"GODOT(
vec3 eawr_fog_to_linear(vec3 encoded_rgb) {
    vec3 nonnegative = max(encoded_rgb, vec3(0.0));
    return mix(pow((nonnegative + 0.055) / 1.055, vec3(2.4)), nonnegative / 12.92,
        lessThanEqual(nonnegative, vec3(0.04045)));
}

vec3 eawr_fog_to_srgb(vec3 linear_rgb) {
    vec3 nonnegative = max(linear_rgb, vec3(0.0));
    return mix(1.055 * pow(nonnegative, vec3(1.0 / 2.4)) - 0.055, 12.92 * nonnegative,
        lessThanEqual(nonnegative, vec3(0.0031308)));
}
)GODOT";
inline constexpr std::string_view derived_fog_fragment =
    "\n    ALBEDO = eawr_fog_to_srgb(eawr_fog_to_linear(ALBEDO) * eawr_fog_attenuation());";

[[nodiscard]] inline std::optional<std::string> derived_legacy_fog_variant(const std::string_view source) {
    const auto once = [source](const std::string_view token) {
        const std::size_t at = source.find(token);
        return at != std::string_view::npos && source.find(token, at + 1) == std::string_view::npos
            ? at : std::string_view::npos;
    };
    for (const std::string_view refused : {std::string_view("eawr_fog_"),
             std::string_view("skip_vertex_transform"), std::string_view("world_vertex_coords")}) {
        if (source.find(refused) != std::string_view::npos) return std::nullopt;
    }
    const std::size_t mode = once("render_mode ");
    const std::size_t fragment = once("void fragment() {");
    const std::size_t albedo = once("ALBEDO = ");
    const std::size_t vertex = source.find("void vertex() {");
    if (mode == std::string_view::npos || fragment == std::string_view::npos
        || albedo == std::string_view::npos || albedo < fragment
        || (vertex != std::string_view::npos && once("void vertex() {") == std::string_view::npos)) {
        return std::nullopt;
    }
    const std::size_t mode_end = source.find(';', mode);
    const std::size_t albedo_end = source.find(';', albedo);
    constexpr std::string_view vertex_open = "void vertex() {\n";
    if (mode_end == std::string_view::npos || albedo_end == std::string_view::npos
        || mode_end > fragment || (vertex != std::string_view::npos
            && (vertex < mode_end || source.compare(vertex, vertex_open.size(), vertex_open) != 0))) {
        return std::nullopt;
    }
    // Insertions at offsets of the unmodified source, applied last-first so
    // every earlier offset stays valid.
    std::array<std::pair<std::size_t, std::string>, 3> inserts{{
        {mode_end + 1, "\n" + std::string(fog_stage1_declarations) + std::string(derived_fog_helpers)},
        vertex != std::string_view::npos
            ? std::pair<std::size_t, std::string>{vertex + vertex_open.size(), std::string(fog_stage1_vertex)}
            : std::pair<std::size_t, std::string>{fragment, std::string(vertex_open)
                + std::string(fog_stage1_vertex) + "}\n\n"},
        {albedo_end + 1, std::string(derived_fog_fragment)},
    }};
    std::sort(inserts.begin(), inserts.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });
    std::string result(source);
    for (const auto& [offset, text] : inserts) result.insert(offset, text);
    return result;
}

inline constexpr std::string_view fixed_mesh_shader_opaque_fog = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, depth_draw_opaque, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
varying vec3 eawr_vertex_diffuse;
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
varying vec3 eawr_fog_world_position;

float eawr_fog_attenuation() {
    vec2 source = vec2(eawr_fog_world_position.x, -eawr_fog_world_position.z);
    vec2 uv = (source - eawr_fog_origin) / eawr_fog_extent;
    float attenuation = 0.0;
    if (eawr_fog_bound && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
        vec2 texel = min(floor(uv * eawr_fog_size), eawr_fog_size - vec2(1.0));
        attenuation = texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
    }
    return attenuation;
}

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
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    eawr_vertex_diffuse = Diffuse.rgb * irradiance * eawr_light_scale.rgb + Emissive.rgb;
    eawr_fog_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse * base_linear_rgb;
    eawr_linear_rgb *= eawr_fog_attenuation();
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
}
)GODOT";

inline constexpr std::string_view fixed_mesh_shader_alpha = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_mix, depth_draw_never, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
varying vec4 eawr_vertex_diffuse;

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
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    eawr_vertex_diffuse = vec4(
        Diffuse.rgb * irradiance * eawr_light_scale.rgb + Emissive.rgb,
        Diffuse.a * eawr_light_scale.a);
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb;
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
    ALPHA = base_sample.a * eawr_vertex_diffuse.a;
}
)GODOT";

// Opt-in fog-stub-v1 stage 1 for exactly BatchMeshAlpha sph_t1/sph_t1_p0.
// The fixed alpha adapter's lighting, source alpha, blend/depth/cull modes and
// pass priority remain unchanged. Visibility multiplies linear RGB once;
// neither texture alpha nor interpolated material alpha is attenuated.
inline constexpr std::string_view fixed_mesh_shader_alpha_fog = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, blend_mix, depth_draw_never, depth_test_default, cull_back;

uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Emissive = vec4(0.0, 0.0, 0.0, 1.0);
uniform vec4 Diffuse = vec4(1.0);
uniform mat4 eawr_sph_r;
uniform mat4 eawr_sph_g;
uniform mat4 eawr_sph_b;
uniform vec4 eawr_light_scale = vec4(1.0);
varying vec4 eawr_vertex_diffuse;
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
varying vec3 eawr_fog_world_position;

float eawr_fog_attenuation() {
    vec2 source = vec2(eawr_fog_world_position.x, -eawr_fog_world_position.z);
    vec2 uv = (source - eawr_fog_origin) / eawr_fog_extent;
    float attenuation = 0.0;
    if (eawr_fog_bound && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0) {
        vec2 texel = min(floor(uv * eawr_fog_size), eawr_fog_size - vec2(1.0));
        attenuation = texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
    }
    return attenuation;
}

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
    vec3 normal_world = normalize(mat3(MODEL_MATRIX) * NORMAL);
    vec4 normal_h = vec4(normal_world, 1.0);
    vec3 irradiance = vec3(
        dot(normal_h, eawr_sph_r * normal_h),
        dot(normal_h, eawr_sph_g * normal_h),
        dot(normal_h, eawr_sph_b * normal_h));
    eawr_vertex_diffuse = vec4(
        Diffuse.rgb * irradiance * eawr_light_scale.rgb + Emissive.rgb,
        Diffuse.a * eawr_light_scale.a);
    eawr_fog_world_position = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}

void fragment() {
    vec4 base_sample = texture(BaseTexture, UV);
    vec3 base_linear_rgb = eawr_srgb_to_linear(base_sample.rgb);
    vec3 eawr_linear_rgb = 2.0 * eawr_vertex_diffuse.rgb * base_linear_rgb;
    eawr_linear_rgb *= eawr_fog_attenuation();
    ALBEDO = eawr_linear_to_srgb(eawr_linear_rgb);
    ALPHA = base_sample.a * eawr_vertex_diffuse.a;
}
)GODOT";

// The ALBEDO writer of the adapters whose product is already a stored value
// (MeshAdditive, MeshAdditiveOffset, the solid-colour pass and the space sky
// MeshAdditive route). Under the stored-value output of docs/rendering.md,
// ALBEDO is the stored value. Each such source contains this text once.
inline constexpr std::string_view stored_albedo_writer = R"GODOT(vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}
)GODOT";

// Its Compatibility-fallback form, the P1-06 fitted compensation (not an
// engine fact): on the pinned Godot 4.7.2 Compatibility backend, measured
// ALBEDO round trips darken stored values below about 60/255, and a cubic
// approximation of the sRGB decode paired with an exact re-encode fits those
// measurements. Newton steps on the monotonic cubic give the ALBEDO whose
// measured round trip is the stored value.
inline constexpr std::string_view compatibility_albedo_writer = R"GODOT(vec3 eawr_compatibility_srgb_to_linear(vec3 encoded_rgb) {
    vec3 nonnegative = max(encoded_rgb, vec3(0.0));
    vec3 low = nonnegative / 12.92;
    vec3 high = pow((nonnegative + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(nonnegative, vec3(0.04045)));
}

vec3 eawr_stored_albedo(vec3 stored_rgb) {
    vec3 target = eawr_compatibility_srgb_to_linear(clamp(stored_rgb, 0.0, 1.0));
    vec3 guess = clamp(stored_rgb, 0.0, 1.0);
    for (int step = 0; step < 6; step++) {
        vec3 value = guess * (guess * (guess * 0.305306011 + 0.682171111) + 0.012522878) - target;
        vec3 slope = guess * (guess * 0.915918033 + 1.364342222) + 0.012522878;
        guess = clamp(guess - value / slope, 0.0, 1.0);
    }
    return guess;
}
)GODOT";

// `source` as the Compatibility fallback compiles it (--rendering-method
// gl_compatibility, until FP-5): the stored-value ALBEDO writer, where the
// source has one, becomes the compensated writer. Every other source is
// compiled as written, since stored and Compatibility semantics agree there.
[[nodiscard]] inline std::string compatibility_source(std::string source) {
    const std::size_t at = source.find(stored_albedo_writer);
    if (at != std::string::npos) source.replace(at, stored_albedo_writer.size(), compatibility_albedo_writer);
    return source;
}

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

} // namespace eawr::presentation::godot_backend
