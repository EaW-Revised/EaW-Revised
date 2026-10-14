#pragma once
#include "eawr/sim/galactic/types.hpp"

namespace eawr::sim::galactic {
struct FleetDeparture {
    math::Fixed base_speed{};
    std::vector<EntityId> packed, left_behind;
    friend bool operator==(const FleetDeparture&, const FleetDeparture&) = default;
};
// WGM-24..27/87/92. Pure admission/packing; caller commits identity transfers only
// after path/endpoints have also validated. Unverified packing cut-off ties refuse.
[[nodiscard]] core::Result<FleetDeparture> prepare_fleet_departure(const Fleet& fleet, const MovementSetup& inputs);
[[nodiscard]] core::Result<void> validate_movement_setup(const GalacticSetup& setup);
} // namespace eawr::sim::galactic
