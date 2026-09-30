#pragma once

// The DEFEND shield shell (#427, docs/behaviour/battle-presentation.md BP-22 to BP-24): a
// ship's SHIELD sub-object drawn with its MeshShield.fx material while its DEFEND ability runs.

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend {

// Clean-room modern_spatial adapter for MeshShield.fx t0/t0_p0 (the public FoC shader source,
// Shield.fxh distort_ps_main and MeshShield.fx sph_vs_main). Per vertex: three UV sets scaled
// from the authored UV and scrolled along v by the effect clock (m_time), and the diffuse
// vertex colour x saturate(N.z + EdgeBrightness) x Color, where N is the model-space normal and
// source +Z is render +Y (the (x, y, z) -> (x, z, -y) upload conversion). The source feeds
// TEXCOORD1 from DistortUV* and TEXCOORD2 from WaveUV*, and the pixel shader samples the
// distortion texture with TEXCOORD2 and the wave texture with TEXCOORD1; that crossing is kept.
// Per pixel: the energy texel at the base UV offset by 0.25 x the distortion texel's rg, times
// the wave texel, times the diffuse. State (t0_p0): ONE/ONE additive, no depth write, depth test
// LESSEQUAL, no cull, no fog. Stored values throughout, like the MeshAdditive sky adapter:
// texels without sRGB decode, ALBEDO the stored value (docs/rendering.md, colour policy). The
// clock is eawr_shield_time, which no engine clock writes (Godot's TIME is not read).
inline constexpr std::string_view meshshield_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, fog_disabled, cull_disabled, depth_draw_never, blend_add;
uniform sampler2D BaseTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D WaveTexture : filter_linear_mipmap, repeat_enable;
uniform sampler2D DistortionTexture : filter_linear_mipmap, repeat_enable;
uniform vec4 Color;
uniform float EdgeBrightness;
uniform float BaseUVScale;
uniform float WaveUVScale;
uniform float DistortUVScale;
uniform float BaseUVScrollRate;
uniform float WaveUVScrollRate;
uniform float DistortUVScrollRate;
uniform float eawr_shield_time;
varying vec2 eawr_tex0;
varying vec2 eawr_tex1;
varying vec2 eawr_tex2;
varying vec4 eawr_diffuse;

void vertex() {
    eawr_tex0 = BaseUVScale * UV + vec2(0.0, eawr_shield_time * BaseUVScrollRate);
    eawr_tex1 = DistortUVScale * UV + vec2(0.0, eawr_shield_time * DistortUVScrollRate);
    eawr_tex2 = WaveUVScale * UV + vec2(0.0, eawr_shield_time * WaveUVScrollRate);
    // A vs_1_1 colour output saturates.
    eawr_diffuse = clamp(COLOR * clamp(NORMAL.y + EdgeBrightness, 0.0, 1.0) * Color, 0.0, 1.0);
}

// ALBEDO under the stored-value output (docs/rendering.md, colour policy);
// the Compatibility fallback compiles compatibility_albedo_writer instead.
vec3 eawr_stored_albedo(vec3 stored_rgb) {
    return stored_rgb;
}

void fragment() {
    vec4 distortion_texel = texture(DistortionTexture, eawr_tex2);
    vec4 texel = texture(BaseTexture, eawr_tex0 + distortion_texel.xy * 0.25);
    vec4 wave_texel = texture(WaveTexture, eawr_tex1);
    ALBEDO = eawr_stored_albedo((texel * wave_texel * eawr_diffuse).rgb);
}
)GODOT";

inline constexpr std::string_view meshshield_program = "MeshShield.fx";
// The material's effect clock binding (seconds).
inline constexpr std::string_view meshshield_time_binding = "eawr_shield_time";

[[nodiscard]] inline bool shield_name_equal(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

// The first sub-object named SHIELD, case ignored (BP-17, the debug build), or nullopt.
[[nodiscard]] inline std::optional<std::uint32_t> shield_mesh_index(const assets::Model& model) {
    for (std::uint32_t index = 0; index < model.meshes.size(); ++index) {
        if (shield_name_equal(model.meshes[index].name, "shield")) return index;
    }
    return std::nullopt;
}

// A MeshShield submesh's material: its authored parameters (the three textures by name, bound
// by the caller) and the clock at 0.
[[nodiscard]] inline MaterialDescription meshshield_material(const assets::Submesh& submesh) {
    MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::transparent,
        .program = std::string(meshshield_shader),
        .technique = {},
        .pass_name = {},
        .bindings = {},
    };
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        material.bindings.push_back({parameter.name, parameter.value});
    }
    material.bindings.push_back({std::string(meshshield_time_binding), 0.0F});
    return material;
}

} // namespace eawr::presentation::godot_backend
