// P2-10 (#73): target choice and weapon fire (docs/behaviour/space-targeting.md,
// docs/behaviour/space-weapon-fire.md). The targeting note's cases run through the opportunity
// service; the session cases use a TIE Defender-like shooter; a combat battle's hashes are pinned
// in tactical-combat.hashes.csv and must be reproduced by every worker count, a scrambled storage
// order and a written-and-parsed replay. `combat_tests <fixtures> <cases.json> --update` rewrites the pin.

#include "../support/self_test.hpp"
#include "../support/check.hpp"
#include "combat_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/fighters_internal.hpp"
#include "../../apps/sim_headless/json.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace combat_test_support {

using sim_headless::Json;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}


[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {units(x), units(y), units(z)};
}

// --- The targeting note's cases (tests/behaviour/space-targeting-cases.json) -----------------


// HP_TIE_DEFENDER_ION_00-like: range 700, fixed 45 x 45 cone, 2-shot pulse 0.5 s apart,
// recharge 0.5 to 3.5 s, opportunity fire when idle and when targeting; the FoC Fighter set.
[[nodiscard]] tactical::WeaponProfile ion(const std::uint64_t restrictions) {
    tactical::WeaponProfile weapon;
    weapon.hardpoint = 0;
    weapon.range = units(700);
    weapon.min_recharge_hundredths = 50;
    weapon.max_recharge_hundredths = 350;
    weapon.pulse_count = 2;
    weapon.pulse_delay_frames = 15;
    weapon.cone_width = units(45);
    weapon.cone_height = units(45);
    weapon.category_restrictions = restrictions;
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(5, 2);
    weapon.fire_b = at(5, -2);
    weapon.has_fire_b = true;
    weapon.randomize_between_fire_bones = false; // This fixture authors WWP-22 endpoint selection.
    return weapon;
}

[[nodiscard]] tactical::CombatTable table(const std::uint64_t restrictions) {
    tactical::CombatTable result;
    tactical::PrioritySet fighter_set;
    fighter_set.unlisted = units(1000);
    fighter_set.rows = {{fighter_type, units(3)}, {bomber_type, units(2)}, {transport_type, units(1)}};
    result.priority_sets.push_back(fighter_set);
    tactical::CombatProfile shooter;
    shooter.type_id = shooter_type;
    shooter.category_bits = fighter_bit;
    shooter.priority_set = 0;
    shooter.max_attack_distance = units(1000);
    shooter.weapons = {ion(restrictions)};
    shooter.hardpoints = {{0, at(5, 0), true}};
    tactical::CombatProfile gunship;
    gunship.type_id = gunship_type;
    gunship.category_bits = fighter_bit;
    gunship.priority_set = 0;
    gunship.max_attack_distance = units(1000);
    tactical::WeaponProfile own;
    own.range = units(1000);
    own.min_recharge_hundredths = 100;
    own.max_recharge_hundredths = 100;
    gunship.weapons = {own};
    result.profiles = {shooter,
        tactical::CombatProfile{fighter_type, fighter_bit, std::nullopt, std::nullopt, {}, {}, {}},
        tactical::CombatProfile{bomber_type, bomber_bit, std::nullopt, std::nullopt, {}, {}, {}},
        tactical::CombatProfile{transport_type, transport_bit, std::nullopt, std::nullopt, {}, {}, {}}, gunship};
    return result;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors(const std::int64_t range) {
    std::vector<tactical::SensorProfile> result;
    for (tactical::TypeId type = 1; type <= 5; ++type) result.push_back({type, units(range)});
    return result;
}

// A unit facing +X (yaw 0) or -X (yaw 180).
[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3 position, const bool facing_west) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = position;
    state.rotation = facing_west ? math::Quat{Fixed{}, Fixed{}, units(1), Fixed{}} : math::identity_quat();
    return state;
}

[[nodiscard]] tactical::TacticalSetup setup(std::vector<tactical::UnitState> units_in) {
    tactical::TacticalSetup result;
    result.seed = 12345;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = std::move(units_in);
    return result;
}

[[nodiscard]] tactical::TacticalSession session(const tactical::TacticalSetup& value,
    const tactical::CombatTable& combat, const std::int64_t sensor_range) {
    auto created = tactical::TacticalSession::create(value, sensors(sensor_range), {}, {}, std::nullopt, combat);
    expect(static_cast<bool>(created), "combat session is created");
    return std::move(created).value();
}

// Steps `ticks` frames and returns every combat event.
std::vector<Shot> run(tactical::TacticalSession& value, const std::uint64_t ticks) {
    std::vector<Shot> result;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "combat step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.push_back({event.tick, event.kind, event.shooter, event.target, event.target_hardpoint});
        }
    }
    return result;
}

[[nodiscard]] eawr::sim::EntityId opportunity_target(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    const auto state = value.combat_state(id);
    return state && !state->weapons.empty() ? state->weapons[0].opportunity.target : 0;
}

void test_charged_blast_budget() {
    namespace d = tactical::detail;
    std::optional<tactical::ChargedBlastState> charge{{2, 0, false}};
    tactical::service_charged_blast(charge, 9);
    expect(charge && charge->charge_remaining == 1 && !charge->charged_presentation,
        "FSC-56: one admitted poll decrements once, without elapsed-time catch-up");
    tactical::service_charged_blast(charge, 2);
    expect(charge && charge->shots_remaining == 2 && charge->charged_presentation,
        "FSC-56: completion snapshots this poll's eligible gun count");
    charge = tactical::ChargedBlastState{1, 0, false};
    tactical::service_charged_blast(charge, 0);
    expect(charge && charge->charged_presentation, "FSC-58: zero eligible guns still publish charged presentation");
    tactical::service_charged_blast(charge, 20);
    expect(!charge, "FSC-58: the next admitted poll cleans the zero budget without recounting");

    auto content = table();
    auto special = content.profiles.front().weapons.front();
    special.special = true;
    special.pulse_count = 1;
    special.min_recharge_hundredths = special.max_recharge_hundredths = 100;
    special.shot.emplace(); special.shot->speed = units(25); special.shot->damage = units(600);
    auto ordinary = special; ordinary.special = false; ordinary.hardpoint = 1;
    content.profiles.front().weapons = {special, ordinary};
    auto state = tactical::initial_combat(content.profiles.front(), 12345, 0, 1);
    for (auto& gun : state.weapons) gun.countdown = 0;
    state.attack_target = 2; state.direct = true;
    const auto players = setup({}).players;
    const std::vector<tactical::SnapshotPlayer> relations{{1, 1}, {2, 2}};
    d::CombatWorld world; world.players = players; world.relationships = relations; world.table = &content;
    world.seed = 12345;
    d::CombatUnit own; own.id = 1; own.owner = 1; own.type_id = shooter_type;
    own.profile = &content.profiles.front(); own.combat = &state; own.visible_to = 3;
    own.transform = math::to_matrix(math::identity_quat(), {}).value();
    d::CombatUnit enemy; enemy.id = 2; enemy.owner = 2; enemy.player_index = 1; enemy.type_id = fighter_type;
    enemy.profile = &content.profiles[1]; enemy.position = enemy.previous_position = at(300, 0); enemy.visible_to = 3;
    enemy.transform = math::to_matrix(math::identity_quat(), enemy.position).value();
    world.units = {own, enemy};
    const auto fire = [&](const tactical::ChargedBlastState blast) {
        own.charged_blast = blast;
        auto result = d::step_combat(world, own);
        expect(static_cast<bool>(result), "FSC-57: charged combat attempt succeeds");
        return result;
    };
    auto pending = fire({1, 0, false});
    expect(pending && pending.value().events.empty(), "FSC-56: pending charge blocks both SPECIAL and ordinary guns");
    auto last = fire({0, 1, true});
    expect(last && last.value().events.size() == 2 && last.value().charged_blast->shots_remaining == 0
        && (last.value().events[0].outcome & tactical::fired_charged_shot) != 0
        && (last.value().events[1].outcome & tactical::fired_charged_shot) == 0,
        "FSC-57: last successful SPECIAL consumes inside the attempt and permits the later ordinary gun this turn");
    auto retained = fire({0, 2, true});
    expect(retained && retained.value().events.size() == 1 && retained.value().charged_blast->shots_remaining == 1,
        "FSC-57/59: a remaining shared budget blocks ordinary fire and is not recounted per gun");
    if (retained) {
        state = retained.value().state;
        state.weapons.front().countdown = 0;
        auto repeated = fire(*retained.value().charged_blast);
        expect(repeated && repeated.value().events.size() == 2 && repeated.value().charged_blast->shots_remaining == 0,
            "FSC-59: the same ready SPECIAL can consume the remaining shared count without a per-gun latch");
        for (auto& clock : state.weapons) clock.countdown = 0;
    }
    content.profiles.front().weapons.front().category_restrictions = fighter_bit;
    auto refused = fire({0, 1, true});
    expect(refused && refused.value().events.empty() && refused.value().charged_blast->shots_remaining == 1,
        "FSC-57: a failed SPECIAL attempt retains the budget and keeps the later ordinary gun blocked");
    content.profiles.front().weapons.front().category_restrictions = 0;
    state.weapons.front().countdown = 30;
    auto cooling = fire({0, 1, true});
    expect(cooling && cooling.value().events.empty() && cooling.value().charged_blast->shots_remaining == 1,
        "FSC-59: ordinary cooldown failure does not consume or reset the shared budget");
}

void test_special_weapon_service() {
    auto content = table();
    auto& weapon = content.profiles[0].weapons[0];
    weapon.special = true; weapon.opportunity_when_idle = false; weapon.opportunity_when_targeting = false;
    tactical::ShotProfile shot; shot.speed = units(25); shot.max_travel = units(700); shot.damage = units(40);
    weapon.shot = shot;
    const auto start = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 0))});
    auto idle = session(start, content);
    const auto idle_rows = run(idle, 180);
    expect(std::none_of(idle_rows.begin(), idle_rows.end(), [](const auto& event) {
        return event.kind == tactical::CombatEventKind::weapon_fired || event.kind == tactical::CombatEventKind::target_acquired;
    }), "WAD-33: SPECIAL with authored false opportunity flags never acquires an idle weapon target");
    for (const bool manual : {false, true}) for (const bool selected : {false, true}) {
        auto modified = content; auto& gun = modified.profiles[0].weapons[0];
        gun.requires_manual_target = manual; if (!selected) gun.shot.reset();
        auto value = session(start, modified);
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "WAD-32: ordinary attack submitted");
        const auto rows = run(value, 180);
        const auto fired = std::any_of(rows.begin(), rows.end(), [](const auto& event) {
            return event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1 && event.target == 2;
        });
        expect(fired == (selected && !manual), "WAD-31/32/39: assigned SPECIAL fires ordinarily; missing projectile and manual guard produce no shot");
    }
    weapon.category_restrictions = fighter_bit;
    auto restricted = session(start, content);
    expect(static_cast<bool>(restricted.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "WAD-33: restricted attack submitted");
    const auto rows = run(restricted, 180);
    expect(std::none_of(rows.begin(), rows.end(), [](const auto& event) {
        return event.kind == tactical::CombatEventKind::weapon_fired;
    }), "WAD-32/33: SPECIAL preserves ordinary category restriction gate");
}

// space-targeting R-01: a hardpoint whose authored maximum recharge is within
// Hardpoint_Recharge_Cutoff_For_Opportunity_Fire counts both opportunity flags as on for an AI owner,
// whatever the hardpoint data says; a human owner and a slower hardpoint keep the authored false flags.
void test_ai_opportunity_cutoff() {
    auto content = table();
    auto& weapon = content.profiles[0].weapons[0];
    weapon.opportunity_when_idle = false;
    weapon.opportunity_when_targeting = false;
    tactical::ShotProfile shot; shot.speed = units(25); shot.max_travel = units(700); shot.damage = units(40);
    weapon.shot = shot;
    const auto start = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 0))});
    const auto acquires = [&](const tactical::CombatTable& combat, const std::optional<bool> ai) {
        tactical::EconomyRules economy;
        if (ai) {
            tactical::EconomyPlayer lobby;
            lobby.player = 1;
            lobby.ai = *ai;
            economy.players = {lobby};
        }
        auto created = tactical::TacticalSession::create(start, sensors(2000), {}, {}, std::nullopt, combat, {}, {}, economy);
        expect(static_cast<bool>(created), "R-01: the economy session is created");
        if (!created) return false;
        auto value = std::move(created).value();
        const auto rows = run(value, 180);
        return std::any_of(rows.begin(), rows.end(), [](const auto& event) {
            return event.kind == tactical::CombatEventKind::weapon_fired || event.kind == tactical::CombatEventKind::target_acquired;
        });
    };
    expect(!acquires(content, std::nullopt), "R-01: without the flag nothing is forced");
    content.profiles[0].weapons[0].ai_opportunity_forced = true;
    expect(!acquires(content, std::nullopt), "R-01: a session without an economy table has no AI owner");
    expect(!acquires(content, false), "R-01: a human owner keeps the authored false flags");
    expect(acquires(content, true), "R-01: an AI owner's quick-recharge hardpoint opportunity-fires despite the false flags");
    content.profiles[0].weapons[0].ai_opportunity_forced = false;
    expect(!acquires(content, true), "R-01: a slower hardpoint keeps the authored false flags for an AI owner");
}

void test_neutral_relationships() {
    // WHZ-51: neutrality is a player relationship, even for a type without capture services.
    auto start = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(0, 30)),
        unit(3, bomber_type, 3, at(250, 0), true), unit(4, bomber_type, 2, at(350, 20), true)});
    start.players.push_back({3, 3, 30, tactical::player_flag_commandable});
    auto content = table();
    content.pad_neutral_factions = {30};
    std::string first_hash;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto value = session(start, content);
        const auto players = value.snapshot()->players();
        expect(!tactical::players_hostile(players, 1, 3) && !tactical::players_hostile(players, 3, 1)
            && tactical::players_hostile(players, 1, 2) && !tactical::players_hostile(players, 1, 1),
            "WHZ-51: the cursor relationship excludes neutral owners in both directions and retains enemies");
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1, 2}, tactical::AttackPayload{3}})),
            "neutral attack is queued for authoritative rejection");
        eawr::platform::ThreadWorkerAdapter pool(workers);
        bool positive = false;
        for (std::size_t frame = 0; frame < 150; ++frame) {
            const auto stepped = value.step(pool);
            expect(static_cast<bool>(stepped), "neutral relationship frame succeeds");
            if (!stepped) break;
            if (frame == 0) {
                expect(stepped.value().snapshot->events().size() == 2
                    && std::all_of(stepped.value().snapshot->events().begin(), stepped.value().snapshot->events().end(),
                        [](const auto& event) { return event.reason == tactical::RejectReason::target_not_hostile; }),
                    "WHZ-51: explicit attacks reject the neutral target for every selected unit");
            }
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                expect(event.target != 3 && event.shooter != 3, "WHZ-51: ship and opportunity routes exclude neutral players");
                positive |= event.kind == tactical::CombatEventKind::weapon_fired && event.target == 4;
            }
        }
        expect(positive, "WHZ-51: enemy positive control is still fired upon");
        if (first_hash.empty()) first_hash = value.state_sha256();
        else expect(first_hash == value.state_sha256(), "neutral combat agrees across 1/2/4/8 workers");
    }
    // A captured object has the captor's current relationship, independent of its original faction/type.
    start.units[2].owner = 2;
    auto captured = session(start, content);
    expect(static_cast<bool>(captured.submit({{0, 1, 0}, {1}, tactical::AttackPayload{3}})), "captured attack queues");
    const auto events = run(captured, 150);
    expect(captured.combat_state(1)->direct && captured.combat_state(1)->attack_target == 3
        && std::any_of(events.begin(), events.end(), [](const Shot& event) {
            return event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1 && event.target == 3;
        }), "WHZ-51: a captured hostile object accepts direct attack and weapon fire");

    // FT-01/02 also use the same gate for a retained approach and a fresh squadron scan.
    tactical::detail::CombatWorld world;
    std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}, {3, 3, true}};
    world.relationships = relationships;
    world.table = &content;
    tactical::CombatProfile scanner;
    scanner.max_attack_distance = units(1000);
    tactical::DurabilityProfile health;
    for (const auto& [id, owner, team, x] : {std::array<unsigned, 4>{1, 1, 1, 0},
             std::array<unsigned, 4>{2, 3, 3, 100}, std::array<unsigned, 4>{3, 2, 2, 200}}) {
        tactical::detail::CombatUnit entry;
        entry.id = id;
        entry.owner = owner;
        entry.team = team;
        entry.position = at(x, 0);
        entry.visible_to = 1;
        entry.profile = &scanner;
        entry.durability_profile = &health;
        world.units.push_back(entry);
    }
    tactical::SquadronState state;
    state.target = 2;
    state.approach = true;
    state.next_scan_frame = 100;
    tactical::SquadronProfile squadron;
    const auto retained = tactical::detail::squadron_target(world, state, squadron, world.units.front(), at(0, 0));
    expect(retained.target == 0, "WHZ-51: a squadron does not retain a neutral target while approaching");
    state.next_scan_frame = 0;
    const auto scanned = tactical::detail::squadron_target(world, state, squadron, world.units.front(), at(0, 0));
    expect(scanned.target == 3, "WHZ-51: a squadron scans past a nearer neutral object to its enemy");

    // WST-11: a stealth-invisible enemy is neither kept (even on a planned approach) nor acquired by a
    // squadron whose type cannot target stealth units; one that can still locks on.
    world.units[2].stealth_invisible = true;
    state.target = 3;
    state.approach = true;
    state.next_scan_frame = 100;
    const auto hidden_kept = tactical::detail::squadron_target(world, state, squadron, world.units.front(), at(0, 0));
    expect(hidden_kept.target == 0, "WST-11: a squadron drops a hidden enemy target, also on its approach");
    state.target = 0;
    state.approach = false;
    state.next_scan_frame = 0;
    const auto hidden_scan = tactical::detail::squadron_target(world, state, squadron, world.units.front(), at(0, 0));
    expect(hidden_scan.target == 0, "WST-11: a squadron scan skips a hidden enemy");
    tactical::CombatProfile seer = scanner;
    seer.target_stealth = true;
    auto detector = world.units.front();
    detector.profile = &seer;
    const auto detected = tactical::detail::squadron_target(world, state, squadron, detector, at(0, 0));
    expect(detected.target == 3, "WST-11: a squadron of a Target_Stealth_Units type still acquires the hidden enemy");
}

[[nodiscard]] tactical::TacticalReplay battle() {
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> list;
    eawr::sim::EntityId id = 1;
    for (std::int64_t row = 0; row < 6; ++row) {
        for (std::int64_t column = 0; column < 4; ++column) {
            list.push_back(unit(id++, row % 2 == 0 ? shooter_type : gunship_type, 1, at(-300 - column * 40, row * 60 - 150)));
            const auto type = static_cast<tactical::TypeId>(2 + (row + column) % 3);
            list.push_back(unit(id++, type == transport_type ? shooter_type : type, 2, at(300 + column * 40, row * 60 - 150), true));
        }
    }
    replay.setup = setup(list);
    replay.commands.push_back({{10, 1, 0}, {1, 3, 5}, tactical::AttackPayload{8}});
    replay.commands.push_back({{40, 2, 0}, {2, 4}, tactical::AttackPayload{7}});
    replay.commands.push_back({{70, 1, 1}, {1}, tactical::StopPayload{}});
    replay.final_tick_count = 120;
    return replay;
}

struct Trace {
    std::vector<std::string> rows; // tick,state,snapshot
    std::size_t shots{};
};

[[nodiscard]] Trace trace(tactical::TacticalSession value, const eawr::sim::PartitionExecutor& executor,
    const std::uint64_t ticks, const bool scramble) {
    Trace result;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (scramble) value.scramble_storage_for_testing();
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "battle step succeeds");
        if (!stepped) break;
        result.rows.push_back(std::to_string(stepped.value().completed_tick) + ',' + stepped.value().state_sha256 + ','
            + stepped.value().snapshot->sha256());
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.shots += event.kind == tactical::CombatEventKind::weapon_fired;
        }
    }
    return result;
}

void test_battle(const std::filesystem::path& fixtures, const bool update) {
    const auto replay = battle();
    const auto golden = fixtures / "tactical-combat.hashes.csv";
    const auto created = [&] {
        auto value = tactical::TacticalSession::from_replay(replay, sensors(), {}, {}, std::nullopt, table());
        expect(static_cast<bool>(value), "battle session is created");
        return std::move(value).value();
    };
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = trace(created(), inline_executor, replay.final_tick_count, false);
    expect(reference.shots > 50, "the battle fires");
    if (update) {
        std::ofstream output(golden, std::ios::binary);
        output << "tick,state_sha256,snapshot_sha256\n";
        for (const auto& row : reference.rows) output << row << '\n';
        std::cout << "wrote " << golden.string() << '\n';
    } else {
        std::ifstream input(golden, std::ios::binary);
        std::string line;
        std::getline(input, line);
        std::vector<std::string> pinned;
        while (std::getline(input, line)) pinned.push_back(line);
        expect(pinned == reference.rows, "the battle matches tactical-combat.hashes.csv");
    }
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(trace(created(), executor, replay.final_tick_count, false).rows == reference.rows,
            "battle with " + std::to_string(workers) + " workers matches");
    }
    const eawr::platform::ThreadWorkerAdapter four(4);
    expect(trace(created(), four, replay.final_tick_count, true).rows == reference.rows,
        "battle with scrambled storage matches");
    auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "battle replay is written");
    if (written) {
        auto parsed = tactical::parse_replay(written.value());
        expect(static_cast<bool>(parsed), "battle replay parses");
        if (parsed) {
            auto again = tactical::TacticalSession::from_replay(parsed.value(), sensors(), {}, {}, std::nullopt, table());
            expect(static_cast<bool>(again)
                    && trace(std::move(again).value(), inline_executor, replay.final_tick_count, false).rows == reference.rows,
                "a written-and-parsed battle replay matches");
        }
    }
}

// WAD-39/40: assignment admission is separate from the later weapon fire cone and clock.
void test_manual_contracts() {
    namespace detail = tactical::detail;
    auto combat = table();
    auto& weapon = combat.profiles.front().weapons.front();
    weapon.requires_manual_target = true;
    weapon.pulse_count = 1;
    weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 100;
    weapon.manual_min_range = units(100);
    weapon.manual_cooldown_frames = 3600;
    weapon.shot.emplace();
    weapon.shot->speed = units(7); weapon.shot->damage = units(6000);
    weapon.shot->blast = {units(1000), units(300), 0, {}};
    weapon.shot->appearance_delay_frames = 15;
    const auto players = setup({}).players;
    const std::vector<tactical::SnapshotPlayer> relations{{1, 1}, {2, 2}};
    auto state = tactical::initial_combat(combat.profiles.front(), 12345, 0, 1);
    state.weapons.front().countdown = 0;
    detail::CombatWorld world;
    world.players = players; world.relationships = relations; world.table = &combat; world.seed = 12345;
    detail::CombatUnit own;
    own.id = 1; own.type_id = shooter_type; own.owner = 1; own.team = 1;
    own.transform = math::to_matrix(math::identity_quat(), {}).value();
    own.profile = &combat.profiles.front(); own.combat = &state; own.visible_to = 3;
    detail::CombatUnit enemy;
    enemy.id = 2; enemy.type_id = fighter_type; enemy.owner = 2; enemy.team = 2; enemy.player_index = 1;
    enemy.profile = &combat.profiles[1]; enemy.position = at(100, 0);
    enemy.transform = math::to_matrix(math::identity_quat(), enemy.position).value(); enemy.visible_to = 3;
    world.units = {own, enemy};
    const auto admitted = [&]() {
        auto result = detail::manual_target_admissible(world, own, enemy, 0);
        expect(static_cast<bool>(result), "MC-01: admission arithmetic succeeds");
        return result && result.value();
    };
    // Fire midpoint is x=5, so the inclusive minimum is the target at x=105.
    enemy.position = at(105, 0); expect(admitted(), "MC-01: inclusive minimum range");
    weapon.fire_in_normal_mode = false;
    own.rocket_attack = false;
    expect(!admitted(), "WRA-02: rocket-only manual gun refuses normal-mode assignment");
    own.rocket_attack = true;
    expect(admitted(), "WRA-02: rocket-only manual gun admits a matching mode");
    weapon.fire_in_normal_mode = true;
    weapon.fire_in_rocket_mode = false;
    own.rocket_attack = true;
    expect(!admitted(), "WRA-02: normal-only manual gun refuses rocket-mode assignment");
    own.rocket_attack = false;
    weapon.fire_in_rocket_mode = true;
    enemy.position = at(104, 0); expect(!admitted(), "MC-01: below minimum range is refused");
    enemy.position = at(705, 0); expect(admitted(), "MC-01: inclusive maximum range");
    enemy.position = at(706, 0); expect(!admitted(), "MC-01: beyond maximum range is refused");
    enemy.position = at(500, 0); enemy.visible_to = 2; expect(!admitted(), "MC-01: fogged target refused");
    enemy.visible_to = 3; enemy.owner = 1; expect(!admitted(), "MC-01: friendly target refused");
    enemy.owner = 2;
    weapon.category_restrictions = fighter_bit | bomber_bit | transport_bit;
    auto target_profile = combat.profiles[2];
    enemy.profile = &target_profile;
    expect(!admitted(), "MC-09/10: the picked bomber craft is refused by the manual cannon");
    target_profile.category_bits = 0;
    expect(admitted(), "MC-09: direct category-less container admission does not inspect its members");
    target_profile.category_bits = 8U; // a synthetic ship category outside the authored exclusions
    expect(admitted(), "MC-09: a target outside the gun's exclusion remains admitted");
    enemy.profile = &combat.profiles[1];
    weapon.category_restrictions = 0;
    state.weapons.front().manual->target = 2;
    expect(!admitted(), "MC-02: retained assignment cannot be replaced");
    state.weapons.front().manual->requesting_player = 1;
    weapon.category_restrictions = fighter_bit;
    world.units[1] = enemy; world.frame = 300;
    auto waiting = detail::step_combat(world, own);
    expect(waiting && waiting.value().state.weapons.front().manual->target == 2
        && waiting.value().manual_feedback.empty(), "MC-03: frame 300 still waits for eligible weapon state");
    world.frame = 301;
    auto timeout = detail::step_combat(world, own);
    expect(timeout && timeout.value().state.weapons.front().manual->target == 0
        && timeout.value().manual_feedback.size() == 1 && timeout.value().manual_feedback.front().player == 1,
        "MC-03: strict frame 301 clears target and addresses requesting player");
    weapon.category_restrictions = 0;
    weapon.fire_in_normal_mode = false;
    const auto mode_timeout = detail::step_combat(world, own);
    expect(mode_timeout && mode_timeout.value().state.weapons.front().manual->target == 0
        && mode_timeout.value().manual_feedback.size() == 1,
        "WRA-02: a later forbidden mode uses eligibility timeout instead of holding a live target forever");
    weapon.fire_in_normal_mode = true;
    world.units[1].position = at(900, 0);
    auto range_wait = detail::step_combat(world, own);
    expect(range_wait && range_wait.value().state.weapons.front().manual->target == 2
        && range_wait.value().manual_feedback.empty(), "MC-03: later range failure cannot create timeout");
    world.units.pop_back();
    state.weapons.front().countdown = 10;
    auto detached = detail::step_combat(world, own);
    expect(detached && detached.value().state.weapons.front().manual->target == 0
        && detached.value().manual_feedback.empty(), "MC-03: deleted target clears immediately without timeout");
    state.weapons.front().countdown = 0;
    enemy.previous_position = enemy.position;
    world.units.push_back(enemy);
    auto fired = detail::step_combat(world, own);
    expect(fired && fired.value().state.weapons.front().manual->target == 0
        && fired.value().state.weapons.front().countdown == 30
        && fired.value().manual_fired.size() == 1 && fired.value().manual_fired.front().cooldown_frames == 3600,
        "MC-04: successful shot clears assignment and separates ordinary and player recharge");
    const tactical::ManualPlayerClock clock{10, 3600};
    expect(tactical::manual_readiness(clock, 10).raw() == 0
        && tactical::manual_readiness(clock, 1810).raw() == one / 2
        && tactical::manual_readiness(clock, 3609).raw() < one
        && tactical::manual_readiness(clock, 3610).raw() == one,
        "MC-05: shared player readiness saturates at stored shot cooldown");
    // A turret admits by extent before it slews into the firing cone.
    tactical::ManualTurretProfile turret;
    turret.axes = {at(1, 0), at(0, 1), at(0, 0, 1)};
    turret.coordinate_axes = {at(0, 1), at(-1, 0), at(0, 0, 1)};
    turret.speed = Fixed::from_raw(one / 2); turret.yaw_extent = units(360); turret.pitch_extent = units(30);
    weapon.manual_turret = turret; weapon.manual_turret_required = true;
    state.weapons.front().manual->target = 0;
    enemy.position = at(0, 500, 500);
    expect(admitted(), "MC-06: turret yaw admission ignores its current cone and pitch");
    state.weapons.front().manual->target = 2; world.units[1] = enemy;
    auto slewed = detail::step_combat(world, own);
    expect(slewed && slewed.value().events.empty()
        && slewed.value().state.weapons.front().manual->yaw == units(1)
        && slewed.value().state.weapons.front().manual->pitch == Fixed::from_raw(-one / 2),
        "MC-06: authored half-degree pitch and both yaw steps precede fire-cone evaluation");
    // The attachment coordinates and zero-pose muzzle differ by a quarter turn.
    // A narrow yaw extent admits the forward target, then the posed gun launches.
    weapon.manual_turret->yaw_extent = units(20);
    enemy.position = at(500, 0); state.weapons.front().manual->target = 0;
    expect(admitted(), "WSW-18: quarter-turn yaw admission accepts the muzzle-forward target");
    enemy.position = at(0, 500);
    expect(!admitted(), "WSW-18: quarter-turn yaw admission rejects the side target beyond extent");
    enemy.position = enemy.previous_position = at(500, 0);
    enemy.transform = math::to_matrix(math::identity_quat(), enemy.position).value();
    world.units[1] = enemy; state.weapons.front().manual->target = 2;
    weapon.fire_axes = turret.axes; weapon.cone_width = units(40); weapon.cone_height = units(3);
    auto aligned = detail::step_combat(world, own);
    expect(aligned && aligned.value().manual_fired.size() == 1
        && aligned.value().state.weapons.front().manual->target == 0
        && aligned.value().state.weapons.front().manual->yaw == Fixed{},
        "WSW-18: nonaligned attachment coordinates preserve the muzzle cone and launch a manual shot");
    // WAD-40: a full off-axis request waits for the authored mechanical speed
    // and muzzle cone, independently of the viewer and the appearance delay.
    for (const auto speed : {Fixed::from_raw(one / 2), units(1)}) {
        weapon.manual_turret->speed = speed;
        weapon.manual_turret->yaw_extent = units(360);
        weapon.fire_a = weapon.fire_b = {};
        state = tactical::initial_combat(combat.profiles.front(), 12345, 0, 1);
        state.weapons.front().countdown = 0;
        state.weapons.front().manual->target = 2;
        enemy.position = enemy.previous_position = at(0, 500);
        enemy.transform = math::to_matrix(math::identity_quat(), enemy.position).value();
        world.units[1] = enemy;
        std::uint64_t shot_frame = 0;
        for (std::uint64_t frame = 1; frame <= 100; ++frame) {
            world.frame = frame;
            auto step = detail::step_combat(world, own);
            expect(static_cast<bool>(step), "WAD-40: turn-to-fire service succeeds");
            if (!step) break;
            const auto yaw = step.value().state.weapons.front().manual->yaw;
            expect(yaw.raw() == static_cast<std::int64_t>(frame) * speed.raw() * 2,
                "WAD-40: both authored yaw steps reach the snapshot each tick");
            if (!step.value().manual_fired.empty()) {
                shot_frame = frame;
                expect(yaw >= units(70) && yaw <= units(72),
                    "WAD-40: first shot waits for the 40-degree cone about the off-axis target");
                break;
            }
            expect(yaw < units(72), "WAD-40: an aligned manual gun does not keep waiting");
            state = std::move(step.value().state);
        }
        const auto expected = speed == units(1) ? 35U : 70U;
        expect(shot_frame >= expected && shot_frame <= expected + 1,
            "WAD-40: changing authored speed changes the first-shot tick proportionally");
    }
}

void test_planetary_purchase_and_burst() {
    auto combat = table();
    for (auto& profile : combat.profiles) profile.weapons.clear();
    auto& source = combat.profiles.back();
    source.special_weapon = true;
    source.max_attack_distance = Fixed{};
    source.planetary_weapon = tactical::PlanetaryWeaponProfile{0, 1, 8, 2, fighter_bit};
    auto gun = ion();
    gun.hardpoint = tactical::object_weapon;
    gun.range = Fixed{}; gun.fire_a = {}; gun.has_fire_b = false;
    gun.cone_width = units(360); gun.cone_height = units(180); gun.pulse_count = 3;
    gun.shot.emplace(); gun.shot->speed = units(7); gun.shot->max_travel = units(10000);
    source.weapons = {gun};
    const auto initial = setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, fighter_type, 2, at(5000, 0)), unit(3, gunship_type, 1, at(0, 0, -50))});
    constexpr tactical::TypeId unlock_type = 90;
    tactical::EconomyRules economy;
    economy.players = {{1, units(100), 20, false}, {2, units(100), 20, true}};
    tactical::BuildOption option{unlock_type, tactical::BuildKind::upgrade, tactical::BuildQueue::upgrades,
        units(1), 1, 1, 0, true};
    option.requirements.current_player = 1;
    economy.menus = {{shooter_type, 1, {option}}};
    tactical::UpgradeProfile unlock;
    unlock.type = unlock_type; unlock.special_unlocks = {{false, false}}; unlock.consume_after_special_fire = true;
    economy.upgrades = {unlock};
    tactical::TacticalReplay replay;
    replay.setup = initial; replay.final_tick_count = 20;
    replay.commands = {
        {{0, 1, 1}, {1}, tactical::BuyPayload{unlock_type}},
        {{2, 1, 2}, {3}, tactical::SpecialFirePayload{2}},
        {{7, 1, 3}, {1}, tactical::BuyPayload{unlock_type}},
        {{8, 1, 4}, {3}, tactical::SpecialFirePayload{2}}, // permission restored, recharge still incomplete
        {{11, 1, 5}, {3}, tactical::SpecialFirePayload{2}},
        {{12, 1, 6}, {3}, tactical::SpecialFirePayload{2, tactical::SpecialFireAdmission::ai_object}}};
    const auto encoded = tactical::write_replay(replay);
    expect(static_cast<bool>(encoded), "WSW: purchase/burst replay writes");
    const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : tactical::parse_replay({});
    expect(parsed && parsed.value() == replay, "WSW: purchase/burst replay round-trips");
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::from_replay(replay, sensors(1), {}, {}, std::nullopt, combat, {}, {}, economy);
        expect(static_cast<bool>(created), "WSW: planetary economy session creates");
        if (!created) return;
        auto world = std::move(created).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> pulses;
        std::vector<std::uint64_t> ready;
        for (std::uint64_t frame = 0; frame < replay.final_tick_count; ++frame) {
            const auto tick = world.step(executor);
            expect(static_cast<bool>(tick), "WSW: planetary purchase/burst tick succeeds");
            if (!tick) return;
            hashes.push_back(tick.value().state_sha256);
            const auto snapshot = tick.value().snapshot;
            for (const auto& event : snapshot->events()) {
                if (event.kind == tactical::EventKind::special_weapon_fired) pulses.emplace_back(event.tick, event.special_pulse);
                if (event.kind == tactical::EventKind::special_weapon_ready) ready.push_back(event.tick);
                if (frame == 8 && event.order == tactical::OrderKind::special_fire)
                    expect(event.kind == tactical::EventKind::order_rejected, "WSW-16: re-purchase does not bypass recharge");
                if (frame == 12 && event.order == tactical::OrderKind::special_fire)
                    expect(event.kind == tactical::EventKind::order_rejected, "WSW-40: a human cannot request AI admission");
            }
            if (frame == 2 || frame == 11) {
                expect(world.ledgers().front().completed.empty() && !snapshot->economy().front().special_weapons_enabled,
                    "WSW-08/11: initial dispatch consumes the marked object and its permission");
                expect(world.ledgers().front().lifetime.at(unlock_type) == (frame == 2 ? 1U : 2U),
                    "WSW-11: consumption retains lifetime production count");
            }
        }
        expect(pulses == std::vector<std::pair<std::uint64_t, std::uint32_t>>{{2, 0}, {4, 1}, {6, 2}, {11, 0}, {13, 1}, {15, 2}},
            "WSW-19/20: three pulses retain frame spacing after enabling purchase consumption");
        expect(ready == std::vector<std::uint64_t>{10}, "WSW-34: ready cue waits for recharge plus permission, fires once");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WSW: purchase/source/burst clocks match at workers 1/2/4/8");
    }
    for (const int scenario : {0, 1, 2, 3, 4, 5, 6}) {
        auto content = combat;
        auto money = economy;
        auto script = replay;
        script.commands.resize(2);
        script.final_tick_count = 12;
        tactical::DurabilityTable health;
        if (scenario == 0) { // WSW-22: cone rejection still advances logical fire and emits each pulse cue.
            // The shared object-gun predicate treats zero extents as unbounded.
            // A nonzero narrow extent rejects the target above the source plane.
            content.profiles.back().weapons.front().cone_width = Fixed::from_raw(one / 10);
            content.profiles.back().weapons.front().cone_height = Fixed::from_raw(one / 10);
        } else if (scenario == 1) { // WSW-08: a non-consuming unlock remains held across recharge.
            money.upgrades.front().consume_after_special_fire = false;
            script.commands.push_back({{11, 1, 3}, {3}, tactical::SpecialFirePayload{2}});
        } else if (scenario == 2) { // WSW-09: rejected helper still consumes an admitted player event.
            content.profiles.back().planetary_weapon->valid_targets = bomber_bit;
        } else if (scenario == 5 || scenario == 6) {
            script.setup.players[1].team_id = script.setup.players[0].team_id;
            if (scenario == 5) content.pad_neutral_factions = {script.setup.players[1].faction_id};
        } else {
            // WSW-48: a designated station hardpoint gates new dispatches, not
            // the continuation of an already-started planetary burst.
            tactical::DurabilityProfile station;
            station.type_id = shooter_type; station.max_hull = units(1000);
            tactical::HardpointProfile point;
            point.destroyable = true; point.max_health = units(50); point.allows_special_weapon_use = true;
            station.hardpoints = {point}; health.profiles = {station};
            money.upgrades.front().consume_after_special_fire = false;
            if (scenario == 3)
                script.commands.insert(script.commands.begin() + 1, {{1, 1, 2}, {1}, tactical::DamagePayload{units(50), 0}});
            else script.commands.push_back({{3, 1, 3}, {1}, tactical::DamagePayload{units(50), 0}});
            script.commands[scenario == 3 ? 2 : 1].key.sequence = 3;
            if (scenario == 4) script.commands.back().key.sequence = 4;
        }
        auto created = tactical::TacticalSession::from_replay(script, sensors(1), health, {}, std::nullopt, content, {}, {}, money);
        expect(static_cast<bool>(created), "WSW: wrapper/permission variant creates");
        if (!created) return;
        auto world = std::move(created).value();
        std::size_t cues = 0, projectiles = 0;
        const eawr::sim::InlineExecutor executor;
        while (world.completed_tick() < script.final_tick_count) {
            if (world.completed_tick() == 2) {
                const auto frozen = world.snapshot();
                const auto expected = scenario == 2 ? tactical::RejectReason::ability_unavailable
                    : scenario == 6 ? tactical::RejectReason::target_not_hostile : tactical::RejectReason::none;
                expect(tactical::planetary_special_fire_target_admission(*frozen, 3, 2) == expected,
                    "WSW-13/16: frozen planetary target gate matches command category/alliance admission, including neutrals");
                const auto picked = tactical::manual_special_fire_admission(*frozen, 3, 2);
                expect(picked && (expected == tactical::RejectReason::none
                        ? picked.value() == std::optional(tactical::attack_hull) : !picked.value()),
                    "WSW-13/16: frozen special admission and opcode 27 share the planetary target gate");
            }
            const auto stepped = world.step(executor);
            expect(static_cast<bool>(stepped), "WSW: wrapper/permission variant ticks");
            if (!stepped) return;
            for (const auto& event : stepped.value().snapshot->events())
                cues += event.kind == tactical::EventKind::special_weapon_fired ? 1U : 0U;
            for (const auto& event : stepped.value().snapshot->combat_events())
                projectiles += event.kind == tactical::CombatEventKind::weapon_fired ? 1U : 0U;
        }
        if (scenario == 0) expect(cues == 3 && projectiles == 0,
            "WSW-22/33: failed projectile construction keeps three wrapper pulse receipts");
        if (scenario == 1) expect(cues == 4 && world.ledgers().front().completed.size() == 1
            && world.snapshot()->economy().front().special_weapons_enabled,
            "WSW-08: persistent permission admits another shot after recharge without another purchase");
        if (scenario == 2) expect(cues == 0 && world.ledgers().front().completed.empty()
            && world.snapshot()->instances().back().planetary_weapon->state.recharge_until == 0,
            "WSW-09/16: invalid category consumes player permission without starting a burst or recharge");
        if (scenario == 3 || scenario == 4) expect(cues == (scenario == 3 ? 0U : 3U)
            && world.ledgers().front().completed.size() == 1 && !world.snapshot()->economy().front().special_weapons_enabled,
            "WSW-48: destroyed required hardpoint blocks admission and ready cues while an active burst keeps its pulses");
        if (scenario == 5 || scenario == 6) expect(cues == (scenario == 5 ? 3U : 0U),
            "WSW-13/16: authoritative planetary command agrees with the frozen gate for neutral and allied targets");
    }
}

void test_special_stealth_construction() {
    for (const bool planetary : {false, true}) for (const bool counter : {false, true}) {
        auto combat = table();
        for (auto& profile : combat.profiles) profile.weapons.clear();
        auto& source = combat.profiles.front();
        source.target_stealth = counter;
        auto gun = ion();
        gun.min_recharge_hundredths = gun.max_recharge_hundredths = 0;
        gun.shot.emplace(); gun.shot->speed = units(7); gun.shot->max_travel = units(10000);
        gun.cone_width = units(360); gun.cone_height = units(180);
        if (planetary) {
            source.special_weapon = true;
            source.planetary_weapon = tactical::PlanetaryWeaponProfile{0, 1, 8, 2, fighter_bit};
            gun.hardpoint = tactical::object_weapon; gun.pulse_count = 3; gun.range = {};
        } else {
            source.star_base = true;
            gun.requires_manual_target = gun.special = true; gun.pulse_count = 1;
        }
        source.weapons = {gun};
        tactical::AbilityProfile cloak;
        cloak.kind = tactical::AbilityKind::stealth; cloak.expiration_frames = 200; cloak.recharge_frames = 90;
        cloak.gui_activated_ability_name = "Special_Stealth_Test";
        tactical::SpecialAbilityProfile handler;
        handler.name = cloak.gui_activated_ability_name; handler.kind = tactical::SpecialAbilityKind::stealth;
        handler.style = tactical::SpecialActivationStyle::user_input;
        handler.service_interval = tactical::special_service_interval(handler.kind);
        handler.stealth_transition = Fixed::from_raw(one / 2);
        tactical::UnitAbilityProfile cloaked;
        cloaked.type_id = fighter_type; cloaked.abilities = {cloak}; cloaked.special = {handler};
        tactical::AbilityTable abilities; abilities.profiles = {cloaked};
        tactical::EconomyRules economy;
        economy.players = {{1, units(100), 20, false}, {2, units(100), 20, false}};
        tactical::BuildOption option{90, tactical::BuildKind::upgrade, tactical::BuildQueue::upgrades,
            units(1), 1, 1, 0, true};
        economy.menus = {{shooter_type, 1, {option}}};
        tactical::UpgradeProfile unlock; unlock.type = 90; unlock.special_unlocks = {{false, false}};
        economy.upgrades = {unlock};
        tactical::TacticalReplay replay;
        replay.setup = setup({unit(1, shooter_type, 1, at(0, 0)),
            unit(2, fighter_type, 2, at(planetary ? 5000 : 500, 0))});
        replay.final_tick_count = 48;
        replay.commands = {{{0, 1, 1}, {1}, tactical::BuyPayload{90}},
            {{0, 2, 1}, {2}, tactical::AbilityPayload{tactical::AbilityKind::stealth, tactical::AbilityAction::activate}},
            {{40, 1, 2}, {1}, tactical::SpecialFirePayload{2}}};
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto created = tactical::TacticalSession::from_replay(replay, planetary ? sensors(1) : sensors(),
                {}, {}, std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "WSW-22/WST-11: cloaked special-target fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; return; }
            auto world = std::move(created).value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::size_t cues = 0, shots = 0, accepted = 0;
            while (world.completed_tick() < replay.final_tick_count) {
                if (world.completed_tick() == 40) {
                    const auto frozen = world.snapshot();
                    expect(frozen->instances().back().stealth_invisible && frozen->instances().front().target_stealth == counter,
                        "WSW-22/WST-07: frozen admission retains target cloak and shooter counter capability");
                    const auto picked = tactical::manual_special_fire_admission(*frozen, 1, 2);
                    expect(picked && picked.value().has_value() == (planetary || counter),
                        "WSW-14/22: planetary target admission precedes stealth construction; manual admission rejects uncountered cloak");
                }
                const auto tick = world.step(executor);
                expect(static_cast<bool>(tick), "WSW-22/WST-11: cloaked special-target tick succeeds");
                if (!tick) return;
                hashes.push_back(tick.value().state_sha256 + tick.value().snapshot->sha256());
                for (const auto& event : tick.value().snapshot->events()) {
                    cues += event.kind == tactical::EventKind::special_weapon_fired ? 1U : 0U;
                    accepted += event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::special_fire ? 1U : 0U;
                }
                for (const auto& event : tick.value().snapshot->combat_events())
                    shots += event.kind == tactical::CombatEventKind::weapon_fired ? 1U : 0U;
            }
            expect(accepted == (planetary || counter ? 1U : 0U) && cues == (planetary ? 3U : 0U)
                && shots == (counter ? planetary ? 3U : 1U : 0U),
                "WSW-22/33: uncountered cloak rejects construction; planetary pulses still report, counters fire through planetary fog");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WSW-22/WST-11: special stealth construction matches workers 1/2/4/8");
        }
    }
}

void test_special_frozen_target_radius() {
    auto combat = table();
    for (auto& profile : combat.profiles) profile.weapons.clear();
    auto gun = ion();
    gun.requires_manual_target = true; gun.special = true; gun.pulse_count = 1;
    gun.min_recharge_hundredths = gun.max_recharge_hundredths = 0;
    gun.fire_a = {}; gun.has_fire_b = false;
    gun.shot.emplace(); gun.shot->speed = units(7);
    combat.profiles.front().weapons = {gun};
    combat.footprints = {{shooter_type, units(35), units(10), units(10)},
        {fighter_type, units(100), units(10), units(10)}};
    const auto initial = setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, fighter_type, 2, at(750, 0))});
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto value = session(initial, combat); // Neither object has a motion profile.
        const auto frozen = value.snapshot();
        expect(frozen->instances().front().soft_radius == units(35)
            && frozen->instances().back().soft_radius == units(100),
            "WWP-21/WSW-40: frozen stationary objects retain loaded combat footprints");
        const auto admitted = tactical::manual_special_fire_admission(*frozen, 1, 2);
        expect(admitted && admitted.value() == std::optional<std::uint32_t>{0},
            "WWP-20/WSW-40: frozen manual admission includes the loaded stationary target radius");
        if (workers == 1) {
            auto source = frozen->instances().front();
            auto craft = frozen->instances().back();
            craft.durability.emplace();
            craft.durability->hull = units(100);
            const auto hovered = tactical::manual_special_fire_admission(frozen->players(), source, craft, false);
            expect(hovered && hovered.value().has_value(), "MC-10: a live craft is valid at hover");
            craft.durability->hull = {};
            const auto clicked = tactical::manual_special_fire_admission(frozen->players(), source, craft, false);
            expect(clicked && !clicked.value(), "MC-10: a craft destroyed between hover and click is refused");
            craft.entity_id = 3;
            craft.durability->hull = units(100);
            craft.category_bits = bomber_bit;
            auto exclusions = std::make_shared<tactical::WeaponProfile>(*source.manual_weapons.front().admission_profile);
            exclusions->category_restrictions = bomber_bit;
            source.manual_weapons.front().admission_profile = exclusions;
            const auto sibling = tactical::manual_special_fire_admission(frozen->players(), source, craft, false);
            expect(sibling && !sibling.value(), "MC-09/10: a living bomber sibling remains excluded after the hovered craft dies");
        }
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::ManualTargetPayload{2, 0}})),
            "stationary-radius manual request submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::size_t shots = 0;
        for (int tick = 0; tick < 3; ++tick) {
            const auto step = value.step(executor);
            expect(static_cast<bool>(step), "stationary-radius manual request steps");
            if (!step) return;
            hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
            for (const auto& event : step.value().snapshot->combat_events())
                shots += event.kind == tactical::CombatEventKind::weapon_fired ? 1U : 0U;
        }
        expect(shots == 1, "WWP-20/WSW-40: authoritative manual fire agrees with frozen loaded-radius admission");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "stationary-radius admission matches on 1/2/4/8 workers");
    }
}

void test_planetary_projectile_target_radius() {
    for (const bool melee : {false, true}) for (const eawr::sim::EntityId target : {2U, 3U}) {
        auto combat = table();
        for (auto& profile : combat.profiles) profile.weapons.clear();
        combat.melee_cutoff = units(50);
        combat.profiles[3].category_bits = fighter_bit;
        combat.footprints = {{fighter_type, units(7), units(3), units(4)},
            {transport_type, units(11), units(1), units(1)}};
        auto& source = combat.profiles.back();
        source.special_weapon = true; source.max_attack_distance = units(melee ? 0 : 1000);
        source.planetary_weapon = tactical::PlanetaryWeaponProfile{0, 1, 8, 2, fighter_bit};
        auto gun = ion(); gun.hardpoint = tactical::object_weapon; gun.range = {};
        gun.fire_a = {}; gun.has_fire_b = false; gun.pulse_count = 1;
        gun.cone_width = units(360); gun.cone_height = units(180);
        gun.shot.emplace(); gun.shot->speed = units(7); gun.shot->max_travel = units(1000);
        source.weapons = {gun};
        tactical::DurabilityTable health; health.damage.emplace(); health.damage->shield_recharge_frames = 1;
        tactical::EconomyRules economy;
        economy.players = {{1, units(100), 20, true}, {2, units(100), 20, false}};
        tactical::BuildOption option{90, tactical::BuildKind::upgrade, tactical::BuildQueue::upgrades,
            units(1), 1, 1, 0, true};
        economy.menus = {{gunship_type, 1, {option}}};
        tactical::UpgradeProfile unlock; unlock.type = 90; unlock.special_unlocks = {{false, false}};
        economy.upgrades = {unlock};
        tactical::TacticalReplay replay;
        replay.setup = setup({unit(1, gunship_type, 1, at(0, 0)),
            unit(2, fighter_type, 2, at(500, 0)), unit(3, transport_type, 2, at(500, 0))});
        replay.setup.squadrons = {{3, {2}}}; replay.final_tick_count = 4;
        replay.commands = {{{0, 1, 1}, {1}, tactical::BuyPayload{90}},
            {{2, 1, 2}, {1}, tactical::SpecialFirePayload{target}}};
        const auto allowance = target == 3 ? 11 : melee ? 7 : 18;
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto created = tactical::TacticalSession::from_replay(replay, sensors(), health, {},
                std::nullopt, combat, {}, {}, economy);
            expect(static_cast<bool>(created), "planetary craft/team travel fixture creates");
            if (!created) return;
            auto value = std::move(created).value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            while (value.completed_tick() < replay.final_tick_count) {
                const auto step = value.step(executor);
                expect(static_cast<bool>(step), "planetary craft/team travel fixture steps");
                if (!step) return;
                hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
            }
            expect(value.projectiles().size() == 1 && value.projectiles().front().target == target
                && value.projectiles().front().max_travel == units(1000 + allowance),
                "WWP-27/WSW-18: planetary travel uses shared craft/team radius and zero-distance melee classification");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "planetary craft/team travel matches on 1/2/4/8 workers");
        }
    }
}

void test_special_manual_routes() {
    auto combat = table();
    for (auto& profile : combat.profiles) profile.weapons.clear();
    auto& station = combat.profiles.front();
    station.star_base = true;
    auto gun = ion(); gun.requires_manual_target = true; gun.special = true;
    gun.pulse_count = 1; gun.manual_cooldown_frames = 90;
    gun.shot.emplace(); gun.shot->speed = units(7);
    station.weapons = {gun};
    tactical::EconomyRules economy;
    economy.players = {{1, units(100), 20, true}, {2, units(100), 20, false}};
    tactical::BuildOption option{90, tactical::BuildKind::upgrade, tactical::BuildQueue::upgrades, units(1), 1, 1, 0, true};
    economy.menus = {{shooter_type, 1, {option}}};
    tactical::UpgradeProfile unlock; unlock.type = 90; unlock.special_unlocks = {{false, false}};
    economy.upgrades = {unlock};
    tactical::TacticalReplay replay;
    replay.setup = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(500, 0)),
        unit(3, shooter_type, 1, at(0, 20))});
    // W-03: let both guns finish the ordinary keyed spawn countdown (at most
    // 105 frames) before testing player versus gun cooldown admission.
    replay.final_tick_count = 118;
    replay.commands = {{{0, 1, 1}, {1}, tactical::BuyPayload{90}},
        {{110, 1, 2}, {1}, tactical::SpecialFirePayload{2}},
        {{112, 1, 3}, {3}, tactical::SpecialFirePayload{2}},
        {{112, 1, 4}, {1}, tactical::SpecialFirePayload{2, tactical::SpecialFireAdmission::ai_object}},
        {{112, 1, 5}, {3}, tactical::SpecialFirePayload{2, tactical::SpecialFireAdmission::ai_object}}};
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::from_replay(replay, sensors(), {}, {}, std::nullopt, combat, {}, {}, economy);
        expect(static_cast<bool>(created), "WSW-40: typed manual route fixture creates");
        if (!created) return;
        auto world = std::move(created).value();
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::size_t accepted = 0, rejected = 0, shots = 0;
        while (world.completed_tick() < replay.final_tick_count) {
            const auto tick = world.step(executor);
            expect(static_cast<bool>(tick), "WSW-40: typed manual route steps");
            if (!tick) return;
            hashes.push_back(tick.value().state_sha256 + tick.value().snapshot->sha256());
            for (const auto& event : tick.value().snapshot->events()) if (event.order == tactical::OrderKind::special_fire) {
                accepted += event.kind == tactical::EventKind::order_accepted ? 1U : 0U;
                rejected += event.kind == tactical::EventKind::order_rejected ? 1U : 0U;
            }
            for (const auto& event : tick.value().snapshot->combat_events()) shots += event.kind == tactical::CombatEventKind::weapon_fired ? 1U : 0U;
        }
        expect(accepted == 2 && rejected == 2 && shots == 2 && world.ledgers().front().completed.size() == 1,
            "WSW-08/40: permanent unlock persists; player clock blocks another station, AI requires its gun but bypasses player clock");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WSW-40: typed manual AI/player routes match at workers 1/2/4/8");
    }
}

void test_manual_session_and_replay() {
    auto combat = table();
    auto& weapon = combat.profiles.front().weapons.front();
    weapon.requires_manual_target = true; weapon.pulse_count = 1;
    weapon.min_recharge_hundredths = weapon.max_recharge_hundredths = 0;
    weapon.manual_cooldown_frames = 90;
    weapon.shot.emplace(); weapon.shot->speed = units(7);
    const auto initial = setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, fighter_type, 2, at(500, 0)), unit(3, shooter_type, 1, at(0, 20))});
    const std::vector<tactical::PlayerCommand> commands{
        {{0, 1, 1}, {1}, tactical::ManualTargetPayload{2, 0}},
        {{2, 1, 2}, {3}, tactical::ManualTargetPayload{2, 0}},
        {{91, 1, 3}, {3}, tactical::ManualTargetPayload{2, 0}}};
    const auto drive = [&](tactical::TacticalSession value, const eawr::sim::PartitionExecutor& executor) {
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 94; ++tick) {
            auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "MC-09: manual session step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().snapshot->sha256());
            // Existing commands commit after targeting: requests at 0/91 fire at 1/92.
            if (tick == 1 || tick == 92) {
                const auto clocks = stepped.value().snapshot->manual_clocks();
                expect(clocks.size() == 1 && clocks.front().first == 1
                    && clocks.front().second.last_fired_frame == tick && clocks.front().second.cooldown_frames == 90,
                    "MC-09: either station records the same player's clock only on successful firing");
            }
            if (tick == 2) {
                expect(std::any_of(stepped.value().snapshot->events().begin(), stepped.value().snapshot->events().end(),
                    [](const tactical::Event& event) { return event.kind == tactical::EventKind::order_rejected
                        && event.order == tactical::OrderKind::manual_target && event.unit == 3; }),
                    "MC-09: another station is refused until the shared player clock is ready");
                const auto admitted = tactical::manual_special_fire_admission(*stepped.value().snapshot, 3, 2);
                expect(admitted && admitted.value() == std::optional<std::uint32_t>{0},
                    "WSW-40: frozen manual admission uses the free gun despite the player's HUD cooldown");
            }
        }
        return hashes;
    };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto value = session(initial, combat);
        const auto admitted = tactical::manual_special_fire_admission(*value.snapshot(), 1, 2);
        expect(admitted && admitted.value() == std::optional<std::uint32_t>{0},
            "WSW-40: public frozen-view admission matches a ready manual gun");
        for (const auto& command : commands) expect(static_cast<bool>(value.submit(command)), "MC-09: manual command queues");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        auto hashes = drive(std::move(value), executor);
        if (reference.empty()) reference = hashes;
        expect(hashes == reference, "MC-09: manual state and player clocks match workers 1/2/4/8");
    }
    tactical::TacticalReplay replay;
    replay.setup = initial; replay.commands = commands; replay.final_tick_count = 94;
    auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "MC-10: manual opcode replay writes");
    if (!written) return;
    auto parsed = tactical::parse_replay(written.value());
    expect(parsed && parsed.value().commands == commands, "MC-10: opcode 18 round trips its source hardpoint and enemy");
    if (parsed) {
        auto restored = tactical::TacticalSession::from_replay(parsed.value(), sensors(), {}, {}, std::nullopt, combat);
        expect(static_cast<bool>(restored), "MC-10: manual replay session restores");
        const eawr::sim::InlineExecutor executor;
        if (restored) expect(drive(std::move(restored).value(), executor) == reference, "MC-10: written replay preserves every manual snapshot hash");
    }
    // Fixed header plus player and unit records precedes the command length and body.
    const auto first = 104 + initial.players.size() * 24 + initial.units.size() * 80 + 4;
    auto malformed = written.value();
    malformed[first + 24 + 8 + 4] = 1;
    expect(!tactical::parse_replay(malformed), "MC-10: nonzero reserved manual payload rejected");
    malformed = written.value();
    std::fill_n(malformed.begin() + static_cast<std::ptrdiff_t>(first + 24), 8, std::uint8_t{0});
    expect(!tactical::parse_replay(malformed), "MC-10: null manual enemy rejected");
    auto value = session(initial, combat);
    expect(!value.submit({{0, 1, 4}, {1}, tactical::ManualTargetPayload{2, 255}}),
        "MC-10: invalid source hardpoint rejected before queue mutation");
}

void test_manual_rocket_mode() {
    for (const bool rocket_only : {false, true}) {
        auto combat = table();
        for (auto& profile : combat.profiles) profile.weapons.clear();
        auto gun = ion();
        gun.requires_manual_target = true; gun.pulse_count = 1;
        gun.min_recharge_hundredths = gun.max_recharge_hundredths = 0;
        gun.manual_cooldown_frames = 0;
        gun.fire_in_normal_mode = !rocket_only; gun.fire_in_rocket_mode = rocket_only;
        gun.shot.emplace(); gun.shot->speed = units(7); gun.shot->max_travel = units(1000);
        combat.profiles.front().weapons = {gun};
        const auto initial = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(500, 0))});
        tactical::AbilityProfile rocket; rocket.kind = tactical::AbilityKind::rocket_attack;
        tactical::UnitAbilityProfile holder; holder.type_id = shooter_type; holder.abilities = {rocket};
        tactical::AbilityTable abilities; abilities.profiles = {holder};
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto created = tactical::TacticalSession::create(initial, sensors(), {}, {}, std::nullopt, combat, {}, abilities);
            expect(static_cast<bool>(created), "WRA-02: manual modal holder session creates");
            if (!created) continue;
            auto value = std::move(created).value();
            if (!rocket_only) expect(static_cast<bool>(value.submit({{0, 1, 0}, {1},
                tactical::AbilityPayload{tactical::AbilityKind::rocket_attack, tactical::AbilityAction::activate}})),
                "WRA-02: normal-only gun starts in forbidden rocket mode");
            expect(static_cast<bool>(value.submit({{1, 1, 1}, {1}, tactical::ManualTargetPayload{2, 0}})),
                "WRA-02: opposite-mode manual command queues for admission");
            expect(static_cast<bool>(value.submit({{2, 1, 2}, {1}, tactical::AbilityPayload{
                tactical::AbilityKind::rocket_attack,
                rocket_only ? tactical::AbilityAction::activate : tactical::AbilityAction::deactivate}})),
                "WRA-02: matching manual mode switch queues");
            expect(static_cast<bool>(value.submit({{3, 1, 3}, {1}, tactical::ManualTargetPayload{2, 0}})),
                "WRA-02: matching-mode manual command queues");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::size_t rejected{}, accepted{}, shots{};
            for (std::uint64_t tick = 0; tick < 7; ++tick) {
                const auto step = value.step(executor);
                expect(static_cast<bool>(step), "WRA-02: modal manual step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
                if (tick <= 2) {
                    const auto frozen = value.snapshot();
                    const auto source = std::find_if(frozen->instances().begin(), frozen->instances().end(),
                        [](const tactical::TacticalInstance& instance) { return instance.entity_id == 1; });
                    const auto admission = tactical::manual_special_fire_admission(*frozen, 1, 2);
                    expect(source != frozen->instances().end() && !source->manual_weapons.empty(),
                        "WRA-02: frozen mode publishes the manual gun");
                    if (source == frozen->instances().end() || source->manual_weapons.empty()) break;
                    expect(source->rocket_attack == (tick < 2 ? !rocket_only : rocket_only),
                        "WRA-02: frozen manual admission carries the current rocket mode");
                    expect(admission && admission.value().has_value() == (tick == 2)
                        && source->manual_weapons.front().available == (tick == 2),
                        "WRA-02: frozen manual admission and HUD availability share the mode gate");
                    expect(source->manual_weapons.front().target == eawr::sim::invalid_entity_id,
                        "WRA-02: forbidden assignment is never retained or released by a later mode switch");
                }
                for (const auto& event : step.value().snapshot->events()) {
                    if (event.order != tactical::OrderKind::manual_target) continue;
                    rejected += event.kind == tactical::EventKind::order_rejected ? 1U : 0U;
                    accepted += event.kind == tactical::EventKind::order_accepted ? 1U : 0U;
                }
                for (const auto& event : step.value().snapshot->combat_events())
                    shots += event.shooter == 1 && event.kind == tactical::CombatEventKind::weapon_fired ? 1U : 0U;
            }
            expect(rejected == 1 && accepted == 1 && shots == 1,
                "WRA-02: opposite mode rejects without a retained request; matching mode fires once");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WRA-02: direct/frozen modal manual paths agree on 1/2/4/8 workers");
        }
    }
}

void test_validation() {
    // Range scalars are independent of the bounded geometry/step domain.
    for (int field = 0; field < 4; ++field) {
        auto ranged = table();
        auto& profile = ranged.profiles.front();
        auto& weapon = profile.weapons.front();
        weapon.shot = tactical::ShotProfile{units(1), 0, units(25), units(700), true, true, {}};
        const auto set_range = [&](const Fixed value) {
            if (field == 0) profile.max_attack_distance = value;
            else if (field == 1) weapon.range = value;
            else if (field == 2) weapon.shot->max_travel = value;
            else { weapon.range = units(1000000000); weapon.manual_min_range = value; }
        };
        set_range(units(999999999));
        expect(static_cast<bool>(tactical::validate_combat(ranged)), "large finite range scalars are admitted independently");
        if (field != 3) {
            set_range(Fixed::from_raw(std::numeric_limits<std::int64_t>::max()));
            expect(static_cast<bool>(tactical::validate_combat(ranged)), "range admission covers the nonnegative Q24 domain");
        }
        set_range(Fixed::from_raw(-1));
        expect(!tactical::validate_combat(ranged), "negative range scalars remain invalid");
    }
    auto geometry = table();
    geometry.profiles.front().weapons.front().fire_a.x = units(tactical::max_combat_distance + 1);
    expect(!tactical::validate_combat(geometry), "large ranges do not relax the fire-point geometry bound");
    auto bad = table();
    bad.profiles[0].weapons[0].pulse_count = 0;
    expect(!tactical::validate_combat(bad), "a zero pulse count is rejected");
    bad = table();
    std::swap(bad.profiles[0], bad.profiles[1]);
    expect(!tactical::validate_combat(bad), "unordered type IDs are rejected");
    bad = table();
    bad.profiles[0].priority_set = 7;
    expect(!tactical::validate_combat(bad), "an unknown priority set is rejected");
    bad = table();
    bad.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(5, 0), at(0, 1), at(0, 0)};
    expect(!tactical::validate_combat(bad), "fire bone axes longer than unit length are rejected");
    bad.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(2, 0), at(0, 1), at(0, 0, 1)};
    expect(!tactical::validate_combat(bad), "a fire bone x axis of length 2 is rejected");
    const auto skewed_y = math::normalize(at(1, 16));
    expect(static_cast<bool>(skewed_y), "the skewed axis normalizes");
    if (skewed_y) {
        bad.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), skewed_y.value(), at(0, 0, 1)};
        expect(static_cast<bool>(tactical::validate_combat(bad)),
            "W-07: nonorthogonal unit fire axes retain their authored cone projections");
    }
    auto rounded = table();
    rounded.profiles[0].weapons[0].fire_axes =
        std::array<math::Vec3, 3>{math::Vec3{units(1), Fixed::from_raw(3), Fixed{}}, at(0, 1), at(0, 0, 1)};
    expect(static_cast<bool>(tactical::validate_combat(rounded)), "a frame within Q24 rounding of orthonormal is valid");
    rounded.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 0, -1), at(0, 1)};
    expect(static_cast<bool>(tactical::validate_combat(rounded)), "the gunboat-like frame is valid");
    expect(static_cast<bool>(tactical::validate_combat(table())), "the test table is valid");
    // Keyed draws depend only on their key and index.
    tactical::CombatRandom first(1, 2, 3, 4);
    tactical::CombatRandom second(1, 2, 3, 4);
    tactical::CombatRandom other(1, 2, 3, 5);
    const auto a = first.uniform(0, 1000000);
    expect(a == second.uniform(0, 1000000) && a != other.uniform(0, 1000000), "keyed draws repeat per key");
    expect(first.uniform(7, 7) == 7 && first.draws() == 2, "a one-value draw still counts");
}

void test_range_query_endpoints() {
    using namespace tactical::detail;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int64_t>::min();
    // Keep indexed body geometry ordinary; only the query endpoints approach int64 limits.
    const std::vector<tactical::SpaceBody> bodies{{1, 2, at(0, 0)}, {2, 2, at(10, 0)}, {3, 1, at(0, 0)}};
    CombatWorld world;
    world.index = tactical::SpaceIndex::build(bodies).value();
    CollectionTrees collection;
    collection.update({{1, 2, {at(0, 0), at(0, 0)}}, {2, 2, {at(10, 0), at(10, 0)}},
        {3, 1, {at(0, 0), at(0, 0)}}}, 0);
    for (const auto centre_raw : {minimum + 5, maximum - 5}) {
        const math::Vec3 centre{Fixed::from_raw(centre_raw), {}, {}};
        const auto half = Fixed::from_raw(maximum);
        auto indexed = world.candidates(centre, half, 2);
        world.collection = &collection;
        auto collected = world.candidates(centre, half, 2);
        world.collection = nullptr;
        std::sort(collected.begin(), collected.end());
        const std::vector<eawr::sim::EntityId> expected = centre_raw < 0
            ? std::vector<eawr::sim::EntityId>{1} : std::vector<eawr::sim::EntityId>{1, 2};
        expect(indexed == expected && collected == expected, "range queries clip endpoints without wrap, preserving owner membership");
    }
    world.collection = &collection;
    expect(world.candidates(at(0, 0), Fixed::from_raw(-1), 2).empty(), "negative query extents remain empty");
}
} // namespace

using namespace combat_test_support;

void test_membership_index() {
    tactical::detail::CombatWorld world;
    world.teams = {{10, {11, 12}}, {20, {21}}};
    const std::map<eawr::sim::EntityId, eawr::sim::EntityId> indexed{{11, 10}, {12, 10}};
    std::vector<eawr::sim::EntityId> expected;
    for (const eawr::sim::EntityId id : {10U, 11U, 12U, 20U, 21U, 99U}) expected.push_back(world.container_of(id));
    world.craft_containers = &indexed;
    std::size_t slot=0;
    for (const eawr::sim::EntityId id : {10U, 11U, 12U, 20U, 21U, 99U}) {
        expect(world.container_of(id) == expected[slot++],
            "WSQ-47: indexed flight members and unindexed teams retain the same container normalization");
    }
}

void test_squadron_object_weapon_reference() {
    tactical::detail::CombatWorld world;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    world.players = owners;
    world.relationships = relationships;
    tactical::CombatProfile profile;
    auto weapon = ion();
    weapon.hardpoint = tactical::object_weapon;
    weapon.cone_width = weapon.cone_height = units(360);
    weapon.has_fire_b = false;
    profile.weapons = {weapon};
    // Target craft can have targetable hardpoints: a team-targeted object shot still aims at hull.
    profile.hardpoints = {{0, at(0, 0), true}};
    tactical::CombatState state;
    state.attack_target = 20;
    state.direct = true;
    state.attack_hardpoint = 0;
    state.weapons.resize(1);
    state.weapons.front().pulses_left = 2;
    tactical::detail::CombatUnit self;
    self.id = 11;
    self.owner = 1;
    self.team = 1;
    self.profile = &profile;
    self.combat = &state;
    self.transform = math::identity_matrix();
    auto parent = self;
    parent.id = 15;
    parent.position = at(0, 200);
    parent.combat = nullptr;
    tactical::detail::CombatUnit team;
    team.id = 20;
    team.owner = 2;
    team.team = 2;
    team.visible_to = 1;
    auto near_shooter = team;
    near_shooter.id = 21;
    near_shooter.position = at(200, 0);
    near_shooter.profile = &profile;
    near_shooter.transform = math::to_matrix(math::identity_quat(), near_shooter.position).value();
    auto near_centre = near_shooter;
    near_centre.id = 22;
    near_centre.position = at(200, 200);
    near_centre.transform = math::to_matrix(math::identity_quat(), near_centre.position).value();
    world.units = {self, parent, team, near_shooter, near_centre};
    world.teams = {{15, {11}}, {20, {21, 22}}};
    const auto fired = [&](const eawr::sim::EntityId expected, const bool hull) {
        const auto step = tactical::detail::step_combat(world, world.units.front());
        expect(static_cast<bool>(step), "WWP-49: object team-target firing succeeds");
        if (!step) return;
        expect(step.value().events.size() == 1 && step.value().events.front().target == expected,
            "WWP-49: the reference point chooses the expected live enemy craft");
        if (hull && !step.value().events.empty()) expect(step.value().events.front().target_hardpoint == tactical::no_hardpoint,
            "WWP-49: a team-targeted object weapon clears the aimed hardpoint");
    };
    fired(21, true); // A parent with target metadata but no ordinary targeting uses the muzzle.
    world.units[1].combat = &state;
    fired(22, true); // An ordinary targeting parent supplies the reference even with no bound muzzle.
    profile.weapons.front().muzzles = {at(0, 200)};
    world.units[1].combat = nullptr;
    fired(22, true); // The actual muzzle, rather than the shooter centre, selects the other craft.
    profile.weapons.front().muzzles = {at(0, 0)};
    fired(21, true);
    world.units[1].combat = &state;
    fired(22, true); // Ordinary parent targeting overrides the actual muzzle.
    world.units.erase(world.units.begin() + 1);
    fired(21, true); // A missing parent falls back to the muzzle too.
    world.units.insert(world.units.begin() + 1, parent);
    world.units[1].combat = &state;
    profile.weapons.front().muzzles.clear();
    profile.weapons.front().hardpoint = 0;
    {
        // WWP-19 (#711): a hardpoint ordered at the team fires at a random craft each attempt, by
        // a keyed draw (seed, frame, unit, slot), aimed by the R-10 search on that craft.
        std::set<eawr::sim::EntityId> hit;
        bool repeatable = true;
        for (std::uint64_t frame = 1; frame <= 32; ++frame) {
            world.frame = frame;
            const auto step = tactical::detail::step_combat(world, world.units.front());
            const auto again = tactical::detail::step_combat(world, world.units.front());
            expect(static_cast<bool>(step) && static_cast<bool>(again), "WWP-19: hardpoint team-target firing succeeds");
            if (!step || !again) return;
            expect(step.value().events.size() == 1, "WWP-19: the hardpoint fires one shot at the team");
            if (step.value().events.size() != 1 || again.value().events.size() != 1) return;
            const auto& event = step.value().events.front();
            hit.insert(event.target);
            repeatable = repeatable && again.value().events.front().target == event.target;
        }
        expect(hit == std::set<eawr::sim::EntityId>{21, 22},
            "WWP-19: a hardpoint's shots spread over the team's craft, not only the nearest");
        expect(repeatable, "WWP-19: the craft draw is keyed, so the same frame picks the same craft");
        world.frame = 0;
    }
    profile.weapons.front().hardpoint = tactical::object_weapon;
    world.units.pop_back();
    fired(21, true); // The selected craft dies; the held team still resolves a live survivor.
    world.units.back().profile = nullptr;
    const auto profileless = tactical::detail::step_combat(world, world.units.front());
    expect(static_cast<bool>(profileless), "WWP-49: a profile-less team member is handled safely");
    if (profileless) expect(profileless.value().events.empty(),
        "WWP-49: a profile-less team member does not admit an object shot");
}

void test_member_held_identity() {
    tactical::detail::CombatWorld world;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    world.players = owners;
    world.relationships = relationships;
    tactical::CombatProfile scanner;
    scanner.max_attack_distance = units(500);
    scanner.weapons.emplace_back();
    tactical::CombatState state;
    tactical::detail::CombatUnit self;
    self.id = 11;
    self.owner = 1;
    self.team = 1;
    self.profile = &scanner;
    self.combat = &state;
    tactical::detail::CombatUnit target;
    target.id = 21;
    target.owner = 2;
    target.team = 2;
    target.position = at(600, 600);
    target.visible_to = 1;
    target.profile = &scanner;
    world.units = {self, target};
    world.teams = {{20, {21}}};
    const std::array<tactical::SpaceBody, 1> candidates{{{21, 2, target.position}}};
    auto indexed = tactical::SpaceIndex::build(candidates);
    expect(static_cast<bool>(indexed), "WSQ-45: held-identity candidates build");
    if (!indexed) return;
    world.index = std::move(indexed).value();
    const auto scan = [&](const eawr::sim::EntityId held, const math::Vec3 anchor) {
        return tactical::detail::target_combat(world, world.units.front(), units(200), held, anchor);
    };
    expect(scan(21, at(0, 0)).attack_target == 21,
        "WSQ-45: the exact held formation target is admitted beyond attack and diversion reach");
    expect(scan(20, at(0, 0)).attack_target == eawr::sim::invalid_entity_id,
        "WSQ-45: membership of the held target's team does not bypass distance admission");
    expect(scan(20, at(300, 300)).attack_target == 21,
        "WSQ-45: a member of the held target's team can still qualify through destination diversion");

    auto team = target;
    team.id = 20;
    team.position = at(1000, 1000);
    auto alternative = target;
    alternative.id = 30;
    alternative.position = at(300, 0);
    world.units = {self, team, target, alternative};
    const std::array<tactical::SpaceBody, 2> replacements{{{21, 2, target.position}, {30, 2, alternative.position}}};
    auto replacement_index = tactical::SpaceIndex::build(replacements);
    expect(static_cast<bool>(replacement_index), "WSQ-45: held-team replacement candidates build");
    if (!replacement_index) return;
    world.index = std::move(replacement_index).value();
    state.attack_target = 20;
    expect(scan(20, at(0, 0)).attack_target == 20,
        "WSQ-45: replacement evaluates the held team rather than its out-of-reach nearest craft");
    const auto* aimed = world.resolve_target(world.find(20), self.position);
    expect(aimed != nullptr && aimed->id == 21,
        "FO-04: holding a team still resolves its nearest live craft for weapon aim");

    state.attack_target = eawr::sim::invalid_entity_id;
    tactical::detail::FormationAttackContext formation{true, false, false, &world.units.front()};
    world.units.back().position = at(600, 600);
    const std::array<tactical::SpaceBody, 1> diagonal{{{30, 2, world.units.back().position}}};
    auto diagonal_index = tactical::SpaceIndex::build(diagonal);
    expect(static_cast<bool>(diagonal_index), "WMV-17: diagonal diversion candidate builds");
    if (!diagonal_index) return;
    world.index = std::move(diagonal_index).value();
    const auto diverted = [&](const math::Vec3 anchor) {
        return tactical::detail::target_combat(world, world.units.front(), units(200), 0, anchor, &formation);
    };
    expect(diverted(at(300, 300)).attack_target == 0,
        "WMV-18: a formation that refuses diversion contributes no scan allowance");
    formation.allows_divert = true;
    expect(diverted(at(0, 0)).attack_target == 0,
        "WMV-17: the collection box alone does not admit an out-of-leash diagonal target");
    expect(diverted(at(300, 300)).attack_target == 30,
        "WMV-17: a target attackable from the allowance circle is admitted");
    auto short_profile = scanner;
    short_profile.max_attack_distance = units(100);
    auto short_leader = self;
    short_leader.profile = &short_profile;
    formation.leader = &short_leader;
    expect(diverted(at(300, 300)).attack_target == 0,
        "WMV-17: the circle reach check uses the leader's attack distance for a mixed-type team");
    formation.present = false;
    expect(diverted(at(300, 300)).attack_target == 0,
        "WMV-18: an absent formation has neither diversion admission nor scan extension");
}

#include "retaliation_cases.hpp"

void test_squadron_adoption_and_retention() {
    namespace d = tactical::detail;
    d::CombatWorld world;
    tactical::CombatProfile profile;
    profile.max_attack_distance = units(500);
    d::CombatUnit self;
    self.id = 11;
    self.position = at(500, 0);
    self.profile = &profile;
    d::CombatUnit target;
    target.id = 20;
    target.position = at(900, 0);
    target.profile = &profile;
    world.units = {self, target};
    tactical::CombatState held;
    held.attack_target = 20;
    held.attack_hardpoint = 1;
    d::FormationAttackContext formation{true, true, false, &self};
    const auto retained = [&](const eawr::sim::EntityId formation_target, const bool same_cell) {
        auto result = d::finish_squadron_target(world, self, held, formation, formation_target, same_cell);
        expect(static_cast<bool>(result), "WDF-02: retention arithmetic succeeds");
        return result ? result.value().attack_target : eawr::sim::invalid_entity_id;
    };
    auto admitted = d::formation_can_attack(world, world.units.back(), units(200), at(0, 0), formation);
    expect(admitted && !admitted.value(),
        "WDF-03: a target in a member's reach but beyond the formation's allowance is not adopted");
    expect(retained(0, false) == 20, "WDF-02: a refused find in firing reach remains held by the member");
    world.units.back().position = at(1100, 0);
    expect(retained(0, false) == 0, "WDF-02: an unadopted out-of-reach target is cleared this frame");
    auto cleared = d::finish_squadron_target(world, self, held, formation, 0, false);
    expect(cleared && cleared.value().attack_hardpoint == tactical::no_hardpoint,
        "WDF-02: clearing also discards the old hardpoint");
    expect(retained(20, false) == 20, "WDF-02: the formation's target remains held outside firing reach");
    expect(retained(0, true) == 20, "WDF-02: the same recorded combat cell keeps a target outside firing reach");
    formation.present = false;
    const auto without_formation = d::finish_squadron_target(world, self, held, formation, 0, false);
    expect(without_formation && without_formation.value().attack_target == 20
        && without_formation.value().attack_hardpoint == 1,
        "WDF-02: an absent formation retains an out-of-reach target and hardpoint");
    formation.present = true;
    held.direct = true;
    expect(retained(0, false) == 20, "WDF-02: a player-direct target remains held outside firing reach");
    held.direct = false;
    world.units.back().position = at(700, 0);
    admitted = d::formation_can_attack(world, world.units.back(), units(200), at(0, 0), formation);
    expect(admitted && admitted.value(), "WDF-03: the allowance-circle attack reach includes its boundary");
    world.units.back().position.x = math::Fixed::from_raw(units(700).raw() + 1);
    admitted = d::formation_can_attack(world, world.units.back(), units(200), at(0, 0), formation);
    expect(admitted && !admitted.value(), "WDF-03: one raw unit beyond the allowance-circle reach is refused");
    formation.allows_divert = false;
    admitted = d::formation_can_attack(world, world.units.back(), units(200), at(500, 0), formation);
    expect(admitted && !admitted.value(), "WDF-03: a formation that cannot divert refuses even a close target");
}

int main(int argc, char** argv) {
    if (!eawr::testing::support_contract_self_test()) {
        std::cerr << "FAIL: shared check/require self-test\n";
        return 1;
    }
    if (argc < 3) {
        std::cerr << "usage: combat_tests <fixtures directory> <space-targeting-cases.json> [--update]\n";
        return 2;
    }
    const bool update = argc > 3 && std::string_view(argv[3]) == "--update";
    retaliation_cases::gates();
    retaliation_cases::damage_path();
    test_planetary_purchase_and_burst();
    test_special_stealth_construction();
    test_special_frozen_target_radius();
    test_planetary_projectile_target_radius();
    test_special_manual_routes();
    test_manual_session_and_replay();
    test_manual_contracts();
    test_manual_rocket_mode();
    test_validation();
    test_range_query_endpoints();
    test_membership_index();
    test_member_held_identity();
    test_squadron_adoption_and_retention();
    test_squadron_object_weapon_reference();
    test_note_cases(argv[2]);
    test_priority_beats_distance();
    test_fire_reveal();
    test_retention_and_replacement();
    test_fire_cycle();
    test_attack_order();
    test_neutral_relationships();
    test_ship_level_choice();
    test_ship_level_suitability();
        test_displayed_health_damaged();
    test_target_stickiness();
    test_target_stickiness_session();
    test_restrictions_and_fog();
    test_noncollidable_opportunity_target();
    test_special_weapon_service();
    test_charged_blast_budget();
    test_ai_opportunity_cutoff();
    test_zero_cone();
    test_fire_bone_cone();
    test_randomized_fire_bones();
    test_launcher_cone();
    test_object_turret();
    test_hardpoint_turret();
    test_fire_bone_pole();
    test_object_weapon_cone();
    test_unit_weapon_targeting_and_muzzles();
    test_rocket_launch_axis();
    test_lead();
    test_history_checkpoint();
    test_order_without_attack_distance();
    test_attack_turn();
    test_attack_turn_edges();
    test_best_hardpoint();
    test_orders_approach();
    test_orders_attack_move();
    test_orders_guard();
    test_orders_workers_and_replay();
    test_orders_phase();
    test_orders_lost_target();
    test_hardpoint_orders();
    test_hardpoint_orders_replay();
    test_battle(argv[1], update);
    if (failures != 0) {
        std::cerr << failures << " combat contract test(s) failed\n";
        return 1;
    }
    std::cout << "combat contracts passed\n";
    return 0;
}
