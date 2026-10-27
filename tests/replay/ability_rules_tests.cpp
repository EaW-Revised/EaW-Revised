#include "ability_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ability_tests {

namespace {

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

} // namespace


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
    // AB-22/AB-23 (#4868): an interval multiplier of 0 loads; a negative one and a too-large one still do not.
    auto interval_zero = table;
    interval_zero.profiles[0].abilities[0].modifiers.shield_regen_interval = Fixed{};
    interval_zero.profiles[0].abilities[0].modifiers.energy_regen_interval = Fixed{};
    expect(static_cast<bool>(tactical::validate_abilities(interval_zero)), "a zero regen interval multiplier is accepted");
    auto interval_negative = table;
    interval_negative.profiles[0].abilities[0].modifiers.shield_regen_interval = Fixed::from_raw(-1);
    expect(!tactical::validate_abilities(interval_negative), "a negative shield interval multiplier is rejected");
    interval_negative.profiles[0].abilities[0].modifiers.shield_regen_interval = Fixed{};
    interval_negative.profiles[0].abilities[0].modifiers.energy_regen_interval = Fixed::from_raw(-1);
    expect(!tactical::validate_abilities(interval_negative), "a negative energy interval multiplier is rejected");
    auto interval_huge = table;
    interval_huge.profiles[0].abilities[0].modifiers.energy_regen_interval = units(65);
    expect(!tactical::validate_abilities(interval_huge), "an interval multiplier beyond 64 is rejected");
    expect(tactical::scaled_interval(90, Fixed{}) == 1, "AB-22: a zero interval multiplier floors at one frame");
    // WAB-70 (#4868): the buzz activation range has no ceiling in the debug build; a negative one is still refused.
    tactical::AbilityProfile buzz_ability;
    buzz_ability.kind = tactical::AbilityKind::buzz_droids; buzz_ability.recharge_frames = 60;
    buzz_ability.buzz = tactical::BuzzDroidsProfile{0x2008, units(0), units(500000), units(5000), units(5000),
        units(8), units(1), units(20), 30, 2, 0};
    tactical::AbilityTable buzz_table; buzz_table.profiles = {unit_of(7, buzz_ability)};
    expect(static_cast<bool>(tactical::validate_abilities(buzz_table)), "WAB-70: a 500000 buzz activation range is accepted");
    auto buzz_negative = buzz_table;
    buzz_negative.profiles[0].abilities[0].buzz->max_range = Fixed::from_raw(-1);
    expect(!tactical::validate_abilities(buzz_negative), "WAB-70: a negative buzz maximum range is rejected");
    buzz_negative = buzz_table;
    buzz_negative.profiles[0].abilities[0].buzz->min_range = Fixed::from_raw(-1);
    expect(!tactical::validate_abilities(buzz_negative), "WAB-70: a negative buzz minimum range is rejected");
    buzz_negative = buzz_table;
    buzz_negative.profiles[0].abilities[0].buzz->chase_radius = units(65537);
    expect(!tactical::validate_abilities(buzz_negative), "WAB-70: the chase radius keeps its 65536 bound");
    // WSH-09 (#4868): a negative automatic Heal_Amount is passive damage; it loads, but only within the health bound.
    tactical::SpecialAbilityProfile aura; aura.name = "Passive_Damage"; aura.kind = tactical::SpecialAbilityKind::force_healing;
    aura.style = tactical::SpecialActivationStyle::space_automatic; aura.initially_enabled = true;
    aura.heal_range = units(1000); aura.heal_amount = units(-30); aura.heal_interval_frames = 15;
    auto aura_table = table;
    aura_table.profiles[0].special.push_back(aura);
    expect(static_cast<bool>(tactical::validate_abilities(aura_table)), "WSH-09: a negative heal amount is accepted");
    aura_table.profiles[0].special.back().heal_amount = Fixed::from_raw(-(tactical::max_hit_damage + 1) * Fixed::scale);
    expect(!tactical::validate_abilities(aura_table), "WSH-09: a negative heal amount beyond the health bound is rejected");
    aura_table.profiles[0].special.back().heal_amount = units(-30);
    aura_table.profiles[0].special.back().heal_percent = Fixed::from_raw(-1);
    expect(!tactical::validate_abilities(aura_table), "WSH-09: a negative heal percent stays rejected");
    aura_table.profiles[0].special.back().heal_percent = Fixed::from_raw(Fixed::scale / 10);
    expect(!tactical::validate_abilities(aura_table), "WSH-09: a negative amount with a positive percent is rejected");
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

} // namespace ability_tests
