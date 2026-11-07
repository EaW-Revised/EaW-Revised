#pragma once

#include <algorithm>
#include <cstddef>

namespace eawr::presentation::particles {

// The cap on the default particle pool below twelve hardware threads (the main thread or the
// particle lane included): the 8-LP rig loses ticks to more helpers on full cores.
inline constexpr std::size_t particle_worker_cap_small = 4;

// The cap from twelve hardware threads up. Measured with the particle lane on (RaW60, 2026-11-03,
// docs/performance/particle-pool-size.md): pool 6 cuts lane time per stream 12 % on a 28-LP host
// and pool 8 is not shown better there (3 % faster, within the run-to-run spread).
inline constexpr std::size_t particle_worker_cap_large = 6;
inline constexpr std::size_t particle_worker_large_host_threads = 12;

// The cap from twenty hardware threads up. Measured on the owner's 20-LP PC (2026-11-04, Remake Korriban
// 8 v 8, patched engine, lane on, Separate render thread, docs/performance/particle-pool-size.md): viewer
// main 17.8 ms at pool 4, 15.6 ms at pool 8; pool 12 is not better than 8 (15.4 ms, one run).
inline constexpr std::size_t particle_worker_cap_huge = 8;
inline constexpr std::size_t particle_worker_huge_host_threads = 20;

// The default size of the viewer's particle pool, the main thread included. Hosts below eight
// hardware threads keep a quarter of them (so the main thread alone under eight); hosts with
// eight or more take half of them, capped at particle_worker_cap_small, at
// particle_worker_cap_large from twelve up and at particle_worker_cap_huge from twenty up
// (docs/performance/battle-bench.md, "The viewer's particles"). --eawr-live-particle-workers
// overrides it.
[[nodiscard]] constexpr std::size_t default_particle_worker_count(const std::size_t hardware_threads) noexcept {
    const std::size_t cap = hardware_threads >= particle_worker_huge_host_threads    ? particle_worker_cap_huge
                            : hardware_threads >= particle_worker_large_host_threads ? particle_worker_cap_large
                                                                                     : particle_worker_cap_small;
    return std::clamp<std::size_t>(hardware_threads >= 8 ? hardware_threads / 2 : hardware_threads / 4, 1, cap);
}

} // namespace eawr::presentation::particles
