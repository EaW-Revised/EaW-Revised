#include "render_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

// Quad construction follows the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp):
// corner order, the 0-1-2 / 2-1-3 index pattern, the texture-coordinate
// assignment. Legacy kite geometry follows MD-07. Blend, depth-write and phase policy
// are taken from the render state of the public Engine/Prim* effects that the
// legacy selector names; see docs/reports/P1-08-rendering.md.

namespace eawr::presentation::particles {
using namespace render_detail;

struct EffectRegistry::Instance final {
    Instance(EffectHandle id, SystemDefinition definition, const std::uint32_t seed,
             const std::size_t capacity, std::optional<MeshBinding> mesh_binding)
        : handle(id), leave_particles(definition.leave_particles), plans(plan_system(definition)),
          cpu(std::move(definition), seed, capacity, std::move(mesh_binding)), streams(plans.size()) {
        for (std::size_t index = 0; index < plans.size(); ++index) {
            if (!plans[index].drawable) continue;
            const auto count = cpu.emitter_capacities()[index].reserved;
            streams[index].vertices.reserve(count * (plans[index].triangles ? 3U : 4U));
            streams[index].indices.reserve(count * (plans[index].triangles ? 3U : 6U));
            if (plans[index].sort_particles) streams[index].sorted_particles.reserve(count);
        }
    }

    EffectHandle handle{};
    bool leave_particles{};
    std::vector<EmitterRenderPlan> plans;
    std::vector<std::uint64_t> resources;
    CpuSystem cpu;
    // One stream per plan, kept between frames so a grown stream allocates nothing (#439); the
    // batched calls build them on the executor and upload them afterwards (#638).
    std::vector<VertexStream> streams;
    float brightness{1.0F};
    bool visible{true};
    bool prepared{true};
    bool stepped{};
    bool deferred{};
    bool updated{};
    bool was_rendered{};
    bool backend_visible{true};
    float accumulated{};
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
    static_cast<void>(instance->cpu.set_detail(detail_));
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

core::Result<void> EffectRegistry::set_detail(ParticleDetail detail) {
    if (!std::isfinite(detail.global) || !std::isfinite(detail.local)) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::invalid_value), core::Severity::error,
            "particle detail inputs must be finite", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    detail.global = std::clamp(detail.global, 0.0F, 1.0F);
    detail.local = std::clamp(detail.local, 0.0F, 1.0F);
    if (detail == detail_) return core::Result<void>::success();
    detail_ = detail;
    for (const auto& instance : instances_) static_cast<void>(instance->cpu.set_detail(detail));
    return core::Result<void>::success();
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
    culling_ = backend_->culling_frame();
    step(*instance, delta_seconds, stats);
    build(*instance, camera, &stats);
    upload(*instance, true, stream_hashes_);
    return core::Result<EffectFrameStats>::success(std::move(stats));
}

core::Result<void> EffectRegistry::present(const EffectHandle handle, const CameraFrame& camera) {
    Instance* const instance = find(handle);
    if (instance == nullptr) return core::Result<void>::failure(unknown(handle));
    instance->cpu.follow_emitter();
    culling_ = backend_->culling_frame();
    build(*instance, camera, nullptr);
    upload(*instance, false, false);
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::advance_all(const std::span<const EffectHandle> handles,
    const float delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats) {
    return advance_batch(handles, delta_seconds, {}, camera, stats);
}

core::Result<void> EffectRegistry::advance_all(const std::span<const EffectHandle> handles,
    const std::span<const float> delta_seconds, const CameraFrame& camera, std::vector<EffectFrameStats>& stats) {
    if (delta_seconds.size() != handles.size()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "the particle batch needs one delta per effect", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    return advance_batch(handles, 0.0F, delta_seconds, camera, stats);
}

core::Result<void> EffectRegistry::advance_batch(const std::span<const EffectHandle> handles,
    const float uniform_delta, const std::span<const float> deltas, const CameraFrame& camera,
    std::vector<EffectFrameStats>& stats) {
    if (auto resolved = resolve(handles); !resolved) return resolved;
    culling_ = backend_->culling_frame();
    stats.resize(batch_.size());
    auto ran = run_batch([&](const std::size_t index) {
        // The slot keeps its per-emitter vector's storage; every field starts over.
        EffectFrameStats& slot = stats[index];
        std::vector<EmitterFrameStats> emitters = std::move(slot.emitters);
        emitters.clear();
        slot = EffectFrameStats{};
        slot.emitters = std::move(emitters);
        Instance& instance = *batch_[index];
        step(instance, deltas.empty() ? uniform_delta : deltas[index], slot);
        build(instance, camera, &slot);
    });
    if (!ran) return ran;
    for (Instance* const instance : batch_) upload(*instance, true, stream_hashes_);
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::present_all(const std::span<const EffectHandle> handles, const CameraFrame& camera) {
    if (auto resolved = resolve(handles); !resolved) return resolved;
    culling_ = backend_->culling_frame();
    auto ran = run_batch([&](const std::size_t index) {
        Instance& instance = *batch_[index];
        instance.cpu.follow_emitter();
        build(instance, camera, nullptr);
    });
    if (!ran) return ran;
    for (Instance* const instance : batch_) upload(*instance, false, false);
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::resolve(const std::span<const EffectHandle> handles) {
    batch_.clear();
    batch_.reserve(handles.size());
    for (const EffectHandle handle : handles) {
        Instance* const instance = find(handle);
        if (instance == nullptr) {
            batch_.clear();
            return core::Result<void>::failure(unknown(handle));
        }
        batch_.push_back(instance);
    }
    // Two tasks on one instance would race: a repeated handle fails the batch before any step.
    sorted_.assign(batch_.begin(), batch_.end());
    std::sort(sorted_.begin(), sorted_.end(), [](const Instance* left, const Instance* right) {
        return left->handle < right->handle;
    });
    const auto repeated = std::adjacent_find(sorted_.begin(), sorted_.end());
    if (repeated != sorted_.end()) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "effect handle " + std::to_string((*repeated)->handle) + " appears twice in one batch", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        batch_.clear();
        return core::Result<void>::failure(std::move(diagnostic));
    }
    return core::Result<void>::success();
}

core::Result<void> EffectRegistry::run_batch(const std::function<void(std::size_t)>& task) {
    const std::size_t count = batch_.size();
    if (executor_ == nullptr || count < 2) {
        for (std::size_t index = 0; index < count; ++index) task(index);
        return core::Result<void>::success();
    }
    // Contiguous slices of the batch in order, at most ADR-009's 64 partitions, which the pool's
    // workers claim; no two slices share an instance.
    constexpr std::size_t max_tasks = 64;
    const std::size_t tasks = std::min(count, max_tasks);
    const bool ran = executor_->run(tasks, [&](const std::size_t slice) {
        const std::size_t first = count * slice / tasks;
        const std::size_t last = count * (slice + 1) / tasks;
        for (std::size_t index = first; index < last; ++index) task(index);
    });
    ++work_.batches;
    work_.tasks += tasks;
    if (!ran) {
        core::Diagnostic diagnostic{std::string(diagnostic_codes::batch), core::Severity::error,
            "the particle executor could not run a batch of " + std::to_string(count) + " effects", {}, {}, {}, {}};
        diagnostics_.push_back(diagnostic);
        return core::Result<void>::failure(std::move(diagnostic));
    }
    return core::Result<void>::success();
}

void EffectRegistry::set_executor(const StepExecutor* const executor) noexcept { executor_ = executor; }
void EffectRegistry::set_stream_hashes(const bool on) noexcept { stream_hashes_ = on; }
const RegistryWorkCounts& EffectRegistry::work() const noexcept { return work_; }

void EffectRegistry::step(Instance& instance, const float delta_seconds, EffectFrameStats& stats) const {
    instance.stepped = false;
    instance.deferred = false;
    if (!culling_.enabled) {
        stats.advance = instance.cpu.advance(delta_seconds);
        instance.stepped = true;
        return;
    }
    if (!std::isfinite(delta_seconds) || delta_seconds <= 0.0F ||
        delta_seconds > std::numeric_limits<float>::max() - instance.accumulated ||
        instance.accumulated + delta_seconds > std::numeric_limits<float>::max() - instance.cpu.presentation_time()) return;
    instance.accumulated += delta_seconds;
    // PS-38: the first update establishes the transform; thereafter successful
    // submission admits the full saved time, otherwise equality at 0.1 s admits.
    // PS-42: zero delta cannot initialize or consume a generation.
    if (instance.updated && !instance.was_rendered && instance.accumulated < 0.1F) {
        instance.deferred = delta_seconds > 0.0F;
        instance.cpu.follow_emitter();
        return;
    }
    instance.was_rendered = false;
    if (instance.updated && (instance.accumulated == 0.0F || instance.cpu.finished())) return;
    stats.advance = instance.cpu.advance(instance.accumulated);
    instance.accumulated = 0.0F;
    instance.updated = true;
    instance.stepped = true;
}

void EffectRegistry::build(Instance& instance, const CameraFrame& camera, EffectFrameStats* const stats) const {
    const std::span<const Particle> live = instance.cpu.particles();
    const ParticleBounds bounds = particle_bounds(instance.plans, live);
    instance.visible = intersects(bounds, culling_);
    instance.prepared = !culling_.enabled || (instance.visible && !instance.cpu.finished());
    const bool hash = stats != nullptr && stream_hashes_;
    if (stats != nullptr) {
        stats->particles = live.size();
        stats->elapsed_seconds = instance.cpu.presentation_time();
        stats->deferred_seconds = instance.accumulated;
        stats->detached = instance.cpu.detached();
        stats->finished = instance.cpu.finished();
        stats->hash = hash && instance.prepared ? 0xcbf29ce484222325ULL : 0U;
        stats->emitters.resize(instance.plans.size());
        for (const Particle& particle : live) {
            if (particle.emitter_index < stats->emitters.size()) ++stats->emitters[particle.emitter_index].particles;
        }
    }
    if (!instance.prepared) {
        if (stats != nullptr) {
            stats->has_bounds = bounds.valid;
            stats->bounds_min = bounds.minimum;
            stats->bounds_max = bounds.maximum;
        }
        return;
    }
    for (std::size_t index = 0; index < instance.plans.size(); ++index) {
        VertexStream& stream = instance.streams[index];
        stream.clear();
        // PS-34: heat phase admission is independent of the ordinary slot mask.
        if (detail_.heat || instance.plans[index].phase != DrawPhase::heat)
            build_stream(instance.plans[index], live, camera, stream);
        if (instance.brightness != 1.0F) {
            // PS-23/BP-45: byte channels receive brightness independently.
            // Out-of-range caller brightness saturates as a presentation safety policy.
            for (ParticleVertex& vertex : stream.vertices) {
                const auto channel=[&](const float value){return
                    std::clamp(std::trunc(value*255*instance.brightness),0.0F,255.0F)/255;};
                vertex.color = {channel(vertex.color.x),channel(vertex.color.y),channel(vertex.color.z),channel(vertex.color.w)};
            }
        }
        if (stats == nullptr) continue;
        const bool drawn = instance.resources[index] != 0U;
        EmitterFrameStats& emitter = stats->emitters[index];
        emitter.quads = stream.quads;
        emitter.triangles = stream.triangles;
        for (const ParticleVertex& vertex : stream.vertices) {
            emitter.maximum_alpha = std::max(emitter.maximum_alpha, vertex.color.w);
        }
        if (hash) {
            emitter.hash = stream_hash(stream);
            stats->hash = stream_hash(stream, stats->hash);
        }
        emitter.drawn = drawn;
        if (drawn && !stream.indices.empty()) {
            if (!stats->has_bounds) {
                stats->bounds_min = stream.bounds_min;
                stats->bounds_max = stream.bounds_max;
                stats->has_bounds = true;
            } else {
                stats->bounds_min = {std::min(stats->bounds_min.x, stream.bounds_min.x),
                    std::min(stats->bounds_min.y, stream.bounds_min.y),
                    std::min(stats->bounds_min.z, stream.bounds_min.z)};
                stats->bounds_max = {std::max(stats->bounds_max.x, stream.bounds_max.x),
                    std::max(stats->bounds_max.y, stream.bounds_max.y),
                    std::max(stats->bounds_max.z, stream.bounds_max.z)};
            }
        }
    }
    if (stats == nullptr) return;
    stats->detached = instance.cpu.detached();
    stats->finished = instance.cpu.finished();
}

void EffectRegistry::upload(Instance& instance, const bool stepped, const bool hashed) {
    backend_->record_group_work(instance.visible, instance.prepared, stepped && instance.stepped);
    work_.groups_visible += instance.visible ? 1U : 0U;
    work_.groups_prepared += instance.prepared ? 1U : 0U;
    if (stepped && instance.deferred) ++work_.updates_deferred;
    if (stepped) { if (instance.stepped) ++work_.steps; }
    else ++work_.presents;
    // PS-39: hide retained geometry when culled; re-entry rebuilds it before
    // restoring visibility. Backend calls remain in caller order.
    if (!instance.prepared) {
        if (instance.backend_visible) {
            for (const auto resource : instance.resources) if (resource != 0U) backend_->set_emitter_visible(resource, false);
            instance.backend_visible = false;
        }
        return;
    }
    for (std::size_t index = 0; index < instance.plans.size(); ++index) {
        if (instance.resources[index] == 0U) continue;
        backend_->update_emitter(instance.resources[index], instance.streams[index]);
        if (!instance.backend_visible) backend_->set_emitter_visible(instance.resources[index], true);
        if (!instance.streams[index].indices.empty()) instance.was_rendered = true;
        ++work_.uploads;
    }
    instance.backend_visible = true;
    work_.streams_built += instance.plans.size();
    if (hashed) work_.streams_hashed += instance.plans.size();
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
