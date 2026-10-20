#include "station_upgrade_fixtures.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <optional>
#include <vector>
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/combat_modifiers.hpp"
#include "station_allocations.hpp"
#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace station_upgrade_test_support {
class ProductionExecutor final : public eawr::sim::PartitionExecutor {
public:
    mutable std::size_t command_phases{};
    std::size_t worker_count() const noexcept override { return 1; }
    eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t slot = 0; slot < count; ++slot) partition(slot);
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        if (phase.starts_with("command-")) ++command_phases;
        allocation_probe::enabled = phase == "production-counts" || phase == "upgrade-profiles";
        const auto result = execute(count, partition);
        allocation_probe::enabled = false;
        return result;
    }
};
void test_staged_source_removal() {
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool scramble : {false, true}) {
        auto world = create(); if (!world) return;
        expect(static_cast<bool>(world->stage_remove(2)), "remove alternative holder before research");
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "staged source buy submits");
        through(*world, 3);
        if (scramble) world->scramble_storage_for_testing();
        expect(static_cast<bool>(world->stage_remove(1)), "staged source removes");
        const auto born = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
        expect(static_cast<bool>(born), "birth after staged source removal succeeds");
        if (!born) return;
        const auto before = world->durability_state(born.value());
        expect(before && before->hull == whole(100) && before->shields == whole(100),
            "WPR-55: staged source removal updates bonuses before the next birth");
        expect(static_cast<bool>(world->submit({{3, 1, 1}, {born.value()}, t::DamagePayload{whole(20)}})),
            "damage after staged source removal submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto tick = world->step(executor);
        expect(static_cast<bool>(tick), "staged removal damage tick succeeds");
        expect(world->durability_state(born.value())->shields == whole(80),
            "WPR-55: removed source cannot absorb the next birth's damage");
        expect(world->durability_state(3)->hull == whole(100), "WPR-51: existing units lose removed bonuses");
    }
    auto rules = economy();
    for (auto& menu : rules.menus) for (auto& item : menu.options) item.requirements.current_allies.reset();
    auto world = create(rules); if (!world) return;
    expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "weak retained source submits");
    expect(static_cast<bool>(world->submit(buy(0, 2, 0, 2, 92))), "strong removable source submits");
    through(*world, 3);
    expect(static_cast<bool>(world->stage_remove(2)), "strong source removes");
    for (unsigned birth = 0; birth < 8; ++birth) {
        const auto born = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
        expect(born && world->durability_state(born.value())->hull == whole(125),
            "WPR-51: staged removal falls back to the surviving category winner");
    }
    ProductionExecutor executor;
    through(*world, 4);
    expect(static_cast<bool>(world->stage_remove(4)), "non-source removes");
    const auto born = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
    expect(born && world->durability_state(born.value())->hull == whole(125), "non-source removal retains bonuses");
    allocation_probe::count = 0;
    const auto tick = world->step(executor);
    expect(tick && tick.value().bonus_profile_evaluations == 0 && allocation_probe::count == 0,
        "WPR-55: unchanged sources keep cached births and allocation-free census");
    expect(static_cast<bool>(world->stage_remove(1)), "last source removes");
    const auto final_birth = world->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
    expect(final_birth && world->durability_state(final_birth.value())->hull == whole(100), "last source leaves no phantom bonus");
}
void test_upgrade_holder_loss() {
    for (const bool staged : {false, true}) {
        auto world = create(); if (!world) return;
        expect(static_cast<bool>(world->stage_remove(2)), "no alternative combat holder");
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "last holder upgrade submits");
        through(*world, 3);
        if (staged) {
            expect(static_cast<bool>(world->stage_remove(1)), "last holder staged removal");
        } else {
            expect(static_cast<bool>(world->submit({{3, 1, 1}, {1}, t::DamagePayload{whole(100000)}})), "last holder combat loss");
            through(*world, 4);
        }
        expect(account(*world, 1).completed.empty() && account(*world, 1).lifetime.at(90) == 1,
            "WPR-57: no home destroys held source but preserves historical build count");
        expect(world->durability_state(3)->hull == whole(100) && world->durability_state(3)->shields == whole(100),
            "WPR-51/57: no home clamps existing durability maxima");
    }
}
void test_respawn_inherits_upgrade() {
    auto rules = economy();
    rules.pads.respawn = {{10, 2}};
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto world = create(rules); if (!world) return;
        expect(static_cast<bool>(world->submit(buy(0, 1, 0, 1, 90))), "respawn bonus purchase submits");
        expect(static_cast<bool>(world->submit({{3, 1, 1}, {3},
            t::DamagePayload{whole(1000), t::attack_hull}})), "respawn bonus death submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t tick = 0; tick < 6; ++tick) {
            const auto stepped = world->step(executor);
            expect(static_cast<bool>(stepped), "respawn with retained upgrade steps");
            if (!stepped) return;
            const auto hash = world->state_sha256();
            if (workers == 1) reference.push_back(hash);
            else expect(hash == reference[tick], "respawn upgrade hashes match at 1/2/4/8 workers");
            if (tick == 5) {
                const auto units = world->units();
                const auto born = std::find_if(units.begin(), units.end(), [](const auto& unit) {
                    return unit.entity_id > 5 && unit.type_id == 10 && unit.owner == 1;
                });
                expect(born != units.end(), "WHZ-52 ordinary replacement retains owner at its due frame");
                if (born == units.end()) return;
                const auto health = world->durability_state(born->entity_id);
                expect(health && health->hull == whole(125) && health->shields == whole(125)
                    && health->hardpoints[0] == whole(125), "WPR-51 respawn receives held bonuses on its birth frame");
                expect(stepped.value().bonus_profile_evaluations == 0,
                    "WPR-55 respawn reads cached bonuses without rebuilding source profiles");
            }
        }
    }
}
// The per-tick production recount (count_owned) against a plain oracle through live ownership changes.
void test_census_recount() {
    auto rules = economy();
    for (auto& menu : rules.menus) {
        menu.options.front().requirements.current_player = 100;
        menu.options.front().requirements.current_allies = 100;
    }
    rules.pads.neutral = 4;
    rules.pads.capture = {{41, whole(100), whole(1), {100, 200}, false, true, false}};
    rules.pads.influence = {{41, false}};
    auto initial = setup();
    const m::Vec3 remote{whole(1000), {}, {}};
    initial.units.push_back({6, 41, 4, remote, m::identity_quat(), {}});
    initial.units.push_back({7, 10, 3, remote, m::identity_quat(), {}});
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto made = t::TacticalSession::create(initial, {}, durability(), {}, std::nullopt, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "recount fixture creates");
        if (!made) return;
        auto world = std::move(made).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned sequence = 0; sequence < 6; ++sequence) expect(static_cast<bool>(world.submit(buy(0, 1, sequence, 1, 10))), "recount buy submits");
        expect(static_cast<bool>(world.submit(buy(0, 2, 0, 2, 10))), "allied key buy submits");
        expect(static_cast<bool>(world.submit({{15, 2, 1}, {4}, t::DamagePayload{whole(10000)}})), "recount lethal damage submits");
        expect(world.ownership_census_matches_recount_for_testing(executor), "recount matches oracle before the first tick");
        for (unsigned tick = 0; tick < 100; ++tick) {
            if (tick == 10) expect(static_cast<bool>(world.stage_remove(5)), "recount: unit removed between ticks");
            if (tick == 20) for (const auto owner : {1U, 2U, 3U}) expect(static_cast<bool>(world.stage_spawn({0, 10, owner, {}, m::identity_quat(), {}})), "recount: allied units spawned between ticks");
            if (tick == 40) expect(static_cast<bool>(world.stage_remove(7)), "recount: capture influence removed");
            if (tick == 50) {
                const auto saved = world.save_checkpoint_state();
                expect(static_cast<bool>(saved), "recount checkpoint saves");
                if (!saved) return;
                auto restored = t::TacticalSession::from_replay(world.record(), {}, durability(), {}, std::nullopt, {}, {}, {}, rules);
                expect(static_cast<bool>(restored), "recount restore fixture creates");
                if (!restored) return;
                const auto loaded = restored.value().load_checkpoint_state(saved.value());
                expect(static_cast<bool>(loaded), "recount checkpoint restores");
                if (!loaded) return;
                expect(restored.value().state_sha256() == world.state_sha256(), "restore retains canonical state");
                world = std::move(restored).value();
            }
            expect(world.ownership_census_matches_recount_for_testing(executor), "recount matches before stepping staged changes or restored state");
            const auto stepped = world.step(executor); expect(static_cast<bool>(stepped), "recount tick steps"); if (!stepped) return;
            expect(world.ownership_census_matches_recount_for_testing(executor), "recount matches after every tick through births, deaths, transfers and restore");
            if (tick == 15) expect(!world.unit_state(4), "recount fixture exercises combat death");
            if (tick == 35) expect(world.unit_state(6) && world.unit_state(6)->owner == 3, "recount fixture captures neutral key for an allied owner");
            if (tick == 99) expect(world.unit_state(6) && world.unit_state(6)->owner == 4, "recount fixture returns captured key to neutral");
            const auto hash = world.state_sha256();
            if (workers == 1) reference.push_back(hash);
            else expect(hash == reference[tick], "recount transitions and restore agree at 1/2/4/8 workers");
        }
    }
}
void test_production_work() {
    auto rules = economy();
    for (auto& menu : rules.menus) menu.options.front().requirements.current_player = 100;
    auto world = create(rules); if (!world) return;
    for (unsigned sequence = 0; sequence < 12; ++sequence) expect(static_cast<bool>(world->submit(buy(0, 1, sequence, 1, 10))), "batch buy submits");
    ProductionExecutor executor; allocation_probe::count = 0;
    auto first = world->step(executor); expect(static_cast<bool>(first), "batch buys step"); if (!first) return;
    expect(first.value().production_census_visits == 2 * setup().units.size(), "WPR-55: twelve buys share one command census and one queue census");
    for (unsigned tick = 1; tick < 15; ++tick) {
        const auto next = world->step(executor); expect(static_cast<bool>(next), "active queue steps"); if (!next) return;
        expect(next.value().production_census_visits == setup().units.size(), "WPR-55: active queues visit each survivor once");
    }
    expect(allocation_probe::count == 0, "WPR-55: production partitions allocate no census nodes");
    auto unlimited = create(); if (!unlimited) return;
    expect(static_cast<bool>(unlimited->submit(buy(0, 1, 0, 1, 10))), "unlimited buy submits");
    const auto uncounted = unlimited->step(executor);
    expect(uncounted && uncounted.value().production_census_visits == 0, "WPR-55: unconstrained buys and queues need no census");
    auto upgraded = create(); if (!upgraded) return;
    expect(static_cast<bool>(upgraded->submit(buy(0, 1, 0, 1, 90))), "bonus budget buy submits");
    allocation_probe::count = 0;
    bool evaluated = false;
    for (unsigned tick = 0; tick < 4; ++tick) {
        const auto next = upgraded->step(executor); expect(static_cast<bool>(next), "bonus budget steps"); if (!next) return;
        evaluated = evaluated || next.value().bonus_profile_evaluations != 0;
    }
    expect(evaluated && allocation_probe::count == 0, "WPR-55: source profiles evaluate without per-unit category allocation");
    for (unsigned birth = 0; birth < 8; ++birth) {
        const auto spawned = upgraded->stage_spawn({0, 10, 1, {}, m::identity_quat(), {}});
        expect(spawned && upgraded->durability_state(spawned.value())->hull == whole(125), "WPR-55: repeated births receive cached bonuses");
        const auto next = upgraded->step(executor);
        expect(next && next.value().bonus_profile_evaluations == 0, "WPR-55: repeated births never rebuild source profiles");
    }
}
void test_hero_command_ledger() {
    auto content = economy();
    content.command_bonuses = {
        {40, 0, {3, {10, 40}, {decimal("0.25"), {}, decimal("0.25"), decimal("0.25"), {}, {}}}, 0, false, false, {}, {}},
        {41, 0, {3, {10}, {decimal("0.5"), {}, {}, {}, {}, {}}}, 0, false, false, {}, {}},
        {41, 1, {4, {10}, {decimal("0.25"), {}, {}, {}, {}, {}}}, 0, false, false, {}, {}},
    };
    auto initial = setup();
    initial.units.erase(initial.units.begin(), initial.units.begin() + 2);
    auto health = durability();
    health.profiles[0].max_shields = {};
    auto created = t::TacticalSession::create(initial, {}, health, {}, std::nullopt, {}, {}, {}, content);
    expect(static_cast<bool>(created), "WHE-53: recipient-first world creates");
    if (!created) return;
    auto world = std::move(created).value();
    expect(static_cast<bool>(world.submit({{0, 1, 0}, {3}, t::DamagePayload{whole(20)}})), "damage hull before command source");
    expect(static_cast<bool>(world.submit({{0, 1, 1}, {3}, t::DamagePayload{whole(20), 0}})), "damage hardpoint before command source");
    through(world, 1);
    const auto before = *world.durability_state(3);
    const auto source = world.stage_spawn({0, 40, 1, {whole(50000), {}, {}}, m::identity_quat(), {}});
    expect(static_cast<bool>(source), "WHE-15/53: distant source stages after recipients");
    if (!source) return;
    const auto raised = *world.durability_state(3);
    expect(raised.hull == Fixed::from_raw(before.hull.raw() + whole(25).raw())
        && raised.hardpoints[0] == m::multiply(before.hardpoints[0], decimal("1.25")).value(),
        "WHE-18: hull gains maximum delta and damaged hardpoints scale");
    expect(raised.energy == whole(125) && raised.shields == whole(0),
        "WHE-18: powered energy gains; unshielded ship gains no shield");
    expect(world.durability_state(4)->hull == whole(125) && world.durability_state(5)->hull == whole(100),
        "WHE-13: all allies qualify and enemy does not");
    expect(world.durability_state(source.value())->hull == whole(1000),
        "WHE-53: source creation self gate defaults off despite matching type");
    const auto next = world.stage_spawn({0, 10, 2, {}, m::identity_quat(), {}});
    expect(next && world.durability_state(next.value())->hull == whole(125),
        "WHE-11: source-first newly created recipient qualifies");
    const auto stronger = world.stage_spawn({0, 41, 1, {}, m::identity_quat(), {}});
    expect(stronger && world.durability_state(4)->hull == whole(175),
        "WHE-17: largest same category and sum distinct categories");
    if (!stronger) return;
    expect(static_cast<bool>(world.stage_remove(stronger.value())), "WHE-12: stronger source is removed");
    expect(world.durability_state(4)->hull == whole(125),
        "WHE-19: removing stronger source reveals the retained weaker contribution");
    expect(static_cast<bool>(world.stage_remove(source.value())), "WHE-12: final source is removed");
    expect(world.durability_state(4)->hull == whole(100) && world.durability_state(4)->energy == whole(100),
        "WHE-19: final source removal clamps current hull and energy");

    content.command_bonuses = {{40, 0, {0, {10}, {decimal("0.25"), {}, {}, {}, {}, {}}}, 200, false, false, {}, {}}};
    auto faction = t::TacticalSession::create(setup(), {}, durability(), {}, std::nullopt, {}, {}, {}, content);
    expect(faction && faction.value().durability_state(3)->hull == whole(100)
        && faction.value().durability_state(4)->hull == whole(125) && faction.value().durability_state(5)->hull == whole(125),
        "WHE-13: exact faction replaces allied eligibility");

    content.command_bonuses = {{40, 0, {0, {10}, {decimal("-0.25"), {}, {}, {}, {}, {}}}, 0, false, false, {}, {}}};
    auto negative = t::TacticalSession::create(initial, {}, health, {}, std::nullopt, {}, {}, {}, content);
    expect(static_cast<bool>(negative), "WHE-55: negative command content creates");
    if (!negative) return;
    const auto loss = negative.value().stage_spawn({0, 40, 1, {}, m::identity_quat(), {}});
    expect(loss && negative.value().durability_state(3)->hull == whole(75), "WHE-55: negative category survives aggregation");
    if (loss) {
        expect(static_cast<bool>(negative.value().stage_remove(loss.value())), "negative source removed");
        expect(negative.value().durability_state(3)->hull == whole(75),
            "WHE-19: removing a negative bonus raises maximum without healing current hull");
    }
    ProductionExecutor budget;
    for (unsigned tick = 0; tick < 8; ++tick)
        expect(static_cast<bool>(negative.value().step(budget)), "idle command budget fixture steps");
    expect(budget.command_phases == 0, "WHE-53: stationary ticks perform no command discovery or application phases");

    content.command_bonuses = {{40, 0, {0, {10}, {decimal("0.25"), {}, decimal("0.25"), {}, {}, {}}}, 0, false, false, {}, {}}};
    auto carried_setup = setup();
    carried_setup.units[0].type_id = 41;
    carried_setup.units[1].type_id = 41;
    content.heroes = {{10, 41, {{40, true, false}}}};
    health.profiles[0].powered = false;
    auto carried = t::TacticalSession::create(carried_setup, {}, health, {}, std::nullopt, {}, {}, {}, content);
    expect(static_cast<bool>(carried), "contained source fixture creates");
    if (!carried) return;
    expect(static_cast<bool>(carried.value().submit(buy(0, 1, 0, 1, 10))), "buy contained source company");
    expect(static_cast<bool>(carried.value().submit({{22, 1, 1}, {}, t::ReinforcePayload{10, {whole(2000), {}, {}}}})),
        "reinforce contained source company");
    through(carried.value(), 24);
    expect(carried.value().durability_state(3)->hull == whole(125)
        && carried.value().durability_state(3)->energy == whole(100),
        "WHE-41: contained source applies; unpowered recipient ignores energy bonus");
    if (carried) {
        const auto live = carried.value().units();
        const auto host = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.purchase_type == 10; });
        expect(host != live.end(), "reinforced company has contained source host");
        if (host == live.end()) return;
        expect(static_cast<bool>(carried.value().stage_remove(host->entity_id)), "contained source host removed");
        expect(carried.value().durability_state(3)->hull == whole(100), "host loss retires contained command source");
    }
}

void test_hero_worker_determinism() {
    auto content = economy();
    content.command_bonuses = {{40, 0, {0, {10}, {decimal("0.25"), decimal("0.1"), {}, {}, decimal("0.1"), decimal("0.1")}}, 0, false, false, {}, {}}};
    std::string expected;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = t::TacticalSession::create(setup(), {}, durability(), {}, std::nullopt, {}, {}, {}, content);
        expect(static_cast<bool>(created), "hero deterministic fixture creates");
        if (!created) return;
        auto world = std::move(created).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        for (unsigned tick = 0; tick < 5; ++tick) {
            if (tick == 2) expect(static_cast<bool>(world.submit({{2, 1, 0}, {1}, t::DamagePayload{whole(5000)}})), "kill first command source");
            const auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "hero source ledger steps on each worker count");
            if (!stepped) return;
            if (tick != 2) expect(stepped.value().bonus_profile_evaluations == 0,
                "WHE-15: idle command sources perform no profile scans");
        }
        const auto hash = world.state_sha256();
        if (expected.empty()) expected = hash;
        else expect(hash == expected, "hero command source loss hashes equal on 1/2/4/8 workers");
    }
}
void test_signed_command_reduction() {
    t::CombatBonuses category{};
    t::accumulate_combat_bonus(category, {decimal("-0.5"), {}, {}, {}, {}, {}});
    t::accumulate_combat_bonus(category, {decimal("-0.25"), {}, {}, {}, {}, {}});
    expect(category[0] == decimal("-0.25"), "WHE-55: numeric largest negative wins over a more negative source");
    std::array<t::CombatBonuses, 2> categories{{
        {whole(-1), whole(-1), whole(-1), whole(-1), decimal("0.5"), whole(-1)},
        {whole(-1), whole(-1), whole(-1), whole(-1), decimal("0.5"), whole(-1)}}};
    const auto capped = t::sum_combat_bonus_categories(categories);
    expect(capped == t::CombatBonuses{decimal("-0.99"), whole(-1), whole(-1), whole(-1), decimal("0.75"), decimal("-0.99")},
        "WHE-55: caps apply after addition with the same fixed rounding as XML percentages");
}
void test_bound_production_relationships() {
    const std::vector<t::SnapshotPlayer> relationships{{1, 0, false}, {2, 0, false}, {3, 0, true}};
    expect(t::players_allied(relationships, 1, 2) && t::players_allied(relationships, 1, 1)
        && !t::players_allied(relationships, 1, 3) && !t::players_allied(relationships, 1, 99),
        "WPR-33: bound neutral or absent players never become allies through numeric teams");
    for (const bool prerequisite : {false, true}) {
        auto start = setup();
        start.players[3].team_id = 0;
        start.players[3].faction_id = 300;
        start.units = {{1, 40, 1, {}, m::identity_quat(), {}},
            {5, prerequisite ? 10ULL : 90ULL, 4, {}, m::identity_quat(), {}}};
        auto rules = economy();
        if (prerequisite) for (auto& menu : rules.menus) menu.options[1].requirements.prerequisites = {10};
        t::CombatTable combat;
        combat.pad_neutral_factions = {300};
        auto created = t::TacticalSession::create(start, {}, durability(), {}, std::nullopt, combat, {}, {}, rules);
        expect(static_cast<bool>(created), "WPR-33 neutral relationship session starts");
        if (!created) continue;
        expect(created.value().build_allowed(1, 1, 90) == !prerequisite,
            "WPR-33: neutral objects neither consume allied limits nor satisfy allied prerequisites");
        expect(static_cast<bool>(created.value().submit(buy(0, 1, 0, 1, 91))), "WPR-02 level-up records");
        through(created.value(), 6);
        expect(account(created.value(), 1).tech_level == 2 && account(created.value(), 4).tech_level == 1,
            "WPR-02: station level-up excludes a neutral with a matching team");
    }
}

void test_live_station_repair() {
    auto health = durability();
    for (auto& profile : health.profiles) profile.max_shields = {}; // isolate hardpoint repair from shield absorption
    for (auto& profile : health.profiles) for (auto& hp : profile.hardpoints) {
        hp.repair_amount_per_frame = decimal("0.5"); hp.repair_cost_per_frame = decimal("1.5");
    }
    const auto station = [&](t::TacticalSession& world, eawr::sim::EntityId id) -> const t::TacticalInstance& {
        const auto instances = world.snapshot()->instances();
        return *std::find_if(instances.begin(), instances.end(), [&](const auto& unit) { return unit.entity_id == id; });
    };
    const auto make = [&](m::Fixed credits, m::Fixed amount = decimal("0.5")) {
        auto rules = economy();
        for (auto& player : rules.players) player.credits = credits;
        auto table = health;
        for (auto& profile : table.profiles) for (auto& hp : profile.hardpoints) hp.repair_amount_per_frame = amount;
        auto result = t::TacticalSession::create(setup(), {}, table, {}, std::nullopt, {}, {}, {}, rules);
        expect(static_cast<bool>(result), "WSL repair fixture creates");
        return std::move(result).value();
    };
    const auto command = [&](t::TacticalSession& world, std::uint64_t tick, t::PlayerId payer, std::uint64_t sequence,
                             eawr::sim::EntityId id, const t::CommandPayload& payload) {
        expect(static_cast<bool>(world.submit({{tick, payer, sequence}, {id}, payload})), "WSL repair command submits");
    };
    for (const auto credits : {decimal("1.4"), decimal("1.5"), whole(100)}) {
        auto world = make(credits);
        expect(account(world, 1).credits == credits, "WSL-38 setup performs no service or debit");
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 1, 2, 1, t::RepairHardpointPayload{0}); // duplicate payer is idempotent
        through(world, 1);
        const bool affordable = credits >= decimal("1.5");
        const auto& hp = station(world, 1).durability->hardpoints[0];
        expect(hp.health == (affordable ? decimal("98.5") : whole(98)), "WSL-41 damaged own station pays once per frame");
        expect(account(world, 1).credits == (affordable ? m::Fixed::from_raw(credits.raw() - decimal("1.5").raw()) : credits),
            "WSL-41 exactly sufficient credits debit; insufficient credits do not");
        expect(hp.repairing_players.size() == (affordable ? 1U : 0U), "WSL-41 unpaid payer drops out");
        through(world, 5);
        if (credits == whole(100)) {
            expect(station(world, 1).durability->hardpoints[0].health == whole(100), "WSL-42 completes at maximum");
            expect(station(world, 1).durability->hardpoints[0].repairing_players.empty(), "WSL-42 full clears all payers");
            expect(account(world, 1).credits == whole(94), "WSL-42 no charge after full completion");
        } else expect(station(world, 1).durability->hardpoints[0].repairing_players.empty(), "WSL-41 insolvency stops next frame");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 2, 0, 1, t::RepairHardpointPayload{0});
        through(world, 1);
        expect(station(world, 1).durability->hardpoints[0].health == whole(99), "WSL-41 two payers contribute in one service");
        expect(account(world, 1).credits == decimal("98.5") && account(world, 2).credits == decimal("98.5"),
            "WSL-40 event/service permit a second payer without an ownership gate");
        through(world, 3);
        expect(account(world, 1).credits == whole(97) && account(world, 2).credits == whole(97), "WSL-42 two payers stop after completion");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{decimal("0.5"), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 2, 0, 1, t::RepairHardpointPayload{0});
        through(world, 2);
        expect(account(world, 1).credits == decimal("98.5") && account(world, 2).credits == whole(100),
            "WSL-41 completing payer prevents a later payer debit");
    }
    {
        auto world = make(decimal("1.5"));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::DamagePayload{whole(2), 1});
        command(world, 0, 1, 2, 2, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 3, 1, t::RepairHardpointPayload{0});
        command(world, 0, 1, 4, 1, t::RepairHardpointPayload{1});
        command(world, 0, 1, 5, 2, t::RepairHardpointPayload{0});
        through(world, 1);
        expect(station(world, 1).durability->hardpoints[0].health == decimal("98.5")
            && station(world, 1).durability->hardpoints[1].health == whole(98)
            && station(world, 2).durability->hardpoints[0].health == whole(98),
            "WSL-41 shared account arbitration orders station, slot, payer without overspending");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(100), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        const auto events = through(world, 1);
        expect(events.back().kind == t::EventKind::order_rejected && events.back().reason == t::RejectReason::hardpoint_invalid,
            "WSL-40 destroyed hardpoint rejects repair");
        expect(account(world, 1).credits == whole(100), "WSL-41 destroyed rejection has no debit");
    }
    {
        auto world = make(whole(100));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 1, 2, 1, t::DamagePayload{whole(1000), t::hull_target});
        through(world, 1);
        expect(account(world, 1).credits == whole(100), "WSL-38 deletion pending parent never charges repair");
    }
    {
        auto world = make(whole(100), whole(0));
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(2), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        through(world, 2);
        expect(station(world, 1).durability->hardpoints[0].health == whole(98) && account(world, 1).credits == whole(97),
            "WSL-42 zero authored repair amount pays without progressing");
    }
    {
        auto before = t::full_durability(health.profiles[1]);
        before.hardpoints[0] = whole(98); before.repairing_players = {{99, 1, 2}, {}};
        std::array<t::RepairBudget, 2> budgets{{{1, decimal("1.4")}, {2, decimal("1.5")}}};
        const auto paid = t::reserve_hardpoint_repairs(health.profiles[1], before, budgets);
        expect(paid[0] == std::vector<t::PlayerId>{2} && budgets[0].credits == decimal("1.4") && budgets[1].credits == whole(0),
            "WSL-41 missing and insolvent payer removal does not skip the shifted next payer");
    }
    for (const bool active : {false, true}) {
        auto world = make(whole(1000));
        command(world, 0, 1, 0, 1, t::DamagePayload{active ? whole(10) : whole(100), 0});
        if (active) command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        expect(static_cast<bool>(world.submit(buy(0, 1, active ? 2 : 1, 1, 91))), "WSL-33 upgrade submits during repair");
        const auto events = through(world, 7);
        const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; });
        expect(replaced != events.end(), "WSL-33 replacement event exists");
        if (replaced == events.end()) continue;
        const auto replacement_id = replaced->sequence;
        const auto& replacement = station(world, replacement_id);
        const auto hp = replacement.durability->hardpoints[0];
        expect(replacement.type_id == 41 && !hp.enabled, "WSL-33 upgrade transfers disabled repair state by index");
        if (active) expect(hp.health == decimal("93.5") && hp.repairing_players == std::vector<t::PlayerId>{1},
            "WSL-33 current health and payers survive replacement and subsequent paid service");
        else {
            expect(hp.health == decimal("0.1"), "WSL-33 destroyed slot becomes disabled at 0.1");
            command(world, 7, 1, 2, replacement_id, t::RepairHardpointPayload{0});
        }
        through(world, 210);
        expect(station(world, replacement_id).durability->hardpoints[0].enabled
            && station(world, replacement_id).durability->hardpoints[0].health == whole(100), "WSL-42 upgrade repair re-enables slot at full");
    }
    {
        auto rules = economy();
        for (auto& player : rules.players) { player.credits = whole(1000); player.max_tech = 5; }
        rules.menus.clear();
        auto table = health;
        for (const auto type : {42ULL, 43ULL, 44ULL}) {
            auto profile = health.profiles.back(); profile.type_id = type; table.profiles.push_back(profile);
        }
        for (const auto type : {40ULL, 41ULL, 42ULL, 43ULL, 44ULL}) for (const auto faction : {100ULL, 200ULL}) {
            auto level = option(91, t::BuildKind::upgrade, t::BuildQueue::units, 4);
            level.requirements.current_allies = 1; level.requirements.prerequisites = {type};
            t::StationMenu menu;
            menu.station = type; menu.faction = faction; menu.options.push_back(std::move(level));
            menu.next_level = type < 44 ? type + 1 : 0;
            rules.menus.push_back(std::move(menu));
        }
        auto result = t::TacticalSession::create(setup(), {}, table, {}, std::nullopt, {}, {}, {}, rules);
        expect(static_cast<bool>(result), "WSL-33 levels one through five fixture creates");
        if (!result) return;
        auto world = std::move(result).value();
        command(world, 0, 1, 0, 1, t::DamagePayload{whole(50), 0});
        command(world, 0, 1, 1, 1, t::RepairHardpointPayload{0});
        command(world, 0, 2, 0, 1, t::RepairHardpointPayload{0});
        eawr::sim::EntityId current = 1;
        for (std::uint64_t level = 1; level < 5; ++level) {
            const auto tick = world.completed_tick();
            expect(static_cast<bool>(world.submit(buy(tick, 1, level + 1, current, 91))), "WSL-33 next level submits with active repair");
            const auto events = through(world, tick + 7);
            const auto replaced = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.kind == t::EventKind::station_replaced; });
            expect(replaced != events.end(), "WSL-33 each level replaces the station");
            if (replaced == events.end()) break;
            current = replaced->sequence;
            const auto& slot = station(world, current).durability->hardpoints[0];
            expect(station(world, current).type_id == 40 + level && !slot.enabled
                && slot.repairing_players == std::vector<t::PlayerId>{1, 2}, "WSL-33 both payers and disabled repair state carry through levels two to five");
        }
        through(world, 100);
        expect(station(world, current).type_id == 44 && station(world, current).durability->hardpoints[0].enabled
            && station(world, current).durability->hardpoints[0].repairing_players.empty(), "WSL-42 level five completes repair and clears both payers");
    }
    t::TacticalReplay replay{setup(), 24, {
        {{0, 1, 0}, {1}, t::DamagePayload{whole(10), 0}},
        {{0, 1, 1}, {1}, t::RepairHardpointPayload{0}},
        {{0, 2, 0}, {1}, t::RepairHardpointPayload{0}},
        buy(1, 1, 2, 1, 91)}};
    const auto encoded = t::write_replay(replay);
    expect(static_cast<bool>(encoded), "WSL-40 repair opcode writes");
    if (!encoded) return;
    const auto parsed = t::parse_replay(encoded.value());
    expect(parsed && parsed.value() == replay, "WSL-40 repair replay round trip");
    const auto run = [&](const eawr::sim::PartitionExecutor& executor, bool scrambled) {
        auto result = t::TacticalSession::from_replay(replay, {}, health, {}, std::nullopt, {}, {}, {}, economy());
        expect(static_cast<bool>(result), "WSL repair replay loads");
        auto world = std::move(result).value();
        if (scrambled) world.scramble_storage_for_testing();
        std::vector<std::string> hashes;
        while (world.completed_tick() < replay.final_tick_count) {
            auto tick = world.step(executor); expect(static_cast<bool>(tick), "WSL repair replay steps");
            if (!tick) break;
            hashes.push_back(tick.value().state_sha256 + tick.value().snapshot->sha256());
        }
        return hashes;
    };
    eawr::sim::InlineExecutor inline_executor;
    const auto reference = run(inline_executor, false);
    for (const auto count : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter workers(count);
        expect(run(workers, false) == reference && run(workers, true) == reference,
            "WSL-41 repair and upgrade hashes/snapshots equal at 1/2/4/8 workers and scrambled storage");
    }
}

void test_human_pad_radar_creation() {
    for (const bool enabled : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup start;
            start.seed = 540;
            start.players = {{1, 0, 100, 1}, {2, 1, 200, 1}};
            start.units = {{1, 20, 1, {}, m::identity_quat(), {}}};
            t::EconomyRules rules;
            rules.players = {{1, whole(1000), 0}};
            t::StationMenu menu;
            menu.station = 20; menu.faction = 100; menu.station_producer = false;
            menu.options = {option(30, t::BuildKind::structure, t::BuildQueue::units, 30)};
            rules.menus = {menu};
            rules.pads.neutral = 2;
            rules.pads.capture = {{20, whole(100), whole(1), {100, 200}, false, true, true}};
            rules.pads.construction = {{30, 40, whole(100), 1, 1}};
            rules.pads.influence = {{20, false}, {30, false}, {40, false}};
            t::SpecialAbilityProfile radar;
            radar.name = "Construction_Radar"; radar.kind = t::SpecialAbilityKind::enable_radar;
            radar.style = t::SpecialActivationStyle::skirmish_automatic; radar.initially_enabled = enabled;
            t::UnitAbilityProfile source; source.type_id = 30; source.special = {radar};
            t::AbilityTable abilities; abilities.profiles = {source};
            t::DurabilityTable health;
            for (const t::TypeId type : {20U, 30U, 40U}) {
                t::DurabilityProfile row; row.type_id = type; row.max_hull = whole(300);
                if (type == 30) {
                    t::HardpointProfile point;
                    point.destroyable = true; point.max_health = whole(100); point.special_ability_name = radar.name;
                    row.hardpoints.push_back(point);
                }
                health.profiles.push_back(row);
            }
            auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, abilities, rules);
            expect(static_cast<bool>(made), "FC-75: human pad radar fixture creates");
            if (!made) { std::cerr << made.error().message << '\n'; continue; }
            auto world = std::move(made).value();
            expect(!world.snapshot()->players().front().radar_enabled, "FC-75: no pad radar source before birth");
            expect(static_cast<bool>(world.submit({{0, 1, 0}, {1}, t::PadBuildPayload{30}})),
                "FC-75: human radar pad build submits");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 3; ++tick) {
                auto result = world.step(executor);
                expect(static_cast<bool>(result), "FC-75: human pad radar construction steps");
                if (!result) { std::cerr << result.error().message << '\n'; break; }
                expect(world.construction().size() == 1 && world.pads().at(1).under_construction != 0,
                    "FC-75: human request creates a construction placeholder");
                expect(world.snapshot()->players().front().radar_enabled == enabled,
                    "FC-75: construction radar publishes immediately without unlocking named gates");
                hashes.push_back(result.value().state_sha256 + result.value().snapshot->sha256());
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "FC-75: human pad radar hashes and snapshots equal on 1/2/4/8 workers");
        }
    }
}

} // namespace station_upgrade_test_support
