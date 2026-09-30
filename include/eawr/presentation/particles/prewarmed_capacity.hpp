#pragma once

#include "eawr/presentation/particles/particles.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace eawr::presentation::particles {

// Presentation allocation for prewarmed, independent continuous shape emitters.
// It does not alter their authored schedule or RNG. Other creator families keep
// the caller's fallback because child/mesh emission needs a different bound.
[[nodiscard]] inline std::size_t prewarmed_capacity(
    const SystemDefinition& system, const std::size_t fallback,
    const std::size_t limit = 65536) {
    bool prewarmed = false;
    double total = 0;
    for (const auto& emitter : system.emitters) {
        if (emitter.creator_id != 34 || emitter.bursting
            || !std::isfinite(emitter.lifetime) || emitter.lifetime <= 0
            || !std::isfinite(emitter.lifetime_variation)
            || !std::isfinite(emitter.particles_per_interval) || emitter.particles_per_interval <= 0
            || !std::isfinite(emitter.spawn_interval) || emitter.spawn_interval <= 0)
            return std::min(fallback, limit);
        prewarmed |= emitter.skip_time > 0;
        double duration = static_cast<double>(emitter.lifetime)
            * (1.0 + std::max(0.0, static_cast<double>(emitter.lifetime_variation)));
        if (emitter.freeze_time > 0) duration = std::min(duration, static_cast<double>(emitter.freeze_time));
        const double rate = static_cast<double>(emitter.particles_per_interval) / emitter.spawn_interval;
        // Include the time-zero birth and one presentation step of scheduling
        // slack at the death/freeze boundary (the pre-roll runs at 30 Hz).
        total += std::ceil((duration + 1.0 / 30.0) * rate) + 1.0;
    }
    if (!prewarmed) return std::min(fallback, limit);
    return static_cast<std::size_t>(std::min(static_cast<double>(limit),
        std::max(static_cast<double>(fallback), total)));
}

} // namespace eawr::presentation::particles
