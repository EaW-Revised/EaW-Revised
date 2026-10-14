#include "eawr/sim/galactic/synthetic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace eawr::sim::galactic {

namespace {

constexpr std::int64_t grid_spacing = 1000;

[[nodiscard]] math::Fixed whole(const std::int64_t value) noexcept {
    return math::Fixed::from_raw(value * math::Fixed::scale);
}

} // namespace

GalacticSetup synthetic_galaxy(const SyntheticGalaxyOptions& options) {
    const auto planets = std::max<std::uint32_t>(options.planets, 2);
    const auto players = std::max<std::uint32_t>(options.players, 1);
    const auto members = std::max<std::uint32_t>(options.members_per_fleet, 1);

    GalacticSetup setup;
    setup.seed = options.seed;
    setup.debug_commands = options.debug_commands;
    for (std::size_t index = 0; index < setup.content_identity.size(); ++index) {
        setup.content_identity[index] = static_cast<std::uint8_t>(0xa0U + index);
        setup.campaign_identity[index] = static_cast<std::uint8_t>(0x50U + index);
    }
    for (std::uint32_t player = 1; player <= players; ++player) {
        setup.players.push_back({player, player, 0});
        // Versus has two human sides; coop seats player 1 against AI-controlled opponents.
        const auto kind = player == 1 || (player == 2 && !options.coop_seat) ? SeatKind::human : SeatKind::ai;
        setup.seats.push_back({SeatId{player}, player, kind, player - 1, true, kind == SeatKind::human ? player : 0U});
    }
    if (options.coop_seat) setup.seats.push_back({SeatId{players + 1}, 1, SeatKind::human, players, true, 1});

    std::uint32_t side = 1;
    while (side * side < planets) ++side;
    EntityId next = 1;
    for (std::uint32_t index = 0; index < planets; ++index) {
        const math::Vec3 position{whole(std::int64_t{index % side} * grid_spacing),
            whole(std::int64_t{index / side} * grid_spacing), whole(0)};
        setup.planets.push_back({next++, position, index % players + 1});
    }
    const auto planet_id = [](const std::uint32_t index) { return EntityId{index} + 1; };
    const auto ring = planets == 2 ? 1U : planets;
    for (std::uint32_t index = 0; index < ring; ++index) {
        setup.routes.push_back({next++, planet_id(index), planet_id((index + 1) % planets)});
        if (planets > 4 && index % 4 == 0) setup.routes.push_back({next++, planet_id(index), planet_id((index + 3) % planets)});
    }
    std::vector<std::uint32_t> orbits;
    for (std::uint32_t player = 0; player < players; ++player) {
        for (std::uint32_t fleet = 0; fleet < options.fleets_per_player; ++fleet) {
            orbits.push_back((fleet * players + player) % planets);
            setup.fleets.push_back({next++, player + 1, 0, {}});
        }
    }
    for (std::size_t fleet = 0; fleet < setup.fleets.size(); ++fleet) {
        setup.fleets[fleet].orbit = planet_id(orbits[fleet]);
        for (std::uint32_t member = 0; member < members; ++member) {
            setup.fleets[fleet].members.push_back({next++, TypeId{1000} + member});
        }
    }
    return setup;
}

} // namespace eawr::sim::galactic
