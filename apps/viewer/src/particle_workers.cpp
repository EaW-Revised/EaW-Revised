#include "particle_workers.hpp"

#include "eawr/presentation/particles/worker_count.hpp"

#include <algorithm>

namespace eawr::presentation::godot_backend {

std::size_t ParticleWorkers::default_count(const std::size_t hardware_threads) noexcept {
    return particles::default_particle_worker_count(hardware_threads);
}

ParticleWorkers::ParticleWorkers(const std::size_t count) : pool_(std::max<std::size_t>(count, 1)) {}

bool ParticleWorkers::run(const std::size_t count, const std::function<void(std::size_t)>& task) const {
    return static_cast<bool>(pool_.execute(count, task));
}

} // namespace eawr::presentation::godot_backend
