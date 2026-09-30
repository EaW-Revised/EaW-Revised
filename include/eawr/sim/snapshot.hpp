#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/sim/fog.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace eawr::sim {

struct RenderInstance {
    EntityId entity_id{};
    AssetId asset_id{};
    math::Mat3x4 fixed_transform{};
    friend constexpr bool operator==(const RenderInstance&, const RenderInstance&) noexcept = default;
};

class RenderSnapshot final {
public:
    RenderSnapshot() = default;
    RenderSnapshot(std::uint64_t completed_tick, std::vector<RenderInstance> instances);
    // Additive fog-stub-v1 attachment: validated immutable grids, sorted by team.
    // An empty set is identical to the two-argument form (no fog attachment).
    RenderSnapshot(
        std::uint64_t completed_tick,
        std::vector<RenderInstance> instances,
        fog::FogGridSet fog_grids);

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] std::span<const RenderInstance> instances() const noexcept;
    [[nodiscard]] const fog::FogGridSet& fog_grids() const noexcept;

private:
    std::uint64_t completed_tick_{};
    std::vector<RenderInstance> instances_;
    fog::FogGridSet fog_grids_;
};

} // namespace eawr::sim
