#include "eawr/sim/snapshot.hpp"

#include <utility>

namespace eawr::sim {

RenderSnapshot::RenderSnapshot(
    const std::uint64_t completed_tick,
    std::vector<RenderInstance> instances)
    : completed_tick_(completed_tick), instances_(std::move(instances)) {}

RenderSnapshot::RenderSnapshot(
    const std::uint64_t completed_tick,
    std::vector<RenderInstance> instances,
    fog::FogGridSet fog_grids)
    : completed_tick_(completed_tick),
      instances_(std::move(instances)),
      fog_grids_(std::move(fog_grids)) {}

std::uint64_t RenderSnapshot::completed_tick() const noexcept {
    return completed_tick_;
}

std::span<const RenderInstance> RenderSnapshot::instances() const noexcept {
    return instances_;
}

const fog::FogGridSet& RenderSnapshot::fog_grids() const noexcept {
    return fog_grids_;
}

} // namespace eawr::sim
