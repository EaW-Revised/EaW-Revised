#pragma once

#include "eawr/core/result.hpp"
#include "eawr/presentation/particles/render.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Visibility-driven lifecycle of one particle system attached to an animated
// host bone. The source model is the pinned MIT alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4, DirectX9/RenderObject.cpp
// (Update, SpawnProxy, KillProxy) and DirectX9/ParticleSystemInstance.cpp
// (Update, Detach). The rules and their divergences are in
// docs/behaviour/particle-attachment-visibility.md.
//
// Header-only: it composes the public EffectRegistry API and adds no source
// file to any build. It is presentation state only; no simulation, snapshot
// or replay code reads it.
namespace eawr::presentation::particles {

// What happens when a host bone becomes visible again after its generation was
// detached. `respawn` is the reference rule: a fresh instance starts while the
// detached one keeps draining. `stay_detached` never starts a second
// generation; it is the conservative choice for owners that must not stack
// instances. `reset` is the debug build's group visibility reset (BP-48): the group is
// reset when its host shows again (it drops every particle,
// the burst count and the internal clock), so the detached generation's
// drains vanish as a fresh one starts.
enum class ReappearancePolicy : std::uint8_t { respawn, stay_detached, reset };

[[nodiscard]] inline std::string_view to_string(const ReappearancePolicy policy) noexcept {
    switch (policy) {
    case ReappearancePolicy::respawn: return "respawn";
    case ReappearancePolicy::stay_detached: return "stay_detached";
    case ReappearancePolicy::reset: return "reset";
    }
    return "respawn";
}

// Generation 0 keeps the owner's seed, so a host that never hides reproduces
// the unmanaged stream bit for bit. Later generations take a distinct,
// deterministic, nonzero seed.
[[nodiscard]] constexpr std::uint32_t generation_seed(const std::uint32_t seed,
                                                      const std::uint32_t generation) noexcept {
    if (generation == 0) return seed;
    std::uint32_t value = seed ^ (generation * 0x9e3779b9U);
    value ^= value >> 16U;
    value *= 0x7feb352dU;
    value ^= value >> 15U;
    return value == 0 ? 1U : value;
}

// Folds one instance's frame stats into a running total. The first instance
// is copied exactly, so one live instance passes through unchanged. Later
// instances add counts per emitter, widen the bounds and chain the hash in
// the order given; `finished` holds only when every instance finished.
inline void merge_frame_stats(EffectFrameStats& total, const EffectFrameStats& next, const bool first) {
    if (first) {
        total = next;
        return;
    }
    const auto chain = [](const std::uint64_t left, const std::uint64_t right) {
        return (left ^ right) * 0x100000001b3ULL;
    };
    total.advance.spawned += next.advance.spawned;
    total.advance.requested += next.advance.requested;
    total.advance.killed += next.advance.killed;
    total.advance.dropped_at_capacity += next.advance.dropped_at_capacity;
    total.advance.child_instances_started += next.advance.child_instances_started;
    total.advance.child_instances_detached += next.advance.child_instances_detached;
    total.advance.death_bursts += next.advance.death_bursts;
    total.advance.instances_dropped_at_capacity += next.advance.instances_dropped_at_capacity;
    total.particles += next.particles;
    if (total.emitters.size() < next.emitters.size()) total.emitters.resize(next.emitters.size());
    for (std::size_t index = 0; index < next.emitters.size(); ++index) {
        EmitterFrameStats& into = total.emitters[index];
        const EmitterFrameStats& from = next.emitters[index];
        into.particles += from.particles;
        into.quads += from.quads;
        into.triangles += from.triangles;
        into.hash = chain(into.hash, from.hash);
        into.drawn = into.drawn || from.drawn;
        into.maximum_alpha = std::max(into.maximum_alpha, from.maximum_alpha);
    }
    total.hash = chain(total.hash, next.hash);
    if (next.has_bounds) {
        if (!total.has_bounds) {
            total.bounds_min = next.bounds_min;
            total.bounds_max = next.bounds_max;
        } else {
            total.bounds_min = {std::min(total.bounds_min.x, next.bounds_min.x),
                std::min(total.bounds_min.y, next.bounds_min.y), std::min(total.bounds_min.z, next.bounds_min.z)};
            total.bounds_max = {std::max(total.bounds_max.x, next.bounds_max.x),
                std::max(total.bounds_max.y, next.bounds_max.y), std::max(total.bounds_max.z, next.bounds_max.z)};
        }
        total.has_bounds = true;
    }
    total.detached = total.detached || next.detached;
    total.finished = total.finished && next.finished;
}

// The outcome of one presentation sample.
struct AttachmentStep final {
    bool visible{};
    // The generation started on this sample, if any.
    std::optional<EffectHandle> spawned;
    // The one detach this sample made, if the active generation was hidden.
    std::optional<EffectDetachState> detached;
    // Finished drains its owner released on this sample.
    std::size_t drains_released{};
    // Drains released before finishing because the draining bound was full.
    std::size_t drains_cut_short{};
    // Drains dropped because the host showed again under the reset policy.
    std::size_t drains_reset{};
    // Instances alive after this sample: the active one plus every drain.
    std::size_t live_instances{};
    bool active{};
    // Merged over every instance advanced on this sample, in handle order.
    // Empty (no emitters, no bounds, hash 0) when no instance is alive.
    EffectFrameStats stats;
};

// Main-thread visibility decisions and frames, ready for a registry batch. Each handle gets
// its own delta so a newly spawned generation stays at age zero while existing drains age.
// Keep its lifecycle alive and unchanged until complete_step consumes the batch's statistics.
struct PreparedAttachmentStep final {
    AttachmentStep result;
    std::vector<EffectHandle> handles;
    std::vector<float> deltas;
};

// Owns the attachment's instances in one EffectRegistry: at most one active
// generation that follows the host, plus detached generations that drain. It
// never releases anything the caller did not hand it, and it is not RAII:
// call release_all() before the registry goes away.
class AttachmentLifecycle final {
public:
    // Spawns one generation with the given seed into the given registry.
    using Spawn = std::function<core::Result<EffectHandle>(EffectRegistry&, std::uint32_t)>;
    static constexpr std::size_t default_max_draining = 8;

    AttachmentLifecycle(EffectRegistry& registry, Spawn spawn, const std::uint32_t seed,
                        const ReappearancePolicy policy,
                        const std::size_t max_draining = default_max_draining)
        : registry_(&registry), spawn_(std::move(spawn)), seed_(seed), policy_(policy),
          max_draining_(max_draining == 0 ? 1 : max_draining) {}

    // One presentation sample. `visible` is the host bone's sampled visibility
    // (animation::Pose::bones[bone].visible from Player::sample). The frames
    // are the host's frames for the same sample; `mesh` is null unless the
    // effect was spawned with a mesh binding. Order: the visibility edge (one
    // detach or one spawn), then the host frames to every live instance, then
    // one advance each, then the owner release of finished drains.
    [[nodiscard]] core::Result<AttachmentStep> step(const bool visible, const EmitterFrame& frame,
                                                    const MeshFrame* mesh, const float delta_seconds,
                                                    const CameraFrame& camera) {
        auto prepared = prepare_step(visible, frame, mesh, delta_seconds);
        if (!prepared) return core::Result<AttachmentStep>::failure(prepared.error());
        std::vector<EffectFrameStats> stats;
        stats.reserve(prepared.value().handles.size());
        for (std::size_t index = 0; index < prepared.value().handles.size(); ++index) {
            auto advanced = registry_->advance(prepared.value().handles[index], prepared.value().deltas[index], camera);
            if (!advanced) return core::Result<AttachmentStep>::failure(advanced.error());
            stats.push_back(std::move(advanced.value()));
        }
        return complete_step(std::move(prepared.value()), stats);
    }

    // Performs only main-thread ownership and frame changes; no CPU advance or backend upload.
    // Owners prepare attachments in their usual order, advance their handles in one registry
    // batch, then complete them in that same order. Do not prepare a lifecycle twice at once.
    [[nodiscard]] core::Result<PreparedAttachmentStep> prepare_step(const bool visible, const EmitterFrame& frame,
                                                                   const MeshFrame* mesh, const float delta_seconds) {
        PreparedAttachmentStep prepared;
        AttachmentStep& result = prepared.result;
        result.visible = visible;
        bool fresh = false;
        if (active_ && !visible) {
            const EffectHandle hidden = *active_;
            auto detached = registry_->detach(hidden);
            if (!detached) return core::Result<PreparedAttachmentStep>::failure(detached.error());
            active_.reset();
            ++detaches_;
            result.detached = detached.value();
            if (detached.value() == EffectDetachState::draining) {
                if (draining_.size() >= max_draining_) {
                    auto cut = registry_->release(draining_.front());
                    if (!cut) return core::Result<PreparedAttachmentStep>::failure(cut.error());
                    draining_.erase(draining_.begin());
                    ++drains_cut_short_;
                    ++result.drains_cut_short;
                }
                draining_.push_back(hidden);
            }
        } else if (!active_ && visible && (generations_ == 0 || policy_ != ReappearancePolicy::stay_detached)) {
            if (policy_ == ReappearancePolicy::reset) {
                for (const EffectHandle drain : draining_) {
                    auto dropped = registry_->release(drain);
                    if (!dropped) return core::Result<PreparedAttachmentStep>::failure(dropped.error());
                    ++drains_reset_;
                    ++result.drains_reset;
                }
                draining_.clear();
            }
            auto spawned = spawn_(*registry_, generation_seed(seed_, generations_));
            if (!spawned) return core::Result<PreparedAttachmentStep>::failure(spawned.error());
            active_ = spawned.value();
            ++generations_;
            result.spawned = spawned.value();
            fresh = true;
        }

        prepared.handles.assign(draining_.begin(), draining_.end());
        if (active_) prepared.handles.push_back(*active_);
        prepared.deltas.reserve(prepared.handles.size());
        for (const EffectHandle handle : prepared.handles) {
            auto applied = registry_->set_frame(handle, frame);
            if (!applied) return core::Result<PreparedAttachmentStep>::failure(applied.error());
            if (mesh) {
                auto meshed = registry_->set_mesh_frame(handle, *mesh);
                if (!meshed) return core::Result<PreparedAttachmentStep>::failure(meshed.error());
            }
            // A generation's first advance is zero-delta, like the owner's first frame.
            const float delta = fresh && active_ && handle == *active_ ? 0.0F : delta_seconds;
            prepared.deltas.push_back(delta);
        }
        return core::Result<PreparedAttachmentStep>::success(std::move(prepared));
    }

    // Consumes the statistics for this prepared sample, releases finished drains, and folds
    // statistics in handle order. Lifecycle ownership and all backend release calls stay here
    // on the calling thread, after the worker batch has joined.
    [[nodiscard]] core::Result<AttachmentStep> complete_step(PreparedAttachmentStep prepared,
                                                            const std::span<const EffectFrameStats> stats) {
        if (prepared.handles.size() != stats.size()) {
            return core::Result<AttachmentStep>::failure({std::string(diagnostic_codes::batch), core::Severity::error,
                "the attachment needs statistics for every prepared effect", {}, {}, {}, {}});
        }
        AttachmentStep result = std::move(prepared.result);
        for (std::size_t index = 0; index < stats.size(); ++index) {
            if (!stats[index].finished) continue;
            const EffectHandle handle = prepared.handles[index];
            const auto drain = std::find(draining_.begin(), draining_.end(), handle);
            if (drain == draining_.end()) {
                return core::Result<AttachmentStep>::failure({std::string(diagnostic_codes::batch), core::Severity::error,
                    "a finished attachment effect is not a live drain", {}, {}, {}, {}});
            }
            // A completed drain is released exactly once, by its owner.
            auto released = registry_->release(handle);
            if (!released) return core::Result<AttachmentStep>::failure(released.error());
            draining_.erase(drain);
            ++drains_released_;
            ++result.drains_released;
        }
        std::vector<std::size_t> order;
        order.reserve(stats.size());
        for (std::size_t index = 0; index < stats.size(); ++index) order.push_back(index);
        std::sort(order.begin(), order.end(), [&](const auto left, const auto right) {
            return prepared.handles[left] < prepared.handles[right];
        });
        for (std::size_t index = 0; index < order.size(); ++index) {
            merge_frame_stats(result.stats, stats[order[index]], index == 0);
        }
        result.active = active_.has_value();
        result.live_instances = draining_.size() + (active_ ? 1U : 0U);
        return core::Result<AttachmentStep>::success(std::move(result));
    }

    // Releases the active generation and every drain; returns how many.
    [[nodiscard]] core::Result<std::size_t> release_all() {
        std::size_t released = 0;
        std::vector<EffectHandle> live(draining_.begin(), draining_.end());
        if (active_) live.push_back(*active_);
        draining_.clear();
        active_.reset();
        for (const EffectHandle handle : live) {
            auto result = registry_->release(handle);
            if (!result) return core::Result<std::size_t>::failure(result.error());
            ++released;
        }
        return core::Result<std::size_t>::success(released);
    }

    [[nodiscard]] std::optional<EffectHandle> active() const noexcept { return active_; }
    [[nodiscard]] std::span<const EffectHandle> draining() const noexcept { return draining_; }
    [[nodiscard]] ReappearancePolicy policy() const noexcept { return policy_; }
    [[nodiscard]] std::uint32_t generations() const noexcept { return generations_; }
    [[nodiscard]] std::uint32_t detaches() const noexcept { return detaches_; }
    [[nodiscard]] std::uint32_t drains_released() const noexcept { return drains_released_; }
    [[nodiscard]] std::uint32_t drains_cut_short() const noexcept { return drains_cut_short_; }
    [[nodiscard]] std::uint32_t drains_reset() const noexcept { return drains_reset_; }

private:
    EffectRegistry* registry_;
    Spawn spawn_;
    std::uint32_t seed_{};
    ReappearancePolicy policy_{ReappearancePolicy::respawn};
    std::size_t max_draining_{default_max_draining};
    std::optional<EffectHandle> active_;
    std::vector<EffectHandle> draining_;
    std::uint32_t generations_{};
    std::uint32_t detaches_{};
    std::uint32_t drains_released_{};
    std::uint32_t drains_cut_short_{};
    std::uint32_t drains_reset_{};
};

} // namespace eawr::presentation::particles
