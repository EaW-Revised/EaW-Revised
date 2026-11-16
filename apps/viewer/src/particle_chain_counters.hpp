#pragma once

#include <atomic>
#include <cstdint>

namespace eawr::presentation::godot_backend {

// How many particle texture chains were built and how long they took, process-wide (diagnostics only; MT-03 in
// docs/behaviour/texture-mip-upload.md). `*_at_first_frame` are the totals when the first battle frame was drawn, so the
// difference is the chains built while the battle was being played, which the load-time resolution of every effect
// texture keeps at zero.
struct ParticleChainCounters final {
    std::atomic<std::uint64_t> builds{0};
    std::atomic<std::uint64_t> microseconds{0};
    std::atomic<std::uint64_t> builds_at_first_frame{0};
    std::atomic<std::uint64_t> microseconds_at_first_frame{0};
};

[[nodiscard]] inline ParticleChainCounters& particle_chain_counters() {
    static ParticleChainCounters counters;
    return counters;
}

inline void particle_chain_mark_first_frame() {
    auto& counters = particle_chain_counters();
    counters.builds_at_first_frame.store(counters.builds.load(std::memory_order_relaxed), std::memory_order_relaxed);
    counters.microseconds_at_first_frame.store(counters.microseconds.load(std::memory_order_relaxed),
                                               std::memory_order_relaxed);
}

} // namespace eawr::presentation::godot_backend
