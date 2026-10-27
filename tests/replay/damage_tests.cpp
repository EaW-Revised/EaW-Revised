// P2-11 (#74): projectiles, damage types against armor, shield absorption and regeneration
// (docs/behaviour/space-damage.md). The pure rules are checked against FoC values; the session
// fixtures cover shield loss, a hit destroying a hardpoint and an out-of-range miss; a battle's
// hashes are pinned in tactical-damage.hashes.csv and must be reproduced by every worker count,
// a scrambled storage order and a written-and-parsed replay. `damage_tests <fixtures> --update`
// rewrites the pin.

#include "../support/check.hpp"
#include "damage_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace damage_test_support {


eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}


[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {units(x), units(y), units(z)};
}
[[nodiscard]] bool close_to(const Fixed value, const double expected, const double tolerance) {
    return std::abs(static_cast<double>(value.raw()) / static_cast<double>(one) - expected) <= tolerance;
}

// --- Rules -------------------------------------------------------------------------------------

// FoC's gameconstants.xml: ShieldRechargeIntervalInSecs 3.0, Depleted_Shield_Disable_Time 5.0,
// Depleted_Shield_Damage_Increment 0, Depleted_Shield_Regen_Cap 0.25, Diminishing_Firepower
// 0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1, EnergyRechargeIntervalInSecs 5.0 (150 frames),
// EnergyToShieldExchangeRate 5.0. Damage types: 0 Damage_Turbolaser, 1 Damage_Laser; armor
// types: 0 Armor_Frigate, 1 Shield_Frigate (Turbolaser: 3 and 2).
[[nodiscard]] tactical::DamageRules rules() {
    tactical::DamageRules result;
    result.shield_recharge_frames = 90;
    result.depleted_disable_seconds = units(5);
    result.depleted_increment_seconds = Fixed{};
    result.depleted_regen_cap = decimal("0.25");
    result.diminishing = {{units(0), decimal("0.6")}, {decimal("0.3"), decimal("0.7")}, {decimal("0.9"), decimal("0.9")},
        {units(1), units(1)}, {units(2), units(1)}};
    result.damage_types = 2;
    result.armor_types = 2;
    result.armor_mods = {units(3), units(2), units(1), units(1)};
    result.energy_recharge_frames = 150;
    result.energy_to_shield = units(5);
    return result;
}


[[nodiscard]] tactical::DurabilityProfile frigate() {
    tactical::DurabilityProfile profile;
    profile.type_id = frigate_type;
    profile.max_hull = units(600);
    profile.hardpoints = {{tactical::HardpointRole::weapon, true, units(90), {}, {}}};
    profile.max_shields = units(100);
    profile.shield_refresh = units(10);
    profile.armor_type = 0;
    profile.shield_armor_type = 1;
    return profile;
}

void test_hostile_projectile_filter() {
    // WHZ-51, WWP-66: permissive collision data cannot make neutral scenery absorb a volley.
    tactical::CombatProfile profile;
    profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}, {3, 3, true}, {4, 1, false}};
    tactical::detail::CombatWorld world;
    world.relationships = relationships;
    std::vector<tactical::SpaceBody> bodies;
    for (std::uint32_t index = 0; index < 128; ++index) {
        tactical::detail::CombatUnit object;
        object.id = index + 1;
        object.owner = index == 127 ? 2U : (index == 126 ? 4U : 3U);
        object.team = object.owner == 4 ? 1U : object.owner;
        object.position = at(static_cast<std::int64_t>(index + 1), 0);
        object.transform = math::to_matrix(math::identity_quat(), object.position).value();
        object.profile = &profile;
        world.units.push_back(object);
        bodies.push_back({object.id, object.owner, object.position});
    }
    world.index = tactical::SpaceIndex::build(bodies).value();
    tactical::detail::CollectionTrees collidables;
    const auto update_collidables = [&] {
        std::vector<tactical::detail::CollectionTrees::Member> members;
        for (const auto& object : world.units) {
            members.push_back({object.id, object.owner,
                {at(static_cast<std::int64_t>(object.id) - 10, -10, -10),
                    at(static_cast<std::int64_t>(object.id) + 10, 10, 10)}});
        }
        collidables.update(members, 0);
    };
    update_collidables();
    world.projectile_collection = &collidables;
    tactical::Projectile projectile;
    projectile.owner = 1;
    projectile.step = at(200, 0);
    projectile.max_travel = units(400);
    tactical::detail::ProjectileScratch scratch;
    const auto hit = tactical::detail::step_projectile(world, projectile, scratch);
    expect(hit && hit.value().hit == 128 && scratch.exact_count == 1,
        "WWP-66: 126 neutral boxes and an allied box are passed; only the hostile box reaches exact collision");
    relationships[1].neutral = true;
    scratch = {};
    const auto passing = tactical::detail::step_projectile(world, projectile, scratch);
    expect(passing && !passing.value().hit && scratch.exact_count == 0,
        "WHZ-51: an all-neutral volley performs no exact collision work");
    relationships[1].neutral = false;
    profile.living_projectile_collision = false;
    scratch = {};
    const auto denied = tactical::detail::step_projectile(world, projectile, scratch);
    expect(denied && !denied.value().hit && scratch.exact_count == 0,
        "WWP-66: the hostile filter preserves the existing living projectile gate");
    profile.living_projectile_collision = true;
    world.units[0].owner = 2;
    world.units[0].team = 2;
    update_collidables();
    scratch = {};
    const auto captured = tactical::detail::step_projectile(world, projectile, scratch);
    expect(captured && captured.value().hit == 1 && scratch.exact_count == 1,
        "WHZ-51: a captured enemy-owned box becomes a normal projectile contact");
}

// --- Sessions -----------------------------------------------------------------------------------

// A HP_Nebulon_Weapon-like laser: range 700, 175 x 160 cone, 5 shots 6 frames apart, recharge 3 to
// 4 s; its projectile: 10 damage of type 0, 25 units per frame.
[[nodiscard]] tactical::WeaponProfile laser(const std::int64_t travel, const std::uint32_t hardpoint) {
    tactical::WeaponProfile weapon;
    weapon.hardpoint = hardpoint;
    weapon.range = units(700);
    weapon.min_recharge_hundredths = 300;
    weapon.max_recharge_hundredths = 400;
    weapon.pulse_count = 5;
    weapon.pulse_delay_frames = 6;
    weapon.cone_width = units(175);
    weapon.cone_height = units(160);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(travel), true, true, {}};
    return weapon;
}

[[nodiscard]] tactical::CombatTable combat(const std::int64_t travel) {
    tactical::CombatTable table;
    const tactical::CollisionBox hull_box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::CombatProfile shooter;
    shooter.type_id = shooter_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(700);
    shooter.weapons = {laser(travel)};
    shooter.collision = hull_box;
    tactical::CombatProfile ship;
    ship.type_id = frigate_type;
    ship.category_bits = 2;
    ship.hardpoints = {{0, at(15, 0), true}};
    ship.collision = hull_box;
    tactical::CombatProfile corvette = ship;
    corvette.type_id = corvette_type;
    tactical::CombatProfile station = ship;
    station.type_id = station_type;
    station.hardpoints.clear();
    table.profiles = {shooter, ship, corvette, station};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    table.damage = rules();
    tactical::DurabilityProfile shooter;
    shooter.type_id = shooter_type;
    shooter.max_hull = units(2000);
    shooter.armor_type = 0;
    auto corvette = frigate();
    corvette.type_id = corvette_type;
    corvette.max_hull = units(300);
    corvette.max_shields = Fixed{};
    corvette.shield_refresh = Fixed{};
    auto station = frigate();
    station.type_id = station_type;
    station.hardpoints.clear();
    // EN-04: the frigate pays for its shield from a pool that can run dry in the battle.
    auto ship = frigate();
    ship.powered = true;
    ship.max_energy = units(120);
    ship.energy_refresh = units(20);
    table.profiles = {shooter, ship, corvette, station};
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    std::vector<tactical::SensorProfile> result;
    for (tactical::TypeId type = 1; type <= 4; ++type) result.push_back({type, units(3000)});
    return result;
}

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

[[nodiscard]] tactical::TacticalSetup setup(std::vector<tactical::UnitState> list) {
    tactical::TacticalSetup result;
    result.seed = 7401;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = std::move(list);
    return result;
}

[[nodiscard]] tactical::TacticalSession session(const tactical::TacticalSetup& value, const std::int64_t travel) {
    auto created = tactical::TacticalSession::create(value, sensors(), durability(), {}, std::nullopt, combat(travel));
    expect(static_cast<bool>(created), "damage session is created");
    return std::move(created).value();
}

Tally run(tactical::TacticalSession& value, const std::uint64_t ticks) {
    Tally tally;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "damage step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            tally.shots += event.kind == tactical::CombatEventKind::weapon_fired;
            tally.hits += event.kind == tactical::CombatEventKind::projectile_hit;
            tally.absorbed += event.kind == tactical::CombatEventKind::projectile_hit
                && (event.outcome & tactical::hit_outcome_shield_absorbed) != 0U;
        }
        const auto events = stepped.value().snapshot->events();
        tally.events.insert(tally.events.end(), events.begin(), events.end());
    }
    return tally;
}


[[nodiscard]] tactical::TacticalReplay battle() {
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> list;
    eawr::sim::EntityId id = 1;
    for (std::int64_t row = 0; row < 5; ++row) {
        for (std::int64_t column = 0; column < 3; ++column) {
            list.push_back(unit(id++, shooter_type, 1, at(-250 - column * 60, row * 70 - 140)));
            const auto type = static_cast<tactical::TypeId>(2 + (row + column) % 3);
            list.push_back(unit(id++, (row + column) % 4 == 0 ? shooter_type : type, 2, at(250 + column * 60, row * 70 - 140), true));
        }
    }
    replay.setup = setup(list);
    replay.commands.push_back({{30, 1, 0}, {1, 3}, tactical::AttackPayload{4}});
    replay.commands.push_back({{60, 2, 0}, {6}, tactical::DamagePayload{units(150), tactical::hull_target}});
    replay.commands.push_back({{90, 1, 1}, {1}, tactical::StopPayload{}});
    replay.final_tick_count = 240;
    return replay;
}

// The battle's shooters also carry a missile battery (MS-01 to MS-05) on a second hardpoint, so
// homing flight runs under every worker count.
[[nodiscard]] tactical::CombatTable battle_combat() {
    auto table = combat();
    auto launcher = missile_combat(true).profiles[0].weapons.front();
    launcher.hardpoint = 1;
    launcher.fire_a = at(-10, 5, 3);
    table.profiles[0].weapons.push_back(launcher);
    return table;
}

struct Trace {
    std::vector<std::string> rows; // tick,state,snapshot
    std::size_t hits{};
    std::size_t deaths{};
    std::uint64_t candidates{}; // #636: the projectile broad phase's work
    std::uint64_t exact_tests{};
    bool projectile_phase{};
    bool projectiles_published{true};
};

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
        names.emplace_back(phase);
        return inner_.execute_phase(phase, partitions, work);
    }
    mutable std::vector<std::string> names;

private:
    const eawr::sim::PartitionExecutor& inner_;
};

[[nodiscard]] Trace trace(tactical::TacticalSession value, const eawr::sim::PartitionExecutor& executor,
    const std::uint64_t ticks, const bool scramble) {
    Trace result;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (scramble) value.scramble_storage_for_testing();
        const PhaseProbe probe(executor);
        const bool flying = !value.projectiles().empty();
        auto stepped = value.step(probe);
        expect(static_cast<bool>(stepped), "battle step succeeds");
        if (!stepped) break;
        // Flight and hull service precede targeting; ordinary systems follow firing.
        if (flying) {
            const auto find = [&](const std::string_view name) {
                return static_cast<std::size_t>(std::find(probe.names.begin(), probe.names.end(), name) - probe.names.begin());
            };
            result.projectile_phase = true;
            expect(find("projectiles") < find("hull-service")
                    && find("hull-service") < find("combat-input-refresh")
                    && find("combat-input-refresh") < find("targeting")
                    && find("targeting") < find("unit-systems")
                    && find("unit-systems") < probe.names.size(),
                "tick " + std::to_string(tick) + ": projectiles and hull service run before targeting and unit-systems");
        }
        result.rows.push_back(std::to_string(stepped.value().completed_tick) + ',' + stepped.value().state_sha256 + ','
            + stepped.value().snapshot->sha256());
        result.candidates += stepped.value().projectile_candidates;
        result.exact_tests += stepped.value().projectile_exact_tests;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.hits += event.kind == tactical::CombatEventKind::projectile_hit;
        }
        // #80: the snapshot publishes the projectiles in flight after the tick.
        const auto published = stepped.value().snapshot->projectiles();
        const auto flying_now = value.projectiles();
        result.projectiles_published = result.projectiles_published
            && std::equal(published.begin(), published.end(), flying_now.begin(), flying_now.end());
        for (const auto& event : stepped.value().snapshot->events()) {
            result.deaths += event.kind == tactical::EventKind::unit_destroyed;
        }
    }
    return result;
}

// MD-03, MD-04: bypass applies independently of armor, shield level and aim route.
void test_mass_driver_damage() {
    for (const auto multiplier : {"1", "1.25", "1.5", "2"}) {
        auto value = rules();
        value.armor_mods[0] = decimal(multiplier);
        for (const auto shields : {0, 1, 100}) {
            for (const auto target : {tactical::hull_target, 0U}) {
                const auto profile = frigate();
                auto state = tactical::full_durability(profile);
                state.shields = units(shields);
                state.energy = units(120);
                const auto before = state;
                tactical::Hit hit{units(10), 0, true, false, true, target};
                const auto result = tactical::apply_hit(profile, value, state, hit, 10);
                expect(result && result.value().absorbed.raw() == 0 && result.value().drained.raw() == 0
                        && !result.value().shield_absorbed && !result.value().shields_depleted
                        && state.shields == before.shields && state.energy == before.energy
                        && state.depleted_frame == before.depleted_frame,
                    "MD-03: mass driver leaves shields and energy intact");
                const auto expected = math::multiply(units(10), decimal(multiplier)).value();
                expect(target == tactical::hull_target ? state.hull.raw() == before.hull.raw() - expected.raw()
                                                          && state.hardpoints == before.hardpoints
                                                      : state.hardpoints[0].raw() == before.hardpoints[0].raw() - expected.raw()
                                                          && state.hull == before.hull,
                    "MD-04: hull armor scales bypass damage at its aimed destination");
                const auto second = tactical::apply_hit(profile, value, state, hit, 10);
                expect(second && (target == tactical::hull_target
                            ? state.hull.raw() == before.hull.raw() - expected.raw()
                                  - math::multiply(expected, decimal("0.6")).value().raw()
                            : state.hardpoints[0].raw() == before.hardpoints[0].raw() - expected.raw()
                                  - math::multiply(expected, decimal("0.6")).value().raw()),
                    "MD-03: bypass still obeys diminishing firepower");
            }
        }
    }
}

void test_mass_driver_flight() {
    const auto make = [] {
        auto table = combat();
        auto& weapon = table.profiles[0].weapons[0];
        weapon.shot->shield_damage = false;
        weapon.shot->speed = units(70);
        weapon.shot->damage = units(10);
        weapon.pulse_count = 8;
        weapon.pulse_delay_frames = 6;
        weapon.min_recharge_hundredths = 300;
        weapon.max_recharge_hundredths = 400;
        // A shield plate alone would catch a laser. A bypass round must reach the hull
        // behind it, then take the explicit aimed hardpoint route (DG-38, DG-39).
        auto& target = table.profiles[1];
        target.meshes = {tactical::collision_mesh(plate(20, 9), tactical::no_hardpoint, tactical::no_hardpoint, true),
            tactical::collision_mesh(plate(10, 9), tactical::no_hardpoint, tactical::no_hardpoint, false)};
        target.mesh_bounds = tactical::CollisionBox{at(10, -9, -9), at(20, 9, 9)};
        target.aimed_routes = {0};
        auto health = durability();
        health.profiles[1].shield_refresh = {};
        health.damage->armor_mods[0] = decimal("1.25");
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(300, 0), true)}),
            sensors(), health, {}, std::nullopt, table);
        expect(static_cast<bool>(created), "MD-02: mass-driver session binds");
        return std::move(created).value();
    };
    auto observed = make();
    const auto tally = run(observed, 110);
    const auto health = observed.durability_state(2);
    expect(tally.hits > 0 && tally.absorbed == 0 && health && health->shields == units(100)
            && health->hardpoints[0] < units(90),
        "MD-03: round crosses the live shield mesh and damages the aimed hardpoint");
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = trace(make(), inline_executor, 300, false);
    expect(reference.hits > 0 && reference.projectile_phase, "MD-02: projectiles hit in the partitioned phase");
    for (std::size_t workers = 1; workers <= 8; workers *= 2) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto parallel = trace(make(), executor, 300, true);
        expect(parallel.rows == reference.rows && parallel.candidates == reference.candidates
                && parallel.exact_tests == reference.exact_tests,
            "MD-02: mass-driver state, snapshots and work match with " + std::to_string(workers) + " workers");
    }
}

void test_battle(const std::filesystem::path& fixtures, const bool update) {
    const auto replay = battle();
    const auto golden = fixtures / "tactical-damage.hashes.csv";
    const auto created = [&] {
        auto value = tactical::TacticalSession::from_replay(replay, sensors(), durability(), {}, std::nullopt, battle_combat());
        expect(static_cast<bool>(value), "battle session is created");
        return std::move(value).value();
    };
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = trace(created(), inline_executor, replay.final_tick_count, false);
    expect(reference.hits > 50 && reference.deaths > 0 && reference.projectile_phase, "the battle hits and kills");
    expect(reference.projectiles_published, "#80: every snapshot publishes the projectiles in flight");
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
        expect(pinned == reference.rows, "the battle matches tactical-damage.hashes.csv");
    }
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto traced = trace(created(), executor, replay.final_tick_count, false);
        expect(traced.rows == reference.rows, "battle with " + std::to_string(workers) + " workers matches");
        expect(traced.candidates == reference.candidates && traced.exact_tests == reference.exact_tests
                && reference.exact_tests > 0 && reference.exact_tests <= reference.candidates,
            "#636: the broad phase's work counts match with " + std::to_string(workers) + " workers");
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
            auto again = tactical::TacticalSession::from_replay(parsed.value(), sensors(), durability(), {}, std::nullopt, battle_combat());
            expect(static_cast<bool>(again)
                    && trace(std::move(again).value(), inline_executor, replay.final_tick_count, false).rows == reference.rows,
                "a written-and-parsed battle replay matches");
        }
    }
}

void test_degraded_ion_storm() {
    auto table = durability();
    table.damage->ion_storm_disable_seconds = units(4000);
    eawr::core::LoadNoteBuffer buffer;
    const auto normalized = tactical::prepare_durability(table, buffer);
    eawr::core::LoadNoteCollector collector; collector.merge(buffer);
    const auto notes = collector.snapshot();
    expect(normalized && normalized.value().damage->ion_storm_disable_seconds == units(3600), "ion-storm window clamps to one hour");
    expect(std::any_of(notes.begin(), notes.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0110" && note.tag == "Ion_Storm_Shield_Disable_Time"
            && note.classification == eawr::core::LoadCategory::unknown_degrade
            && note.authored_value == "4000" && note.effective_value == "3600";
    }), "ion-storm clamp records a class C note");
    eawr::core::LoadNoteBuffer session_notes;
    auto created = tactical::TacticalSession::create(setup({}), sensors(), table, {}, std::nullopt, combat(1), {}, {}, {}, {}, &session_notes);
    eawr::core::LoadNoteCollector session_collector; session_collector.merge(session_notes);
    const auto captured = session_collector.snapshot();
    expect(std::any_of(captured.begin(), captured.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0110" && note.diagnostic_code == "EAWR-SIM-0305";
    }), "session creation publishes the original diagnostic in its note");
    expect(static_cast<bool>(created), "normalized damage session creates");
    if (created) {
        const eawr::sim::InlineExecutor executor;
        expect(static_cast<bool>(created.value().step(executor)), "normalized damage session steps");
    }
}

void test_degraded_damage_shapes() {
    auto table = durability();
    auto& damage = *table.damage;
    damage.diminishing = {{units(2), units(1)}, {units(1), units(0)}, {units(1), units(2)}};
    damage.armor_types = tactical::max_damage_types + 1;
    eawr::core::LoadNoteBuffer buffer;
    tactical::normalize_durability(table, buffer);
    eawr::core::LoadNoteCollector collector; collector.merge(buffer);
    const auto notes = collector.snapshot();
    expect(damage.diminishing.size() == 2 && damage.diminishing[0].x == units(1)
        && damage.diminishing[0].y == units(2), "curve sorts and retains the last duplicate");
    expect(damage.armor_types == tactical::max_damage_types && damage.armor_mods.size()
        == static_cast<std::size_t>(damage.damage_types) * damage.armor_types, "armor dimensions are bounded and complete");
    expect(std::any_of(notes.begin(), notes.end(), [](const auto& n) { return n.code == "EAWR-LOAD-0112"; })
        && std::any_of(notes.begin(), notes.end(), [](const auto& n) { return n.code == "EAWR-LOAD-0113"; }),
        "curve and armor normalization record notes");
    expect(static_cast<bool>(tactical::validate_durability(table)), "normalized curve and armor validate");
    auto session = tactical::TacticalSession::create(setup({}), sensors(), table, {}, std::nullopt, combat(1));
    expect(static_cast<bool>(session), "normalized curve and armor session creates");
    if (session) {
        const eawr::sim::InlineExecutor executor;
        expect(static_cast<bool>(session.value().step(executor)), "normalized curve and armor session steps");
    }
}

void test_duplicate_durability_ids() {
    auto table = durability();
    table.profiles.push_back(table.profiles.front());
    table.difficulty = {{2, units(1), units(1), units(1)}, {1, units(1), units(1), units(1)},
        {1, units(2), units(1), units(1)}};
    eawr::core::LoadNoteBuffer notes;
    tactical::normalize_durability(table, notes);
    eawr::core::LoadNoteCollector collector;
    collector.merge(notes);
    const auto captured = collector.snapshot();
    expect(table.profiles.size() == 4 && table.difficulty.size() == 2
        && table.difficulty.front().health == units(1), "duplicate IDs retain the first row");
    expect(std::any_of(captured.begin(), captured.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0104";
    }) && std::any_of(captured.begin(), captured.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0105";
    }), "duplicate ID normalization records notes");
    expect(static_cast<bool>(tactical::validate_durability(table)), "deduplicated table validates");
}

void test_spline_overflow_degrades() {
    auto table = durability();
    table.damage->diminishing = {{Fixed::from_raw(0), units(0)}, {Fixed::from_raw(17), units(256)},
        {Fixed::from_raw(34), units(0)}};
    eawr::core::LoadNoteBuffer notes;
    tactical::normalize_durability(table, notes);
    eawr::core::LoadNoteCollector collector;
    collector.merge(notes);
    const auto captured = collector.snapshot();
    expect(table.damage->diminishing_linear_fallback, "unsafe spline uses linear fallback");
    expect(std::any_of(captured.begin(), captured.end(), [](const auto& note) {
        return note.code == "EAWR-LOAD-0116";
    }), "unsafe spline records its fallback");
    expect(static_cast<bool>(tactical::validate_durability(table)), "fallback curve validates");
    const auto factor = tactical::diminishing_factor(*table.damage, 0);
    expect(factor && factor.value() >= units(0) && factor.value() <= units(256), "fallback factor is bounded");
    eawr::core::LoadNoteBuffer repeat;
    tactical::normalize_durability(table, repeat);
    eawr::core::LoadNoteCollector repeated;
    repeated.merge(repeat);
    expect(repeated.snapshot().empty(), "normalizing fallback twice emits no second note");
}

void test_validation() {
    auto table = durability();
    expect(static_cast<bool>(tactical::validate_durability(table)), "the test durability table is valid");
    table.damage->shield_recharge_frames = 0;
    expect(!tactical::validate_durability(table), "a zero recharge interval is rejected");
    table = durability();
    table.damage->armor_mods.pop_back();
    expect(!tactical::validate_durability(table), "a short armor table is rejected");
    table = durability();
    table.profiles[1].shield_armor_type = 9;
    expect(!tactical::validate_durability(table), "an unknown armor index is rejected");
    table = durability();
    std::swap(table.damage->diminishing[0], table.damage->diminishing[1]);
    expect(!tactical::validate_durability(table), "an unordered curve is rejected");
    // The bounds keep the pipeline in Q24: the largest projectile damage through the largest
    // curve value and armor multipliers destroys a unit instead of failing the tick.
    table = durability();
    const auto most = units(tactical::max_damage_multiplier);
    for (auto& point : table.damage->diminishing) point.y = most;
    for (auto& value : table.damage->armor_mods) value = most;
    expect(static_cast<bool>(tactical::validate_durability(table)), "the largest multipliers are valid");
    const tactical::Hit largest{units(tactical::max_hit_damage), 0, true, true, true, tactical::hull_target};
    for (const auto& profile : {table.profiles[1], table.profiles[2]}) {
        auto state = tactical::full_durability(profile);
        const auto outcome = tactical::apply_hit(profile, *table.damage, state, largest, 10);
        expect(outcome && state.hull.raw() == 0 && state.shields.raw() == 0,
            "the largest validated hit on type " + std::to_string(profile.type_id) + " stays in range");
    }
    table.damage->armor_mods[0] = Fixed::from_raw(most.raw() + 1);
    expect(!tactical::validate_durability(table), "an armor multiplier above the bound is rejected");
    table = durability();
    table.damage->diminishing[3].y = Fixed::from_raw(most.raw() + 1);
    expect(!tactical::validate_durability(table), "a curve value above the bound is rejected");
    // A spline that overshoots its points is capped at the bound: 0, 256, 256, 0 at whole
    // seconds peaks near 294 at 1.5 s (45 frames).
    table = durability();
    table.damage->diminishing = {{units(0), units(0)}, {units(1), most}, {units(2), most}, {units(3), units(0)}};
    expect(static_cast<bool>(tactical::validate_durability(table)), "an overshooting curve is valid");
    const auto capped = tactical::diminishing_factor(*table.damage, 45);
    expect(capped && capped.value() == most, "an overshooting curve is capped at the bound");
    // Points a raw step apart would overflow the spline in a hit: rejected up front.
    table.damage->diminishing = {{Fixed::from_raw(0), units(0)}, {Fixed::from_raw(1), most}, {Fixed::from_raw(2), units(0)}};
    expect(!tactical::validate_durability(table), "a curve that overflows between its points is rejected");
    auto weapons = combat();
    weapons.profiles[0].weapons[0].shot->speed = Fixed{};
    expect(!tactical::validate_combat(weapons), "a projectile without speed is rejected");
    weapons = combat();
    weapons.profiles[1].collision = tactical::CollisionBox{at(5, 0), at(-5, 0)};
    expect(!tactical::validate_combat(weapons), "an inverted collision box is rejected");
}

void test_large_range_roundtrip() {
    constexpr std::int64_t authored = 999999999;
    auto profiles = combat(authored);
    auto& shooter = profiles.profiles.front();
    shooter.max_attack_distance = units(authored);
    auto& weapon = shooter.weapons.front();
    weapon.hardpoint = tactical::object_weapon;
    weapon.range = units(authored);
    tactical::TacticalReplay replay;
    replay.setup = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, frigate_type, 2, at(1500, 0), true)});
    replay.final_tick_count = 90;
    const auto written = tactical::write_replay(replay);
    expect(static_cast<bool>(written), "large-range replay writes");
    if (!written) return;
    const auto parsed = tactical::parse_replay(written.value());
    expect(static_cast<bool>(parsed), "large-range replay parses");
    if (!parsed) return;
    const auto make = [&](const tactical::TacticalReplay& recording) {
        return tactical::TacticalSession::from_replay(recording, sensors(), durability(), {}, std::nullopt, profiles);
    };
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = make(workers == 1 ? replay : parsed.value());
        expect(static_cast<bool>(created), "billion-unit acquisition, object weapon and shot travel bind together");
        if (!created) return;
        auto live = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool fired = false;
        bool full_travel = false;
        for (int tick = 0; tick < 30; ++tick) {
            auto step = live.step(executor);
            expect(static_cast<bool>(step), "large-range acquisition step succeeds");
            if (!step) return;
            hashes.push_back(step.value().state_sha256);
            for (const auto& event : step.value().snapshot->combat_events())
                fired = fired || (event.shooter == 1 && event.target == 2
                    && event.kind == tactical::CombatEventKind::weapon_fired);
            for (const auto& projectile : live.projectiles())
                full_travel = full_travel || projectile.max_travel >= units(authored);
        }
        expect(fired && full_travel && !live.projectiles().empty(),
            "large finite range acquires beyond the old weapon reach and launches with full travel");
        const auto saved = live.save_checkpoint_state();
        auto fresh = make(parsed.value());
        expect(saved && fresh, "large-range in-flight checkpoint creates");
        if (!saved || !fresh) return;
        auto restored = std::move(fresh).value();
        const auto loaded = restored.load_checkpoint_state(saved.value());
        expect(loaded && restored.canonical_state_bytes() == live.canonical_state_bytes(),
            "checkpoint preserves full projectile travel and acquisition state");
        if (!loaded) return;
        const auto resaved = restored.save_checkpoint_state();
        expect(resaved && resaved.value() == saved.value(), "large-range checkpoint round-trips byte for byte");
        for (int tick = 30; tick < 90; ++tick) {
            const auto a = live.step(executor), b = restored.step(executor);
            expect(a && b && a.value().state_sha256 == b.value().state_sha256,
                "large-range restored continuation equals live");
            if (!a || !b) return;
            hashes.push_back(a.value().state_sha256);
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "large-range replay and every tick match on 1/2/4/8 workers");
    }
}

} // namespace

using namespace damage_test_support;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: damage_tests <fixtures directory> [--update]\n";
        return 2;
    }
    const bool update = argc > 2 && std::string_view(argv[2]) == "--update";
    test_validation();
    test_degraded_ion_storm();
    test_degraded_damage_shapes();
    test_spline_overflow_degrades();
    test_duplicate_durability_ids();
    test_large_range_roundtrip();
    test_zero_direct_blast();
    test_curve();
    test_hit_pipeline();
    // DG-40: protection stops ordinary hits before all health/energy/stun state changes,
    // without making the hardpoints non-destroyable to privileged scenario damage.
    {
        auto profile = frigate();
        profile.scenario_invulnerable = true;
        for (const auto shields : {0, 100}) {
            for (const auto target : {tactical::hull_target, 0U}) {
                auto state = tactical::full_durability(profile);
                state.shields = units(shields);
                const auto before = state;
                tactical::Hit hit{units(100), 0, true, true, true, target};
                hit.energy_damage = true;
                const auto protected_hit = tactical::apply_hit(profile, rules(), state, hit, 10);
                expect(protected_hit && state == before, "DG-40: ordinary damage preserves the complete durability state");
                hit.projectile = false;
                hit.kind = tactical::HitKind::asteroid;
                const auto asteroid = tactical::apply_hit(profile, rules(), state, hit, 10);
                expect(asteroid && state == before, "DG-40: environmental damage respects protection");
                hit.kind = tactical::HitKind::ordinary;
                const auto scripted = tactical::apply_hit(profile, rules(), state, hit, 10);
                expect(scripted && state != before, "DG-40: privileged scripted damage bypasses protection");
            }
        }
    }
    test_diminishing_gates();
    test_out_of_combat();
    test_arrival_vulnerability();
    test_recharge();
    test_energy();
    test_segment();
    test_hostile_projectile_filter();
    test_first_projectile_contact();
    test_shield_loss();
    test_arrival_in_battle();
    test_hardpoint_destroyed();
    test_lost_shooter_withdraws_shots();
    test_same_frame_retarget_after_kill();
    test_hull_drag_before_fire();
    test_hull_drag_lost_weapon_same_frame();
    test_meshes();
    test_mesh_hits();
    test_craft_sphere();
    test_object_weapon_scatter();
    test_object_burst_clock();
    test_aimed_routes();
    test_ordered_hardpoint_route();
    test_out_of_range_miss();
    test_range_boundary();
    test_path();
    test_broad_phase_reach();
    test_broad_phase_sphere_reach();
    test_energy_weapon();
    test_recharge_phase();
    test_missile_homing();
    test_missile_lock_loss();
    test_difficulty();
    test_mass_driver_damage();
    test_mass_driver_flight();
    test_battle(argv[1], update);
    if (failures != 0) {
        std::cerr << failures << " damage contract test(s) failed\n";
        return 1;
    }
    std::cout << "damage contracts passed\n";
    return 0;
}
