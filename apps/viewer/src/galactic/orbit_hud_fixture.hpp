#pragma once
#include "ground_hud_fixture.hpp"
#include <charconv>

namespace eawr::viewer::galactic {
// Explicit faction models and cursor colours; never used for a live campaign.
inline void configure_orbit_scene_fixture(GalaxySnapshotStub& snapshot, std::string_view fixture) {
    if (fixture.starts_with("orbit-placement-")) {
        auto& planet = snapshot.planets.front();
        const bool yavin = fixture.substr(16).starts_with("yavin");
        planet.model = yavin ? "data/art/models/w_planet_yaviniv_low.alo"
            : "data/art/models/w_planet_kamino_low.alo";
        planet.scale = sim::math::Fixed::from_raw((yavin ? 13 : 18) * sim::math::Fixed::scale / 10);
        return;
    }
    if (!fixture.starts_with("orbit-look-")) return;
    auto& planet = snapshot.planets.front();
    const auto name = fixture.substr(11);
    planet.model = "data/art/models/w_planet_urban01.alo";
    if (name.starts_with("underworld")) planet.model = "data/art/models/w_planet_nalhutta01.alo";
    // Change only the cursor's owner tint between paired captures. The authored
    // station material must retain the same textured pixels in either capture.
    snapshot.players.front().colour = fixture.ends_with("-blue")
        ? std::array<std::uint8_t, 4>{0, 0, 255, 255}
        : std::array<std::uint8_t, 4>{255, 0, 0, 255};
}

// Explicit GPU fixture inputs; station names and levels here are test data only.
inline void configure_orbit_fixture(sim::galactic::GalacticSetup& setup, PlayerId local,
    std::string_view fixture, std::vector<sim::galactic::FleetDisplayType>& traits,
    std::vector<sim::galactic::FleetDisplayVisibility>& visibility,
    std::map<sim::galactic::TypeId, std::string>& names) {
    if (!fixture.starts_with("orbit-")) return;
    configure_ground_fixture(setup, local, "ground-structures", traits, visibility, names);
    namespace g = sim::galactic;
    std::int64_t level = fixture.ends_with("-empty") ? 0 : 1;
    constexpr std::string_view prefix = "orbit-level-";
    if (fixture.starts_with(prefix)) {
        const auto value = fixture.substr(prefix.size());
        std::from_chars(value.data(), value.data() + value.size(), level);
    }
    names[1006] = "Rebel_Star_Base_" + std::to_string(level);
    if (fixture == "orbit-neutral") names[1006] = "Pirate_Asteroid_Base";
    if (fixture.starts_with("orbit-look-empire")) names[1006] = "Empire_Star_Base_1";
    if (fixture.starts_with("orbit-look-underworld")) names[1006] = "Underworld_Star_Base_1";
    // The control owner publishes the retained base level into production at startup.
    // Seed both providers consistently, as authored campaign starting forces do.
    for (auto& planet : setup.control->planets) if (planet.planet == 101)
        planet.starbase = {fixture == "orbit-hidden" ? (local == 1 ? 2U : 1U) : local,
            static_cast<std::uint32_t>(level)};
    setup.census->types.push_back({1006, {}, 0});
    auto mask = g::bit(g::Intelligence::ownership) | g::bit(g::Intelligence::fleet_presence)
        | g::bit(g::Intelligence::fleet_count);
    if (fixture != "orbit-hidden") mask |= g::bit(g::Intelligence::space_level) | g::bit(g::Intelligence::structures);
    for (auto& player : setup.observer->players) player.default_intelligence = mask;
    for (auto& record : setup.observer->records) record.mask = mask;
    for (auto& planet : setup.observer->planets) if (planet.planet == 101) {
        // A foreign base prevents owner intelligence from revealing the hidden fixture.
        planet.space_owner = fixture == "orbit-hidden" ? (local == 1 ? 2U : 1U) : local;
        planet.space_level = level;
    }
    for (auto& planet : setup.production->planets) if (planet.planet == 101) {
        planet.starbase_type = level ? 1006U : 0U; planet.space_level = level;
        planet.maximum_space_level = 5; planet.space_slots = fixture == "orbit-extra" ? 3 : 2;
        planet.objects.clear();
        if (fixture == "orbit-filled") {
            planet.objects.push_back({9201, 1004, local, {}, g::ProductionDomain::space, g::ProductionKind::structure});
            planet.objects.push_back({9202, 1005, local, {}, g::ProductionDomain::space, g::ProductionKind::structure});
        }
    }
    if (fixture == "orbit-hidden") {
        const auto enemy = local == 1 ? 2U : 1U;
        for (auto& row : setup.observer->players) row.minimum_visible_base_level = 100;
        setup.fleets.clear();
        for (auto& row : setup.observer->planets) if (row.planet == 101) {
            row.allegiance = row.target_owner = row.ground_owner = row.current_owner = row.current_controller = enemy;
            row.landed_transports.clear();
        }
        for (auto& row : setup.control->planets) if (row.planet == 101) row.landed.clear();
    }
}
} // namespace eawr::viewer::galactic
