#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <cstdint>

// A synthetic galaxy for tests, tools and the viewer's galactic mode before campaign data loads:
// written for the project, no game data. Deterministic in its options.
namespace eawr::sim::galactic {

struct SyntheticGalaxyOptions {
    std::uint64_t seed{1};
    std::uint32_t planets{12};          // at least 2
    std::uint32_t players{2};           // at least 1; planets are dealt round robin
    std::uint32_t fleets_per_player{2};
    std::uint32_t members_per_fleet{3}; // at least 1
    bool debug_commands{true};
    bool coop_seat{};                   // a second human seat in player 1 (ADR-020)
    friend bool operator==(const SyntheticGalaxyOptions&, const SyntheticGalaxyOptions&) = default;
};

// Planets on a square grid 1000 units apart (Q24 positions, z = 0) with IDs 1..P; routes link
// each planet to the next (a ring) and every fourth planet to the one three ahead; fleets orbit
// their owner's planets in turn; members follow, typed 1000 + member index. Players are 1..N,
// faction = player ID, seated by seat N. Players 1 and 2 are human versus sides, others AI;
// with `coop_seat`, only player 1 is human and seat N+1 is its second human seat, against AI.
// The galaxy passes validate_setup.
[[nodiscard]] GalacticSetup synthetic_galaxy(const SyntheticGalaxyOptions& options = {});

} // namespace eawr::sim::galactic
