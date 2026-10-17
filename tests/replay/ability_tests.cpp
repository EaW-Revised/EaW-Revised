#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"
#include "../../src/sim/tactical/blast_internal.hpp"
#include "../../src/sim/tactical/checkpoint_fields_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// P2-13 (#76): unit abilities (docs/behaviour/space-abilities.md). The pure rules are checked with
// the M2 fleet's FoC values for each modelled ability (activation, duration, recharge and the
// multipliers); the session cases cover TURBO speeding up a move under way (AB-24), the DEFEND
// stand-in for a non-human and a human owner (AB-41 to AB-43), command rejections, and the
// same hashes for 1, 2, 4 and 8 workers and for the recorded replay.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// The M2 fleet's Unit_Ability data (spaceunitsfrigates.xml, spaceunitscorvettes.xml,
// spaceunitsfighters.xml), seconds truncated to frames (AB-04).
[[nodiscard]] tactical::AbilityProfile defend() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::defend;
    profile.expiration_frames = 450;
    profile.recharge_frames = 1800;
    profile.modifiers.weapon_delay = units(1);
    profile.modifiers.shield_regen = units(1);
    profile.modifiers.shield_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen_interval = decimal("0.1");
    profile.modifiers.energy_regen = units(3);
    profile.modifiers.speed = decimal("0.8");
    profile.supports_autofire = true;
    return profile;
}
[[nodiscard]] tactical::AbilityProfile turbo() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::turbo;
    profile.expiration_frames = 600;
    profile.recharge_frames = 1500;
    profile.modifiers.weapon_delay = units(3);
    profile.modifiers.shield_regen = Fixed{};
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.speed = units(2);
    return profile;
}
[[nodiscard]] tactical::AbilityProfile tartan_power() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::power_to_weapons;
    profile.expiration_frames = 210;
    profile.recharge_frames = 1800;
    profile.modifiers.speed = decimal("0.5");
    profile.modifiers.shield_regen = units(-25);
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.weapon_delay = decimal("0.2");
    return profile;
}
[[nodiscard]] tactical::AbilityProfile acclamator_power() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::power_to_weapons;
    profile.expiration_frames = 600;
    profile.recharge_frames = 1800;
    profile.modifiers.weapon_delay = decimal("0.5");
    profile.modifiers.shield_regen = units(-3);
    profile.modifiers.energy_regen = units(1);
    profile.modifiers.speed = decimal("0.5");
    return profile;
}
[[nodiscard]] tactical::AbilityProfile spoiler_lock() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::spoiler_lock;
    profile.modifiers.weapon_delay = units(3);
    profile.modifiers.shield_regen = units(3);
    profile.modifiers.energy_regen = units(3);
    profile.modifiers.speed = decimal("1.3");
    return profile;
}

[[nodiscard]] tactical::UnitAbilityProfile unit_of(const tactical::TypeId type, const tactical::AbilityProfile& ability,
    const bool script = false) {
    return {type, {ability}, script};
}

const tactical::AbilityGate open_gate{true, true, false, true};

void test_names_and_validation() {
    expect(tactical::ability_kind("invulnerability") == tactical::AbilityKind::invulnerability,
        "WHE-22: Falcon mode has an ordinary ability kind");
    expect(tactical::ability_kind("DEFEND") == tactical::AbilityKind::defend, "DEFEND");
    expect(tactical::ability_kind("power_to_weapons") == tactical::AbilityKind::power_to_weapons,
        "the Acclamator's lower-case spelling");
    expect(tactical::ability_kind("Spoiler_Lock") == tactical::AbilityKind::spoiler_lock, "any case");
    expect(tactical::ability_kind("HUNT") == tactical::AbilityKind::hunt, "WAB-50: HUNT is modelled");
    expect(tactical::ability_kind("ION_CANNON_SHOT") == tactical::AbilityKind::ion_cannon_shot,
        "ION_CANNON_SHOT is modelled since #561 (AB-60)");
    expect(tactical::to_string(tactical::AbilityKind::turbo) == "TURBO", "TURBO's name");

    tactical::AbilityTable table;
    table.profiles = {unit_of(7, turbo()), unit_of(9, defend(), true)};
    table.humans = {1};
    expect(static_cast<bool>(tactical::validate_abilities(table)), "an M2-shaped table is valid");
    auto unsorted = table;
    std::swap(unsorted.profiles[0], unsorted.profiles[1]);
    expect(!tactical::validate_abilities(unsorted), "type IDs must increase");
    auto doubled = table;
    doubled.profiles[0].abilities.push_back(turbo());
    expect(!tactical::validate_abilities(doubled), "two abilities of one kind are rejected");
    auto stalled = table;
    stalled.profiles[0].abilities[0].modifiers.speed = Fixed{};
    expect(!tactical::validate_abilities(stalled), "a zero speed multiplier is rejected");
    auto huge = table;
    huge.profiles[0].abilities[0].modifiers.shield_regen = units(65);
    expect(!tactical::validate_abilities(huge), "a multiplier beyond 64 is rejected");
    auto defaults = table;
    defaults.autofire_defaults = {1};
    expect(static_cast<bool>(tactical::validate_abilities(defaults)), "a human creation preference is valid");
    defaults.autofire_defaults = {2};
    expect(!tactical::validate_abilities(defaults), "an AI owner is not a local profile owner");
    defaults.autofire_defaults = {1, 1};
    expect(!tactical::validate_abilities(defaults), "duplicate creation preferences are rejected");
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    expect(!tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, unsorted),
        "a session rejects an invalid ability table");
}

void test_timers() {
    // AB-11, AB-12: TURBO lasts 600 frames, then recharges for 1500.
    const auto profile = turbo();
    tactical::AbilitySlot slot;
    expect(tactical::ability_ready(profile, slot, open_gate, 0), "a fresh ability is ready");
    const auto on = tactical::activate_ability(profile, slot, open_gate, 100);
    expect(on.changed && on.speed && slot.active && slot.expires_tick == 700, "TURBO switches on until tick 700");
    expect(!tactical::activate_ability(profile, slot, open_gate, 101).changed, "an active ability does not switch on again");
    expect(tactical::ability_ready(profile, slot, open_gate, 101), "an active ability reads as ready (AB-13)");
    tactical::UnitAbilityProfile unit = unit_of(7, profile);
    tactical::AbilityState state;
    state.slots = {slot};
    expect(!tactical::expire_abilities(unit, state, 699).changed && state.slots[0].active, "still on at tick 699");
    const auto expired = tactical::expire_abilities(unit, state, 700);
    expect(expired.changed && expired.speed && !state.slots[0].active && state.slots[0].ready_tick == 2200,
        "it runs out at tick 700 and recharges until tick 2200");
    expect(!tactical::ability_ready(profile, state.slots[0], open_gate, 2199), "recharging at tick 2199");
    expect(tactical::ability_ready(profile, state.slots[0], open_gate, 2200), "ready at tick 2200");

    // AB-12: DEFEND ended early recharges for the share of its 450 frames that ran.
    const auto shield = defend();
    tactical::AbilitySlot early;
    static_cast<void>(tactical::activate_ability(shield, early, open_gate, 1000));
    static_cast<void>(tactical::deactivate_ability(shield, early, 1150));
    expect(early.ready_tick == 1150 + 600, "150 of 450 frames: a third of 1800 (600 frames)");
    tactical::AbilitySlot odd;
    static_cast<void>(tactical::activate_ability(shield, odd, open_gate, 0));
    static_cast<void>(tactical::deactivate_ability(shield, odd, 1));
    expect(odd.ready_tick == 1 + 4, "1 of 450 frames: 1800 / 450 = 4 frames");
    tactical::AbilitySlot instant;
    static_cast<void>(tactical::activate_ability(shield, instant, open_gate, 50));
    static_cast<void>(tactical::deactivate_ability(shield, instant, 50));
    expect(instant.ready_tick == 50, "ended in its first frame: no recharge");

    // SPOILER_LOCK has no time limit and no recharge: a free toggle (the owner's S-foil note).
    const auto foils = spoiler_lock();
    tactical::AbilitySlot lock;
    for (std::uint64_t tick = 10; tick < 20; tick += 2) {
        expect(tactical::activate_ability(foils, lock, open_gate, tick).changed && lock.expires_tick == 0,
            "the S-foils lock at once");
        expect(tactical::deactivate_ability(foils, lock, tick + 1).changed && lock.ready_tick <= tick + 1,
            "and open again at once");
    }

    // AB-14, AB-16: DEFEND needs an online shield outside its depletion effect; TURBO and
    // SPOILER_LOCK need engines.
    tactical::AbilitySlot gated;
    expect(!tactical::ability_ready(shield, gated, {false, true, false, true}, 0), "no shield: no DEFEND");
    expect(!tactical::ability_ready(shield, gated, {true, false, false, true}, 0), "generators lost: no DEFEND");
    expect(!tactical::ability_ready(shield, gated, {true, true, true, true}, 0), "depleted shield: no DEFEND");
    expect(!tactical::ability_ready(profile, gated, {true, true, false, false}, 0), "engines lost: no TURBO");
    expect(!tactical::ability_ready(foils, gated, {true, true, false, false}, 0), "engines lost: no S-foil lock");
    expect(tactical::ability_ready(tartan_power(), gated, {false, false, true, false}, 0), "POWER_TO_WEAPONS has no gate");
}

void test_multipliers() {
    // AB-20: one active ability's multipliers; none active: 1.
    tactical::UnitAbilityProfile corvette = unit_of(7, turbo());
    tactical::AbilityState state = tactical::initial_abilities(corvette);
    using M = tactical::AbilityModifier;
    expect(tactical::ability_multiplier(corvette, state, M::speed).raw() == one, "inactive: speed 1");
    static_cast<void>(tactical::activate_ability(corvette.abilities[0], state.slots[0], open_gate, 0));
    expect(tactical::ability_multiplier(corvette, state, M::speed).raw() == 2 * one, "TURBO: speed 2");
    expect(tactical::ability_multiplier(corvette, state, M::weapon_delay).raw() == 3 * one, "TURBO: weapon delay 3");
    expect(tactical::ability_multiplier(corvette, state, M::shield_regen).raw() == 0, "TURBO: no shield regen");
    expect(tactical::ability_multiplier(corvette, state, M::shield_regen_interval).raw() == one, "unauthored: 1");
    // Two abilities multiply (a primary and a secondary; no M2 type has two).
    tactical::UnitAbilityProfile both{8, {turbo(), spoiler_lock()}, false};
    tactical::AbilityState two = tactical::initial_abilities(both);
    static_cast<void>(tactical::activate_ability(both.abilities[0], two.slots[0], open_gate, 0));
    static_cast<void>(tactical::activate_ability(both.abilities[1], two.slots[1], open_gate, 0));
    expect(tactical::ability_multiplier(both, two, M::speed) == decimal("2.6"), "2 x 1.3 = 2.6");

    // AB-21: the weapon delay lengthens a burst's gap; the recharge after it only shortens.
    expect(tactical::scaled_weapon_delay(90, units(3), false) == 270, "S-foils locked: a 90-frame gap takes 270");
    expect(tactical::scaled_weapon_delay(90, units(3), true) == 90, "a full recharge is never lengthened");
    expect(tactical::scaled_weapon_delay(90, decimal("0.2"), true) == 18, "the Tartan's POWER_TO_WEAPONS: 90 -> 18");
    expect(tactical::scaled_weapon_delay(45, acclamator_power().modifiers.weapon_delay, true) == 23, "the Acclamator's: 45 -> 22.5, rounded up");
    expect(tactical::scaled_weapon_delay(7, decimal("0.5"), false) == 4, "half of 7 rounds half up");
    // AB-22, AB-23: DEFEND's 0.1 intervals: shields every 9 frames, energy every 15.
    expect(tactical::scaled_interval(90, decimal("0.1")) == 9, "90 x 0.1 = 9 frames");
    expect(tactical::scaled_interval(150, decimal("0.1")) == 15, "150 x 0.1 = 15 frames");
    expect(tactical::scaled_interval(3, decimal("0.1")) == 1, "at least one frame");
}

// --- Sessions ----------------------------------------------------------------------------------

constexpr tactical::TypeId corvette_type = 11;
constexpr tactical::TypeId frigate_type = 12;
constexpr tactical::TypeId plain_type = 13;

[[nodiscard]] tactical::MotionTable motion() {
    const tactical::MotionProfile corvette{corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"),
        decimal("1.5"), units(2), decimal("0.24"), units(15)};
    const tactical::MotionProfile frigate{frigate_type, decimal("2.64"), decimal("0.048"), decimal("0.048"),
        decimal("0.84"), units(3), decimal("0.24"), units(5)};
    return {{units(15), units(300)}, {corvette, frigate}, std::nullopt, {}, {}};
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    tactical::DamageRules damage;
    damage.shield_recharge_frames = 90;
    damage.depleted_disable_seconds = units(5);
    damage.depleted_regen_cap = decimal("0.25");
    damage.damage_types = 1;
    damage.armor_types = 1;
    damage.armor_mods = {units(1)};
    damage.energy_recharge_frames = 150;
    damage.energy_to_shield = units(5);
    table.damage = damage;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::DurabilityProfile profile;
        profile.type_id = type;
        profile.max_hull = units(3600);
        profile.max_shields = units(700);
        profile.shield_refresh = units(50);
        profile.armor_type = 0;
        profile.shield_armor_type = 0;
        profile.damage_tracking = true; // WCC-82: every M2 ship has DAMAGE_TRACKING
        table.profiles.push_back(profile);
    }
    return table;
}

[[nodiscard]] tactical::AbilityTable abilities(const std::vector<tactical::PlayerId>& humans) {
    tactical::AbilityTable table;
    table.profiles = {unit_of(corvette_type, turbo()), unit_of(frigate_type, defend(), true)};
    table.humans = humans;
    table.autofire_defaults = humans;
    return table;
}

[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 76;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = {
        {1, corvette_type, 1, at(-1800, -1500), math::identity_quat(), {}},
        {2, frigate_type, 1, at(0, 2000), math::identity_quat(), {}},
        {3, plain_type, 2, at(4000, 4000), math::identity_quat(), {}},
    };
    return result;
}

[[nodiscard]] tactical::PlayerCommand command(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const eawr::sim::EntityId unit, tactical::CommandPayload payload) {
    return {{tick, player, sequence}, {unit}, std::move(payload)};
}

struct Run {
    std::vector<std::string> hashes;
    std::vector<math::Vec3> corvette;           // position after each tick
    std::vector<Fixed> frigate_shield;          // after each tick
    std::vector<tactical::Event> events;
    std::optional<tactical::TacticalSession> session;
};

[[nodiscard]] Run run(const std::vector<tactical::PlayerCommand>& commands, const std::vector<tactical::PlayerId>& humans,
    const std::size_t workers, const std::uint64_t ticks) {
    Run result;
    auto created = tactical::TacticalSession::create(
        setup(), {}, durability(), motion(), std::nullopt, {}, {}, abilities(humans));
    expect(static_cast<bool>(created), "the ability session is created");
    if (!created) return result;
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "a command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        for (const auto& unit : session.units()) {
            if (unit.entity_id == 1) result.corvette.push_back(unit.position);
        }
        const auto health = session.durability_state(2);
        result.frigate_shield.push_back(health ? health->shields : Fixed{});
    }
    result.session.emplace(std::move(session));
    return result;
}

void test_deployment_abilities() {
    using Kind = tactical::AbilityKind;
    using Action = tactical::AbilityAction;
    tactical::AbilityProfile well;
    well.kind = Kind::interdict;
    well.disable_movement = true;
    tactical::AbilityProfile shield;
    shield.kind = Kind::missile_shield;
    shield.effective_radius = units(750);
    shield.disable_movement = true;
    shield.supports_autofire = true;
    tactical::UnitAbilityProfile modes{corvette_type, {well, shield}};
    modes.gravity_well_hardpoints = {0};
    tactical::AbilityGate gate;
    gate.gravity_well_online = false;
    expect(!tactical::ability_ready(well, {}, gate, 0), "WIN-02: destroyed gravity well refuses INTERDICT");
    expect(tactical::ability_ready(shield, {}, gate, 0), "WMS-02: shield does not require the gravity well");
    auto timed = well;
    timed.expiration_frames = 20;
    timed.recharge_frames = 40;
    auto clock = tactical::initial_abilities({corvette_type, {timed}});
    expect(tactical::activate_ability(timed, clock.slots[0], {}, 10).changed, "WIN-02: optional timer starts");
    expect(tactical::expire_abilities({corvette_type, {timed}}, clock, 30).changed
        && !clock.slots[0].active && clock.slots[0].ready_tick == 70, "WIN-04: optional expiry releases deployment");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto health = durability();
        health.profiles[0].hardpoints = {{tactical::HardpointRole::other, true, units(50)}};
        auto created = tactical::TacticalSession::create(setup(), {}, health, motion(), std::nullopt, {}, {},
            tactical::AbilityTable{{modes}, {1}});
        expect(static_cast<bool>(created), "WIN/WMS: deployment session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        const auto submit = [&](std::uint64_t tick, tactical::CommandPayload payload) {
            expect(static_cast<bool>(session.submit(command(tick, 1, tick, 1, std::move(payload)))), "deployment command submits");
        };
        submit(0, tactical::MovePayload{at(3000, -1500)});
        submit(60, tactical::AbilityPayload{Kind::interdict, Action::activate});
        submit(70, tactical::AbilityPayload{Kind::missile_shield, Action::activate});
        submit(80, tactical::MovePayload{at(3000, -1500)});
        submit(90, tactical::FacePayload{at(-3000, -1500)});
        submit(100, tactical::AbilityPayload{Kind::interdict, Action::deactivate});
        submit(110, tactical::MovePayload{at(3000, -1500)});
        submit(120, tactical::AbilityPayload{Kind::missile_shield, Action::deactivate});
        submit(121, tactical::MovePayload{at(3000, -1500)});
        submit(130, tactical::AbilityPayload{Kind::interdict, Action::activate});
        submit(140, tactical::DamagePayload{units(10000), 0});
        submit(150, tactical::AbilityPayload{Kind::interdict, Action::activate});
        submit(155, tactical::AbilityPayload{Kind::missile_shield, Action::activate});
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        math::Vec3 stopped{};
        math::Quat facing{};
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 170; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "WIN/WMS: deployment step succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            const auto units_now = session.units();
            const auto& ship = units_now.front();
            if (tick == 60) { stopped = ship.position; facing = ship.rotation; }
            if (tick > 60 && tick < 120)
                expect(ship.position == stopped && ship.rotation == facing, "WIN/WMS movement lock and inferred facing lock hold across overlapping modes");
            if (tick == 119) expect(session.ability_state(1)->slots[1].active,
                "WMS-01: zero duration persists until switched off");
            if (tick == 139) expect(ship.position != stopped, "WIN/WMS: last switch-off permits a new move");
            if (tick == 141) expect(!session.ability_state(1)->slots[0].active,
                "WHD-04: generator loss ends INTERDICT");
            if (tick == 151) {
                const auto& status = step.value().snapshot->instances().front().abilities.front();
                expect(!status.active && !status.ready, "WIN-02: lost generator gates command and HUD readiness");
            }
            if (tick == 156) expect(session.ability_state(1)->slots[1].active,
                "WMS-02: shield still works after generator destruction");
        }
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WIN/WMS: deployment hashes equal at 1/2/4/8 workers");
    }
    auto unlocked = modes;
    unlocked.abilities[0].disable_movement = false;
    auto state = tactical::initial_abilities(unlocked);
    static_cast<void>(tactical::activate_ability(unlocked.abilities[0], state.slots[0], {}, 0));
    expect(!tactical::ability_movement_disabled(unlocked, state), "WIN-02: Disable_Movement is data-driven");
}

void test_turbo_session() {
    // S-17 shape: a move at tick 30, TURBO at tick 60 (AB-24: the move plans again at once).
    const std::vector<tactical::PlayerCommand> plain{command(30, 1, 0, 1, tactical::MovePayload{at(3000, -1500)})};
    auto turbo_commands = plain;
    turbo_commands.push_back(command(60, 1, 1, 1, tactical::AbilityPayload{tactical::AbilityKind::turbo,
        tactical::AbilityAction::activate}));
    const auto slow = run(plain, {}, 1, 200);
    const auto fast = run(turbo_commands, {}, 1, 700);
    if (slow.corvette.size() < 200 || fast.corvette.size() < 700) return;
    expect(slow.corvette[60].x == fast.corvette[60].x, "TURBO leaves the ticks before it unchanged");
    const auto step = [](const Run& run, const std::size_t tick) {
        return Fixed::from_raw(run.corvette[tick].x.raw() - run.corvette[tick - 1].x.raw());
    };
    expect(step(slow, 199) == decimal("3.72"), "without TURBO the corvette cruises at 3.72");
    expect(step(fast, 199).raw() == 2 * step(slow, 199).raw(), "with TURBO it cruises at twice that");
    const auto state = fast.session->ability_state(1);
    expect(state && !state->slots[0].active && state->slots[0].ready_tick == 660 + 1500,
        "TURBO from tick 60 ran out at 660 and recharges to 2160");
    // AB-50: the snapshot's status.
    const auto snapshot = fast.session->snapshot();
    for (const auto& instance : snapshot->instances()) {
        if (instance.entity_id != 1) continue;
        expect(instance.abilities.size() == 1 && instance.abilities[0].kind == tactical::AbilityKind::turbo
                && !instance.abilities[0].active && !instance.abilities[0].ready
                && instance.abilities[0].total_frames == 1500 && instance.abilities[0].remaining_frames == 1460,
            "the snapshot shows TURBO recharging, 1460 of 1500 frames left");
    }
    // Every worker count and the recorded replay give the same hashes.
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run(turbo_commands, {}, workers, 700).hashes == fast.hashes,
            "TURBO hashes with " + std::to_string(workers) + " workers");
    }
    const auto replay = fast.session->record();
    auto bytes = tactical::write_replay(replay);
    auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
    expect(parsed && parsed.value() == replay, "an ability command survives the replay format");
    if (parsed) {
        auto again = tactical::TacticalSession::from_replay(
            parsed.value(), {}, durability(), motion(), std::nullopt, {}, {}, abilities({}));
        expect(static_cast<bool>(again), "the replay session is created");
        if (again) {
            const eawr::platform::ThreadWorkerAdapter executor(2);
            std::vector<std::string> hashes;
            while (again.value().completed_tick() < replay.final_tick_count) {
                hashes.push_back(again.value().step(executor).value().state_sha256);
            }
            expect(hashes == fast.hashes, "the replay reproduces every tick hash");
        }
    }
}

void test_expiry_snapshot_keeps_timed_duration() {
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto table = abilities({1});
        table.profiles.front().abilities.front().expiration_frames = 3;
        table.profiles.front().abilities.front().recharge_frames = 7;
        auto created = tactical::TacticalSession::create(
            setup(), {}, durability(), motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "the expiry boundary session is created");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1,
            tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::activate}))),
            "the short timed ability is accepted");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint32_t completed = 1; completed <= 4; ++completed) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the expiry boundary step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto& instances = stepped.value().snapshot->instances();
            const auto instance = std::find_if(instances.begin(), instances.end(), [](const auto& value) {
                return value.entity_id == 1;
            });
            expect(instance != instances.end() && instance->abilities.size() == 1,
                "the timed ability is published");
            if (instance == instances.end() || instance->abilities.size() != 1) continue;
            const auto& status = instance->abilities.front();
            if (completed <= 3) {
                expect(status.active && status.total_frames == 3 && status.remaining_frames == 3 - completed,
                    "AB-05: every active snapshot retains its duration, including zero remaining");
            } else {
                expect(!status.active && status.total_frames == 7 && status.remaining_frames == 6,
                    "the next snapshot publishes the recharge countdown");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "the expiry boundary hashes agree for 1/2/4/8 workers");
    }
}

void test_defend_stand_in() {
    // AB-41 to AB-43: 400 damage at tick 500 makes the next window's rate 400 > 20; a non-human
    // owner's frigate switches DEFEND on at tick 510 and its shield recharges from tick 511 every
    // 9 frames (the S-15 shape, G-D7).
    const std::vector<tactical::PlayerCommand> hit{command(500, 2, 0, 2, tactical::DamagePayload{units(400)})};
    const auto ai = run(hit, {}, 1, 560);
    if (ai.frigate_shield.size() < 560) return;
    expect(ai.frigate_shield[500] == units(300), "the hit takes 400 of the 700 shield");
    std::vector<std::uint64_t> gains;
    // Index i holds the shield after the step that ran tick i; the frigate's own recharge phase may add
    // one gain before DEFEND, so only gains from tick 510 on are compared.
    for (std::size_t index = 510; index < 559; ++index) {
        if (ai.frigate_shield[index] > ai.frigate_shield[index - 1]) gains.push_back(index);
    }
    expect(gains.size() >= 3 && gains[0] == 511 && gains[1] == 520 && gains[2] == 529,
        "DEFEND recharges the shield at 511, 520 and 529");
    const auto state = ai.session->ability_state(2);
    expect(state && state->slots[0].active && state->slots[0].expires_tick == 510 + 450, "DEFEND runs until tick 960");
    // AB-45: a human owner's supported ability starts armed, as a fresh retail profile does.
    const auto human_default = run(hit, {1}, 1, 560);
    const auto armed_default = human_default.session->ability_state(2);
    expect(armed_default && armed_default->slots[0].active && armed_default->slots[0].autofire,
        "a human owner's default DEFEND autofire needs no ability command");
    auto manual = hit;
    manual.push_back(command(10, 1, 0, 2, tactical::AbilityPayload{tactical::AbilityKind::defend,
        tactical::AbilityAction::autofire_off}));
    const auto human = run(manual, {1}, 1, 560);
    const auto idle = human.session->ability_state(2);
    expect(idle && !idle->slots[0].active, "a human owner's DEFEND stays off without autofire");
    auto armed = hit;
    armed.push_back(command(10, 1, 0, 2, tactical::AbilityPayload{tactical::AbilityKind::defend,
        tactical::AbilityAction::autofire_on}));
    const auto autofire = run(armed, {1}, 1, 560);
    const auto fired = autofire.session->ability_state(2);
    expect(fired && fired->slots[0].active && fired->slots[0].autofire, "with autofire on it fires like the AI's");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        expect(run(hit, {}, workers, 560).hashes == ai.hashes, "DEFEND hashes with " + std::to_string(workers) + " workers");
        expect(run(hit, {1}, workers, 560).hashes == human_default.hashes,
            "human default autofire hashes with " + std::to_string(workers) + " workers");
    }
}

void test_autofire_rules() {
    const tactical::UnitAbilityProfile mixed{9, {turbo(), defend()}, true};
    const auto enabled = tactical::initial_abilities(mixed, true);
    expect(!enabled.slots[0].autofire && enabled.slots[1].autofire,
        "AB-45: only supported abilities start armed, including a secondary ability");
    expect(!enabled.slots[0].active && !enabled.slots[1].active,
        "AB-40: arming never activates an ability");
    const auto disabled = tactical::initial_abilities(mixed, false);
    expect(!disabled.slots[1].autofire, "AB-45: the disabled creation preference is retained");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto idle = run({}, {1}, workers, 31);
        const auto ready = idle.session->ability_state(2);
        expect(ready && ready->slots[0].autofire && !ready->slots[0].active,
            "AB-41: an armed idle frigate needs damage, not merely an enemy");
        for (const auto damage : {20, 21}) {
            const auto result = run({command(10, 2, 0, 2, tactical::DamagePayload{units(damage)})}, {1}, workers, 31);
            const auto state = result.session->ability_state(2);
            expect(state && state->slots[0].active == (damage > 20),
                "AB-41: the damage threshold is strictly greater than twenty");
        }
        const auto depleted = run({command(10, 2, 0, 2, tactical::DamagePayload{units(700)})}, {1}, workers, 31);
        const auto state = depleted.session->ability_state(2);
        expect(state && state->slots[0].autofire && !state->slots[0].active,
            "AB-14: autofire cannot bypass the shield depletion gate");
        const auto cooldown = run({command(10, 2, 0, 2, tactical::DamagePayload{units(21)}),
            command(500, 2, 1, 2, tactical::DamagePayload{units(21)})}, {1}, workers, 541);
        const auto cooling = cooldown.session->ability_state(2);
        expect(cooling && !cooling->slots[0].active && cooling->slots[0].ready_tick == 2280,
            "AB-13: damage while recharging does not retrigger DEFEND");
    }
}

void test_rejections() {
    const auto activate = [](const tactical::AbilityKind kind) {
        return tactical::AbilityPayload{kind, tactical::AbilityAction::activate};
    };
    const std::vector<tactical::PlayerCommand> commands{
        command(5, 1, 0, 1, activate(tactical::AbilityKind::defend)),  // the corvette has no DEFEND
        command(6, 1, 1, 1, activate(tactical::AbilityKind::turbo)),   // accepted
        command(7, 1, 2, 1, activate(tactical::AbilityKind::turbo)),   // already on
        command(8, 1, 3, 1, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::deactivate}),
        command(9, 1, 4, 1, activate(tactical::AbilityKind::turbo)),   // recharging (2 frames x 1500 / 600 = 5)
        command(9, 2, 0, 1, activate(tactical::AbilityKind::turbo)),   // not the owner
        command(9, 1, 5, 2, tactical::AbilityPayload{tactical::AbilityKind::defend, tactical::AbilityAction::autofire_on}),
        command(9, 1, 6, 1, tactical::AbilityPayload{tactical::AbilityKind::turbo, tactical::AbilityAction::autofire_on}),
    };
    const auto result = run(commands, {1}, 1, 20);
    std::vector<std::pair<tactical::EventKind, tactical::RejectReason>> seen;
    for (const auto& event : result.events) {
        if (event.order == tactical::OrderKind::ability) seen.emplace_back(event.kind, event.reason);
    }
    using K = tactical::EventKind;
    using R = tactical::RejectReason;
    const std::vector<std::pair<K, R>> expected{{K::order_rejected, R::ability_unavailable}, {K::order_accepted, R::none},
        {K::order_rejected, R::ability_unavailable}, {K::order_accepted, R::none}, {K::order_rejected, R::ability_unavailable},
        {K::order_accepted, R::none}, {K::order_rejected, R::ability_unavailable}, {K::order_rejected, R::unit_not_owned}};
    // In canonical order: tick, then player, then sequence.
    expect(seen == expected, "ability commands are accepted and rejected as AB-10 to AB-15 say");
    const auto state = result.session->ability_state(1);
    expect(state && !state->slots[0].active && state->slots[0].ready_tick == 8 + 5, "TURBO ended after 2 frames");
    const auto frigate = result.session->ability_state(2);
    expect(frigate && frigate->slots[0].autofire, "DEFEND is on autofire");
}

// #614: a squadron a hangar launches mid-battle locks its S-foils like a tick-zero one. The
// button's order goes to the squadron's container (AB-40); the launched container and craft
// enter with the ability off and ready (AB-10), so the order reaches the flying craft.
constexpr tactical::TypeId station_type = 20;
constexpr tactical::TypeId xwing_type = 21;
constexpr tactical::TypeId xwing_squadron = 22;

[[nodiscard]] tactical::MotionTable hangar_motion() {
    tactical::MotionTable table;
    tactical::CraftProfile craft;
    craft.type_id = xwing_type;
    craft.max_speed = decimal("5.4");
    craft.min_speed = decimal("1.8");
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = decimal("0.2");
    craft.roll_rate = units(6);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(200);
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {
        {xwing_squadron, {xwing_type, xwing_type}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(20)}};
    tactical::SpawnerProfile spawner;
    spawner.type_id = station_type;
    spawner.entries = {{xwing_squadron, 1, 0}};
    spawner.delay_frames = 150;
    spawner.bays = {{0, at(20, 0, -10), at(1, 0, -1)}};
    table.squadrons.spawners = {spawner};
    return table;
}

[[nodiscard]] tactical::DurabilityTable hangar_durability() {
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    table.profiles = {tactical::DurabilityProfile{station_type, units(5000), std::nullopt, false, {}},
        tactical::DurabilityProfile{xwing_type, units(90), units(5), false, {}}};
    return table;
}

void test_launched_squadron_sfoils() {
    tactical::TacticalSetup setup;
    setup.seed = 614;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    // A tick-zero squadron (container 10, craft 11 and 12) beside the station; its hangar
    // launches one more (craft 13 and 14, container 15).
    setup.units = {{1, station_type, 1, at(0, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, at(-500, 0), math::identity_quat(), {}},
        {11, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}, {12, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11, 12}}};
    tactical::AbilityTable table;
    table.profiles = {unit_of(xwing_type, spoiler_lock())};
    const auto lock = [](const tactical::AbilityAction action) {
        return tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, action};
    };
    const auto foils = [](const tactical::TacticalSession& session, const eawr::sim::EntityId craft) {
        const auto state = session.ability_state(craft);
        return state && state->slots.size() == 1 && state->slots[0].active;
    };
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(
            setup, {}, hangar_durability(), hangar_motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "#614: the hangar session is created");
        if (!created) return;
        auto session = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto step_to = [&](const std::uint64_t tick) {
            while (session.completed_tick() < tick) {
                auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "#614: a step succeeds");
                if (!stepped) return;
                hashes.push_back(stepped.value().state_sha256);
            }
        };
        const auto order = [&](const std::uint64_t tick, const std::uint64_t sequence, const eawr::sim::EntityId unit,
                               const tactical::AbilityAction action) {
            expect(static_cast<bool>(session.submit(command(tick, 1, sequence, unit, lock(action)))), "#614: the order is accepted");
        };
        step_to(60);
        expect(session.ability_state(13) && session.ability_state(14) && session.ability_state(15) == std::nullopt,
            "#614: the launched craft hold the ability, their container does not");
        expect(!foils(session, 11) && !foils(session, 13), "#614: the S-foils start off (AB-10)");
        // The starting and the launched squadron lock together, each by its container.
        order(61, 0, 10, tactical::AbilityAction::activate);
        order(61, 1, 15, tactical::AbilityAction::activate);
        step_to(70);
        expect(foils(session, 11) && foils(session, 12), "#614: the starting squadron's craft lock");
        expect(foils(session, 13) && foils(session, 14), "#614: the launched squadron's craft lock");
        // A snapshot shows the launched craft's ability as active, which is what the viewer draws.
        std::size_t shown = 0;
        for (const auto& instance : session.snapshot()->instances()) {
            if (instance.entity_id != 13 && instance.entity_id != 14) continue;
            for (const auto& ability : instance.abilities) {
                if (ability.kind == tactical::AbilityKind::spoiler_lock && ability.active) ++shown;
            }
        }
        expect(shown == 2, "#614: the snapshot shows both launched craft with SPOILER_LOCK on");
        // The order also reaches a launched craft that is ordered on its own.
        order(71, 0, 13, tactical::AbilityAction::deactivate);
        step_to(80);
        expect(!foils(session, 13) && foils(session, 14), "#614: a single launched craft unlocks alone");
        order(81, 0, 15, tactical::AbilityAction::deactivate);
        order(81, 1, 10, tactical::AbilityAction::deactivate);
        step_to(90);
        expect(!foils(session, 11) && !foils(session, 12) && !foils(session, 13) && !foils(session, 14),
            "#614: both squadrons unlock");
        if (baseline.empty()) {
            baseline = hashes;
        } else {
            expect(hashes == baseline, "#614: the hangar session hashes with " + std::to_string(workers) + " workers");
        }
    }
}

void test_created_squadron_autofire() {
    // AB-45: a supported team ability is created for the hangar's container and craft.
    // This synthetic squadron uses the hangar fixture's IDs and motion, without weapons.
    tactical::TacticalSetup battle;
    battle.seed = 1054;
    battle.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    battle.units = {{1, station_type, 1, at(0, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, at(-500, 0), math::identity_quat(), {}},
        {11, xwing_type, 1, at(-500, 0), math::identity_quat(), {}},
        {12, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}};
    battle.squadrons = {{10, {11, 12}}};
    tactical::AbilityProfile ion;
    ion.kind = tactical::AbilityKind::ion_cannon_shot;
    ion.supports_autofire = true;
    auto team = ion;
    team.team = true;
    tactical::AbilityTable table;
    table.profiles = {unit_of(xwing_type, ion), unit_of(xwing_squadron, team)};
    table.humans = {1};
    table.autofire_defaults = {1};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(battle, {}, hangar_durability(), hangar_motion(),
            std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "AB-45: the supported hangar session is created");
        if (!created) return;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 10,
            tactical::AbilityPayload{tactical::AbilityKind::ion_cannon_shot, tactical::AbilityAction::autofire_off}))),
            "the starting container's autofire can be switched off");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::size_t tick = 0; tick < 60; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the supported hangar session steps");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
        }
        const auto original = session.ability_state(10);
        expect(original && !original->slots[0].autofire, "an existing toggle is not reapplied every tick");
        for (const auto id : {13U, 14U, 15U}) {
            const auto spawned = session.ability_state(id);
            expect(spawned && spawned->slots[0].autofire && !spawned->slots[0].active,
                "AB-45: newly launched craft and their container inherit the creation preference");
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "AB-45: launched defaults are deterministic for 1/2/4/8 workers");
    }
}

struct SpecialProbe final : tactical::SpecialAbilityHandler {
    bool is_ready{true}, mode_ok{true}, target_ok{true}, is_available{true}, succeeds{true};
    std::vector<std::size_t> applied, serviced, terminated;
    std::vector<eawr::sim::EntityId> removed;
    std::size_t target_checks{};
    bool context_valid{true}, schedule_valid{true};
    bool ready(std::size_t, const tactical::SpecialActivationContext&) const override { return is_ready; }
    bool appropriate_mode(std::size_t, tactical::SpecialAbilityMode) const override { return mode_ok; }
    bool appropriate_target(std::size_t, const tactical::SpecialActivationContext&) const override { return target_ok; }
    bool available(std::size_t, const tactical::SpecialActivationContext&) const override { return is_available; }
    bool apply(const std::size_t slot, tactical::SpecialAbilitySlot& state, const tactical::SpecialActivationContext& context) override {
        applied.push_back(slot);
        context_valid = context_valid && state.context == context;
        return succeeds && slot != 0;
    }
    void service(const std::size_t slot, tactical::SpecialAbilitySlot& state, const std::uint64_t frame) override {
        serviced.push_back(slot);
        schedule_valid = schedule_valid && state.next_service_frame == frame + 1;
    }
    bool target_live(const eawr::sim::EntityId target) const override { return target != 99; }
    void remove_effect(std::size_t, const eawr::sim::EntityId target) override { removed.push_back(target); ++target_checks; }
    void terminate(const std::size_t slot) override { terminated.push_back(slot); }
};

void test_hero_damage_modes() {
    tactical::AbilityProfile falcon;
    falcon.kind = tactical::AbilityKind::invulnerability;
    falcon.expiration_frames = 180;
    falcon.recharge_frames = 1800;
    falcon.modifiers.take_damage = Fixed{};
    falcon.supports_autofire = true;
    auto profile = unit_of(corvette_type, falcon);
    auto state = tactical::initial_abilities(profile);
    expect(tactical::ability_multiplier(profile, state, tactical::AbilityModifier::take_damage) == units(1),
        "WHE-22: inactive Falcon takes ordinary damage");
    expect(tactical::activate_ability(falcon, state.slots[0], {}, 10).changed,
        "WHE-22: Falcon activation succeeds");
    expect(state.slots[0].expires_tick == 190
        && tactical::ability_multiplier(profile, state, tactical::AbilityModifier::take_damage) == Fixed{},
        "WHE-22: active Falcon authors six seconds of zero take damage");
    static_cast<void>(tactical::expire_abilities(profile, state, 189));
    expect(state.slots[0].active, "WHE-22: Falcon stays active until its expiry frame");
    static_cast<void>(tactical::expire_abilities(profile, state, 190));
    expect(!state.slots[0].active && state.slots[0].ready_tick == 1990
        && tactical::ability_multiplier(profile, state, tactical::AbilityModifier::take_damage) == units(1),
        "WHE-22: expiry restores ordinary damage and the generic sixty-second cooldown");
    expect(!tactical::ability_ready(falcon, state.slots[0], {}, 1989)
        && tactical::ability_ready(falcon, state.slots[0], {}, 1990), "WHE-22: exact cooldown boundary");

    auto table = durability();
    auto hull = table.profiles.front();
    hull.max_shields = Fixed{};
    // WHE-51: direct, area, environmental and privileged script delivery all take the mode.
    for (const unsigned kind : {0U, 1U, 2U, 3U}) {
        tactical::Hit hit;
        hit.amount = units(20);
        hit.projectile = kind < 2;
        hit.area = kind == 1;
        hit.kind = kind == 2 ? tactical::HitKind::asteroid : tactical::HitKind::ordinary;
        hit.take_damage_multiplier = Fixed{};
        auto health = tactical::full_durability(hull);
        const auto immune = tactical::apply_hit(hull, *table.damage, health, hit, 0);
        expect(immune && health.hull == hull.max_hull && !immune.value().cancelled,
            "WHE-51: zero mode prevents damage without canceling a projectile blast");
        hit.bypass_take_damage_mode = true;
        const auto bypassed = tactical::apply_hit(hull, *table.damage, health, hit, 1);
        expect(bypassed && health.hull == Fixed::from_raw(hull.max_hull.raw() - units(20).raw()),
            "WHE-51: only the explicit delivery bypass skips the take mode");
    }
    tactical::Hit hit;
    hit.amount = units(20);
    hit.projectile = true;
    hit.cause_damage_multiplier = decimal("2.5");
    hit.take_damage_multiplier = decimal("0.5");
    auto health = tactical::full_durability(hull);
    const auto boosted = tactical::apply_hit(hull, *table.damage, health, hit, 0);
    expect(boosted && health.hull == Fixed::from_raw(hull.max_hull.raw() - units(25).raw()),
        "WHE-51: take and cause are multiplicative modes, separate from fractional bonuses");

    std::vector<std::string> expiry_baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto raw_health = durability();
        raw_health.damage.reset();
        tactical::AbilityTable modes;
        modes.profiles = {unit_of(corvette_type, falcon)};
        modes.humans = {1};
        auto created = tactical::TacticalSession::create(setup(), {}, raw_health, {}, std::nullopt, {}, {}, modes);
        expect(static_cast<bool>(created), "WHE-52: raw-damage Falcon session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1,
            tactical::AbilityPayload{falcon.kind, tactical::AbilityAction::activate}))), "WHE-52: raw Falcon activates");
        for (const auto frame : {1U, 179U, 180U}) {
            expect(static_cast<bool>(session.submit(command(frame, 1, frame, 1,
                tactical::DamagePayload{units(10), tactical::hull_target}))), "WHE-52: raw scripted hit submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (unsigned frame = 0; frame <= 180; ++frame) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WHE-52: raw Falcon step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
        }
        const auto final = session.durability_state(1);
        const auto ability = session.ability_state(1);
        expect(final && final->hull == units(3590) && ability && !ability->slots[0].active
            && ability->slots[0].ready_tick == 1980,
            "WHE-51/52: raw script damage is blocked until exact expiry, then restored with full recharge");
        if (expiry_baseline.empty()) expiry_baseline = hashes;
        else expect(hashes == expiry_baseline, "WHE-52: raw expiry hashes agree on 1/2/4/8 workers");
    }

    // An in-flight shot observes switches at delivery, rather than retaining launch modes.
    std::vector<std::string> baseline;
    for (const unsigned scenario : {0U, 1U, 2U, 3U}) {
        baseline.clear();
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto health_table = durability();
            for (auto& row : health_table.profiles) { row.max_shields = Fixed{}; row.shield_refresh = Fixed{}; }
            tactical::AbilityProfile assault;
            assault.kind = tactical::AbilityKind::power_to_weapons;
            assault.expiration_frames = 21;
            assault.recharge_frames = 50;
            assault.modifiers.cause_damage = units(2);
            tactical::AbilityTable modes;
            modes.profiles = {unit_of(corvette_type, assault), unit_of(frigate_type, falcon)};
            modes.humans = {1, 2};
            tactical::CombatTable combat;
            tactical::CombatProfile shooter;
            shooter.type_id = corvette_type;
            shooter.category_bits = 1;
            shooter.max_attack_distance = units(700);
            tactical::WeaponProfile weapon;
            weapon.range = units(700);
            weapon.min_recharge_hundredths = 0;
            weapon.max_recharge_hundredths = 0;
            weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(700), true, true, {}};
            shooter.weapons = {weapon};
            tactical::CombatProfile target;
            target.type_id = frigate_type; target.category_bits = 2;
            target.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            combat.profiles = {shooter, target};
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}}};
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health_table, {}, std::nullopt, combat, {}, modes);
            expect(static_cast<bool>(created), "WHE-51: damage-mode projectile session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1,
                tactical::AttackPayload{2}))), "WHE-51: object weapon receives an attack order");
            expect(static_cast<bool>(session.submit(command(1, 1, 1, 1,
                tactical::AbilityPayload{assault.kind, tactical::AbilityAction::activate}))), "WHE-51: Assault submits");
            expect(static_cast<bool>(session.submit(command(1, 1, 2, 1,
                tactical::StopPayload{}))), "WHE-51: stop leaves just the first shot in flight");
            if (scenario == 1) expect(static_cast<bool>(session.submit(command(2, 1, 3, 1,
                tactical::AbilityPayload{assault.kind, tactical::AbilityAction::deactivate}))), "WHE-51: early switch submits");
            if (scenario == 2) {
                expect(static_cast<bool>(session.submit(command(1, 2, 0, 2,
                    tactical::AbilityPayload{falcon.kind, tactical::AbilityAction::activate}))), "WHE-22: Falcon submits");
                expect(static_cast<bool>(session.submit(command(2, 2, 1, 2,
                    tactical::DamagePayload{units(100), tactical::hull_target}))), "WHE-51: privileged script hit submits");
            }
            if (scenario == 3) expect(static_cast<bool>(session.submit(command(2, 1, 3, 1,
                tactical::DamagePayload{units(100000), tactical::hull_target}))), "WHE-51: source removal submits");
            std::vector<std::string> hashes;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            unsigned hits = 0;
            for (unsigned tick = 0; tick < 40; ++tick) {
                auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "WHE-51: projectile step succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                for (const auto& event : stepped.value().snapshot->combat_events())
                    if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
            }
            expect(hits == 1, "WHE-51: the fixture delivers exactly one in-flight hit");
            const auto final = session.durability_state(2);
            expect(final && final->hull == units(3600 - (scenario == 0 ? 20 : scenario == 2 ? 0 : 10)),
                "WHE-51: current Assault/Falcon modes affect projectile and script damage at delivery");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "WHE-51: damage-mode hashes agree on 1/2/4/8 workers");
            const auto recorded = session.record();
            const auto bytes = tactical::write_replay(recorded);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == recorded, "WHE-22: invulnerability kind round-trips through replay commands");
        }
    }
}

void test_impact_shooter_bonus() {
    // WPR-51/WCC-44, EUS-16: instance damage, live shooter bonus and victim defense
    // are independent. Exercise both weapon fire and an explicit stationary launch.
    for (const unsigned route : {0U, 1U, 2U}) for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U}) {
        const bool spawned = route != 0, delayed = route == 2;
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; row.hardpoints.clear(); }
            tactical::CombatProfile shooter;
            shooter.type_id = corvette_type; shooter.max_attack_distance = units(700);
            tactical::WeaponProfile weapon;
            weapon.range = units(700); weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(700), true, true, {}};
            if (!spawned) shooter.weapons = {weapon};
            tactical::CombatProfile target;
            target.type_id = frigate_type;
            target.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            tactical::CombatProfile provider; provider.type_id = plain_type;
            tactical::CombatTable combat; combat.profiles = {shooter, target, provider};
            tactical::AbilityTable abilities;
            if (spawned) {
                tactical::AbilityProfile bomb;
                bomb.kind = tactical::AbilityKind::harmonic_bomb; bomb.recharge_frames = 100;
                bomb.spawned = tactical::SpawnedAbilityProfile{};
                bomb.spawned->type = 99; bomb.spawned->countdown_frames = 5;
                bomb.spawned->damage_type = 0;
                bomb.spawned->blast.damage = units(10); bomb.spawned->blast.radius = units(400);
                bomb.spawned->blast.max_delay = delayed ? decimal("0.2") : Fixed{};
                abilities.profiles = {unit_of(corvette_type, bomb)};
            }
            tactical::EconomyRules economy;
            tactical::CommandBonusProfile bonus;
            bonus.type = plain_type; bonus.bonus.applicable = {corvette_type};
            bonus.bonus.percentages[1] = decimal("0.5");
            tactical::CommandBonusProfile defense;
            defense.type = plain_type; defense.slot = 1; defense.bonus.applicable = {frigate_type};
            defense.bonus.percentages[4] = decimal("0.25");
            economy.command_bonuses = {bonus, defense};
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}}};
            if (scenario != 0 && scenario != 4)
                initial.units.push_back({3, plain_type, 1, at(3000, 0), math::identity_quat(), {}});
            if (scenario == 5 || scenario == 6)
                initial.units.push_back({4, plain_type, 2, at(3000, 1000), math::identity_quat(), {}});
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health, {}, std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "EUS-16: impact bonus fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            if (spawned) expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                tactical::AbilityPayload{tactical::AbilityKind::harmonic_bomb, tactical::AbilityAction::activate}))),
                "EUS-16: explicit launch submits");
            else {
                expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::AttackPayload{2}))),
                    "EUS-16: ordinary launch submits");
                expect(static_cast<bool>(world.submit(command(1, 1, 1, 1, tactical::StopPayload{}))),
                    "EUS-16: fixture stops after one shot");
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            unsigned hits = 0;
            for (unsigned frame = 0; frame < 30; ++frame) {
                if (frame == 2) {
                    if (scenario == 2 || scenario == 3 || scenario == 5)
                        expect(static_cast<bool>(world.stage_remove(scenario == 2 ? 3 : scenario == 3 ? 1 : 4)),
                            "EUS-16: source/bonus/defense removal before contact succeeds");
                    if (scenario == 4) expect(static_cast<bool>(world.stage_spawn(
                        {3, plain_type, 1, at(3000, 0), math::identity_quat(), {}})),
                        "EUS-16: new damage bonus before contact succeeds");
                }
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "EUS-16: impact bonus step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                if (frame == 0 && !spawned) {
                    const auto shots = step.value().snapshot->projectiles();
                    expect(shots.size() == 1 && shots.front().damage == units(10),
                        "EUS-16: projectile stores base instance damage without baking shooter bonus");
                }
                for (const auto& event : step.value().snapshot->combat_events())
                    if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
            }
            const auto final = world.durability_state(2);
            // EUS-15/WAD-26: queued raw damage drops shooter modifiers; recipient defense
            // still resolves at delivery. EUS-16 keeps the current bonus for immediate hits.
            const bool boosted = !delayed && (scenario == 1 || scenario == 4 || scenario == 5 || scenario == 6);
            const auto amount = scenario == 6 ? decimal(delayed ? "7.5" : "11.25") : units(boosted ? 15 : 10);
            expect(final && final->hull == Fixed::from_raw(units(3600).raw() - amount.raw()) && (spawned || hits == 1),
                delayed ? "EUS-15/WAD-26: queued amount drops source bonus and reads current defense"
                    : "EUS-16: contact uses current bonus once, removed shooter contributes none, current defense survives");
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "EUS-16: impact bonus hashes agree at 1/2/4/8 workers");
        }
    }
}

void test_concentrate_damage() {
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
        const bool speed_case = scenario >= 6;
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = tactical::AbilityKind::concentrate_fire;
            ordinary.expiration_frames = 100;
            if (scenario == 6) ordinary.expiration_frames = 8;
            ordinary.recharge_frames = 20;
            ordinary.effective_radius = units(6000);
            ordinary.gui_activated_ability_name = "focus";
            auto hero = unit_of(plain_type, ordinary);
            tactical::SpecialAbilityProfile special;
            special.name = "focus"; special.kind = tactical::SpecialAbilityKind::concentrate_fire;
            special.style = tactical::SpecialActivationStyle::user_input; special.service_interval = 1;
            special.target_damage_increase = decimal("0.5"); special.filter.applicable_categories = 2;
            if (speed_case) special.target_speed_decrease = decimal("0.4");
            hero.special = {special};
            tactical::AbilityTable abilities;
            abilities.profiles = {hero};
            tactical::CombatTable combat;
            tactical::CombatProfile shooter;
            shooter.type_id = corvette_type; shooter.category_bits = 1; shooter.max_attack_distance = units(700);
            tactical::WeaponProfile weapon;
            weapon.range = units(700); weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(10), 0, units(25), units(700), true, true, {}};
            shooter.weapons = {weapon};
            tactical::CombatProfile target;
            target.type_id = frigate_type; target.category_bits = 2;
            target.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            tactical::CombatProfile source; source.type_id = plain_type; source.category_bits = 4;
            combat.profiles = {shooter, target, source};
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}},
                {3, plain_type, 1, at(3000, 0), math::identity_quat(), {}}};
            tactical::EconomyRules economy;
            if (scenario == 4 || scenario == 5) {
                tactical::CommandBonusProfile bonus;
                bonus.type = frigate_type; bonus.apply_to_self = true;
                bonus.bonus.applicable = {frigate_type};
                bonus.bonus.stacking_category = scenario == 4 ? 0 : 1;
                bonus.bonus.percentages[4] = decimal("0.25");
                economy.command_bonuses = {bonus};
            }
            auto movement = speed_case ? motion() : tactical::MotionTable{};
            for (auto& row : movement.profiles) { row.acceleration = row.max_speed; row.deceleration = row.max_speed; }
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}},
                health, movement, std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "WHE-25: concentrate-fire damage session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::AttackPayload{2}))), "single-shot fixture attack submits");
            if (scenario != 0) expect(static_cast<bool>(session.submit(command(1, 1, 1, 3,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::activate, 2}))), "concentrate fire locks target");
            expect(static_cast<bool>(session.submit(command(1, 1, 2, 1, tactical::StopPayload{}))), "single-shot fixture stops firing");
            if (scenario == 2) expect(static_cast<bool>(session.submit(command(2, 1, 3, 3,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::deactivate}))), "release submits before impact");
            if (scenario == 3) expect(static_cast<bool>(session.submit(command(2, 1, 3, 3,
                tactical::DamagePayload{units(100000), tactical::hull_target}))), "source death submits before impact");
            if (scenario == 7) expect(static_cast<bool>(session.submit(command(8, 1, 3, 3,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::deactivate}))), "speed effect releases before impact");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            if (speed_case) expect(static_cast<bool>(session.submit(command(0, 2, 0, 2,
                tactical::MovePayload{at(6000, 0)}))), "WHE-25: target moves through concentrate-fire speed changes");
            std::vector<std::string> hashes;
            unsigned hits = 0;
            for (unsigned frame = 0; frame < 30; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "concentrate-fire damage step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                if (speed_case && (frame == 3 || frame == 12)) {
                    const auto* profile = movement.find(frigate_type);
                    const auto state = session.motion_state(2);
                    const auto sampled = state ? tactical::sample_motion(*state, session.completed_tick(),
                        state->start_position, state->start_yaw) : eawr::core::Result<tactical::MotionSample>::failure({});
                    const auto factor = frame == 3 ? Fixed::from_raw(one - decimal("0.4").raw()) : units(1);
                    const auto expected_speed = profile ? math::multiply(profile->max_speed, factor)
                        : eawr::core::Result<Fixed>::failure({});
                    expect(sampled && expected_speed && sampled.value().speed == expected_speed.value(),
                        "WHE-25: target slows by the authored fraction and regains speed when the effect expires; frame="
                            + std::to_string(frame) + ", scenario=" + std::to_string(scenario)
                            + ", expected=" + std::to_string(expected_speed ? expected_speed.value().raw() : -1)
                            + ", sampled=" + std::to_string(sampled ? sampled.value().speed.raw() : -1));
                }
                for (const auto& event : step.value().snapshot->combat_events()) if (event.kind == tactical::CombatEventKind::projectile_hit) ++hits;
            }
            const auto final = session.durability_state(2);
            const auto expected = scenario == 1 ? decimal("15") : scenario == 4 ? decimal("7.5")
                : scenario == 5 ? decimal("12.5") : units(10);
            expect(hits == 1 && final && final->hull == Fixed::from_raw(units(3600).raw() - expected.raw()),
                "WHE-25/55: target defense applies at delivery; release/death restore; same-category positive wins, distinct categories add");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "concentrate-fire damage hashes equal at 1/2/4/8 workers");
            const auto record = session.record();
            const auto encoded = tactical::write_replay(record);
            const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "concentrate-fire targeted commands round-trip through the existing opcode");
        }
    }
}

void test_concentrate_fire() {
    using K = tactical::AbilityKind;
    tactical::AbilityProfile ordinary;
    ordinary.kind = K::concentrate_fire;
    ordinary.effective_radius = units(6000);
    ordinary.expiration_frames = 3;
    ordinary.recharge_frames = 6;
    ordinary.gui_activated_ability_name = "focus";
    tactical::SpecialAbilityProfile nested;
    nested.name = "focus";
    nested.kind = tactical::SpecialAbilityKind::concentrate_fire;
    nested.style = tactical::SpecialActivationStyle::user_input;
    nested.service_interval = 1;
    nested.filter.applicable_categories = 1;
    nested.target_damage_increase = decimal("0.5");
    auto hero = unit_of(corvette_type, ordinary);
    hero.special = {nested};
    tactical::AbilityTable abilities;
    abilities.profiles = {hero};
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type, xwing_type}) {
        tactical::CombatProfile item;
        item.type_id = type;
        item.category_bits = 1;
        combat.profiles.push_back(item);
    }
    std::sort(combat.profiles.begin(), combat.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    auto battle = setup();
    battle.players.push_back({3, 1, 1, tactical::player_flag_commandable});
    battle.units = {
        {1, corvette_type, 1, at(-20000, 0), math::identity_quat(), {}},
        {2, frigate_type, 1, at(10000, 100), math::identity_quat(), {}},
        {3, plain_type, 2, at(10000, 0), math::identity_quat(), {}},
        {4, frigate_type, 3, at(10000, 200), math::identity_quat(), {}},
        {5, frigate_type, 1, at(-19900, 0), math::identity_quat(), {}},
        {6, frigate_type, 1, at(16001, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, at(10000, 300), math::identity_quat(), {}},
        {11, xwing_type, 1, at(10000, 300), math::identity_quat(), {}},
        {12, xwing_type, 1, at(10000, 310), math::identity_quat(), {}},
    };
    battle.squadrons = {{10, {11, 12}}};
    // Spread a large unrelated census across far-away cells: recruitment work stays local.
    for (unsigned i = 0; i < 512; ++i) battle.units.push_back({100 + i, frigate_type, 1,
        at(100000 + 1000 * i, 100000), math::identity_quat(), {}});
    auto movement = motion();
    movement.squadrons = hangar_motion().squadrons;
    movement.squadrons.spawners.clear();
    auto health = durability();
    health.profiles.push_back({xwing_type, units(90), units(5), false, {}});
    std::sort(health.profiles.begin(), health.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(battle, {}, health, movement, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "WHE-24: concentrate-fire session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        const auto activate = tactical::AbilityPayload{K::concentrate_fire, tactical::AbilityAction::activate, 3};
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, activate))), "targeted concentrate command submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (unsigned tick = 0; tick < 5; ++tick) {
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "concentrate-fire step succeeds");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            const auto state = session.ability_state(1);
            if (tick == 0) {
                expect(state && state->slots[0].active && state->slots[0].target == 3
                    && state->concentrate_recruits == std::vector<eawr::sim::EntityId>{2, 10},
                    "WHE-24: target-centred own units; source/allied owner/out-of-range excluded; craft promote once");
                const auto work = session.tick_work();
                expect(work.concentrate_queries == 1 && work.concentrate_candidates < 30 && work.concentrate_batches == 1,
                    "one bounded spatial recruitment query and one attack batch, independent of remote census");
                const auto live_units = session.units();
                const auto ordered = std::find_if(live_units.begin(), live_units.end(), [](const auto& unit) { return unit.entity_id == 2; });
                expect(ordered != live_units.end() && ordered->order.kind == tactical::OrderKind::attack && ordered->order.target == 3,
                    "recruited ships use the ordinary attack/approach path");
            } else {
                expect(session.tick_work().concentrate_queries == 0, "recruitment is activation-only");
                if (tick >= 3) expect(state && !state->slots[0].active && state->slots[0].target == 0
                    && state->concentrate_recruits.empty() && state->special.slots[0].targets.empty(),
                    "WHE-25: expiry releases the target and recruitment ledgers");
            }
        }
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "concentrate state/events equal at 1/2/4/8 workers");
    }
}

void test_concentrate_sparse_lifecycle() {
    for (const unsigned scenario : {0U, 1U, 2U}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(-2200, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(0, 0), math::identity_quat(), {}},
                {3, corvette_type, 1, at(100, 600), math::identity_quat(), {}},
                {4, 99, 2, at(0, 0), math::identity_quat(), {}}};
            tactical::AbilityProfile ordinary;
            ordinary.kind = tactical::AbilityKind::concentrate_fire;
            ordinary.expiration_frames = 20; ordinary.recharge_frames = 20;
            ordinary.effective_radius = units(6000); ordinary.gui_activated_ability_name = "focus";
            auto source = unit_of(plain_type, ordinary);
            tactical::SpecialAbilityProfile nested;
            nested.name = "focus"; nested.kind = tactical::SpecialAbilityKind::concentrate_fire;
            nested.style = tactical::SpecialActivationStyle::user_input; nested.service_interval = 1;
            nested.target_damage_increase = decimal("0.5"); nested.filter.applicable_categories = 1;
            source.special = {nested};
            tactical::AbilityTable abilities; abilities.profiles = {source};
            tactical::CombatTable combat;
            for (const auto type : {corvette_type, frigate_type, plain_type}) {
                tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 1;
                combat.profiles.push_back(profile);
            }
            auto health = durability();
            for (auto& profile : health.profiles) { profile.max_shields = {}; profile.shield_refresh = {}; }
            auto movement = motion();
            if (scenario == 0) {
                movement.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
                    decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
                tactical::Footprint victim; victim.type_id = frigate_type; victim.layer = tactical::SpaceLayer::frigate;
                victim.radius = units(10); victim.asteroid_damage = true; victim.locomotor = false;
                tactical::Footprint field; field.type_id = 99; field.layer = tactical::SpaceLayer::static_object;
                field.asteroid_field = true; field.obstacle = true; field.radius = units(100);
                movement.footprints = {victim, field};
                health.damage->asteroid_damage = units(20); health.damage->asteroid_rate = units(1);
            }
            tactical::EconomyRules economy;
            economy.players = {{1, units(100), 25, false, {}, 1, 3}};
            tactical::BuildOption option;
            option.type = scenario == 0 ? 90 : corvette_type;
            option.kind = scenario == 0 ? tactical::BuildKind::upgrade : tactical::BuildKind::unit;
            option.queue = scenario == 0 ? tactical::BuildQueue::upgrades : tactical::BuildQueue::units;
            option.price = units(10);
            option.build_frames = 2; option.ai_build_frames = 2; option.available = true;
            economy.menus = {{plain_type, 1, {option}}};
            economy.footprints = {{corvette_type, std::nullopt, {}}};
            if (scenario == 0) {
                tactical::UpgradeBonus bonus; bonus.applicable = {corvette_type}; bonus.percentages[5] = decimal("0.25");
                economy.upgrades = {{90, false, false, 0, {bonus}}};
            }
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt,
                combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "concentrate sparse lifecycle fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::BuyPayload{option.type}))), "lifecycle build submits");
            if (scenario != 2) expect(static_cast<bool>(session.submit(command(0, 1, 2, 1,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::activate, 2}))), "lifecycle focus submits");
            if (scenario != 0) {
                tactical::PlayerCommand arrival{{3, 1, scenario == 2 ? 1U : 3U}, {}, tactical::ReinforcePayload{corvette_type,
                    {Fixed::from_raw(tactical::arrival_tail(0).raw() + units(100).raw()), units(200), {}}}};
                expect(static_cast<bool>(session.submit(arrival)), "same-frame arrival submits");
            }
            if (scenario == 2) expect(static_cast<bool>(session.submit(command(3, 1, 2, 1,
                tactical::AbilityPayload{ordinary.kind, tactical::AbilityAction::activate, 2}))), "lifecycle focus submits");
            if (scenario == 1) expect(static_cast<bool>(session.submit(command(3, 2, 0, 2,
                tactical::DamagePayload{units(100000)}))), "target death submits alongside birth");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 6; ++frame) {
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "sparse lifecycle step succeeds");
                if (!step) { std::cerr << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                if (scenario == 0 && frame >= 3) {
                    expect(step.value().asteroid_impacts.size() == 1 && session.durability_state(2)->hull == units(3600 - 30 * (frame + 1)),
                        "station bonus rebuild preserves active concentrate defense for cached asteroid damage");
                }
                if (frame == 3 && scenario != 0) {
                    const auto state = session.ability_state(1);
                    expect(session.arrivals().size() == 1, "fixture actually created a movement-locked reinforcement");
                    if (scenario == 1) expect(session.units().size() == initial.units.size() && state && !state->slots[0].active
                        && state->concentrate_recruits.empty() && state->special.slots[0].targets.empty(),
                        "target death plus birth at unchanged census releases concentrate in the same tick");
                    else expect(state && state->slots[0].active && state->concentrate_recruits == std::vector<eawr::sim::EntityId>{3},
                        "source inside radius and newly arriving movement-locked ship are excluded from the batch");
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "sparse lifecycle hashes equal at 1/2/4/8 workers");
        }
    }
}

void test_nested_special_handlers() {
    using Kind = tactical::SpecialAbilityKind;
    using Style = tactical::SpecialActivationStyle;
    tactical::SpecialAbilityProfile profile;
    profile.name = "typed-handler";
    profile.kind = Kind::tractor_beam;
    profile.style = Style::user_input;
    profile.initially_enabled = true;
    profile.service_interval = tactical::special_service_interval(profile.kind);
    auto profiles = std::vector(3, profile);
    profiles[1].causes_despawn = true;
    profiles[2].service_interval = 0;
    tactical::SpecialActivationContext context;
    context.owner = 7; context.target = 8;
    auto state = tactical::initial_special_abilities(profiles);
    SpecialProbe probe;
    expect(tactical::activate_special_abilities(profiles, state, Style::user_input, context, true, probe) == 1
        && probe.applied == std::vector<std::size_t>{0, 1} && state.slots[1].despawn_success,
        "WHE-10: first success follows declared order; successful despawn is recorded");
    expect(std::all_of(state.slots.begin(), state.slots.end(), [](const auto& slot) { return !slot.context; }),
        "WHE-10: activation context is cleared after failed and successful Apply");
    state = tactical::initial_special_abilities(profiles); probe.applied.clear();
    expect(tactical::activate_special_abilities(profiles, state, Style::user_input, context, false, probe) == 2
        && probe.applied == std::vector<std::size_t>{0, 1, 2}, "WHE-10: activate-all keeps visiting after success");
    for (unsigned gate = 0; gate < 7; ++gate) {
        state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
        auto requested = Style::user_input;
        if (gate == 0) requested = Style::space_automatic;
        if (gate == 1) probe.is_ready = false;
        if (gate == 2) probe.mode_ok = false;
        if (gate == 3) for (auto& slot : state.slots) slot.enabled = false;
        if (gate == 4) probe.target_ok = false;
        if (gate == 5) probe.is_available = false;
        if (gate == 6) probe.succeeds = false;
        expect(tactical::activate_special_abilities(profiles, state, requested, context, false, probe) == 0
            && std::none_of(state.slots.begin(), state.slots.end(), [](const auto& slot) { return slot.despawn_success || slot.context; }),
            "WHE-10: each activation gate blocks success and despawn");
        if (gate != 6) expect(probe.applied.empty(), "WHE-10: failed gates do not reach Apply");
    }
    tactical::SpecialAbilityFilter filter;
    filter.applicable_types = {42}; filter.excluded_types = {42, 43};
    filter.applicable_categories = 1; filter.excluded_categories = 2;
    expect(tactical::special_type_matches(filter, 42, 2), "WHE-14: explicit type bypasses categories and both exclusions");
    expect(!tactical::special_type_matches(filter, 43, 1) && !tactical::special_type_matches(filter, 44, 3)
        && !tactical::special_type_matches(filter, 45, 0) && tactical::special_type_matches(filter, 45, 1),
        "WHE-14: category overlap is required, exclusions apply and named variants are not expanded");
    state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
    state.slots[0].next_service_frame = 100; state.slots[1].enabled = false;
    expect(tactical::service_special_abilities(profiles, state, context, 99, probe) == 0, "WHE-09: not due means no call");
    expect(tactical::service_special_abilities(profiles, state, context, 900, probe) == 1
        && state.slots[0].next_service_frame == 901 && probe.serviced == std::vector<std::size_t>{0},
        "WHE-09: missed service invokes once and schedules current plus interval; disabled/zero interval skip");
    expect(tactical::service_special_abilities(profiles, state, context, 900, probe) == 0,
        "WHE-09: no second invocation at the same frame");
    for (unsigned exit = 0; exit < 5; ++exit) {
        state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
        auto blocked = context;
        if (exit == 0) blocked.owner_exists = false;
        if (exit == 1) blocked.type_exists = false;
        if (exit == 2) blocked.death_clone = true;
        if (exit == 3) blocked.map_editor = true;
        if (exit == 4) for (auto& slot : state.slots) slot.cancelled = true;
        expect(tactical::service_special_abilities(profiles, state, blocked, 900, probe) == 0 && probe.serviced.empty(),
            "WHE-09: absent owner/type, clone, editor and all-cancelled exits");
    }
    state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{};
    state.slots[0].targets = {8, 9, 99}; state.slots[1].despawn_success = true; state.slots[1].targets = {10};
    state.slots[2].enabled = false;
    tactical::delete_special_target(state, 9);
    tactical::delete_special_owner(profiles, state, context.mode, probe);
    expect(state.service_cancelled && state.slots[0].targets.empty() && state.slots[1].targets == std::vector<eawr::sim::EntityId>{10}
        && probe.removed == std::vector<eawr::sim::EntityId>{8} && probe.terminated == std::vector<std::size_t>{0},
        "WHE-12: target deletion forgets IDs; owner deletion removes live effects and skips disabled/successful-despawn handlers");
    expect(tactical::service_special_abilities(profiles, state, context, 901, probe) == 0, "WHE-12: deleted owner cancels service");
    state = tactical::initial_special_abilities(profiles); probe = SpecialProbe{}; probe.mode_ok = false;
    tactical::delete_special_owner(profiles, state, context.mode, probe);
    expect(probe.terminated.empty(), "WHE-12: owner termination requires an appropriate mode");

    // Immutable copied inputs and disjoint owner staging; commit encoded records in owner order.
    const std::vector inputs(64, tactical::initial_special_abilities(profiles));
    std::vector<std::uint8_t> baseline;
    std::vector<std::uint8_t> session_baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        eawr::platform::ThreadWorkerAdapter executor(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::always_pool);
        auto staged = inputs;
        std::vector<std::size_t> calls(inputs.size());
        expect(static_cast<bool>(executor.execute_phase("nested-special-handlers", 16, [&](const std::size_t partition) {
            for (auto owner = partition; owner < staged.size(); owner += 16) {
                SpecialProbe local;
                auto local_context = context; local_context.owner = owner + 1;
                calls[owner] = tactical::service_special_abilities(profiles, staged[owner], local_context, 900, local);
                calls[owner] += tactical::activate_special_abilities(profiles, staged[owner], Style::user_input, local_context, false, local);
                if (!local.context_valid || !local.schedule_valid) calls[owner] = 0;
            }
        })), "nested handler partitioned phase succeeds");
        std::vector<std::uint8_t> bytes;
        for (const auto& owner : staged) tactical::append_special_abilities(bytes, owner);
        expect(std::all_of(calls.begin(), calls.end(), [](const auto count) { return count == 4; }),
            "nested handler work is bounded by two due and three declared activation slots per owner");
        if (workers == 1) baseline = bytes;
        else expect(bytes == baseline, "nested handler state is identical at 1/2/4/8 workers");
        auto table = abilities({1}); table.profiles.front().special = profiles;
        table.autofire_defaults = {1};
        table.profiles.front().abilities.front().supports_autofire = true;
        auto created = tactical::TacticalSession::create(setup(), {}, durability(), motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "nested metadata session validates");
        if (!created) continue;
        auto world = std::move(created).value();
        const auto combined = world.ability_state(1);
        expect(combined && combined->slots.front().autofire && combined->special.slots.size() == 3,
            "AB-45 and WHE-09: creation initializes autofire and nested slots together");
        expect(static_cast<bool>(world.step(executor)), "nested metadata participates in staged session copies");
        const auto session_bytes = world.canonical_state_bytes();
        constexpr std::string_view tag = "SPAB";
        expect(std::search(session_bytes.begin(), session_bytes.end(), tag.begin(), tag.end()) != session_bytes.end()
            && world.ability_state(1)->special.slots.size() == 3, "SPAB binds nested owner state after ordered commit");
        if (workers == 1) session_baseline = session_bytes;
        else expect(session_bytes == session_baseline, "SPAB session state is identical at 1/2/4/8 workers");
    }
}

void test_barrage_proxy_and_override() {
    tactical::AbilityProfile barrage;
    barrage.kind = tactical::AbilityKind::barrage;
    barrage.expiration_frames = 300;
    barrage.recharge_frames = 1200;
    barrage.modifiers.fire_rate = units(3);
    barrage.fixed_inaccuracy = units(320);
    barrage.target_z_offset = units(-150);
    barrage.barrage_target_type = 77;
    const tactical::AbilityTable ability_data{{unit_of(corvette_type, barrage)}, {1}};
    tactical::CombatTable combat;
    tactical::CombatProfile ship;
    ship.type_id = corvette_type; ship.category_bits = 1;
    tactical::WeaponProfile weapon;
    weapon.range = units(1500); weapon.pulse_count = 5; weapon.pulse_delay_frames = 30;
    tactical::ShotProfile ordinary;
    ordinary.damage = units(5); ordinary.damage_type = 0;
    ordinary.speed = units(7); ordinary.max_travel = units(1500);
    ordinary.energy_per_shot = units(20);
    weapon.shot = ordinary;
    auto override_shot = ordinary;
    override_shot.energy_per_shot = {};
    override_shot.damage = units(9); override_shot.speed = units(12);
    override_shot.blast.damage = units(150); override_shot.blast.radius = units(200);
    override_shot.blast.dropoff = true; override_shot.blast.tiers = 5;
    tactical::FlightProfile flight;
    flight.kind = tactical::FlightKind::rocket; flight.target_radius = true;
    flight.authored_distance = units(3000); flight.curve_distance = units(500); flight.straight_distance = units(500);
    override_shot.flight = flight;
    weapon.barrage_shot = override_shot;
    ship.weapons.push_back(weapon);
    tactical::CombatProfile marker; marker.type_id = 77; marker.category_bits = 1;
    combat.profiles = {ship, marker};
    auto start = setup(); start.units[0].position = at(0, 0);
    const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(2000)}};
    const auto activate = command(0, 1, 1, 1, tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(500, 0)});
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            combat, {}, ability_data);
        expect(static_cast<bool>(created), "WAD-38: barrage session binds");
        if (!created) return;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(activate)), "WAD-38: point command submits");
        expect(static_cast<bool>(session.submit(command(2, 1, 2, 1,
            tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(700, 0)}))),
            "WAD-38: repeated activation submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::size_t override_fired = 0;
        for (std::uint64_t tick = 0; tick < 305; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WAD-38: barrage step succeeds");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_sha256);
            if (tick == 0 || tick == 2) {
                const auto units_now = session.units();
                const auto proxy = std::find_if(units_now.begin(), units_now.end(),
                    [](const tactical::UnitState& unit) { return unit.barrage_source == 1; });
                expect(proxy != units_now.end() && proxy->owner == 2 && proxy->position == at(500, 0, -150),
                    "WAD-38: enemy proxy retains Z offset; repeated activation does not replace it");
                expect(units_now.size() == start.units.size() + 1, "WAD-38: one live proxy");
                expect(proxy == units_now.end() || !session.durability_state(proxy->entity_id),
                    "WAD-38: immune marker has no damageable hull");
            }
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                ++override_fired;
                expect(event.outcome == tactical::fired_barrage_shot, "WAD-38: override event uses ordinary fire");
                expect(std::abs(event.aim.x.raw() - units(500).raw()) <= units(320).raw()
                    && std::abs(event.aim.y.raw()) <= units(320).raw()
                    && std::abs(event.aim.z.raw() - units(-150).raw()) <= units(320).raw(),
                    "WAD-38: fixed scatter bounds are independent of travel range");
            }
            for (const auto& shot : stepped.value().snapshot->projectiles()) {
                expect(shot.damage == units(9) && shot.speed == units(12)
                    && shot.blast.damage == units(150) && shot.blast.radius == units(200)
                    && shot.blast.dropoff && shot.blast.tiers == 5 && shot.flight
                    && shot.flight->profile.authored_distance == units(3000),
                    "WAD-38: override keeps inherited area budget and separate authored flight distance");
            }
        }
        expect(override_fired > 0, "WAD-38: override uses its zero energy cost instead of the ordinary shot's cost");
        const auto state = session.ability_state(1);
        expect(state && !state->slots[0].active && state->slots[0].target == 0
            && state->slots[0].ready_tick == 1500, "WAD-38: expiry clears proxy/override and starts full recharge");
        expect(session.units().size() == start.units.size(), "WAD-38: expired proxy is removed");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "WAD-38: proxy/override hashes match worker counts");
        auto encoded = tactical::write_replay(session.record());
        auto parsed = encoded ? tactical::parse_replay(encoded.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == session.record(), "WAD-38: point commands round-trip without changing entity-target codec");
        if (parsed) {
            auto playback = tactical::TacticalSession::from_replay(parsed.value(), sensors, durability(), motion(),
                std::nullopt, combat, {}, ability_data);
            expect(static_cast<bool>(playback), "WAD-38: retained point replay binds");
            if (playback) {
                std::vector<std::string> replay_hashes;
                while (playback.value().completed_tick() < parsed.value().final_tick_count) {
                    const auto frame = playback.value().step(executor);
                    expect(static_cast<bool>(frame), "WAD-38: point replay step succeeds");
                    if (!frame) break;
                    replay_hashes.push_back(frame.value().state_sha256);
                }
        expect(replay_hashes == hashes, "WAD-38: recorded point activation/expiry reproduces every hash");
            }
        }
    }
    {
        auto costly = combat;
        costly.profiles[0].weapons[0].shot->energy_per_shot = {};
        costly.profiles[0].weapons[0].barrage_shot->energy_per_shot = units(20);
        auto blocked = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            costly, {}, ability_data);
        expect(static_cast<bool>(blocked), "WAD-38: costly override session binds");
        if (blocked) {
            expect(static_cast<bool>(blocked.value().submit(activate)), "WAD-38: costly override activates");
            const eawr::platform::ThreadWorkerAdapter executor(1);
            for (std::uint64_t frame = 0; frame < 10; ++frame) {
                const auto stepped = blocked.value().step(executor);
                expect(static_cast<bool>(stepped), "WAD-38: costly override step succeeds");
                if (!stepped) break;
                expect(std::none_of(stepped.value().snapshot->combat_events().begin(),
                    stepped.value().snapshot->combat_events().end(), [](const tactical::CombatEvent& event) {
                        return event.shooter == 1 && event.kind == tactical::CombatEventKind::weapon_fired;
                    }), "WAD-38: a free ordinary shot cannot bypass the selected override's energy cost");
            }
        }
    }
    auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
        combat, {}, ability_data);
    if (!created) return;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.submit(command(0, 1, 1, 1,
        tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(9000, 0)}))), "WAD-38: fogged command submits");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    auto rejected = session.step(executor);
    expect(rejected && rejected.value().snapshot->events()[0].reason == tactical::RejectReason::invalid_position
        && session.units().size() == start.units.size(), "WAD-38: fogged point creates no proxy");
    expect(static_cast<bool>(session.submit(command(1, 1, 2, 1,
        tactical::AreaAbilityPayload{tactical::AbilityKind::barrage, at(500, 0)}))), "WAD-38: visible point submits");
    expect(static_cast<bool>(session.submit(command(2, 1, 3, 1,
        tactical::AbilityPayload{tactical::AbilityKind::barrage, tactical::AbilityAction::deactivate}))),
        "WAD-38: cancellation submits");
    expect(static_cast<bool>(session.step(executor)) && static_cast<bool>(session.step(executor)),
        "WAD-38: activation/cancellation step");
    const auto canceled = session.ability_state(1);
    expect(canceled && !canceled->slots[0].active && canceled->slots[0].target == 0
        && session.units().size() == start.units.size(), "WAD-38: cancellation removes proxy without a dangling override");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto mortal = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            combat, {}, ability_data);
        expect(static_cast<bool>(mortal), "WAD-38: source-death session binds");
        if (!mortal) continue;
        expect(static_cast<bool>(mortal.value().submit(activate)), "WAD-38: source activates before death");
        expect(static_cast<bool>(mortal.value().submit(command(1, 1, 2, 1,
            tactical::DamagePayload{units(10000)}))), "WAD-38: lethal source damage submits");
        const eawr::platform::ThreadWorkerAdapter death_executor(workers);
        expect(static_cast<bool>(mortal.value().step(death_executor))
            && static_cast<bool>(mortal.value().step(death_executor)), "WAD-38: lethal source damage steps");
        const auto survivors = mortal.value().units();
        expect(std::none_of(survivors.begin(), survivors.end(), [](const tactical::UnitState& unit) {
            return unit.entity_id == 1 || unit.barrage_source == 1;
        }), "WAD-38: source death removes its target proxy in the same ordered commit");
    }
}

void test_squadron_reveal_ping() {
    for (const bool blocked : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup initial;
            initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
            initial.units = {{2, xwing_type, 1, at(0, 0), math::identity_quat(), {}},
                {3, xwing_type, 1, at(-10, 10), math::identity_quat(), {}},
                {10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}}};
            initial.squadrons = {{10, {2, 3}}};
            tactical::AbilityProfile ability;
            ability.kind = tactical::AbilityKind::fow_reveal_ping;
            ability.recharge_frames = 900;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            ability.spawned->type = 99;
            ability.spawned->damage_type = 0;
            ability.spawned->shield_damage = false;
            ability.spawned->reveal_range = units(1000);
            ability.spawned->hitpoint_damage = false;
            tactical::AbilityTable table; table.profiles = {unit_of(xwing_type, ability)};
            if (!blocked && workers == 1) {
                auto no_reveal = table;
                no_reveal.profiles.front().abilities.front().spawned->reveal_range.reset();
                expect(static_cast<bool>(tactical::validate_abilities(no_reveal)),
                    "WFP-02: an authored spawned object without REVEAL does not refuse the ability table");
            }
            auto health = hangar_durability(); health.damage = durability().damage;
            for (auto& profile : health.profiles) { profile.armor_type = 0; profile.shield_armor_type = 0; }
            tactical::FogRules fog; fog.map_left = units(-2000); fog.map_top = units(2000);
            fog.cell_size = units(100); fog.cells_wide = 80; fog.cells_tall = 40;
            fog.service_period = 16; fog.ramp_down_step = 21;
            tactical::CombatTable combat;
            tactical::CombatProfile box; box.type_id = xwing_type;
            box.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
            combat.profiles = {box};
            auto created = tactical::TacticalSession::create(initial, {}, health, hangar_motion(), fog, combat, {}, table);
            expect(static_cast<bool>(created), "AB-15/WFP: squadron ping session creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const tactical::AbilityPayload ping{tactical::AbilityKind::fow_reveal_ping,
                tactical::AbilityAction::activate, 0, tactical::no_hardpoint, at(3000, 0)};
            expect(static_cast<bool>(world.submit(command(0, 1, 0, blocked ? 2 : 10, ping))), "AB-15/WFP: first ping queues");
            expect(static_cast<bool>(world.submit(command(1, 1, 1, 10, ping))), "AB-15/WFP: squadron repeat queues");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 211; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "AB-15/WFP: squadron ping steps");
                if (!step) { std::cerr << "frame " << frame << ": " << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                if (frame == 0) {
                    expect(world.projectiles().size() == (blocked ? 1U : 2U), "AB-15/WFP: one ping per acting craft");
                    expect(world.ability_state(2)->slots[0].ready_tick == 900
                        && world.ability_state(3)->slots[0].ready_tick == (blocked ? 0U : 900U),
                        "AB-15/WFP: recharge belongs to acting craft, not container");
                    expect(world.fog_cells()->revealed(0, at(3000, 0)) && !world.fog_cells()->revealed(1, at(3000, 0)),
                        "WFP-01/02: squadron point reveals only to its owner");
                }
                if (frame == 1) {
                    expect(!step.value().diagnostics.empty(), "AB-15/WFP: any unready holder rejects the whole squadron");
                    expect(world.ability_state(3)->slots[0].ready_tick == (blocked ? 0U : 900U),
                        "AB-15/WFP: rejected squadron cannot partially recharge or spawn");
                    const auto terminals = std::count_if(step.value().snapshot->combat_events().begin(),
                        step.value().snapshot->combat_events().end(), [](const auto& event) {
                            return event.kind == tactical::CombatEventKind::projectile_expired && event.aim == at(3000, 0);
                        });
                    expect(terminals == (blocked ? 1 : 2), "WFP-03/WPJ-35: each craft ping publishes one point terminal");
                }
                if (frame >= 1) expect(world.projectiles().empty(), "WFP-03: squadron pings expire at first flight service");
                expect(world.fog_cells()->revealed(0, at(3000, 0)) == (frame < 209), "V-15: overlapping craft reveals regrow normally");
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "AB-15/WFP: squadron ping hashes equal on 1/2/4/8 workers");
            const auto bytes = tactical::write_replay(world.record());
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == world.record(), "AB-15/WFP: squadron point replay round-trips");
        }
    }
}

void test_cluster_bomb_and_reveal_ping() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::cluster_bomb, K::fow_reveal_ping}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}}};
            tactical::AbilityProfile ability;
            ability.kind = kind; ability.recharge_frames = kind == K::cluster_bomb ? 1500 : 900;
            ability.supports_autofire = kind == K::cluster_bomb;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            auto& spawn = *ability.spawned;
            spawn.type = 99; spawn.damage_type = 0; spawn.shield_damage = false;
            if (kind == K::cluster_bomb) {
                spawn.ring_count = 12; spawn.countdown_frames = 45; spawn.speed = units(12); spawn.max_travel = units(1000);
                spawn.blast.damage = units(150); spawn.blast.radius = units(150);
            } else {
                spawn.reveal_range = units(1000); spawn.hitpoint_damage = false;
                // WFP-03: stock motionless, zero-distance profile has no authored lifetime.
            }
            tactical::AbilityTable table; table.profiles = {unit_of(plain_type, ability)};
            tactical::FogRules fog; fog.map_left = units(-2000); fog.map_top = units(2000);
            fog.cell_size = units(100); fog.cells_wide = 80; fog.cells_tall = 40;
            fog.service_period = kind == K::fow_reveal_ping ? 16 : 1;
            fog.ramp_down_step = kind == K::fow_reveal_ping ? 21 : 238;
            tactical::CombatTable combat;
            tactical::CombatProfile collision; collision.type_id = plain_type;
            collision.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            combat.profiles = {collision};
            auto created = tactical::TacticalSession::create(initial, {}, durability(), {}, fog, combat, {}, table);
            expect(static_cast<bool>(created), "WCB/WFP: session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            tactical::AbilityPayload input{kind, tactical::AbilityAction::activate};
            if (kind == K::fow_reveal_ping) input.position = at(3000, 0);
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, input))), "WCB/WFP: activation queues");
            expect(static_cast<bool>(world.submit(command(1, 1, 1, 1, input))), "WCB/WFP: recharge use queues");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < (kind == K::fow_reveal_ping ? 211U : 47U); ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "WCB/WFP: step succeeds");
                if (!stepped) { std::cerr << stepped.error().message << '\n'; break; }
                hashes.push_back(stepped.value().state_sha256);
                if (frame == 0) {
                    const auto projectiles = world.projectiles();
                    expect(projectiles.size() == (kind == K::cluster_bomb ? 12U : 1U), "WCB/WFP: exact spawn count");
                    expect(world.ability_state(1)->slots[0].ready_tick == ability.recharge_frames
                        && !world.ability_state(1)->slots[0].active, "WCB/WFP: instant use starts full recharge");
                    if (kind == K::cluster_bomb && projectiles.size() == 12) {
                        for (std::size_t i = 0; i < projectiles.size(); ++i) {
                            expect(projectiles[i].id == i + 1 && projectiles[i].position == at(0, 0), "WCB-02: ordered launch at ship");
                            expect(projectiles[i].yaw == units(static_cast<std::int64_t>(i) * 30), "WCB-02: twelve 30-degree headings");
                        }
                    } else if (kind == K::fow_reveal_ping) {
                        expect(world.fog_cells()->revealed(0, at(3000, 0)), "WFP-01/02: unlimited click reveals for owner");
                        expect(!world.fog_cells()->revealed(1, at(3000, 0)), "WFP-01: enemy receives no reveal");
                        expect(!world.fog_cells()->revealed(0, at(4500, 0)), "WFP-01: reveal uses projectile radius");
                    }
                }
                if (frame == 1) {
                    expect(std::any_of(stepped.value().snapshot->events().begin(), stepped.value().snapshot->events().end(),
                        [](const auto& event) { return event.kind == tactical::EventKind::order_rejected; }), "WCB/WFP: recharge rejects repeat");
                    if (kind == K::cluster_bomb) expect(!world.projectiles().empty() && world.projectiles().front().position != at(0, 0), "WCB-02: bombs really fly");
                    else {
                        expect(world.projectiles().empty(), "WFP-03: stock ping expires on its first flight service");
                        const auto terminals = std::count_if(stepped.value().snapshot->combat_events().begin(), stepped.value().snapshot->combat_events().end(),
                            [](const auto& event) { return event.kind == tactical::CombatEventKind::projectile_expired
                                && event.aim == at(3000, 0); });
                        expect(terminals == 1, "WFP-03/WPJ-35: ping publishes one lifetime terminal at the clicked point");
                    }
                }
                if (kind == K::cluster_bomb && frame == 44) {
                    expect(world.projectiles().size() == 12, "WCB-02: bombs wait 45 frames before countdown detonation");
                }
                if (kind == K::cluster_bomb && frame == 45) expect(world.projectiles().empty(), "WCB-02: countdown detonates ring");
                if (kind == K::fow_reveal_ping && frame >= 1) {
                    expect(world.projectiles().empty(), "WFP-03: ping never returns after expiry");
                    // V-15: player 1's expiry-frame service precedes source release.
                    // First decay at 17 gives 238; the thirteenth at 209 gives 0.
                    expect(world.fog_cells()->revealed(0, at(3000, 0)) == (frame < 209),
                        "WFP-03/V-15: stock cadence keeps the released area revealed for 208 ticks");
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WCB/WFP: 1/2/4/8 workers equal");
            const auto encoded = tactical::write_replay(world.record());
            const auto parsed = encoded ? tactical::parse_replay(encoded.value())
                : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == world.record(), "WCB/WFP: new kinds and position commands round-trip");
            if (parsed) {
                auto replay = tactical::TacticalSession::from_replay(parsed.value(), {}, durability(), {}, fog, combat, {}, table);
                expect(static_cast<bool>(replay), "WCB/WFP: replay binds");
                if (replay) {
                    std::vector<std::string> replay_hashes;
                    while (replay.value().completed_tick() < parsed.value().final_tick_count) {
                        const auto step = replay.value().step(executor);
                        if (!step) break;
                        replay_hashes.push_back(step.value().state_sha256);
                    }
                    expect(replay_hashes == hashes, "WCB/WFP: replay reproduces projectile and fog state");
                }
            }
        }
    }
}

// FSC-45/46: the previous sample, strict clock admission, and human state transition.
void test_mc30_object_script() {
    using K = tactical::AbilityKind;
    for (const bool human : {false, true}) for (const bool control : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{30, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {31, frigate_type, 2, at(200, 0), math::identity_quat(), {}},
                {32, frigate_type, 2, at(200, 100), math::identity_quat(), {}},
                {33, frigate_type, 2, at(200, -100), math::identity_quat(), {}}};
            if (!human) initial.units.resize(2);
            tactical::AbilityProfile ability;
            ability.kind = K::cluster_bomb; ability.recharge_frames = 1500; ability.supports_autofire = true;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            auto& spawn = *ability.spawned;
            spawn.type = 99; spawn.ring_count = 12; spawn.countdown_frames = 45;
            spawn.speed = units(12); spawn.max_travel = units(1000);
            auto profile = unit_of(plain_type, ability); profile.mc30_script = true;
            tactical::AbilityTable table; table.profiles = {profile};
            table.combat_ratings = {{frigate_type, units(control && !human ? 999 : 1000)}};
            if (human) table.humans = {1};
            tactical::CombatTable combat;
            for (const auto type : {frigate_type, plain_type}) {
                tactical::CombatProfile collision; collision.type_id = type;
                collision.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
                combat.profiles.push_back(collision);
            }
            auto movement = motion();
            for (auto& row : movement.profiles) {
                row.max_speed = units(100); row.acceleration = units(100); row.deceleration = units(100);
                row.rate_of_turn = units(360);
            }
            auto created = tactical::TacticalSession::create(initial, {}, durability(), movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "FSC-45/46: MC30 script session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            if (human) {
                expect(static_cast<bool>(world.submit(command(30, 1, 0, 30,
                    tactical::AbilityPayload{K::cluster_bomb, tactical::AbilityAction::autofire_on}))), "FSC-46: arm before due turn");
                if (control) expect(static_cast<bool>(world.submit(command(32, 1, 1, 30,
                    tactical::AbilityPayload{K::cluster_bomb, tactical::AbilityAction::autofire_off}))), "FSC-46: disarm before next due turn");
            } else {
                expect(static_cast<bool>(world.submit(command(1, 2, 0, 31, tactical::MovePayload{at(2000, 0)}))),
                    "FSC-45: sampled enemy leaves before due update");
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 64; ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "FSC-45/46: step succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto state = world.ability_state(30);
                expect(state && state->mc30_script, "FC-57: first pump initializes hashed script");
                if (!state || !state->mc30_script) break;
                const auto& script = *state->mc30_script;
                if (frame == 0) expect(state->slots[0].ready_tick == 0 && script.nearby == (human ? 3U : 1U),
                    "FD-47: first empty update does not fire; wrapper builds sample");
                if (frame == 30) expect(script.last_service == 0 && state->slots[0].ready_tick == 0,
                    "FSC-45: exactly one second skips and retains previous sample");
                if (frame == 31) {
                    expect(script.last_service > (std::int64_t{1} << 28), "FC-57: frame 31 admits update");
                    expect(state->slots[0].ready_tick == (!human && !control ? 1531U : 0U),
                        "FSC-45/46: old AI threat fires; arming clears human sample");
                    expect(world.projectiles().size() == (!human && !control ? 12U : 0U),
                        "FD-47: twelve bombs created directly in the admitted turn");
                    expect(script.nearby == (human ? 3U : 0U), "FD-47: new wrapper sample follows update");
                    if (!human && !control) for (const auto& projectile : world.projectiles())
                        expect(projectile.position == at(0, 0), "FD-47: creation precedes first flight service");
                }
                if (frame == 32) {
                    expect(script.last_service > (std::int64_t{1} << 28) && script.last_service < (std::int64_t{2} << 28),
                        "FC-57: one frame since last service skips");
                    if (!human && !control) expect(!world.projectiles().empty() && world.projectiles().front().position != at(0, 0),
                        "FD-47: record next-frame first projectile flight separately");
                }
                if (frame == 32 && human && control) expect(script.armed && script.nearby == 3 && !state->slots[0].autofire,
                    "FSC-46: disarming on an unadmitted frame retains the script sample and state");
                // FC-57: rounding makes this interval 30 frames; it is not a fixed 31-frame deadline.
                if (frame == 61 && human) {
                    expect(state->slots[0].ready_tick == (control ? 0U : 1561U), "FSC-46: staying armed uses retained count; disarming avoids use");
                    expect(world.projectiles().size() == (control ? 0U : 12U), "FSC-46: direct human activation at next due pump");
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "FSC-45/46: 1/2/4/8 worker hashes equal");
            auto playback = tactical::TacticalSession::from_replay(world.record(), {}, durability(), movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(playback), "MSCR: object-script replay creates");
            if (playback) {
                std::vector<std::string> replay_hashes;
                while (playback.value().completed_tick() < world.completed_tick()) {
                    const auto step = playback.value().step(executor);
                    if (!step) break;
                    replay_hashes.push_back(step.value().state_sha256);
                }
                expect(replay_hashes == hashes, "MSCR: retained samples and stamps replay exactly");
            }
        }
    }
}

void test_mc30_proximity_sample() {
    using K = tactical::AbilityKind;
    // FC-58: lower distance uses promoted position; two collected craft count once.
    for (const bool squadron : {false, true}) for (const auto distance : {149, 150, 501, 502}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{30, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {31, squadron ? xwing_squadron : frigate_type, 2, at(distance, 0), math::identity_quat(), {}}};
            auto movement = motion();
            auto health = durability();
            tactical::CombatTable combat;
            tactical::CombatProfile box; box.type_id = squadron ? xwing_type : frigate_type;
            box.collision = tactical::CollisionBox{at(-1, -1, -1), at(1, 1, 1)};
            combat.profiles = {box};
            if (squadron) {
                initial.units.push_back({32, xwing_type, 2, at(200, 0), math::identity_quat(), {}});
                initial.units.push_back({33, xwing_type, 2, at(200, 0), math::identity_quat(), {}});
                initial.squadrons = {{31, {32, 33}}};
                movement.squadrons = hangar_motion().squadrons;
                movement.squadrons.spawners.clear();
                // Keep the craft positions where the test stages them; only the collection and promotion matter.
                for (auto& craft : movement.squadrons.craft) { craft.min_speed = {}; craft.max_speed = units(1); }
                const auto extra = hangar_durability();
                for (const auto& row : extra.profiles) if (row.type_id == xwing_type) health.profiles.push_back(row);
                std::sort(health.profiles.begin(), health.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            }
            tactical::AbilityProfile ability; ability.kind = K::cluster_bomb; ability.recharge_frames = 1500;
            ability.spawned = tactical::SpawnedAbilityProfile{};
            ability.spawned->type = 99; ability.spawned->ring_count = 12; ability.spawned->countdown_frames = 45;
            ability.spawned->speed = units(12);
            auto profile = unit_of(plain_type, ability); profile.mc30_script = true;
            tactical::AbilityTable table; table.profiles = {profile};
            table.combat_ratings = {{squadron ? xwing_squadron : frigate_type, units(1000)}};
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "FC-58: proximity session creates");
            if (!created) continue;
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "FC-58: sample step succeeds");
            if (!step) continue;
            const auto state = world.ability_state(30);
            const bool eligible = distance >= 150 && (squadron || distance <= 501);
            expect(state && state->mc30_script && state->mc30_script->nearby == (eligible ? 1U : 0U)
                && state->mc30_script->threat == units(eligible ? 1000 : 0),
                "FC-58: exclude below 150, include exactly 150/bounds, deduplicate and rate promoted parent");
            if (reference.empty()) reference.push_back(step.value().state_sha256);
            else expect(reference.front() == step.value().state_sha256, "FC-58: promoted samples equal on 1/2/4/8 workers");
        }
    }
}

void test_spawned_hero_abilities() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::harmonic_bomb, K::weaken_enemy}) for (const bool zero : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0), math::identity_quat(), {}},
                {3, corvette_type, 1, at(100, 0), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = kind; ordinary.recharge_frames = 20;
            ordinary.spawned = tactical::SpawnedAbilityProfile{};
            auto& spawned = *ordinary.spawned;
            spawned.type = 0x1122334455667788ULL; spawned.damage_type = 0;
            spawned.countdown_frames = kind == K::harmonic_bomb ? 3 : 0;
            spawned.reach = units(300);
            if (kind == K::harmonic_bomb) { spawned.blast.damage = units(50); spawned.blast.radius = units(120); spawned.blast.max_delay = {}; }
            else {
                spawned.weaken.on_detonation = true; spawned.weaken.radius = units(120);
                spawned.weaken.duration_frames = 3; spawned.weaken.categories = 2;
                spawned.weaken.take_damage_increase = decimal("0.5");
                spawned.weaken.cause_damage_reduction = decimal("0.2");
            }
            tactical::AbilityTable table; table.profiles = {unit_of(plain_type, ordinary)};
            tactical::CombatTable combat;
            for (const auto type : {corvette_type, frigate_type, plain_type}) {
                tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
                profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
                if (type == frigate_type) {
                    profile.max_attack_distance = units(300);
                    tactical::WeaponProfile weapon; weapon.range = units(300); weapon.pulse_count = 1;
                    weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
                    weapon.cone_width = units(360); weapon.cone_height = units(160);
                    weapon.shot = tactical::ShotProfile{units(1), 0, units(1), units(300), true, true, {}};
                    profile.weapons = {weapon};
                }
                combat.profiles.push_back(profile);
            }
            auto created = tactical::TacticalSession::create(initial,
                std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}, {plain_type, units(1000)}},
                health, {}, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "spawned hero fixture creates");
            if (!created) continue;
            auto world = std::move(created).value();
            tactical::AbilityPayload input{kind, tactical::AbilityAction::activate};
            if (kind == K::weaken_enemy) input.position = zero ? math::Vec3{} : at(100, 0);
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, input))), "spawn input accepted");
            expect(static_cast<bool>(world.submit(command(1, 1, 1, 1, input))), "cooldown input queues for rejection");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool reduced_shot = false;
            std::uint64_t spawned_projectile = 0;
            for (unsigned frame = 0; frame < 7; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "spawned hero step succeeds");
                if (!step) { std::cerr << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                const auto spawns = step.value().snapshot->ability_spawns();
                if (frame == 0 && !spawns.empty()) spawned_projectile = spawns.front().id;
                expect(std::none_of(step.value().snapshot->combat_events().begin(),
                    step.value().snapshot->combat_events().end(), [&](const auto& event) {
                        return event.kind == tactical::CombatEventKind::projectile_expired
                            && event.target == spawned_projectile;
                    }), "WHE-62/WAD-07: a stationary ability spawn has only its hero detonation presentation");
                if (frame == 0) expect(spawns.size() == 1 && spawns.front().type == spawned.type
                    && spawns.front().position == (kind == K::harmonic_bomb || zero ? at(0, 0) : at(100, 0))
                    && !world.ability_state(1)->slots[0].active && world.ability_state(1)->slots[0].ready_tick == 20,
                    "instant spawn retains type, position and cooldown without an active slot");
                if (frame == 1) expect(std::any_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(),
                    [](const auto& event) { return event.kind == tactical::EventKind::order_rejected; }), "recharge rejects another spawn");
                if (kind == K::weaken_enemy && frame == 1) expect(spawns.size() == 1 && spawns.front().detonated
                    && spawns.front().recipients == std::vector<tactical::WeakenRecipient>{{2, 4}},
                    "weaken admits the different-owner collidable combat recipient and keeps its expiry");
                for (const auto& projectile : step.value().snapshot->projectiles()) if (projectile.shooter == 2
                    && projectile.source_damage_factor == math::Fixed::from_raw(one + decimal("-0.2").raw())) reduced_shot = true;
                if (frame >= 4) expect(spawns.empty(), "detonated metadata releases after countdown or final recipient expiry");
                if (kind == K::harmonic_bomb && frame == 2) expect(world.durability_state(2)->hull == units(3600), "bomb waits its authored countdown");
                if (kind == K::harmonic_bomb && frame == 3) expect(world.durability_state(2)->hull < units(3600), "bomb countdown delivers ordinary area damage");
            }
            if (kind == K::weaken_enemy) expect(reduced_shot, "timed weaken contributes to captured outgoing projectile damage");
            const auto record = world.record();
            const auto encoded = tactical::write_replay(record);
            const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "world-point ability extension round-trips");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "spawn and timed-status hashes equal on 1/2/4/8 workers");
        }
    }
}

// FS-23/FC-51/FD-32: one team request approaches and retries independently of the
// ordinary target scan. Application starts recharge; UFS-21 also resolves ended approaches.
void test_buzz_team_request() {
    constexpr tactical::TypeId team_type = 40, craft_type = 41, droid_type = 42;
    std::vector<std::string> reference;
    std::map<std::string, std::vector<std::string>> case_references;
    const auto compare = [&](const std::string& name, const std::vector<std::string>& hashes) {
        const auto [entry, inserted] = case_references.emplace(name, hashes);
        if (!inserted) expect(entry->second == hashes, "UFS-21: regression hashes equal on 1/2/4/8 workers");
    };
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{10, team_type, 1, at(0, 0), math::identity_quat(), {}},
            {11, craft_type, 1, at(0, 0), math::identity_quat(), {}},
            {20, team_type, 1, at(0, 2000), math::identity_quat(), {}},
            {21, craft_type, 1, at(0, 2000), math::identity_quat(), {}},
            {30, plain_type, 2, at(4000, 4000), math::identity_quat(), {}}};
        initial.squadrons = {{10, {11}}, {20, {21}}};
        auto health = durability();
        for (const auto type : {team_type, craft_type, droid_type}) {
            tactical::DurabilityProfile profile;
            profile.type_id = type; profile.max_hull = units(20);
            health.profiles.push_back(profile);
        }
        auto movement = motion();
        tactical::CraftProfile craft;
        craft.type_id = craft_type;
        craft.max_speed = units(3); craft.min_speed = units(2);
        craft.rate_of_turn = units(6); craft.lift = units(6);
        craft.thrust = units(1); craft.roll_rate = units(3); craft.bank_angle = units(70);
        craft.strafe_distance = units(500);
        movement.squadrons.craft = {craft};
        movement.squadrons.squadrons = {{team_type, {craft_type}, {at(0, 0)},
            units(1000), units(200), units(300), units(25)}};
        tactical::AbilityProfile buzz;
        buzz.kind = tactical::AbilityKind::buzz_droids; buzz.recharge_frames = 60;
        buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
            units(5), units(1), units(20), 1000, 2, 0};
        tactical::AbilityTable table; table.profiles = {unit_of(team_type, buzz)};
        auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "FS-23: team buzz request fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        expect(world.squadron_state(10).has_value() && world.squadron_state(20).has_value(),
            "FS-23: each ability holder has its authored team membership");
        tactical::AbilityPayload input{tactical::AbilityKind::buzz_droids, tactical::AbilityAction::activate};
        input.position = at(500, 0);
        auto far = input; far.position = at(5000, 2000);
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 10, input))), "FS-23: one team point request submits");
        expect(static_cast<bool>(world.submit(command(0, 1, 1, 20, far))), "FS-23: distant request submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<std::uint64_t> applied;
        for (std::uint64_t frame = 0; frame < 120; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "FC-51: retained team request step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            const auto holder = world.ability_state(10);
            const auto blocked = world.ability_state(20);
            expect(blocked && blocked->pending_buzz_position == far.position && blocked->slots[0].ready_tick == 0,
                "FC-51: continuing range refusal retains the marker and never starts recharge");
            const auto units_now = world.units();
            const auto count = std::count_if(units_now.begin(), units_now.end(),
                [](const tactical::UnitState& unit) { return unit.type_id == droid_type; });
            if (count != 0 && !applied) applied = frame;
            if (!applied) {
                expect(holder && holder->pending_buzz_position == input.position && holder->slots[0].ready_tick == 0,
                    "FC-51: out-of-range refusal keeps the request without recharge");
            } else {
                expect(count == 1 && holder && !holder->pending_buzz_position
                    && holder->slots[0].started_tick == *applied && holder->slots[0].ready_tick == *applied + 60,
                    "FS-08/23: successful retry creates exactly one droid and clears only that request");
                const auto droid = std::find_if(units_now.begin(), units_now.end(),
                    [](const tactical::UnitState& unit) { return unit.type_id == droid_type; });
                expect(droid != units_now.end() && droid->owner == 1 && droid->position == *input.position,
                    "WAB-71: the retry's droid appears at its retained point for the holder's owner");
            }
            if (frame == 0) expect(!applied && std::count_if(step.value().snapshot->events().begin(),
                step.value().snapshot->events().end(), [](const tactical::Event& event) {
                    return event.kind == tactical::EventKind::order_accepted && event.order == tactical::OrderKind::ability;
                }) == 2, "FS-23: request admission precedes application, including out-of-range points");
        }
        expect(applied && *applied > 1 && *applied < 120,
            "FD-32: the admitted approach enters range and applies without another command");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "FC-51: retained request and ordered allocation agree on 1/2/4/8 workers");
        const auto record = world.record();
        const auto encoded = tactical::write_replay(record);
        const auto parsed = encoded ? tactical::parse_replay(encoded.value())
            : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(static_cast<bool>(parsed), "FS-23: team request replay round-trips");
        if (!parsed) continue;
        auto replayed = tactical::TacticalSession::from_replay(parsed.value(), {}, health, movement,
            std::nullopt, {}, {}, table);
        expect(static_cast<bool>(replayed), "FS-23: retained request replay creates");
        if (replayed) {
            std::vector<std::string> replay_hashes;
            while (replayed.value().completed_tick() < record.final_tick_count) {
                const auto step = replayed.value().step(executor);
                if (!step) break;
                replay_hashes.push_back(step.value().state_sha256);
            }
            expect(replay_hashes == hashes, "FC-51: replay reconstructs retained points and delayed application");
        }
        // UFS-21: preserve BASE admission for points that cannot be reached by approach.
        for (const auto distance : {3, 5}) {
            auto refused = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
            expect(static_cast<bool>(refused), "WAB-70: minimum-range team fixture creates");
            if (!refused) continue;
            auto& near_world = refused.value();
            auto near = input; near.position = at(distance, 0);
            expect(static_cast<bool>(near_world.submit(command(0, 1, 0, 10, near))),
                "WAB-70: near-point command submits for admission checking");
            std::vector<std::string> near_hashes;
            for (std::uint64_t frame = 0; frame < 120; ++frame) {
                const auto step = near_world.step(executor);
                expect(static_cast<bool>(step), "WAB-70: minimum-range refusal steps");
                if (!step) break;
                near_hashes.push_back(step.value().state_sha256);
                if (frame == 0) expect(std::any_of(step.value().snapshot->events().begin(),
                    step.value().snapshot->events().end(), [](const tactical::Event& event) {
                        return event.kind == tactical::EventKind::order_rejected
                            && event.reason == tactical::RejectReason::invalid_position;
                    }), "WAB-70: a point at or inside the minimum is refused at admission");
                const auto state = near_world.ability_state(10);
                expect(state && !state->pending_buzz_position && state->slots[0].ready_tick == 0,
                    "WAB-70: refused near point leaves no retained request or recharge");
                const auto live = near_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                    return unit.type_id == droid_type;
                }), "WAB-70: refused near point never creates a swarm");
            }
            compare("refused-" + std::to_string(distance), near_hashes);
        }
        for (const auto distance : {6, 7}) for (const bool ordinary_attack : {false, true}) {
            auto boundary = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
            expect(static_cast<bool>(boundary), "UFS-21: near-band team fixture creates");
            if (!boundary) continue;
            auto& boundary_world = boundary.value();
            if (ordinary_attack) expect(static_cast<bool>(boundary_world.submit(
                command(0, 1, 0, 10, tactical::AttackPayload{30, tactical::attack_hull}))),
                "UFS-21: near-band request also covers a team already attacking");
            auto near = input; near.position = at(distance, 0);
            expect(static_cast<bool>(boundary_world.submit(command(0, 1, 1, 10, near))),
                "UFS-21: point just above the minimum submits");
            std::vector<std::string> boundary_hashes;
            for (std::uint64_t frame = 0; frame < 120; ++frame) {
                const auto step = boundary_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: near-band request steps");
                if (!step) break;
                boundary_hashes.push_back(step.value().state_sha256);
            }
            const auto state = boundary_world.ability_state(10);
            const auto live = boundary_world.units();
            const auto count = std::count_if(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                return unit.type_id == droid_type;
            });
            expect(state && !state->pending_buzz_position && count == 1 && state->slots[0].ready_tick > 0,
                "UFS-21: the 6/7-unit approach applies exactly once and starts recharge");
            compare("boundary-" + std::to_string(distance) + (ordinary_attack ? "-attack" : "-idle"), boundary_hashes);
        }
        // UFS-21: Stop explicitly cancels an unsuccessful retained approach.
        auto stopped = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(stopped), "UFS-21: completed approach fixture creates");
        if (stopped) {
            auto& stopped_world = stopped.value();
            expect(static_cast<bool>(stopped_world.submit(command(0, 1, 0, 10, far))),
                "UFS-21: unsuccessful approach submits");
            expect(static_cast<bool>(stopped_world.submit(command(1, 1, 1, 10, tactical::StopPayload{}))),
                "UFS-21: approach stops at rest before entering range");
            std::vector<std::string> stopped_hashes;
            for (std::uint64_t frame = 0; frame < 120; ++frame) {
                const auto step = stopped_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: completed approach steps");
                if (!step) break;
                stopped_hashes.push_back(step.value().state_sha256);
                const auto state = stopped_world.ability_state(10);
                expect(state && state->slots[0].ready_tick == 0
                    && state->pending_buzz_position.has_value() == (frame < 1),
                    "UFS-21: Stop cancels the admitted approach without recharge");
                const auto live = stopped_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                    return unit.type_id == droid_type;
                }), "UFS-21: completed unsuccessful approach creates no swarm");
            }
            compare("stop", stopped_hashes);
        }
        // UFS-21: all accepted replacements end the approach, independent of completion.
        const std::vector<tactical::CommandPayload> replacements{
            tactical::AttackPayload{30, tactical::attack_hull},
            tactical::GuardPayload{at(0, 0), 20},
            tactical::AttackMovePayload{at(0, 0), 20},
            tactical::MovePayload{*far.position}};
        for (std::size_t replacement = 0; replacement < replacements.size(); ++replacement) {
            auto interrupted = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
            expect(static_cast<bool>(interrupted), "UFS-21: replacement fixture creates");
            if (!interrupted) continue;
            auto& interrupted_world = interrupted.value();
            expect(static_cast<bool>(interrupted_world.submit(command(0, 1, 0, 10, far))),
                "UFS-21: pending approach submits before replacement");
            expect(static_cast<bool>(interrupted_world.submit(command(1, 1, 1, 10, replacements[replacement]))),
                "UFS-21: ordinary replacement submits");
            std::vector<std::string> interrupted_hashes;
            for (unsigned frame = 0; frame < 300; ++frame) {
                const auto step = interrupted_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: interrupted approach steps");
                if (!step) break;
                interrupted_hashes.push_back(step.value().state_sha256);
                const auto state = interrupted_world.ability_state(10);
                expect(state && state->pending_buzz_position.has_value() == (frame == 0)
                    && state->slots[0].ready_tick == 0,
                    "UFS-21: replacement clears the old request without swarm or recharge");
                const auto live = interrupted_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const tactical::UnitState& unit) {
                    return unit.type_id == droid_type;
                }), "UFS-21: interrupted request never fires at its old point");
            }
            compare("replacement-" + std::to_string(replacement), interrupted_hashes);
        }
        // UFS-21: the narrow activation band keeps the 7-unit interrupt unready.
        auto narrow_table = table;
        narrow_table.profiles[0].abilities[0].buzz->max_range = units(6);
        auto near_interrupt = tactical::TacticalSession::create(initial, {}, health, movement,
            std::nullopt, {}, {}, narrow_table);
        expect(static_cast<bool>(near_interrupt), "UFS-21: near-band interrupt fixture creates");
        if (near_interrupt) {
            auto& interrupted_world = near_interrupt.value();
            auto near = input; near.position = at(7, 0);
            expect(static_cast<bool>(interrupted_world.submit(command(0, 1, 0, 10, near))),
                "UFS-21: near-band approach submits");
            expect(static_cast<bool>(interrupted_world.submit(command(1, 1, 1, 10,
                tactical::AttackPayload{30, tactical::attack_hull}))), "UFS-21: player Attack interrupts near-band approach");
            std::vector<std::string> near_interrupt_hashes;
            for (unsigned frame = 0; frame < 300; ++frame) {
                const auto step = interrupted_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: near-band interrupt steps");
                if (!step) break;
                near_interrupt_hashes.push_back(step.value().state_sha256);
                const auto state = interrupted_world.ability_state(10);
                expect(state && state->pending_buzz_position.has_value() == (frame == 0)
                    && state->slots[0].ready_tick == 0,
                    "UFS-21: unready near-band Attack resolves the request without recharge");
                const auto live = interrupted_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const auto& unit) { return unit.type_id == droid_type; }),
                    "UFS-21: the interrupted near-band marker never spawns droids");
            }
            compare("near-band-interrupt", near_interrupt_hashes);
        }
        // UFS-21: losing the base target cancels the failed special approach.
        auto lost = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(lost), "UFS-21: target-loss fixture creates");
        if (lost) {
            auto& lost_world = lost.value();
            expect(static_cast<bool>(lost_world.submit(command(0, 1, 0, 10,
                tactical::AttackPayload{30, tactical::attack_hull}))), "UFS-21: ordinary base attack submits");
            expect(static_cast<bool>(lost_world.submit(command(0, 1, 1, 10, far))), "UFS-21: retained approach submits");
            std::vector<std::string> lost_hashes;
            for (unsigned frame = 0; frame < 120; ++frame) {
                if (frame == 1) expect(static_cast<bool>(lost_world.stage_remove(30)), "UFS-21: approach base target leaves");
                const auto step = lost_world.step(executor);
                expect(static_cast<bool>(step), "UFS-21: target-loss approach steps");
                if (!step) break;
                lost_hashes.push_back(step.value().state_sha256);
                const auto state = lost_world.ability_state(10);
                expect(state && state->pending_buzz_position.has_value() == (frame == 0)
                    && state->slots[0].ready_tick == 0,
                    "UFS-21: lost base drops its failed request without recharge");
                const auto live = lost_world.units();
                expect(std::none_of(live.begin(), live.end(), [](const auto& unit) { return unit.type_id == droid_type; }),
                    "UFS-21: lost approach never creates a swarm");
            }
            compare("lost-base", lost_hashes);
        }
        // FS-23/WMV-18 (#3491): base loss converts the object base to a position;
        // autonomous targeting can acquire another live target in the preserved formation.
        auto reacquire_initial = initial;
        reacquire_initial.units.push_back({31, plain_type, 2, at(100, 0), math::identity_quat(), {}});
        tactical::CombatTable reacquire_combat;
        tactical::CombatProfile enemy; enemy.type_id = plain_type; enemy.category_bits = 1;
        tactical::CombatProfile fighter; fighter.type_id = craft_type; fighter.category_bits = 1;
        fighter.max_attack_distance = units(2000);
        tactical::WeaponProfile weapon; weapon.range = units(2000);
        weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
        fighter.weapons = {weapon};
        reacquire_combat.profiles = {enemy, fighter};
        const std::vector<tactical::SensorProfile> reacquire_sensors{{craft_type, units(10000)}};
        auto reacquired = tactical::TacticalSession::create(reacquire_initial, reacquire_sensors, health, movement,
            std::nullopt, reacquire_combat, {}, table);
        expect(static_cast<bool>(reacquired), "FS-23: base-loss reacquisition fixture creates");
        if (reacquired) {
            auto& reacquire_world = reacquired.value();
            expect(static_cast<bool>(reacquire_world.submit(command(0, 1, 0, 10,
                tactical::AttackPayload{30, tactical::attack_hull}))), "FS-23: removable base target submits");
            std::vector<std::string> reacquire_hashes;
            const auto first = reacquire_world.step(executor);
            expect(static_cast<bool>(first), "FS-23: base attack steps");
            if (first) reacquire_hashes.push_back(first.value().state_sha256);
            expect(static_cast<bool>(reacquire_world.stage_remove(30)), "FS-23: ordinary base target leaves");
            bool found_preserved = false;
            for (unsigned frame = 0; frame < 300; ++frame) {
                const auto step = reacquire_world.step(executor);
                expect(static_cast<bool>(step), "FS-23: reacquisition steps");
                if (!step) break;
                reacquire_hashes.push_back(step.value().state_sha256);
                const auto team = reacquire_world.squadron_state(10);
                if (frame == 0) expect(team && team->formation && team->formation->base_target == 0,
                    "WMV-18: base loss preserves the formation with a position base");
                // FS-23 checks the team's ordinary target; movement destination
                // admission separately retains WMV-17's base allowance.
                if (team && team->formation && team->target == 31 && team->formation->base_target == 0) {
                    found_preserved = true;
                    break;
                }
            }
            expect(found_preserved, "FS-23: live ordinary target is acquired in the preserved formation");
            compare("preserved-formation", reacquire_hashes);
            if (!found_preserved) continue;

            // WMV-18: an actually absent formation is distinct from base loss. Stage
            // that state through the checkpoint fields, as in the fighter scan contracts,
            // preserving the acquired target, live units and sealed command log.
            namespace checkpoint = tactical::checkpoint_detail;
            const auto saved = reacquire_world.save_checkpoint_state();
            expect(static_cast<bool>(saved), "FS-23: absent formation staging checkpoint saves");
            if (!saved) continue;
            checkpoint::Access::State staged(reacquire_initial, reacquire_sensors, health, movement,
                std::nullopt, reacquire_combat, {}, table, {}, {});
            checkpoint::Reader reader(saved.value());
            std::uint32_t version{};
            reader(version);
            checkpoint::Access::state(reader, staged);
            std::vector<tactical::session_detail::LiveUnit> live;
            reader(live);
            const auto tail = reader.remaining();
            staged.minds.at(10).formation.reset();
            checkpoint::Writer writer;
            writer(version);
            checkpoint::Access::state(writer, std::as_const(staged));
            writer(live);
            writer.bytes.insert(writer.bytes.end(), saved.value().end() - static_cast<std::ptrdiff_t>(tail), saved.value().end());
            auto absent = tactical::TacticalSession::from_replay(reacquire_world.record_through_next_tick(),
                reacquire_sensors, health, movement, std::nullopt, reacquire_combat, {}, table);
            expect(static_cast<bool>(absent), "FS-23: absent formation preserves the sealed command log");
            if (!absent) continue;
            auto& absent_world = absent.value();
            const auto loaded = absent_world.load_checkpoint_state(writer.bytes);
            expect(static_cast<bool>(loaded), "FS-23: absent formation staged state restores");
            if (!loaded) continue;
            const auto before_point = absent_world.squadron_state(10);
            expect(before_point && !before_point->formation && before_point->target == 31
                && before_point->target_hardpoint == tactical::attack_hull,
                "FS-23: special point starts with a live ordinary target and genuinely absent formation");
            auto point = input;
            const auto holder = absent_world.units();
            const auto container = std::find_if(holder.begin(), holder.end(), [](const auto& unit) { return unit.entity_id == 10; });
            if (container != holder.end()) point.position = math::Vec3{math::Fixed::from_raw(container->position.x.raw() + units(100).raw()),
                container->position.y, container->position.z};
            expect(static_cast<bool>(absent_world.submit(command(absent_world.completed_tick(), 1, 1, 10, point))),
                "FS-23: absent formation special point submits");
            const auto admitted = absent_world.step(executor);
            expect(static_cast<bool>(admitted), "FS-23: absent formation admission steps");
            std::vector<std::string> absent_hashes;
            if (admitted) {
                absent_hashes.push_back(admitted.value().state_sha256);
                expect(admitted.value().diagnostics.empty(), "FS-23: absent formation special point is admitted");
            }
            const auto team = absent_world.squadron_state(10);
            expect(team && team->target == 31 && team->target_hardpoint == tactical::attack_hull
                && team->formation && team->formation->base_target == 0
                && team->formation->base_position == *point.position
                && team->formation->attack_target == team->target
                && team->formation->attack_hardpoint == team->target_hardpoint,
                "FS-23: absent formation retains its live ordinary target in the new formation");
            const auto queued = absent_world.ability_state(10);
            expect(queued && queued->pending_buzz_position == point.position && queued->slots[0].ready_tick == 0,
                "FS-23: absent formation queues the special point without recharge");
            compare("absent-formation", absent_hashes);
        }
        auto fighting = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(fighting), "FS-23: ordinary-target team fixture creates");
        if (!fighting) continue;
        auto& fighting_world = fighting.value();
        expect(static_cast<bool>(fighting_world.submit(command(0, 1, 0, 10, tactical::AttackPayload{30, tactical::attack_hull}))),
            "FS-23: ordinary team attack submits before special point");
        auto in_range = input; in_range.position = at(100, 0);
        expect(static_cast<bool>(fighting_world.submit(command(0, 1, 1, 10, in_range))),
            "FSC-33: in-range team point submits");
        const auto admitted = fighting_world.step(executor);
        expect(static_cast<bool>(admitted), "FS-23: ordinary target and special point admission steps");
        const auto team = fighting_world.squadron_state(10);
        expect(team && team->target == 30 && team->target_hardpoint == tactical::attack_hull && team->formation
            && team->formation->attack_target == 30 && team->formation->base_target == 30
            && team->formation->base_position == *in_range.position,
            "FS-23: special approach retains the ordinary target in both team and movement formation");
        const auto queued = fighting_world.ability_state(10);
        expect(queued && queued->pending_buzz_position == in_range.position && queued->slots[0].ready_tick == 0,
            "FSC-33: even an in-range team point is queued at t without application");
        std::vector<std::string> fighting_hashes;
        if (admitted) fighting_hashes.push_back(admitted.value().state_sha256);
        const auto applied_next = fighting_world.step(executor);
        if (applied_next) fighting_hashes.push_back(applied_next.value().state_sha256);
        compare("prior-formation", fighting_hashes);
        expect(static_cast<bool>(applied_next), "FSC-33: in-range team retry steps at t+1");
        const auto applied_state = fighting_world.ability_state(10);
        const auto fighting_units = fighting_world.units();
        expect(applied_state && !applied_state->pending_buzz_position && applied_state->slots[0].started_tick == 1
            && applied_state->slots[0].ready_tick == 61
            && std::count_if(fighting_units.begin(), fighting_units.end(), [](const tactical::UnitState& unit) {
                return unit.type_id == droid_type;
            }) == 1, "FSC-33: the t+1 retry applies once and begins the full recharge");
    }
}

// WAB-70..74: a BUZZ_DROIDS activation at a point creates the droid object there; from its arming
// frame it wears itself down, chases the nearest enemy mover in the chase radius around the creation
// point and damages every one in damage reach. 1/2/4/8 workers give the same hashes.
void test_buzz_droids() {
    using K = tactical::AbilityKind;
    constexpr tactical::TypeId droid_type = 30;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
            {2, frigate_type, 2, at(160, 0), math::identity_quat(), {}},
            {3, corvette_type, 2, at(100, 1000), math::identity_quat(), {}},
            {4, plain_type, 1, at(0, 2000), math::identity_quat(), {}}};
        auto health = durability();
        for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
        tactical::DurabilityProfile droid_health;
        droid_health.type_id = droid_type; droid_health.max_hull = units(20);
        health.profiles.push_back(droid_health);
        tactical::AbilityProfile buzz;
        buzz.kind = K::buzz_droids; buzz.recharge_frames = 60;
        buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
            units(5), units(1), units(20), 3, 2, 0};
        tactical::AbilityTable table; table.profiles = {unit_of(plain_type, buzz)};
        tactical::CombatTable combat;
        for (const auto type : {corvette_type, frigate_type, plain_type}) {
            tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
            profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            combat.profiles.push_back(profile);
        }
        auto created = tactical::TacticalSession::create(initial,
            std::vector<tactical::SensorProfile>{{corvette_type, units(1000)}, {frigate_type, units(1000)}, {plain_type, units(1000)}},
            health, motion(), std::nullopt, combat, {}, table);
        expect(static_cast<bool>(created), "WAB-70: buzz-droid fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        tactical::AbilityPayload input{K::buzz_droids, tactical::AbilityAction::activate};
        input.position = at(100, 0);
        auto far = input;
        far.position = at(0, 2500);
        auto near = input;
        near.position = at(0, 2003);
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, input))), "WAB-70: buzz-droid point input accepted");
        expect(static_cast<bool>(world.submit(command(0, 1, 1, 4, far))), "WAB-70: out-of-reach point queues for rejection");
        expect(static_cast<bool>(world.submit(command(0, 1, 2, 4, near))), "WAB-70: too-near point queues for rejection");
        expect(static_cast<bool>(world.submit(command(1, 1, 3, 1, input))), "WAB-71: recharging input queues for rejection");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<eawr::sim::EntityId> droid;
        bool droid_died = false;
        for (unsigned frame = 0; frame < 26; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "WAB-72: buzz-droid step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            const auto& events = step.value().snapshot->events();
            if (frame == 0) {
                const auto units_now = world.units();
                const auto found = std::find_if(units_now.begin(), units_now.end(),
                    [&](const tactical::UnitState& unit) { return unit.type_id == droid_type; });
                expect(found != units_now.end() && found->owner == 1 && found->position == at(100, 0)
                    && std::count_if(units_now.begin(), units_now.end(),
                        [&](const tactical::UnitState& unit) { return unit.type_id == droid_type; }) == 1,
                    "WAB-71: one droid object at the point for the activating owner");
                if (found != units_now.end()) droid = found->entity_id;
                expect(world.ability_state(1) && !world.ability_state(1)->slots[0].active
                    && world.ability_state(1)->slots[0].ready_tick == 60, "WAB-71: instant activation starts the full recharge");
                expect(std::count_if(events.begin(), events.end(), [](const tactical::Event& event) {
                    return event.kind == tactical::EventKind::order_rejected && event.unit == 4
                        && event.reason == tactical::RejectReason::invalid_position; }) == 2,
                    "WAB-70: points outside the authored planar reach, or inside the minimum, are refused");
            }
            if (frame == 1) expect(std::any_of(events.begin(), events.end(), [](const tactical::Event& event) {
                return event.kind == tactical::EventKind::order_rejected && event.unit == 1; }), "WAB-71: recharge refuses another activation");
            if (!droid) continue;
            const auto droid_now = world.durability_state(*droid);
            const auto units_now = world.units();
            const auto unit = std::find_if(units_now.begin(), units_now.end(),
                [&](const tactical::UnitState& entry) { return entry.entity_id == *droid; });
            if (frame < 3) expect(droid_now && droid_now->hull == units(20) && unit != units_now.end()
                && unit->position == at(100, 0), "WAB-72: the droids wait for their arming frame");
            if (frame == 3) expect(droid_now && droid_now->hull == units(19) && unit != units_now.end()
                && unit->position == at(120, 0) && world.durability_state(2)->hull == units(3600),
                "WAB-74: armed droids out of damage reach close on the nearest enemy at full speed");
            if (frame == 4) expect(droid_now && droid_now->hull == units(18) && unit != units_now.end()
                && unit->position == at(120, 0) && world.durability_state(2)->hull == units(3595)
                && world.durability_state(3)->hull == units(3600),
                "WAB-72/73: droids in reach stay and damage every enemy there; one outside the chase radius is untouched");
            if (!droid_now && !droid_died) {
                droid_died = true;
                expect(frame == 22 && std::any_of(events.begin(), events.end(), [&](const tactical::Event& event) {
                    return event.kind == tactical::EventKind::unit_destroyed && event.unit == *droid; }),
                    "WAB-72: the droids' own wear destroys them after their authored health");
            }
        }
        expect(droid_died, "WAB-72: the droid object dies of its own wear");
        const auto record = world.record();
        const auto encoded = tactical::write_replay(record);
        const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == record, "WAB-70: buzz-droid point command round-trips");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WAB-72: buzz-droid hashes equal on 1/2/4/8 workers");
    }
}

void test_hostile_buzz_droids() {
    using K = tactical::AbilityKind;
    constexpr tactical::TypeId droid_type = 30;
    constexpr tactical::TypeId victim_type = 31;
    for (const bool droid_locomotor : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, plain_type, 2, at(300, 0), math::identity_quat(), {}},
                {3, victim_type, 2, at(100, 0), math::identity_quat(), {}}};
            auto health = durability();
            tactical::DurabilityProfile droid_health;
            droid_health.type_id = droid_type; droid_health.max_hull = units(20);
            health.profiles.push_back(droid_health);
            auto victim_health = droid_health;
            victim_health.type_id = victim_type; victim_health.max_hull = units(100);
            health.profiles.push_back(victim_health);
            tactical::AbilityProfile buzz;
            buzz.kind = K::buzz_droids; buzz.recharge_frames = 60;
            buzz.buzz = tactical::BuzzDroidsProfile{droid_type, units(5), units(400), units(300), units(50),
                units(20), units(1), units(20), 1, 2, 0};
            tactical::AbilityTable table; table.profiles = {unit_of(plain_type, buzz)};
            tactical::CombatTable combat;
            for (const auto type : {droid_type, victim_type}) {
                tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
                combat.profiles.push_back(profile);
            }
            auto movement = motion();
            auto droid_motion = movement.profiles.front();
            droid_motion.type_id = droid_type;
            movement.profiles.push_back(droid_motion);
            tactical::Footprint droid_footprint;
            droid_footprint.type_id = droid_type; droid_footprint.locomotor = droid_locomotor;
            auto victim_footprint = droid_footprint;
            victim_footprint.type_id = victim_type; victim_footprint.locomotor = true;
            movement.footprints = {droid_footprint, victim_footprint};
            auto created = tactical::TacticalSession::create(initial, {}, health, movement,
                std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-73: hostile droids with authored locomotor flags create");
            if (!created) continue;
            auto world = std::move(created).value();
            tactical::AbilityPayload launch{K::buzz_droids, tactical::AbilityAction::activate};
            launch.position = at(100, 0);
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, launch))), "first hostile swarm launches");
            launch.position = at(140, 0);
            expect(static_cast<bool>(world.submit(command(0, 2, 0, 2, launch))), "second hostile swarm launches");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::optional<eawr::sim::EntityId> first, second;
            for (unsigned frame = 0; frame < 3; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WAB-72: hostile swarm commit succeeds after an earlier droid kill");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                if (frame == 0) {
                    for (const auto& unit : world.units()) {
                        if (unit.type_id != droid_type) continue;
                        (unit.owner == 1 ? first : second) = unit.entity_id;
                    }
                    expect(first && second && *first < *second, "hostile swarm fixture orders the killing droid first");
                }
                if (frame == 1 && first && second) {
                    const auto survivor = world.durability_state(*first);
                    const auto victim = world.durability_state(*second);
                    expect(survivor && survivor->hull == units(19), "killed hostile droid does not commit its staged damage");
                    expect(droid_locomotor ? !victim : victim && victim->hull == units(19),
                        "WAB-73: motion data alone does not make a droid a locomotor victim");
                    const auto& events = step.value().snapshot->events();
                    expect(std::count_if(events.begin(), events.end(), [&](const tactical::Event& event) {
                        return event.kind == tactical::EventKind::unit_destroyed && event.unit == *second;
                    }) == (droid_locomotor ? 1 : 0), "hostile droid destruction is emitted exactly once");
                    const auto stationary = world.durability_state(3);
                    expect(stationary && stationary->hull == units(80),
                        "WAB-73: a locomotor victim without a motion profile still takes damage");
                }
            }
            expect(hashes.size() == 3, "hostile swarm service continues after the killing tick");
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "hostile swarm hashes equal on 1/2/4/8 workers");
        }
    }
}

void test_banked_ability_geometry() {
    using K = tactical::AbilityKind;
    // ABF-01/02: distinguish the rolled bone/box from the level geometry, including
    // collection admission outside the level box. No retail fixture or pin is changed.
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto kind = scenario == 0 ? K::energy_weapon : scenario == 1 ? K::buzz_droids : K::corrupt_systems;
            auto initial = setup();
            const tactical::PlayerId victim_owner = scenario == 4 ? 1U : 2U;
            initial.units = {{2, frigate_type, victim_owner, at(0, 0), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            health.profiles[1].hardpoints = {{tactical::HardpointRole::other, true, units(100)},
                {tactical::HardpointRole::other, true, units(100)}};
            health.profiles[1].destroyed_with_hardpoints = true;
            constexpr tactical::TypeId droid_type = 30;
            tactical::DurabilityProfile droid_health;
            droid_health.type_id = droid_type; droid_health.max_hull = units(20);
            health.profiles.push_back(droid_health);
            auto movement = motion();
            movement.profiles[1].rate_of_turn = decimal("0.5");
            movement.profiles[1].turn_in_place_slowdown = units(1);
            movement.profiles[1].roll_rate = units(3);
            movement.profiles[1].bank_angle = units(60);
            tactical::CombatProfile target;
            target.type_id = frigate_type; target.category_bits = 2;
            target.hardpoints = {{0, at(0, 100, 0), true}, {1, at(0, 0, 100), true}};
            target.collision = tactical::CollisionBox{at(-1, -20, -100), at(1, 20, 100)};
            tactical::CombatProfile source; source.type_id = plain_type; source.category_bits = 1;
            tactical::CombatTable combat; combat.profiles = {target, source};
            tactical::AbilityProfile ability;
            ability.kind = kind; ability.recharge_frames = 100;
            ability.expiration_frames = scenario == 0 ? 20 : 0;
            ability.recharge_at_activation = scenario == 2 || scenario == 3;
            ability.effective_radius = units(5); ability.gui_activated_ability_name = "banked";
            auto holder = unit_of(plain_type, ability);
            tactical::SpecialAbilityProfile nested;
            nested.name = "banked"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.kind = scenario == 0 ? tactical::SpecialAbilityKind::energy_weapon : tactical::SpecialAbilityKind::corrupt_systems;
            nested.service_interval = 1; nested.beam_max_range = units(500);
            nested.damage_per_frame = units(5); nested.filter.applicable_categories = 2;
            nested.corrupt_max_range = units(350); nested.corrupt_duration_frames = 30;
            nested.corrupt_drain_per_frame = units(1); nested.corrupt_inaccuracy = units(10);
            if (scenario == 1) holder.abilities[0].buzz = tactical::BuzzDroidsProfile{droid_type,
                units(1), units(400), units(300), units(10), units(5), units(1), units(20), 1, 2, 0};
            else if (scenario == 4) {
                holder.abilities.clear();
                nested.kind = tactical::SpecialAbilityKind::force_healing;
                nested.style = tactical::SpecialActivationStyle::space_automatic;
                nested.initially_enabled = true; nested.heal_range = units(5);
                nested.heal_amount = units(5); nested.heal_interval_frames = 1;
                holder.special = {nested};
            } else holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "ABF-01: banked ability fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto& world = created.value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(static_cast<bool>(world.submit(command(0, victim_owner, 0, 2, tactical::FacePayload{at(0, -10000)}))),
                "ABF-01: victim starts an in-place banked turn");
            if (scenario == 4) expect(static_cast<bool>(world.submit(command(49, 1, 1, 2, tactical::DamagePayload{units(50), 0}))),
                "ABF-02: banked healing victim has a damaged standing point");
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 50; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "ABF-01: bank warmup succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
            }
            const auto drawn = world.snapshot()->instances().front().fixed_transform;
            const auto state = world.units().front();
            const auto level = math::to_matrix(state.rotation, state.position).value();
            expect(drawn != level, "ABF-01: fixture has nonzero bank");
            auto point = math::transform_point(scenario == 3 ? level : drawn,
                scenario < 2 ? at(0, 100, 0) : at(0, scenario == 3 ? 18 : 0, 90)).value();
            auto origin = point;
            for (std::size_t axis = 0; axis < 3; ++axis) {
                auto* coordinate = axis == 0 ? &origin.x : axis == 1 ? &origin.y : &origin.z;
                *coordinate = Fixed::from_raw(coordinate->raw() - drawn.rows[axis][0].raw() * 10);
            }
            if (scenario < 2) {
                const auto distance = [&](const math::Mat3x4& matrix, const math::Vec3 local) {
                    const auto p = math::transform_point(matrix, local).value();
                    return math::length(math::Vec3{Fixed::from_raw(p.x.raw() - origin.x.raw()),
                        Fixed::from_raw(p.y.raw() - origin.y.raw()), Fixed::from_raw(p.z.raw() - origin.z.raw())}).value();
                };
                expect(distance(drawn, target.hardpoints[0].position) < distance(drawn, target.hardpoints[1].position)
                    && distance(level, target.hardpoints[0].position) > distance(level, target.hardpoints[1].position),
                    "ABF-01: rolled and level frames select different nearest hardpoints");
            } else {
                tactical::detail::CombatUnit geometry; geometry.profile = &target; geometry.transform = drawn;
                const auto banked_overlap = tactical::detail::collision_overlaps_sphere(geometry, point, units(5));
                geometry.transform = level;
                const auto level_overlap = tactical::detail::collision_overlaps_sphere(geometry, point, units(5));
                expect(banked_overlap && level_overlap && banked_overlap.value() == (scenario != 3)
                    && level_overlap.value() == (scenario == 3), "ABF-02: rolled and level boxes disagree on sphere overlap");
            }
            if (scenario == 4) origin = point;
            const auto spawned = world.stage_spawn({0, plain_type, 1, origin, math::identity_quat(), {}});
            expect(static_cast<bool>(spawned),
                "ABF-01: ability holder stages next to the rolled bone or box");
            if (!spawned) continue;
            const auto holder_id = spawned.value();
            tactical::AbilityPayload payload{kind, tactical::AbilityAction::activate,
                scenario == 0 ? 2U : eawr::sim::invalid_entity_id, tactical::no_hardpoint};
            if (scenario != 0) payload.position = point;
            if (scenario != 4) {
                const auto submitted = world.submit(command(50, 1, 0, holder_id, payload));
                expect(static_cast<bool>(submitted), "ABF-01: banked ability command submits");
                if (!submitted) std::cerr << submitted.error().message << '\n';
            }
            for (unsigned frame = 0; frame < 3; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "ABF-01: banked ability service succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256 + step.value().snapshot->sha256());
                if (frame == 0 && scenario == 0)
                    expect(world.ability_state(holder_id)->slots.front().target_hardpoint == 0,
                        "ABF-01: energy fallback chooses the rolled nearest standing hardpoint");
                if (frame == 0 && (scenario == 2 || scenario == 3)) {
                    const auto instances = world.snapshot()->instances();
                    const auto victim = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 2; });
                    expect(victim != instances.end() && victim->corrupt.has_value() == (scenario == 2),
                        "ABF-02: collection bounds and area overlap both follow the rolled hull");
                }
            }
            if (scenario < 2) expect(world.durability_state(2)->hardpoints[0] < units(100)
                && world.durability_state(2)->hardpoints[1] == units(100), "ABF-01: banked energy/Buzz damage reaches only the nearest rolled bone");
            if (scenario == 4) expect(world.durability_state(2)->hardpoints[0] > units(50)
                && world.durability_state(2)->hardpoints[1] == units(100), "ABF-02: healing reaches the rolled box outside the planar centre radius");
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "ABF-01/02: banked ability state and snapshot equality at 1/2/4/8 workers");
        }
    }
}

void test_hero_beams() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::energy_weapon, K::tractor_beam}) {
        for (const auto scenario : {0U, 1U, 2U, 3U, 4U, 5U}) {
            std::vector<std::string> baseline;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                auto initial = setup();
                initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                    {2, frigate_type, 2, at(scenario == 1 ? 300 : scenario == 2 ? 5 : 100, 0, 100), math::identity_quat(), {}}};
                auto health = durability();
                for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
                if (scenario == 3 && kind == K::energy_weapon)
                    health.profiles[1].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                        {tactical::HardpointRole::other, true, units(100)}};
                tactical::AbilityProfile ordinary;
                ordinary.kind = kind; ordinary.recharge_frames = 20;
                ordinary.expiration_frames = kind == K::energy_weapon ? 4 : 0;
                ordinary.gui_activated_ability_name = "beam";
                ordinary.modifiers.speed = decimal("0.8");
                ordinary.supports_autofire = true;
                auto holder = unit_of(corvette_type, ordinary);
                tactical::SpecialAbilityProfile nested;
                nested.name = "beam"; nested.style = tactical::SpecialActivationStyle::user_input;
                nested.kind = kind == K::energy_weapon ? tactical::SpecialAbilityKind::energy_weapon
                    : tactical::SpecialAbilityKind::tractor_beam;
                nested.service_interval = 1; nested.beam_min_range = units(5); nested.beam_max_range = units(300);
                nested.damage_per_frame = units(50);
                nested.target_speed_decrease = decimal("0.2"); nested.doubled_speed_categories = 2;
                nested.filter.applicable_categories = 2;
                holder.special = {nested};
                tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
                tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
                tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
                target.hero = scenario == 4;
                if (scenario == 3 && kind == K::energy_weapon)
                    target.hardpoints = {{0, at(-10, 0), true}, {1, at(10, 0), true}};
                tactical::CombatTable combat; combat.profiles = {source, target};
                auto movement = motion();
                if (kind == K::tractor_beam) {
                    movement.profiles[1].max_speed = units(2);
                    movement.profiles[1].acceleration = units(10);
                    movement.profiles[1].deceleration = units(10);
                }
                auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(created), "WHE-26/27: beam session binds typed ordinary and nested profiles");
                if (!created) continue;
                auto world = std::move(created).value();
                if (kind == K::tractor_beam && scenario != 1 && scenario != 2) expect(static_cast<bool>(world.submit(command(0, 2, 0, 2,
                    tactical::MovePayload{at(scenario == 1 ? 300 : scenario == 2 ? 5 : 100, 100, 100)}))),
                    "tractor fixture target starts a move");
                expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                    tactical::AbilityPayload{kind, tactical::AbilityAction::activate, 2, scenario == 3 ? 99U : tactical::no_hardpoint}))),
                    "beam targeted command validates, including an invalid requested hardpoint for fallback");
                if (scenario == 5) expect(static_cast<bool>(world.submit(command(2, 1, 1, 1,
                    tactical::AbilityPayload{kind, tactical::AbilityAction::deactivate}))), "beam release command submits");
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> hashes;
                for (unsigned frame = 0; frame < 6; ++frame) {
                    const auto stepped = world.step(executor);
                    expect(static_cast<bool>(stepped), "beam service tick succeeds");
                    if (!stepped) break;
                    hashes.push_back(stepped.value().state_sha256);
                    if (frame == 0) {
                        const auto slot = world.ability_state(1)->slots.front();
                        expect(slot.active == (scenario != 2), "WHE-58: adjusted maximum equality accepted, positive minimum equality refused; Z ignored");
                        if (scenario == 3 && kind == K::energy_weapon)
                            expect(slot.target_hardpoint == 0, "WHE-59: invalid explicit hardpoint falls back to nearest live targetable point");
                    }
                    if (kind == K::tractor_beam && frame <= 2) {
                        const auto instances = stepped.value().snapshot->instances();
                        const auto target_instance = std::find_if(instances.begin(), instances.end(), [](const auto& value) { return value.entity_id == 2; });
                        expect(target_instance != instances.end() && target_instance->in_tractor_beam == (scenario != 2 && !(frame == 2 && scenario == 5)),
                            "WHE-27: tractor held flag follows active source entries and release");
                        if ((frame == 0 || (frame == 2 && scenario == 5)) && scenario != 1 && scenario != 2) {
                            const auto moving = world.motion_state(2);
                            Fixed peak{};
                            if (moving) for (const auto& node : moving->nodes) peak = std::max(peak, node.speed);
                            const auto decrease = scenario == 2 || (frame == 2 && scenario == 5) ? Fixed{}
                                : math::Fixed::from_raw(nested.target_speed_decrease.raw() * (scenario == 4 ? 1 : 2));
                            const auto expected = math::multiply(units(2), Fixed::from_raw(one - decrease.raw())).value();
                            expect(peak == expected, "WHE-60: tractor replans moving target with doubled non-hero speed loss, hero exemption and release restoration");
                        }
                    }
                    if (frame == 2 && scenario == 3 && kind == K::energy_weapon)
                        expect(!world.ability_state(1)->slots.front().active && world.durability_state(2)->hardpoints[1] == units(100),
                            "WHE-26/59: dead selected hardpoint releases without retargeting the survivor");
                }
                const auto final = world.durability_state(2);
                if (kind == K::energy_weapon && scenario != 3) {
                    // The tick's impacts precede its commands; tick-2 release follows that tick's service.
                    const auto hits = scenario == 2 ? 0 : scenario == 5 ? 2 : 3;
                    expect(final && final->hull == units(3600 - hits * 50),
                        "WHE-26: Damage_Per_Frame enters direct ordinary damage once per service, stopping at release or expiry");
                }
                if (scenario == 5) expect(!world.ability_state(1)->slots.front().active
                    && world.ability_state(1)->slots.front().target == 0, "beam release clears tracked target and nested recipients");
                if (baseline.empty()) baseline = hashes;
                else expect(hashes == baseline, "beam state hashes equal at 1/2/4/8 workers");
                const auto record = world.record();
                const auto encoded = tactical::write_replay(record);
                const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
                expect(parsed && parsed.value() == record, "beam commands round-trip through the existing ability opcode");
            }
        }
    }
}

// WAB-36 (legacy #2997): the energy weapon and the tractor beam admit an enemy attack target, so a
// player the engine plays switches them on through the attack-state pass with no ability command:
// one per pass, in the nested abilities' authored order. Scenarios: 0 AI owner, energy weapon
// authored first; 1 the same unit owned by a human with autofire off; 2 AI owner, tractor first.
void test_beam_autofire() {
    using K = tactical::AbilityKind;
    for (const unsigned scenario : {0U, 1U, 2U}) {
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0, 100), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            health.profiles[1].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                {tactical::HardpointRole::other, true, units(100)}};
            tactical::UnitAbilityProfile holder{corvette_type, {}, false};
            std::vector<tactical::SpecialAbilityProfile> nested;
            for (const auto kind : {K::energy_weapon, K::tractor_beam}) {
                tactical::AbilityProfile ordinary;
                ordinary.kind = kind; ordinary.recharge_frames = 20;
                ordinary.expiration_frames = kind == K::energy_weapon ? 30 : 0;
                ordinary.gui_activated_ability_name = kind == K::energy_weapon ? "energy" : "tractor";
                holder.abilities.push_back(ordinary);
                tactical::SpecialAbilityProfile beam;
                beam.name = ordinary.gui_activated_ability_name; beam.style = tactical::SpecialActivationStyle::user_input;
                beam.kind = kind == K::energy_weapon ? tactical::SpecialAbilityKind::energy_weapon
                    : tactical::SpecialAbilityKind::tractor_beam;
                beam.service_interval = 1; beam.beam_min_range = units(5); beam.beam_max_range = units(300);
                beam.damage_per_frame = units(1);
                beam.target_speed_decrease = decimal("0.2"); beam.doubled_speed_categories = 2;
                beam.filter.applicable_categories = 2;
                nested.push_back(beam);
            }
            if (scenario == 2) std::swap(nested[0], nested[1]);
            holder.special = nested;
            tactical::AbilityTable table; table.profiles = {holder}; table.beam_damage_type = 0;
            if (scenario == 1) table.humans = {1};
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::WeaponProfile weapon;
            weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
            weapon.cone_width = units(360); weapon.cone_height = units(180);
            weapon.shot = tactical::ShotProfile{units(1), 0, units(1000), units(1000), true, true, {}};
            source.weapons = {weapon}; source.max_attack_distance = units(300);
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            target.living_projectile_collision = true;
            target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            target.hardpoints = {{0, at(-10, 0), true}, {1, at(10, 0), true}};
            tactical::CombatTable combat; combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            auto created = tactical::TacticalSession::create(initial, sensors, health, motion(), std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-36: beam autofire fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::array<std::optional<unsigned>, 2> locked{}; // energy, tractor
            for (unsigned frame = 0; frame < 8; ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "WAB-36: beam autofire tick succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto attack = world.combat_state(1);
                const auto slots = world.ability_state(1)->slots;
                for (std::size_t index = 0; index < 2; ++index) {
                    if (!slots[index].active || locked[index]) continue;
                    locked[index] = frame;
                    expect(attack && slots[index].target == attack->attack_target && slots[index].target == 2,
                        "WAB-36: the beam locks onto the unit's own attack target");
                    if (index == 0) expect(slots[index].target_hardpoint == 0,
                        "WHE-59: the autofired energy beam takes the nearest standing targetable hardpoint");
                }
                if (locked[1]) {
                    const auto instances = stepped.value().snapshot->instances();
                    const auto held = std::find_if(instances.begin(), instances.end(), [](const auto& value) { return value.entity_id == 2; });
                    expect(held != instances.end() && held->in_tractor_beam, "WHE-27: the autofired tractor holds its target");
                }
            }
            if (scenario == 1) expect(!locked[0] && !locked[1], "WAB-36: a human's beams stay off without autofire");
            else {
                expect(locked[0] && locked[1], "WAB-36: an AI owner's energy weapon and tractor beam both lock without a command");
                if (locked[0] && locked[1]) {
                    const auto first = scenario == 0 ? *locked[0] : *locked[1];
                    const auto second = scenario == 0 ? *locked[1] : *locked[0];
                    expect(second == first + 1, "WAB-36: one ability per pass, in the nested abilities' authored order");
                }
            }
            const auto record = world.record();
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                return std::holds_alternative<tactical::AbilityPayload>(entry.payload);
            }), "WAB-36: no ability command is recorded");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "WAB-36: beam autofire hashes equal at 1/2/4/8 workers");
        }
    }
}

// WAB-36, WHE-24 (legacy #2997): CONCENTRATE_FIRE admits an enemy attack target, so a player the
// engine plays switches it on with no command; the command path's recruitment runs (the same-owner
// ally near the target gets the attack). The activation is engine-synthesised: no recorded command
// carries a bit-63 sequence, no order event announces it, and a replay re-derives it.
// Scenarios: 0 AI; 1 human disarmed; 2 human armed; 3 CF then beam; 4 beam then CF;
// 5 CF empty-query release then beam; 6 departed issuer without takeover;
// 7 explicitly disabled CF; 8 explicitly disabled beam.
void test_concentrate_autofire() {
    using K = tactical::AbilityKind;
    for (const unsigned scenario : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U}) {
        std::vector<std::string> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0, 100), math::identity_quat(), {}},
                {3, frigate_type, 1, at(150, 50), math::identity_quat(), {}}};
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            tactical::AbilityProfile ordinary;
            ordinary.kind = K::concentrate_fire;
            ordinary.effective_radius = units(scenario == 5 ? 1 : 6000);
            ordinary.expiration_frames = 30;
            ordinary.recharge_frames = 60;
            ordinary.supports_autofire = true;
            ordinary.gui_activated_ability_name = "focus";
            tactical::SpecialAbilityProfile nested;
            nested.name = "focus";
            nested.kind = tactical::SpecialAbilityKind::concentrate_fire;
            nested.style = tactical::SpecialActivationStyle::user_input;
            nested.service_interval = 1;
            nested.filter.applicable_categories = 2;
            nested.target_damage_increase = decimal("0.5");
            auto holder = unit_of(corvette_type, ordinary);
            holder.special = {nested};
            if (scenario == 7 || scenario == 8) holder.special.front().initially_enabled = false;
            if ((scenario >= 3 && scenario <= 6) || scenario == 8) {
                tactical::AbilityProfile beam;
                beam.kind = K::tractor_beam;
                beam.recharge_frames = 20;
                beam.gui_activated_ability_name = "tractor";
                holder.abilities.push_back(beam);
                tactical::SpecialAbilityProfile tractor;
                tractor.name = "tractor";
                tractor.kind = tactical::SpecialAbilityKind::tractor_beam;
                tractor.style = tactical::SpecialActivationStyle::user_input;
                tractor.service_interval = 1;
                tractor.beam_max_range = units(300);
                tractor.filter.applicable_categories = 2;
                if (scenario == 8) tractor.initially_enabled = false;
                holder.special.push_back(tractor);
                if (scenario == 4) std::swap(holder.special[0], holder.special[1]);
            }
            tactical::AbilityTable table; table.profiles = {holder};
            if (scenario == 1 || scenario == 2) table.humans = {1};
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::WeaponProfile weapon;
            weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
            weapon.cone_width = units(360); weapon.cone_height = units(180);
            weapon.shot = tactical::ShotProfile{units(1), 0, units(1000), units(1000), true, true, {}};
            source.weapons = {weapon}; source.max_attack_distance = units(300);
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            tactical::CombatTable combat; combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            const auto movement = motion();
            auto created = tactical::TacticalSession::create(initial, sensors, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-36: concentrate autofire fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            if (scenario == 2) expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                tactical::AbilityPayload{K::concentrate_fire, tactical::AbilityAction::autofire_on}))), "WAB-36: autofire switches on");
            if (scenario == 6) expect(static_cast<bool>(world.submit({{0, 1, 0}, {}, tactical::QuitPayload{}})),
                "WAB-36: the holder's issuer leaves without takeover");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::optional<unsigned> locked;
            bool announced = false;
            std::optional<unsigned> first_switch;
            std::optional<unsigned> first_target;
            std::optional<tactical::TacticalSession> restored;
            for (unsigned frame = 0; frame < (scenario == 6 ? 100U : 8U); ++frame) {
                const auto stepped = world.step(executor);
                expect(static_cast<bool>(stepped), "WAB-36: concentrate autofire tick succeeds");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                for (const auto& event : stepped.value().snapshot->events())
                    announced = announced || (event.sequence & (std::uint64_t{1} << 63)) != 0;
                if (restored) {
                    const auto resumed = restored->step(executor);
                    expect(resumed && resumed.value().state_sha256 == stepped.value().state_sha256,
                        "WAB-36: checkpoint continuation equals uninterrupted holder pass at 1/2/4/8 workers");
                }
                const auto attack_state = world.combat_state(1);
                if (!first_target && attack_state && attack_state->attack_target == 2) first_target = frame;
                const auto state = world.ability_state(1);
                if (scenario >= 3 && scenario <= 6 && state) {
                    const bool focus = state->slots[0].active;
                    const bool beam = state->slots[1].active;
                    if (!first_switch && (focus || beam)) {
                        first_switch = frame;
                        expect(first_target && frame == *first_target,
                            "WAB-36: the command-stage beam or concentrate activation keeps the first eligible tick");
                        expect(focus != beam, "WAB-36: only one handler switches on in the first eligible tick");
                        expect(focus == (scenario == 3 || scenario == 6), "WAB-36: authored order and actual switch-on choose the winner");
                    }
                    if (scenario == 5) expect(!focus, "WHE-24: empty same-owner query releases without consuming the pass");
                    if (scenario == 6) {
                        // WBF-48: quit commits at frame tail; the tick-zero pass may activate CF.
                        // Thereafter the ready beam and, later, the recharged CF remain refused.
                        expect(!beam && (!focus || state->slots[0].started_tick == 0),
                            "WAB-36: departed issuer cannot switch another handler on after the admitted quit tick");
                        expect(stepped.value().diagnostics.empty(), "WAB-36: departed holder is not requeued with rejected commands");
                    }
                }
                if (scenario == 7 || scenario == 8) {
                    expect(state && std::none_of(state->slots.begin(), state->slots.end(),
                        [](const auto& slot) { return slot.active; }),
                        "WAB-36: explicitly disabled CF and beam never switch on");
                    expect(attack_state && attack_state->attack_target == 2,
                        "WAB-36: disabled handlers still have an eligible attack target");
                }
                if (state && state->slots.front().active && !locked) {
                    locked = frame;
                    const auto attack = world.combat_state(1);
                    expect(attack && state->slots.front().target == attack->attack_target && state->slots.front().target == 2,
                        "WAB-36: concentrate fire locks the unit's own attack target");
                    expect(state->concentrate_recruits == std::vector<eawr::sim::EntityId>{3},
                        "WHE-24: the autofired activation recruits the same-owner ally near the target");
                    const auto live_units = world.units();
                    const auto ally = std::find_if(live_units.begin(), live_units.end(), [](const auto& unit) { return unit.entity_id == 3; });
                    expect(ally != live_units.end() && ally->order.kind == tactical::OrderKind::attack && ally->order.target == 2,
                        "WHE-24: the recruited ally takes the ordinary attack order");
                }
                if (!restored && state && std::any_of(state->slots.begin(), state->slots.end(),
                        [](const auto& slot) { return slot.active; })) {
                    const auto checkpoint = world.save_checkpoint_state();
                    auto fresh = tactical::TacticalSession::from_replay(world.record(), sensors, health, movement, std::nullopt, combat, {}, table);
                    expect(checkpoint && fresh, "WAB-36: checkpoint between passes and fresh session create");
                    if (checkpoint && fresh) {
                        restored.emplace(std::move(fresh).value());
                        expect(static_cast<bool>(restored->load_checkpoint_state(checkpoint.value())),
                            "WAB-36: checkpoint between passes loads");
                        const auto recaptured = restored->save_checkpoint_state();
                        expect(recaptured && recaptured.value() == checkpoint.value(),
                            "WAB-36: checkpoint round-trip preserves canonical bytes");
                    }
                }
            }
            if (scenario == 3 || scenario == 4 || scenario == 5)
                expect(first_switch && restored, "WAB-36: ordered holder fixture exercises a switch and checkpoint continuation");
            expect(!announced, "WAB-36: a synthesised activation announces no order event");
            expect(locked.has_value() == (scenario != 1 && scenario != 5 && scenario != 7 && scenario != 8), scenario == 1
                ? "WAB-36: a human's concentrate fire stays off without autofire"
                : "WAB-36: an armed concentrate fire switches on without a command");
            const auto record = world.record();
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                return (entry.key.sequence & (std::uint64_t{1} << 63)) != 0;
            }), "WAB-36: no recorded command carries a synthesised (bit-63) sequence");
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                const auto* ability = std::get_if<tactical::AbilityPayload>(&entry.payload);
                return ability && ability->action == tactical::AbilityAction::activate;
            }), "WAB-36: no activation command is recorded");
            if (baseline.empty()) baseline = hashes;
            else expect(hashes == baseline, "WAB-36: concentrate autofire hashes equal at 1/2/4/8 workers");
            const auto bytes = tactical::write_replay(record);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "WAB-36: the concentrate autofire record round-trips");
            if (parsed) {
                auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(replayed), "WAB-36: concentrate autofire replay binds");
                if (replayed) for (const auto& hash : hashes)
                    expect(replayed.value().step(executor).value().state_sha256 == hash,
                        "WAB-36: the replay re-derives the synthesised activation");
            }
        }
    }
}

// PU-39/WAB-36: an arriving beam craft can receive its team's retaliatory target,
// but the holder pass still waits for the arrival-end tick.
void test_arriving_beam_autofire() {
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, station_type, 1, at(-2000, 0), math::identity_quat(), {}},
            {2, plain_type, 2, at(100, 0), math::identity_quat(), {}}};
        auto movement = hangar_motion();
        movement.squadrons.spawners.clear();
        // U-09: tractor admission requires a ship locomotor on its target.
        auto enemy_motion = motion().profiles.front(); enemy_motion.type_id = plain_type;
        movement.profiles = {enemy_motion}; movement.rules = motion().rules;
        auto& squadron = movement.squadrons.squadrons.front();
        squadron.members = {xwing_type}; squadron.offsets = {at(0, 0)};
        squadron.attack_move_response_range = units(10000);
        squadron.autonomous_move_extension_vs_attacker = units(10000);
        auto health = durability();
        health.profiles = {{plain_type, units(10000), {}, false, {}},
            {station_type, units(10000), {}, false, {}}, {xwing_type, units(10000), {}, false, {}}};
        tactical::AbilityProfile beam;
        beam.kind = tactical::AbilityKind::tractor_beam;
        beam.recharge_frames = 20; beam.gui_activated_ability_name = "arrival-tractor";
        auto holder = unit_of(xwing_type, beam);
        tactical::SpecialAbilityProfile nested;
        nested.name = "arrival-tractor"; nested.kind = tactical::SpecialAbilityKind::tractor_beam;
        nested.style = tactical::SpecialActivationStyle::user_input; nested.service_interval = 1;
        nested.beam_max_range = units(10000); nested.filter.applicable_categories = 2;
        holder.special = {nested};
        tactical::AbilityTable abilities; abilities.profiles = {holder};
        tactical::CombatProfile craft; craft.type_id = xwing_type; craft.category_bits = 1;
        craft.max_attack_distance = units(10000);
        tactical::CombatProfile enemy; enemy.type_id = plain_type; enemy.category_bits = 2;
        enemy.max_attack_distance = units(10000);
        enemy.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
        tactical::WeaponProfile weapon;
        weapon.range = units(10000); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
        weapon.cone_width = units(360); weapon.cone_height = units(180);
        weapon.shot = tactical::ShotProfile{units(1), 0, units(10000), units(10000), true, true, {}};
        enemy.weapons = {weapon};
        craft.weapons = {weapon};
        craft.collision = enemy.collision;
        tactical::CombatTable combat; combat.profiles = {enemy, craft}; combat.unit_ai_types = {xwing_squadron};
        auto team = craft; team.type_id = xwing_squadron; team.weapons.clear();
        combat.retaliation_team_types = {team};
        tactical::EconomyRules economy;
        economy.players = {{1, units(100), 25, false, {}, 1, 3}};
        tactical::BuildOption option;
        option.type = xwing_squadron; option.price = units(10);
        option.build_frames = 1; option.ai_build_frames = 1; option.available = true;
        economy.menus = {{station_type, 1, {option}}};
        economy.footprints = {{xwing_squadron, std::nullopt, {}}};
        const std::vector<tactical::SensorProfile> sensors{{plain_type, units(10000)}, {xwing_type, units(10000)}};
        auto created = tactical::TacticalSession::create(initial, sensors, health, movement, std::nullopt,
            combat, {}, abilities, economy);
        expect(static_cast<bool>(created), "PU-39: arriving beam fixture creates");
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto world = std::move(created).value();
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::BuyPayload{xwing_squadron}))),
            "PU-39: beam squadron purchase submits");
        constexpr unsigned arrival_start = 2;
        expect(static_cast<bool>(world.submit({{arrival_start, 1, 1}, {}, tactical::ReinforcePayload{xwing_squadron, at(0, 0)}})),
            "PU-39: beam squadron reinforcement submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<unsigned> first_lock;
        bool propagated_in_lane = false;
        for (unsigned tick = 0; tick <= arrival_start + tactical::arrival_frames; ++tick) {
            // The reinforcement allocates craft 3, then team container 4. A hit on the
            // visible craft changes the team target and propagates it back to the roster.
            if (tick == arrival_start + tactical::arrival_visible_frame)
                expect(static_cast<bool>(world.submit(command(tick, 2, 0, 2, tactical::AttackPayload{3}))),
                    "PU-39: enemy attacks the visible arriving craft");
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "PU-39: arriving beam fixture steps");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            const auto state = world.ability_state(3);
            if (!state) continue;
            const auto attack = world.combat_state(3);
            const auto mind = world.squadron_state(4);
            if (world.arrivals().contains(3) && attack && mind && attack->attack_target == 2 && mind->target == 2) {
                const auto live = world.units();
                const auto source = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 3; });
                const auto target = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 2; });
                expect(source != live.end() && target != live.end()
                    && tactical::within_range(source->position, target->position, nested.beam_max_range, tactical::RangeMetric::planar),
                    "PU-39: propagated team target is in beam range during arrival");
                propagated_in_lane = true;
            }
            if (tick < arrival_start + tactical::arrival_frames)
                expect(!state->slots[0].active, "PU-39: an arriving holder never locks its propagated target");
            if (state->slots[0].active && !first_lock) first_lock = tick;
        }
        expect(propagated_in_lane, "PU-39: fixture witnesses a propagated team target inside the arrival lane");
        expect(first_lock && *first_lock == arrival_start + tactical::arrival_frames,
            "PU-39: first beam lock is exactly the arrival-end tick");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "PU-39: arriving beam hashes equal at 1/2/4/8 workers");
    }
}

void test_hero_wingmen() {
    using K = tactical::AbilityKind;
    constexpr tactical::TypeId escort = 23;
    for (const unsigned parent_kind : {0U, 1U, 2U}) for (const bool ordinary : {false, true}) for (const bool guard : {false, true}) {
        const bool parent = parent_kind == 1;
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{1, plain_type, 2, at(0, 0), math::identity_quat(), {}},
                {11, xwing_type, 1, at(0, 0), math::identity_quat(), {}}};
            if (parent) {
                initial.units.insert(initial.units.end(), {{10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}},
                    {12, escort, 1, at(-100, 0), math::identity_quat(), {}},
                    {13, escort, 1, at(100, 0), math::identity_quat(), {}}});
                std::sort(initial.units.begin(), initial.units.end(), [](const auto& a, const auto& b) { return a.entity_id < b.entity_id; });
                initial.squadrons = {{10, {11, 12, 13}}};
            }
            auto movement = hangar_motion();
            movement.squadrons.spawners.clear();
            movement.squadrons.craft.front().max_speed = decimal("0.0001");
            movement.squadrons.craft.front().min_speed = decimal("0.0001");
            movement.squadrons.craft.front().thrust = decimal("0.0001");
            auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
            movement.squadrons.craft.push_back(wing);
            auto& team = movement.squadrons.squadrons.front();
            const auto authored_count = parent_kind == 2 ? 17U : 3U;
            team.members.assign(authored_count, escort); team.members.front() = xwing_type;
            team.offsets.assign(authored_count, at(100, 0)); team.offsets.front() = at(0, 0);
            if (parent_kind == 2) {
                movement.squadrons.squadrons.insert(movement.squadrons.squadrons.begin(),
                    {xwing_type, {xwing_type}, {at(0, 0)}, {}, {}, {}, {}});
            }
            auto health = durability();
            health.profiles = {{plain_type, units(1000), {}, false, {}},
                {xwing_type, units(1000), {}, false, {}}, {escort, units(1000), {}, false, {}}};
            tactical::CombatTable combat;
            for (const auto type : {plain_type, xwing_type, escort}) {
                tactical::CombatProfile item; item.type_id = type;
                item.category_bits = 2;
                item.redirect_damage_to_teammates = type == xwing_type || (guard && type == escort);
                item.collision = tactical::CollisionBox{at(-3, -3, -3), at(3, 3, 3)};
                combat.profiles.push_back(item);
            }
            tactical::AbilityProfile replenish;
            replenish.kind = K::replenish_wingmen; replenish.recharge_frames = 6; replenish.replenish_team = xwing_squadron;
            tactical::AbilityProfile mode; mode.kind = K::invulnerability; mode.expiration_frames = 100;
            mode.modifiers.take_damage = decimal("0.1");
            auto leader = unit_of(xwing_type, mode); leader.abilities.push_back(replenish);
            tactical::AbilityProfile beam; beam.kind = K::energy_weapon; beam.recharge_frames = 5;
            beam.expiration_frames = 2; beam.effective_radius = units(10000); beam.gui_activated_ability_name = "routing-beam";
            auto source = unit_of(plain_type, beam);
            tactical::SpecialAbilityProfile service; service.name = "routing-beam";
            service.kind = tactical::SpecialAbilityKind::energy_weapon; service.service_interval = 1;
            service.style = tactical::SpecialActivationStyle::user_input; service.damage_per_frame = units(60);
            service.beam_max_range = units(10000); service.filter.applicable_categories = 2;
            source.special = {service};
            tactical::AbilityTable table; auto escort_mode = mode; escort_mode.modifiers.take_damage = decimal("0.5");
            table.profiles = {source, leader, unit_of(escort, escort_mode)}; table.beam_damage_type = 0;
            auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WHE-63/64 wingmen fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const auto fill = tactical::AbilityPayload{K::replenish_wingmen, tactical::AbilityAction::activate};
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 11, fill))), "wingmen activate submits");
            expect(static_cast<bool>(world.submit(command(0, 1, 1, 11,
                tactical::AbilityPayload{K::invulnerability, tactical::AbilityAction::activate}))), "leader take mode submits");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            auto first = world.step(executor);
            expect(static_cast<bool>(first), "wingmen first frame completes");
            if (!first) { std::cerr << first.error().message << '\n'; continue; }
            hashes.push_back(first.value().state_sha256);
            expect(world.squadrons().size() == 1 && world.squadrons()[0].members.size() == authored_count,
                "WHE-63 absent parent creates indices one onward, full parent retains members");
            auto state = world.ability_state(11);
            expect(state && state->slots[1].ready_tick == (parent ? 0U : 6U),
                "WHE-63 full team returns before recharge, newly formed team recharges");
            const auto dead = world.squadrons()[0].members[1];
            expect(static_cast<bool>(world.submit(command(1, 1, 0, dead, tactical::DamagePayload{units(2000), tactical::hull_target}))),
                "one escort destruction submits");
            auto killed = world.step(executor); expect(static_cast<bool>(killed), "escort destruction completes");
            if (!killed) continue;
            hashes.push_back(killed.value().state_sha256);
            expect(world.squadrons()[0].members.size() == authored_count - 1, "destroyed escort is pruned");
            const auto refill_tick = parent ? 2U : 6U;
            expect(static_cast<bool>(world.submit(command(refill_tick, 1, 0, 11, fill))), "partial team refill submits");
            for (unsigned frame = 2; frame <= refill_tick; ++frame) {
                auto step = world.step(executor); expect(static_cast<bool>(step), "refill frame completes");
                if (step) hashes.push_back(step.value().state_sha256);
            }
            expect(world.squadrons().size() == 1 && world.squadrons()[0].members.size() == authored_count
                && world.squadrons()[0].members.front() == 11 && world.squadrons()[0].members.back() > dead,
                "WHE-63 survivor roster retained and authored escort appended with a fresh stable ID");
            state = world.ability_state(11);
            expect(state && !state->slots[1].active && state->slots[1].ready_tick == refill_tick + 6,
                "WHE-63 replenishment is instantaneous with authored recharge");
            const auto damage_tick = refill_tick + 1;
            unsigned sequence = 0;
            for (const auto id : world.squadrons()[0].members) if (id != 11)
                expect(static_cast<bool>(world.submit(command(damage_tick, 1, sequence++, id,
                    tactical::AbilityPayload{K::invulnerability, tactical::AbilityAction::activate}))), "recipient take mode submits before script damage");
            expect(static_cast<bool>(world.submit(command(damage_tick, ordinary ? 2 : 1, sequence, ordinary ? 1 : 11,
                ordinary ? tactical::CommandPayload{tactical::AbilityPayload{K::energy_weapon, tactical::AbilityAction::activate, 11}}
                    : tactical::CommandPayload{tactical::DamagePayload{units(60), tactical::hull_target}}))), "routed damage submits");
            for (unsigned frame = damage_tick; frame <= damage_tick + 2; ++frame) {
                auto step = world.step(executor); expect(static_cast<bool>(step), "routed damage frame completes");
                if (step) hashes.push_back(step.value().state_sha256);
            }
            const auto leader_health = world.durability_state(11);
            expect(leader_health && leader_health->hull == Fixed::from_raw(units(1000).raw()
                - (ordinary ? 0 : math::multiply(units(60), decimal("0.1")).value().raw())),
                "WHE-64 ordinary hit leaves leader intact; LuaDebugDamage additionally applies leader's take mode");
            for (const auto id : world.squadrons()[0].members) if (id != 11) {
                const auto recipient = world.durability_state(id);
                expect(recipient && recipient->hull == Fixed::from_raw(units(1000).raw() - (guard ? 0
                    : math::multiply(Fixed::from_raw(units(60).raw() / (authored_count - 1)), decimal("0.5")).value().raw())), "WHE-64 raw shares use recipient take modes; recursion guard skips redirecting members");
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "wingmen refill and redirected damage state/events equal at 1/2/4/8 workers");
        }
    }
}

void test_hero_environment_damage_routing() {
    constexpr tactical::TypeId escort = 23;
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, 99, 2, at(0, 0), math::identity_quat(), {}},
            {10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}},
            {11, xwing_type, 1, at(0, 0), math::identity_quat(), {}},
            {12, escort, 1, at(-100, 0), math::identity_quat(), {}},
            {13, escort, 1, at(100, 0), math::identity_quat(), {}}};
        initial.squadrons = {{10, {11, 12, 13}}};
        auto movement = hangar_motion(); movement.squadrons.spawners.clear();
        auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
        movement.squadrons.craft.push_back(wing);
        movement.squadrons.squadrons.front().members = {xwing_type, escort, escort};
        movement.squadrons.squadrons.front().offsets = {at(0, 0), at(-100, 0), at(100, 0)};
        movement.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
            decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
        tactical::Footprint victim; victim.type_id = xwing_type; victim.asteroid_damage = true; victim.radius = units(3);
        victim.layer = tactical::SpaceLayer::frigate;
        tactical::Footprint field; field.type_id = 99; field.layer = tactical::SpaceLayer::static_object;
        field.asteroid_field = true; field.obstacle = true; field.radius = units(20);
        movement.footprints = {victim, field};
        auto health = durability(); health.profiles = {{xwing_type, units(1000), {}, false, {}}, {escort, units(1000), {}, false, {}}};
        health.damage->asteroid_damage = units(60); health.damage->asteroid_rate = units(1);
        tactical::CombatProfile leader; leader.type_id = xwing_type; leader.redirect_damage_to_teammates = true;
        tactical::CombatTable combat; combat.profiles = {leader};
        tactical::AbilityProfile mode; mode.kind = tactical::AbilityKind::invulnerability; mode.expiration_frames = 50;
        mode.modifiers.take_damage = decimal("0.1");
        auto recipient_mode = mode; recipient_mode.modifiers.take_damage = decimal("0.5");
        tactical::AbilityTable abilities; abilities.profiles = {unit_of(xwing_type, mode), unit_of(escort, recipient_mode)};
        auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "WHE-64 environmental routing fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        unsigned sequence = 0;
        for (const auto id : {11U, 12U, 13U}) expect(static_cast<bool>(world.submit(command(0, 1, sequence++, id,
            tactical::AbilityPayload{mode.kind, tactical::AbilityAction::activate}))), "environmental recipient mode submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto step = world.step(executor);
        expect(static_cast<bool>(step), "environmental routing ordered commit completes");
        if (!step) continue;
        expect(world.durability_state(11)->hull == units(1000) && world.durability_state(12)->hull == units(985)
            && world.durability_state(13)->hull == units(985), "WHE-64 one environmental raw hit routes before modes to both remote worker recipients");
        if (workers == 1) reference = step.value().state_sha256;
        else expect(step.value().state_sha256 == reference, "environmental recipient commit is equal at 1/2/4/8 workers");
    }
}

// #942: the stored roster keeps destroyed escort IDs; WHE-64 shares count only living members, and a hero
// whose escorts are all destroyed takes its own ordinary damage until a refill.
void test_hero_damage_after_escorts_die() {
    constexpr tactical::TypeId escort = 23;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto initial = setup();
        initial.units = {{1, 99, 2, at(0, 0), math::identity_quat(), {}},
            {10, xwing_squadron, 1, at(0, 0), math::identity_quat(), {}},
            {11, xwing_type, 1, at(0, 0), math::identity_quat(), {}},
            {12, escort, 1, at(-100, 0), math::identity_quat(), {}},
            {13, escort, 1, at(100, 0), math::identity_quat(), {}}};
        initial.squadrons = {{10, {11, 12, 13}}};
        auto movement = hangar_motion(); movement.squadrons.spawners.clear();
        auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
        movement.squadrons.craft.push_back(wing);
        movement.squadrons.squadrons.front().members = {xwing_type, escort, escort};
        movement.squadrons.squadrons.front().offsets = {at(0, 0), at(-100, 0), at(100, 0)};
        movement.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
            decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45, units(50)};
        tactical::Footprint victim; victim.type_id = xwing_type; victim.asteroid_damage = true; victim.radius = units(3);
        victim.layer = tactical::SpaceLayer::frigate;
        tactical::Footprint field; field.type_id = 99; field.layer = tactical::SpaceLayer::static_object;
        field.asteroid_field = true; field.obstacle = true; field.radius = units(20);
        movement.footprints = {victim, field};
        auto health = durability(); health.profiles = {{xwing_type, units(1000), {}, false, {}}, {escort, units(1000), {}, false, {}}};
        health.damage->asteroid_damage = units(60); health.damage->asteroid_rate = units(1);
        tactical::CombatProfile leader; leader.type_id = xwing_type; leader.redirect_damage_to_teammates = true;
        tactical::CombatTable combat; combat.profiles = {leader};
        auto created = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, combat, {}, {});
        expect(static_cast<bool>(created), "#942 escort-death fixture creates");
        if (!created) continue;
        auto world = std::move(created).value();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto frame = [&] {
            auto step = world.step(executor); expect(static_cast<bool>(step), "#942 escort-death frame completes");
            if (step) hashes.push_back(step.value().state_sha256);
            return static_cast<bool>(step);
        };
        // Every frame the leader stays in the field and takes one ordinary 60-unit environmental hit.
        if (!frame()) continue;
        expect(world.durability_state(11)->hull == units(1000) && world.durability_state(13)->hull == units(970),
            "WHE-64 two living escorts split the hit; the hero stays intact");
        expect(static_cast<bool>(world.submit(command(1, 1, 0, 12, tactical::DamagePayload{units(2000), tactical::hull_target}))),
            "#942 first escort kill submits");
        if (!frame()) continue;
        expect(world.squadrons()[0].members.size() == 2, "#942 first destroyed escort is pruned");
        const auto before = world.durability_state(13)->hull;
        if (!frame()) continue;
        expect(world.durability_state(11)->hull == units(1000)
            && world.durability_state(13)->hull == Fixed::from_raw(before.raw() - units(60).raw()),
            "#942 the share count is the living members: the last escort takes the whole hit");
        expect(static_cast<bool>(world.submit(command(3, 1, 0, 13, tactical::DamagePayload{units(2000), tactical::hull_target}))),
            "#942 last escort kill submits");
        if (!frame() || !frame()) continue;
        expect(world.squadrons()[0].members.size() == 1, "#942 both escorts are pruned from the squadron");
        expect(world.durability_state(11)->hull < units(1000), "#942 hero with no living escort takes its own damage");
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "#942 escort-death routing equal at 1/2/4/8 workers");
    }
}

void test_hunt_destinations() {
    const std::optional<std::array<Fixed, 4>> area = std::array{units(-2000), units(-2000), units(2000), units(2000)};
    const std::vector<tactical::HuntEnemy> enemies = {{at(600, 700, 20), true, false}, {at(-700, -800, 30), false, true}};
    tactical::CombatRandom sensitive(1036, 0, 1, 0xfffe0006U);
    auto preferred = tactical::hunt_destination(at(0, 0, 50), area, units(100), true, enemies,
        [](math::Vec3) { return false; }, sensitive);
    expect(preferred && preferred.value() == enemies[1].position, "WAB-54: force-sensitive preference without offsets");
    tactical::CombatRandom fallback(1036, 0, 1, 0xfffe0006U);
    const std::optional<std::array<Fixed, 4>> inset = std::array{units(-1000), units(-1000), units(1000), units(1000)};
    const auto clamped = tactical::hunt_destination(at(-4000, -4000), inset, units(500), true, {},
        [](math::Vec3) { return false; }, fallback);
    expect(clamped && clamped.value() == at(-750, -750),
        "WAB-54: adjusted fallback clamps by half the craft's authored reveal range");
    std::size_t point_checks = 0;
    bool saw_fog_branch = false;
    for (std::uint64_t seed = 0; seed < 32 && !saw_fog_branch; ++seed) {
        tactical::CombatRandom random(seed, 0, 1, 0xfffe0006U);
        point_checks = 0;
        const auto chosen = tactical::hunt_destination(at(0, 0, 50), area, units(100), false, enemies,
            [&](math::Vec3) { ++point_checks; return true; }, random);
        expect(chosen && chosen.value().z == units(50), "WAB-54: ordinary hunters preserve their height");
        if (point_checks != 0) {
            expect(point_checks == 99, "WAB-54: exactly 99 diagonal fog samples");
            saw_fog_branch = true;
        } else {
            expect(chosen && chosen.value().x > enemies[0].position.x && chosen.value().x < units(1000)
                && chosen.value().y > enemies[0].position.y && chosen.value().y < units(1100),
                "WAB-54: fogged enemy preferred before positive XY offsets");
        }
    }
    expect(saw_fog_branch, "HUNT contract exercises the fog sampling branch");
    tactical::CombatRandom no_bounds(1036, 0, 1, 0xfffe0006U);
    const auto retained = tactical::hunt_destination(at(10, 20, 30), std::nullopt, units(100), true, enemies,
        [](math::Vec3) { return false; }, no_bounds);
    expect(retained && retained.value() == at(10, 20, 30), "WAB-54: no bounds keeps the base position");
}

void test_hunt_ship_order() {
    tactical::AbilityProfile hunt;
    hunt.kind = tactical::AbilityKind::hunt;
    tactical::AbilityTable table;
    table.profiles = {unit_of(corvette_type, hunt)};
    const auto created_setup = setup();
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(created_setup, {}, {}, motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "WAB-52: lone hunter session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::MovePayload{at(2000, -1500)}))), "ship move submits");
        expect(static_cast<bool>(session.submit(command(1, 1, 1, 1, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}))), "ship Hunt submits");
        expect(static_cast<bool>(session.submit(command(32, 1, 2, 1, tactical::AttackPayload{3}))), "replacement attack submits");
        expect(static_cast<bool>(session.submit(command(33, 1, 3, 1, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::autofire_on}))), "autofire command is recordable");
        expect(static_cast<bool>(session.submit(command(34, 1, 4, 1, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}))), "ship Hunt restarts");
        expect(static_cast<bool>(session.submit(command(35, 1, 5, 1, tactical::StopPayload{}))), "ship Stop submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 70; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "lone hunter advances");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            if (tick == 1 || tick == 31) {
                const auto movement = session.motion_state(1);
                expect(movement && movement->kind == tactical::MotionKind::path && movement->target == at(2000, -1500),
                    "WAB-04/53/56: activation and later service preserve a move already under way");
            }
            if (tick == 32) {
                const auto state = session.ability_state(1);
                expect(state && !state->slots[0].active && state->slots[0].ready_tick == 0,
                    "WAB-51: attack cancels a lone hunter without recharge");
            }
            if (tick == 33) {
                const auto events = stepped.value().snapshot->events();
                expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
                }), "WAB-50: Hunt has no autofire");
            }
            if (tick >= 35) {
                const auto state = session.ability_state(1);
                const auto movement = session.motion_state(1);
                expect(state && !state->slots[0].active && state->slots[0].ready_tick == 0,
                    "WAB-51: Stop ends a lone hunter without recharge or later patrol restart");
                expect(movement && movement->kind == tactical::MotionKind::none,
                    "WAB-05/51: Stop ends the lone hunter's movement");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "lone hunters remain deterministic");
    }
}

void test_hunt_session() {
    tactical::TacticalSetup initial;
    initial.seed = 1036;
    initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    initial.units = {{10, xwing_squadron, 1, at(-500, 0), math::identity_quat(), {}},
        {11, xwing_type, 1, at(-500, 0), math::identity_quat(), {}},
        {12, xwing_type, 1, at(-500, 0), math::identity_quat(), {}}};
    initial.squadrons = {{10, {11, 12}}};
    tactical::AbilityProfile hunt;
    hunt.kind = tactical::AbilityKind::hunt;
    tactical::AbilityTable table;
    table.profiles = {unit_of(xwing_type, hunt)};
    const std::vector<tactical::PlayerCommand> orders = {
        command(0, 1, 0, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(20, 1, 1, 11, tactical::MovePayload{at(0, 0)}),
        command(30, 1, 2, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(40, 1, 3, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::deactivate}),
        command(45, 1, 4, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(50, 1, 5, 10, tactical::GuardPayload{at(0, 0)}),
        command(55, 1, 6, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(58, 2, 0, 10, tactical::StopPayload{}),
        command(60, 1, 7, 10, tactical::StopPayload{}),
        command(65, 1, 8, 10, tactical::AbilityPayload{tactical::AbilityKind::hunt, tactical::AbilityAction::activate}),
        command(70, 1, 9, 11, tactical::StopPayload{}),
    };
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, {}, hangar_motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "WAB-50: hunt squadron session is valid");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& order : orders) expect(static_cast<bool>(session.submit(order)), "hunt command is recordable");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 110; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "hunt session step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto a = session.ability_state(11), b = session.ability_state(12);
            const bool active = tick < 20 || (tick >= 30 && tick < 40) || (tick >= 45 && tick < 50)
                || (tick >= 55 && tick < 60) || (tick >= 65 && tick < 70);
            expect(a && b && a->slots[0].active == active && b->slots[0].active == active,
                "WAB-50/51: accepted orders and switch-off control the whole squadron; foreign Stop does not");
            if (tick == 40 || tick == 60 || tick >= 70) {
                const auto mind = session.squadron_state(10);
                expect(mind && mind->mode == tactical::SquadronMode::idle,
                    "WAB-05/51: switch-off and team/member Stop end patrol across later service ticks");
            }
            if (tick == 58) {
                const auto events = stepped.value().snapshot->events();
                expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::EventKind::order_rejected
                        && event.reason == tactical::RejectReason::unit_not_owned;
                }), "WAB-51: a rejected foreign Stop leaves Hunt running");
            }
            if (tick == 0) {
                const auto units = session.units();
                const auto container = std::find_if(units.begin(), units.end(), [](const auto& unit) { return unit.entity_id == 10; });
                expect(container != units.end() && container->order.kind == tactical::OrderKind::attack_move,
                    "WAB-55: hunt sends the container on an attack-move");
                expect(a && a->slots[0].expires_tick == 0 && a->slots[0].ready_tick == 0 && !a->slots[0].autofire,
                    "WAB-50: no duration, recharge or autofire");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "hunt hashes are equal for 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "HUNT uses the existing ability command encoding");
        if (!encoded) continue;
        const auto parsed = tactical::parse_replay(encoded.value());
        expect(parsed && parsed.value() == session.record(), "HUNT command bytes round trip");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, hangar_motion(), std::nullopt, {}, {}, table);
        expect(static_cast<bool>(playback), "HUNT opcode 8 round trip creates a session");
        if (playback) {
            for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
                auto step = playback.value().step(executor);
                expect(step && step.value().state_sha256 == hashes[tick], "HUNT replay matches every live frame");
            }
        }
    }
}

void test_projectile_defence_producers() {
    namespace detail = tactical::detail;
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("MISSILE_SHIELD") == Kind::missile_shield, "WPJ-17: shield kind binds");
    expect(tactical::ability_kind("sensor_jamming") == Kind::sensor_jamming, "WHE-32: jammer kind binds");
    tactical::CombatProfile profile; profile.type_id = 1;
    tactical::CombatProfile passive = profile; passive.passive_missile_shield_radius = units(7);
    passive.ranged_target_z_adjust = units(2);
    tactical::CombatState combat;
    tactical::DurabilityState dead; dead.hull = {};
    tactical::MotionTable motion; tactical::MotionProfile mover; mover.type_id = 1;
    motion.profiles = {mover};
    const std::vector<tactical::SnapshotPlayer> players{{1, 1, false}, {2, 1, false}, {3, 3, false}, {4, 4, true}};
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        detail::CombatWorld world; world.relationships = players; world.motion = &motion;
        world.units.resize(10);
        for (std::size_t slot = 0; slot < world.units.size(); ++slot) {
            auto& unit = world.units[slot]; unit.id = slot + 1; unit.type_id = 1;
            unit.owner = 1; unit.profile = &profile; unit.combat = &combat;
        }
        world.units[0].profile = &passive;
        world.units[3].owner = 2; // another player on the same team
        world.units[4].owner = 4; // neutral, not allied
        world.units[5].owner = 3; // enemy
        world.units[6].position = at(0, 0, 11); // planar inclusion is insufficient
        world.units[7].combat = nullptr;
        world.units[8].type_id = 2; // no locomotor
        world.units[9].durability = &dead;
        world.units[3].position = at(0, 0, 10); // inclusive spatial boundary
        std::vector<detail::ProjectileDefenceAbilityInput> inputs(10);
        inputs[1] = {true, false, units(8), {}};
        inputs[2] = {false, true, {}, units(10)};
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto prepared = detail::prepare_projectile_defences(world, inputs, executor, std::vector<eawr::sim::EntityId>{3, 2, 1});
        expect(static_cast<bool>(prepared), "WPJ-17/WHE-32: production copied inputs prepare");
        if (!prepared) continue;
        const auto& sources = world.projectile_defences.sources;
        expect(sources.size() == 3 && sources[0].id == 3 && sources[1].id == 2 && sources[2].id == 1,
            "WPJ-17: source registration order survives spatial indexing");
        expect(world.units[0].sensor_jammed && world.units[2].sensor_jammed && world.units[3].sensor_jammed
            && world.units[4].sensor_jammed, "WHE-32: self, ally and nonenemy neutral receive inclusive 3D stamps");
        for (std::size_t slot = 5; slot < 10; ++slot)
            expect(!world.units[slot].sensor_jammed, "WHE-32: enemy, height, no combat, no locomotor and dead gates");
        tactical::CraftProfile craft; craft.type_id = 2;
        motion.squadrons.craft = {craft};
        prepared = detail::prepare_projectile_defences(world, inputs, executor, std::vector<eawr::sim::EntityId>{3, 2, 1});
        expect(prepared && world.units[8].sensor_jammed, "WHE-32: craft locomotor recipients are included");
        motion.squadrons.craft.clear();
        expect(sources.size() == 3, "WHE-32: ordinary recipients are stamped without registering as emitters");
        expect(sources.size() == 3 && sources[2].adjusted_position.z == units(2), "WPJ-17: copied source uses adjusted height");
        const auto previous = sources;
        inputs[1].active_shield = false;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.size() == 3
            && !world.projectile_defences.sources[1].active_shield && world.projectile_defences.sources[1].sensor_jammed,
            "WPJ-17: an inactive declared shield remains eligible while externally jammed");
        inputs[2].active_jamming = false;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.size() == 1
            && world.projectile_defences.sources[0].id == 1, "WPJ-17: deactivation preserves passive source");
        expect(std::none_of(world.units.begin(), world.units.end(), [](const auto& unit) { return unit.sensor_jammed; }),
            "WHE-32: deactivation clears every current-frame stamp");
        expect(previous.size() == 3 && previous[0].sensor_jammed && previous[1].active_shield,
            "WPJ-17: published records are immutable copies of activation state");
        inputs[2].active_jamming = true; world.units[2].in_nebula = true;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && !world.units[0].sensor_jammed && !world.units[2].sensor_jammed,
            "WHE-31: nebula suspends stamping");
        world.units[2].in_nebula = false; world.units[2].durability = &dead;
        world.units[0].durability = &dead;
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.projectile_defences.sources.empty() && !world.units[3].sensor_jammed,
            "WPJ-17/WHE-32: source death removes registration and stamps");
        world.units[0].durability = nullptr; world.units[2].durability = nullptr;
        inputs[2].jamming_radius.reset(); world.units[2].profile = &passive;
        world.units[3].position = at(0, 0, 7);
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && world.units[3].sensor_jammed, "WHE-32: absent jamming radius falls back to passive radius");
        world.units[3].position = at(0, 0, 8);
        prepared = detail::prepare_projectile_defences(world, inputs, executor);
        expect(prepared && !world.units[3].sensor_jammed, "WHE-32: fallback radius excludes beyond-boundary recipient");
    }
    tactical::AbilityProfile jammer; jammer.kind = Kind::sensor_jamming;
    jammer.expiration_frames = 3; jammer.recharge_frames = 6;
    auto profile_of = unit_of(1, jammer);
    auto state = tactical::initial_abilities(profile_of);
    expect(tactical::activate_ability(jammer, state.slots[0], {}, 10).changed, "WHE-31: existing activation slot starts jamming");
    static_cast<void>(tactical::expire_abilities(profile_of, state, 12));
    expect(state.slots[0].active, "WHE-31: timer stays active before authored expiry boundary");
    static_cast<void>(tactical::expire_abilities(profile_of, state, 13));
    expect(!state.slots[0].active && state.slots[0].ready_tick == 19, "WHE-31: exact expiry starts existing recharge");
    auto invalid = tactical::AbilityTable{}; jammer.effective_radius = units(-1);
    invalid.profiles = {unit_of(1, jammer)};
    expect(!tactical::validate_abilities(invalid), "WPJ-17: invalid authored defence radius is refused");
}

void test_projectile_defence_transaction() {
    using Kind = tactical::AbilityKind;
    const auto order_of = [](const tactical::TacticalSession& session) {
        const auto bytes = session.canonical_state_bytes();
        const std::vector<std::uint8_t> tag{'P', 'D', 'E', 'F'};
        auto position = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end());
        std::vector<eawr::sim::EntityId> order;
        if (position == bytes.end()) return order;
        auto offset = static_cast<std::size_t>(position - bytes.begin()) + 4;
        const auto read = [&]() {
            std::uint64_t value = 0;
            for (unsigned shift = 0; shift < 64 && offset < bytes.size(); shift += 8)
                value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
            return value;
        };
        const auto count = read();
        if (count > 16) { expect(false, "PDEF test block has bounded source count"); return order; }
        for (std::uint64_t i = 0; i < count; ++i) order.push_back(read());
        return order;
    };
    class FailVisibility final : public eawr::sim::PartitionExecutor {
    public:
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (phase == "visibility") return eawr::core::Result<void>::failure({});
            return execute(count, partition);
        }
    };
    tactical::AbilityProfile jammer; jammer.kind = Kind::sensor_jamming;
    jammer.effective_radius = units(100); jammer.expiration_frames = 3;
    tactical::AbilityProfile shield; shield.kind = Kind::missile_shield; shield.effective_radius = units(100);
    tactical::AbilityTable abilities; abilities.profiles = {unit_of(corvette_type, jammer),
        unit_of(frigate_type, shield), unit_of(plain_type, jammer)};
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile; profile.type_id = type; combat.profiles.push_back(profile);
    }
    auto initial = setup();
    initial.units.push_back({4, plain_type, 1, at(0, 0), math::identity_quat(), {}});
    const std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 2, tactical::AbilityPayload{Kind::missile_shield, tactical::AbilityAction::activate}),
        command(0, 1, 1, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(1, 1, 0, 4, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(2, 1, 0, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::deactivate}),
        command(2, 1, 1, 1, tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
        command(3, 1, 0, 1, tactical::DamagePayload{units(100000), tactical::hull_target})};
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, durability(), motion(), std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "PDEF lifecycle session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& item : commands) expect(static_cast<bool>(session.submit(item)), "PDEF lifecycle command submits");
        expect(order_of(session).empty(), "PDEF is absent before any jammer activation");
        const auto before = session.canonical_state_bytes();
        expect(!session.step(FailVisibility{}), "PDEF failed frame reaches post-command failure seam");
        expect(session.canonical_state_bytes() == before && session.completed_tick() == 0,
            "PDEF failed frame rolls back registration and ability activation");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const std::vector<std::vector<eawr::sim::EntityId>> orders{{2, 1}, {2, 1, 4}, {2, 4, 1}, {2, 4}, {}, {}};
        for (std::size_t frame = 0; frame < orders.size(); ++frame) {
            session.scramble_storage_for_testing();
            const auto step = session.step(executor);
            expect(static_cast<bool>(step), "PDEF lifecycle frame succeeds after retry");
            if (!step) break;
            hashes.push_back(step.value().state_sha256);
            expect(order_of(session) == orders[frame], "PDEF retains activation order, appends reactivation, prunes death/expiry");
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "PDEF transaction hashes equal at 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == session.record(), "WPJ-17/WHE-32: appended kinds round trip through opcode 8");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, durability(), motion(), std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(playback), "PDEF lifecycle replay creates");
        if (playback) for (const auto& hash : hashes) {
            const auto step = playback.value().step(executor);
            expect(step && step.value().state_sha256 == hash, "PDEF replay reproduces source ordering at every frame");
        }
    }
    // Creation registers a declared shield immediately, even while inactive.
    // Exercise both orders in the command batch, including a post-command rollback.
    for (const bool reinforce_first : {true, false}) {
        std::vector<std::string> creation_reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto creation_setup = setup();
            tactical::EconomyRules economy;
            economy.players = {{1, units(100), 25, false, {}, 1, 3}};
            tactical::BuildOption option;
            option.type = frigate_type; option.price = units(10);
            option.build_frames = 1; option.ai_build_frames = 1; option.available = true;
            economy.menus = {{corvette_type, 1, {option}}};
            economy.footprints = {{frigate_type, std::nullopt, {}}};
            auto created = tactical::TacticalSession::create(creation_setup, {}, durability(), motion(),
                std::nullopt, combat, {}, abilities, economy);
            expect(static_cast<bool>(created), "PDEF reinforcement ordering session creates");
            if (!created) continue;
            auto session = std::move(created).value();
            expect(static_cast<bool>(session.submit(command(0, 1, 0, 1, tactical::BuyPayload{frigate_type}))),
                "PDEF reinforcement purchase submits");
            const auto activate_sequence = reinforce_first ? 1U : 0U;
            const auto reinforce_sequence = reinforce_first ? 0U : 1U;
            std::vector<tactical::PlayerCommand> batch{
                command(2, 1, activate_sequence, 1,
                    tactical::AbilityPayload{Kind::sensor_jamming, tactical::AbilityAction::activate}),
                {{2, 1, reinforce_sequence}, {}, tactical::ReinforcePayload{frigate_type, at(0, 0)}}};
            std::sort(batch.begin(), batch.end(), [](const auto& a, const auto& b) {
                return a.key.sequence < b.key.sequence;
            });
            for (const auto& item : batch) expect(static_cast<bool>(session.submit(item)),
                "PDEF same-frame creation commands submit in sequence order");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 3; ++frame) {
                if (frame == 2) {
                    const auto before = session.canonical_state_bytes();
                    expect(!session.step(FailVisibility{}), "PDEF creation frame reaches failure seam");
                    expect(session.canonical_state_bytes() == before && session.units().size() == 3,
                        "PDEF creation rollback restores ledger, entity allocation and reinforcement pool");
                }
                const auto step = session.step(executor);
                expect(static_cast<bool>(step), "PDEF creation frame succeeds after retry");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
            }
            expect(session.units().size() == 4 && session.arrivals().size() == 1,
                "PDEF fixture creates one reinforcement in the activation frame");
            const std::vector<eawr::sim::EntityId> expected = reinforce_first
                ? std::vector<eawr::sim::EntityId>{2, 4, 1} : std::vector<eawr::sim::EntityId>{2, 1, 4};
            expect(order_of(session) == expected,
                "PDEF static creation registers at its command position before or after jammer activation");
            if (workers == 1) creation_reference = hashes;
            else expect(hashes == creation_reference, "PDEF creation order hashes equal at 1/2/4/8 workers");
        }
    }
}

// WLS-01..03: shield-only drain, the owner's damage bonus, admission and release.
void test_leech_shields() {
    using K = tactical::AbilityKind;
    expect(tactical::ability_kind("leech_shields") == K::leech_shields && static_cast<unsigned>(K::leech_shields) == 23,
        "WLS-01: shield leech binds its reserved kind");
    for (unsigned scenario = 0; scenario < 8; ++scenario) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto start = setup();
            start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, scenario == 4 ? 1U : 2U, at(scenario == 5 ? 5 : scenario == 6 ? 301 : 100, 0, 200), math::identity_quat(), {}}};
            auto health = durability();
            health.damage->shield_recharge_frames = 1000;
            for (auto& row : health.profiles) { row.shield_refresh = {}; row.max_shields = units(100); }
            health.profiles[1].max_shields = scenario == 0 ? units(1000) : scenario == 1 ? units(2) : units(100);
            if (scenario == 7) {
                health.damage->shield_recharge_frames = 1;
                health.profiles[1].shield_refresh = units(1);
            }
            tactical::AbilityProfile leech;
            leech.kind = K::leech_shields; leech.expiration_frames = scenario == 3 ? 12U : 4U; leech.recharge_frames = 12;
            leech.recharge_at_activation = true; leech.supports_autofire = true; leech.gui_activated_ability_name = "leech";
            auto holder = unit_of(corvette_type, leech);
            tactical::SpecialAbilityProfile nested;
            nested.kind = tactical::SpecialAbilityKind::leech_shields;
            nested.name = "leech"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.service_interval = 1; nested.beam_min_range = units(5); nested.beam_max_range = units(300);
            nested.damage_per_frame = units(3); nested.leech_damage_bonus = units(3); nested.leech_duration_frames = leech.expiration_frames;
            nested.filter.applicable_categories = 2;
            holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder}; table.humans = {1, 2};
            tactical::CombatTable combat;
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            if (scenario == 0) {
                tactical::WeaponProfile weapon;
                weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
                weapon.cone_width = units(360); weapon.cone_height = units(180);
                weapon.shot = tactical::ShotProfile{units(10), 0, units(1000), units(1000), true, true, {}};
                source.weapons = {weapon}; source.max_attack_distance = units(300);
                target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            }
            combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            auto movement = motion();
            if (scenario == 3) {
                movement.profiles[1].max_speed = units(400);
                movement.profiles[1].acceleration = units(400);
                movement.profiles[1].deceleration = units(400);
                movement.profiles[1].rate_of_turn = units(360);
            }
            const auto create = [&]() { return tactical::TacticalSession::create(start, sensors, health, movement, std::nullopt, combat, {}, table); };
            auto created = create();
            expect(static_cast<bool>(created), "WLS-01: shield leech fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 1,
                tactical::AbilityPayload{K::leech_shields, tactical::AbilityAction::activate, 2}))), "WLS-01: targeted command submits");
            if (scenario == 0) expect(static_cast<bool>(world.submit(command(0, 1, 1, 1, tactical::AttackPayload{2}))),
                "WLS-02: source fires while leeching");
            if (scenario == 2) expect(static_cast<bool>(world.submit(command(2, 1, 1, 1,
                tactical::AbilityPayload{K::leech_shields, tactical::AbilityAction::deactivate}))), "WLS-03: cancel submits");
            if (scenario == 3) expect(static_cast<bool>(world.submit(command(2, 2, 0, 2, tactical::MovePayload{at(1000, 0, 200)}))),
                "WLS-03: target starts leaving range");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            unsigned boosted_hits = 0, ordinary_hits = 0;
            for (unsigned frame = 0; frame < 20; ++frame) {
                const auto result = world.step(executor);
                expect(static_cast<bool>(result), "WLS-02: shield drain tick succeeds");
                if (!result) break;
                hashes.push_back(result.value().state_sha256);
                const auto state = world.ability_state(1)->slots.front();
                const bool admitted = scenario < 4 || scenario == 7;
                if (frame == 0) expect(state.active == admitted && (!admitted || state.ready_tick == 12),
                    "WLS-01/03: hostile shielded range gate; recharge starts at activation");
                if (frame == 1 && admitted) expect(world.durability_state(2)->shields == (scenario == 0 ? units(957)
                    : scenario == 1 ? Fixed{} : scenario == 7 ? units(98) : units(97)),
                    "WLS-02: first drain is raw three; ordinary ten-damage shot becomes forty");
                if (scenario == 0) {
                    for (const auto& event : result.value().snapshot->combat_events())
                        if (event.kind == tactical::CombatEventKind::projectile_hit)
                            ++(frame < 4 ? boosted_hits : ordinary_hits);
                    expect(world.durability_state(2)->shields == units(1000 - std::min(frame, 3U) * 3
                        - boosted_hits * 40 - ordinary_hits * 10), "WLS-02: each hit uses the current lock's damage bonus");
                }
                if (scenario == 7 && frame == 2) expect(world.durability_state(2)->shields == units(96),
                    "WLS-02: normal regeneration continues between shield drains");
                expect(world.durability_state(1)->shields == units(100), "WLS-02: source gains no shields");
                expect(world.durability_state(2)->hull == health.profiles[1].max_hull, "WLS-02: drain never overflows to hull");
                if (scenario == 1 && frame >= 1) expect(!state.active && state.ready_tick == 12,
                    "WLS-03: depletion releases immediately and retains activation recharge");
                if (scenario == 2 && frame >= 2) expect(!state.active && state.ready_tick == 12,
                    "WLS-03: cancel retains activation recharge");
                if (scenario == 3 && frame == 10) {
                    expect(!state.active && state.ready_tick == 12,
                        "WLS-03: movement outside the planar range releases before duration expiry");
                    const auto live = world.units();
                    const auto moved = std::find_if(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 2; });
                    expect(moved != live.end() && moved->position.x > units(300),
                        "WLS-03: the movement fixture actually exits the beam range");
                }
                if (frame >= leech.expiration_frames && admitted) expect(!state.active && state.ready_tick == 12,
                    "WLS-03: duration expiry retains activation recharge");
            }
            if (scenario == 0) expect(boosted_hits > 0 && ordinary_hits > 0,
                "WLS-02: the fixture exercises weapon damage both during and after the lock");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WLS-02: every frame matches 1/2/4/8 workers");
            const auto record = world.record();
            const auto bytes = tactical::write_replay(record);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "WLS-01: targeted command round-trips");
            if (parsed) {
                auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(replayed), "WLS-02: replay binds");
                if (replayed) for (const auto& hash : hashes)
                    expect(replayed.value().step(executor).value().state_sha256 == hash, "WLS-02: replay reproduces drain and release");
            }
        }
    }
    // WLS-02: direct shield damage bypasses armor/modes and preserves independent pool/hull state.
    auto health = durability();
    auto state = tactical::full_durability(health.profiles.front());
    state.shields = units(2); state.energy = units(17);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 10);
    expect(state.shields == Fixed{} && state.energy == units(17) && state.hull == health.profiles.front().max_hull
        && state.depleted_frame == 10, "WLS-02: excess raw drain never reaches hull or energy and starts depletion");
    health.damage->depleted_increment_seconds = units(1);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 20);
    expect(state.depleted_frame == 20, "WLS-02: depleted direct damage extends the depletion window");
    state.shields = units(1);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 21);
    expect(state.shields == units(1) && state.depleted_frame == 21,
        "WLS-02: positive regenerated shields remain protected by an active depletion window");
    state.shields = units(50); state.depleted_frame.reset(); state.ion_storm_contact = 21;
    health.damage->ion_storm_disable_seconds = units(5);
    tactical::drain_shields(health.profiles.front(), *health.damage, state, units(5), 21);
    expect(state.shields == units(50), "WLS-02: an ion storm absorbs no direct shield damage");
}

// WAB-36: an autofired targeted ability locks onto its unit's own attack target with no ability command.
// Scenarios: 0 AI owner, its own targeting picks; 1 human, autofire off; 2 human, autofire switched on; 3 unshielded target;
// 4 attack target inside the nested minimum range; 5 no attack target.
void test_targeted_autofire() {
    using K = tactical::AbilityKind;
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto start = setup();
            // Two shielded enemies in beam range: the lock follows the unit's attack target, never a search.
            start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(200, 0, 200), math::identity_quat(), {}},
                {3, frigate_type, 2, at(-120, 0, 200), math::identity_quat(), {}}};
            auto health = durability();
            health.damage->shield_recharge_frames = 1000;
            for (auto& row : health.profiles) { row.shield_refresh = {}; row.max_shields = units(1000); }
            if (scenario == 3) health.profiles[1].max_shields = Fixed{};
            tactical::AbilityProfile leech;
            leech.kind = K::leech_shields; leech.expiration_frames = 30; leech.recharge_frames = 60;
            leech.recharge_at_activation = true; leech.supports_autofire = true; leech.gui_activated_ability_name = "leech";
            auto holder = unit_of(corvette_type, leech);
            tactical::SpecialAbilityProfile nested;
            nested.kind = tactical::SpecialAbilityKind::leech_shields;
            nested.name = "leech"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.service_interval = 1; nested.beam_min_range = units(scenario == 4 ? 250 : 5); nested.beam_max_range = units(300);
            nested.damage_per_frame = units(3); nested.leech_damage_bonus = units(3); nested.leech_duration_frames = leech.expiration_frames;
            nested.filter.applicable_categories = 2;
            holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder};
            table.humans = scenario == 1 || scenario == 2 ? std::vector<tactical::PlayerId>{1, 2} : std::vector<tactical::PlayerId>{2};
            tactical::CombatTable combat;
            tactical::CombatProfile source; source.type_id = corvette_type; source.category_bits = 1;
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            if (scenario != 5) {
                tactical::WeaponProfile weapon;
                weapon.range = units(300); weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
                weapon.cone_width = units(360); weapon.cone_height = units(180);
                weapon.shot = tactical::ShotProfile{units(10), 0, units(1000), units(1000), true, true, {}};
                source.weapons = {weapon}; source.max_attack_distance = units(300);
            }
            target.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
            combat.profiles = {source, target};
            const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(1000)}, {frigate_type, units(1000)}};
            const auto movement = motion();
            auto created = tactical::TacticalSession::create(start, sensors, health, movement, std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WAB-36: targeted autofire fixture binds");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            if (scenario != 0 && scenario != 5) expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::AttackPayload{2}))),
                "WAB-36: the source attacks the farther enemy");
            if (scenario == 2) expect(static_cast<bool>(world.submit(command(0, 1, 1, 1,
                tactical::AbilityPayload{K::leech_shields, tactical::AbilityAction::autofire_on}))), "WAB-36: autofire switches on");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            const bool locks = scenario == 0 || scenario == 2;
            std::vector<std::string> hashes;
            std::optional<unsigned> locked;
            eawr::sim::EntityId locked_target{};
            for (unsigned frame = 0; frame < 12; ++frame) {
                const auto result = world.step(executor);
                expect(static_cast<bool>(result), "WAB-36: targeted autofire tick succeeds");
                if (!result) break;
                // The ability phase reads this tick's targeting result; only tick 0 carries commands.
                const auto after = world.combat_state(1);
                const auto attack_target = after ? after->attack_target : eawr::sim::EntityId{};
                hashes.push_back(result.value().state_sha256);
                const auto state = world.ability_state(1)->slots.front();
                if (state.active && !locked) {
                    locked = frame;
                    locked_target = state.target;
                    expect(attack_target != 0 && state.target == attack_target && state.ready_tick == frame + 60U,
                        "WAB-36: the lock takes the unit's attack target and starts the recharge");
                    if (scenario == 2) expect(state.target == 2, "WAB-36: a human's ordered attack target is the one locked");
                }
                if (locked && state.active) expect(state.target == locked_target, "WAB-36: the lock keeps its target");
            }
            expect(locked.has_value() == locks, locks ? "WAB-36: an armed ready leech locks without a command"
                : "WAB-36: no lock without autofire, a shielded attack target in range, or an attack target");
            if (locks && locked) {
                expect(*locked <= 2, "WAB-36: the lock follows the attack target within the attack-state pass");
                expect(world.durability_state(locked_target)->shields < units(1000), "WLS-02: the autofired lock drains the target");
            }
            const auto record = world.record();
            expect(std::none_of(record.commands.begin(), record.commands.end(), [](const auto& entry) {
                const auto* ability = std::get_if<tactical::AbilityPayload>(&entry.payload);
                return ability && ability->action == tactical::AbilityAction::activate;
            }), "WAB-36: no activation command is recorded");
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "WAB-36: every frame matches 1/2/4/8 workers");
            const auto bytes = tactical::write_replay(record);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == record, "WAB-36: the record round-trips");
            if (parsed) {
                auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, movement, std::nullopt, combat, {}, table);
                expect(static_cast<bool>(replayed), "WAB-36: replay binds");
                if (replayed) for (const auto& hash : hashes)
                    expect(replayed.value().step(executor).value().state_sha256 == hash, "WAB-36: replay reproduces the autofired lock");
            }
        }
    }
}

// WAB-80: FULL_SALVO changes only hardpoints that author the full-salvo delay.
void test_full_salvo() {
    expect(tactical::ability_kind("full_salvo") == tactical::AbilityKind::full_salvo, "WAB-80: FULL_SALVO binds");
    expect(tactical::to_string(tactical::AbilityKind::full_salvo) == "FULL_SALVO", "WAB-80: FULL_SALVO's name");
    expect(static_cast<unsigned>(tactical::AbilityKind::full_salvo) == 19U, "WAB-80: the reserved kind number");

    tactical::AbilityProfile salvo;
    salvo.kind = tactical::AbilityKind::full_salvo;
    salvo.expiration_frames = 210; // Expiration_Seconds 7
    salvo.recharge_frames = 1350;  // Recharge_Seconds 45
    const tactical::AbilityTable ability_data{{unit_of(corvette_type, salvo)}, {1}};
    tactical::CombatTable combat;
    tactical::CombatProfile ship;
    ship.type_id = corvette_type; ship.category_bits = 1;
    ship.hardpoints = {{0, at(5, 0), true}, {1, at(-5, 0), true}};
    tactical::ShotProfile shot;
    shot.damage = units(1); shot.damage_type = 0; shot.speed = units(40); shot.max_travel = units(1500);
    // Hardpoint 0 is the missile launcher: six pulses 0.5 s apart, a 4 to 5 s recharge, multiplier 0.18.
    tactical::WeaponProfile missile;
    missile.hardpoint = 0; missile.range = units(1500);
    missile.min_recharge_hundredths = 400; missile.max_recharge_hundredths = 500;
    missile.pulse_count = 6; missile.pulse_delay_frames = 15;
    missile.fire_a = at(5, 2); missile.fire_b = at(5, 2);
    missile.full_salvo_delay = decimal("0.18");
    missile.cone_width = units(90); missile.cone_height = units(90);
    missile.opportunity_when_idle = true; missile.opportunity_when_targeting = true;
    missile.shot = shot;
    // Hardpoint 1 authors no multiplier: it keeps its cadence while FULL_SALVO is on.
    tactical::WeaponProfile laser = missile;
    laser.hardpoint = 1; laser.pulse_count = 2; laser.pulse_delay_frames = 6;
    laser.min_recharge_hundredths = 100; laser.max_recharge_hundredths = 100;
    laser.fire_a = at(-5, 2); laser.fire_b = at(-5, 2);
    laser.full_salvo_delay = units(1);
    ship.weapons = {missile, laser};
    ship.max_attack_distance = units(1500);
    tactical::CombatProfile target;
    target.type_id = plain_type; target.category_bits = 1;
    combat.profiles = {ship, target};
    auto start = setup();
    start.units[0].position = at(0, 0);
    start.units[2].position = at(800, 0);
    const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(2000)}};
    constexpr std::uint64_t ticks = 520;
    struct Fire {
        std::vector<std::uint64_t> missile, laser;
        std::vector<std::string> hashes;
        std::uint64_t ready_tick{};
        bool active_at_end{};
    };
    const auto play = [&](const bool use, const std::size_t workers) {
        Fire result;
        auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
            combat, {}, ability_data);
        expect(static_cast<bool>(created), "WAB-80: the session binds");
        if (!created) return result;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit(command(0, 1, 1, 1, tactical::AttackPayload{3}))),
            "WAB-80: the attack order submits");
        if (use) {
            expect(static_cast<bool>(session.submit(command(0, 1, 2, 1,
                tactical::AbilityPayload{tactical::AbilityKind::full_salvo, tactical::AbilityAction::activate}))),
                "WAB-80: the activation submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t tick = 0; tick < ticks; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WAB-80: a step succeeds");
            if (!stepped) return result;
            result.hashes.push_back(stepped.value().state_sha256);
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                (event.weapon == 0 ? result.missile : result.laser).push_back(event.tick);
            }
        }
        if (const auto state = session.ability_state(1)) {
            result.ready_tick = state->slots[0].ready_tick;
            result.active_at_end = state->slots[0].active;
        }
        return result;
    };
    const auto plain = play(false, 1);
    const auto boosted = play(true, 1);
    expect(plain.missile.size() >= 6 && boosted.missile.size() >= 6, "WAB-80: the missile hardpoint fires volleys");
    // Pulses within a volley are 15 frames apart; with the multiplier round(15 x 0.18) = 3.
    const auto gaps = [](const std::vector<std::uint64_t>& frames, const std::uint64_t until) {
        std::vector<std::uint64_t> result;
        for (std::size_t index = 1; index < frames.size() && frames[index] <= until; ++index) {
            result.push_back(frames[index] - frames[index - 1]);
        }
        return result;
    };
    const auto plain_gaps = gaps(plain.missile, ticks);
    expect(!plain_gaps.empty() && *std::min_element(plain_gaps.begin(), plain_gaps.end()) == 15,
        "WAB-80: without FULL_SALVO the pulse gap is 15 frames");
    const auto boosted_gaps = gaps(boosted.missile, 200);
    expect(std::count(boosted_gaps.begin(), boosted_gaps.end(), 3) >= 5,
        "WAB-80: while FULL_SALVO is on the pulse gap is three frames");
    expect(!boosted_gaps.empty() && *std::min_element(boosted_gaps.begin(), boosted_gaps.end()) == 3,
        "WAB-80: no gap is shorter than the scaled pulse delay");
    // The recharge between volleys (4 to 5 s) is scaled too: round(120..150 x 0.18) = 22 to 27 frames.
    expect(std::any_of(boosted_gaps.begin(), boosted_gaps.end(), [](const std::uint64_t gap) {
        return gap >= 22 && gap <= 27;
    }), "WAB-80: the recharge between volleys shrinks to the scaled 4 to 5 s");
    const auto in_window = [](const std::vector<std::uint64_t>& frames, const std::uint64_t from, const std::uint64_t to) {
        return static_cast<std::size_t>(std::count_if(frames.begin(), frames.end(),
            [&](const std::uint64_t tick) { return tick >= from && tick < to; }));
    };
    expect(in_window(boosted.missile, 0, 211) >= 2 * in_window(plain.missile, 0, 211) + 6,
        "WAB-80: the seven seconds of FULL_SALVO fire far more missiles");
    // After the seven seconds the ordinary delays are back (the gap to the first shot after expiry is not
    // scaled below the pulse delay), and the recharge runs to 45 s from expiry.
    const auto late_gaps = gaps(boosted.missile, ticks);
    expect(late_gaps.size() > boosted_gaps.size()
        && *std::max_element(late_gaps.begin(), late_gaps.end()) >= 60,
        "WAB-80: after expiry the hardpoint is back on its ordinary recharge");
    expect(!boosted.active_at_end && boosted.ready_tick > ticks,
        "WAB-80: FULL_SALVO has expired and is recharging");
    // The hardpoint without a multiplier fires exactly as it does without FULL_SALVO.
    expect(plain.laser == boosted.laser && !plain.laser.empty(), "WAB-80: a hardpoint without a multiplier is unchanged");
    for (const std::size_t workers : {2U, 4U, 8U}) {
        const auto again = play(true, workers);
        expect(again.hashes == boosted.hashes && again.missile == boosted.missile,
            "WAB-80: FULL_SALVO hashes match with " + std::to_string(workers) + " workers");
    }
    // The replay round-trips and reproduces every tick.
    auto created = tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt,
        combat, {}, ability_data);
    if (!created) return;
    auto session = std::move(created).value();
    static_cast<void>(session.submit(command(0, 1, 1, 1, tactical::AttackPayload{3})));
    static_cast<void>(session.submit(command(0, 1, 2, 1,
        tactical::AbilityPayload{tactical::AbilityKind::full_salvo, tactical::AbilityAction::activate})));
    const eawr::platform::ThreadWorkerAdapter executor(2);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) static_cast<void>(session.step(executor));
    const auto replay = session.record();
    auto bytes = tactical::write_replay(replay);
    auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
    expect(parsed && parsed.value() == replay, "WAB-80: the FULL_SALVO command survives the replay format");
    if (parsed) {
        auto again = tactical::TacticalSession::from_replay(parsed.value(), sensors, durability(), motion(),
            std::nullopt, combat, {}, ability_data);
        expect(static_cast<bool>(again), "WAB-80: the replay session is created");
        if (again) {
            std::vector<std::string> hashes;
            while (again.value().completed_tick() < replay.final_tick_count) {
                hashes.push_back(again.value().step(executor).value().state_sha256);
            }
            expect(hashes == boosted.hashes, "WAB-80: the replay reproduces every tick hash");
        }
    }
    // A multiplier outside [0, 64] is refused at creation.
    auto bad = combat;
    bad.profiles[0].weapons[0].full_salvo_delay = units(65);
    expect(!tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt, bad, {}, ability_data),
        "WAB-80: a full-salvo multiplier beyond 64 is rejected");
    bad.profiles[0].weapons[0].full_salvo_delay = units(-1);
    expect(!tactical::TacticalSession::create(start, sensors, durability(), motion(), std::nullopt, bad, {}, ability_data),
        "WAB-80: a negative full-salvo multiplier is rejected");
}

// WST-01 to WST-11 (#1887): the Vengeance's STEALTH. One cloaked frigate (player 1) beside an
// enemy corvette that fires at anything it sees; a detector of the enemy's (Target_Stealth_Units)
// comes and goes. Scenario 0 runs the whole interplay, scenario 1 only the time limit.
[[nodiscard]] tactical::AbilityTable stealth_abilities(const std::uint32_t expiration) {
    tactical::AbilityProfile cloak;
    cloak.kind = tactical::AbilityKind::stealth;
    cloak.expiration_frames = expiration;
    cloak.recharge_frames = 90;
    cloak.gui_activated_ability_name = "Stealth_Test";
    tactical::SpecialAbilityProfile handler;
    handler.name = "Stealth_Test";
    handler.kind = tactical::SpecialAbilityKind::stealth;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = tactical::special_service_interval(tactical::SpecialAbilityKind::stealth);
    handler.stealth_transition = decimal("0.5");
    auto profile = unit_of(frigate_type, cloak);
    profile.special = {handler};
    tactical::AbilityTable table;
    table.profiles = {profile};
    return table;
}

// WST-03/10/11: a team attack must not reinstall member targets after cloak activation.
// Use the Phantom's 80-second duration with two independently armed members.
[[nodiscard]] eawr::core::Result<tactical::TacticalSession> squadron_stealth_fixture(const bool distant = false) {
    auto initial = setup();
    initial.units = {{2, plain_type, 2, at(distant ? 2000 : 300, 0), math::identity_quat(), {}},
        {10, xwing_squadron, 1, {}, math::identity_quat(), {}},
        {11, xwing_type, 1, {}, math::identity_quat(), {}},
        {12, xwing_type, 1, at(0, 10), math::identity_quat(), {}},
        {20, xwing_squadron, 1, at(-20000, 0), math::identity_quat(), {}},
        {21, xwing_type, 1, at(-20000, 0), math::identity_quat(), {}},
        {22, xwing_type, 1, at(-20000, 10), math::identity_quat(), {}}};
    initial.squadrons = {{10, {11, 12}}, {20, {21, 22}}};
    auto movement = hangar_motion();
    movement.squadrons.spawners.clear();
    movement.squadrons.craft.front().min_speed = {};
    movement.squadrons.craft.front().max_speed = units(1);
    tactical::DurabilityTable health;
    health.profiles = {{plain_type, units(1000000), {}, false, {}},
        {xwing_type, units(1000000), {}, false, {}}};
    tactical::WeaponProfile weapon;
    weapon.range = units(distant ? 500 : 10000);
    weapon.min_recharge_hundredths = 10; weapon.max_recharge_hundredths = 10;
    weapon.cone_width = units(360); weapon.cone_height = units(180);
    weapon.shot = tactical::ShotProfile{units(1), 0, units(10000), units(10000), true, true, {}};
    tactical::CombatProfile craft;
    craft.type_id = xwing_type; craft.max_attack_distance = weapon.range;
    craft.collision = tactical::CollisionBox{at(-5, -5, -5), at(5, 5, 5)};
    craft.weapons = {weapon};
    auto enemy = craft; enemy.type_id = plain_type;
    auto team = craft; team.type_id = xwing_squadron; team.weapons.clear();
    tactical::CombatTable combat;
    combat.profiles = {enemy, craft}; combat.unit_ai_types = {xwing_squadron};
    combat.retaliation_team_types = {team};
    auto abilities = stealth_abilities(2400);
    abilities.profiles.front().type_id = xwing_type;
    abilities.profiles.front().abilities.front().recharge_frames = 300;
    abilities.profiles.front().special.front().stealth_transition = decimal("0.8");
    const std::vector<tactical::SensorProfile> sensors{{plain_type, units(10000)}, {xwing_type, units(10000)}};
    return tactical::TacticalSession::create(initial, sensors, health, movement,
        std::nullopt, combat, {}, abilities);
}

void test_squadron_stealth() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = squadron_stealth_fixture();
        expect(static_cast<bool>(created), "WST-03: squadron cloak fixture creates");
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto world = std::move(created).value();
        const auto submit = [&](const tactical::PlayerCommand& item) {
            expect(static_cast<bool>(world.submit(item)), "WST-03: squadron command submits");
        };
        submit(command(0, 1, 0, 10, tactical::AttackPayload{2}));
        submit(command(0, 2, 0, 2, tactical::AttackPayload{10}));
        submit(command(40, 1, 1, 10, tactical::AbilityPayload{tactical::AbilityKind::stealth, tactical::AbilityAction::activate}));
        tactical::ObjectTurnUse reactivate;
        reactivate.unit = 10; reactivate.sequence = 0; reactivate.ability = tactical::AbilityKind::stealth;
        reactivate.action = tactical::AbilityAction::activate;
        submit(command(2700, 1, 2, 10, tactical::AttackPayload{2}));
        expect(static_cast<bool>(world.submit_object_turn({2740, reactivate})),
            "WST-03: named object-turn activation submits on the squadron container");
        // Explicit attacks remain usable while cloaked and their first shot ends the cloak (WST-05/10).
        submit(command(2750, 1, 3, 10, tactical::AttackPayload{2}));
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool fired_before = false, own_fired_before = false, explicit_shot = false;
        for (std::uint64_t frame = 0; frame <= 2760; ++frame) {
            const auto step = world.step(executor);
            expect(static_cast<bool>(step), "WST-03: squadron cloak step succeeds");
            if (!step) { std::cerr << step.error().message << '\n'; break; }
            hashes.push_back(step.value().state_sha256);
            if (frame == 39) {
                const auto mind = world.squadron_state(10);
                expect(mind && mind->target == 2,
                    "WST-03: fixture holds an explicit squadron attack before cloaking");
            }
            const bool hidden = (frame >= 40 && frame < 2440) || (frame >= 2740 && frame < 2750);
            for (const auto id : {11U, 12U}) {
                const auto state = world.ability_state(id);
                expect(state.has_value(), "WST-03: every live member retains its ability state");
                if (!state) continue;
                if (hidden) expect(state->stealth_on && state->slots[0].active,
                    "WST-03/10: all squadron members stay cloaked until expiry or a new explicit attack");
                if (hidden) {
                    const auto target = world.combat_state(id);
                    expect(target && target->attack_target == eawr::sim::invalid_entity_id,
                        "WST-03/10: a retained team attack cannot reinstall a cloaked member's target");
                }
                if (frame == 2440) expect(!state->stealth_on && !state->slots[0].active && state->slots[0].ready_tick == 2740,
                    "WST-04: 80-second cloak expires and recharges independently on every member");
            }
            if (hidden) {
                const auto mind = world.squadron_state(10);
                expect(mind && mind->target == eawr::sim::invalid_entity_id,
                    "WST-03: cloak activation releases the team's shared target");
                const auto visible = step.value().snapshot->visible_entities(2);
                expect(std::find(visible.begin(), visible.end(), 11) == visible.end()
                    && std::find(visible.begin(), visible.end(), 12) == visible.end()
                    && std::find(visible.begin(), visible.end(), 10) == visible.end(),
                    "WST-11/13: enemies without detection cannot see cloaked members or their container");
                if (frame != 40 && frame != 2740) {
                    const auto target = world.combat_state(2);
                    expect(target && target->attack_target == eawr::sim::invalid_entity_id,
                        "WST-11: the enemy releases the hidden squadron container as a held target");
                }
            }
            for (const auto& event : step.value().snapshot->combat_events()) {
                if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
                fired_before = fired_before || frame < 40;
                own_fired_before = own_fired_before || (frame < 40 && (event.shooter == 11 || event.shooter == 12));
                // Commands and object turns follow the combat service of their activation tick.
                if (hidden && frame != 40 && frame != 2740 && (event.shooter == 2 || event.shooter == 11 || event.shooter == 12)) expect(false,
                    "WST-10/11: neither the cloaked squadron nor its undetecting enemy fires");
                if (frame >= 2750 && (event.shooter == 11 || event.shooter == 12)) {
                    if (!explicit_shot) {
                        const auto state = world.ability_state(event.shooter);
                        expect(state && !state->stealth_on,
                            "WST-05: the first shot after a fresh explicit attack ends that member's cloak");
                    }
                    explicit_shot = true;
                }
            }
        }
        expect(fired_before && own_fired_before && explicit_shot,
            "WST-05/10: fixture fires before cloak and after a fresh explicit attack");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WST-03: squadron cloak hashes equal on 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(world.record());
        const auto parsed = encoded ? tactical::parse_replay(encoded.value())
                                    : eawr::core::Result<tactical::TacticalReplay>::failure(encoded.error());
        expect(parsed && parsed.value() == world.record(), "WST-03: squadron cloak replay round-trips");
    }
}

void test_squadron_stealth_orders() {
    // Move, friendly Guard, each attack-endpoint path, mixed visibility, and each member-only seam.
    for (unsigned scenario = 0; scenario < 7; ++scenario) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const bool endpoint = scenario == 2 || scenario == 3;
            auto created = squadron_stealth_fixture(endpoint);
            expect(static_cast<bool>(created), "WST-03: cloak order fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            const auto submit = [&](const tactical::PlayerCommand& item) {
                expect(static_cast<bool>(world.submit(item)), "WST-03: cloak order control submits");
            };
            const auto step = [&] {
                const auto result = world.step(executor);
                expect(static_cast<bool>(result), "WST-03: cloak order control steps");
                if (!result) { std::cerr << result.error().message << '\n'; return false; }
                hashes.push_back(result.value().state_sha256);
                return true;
            };
            const bool guard = scenario == 1 || scenario >= 5;
            if (guard) {
                tactical::GuardPayload order; order.target = 20;
                submit(command(0, 1, 0, 10, order));
            } else if (scenario == 0) submit(command(0, 1, 0, 10, tactical::MovePayload{at(500, 500)}));
            else submit(command(0, 1, 0, 10, tactical::AttackPayload{2}));
            if (scenario == 4) submit(command(0, 2, 0, 2, tactical::AttackPayload{10}));

            std::uint64_t activation = 40;
            math::Vec3 expected_endpoint{};
            bool path_found = false;
            while (world.completed_tick() < (endpoint ? 180U : 40U)) {
                if (!step()) break;
                if (!endpoint) continue;
                const auto team = world.squadron_state(10);
                if (team && team->attack_path && !team->attack_path->stack.empty()
                    && team->attack_path->stack.back().nodes.size() >= 2) {
                    expected_endpoint = team->attack_path->stack.back().nodes.back();
                    path_found = true;
                    activation = world.completed_tick();
                    break;
                }
            }
            if (endpoint) {
                expect(path_found, "WST-03: distant attack has an accepted path before cloaking");
                if (!path_found) continue;
            }
            const eawr::sim::EntityId requested = scenario >= 5 ? 11 : 10;
            if (scenario == 3 || scenario == 6) {
                tactical::ObjectTurnUse use;
                use.unit = requested; use.sequence = 0; use.ability = tactical::AbilityKind::stealth;
                use.action = tactical::AbilityAction::activate;
                expect(static_cast<bool>(world.submit_object_turn({activation, use})), "WST-03: object-turn cloak submits");
            } else submit(command(activation, 1, 1, requested,
                tactical::AbilityPayload{tactical::AbilityKind::stealth, tactical::AbilityAction::activate}));
            if (!step()) continue;
            auto team = world.squadron_state(10);
            expect(team.has_value(), "WST-03: cloaked team remains live");
            if (!team) continue;
            const auto rows = world.units();
            const auto container = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.entity_id == 10; });
            expect(container != rows.end(), "WST-03: team order remains published");
            if (guard) {
                expect(team->mode == tactical::SquadronMode::escort && team->escorted == 20
                    && team->formation && team->formation->base_target == 20,
                    "WST-03: a friendly Guard keeps its escort and formation base through cloak activation");
                expect(container != rows.end() && container->order.kind == tactical::OrderKind::guard && container->order.target == 20,
                    "WST-03: friendly Guard retains its player order");
            } else if (scenario == 0) {
                expect(team->mode == tactical::SquadronMode::move && team->anchor == at(500, 500),
                    "WST-03: an ordinary move retains its destination through cloak activation");
                const auto before = container != rows.end() ? container->position : math::Vec3{};
                for (unsigned count = 0; count < 5; ++count) if (!step()) break;
                const auto moved = world.units();
                const auto after = std::find_if(moved.begin(), moved.end(), [](const auto& row) { return row.entity_id == 10; });
                expect(after != moved.end() && after->position != before, "WST-03: the cloaked squadron keeps moving");
            } else if (endpoint) {
                expect(team->target == eawr::sim::invalid_entity_id && team->mode == tactical::SquadronMode::move
                    && team->anchor == expected_endpoint, "WST-03: cloak replaces the attack with its accepted path endpoint");
            }
            if (scenario >= 5) {
                const auto first = world.ability_state(11), second = world.ability_state(12);
                expect(first && first->stealth_on && second && !second->stealth_on,
                    "WST-03: a member-addressed request switches only that member and keeps the team order");
            }
            if (scenario == 4) {
                submit(command(55, 1, 2, 10, tactical::MovePayload{at(1000, 0)}));
                submit(command(60, 1, 3, 11,
                    tactical::AbilityPayload{tactical::AbilityKind::stealth, tactical::AbilityAction::deactivate}));
                submit(command(61, 2, 1, 2, tactical::AttackPayload{10}));
                while (world.completed_tick() <= 62) {
                    const auto tick = world.completed_tick();
                    if (!step()) break;
                    if (tick == 60) {
                        const auto first = world.ability_state(11), second = world.ability_state(12);
                        expect(first && !first->stealth_on && second && second->stealth_on,
                            "WST-11: mixed visibility fixture has exactly one cloaked member");
                        const auto snapshot = world.snapshot();
                        const auto instances = snapshot->instances();
                        const auto published = std::find_if(instances.begin(), instances.end(), [](const auto& row) { return row.entity_id == 10; });
                        expect(published != instances.end() && !published->stealth_on && !published->stealth_invisible,
                            "WST-11/13: a mixed live roster keeps the container visible");
                        const auto visible = snapshot->visible_entities(2);
                        expect(std::find(visible.begin(), visible.end(), 10) != visible.end()
                            && std::find(visible.begin(), visible.end(), 11) != visible.end()
                            && std::find(visible.begin(), visible.end(), 12) == visible.end(),
                            "WST-13: a mixed team exposes the container and visible member only");
                    }
                    if (tick == 62) {
                        const auto enemy = world.combat_state(2);
                        expect(enemy && enemy->attack_target == 10, "WST-11: combat retains a visible mixed squadron as a target");
                    }
                }
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WST-03/11/13: cloak order controls equal on 1/2/4/8 workers");
        }
    }
}

void test_stealth_session() {
    tactical::UnitAbilityProfile mod_profile;
    mod_profile.stealth_detection_frames = 67;
    tactical::AbilityState mod_state;
    mod_state.stealth_on = true;
    mod_state.stealth_detected = 100;
    expect(!tactical::stealth_invisible(mod_state, 167, &mod_profile)
        && tactical::stealth_invisible(mod_state, 168, &mod_profile),
        "WST-09: non-default reveal hold includes its final frame");
    mod_profile.stealth_detection_frames = 0;
    expect(!tactical::stealth_invisible(mod_state, 100, &mod_profile)
        && tactical::stealth_invisible(mod_state, 101, &mod_profile),
        "WST-09: zero hold reveals only the detection frame");

    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("Stealth") == Kind::stealth && tactical::to_string(Kind::stealth) == "STEALTH",
        "WST-01: STEALTH binds by name to kind 20");
    expect(static_cast<unsigned>(Kind::stealth) == 20U, "WST-01: coordinator-reserved kind number");
    expect(static_cast<bool>(tactical::validate_abilities(stealth_abilities(100))), "WST-02: a cloak with its handler is valid");
    auto orphan = stealth_abilities(100);
    orphan.profiles[0].special.clear();
    expect(!tactical::validate_abilities(orphan), "WST-02: a cloak without its nested handler is refused");
    expect(!tactical::validate_abilities(stealth_abilities(0)), "WST-05: a cloak without a time limit is refused");
    tactical::AbilityState probe;
    probe.stealth_on = true;
    expect(!tactical::stealth_invisible(probe, 30) && tactical::stealth_invisible(probe, 31),
        "WST-09: invisible once more than 30 frames passed since the last detection");

    for (const unsigned scenario : {0U, 1U}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_hull = units(1000000); row.max_shields = {}; row.shield_refresh = {}; }
            tactical::WeaponProfile weapon;
            weapon.range = units(700); weapon.pulse_count = 1;
            weapon.cone_width = units(175); weapon.cone_height = units(160);
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.shot = tactical::ShotProfile{units(1), 0, units(25), units(700), true, true, {}};
            tactical::CombatProfile cloaked;
            cloaked.type_id = frigate_type; cloaked.max_attack_distance = units(700);
            cloaked.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
            cloaked.weapons = {weapon};
            tactical::CombatProfile gunner = cloaked;
            gunner.type_id = corvette_type;
            tactical::CombatProfile detector;
            detector.type_id = plain_type;
            detector.target_stealth = true;
            detector.stealth_detection_range = units(500);
            tactical::CombatTable combat; combat.profiles = {gunner, cloaked, detector};
            auto initial = setup();
            initial.units = {{1, frigate_type, 1, at(0, 0), math::identity_quat(), {}},
                // Turned 180 degrees about Z, so each gun faces the other (weapon cones point forward).
                {2, corvette_type, 2, at(300, 0), math::Quat{Fixed{}, Fixed{}, units(1), Fixed{}}, {}}};
            const std::vector<tactical::SensorProfile> sensors{
                {corvette_type, units(1000)}, {frigate_type, units(1000)}, {plain_type, units(1000)}};
            const auto abilities = stealth_abilities(scenario == 0 ? 400 : 100);
            auto created = tactical::TacticalSession::create(initial, sensors, health, {}, std::nullopt, combat, {}, abilities);
            expect(static_cast<bool>(created), "WST: stealth fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const auto submit = [&](const tactical::PlayerCommand& item) {
                expect(static_cast<bool>(world.submit(item)), "WST: fixture command submits");
            };
            // The fixture's guns fire on orders: the enemy attacks the frigate before the cloak, and
            // again whenever the cloak should no longer hide it.
            submit(command(0, 2, 0, 2, tactical::AttackPayload{1}));
            submit(command(40, 1, 0, 1, tactical::AbilityPayload{Kind::stealth, tactical::AbilityAction::activate}));
            if (scenario == 1) submit(command(145, 2, 0, 2, tactical::AttackPayload{1}));
            if (scenario == 0) {
                submit(command(50, 2, 0, 2, tactical::AttackPayload{1}));
                submit(command(105, 2, 0, 2, tactical::AttackPayload{1}));
                submit(command(140, 2, 0, 2, tactical::StopPayload{}));
                submit(command(210, 2, 0, 2, tactical::AttackPayload{1}));
                submit(command(200, 1, 1, 1, tactical::AttackPayload{2}));
                submit(command(250, 1, 2, 1, tactical::AbilityPayload{Kind::stealth, tactical::AbilityAction::deactivate}));
            }
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            std::vector<std::uint64_t> enemy_shots, own_shots;
            bool hidden_rejected = false;
            for (std::uint64_t frame = 0; frame < 280; ++frame) {
                if (scenario == 0 && frame == 100) expect(static_cast<bool>(world.stage_spawn(
                    {3, plain_type, 2, at(0, 400), math::identity_quat(), {}})), "WST-08: the detector arrives");
                if (scenario == 0 && frame == 130) expect(static_cast<bool>(world.stage_remove(3)), "WST-08: the detector leaves");
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WST: stealth step succeeds");
                if (!step) break;
                hashes.push_back(step.value().state_sha256);
                for (const auto& event : step.value().snapshot->combat_events()) {
                    if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
                    if (event.shooter == 2) enemy_shots.push_back(frame);
                    if (event.shooter == 1) own_shots.push_back(frame);
                }
                for (const auto& event : step.value().snapshot->events())
                    hidden_rejected = hidden_rejected || (event.kind == tactical::EventKind::order_rejected
                        && event.reason == tactical::RejectReason::target_hidden && event.unit == 2);
                const auto state = world.ability_state(1);
                const auto instances = step.value().snapshot->instances();
                const auto self = std::find_if(instances.begin(), instances.end(), [](const auto& item) { return item.entity_id == 1; });
                if (!state || self == instances.end()) { expect(false, "WST: the cloaked unit lives"); break; }
                const auto bytes = world.canonical_state_bytes();
                const std::vector<std::uint8_t> tag{'S', 'T', 'L', 'H'};
                const bool block = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
                expect(block == state->stealth_on, "WST-03: the STLH block is written exactly while the cloak is on");
                expect(self->stealth_on == state->stealth_on, "WST-12: the snapshot publishes the cloak");
                if (frame == 40) {
                    expect(state->stealth_on && state->slots[0].active && state->stealth_detected == 0,
                        "WST-03: switching on cloaks undetected");
                    const auto combat_state = world.combat_state(1);
                    expect(combat_state && combat_state->attack_target == 0,
                        "WST-03: switching on drops the attack target");
                }
                if (frame == 60 || (scenario == 0 && frame == 180)) {
                    expect(self->stealth_invisible, "WST-09: hidden while no detector is near");
                    const auto seen = step.value().snapshot->visible_entities(2);
                    const auto own = step.value().snapshot->visible_entities(1);
                    expect(!std::binary_search(seen.begin(), seen.end(), 1) && std::binary_search(own.begin(), own.end(), 1),
                        "WST-13: the enemy no longer sees the cloaked unit; its owner does");
                }
                if (scenario == 0 && frame == 110) expect(state->stealth_on && !self->stealth_invisible && state->stealth_detected == frame,
                    "WST-08: a detector in range reveals the cloaked unit every frame");
                // The detector's last frame is 129; the unit stays seen while 129 + 30 is not before the frame.
                if (scenario == 0 && frame == 158) expect(!self->stealth_invisible, "WST-09: revealed for 30 frames after the detector left");
                if (scenario == 0 && frame == 159) expect(self->stealth_invisible, "WST-09: hidden once the detection lapsed");
                if (scenario == 0 && frame == 230) expect(!state->stealth_on && state->slots[0].active,
                    "WST-05: firing ends the cloak while the STEALTH countdown runs on");
                if (scenario == 0 && frame == 250) expect(!state->slots[0].active && state->slots[0].ready_tick > 250,
                    "WAB-05: a switch-off ends the slot with a share of the recharge");
                if (scenario == 1 && frame == 140) expect(!state->stealth_on && !state->slots[0].active
                    && state->slots[0].ready_tick == 140 + 90, "WST-05: the cloak ends with its time limit and recharges");
            }
            const auto shots_between = [](const std::vector<std::uint64_t>& shots, std::uint64_t from, std::uint64_t to) {
                return std::count_if(shots.begin(), shots.end(), [&](const auto frame) { return frame >= from && frame < to; });
            };
            expect(shots_between(enemy_shots, 0, 40) > 0, "WST: the enemy fires at the frigate before the cloak");
            expect(shots_between(enemy_shots, 41, 100) == 0, "WST-11: nothing fires at an uncountered cloak");
            expect(shots_between(own_shots, 41, 100) == 0, "WST-10: the cloaked unit finds no targets of its own");
            if (scenario == 0) {
                expect(hidden_rejected, "WST-11: an attack order on a hidden enemy is refused");
                expect(shots_between(enemy_shots, 100, 135) > 0, "WST-08/09: the detector's reveal lets the others fire");
                expect(shots_between(enemy_shots, 165, 200) == 0, "WST-09: hidden again once the detection lapsed");
                expect(shots_between(own_shots, 200, 230) > 0 && shots_between(enemy_shots, 205, 250) > 0,
                    "WST-05: its own shot breaks the cloak and the enemy fires again");
            } else {
                expect(shots_between(enemy_shots, 41, 140) == 0 && shots_between(enemy_shots, 141, 200) > 0,
                    "WST-05: the time limit ends the cloak");
            }
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "WST: stealth hashes agree at 1/2/4/8 workers");
            const auto encoded = tactical::write_replay(world.record());
            const auto parsed = encoded ? tactical::parse_replay(encoded.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
            expect(parsed && parsed.value() == world.record(), "WST: STEALTH commands round trip through the ability opcode");
        }
    }
}

// WST-03/04 (#1906): a Stealth_Duration_Multiplier upgrade lengthens the cloak's own count,
// trunc(Expiration_Seconds x m x 30) with m sampled at the switch-on, while the STEALTH slot keeps
// its plain countdown. Scenario 0 holds no upgrade; in scenario 1 the owner holds three: +0.25 and
// +0.5 in category 3 (the larger counts) and +0.1 in category 1 (added), so m = 1.6. In scenario 2
// an ally (player 3) holds an owner-only +0.5 and the enemy an all-allies +0.5: neither reaches the
// owner. In scenario 3 the ally holds the all-allies +0.5, so m = 1.5.
void test_stealth_duration_modifiers() {
    using Kind = tactical::AbilityKind;
    const auto seconds = [](const std::int64_t whole) { return Fixed::from_raw(whole * Fixed::scale); };
    expect(tactical::stealth_frames(seconds(60), decimal("1.25")) == 2250 && tactical::stealth_frames(seconds(60), decimal("1.5")) == 2700
        && tactical::stealth_frames(seconds(80), decimal("1.25")) == 3000 && tactical::stealth_frames(seconds(180), decimal("1.5")) == 8100,
        "WST-03: the stock cloaks' lengthened counts (Vengeance, TIE Phantom, Peacebringer)");
    expect(tactical::stealth_frames(seconds(3), decimal("1.25")) == 112, "WST-03: the count truncates (112.5 frames)");
    // 0.7 is just below 0.7 in Q24, so the exact product sits just below 210 and truncates to 209.
    expect(decimal("0.7").raw() * 300 < 210 * Fixed::scale && tactical::stealth_frames(seconds(10), decimal("0.7")) == 209,
        "WST-03: the exact Q24 product truncates on the edge, no host float");
    expect(tactical::stealth_frames(seconds(3), Fixed{}) == 90 && tactical::stealth_frames(seconds(3), decimal("-0.5")) == 90,
        "WST-03: a factor of 0 or less counts as 1");
    for (const unsigned scenario : {0U, 1U, 2U, 3U}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto abilities = stealth_abilities(90);
            abilities.profiles[0].abilities[0].expiration_seconds = seconds(3);
            constexpr tactical::TypeId station = 98;
            auto initial = setup();
            initial.units = {{1, frigate_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, station, 1, at(-2000, 0), math::identity_quat(), {}},
                {3, station, 2, at(2000, 0), math::identity_quat(), {}},
                {4, station, 3, at(-2000, 1000), math::identity_quat(), {}}};
            initial.players.push_back({3, 1, 3, tactical::player_flag_commandable});
            tactical::EconomyRules economy;
            economy.players = {{1, units(100), 25, false, {}, 1, 3}, {2, units(100), 25, false, {}, 1, 3},
                {3, units(100), 25, false, {}, 1, 3}};
            std::vector<tactical::BuildOption> options;
            for (const tactical::TypeId type : {90U, 91U, 92U, 93U}) {
                tactical::BuildOption option;
                option.type = type; option.kind = tactical::BuildKind::upgrade; option.queue = tactical::BuildQueue::upgrades;
                option.price = units(10); option.build_frames = 2; option.ai_build_frames = 2; option.available = true;
                options.push_back(option);
            }
            economy.menus = {{station, 1, options}, {station, 2, options}, {station, 3, options}};
            const auto upgrade = [](const tactical::TypeId type, const std::uint32_t category, const char* percentage, const bool allies) {
                tactical::UpgradeProfile profile;
                profile.type = type;
                profile.stealth_duration = {{category, decimal(percentage), allies}};
                return profile;
            };
            economy.upgrades = {upgrade(90, 3, "0.25", true), upgrade(91, 3, "0.5", true), upgrade(92, 1, "0.1", true),
                upgrade(93, 3, "0.5", false)};
            auto created = tactical::TacticalSession::create(initial, {}, durability(), {}, std::nullopt, {}, {}, abilities, economy);
            expect(static_cast<bool>(created), "WST-03: duration fixture creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            const auto submit = [&](const tactical::PlayerCommand& item) {
                expect(static_cast<bool>(world.submit(item)), "WST-03: duration fixture command submits");
            };
            if (scenario == 1) {
                submit(command(0, 1, 0, 2, tactical::BuyPayload{90}));
                submit(command(0, 1, 1, 2, tactical::BuyPayload{91}));
                submit(command(0, 1, 2, 2, tactical::BuyPayload{92}));
            }
            if (scenario == 2) {
                submit(command(0, 3, 0, 4, tactical::BuyPayload{93}));
                submit(command(0, 2, 0, 3, tactical::BuyPayload{91}));
            }
            if (scenario == 3) submit(command(0, 3, 0, 4, tactical::BuyPayload{91}));
            submit(command(20, 1, 3, 1, tactical::AbilityPayload{Kind::stealth, tactical::AbilityAction::activate}));
            // m = 1.6: trunc(3 x 1.6 x 30) = 144 frames from the switch-on, m = 1.5: 135; the slot's 90 run out first.
            const bool lengthened = scenario == 1 || scenario == 3;
            const std::uint64_t end = scenario == 1 ? 20 + 144 : (scenario == 3 ? 20 + 135 : 20 + 90);
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (std::uint64_t frame = 0; frame < 200; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WST-03: duration step succeeds");
                if (!step) { std::cerr << step.error().message << '\n'; break; }
                hashes.push_back(step.value().state_sha256);
                const auto state = world.ability_state(1);
                if (!state) { expect(false, "WST-03: the cloaked unit lives"); break; }
                const auto bytes = world.canonical_state_bytes();
                const std::vector<std::uint8_t> tag{'S', 'T', 'L', 'X'};
                const bool block = std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
                expect(block == (lengthened && state->stealth_on), "WST-03: the STLX block is written exactly while a lengthened cloak is on");
                if (frame == 20) expect(state->stealth_on && state->stealth_end == (lengthened ? end : 0),
                    "WST-03: the switch-on samples the owner's modifiers");
                if (frame == 20 + 90) expect(!state->slots[0].active && state->slots[0].ready_tick == 20 + 90 + 90,
                    "WST-04: the slot expires on its plain countdown and recharges");
                if (frame + 1 == end) expect(state->stealth_on, "WST-04: the cloak is on until its own count ends");
                if (frame == end) expect(!state->stealth_on && state->stealth_end == 0, "WST-04: the cloak ends with its own count");
                if (lengthened && frame == 130) expect(state->stealth_on && !state->slots[0].active,
                    "WST-04: the slot's expiry leaves a lengthened cloak on");
            }
            if (reference.empty()) reference = hashes;
            else expect(reference == hashes, "WST-03: duration hashes agree at 1/2/4/8 workers");
        }
    }
}

// --- LASER_DEFENSE (WLD-01 to WLD-12, the Crusader's point defence) --------------------------------

constexpr tactical::TypeId missile_shooter_type = 14;
constexpr tactical::TypeId crusader_type = 15;

// The Crusader's authored data (units_space_underworld_crusader_gunship.xml): LASER_DEFENSE with a 60 s
// recharge, and the nested handler's 35 s run, 500 reach, 100 minimum flight, 0.5 s between looks.
[[nodiscard]] tactical::AbilityProfile laser_defence_ability() {
    tactical::AbilityProfile profile;
    profile.kind = tactical::AbilityKind::laser_defense;
    profile.expiration_frames = 1050;
    profile.recharge_frames = 1800;
    profile.recharge_at_activation = true;
    profile.gui_activated_ability_name = "Crusader_Laser_Defense";
    return profile;
}

[[nodiscard]] tactical::SpecialAbilityProfile laser_defence_handler() {
    tactical::SpecialAbilityProfile handler;
    handler.name = "Crusader_Laser_Defense";
    handler.kind = tactical::SpecialAbilityKind::laser_defense;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = 1;
    handler.defence_duration_frames = 1050;
    handler.defence_look_frames = 15;
    handler.defence_beam_frames = 2;
    handler.defence_radius = units(500);
    handler.defence_min_travel = units(100);
    handler.defence_categories = tactical::defence_category_missile | tactical::defence_category_rocket
        | tactical::defence_category_mptl_rocket;
    return handler;
}

[[nodiscard]] tactical::AbilityTable laser_defence_abilities() {
    tactical::AbilityTable table;
    table.profiles = {{crusader_type, {laser_defence_ability()}, false, {laser_defence_handler()}}};
    return table;
}

// A shooter at the origin with one concussion-missile-like weapon: a missile a second at 7 units a frame.
[[nodiscard]] tactical::CombatTable laser_defence_combat(const bool homing) {
    tactical::CombatTable table;
    const tactical::CollisionBox box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::WeaponProfile weapon;
    weapon.hardpoint = 0;
    weapon.range = units(1400);
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    weapon.cone_width = units(175);
    weapon.cone_height = units(160);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 0, units(7), units(1400), true, true, {}};
    weapon.shot->homing = homing;
    weapon.shot->turn_rate = homing ? units(3) : Fixed{};
    tactical::CombatProfile shooter;
    shooter.type_id = missile_shooter_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(1400);
    shooter.weapons = {weapon};
    shooter.collision = box;
    tactical::CombatProfile crusader;
    crusader.type_id = crusader_type;
    crusader.category_bits = 2;
    crusader.collision = box;
    table.profiles = {shooter, crusader};
    return table;
}

[[nodiscard]] tactical::DurabilityTable laser_defence_durability() {
    auto table = durability();
    auto profile = table.profiles.front();
    for (const auto type : {missile_shooter_type, crusader_type}) {
        profile.type_id = type;
        table.profiles.push_back(profile);
    }
    std::sort(table.profiles.begin(), table.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> laser_defence_sensors() {
    return {{missile_shooter_type, units(3000)}, {crusader_type, units(3000)}};
}

// Unit 1 (player 1) fires missiles at unit 2 (player 2, a Crusader `distance` away); unit 3 is a second
// Crusader of the shooter's owner, close to the missile path.
[[nodiscard]] tactical::TacticalSetup laser_defence_setup(const std::int64_t distance) {
    tactical::TacticalSetup result;
    result.seed = 1876;
    result.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    result.units = {
        {1, missile_shooter_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, crusader_type, 2, at(distance, 0), math::identity_quat(), {}},
        {3, crusader_type, 1, at(distance / 2, 90), math::identity_quat(), {}},
    };
    return result;
}

struct LaserRun {
    std::vector<std::string> hashes;
    std::vector<tactical::CombatEvent> zaps, expired, hits;
    std::vector<tactical::Event> events;
    std::vector<tactical::AbilityStatus> status; // unit 2's laser status after each tick
    std::optional<tactical::TacticalSession> session;
    std::optional<tactical::TacticalReplay> replay;
};

[[nodiscard]] LaserRun laser_run(const std::vector<tactical::PlayerCommand>& commands, const std::int64_t distance,
    const std::size_t workers, const std::uint64_t ticks, const bool homing = true) {
    LaserRun result;
    auto created = tactical::TacticalSession::create(laser_defence_setup(distance), laser_defence_sensors(),
        laser_defence_durability(), motion(), std::nullopt, laser_defence_combat(homing), {}, laser_defence_abilities());
    expect(static_cast<bool>(created), "the laser defence session is created");
    if (!created) return result;
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "a laser defence command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a laser defence step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind == tactical::CombatEventKind::point_defence_zap) result.zaps.push_back(event);
            if (event.kind == tactical::CombatEventKind::projectile_expired) result.expired.push_back(event);
            if (event.kind == tactical::CombatEventKind::projectile_hit && event.target == 2) result.hits.push_back(event);
        }
        for (const auto& instance : stepped.value().snapshot->instances()) {
            if (instance.entity_id != 2) continue;
            for (const auto& status : instance.abilities) {
                if (status.kind == tactical::AbilityKind::laser_defense) result.status.push_back(status);
            }
        }
    }
    result.replay = session.record();
    result.session.emplace(std::move(session));
    return result;
}

[[nodiscard]] tactical::AbilityPayload laser(const tactical::AbilityAction action) {
    return {tactical::AbilityKind::laser_defense, action};
}

void test_laser_defence() {
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("LASER_DEFENSE") == Kind::laser_defense && static_cast<int>(Kind::laser_defense) == 18
        && tactical::to_string(Kind::laser_defense) == "LASER_DEFENSE", "WLD-01: LASER_DEFENSE is AbilityKind 18");
    expect(tactical::special_ability_kind("Laser_Defense_Ability") == tactical::SpecialAbilityKind::laser_defense
        && tactical::special_service_interval(tactical::SpecialAbilityKind::laser_defense) == 1,
        "WLD-03: the nested handler binds and services every frame");

    // WLD-04: the categories a shot in flight belongs to.
    tactical::Projectile shot;
    expect(tactical::projectile_defence_category(shot) == 0, "WLD-04: a laser bolt is not a targeted category");
    shot.homing = true;
    expect(tactical::projectile_defence_category(shot) == tactical::defence_category_missile, "WLD-04: a homing shot is a MISSILE");
    shot.homing = false;
    shot.flight = tactical::FlightState{};
    shot.flight->profile.kind = tactical::FlightKind::rocket;
    expect(tactical::projectile_defence_category(shot) == tactical::defence_category_rocket, "WLD-04: a rocket-path shot is a ROCKET");

    // WLD-02/WLD-06 table rules: the handler is required, the run is the handler's, the recharge starts at switch-on.
    {
        auto table = laser_defence_abilities();
        expect(static_cast<bool>(tactical::validate_abilities(table)), "WLD-02: the Crusader's table is valid");
        auto without = table;
        without.profiles[0].special.clear();
        expect(!tactical::validate_abilities(without), "WLD-02: LASER_DEFENSE without its nested handler is refused");
        auto mismatched = table;
        mismatched.profiles[0].abilities[0].expiration_frames = 900;
        expect(!tactical::validate_abilities(mismatched), "WLD-02: the time limit is the handler's duration");
        auto reach = table;
        reach.profiles[0].special[0].defence_radius = {};
        expect(!tactical::validate_abilities(reach), "WLD-04: a handler with no reach is refused");
        auto others = table;
        others.profiles[0].abilities[0].kind = Kind::turbo;
        expect(!tactical::validate_abilities(others), "WLD-06: only LASER_DEFENSE recharges from its switch-on");
        const auto ability = laser_defence_ability();
        tactical::AbilitySlot slot;
        expect(tactical::activate_ability(ability, slot, open_gate, 40).changed && slot.expires_tick == 1090
            && slot.ready_tick == 1840, "WLD-06: the 60 s countdown starts when the ability switches on");
        expect(tactical::deactivate_ability(ability, slot, 100).changed && !slot.active && slot.ready_tick == 1840,
            "WLD-06: switching off early changes nothing about the countdown");
        expect(!tactical::activate_ability(ability, slot, open_gate, 200).changed, "WLD-06: it cannot switch on again while counting down");
        tactical::AbilityState state;
        state.slots.resize(1);
        const auto profile = laser_defence_abilities().profiles[0];
        static_cast<void>(tactical::activate_ability(ability, state.slots[0], open_gate, 10));
        static_cast<void>(tactical::expire_abilities(profile, state, 1060));
        expect(!state.slots[0].active && state.slots[0].ready_tick == 1810,
            "WLD-06: running out after 35 s leaves the countdown where the switch-on put it");
    }

    const std::vector<tactical::PlayerCommand> none;
    const std::vector<tactical::PlayerCommand> armed{command(0, 2, 0, 2, laser(tactical::AbilityAction::activate)),
        command(0, 1, 1, 3, laser(tactical::AbilityAction::activate))};
    const auto control = laser_run(none, 800, 1, 400);
    expect(!control.hits.empty() && control.zaps.empty(), "WLD-05 control: without the ability the missiles hit the Crusader");

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto active = laser_run(armed, 800, workers, 1900);
        if (!active.session) continue;
        if (workers == 1) reference = active.hashes;
        else expect(active.hashes == reference, "WLD-12: laser defence hashes equal at 1/2/4/8 workers");
        const auto started = active.status.empty() ? std::uint64_t{0} : active.status.front().started_tick;
        expect(!active.zaps.empty(), "WLD-05: the active Crusader shoots missiles down");
        bool inside = true, spaced = true, own_side_spared = true, expired_in_step = true;
        for (const auto& zap : active.zaps) {
            const math::Vec3 crusader = at(800, 0);
            inside = inside && tactical::within_range(crusader, zap.aim, units(500), tactical::RangeMetric::spatial)
                && zap.origin == crusader && zap.shooter == 2;
            // WLD-03: the first look is the service after the switch-on, then one every 16 services.
            spaced = spaced && zap.tick > started && (zap.tick - started - 1) % 16 == 0 && zap.tick <= started + 1050;
            own_side_spared = own_side_spared && zap.shooter != 3;
            const auto gone = std::find_if(active.expired.begin(), active.expired.end(), [&](const auto& event) {
                return event.target == zap.target && event.tick == zap.tick
                    && event.outcome == static_cast<std::uint32_t>(tactical::ProjectileExpiryReason::explicit_request);
            });
            expired_in_step = expired_in_step && gone != active.expired.end();
        }
        expect(inside, "WLD-04: a zapped shot was inside the 500 reach, from the Crusader");
        expect(spaced, "WLD-03: looks come on the 0.5 s counter after the switch-on, within the 35 s run");
        expect(own_side_spared, "WLD-04: another Crusader of the shooter's owner never shoots its own side's missile");
        expect(expired_in_step, "WLD-05: a zapped shot detonates where it is in the same frame");
        for (const auto& hit : active.hits) {
            expect(hit.tick > started + 1050 || hit.tick <= started, "WLD-05: no missile reaches the Crusader during its 35 s run");
        }
        expect(std::any_of(active.hits.begin(), active.hits.end(), [&](const auto& hit) { return hit.tick > started + 1050; }),
            "WLD-02: missiles reach the Crusader again once the run ends");
        // WLD-06 through the snapshot: the dial counts down from the switch-on.
        const auto& status = active.status;
        expect(status.size() == 1900 && status[0].active && status[0].remaining_frames > 0, "WLD-06: the status is published while on");
        expect(status.size() == 1900 && !status[1100].active && !status[1100].ready
            && status[1100].remaining_frames == started + 1800 - 1101, "WLD-06: after the run it still counts down from the switch-on");
        expect(status.size() == 1900 && status[1899].ready, "WLD-06: it is ready 60 s after the switch-on");
        const auto state = active.session->ability_state(2);
        expect(state && state->slots[0].ready_tick == started + 1800, "WLD-06: ready tick is switch-on plus the recharge");
        // The same record replays to the same hashes and event stream.
        const auto bytes = tactical::write_replay(*active.replay);
        const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == *active.replay, "WLD-12: the commands round trip through the replay format");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), laser_defence_sensors(), laser_defence_durability(),
            motion(), std::nullopt, laser_defence_combat(true), {}, laser_defence_abilities());
        expect(static_cast<bool>(playback), "WLD-12: the replay creates a session");
        if (playback) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            for (std::size_t tick = 0; tick < 400 && tick < active.hashes.size(); ++tick) {
                const auto step = playback.value().step(executor);
                expect(step && step.value().state_sha256 == active.hashes[tick], "WLD-12: the replay matches every live frame");
            }
        }
    }

    // WLD-04: a shot that has not flown 100 units yet is spared, and so is a Crusader that is switched off.
    const auto close = laser_run(armed, 90, 1, 120);
    expect(!close.hits.empty() && close.zaps.empty(), "WLD-04: a missile inside its first 100 units is not shot down");

    // WLD-06: switching off early keeps the countdown; a second switch-on before it ends is refused.
    const std::vector<tactical::PlayerCommand> toggled{command(0, 2, 0, 2, laser(tactical::AbilityAction::activate)),
        command(100, 2, 1, 2, laser(tactical::AbilityAction::deactivate)),
        command(200, 2, 2, 2, laser(tactical::AbilityAction::activate))};
    const auto early = laser_run(toggled, 800, 1, 300);
    const auto early_state = early.session ? early.session->ability_state(2) : std::nullopt;
    expect(early_state && !early_state->slots[0].active && early_state->slots[0].ready_tick == 1800,
        "WLD-06: an early switch-off leaves the switch-on's countdown");
    expect(std::any_of(early.events.begin(), early.events.end(), [](const auto& event) {
        return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
    }), "WLD-06: a second switch-on during the countdown is rejected");
    expect(std::none_of(early.zaps.begin(), early.zaps.end(), [](const auto& zap) { return zap.tick > 101; }),
        "WLD-03: a Crusader switched off stops looking");
}


// WSD-01..10: SELF_DESTRUCT. The countdown runs 105 frames (Expiration_Seconds 3.5), the unit then leaves the world
// with its blast object (900 damage within 500, enemies only, the next frame), a cancel recharges for the share of
// the countdown that ran, and the Krayt object script's triggers are served for the AI and for a human's autofire.
constexpr tactical::TypeId blast_type = 0x5D5D5D5D5D5D5D5DULL;

[[nodiscard]] tactical::AbilityProfile self_destruct_ability() {
    tactical::AbilityProfile ability;
    ability.kind = tactical::AbilityKind::self_destruct;
    ability.expiration_frames = 105;
    ability.recharge_frames = 300;
    ability.supports_autofire = true;
    ability.spawned = tactical::SpawnedAbilityProfile{};
    ability.spawned->type = blast_type;
    ability.spawned->damage_type = 0;
    ability.spawned->blast.damage = units(900);
    ability.spawned->blast.radius = units(500);
    ability.spawned->blast.max_delay = {};
    return ability;
}

struct SelfDestructWorld {
    tactical::TacticalSetup start;
    tactical::DurabilityTable health;
    tactical::CombatTable combat;
    tactical::AbilityTable table;
    std::vector<tactical::SensorProfile> sensors;
};

// Unit 1 (plain type, player 1) holds the ability; 2 is a player-2 frigate in reach, 3 a player-2 corvette out of reach
// and 4 a corvette of the holder's own player.
[[nodiscard]] SelfDestructWorld self_destruct_world(const bool script, const std::vector<tactical::PlayerId>& humans) {
    SelfDestructWorld world;
    world.start = setup();
    world.start.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, frigate_type, 2, at(300, 0), math::identity_quat(), {}},
        {3, corvette_type, 2, at(800, 0), math::identity_quat(), {}},
        {4, corvette_type, 1, at(100, 0), math::identity_quat(), {}}};
    world.health = durability();
    for (auto& row : world.health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = 2;
        profile.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
        world.combat.profiles.push_back(profile);
        world.sensors.push_back({type, units(1000)});
    }
    tactical::UnitAbilityProfile holder{plain_type, {self_destruct_ability()}, false};
    holder.self_destruct_script = script;
    world.table.profiles = {holder};
    world.table.humans = humans;
    world.table.autofire_defaults = humans;
    return world;
}

struct SelfDestructRun {
    std::vector<std::string> hashes;
    std::vector<bool> alive;                 // unit 1 after each tick
    std::vector<bool> active;                // its SELF_DESTRUCT is on after each tick
    std::vector<Fixed> enemy, far, ally;     // hulls of units 2, 3, 4 after each tick
    std::vector<Fixed> holder;               // hull of unit 1 after each tick
    std::vector<std::uint64_t> services;     // the ticks unit 1's Krayt script ran (WSD-09)
    std::vector<std::size_t> spawns;         // ability_spawns() per tick
    std::vector<tactical::Event> events;
    std::optional<tactical::TacticalSession> session;
};

[[nodiscard]] SelfDestructRun run_self_destruct(const SelfDestructWorld& world,
    const std::vector<tactical::PlayerCommand>& commands, const std::size_t workers, const std::uint64_t ticks) {
    SelfDestructRun result;
    auto created = tactical::TacticalSession::create(world.start, world.sensors, world.health, {}, std::nullopt,
        world.combat, {}, world.table);
    expect(static_cast<bool>(created), "WSD-01: the self-destruct session is created");
    if (!created) { std::cerr << created.error().message << '\n'; return result; }
    auto session = std::move(created).value();
    for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "WSD-01: a command is accepted");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    const auto hull = [&](const eawr::sim::EntityId id) {
        const auto state = session.durability_state(id);
        return state ? state->hull : Fixed{};
    };
    std::optional<std::int64_t> last_service;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "WSD-01: a step succeeds");
        if (!stepped) break;
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->events()) result.events.push_back(event);
        const auto live = session.units();
        result.alive.push_back(std::any_of(live.begin(), live.end(), [](const auto& unit) { return unit.entity_id == 1; }));
        const auto abilities_of_holder = session.ability_state(1);
        result.active.push_back(abilities_of_holder && abilities_of_holder->slots[0].active);
        result.enemy.push_back(hull(2));
        result.far.push_back(hull(3));
        result.ally.push_back(hull(4));
        result.holder.push_back(hull(1));
        const auto script = abilities_of_holder ? abilities_of_holder->self_destruct_script : std::nullopt;
        if (script && (!last_service || *last_service != script->last_service)) result.services.push_back(tick);
        if (script) last_service = script->last_service;
        result.spawns.push_back(stepped.value().snapshot->ability_spawns().size());
    }
    result.session.emplace(std::move(session));
    return result;
}

void test_self_destruct_projectile_order() {
    auto world = self_destruct_world(false, {1});
    for (auto& profile : world.combat.profiles) {
        if (profile.type_id != corvette_type) continue;
        // Leave ship acquisition disabled so only the explicit attack below starts firing.
        tactical::WeaponProfile weapon;
        weapon.range = units(1000); weapon.pulse_count = 1;
        // Zero recharge guarantees the first shot on the expiry tick regardless of the spawn draw.
        weapon.cone_width = units(360); weapon.cone_height = units(160);
        weapon.shot = tactical::ShotProfile{units(10), 0, units(1), units(1000), true, true, {}};
        profile.weapons = {weapon};
    }
    const std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 1, tactical::AbilityPayload{tactical::AbilityKind::self_destruct, tactical::AbilityAction::activate}),
        // Commands commit after this tick's launches; arm the attack for tick 105.
        command(104, 1, 1, 4, tactical::AttackPayload{3})};
    std::vector<std::string> first_hashes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(world.start, world.sensors, world.health, {}, std::nullopt,
            world.combat, {}, world.table);
        expect(static_cast<bool>(created), "SELF_DESTRUCT order: the armed session is created");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& entry : commands)
            expect(static_cast<bool>(session.submit(entry)), "SELF_DESTRUCT order: a command is accepted");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::optional<std::uint64_t> shot_id, blast_id;
        for (std::uint64_t tick = 0; tick < 107; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "SELF_DESTRUCT order: a step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto projectiles = session.projectiles();
            expect(std::adjacent_find(projectiles.begin(), projectiles.end(), [](const auto& left, const auto& right) {
                return left.id >= right.id;
            }) == projectiles.end(), "SELF_DESTRUCT order: projectile IDs stay strictly ascending");
            if (tick == 105) {
                expect(projectiles.size() == 2, "SELF_DESTRUCT order: expiry coincides with one ordinary launch");
                for (const auto& projectile : projectiles) {
                    expect(session.request_projectile_explosion(projectile.id),
                        "SELF_DESTRUCT order: lookup finds each ordinary shot and blast");
                    if (projectile.shooter == 4) shot_id = projectile.id;
                    if (projectile.shooter == 1) blast_id = projectile.id;
                }
                expect(shot_id && blast_id && *shot_id < *blast_id,
                    "SELF_DESTRUCT order: the ordinary launch precedes the newly allocated blast");
            }
            if (tick == 106) {
                expect(shot_id && !session.request_projectile_explosion(*shot_id),
                    "SELF_DESTRUCT order: the ordinary shot's requested explosion is serviced next tick");
                expect(blast_id && !session.request_projectile_explosion(*blast_id),
                    "SELF_DESTRUCT order: the blast detonates next tick");
                const auto enemy = session.durability_state(2);
                expect(enemy && enemy->hull == units(2700), "SELF_DESTRUCT order: the blast still delivers its damage");
            }
        }
        expect(hashes.size() == 107, "SELF_DESTRUCT order: every tick completes");
        if (workers == 1) first_hashes = hashes;
        else expect(hashes == first_hashes, "SELF_DESTRUCT order: hashes match with " + std::to_string(workers) + " workers");
    }
}

void test_self_destruct() {
    using K = tactical::AbilityKind;
    expect(tactical::ability_kind("self_destruct") == K::self_destruct && tactical::to_string(K::self_destruct) == "SELF_DESTRUCT"
        && static_cast<unsigned>(K::self_destruct) == 21U, "WSD-01: SELF_DESTRUCT binds as kind 21");
    const std::vector<tactical::PlayerCommand> activate{command(0, 1, 0, 1,
        tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::activate})};

    // WSD-03..07: the countdown, the kill, the blast.
    const auto world = self_destruct_world(false, {1});
    const auto first = run_self_destruct(world, activate, 1, 130);
    expect(first.hashes.size() == 130, "WSD-03: the battle runs");
    if (first.hashes.size() == 130) {
        expect(first.alive[0] && first.alive[50] && first.alive[103] && first.alive[104],
            "WSD-03: the unit keeps its place through the countdown");
        expect(!first.alive[105] && !first.alive[129], "WSD-04: the unit is gone once the 105 frames have run");
        const auto died = std::find_if(first.events.begin(), first.events.end(), [](const auto& event) {
            return event.kind == tactical::EventKind::unit_destroyed && event.unit == 1; });
        expect(died != first.events.end() && died->tick == 105, "WSD-05: the unit is destroyed in the tick its countdown runs out");
        expect(first.spawns[104] == 0 && first.spawns[105] == 1 && first.spawns[106] == 0,
            "WSD-06: the blast object exists for the frame between the kill and its detonation");
        expect(first.enemy[105] == units(3600) && first.enemy[106] == units(3600 - 900),
            "WSD-06: the enemy in reach takes the area damage the frame after the kill");
        expect(first.far[129] == units(3600), "WSD-06: an enemy beyond the blast radius takes nothing");
        expect(first.ally[129] == units(3600), "WSD-07: the blast never damages the owner's own units");
        expect(first.enemy[129] == units(2700), "WSD-06: the blast damages once");
    }
    // 1/2/4/8 workers and the replay agree.
    for (const std::size_t workers : {2U, 4U, 8U}) {
        const auto again = run_self_destruct(world, activate, workers, 130);
        expect(again.hashes == first.hashes, "WSD-03: self-destruct hashes match with " + std::to_string(workers) + " workers");
    }
    if (first.session) {
        const auto replay = first.session->record();
        const auto bytes = tactical::write_replay(replay);
        const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == replay, "WSD-03: the SELF_DESTRUCT command survives the replay format");
        if (parsed) {
            auto again = tactical::TacticalSession::from_replay(parsed.value(), world.sensors, world.health, {},
                std::nullopt, world.combat, {}, world.table);
            expect(static_cast<bool>(again), "WSD-03: the replay session is created");
            if (again) {
                const eawr::platform::ThreadWorkerAdapter executor(2);
                std::vector<std::string> hashes;
                while (again.value().completed_tick() < replay.final_tick_count) {
                    hashes.push_back(again.value().step(executor).value().state_sha256);
                }
                expect(hashes == first.hashes, "WSD-03: the replay reproduces every tick hash");
            }
        }
    }

    // WSD-02, WSD-08: a cancel switches it off and recharges for the share of the countdown that ran; the
    // ability is refused while it recharges; the unit lives on.
    std::vector<tactical::PlayerCommand> cancel = activate;
    cancel.push_back(command(50, 1, 1, 1, tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::deactivate}));
    cancel.push_back(command(100, 1, 2, 1, tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::activate}));
    cancel.push_back(command(193, 1, 3, 1, tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::activate}));
    const auto cancelled = run_self_destruct(world, cancel, 1, 260);
    if (cancelled.hashes.size() == 260) {
        expect(cancelled.alive[120] && cancelled.alive[200], "WSD-08: a cancelled countdown leaves the unit alive");
        expect(cancelled.enemy[120] == units(3600), "WSD-08: a cancelled countdown never explodes");
        const auto refused = std::count_if(cancelled.events.begin(), cancelled.events.end(), [](const auto& event) {
            return event.kind == tactical::EventKind::order_rejected && event.tick == 100; });
        expect(refused == 1, "WSD-08: a new countdown is refused while the cancel's recharge runs");
    }
    {
        // 50 of 105 frames ran: round(50 / 105 x 300) = 143 frames of recharge, ready at 193; the command at 193 acts.
        const auto state = cancelled.session ? cancelled.session->ability_state(1) : std::nullopt;
        expect(state && state->slots[0].active && state->slots[0].started_tick == 193 && state->slots[0].expires_tick == 298,
            "WSD-08: the recharge ended after 143 frames, so the command at 193 starts a countdown to 298");
    }

    // WSD-09: the script's AI and autofire triggers. A frigate of player 2 at (200, 0) shoots the holder once,
    // whatever the world; `crowd` places two player-2 corvettes.
    const auto scripted = [&](const bool human, const bool armed, const std::int64_t damage, const std::int64_t rating,
                              const std::array<math::Vec3, 2>& crowd, const std::vector<tactical::PlayerCommand>& extra,
                              const std::uint64_t ticks) {
        auto fixture = self_destruct_world(true, human ? std::vector<tactical::PlayerId>{1} : std::vector<tactical::PlayerId>{});
        if (human && !armed) fixture.table.autofire_defaults.clear();
        fixture.table.combat_ratings = {{frigate_type, units(rating)}};
        for (auto& row : fixture.health.profiles) if (row.type_id == plain_type) row.max_hull = units(100);
        fixture.start.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}},
            {2, frigate_type, 2, at(200, 0), math::identity_quat(), {}},
            {3, corvette_type, 2, crowd[0], math::identity_quat(), {}},
            {4, corvette_type, 2, crowd[1], math::identity_quat(), {}}};
        for (auto& profile : fixture.combat.profiles) {
            if (profile.type_id != frigate_type) continue;
            profile.max_attack_distance = units(300);
            tactical::WeaponProfile weapon; weapon.range = units(300); weapon.pulse_count = 1;
            weapon.min_recharge_hundredths = 2000; weapon.max_recharge_hundredths = 2000; // its first shot lands within 20 s
            weapon.opportunity_when_idle = true; weapon.opportunity_when_targeting = true;
            weapon.cone_width = units(360); weapon.cone_height = units(160);
            weapon.shot = tactical::ShotProfile{units(damage), 0, units(40), units(300), true, true, {}};
            profile.weapons = {weapon};
        }
        std::vector<tactical::PlayerCommand> commands{command(0, 2, 0, 2, tactical::AttackPayload{1})};
        commands.insert(commands.end(), extra.begin(), extra.end());
        return run_self_destruct(fixture, commands, 1, ticks);
    };
    const std::array<math::Vec3, 2> near_crowd{at(250, 0), at(250, 50)};
    const std::array<math::Vec3, 2> far_crowd{at(900, 0), at(900, 50)};
    const auto shooting = [&](const bool human, const bool armed, const std::int64_t damage, const std::int64_t rating,
                              const bool crowd) {
        return scripted(human, armed, damage, rating, crowd ? near_crowd : far_crowd, {}, 800);
    };
    // The countdown has started (the unit is counting down or already gone).
    const auto triggered = [](const SelfDestructRun& run) {
        return std::any_of(run.active.begin(), run.active.end(), [](const bool on) { return on; });
    };
    const auto first_active = [](const SelfDestructRun& run) -> std::optional<std::uint64_t> {
        const auto on = std::find(run.active.begin(), run.active.end(), true);
        if (on == run.active.end()) return std::nullopt;
        return static_cast<std::uint64_t>(on - run.active.begin());
    };
    // The AI below half hull with the threat nearby (rating 2000 against 2 x hull share x 1500 = 1200).
    const auto ai = shooting(false, false, 60, 2000, false);
    expect(triggered(ai), "WSD-09: the AI's Krayt self-destructs below half hull when the nearby threat reaches the trigger");
    // A weak threat (rating 100 < 1200) or a healthy hull does nothing.
    expect(!triggered(shooting(false, false, 60, 100, false)), "WSD-09: the AI waits while the nearby threat is below the trigger");
    const auto healthy = shooting(false, false, 10, 2000, false);
    expect(!triggered(healthy), "WSD-09: the AI waits above half hull");
    // A human's unit needs autofire, at most 20 % hull and more than two neighbours in range.
    expect(triggered(shooting(true, true, 85, 0, true)), "WSD-09: autofire self-destructs a human's unit at 20 % hull among three enemies");
    expect(!triggered(shooting(true, false, 85, 0, true)), "WSD-09: a human's unit without autofire never self-destructs on its own");
    expect(!triggered(shooting(true, true, 85, 0, false)), "WSD-09: autofire waits for more than two neighbours");
    expect(!triggered(shooting(true, true, 60, 0, true)), "WSD-09: autofire waits for 20 % hull");

    // WFO-22, WSD-09: the script runs in the unit's first frame, then whenever the engine's single-precision
    // clock is more than 1 s past LastService; the first LastService is minus the entity's phase (1 frame for
    // unit 1). The float clock puts the services 29, 30 or 31 frames apart.
    const std::vector<std::uint64_t> services{0, 29, 59, 89, 120, 150, 181, 212, 242, 273, 304, 335, 366, 397};
    expect(healthy.services.size() >= services.size()
            && std::equal(services.begin(), services.end(), healthy.services.begin()),
        "WFO-22: the Krayt script runs at frame 0, then on the single-precision clock's 1 s service rate");
    // WSD-09: the AI acts in the first service after the hull falls below half (the frigate was in range at
    // every earlier service's proximity count).
    if (ai.holder.size() == 800) {
        const auto hit = std::find_if(ai.holder.begin(), ai.holder.end(), [](const Fixed hull) { return hull < units(100); });
        const auto hit_tick = static_cast<std::uint64_t>(hit - ai.holder.begin());
        const auto next = std::find_if(ai.services.begin(), ai.services.end(), [&](const std::uint64_t tick) { return tick > hit_tick; });
        expect(hit != ai.holder.end() && next != ai.services.end() && first_active(ai) == std::optional<std::uint64_t>(*next),
            "WSD-09: the AI's Krayt switches SELF_DESTRUCT on in the script's first service after the hull falls");
    }
    // WSD-10: the proximity query is a box of 400 around the unit in all three axes that touches each object's
    // box (20 wide here): corvettes at (350, +/-350) (planar 495) count, corvettes 405 out count by their box,
    // corvettes 600 above the plane or 460 out do not.
    expect(triggered(scripted(true, true, 85, 0, {at(350, 350), at(350, -350)}, {}, 800)),
        "WSD-10: the proximity box reaches the corners of the 400 square");
    expect(triggered(scripted(true, true, 85, 0, {at(405, 0), at(0, -405)}, {}, 800)),
        "WSD-10: an object whose box reaches into the 400 square counts");
    expect(!triggered(scripted(true, true, 85, 0, {at(250, 0, 600), at(250, 50, 600)}, {}, 800)),
        "WSD-10: the proximity box ends 400 above and below the unit");
    expect(!triggered(scripted(true, true, 85, 0, {at(460, 0), at(0, 460)}, {}, 800)),
        "WSD-10: boxes beyond the 400 square do not count");
    // WSD-09: autofire switched on later takes a service to arm the human script; it acts in the service after.
    // A run without autofire gives the frame the frigate's shot brings the holder to 15 % and the script's
    // services; the same battle then switches autofire on in the frame after that hit.
    {
        const auto calm = scripted(true, false, 85, 0, near_crowd, {}, 1300);
        const auto hit = std::find_if(calm.holder.begin(), calm.holder.end(), [](const Fixed hull) { return hull <= units(20); });
        expect(calm.holder.size() == 1300 && hit != calm.holder.end(), "WSD-09: the frigate brings the holder to 15 %");
        if (calm.holder.size() == 1300 && hit != calm.holder.end()) {
            const auto armed_at = static_cast<std::uint64_t>(hit - calm.holder.begin()) + 1;
            const auto died = std::find(calm.alive.begin(), calm.alive.end(), false);
            const auto death = static_cast<std::uint64_t>(died - calm.alive.begin());
            // The command commits after the frame's ability phase: the script sees autofire from the next frame.
            const auto arms = std::find_if(calm.services.begin(), calm.services.end(),
                [&](const std::uint64_t tick) { return tick > armed_at; });
            const auto acts = arms == calm.services.end() ? arms : std::next(arms);
            expect(acts != calm.services.end() && *acts < death, "WSD-09: the holder lives two services past the hit");
            if (acts != calm.services.end() && *acts < death) {
                const std::vector<tactical::PlayerCommand> arm{command(armed_at, 1, 0, 1,
                    tactical::AbilityPayload{K::self_destruct, tactical::AbilityAction::autofire_on})};
                const auto late = scripted(true, false, 85, 0, near_crowd, arm, 1300);
                expect(first_active(late) == std::optional<std::uint64_t>(*acts),
                    "WSD-09: autofire switched on after the hit arms the script at its next service and acts at the one after");
            }
        }
    }

    // WSD-01: the table refuses a self-destruct without its countdown or blast object.
    for (const int variant : {0, 1, 2}) {
        auto bad = self_destruct_world(false, {1});
        auto& ability = bad.table.profiles[0].abilities[0];
        if (variant == 0) ability.expiration_frames = 0;
        if (variant == 1) ability.spawned.reset();
        if (variant == 2) ability.spawned->blast = {};
        expect(!tactical::TacticalSession::create(bad.start, bad.sensors, bad.health, {}, std::nullopt, bad.combat, {}, bad.table),
            "WSD-01: a self-destruct needs an expiration and a blast object");
    }
}

// --- REDIRECT_BLASTER (WRB-01 to WRB-12, the Underworld armour upgrades' deflection) ----------------

constexpr tactical::TypeId blaster_type = 16;
constexpr tactical::TypeId deflector_type = 17;
constexpr tactical::TypeId upgrade_station_type = 18;
constexpr tactical::TypeId armour_upgrade_type = 90;

struct RedirectScenario {
    bool initially_enabled{true};
    bool buy_upgrade{};
    bool upgrade_all_allies{};
    bool deflector_owned_by_ally{};   // the deflector belongs to a third player allied to the upgrade's holder
    const char* block{"1.0"};
    const char* redirect{"0.0"};
    const char* arc{"360"};
    bool blockable{true};
    std::uint64_t filter_categories{1};
    std::int64_t distance{400};
    bool duplicate{};
    bool delayed{};
    bool moving{};
    bool reflect_twice{};
};

[[nodiscard]] tactical::AbilityTable redirect_abilities(const RedirectScenario& scenario) {
    tactical::SpecialAbilityProfile handler;
    handler.name = "Magnetic_Armor_L2";
    handler.kind = tactical::SpecialAbilityKind::redirect_blaster;
    handler.style = tactical::SpecialActivationStyle::unspecified;
    if (!scenario.initially_enabled) handler.initially_enabled = false;
    handler.service_interval = 0;
    handler.redirect_chance = decimal(scenario.redirect);
    handler.block_chance = decimal(scenario.block);
    handler.reaction_arc = decimal(scenario.arc);
    handler.max_redirect_angle = decimal("30");
    handler.filter.applicable_categories = scenario.filter_categories;
    tactical::AbilityTable table;
    table.profiles = {{deflector_type, {}, false, {handler}}};
    if (scenario.duplicate) {
        handler.name = "Second_Magnetic_Armor";
        table.profiles[0].special.push_back(handler);
    }
    if (scenario.reflect_twice) {
        handler.filter.applicable_categories = 2;
        table.profiles.insert(table.profiles.begin(), {blaster_type, {}, false, {handler}});
    }
    return table;
}

[[nodiscard]] tactical::CombatTable redirect_combat(const RedirectScenario& scenario) {
    tactical::CombatTable table;
    const tactical::CollisionBox box{at(-20, -10, -10), at(20, 10, 10)};
    tactical::WeaponProfile weapon;
    weapon.hardpoint = 0;
    weapon.range = units(1400);
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.pulse_count = 1;
    weapon.cone_width = units(175);
    weapon.cone_height = units(160);
    weapon.opportunity_when_idle = true;
    weapon.opportunity_when_targeting = true;
    weapon.fire_a = at(20, 0);
    weapon.shot = tactical::ShotProfile{units(10), 0, units(7), units(1400), true, true, {}};
    weapon.shot->redirect_blockable = scenario.blockable;
    if (scenario.delayed) weapon.shot->damage_delay = decimal("0.1");
    tactical::CombatProfile shooter;
    shooter.type_id = blaster_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(1400);
    shooter.weapons = {weapon};
    shooter.collision = box;
    tactical::CombatProfile deflector;
    deflector.type_id = deflector_type;
    deflector.category_bits = 2;
    deflector.collision = box;
    if (scenario.reflect_twice) {
        // A colliding slot is deliberately a different, nondeflectable projectile profile.
        weapon.opportunity_when_idle = weapon.opportunity_when_targeting = false;
        weapon.shot->speed = units(99);
        weapon.shot->redirect_blockable = false;
        deflector.weapons = {weapon};
    }
    tactical::CombatProfile station;
    station.type_id = upgrade_station_type;
    station.category_bits = 4;
    station.collision = box;
    table.profiles = {shooter, deflector, station};
    return table;
}

[[nodiscard]] tactical::DurabilityTable redirect_durability() {
    auto table = durability();
    auto profile = table.profiles.front();
    for (const auto type : {blaster_type, deflector_type, upgrade_station_type}) {
        profile.type_id = type;
        table.profiles.push_back(profile);
    }
    std::sort(table.profiles.begin(), table.profiles.end(), [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
    return table;
}

struct RedirectRun {
    std::vector<std::string> hashes;
    std::uint64_t deflected{}, landed{}, fired_back{}, reflected_again{};
    bool fired_back_toward_shooter{};
    math::Fixed hull{};
    std::vector<std::string> events;
};

[[nodiscard]] RedirectRun redirect_run(const RedirectScenario& scenario, const std::size_t workers, const std::uint64_t ticks) {
    RedirectRun result;
    tactical::TacticalSetup initial;
    initial.seed = 1876;
    initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable},
        {3, 2, 2, tactical::player_flag_commandable}};
    const tactical::PlayerId owner = scenario.deflector_owned_by_ally ? 3 : 2;
    initial.units = {
        {1, blaster_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, deflector_type, owner, at(scenario.distance, 0), math::identity_quat(), {}},
        {3, upgrade_station_type, 2, at(-3000, 3000), math::identity_quat(), {}},
    };
    tactical::EconomyRules economy;
    economy.players = {{2, units(100), 25, false, {}, 1, 3}};
    tactical::BuildOption option;
    option.type = armour_upgrade_type;
    option.kind = tactical::BuildKind::upgrade;
    option.queue = tactical::BuildQueue::upgrades;
    option.price = units(10);
    option.build_frames = 2; option.ai_build_frames = 2; option.available = true;
    economy.menus = {{upgrade_station_type, 2, {option}}};
    tactical::UpgradeProfile upgrade;
    upgrade.type = armour_upgrade_type;
    tactical::AbilityEnableRule rule;
    rule.ability_name = "magnetic_armor_l2";
    rule.applicable = {deflector_type};
    rule.all_allies = scenario.upgrade_all_allies;
    upgrade.ability_enables = {rule};
    economy.upgrades = {upgrade};
    const std::vector<tactical::SensorProfile> sensors{{blaster_type, units(3000)}, {deflector_type, units(3000)},
        {upgrade_station_type, units(3000)}};
    auto moving_table = motion();
    if (scenario.moving) {
        auto moving_profile = moving_table.profiles.front();
        moving_profile.type_id = deflector_type;
        moving_profile.max_speed = decimal("0.2");
        moving_table.profiles.push_back(moving_profile);
    }
    auto created = tactical::TacticalSession::create(initial, sensors, redirect_durability(), moving_table, std::nullopt,
        redirect_combat(scenario), {}, redirect_abilities(scenario), economy);
    expect(static_cast<bool>(created), "the redirect blaster session is created");
    if (!created) { std::cerr << created.error().message << '\n'; return result; }
    auto session = std::move(created).value();
    if (scenario.moving) expect(static_cast<bool>(session.submit(command(0, owner, 0, 2,
        tactical::MovePayload{at(scenario.distance, 1000)}))), "moving defender command");
    if (scenario.buy_upgrade) expect(static_cast<bool>(session.submit(command(0, 2, 0, 3,
        tactical::BuyPayload{armour_upgrade_type}))), "the armour upgrade is bought");
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "a redirect blaster step succeeds");
        if (!stepped) { std::cerr << stepped.error().message << '\n'; break; }
        result.hashes.push_back(stepped.value().state_sha256);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::projectile_hit || event.target != 2) continue;
            if ((event.outcome & tactical::hit_outcome_redirected) != 0U) ++result.deflected;
            else ++result.landed;
        }
        for (const auto& projectile : session.projectiles()) {
            if (projectile.reflected) {
                expect(projectile.profile_key == tactical::ProjectileProfileKey{
                    blaster_type, 0, tactical::ProjectileProfileKind::ordinary} && projectile.speed == units(7),
                    "WRB-10: reflected shots retain their launch type and profile independently of the new shooter's slot");
                if (projectile.shooter == 1) ++result.reflected_again;
            }
            if (projectile.owner != owner || projectile.shooter != 2) continue;
            ++result.fired_back;
            // Flying back towards the shooter at the origin: a step with a negative x part.
            if (projectile.step.x.raw() < 0) result.fired_back_toward_shooter = true;
        }
    }
    if (const auto state = session.durability_state(2)) result.hull = math::Fixed::from_raw(state->hull.raw() + state->shields.raw());
    return result;
}

void test_redirect_blaster() {
    // WRB-01: the nested handler binds by its authored name.
    expect(tactical::special_ability_kind("Redirect_Blaster_Ability") == tactical::SpecialAbilityKind::redirect_blaster
        && tactical::special_service_interval(tactical::SpecialAbilityKind::redirect_blaster) == 0,
        "WRB-01: the Redirect_Blaster handler binds and has no service");
    // WRB-04: the table refuses chances and angles outside the authored ranges.
    {
        auto table = redirect_abilities({});
        expect(static_cast<bool>(tactical::validate_abilities(table)), "WRB-04: the deflector table is valid");
        auto chance = table;
        chance.profiles[0].special[0].block_chance = decimal("1.5");
        expect(!tactical::validate_abilities(chance), "WRB-04: a block chance above 1 is refused");
        auto arc = table;
        arc.profiles[0].special[0].reaction_arc = decimal("361");
        expect(!tactical::validate_abilities(arc), "WRB-04: an arc above 360 degrees is refused");
        auto angle = table;
        angle.profiles[0].special[0].max_redirect_angle = decimal("181");
        expect(!tactical::validate_abilities(angle), "WRB-04: a redirection angle above 180 degrees is refused");
    }
    const auto control = redirect_run(RedirectScenario{true, false, false, false, "0.0"}, 1, 400);
    expect(control.landed > 0 && control.deflected == 0 && control.fired_back == 0,
        "WRB-06 control: with no chance every shot lands and nothing is flown back");

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const auto blocked = redirect_run(RedirectScenario{}, workers, 400);
        if (workers == 1) reference = blocked.hashes;
        else expect(blocked.hashes == reference, "WRB-11: redirect blaster hashes equal at 1/2/4/8 workers");
        expect(blocked.deflected > 0 && blocked.landed == 0, "WRB-06: with block chance 1 every shot is deflected and none lands");
        expect(blocked.fired_back > 0 && blocked.fired_back_toward_shooter,
            "WRB-10: a block flies a fresh projectile of the deflector's owner back, turned around");
    }
    {
        const auto none = redirect_run(RedirectScenario{true, false, false, false, "0.0"}, 1, 400);
        const auto full = redirect_run(RedirectScenario{}, 1, 400);
        expect(full.hull.raw() > none.hull.raw(), "WRB-06: a deflected shot deals no direct damage (hull plus shields)");
    }
    // WRB-05: disabled until an upgrade names it; only then does it react.
    {
        const auto disabled = redirect_run(RedirectScenario{false, false}, 1, 400);
        expect(disabled.deflected == 0 && disabled.landed > 0, "WRB-05: Initially_Enabled No does nothing without the upgrade");
        const auto enabled = redirect_run(RedirectScenario{false, true}, 1, 400);
        expect(enabled.deflected > 0 && enabled.landed == 0, "WRB-05: the bought upgrade's Enable_Ability switches it on");
        // The deflector's owner is an ally of the holder: only an upgrade that affects all allies reaches it.
        const auto ally = redirect_run(RedirectScenario{false, true, false, true}, 1, 400);
        expect(ally.deflected == 0, "WRB-05: an upgrade without Affects_All_Allies does not enable an ally's unit");
        const auto shared = redirect_run(RedirectScenario{false, true, true, true}, 1, 400);
        expect(shared.deflected > 0, "WRB-05: an upgrade that affects all allies enables an ally's unit");
    }
    // WRB-06: shots the handler refuses (a missile, a bomb, a rocket and the like) land.
    {
        const auto missile = redirect_run(RedirectScenario{true, false, false, false, "1.0", "0.0", "360", false}, 1, 400);
        expect(missile.deflected == 0 && missile.landed > 0, "WRB-06: a shot the category list refuses is not deflected");
    }
    // WRB-06: Applicable_Unit_Categories names the shooters it reacts to.
    {
        const auto other = redirect_run(RedirectScenario{true, false, false, false, "1.0", "0.0", "360", true, 4}, 1, 400);
        expect(other.deflected == 0 && other.landed > 0, "WRB-06: a shooter outside the authored categories is not deflected");
    }
    // WRB-07: a narrow reaction arc ignores shots that travel along the deflector's own facing.
    {
        const auto narrow = redirect_run(RedirectScenario{true, false, false, false, "1.0", "0.0", "90"}, 1, 400);
        expect(narrow.deflected == 0 && narrow.landed > 0, "WRB-07: a 90 degree arc ignores a shot from behind");
    }
    // WRB-09, WRB-10: redirection is rolled first and aims at the shooter.
    {
        const auto redirected = redirect_run(RedirectScenario{true, false, false, false, "0.0", "1.0"}, 1, 400);
        expect(redirected.deflected > 0 && redirected.landed == 0 && redirected.fired_back > 0 && redirected.fired_back_toward_shooter,
            "WRB-09/WRB-10: a redirect chance of 1 sends a fresh projectile at the shooter");
    }
    // WRB-09: a middling chance deflects some and lets some land, the same at every worker count.
    {
        const auto half = redirect_run(RedirectScenario{true, false, false, false, "0.5"}, 1, 900);
        expect(half.deflected > 0 && half.landed > 0, "WRB-09: a block chance of one half deflects some shots and lets some land");
        const auto again = redirect_run(RedirectScenario{true, false, false, false, "0.5"}, 4, 900);
        expect(half.hashes == again.hashes && half.deflected == again.deflected && half.landed == again.landed,
            "WRB-11: the rolls are keyed by the shot, not by the worker count");
    }
    // WRB-03/08: creating the replacement is a side effect even when movement reports failure.
    {
        tactical::CombatEvent event; event.shooter = 1; event.weapon = 7; event.aim = at(100, 0);
        tactical::ShotProfile shot; shot.speed = units(7); shot.max_travel = units(1400); shot.redirect_blockable = true;
        auto original = tactical::detail::launch_projectile(event, shot, 1, true, 1, {}).value();
        tactical::detail::CombatUnit defender; defender.id = 2; defender.owner = 2;
        tactical::detail::CombatUnit attacker; attacker.id = 1; attacker.position = at(-100, 0);
        auto handler = redirect_abilities({}).profiles[0].special[0];
        for (const auto kind : {tactical::ProjectileProfileKind::ordinary, tactical::ProjectileProfileKind::ability,
                               tactical::ProjectileProfileKind::barrage}) {
            original.profile_key = tactical::ProjectileProfileKey{blaster_type, 7, kind};
            tactical::CombatRandom rolls(42, 1, 1, tactical::redirect_blaster_slot);
            tactical::CombatRandom directions(42, 1, 1, tactical::redirect_blaster_direction_slot);
            const auto result = tactical::detail::redirect_blaster_reaction(original, shot, handler, defender, attacker,
                2, rolls, directions, false);
            expect(result && result.value().created && result.value().created->profile_key == original.profile_key,
                "WRB-10: ordinary, ability and barrage profile identities survive reflection");
            expect(rolls.draws() == 2 && directions.draws() >= 2,
                "WRB-09: redirect/block rolls and angle/pitch use their separate reserved streams");
            if (!result || !result.value().created) continue;
            auto reflected = *result.value().created;
            std::vector<std::uint8_t> bytes, changed;
            tactical::detail::append_projectile(bytes, reflected);
            ++reflected.profile_key->type;
            tactical::detail::append_projectile(changed, reflected);
            expect(bytes != changed, "WRB-10: a reflected shot's retained type is canonical state");
            reflected = *result.value().created; ++reflected.profile_key->weapon; changed.clear();
            tactical::detail::append_projectile(changed, reflected);
            expect(bytes != changed, "WRB-10: a reflected shot's retained slot is canonical state");
            reflected = *result.value().created;
            reflected.profile_key->kind = kind == tactical::ProjectileProfileKind::ordinary
                ? tactical::ProjectileProfileKind::ability : tactical::ProjectileProfileKind::ordinary;
            changed.clear(); tactical::detail::append_projectile(changed, reflected);
            expect(bytes != changed, "WRB-10: a reflected shot's retained override kind is canonical state");
        }
    }
    for (const auto redirect : {false, true}) {
        RedirectScenario scenario;
        if (redirect) { scenario.redirect = "1.0"; scenario.block = "0.0"; }
        const auto stationary = redirect_run(scenario, 1, 400);
        scenario.moving = true;
        const auto moving = redirect_run(scenario, 1, 400);
        expect(stationary.deflected > 0 && stationary.landed == 0 && moving.deflected == 0
            && moving.landed > 0 && moving.fired_back > 0 && moving.hull < stationary.hull,
            "WRB-03/08: moving defenders launch the replacement but retain incoming damage and no block-sound event");
        for (const auto workers : {2U, 4U, 8U})
            expect(moving.hashes == redirect_run(scenario, workers, 400).hashes,
                "WRB-11: moving-defender side effects are worker independent");
    }
    {
        RedirectScenario scenario;
        scenario.block = "0.0"; scenario.redirect = "1.0"; scenario.reflect_twice = true;
        const auto twice = redirect_run(scenario, 1, 900);
        expect(twice.reflected_again > 0,
            "WRB-10: a subsequent handler uses the actual original profile despite a colliding nondeflectable slot");
        for (const auto workers : {2U, 4U, 8U})
            expect(twice.hashes == redirect_run(scenario, workers, 900).hashes,
                "WRB-11: repeated reflection is worker independent");
    }
    {
        RedirectScenario scenario; scenario.block = "0.5";
        const auto single = redirect_run(scenario, 1, 900);
        scenario.duplicate = true;
        const auto two = redirect_run(scenario, 1, 900);
        expect(two.deflected > single.deflected && two.landed < single.landed,
            "WRB-02/09: the next enabled handler receives new rolls after the first fails");
        for (const auto workers : {2U, 4U, 8U})
            expect(two.hashes == redirect_run(scenario, workers, 900).hashes,
                "WRB-11: two-handler draws are worker independent");
    }
}

} // namespace

namespace {
void test_corrupt_scatter() {
    namespace detail = tactical::detail;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    for (const auto weapon_kind : {tactical::object_weapon, 0U}) {
        tactical::CombatProfile shooter, target;
        shooter.category_bits = target.category_bits = 1;
        tactical::WeaponProfile weapon;
        weapon.hardpoint = weapon_kind; weapon.range = units(1000); weapon.pulse_count = 1;
        weapon.cone_width = weapon.cone_height = units(360);
        tactical::ShotProfile shot;
        shot.speed = units(10); shot.max_travel = units(1000); shot.inaccuracy = {{1, units(10)}};
        weapon.shot = shot; shooter.weapons = {weapon};
        tactical::CombatState state;
        state.attack_target = 2; state.direct = true; state.weapons.resize(1); state.weapons[0].pulses_left = 1;
        detail::CombatWorld world;
        world.players = owners; world.relationships = relationships; world.seed = 777; world.frame = 100;
        detail::CombatUnit self, enemy;
        self.id = 1; self.owner = self.team = 1; self.profile = &shooter; self.combat = &state;
        self.transform = math::identity_matrix();
        enemy.id = 2; enemy.owner = enemy.team = 2; enemy.visible_to = 1; enemy.profile = &target;
        const auto fire = [&](std::int64_t distance, std::optional<Fixed> radius) {
            enemy.position = enemy.previous_position = at(distance, 0);
            enemy.transform = math::to_matrix(math::identity_quat(), enemy.position).value();
            self.corrupt_inaccuracy = radius;
            world.units = {self, enemy};
            const auto step = detail::step_combat(world, world.units.front());
            expect(step && step.value().events.size() == 1, "WHE-66: both weapon routes fire a corruption test shot");
            if (!step || step.value().events.empty()) return math::Vec3{};
            const auto aim = step.value().events.front().aim;
            return math::Vec3{Fixed::from_raw(aim.x.raw() - enemy.position.x.raw()), aim.y, aim.z};
        };
        const auto near = fire(200, units(500)), far = fire(400, units(500));
        expect(near == far && near != math::Vec3{}, "WHE-66: corrupt scatter is nonzero and independent of target range");
        expect(std::abs(near.x.raw()) <= units(500).raw() && std::abs(near.y.raw()) <= units(500).raw()
            && std::abs(near.z.raw()) <= units(500).raw(), "WHE-66: all three offsets obey the authored radius");
        expect(fire(200, Fixed{}) == fire(200, std::nullopt), "WHE-66: zero corruption radius retains ordinary scatter");
    }
}

void test_corrupt_systems() {
    using Kind = tactical::AbilityKind;
    expect(tactical::ability_kind("corrupt_systems") == Kind::corrupt_systems
        && static_cast<unsigned>(Kind::corrupt_systems) == 22U, "WHE-33: corrupt systems binds its reserved kind");
    auto health = durability();
    for (auto& profile : health.profiles) profile.shield_refresh = {};
    auto movement = motion();
    auto edge_motion = *movement.find(frigate_type);
    edge_motion.type_id = 14;
    movement.profiles.push_back(edge_motion);
    std::sort(movement.profiles.begin(), movement.profiles.end(), [](const auto& left, const auto& right) { return left.type_id < right.type_id; });
    tactical::CombatTable combat;
    for (const auto type : {corvette_type, frigate_type, plain_type}) {
        tactical::CombatProfile profile;
        profile.type_id = type;
        profile.category_bits = 1;
        combat.profiles.push_back(profile);
    }
    tactical::CombatProfile edge;
    edge.type_id = 14; edge.category_bits = 1;
    edge.collision = tactical::CollisionBox{at(-10, -10, -10), at(10, 10, 10)};
    combat.profiles.push_back(edge);
    tactical::AbilityProfile cast;
    cast.kind = Kind::corrupt_systems;
    cast.recharge_frames = 1800;
    cast.recharge_at_activation = true;
    cast.effective_radius = units(400);
    cast.gui_activated_ability_name = "area";
    tactical::SpecialAbilityProfile handler;
    handler.name = "area";
    handler.kind = tactical::SpecialAbilityKind::corrupt_systems;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.corrupt_min_range = units(20);
    handler.corrupt_max_range = units(350);
    handler.corrupt_drain_per_frame = units(2);
    handler.corrupt_inaccuracy = units(500);
    handler.corrupt_duration_frames = 360;
    tactical::AbilityTable table;
    table.profiles = {{corvette_type, {cast}, false, {handler}}, {frigate_type, {turbo(), defend()}, false}};
    table.humans = {1, 2};
    auto invalid = table;
    invalid.profiles[0].special[0].corrupt_duration_frames = 0;
    expect(!tactical::validate_abilities(invalid), "WHE-34: zero status duration is refused");
    auto scene = setup();
    scene.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
        {2, frigate_type, 1, at(100, 0), math::identity_quat(), {}},
        {3, frigate_type, 2, at(100, 0), math::identity_quat(), {}},
        {4, plain_type, 2, at(100, 0), math::identity_quat(), {}},
        {5, 14, 2, at(501, 0), math::identity_quat(), {}},
        {6, frigate_type, 2, at(500, 0), math::identity_quat(), {}},
        {7, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
        {8, frigate_type, 2, at(100, 0, 1000), math::identity_quat(), {}}};
    for (std::uint64_t id = 9; id < 209; ++id)
        scene.units.push_back({id, frigate_type, 2, at(10000, static_cast<std::int64_t>(id)), math::identity_quat(), {}});
    tactical::AbilityPayload payload{Kind::corrupt_systems, tactical::AbilityAction::activate};
    payload.position = at(100, 0);
    auto minimum = payload; minimum.position = at(20, 0);
    auto beyond = payload; beyond.position = at(351, 0);
    std::vector<tactical::PlayerCommand> commands{
        command(0, 1, 0, 7, minimum), command(0, 1, 1, 7, beyond),
        command(0, 2, 0, 3, tactical::AbilityPayload{Kind::turbo, tactical::AbilityAction::activate}),
        command(1, 1, 2, 1, payload),
        command(2, 1, 3, 1, payload),
        command(2, 2, 1, 3, tactical::AbilityPayload{Kind::defend, tactical::AbilityAction::activate}),
        command(10, 1, 4, 7, payload)};
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(scene, {}, health, movement, std::nullopt, combat, {}, table);
        expect(static_cast<bool>(created), "WHE-33: area-status session creates");
        if (!created) return;
        auto session = std::move(created).value();
        for (const auto& entry : commands) expect(static_cast<bool>(session.submit(entry)), "WHE-33: valid wire payload is admitted");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick <= 371; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "WHE-33: area-status tick succeeds");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_sha256);
            const auto instances = session.snapshot()->instances();
            const auto recipient = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 3; });
            expect(recipient != instances.end(), "WHE-34: drain does not destroy its recipient");
            if (tick == 1) {
                expect(recipient->corrupt && recipient->corrupt->expires_tick == 361 && recipient->corrupt->inaccuracy == units(500),
                    "WHE-33: authored duration and accuracy are copied once");
                for (const auto& instance : instances) {
                    const bool selected = instance.entity_id == 3 || instance.entity_id == 5 || instance.entity_id == 6 || instance.entity_id == 8;
                    expect(instance.corrupt.has_value() == selected, "WHE-33: enemy locomotor and closed radius gates exclude other units");
                }
                const auto work = session.tick_work();
                expect(work.corrupt_queries == 1 && work.corrupt_recipients == 4 && work.corrupt_candidates < 10,
                    "WHE-33: remote fleet size does not expand the local recipient scan");
                expect(session.ability_state(1)->slots[0].ready_tick == 1801 && !session.ability_state(1)->slots[0].active,
                    "WHE-33: instant cast starts recharge without an active toggle");
            }
            if (tick == 2) {
                expect(session.durability_state(3)->shields == units(698), "WHE-34: 60 per second drains two shields each frame");
                const auto state = session.ability_state(3);
                expect(state->slots[0].active && !state->slots[1].active && !recipient->abilities[1].ready,
                    "WHE-65: existing mode remains active but a new mode cannot activate");
            }
            if (tick == 10) expect(recipient->corrupt && recipient->corrupt->started_tick == 10 && recipient->corrupt->expires_tick == 370,
                "WHE-33: second source overwrites one recipient effect and refreshes its lifetime");
            if (tick == 11) expect(session.durability_state(3)->shields == units(680), "WHE-34: refresh never doubles the drain");
            if (tick == 369) expect(recipient->corrupt.has_value(), "WHE-34: effect survives until its final service");
            if (tick == 370) {
                expect(!recipient->corrupt && session.durability_state(3)->shields == Fixed{}, "WHE-34: drain clamps at zero and status expires");
                const auto state = session.ability_state(3);
                expect(state->slots[0].active && state->slots[0].ready_tick == 0 && state->slots[1].ready_tick == 2170,
                    "WHE-34: only a recharged slot starts full recharge; an expiration countdown is preserved");
            }
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "WHE-33/34: state hashes match at every worker count");
        const auto recorded = session.record();
        const auto bytes = tactical::write_replay(recorded);
        const auto parsed = bytes ? tactical::parse_replay(bytes.value()) : eawr::core::Result<tactical::TacticalReplay>::failure({});
        expect(parsed && parsed.value() == recorded, "WHE-33: point and kind round-trip through replay");
        if (parsed && workers == 1) {
            auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, health, movement,
                std::nullopt, combat, {}, table);
            expect(playback.has_value(), "WHE-33: corruption replay creates");
            if (playback) {
                std::vector<std::string> replay_hashes;
                while (playback.value().completed_tick() < recorded.final_tick_count) {
                    const auto replay_tick = playback.value().step(executor);
                    if (!replay_tick) { expect(false, "WHE-33: corruption replay steps"); break; }
                    replay_hashes.push_back(replay_tick.value().state_sha256);
                }
                expect(replay_hashes == hashes, "WHE-33/34: replay reproduces activation, refresh and expiry hashes");
            }
        }
    }
}
} // namespace

// WLU-01..WLU-07: LURE. A pulse orders each hostile team with a listed craft type in reach of
// a luring craft to attack that craft; unlisted, distant and friendly teams are left alone; the
// last craft to pulse wins; pulses repeat every pulse period while on; expiry stops the lurers.
void test_lure() {
    constexpr tactical::TypeId awing = 40, tie = 41, ywing = 42, awing_team = 43, tie_team = 44, ywing_team = 45;
    tactical::LureProfile cadence;
    cadence.pulse_frames = 15;
    cadence.max_pulses = 40;
    expect(tactical::lure_pulses(cadence, 2, 2) && tactical::lure_pulses(cadence, 2, 17) && !tactical::lure_pulses(cadence, 2, 3)
            && tactical::lure_pulses(cadence, 2, 2 + 15 * 39) && !tactical::lure_pulses(cadence, 2, 2 + 15 * 40)
            && !tactical::lure_pulses(cadence, 2, 1),
        "WLU-03: a pulse at the switch-on, then every period, at most the authored count");
    cadence.pulse_frames = 0;
    cadence.max_pulses = 3;
    expect(tactical::lure_pulses(cadence, 5, 7) && !tactical::lure_pulses(cadence, 5, 8), "WLU-03: a zero period pulses every frame");

    tactical::AbilityProfile lure;
    lure.kind = tactical::AbilityKind::lure;
    lure.expiration_frames = 480;
    lure.recharge_frames = 1350;
    lure.modifiers.take_damage = decimal("0.7");
    lure.lure = tactical::LureProfile{units(350), 15, 40, {tie}, "Lure_Ability_Particle"};
    tactical::AbilityTable table;
    table.profiles = {unit_of(awing, lure)};
    expect(static_cast<bool>(tactical::validate_abilities(table)), "WLU-01: a craft's lure profile validates");
    auto broken = table;
    broken.profiles[0].abilities[0].lure->target_types = {tie, tie};
    expect(!tactical::validate_abilities(broken), "WLU-01: target types are sorted and unique");
    broken = table;
    broken.profiles[0].abilities[0].lure.reset();
    expect(!tactical::validate_abilities(broken), "WLU-01: LURE needs its pulse data");
    {
        tactical::AbilityState state = tactical::initial_abilities(table.profiles[0]);
        static_cast<void>(tactical::activate_ability(lure, state.slots[0], open_gate, 0));
        expect(tactical::ability_multiplier(table.profiles[0], state, tactical::AbilityModifier::take_damage) == decimal("0.7"),
            "WLU-08: the lurer takes 0.7 of the damage while on");
    }

    tactical::MotionTable motion;
    for (const auto type : {awing, tie, ywing}) {
        tactical::CraftProfile craft;
        craft.type_id = type;
        craft.max_speed = decimal("5.2");
        craft.min_speed = decimal("3.5");
        craft.rate_of_turn = units(4);
        craft.lift = units(4);
        craft.thrust = units(1);
        craft.roll_rate = units(5);
        craft.bank_angle = units(50);
        craft.strafe_distance = units(200);
        motion.squadrons.craft.push_back(craft);
    }
    motion.squadrons.squadrons = {
        {awing_team, {awing, awing}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(25)},
        {tie_team, {tie, tie}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(25)},
        {ywing_team, {ywing}, {at(0, 0)}, units(1000), units(200), units(300), units(25)}};
    tactical::TacticalSetup initial;
    initial.seed = 1876;
    initial.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    const auto unit = [](const eawr::sim::EntityId id, const tactical::TypeId type, const tactical::PlayerId owner,
                          const math::Vec3 position) {
        return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
    };
    initial.units = {unit(10, awing_team, 1, at(0, 0)), unit(11, awing, 1, at(0, 0)), unit(12, awing, 1, at(-10, 10)),
        unit(20, tie_team, 2, at(200, 0)), unit(21, tie, 2, at(200, 0)), unit(22, tie, 2, at(190, 10)),
        unit(30, ywing_team, 2, at(200, 60)), unit(31, ywing, 2, at(200, 60)),
        unit(40, tie_team, 2, at(2000, 0)), unit(41, tie, 2, at(2000, 0)), unit(42, tie, 2, at(1990, 10)),
        unit(50, tie_team, 1, at(100, 100)), unit(51, tie, 1, at(100, 100)), unit(52, tie, 1, at(90, 110))};
    initial.squadrons = {{10, {11, 12}}, {20, {21, 22}}, {30, {31}}, {40, {41, 42}}, {50, {51, 52}}};
    const std::vector<tactical::PlayerCommand> orders = {
        command(0, 1, 0, 10, tactical::MovePayload{at(-400, 0)}),
        command(2, 1, 1, 10, tactical::AbilityPayload{tactical::AbilityKind::lure, tactical::AbilityAction::activate}),
        command(5, 2, 0, 20, tactical::MovePayload{at(3000, 0)}),
        command(490, 2, 1, 20, tactical::MovePayload{at(3000, 0)}),
    };
    const auto order_of = [](const tactical::TacticalSession& session, const eawr::sim::EntityId id) {
        for (const auto& state : session.units()) if (state.entity_id == id) return state.order;
        return tactical::Order{};
    };
    const auto lured_by = [&](const tactical::TacticalSession& session, const eawr::sim::EntityId id, const eawr::sim::EntityId craft) {
        const auto order = order_of(session, id);
        return order.kind == tactical::OrderKind::attack && order.target == craft;
    };
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(initial, {}, {}, motion, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(created), "WLU-01: lure session is valid");
        if (!created) continue;
        auto session = std::move(created).value();
        for (const auto& order : orders) expect(static_cast<bool>(session.submit(order)), "lure command is recordable");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 520; ++tick) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "lure session step succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto a = session.ability_state(11), b = session.ability_state(12);
            // WAB-12: the countdown ends the slot 479 or 480 frames after the switch.
            if (tick < 2 + 479 || tick >= 2 + 480) {
                const bool active = tick >= 2 && tick < 2 + 479;
                expect(a && b && a->slots[0].active == active && b->slots[0].active == active,
                    "WLU-02: the squadron's command switches both craft; the 16 s limit ends it");
            }
            if (tick == 2) {
                expect(lured_by(session, 20, 12), "WLU-05/06: the in-reach enemy team attacks the last craft to pulse");
                const auto team = session.squadron_state(20);
                expect(team && team->target == 10, "WLU-05: the lured team's destination is the luring team");
                expect(order_of(session, 30).kind != tactical::OrderKind::attack, "WLU-04: an unlisted type is not lured");
                expect(order_of(session, 40).kind != tactical::OrderKind::attack, "WLU-04: a team out of reach is not lured");
                expect(order_of(session, 50).kind != tactical::OrderKind::attack, "WLU-05: a friendly team is not lured");
                const auto own = session.squadron_state(10);
                expect(own && own->mode == tactical::SquadronMode::idle && order_of(session, 10).kind == tactical::OrderKind::stop,
                    "WLU-02: switching LURE on stops the luring squadron's move");
                expect(a && a->slots[0].expires_tick != 0, "WLU-02: the switch starts the expiration countdown");
            }
            if (tick == 10) {
                expect(order_of(session, 20).kind == tactical::OrderKind::move, "WLU-06: between pulses the enemy's own order stands");
            }
            if (tick == 17) expect(lured_by(session, 20, 12), "WLU-03: the next pulse lures it again");
            if (tick == 2 + 480) {
                const auto own = session.squadron_state(10);
                expect(own && own->mode == tactical::SquadronMode::idle, "WLU-07: expiry stops the luring squadron");
                expect(a && !a->slots[0].active && a->slots[0].ready_tick > tick, "WLU-07: expiry starts the recharge");
            }
            if (tick == 515) {
                expect(order_of(session, 20).kind == tactical::OrderKind::move, "WLU-07: no pulse after the ability ends");
            }
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "lure hashes are equal for 1/2/4/8 workers");
        const auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "LURE uses the existing ability command encoding");
        if (!encoded) continue;
        const auto parsed = tactical::parse_replay(encoded.value());
        expect(parsed && parsed.value() == session.record(), "LURE command bytes round trip");
        if (!parsed) continue;
        auto playback = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, motion, std::nullopt, {}, {}, table);
        expect(static_cast<bool>(playback), "LURE replay creates a session");
        if (playback) {
            for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
                auto step = playback.value().step(executor);
                expect(step && step.value().state_sha256 == hashes[tick], "LURE replay matches every live frame");
            }
        }
    }
}

void test_space_healing() {
    tactical::DurabilityProfile health;
    health.type_id = 100; health.max_hull = units(1000); health.max_shields = units(500);
    health.destroyed_with_hardpoints = true;
    health.hardpoints = {{tactical::HardpointRole::other, true, units(100)},
        {tactical::HardpointRole::other, true, units(200)}, {tactical::HardpointRole::other, true, units(100)}};
    auto state = tactical::full_durability(health);
    state.hull = units(500); state.shields = units(200);
    state.hardpoints = {units(20), units(100), {}};
    const auto fraction = tactical::healing_fraction(health, state);
    expect(fraction == decimal("0.4"), "WSH-03: dead hardpoints do not dilute the healing-health fraction");
    const auto healed = tactical::heal_health(health, state, decimal("0.1"), units(2));
    expect(healed && healed.value() == 0 && std::abs(state.hardpoints[0].raw() - units(32).raw()) <= 64,
        "WSH-04: lowest surviving point gains percent of maximum plus amount");
    expect(state.hull == units(500) && state.shields == units(200) && state.hardpoints[2].raw() == 0,
        "WSH-04: healing a point leaves hull, shields and destroyed points alone");
    state.hardpoints = {units(100), units(200), {}};
    expect(tactical::heal_health(health, state, decimal("0.1"), {}).value() == tactical::hull_target
        && std::abs(state.hull.raw() - units(600).raw()) <= 1000, "WSH-04: no damaged surviving point falls back to hull");
    state.hull = health.max_hull;
    expect(tactical::healing_fraction(health, state) == units(1),
        "FSC-49/50: missing shields and destroyed points do not admit full surviving health");

    tactical::TacticalSetup setup;
    setup.seed = 2275;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 1, 1, tactical::player_flag_commandable},
        {3, 3, 3, tactical::player_flag_commandable}};
    setup.units = {{1, 99, 1, at(0, 0), math::identity_quat(), {}},
        {2, 100, 1, at(500, 0), math::identity_quat(), {}},
        {3, 100, 2, at(200, 0), math::identity_quat(), {}},
        {4, 100, 3, at(100, 0), math::identity_quat(), {}},
        {5, 101, 1, at(100, 0), math::identity_quat(), {}},
        {6, 100, 1, at(700, 0), math::identity_quat(), {}}};
    tactical::DurabilityTable durability;
    for (const tactical::TypeId type : {99U, 100U, 101U}) {
        tactical::DurabilityProfile profile; profile.type_id = type; profile.max_hull = units(1000);
        durability.profiles.push_back(profile);
    }
    tactical::CombatTable combat;
    for (const tactical::TypeId type : {99U, 100U, 101U}) {
        tactical::CombatProfile profile; profile.type_id = type; profile.category_bits = type == 101 ? 2 : 1;
        combat.profiles.push_back(profile);
    }
    tactical::AbilityTable abilities;
    tactical::UnitAbilityProfile owner; owner.type_id = 99;
    tactical::SpecialAbilityProfile heal; heal.name = "Repair"; heal.kind = tactical::SpecialAbilityKind::force_healing;
    heal.style = tactical::SpecialActivationStyle::space_automatic; heal.initially_enabled = true;
    heal.heal_range = units(550); heal.heal_percent = decimal("0.1"); heal.heal_interval_frames = 60;
    heal.single_target_heal = true; heal.filter.applicable_categories = 1;
    owner.special.push_back(heal); abilities.profiles.push_back(owner);
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto made = tactical::TacticalSession::create(setup, {}, durability, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(made), "automatic healer session creates"); if (!made) continue;
        auto session = std::move(made.value());
        const auto first_poll = session.ability_state(1)->special.slots[0].next_service_frame;
        for (const eawr::sim::EntityId id : {2U, 3U, 4U, 5U, 6U}) {
            expect(static_cast<bool>(session.submit(command(1, 1, id, id,
                tactical::DamagePayload{units(id == 2 ? 300 : 800), tactical::hull_target}))), "healing fixture damage submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick <= 120; ++tick) {
            const auto stepped = session.step(executor); expect(static_cast<bool>(stepped), "healing tick advances");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto events = stepped.value().snapshot->combat_events();
            const auto pulse = std::find_if(events.begin(), events.end(), [](const auto& event) {
                return event.kind == tactical::CombatEventKind::heal_pulse;
            });
            expect((pulse != events.end()) == (tick >= 1 && tick >= first_poll && (tick - first_poll) % 60 == 0),
                "WSH-06: immediate first poll, then retained full-interval pulses");
            if (pulse != events.end()) expect(pulse->target == 3, "WSH-02: most damaged ally wins over closer hostile/excluded/outside targets");
        }
        if (baseline.empty()) baseline = hashes; else expect(hashes == baseline, "healing 1/2/4/8 hashes match");
        const auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "healer replay writes"); if (!encoded) continue;
        const auto parsed = tactical::parse_replay(encoded.value());
        expect(static_cast<bool>(parsed), "healer replay encodes");
        if (parsed) {
            auto replay = tactical::TacticalSession::from_replay(parsed.value(), {}, durability, {}, std::nullopt, combat, {}, abilities);
            expect(static_cast<bool>(replay), "healer replay creates");
            if (replay) for (const auto& hash : hashes) {
                const auto step = replay.value().step(executor);
                expect(step && step.value().state_sha256 == hash, "healer deadline and health replay identically");
            }
        }
        // FSC-48/51: the adapter consumes an empty due scan, and deliberately uses
        // damage-before-healing inputs at the next due poll (WSH-07's named seam).
        auto empty_setup = setup;
        empty_setup.units.resize(2);
        auto empty = tactical::TacticalSession::create(empty_setup, {}, durability, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(empty), "empty-scan control creates");
        if (empty) {
            const auto first = empty.value().ability_state(1)->special.slots[0].next_service_frame;
            expect(static_cast<bool>(empty.value().submit(command(first + 1, 1, 1, 2,
                tactical::DamagePayload{units(200), tactical::hull_target}))), "post-empty damage submits");
            expect(static_cast<bool>(empty.value().submit(command(first + 60, 1, 2, 2,
                tactical::DamagePayload{units(200), tactical::hull_target}))), "due-frame damage submits");
            for (std::uint64_t tick = 0; tick <= first + 60; ++tick) {
                const auto step = empty.value().step(executor);
                expect(static_cast<bool>(step), "empty-scan control advances");
                if (!step) break;
                const auto events = step.value().snapshot->combat_events();
                const auto pulses = std::count_if(events.begin(), events.end(), [](const auto& event) {
                    return event.kind == tactical::CombatEventKind::heal_pulse;
                });
                expect(pulses == (tick == first + 60 ? 1 : 0),
                    "FSC-51: damage after an empty scan waits for deadline equality");
            }
            expect(std::abs(empty.value().durability_state(2)->hull.raw() - units(700).raw()) <= 1000,
                "FSC-48/WSH-07: same-frame command damage precedes the maximum-health repair pulse");
        }
    }
}

void test_peacebringer_blast_session() {
    class PhaseOrder final : public eawr::sim::PartitionExecutor {
    public:
        mutable std::vector<std::string> names;
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            names.emplace_back(phase);
            return execute(count, partition);
        }
    };
    tactical::AbilityProfile ability;
    ability.kind = tactical::AbilityKind::blast;
    ability.gui_activated_ability_name = "charged_guns";
    ability.recharge_frames = 2700;
    ability.recharge_at_activation = true;
    ability.charged_blast = tactical::ChargedBlastProfile{150, units(5), {}, {}, "charge_sound"};
    auto source = unit_of(1, ability);
    tactical::SpecialAbilityProfile nested;
    nested.name = "charged_guns"; nested.kind = tactical::SpecialAbilityKind::blast;
    nested.style = tactical::SpecialActivationStyle::user_input; nested.service_interval = 1;
    nested.charged_blast = ability.charged_blast;
    source.special = {nested};
    tactical::AbilityTable abilities; abilities.profiles = {source}; abilities.humans = {1};
    expect(static_cast<bool>(tactical::validate_abilities(abilities)), "BLAST matching nested profile validates");
    auto invalid = abilities; invalid.profiles.front().special.front().name = "another_handler";
    expect(!tactical::validate_abilities(invalid), "BLAST rejects a mismatched named handler");
    tactical::CombatProfile shooter; shooter.type_id = 1; shooter.max_attack_distance = units(1000);
    tactical::WeaponProfile gun; gun.special = true; gun.hardpoint = 0; gun.range = units(1000);
    gun.min_recharge_hundredths = gun.max_recharge_hundredths = 100;
    gun.shot.emplace(); gun.shot->speed = units(1); gun.shot->max_travel = units(1000); gun.shot->damage = units(600);
    gun.charged_shot = gun.shot; gun.charged_shot->blast = {units(400), units(250), 0, {}};
    shooter.weapons = {gun};
    gun.hardpoint = 1; gun.charged_shot.reset(); shooter.weapons.push_back(gun);
    {
        tactical::DurabilityProfile profile; profile.type_id = 1; profile.max_hull = units(100);
        profile.hardpoints = {{tactical::HardpointRole::other, true, units(100)},
            {tactical::HardpointRole::other, true, units(100)}};
        auto state = tactical::full_durability(profile);
        state.hardpoints[0] = {}; state.disabled = {false, true};
        for (const bool healthy : {false, true}) {
            auto guns = shooter;
            if (healthy) {
                profile.hardpoints.push_back({tactical::HardpointRole::other, true, units(100)});
                state.hardpoints.push_back(units(100)); state.disabled.push_back(false);
                auto third = gun; third.hardpoint = 2; guns.weapons.push_back(third);
            }
            std::optional<tactical::ChargedBlastState> charge{tactical::ChargedBlastState{1, 0, false}};
            tactical::service_charged_blast(charge, tactical::detail::eligible_blast_specials(guns, &profile, &state));
            expect(charge && charge->shots_remaining == (healthy ? 1U : 0U) && charge->charged_presentation,
                "FC-63/FSC-60: destroyed and disabled SPECIALs are excluded; only healthy guns count");
            if (!healthy) {
                tactical::service_charged_blast(charge, 0);
                expect(!charge, "FD-55: zero eligible guns publish once and clean on the next poll");
            }
        }
        // FSC-60 local constraint seam: snapshot precedes same-tick hull drag.
        profile.hardpoints.resize(2);
        state = tactical::full_durability(profile); state.hull = units(10);
        std::optional<tactical::ChargedBlastState> charge{tactical::ChargedBlastState{1, 0, false}};
        tactical::service_charged_blast(charge, tactical::detail::eligible_blast_specials(shooter, &profile, &state));
        const auto dragged = tactical::service_durability(profile, {decimal("0.2"), {}, {}}, state);
        expect(dragged && state.hardpoints[0] > units(29) && state.hardpoints[0] < units(31)
            && state.hardpoints[0] == state.hardpoints[1]
            && charge && charge->shots_remaining == 2 && charge->charged_presentation,
            "FS-34/FSC-60: hull drag follows the published two-gun snapshot and retains its budget");
    }
    tactical::CombatProfile target; target.type_id = 2;
    tactical::CombatTable combat; combat.profiles = {shooter, target};
    tactical::TacticalSetup start; start.seed = 946;
    start.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    start.units = {{1, 1, 1, at(0, 0), math::identity_quat(), {}}, {2, 2, 2, at(500, 0), math::identity_quat(), {}}};
    const std::vector<tactical::SensorProfile> sensors{{1, units(2000)}, {2, units(2000)}};
    // Projectile creation is enabled by damage rules; geometry-only recipients keep
    // both slow shots alive so their selected profiles can be inspected at completion.
    auto health = durability();
    health.profiles.clear();
    for (const bool enabled : {false, true}) {
        auto zero_guns = abilities;
        auto& profile = zero_guns.profiles.front();
        profile.abilities.front().charged_blast->charge_frames = 2;
        profile.special.front().charged_blast->charge_frames = 2;
        profile.special.front().initially_enabled = enabled;
        auto created = tactical::TacticalSession::create(start, sensors, {}, {}, std::nullopt, {}, {}, zero_guns);
        expect(static_cast<bool>(created), "FSC-58: zero-gun BLAST fixture creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(session.snapshot()->instances().front().abilities.front().ready == enabled,
            "WHE-35: HUD and existing AI readiness honor named handler admission");
        expect(static_cast<bool>(session.submit({{1, 1, 0}, {1}, tactical::AbilityPayload{
            tactical::AbilityKind::blast, tactical::AbilityAction::activate}})), "zero-gun BLAST queues");
        for (std::uint64_t frame = 0; frame < 5; ++frame) {
            PhaseOrder executor;
            auto step = session.step(executor);
            expect(static_cast<bool>(step), "zero-gun BLAST frame succeeds");
            if (!step) break;
            if (enabled && frame == 3) {
                const auto poll = std::find(executor.names.begin(), executor.names.end(), "blast-charge");
                const auto hull = std::find(executor.names.begin(), executor.names.end(), "hull-service");
                expect(poll != executor.names.end() && hull != executor.names.end() && poll < hull,
                    "FS-34/FSC-60: the completion phase runs before hull constraints in the live tick");
            }
            const auto& status = step.value().snapshot->instances().front().abilities.front();
            if (frame == 3) expect(status.blast_charged_presentation == enabled,
                "FSC-58: admitted zero-gun completion publishes exactly once; disabled initiation stays absent");
            if (frame == 4) expect(!status.blast_charged_presentation,
                "FD-55: a zero-gun budget cleans on the following admitted poll");
        }
    }
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(start, sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(created), "BLAST session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.submit({{0, 1, 0}, {1}, tactical::AbilityPayload{tactical::AbilityKind::blast,
            tactical::AbilityAction::activate}})), "BLAST activation queues through the existing opcode");
        expect(static_cast<bool>(session.submit({{1, 1, 1}, {1}, tactical::AttackPayload{2}})), "BLAST attack queues after charge initiation");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t frame = 0; frame < 154; ++frame) {
            auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "BLAST frame succeeds");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256);
            const auto& snapshot = *stepped.value().snapshot;
            const auto& status = snapshot.instances().front().abilities.front();
            if (frame < 150) expect(status.blast_charge_remaining == 150 - frame && !status.blast_charged_presentation,
                "FC-62: late-command activation completes after exactly 150 subsequent admitted polls");
            if (frame == 150) {
                expect(status.blast_charged_presentation && status.blast_charge_remaining == 0,
                    "FD-55: completion and shot consumption retain charged presentation until the next poll");
                expect(snapshot.projectiles().size() == 2, "FSC-56: both eligible SPECIAL guns fire on the completion frame");
                expect(std::count_if(snapshot.combat_events().begin(), snapshot.combat_events().end(), [](const auto& event) {
                    return event.kind == tactical::CombatEventKind::weapon_fired && (event.outcome & tactical::fired_charged_shot) != 0;
                }) == 2, "FD-54: completion consumes both successful charged fire attempts");
                for (const auto& projectile : snapshot.projectiles()) {
                    expect(projectile.damage == units(3000) && projectile.profile_key
                        && projectile.profile_key->kind == tactical::ProjectileProfileKind::charged_blast,
                        "WAD-35/36: ordinary and override SPECIAL shots retain kind 4 and direct multiplier 5");
                    expect(projectile.blast.damage == (projectile.weapon == 0 ? units(400) : Fixed{}),
                        "WAD-36: charged instance scaling leaves the selected type's area budget unchanged");
                }
            }
            if (frame == 151) expect(!status.blast_charged_presentation, "FD-55: next admitted poll clears exhausted presentation");
            expect(!status.active && !status.ready, "BLAST initiation is instantaneous and its 90-second recharge remains running");
        }
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "BLAST live hashes match for 1/2/4/8 workers");
        auto encoded = tactical::write_replay(session.record());
        expect(static_cast<bool>(encoded), "BLAST replay encodes");
        if (!encoded) continue;
        auto parsed = tactical::parse_replay(encoded.value());
        expect(parsed && parsed.value() == session.record(), "BLAST replay round trips without a new command opcode");
        if (!parsed) continue;
        auto replay = tactical::TacticalSession::from_replay(parsed.value(), sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(replay), "BLAST replay creates");
        if (replay) for (const auto& hash : hashes) {
            auto step = replay.value().step(executor);
            expect(step && step.value().state_sha256 == hash, "BLAST replay matches every live frame");
        }
    }
}

void test_replenished_radar_creation() {
    constexpr tactical::TypeId escort = 23;
    for (const auto source_type : {escort, xwing_squadron}) for (const bool enabled : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto initial = setup();
            initial.units = {{11, xwing_type, 1, at(0, 0), math::identity_quat(), {}}};
            auto movement = hangar_motion();
            movement.squadrons.spawners.clear();
            auto wing = movement.squadrons.craft.front(); wing.type_id = escort;
            movement.squadrons.craft.push_back(wing);
            movement.squadrons.squadrons.front().members = {xwing_type, escort};
            auto health = hangar_durability();
            auto escort_health = health.profiles.back(); escort_health.type_id = escort;
            health.profiles.push_back(escort_health);
            tactical::SpecialAbilityProfile radar;
            radar.name = "Replenished_Radar";
            radar.kind = tactical::SpecialAbilityKind::enable_radar;
            radar.style = tactical::SpecialActivationStyle::skirmish_automatic;
            radar.initially_enabled = enabled;
            // A full creation notification would unlock this even in the disabled case.
            for (auto& row : health.profiles) if (row.type_id == source_type) {
                tactical::HardpointProfile point;
                point.destroyable = true; point.max_health = units(100); point.special_ability_name = radar.name;
                row.hardpoints.push_back(point);
            }
            tactical::AbilityProfile fill;
            fill.kind = tactical::AbilityKind::replenish_wingmen;
            fill.replenish_team = xwing_squadron; fill.recharge_frames = 6;
            tactical::UnitAbilityProfile leader; leader.type_id = xwing_type; leader.abilities = {fill};
            tactical::UnitAbilityProfile source; source.type_id = source_type; source.special = {radar};
            tactical::AbilityTable abilities; abilities.profiles = {leader, source};
            std::sort(abilities.profiles.begin(), abilities.profiles.end(),
                [](const auto& a, const auto& b) { return a.type_id < b.type_id; });
            auto made = tactical::TacticalSession::create(initial, {}, health, movement, std::nullopt, {}, {}, abilities);
            expect(static_cast<bool>(made), "FC-75: replenish radar fixture creates");
            if (!made) { std::cerr << made.error().message << '\n'; continue; }
            auto world = std::move(made).value();
            expect(!world.snapshot()->players().front().radar_enabled, "FC-75: no replenish radar source before birth");
            expect(static_cast<bool>(world.submit(command(0, 1, 0, 11,
                tactical::AbilityPayload{tactical::AbilityKind::replenish_wingmen, tactical::AbilityAction::activate}))),
                "FC-75: replenish radar submits");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 3; ++tick) {
                auto result = world.step(executor);
                expect(static_cast<bool>(result), "FC-75: replenish radar steps");
                if (!result) { std::cerr << result.error().message << '\n'; break; }
                expect(world.squadrons().size() == 1 && world.squadrons().front().members.size() == 2,
                    "FC-75: replenish creates escort and container");
                expect(world.snapshot()->players().front().radar_enabled == enabled,
                    "FC-75: newly replenished source publishes without changing its named gate");
                hashes.push_back(result.value().state_sha256 + result.value().snapshot->sha256());
            }
            if (workers == 1) reference = hashes;
            else expect(hashes == reference, "FC-75: replenish radar hashes and snapshots equal on 1/2/4/8 workers");
        }
    }
}

void test_radar_lifecycle_work_budget() {
    class RadarWork final : public eawr::sim::PartitionExecutor {
    public:
        mutable std::size_t phases{};
        bool fail{};
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return eawr::sim::InlineExecutor{}.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view name, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (name == "named-ability-radar") {
                ++phases;
                if (fail) return eawr::core::Result<void>::failure({});
            }
            return execute(count, partition);
        }
    } executor;
    tactical::SpecialAbilityProfile radar;
    radar.name = "Comm_Radar"; radar.kind = tactical::SpecialAbilityKind::enable_radar;
    radar.style = tactical::SpecialActivationStyle::skirmish_automatic;
    radar.initially_enabled = false;
    tactical::UnitAbilityProfile profile; profile.type_id = corvette_type; profile.special = {radar};
    tactical::AbilityTable table; table.profiles = {profile};
    auto start = setup();
    start.units = {{1, plain_type, 1, at(0, 0), math::identity_quat(), {}}};
    auto health = durability();
    health.profiles[0].max_shields = {}; health.profiles[0].shield_refresh = {};
    health.profiles[0].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
        {tactical::HardpointRole::other, true, units(500)}};
    health.profiles[0].hardpoints[0].special_ability_name = radar.name;
    auto made = tactical::TacticalSession::create(start, {}, health, {}, {}, {}, {}, table);
    expect(static_cast<bool>(made), "FC-75: radar-free battle with a radar table creates");
    if (!made) return;
    auto world = std::move(made).value();
    for (unsigned tick = 0; tick < 4; ++tick)
        expect(static_cast<bool>(world.step(executor)), "FC-75: radar-free tick succeeds");
    expect(executor.phases == 0, "FC-75: no radar station means zero radar work despite table handlers");
    const auto born = world.stage_spawn({0, corvette_type, 1, at(50, 0), math::identity_quat(), {}});
    expect(static_cast<bool>(born), "FC-75: radar station stages");
    if (!born) return;
    world.scramble_storage_for_testing();
    for (unsigned tick = 0; tick < 4; ++tick)
        expect(static_cast<bool>(world.step(executor)), "FC-75: steady radar tick succeeds");
    expect(executor.phases == 0 && world.snapshot()->players().front().radar_enabled,
        "FC-75: enabled radar has no recurring handler poll after storage rebuild");
    expect(static_cast<bool>(world.submit(command(world.completed_tick(), 1, 0, born.value(),
        tactical::DamagePayload{units(50), 0}))), "FC-75: radar lock change submits");
    const auto before = world.canonical_state_bytes();
    executor.fail = true;
    expect(!world.step(executor) && world.canonical_state_bytes() == before
        && world.snapshot()->players().front().radar_enabled,
        "FC-75: failed radar publication preserves canonical state and cached permission");
    executor.fail = false;
    expect(static_cast<bool>(world.step(executor)) && executor.phases == 2
        && !world.snapshot()->players().front().radar_enabled,
        "FC-75: retry publishes the lock change once in the admitted tick");
    expect(static_cast<bool>(world.step(executor)) && executor.phases == 2,
        "FC-75: unchanged disabled radar performs no further work");
    expect(static_cast<bool>(world.stage_remove(born.value())), "FC-75: radar station removes");
    expect(static_cast<bool>(world.step(executor)) && executor.phases == 2,
        "FC-75: removing the last radar source restores zero radar work");
}

void test_station_radar_and_named_locks() {
    tactical::SpecialAbilityProfile radar;
    radar.name = "Comm_Radar"; radar.kind = tactical::SpecialAbilityKind::enable_radar;
    radar.style = tactical::SpecialActivationStyle::skirmish_automatic;
    radar.initially_enabled = false; radar.affects_all_allies = true;
    tactical::UnitAbilityProfile profile; profile.type_id = corvette_type; profile.special = {radar};
    auto other = radar; other.name = "other"; other.initially_enabled = true;
    other.style = tactical::SpecialActivationStyle::user_input;
    profile.special.push_back(other);
    tactical::NamedAbilityLocks locks;
    expect(!tactical::set_named_ability_lock(locks, 1, profile, "absent", true) && locks.empty(),
        "WHD-05: absent names create no retained lock");
    expect(!tactical::named_ability_enabled(locks, 1, corvette_type, radar), "WRAD-04: authored radar starts disabled");
    expect(tactical::set_named_ability_lock(locks, 1, profile, "COMM_RADAR", false), "WHD-05: first explicit unlock writes");
    expect(!tactical::set_named_ability_lock(locks, 1, profile, "comm_radar", false) && locks.size() == 1,
        "WHD-05: case-insensitive repeated writes are idempotent");
    expect(tactical::set_named_ability_lock(locks, 1, profile, "Comm_Radar", true)
        && !tactical::named_ability_enabled(locks, 1, corvette_type, radar)
        && tactical::named_ability_enabled(locks, 1, corvette_type, other),
        "WHD-05: opposite write replaces the boolean without locking an unrelated name");

    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto health = durability();
        for (auto& row : health.profiles) {
            row.max_shields = {}; row.shield_refresh = {};
            row.hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                {tactical::HardpointRole::other, true, units(500)}};
            row.hardpoints[0].special_ability_name = radar.name;
        }
        tactical::AbilityTable table; table.profiles = {profile}; table.humans = {1, 2, 3};
        auto different = profile; different.type_id = frigate_type; table.profiles.push_back(different);
        auto start = setup();
        start.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 1, 2, tactical::player_flag_commandable},
            {3, 3, 3, tactical::player_flag_commandable}};
        start.departure_policy = tactical::MultiplayerDeparturePolicy{{1, 2, 3}};
        start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
            {2, corvette_type, 1, at(50, 0), math::identity_quat(), {}},
            {3, corvette_type, 2, at(100, 0), math::identity_quat(), {}},
            {4, frigate_type, 1, at(150, 0), math::identity_quat(), {}},
            {5, plain_type, 3, at(5000, 0), math::identity_quat(), {}}};
        const std::vector<tactical::SensorProfile> sensors{{corvette_type, units(500)}, {frigate_type, units(500)}};
        tactical::FogRules fog; fog.map_left = units(-2000); fog.map_top = units(2000);
        fog.cell_size = units(100); fog.cells_wide = 80; fog.cells_tall = 40;
        fog.service_period = 1; fog.ramp_down_step = 238;
        auto created = tactical::TacticalSession::create(start, sensors, health, {}, fog, {}, {}, table);
        expect(static_cast<bool>(created), "WRAD: station radar fixture creates");
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto world = std::move(created).value();
        const auto enabled = [&](const eawr::sim::EntityId id) {
            const auto state = world.ability_state(id);
            return state && state->special.slots.front().enabled;
        };
        const auto permission = [&](const tactical::PlayerId owner) {
            for (const auto& player : world.snapshot()->players()) if (player.player_id == owner) return player.radar_enabled;
            return false;
        };
        const auto bytes_have_lock = [&] {
            const auto bytes = world.canonical_state_bytes();
            constexpr std::array<std::uint8_t, 4> tag{'N','L','C','K'};
            return std::search(bytes.begin(), bytes.end(), tag.begin(), tag.end()) != bytes.end();
        };
        expect(enabled(1) && enabled(2) && enabled(3) && enabled(4) && permission(1) && permission(2) && !permission(3),
            "WRAD-01/04: creation unlocks named radar for owner and allies only");
        expect(world.snapshot()->instances().back().visible_to == 4, "WRAD-01: radar does not reveal the distant enemy");
        expect(world.fog_cells() && !world.fog_cells()->revealed(0, start.units.back().position),
            "WRAD-01: world fog cells stay hidden with allied radar enabled");
        expect(bytes_have_lock(), "WHD-05: explicit creation unlock is canonical NLCK state");
        expect(static_cast<bool>(world.submit(command(0, 1, 0, 1, tactical::DamagePayload{units(50), 0}))), "radar loss submits");
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto step = [&] {
            const auto result = world.step(executor);
            expect(static_cast<bool>(result), "WRAD: radar lifecycle tick succeeds");
            if (result) hashes.push_back(result.value().state_sha256);
        };
        step();
        expect(!enabled(1) && !enabled(2) && enabled(3) && enabled(4),
            "WHD-05: one comm-array loss locks intact same-owner/type instances and isolates owner/type");
        expect(world.ability_state(2)->special.slots[1].enabled && permission(1),
            "WRAD-03: unrelated named handler and another allied radar source survive");
        expect(static_cast<bool>(world.stage_remove(3)) && static_cast<bool>(world.stage_remove(4)), "other radar sources remove");
        step();
        expect(!permission(1) && !permission(2) && !enabled(2) && bytes_have_lock(),
            "WRAD-03/WHD-05: removal clears source contributions without forgetting retained locks");
        auto spawn = start.units.front(); spawn.entity_id = 0;
        const auto born = world.stage_spawn(spawn);
        expect(static_cast<bool>(born), "new station stages");
        step();
        expect(enabled(2) && permission(1) && permission(2), "WHD-05: fresh intact comm array unlocks existing same-type holders");
        if (born) expect(static_cast<bool>(world.stage_remove(born.value())), "fresh station removes");
        step();
        expect(enabled(2) && permission(1), "WHD-05: explicit unlock persists beyond its originating object");
        expect(static_cast<bool>(world.submit(command(world.completed_tick(), 1, 1, 2,
            tactical::DamagePayload{units(50), 0}))), "second loss submits");
        step();
        expect(!enabled(2) && !permission(1), "WHD-05: next opposite notification replaces the unlock");
        expect(static_cast<bool>(world.submit({{world.completed_tick(), 1, 2}, {}, tactical::QuitPayload{}})), "ownership transfer submits");
        step();
        expect(world.units().front().owner == 2 && enabled(2) && permission(2),
            "WHD-05: transferred holders read the new owner's retained lock, not the old owner's lock");
        expect(world.snapshot()->instances().back().visible_to == 4, "WRAD-01: lifecycle changes still do not reveal world fog");
        expect(world.fog_cells() && !world.fog_cells()->revealed(0, start.units.back().position),
            "WRAD-01: loss, restoration, removal and transfer never reveal the distant fog cells");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WRAD/WHD-05: lifecycle and retained state equal on 1/2/4/8 workers");
    }
}

void test_hardpoint_dependencies() {
    using K = tactical::AbilityKind;
    for (const auto kind : {K::blast, K::leech_shields, K::tractor_beam}) {
      for (const bool activation_tick_loss : {false, true}) {
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto health = durability();
            for (auto& row : health.profiles) { row.max_shields = {}; row.shield_refresh = {}; }
            health.profiles[0].hardpoints = {{tactical::HardpointRole::other, true, units(50)},
                {tactical::HardpointRole::other, true, units(50)}, {tactical::HardpointRole::other, true, units(500)}};
            health.profiles[1].max_shields = units(1000);
            tactical::AbilityProfile ability;
            ability.kind = kind; ability.requires_hardpoint = true; ability.required_hardpoints = {0, 1};
            ability.gui_activated_ability_name = "dependency"; ability.recharge_frames = 10;
            ability.expiration_frames = 100; ability.recharge_at_activation = kind != K::tractor_beam;
            tactical::SpecialAbilityProfile nested;
            nested.name = "dependency"; nested.style = tactical::SpecialActivationStyle::user_input;
            nested.kind = kind == K::blast ? tactical::SpecialAbilityKind::blast
                : kind == K::leech_shields ? tactical::SpecialAbilityKind::leech_shields : tactical::SpecialAbilityKind::tractor_beam;
            nested.service_interval = 1; nested.beam_max_range = units(1000); nested.filter.applicable_categories = 2;
            nested.leech_duration_frames = 100; nested.damage_per_frame = units(1);
            nested.leech_damage_bonus = units(3);
            if (kind == K::blast) {
                ability.expiration_frames = 0;
                ability.charged_blast = tactical::ChargedBlastProfile{8, units(5), {}, {}, {}};
                nested.charged_blast = ability.charged_blast;
            }
            auto holder = unit_of(corvette_type, ability); holder.special = {nested};
            tactical::AbilityTable table; table.profiles = {holder}; table.humans = {1, 2};
            tactical::CombatProfile shooter; shooter.type_id = corvette_type; shooter.category_bits = 1;
            tactical::CombatProfile target; target.type_id = frigate_type; target.category_bits = 2;
            if (kind == K::blast) {
                tactical::WeaponProfile gun; gun.special = true; gun.hardpoint = 0;
                shooter.weapons = {gun}; gun.hardpoint = 1; shooter.weapons.push_back(gun);
            }
            tactical::CombatTable combat; combat.profiles = {shooter, target};
            auto start = setup();
            start.units = {{1, corvette_type, 1, at(0, 0), math::identity_quat(), {}},
                {2, frigate_type, 2, at(100, 0), math::identity_quat(), {}}};
            auto created = tactical::TacticalSession::create(start, {}, health, motion(), std::nullopt, combat, {}, table);
            expect(static_cast<bool>(created), "WHD: dependency session creates");
            if (!created) { std::cerr << created.error().message << '\n'; continue; }
            auto world = std::move(created).value();
            std::uint64_t sequence{};
            const auto submit = [&](const std::uint64_t tick, tactical::CommandPayload payload) {
                expect(static_cast<bool>(world.submit(command(tick, 1, sequence++, 1, std::move(payload)))), "WHD: command submits");
            };
            submit(0, tactical::AbilityPayload{kind, tactical::AbilityAction::activate, kind == K::blast ? 0U : 2U});
            submit(activation_tick_loss ? 0 : 2, tactical::DamagePayload{units(50), 0});
            submit(activation_tick_loss ? 0 : 4, tactical::DamagePayload{units(50), 1});
            submit(20, tactical::AbilityPayload{kind, tactical::AbilityAction::activate, kind == K::blast ? 0U : 2U});
            eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 0; frame < 22; ++frame) {
                const auto step = world.step(executor);
                expect(static_cast<bool>(step), "WHD: dependency tick succeeds"); if (!step) break;
                hashes.push_back(step.value().state_sha256);
                const auto& status = step.value().snapshot->instances().front().abilities.front();
                if (frame == 0) {
                    expect(kind == K::blast ? world.ability_state(1)->charged_blast.has_value()
                        : status.active == !activation_tick_loss,
                        "WHD-02/03: activation-tick loss releases beams but preserves an admitted BLAST charge");
                    expect(std::none_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(), [](const auto& event) {
                        return event.kind == tactical::EventKind::order_rejected;
                    }), "WHD-01: activation is admitted before later damage commands in the same tick");
                }
                if (!activation_tick_loss && frame == 3) {
                    const auto state = world.durability_state(1);
                    tactical::AbilityGate gate; gate.hardpoint_profile = &health.profiles[0];
                    gate.hardpoint_state = state ? &*state : nullptr;
                    expect(tactical::ability_hardpoints_online(ability, gate),
                        "WHD-01: first loss leaves the second dependency available");
                }
                if (frame >= (activation_tick_loss ? 0U : 5U))
                    expect(!status.ready, "WHD-01: last loss greys HUD even after recharge");
                if (kind != K::blast && frame == 5) expect(!status.active && world.ability_state(1)->slots.front().target == 0,
                    "WHD-02: lost beam dependency releases active target");
                if (kind == K::blast && frame == 5) expect(status.blast_charge_remaining == 3,
                    "WHD-03: destruction does not cancel an in-progress charge");
                if (kind == K::blast && frame == 8) expect(status.blast_charged_presentation
                    && world.ability_state(1)->charged_blast->shots_remaining == 0,
                    "WHD-03: completion counts zero surviving SPECIALs");
                if (kind == K::blast && frame == 9) expect(!status.blast_charged_presentation,
                    "WHD-03: zero budget cleans on the next poll");
                if (frame == 20) expect(std::any_of(step.value().snapshot->events().begin(), step.value().snapshot->events().end(), [](const auto& event) {
                    return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::ability_unavailable;
                }), "WHD-01: lost dependency refuses with ability_unavailable");
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "WHD: dependency hashes equal at 1/2/4/8 workers");
        }
      }
    }
    tactical::DurabilityProfile health; health.hardpoints = {{tactical::HardpointRole::other, false, {}}};
    auto state = tactical::full_durability(health);
    tactical::AbilityProfile ability; ability.kind = K::blast; ability.requires_hardpoint = true; ability.required_hardpoints = {0};
    tactical::AbilityGate gate; gate.hardpoint_profile = &health; gate.hardpoint_state = &state;
    expect(tactical::ability_ready(ability, {}, gate, 0), "WHD-01: indestructible dependency remains available at zero health");
    state.disabled = {true};
    expect(!tactical::ability_ready(ability, {}, gate, 0), "WHD-01: disabled dependency is excluded");
    ability.required_hardpoints.clear();
    expect(!tactical::ability_ready(ability, {}, gate, 0), "WHD-01: absent required type is unavailable");
    ability.requires_hardpoint = false; ability.kind = K::energy_weapon;
    expect(tactical::ability_ready(ability, {}, gate, 0), "WHD-01: visual energy-weapon bone creates no dependency");
    gate.ion_stunned = true;
    for (const auto kind : {K::tractor_beam, K::energy_weapon}) {
        ability.kind = kind;
        ability.requires_hardpoint = kind == K::tractor_beam;
        ability.required_hardpoints = {0};
        state.disabled = {false};
        expect(tactical::ability_ready(ability, {}, gate, 0),
            "WHD-07: ion stun does not inherit ordinary-stun tractor/energy refusal");
    }
}

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--stealth") {
        test_stealth_session();
        test_stealth_duration_modifiers();
        test_squadron_stealth();
        test_squadron_stealth_orders();
        if (failures != 0) return EXIT_FAILURE;
        std::cout << "stealth contracts accepted\n";
        return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--reveal-ping") {
        test_squadron_reveal_ping();
        test_cluster_bomb_and_reveal_ping();
        if (failures != 0) return EXIT_FAILURE;
        std::cout << "reveal ping contracts accepted\n";
        return EXIT_SUCCESS;
    }
    test_station_radar_and_named_locks();
    test_radar_lifecycle_work_budget();
    test_replenished_radar_creation();
    test_hardpoint_dependencies();
    test_space_healing();
    test_peacebringer_blast_session();
    test_deployment_abilities();
    test_lure();
    test_self_destruct_projectile_order();
    test_self_destruct();
    test_corrupt_scatter();
    test_corrupt_systems();
    test_full_salvo();
    test_stealth_session();
    test_stealth_duration_modifiers();
    test_squadron_stealth();
    test_squadron_stealth_orders();
    test_leech_shields();
    test_redirect_blaster();
    test_targeted_autofire();
    test_laser_defence();
    test_projectile_defence_producers();
    test_projectile_defence_transaction();
    test_impact_shooter_bonus();
    test_hero_environment_damage_routing();
    test_hero_damage_after_escorts_die();
    test_hero_wingmen();
    test_spawned_hero_abilities();
    test_buzz_team_request();
    test_buzz_droids();
    test_hostile_buzz_droids();
    test_hero_beams();
    test_beam_autofire();
    test_concentrate_autofire();
    test_arriving_beam_autofire();
    test_banked_ability_geometry();
    test_hunt_destinations();
    test_hunt_ship_order();
    test_hunt_session();
    test_names_and_validation();
    test_hero_damage_modes();
    test_nested_special_handlers();
    test_concentrate_fire();
    test_concentrate_damage();
    test_concentrate_sparse_lifecycle();
    test_timers();
    test_multipliers();
    test_turbo_session();
    test_expiry_snapshot_keeps_timed_duration();
    test_defend_stand_in();
    test_autofire_rules();
    test_rejections();
    test_launched_squadron_sfoils();
    test_created_squadron_autofire();
    test_barrage_proxy_and_override();
    test_cluster_bomb_and_reveal_ping();
    test_squadron_reveal_ping();
    test_mc30_object_script();
    test_mc30_proximity_sample();
    if (failures != 0) {
        std::cerr << failures << " ability contract failure(s)\n";
        return 1;
    }
    std::cout << "ability contracts passed\n";
    return 0;
}
