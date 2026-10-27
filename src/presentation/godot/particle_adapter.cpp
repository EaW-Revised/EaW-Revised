#include "particle_adapter.hpp"
#include "eawr/presentation/render_categories.hpp"
#include "particle_texture.hpp"
#include "particle_prewarm.hpp"
#include "particle_reuse.hpp"
#include "particle_spawn_trace.hpp"
#include "gpu_memory_ledger.hpp"
#include "texture_budget.hpp"
#include <godot_cpp/classes/engine.hpp>
#include "particle_upload.hpp"
#include "particle_culling.hpp"

#include "eawr/presentation/lighting/lighting.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <map>
#include <optional>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {
using UploadClock = std::chrono::steady_clock;
thread_local bool measure_uploads{};
thread_local GodotParticleBackend::FrameWork upload_work;
std::atomic<bool> measure_preparations{};

[[nodiscard]] bool spawn_trace_requested() {
    OS* const os = OS::get_singleton();
    if (os == nullptr) return false;
    const PackedStringArray arguments = os->get_cmdline_user_args();
    for (int64_t i = 0; i < arguments.size(); ++i) {
        if (String(arguments[i]) == String("--eawr-particle-spawn-trace")) return true;
    }
    return false;
}

[[nodiscard]] bool residency_trace_requested() {
    OS* const os = OS::get_singleton();
    if (os == nullptr) return false;
    const PackedStringArray arguments = os->get_cmdline_user_args();
    for (int64_t i = 0; i < arguments.size(); ++i) {
        if (String(arguments[i]) == String("--eawr-particle-residency-trace")) return true;
    }
    return false;
}

class UploadTimer final {
public:
    explicit UploadTimer(double& target, const bool enabled = measure_uploads) : target_(enabled ? &target : nullptr) {
        if (target_ != nullptr) start_ = UploadClock::now();
    }
    ~UploadTimer() {
        if (target_ != nullptr) *target_ += std::chrono::duration<double, std::milli>(UploadClock::now() - start_).count();
    }
private:
    double* target_;
    UploadClock::time_point start_{};
};

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
    friend class GodotParticleBackend;
public:
    static constexpr std::size_t inactive_index = ~std::size_t{0};
    struct Shared final {
        RID rid;
        std::size_t references{};
        detail::GpuItem item{};
    };
    struct Emitter final {
        std::uint64_t category_submit_frame{~std::uint64_t{0}};
        particles::EmitterRenderPlan plan;
        std::size_t active_index{inactive_index};
        bool requested_visible{};
        bool empty_pending{};
        std::uint64_t empty_frame{};
        detail::ParticleRestoreRetry retry;
        std::string trace_effect;
        std::uint64_t trace_tick{};
        std::string texture_key;
        std::string normal_key;
        std::string shader_key;
        RID material;
        RID mesh;
        RID instance;
        std::uint64_t fog_handle{};
        std::size_t emitter_index{};
        std::int32_t priority{};
        std::size_t quads{};
        bool bump{};
        bool owns_textures{true};
        bool reuse_allowed{};
        std::vector<std::pair<float, float>> source_points;
        detail::ParticleSurfaceShape shape;
        detail::ParticleSurfaceShape prepared_shape;
        PackedByteArray vertex_bytes;
        PackedByteArray attribute_bytes;
        PackedByteArray index_bytes;
        std::vector<std::uint32_t> topology;
        std::uint32_t vertex_stride{};
        std::uint32_t attribute_stride{};
        std::uint32_t colour_offset{};
        std::uint32_t uv_offset{};
        std::uint32_t index_stride{};
        bool prepared{};
        bool replace_surface{};
        bool changed_indices{};
        double preparation_ms{};
        AABB bounds;
        detail::ParticleSubmissionCache<AABB> submission;
        Array arrays;
    };

    Impl(Node3D& host, TextureResolver resolver, GodotParticleBackend::FogCallbacks fog)
        : resolver_(std::move(resolver)), fog_(std::move(fog)), spawn_journal_(spawn_trace_requested()) {
        trace_residency_ = residency_trace_requested();
        if (trace_residency_) census_backends_.push_back(this);
        if (host.get_world_3d().is_valid()) scenario_ = host.get_world_3d()->get_scenario();
        if (render_categories::enabled && host.get_viewport()) category_viewport_ = host.get_viewport()->get_viewport_rid();
    }

    ~Impl() {
        if (trace_residency_) {
            std::erase(census_backends_, this);
            if (census_backends_.empty()) {
                for (const auto& row : census_rows_) {
                    std::printf("EAWR_PARTICLE_RESIDENCY %llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                        static_cast<unsigned long long>(row[0]), static_cast<unsigned long long>(row[1]),
                        static_cast<unsigned long long>(row[2]), static_cast<unsigned long long>(row[3]),
                        static_cast<unsigned long long>(row[4]), static_cast<unsigned long long>(row[5]),
                        static_cast<unsigned long long>(row[6]), static_cast<unsigned long long>(row[7]),
                        static_cast<unsigned long long>(row[8]),
                        static_cast<unsigned long long>(row[9]),
                        static_cast<unsigned long long>(row[10]));
                }
                census_rows_.clear();
            }
        }
        set_reuse(false);
        release_warm();
        while (!emitters_.empty()) destroy(emitters_.begin()->first);
    }

    render_churn::Owner churn_owner_{render_churn::Owner::particle};
    void free_rid(RenderingServer& rendering, const RID& rid) {
        render_churn::free_rid(rendering, rid, churn_owner_);
    }

    std::optional<Emitter> allocate(const particles::EmitterRenderPlan& plan, const std::uint64_t resource,
        const Emitter* const shared = nullptr) {
        detail::ParticleSpawnTrace trace(spawn_journal_.enabled() ? &spawn_journal_ : nullptr,
            shared != nullptr ? shared->trace_effect : spawn_effect_,
            shared != nullptr ? shared->trace_tick : spawn_tick_, plan.texture, plan.emitter_index);
        trace.resource = resource;
        failure_.clear();
        RenderingServer* rendering = RenderingServer::get_singleton();
        if (rendering == nullptr || !scenario_.is_valid()) {
            failure_ = "RenderingServer or scenario is unavailable";
            return std::nullopt;
        }
        if (!plan.drawable) {
            failure_ = "plan is not drawable: " + plan.cause;
            return std::nullopt;
        }
        if (static_cast<bool>(fog_.register_material) != static_cast<bool>(fog_.unregister_material)) {
            failure_ = "fog registration callbacks must include both register and unregister";
            return std::nullopt;
        }
        if (fog_.register_material && (plan.blend == particles::Blend::heat_distortion
            || !plan.depth_test)) {
            failure_ = "emitter " + std::to_string(plan.emitter_index)
                + ": fog-stub-v1 does not support heat/screen distortion or disabled depth test";
            return std::nullopt;
        }
        const MaterialDescription material = GodotParticleBackend::material_for(plan,
            static_cast<bool>(fog_.register_material));
        if (const auto valid = validate_material(material); !valid) {
            failure_ = valid.error().message;
            return std::nullopt;
        }
        trace.lap(detail::ParticleSpawnTrace::validation);
        std::optional<RID> texture_rid;
        if (shared != nullptr) {
            texture_rid = textures_.at(shared->texture_key).rid;
        } else {
            const assets::Texture* texture = resolver_ ? resolver_(plan.texture) : nullptr;
            trace.lap(detail::ParticleSpawnTrace::texture_resolve);
            if (texture == nullptr) {
                failure_ = "colour texture '" + plan.texture + "' did not resolve";
                return std::nullopt;
            }
            if (trace.enabled()) trace.texture_cold = !textures_.contains(plan.texture);
            texture_rid = acquire_texture(*rendering, plan.texture, *texture);
            if (!texture_rid) {
                failure_ = "colour texture '" + plan.texture + "': " + failure_;
                return std::nullopt;
            }
        }
        trace.lap(detail::ParticleSpawnTrace::texture_setup_upload);
        // Shared references survive dormancy. Restore borrows their existing RIDs
        // and transfers ownership only after all per-emitter creation succeeds.
        std::optional<RID> normal_rid;
        const bool bump = plan.blend == particles::Blend::bump_alpha;
        if (shared != nullptr) {
            if (!shared->normal_key.empty()) normal_rid = textures_.at(shared->normal_key).rid;
        } else if (bump && !plan.normal_texture.empty()) {
            const assets::Texture* normal = resolver_ ? resolver_(plan.normal_texture) : nullptr;
            trace.lap(detail::ParticleSpawnTrace::normal_resolve);
            if (trace.enabled()) trace.normal_cold = !textures_.contains(plan.normal_texture);
            if (normal != nullptr) normal_rid = acquire_texture(*rendering, plan.normal_texture, *normal);
            if (!normal_rid) {
                failure_ = "normal texture '" + plan.normal_texture + "' did not resolve"
                    + (normal != nullptr ? ": " + failure_ : std::string{});
                release_texture(*rendering, plan.texture);
                return std::nullopt;
            }
        }
        trace.lap(detail::ParticleSpawnTrace::normal_setup_upload);
        const std::string normal_key = normal_rid ? plan.normal_texture : std::string{};
        const auto release_textures = [&] {
            if (shared != nullptr) return;
            release_texture(*rendering, plan.texture);
            if (!normal_key.empty()) release_texture(*rendering, normal_key);
        };
        const std::string shader_key = material.program;
        assert(shared == nullptr || shared->shader_key == shader_key);
        if (trace.enabled()) trace.shader_cold = !shaders_.contains(shader_key);
        const std::optional<RID> shader = shared != nullptr
            ? std::optional<RID>{shaders_.at(shared->shader_key).rid}
            : acquire_shader(*rendering, shader_key);
        const auto release_new_shader = [&] {
            if (shared == nullptr) release_shader(*rendering, shader_key);
        };
        trace.lap(detail::ParticleSpawnTrace::shader_compile);
        if (!shader) {
            release_textures();
            failure_ = "Godot rejected the particle program (EAWR-RENDER-0007)";
            return std::nullopt;
        }
        const auto priority = render_pass_priority(material.pass)
            + static_cast<std::int32_t>(std::min<std::uint32_t>(plan.order_in_phase, 7U));
        // Opaque draws may sort by resource identity. Keep their ordinary fresh
        // resources; transparent draws use the same priority and current depth.
        const bool reuse_allowed = plan.blend != particles::Blend::opaque;
        if (reuse_enabled_ && reuse_allowed) {
            if (auto retained = reuse_.take({plan.texture, normal_key, shader_key, priority})) {
                // The retained emitter holds its program, but releases texture
                // residency on retirement. Texture refs are newly acquired for
                // a create, or transferred from the dormant owner for a restore.
                release_shader(*rendering, shader_key);
                Emitter emitter = std::move(*retained);
                render_churn::record(churn_owner_, render_churn::Event::restored, 3);
                emitter.submission.reset(); // new owner/scenario attachment
                emitter.owns_textures = true;
                rendering->material_set_param(emitter.material, StringName("eawr_particle_texture"), *texture_rid);
                if (normal_rid) rendering->material_set_param(emitter.material,
                    StringName("eawr_particle_normal"), *normal_rid);
                emitter.emitter_index = plan.emitter_index;
                emitter.quads = 0;
                emitter.prepared = emitter.replace_surface = emitter.changed_indices = false;
                emitter.prepared_shape = {};
                emitter.preparation_ms = 0.0;
                emitter.source_points.clear();
                emitter.arrays.clear();
                emitter.bounds = AABB{};
                rendering->instance_set_scenario(emitter.instance, scenario_);
                // Cached bump materials missed intervening lighting updates.
                if (emitter.bump) apply_lighting(*rendering, emitter.material);
                emitter.plan = plan;
                if (spawn_journal_.enabled()) {
                    emitter.trace_effect = shared != nullptr ? shared->trace_effect : spawn_effect_;
                    emitter.trace_tick = shared != nullptr ? shared->trace_tick : spawn_tick_;
                }
                return emitter;
            }
        }
        Emitter emitter;
        emitter.texture_key = plan.texture;
        emitter.normal_key = normal_key;
        emitter.shader_key = shader_key;
        emitter.emitter_index = plan.emitter_index;
        emitter.priority = priority;
        emitter.reuse_allowed = reuse_allowed;
        emitter.material = rendering->material_create();
        if (!emitter.material.is_valid()) {
            release_new_shader();
            release_textures();
            failure_ = "Godot rejected the particle material RID";
            return std::nullopt;
        }
        rendering->material_set_shader(emitter.material, *shader);
        // Emitters are ordered inside the public pass range; the spacing of
        // eight between pass priorities keeps them from crossing a boundary.
        rendering->material_set_render_priority(emitter.material, priority);
        rendering->material_set_param(emitter.material, StringName("eawr_particle_texture"), *texture_rid);
        if (bump) {
            if (normal_rid) {
                rendering->material_set_param(emitter.material, StringName("eawr_particle_normal"), *normal_rid);
                rendering->material_set_param(emitter.material, StringName("eawr_particle_has_normal"), true);
            }
            apply_lighting(*rendering, emitter.material);
            emitter.bump = true;
        }
        trace.lap(detail::ParticleSpawnTrace::material_setup);
        emitter.mesh = rendering->mesh_create();
        emitter.instance = rendering->instance_create();
        if (!emitter.mesh.is_valid() || !emitter.instance.is_valid()) {
            if (emitter.instance.is_valid()) free_rid(*rendering, emitter.instance);
            if (emitter.mesh.is_valid()) free_rid(*rendering, emitter.mesh);
            free_rid(*rendering, emitter.material);
            release_new_shader();
            release_textures();
            failure_ = "Godot rejected a particle mesh or instance RID";
            return std::nullopt;
        }
        rendering->instance_set_base(emitter.instance, emitter.mesh);
        rendering->instance_set_scenario(emitter.instance, scenario_);
        rendering->instance_geometry_set_cast_shadows_setting(
            emitter.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
        trace.lap(detail::ParticleSpawnTrace::mesh_instance_create);
        if (fog_.register_material) {
            const auto registered = fog_.register_material(emitter.material, *shader);
            if (!registered || registered.value() == 0) {
                failure_ = "emitter " + std::to_string(plan.emitter_index) + ": "
                    + (registered ? "fog callback returned an empty consumer handle"
                        : core::format_diagnostic(registered.error()));
                free_rid(*rendering, emitter.instance);
                free_rid(*rendering, emitter.mesh);
                free_rid(*rendering, emitter.material);
                release_new_shader();
                release_textures();
                return std::nullopt;
            }
            emitter.fog_handle = registered.value();
        }
        trace.lap(detail::ParticleSpawnTrace::fog_registration);
        emitter.plan = plan;
        if (spawn_journal_.enabled()) {
            emitter.trace_effect = shared != nullptr ? shared->trace_effect : spawn_effect_;
            emitter.trace_tick = shared != nullptr ? shared->trace_tick : spawn_tick_;
        }
        return emitter;
    }

    std::uint64_t create(const particles::EmitterRenderPlan& plan) {
        auto emitter = allocate(plan, next_id_);
        if (!emitter) return 0;
        const auto id = next_id_++;
        auto& stored = emitters_.emplace(id, std::move(*emitter)).first->second;
        activate(stored);
        return id;
    }

    static void submit_visible(RenderingServer& rendering, Emitter& emitter, const bool visible) {
        emitter.submission.visibility.apply(visible, [&](const bool value) {
            rendering.instance_set_visible(emitter.instance, value);
        }, measure_uploads ? &upload_work.visibility_calls : nullptr);
    }

    static void submit_bounds(RenderingServer& rendering, Emitter& emitter) {
        emitter.submission.bounds.apply(emitter.bounds, [&](const AABB& value) {
            rendering.mesh_set_custom_aabb(emitter.mesh, value);
        }, measure_uploads ? &upload_work.bounds_calls : nullptr);
    }

    void activate(Emitter& emitter) {
        assert(emitter.active_index == inactive_index);
        emitter.active_index = active_.size();
        active_.push_back(&emitter);
    }

    void deactivate(Emitter& emitter) {
        if (emitter.active_index == inactive_index) return;
        const auto index = emitter.active_index;
        active_[index] = active_.back();
        active_[index]->active_index = index;
        active_.pop_back();
        emitter.active_index = inactive_index;
    }

    // Empty geometry is independent of camera/fog admission. Keep the public
    // handle and authored plan; CPU age, RNG and future births stay in the registry.
    void suspend(Emitter& emitter, RenderingServer* rendering, const bool terminal = false) {
        if (!emitter.material.is_valid()) return;
        const bool retained = emitter.active_index == inactive_index;
        if (retained && !terminal) return;
        UploadTimer timer(upload_work.retirement_ms);
        if (retained) {
            assert(retained_identities_ != 0);
            --retained_identities_;
        }
        deactivate(emitter);
        ++suspensions_;
        Emitter dormant;
        // Opaque equal-depth draws can sort by resource identity. Fog-backed
        // resumable owners also keep their material registration until terminal
        // retirement. Both relinquish every surface and packing buffer.
        if (detail::ParticleResidencyPolicy::keep_identity(terminal,
            emitter.plan.blend == particles::Blend::opaque, emitter.fog_handle != 0)) {
            if (rendering != nullptr) rendering->mesh_clear(emitter.mesh);
            dormant.fog_handle = emitter.fog_handle;
            dormant.material = emitter.material;
            dormant.mesh = emitter.mesh;
            dormant.instance = emitter.instance;
            ++retained_identities_;
            render_churn::record(churn_owner_, render_churn::Event::retained, 3);
        } else {
            release_draw(emitter, rendering);
        }
        dormant.plan = std::move(emitter.plan);
        dormant.texture_key = std::move(emitter.texture_key);
        dormant.normal_key = std::move(emitter.normal_key);
        dormant.shader_key = std::move(emitter.shader_key);
        dormant.priority = emitter.priority;
        dormant.bump = emitter.bump;
        // Only live owners enter this map; pooled owners alone drop texture refs.
        dormant.owns_textures = emitter.owns_textures;
        dormant.emitter_index = emitter.emitter_index;
        dormant.trace_effect = std::move(emitter.trace_effect);
        dormant.trace_tick = emitter.trace_tick;
        emitter = std::move(dormant);
    }

    [[nodiscard]] static std::uint64_t current_frame() {
        const auto* engine = Engine::get_singleton();
        return engine != nullptr ? static_cast<std::uint64_t>(engine->get_process_frames()) : 0;
    }

    void restore_failed(const std::uint64_t id, Emitter& emitter, const std::uint64_t frame) {
        if (!emitter.retry.failed(frame)) return;
        constexpr auto code = particles::diagnostic_codes::backend_resource;
        std::fprintf(stderr,
            "%.*s: resource %llu restore failed: %s; retrying with bounded backoff\n",
            static_cast<int>(code.size()), code.data(),
            static_cast<unsigned long long>(id), failure_.c_str());
    }

    void cancel_empty(const std::uint64_t id, Emitter& emitter) {
        if (!emitter.empty_pending) return;
        const auto found = std::lower_bound(pending_empty_.begin(), pending_empty_.end(), id,
            [](const auto& item, const auto wanted) { return item.first < wanted; });
        if (found != pending_empty_.end() && found->first == id) pending_empty_.erase(found);
        emitter.empty_pending = false;
    }

    void empty(const std::uint64_t id, const bool terminal) {
        const auto found = emitters_.find(id);
        if (found == emitters_.end() || !found->second.material.is_valid()) return;
        auto& emitter = found->second;
        auto* rendering = RenderingServer::get_singleton();
        // Configured transparent reuse owners commonly release a completed
        // drain in this same frame. Give that existing cache path the ordinary
        // bounded dwell, avoiding a free/recreate cycle before owner release.
        // Unreleased terminal resources still retire after the dwell/budget.
        if (detail::ParticleResidencyPolicy::immediate(terminal, reuse_enabled_, emitter.reuse_allowed)) {
            cancel_empty(id, emitter);
            suspend(emitter, rendering, true);
            return;
        }
        if (rendering != nullptr) submit_visible(*rendering, emitter, false);
        emitter.quads = 0;
        emitter.requested_visible = false;
        if (emitter.empty_pending) return;
        emitter.empty_pending = true;
        emitter.empty_frame = current_frame();
        const auto slot = std::lower_bound(pending_empty_.begin(), pending_empty_.end(), id,
            [](const auto& item, const auto wanted) { return item.first < wanted; });
        pending_empty_.insert(slot, {id, &emitter});
    }

    void retire_empty(const std::uint64_t frame, RenderingServer* rendering) {
        // Short empty spans keep their existing surface and skip recreation.
        // Only render residency uses this dwell, never CPU emission or RNG.
        constexpr std::uint64_t dwell_frames = 4;
        constexpr std::size_t retirement_budget = 8;
        std::size_t retired{};
        for (std::size_t index = 0; index < pending_empty_.size() && retired < retirement_budget;) {
            auto& emitter = *pending_empty_[index].second;
            if (frame < emitter.empty_frame || frame - emitter.empty_frame < dwell_frames) { ++index; continue; }
            pending_empty_.erase(pending_empty_.begin() + static_cast<std::ptrdiff_t>(index));
            emitter.empty_pending = false;
            suspend(emitter, rendering);
            ++retired;
        }
    }

    bool prewarm(const particles::EmitterRenderPlan& plan){
        const auto material=GodotParticleBackend::material_for(plan,static_cast<bool>(fog_.register_material));
        const detail::ParticleWarmPolicy policy{plan.texture,
            plan.blend==particles::Blend::bump_alpha?plan.normal_texture:std::string{},material.program,
            render_pass_priority(material.pass)+static_cast<std::int32_t>(std::min<std::uint32_t>(plan.order_in_phase,7U))};
        if(warm_budget_.contains(policy))return true;
        if(warm_budget_.emitters()>=detail::ParticleWarmBudget::max_emitters)return false;
        std::vector<detail::ParticleWarmBudget::TextureCharge> charges;
        for(const auto& name : {policy.texture,policy.normal}){
            if(name.empty())continue;
            const auto* source=resolver_?resolver_(name):nullptr;
            if(!source)return false;
            charges.emplace_back(name,detail::texture_item(*source).bytes);
        }
        if(!warm_budget_.admits(policy,charges))return false;
        const auto id=create(plan);
        if(id==0)return false;
        auto& emitter=emitters_.at(id);
        submit_visible(*RenderingServer::get_singleton(), emitter, false);
        RenderingServer::get_singleton()->instance_set_scenario(emitter.instance,RID{});
        emitter.submission.reset();
        // No surface is created: the retained instance can never submit a primitive.
        warm_ids_.push_back(id);
        warm_budget_.commit(policy,charges);
        return true;
    }
    bool prewarm_program(const particles::EmitterRenderPlan& plan) {
        ++warm_program_work_.requested;
        const auto refuse = [this] { ++warm_program_work_.refused; return false; };
        if (!plan.drawable) return refuse();
        if (static_cast<bool>(fog_.register_material) != static_cast<bool>(fog_.unregister_material)) return refuse();
        if (fog_.register_material && (plan.blend == particles::Blend::heat_distortion || !plan.depth_test)) return refuse();
        const auto material = GodotParticleBackend::material_for(plan, static_cast<bool>(fog_.register_material));
        if (!validate_material(material)) return refuse();
        // Program identity is independent of texture bindings and render priority.
        if (warm_program_budget_.contains(material.program)) {
            ++warm_program_work_.duplicate_requests;
            return true;
        }
        if (!warm_program_budget_.admits(material.program)) return refuse();
        RenderingServer* const rendering = RenderingServer::get_singleton();
        if (rendering == nullptr || !acquire_shader(*rendering, material.program)) return refuse();
        warm_program_budget_.commit(material.program);
        warm_program_work_.programs = std::max(warm_program_work_.programs, warm_program_budget_.size());
        return true;
    }
    void release_warm() {
        for (const auto id : warm_ids_) destroy(id);
        warm_ids_.clear();
        warm_budget_.clear();
        if (RenderingServer* const rendering = RenderingServer::get_singleton()) {
            for (const auto& program : warm_program_budget_.programs()) release_shader(*rendering, program);
        }
        warm_program_budget_.clear();
    }

    void prepare(const std::uint64_t id, const particles::VertexStream& stream) {
        // The map topology is immutable throughout an EffectRegistry worker batch.
        // Each task owns a different emitter and touches only that emitter's buffers.
        const auto found = emitters_.find(id);
        if (found == emitters_.end()) return;
        Emitter& emitter = found->second;
        // Tasks own their timing slots; the caller folds every slot after the worker barrier.
        emitter.preparation_ms = 0.0;
        UploadTimer timer(emitter.preparation_ms, measure_preparations.load(std::memory_order_relaxed));
        emitter.prepared = true;
        emitter.source_points.clear();
        if (stream.vertices.empty()) return;
        emitter.bounds = AABB(axis_convert(stream.vertices.front().position), Vector3());
        if (fog_.attenuation_at_source_xy) emitter.source_points.reserve(stream.vertices.size());
        emitter.replace_surface = !emitter.shape.matches(stream);
        emitter.changed_indices = emitter.topology != stream.indices;
        if (!emitter.replace_surface) {
            const auto vertex_stride = emitter.vertex_stride;
            const auto attribute_stride = emitter.attribute_stride;
            emitter.vertex_bytes.resize(static_cast<int64_t>(stream.vertices.size() * vertex_stride));
            emitter.attribute_bytes.resize(static_cast<int64_t>(stream.vertices.size() * attribute_stride));
            auto* const vertex_out = emitter.vertex_bytes.ptrw();
            auto* const attribute_out = emitter.attribute_bytes.ptrw();
            for (std::size_t index = 0; index < stream.vertices.size(); ++index) {
                const auto& vertex = stream.vertices[index];
                detail::pack_particle_vertex(vertex, vertex_out + index * vertex_stride,
                    attribute_out + index * attribute_stride + emitter.colour_offset,
                    attribute_out + index * attribute_stride + emitter.uv_offset);
                emitter.bounds.expand_to(axis_convert(vertex.position));
                if (fog_.attenuation_at_source_xy) emitter.source_points.emplace_back(vertex.position.x, vertex.position.y);
            }
            if (emitter.changed_indices) {
                const auto stride = emitter.index_stride;
                emitter.index_bytes.resize(static_cast<int64_t>(emitter.shape.indices * stride));
                auto* const output = emitter.index_bytes.ptrw();
                // Clear the complete retained tail when the live stream shrinks.
                std::memset(output, 0, emitter.shape.indices * stride);
                for (std::size_t index = 0; index < stream.indices.size(); ++index) {
                    if (stride == sizeof(std::uint16_t)) {
                        const auto value = static_cast<std::uint16_t>(stream.indices[index]);
                        std::memcpy(output + index * stride, &value, sizeof(value));
                    } else {
                        std::memcpy(output + index * stride, &stream.indices[index], sizeof(std::uint32_t));
                    }
                }
            }
        } else {
            emitter.prepared_shape.assign(stream);
            PackedVector3Array vertices;
            PackedColorArray colors;
            PackedVector2Array uv;
            PackedInt32Array indices;
            vertices.resize(static_cast<int64_t>(emitter.prepared_shape.vertices));
            colors.resize(static_cast<int64_t>(emitter.prepared_shape.vertices));
            uv.resize(static_cast<int64_t>(emitter.prepared_shape.vertices));
            Vector3* const vertex_out = vertices.ptrw();
            Color* const color_out = colors.ptrw();
            Vector2* const uv_out = uv.ptrw();
            for (std::size_t index = 0; index < stream.vertices.size(); ++index) {
                const auto& vertex = stream.vertices[index];
                vertex_out[index] = axis_convert(vertex.position);
                color_out[index] = Color(vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w);
                uv_out[index] = Vector2(vertex.u, vertex.v);
                emitter.bounds.expand_to(vertex_out[index]);
                if (fog_.attenuation_at_source_xy) emitter.source_points.emplace_back(vertex.position.x, vertex.position.y);
            }
            indices.resize(static_cast<int64_t>(emitter.prepared_shape.indices));
            std::int32_t* const index_out = indices.ptrw();
            for (std::size_t index = 0; index < emitter.prepared_shape.indices; ++index)
                index_out[index] = index < stream.indices.size() ? static_cast<std::int32_t>(stream.indices[index]) : 0;
            emitter.arrays.resize(RenderingServer::ARRAY_MAX);
            emitter.arrays[RenderingServer::ARRAY_VERTEX] = vertices;
            emitter.arrays[RenderingServer::ARRAY_COLOR] = colors;
            emitter.arrays[RenderingServer::ARRAY_TEX_UV] = uv;
            emitter.arrays[RenderingServer::ARRAY_INDEX] = indices;
        }
        emitter.bounds.size = emitter.bounds.size.max(Vector3(0.00001F, 0.00001F, 0.00001F));
    }

    void update(const std::uint64_t id, const particles::VertexStream& stream) {
        RenderingServer* rendering = RenderingServer::get_singleton();
        const auto found = emitters_.find(id);
        if (rendering == nullptr || found == emitters_.end()) return;
        Emitter& emitter = found->second;
        if (measure_uploads) ++upload_work.streams;
        if (render_categories::enabled) emitter.category_submit_frame = current_frame();
        if (!emitter.prepared) {
            // Direct projectile/beam streams also use this backend, outside a registry batch.
            prepare(id, stream);
        }
        if (measure_uploads) upload_work.conversion_ms += emitter.preparation_ms;
        emitter.prepared = false;
        if (stream.vertices.empty()) {
            empty(id, false);
            return;
        }
        cancel_empty(id, emitter);
        if (!emitter.material.is_valid()) {
            const auto frame = current_frame();
            if (!emitter.retry.admits(frame)) return;
            auto restored = [&] {
                [[maybe_unused]] const auto scope = asset_work_.restoring();
                return allocate(emitter.plan, id, &emitter);
            }();
            if (!restored) {
                restore_failed(id, emitter, frame);
                return;
            }
            restored->retry.succeeded();
            // A cache hit has a different retained surface than the worker saw.
            // Empty handles always prepare full arrays; discard any reused surface.
            restored->shape = {};
            restored->topology.clear();
            restored->prepared_shape = emitter.prepared_shape;
            restored->arrays = std::move(emitter.arrays);
            restored->bounds = emitter.bounds;
            restored->source_points = std::move(emitter.source_points);
            emitter = std::move(*restored);
            emitter.submission.reset(); // restored resource identity
            emitter.replace_surface = true;
            activate(emitter);
            ++recreations_;
        }
        if (emitter.active_index == inactive_index) {
            assert(emitter.plan.blend == particles::Blend::opaque || emitter.fog_handle != 0);
            assert(emitter.shape.vertices == 0 && emitter.shape.indices == 0);
            const auto frame = current_frame();
            if (!emitter.retry.admits(frame)) return;
            [[maybe_unused]] const auto scope = asset_work_.restoring();
            if (fog_.register_material && emitter.fog_handle == 0) {
                const auto registered = fog_.register_material(emitter.material, shaders_.at(emitter.shader_key).rid);
                if (!registered || registered.value() == 0) {
                    failure_ = registered ? "fog callback returned an empty consumer handle"
                        : core::format_diagnostic(registered.error());
                    restore_failed(id, emitter, frame);
                    return;
                }
                emitter.fog_handle = registered.value();
            }
            // Dormant bump materials missed any intervening lighting updates.
            if (emitter.bump) apply_lighting(*rendering, emitter.material);
            emitter.retry.succeeded();
            assert(retained_identities_ != 0);
            --retained_identities_;
            emitter.submission.reset(); // restored dormant identity
            render_churn::record(churn_owner_, render_churn::Event::restored, 3);
            activate(emitter);
            ++recreations_;
        }
        const bool was_empty = emitter.quads == 0;
        emitter.quads = stream.quads + stream.triangles;
        if (measure_uploads) ++upload_work.uploads;
        UploadTimer timer(upload_work.submission_ms);
        if (!emitter.replace_surface) {
            rendering->mesh_surface_update_vertex_region(emitter.mesh, 0, 0, emitter.vertex_bytes);
            rendering->mesh_surface_update_attribute_region(emitter.mesh, 0, 0, emitter.attribute_bytes);
            if (emitter.changed_indices)
                rendering->mesh_surface_update_index_region(emitter.mesh, 0, 0, emitter.index_bytes);
        } else {
            emitter.submission.reset(); // replacement surface
            rendering->mesh_clear(emitter.mesh);
            rendering->mesh_add_surface_from_arrays(emitter.mesh, RenderingServer::PRIMITIVE_TRIANGLES, emitter.arrays,
                Array(), Dictionary(), RenderingServer::ARRAY_FLAG_USE_DYNAMIC_UPDATE);
            rendering->mesh_surface_set_material(emitter.mesh, 0, emitter.material);
            emitter.arrays.clear();
            emitter.shape = emitter.prepared_shape;
            const auto format = BitField<RenderingServer::ArrayFormat>(
                RenderingServer::ARRAY_FORMAT_VERTEX | RenderingServer::ARRAY_FORMAT_COLOR
                | RenderingServer::ARRAY_FORMAT_TEX_UV | RenderingServer::ARRAY_FORMAT_INDEX
                | RenderingServer::ARRAY_FLAG_USE_DYNAMIC_UPDATE | RenderingServer::ARRAY_FLAG_FORMAT_CURRENT_VERSION);
            const auto count = static_cast<int32_t>(emitter.shape.vertices);
            emitter.vertex_stride = rendering->mesh_surface_get_format_vertex_stride(format, count);
            emitter.attribute_stride = rendering->mesh_surface_get_format_attribute_stride(format, count);
            emitter.colour_offset = rendering->mesh_surface_get_format_offset(format, count, RenderingServer::ARRAY_COLOR);
            emitter.uv_offset = rendering->mesh_surface_get_format_offset(format, count, RenderingServer::ARRAY_TEX_UV);
            emitter.index_stride = rendering->mesh_surface_get_format_index_stride(format, count);
            if (measure_uploads) ++upload_work.replacements;
        }
        if (emitter.replace_surface || emitter.changed_indices) emitter.topology = stream.indices;
        // Bounds reflect live vertices only, never the retained surface's unused storage.
        submit_bounds(*rendering, emitter);
        if (was_empty) {
            emitter.requested_visible = true;
            submit_visible(*rendering, emitter, true);
        }
    }

    void destroy(const std::uint64_t id) {
        UploadTimer timer(upload_work.retirement_ms);
        const auto found = emitters_.find(id);
        if (found == emitters_.end()) return;
        RenderingServer* rendering = RenderingServer::get_singleton();
        cancel_empty(id, found->second);
        if (!found->second.material.is_valid()) {
            release_emitter(found->second, rendering);
            emitters_.erase(found);
            return;
        }
        if (found->second.active_index == inactive_index) {
            assert(retained_identities_ != 0);
            --retained_identities_;
        }
        deactivate(found->second);
        if (reuse_enabled_ && found->second.reuse_allowed && rendering != nullptr) {
            Emitter& emitter = found->second;
            const auto bytes = detail::particle_reuse_charge(emitter.shape.vertices, emitter.vertex_stride,
                emitter.attribute_stride, emitter.shape.indices, emitter.index_stride, 0, 0);
            submit_visible(*rendering, emitter, false);
            rendering->instance_set_scenario(emitter.instance, RID{});
            emitter.submission.reset();
            // Cached RIDs keep geometry, not texture residency. Unbind before
            // releasing refs so the adaptive texture budget follows live demand.
            rendering->material_set_param(emitter.material, StringName("eawr_particle_texture"), Variant{});
            if (!emitter.normal_key.empty()) rendering->material_set_param(emitter.material,
                StringName("eawr_particle_normal"), Variant{});
            release_texture(*rendering, emitter.texture_key);
            if (!emitter.normal_key.empty()) release_texture(*rendering, emitter.normal_key);
            emitter.owns_textures = false;
            // CPU packing shadows are rebuilt from the new owner's stream. The
            // GPU surface/strides survive, and the empty topology forces index
            // upload (including zeroing its unused tail) before visibility.
            emitter.vertex_bytes = PackedByteArray{};
            emitter.attribute_bytes = PackedByteArray{};
            emitter.index_bytes = PackedByteArray{};
            std::vector<std::uint32_t>{}.swap(emitter.topology);
            std::vector<std::pair<float, float>>{}.swap(emitter.source_points);
            emitter.arrays.clear();
            // No eviction: a full cache uses ordinary destruction instead of
            // adding another synchronous free to a successful cache admission.
            // Refusals are reported, so persistent policy starvation is visible.
            if (reuse_.retain({emitter.texture_key, emitter.normal_key, emitter.shader_key, emitter.priority},
                // Most recent caller-thread frame; cache age never affects CPU clocks.
                bytes, emitter, reuse_frame_)) {
                render_churn::record(churn_owner_, render_churn::Event::retained, 3);
                emitters_.erase(found);
                return;
            }
        }
        release_emitter(found->second, rendering);
        emitters_.erase(found);
    }

    void set_reuse(const bool enabled) {
        // Fog consumer ownership remains on the immediate-destruction path.
        reuse_enabled_ = enabled && !fog_.register_material;
        if (!reuse_enabled_) {
            RenderingServer* const rendering = RenderingServer::get_singleton();
            reuse_.drain([&](Emitter& emitter) { release_emitter(emitter, rendering); });
        }
    }

    void expire_reuse() {
        const auto* engine = Engine::get_singleton();
        if (engine == nullptr) return;
        const auto frame = static_cast<std::uint64_t>(engine->get_process_frames());
        if (frame == reuse_frame_) return;
        reuse_frame_ = frame;
        RenderingServer* const rendering = RenderingServer::get_singleton();
        retire_empty(frame, rendering);
        reuse_.expire(frame, [&](Emitter& emitter) { release_emitter(emitter, rendering); });
    }

    void sample_residency() const {
        if (!trace_residency_) return;
        const auto* engine = Engine::get_singleton();
        if (engine == nullptr) return;
        const auto frame = static_cast<std::uint64_t>(engine->get_process_frames());
        if (frame / 30 == census_frame_ / 30 || census_rows_.size() >= 32768) return;
        census_frame_ = frame;
        std::array<std::uint64_t, 11> row{frame, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (const auto* backend : census_backends_) {
            row[1] += backend->active_.size() + backend->retained_identities_ + backend->reuse_.size();
            row[3] += backend->emitters_.size();
            row[4] += backend->reuse_.size();
            row[5] += backend->suspensions_;
            row[6] += backend->recreations_;
            row[7] += backend->asset_work_.restores();
            row[8] += backend->textures_.size() + backend->shaders_.size();
            row[9] += backend->retained_identities_ * 3;
            row[10] += backend->live_rids();
            for (const auto* emitter : backend->active_)
                row[2] += emitter->requested_visible && emitter->quads != 0 ? 1U : 0U;
        }
        census_rows_.push_back(row);
    }

    void release_draw(const Emitter& emitter, RenderingServer* rendering) {
        if (emitter.fog_handle && fog_.unregister_material)
            fog_.unregister_material(emitter.fog_handle);
        if (rendering != nullptr) {
            if (emitter.instance.is_valid()) free_rid(*rendering, emitter.instance);
            if (emitter.mesh.is_valid()) free_rid(*rendering, emitter.mesh);
            if (emitter.material.is_valid()) free_rid(*rendering, emitter.material);
        }
    }

    void release_emitter(const Emitter& emitter, RenderingServer* rendering) {
        release_draw(emitter, rendering);
        if (rendering != nullptr) {
            release_shader(*rendering, emitter.shader_key);
            if (emitter.owns_textures) {
                release_texture(*rendering, emitter.texture_key);
                if (!emitter.normal_key.empty()) release_texture(*rendering, emitter.normal_key);
            }
        }
    }

    void set_lighting(const GodotParticleBackend::BumpLighting& lighting) {
        if (lighting == lighting_) return;
        lighting_ = lighting;
        RenderingServer* rendering = RenderingServer::get_singleton();
        if (rendering == nullptr) return;
        for (const auto* emitter : active_) {
            if (emitter->bump) apply_lighting(*rendering, emitter->material);
        }
    }

    [[nodiscard]] std::size_t live_rids() const noexcept {
        return detail::ParticleResidencyPolicy::live_rids(active_.size(), reuse_.size(), retained_identities_,
            textures_.size() + shaders_.size());
    }
    [[nodiscard]] std::size_t live_emitters() const noexcept { return active_.size(); }
    [[nodiscard]] std::vector<GodotParticleBackend::FogEmitterEvidence> fog_emitter_evidence() const {
        std::vector<GodotParticleBackend::FogEmitterEvidence> result;
        for (const auto& [resource, emitter] : emitters_) {
            if (!emitter.material.is_valid()) continue;
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
        asset_work_.acquired();
        if (auto found = textures_.find(key); found != textures_.end()) {
            ++found->second.references;
            detail::gpu_memory_ledger().reuse(found->second.item);
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
        const auto item=detail::texture_item(source);
        textures_.emplace(key, Shared{rid, 1, item});
        detail::gpu_memory_ledger().add(item);
        detail::texture_budget().add(item.bytes);
        return rid;
    }

    void release_texture(RenderingServer& rendering, const std::string& key) {
        const auto found = textures_.find(key);
        if (found == textures_.end() || --found->second.references != 0) return;
        free_rid(rendering, found->second.rid);
        detail::gpu_memory_ledger().remove(found->second.item);
        Engine* const engine=Engine::get_singleton();
        detail::texture_budget().remove(found->second.item.bytes,
            engine?static_cast<std::uint64_t>(engine->get_frames_drawn()):0U);
        textures_.erase(found);
    }

    std::optional<RID> acquire_shader(RenderingServer& rendering, const std::string& program) {
        asset_work_.acquired();
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
            free_rid(rendering, shader);
            return std::nullopt;
        }
        shaders_.emplace(program, Shared{shader, 1});
        return shader;
    }

    void release_shader(RenderingServer& rendering, const std::string& program) {
        const auto found = shaders_.find(program);
        if (found == shaders_.end() || --found->second.references != 0) return;
        free_rid(rendering, found->second.rid);
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
    RID category_viewport_;
    render_categories::Source category_source_{this, [](const void* owner, render_categories::Sample& sample) {
        const auto& self = *static_cast<const Impl*>(owner);
        for (const auto& [id, emitter] : self.emitters_) {
            static_cast<void>(id);
            if (emitter.instance.is_valid()) sample.record(render_categories::Role::particle,
                emitter.category_submit_frame == sample.frame, false, emitter.shape.vertices != 0 ? 1U : 0U);
        }
    }, [](const void* owner) { return static_cast<std::uint64_t>(static_cast<const Impl*>(owner)->category_viewport_.get_id()); }};
    std::vector<Emitter*> active_;
    // Stable map-node pointers. destroy() cancels an entry before erasing its node.
    std::vector<std::pair<std::uint64_t, Emitter*>> pending_empty_;
    detail::ParticleReuseCache<Emitter> reuse_;
    std::uint64_t reuse_frame_{};
    bool reuse_enabled_{};
    std::vector<std::uint64_t> warm_ids_;
    detail::ParticleWarmBudget warm_budget_;
    detail::ParticleProgramWarmBudget warm_program_budget_;
    ProgramPrewarmWork warm_program_work_;
    std::map<std::string, Shared> textures_;
    std::string spawn_effect_;
    std::uint64_t spawn_tick_{};
    detail::ParticleSpawnJournal spawn_journal_;
    std::map<std::string, Shared> shaders_;
    std::uint64_t next_id_{1};
    std::uint64_t suspensions_{}, recreations_{};
    std::size_t retained_identities_{};
    detail::ParticleAssetWork asset_work_;
    bool trace_residency_{};
    // Opt-in, bounded and flushed only at teardown: no measured-frame I/O.
    static inline std::vector<Impl*> census_backends_;
    static inline std::vector<std::array<std::uint64_t, 11>> census_rows_;
    static inline std::uint64_t census_frame_{};
    std::string failure_;
};

GodotParticleBackend::GodotParticleBackend(Node3D& host, TextureResolver resolver, FogCallbacks fog)
    : impl_(std::make_unique<Impl>(host, std::move(resolver), std::move(fog))) {}

GodotParticleBackend::~GodotParticleBackend() = default;

void GodotParticleBackend::begin_frame_measurement(const bool enabled) {
    measure_uploads = enabled;
    measure_preparations.store(enabled, std::memory_order_relaxed);
    upload_work = {};
}

GodotParticleBackend::FrameWork GodotParticleBackend::frame_work() { return upload_work; }

std::uint64_t GodotParticleBackend::create_emitter(const particles::EmitterRenderPlan& plan) {
    return impl_->create(plan);
}
void GodotParticleBackend::set_churn_owner(const render_churn::Owner owner) noexcept {
    impl_->churn_owner_ = owner;
}

void GodotParticleBackend::set_spawn_context(const std::string_view effect, const std::uint64_t tick) noexcept {
    if (!impl_->spawn_journal_.enabled()) return;
    impl_->spawn_effect_ = effect;
    impl_->spawn_tick_ = tick;
}
bool GodotParticleBackend::prewarm_emitter(const particles::EmitterRenderPlan& plan){return impl_->prewarm(plan);}
bool GodotParticleBackend::prewarm_program(const particles::EmitterRenderPlan& plan) { return impl_->prewarm_program(plan); }
void GodotParticleBackend::release_prewarm(){impl_->release_warm();}
std::size_t GodotParticleBackend::prewarm_program_limit() noexcept { return detail::ParticleProgramWarmBudget::max_programs; }
bool GodotParticleBackend::begin_prewarm_reference() {
    ++impl_->warm_program_work_.references;
    if (!impl_->warm_program_budget_.full()) return true;
    ++impl_->warm_program_work_.skipped_references;
    return false;
}
void GodotParticleBackend::record_prewarm_lookup(const double milliseconds, const bool decoded) {
    impl_->warm_program_work_.lookup_ms += milliseconds;
    if (decoded) ++impl_->warm_program_work_.decoded_systems;
}
void GodotParticleBackend::finish_program_prewarm(const double milliseconds) {
    impl_->warm_program_work_.prepare_ms = milliseconds;
    impl_->warm_program_work_.prepared_shaders = impl_->shaders_.size();
}
GodotParticleBackend::ProgramPrewarmWork GodotParticleBackend::program_prewarm_work() const noexcept {
    auto work = impl_->warm_program_work_;
    work.live_shaders = impl_->shaders_.size();
    return work;
}
std::size_t GodotParticleBackend::prewarmed_emitters() const noexcept {return impl_->warm_budget_.emitters();}
std::uint64_t GodotParticleBackend::prewarmed_texture_bytes() const noexcept {return impl_->warm_budget_.texture_bytes();}
void GodotParticleBackend::prepare_emitter(const std::uint64_t resource, const particles::VertexStream& stream) {
    impl_->prepare(resource, stream);
}
void GodotParticleBackend::update_emitter(const std::uint64_t resource, const particles::VertexStream& stream) {
    impl_->update(resource, stream);
}
void GodotParticleBackend::empty_emitter(const std::uint64_t resource, const bool terminal) { impl_->empty(resource, terminal); }
void GodotParticleBackend::destroy_emitter(const std::uint64_t resource) { impl_->destroy(resource); }
void GodotParticleBackend::set_emitter_reuse(const bool enabled) { impl_->set_reuse(enabled); }
GodotParticleBackend::ReuseWork GodotParticleBackend::reuse_work() const noexcept {
    const auto& cache = impl_->reuse_;
    return {cache.size(), cache.bytes(), cache.peak_emitters(), cache.peak_bytes(),
        cache.hits(), cache.misses(), cache.refusals(), impl_->live_rids()};
}
std::string GodotParticleBackend::failure_cause() const { return impl_->failure(); }
particles::CullingFrame GodotParticleBackend::culling_frame() const {
    impl_->expire_reuse();
    impl_->sample_residency();
    return particle_culling::frame(impl_->scenario_);
}
void GodotParticleBackend::set_emitter_visible(const std::uint64_t resource, const bool visible) {
    const auto found = impl_->emitters_.find(resource);
    if (found != impl_->emitters_.end() && found->second.instance.is_valid()) {
        found->second.requested_visible = visible && found->second.quads != 0;
        Impl::submit_visible(*RenderingServer::get_singleton(), found->second, found->second.requested_visible);
    }
}
void GodotParticleBackend::record_group_work(const bool visible, const bool prepared, const bool stepped) {
    if (!measure_uploads) return;
    ++upload_work.groups;
    upload_work.visible_groups += visible ? 1U : 0U;
    upload_work.prepared_groups += prepared ? 1U : 0U;
    upload_work.stepped_groups += stepped ? 1U : 0U;
}
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
std::uint64_t GodotParticleBackend::suspensions() const noexcept { return impl_->suspensions_; }
std::uint64_t GodotParticleBackend::recreations() const noexcept { return impl_->recreations_; }
std::uint64_t GodotParticleBackend::restore_asset_acquisitions() const noexcept { return impl_->asset_work_.restores(); }
std::uint64_t GodotParticleBackend::asset_acquisitions() const noexcept { return impl_->asset_work_.total(); }
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
