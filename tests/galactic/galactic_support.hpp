#pragma once

#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/galactic/synthetic.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Shared helpers of the galactic tests: expectations, command builders and the scripted command
// mix over a synthetic galaxy.
namespace galactic_test {

namespace galactic = eawr::sim::galactic;

inline int failures = 0;

inline void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

[[nodiscard]] inline bool has_code(const eawr::core::Result<void>& result, const std::string_view code) {
    return !result && result.error().code == code;
}

[[nodiscard]] inline galactic::GameCommand owner(const std::uint64_t tick, const galactic::SeatId seat,
    const std::uint64_t sequence, const galactic::PlanetId planet, const galactic::PlayerId player) {
    return {{tick, seat, sequence}, galactic::DebugSetPlanetOwner{planet, player}};
}

[[nodiscard]] inline galactic::GameCommand orbit(const std::uint64_t tick, const galactic::SeatId seat,
    const std::uint64_t sequence, const galactic::FleetId fleet, const galactic::PlanetId planet) {
    return {{tick, seat, sequence}, galactic::DebugSetFleetOrbit{fleet, planet}};
}

// The seat of a synthetic galaxy's player: seat N sits in player N.
[[nodiscard]] inline galactic::SeatId seat_of(const galactic::PlayerId player) { return {player}; }

// A scripted command mix over a synthetic galaxy: ownership flips, orbit moves by the owning seat,
// unauthorized and unknown orders rejected at execution, no-op orders, and quiet ticks. Commands
// are in canonical order.
[[nodiscard]] inline std::vector<galactic::GameCommand> script(const galactic::GalacticSetup& setup, const std::uint64_t ticks = 40) {
    std::vector<galactic::GameCommand> commands;
    const auto planets = setup.planets.size();
    const auto players = static_cast<std::uint32_t>(setup.players.size());
    std::vector<std::uint64_t> sequence(setup.seats.size() + 2, 0);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (tick % 5 == 3) continue; // quiet ticks reuse every section but the session's
        std::vector<galactic::GameCommand> batch;
        const auto player = static_cast<std::uint32_t>(tick % players) + 1;
        const auto seat = seat_of(player);
        batch.push_back(owner(tick, seat, ++sequence[seat.value], setup.planets[(tick * 7) % planets].id, player));
        if (tick % 3 == 0) {
            const auto& fleet = setup.fleets[tick % setup.fleets.size()];
            const auto by = seat_of(fleet.owner);
            batch.push_back(orbit(tick, by, ++sequence[by.value], fleet.id, setup.planets[(tick * 11) % planets].id));
        }
        if (tick % 13 == 4) batch.push_back(orbit(tick, seat, ++sequence[seat.value], setup.planets[0].id, 1));
        if (tick % 7 == 2) { // another player's seat orders this fleet: not authorized
            const auto& fleet = setup.fleets[0];
            const auto by = seat_of(fleet.owner % players + 1);
            batch.push_back(orbit(tick, by, ++sequence[by.value], fleet.id, setup.planets[1].id));
        }
        std::vector<std::size_t> order(batch.size());
        for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
        std::sort(order.begin(), order.end(),
            [&batch](const std::size_t left, const std::size_t right) { return batch[left].key < batch[right].key; });
        for (const auto index : order) commands.push_back(batch[index]);
    }
    return commands;
}

class PhaseProbe final : public eawr::sim::PartitionExecutor {
public:
    explicit PhaseProbe(const eawr::sim::PartitionExecutor& inner) : inner_(inner) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return inner_.worker_count(); }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t partitions, const std::function<void(std::size_t)>& work) const override {
        return inner_.execute(partitions, work);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t partitions,
        const std::function<void(std::size_t)>& work) const override {
        phases.emplace_back(std::string(phase), partitions);
        return inner_.execute_phase(phase, partitions, work);
    }
    mutable std::vector<std::pair<std::string, std::size_t>> phases;

private:
    const eawr::sim::PartitionExecutor& inner_;
};

} // namespace galactic_test
