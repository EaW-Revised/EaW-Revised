#pragma once

#include <algorithm>
#include <cstddef>

namespace eawr::presentation::particles {

// The default size of the viewer's particle pool, the main thread included. Hosts below eight
// hardware threads keep a quarter of them (so the main thread alone under eight); hosts with
// eight or more take half of them, capped at 4 (docs/performance/battle-bench.md, "The viewer's
// particles").
[[nodiscard]] constexpr std::size_t default_particle_worker_count(const std::size_t hardware_threads) noexcept {
    return std::clamp<std::size_t>(hardware_threads >= 8 ? hardware_threads / 2 : hardware_threads / 4, 1, 4);
}

} // namespace eawr::presentation::particles
