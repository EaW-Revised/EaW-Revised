#include "eawr/presentation/particles/render.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

// Quad construction follows the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp):
// corner order, the 0-1-2 / 2-1-3 index pattern, the texture-coordinate
// assignment and the kite tail geometry. Blend, depth-write and phase policy
// are taken from the render state of the public Engine/Prim* effects that the
// legacy selector names; see docs/reports/P1-08-rendering.md.

namespace eawr::presentation::particles {
namespace {

constexpr float pi = 3.14159265358979323846F;

Vec3 operator+(const Vec3 a, const Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(const Vec3 a, const Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(const Vec3 a, const float b) { return {a.x * b, a.y * b, a.z * b}; }
float dot(const Vec3 a, const Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3 a, const Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
float length(const Vec3 value) { return std::sqrt(dot(value, value)); }
Vec3 normalized(const Vec3 value) {
    const float magnitude = length(value);
    return magnitude > 1.0e-12F ? value * (1.0F / magnitude) : Vec3{};
}
bool finite(const Vec3 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool finite(const Vec4 value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}
// Render basis (Y up) back to the ALO basis (Z up): inverse of (x,y,z)->(x,z,-y).
Vec3 render_to_particle(const float x, const float y, const float z) { return {x, -z, y}; }

// Legacy V1 selector table. Program names are the public engine effects the
// selector indexes; techniques are the first active technique in each source.
constexpr std::array<LegacyBlendSelector, 14> selectors{{
    {0, "Engine/PrimOpaque.fx", "t0", true, ""},
    {1, "Engine/PrimAdditive.fx", "t1", true, ""},
    {2, "Engine/PrimAlpha.fx", "t1", true, ""},
    {3, "Engine/PrimModulate.fx", "t0", true, ""},
    {4, "Engine/PrimDepthSpriteAdditive.fx", "Depth_Sprite_PS20", false,
     "depth sprite replaces per-pixel depth from the emitter's depth texture; not implemented"},
    {5, "Engine/PrimDepthSpriteAlpha.fx", "Depth_Sprite_PS20", false,
     "depth sprite replaces per-pixel depth from the emitter's depth texture; not implemented"},
    {6, "Engine/PrimDepthSpriteModulate.fx", "Depth_Sprite_PS20", false,
     "depth sprite replaces per-pixel depth from the emitter's depth texture; not implemented"},
    {7, "Engine/PrimDiffuseAlpha.fx", "t1", false,
     "lit 2x diffuse primitive needs the #25 lighting interface"},
    {8, "Engine/StencilDarken.fx", "", false, "stencil darkening pipeline is not implemented"},
    {9, "Engine/StencilDarkenFinalBlur.fx", "", false,
     "stencil darkening blur pipeline is not implemented"},
    {10, "Engine/PrimHeat.fx", "t2", true, ""},
    {11, "Engine/PrimParticleBumpAlpha.fx", "t0", true, ""},
    {12, "Engine/PrimDecalBumpAlpha.fx", "", false,
     "bump-mapped decal lighting needs the #25 lighting interface"},
    {13, "Engine/PrimAlphaScanlines.fx", "t0", false,
     "scanline pattern shading is not implemented"},
}};

// Corners in the quad's local plane, before rotation: the alo-viewer order
// (-s,+s), (-s,-s), (+s,+s), (+s,-s) with indices 0-1-2 and 2-1-3.
void append_quad(VertexStream& stream, const Particle& particle, const Vec3 axis_x,
                 const Vec3 axis_y, const std::array<std::array<float, 2>, 4>& corners,
                 const float angle) {
    if (!std::isfinite(angle) || !finite(axis_x) || !finite(axis_y) ||
        !finite(particle.color) || !finite(particle.texcoords) ||
        stream.vertices.size() > std::numeric_limits<std::uint32_t>::max() - 3U) return;
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const Vec4 uv = particle.texcoords;
    const std::array<std::array<float, 2>, 4> texcoords{{
        {uv.x, uv.y}, {uv.x, uv.y + uv.w}, {uv.x + uv.z, uv.y}, {uv.x + uv.z, uv.y + uv.w},
    }};
    std::array<ParticleVertex, 4> vertices;
    Vec3 bounds_min = stream.bounds_min;
    Vec3 bounds_max = stream.bounds_max;
    for (std::size_t corner = 0; corner < 4; ++corner) {
        const float x = corners[corner][0] * c - corners[corner][1] * s;
        const float y = corners[corner][0] * s + corners[corner][1] * c;
        const Vec3 position = particle.position + axis_x * x + axis_y * y;
        if (!finite(position) || !std::isfinite(texcoords[corner][0]) ||
            !std::isfinite(texcoords[corner][1])) return;
        if (stream.vertices.empty() && corner == 0) {
            bounds_min = bounds_max = position;
        } else {
            bounds_min = {std::min(bounds_min.x, position.x),
                          std::min(bounds_min.y, position.y),
                          std::min(bounds_min.z, position.z)};
            bounds_max = {std::max(bounds_max.x, position.x),
                          std::max(bounds_max.y, position.y),
                          std::max(bounds_max.z, position.z)};
        }
        vertices[corner] = {position, particle.color, texcoords[corner][0], texcoords[corner][1]};
    }
    if (!finite(bounds_min) || !finite(bounds_max)) return;
    const auto base = static_cast<std::uint32_t>(stream.vertices.size());
    stream.vertices.insert(stream.vertices.end(), vertices.begin(), vertices.end());
    for (const std::uint32_t offset : {0U, 1U, 2U, 2U, 1U, 3U}) stream.indices.push_back(base + offset);
    stream.bounds_min = bounds_min;
    stream.bounds_max = bounds_max;
    ++stream.quads;
}

void mix_hash(std::uint64_t& hash, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        hash ^= (value >> shift) & 0xffU;
        hash *= 0x100000001b3ULL;
    }
}
void mix_hash(std::uint64_t& hash, const float value) { mix_hash(hash, std::bit_cast<std::uint32_t>(value)); }

} // namespace

float heat_distortion_pixel_change_bound(const float peak_vertex_alpha,
    const float distortion_amount, const std::int32_t width, const std::int32_t height) noexcept {
    // The heat shader draws blend_mix with ALPHA = texel alpha x vertex alpha
    // (at most a) and ALBEDO = the screen copy sampled with filter_linear at
    // SCREEN_UV + ALPHA x distortion x (a texel offset in [-1, 1] per axis).
    // So the sample moves at most a x distortion x width pixels in x and
    // a x distortion x height in y. A bilinear sample offset by fx, fy (each
    // clamped to one pixel, plus one step of 8-bit filter weight precision)
    // keeps weight (1 - fx)(1 - fy) on its own texel, so it differs from the
    // unshifted pixel by at most w = 1 - (1 - fx)(1 - fy) per channel. With
    // the covered pixel equal to the copy, blend_mix changes it by
    // ALPHA x (sample - pixel), at most a x w. The viewer's linear tonemap
    // without post effects keeps the copy, the blend and the capture in the
    // same 8-bit encoded values, and the store rounds to nearest (allowing
    // 0.6 of a step of conversion error), so each channel moves by at most
    // floor(255 a w + 0.6) whole steps. The bound sums three channels.
    const float alpha = std::clamp(peak_vertex_alpha, 0.0F, 1.0F);
    if (!(alpha > 0.0F)) return 0.0F;
    constexpr float filter_step = 1.0F / 256.0F;
    const float reach = alpha * distortion_amount;
    const float fx = std::min(1.0F, reach * static_cast<float>(std::max(width, 0)) + filter_step);
    const float fy = std::min(1.0F, reach * static_cast<float>(std::max(height, 0)) + filter_step);
    const float neighbour_weight = 1.0F - (1.0F - fx) * (1.0F - fy);
    const float steps = std::floor(255.0F * alpha * neighbour_weight + 0.6F);
    return 3.0F * steps / 255.0F;
}

std::string_view to_string(const RenderFamily family) noexcept {
    switch (family) {
    case RenderFamily::billboard: return "billboard";
    case RenderFamily::xy_aligned: return "xy_aligned";
    case RenderFamily::heat_saturation: return "heat_saturation";
    case RenderFamily::kites: return "kites";
    case RenderFamily::unsupported: return "unsupported";
    }
    return "unsupported";
}

std::string_view to_string(const Blend blend) noexcept {
    switch (blend) {
    case Blend::opaque: return "opaque";
    case Blend::additive: return "additive";
    case Blend::alpha: return "alpha";
    case Blend::modulate: return "modulate";
    case Blend::heat_distortion: return "heat_distortion";
    case Blend::bump_alpha: return "bump_alpha";
    }
    return "invalid";
}

std::string_view to_string(const DrawPhase phase) noexcept {
    switch (phase) {
    case DrawPhase::opaque: return "opaque";
    case DrawPhase::transparent: return "transparent";
    case DrawPhase::heat: return "heat";
    }
    return "invalid";
}

std::string_view to_string(const EffectDetachState state) noexcept {
    switch (state) {
    case EffectDetachState::draining: return "draining";
    case EffectDetachState::released: return "released";
    }
    return "invalid";
}

std::span<const LegacyBlendSelector> legacy_blend_selectors() noexcept { return selectors; }

EmitterRenderPlan plan_emitter(const EmitterDefinition& emitter, const std::size_t index) {
    EmitterRenderPlan plan;
    plan.emitter_index = index;
    plan.renderer_id = emitter.renderer_id;
    plan.blend_selector = emitter.blend_mode;
    plan.texture = emitter.color_texture;
    plan.tail_size = emitter.tail_size;
    plan.depth_test = !emitter.disable_depth_test;
    plan.order_in_phase = static_cast<std::uint32_t>(std::min<std::size_t>(index, 0xffffffffU));
    const auto fail = [&](std::string cause) {
        plan.drawable = false;
        plan.cause = std::move(cause);
        return plan;
    };

    switch (emitter.renderer_id) {
    case 22: plan.family = RenderFamily::billboard; break;
    case 28: plan.family = RenderFamily::xy_aligned; break;
    case 38: plan.family = RenderFamily::heat_saturation; break;
    case 52: plan.family = RenderFamily::kites; break;
    default: {
        const PluginInfo* info = find_plugin(emitter.renderer_id);
        return fail("renderer family " + std::string(info ? info->name : "unknown")
            + " has no adapter; the CPU runtime produces no data for it");
    }
    }

    if (emitter.blend_mode >= selectors.size()) {
        return fail("legacy blend selector " + std::to_string(emitter.blend_mode)
            + " is outside the fourteen-entry engine effect table");
    }
    const LegacyBlendSelector& selector = selectors[emitter.blend_mode];
    plan.program = selector.program;
    plan.technique = selector.technique;
    if (!emitter.cpu_ready) {
        return fail("emitter is not CPU-ready: " + emitter.unsupported_reason);
    }
    const bool heat_selector = emitter.blend_mode == 10;
    const bool heat_family = plan.family == RenderFamily::heat_saturation;
    // Every corpus heat emitter carries selector 1 or 2 with a distortion
    // texture (signed red/green offset, alpha mask), so the heat renderer draws
    // it as screen distortion, the only public heat-phase program. The authored
    // selector stays in blend_selector.
    if (heat_family && !heat_selector && emitter.blend_mode != 1 && emitter.blend_mode != 2) {
        return fail("heat renderer with selector " + std::to_string(emitter.blend_mode)
            + " has no derived distortion policy");
    }
    if (!heat_family && heat_selector) {
        return fail("PrimHeat selector outside the heat renderer has no derived distortion source");
    }
    if (!selector.supported) return fail(std::string(selector.cause));
    if (emitter.color_texture.empty()) return fail("emitter declares no colour texture");
    if (heat_family) {
        const LegacyBlendSelector& heat = selectors[10];
        plan.program = heat.program;
        plan.technique = heat.technique;
        plan.blend = Blend::heat_distortion;
        plan.phase = DrawPhase::heat;
        plan.drawable = true;
        return plan;
    }

    switch (emitter.blend_mode) {
    case 0: plan.blend = Blend::opaque; plan.phase = DrawPhase::opaque; plan.depth_write = true; break;
    case 1: plan.blend = Blend::additive; break;
    case 2: plan.blend = Blend::alpha; break;
    case 3: plan.blend = Blend::modulate; break;
    case 10: plan.blend = Blend::heat_distortion; plan.phase = DrawPhase::heat; break;
    case 11: plan.blend = Blend::bump_alpha; plan.normal_texture = emitter.normal_texture; break;
    default: return fail("selector has no blend policy");
    }
    plan.drawable = true;
    return plan;
}

std::vector<EmitterRenderPlan> plan_system(const SystemDefinition& system) {
    std::vector<EmitterRenderPlan> plans;
    plans.reserve(system.emitters.size());
    for (std::size_t index = 0; index < system.emitters.size(); ++index) {
        plans.push_back(plan_emitter(system.emitters[index], index));
    }
    return plans;
}

CameraFrame camera_frame_from_render(const std::array<float, 3>& eye,
                                     const std::array<float, 3>& target,
                                     const std::array<float, 3>& up) {
    const Vec3 position = render_to_particle(eye[0], eye[1], eye[2]);
    const Vec3 look = render_to_particle(target[0], target[1], target[2]);
    const Vec3 up_hint = render_to_particle(up[0], up[1], up[2]);
    const Vec3 forward = normalized(look - position);
    // The asset-to-render conversion is a proper rotation, so right-handed
    // look-at construction carries over unchanged.
    Vec3 right = normalized(cross(forward, up_hint));
    if (length(right) == 0.0F) right = {1.0F, 0.0F, 0.0F};
    const Vec3 camera_up = normalized(cross(right, forward));
    return {position, right, camera_up};
}

EmitterFrame emitter_frame_from_render(const std::array<float, 16>& m) {
    // C^-1 * M * C: the asset axes map through C to render (1,0,0), (0,0,-1)
    // and (0,1,0) respectively.
    const auto column = [&](const std::size_t index) {
        return render_to_particle(m[index * 4], m[index * 4 + 1], m[index * 4 + 2]);
    };
    EmitterFrame frame;
    frame.origin = column(3);
    frame.basis.x = column(0);
    frame.basis.y = column(2) * -1.0F;
    frame.basis.z = column(1);
    return frame;
}

void VertexStream::clear() {
    vertices.clear();
    indices.clear();
    quads = 0;
    bounds_min = bounds_max = {};
}

void build_stream(const EmitterRenderPlan& plan, const std::span<const Particle> particles,
                  const CameraFrame& camera, VertexStream& stream) {
    if (!plan.drawable) return;
    const bool xy = plan.family == RenderFamily::xy_aligned;
    for (const Particle& particle : particles) {
        if (particle.emitter_index != plan.emitter_index) continue;
        if (!finite(particle.position) || !std::isfinite(particle.size)) continue;
        const float size = particle.size;
        if (plan.family == RenderFamily::kites) {
            // Kite: the quad plane contains the velocity and faces the camera;
            // the tail corner trails the velocity by the alo-viewer 3/4-pi turn.
            if (!finite(particle.velocity) || !std::isfinite(plan.tail_size) ||
                !std::isfinite(dot(particle.velocity, particle.velocity))) continue;
            const Vec3 along = normalized(particle.velocity);
            Vec3 across{};
            Vec3 axis_y = along;
            if (length(along) != 0.0F) {
                const Vec3 to_camera = camera.position - particle.position;
                if (!finite(camera.position) || !finite(to_camera)) continue;
                const Vec3 across_raw = cross(along, to_camera);
                if (!finite(across_raw) || !std::isfinite(dot(across_raw, across_raw))) continue;
                across = normalized(across_raw);
            }
            if (length(along) == 0.0F || length(across) == 0.0F) {
                // No defined direction of travel: face the camera instead.
                across = camera.right;
                axis_y = camera.up;
            }
            const float half = size * 0.5F;
            const float tail = (size + plan.tail_size) * 0.5F;
            append_quad(stream, particle, across, axis_y,
                {{{-tail, tail}, {-half, -half}, {half, half}, {half, -half}}}, 0.75F * pi);
            continue;
        }
        const Vec3 axis_x = xy ? Vec3{1.0F, 0.0F, 0.0F} : camera.right;
        const Vec3 axis_y = xy ? Vec3{0.0F, 1.0F, 0.0F} : camera.up;
        append_quad(stream, particle, axis_x, axis_y,
            {{{-size, size}, {-size, -size}, {size, size}, {size, -size}}},
            particle.rotation * 2.0F * pi);
    }
}

std::uint64_t stream_hash(const VertexStream& stream, std::uint64_t seed) {
    std::uint64_t hash = seed;
    mix_hash(hash, static_cast<std::uint32_t>(stream.vertices.size()));
    for (const ParticleVertex& vertex : stream.vertices) {
        mix_hash(hash, vertex.position.x); mix_hash(hash, vertex.position.y);
        mix_hash(hash, vertex.position.z);
        mix_hash(hash, vertex.color.x); mix_hash(hash, vertex.color.y);
        mix_hash(hash, vertex.color.z); mix_hash(hash, vertex.color.w);
        mix_hash(hash, vertex.u); mix_hash(hash, vertex.v);
    }
    for (const std::uint32_t index : stream.indices) mix_hash(hash, index);
    return hash;
}

std::string hex64(const std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 0; index < 16; ++index) {
        text[15 - index] = digits[(value >> (index * 4)) & 15U];
    }
    return text;
}

struct EffectRegistry::Instance final {
    Instance(EffectHandle id, SystemDefinition definition, const std::uint32_t seed,
             const std::size_t capacity, std::optional<MeshBinding> mesh_binding)
        : handle(id), leave_particles(definition.leave_particles), plans(plan_system(definition)),
          cpu(std::move(definition), seed, capacity, std::move(mesh_binding)) {}

    EffectHandle handle{};
    bool leave_particles{};
    std::vector<EmitterRenderPlan> plans;
    std::vector<std::uint64_t> resources;
    CpuSystem cpu;
    VertexStream stream;
    float brightness{1.0F};
};

EffectRegistry::EffectRegistry(RenderBackend& backend) : backend_(&backend) {}

EffectRegistry::~EffectRegistry() {
    while (!instances_.empty()) static_cast<void>(release(instances_.back()->handle));
}

// Handles only grow (spawn appends next_handle_++) and release erases in place, so instances_ is
// sorted by handle: a lookup is a binary search, not a scan (#439: presented per running effect
// every drawn frame).
EffectRegistry::Instance* EffectRegistry::find(const EffectHandle handle) const noexcept {
    const auto found = std::lower_bound(instances_.begin(), instances_.end(), handle,
        [](const auto& instance, const EffectHandle wanted) { return instance->handle < wanted; });
    return found != instances_.end() && (*found)->handle == handle ? found->get() : nullptr;
}

core::Diagnostic EffectRegistry::unknown(const EffectHandle handle) {
    core::Diagnostic diagnostic{std::string(diagnostic_codes::unknown_effect), core::Severity::error,
        "effect handle " + std::to_string(handle) + " is not live", {}, {}, {}, {}};
    diagnostics_.push_back(diagnostic);
    return diagnostic;
}

core::Result<EffectHandle> EffectRegistry::spawn(
    SystemDefinition system, const std::uint32_t seed, const std::size_t max_particles) {
    return spawn_impl(std::move(system),seed,max_particles,std::nullopt);
}

core::Result<EffectHandle> EffectRegistry::spawn(
    SystemDefinition system, const std::uint32_t seed, const std::size_t max_particles,
    MeshBinding mesh_binding) {
    return spawn_impl(std::move(system),seed,max_particles,std::move(mesh_binding));
}

core::Result<EffectHandle> EffectRegistry::spawn_impl(
    SystemDefinition system, const std::uint32_t seed, const std::size_t max_particles,
    std::optional<MeshBinding> mesh_binding) {
    const auto fail_mesh=[&](const std::string& message){
        core::Diagnostic diagnostic{std::string(diagnostic_codes::mesh_binding),
            core::Severity::error,message,{},{},{},{}};
        diagnostics_.push_back(diagnostic);
        return core::Result<EffectHandle>::failure(std::move(diagnostic));
    };
    bool needs_mesh{},needs_surface{};
    for(std::size_t index=0;index<system.emitters.size();++index){
        const auto& emitter=system.emitters[index];
        if(system.version!=AloParticleVersion::legacy_v1||!emitter.cpu_ready||
           emitter.creator_id!=35)continue;
        needs_mesh=true;
        if((emitter.mesh_mode!=MeshSpawnMode::random_vertex&&
            emitter.mesh_mode!=MeshSpawnMode::random_surface&&
            emitter.mesh_mode!=MeshSpawnMode::every_vertex)||
            !std::isfinite(emitter.mesh_surface_offset))
            return fail_mesh("EnhancedMesh emitter has invalid mode or surface offset");
        needs_surface|=emitter.mesh_mode==MeshSpawnMode::random_surface;
        std::size_t node=index;
        for(std::size_t hops=0;hops<=system.emitters.size();++hops){
            const auto parent=system.emitters[node].parent_emitter;
            if(parent==EmitterDefinition::no_parent)break;
            if(parent>=system.emitters.size()||hops==system.emitters.size())
                return fail_mesh("EnhancedMesh parent metadata contains an invalid or cyclic link");
            node=parent;
        }
    }
    if(needs_mesh&&!mesh_binding)return fail_mesh("EnhancedMesh emitter requires a mesh binding");
    if(mesh_binding){
        const auto checked=validate_mesh_binding(*mesh_binding,
            needs_surface?MeshSpawnMode::random_surface:MeshSpawnMode::random_vertex);
        if(!checked)return fail_mesh(checked.error().message);
        std::size_t vertices{};
        for(const auto& submesh:mesh_binding->geometry.submeshes)vertices+=submesh.vertices.size();
        for(const auto& emitter:system.emitters)if(system.version==AloParticleVersion::legacy_v1&&
            emitter.cpu_ready&&emitter.creator_id==35){
            const long double base=emitter.bursting?
                std::trunc(static_cast<long double>(emitter.particles_per_interval)):1.0L;
            const long double limit=std::ldexp(1.0L,std::numeric_limits<std::size_t>::digits);
            if(!std::isfinite(emitter.particles_per_interval)||base<0||
                base>=limit||
                (emitter.mesh_mode==MeshSpawnMode::every_vertex&&
                 base*vertices>=limit))
                return fail_mesh("EnhancedMesh spawn count overflows");
        }
    }
    if (next_handle_ == std::numeric_limits<EffectHandle>::max()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::backend_resource),
            core::Severity::error, "effect handle space is exhausted", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<EffectHandle>::failure(std::move(diagnostic));
    }
    auto instance = std::make_unique<Instance>(next_handle_++, std::move(system), seed,
        max_particles, std::move(mesh_binding));
    instance->resources.assign(instance->plans.size(), 0U);
    for (std::size_t index = 0; index < instance->plans.size(); ++index) {
        EmitterRenderPlan& plan = instance->plans[index];
        if (!plan.drawable) continue;
        const std::uint64_t resource = backend_->create_emitter(plan);
        if (resource == 0U) {
            // The backend refused (for example a texture that did not resolve
            // or a shader the compiler rejected): the emitter is reported as
            // not drawn with that cause instead of being drawn another way.
            plan.drawable = false;
            const std::string detail = backend_->failure_cause();
            plan.cause = "backend could not create the emitter's resources"
                + (detail.empty() ? std::string{} : ": " + detail);
            diagnostics_.push_back({std::string(diagnostic_codes::backend_resource),
                core::Severity::warning, "emitter " + std::to_string(index) + ": " + plan.cause,
                {}, {}, {}, {}});
            continue;
        }
        instance->resources[index] = resource;
        ++live_resources_;
    }
    const EffectHandle handle = instance->handle;
    instances_.push_back(std::move(instance));
    return core::Result<EffectHandle>::success(handle);
}

core::Result<void> EffectRegistry::set_frame(const EffectHandle handle, const EmitterFrame& frame) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    if(!finite(frame.origin)||!finite(frame.basis.x)||!finite(frame.basis.y)||
       !finite(frame.basis.z)){
        core::Diagnostic diagnostic{std::string(diagnostic_codes::invalid_value),
            core::Severity::error,"emitter frame contains a non-finite value",{},{},{},{}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    instance->cpu.set_origin(frame.origin);
    instance->cpu.set_basis(frame.basis);
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::set_mesh_frame(const EffectHandle handle, const MeshFrame& frame) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    auto result=instance->cpu.set_mesh_frame(frame);
    if(!result)diagnostics_.push_back(result.error());
    return result;
}

core::Result<void> EffectRegistry::set_brightness(const EffectHandle handle, const float brightness) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    if (!std::isfinite(brightness) || brightness < 0.0F) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::invalid_value), core::Severity::error,
            "effect brightness must be finite and not negative", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    instance->brightness = brightness;
    return core::Result<void>::success();
}

core::Result<EffectFrameStats> EffectRegistry::advance(
    const EffectHandle handle, const float delta_seconds, const CameraFrame& camera) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<EffectFrameStats>::failure(unknown(handle));
    EffectFrameStats stats;
    stats.advance = instance->cpu.advance(delta_seconds);
    publish(*instance, camera, &stats);
    return core::Result<EffectFrameStats>::success(std::move(stats));
}

core::Result<void> EffectRegistry::present(const EffectHandle handle, const CameraFrame& camera) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    instance->cpu.follow_emitter();
    publish(*instance, camera, nullptr);
    return core::Result<void>::success();
}

void EffectRegistry::publish(Instance& instance, const CameraFrame& camera, EffectFrameStats* const stats) {
    const std::span<const Particle> live = instance.cpu.particles();
    if (stats != nullptr) {
        stats->particles = live.size();
        stats->hash = 0xcbf29ce484222325ULL;
        stats->emitters.resize(instance.plans.size());
        for (const Particle& particle : live) {
            if (particle.emitter_index < stats->emitters.size()) ++stats->emitters[particle.emitter_index].particles;
        }
    }
    for (std::size_t index = 0; index < instance.plans.size(); ++index) {
        instance.stream.clear();
        build_stream(instance.plans[index], live, camera, instance.stream);
        if (instance.brightness != 1.0F) {
            // BP-45, as FoC's renderer: the vertex colour times the
            // brightness (FoC truncates to 8 bits; the stream keeps floats).
            for (ParticleVertex& vertex : instance.stream.vertices) {
                vertex.color = {vertex.color.x * instance.brightness, vertex.color.y * instance.brightness,
                                vertex.color.z * instance.brightness, vertex.color.w * instance.brightness};
            }
        }
        const bool drawn = instance.resources[index] != 0U;
        if (drawn) backend_->update_emitter(instance.resources[index], instance.stream);
        if (stats == nullptr) continue;
        EmitterFrameStats& emitter = stats->emitters[index];
        emitter.quads = instance.stream.quads;
        for (const ParticleVertex& vertex : instance.stream.vertices) {
            emitter.maximum_alpha = std::max(emitter.maximum_alpha, vertex.color.w);
        }
        emitter.hash = stream_hash(instance.stream);
        emitter.drawn = drawn;
        stats->hash = stream_hash(instance.stream, stats->hash);
        if (drawn && instance.stream.quads != 0) {
            if (!stats->has_bounds) {
                stats->bounds_min = instance.stream.bounds_min;
                stats->bounds_max = instance.stream.bounds_max;
                stats->has_bounds = true;
            } else {
                stats->bounds_min = {std::min(stats->bounds_min.x, instance.stream.bounds_min.x),
                    std::min(stats->bounds_min.y, instance.stream.bounds_min.y),
                    std::min(stats->bounds_min.z, instance.stream.bounds_min.z)};
                stats->bounds_max = {std::max(stats->bounds_max.x, instance.stream.bounds_max.x),
                    std::max(stats->bounds_max.y, instance.stream.bounds_max.y),
                    std::max(stats->bounds_max.z, instance.stream.bounds_max.z)};
            }
        }
    }
    if (stats == nullptr) return;
    stats->detached = instance.cpu.detached();
    stats->finished = instance.cpu.finished();
}

core::Result<void> EffectRegistry::release(const EffectHandle handle) {
    const auto found = std::lower_bound(instances_.begin(), instances_.end(), handle,
        [](const auto& instance, const EffectHandle wanted) { return instance->handle < wanted; });
    if (found == instances_.end() || (*found)->handle != handle) return core::Result<void>::failure(unknown(handle));
    for (const std::uint64_t resource : (*found)->resources) {
        if (resource == 0U) continue;
        backend_->destroy_emitter(resource);
        --live_resources_;
    }
    instances_.erase(found);
    return core::Result<void>::success();
}

core::Result<EffectDetachState> EffectRegistry::detach(const EffectHandle handle) {
    Instance* const found = find(handle);
    if (found == nullptr) return core::Result<EffectDetachState>::failure(unknown(handle));
    if (!found->leave_particles) {
        // The source drops its last reference here, deleting every emitter.
        auto released = release(handle);
        if (!released) return core::Result<EffectDetachState>::failure(released.error());
        return core::Result<EffectDetachState>::success(EffectDetachState::released);
    }
    found->cpu.detach();
    return core::Result<EffectDetachState>::success(EffectDetachState::draining);
}

core::Result<void> EffectRegistry::stop_emission(const EffectHandle handle) {
    Instance* const found = find(handle);
    if (found == nullptr) return core::Result<void>::failure(unknown(handle));
    found->cpu.detach();
    return core::Result<void>::success();
}

const std::vector<EmitterRenderPlan>* EffectRegistry::plans(const EffectHandle handle) const {
    const Instance* const instance = find(handle);
    return instance != nullptr ? &instance->plans : nullptr;
}

std::size_t EffectRegistry::live_effects() const noexcept { return instances_.size(); }
std::size_t EffectRegistry::live_backend_resources() const noexcept { return live_resources_; }
std::span<const core::Diagnostic> EffectRegistry::diagnostics() const noexcept { return diagnostics_; }

} // namespace eawr::presentation::particles
