#include "particle_adapter.hpp"
#include "particle_texture.hpp"

#include "eawr/presentation/lighting/lighting.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

// Clean-room Godot spatial programs, one per blend policy. Each reproduces the
// render state of the public engine effect the legacy selector names (see
// particles::legacy_blend_selectors): the colour is texel times vertex colour,
// and the blend equation, depth write and depth test come from that effect's
// active technique. They are unshaded because the Prim* particle effects apply
// no lighting. `%DEPTH_TEST%` is replaced by the per-emitter noDepthTest flag.
constexpr std::string_view additive_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_add, depth_draw_never%DEPTH_TEST%;
uniform sampler2D eawr_particle_texture : filter_linear_mipmap, repeat_disable;
void fragment() {
    vec4 pixel = texture(eawr_particle_texture, UV) * COLOR;
    // SrcBlend ONE, DestBlend ONE: Godot's additive mode scales the source by
    // ALPHA, so a unit alpha reproduces the effect's ONE/ONE equation.
    ALBEDO = pixel.rgb;
    ALPHA = 1.0;
}
)GODOT";

constexpr std::string_view alpha_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never%DEPTH_TEST%;
uniform sampler2D eawr_particle_texture : filter_linear_mipmap, repeat_disable;
void fragment() {
    vec4 pixel = texture(eawr_particle_texture, UV) * COLOR;
    ALBEDO = pixel.rgb;
    ALPHA = pixel.a;
}
)GODOT";

constexpr std::string_view modulate_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mul, depth_draw_never%DEPTH_TEST%;
uniform sampler2D eawr_particle_texture : filter_linear_mipmap, repeat_disable;
void fragment() {
    // SrcBlend ZERO, DestBlend SRCCOLOR: destination times source colour.
    vec4 pixel = texture(eawr_particle_texture, UV) * COLOR;
    ALBEDO = pixel.rgb;
    ALPHA = 1.0;
}
)GODOT";

constexpr std::string_view opaque_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_opaque%DEPTH_TEST%;
uniform sampler2D eawr_particle_texture : filter_linear_mipmap, repeat_disable;
void fragment() {
    ALBEDO = (texture(eawr_particle_texture, UV) * COLOR).rgb;
}
)GODOT";

constexpr std::string_view heat_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never%DEPTH_TEST%;
uniform sampler2D eawr_particle_texture : filter_linear_mipmap, repeat_disable;
uniform sampler2D eawr_scene : hint_screen_texture, filter_linear, repeat_disable;
void fragment() {
    // The texture's red/green channels are a signed screen-space offset and its
    // alpha, scaled by vertex alpha, attenuates it; the pixel is replaced by
    // the offset scene sample with blending disabled (PrimHeat technique t2).
    vec4 texel = texture(eawr_particle_texture, UV);
    vec2 distort = 2.0 * (texel.xy - vec2(0.5));
    float attenuation = texel.a * COLOR.a;
    ALBEDO = texture(eawr_scene, SCREEN_UV + attenuation * %HEAT_DISTORTION% * distort).rgb;
    // The screen copy predates the transparent pass, so the distorted sample
    // is weighted by the mask instead of replacing the whole quad; outside the
    // mask the transparents already drawn stay untouched.
    ALPHA = clamp(attenuation, 0.0, 1.0);
}
)GODOT";

// PrimParticleBumpAlpha t2, the technique retail selects at Highest (FoC
// shader source). With bump support the engine replaces each vertex's colour
// RGB by the particle's tangent, so the authored colour keys reach only alpha.
// The vertex stage evaluates SPH_LIGHT_FILL once at the camera-facing normal
// (a vs_1_1 colour, so clamped to [0, 1]); per pixel, with n the normal map
// in the camera-facing tangent frame, RGB = 2 base (saturate(n.L) light-0
// diffuse + fill) + light-0 specular saturate(n.H)^5 normal.a and alpha is
// vertex alpha times base alpha. Arithmetic runs on stored texel values, as
// the legacy mesh adapters do.
constexpr std::string_view bump_alpha_program = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, blend_mix, depth_draw_never%DEPTH_TEST%;
uniform sampler2D eawr_particle_texture : filter_linear_mipmap, repeat_disable;
uniform sampler2D eawr_particle_normal : filter_linear_mipmap, repeat_disable;
uniform bool eawr_particle_has_normal = false;
// Light 0 toward the light in the render (Y-up) world basis, its colours, and
// the SPH_LIGHT_FILL matrices in the render basis.
uniform vec3 eawr_particle_light = vec3(0.0, 1.0, 0.0);
uniform vec3 eawr_particle_light_diffuse = vec3(2.0, 1.88, 1.72);
uniform vec3 eawr_particle_light_specular = vec3(2.0, 1.88, 1.72);
uniform mat4 eawr_particle_fill_r;
uniform mat4 eawr_particle_fill_g;
uniform mat4 eawr_particle_fill_b;
void fragment() {
    vec4 base = texture(eawr_particle_texture, UV);
    // Tangent frame of the quad from view-space and UV derivatives, so no
    // tangent stream is needed; the face normal points at the camera.
    vec3 dp1 = dFdx(VERTEX);
    vec3 dp2 = dFdy(VERTEX);
    vec2 duv1 = dFdx(UV);
    vec2 duv2 = dFdy(UV);
    vec3 face = normalize(cross(dp1, dp2));
    if (face.z < 0.0) face = -face;
    vec3 normal = face;
    float gloss = 0.0;
    if (eawr_particle_has_normal) {
        vec3 dp2perp = cross(dp2, face);
        vec3 dp1perp = cross(face, dp1);
        vec3 tangent = dp2perp * duv1.x + dp1perp * duv2.x;
        vec3 bitangent = dp2perp * duv1.y + dp1perp * duv2.y;
        float scale = inversesqrt(max(max(dot(tangent, tangent), dot(bitangent, bitangent)), 1e-20));
        vec4 normal_texel = texture(eawr_particle_normal, UV);
        vec3 texel = normal_texel.xyz * 2.0 - 1.0;
        // Texture v grows downward while the map's green axis points up.
        normal = normalize(texel.x * tangent * scale - texel.y * bitangent * scale + texel.z * face);
        gloss = normal_texel.a;
    }
    vec3 light = normalize((VIEW_MATRIX * vec4(eawr_particle_light, 0.0)).xyz);
    vec3 half_vector = normalize(VIEW + light);
    float n_dot_l = clamp(dot(normal, light), 0.0, 1.0);
    float n_dot_h = clamp(dot(normal, half_vector), 0.0, 1.0);
    // The camera-facing normal in world space: the view basis Z axis.
    vec4 facing = vec4(normalize(INV_VIEW_MATRIX[2].xyz), 1.0);
    vec3 fill = clamp(vec3(dot(facing, eawr_particle_fill_r * facing),
        dot(facing, eawr_particle_fill_g * facing), dot(facing, eawr_particle_fill_b * facing)), 0.0, 1.0);
    vec3 specular = eawr_particle_light_specular * pow(n_dot_h, 5.0) * gloss;
    ALBEDO = min(2.0 * base.rgb * (n_dot_l * eawr_particle_light_diffuse + fill) + specular, vec3(1.0));
    ALPHA = base.a * COLOR.a;
}
)GODOT";

[[nodiscard]] std::string program_source(const particles::EmitterRenderPlan& plan, const bool fog) {
    std::string_view base;
    switch (plan.blend) {
    case particles::Blend::opaque: base = opaque_program; break;
    case particles::Blend::additive: base = additive_program; break;
    case particles::Blend::alpha: base = alpha_program; break;
    case particles::Blend::modulate: base = modulate_program; break;
    case particles::Blend::heat_distortion: base = heat_program; break;
    case particles::Blend::bump_alpha: base = bump_alpha_program; break;
    }
    std::string source(base);
    constexpr std::string_view marker = "%DEPTH_TEST%";
    const std::size_t position = source.find(marker);
    if (position == std::string::npos) return {};
    source.replace(position, marker.size(), plan.depth_test ? "" : ", depth_test_disabled");
    constexpr std::string_view heat_marker = "%HEAT_DISTORTION%";
    if (const std::size_t heat = source.find(heat_marker); heat != std::string::npos) {
        source.replace(heat, heat_marker.size(), std::to_string(GodotParticleBackend::heat_distortion_amount));
    }
    if (fog && plan.blend != particles::Blend::heat_distortion) {
        constexpr std::string_view fog_source = R"GODOT(
uniform sampler2D eawr_fog_texture : filter_nearest, repeat_disable;
uniform vec2 eawr_fog_origin = vec2(0.0);
uniform vec2 eawr_fog_extent = vec2(1.0);
uniform vec2 eawr_fog_size = vec2(1.0);
uniform bool eawr_fog_bound = false;
varying vec3 eawr_particle_world;
void vertex() {
    eawr_particle_world = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
float eawr_particle_fog() {
    vec2 source_xy = vec2(eawr_particle_world.x, -eawr_particle_world.z);
    vec2 cell = (source_xy - eawr_fog_origin) / eawr_fog_extent;
    if (!eawr_fog_bound || cell.x < 0.0 || cell.y < 0.0 || cell.x >= 1.0 || cell.y >= 1.0) return 0.0;
    vec2 texel = min(floor(cell * eawr_fog_size), eawr_fog_size - vec2(1.0));
    return texture(eawr_fog_texture, (texel + vec2(0.5)) / eawr_fog_size).r;
}
)GODOT";
        const std::size_t fragment = source.find("void fragment()");
        const std::size_t fragment_end = source.rfind("\n}");
        if (fragment == std::string::npos || fragment_end == std::string::npos
            || fragment_end <= fragment) return {};
        source.insert(fragment_end, "\n    ALBEDO *= eawr_particle_fog();");
        source.insert(fragment, fog_source);
    }
    return source;
}

[[nodiscard]] Vector3 axis_convert(const particles::Vec3 value) { return {value.x, value.z, -value.y}; }

[[nodiscard]] std::optional<Image::Format> image_format(const assets::PixelFormat format) {
    switch (format) {
    case assets::PixelFormat::rgba8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgra8: return Image::FORMAT_RGBA8;
    case assets::PixelFormat::bgr8: return Image::FORMAT_RGB8;
    case assets::PixelFormat::l8: return Image::FORMAT_L8;
    case assets::PixelFormat::a8: return Image::FORMAT_LA8;
    case assets::PixelFormat::bc1: return Image::FORMAT_DXT1;
    case assets::PixelFormat::bc2: return Image::FORMAT_DXT3;
    case assets::PixelFormat::bc3: return Image::FORMAT_DXT5;
    case assets::PixelFormat::bc4: return Image::FORMAT_RGTC_R;
    case assets::PixelFormat::bc5: return Image::FORMAT_RGTC_RG;
    case assets::PixelFormat::bc7: return Image::FORMAT_BPTC_RGBA;
    }
    return std::nullopt;
}

} // namespace

class GodotParticleBackend::Impl final {
public:
    struct Shared final {
        RID rid;
        std::size_t references{};
    };
    struct Emitter final {
        std::string texture_key;
        std::string normal_key;
        std::string shader_key;
        RID material;
        RID mesh;
        RID instance;
        std::uint64_t fog_handle{};
        std::size_t emitter_index{};
        std::size_t quads{};
        bool bump{};
        std::vector<std::pair<float, float>> source_points;
    };

    Impl(Node3D& host, TextureResolver resolver, GodotParticleBackend::FogCallbacks fog)
        : resolver_(std::move(resolver)), fog_(std::move(fog)) {
        if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
    }

    ~Impl() {
        while (!emitters_.empty()) destroy(emitters_.begin()->first);
    }

    std::uint64_t create(const particles::EmitterRenderPlan& plan) {
        failure_.clear();
        RenderingServer* rendering = RenderingServer::get_singleton();
        if (rendering == nullptr || !scenario_.is_valid()) {
            failure_ = "RenderingServer or scenario is unavailable";
            return 0;
        }
        if (!plan.drawable) {
            failure_ = "plan is not drawable: " + plan.cause;
            return 0;
        }
        if (static_cast<bool>(fog_.register_material) != static_cast<bool>(fog_.unregister_material)) {
            failure_ = "fog registration callbacks must include both register and unregister";
            return 0;
        }
        if (fog_.register_material && (plan.blend == particles::Blend::heat_distortion
            || !plan.depth_test)) {
            failure_ = "emitter " + std::to_string(plan.emitter_index)
                + ": fog-stub-v1 does not support heat/screen distortion or disabled depth test";
            return 0;
        }
        const MaterialDescription material = GodotParticleBackend::material_for(plan,
            static_cast<bool>(fog_.register_material));
        if (const auto valid = validate_material(material); !valid) {
            failure_ = valid.error().message;
            return 0;
        }
        const assets::Texture* texture = resolver_ ? resolver_(plan.texture) : nullptr;
        if (texture == nullptr) {
            failure_ = "colour texture '" + plan.texture + "' did not resolve";
            return 0;
        }
        const std::optional<RID> texture_rid = acquire_texture(*rendering, plan.texture, *texture);
        if (!texture_rid) {
            failure_ = "colour texture '" + plan.texture + "': " + failure_;
            return 0;
        }
        // A bump emitter without a declared normal map is lit as a flat quad;
        // a declared map that does not resolve fails like the colour texture.
        std::optional<RID> normal_rid;
        const bool bump = plan.blend == particles::Blend::bump_alpha;
        if (bump && !plan.normal_texture.empty()) {
            const assets::Texture* normal = resolver_ ? resolver_(plan.normal_texture) : nullptr;
            if (normal != nullptr) normal_rid = acquire_texture(*rendering, plan.normal_texture, *normal);
            if (!normal_rid) {
                failure_ = "normal texture '" + plan.normal_texture + "' did not resolve"
                    + (normal != nullptr ? ": " + failure_ : std::string{});
                release_texture(*rendering, plan.texture);
                return 0;
            }
        }
        const std::string normal_key = normal_rid ? plan.normal_texture : std::string{};
        const auto release_textures = [&] {
            release_texture(*rendering, plan.texture);
            if (!normal_key.empty()) release_texture(*rendering, normal_key);
        };
        const std::string shader_key = material.program;
        const std::optional<RID> shader = acquire_shader(*rendering, shader_key);
        if (!shader) {
            release_textures();
            failure_ = "Godot rejected the particle program (EAWR-RENDER-0007)";
            return 0;
        }
        Emitter emitter;
        emitter.texture_key = plan.texture;
        emitter.normal_key = normal_key;
        emitter.shader_key = shader_key;
        emitter.emitter_index = plan.emitter_index;
        emitter.material = rendering->material_create();
        if (!emitter.material.is_valid()) {
            release_shader(*rendering, shader_key);
            release_textures();
            failure_ = "Godot rejected the particle material RID";
            return 0;
        }
        rendering->material_set_shader(emitter.material, *shader);
        // Emitters are ordered inside the public pass range; the spacing of
        // eight between pass priorities keeps them from crossing a boundary.
        rendering->material_set_render_priority(emitter.material,
            render_pass_priority(material.pass)
                + static_cast<std::int32_t>(std::min<std::uint32_t>(plan.order_in_phase, 7U)));
        rendering->material_set_param(emitter.material, StringName("eawr_particle_texture"), *texture_rid);
        if (bump) {
            if (normal_rid) {
                rendering->material_set_param(emitter.material, StringName("eawr_particle_normal"), *normal_rid);
                rendering->material_set_param(emitter.material, StringName("eawr_particle_has_normal"), true);
            }
            apply_lighting(*rendering, emitter.material);
            emitter.bump = true;
        }
        emitter.mesh = rendering->mesh_create();
        emitter.instance = rendering->instance_create();
        if (!emitter.mesh.is_valid() || !emitter.instance.is_valid()) {
            if (emitter.instance.is_valid()) rendering->free_rid(emitter.instance);
            if (emitter.mesh.is_valid()) rendering->free_rid(emitter.mesh);
            rendering->free_rid(emitter.material);
            release_shader(*rendering, shader_key);
            release_textures();
            failure_ = "Godot rejected a particle mesh or instance RID";
            return 0;
        }
        rendering->instance_set_base(emitter.instance, emitter.mesh);
        rendering->instance_set_scenario(emitter.instance, scenario_);
        rendering->instance_geometry_set_cast_shadows_setting(
            emitter.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
        if (fog_.register_material) {
            const auto registered = fog_.register_material(emitter.material, *shader);
            if (!registered || registered.value() == 0) {
                failure_ = "emitter " + std::to_string(plan.emitter_index) + ": "
                    + (registered ? "fog callback returned an empty consumer handle"
                        : core::format_diagnostic(registered.error()));
                rendering->free_rid(emitter.instance);
                rendering->free_rid(emitter.mesh);
                rendering->free_rid(emitter.material);
                release_shader(*rendering, shader_key);
                release_textures();
                return 0;
            }
            emitter.fog_handle = registered.value();
        }
        const std::uint64_t id = next_id_++;
        emitters_.emplace(id, std::move(emitter));
        return id;
    }

    void update(const std::uint64_t id, const particles::VertexStream& stream) {
        RenderingServer* rendering = RenderingServer::get_singleton();
        const auto found = emitters_.find(id);
        if (rendering == nullptr || found == emitters_.end()) return;
        Emitter& emitter = found->second;
        rendering->mesh_clear(emitter.mesh);
        emitter.quads = stream.quads;
        emitter.source_points.clear();
        if (stream.quads == 0 || stream.vertices.empty()) return;
        if (fog_.attenuation_at_source_xy) emitter.source_points.reserve(stream.vertices.size());
        PackedVector3Array vertices;
        PackedColorArray colors;
        PackedVector2Array uv;
        PackedInt32Array indices;
        vertices.resize(static_cast<int64_t>(stream.vertices.size()));
        colors.resize(static_cast<int64_t>(stream.vertices.size()));
        uv.resize(static_cast<int64_t>(stream.vertices.size()));
        for (std::size_t index = 0; index < stream.vertices.size(); ++index) {
            const particles::ParticleVertex& vertex = stream.vertices[index];
            vertices.set(static_cast<int64_t>(index), axis_convert(vertex.position));
            colors.set(static_cast<int64_t>(index),
                Color(vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w));
            uv.set(static_cast<int64_t>(index), Vector2(vertex.u, vertex.v));
            if (fog_.attenuation_at_source_xy) {
                emitter.source_points.emplace_back(vertex.position.x, vertex.position.y);
            }
        }
        indices.resize(static_cast<int64_t>(stream.indices.size()));
        for (std::size_t index = 0; index < stream.indices.size(); ++index) {
            indices.set(static_cast<int64_t>(index), static_cast<std::int32_t>(stream.indices[index]));
        }
        Array arrays;
        arrays.resize(RenderingServer::ARRAY_MAX);
        arrays[RenderingServer::ARRAY_VERTEX] = vertices;
        arrays[RenderingServer::ARRAY_COLOR] = colors;
        arrays[RenderingServer::ARRAY_TEX_UV] = uv;
        arrays[RenderingServer::ARRAY_INDEX] = indices;
        rendering->mesh_add_surface_from_arrays(emitter.mesh, RenderingServer::PRIMITIVE_TRIANGLES, arrays);
        rendering->mesh_surface_set_material(emitter.mesh, 0, emitter.material);
    }

    void destroy(const std::uint64_t id) {
        const auto found = emitters_.find(id);
        if (found == emitters_.end()) return;
        RenderingServer* rendering = RenderingServer::get_singleton();
        if (found->second.fog_handle && fog_.unregister_material)
            fog_.unregister_material(found->second.fog_handle);
        if (rendering != nullptr) {
            const Emitter& emitter = found->second;
            if (emitter.instance.is_valid()) rendering->free_rid(emitter.instance);
            if (emitter.mesh.is_valid()) rendering->free_rid(emitter.mesh);
            if (emitter.material.is_valid()) rendering->free_rid(emitter.material);
            release_shader(*rendering, emitter.shader_key);
            release_texture(*rendering, emitter.texture_key);
            if (!emitter.normal_key.empty()) release_texture(*rendering, emitter.normal_key);
        }
        emitters_.erase(found);
    }

    void set_lighting(const GodotParticleBackend::BumpLighting& lighting) {
        if (lighting == lighting_) return;
        lighting_ = lighting;
        RenderingServer* rendering = RenderingServer::get_singleton();
        if (rendering == nullptr) return;
        for (const auto& [id, emitter] : emitters_) {
            if (emitter.bump) apply_lighting(*rendering, emitter.material);
        }
    }

    [[nodiscard]] std::size_t live_rids() const noexcept {
        return emitters_.size() * 3 + textures_.size() + shaders_.size();
    }
    [[nodiscard]] std::size_t live_emitters() const noexcept { return emitters_.size(); }
    [[nodiscard]] std::vector<GodotParticleBackend::FogEmitterEvidence> fog_emitter_evidence() const {
        std::vector<GodotParticleBackend::FogEmitterEvidence> result;
        for (const auto& [resource, emitter] : emitters_) {
            std::uint8_t minimum = fog_.attenuation_at_source_xy ? 0 : 255;
            std::uint8_t maximum = minimum;
            if (fog_.attenuation_at_source_xy && !emitter.source_points.empty()) {
                minimum = 255;
                maximum = 0;
                for (const auto [x, y] : emitter.source_points) {
                    const std::uint8_t byte = fog_.attenuation_at_source_xy(x, y);
                    minimum = std::min(minimum, byte);
                    maximum = std::max(maximum, byte);
                }
            }
            result.push_back({resource, emitter.fog_handle, emitter.emitter_index, emitter.quads,
                minimum, maximum});
        }
        return result;
    }
    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }

private:
    std::optional<RID> acquire_texture(RenderingServer& rendering, const std::string& key,
                                       const assets::Texture& source) {
        if (auto found = textures_.find(key); found != textures_.end()) {
            ++found->second.references;
            return found->second.rid;
        }
        const std::optional<Image::Format> format = image_format(source.format);
        if (!format) {
            failure_ = "unsupported pixel format";
            return std::nullopt;
        }
        const detail::ParticleTexturePreparation prepared = detail::prepare_particle_texture(source);
        if (!prepared) {
            failure_ = prepared.failure;
            return std::nullopt;
        }
        PackedByteArray bytes;
        bytes.resize(static_cast<int64_t>(prepared.bytes.size()));
        if (!prepared.bytes.empty()) std::memcpy(bytes.ptrw(), prepared.bytes.data(), prepared.bytes.size());
        const Ref<Image> image = Image::create_from_data(static_cast<int32_t>(source.width),
            static_cast<int32_t>(source.height), source.mips.size() > 1, *format, bytes);
        if (image.is_null() || image->is_empty()) {
            failure_ = "Godot rejected prepared image data";
            return std::nullopt;
        }
        const RID rid = rendering.texture_2d_create(image);
        if (!rid.is_valid()) {
            failure_ = "Godot rejected prepared texture RID";
            return std::nullopt;
        }
        textures_.emplace(key, Shared{rid, 1});
        return rid;
    }

    void release_texture(RenderingServer& rendering, const std::string& key) {
        const auto found = textures_.find(key);
        if (found == textures_.end() || --found->second.references != 0) return;
        rendering.free_rid(found->second.rid);
        textures_.erase(found);
    }

    std::optional<RID> acquire_shader(RenderingServer& rendering, const std::string& program) {
        if (auto found = shaders_.find(program); found != shaders_.end()) {
            ++found->second.references;
            return found->second.rid;
        }
        // The same compiler observation as the renderer's modern route: a probe
        // uniform must survive compilation, so a valid RID alone is not success.
        std::string probed = program;
        const std::size_t declaration_end = probed.find(';');
        probed.insert(declaration_end + 1, "\nuniform float eawr_compile_probe;\n");
        const RID shader = rendering.shader_create();
        rendering.shader_set_code(shader, String::utf8(probed.data(), static_cast<int64_t>(probed.size())));
        const TypedArray<Dictionary> parameters = rendering.get_shader_parameter_list(shader);
        const StringName marker("eawr_compile_probe");
        bool compiled = false;
        for (int64_t index = 0; index < parameters.size(); ++index) {
            const Dictionary parameter = parameters[index];
            if (static_cast<StringName>(parameter.get("name", StringName())) == marker) compiled = true;
        }
        if (!compiled) {
            rendering.free_rid(shader);
            return std::nullopt;
        }
        shaders_.emplace(program, Shared{shader, 1});
        return shader;
    }

    void release_shader(RenderingServer& rendering, const std::string& program) {
        const auto found = shaders_.find(program);
        if (found == shaders_.end() || --found->second.references != 0) return;
        rendering.free_rid(found->second.rid);
        shaders_.erase(found);
    }

    void apply_lighting(RenderingServer& rendering, const RID& material) const {
        const auto vector = [](const std::array<float, 3>& value) { return Vector3(value[0], value[1], value[2]); };
        const auto matrix = [](const std::array<float, 16>& m) {
            return Projection(Vector4(m[0], m[1], m[2], m[3]), Vector4(m[4], m[5], m[6], m[7]),
                Vector4(m[8], m[9], m[10], m[11]), Vector4(m[12], m[13], m[14], m[15]));
        };
        rendering.material_set_param(material, StringName("eawr_particle_light"), vector(lighting_.toward_light));
        rendering.material_set_param(material, StringName("eawr_particle_light_diffuse"), vector(lighting_.diffuse));
        rendering.material_set_param(material, StringName("eawr_particle_light_specular"), vector(lighting_.specular));
        rendering.material_set_param(material, StringName("eawr_particle_fill_r"), matrix(lighting_.fill[0]));
        rendering.material_set_param(material, StringName("eawr_particle_fill_g"), matrix(lighting_.fill[1]));
        rendering.material_set_param(material, StringName("eawr_particle_fill_b"), matrix(lighting_.fill[2]));
    }

    GodotParticleBackend::BumpLighting lighting_{GodotParticleBackend::default_bump_lighting()};
    TextureResolver resolver_;
    GodotParticleBackend::FogCallbacks fog_;
    RID scenario_;
    std::map<std::uint64_t, Emitter> emitters_;
    std::map<std::string, Shared> textures_;
    std::map<std::string, Shared> shaders_;
    std::uint64_t next_id_{1};
    std::string failure_;
};

GodotParticleBackend::GodotParticleBackend(Node3D& host, TextureResolver resolver, FogCallbacks fog)
    : impl_(std::make_unique<Impl>(host, std::move(resolver), std::move(fog))) {}

GodotParticleBackend::~GodotParticleBackend() = default;

std::uint64_t GodotParticleBackend::create_emitter(const particles::EmitterRenderPlan& plan) {
    return impl_->create(plan);
}
void GodotParticleBackend::update_emitter(const std::uint64_t resource, const particles::VertexStream& stream) {
    impl_->update(resource, stream);
}
void GodotParticleBackend::destroy_emitter(const std::uint64_t resource) { impl_->destroy(resource); }
std::string GodotParticleBackend::failure_cause() const { return impl_->failure(); }
GodotParticleBackend::BumpLighting GodotParticleBackend::default_bump_lighting() noexcept {
    // The renderer's hemisphere fallback split for a per-pixel sun, as the
    // legacy bump adapters receive it without a scene environment.
    const auto& raw = lighting::hemisphere_light_direction;
    const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
    const lighting::IrradianceMatrices fill = lighting::hemisphere_fill_matrices();
    return {.toward_light = {raw[0] / length, raw[1] / length, raw[2] / length},
        .diffuse = lighting::hemisphere_directional,
        .specular = lighting::hemisphere_directional,
        .fill = fill.rgb};
}
void GodotParticleBackend::set_lighting(const BumpLighting& lighting) { impl_->set_lighting(lighting); }
std::size_t GodotParticleBackend::live_rids() const noexcept { return impl_->live_rids(); }
std::size_t GodotParticleBackend::live_emitters() const noexcept { return impl_->live_emitters(); }
std::vector<GodotParticleBackend::FogEmitterEvidence> GodotParticleBackend::fog_emitter_evidence() const {
    return impl_->fog_emitter_evidence();
}

RenderPass GodotParticleBackend::pass_for(const particles::DrawPhase phase) noexcept {
    switch (phase) {
    case particles::DrawPhase::opaque: return RenderPass::opaque;
    case particles::DrawPhase::transparent: return RenderPass::transparent;
    // Heat samples the finished scene, so it is post work that consumes the
    // preceding passes through the screen texture, as the renderer's post
    // pass does.
    case particles::DrawPhase::heat: return RenderPass::post;
    }
    return RenderPass::transparent;
}

MaterialDescription GodotParticleBackend::material_for(const particles::EmitterRenderPlan& plan,
    const bool fog) {
    MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = pass_for(plan.phase),
        .program = program_source(plan, fog),
        .technique = {},
        .pass_name = {},
        .bindings = {{"eawr_particle_texture", plan.texture}},
    };
    if (plan.blend == particles::Blend::bump_alpha && !plan.normal_texture.empty()) {
        material.bindings.push_back({"eawr_particle_normal", plan.normal_texture});
    }
    return material;
}

std::string_view GodotParticleBackend::adapter_name(const particles::RenderFamily family) noexcept {
    switch (family) {
    case particles::RenderFamily::billboard: return "eawr-particle-billboard-v1";
    case particles::RenderFamily::xy_aligned: return "eawr-particle-xy-aligned-v1";
    case particles::RenderFamily::heat_saturation: return "eawr-particle-heat-v1";
    case particles::RenderFamily::kites: return "eawr-particle-kite-v1";
    case particles::RenderFamily::unsupported: return "";
    }
    return "";
}

std::string_view GodotParticleBackend::material_adapter_name(const particles::Blend blend) noexcept {
    switch (blend) {
    case particles::Blend::opaque: return "eawr-particle-opaque-v1";
    case particles::Blend::additive: return "eawr-particle-additive-v1";
    case particles::Blend::alpha: return "eawr-particle-alpha-v1";
    case particles::Blend::modulate: return "eawr-particle-modulate-v1";
    case particles::Blend::heat_distortion: return "eawr-particle-heat-distortion-v1";
    case particles::Blend::bump_alpha: return "eawr-particle-bump-alpha-v1";
    }
    return "";
}

} // namespace eawr::presentation::godot_backend
