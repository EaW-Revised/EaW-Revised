#include "galactic_support.hpp"

#include "eawr/sim/world.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Published snapshot sections are shared between consecutive publications while their section is unchanged. A
// shared section must read exactly like a fresh build, also after a change in an unrelated section: every tick's
// sections are compared with the snapshot of a second session loaded from the first one's checkpoint (nothing
// cached there), and the session itself recomputes every reused copy while hash verification is on.
namespace {

using namespace galactic_test;
using galactic::GalacticSession;
using Fixed = eawr::sim::math::Fixed;

galactic::GalacticSetup economy_galaxy() {
    galactic::SyntheticGalaxyOptions options;
    options.planets = 12;
    options.players = 3;
    options.fleets_per_player = 2;
    auto setup = galactic::synthetic_galaxy(options);
    galactic::PlayerEconomySetup economy;
    for (const auto& player : setup.players)
        economy.players.push_back({player.player_id, Fixed::from_raw(100 * Fixed::scale), Fixed::from_raw(1000 * Fixed::scale), 0, 0});
    setup.player_economy = std::move(economy);
    return setup;
}

template <class T> bool same_rows(const std::span<const T> left, const std::span<const T> right) {
    return std::equal(left.begin(), left.end(), right.begin(), right.end());
}

// The published sections of `shared` equal those of a snapshot built from scratch.
void expect_fresh(const galactic::GalacticSnapshot& shared, const galactic::GalacticSnapshot& fresh, const std::string& when) {
    expect(same_rows(shared.players(), fresh.players()), when + ": players equal a fresh build");
    expect(same_rows(shared.planets(), fresh.planets()), when + ": planets equal a fresh build");
    expect(same_rows(shared.routes(), fresh.routes()), when + ": routes equal a fresh build");
    expect(same_rows(shared.fleets(), fresh.fleets()), when + ": fleets equal a fresh build");
    expect(shared.player_economy() == fresh.player_economy(), when + ": the economy equals a fresh build");
    expect(shared.completed_tick() == fresh.completed_tick(), when + ": same tick");
}

struct Run {
    galactic::GalacticSetup setup = economy_galaxy();
    GalacticSession live = GalacticSession::create(setup).value();
    eawr::sim::InlineExecutor executor;
    std::shared_ptr<const galactic::GalacticSnapshot> previous;

    Run() {
        live.set_hash_verification(true);
        previous = live.snapshot();
    }
    // Steps once and returns the new publication after comparing it with a snapshot built from scratch.
    std::shared_ptr<const galactic::GalacticSnapshot> step(const std::string& when) {
        auto stepped = live.step(executor);
        expect(static_cast<bool>(stepped), when + ": the step succeeds");
        if (!stepped) return previous;
        auto current = stepped.value().snapshot;
        auto saved = live.save_checkpoint_state();
        expect(static_cast<bool>(saved), when + ": the checkpoint saves");
        auto fresh = GalacticSession::create(setup);
        expect(static_cast<bool>(fresh), when + ": a reference session creates");
        if (saved && fresh) {
            auto reference = std::move(fresh).value();
            expect(static_cast<bool>(reference.load_checkpoint_state(saved.value())), when + ": the checkpoint loads");
            expect_fresh(*current, *reference.snapshot(), when);
        }
        return current;
    }
};

bool shares(const std::span<const galactic::Planet> a, const std::span<const galactic::Planet> b) { return a.data() == b.data(); }
bool shares(const std::span<const galactic::Route> a, const std::span<const galactic::Route> b) { return a.data() == b.data(); }
bool shares(const std::span<const galactic::Fleet> a, const std::span<const galactic::Fleet> b) { return a.data() == b.data(); }
bool shares(const std::optional<galactic::PlayerEconomySetup>& a, const std::optional<galactic::PlayerEconomySetup>& b) { return &a == &b; }

void sections_follow_their_own_changes() {
    Run run;
    const auto& setup = run.setup;
    // Tick 1 moves one fleet (the fleets section only); tick 3 flips one planet's owner (the planets section only).
    const auto& fleet = setup.fleets.front();
    const auto home = fleet.orbit;
    const auto elsewhere = std::find_if(setup.planets.begin(), setup.planets.end(), [&](const auto& planet) { return planet.id != home; })->id;
    expect(static_cast<bool>(run.live.submit(orbit(1, seat_of(fleet.owner), 1, fleet.id, elsewhere))), "the fleet order is admitted");
    const auto flipped = std::find_if(setup.planets.begin(), setup.planets.end(), [&](const auto& planet) { return planet.owner != 1; });
    expect(flipped != setup.planets.end(), "a planet of another player exists");
    if (flipped == setup.planets.end()) return;
    expect(static_cast<bool>(run.live.submit(owner(3, seat_of(1), 1, flipped->id, 1))), "the owner order is admitted");

    auto before = run.step("tick 0");
    auto after = run.step("tick 1 (fleet moved)");
    expect(!same_rows(before->fleets(), after->fleets()), "the fleet order changed the fleets section");
    expect(shares(before->planets(), after->planets()), "planets are shared across a fleet-only change");
    expect(shares(before->routes(), after->routes()), "routes are shared across a fleet-only change");
    expect(shares(before->player_economy(), after->player_economy()), "the economy is shared across a fleet-only change");
    before = after;

    after = run.step("tick 2 (quiet)");
    expect(shares(before->fleets(), after->fleets()) && shares(before->planets(), after->planets())
            && shares(before->routes(), after->routes()),
        "a quiet tick shares every physical section");
    before = after;

    after = run.step("tick 3 (planet owner changed)");
    expect(!same_rows(before->planets(), after->planets()), "the owner order changed the planets section");
    expect(shares(before->fleets(), after->fleets()), "fleets are shared across a planet-only change");
    expect(shares(before->routes(), after->routes()), "routes are shared across a planet-only change");
    before = after;

    after = run.step("tick 4 (quiet)");
    expect(shares(before->fleets(), after->fleets()) && shares(before->planets(), after->planets()),
        "a quiet tick shares every physical section again");
}

void scripted_mix_equals_fresh_builds() {
    Run run;
    for (const auto& command : script(run.setup)) expect(static_cast<bool>(run.live.submit(command)), "scripted command admitted");
    auto before = run.live.snapshot();
    std::size_t shared_routes = 0, shared_planets = 0, shared_fleets = 0, steps = 0;
    for (std::uint64_t tick = 0; tick < 45; ++tick) {
        const auto after = run.step("scripted tick " + std::to_string(tick));
        ++steps;
        if (tick > 0) {
            shared_routes += shares(before->routes(), after->routes());
            shared_planets += shares(before->planets(), after->planets());
            shared_fleets += shares(before->fleets(), after->fleets());
        }
        before = after;
    }
    expect(shared_routes == steps - 1, "routes never change: every publication after the first shares them");
    expect(shared_planets > 0 && shared_fleets > 0, "quiet ticks share planets and fleets");
}

} // namespace

int case_galactic_snapshot_sections_tests() {
    sections_follow_their_own_changes();
    scripted_mix_equals_fresh_builds();
    if (failures != 0) {
        std::cerr << failures << " galactic snapshot section check(s) failed\n";
        return 1;
    }
    std::cout << "galactic snapshot sections passed\n";
    return 0;
}
