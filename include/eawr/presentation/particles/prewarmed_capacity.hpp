#pragma once

#include "eawr/presentation/particles/particles.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace eawr::presentation::particles {

// PL-01, PS-07, PS-08: bound the effective sampler, including the admitted
// first update's prewarm before freeze. Scheduling slack is project policy.
[[nodiscard]] inline double capacity_duration(const EmitterDefinition& emitter) {
    const auto age = postload_lifetime_range(emitter);
    if (!std::isfinite(age.minimum) || !std::isfinite(age.maximum)) return 0;
    double duration = std::max(age.minimum, age.maximum);
    if (emitter.freeze_time > 0) {
        const double prewarm = emitter.skip_time > 0
            ? std::min(emitter.skip_time, CpuSystem::max_skip_seconds) + CpuSystem::preroll_step : 0;
        duration = std::min(duration, static_cast<double>(emitter.freeze_time) + prewarm);
    }
    return duration;
}

[[nodiscard]] inline double capacity_step(const EmitterDefinition& emitter) {
    return emitter.skip_time > 0 ? static_cast<double>(CpuSystem::preroll_step) : 1.0 / 30.0;
}

// PS-18..PS-20: base clamp precedes both multiplicities. The caller budget
// and scheduler allowance are project policies, not native global ceilings.
[[nodiscard]] inline std::vector<EmitterCapacity> emitter_capacity_plan(
    const SystemDefinition& system, const std::size_t vertices, const std::size_t budget) {
    std::vector<EmitterCapacity> result(system.emitters.size());
    std::vector<std::uint8_t> done(system.emitters.size());
    const auto multiply = [](const std::size_t a, const std::size_t b) {
        return b != 0 && a > std::numeric_limits<std::size_t>::max() / b
            ? std::numeric_limits<std::size_t>::max() : a * b;
    };
    // Resolve parent-first, including forward links; malformed cycles reserve nothing.
    for (std::size_t pass = 0; pass < result.size(); ++pass) {
        bool progress = false;
        for (std::size_t index = 0; index < result.size(); ++index) {
            if (done[index]) continue;
            const auto& emitter = system.emitters[index];
            const auto parent = emitter.parent_emitter;
            if (parent != EmitterDefinition::no_parent && (parent >= result.size() || !done[parent])) continue;
            done[index] = 1; progress = true;
            if (!emitter.cpu_ready || !std::isfinite(emitter.lifetime) || emitter.lifetime < 0
                || !std::isfinite(emitter.lifetime_variation) || !std::isfinite(emitter.freeze_time)
                || !std::isfinite(emitter.skip_time)
                || (emitter.lifetime_range && (!std::isfinite(emitter.lifetime_range->minimum)
                    || !std::isfinite(emitter.lifetime_range->maximum)))
                || !std::isfinite(emitter.particles_per_interval) || emitter.particles_per_interval <= 0
                || !std::isfinite(emitter.spawn_interval) || emitter.spawn_interval <= 0
                || (emitter.creator_id != 34 && emitter.creator_id != 35
                    && emitter.creator_id != 39 && emitter.creator_id != 40)) continue;
            const double count = emitter.bursting ? std::trunc(static_cast<double>(emitter.particles_per_interval)) : 1.0;
            const double interval = emitter.bursting ? emitter.spawn_interval
                : static_cast<double>(emitter.spawn_interval) / emitter.particles_per_interval;
            const double age = emitter.lifetime;
            const double native = interval <= age ? std::ceil(age * count / interval) : count;
            const double duration = capacity_duration(emitter);
            // Keep the existing whole-batch/one-step allowance for the sampled
            // presentation scheduler, but an isolated burst reserves just its event.
            const double scheduled = interval > duration ? count : emitter.bursting
                ? std::ceil((duration + capacity_step(emitter)) / interval) * count
                : std::ceil((duration + capacity_step(emitter)) / interval) + 1.0;
            auto& capacity = result[index];
            capacity.native_requested = static_cast<std::size_t>(std::clamp(native, 0.0, 5003.0));
            capacity.requested = static_cast<std::size_t>(std::clamp(std::max(native, scheduled), 0.0, 5003.0));
            if (parent != EmitterDefinition::no_parent) {
                capacity.native_requested = multiply(capacity.native_requested, result[parent].native_requested);
                capacity.requested = multiply(capacity.requested, result[parent].requested);
            }
            if (emitter.creator_id == 35 && emitter.mesh_mode == MeshSpawnMode::every_vertex) {
                capacity.native_requested = multiply(capacity.native_requested, vertices);
                capacity.requested = multiply(capacity.requested, vertices);
            }
        }
        if (!progress) break;
    }
    const auto admitted = static_cast<std::size_t>(std::count_if(result.begin(), result.end(),
        [](const auto& capacity) { return capacity.requested != 0; }));
    if (admitted == 0) return result;
    // Fair initial shares prevent a dense first root from taking a later root's
    // allocation. Distribute remaining storage once; never borrow during emission.
    std::size_t remaining = budget;
    for (auto& capacity : result) {
        capacity.reserved = std::min(capacity.requested, budget / admitted);
        remaining -= capacity.reserved;
    }
    for (auto& capacity : result) {
        const auto extra = std::min(remaining, capacity.requested - capacity.reserved);
        capacity.reserved += extra; remaining -= extra;
    }
    return result;
}

// PL-02, PS-18: automatic production sizing for independent legacy emitters.
// Clamp each emitter before summing; parent and every-vertex families
// retain one caller fallback reserve. Explicit host budgets remain caller policy.
[[nodiscard]] inline std::size_t continuous_capacity(
    const SystemDefinition& system, const std::size_t fallback,
    const std::size_t limit = 65536) {
    if (system.version != AloParticleVersion::legacy_v1 || system.emitters.empty())
        return std::min(fallback, limit);
    constexpr double emitter_limit = 5003;
    double total = 0;
    bool excluded = false;
    for (const auto& emitter : system.emitters) {
        const bool independent = emitter.parent_emitter == EmitterDefinition::no_parent
            && (emitter.creator_id == 34 || (emitter.creator_id == 35
                && (emitter.mesh_mode == MeshSpawnMode::random_vertex
                    || emitter.mesh_mode == MeshSpawnMode::random_surface)));
        if (!independent || !emitter.cpu_ready
            || !std::isfinite(emitter.lifetime) || emitter.lifetime <= 0
            || !std::isfinite(emitter.lifetime_variation)
            || !std::isfinite(emitter.freeze_time)
            || (emitter.lifetime_range && (!std::isfinite(emitter.lifetime_range->minimum)
                || !std::isfinite(emitter.lifetime_range->maximum)))
            || !std::isfinite(emitter.particles_per_interval) || emitter.particles_per_interval <= 0
            || !std::isfinite(emitter.spawn_interval) || emitter.spawn_interval <= 0) {
            excluded = true;
            continue;
        }
        const double duration = capacity_duration(emitter);
        if (duration <= 0) {
            excluded = true;
            continue;
        }
        double capacity;
        if (emitter.bursting) {
            // PS-18/PS-19: reserve whole overlapping bursts, including one
            // ordinary or prewarm scheduling step. The whole-batch allowance
            // is project policy; native allocation rounds age x births/second.
            const double count = std::trunc(static_cast<double>(emitter.particles_per_interval));
            capacity = std::ceil((duration + capacity_step(emitter)) / emitter.spawn_interval) * count;
        } else {
            const double rate = static_cast<double>(emitter.particles_per_interval) / emitter.spawn_interval;
            // One birth plus one ordinary or prewarm scheduling step, within the ceiling.
            capacity = std::ceil((duration + capacity_step(emitter)) * rate) + 1.0;
        }
        total += std::min(emitter_limit, capacity);
        total = std::min(total, static_cast<double>(limit));
    }
    if (excluded) total += static_cast<double>(fallback);
    return static_cast<std::size_t>(std::min(static_cast<double>(limit),
        std::max(static_cast<double>(fallback), total)));
}

// Presentation allocation for prewarmed, independent continuous shape emitters.
// It does not alter their authored schedule or RNG. Other creator families keep
// the caller's fallback because child/mesh emission needs a different bound.
[[nodiscard]] inline std::size_t prewarmed_capacity(
    const SystemDefinition& system, const std::size_t fallback,
    const std::size_t limit = 65536) {
    if (system.version != AloParticleVersion::legacy_v1 || system.emitters.empty())
        return std::min(fallback, limit);
    bool prewarmed = false;
    double total = 0;
    for (const auto& emitter : system.emitters) {
        if (emitter.creator_id != 34 || emitter.bursting
            || !std::isfinite(emitter.lifetime) || emitter.lifetime <= 0
            || !std::isfinite(emitter.lifetime_variation)
            || (emitter.lifetime_range && (!std::isfinite(emitter.lifetime_range->minimum)
                || !std::isfinite(emitter.lifetime_range->maximum)))
            || !std::isfinite(emitter.particles_per_interval) || emitter.particles_per_interval <= 0
            || !std::isfinite(emitter.spawn_interval) || emitter.spawn_interval <= 0)
            return std::min(fallback, limit);
        prewarmed |= emitter.skip_time > 0;
        const double duration = capacity_duration(emitter);
        if (duration <= 0) return std::min(fallback, limit);
        const double rate = static_cast<double>(emitter.particles_per_interval) / emitter.spawn_interval;
        // Include one birth and one actual scheduling step at the death boundary.
        total += std::ceil((duration + capacity_step(emitter)) * rate) + 1.0;
    }
    if (!prewarmed) return std::min(fallback, limit);
    return static_cast<std::size_t>(std::min(static_cast<double>(limit),
        std::max(static_cast<double>(fallback), total)));
}

// Independent, continuous mesh emitters have one birth per scheduling event.
// The debug build bounds rate x maximum age at 5003 before
// parent/every-vertex multiplication. Those multiplicities are not inferred here.
// Keep one ordinary or prewarm step of slack at the CPU death/birth boundary.
[[nodiscard]] inline std::size_t continuous_mesh_capacity(
    const SystemDefinition& system, const std::size_t fallback,
    const std::size_t limit = 5003) {
    if (system.version != AloParticleVersion::legacy_v1 || system.emitters.empty())
        return std::min(fallback, limit);
    double total = 0;
    for (const auto& emitter : system.emitters) {
        if (emitter.creator_id != 35 || emitter.bursting || !emitter.cpu_ready
            || emitter.parent_emitter != EmitterDefinition::no_parent
            || (emitter.mesh_mode != MeshSpawnMode::random_vertex
                && emitter.mesh_mode != MeshSpawnMode::random_surface)
            || !std::isfinite(emitter.lifetime) || emitter.lifetime <= 0
            || !std::isfinite(emitter.lifetime_variation)
            || (emitter.lifetime_range && (!std::isfinite(emitter.lifetime_range->minimum)
                || !std::isfinite(emitter.lifetime_range->maximum)))
            || !std::isfinite(emitter.particles_per_interval) || emitter.particles_per_interval <= 0
            || !std::isfinite(emitter.spawn_interval) || emitter.spawn_interval <= 0)
            return std::min(fallback, limit);
        const double duration = capacity_duration(emitter);
        if (duration <= 0) return std::min(fallback, limit);
        const double rate = static_cast<double>(emitter.particles_per_interval) / emitter.spawn_interval;
        total += std::ceil((duration + capacity_step(emitter)) * rate) + 1.0;
    }
    return static_cast<std::size_t>(std::min(static_cast<double>(limit),
        std::max(static_cast<double>(fallback), total)));
}

} // namespace eawr::presentation::particles
