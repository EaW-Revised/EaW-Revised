// WGM-150 / WGEP-112/113: a fleet leaving a piracy-corrupted planet accrues the summed original-type
// piracy values for the first corruption beneficiary; the fiscal service pays and resets them.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../support/env.hpp"
#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/corruption_model.hpp"

using eawr::testing::take;

namespace {
namespace gc = eawr::sim::galactic;
namespace cf = gc::corruption_fixture;
using eawr::sim::math::Fixed;
using galactic_test::expect;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }

constexpr gc::PlanetId home = 1, neighbour = 4;

struct Departure { std::size_t fleet; gc::PlayerId seat; std::uint64_t tick; };

struct Options {
    Fixed first{f(50)}, second{f(250)};
    bool first_stealth{}, second_stealth{};
    cf::Type corruption{cf::Type::piracy};
    bool beneficiary{true};
    bool ineligible_first{}; // an earlier benefiting player that earns no planet income
    std::uint32_t fiscal_frames{100000};
};

// Two players, four planets, two fleets per player of two members (types 1000 and 1001). Fleets 0 and 1
// (player 1) and fleet 2 (player 2, the corruption beneficiary) all sit at the piracy planet.
gc::GalacticSetup setup(const Options& options) {
    gc::SyntheticGalaxyOptions synthetic; synthetic.players = 2; synthetic.planets = 4;
    synthetic.fleets_per_player = 2; synthetic.members_per_fleet = 2; synthetic.debug_commands = true;
    auto s = gc::synthetic_galaxy(synthetic);
    s.rules.fiscal_cycle_frames = options.fiscal_frames; s.rules.maximum_fleet_movement_distance = f(10000);
    s.fleets[0].orbit = s.fleets[1].orbit = s.fleets[2].orbit = home;
    s.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}, {1001, false, true, false}}, {}};
    s.player_economy = gc::PlayerEconomySetup{{{1, f(1000), f(10000), 2, 5}, {2, f(1000), f(10000), 2, 5}}};
    s.fiscal = gc::FiscalSetup{f(10000), {{1, true, f(1)}, {2, true, f(1)}}, {}};
    for (const auto& planet : s.planets) s.fiscal->planets.push_back({planet.id, planet.id == home ? f(100) : Fixed{}});
    auto& state = s.economy_corruption.emplace();
    state.factor_min = f(1); state.factor_max = f(3);
    state.income_factors = {Fixed{}, f(1), f(1), f(1), f(1), f(1), f(1), f(1), f(1)};
    gc::CorruptionEconomyPlayer owner; owner.player = 1; owner.faction = 10; owner.playable = owner.human = true;
    auto beneficiary = owner; beneficiary.player = 2; beneficiary.faction = 30; beneficiary.benefits = options.beneficiary;
    if (options.ineligible_first) {
        owner.benefits = true; s.fiscal->players[0].earns_planet_income = false;
    }
    state.players = {owner, beneficiary};
    for (const auto& planet : s.planets) state.planets.push_back({planet.id, {}, false, false, cf::one, {}, {}, {}});
    s.planets[0].owner = 1;
    state.planets[0].corruption = options.corruption;
    auto& movement = s.movement.emplace(); movement.mode_modifier = f(1);
    gc::MovementType a; a.type = 1000; a.hyperspace_speed = f(2); a.build_cost = f(100); a.hyperspace_eligible = true;
    a.piracy_value = options.first; a.skips_combat = options.first_stealth;
    auto b = a; b.type = 1001; b.piracy_value = options.second; b.skips_combat = options.second_stealth;
    movement.types = {a, b};
    for (const auto& player : s.players) {
        movement.players.push_back({player.player_id, f(1), f(1), false});
        for (const auto& planet : s.planets) movement.access.push_back({player.player_id, planet.id, true, false});
    }
    for (const auto& planet : s.planets) movement.planets.push_back({planet.id, {}, false});
    return s;
}

struct Outcome { Fixed piracy; std::vector<Fixed> credits; std::vector<std::string> hashes; };

Outcome run(const Options& options, const std::vector<Departure>& departures, std::uint64_t ticks, unsigned workers = 1) {
    eawr::platform::ThreadWorkerAdapter pool(workers);
    auto config = setup(options);
    auto game = take(gc::GalacticSession::create(config));
    Outcome out;
    std::uint64_t sequence = 0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        for (const auto& move : departures) if (move.tick == tick)
            take(game.submit({{tick, {move.seat}, ++sequence}, gc::MoveFleet{config.fleets[move.fleet].id, {home, neighbour}, {}}}));
        out.hashes.push_back(take(game.step(pool)).state_hash.get());
    }
    out.piracy = game.economy_corruption()->planets[0].piracy;
    for (const auto& row : game.snapshot()->player_economy()->players) out.credits.push_back(row.credits);
    return out;
}

void gates() {
    const auto departing = [](const Options& options, std::size_t fleet = 0, gc::PlayerId seat = 1) {
        return run(options, {{fleet, seat, 0}}, 2).piracy;
    };
    expect(departing({}) == f(300), "mixed fleet adds the sum of its members' original-type values");
    expect(departing({f(50), f(0)}) == f(50), "a zero-value member adds nothing to the sum");
    expect(departing({f(0), f(0)}) == Fixed{}, "a zero-value fleet accrues nothing");
    Options stealth; stealth.first_stealth = stealth.second_stealth = true;
    expect(departing(stealth) == Fixed{}, "a stealth fleet accrues nothing");
    Options partial; partial.first_stealth = true;
    expect(departing(partial) == f(300), "stealth needs every member to skip combat");
    expect(departing({}, 2, 2) == Fixed{}, "a fleet of the beneficiary is allied to it and accrues nothing");
    Options militia; militia.corruption = cf::Type::militia;
    expect(departing(militia) == Fixed{}, "only piracy corruption accrues");
    Options ineligible; ineligible.ineligible_first = true;
    expect(departing(ineligible) == f(300), "an ineligible benefiting player listed first is skipped (WGEP-112)");
    Options none; none.beneficiary = false;
    expect(departing(none) == Fixed{}, "no corruption-benefiting player, no accrual");
}

void repeated_and_payout() {
    const auto both = run({}, {{0, 1, 0}, {1, 1, 1}}, 3);
    expect(both.piracy == f(600), "repeated departures from the same planet accumulate");
    // The departure debits nobody: the fleet owner's credits match a run without any departure.
    const auto idle = run({}, {}, 3);
    expect(idle.piracy == Fixed{} && both.credits == idle.credits, "the accrual changes no account");
    Options paying; paying.fiscal_frames = 3;
    const auto paid = run(paying, {{0, 1, 0}}, 12);
    const auto baseline = run(paying, {}, 12);
    expect(paid.piracy == Fixed{}, "the fiscal service resets the accrued balance");
    expect(paid.credits[1] > baseline.credits[1] && paid.credits[0] == baseline.credits[0],
        "the first beneficiary receives the balance and the fleet owner does not");
}

void workers_equal() {
    const std::vector<Departure> moves{{0, 1, 0}, {1, 1, 1}};
    const auto reference = run({}, moves, 6, 1).hashes;
    for (const auto workers : {2U, 4U, 8U})
        expect(run({}, moves, 6, workers).hashes == reference, "departure accrual is equal on 1/2/4/8 workers");
}
} // namespace

int main() {
    try {
        gates();
        repeated_and_payout();
        workers_equal();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
