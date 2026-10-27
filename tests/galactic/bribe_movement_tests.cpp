// WGM-84 / WGM-85: a corruption-benefiting mover pays, before departure, each intermediate path planet's
// owner the summed Cheap/Expensive_Bribe_Cost of the leaving members; the choice follows that owner's
// Easily_Bribed, the mover's own orbit is free, and a mover who cannot pay is refused unchanged.
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "../support/env.hpp"
#include "../support/result.hpp"
#include "../../src/sim/galactic/native_movement_admission.hpp"
#include "galactic_support.hpp"
#include "eawr/platform/sim_workers.hpp"

using eawr::testing::take;

namespace {
namespace gc = eawr::sim::galactic;
using eawr::sim::math::Fixed;
using galactic_test::expect;

Fixed f(std::int64_t n) { return take(Fixed::from_integer(n)); }

constexpr gc::PlanetId home = 1;

struct Options {
    bool easily_bribed{true};   // Easily_Bribed of the owner of planet 2 (player 2)
    bool mover_benefits{true};  // Faction benefits from corruption (the mover, player 1)
    Fixed mover_credits{f(1000)};
    Fixed cheap_a{f(10)}, cheap_b{f(20)}, expensive_a{f(40)}, expensive_b{f(60)};
};

// Two players, a four-planet ring (1-2-3-4), planets 1 and 3 owned by player 1, 2 and 4 by player 2. Fleet 0 (player 1,
// members of types 1000 and 1001) sits at planet 1.
gc::GalacticSetup setup(const Options& options) {
    gc::SyntheticGalaxyOptions synthetic; synthetic.players = 2; synthetic.planets = 4;
    synthetic.fleets_per_player = 2; synthetic.members_per_fleet = 2; synthetic.debug_commands = true;
    auto s = gc::synthetic_galaxy(synthetic);
    s.rules.maximum_fleet_movement_distance = f(10000);
    for (auto& fleet : s.fleets) fleet.orbit = home;
    s.fleet_control = gc::FleetControlSetup{{}, {{1000, false, true, false}, {1001, false, true, false}}, {}};
    s.player_economy = gc::PlayerEconomySetup{{{1, options.mover_credits, f(100000), 2, 5}, {2, f(1000), f(100000), 2, 5}}};
    auto& movement = s.movement.emplace(); movement.mode_modifier = f(1);
    gc::MovementType a; a.type = 1000; a.hyperspace_speed = f(2); a.build_cost = f(100); a.hyperspace_eligible = true;
    a.cheap_bribe = options.cheap_a; a.expensive_bribe = options.expensive_a;
    auto b = a; b.type = 1001; b.cheap_bribe = options.cheap_b; b.expensive_bribe = options.expensive_b;
    movement.types = {a, b};
    movement.players = {{1, f(1), f(1), options.mover_benefits, false}, {2, f(1), f(1), false, options.easily_bribed}};
    for (const auto& player : s.players)
        for (const auto& planet : s.planets) movement.access.push_back({player.player_id, planet.id, true, false});
    for (const auto& planet : s.planets) movement.planets.push_back({planet.id, {}, false});
    return s;
}

struct Outcome {
    std::vector<Fixed> credits;
    bool departed{};
    std::vector<std::string> hashes;
    gc::GalacticRejectReason reason{gc::GalacticRejectReason::none};
};

Outcome run(const Options& options, const std::vector<gc::PlanetId>& path, unsigned workers = 1, std::uint64_t ticks = 3) {
    eawr::platform::ThreadWorkerAdapter pool(workers);
    auto config = setup(options);
    auto game = take(gc::GalacticSession::create(config));
    take(game.submit({{0, {1}, 1}, gc::MoveFleet{config.fleets[0].id, path, {}}}));
    Outcome out;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        out.hashes.push_back(take(game.step(pool)).state_hash.get());
        for (const auto& event : game.snapshot()->events())
            if (event.kind == gc::GalacticEventKind::command_rejected) out.reason = event.reason;
    }
    for (const auto& row : game.snapshot()->player_economy()->players) out.credits.push_back(row.credits);
    for (const auto& fleet : game.snapshot()->fleets()) if (fleet.id == config.fleets[0].id) out.departed = fleet.orbit == 0;
    return out;
}

void cheap_and_expensive() {
    const auto cheap = run({}, {1, 2, 3});
    expect(cheap.departed && cheap.credits[0] == f(970) && cheap.credits[1] == f(1030),
        "an Easily_Bribed owner is paid the summed Cheap_Bribe_Cost of the leaving members");
    Options hard; hard.easily_bribed = false;
    const auto expensive = run(hard, {1, 2, 3});
    expect(expensive.departed && expensive.credits[0] == f(900) && expensive.credits[1] == f(1100),
        "any other owner is paid the summed Expensive_Bribe_Cost");
    Options ordinary_mover; ordinary_mover.mover_benefits = false;
    const auto ordinary = run(ordinary_mover, {1, 2, 3});
    expect(ordinary.departed && ordinary.credits[0] == f(1000) && ordinary.credits[1] == f(1000),
        "only a corruption-benefiting mover pays");
    const auto direct = run({}, {1, 2});
    expect(direct.departed && direct.credits[0] == f(1000), "the final hop is not an intermediate planet and costs nothing");
    Options zero; zero.cheap_a = zero.cheap_b = Fixed{};
    const auto omitted = run(zero, {1, 2, 3});
    expect(omitted.departed && omitted.credits[0] == f(1000), "an omitted (zero) cost admits the move for free");
}

void insufficient_credits() {
    Options poor; poor.mover_credits = f(29);
    const auto refused = run(poor, {1, 2, 3});
    expect(!refused.departed && refused.reason == gc::GalacticRejectReason::insufficient_credits
        && refused.credits[0] == f(29) && refused.credits[1] == f(1000), "a mover short of the total is refused with nothing charged");
    Options exact; exact.mover_credits = f(30);
    const auto paid = run(exact, {1, 2, 3});
    expect(paid.departed && paid.credits[0] == Fixed{} && paid.credits[1] == f(1030), "exactly enough credits pays and departs");
    // Planet 3 belongs to the mover: the whole total (30 + 100) must be affordable even though its own share returns.
    Options partial; partial.mover_credits = f(50);
    const auto total_gate = run(partial, {1, 2, 3, 4});
    expect(!total_gate.departed && total_gate.reason == gc::GalacticRejectReason::insufficient_credits,
        "affordability tests the whole path total, not the net after the mover's own share");
    const auto settled = run({}, {1, 2, 3, 4});
    expect(settled.departed && settled.credits[0] == f(970) && settled.credits[1] == f(1030),
        "each intermediate planet owner is credited its own share; the mover's own share nets out");
}

void quote_rules() {
    namespace sd = gc::session_detail;
    gc::MovementSetup m;
    gc::MovementType a; a.type = 1; a.cheap_bribe = f(5); a.expensive_bribe = f(9);
    m.types = {a};
    m.players = {{1, f(1), f(1), true, false}, {2, f(1), f(1), false, true}, {3, f(1), f(1), false, false}};
    const std::vector<gc::FleetMember> members{{10, 1, false}, {11, 1, false}};
    const std::vector<gc::PlanetId> path{1, 2, 3, 4};
    const auto state = [](gc::PlanetId planet) -> std::pair<gc::PlayerId, gc::PlayerId> {
        switch (planet) {
        case 2: return {2, 2}; // owner 2 (easy), occupied by its own owner
        case 3: return {3, 1}; // owner 3 (hard), but the mover is the orbital occupant
        default: return {};
        }
    };
    const auto quote = sd::quote_native_bribe(1, members, path, m, state);
    expect(!quote.invalid && quote.total == f(10) && quote.recipients.size() == 1 && quote.recipients[0].first == 2,
        "an orbit held by the mover is free; the other planet pays the per-member cheap cost");
    const auto elsewhere = sd::quote_native_bribe(1, members, path, m, [](gc::PlanetId planet) -> std::pair<gc::PlayerId, gc::PlayerId> {
        return planet == 3 ? std::pair<gc::PlayerId, gc::PlayerId>{3, 0} : std::pair<gc::PlayerId, gc::PlayerId>{};
    });
    expect(elsewhere.total == f(18), "a hard owner's planet with no mover occupant costs the expensive cost per member");
    const auto unowned = sd::quote_native_bribe(1, members, path, m, [](gc::PlanetId) { return std::pair<gc::PlayerId, gc::PlayerId>{}; });
    expect(unowned.total == Fixed{} && unowned.recipients.empty(), "an unowned planet has nobody to pay");
    const auto pair_only = sd::quote_native_bribe(1, members, std::vector<gc::PlanetId>{1, 4}, m, state);
    expect(pair_only.total == Fixed{}, "a two-planet path has no intermediate planet");
}

void workers_equal() {
    const auto reference = run({}, {1, 2, 3, 4}, 1, 6);
    for (const auto workers : {2U, 4U, 8U}) {
        const auto other = run({}, {1, 2, 3, 4}, workers, 6);
        expect(other.hashes == reference.hashes && other.credits == reference.credits, "paid movement is equal on 1/2/4/8 workers");
    }
}
} // namespace

int main() {
    try {
        cheap_and_expensive();
        insufficient_credits();
        quote_rules();
        workers_equal();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return galactic_test::checker.finish();
}
