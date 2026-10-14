#pragma once

#include "eawr/sim/galactic/types.hpp"

namespace eawr::sim::galactic {
// WGA-233/236: retained data for explicit native invasion admission and return.
struct NativeInvasion {
    enum class State : std::uint8_t { unqueued, pending, conflict, completed } state{State::pending};
    std::uint64_t request{};
    PlayerId invader{}, defender{};
    FleetId fleet{};
    PlanetId planet{};
    std::vector<FleetMember> transports; // direct fleet contents, in original order
    EntityId conflict{};
    bool success{}, reader_released{};
    bool operator==(const NativeInvasion&) const = default;
};
} // namespace eawr::sim::galactic
