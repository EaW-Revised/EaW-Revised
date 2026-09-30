#pragma once

// Private to land_look.cpp: the embedded terrain, sky, sun and water shader sources.
#include <string_view>

namespace eawr::presentation::godot_backend::land_look {

// ---- shaders (clean-room modern_spatial adapters) -------------------------------

// The layer blend: each fragment finds its cell's four corner samples, reads
// each one's layer, and mixes those layers' texels by the bilinear weights, so
// the result is the BlendMap reference arithmetic (terrain::blend_weights).
// Each layer's texture coordinates are the retail TexU/TexV of its mapping
// (tile size, rotation, tilt; terrain::retail_texgen) dotted with the source
// position, on flat and steep ground alike; explicit gradients keep the mip
// level continuous across cell edges where a corner's layer changes.
constexpr std::string_view blend_declarations = R"GODOT(
uniform sampler2DArray eawr_layers : filter_linear_mipmap_anisotropic, repeat_enable;
uniform sampler2D eawr_layer_index : filter_nearest, repeat_disable;
uniform sampler2D eawr_layer_params : filter_nearest, repeat_disable;
uniform vec4 eawr_grid = vec4(2.0, 2.0, 20.0, 0.0);
varying vec3 eawr_source_position;
float eawr_u16(vec2 bytes) {
    vec2 value = floor(bytes * 255.0 + 0.5);
    return value.x * 256.0 + value.y;
}
vec3 eawr_layer_texel(int layer, vec3 dx, vec3 dy) {
    vec4 packed = texelFetch(eawr_layer_params, ivec2(layer, 0), 0);
    vec4 extra = texelFetch(eawr_layer_params, ivec2(layer, 1), 0);
    vec3 tint = texelFetch(eawr_layer_params, ivec2(layer, 2), 0).rgb;
    float tile = max(eawr_u16(packed.rg) / 16.0, 1.0);
    float angle = eawr_u16(packed.ba) / 65535.0 * 6.28318530718;
    float tilt = eawr_u16(extra.rg) / 65535.0 * 6.28318530718;
    vec3 u_axis = vec3(cos(angle), -sin(angle), 0.0) / tile;
    vec3 v_axis = vec3(sin(angle) * cos(tilt), cos(angle) * cos(tilt), -sin(tilt)) / tile;
    vec2 uv = vec2(dot(u_axis, eawr_source_position), dot(v_axis, eawr_source_position));
    vec2 gradient_x = vec2(dot(u_axis, dx), dot(v_axis, dx));
    vec2 gradient_y = vec2(dot(u_axis, dy), dot(v_axis, dy));
    return textureGrad(eawr_layers, vec3(uv, float(layer)), gradient_x, gradient_y).rgb * tint;
}
int eawr_layer_at(ivec2 grid_sample) {
    return int(texelFetch(eawr_layer_index, grid_sample, 0).r * 255.0 + 0.5);
}
vec3 eawr_terrain_albedo() {
    vec3 dx = dFdx(eawr_source_position);
    vec3 dy = dFdy(eawr_source_position);
    vec2 grid = eawr_source_position.xy / eawr_grid.z;
    vec2 cell = clamp(floor(grid), vec2(0.0), eawr_grid.xy - vec2(2.0));
    vec2 f = clamp(grid - cell, vec2(0.0), vec2(1.0));
    ivec2 base = ivec2(cell);
    int l00 = eawr_layer_at(base);
    int l10 = eawr_layer_at(base + ivec2(1, 0));
    int l01 = eawr_layer_at(base + ivec2(0, 1));
    int l11 = eawr_layer_at(base + ivec2(1, 1));
    float w00 = (1.0 - f.x) * (1.0 - f.y);
    float w10 = f.x * (1.0 - f.y);
    float w01 = (1.0 - f.x) * f.y;
    float w11 = f.x * f.y;
    // Corners naming the same layer share one sample.
    if (l10 == l00) { w00 += w10; w10 = 0.0; }
    if (l01 == l00) { w00 += w01; w01 = 0.0; }
    if (l11 == l00) { w00 += w11; w11 = 0.0; }
    if (w01 > 0.0 && l01 == l10) { w10 += w01; w01 = 0.0; }
    if (w11 > 0.0 && l11 == l10) { w10 += w11; w11 = 0.0; }
    if (w11 > 0.0 && l11 == l01) { w01 += w11; w11 = 0.0; }
    vec3 color = eawr_layer_texel(l00, dx, dy) * w00;
    if (w10 > 0.0) color += eawr_layer_texel(l10, dx, dy) * w10;
    if (w01 > 0.0) color += eawr_layer_texel(l01, dx, dy) * w01;
    if (w11 > 0.0) color += eawr_layer_texel(l11, dx, dy) * w11;
    return color;
}
)GODOT";

// A dome drawn around the eye just inside the far plane: the uploaded vertices
// are unit directions scaled far out (so the instance is never culled), and
// the vertex stage keeps only their direction, rotated into view space, at
// eawr_sky.x of the far distance (read back from the projection, so any
// camera works). Base and cloud are mixed as Skydome.fx does, with the cloud
// alpha; the sky is unlit in every lighting policy (its SH term is #25's), and
// clouds do not scroll so fixed captures stay repeatable. The transparent pass
// samples reverse-Z scene depth and fills only pixels with no opaque geometry.
constexpr std::string_view dome_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_mix, skip_vertex_transform;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
uniform sampler2D eawr_clouds : filter_linear_mipmap, repeat_enable;
uniform sampler2D eawr_scene_depth : hint_depth_texture, filter_nearest;
uniform vec4 eawr_sky = vec4(0.9, 1.0, 0.0, 0.0);
void vertex() {
    vec4 far_a = INV_PROJECTION_MATRIX * vec4(0.0, 0.0, 1.0, 1.0);
    vec4 far_b = INV_PROJECTION_MATRIX * vec4(0.0, 0.0, 0.0, 1.0);
    float far_distance = max(abs(far_a.z / far_a.w), abs(far_b.z / far_b.w));
    VERTEX = mat3(VIEW_MATRIX) * (normalize(VERTEX) * far_distance * eawr_sky.x);
}
void fragment() {
    if (texture(eawr_scene_depth, SCREEN_UV).r > 0.00001) discard;
    vec3 base = texture(eawr_diffuse, UV).rgb;
    vec4 cloud = texture(eawr_clouds, UV * eawr_sky.y);
    ALBEDO = mix(base, cloud.rgb, cloud.a * eawr_sky.w);
}
)GODOT";

// Mode-7 sun (docs/behaviour/meshadditive-sun-mode7-retail.md, approximated):
// the quad sits at the eye plus the bone's distance along the direction toward
// light 0 and faces the camera; MeshAdditive's texel x Color, added. A sun
// farther than the dome is pulled in with its size scaled alike, so its
// angular size is kept.
constexpr std::string_view sun_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_add, skip_vertex_transform;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_disable;
uniform vec4 eawr_sun_color = vec4(1.0);
uniform vec4 eawr_sun = vec4(0.0, 1.0, 0.0, 1000.0);
uniform float eawr_sun_limit = 0.85;
void vertex() {
    vec4 far_a = INV_PROJECTION_MATRIX * vec4(0.0, 0.0, 1.0, 1.0);
    vec4 far_b = INV_PROJECTION_MATRIX * vec4(0.0, 0.0, 0.0, 1.0);
    float far_distance = max(abs(far_a.z / far_a.w), abs(far_b.z / far_b.w));
    float scale = min(1.0, far_distance * eawr_sun_limit / eawr_sun.w);
    vec3 centre = mat3(VIEW_MATRIX) * (normalize(eawr_sun.xyz) * eawr_sun.w * scale);
    VERTEX = centre + vec3(VERTEX.x, VERTEX.y, 0.0) * scale;
}
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb * eawr_sun_color.rgb;
}
)GODOT";

// Approximate water: a textured, tinted, alpha-blended surface. Rivers fade
// to nothing at both banks over the outer quarter of their width.
constexpr std::string_view water_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never, blend_mix;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
uniform vec4 eawr_water_color = vec4(1.0, 1.0, 1.0, 0.5);
uniform float eawr_bank_fade = 0.0;
uniform float eawr_flow_rate = 0.0;
uniform float eawr_texture_alpha = 0.0;
uniform float eawr_flow_time = 0.0;
uniform float eawr_flow_animate = 0.0;
uniform float eawr_water_ribbon = 0.0;
void fragment() {
    vec4 texel = texture(eawr_diffuse,
        UV + vec2(0.0, (eawr_flow_time + TIME * eawr_flow_animate) * eawr_flow_rate));
    float bank = mix(1.0, smoothstep(0.0, 0.25, UV.x) * smoothstep(1.0, 0.75, UV.x), eawr_bank_fade);
    float fall = eawr_water_ribbon * smoothstep(0.25, 0.8, COLOR.r);
    float sheen = fall * smoothstep(0.48, 0.68, UV.x) *
        (1.0 - smoothstep(0.78, 0.92, UV.x)) * (0.65 + 0.35 * texel.r);
    ALBEDO = mix(texel.rgb * eawr_water_color.rgb, vec3(0.95, 0.96, 1.0), sheen);
    ALPHA = bank * mix(1.0, texel.a, eawr_texture_alpha) *
        min(1.0, eawr_water_color.a + sheen * 0.6);
}
)GODOT";

} // namespace eawr::presentation::godot_backend::land_look
