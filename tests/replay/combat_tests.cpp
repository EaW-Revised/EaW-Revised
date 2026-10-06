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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-10 (#73): target choice and weapon fire (docs/behaviour/space-targeting.md,
// docs/behaviour/space-weapon-fire.md). The targeting note's cases run through the opportunity
// service; the session cases use a TIE Defender-like shooter; a combat battle's hashes are pinned
// in tactical-combat.hashes.csv and must be reproduced by every worker count, a scrambled storage
// order and a written-and-parsed replay. `combat_tests <fixtures> <cases.json> --update` rewrites the pin.
namespace combat_test_support {

using sim_headless::Json;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
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
    enemy.position = at(104, 0); expect(!admitted(), "MC-01: below minimum range is refused");
    enemy.position = at(705, 0); expect(admitted(), "MC-01: inclusive maximum range");
    enemy.position = at(706, 0); expect(!admitted(), "MC-01: beyond maximum range is refused");
    enemy.position = at(500, 0); enemy.visible_to = 2; expect(!admitted(), "MC-01: fogged target refused");
    enemy.visible_to = 3; enemy.owner = 1; expect(!admitted(), "MC-01: friendly target refused");
    enemy.owner = 2;
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
    turret.axes = turret.coordinate_axes = {at(1, 0), at(0, 1), at(0, 0, 1)};
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
            }
        }
        return hashes;
    };
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto value = session(initial, combat);
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

void test_validation() {
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
        expect(!tactical::validate_combat(bad), "a skewed fire bone frame of unit axes is rejected");
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
    self.squadron_centre = at(0, 200);
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
    world.units = {self, team, near_shooter, near_centre};
    world.teams = {{20, {21, 22}}};
    const auto fired = [&](const eawr::sim::EntityId expected, const bool hull) {
        const auto step = tactical::detail::step_combat(world, world.units.front());
        expect(static_cast<bool>(step), "WWP-49: object team-target firing succeeds");
        if (!step) return;
        expect(step.value().events.size() == 1 && step.value().events.front().target == expected,
            "WWP-49: the reference point chooses the expected live enemy craft");
        if (hull && !step.value().events.empty()) expect(step.value().events.front().target_hardpoint == tactical::no_hardpoint,
            "WWP-49: a team-targeted object weapon clears the aimed hardpoint");
    };
    fired(22, true);
    world.units.front().squadron_centre.reset();
    fired(21, true); // No squadron centre: retain the existing ungrouped reference.
    world.units.front().squadron_centre = at(0, 200);
    profile.weapons.front().hardpoint = 0;
    fired(21, false); // WWP-19 is a separate route; this change does not alter it.
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

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: combat_tests <fixtures directory> <space-targeting-cases.json> [--update]\n";
        return 2;
    }
    const bool update = argc > 3 && std::string_view(argv[3]) == "--update";
    test_manual_session_and_replay();
    test_manual_contracts();
    test_validation();
    test_membership_index();
    test_member_held_identity();
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
    test_restrictions_and_fog();
    test_noncollidable_opportunity_target();
    test_special_weapon_service();
    test_zero_cone();
    test_fire_bone_cone();
    test_launcher_cone();
    test_fire_bone_pole();
    test_object_weapon_cone();
    test_lead();
    test_order_without_attack_distance();
    test_attack_turn();
    test_attack_turn_edges();
    test_orders_approach();
    test_orders_attack_move();
    test_orders_guard();
    test_orders_workers_and_replay();
    test_orders_phase();
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
