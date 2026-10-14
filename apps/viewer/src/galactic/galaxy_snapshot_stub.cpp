#include "galaxy_snapshot_stub.hpp"

#include <utility>

namespace eawr::viewer::galactic {
namespace {

using sim::math::Fixed;

[[nodiscard]] Fixed whole(const std::int64_t value) noexcept { return Fixed::from_raw(value * Fixed::scale); }

// Hundredths, for scales and progress.
[[nodiscard]] Fixed hundredths(const std::int64_t value) noexcept {
    return Fixed::from_raw(value * Fixed::scale / 100);
}

} // namespace

GalaxySnapshotStub synthetic_galaxy() {
    GalaxySnapshotStub galaxy;
    galaxy.completed_tick = 0;
    // Faction colours are the stock `Color` values of these factions (fixture
    // data until the galactic loaders supply faction data).
    galaxy.players = {
        {1, "Rebel", {185, 40, 39, 255}},
        {2, "Empire", {54, 134, 242, 255}},
        {3, "Underworld", {247, 201, 13, 255}},
    };
    // Coop parties against each other: seats 1 and 2 both command player 1,
    // seats 3 and 5 player 2, and an AI seat commands player 3. Seat IDs are
    // not player IDs.
    galaxy.seats = {
        {1, 1, true, true},
        {2, 1, true, true},
        {3, 2, true, true},
        {4, 3, false, true},
        {5, 2, true, true},
    };
    // Every position lies on the scroll plane, Z 10 (WGP-04, WGUI-57).
    const auto planet = [](const ObjectId id, std::string name, std::string model, const std::int64_t x,
                           const std::int64_t y, const std::int64_t scale, const std::optional<PlayerId> owner) {
        StubPlanet result;
        result.id = id;
        result.name = std::move(name);
        result.model = std::move(model);
        result.position = {whole(x), whole(y), whole(10)};
        result.scale = hundredths(scale);
        result.owner = owner;
        return result;
    };
    galaxy.planets = {
        planet(101, "Synthetic Alpha", "Data/Art/Models/W_planet_Temperate01.alo", -220, 120, 180, 1),
        planet(102, "Synthetic Beta", "Data/Art/Models/W_planet_Ice01.alo", -120, 200, 160, 1),
        planet(103, "Synthetic Gamma", "Data/Art/Models/W_planet_Ocean01.alo", -60, 60, 180, std::nullopt),
        planet(104, "Synthetic Delta", "Data/Art/Models/W_planet_Gas01.alo", 40, 170, 230, 3),
        planet(105, "Synthetic Epsilon", "Data/Art/Models/W_planet_Desert01.alo", 30, -40, 180, std::nullopt),
        planet(106, "Synthetic Zeta", "Data/Art/Models/W_planet_Swamp01.alo", -150, -120, 160, 1),
        planet(107, "Synthetic Eta", "Data/Art/Models/W_planet_Volcanic01.alo", 150, 60, 180, 2),
        planet(108, "Synthetic Theta", "Data/Art/Models/W_planet_bonadan_low.alo", 200, -110, 180, 2),
        planet(109, "Synthetic Iota", "Data/Art/Models/W_planet_Asteroids.alo", 90, -190, 180, 3),
    };
    galaxy.routes = {
        {201, 101, 102}, {202, 101, 103}, {203, 102, 104}, {204, 103, 104}, {205, 103, 105},
        {206, 101, 106}, {207, 106, 105}, {208, 104, 107}, {209, 105, 107}, {210, 107, 108},
        {211, 105, 109}, {212, 108, 109},
    };
    const auto orbiting = [](const ObjectId id, const PlayerId owner, const ObjectId at, const std::uint32_t members) {
        StubFleet fleet;
        fleet.id = id;
        fleet.owner = owner;
        fleet.orbit = at;
        fleet.from = at;
        fleet.to = at;
        fleet.members = members;
        return fleet;
    };
    const auto moving = [](const ObjectId id, const PlayerId owner, const ObjectId from, const ObjectId to,
                           const std::int64_t progress, const std::uint32_t members) {
        StubFleet fleet;
        fleet.id = id;
        fleet.owner = owner;
        fleet.from = from;
        fleet.to = to;
        fleet.progress = hundredths(progress);
        fleet.members = members;
        return fleet;
    };
    galaxy.fleets = {
        orbiting(301, 1, 101, 4),
        orbiting(302, 1, 101, 2),
        orbiting(303, 2, 107, 5),
        orbiting(304, 3, 104, 3),
        orbiting(305, 2, 105, 2),
        moving(306, 1, 103, 105, 40, 3),
        moving(307, 2, 108, 109, 65, 6),
    };
    return galaxy;
}

} // namespace eawr::viewer::galactic
