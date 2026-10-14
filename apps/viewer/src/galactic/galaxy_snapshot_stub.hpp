#pragma once

// Presentation metadata and the original synthetic visual fixture. Live mode
// combines names/models/colours here with immutable GalacticSnapshot values
// through build_live_view; dynamic ownership, fleets and routes come from the sim.

#include "eawr/sim/math/fixed.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {

using PlayerId = std::uint32_t;
using SeatId = std::uint32_t;
using ObjectId = std::uint64_t;

struct StubPlayer final {
    PlayerId id{};
    // The faction's XML name; its colour stands in for the faction data the
    // galactic loaders will supply (faction `Color`, WGP-05).
    std::string faction;
    std::array<std::uint8_t, 4> colour{255, 255, 255, 255};
};

// A controller seated in exactly one player (ADR-020). Coop is two human
// seats in one player.
struct StubSeat final {
    SeatId id{};
    PlayerId player{};
    bool human{};
    bool active{true};
};

// GHUD-S95: shared provider for the selected bonus and future galaxy filter.
// Authored keys remain distinct from the sourced encyclopedia title/body.
struct PlanetAbilityMetadata final {
    std::string name_key{}, description_key{}, icon{};
    std::string authored_name{}, authored_description{};
    std::string title{}, description{};
};

struct StubPlanet final {
    ObjectId id{};
    std::string name;
    // Logical model path (the planet type's `Galactic_Model_Name`).
    std::string model;
    std::array<sim::math::Fixed, 3> position{};
    sim::math::Fixed scale = sim::math::Fixed::from_raw(sim::math::Fixed::scale);
    std::optional<PlayerId> owner;
    PlanetAbilityMetadata ability{};
};

struct StubRoute final {
    ObjectId id{};
    ObjectId from{};
    ObjectId to{};
};

// A fleet in orbit (`orbit` set) or in transit along one route hop (`orbit`
// empty, `from` -> `to` with `progress` in [0, 1] as Q24).
struct StubFleet final {
    ObjectId id{};
    PlayerId owner{};
    std::optional<ObjectId> orbit;
    ObjectId from{};
    ObjectId to{};
    sim::math::Fixed progress{};
    std::uint32_t members{};
    // Published live motion overrides synthetic hop interpolation.
    std::optional<std::array<sim::math::Fixed, 3>> position{};
    std::optional<std::array<sim::math::Fixed, 3>> facing{};
};

struct GalaxySnapshotStub final {
    std::uint64_t completed_tick{};
    std::vector<StubPlayer> players;
    std::vector<StubSeat> seats;
    std::vector<StubPlanet> planets;
    std::vector<StubRoute> routes;
    std::vector<StubFleet> fleets;
};

// The synthetic galaxy: no game data, a fixed layout. Three players: two coop
// parties of two human seats each against each other, and one AI seat. Planet models are
// stock galactic models so a run with game data draws real planets.
inline constexpr std::string_view synthetic_campaign = "synthetic";
[[nodiscard]] GalaxySnapshotStub synthetic_galaxy();

} // namespace eawr::viewer::galactic
