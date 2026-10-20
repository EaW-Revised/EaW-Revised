// MAT-15 (docs/behaviour/main-menu-attract.md): in an attract session a destroyed object that is
// not a garrison unit and not a team member returns its type to its owner's reinforcement pool;
// a squadron container returns once, when its last craft is gone. Without the attract record the
// same battle returns nothing and hashes as before.

#include "fighter_support.hpp"
#include "eawr/sim/tactical/economy.hpp"
#include "eawr/sim/tactical/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fighter_test_support {

namespace {

struct Outcome final {
    std::vector<tactical::TypeId> empire_pool;
    std::vector<tactical::TypeId> rebel_pool;
    std::vector<std::uint64_t> empire_tokens;
    std::vector<std::string> hashes;
    std::uint64_t launched{};
    bool garrison_marked{};
    bool ok{};
};

[[nodiscard]] tactical::TacticalSetup attract_setup(const bool attract) {
    tactical::TacticalSetup setup;
    setup.seed = seed;
    setup.players = players();
    // 10 (squadron_b, craft 11) starts with the setup; the carrier launches squadron_a (container 14,
    // craft 12 and 13: the ids follow the highest setup id) on its own, a garrison unit by birth (WU-37).
    // 2 is a lone frigate and 3 a rebel corvette.
    setup.units = {unit(1, carrier_type, empire, at(0, 0)), unit(2, frigate_type, empire, at(-300, 400)),
        unit(3, corvette_type, rebel, at(300, -400)), unit(10, squadron_b, empire, at(-500, 0)),
        unit(11, craft_type, empire, at(-500, 0))};
    setup.squadrons = {{10, {11}}};
    if (attract) setup.attract_pool = tactical::AttractPoolReturn{{craft_type, squadron_a, squadron_b, frigate_type}};
    return setup;
}

[[nodiscard]] tactical::EconomyRules attract_economy() {
    tactical::EconomyRules rules;
    rules.players = {{empire, Fixed{}, 25, true, Fixed{}}, {rebel, Fixed{}, 25, true, Fixed{}}};
    return rules;
}

[[nodiscard]] Outcome attract_run(const bool attract, const std::size_t workers, const bool scramble) {
    Outcome out;
    auto table = motion();
    table.squadrons.spawners[0].entries = {{squadron_a, 1, 0}};
    auto created = tactical::TacticalSession::create(attract_setup(attract), sensors(), durability(), table,
        std::nullopt, tactical::CombatTable{}, tactical::VictoryRules{}, tactical::AbilityTable{}, attract_economy(),
        tactical::MapBox{units(-2400), units(-2400)});
    expect(static_cast<bool>(created), "MAT-15: the attract session builds");
    if (!created) return out;
    auto world = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto advance = [&]() {
        if (scramble) world.scramble_storage_for_testing();
        auto step = world.step(executor);
        expect(static_cast<bool>(step), "MAT-15: the attract session steps");
        if (!step) return false;
        out.hashes.push_back(step.value().state_sha256);
        return true;
    };
    while (world.completed_tick() < 60 && out.launched == 0) {
        if (!advance()) return out;
        if (instance(*world.snapshot(), 14) != nullptr) out.launched = world.completed_tick();
    }
    expect(out.launched != 0, "MAT-15: the hangar launches its squadron");
    if (out.launched == 0) return out;
    out.garrison_marked = instance(*world.snapshot(), 14)->garrison_unit;
    // One tick later every kill lands in the same frame: the garrison squadron's craft, the starting
    // team's craft, the frigate and the rebel corvette (a type the setup does not list).
    const auto when = world.completed_tick();
    const auto kill = [&](const tactical::PlayerId issuer, const std::uint64_t sequence,
                          std::vector<eawr::sim::EntityId> targets) {
        return tactical::PlayerCommand{{when, issuer, sequence}, std::move(targets), tactical::DamagePayload{units(10000)}};
    };
    expect(static_cast<bool>(world.submit(kill(empire, 0, {2, 11, 12, 13}))), "MAT-15: the empire's kills submit");
    expect(static_cast<bool>(world.submit(kill(rebel, 0, {3}))), "MAT-15: the rebel's kill submits");
    for (int tick = 0; tick < 30; ++tick) if (!advance()) return out;
    for (const auto& account : world.ledgers()) {
        if (account.player == empire) {
            out.empire_pool = account.pool;
            out.empire_tokens = account.pool_tokens;
        }
        if (account.player == rebel) out.rebel_pool = account.pool;
    }
    out.ok = true;
    return out;
}

} // namespace

void test_attract_pool_return() {
    const auto on = attract_run(true, 1, false);
    expect(on.ok, "MAT-15: the attract battle runs");
    if (!on.ok) return;
    expect(on.garrison_marked, "WU-37: the launched squadron is a garrison unit");
    // The frigate (2) and the starting team (container 10, once its last craft 11 is gone) return; the
    // garrison squadron (14) and every craft return nothing; the corvette's type is not listed.
    auto pool = on.empire_pool;
    std::sort(pool.begin(), pool.end());
    expect(pool == std::vector<tactical::TypeId>{squadron_b, frigate_type},
        "MAT-15: a lone ship and an emptied team return; a garrison team and its craft do not");
    expect(on.rebel_pool.empty(), "MAT-15: a type the setup does not list returns nothing");
    expect(on.empire_tokens.size() == 2 && on.empire_tokens[0] != on.empire_tokens[1],
        "SAE-11: each returned entry gets its own pool identity");

    const auto off = attract_run(false, 1, false);
    expect(off.ok && off.empire_pool.empty() && off.rebel_pool.empty(),
        "WPR-23: outside the attract nothing is returned");
    // Nothing differs until the first death.
    const auto before = static_cast<std::size_t>(on.launched);
    expect(on.hashes.size() > before && off.hashes.size() > before
            && std::vector(on.hashes.begin(), on.hashes.begin() + static_cast<std::ptrdiff_t>(before))
                == std::vector(off.hashes.begin(), off.hashes.begin() + static_cast<std::ptrdiff_t>(before)),
        "MAT-15: the attract record changes no hash before the first death");

    for (const std::size_t workers : {2U, 4U, 8U}) {
        const auto again = attract_run(true, workers, workers != 2);
        expect(again.ok && again.hashes == on.hashes && again.empire_pool == on.empire_pool
                && again.empire_tokens == on.empire_tokens,
            "MAT-15: the pool return hashes alike on " + std::to_string(workers) + " workers");
    }

    // The record is part of the setup: a replay writes and reads it, and rejects a malformed set.
    tactical::TacticalReplay replay;
    replay.setup = attract_setup(true);
    replay.final_tick_count = 10;
    const auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "MAT-15: an attract setup writes");
    if (written) {
        const auto parsed = tactical::parse_replay(written.value());
        expect(parsed && parsed.value() == replay && parsed.value().setup.attract_pool == replay.setup.attract_pool,
            "MAT-15: the attract record round-trips");
    }
    auto plain = replay;
    plain.setup.attract_pool.reset();
    const auto plain_bytes = tactical::write_replay(plain);
    expect(plain_bytes && (!written || plain_bytes.value().size() + 16U + 8U * 4U == written.value().size()),
        "MAT-15: without the record the replay bytes are the legacy encoding");
    auto unsorted = replay;
    unsorted.setup.attract_pool->types = {squadron_a, craft_type};
    expect(!tactical::validate_setup(unsorted.setup), "MAT-15: an unsorted type set is rejected");
    unsorted.setup.attract_pool->types = {craft_type, craft_type};
    expect(!tactical::validate_setup(unsorted.setup), "MAT-15: a repeated type is rejected");
}

} // namespace fighter_test_support
