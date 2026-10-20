#include "combat_support.hpp"
#include "../../src/sim/tactical/combat_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace combat_test_support {

void test_zero_cone() {
    // W-07: an unauthored (zero) Fire_Cone_Width/Height on a fixed hardpoint accepts only a point
    // exactly dead ahead of the weapon midpoint, as FoC's half-width comparison does.
    auto zero = table();
    zero.profiles[0].weapons[0].cone_width = Fixed{};
    zero.profiles[0].weapons[0].cone_height = Fixed{};
    auto ahead = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0), true)}), zero);
    const auto shots = run(ahead, 150);
    expect(std::any_of(shots.begin(), shots.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == 2;
    }), "zero cone: a target dead ahead is fired at");
    auto off_axis = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 20), true)}), zero);
    expect(run(off_axis, 150).empty(), "zero cone: a target off the axis is never fired at");
    auto behind = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}), zero);
    expect(run(behind, 150).empty(), "zero cone: a target behind is never fired at");
}

void test_fire_bone_cone() {
    // W-07 (#361): a fixed hardpoint's cone opens along Fire_Bone_A's x axis, not the unit's.
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    auto aft = table();
    aft.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(-1, 0), at(0, -1), at(0, 0, 1)};
    auto astern = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}), aft);
    expect(fired_at(run(astern, 150), 2), "an aft-facing fire bone fires at a target astern");
    auto ahead = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0), true)}), aft);
    expect(run(ahead, 150).empty(), "an aft-facing fire bone does not fire ahead");
    // A port broadside (x toward +Y) with FoC's 175-degree side cone reaches aft of the beam, not dead astern.
    auto broadside = table();
    broadside.profiles[0].weapons[0].cone_width = units(175);
    broadside.profiles[0].weapons[0].fire_axes =
        std::array<math::Vec3, 3>{at(0, 1), at(-1, 0), at(0, 0, 1)};
    auto quarter = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 50), true)}),
        broadside);
    expect(fired_at(run(quarter, 150), 2), "a 175-degree broadside fires at a target on its quarter");
    auto dead_astern = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}),
        broadside);
    expect(run(dead_astern, 150).empty(), "a 175-degree broadside does not reach dead astern");
}

void test_launcher_cone() {
    // W-07, W-12 (#516): a missile or torpedo hardpoint passes the same cone test as a laser. The
    // Acclamator's torpedo launcher (130 x 130 along the bow) reaches 65 degrees either side of
    // the bow and never its beam; the Empire station's missile battery (360 x 360) reaches every
    // direction. Targets 300 units from the weapon midpoint (5, 0).
    const auto shoot = [](const std::int64_t width, const math::Vec3 at_point) {
        auto launcher = table();
        launcher.profiles[0].weapons[0].cone_width = units(width);
        launcher.profiles[0].weapons[0].cone_height = units(width);
        launcher.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 1), at(0, 0, 1)};
        auto value =
            session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at_point, true)}), launcher);
        const auto shots = run(value, 150);
        return std::any_of(shots.begin(), shots.end(), [](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == 2;
        });
    };
    expect(shoot(130, at(155, 260)), "a 130-degree launcher fires 60 degrees off the bow");
    expect(!shoot(130, at(108, 282)), "a 130-degree launcher does not fire 70 degrees off the bow");
    expect(!shoot(130, at(5, 300)), "a 130-degree launcher does not fire on the beam");
    expect(!shoot(130, at(-295, 0)), "a 130-degree launcher does not fire astern");
    expect(shoot(360, at(5, 300)), "a 360-degree battery fires on the beam");
    expect(shoot(360, at(-295, 0)), "a 360-degree battery fires astern");
    expect(shoot(180, at(5, 300)), "a 180-degree battery reaches its beam");

    // W-07, WAD-37: asymmetric cones keep yaw and pitch independent, also on SPECIAL.
    // Positions are relative to the midpoint (5, 0); the 30/60-degree pairs are rounded.
    for (const bool special : {false, true}) {
        const auto asymmetric = [&](const math::Vec3 position) {
            auto content = table();
            auto& gun = content.profiles.front().weapons.front();
            gun.special = special;
            gun.cone_width = units(special ? 40 : 130);
            gun.cone_height = units(special ? 180 : 30);
            gun.fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 1), at(0, 0, 1)};
            gun.shot.emplace(); gun.shot->speed = units(25); gun.shot->damage = units(1);
            auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)),
                unit(2, bomber_type, 2, position, true)}), content);
            expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                "asymmetric cone receives a directed attack");
            const auto events = run(value, 150);
            return std::any_of(events.begin(), events.end(), [](const Shot& shot) {
                return shot.kind == tactical::CombatEventKind::weapon_fired && shot.shooter == 1;
            });
        };
        expect(asymmetric(at(305, 0)), "asymmetric cone fires forward");
        expect(asymmetric(at(265, 150)) == !special, "asymmetric cone at horizontal 30 degrees");
        expect(asymmetric(at(155, 260)) == !special, "asymmetric cone at horizontal 60 degrees");
        expect(!asymmetric(at(5, 300)), "asymmetric cone rejects horizontal 90 degrees");
        expect(asymmetric(at(265, 0, 150)) == special, "asymmetric cone at vertical 30 degrees");
        expect(asymmetric(at(155, 0, 260)) == special, "asymmetric cone at vertical 60 degrees");
        expect(asymmetric(at(5, 0, 300)) == special, "asymmetric cone at vertical 90 degrees");
    }
}

void test_object_turret() {
    tactical::detail::CombatWorld world;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    world.players = owners;
    world.relationships = relationships;
    tactical::CombatProfile source, victim;
    source.max_attack_distance = units(1000);
    auto weapon = ion();
    weapon.hardpoint = tactical::object_weapon;
    weapon.pulse_count = 1;
    weapon.cone_width = weapon.cone_height = units(360);
    weapon.object_turret_required = true;
    auto& profile = weapon.object_turret.emplace();
    profile.axes = {at(1, 0), at(0, 1), at(0, 0, 1)};
    profile.speed = Fixed::from_raw(7 * one / 2);
    profile.yaw_extent = units(360);
    profile.pitch_extent = units(90);
    weapon.muzzles = {at(10, 0)};
    source.weapons = {weapon};
    auto state = tactical::initial_combat(source, 1, 0, 1);
    state.attack_target = 2;
    state.direct = true;
    tactical::detail::CombatUnit self, target;
    self.id = 1; self.owner = self.team = 1; self.profile = &source; self.combat = &state;
    self.transform = math::identity_matrix();
    target.id = 2; target.owner = target.team = 2; target.visible_to = 1; target.profile = &victim;
    target.position = target.previous_position = at(0, 200, 200);
    target.transform = math::to_matrix(math::identity_quat(), target.position).value();
    world.units = {self, target};
    const auto point = [&]() {
        state.attack_target = 2; state.direct = true;
        state.weapons[0].countdown = 0;
        state.weapons[0].pulses_left = 1;
        ++world.frame;
        auto result = tactical::detail::step_combat(world, world.units.front());
        expect(static_cast<bool>(result), "UA-05: ordinary turret step succeeds");
        if (result) state = result.value().state;
        return result;
    };
    const auto first = point();
    expect(first && first.value().events.empty() && state.weapons[0].turret->yaw == profile.speed
        && state.weapons[0].turret->pitch == Fixed::from_raw(-profile.speed.raw())
        && !state.weapons[0].turret->ready, "UA-05: simultaneous rate-limited yaw/pitch gates shots");
    for (unsigned count = 1; count < 25; ++count) {
        const auto step = point();
        expect(step && step.value().events.empty(), "UA-05: no firing before yaw has reached its target");
    }
    const auto ready = point();
    expect(ready && !ready.value().events.empty() && state.weapons[0].turret->ready
        && state.weapons[0].turret->target == 2 && state.weapons[0].turret->yaw == units(90),
        "UA-05: the 26th pointing call reaches 90 degrees and releases firing");
    if (ready && !ready.value().events.empty()) {
        const auto& origin = ready.value().events.front().origin;
        expect(std::llabs(origin.x.raw()) < one / 100 && origin.y.raw() > 7 * one && origin.z.raw() > 7 * one,
            "UA-05: no-barrel pitch and yaw rotate the firing muzzle together");
    }
    state.weapons[0].turret->yaw = units(179); state.weapons[0].turret->pitch = {};
    world.units.back().position = at(-200, -3);
    static_cast<void>(point());
    expect(state.weapons[0].turret->yaw.raw() < -179 * one && state.weapons[0].turret->ready, "UA-05: yaw crosses the wrap by the shortest step");
    source.weapons[0].object_turret->yaw_extent = units(45);
    state.weapons[0].turret->yaw = {};
    world.units.back().position = at(0, 200);
    const auto outside = point();
    expect(outside && outside.value().events.empty() && state.weapons[0].turret->yaw.raw() == 0 && !state.weapons[0].turret->ready,
        "UA-05: an out-of-extent target is rejected without clamping toward it");
    source.weapons[0].object_turret->yaw_extent = units(360);
    source.weapons[0].object_turret->pitch_extent = units(10);
    world.units.back().position = at(200, 0, 200);
    const auto high = point();
    expect(high && high.value().events.empty() && !state.weapons[0].turret->ready, "UA-05: pitch extent also gates firing");
    source.weapons[0].object_turret->xy_only = true;
    expect(point() && state.weapons[0].turret->ready, "UA-05: XY-only turrets omit the pitch admission and pose");
    source.weapons[0].object_turret.reset();
    const auto absent = point();
    expect(absent && absent.value().events.empty() && !state.weapons[0].turret->ready, "UA-05: missing required bone profile cannot fire");
    source.weapons[0] = weapon;
    state = tactical::initial_combat(source, 1, 0, 1);
    state.attack_target = 2; state.direct = true;
    state.weapons[0].countdown = 100;
    world.units.back().position = at(0, 200);
    const auto delayed = tactical::detail::step_combat(world, world.units.front());
    expect(delayed && delayed.value().state.weapons[0].turret->yaw == profile.speed
        && delayed.value().state.weapons[0].countdown == 99 && delayed.value().events.empty(),
        "UA-05: targeting advances during recharge without releasing a shot");
    world.units.back().position = at(5000, 0);
    state.weapons[0].turret->yaw = units(14);
    const auto distant = tactical::detail::step_combat(world, world.units.front());
    expect(distant && distant.value().state.weapons[0].turret->yaw == units(14)
        && !distant.value().state.weapons[0].turret->ready,
        "FC-60: retained range loss holds a nonzero pose without pointing");
    if (distant) state = distant.value().state;
    for (const auto frame : {100U, 161U, 1000U}) {
        world.frame = frame;
        const auto held = tactical::detail::step_combat(world, world.units.front());
        expect(held && held.value().state.weapons[0].turret->yaw == units(14)
            && held.value().state.weapons[0].turret->target == 2
            && !held.value().state.weapons[0].turret->rest_started_frame
            && !held.value().state.weapons[0].turret->returning_to_rest,
            "FC-60: retained out-of-range target starts no idle timer even after the default hold duration");
        if (held) state = held.value().state;
    }
    world.units.back().position = at(0, 200);
    const auto reentered = point();
    expect(reentered && state.weapons[0].turret->yaw == Fixed::from_raw(35 * one / 2)
        && state.weapons[0].turret->target == 2 && !state.weapons[0].turret->rest_started_frame,
        "FC-60: re-entry resumes tracking from the held angle");
    source.min_attack_distance = units(300);
    world.units.back().position = at(0, 200);
    const auto too_near = tactical::detail::step_combat(world, world.units.front());
    expect(too_near && too_near.value().state.weapons[0].turret->yaw == Fixed::from_raw(35 * one / 2)
        && !too_near.value().state.weapons[0].turret->ready, "FS-33: minimum distance excludes pointing");
    source.min_attack_distance = {};
    source.weapons[0].object_turret_rest_delay = Fixed::from_raw(one / 30);
    state = tactical::initial_combat(source, 1, 0, 1);
    state.weapons[0].turret->yaw = units(14);
    world.frame = 100;
    const auto idle_step = [&]() {
        auto step = tactical::detail::step_combat(world, world.units.front());
        expect(static_cast<bool>(step), "FC-61: idle step succeeds");
        if (step) state = step.value().state;
    };
    idle_step();
    expect(state.weapons[0].turret->yaw == units(14) && state.weapons[0].turret->rest_started_frame == 100,
        "FSC-54: target loss holds pose and starts the idle delay");
    ++world.frame; idle_step();
    expect(state.weapons[0].turret->yaw == units(14), "FSC-54: equality with the idle delay holds");
    ++world.frame; idle_step();
    expect(state.weapons[0].turret->yaw == Fixed::from_raw(21 * one / 2), "FSC-54: strict expiry releases one rest step");
    for (unsigned tick = 0; tick < 3; ++tick) { ++world.frame; idle_step(); }
    expect(state.weapons[0].turret->yaw.raw() == 0 && !state.weapons[0].turret->returning_to_rest,
        "FC-61: subsequent calls reach rest without overshoot");
    state.weapons[0].turret->yaw = units(14);
    ++world.frame; idle_step();
    world.units.back().position = at(0, 200);
    static_cast<void>(point());
    expect(!state.weapons[0].turret->rest_started_frame && !state.weapons[0].turret->returning_to_rest,
        "FC-60: reacquisition cancels the idle hold");
    source.weapons[0] = weapon; // FC-61/EFSB-06: default two-second idle hold.
    state = tactical::initial_combat(source, 1, 0, 1);
    state.weapons[0].turret->yaw = units(14);
    world.frame = 200; idle_step();
    world.frame = 260; idle_step();
    expect(state.weapons[0].turret->yaw == units(14), "FC-61: stock default holds through exactly two seconds");
    ++world.frame; idle_step();
    expect(state.weapons[0].turret->yaw == Fixed::from_raw(21 * one / 2),
        "FC-61: stock default returns on the first admitted turn after two seconds");
    auto cases = table();
    cases.profiles.back().weapons = {weapon};
    const auto initial = setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(0, 200, 200))});
    for (const bool xy_only : {false, true}) for (const bool barrel : {false, true}) {
        auto variant = cases;
        auto& turret_profile = *variant.profiles.back().weapons.front().object_turret;
        turret_profile.xy_only = xy_only;
        turret_profile.barrel = barrel;
        turret_profile.barrel_axes = turret_profile.axes;
        auto value = session(initial, variant);
        const eawr::platform::ThreadWorkerAdapter executor(1);
        for (unsigned tick = 0; tick < 20; ++tick) {
            const auto step = value.step(executor);
            expect(static_cast<bool>(step), "UA-05: static turret-profile variant steps");
            if (!step) return;
            const auto& instance = step.value().snapshot->instances().front();
            expect(instance.object_turret && instance.object_turret_xy_only == xy_only
                && instance.object_turret_barrel == barrel,
                "UA-05: snapshot carries the same validated XY-only and barrel admission as the sim");
            if (xy_only) expect(instance.object_turret->pitch.raw() == 0,
                "UA-05: XY-only picture flags correspond to omitted simulated pitch");
        }
    }
    auto original = session(initial, cases);
    expect(static_cast<bool>(original.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
        "OTUR: tracking order is recorded");
    std::vector<std::string> recorded_hashes;
    std::vector<tactical::WeaponState::TurretState> recorded_turrets;
    std::vector<std::string> recorded_snapshots;
    const eawr::platform::ThreadWorkerAdapter replay_executor(1);
    for (unsigned tick = 0; tick < 120; ++tick) {
        const auto step = original.step(replay_executor);
        expect(static_cast<bool>(step), "OTUR: original replay steps");
        if (!step) return;
        recorded_hashes.push_back(step.value().state_sha256);
        recorded_snapshots.push_back(step.value().snapshot->sha256());
        recorded_turrets.push_back(*step.value().snapshot->instances().front().object_turret);
    }
    expect(recorded_turrets.front().yaw.raw() != 0 && recorded_turrets.back().ready,
        "OTUR: round-trip covers moving nonzero angles and firing readiness");
    const auto recording = original.record();
    const auto encoded = tactical::write_replay(recording);
    expect(static_cast<bool>(encoded), "OTUR: replay encodes");
    if (!encoded) return;
    const auto decoded = tactical::parse_replay(encoded.value());
    expect(decoded && decoded.value() == recording, "OTUR: replay encode/decode round-trips");
    if (!decoded) return;
    auto replayed = tactical::TacticalSession::from_replay(decoded.value(), sensors(), {}, {}, std::nullopt, cases);
    expect(static_cast<bool>(replayed), "OTUR: decoded replay creates the same session");
    if (!replayed) return;
    for (unsigned tick = 0; tick < 120; ++tick) {
        const auto step = replayed.value().step(replay_executor);
        expect(step && step.value().state_sha256 == recorded_hashes[tick]
            && step.value().snapshot->sha256() == recorded_snapshots[tick]
            && step.value().snapshot->instances().front().object_turret == recorded_turrets[tick],
            "OTUR: decoded replay reproduces canonical state, snapshot and every turret field at each tick");
        if (!step) return;
    }
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto value = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(0, 200, 200))}), cases);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        bool published = false;
        for (unsigned tick = 0; tick < 120; ++tick) {
            auto result = value.step(executor);
            expect(static_cast<bool>(result), "UA-05: partitioned turret session steps");
            if (!result) break;
            hashes.push_back(result.value().state_sha256);
            for (const auto& instance : result.value().snapshot->instances()) {
                if (instance.entity_id == 1 && instance.object_turret) {
                    published = published || (instance.object_turret->target == 2 && instance.object_turret->yaw.raw() != 0);
                }
            }
        }
        expect(published, "UA-05: snapshot publishes the ordinary target and moving angles");
        if (reference.empty()) reference = std::move(hashes);
        else expect(reference == hashes, "UA-05: ordinary turret canonical state equals on 1/2/4/8 workers");
    }
}

void test_fire_bone_pole() {
    // W-07 (#384): a point on the fire bone's z axis has no planar part; FoC's facing gives it yaw 0
    // and pitch 90 degrees instead of an undefined atan2 (research WA-14). A frame like
    // Corellian_Gunboat's HP_Corellian_Gunship_04 (z horizontal); the target sits on +z from the
    // weapon midpoint (5, 0, 0).
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    auto gunboat = table();
    gunboat.profiles[0].weapons[0].fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 0, -1), at(0, 1)};
    // run() also fails the case when a step fails (EAWR-SIM-0310 before the fix).
    auto narrow = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, 300), true)}), gunboat);
    expect(run(narrow, 150).empty(), "the pole's 90-degree pitch is outside a 45-degree cone height");
    auto tall = gunboat;
    tall.profiles[0].weapons[0].cone_height = units(180);
    auto upright = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, 300), true)}), tall);
    expect(fired_at(run(upright, 150), 2), "the pole is inside a 180-degree cone height (yaw 0, pitch 90)");
    auto below = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, -300), true)}), tall);
    expect(fired_at(run(below, 150), 2), "the opposite pole is inside a 180-degree cone height");
    // The weapon midpoint itself: yaw and pitch 0, inside any cone.
    auto midpoint = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(5, 0), true)}), gunboat);
    expect(fired_at(run(midpoint, 150), 2), "a target at the weapon midpoint is pointable");
}

// W-09 (#388): an object weapon with turret extents (a TIE's 20 degrees of yaw, 40 of pitch) fires
// only at an aim point within them about the unit's facing, each compared whole.
void test_object_weapon_cone() {
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    auto turret = table();
    turret.profiles[4].weapons[0].cone_width = units(20);
    turret.profiles[4].weapons[0].cone_height = units(40);
    const auto shoot = [&](const math::Vec3 at_point) {
        auto value = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at_point, true)}), turret);
        return fired_at(run(value, 150), 2);
    };
    expect(shoot(at(300, 80)), "W-09: 15 degrees of yaw is inside a 20-degree extent (compared whole)");
    expect(!shoot(at(300, 150)), "W-09: 27 degrees of yaw is outside a 20-degree extent");
    expect(shoot(at(300, 0, 200)), "W-09: 34 degrees of pitch is inside a 40-degree extent");
    expect(!shoot(at(300, 0, 300)), "W-09: 45 degrees of pitch is outside a 40-degree extent");
    expect(!shoot(at(-300, 0)), "W-09: a craft does not fire at a target behind it");
    auto open = session(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, 0), true)}));
    expect(fired_at(run(open, 150), 2), "W-09: an object weapon without extents fires in any direction");
}

// WWP-47/49/50 (#710): the unit's own weapon aims at the hardpoint attached to its attack target
// (ordered, else the nearest standing one, G-W3), else a keyed draw over the target's Target_Bones,
// else the height-adjusted centre; and it leaves its muzzle bones in turn.
void test_unit_weapon_targeting_and_muzzles() {
    tactical::detail::CombatWorld world;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    world.players = owners;
    world.relationships = relationships;
    world.seed = 777;
    tactical::CombatProfile shooter_profile;
    auto weapon = ion();
    weapon.hardpoint = tactical::object_weapon;
    weapon.cone_width = weapon.cone_height = units(360);
    weapon.has_fire_b = false;
    weapon.fire_a = {};
    weapon.pulse_count = 1;
    weapon.muzzles = {at(5, 4), at(5, -4), at(-2, 7)};
    shooter_profile.weapons = {weapon};
    tactical::CombatProfile target_profile;
    target_profile.ranged_target_z_adjust = units(35);
    tactical::CombatState state;
    state.attack_target = 2;
    state.direct = true;
    state.weapons.resize(1);
    tactical::detail::CombatUnit self;
    self.id = 1;
    self.owner = self.team = 1;
    self.profile = &shooter_profile;
    self.combat = &state;
    self.transform = math::identity_matrix();
    tactical::detail::CombatUnit target;
    target.id = 2;
    target.owner = target.team = 2;
    target.visible_to = 1;
    target.position = target.previous_position = at(200, 0);
    target.transform = math::to_matrix(math::identity_quat(), target.position).value();
    target.profile = &target_profile;
    world.units = {self, target};
    // One shot per frame: the burst and the countdown are reset by hand, the muzzle index carries over.
    const auto shoot = [&](const std::uint64_t frame) -> std::optional<tactical::CombatEvent> {
        state.weapons[0].countdown = 0;
        state.weapons[0].pulses_left = 1;
        world.frame = frame;
        const auto step = tactical::detail::step_combat(world, world.units.front());
        expect(static_cast<bool>(step), "WWP-50: the unit weapon's step succeeds");
        if (!step) return std::nullopt;
        state = step.value().state;
        if (step.value().events.empty()) return std::nullopt;
        return step.value().events.front();
    };
    // WWP-47: the shot leaves the muzzle bones round robin.
    const std::array<math::Vec3, 5> expected{at(5, 4), at(5, -4), at(-2, 7), at(5, 4), at(5, -4)};
    for (std::size_t shot = 0; shot < expected.size(); ++shot) {
        const auto event = shoot(100 + shot);
        expect(event && event->origin == expected[shot], "WWP-47: successive shots leave the next muzzle bone in turn");
    }
    expect(state.weapons[0].next_muzzle == 2, "WWP-47: the round-robin index is kept in the weapon state");
    // WWP-50, G-W3: a target with standing hardpoints is aimed at its nearest one (the hardpoint the
    // targeting attached when it acquired the target), whatever bones it has.
    target_profile.hardpoints = {{0, at(0, 30), true}, {1, at(0, -30, 40), true}, {2, at(0, 5), false}};
    target_profile.target_bones = {at(0, 12)};
    const auto attached = shoot(150);
    expect(attached && attached->target_hardpoint == 0 && attached->aim == at(200, 30),
        "WWP-50: the nearest targetable hardpoint is the aim point (ties to the first), not a bone");
    // The ordered hardpoint wins over the nearest one.
    state.attack_hardpoint = 1;
    const auto ordered = shoot(151);
    expect(ordered && ordered->target_hardpoint == 1 && ordered->aim == at(200, -30, 40),
        "WWP-50: an ordered hardpoint is the aim point, with the hardpoint named");
    state.attack_hardpoint = tactical::no_hardpoint;
    // WWP-50: no standing targetable hardpoint left: the height-adjusted centre without bones.
    target_profile.target_bones.clear();
    target_profile.hardpoints = {{0, at(0, 5), false}};
    for (std::uint64_t frame = 0; frame < 6; ++frame) {
        const auto event = shoot(200 + frame);
        expect(event && event->aim == at(200, 0, 35) && event->target_hardpoint == tactical::no_hardpoint,
            "WWP-50: without a hardpoint or bones the unit weapon aims at the height-adjusted centre");
    }
    // WWP-50: with target bones, a keyed draw over them; never a hardpoint, and the same for the
    // same frame whatever else happened before.
    target_profile.target_bones = {at(0, 12), at(0, -12), at(0, 0, 9)};
    std::array<bool, 3> seen{};
    std::vector<math::Vec3> first;
    for (std::uint64_t frame = 0; frame < 60; ++frame) {
        const auto event = shoot(300 + frame);
        expect(event && event->target_hardpoint == tactical::no_hardpoint, "WWP-50: a bone aim names no hardpoint");
        if (!event) continue;
        first.push_back(event->aim);
        const std::array<math::Vec3, 3> points{at(200, 12), at(200, -12), at(200, 0, 9)};
        for (std::size_t bone = 0; bone < seen.size(); ++bone) {
            if (event->aim == points[bone]) seen[bone] = true;
        }
    }
    expect(seen[0] && seen[1] && seen[2], "WWP-50: the draw spreads the aim over every target bone");
    state = tactical::CombatState{};
    state.attack_target = 2;
    state.direct = true;
    state.weapons.resize(1);
    std::vector<math::Vec3> second;
    for (std::uint64_t frame = 0; frame < 60; ++frame) {
        state.weapons[0].next_muzzle = 0;
        const auto event = shoot(300 + frame);
        if (event) second.push_back(event->aim);
    }
    expect(first == second, "WWP-50: the draw depends on the seed, frame, unit and weapon only");
    // WWP-47, WWP-53: the cone opens at the muzzle, and a muzzle whose launch the cone rejects is still
    // used up: with a 2-degree yaw extent a target at (200, 10) is inside from the first and the third
    // muzzle (1.8 and 0.9 degrees) and outside from the second (4.1 degrees).
    shooter_profile.weapons[0].cone_width = units(2);
    target_profile.hardpoints.clear();
    target_profile.target_bones.clear();
    world.units.back().position = world.units.back().previous_position = at(200, 10);
    world.units.back().transform = math::to_matrix(math::identity_quat(), at(200, 10)).value();
    state = tactical::CombatState{};
    state.attack_target = 2;
    state.direct = true;
    state.weapons.resize(1);
    const auto first_cone = shoot(600);
    const auto second_cone = shoot(601);
    const auto third_cone = shoot(602);
    const auto fourth_cone = shoot(603);
    expect(first_cone && first_cone->origin == at(5, 4), "WWP-53: the first muzzle is inside the cone and fires");
    expect(!second_cone, "WWP-53: the second muzzle's launch is rejected by the cone");
    expect(third_cone && third_cone->origin == at(-2, 7), "WWP-47: the rejected shot used its muzzle up, the third fires");
    expect(fourth_cone && fourth_cone->origin == at(5, 4), "WWP-47: the round robin wraps to the first muzzle");
    shooter_profile.weapons[0].cone_width = units(360);
    world.units.back().position = world.units.back().previous_position = at(200, 0);
    world.units.back().transform = math::to_matrix(math::identity_quat(), at(200, 0)).value();
    // WWP-49: a lone shooter resolves a squadron's team container to the craft nearest the muzzle it
    // is about to fire from, so the nearer craft changes with the muzzle in turn.
    tactical::detail::CombatUnit container;
    container.id = 20;
    container.owner = container.team = 2;
    container.visible_to = 1;
    auto craft_a = world.units.back();
    craft_a.id = 21;
    craft_a.position = craft_a.previous_position = at(100, 10);
    craft_a.transform = math::to_matrix(math::identity_quat(), craft_a.position).value();
    auto craft_b = craft_a;
    craft_b.id = 22;
    craft_b.position = craft_b.previous_position = at(100, -10);
    craft_b.transform = math::to_matrix(math::identity_quat(), craft_b.position).value();
    world.units = {world.units.front(), container, craft_a, craft_b};
    world.teams = {{20, {21, 22}}};
    state = tactical::CombatState{};
    state.attack_target = 20;
    state.direct = true;
    state.weapons.resize(1);
    const std::array<eawr::sim::EntityId, 4> nearest{21, 22, 21, 21};
    // Muzzles (5, 4), (5, -4), (-2, 7), then (5, 4) again: the first and third are nearer craft 21 (y 10),
    // the second craft 22 (y -10).
    for (std::size_t shot = 0; shot < nearest.size(); ++shot) {
        const auto event = shoot(500 + shot);
        expect(event && event->target == nearest[shot],
            "WWP-49: the team container resolves to the craft nearest the muzzle in turn");
    }
}

// W-10 (#409): a shot leads a moving target to the point where a projectile meets it.
void test_lead() {
    const auto near = [](const Fixed value, const std::int64_t thousandths) {
        const auto difference = value.raw() * 1000 / one - thousandths;
        return difference >= -2 && difference <= 2;
    };
    const auto still = tactical::lead_point(at(700, 0), at(0, 0), at(0, 0), units(25));
    expect(still && *still == at(700, 0), "W-10: a target that does not move is not led");
    // Head-on at 5 per frame: 700 = 30 t, t = 23.333, so the shots meet it 116.667 closer.
    const auto closing = tactical::lead_point(at(700, 0), at(0, 0), at(-5, 0), units(25));
    expect(closing && near(closing->x, 583333) && closing->y.raw() == 0, "W-10: a closing target is met nearer");
    // Crossing at 5 per frame: 700^2 + 25 t^2 = 625 t^2, t = 28.577, 142.887 along its path.
    const auto crossing = tactical::lead_point(at(700, 0), at(0, 0), at(0, 5), units(25));
    expect(crossing && crossing->x == units(700) && near(crossing->y, 142887), "W-10: a crossing target is led");
    // The aim point is measured from the shot's origin, not the shooter.
    const auto offset = tactical::lead_point(at(710, 20), at(10, 20), at(0, 5), units(25));
    expect(offset && near(offset->y, 162887), "W-10: the lead starts at the muzzle and keeps the aim offset");
    expect(!tactical::lead_point(at(100, 0), at(0, 0), at(30, 0), units(25)),
        "W-10: no shot meets a target that outruns it; the attempt fails");
    // WWP-51: these three points lie on a circle of radius 100. From its centre,
    // a speed-100 projectile meets the target after one frame, at angle sqrt(0.8).
    const auto arc = tactical::detail::lead_history(at(100, 0), at(60, -80), at(-28, -96),
        at(100, 0), at(0, 0), units(100));
    expect(arc && near(arc->x, 62597) && near(arc->y, 77985),
        "WWP-51: a turning target is met on its circle, rather than its tangent");
    const auto reversed = tactical::detail::lead_history(at(100, 0), at(60, 80), at(-28, 96),
        at(100, 0), at(0, 0), units(100));
    expect(reversed && arc && reversed->x == arc->x && reversed->y.raw() == -arc->y.raw(),
        "WWP-51: mirrored history reverses the predicted arc");
    const auto raised = tactical::detail::lead_history(at(100, 0), at(60, -80), at(-28, -96),
        at(100, 0, 20), at(0, 0), units(100));
    expect(raised && arc && raised->x == arc->x && raised->y == arc->y && raised->z == units(20),
        "WWP-51: a selected aim offset is retained after predicting the centre");
    const auto line = tactical::detail::lead_history(at(700, 0), at(705, 0), at(710, 0),
        at(700, 0), at(0, 0), units(25));
    expect(line == closing, "WWP-51: aligned history retains linear interception");
    const auto repeated = tactical::detail::lead_history(at(700, 0), at(700, 0), at(700, 0),
        at(700, 0), at(0, 0), units(25));
    expect(repeated == still, "WWP-51: a stationary history retains its aim");
    const auto broad = tactical::detail::lead_history(at(5000, 0), at(3000, -4000), at(-1400, -4800),
        at(5000, 0), at(0, 0), units(5000));
    expect(broad == tactical::lead_point(at(5000, 0), at(0, 0), at(2000, 4000), units(5000)),
        "WWP-51: a circle above the radius limit uses linear interception");
    auto retained = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0))}));
    const auto initial_bytes = retained.canonical_state_bytes();
    const std::array<std::uint8_t, 4> history_tag{'P', 'H', 'S', 'T'};
    expect(std::search(initial_bytes.begin(), initial_bytes.end(), history_tag.begin(), history_tag.end())
        == initial_bytes.end(), "PHST: a fresh session has no position-history block");
    (void)run(retained, 1);
    const auto stationary_bytes = retained.canonical_state_bytes();
    expect(std::search(stationary_bytes.begin(), stationary_bytes.end(), history_tag.begin(), history_tag.end())
        == stationary_bytes.end(), "PHST: stationary targets keep history absent");
    tactical::MotionTable motion;
    motion.rules.arc_degrees = units(15);
    motion.rules.expansion_distance = units(300);
    tactical::MotionProfile moving;
    moving.type_id = bomber_type;
    moving.max_speed = moving.acceleration = moving.deceleration = units(80);
    moving.rate_of_turn = units(90);
    moving.turn_in_place_slowdown = units(1);
    motion.profiles.push_back(moving);
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0))}),
        sensors(), {}, motion, std::nullopt, table());
    expect(static_cast<bool>(created), "PHST: a moving-target session is created");
    if (!created) return;
    auto tracked = std::move(created).value();
    expect(static_cast<bool>(tracked.submit({{0, 2, 0}, {2}, tactical::MovePayload{at(300, 1000)}})),
        "PHST: the target's move is queued");
    (void)run(tracked, 10);
    const auto history_bytes = tracked.canonical_state_bytes();
    constexpr auto block_size = std::size_t{20 + 56};
    expect(history_bytes.size() >= block_size && std::equal(history_tag.begin(), history_tag.end(),
        history_bytes.end() - block_size), "PHST: the moving target appends its versioned history block");
    tracked.scramble_storage_for_testing();
    expect(tracked.canonical_state_bytes() == history_bytes, "PHST: registry reconstruction preserves history");
}

// PHST is part of the checkpoint (review of the WWP-51 change): a target with history, saved while
// it is shot at, restores with its two preceding positions and the run then continues byte for byte
// like the live one. A restored session without them would lead the target linearly again.
void test_history_checkpoint() {
    tactical::MotionTable motion;
    motion.rules.arc_degrees = units(15);
    motion.rules.expansion_distance = units(300);
    tactical::MotionProfile moving;
    moving.type_id = bomber_type;
    moving.max_speed = moving.acceleration = moving.deceleration = units(80);
    moving.rate_of_turn = units(90);
    moving.turn_in_place_slowdown = units(1);
    motion.profiles.push_back(moving);
    const std::array<std::uint8_t, 4> history_tag{'P', 'H', 'S', 'T'};
    std::vector<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const auto label = "PHST checkpoint (" + std::to_string(workers) + " workers): ";
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 0))}),
            sensors(), {}, motion, std::nullopt, table());
        expect(static_cast<bool>(created), label + "the session is created");
        if (!created) return;
        auto live = std::move(created).value();
        expect(static_cast<bool>(live.submit({{0, 2, 0}, {2}, tactical::MovePayload{at(300, 1000)}})),
            label + "the target's move is queued");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto step = [&](tactical::TacticalSession& value) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), label + "the step succeeds");
            return stepped ? stepped.value().state_sha256 : std::string{};
        };
        for (int tick = 0; tick < 10; ++tick) (void)step(live);
        const auto live_bytes = live.canonical_state_bytes();
        expect(std::search(live_bytes.begin(), live_bytes.end(), history_tag.begin(), history_tag.end())
            != live_bytes.end(), label + "the moving target has history at the save");
        const auto saved = live.save_checkpoint_state();
        auto fresh = tactical::TacticalSession::from_replay(live.record(), sensors(), {}, motion, std::nullopt, table());
        expect(saved && fresh, label + "the save and a fresh session exist");
        if (!saved || !fresh) return;
        auto restored = std::move(fresh).value();
        expect(static_cast<bool>(restored.load_checkpoint_state(saved.value())), label + "the checkpoint loads");
        expect(restored.canonical_state_bytes() == live_bytes, label + "the restored state keeps the history");
        const auto resaved = restored.save_checkpoint_state();
        expect(resaved && resaved.value() == saved.value(), label + "the checkpoint round-trips");
        std::vector<std::string> hashes;
        for (int tick = 0; tick < 30; ++tick) {
            const auto expected = step(live);
            expect(step(restored) == expected, label + "the restored run follows the live run");
            hashes.push_back(expected);
        }
        if (workers == 1U) reference = hashes;
        else expect(hashes == reference, label + "the continuation hashes like one worker");
    }
}


// --- Turning toward an ordered target (A-04 to A-07, #361) --------------------------------------

// The shooter turns in place at 1.5 degrees per frame with a slowdown of 2 (0.75 per frame).
[[nodiscard]] tactical::MotionTable turning_motion() {
    tactical::MotionTable motion;
    motion.rules.arc_degrees = units(15);
    motion.rules.expansion_distance = units(300);
    tactical::MotionProfile profile;
    profile.type_id = shooter_type;
    profile.max_speed = units(3);
    profile.acceleration = Fixed::from_raw(one / 20);
    profile.deceleration = Fixed::from_raw(one / 20);
    profile.rate_of_turn = Fixed::from_raw(3 * one / 2);
    profile.turn_in_place_slowdown = units(2);
    motion.profiles.push_back(profile);
    return motion;
}

[[nodiscard]] tactical::TacticalSession turning_session(
    const tactical::TacticalSetup& value, const tactical::CombatTable& combat = table()) {
    auto created = tactical::TacticalSession::create(value, sensors(), {}, turning_motion(), std::nullopt, combat);
    expect(static_cast<bool>(created), "turning session is created");
    return std::move(created).value();
}

[[nodiscard]] double yaw_of(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    for (const auto& state : value.units()) {
        if (state.entity_id != id) continue;
        const auto yaw = tactical::yaw_degrees(state.rotation);
        return yaw ? static_cast<double>(yaw.value().raw()) / static_cast<double>(one) : 1000.0;
    }
    return 1000.0;
}

void test_banked_fire() {
    // WWP-73/74: an off-axis gun follows the hull bank for hardpoint, own, SPECIAL and missile fire.
    // A level muzzle is several units away by the time the narrow yaw cone admits this turn.
    for (const unsigned kind : {0U, 1U, 2U, 3U}) {
        auto content = table();
        auto& gun = content.profiles.front().weapons.front();
        gun.fire_a = at(5, 20, 40);
        gun.has_fire_b = false;
        gun.fire_axes = std::array<math::Vec3, 3>{at(1, 0), at(0, 1), at(0, 0, 1)};
        gun.cone_width = units(40); gun.cone_height = units(180);
        gun.min_recharge_hundredths = gun.max_recharge_hundredths = 10;
        gun.pulse_count = 1;
        gun.special = kind == 2;
        if (kind == 1) {
            gun.hardpoint = tactical::object_weapon;
            gun.muzzles = {gun.fire_a};
        }
        gun.shot.emplace();
        gun.shot->speed = units(25); gun.shot->damage = units(1); gun.shot->max_travel = units(700);
        gun.shot->homing = kind == 3;
        if (kind == 3) gun.shot->turn_rate = units(1);
        auto motion = turning_motion();
        motion.profiles.front().bank_angle = units(20);
        motion.profiles.front().roll_rate = Fixed::from_raw(24 * one / 100);
        tactical::DurabilityTable health;
        health.damage.emplace();
        health.damage->shield_recharge_frames = 1;
        const auto initial = setup({unit(1, shooter_type, 1, at(0, 0)),
            unit(2, bomber_type, 2, at(0, 300), true)});
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            auto created = tactical::TacticalSession::create(initial, sensors(), health, motion, std::nullopt, content);
            expect(static_cast<bool>(created), "WWP-73: synthetic banked combat session creates");
            if (!created) continue;
            auto& value = created.value();
            expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})),
                "WWP-73: banked turn receives attack");
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            bool witnessed = false;
            for (unsigned tick = 0; tick < 125; ++tick) {
                const auto stepped = value.step(executor);
                expect(static_cast<bool>(stepped), "WWP-73: banked turn steps");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256);
                const auto& snapshot = *stepped.value().snapshot;
                const auto source = std::find_if(snapshot.instances().begin(), snapshot.instances().end(),
                    [](const auto& entry) { return entry.entity_id == 1; });
                expect(source != snapshot.instances().end(), "WWP-73: banked shooter is published");
                if (source == snapshot.instances().end()) continue;
                const auto roll = value.roll_degrees(1).value_or(Fixed{});
                expect(value.tick_work().banked_matrix_builds <= 1U,
                    "WWP-73: combat and snapshot reuse at most one banked matrix per rolling unit");
                for (const auto& event : snapshot.combat_events()) {
                    if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                    const auto muzzle = math::transform_point(source->fixed_transform, gun.fire_a).value();
                    expect(math::length(math::Vec3{Fixed::from_raw(muzzle.x.raw() - event.origin.x.raw()),
                        Fixed::from_raw(muzzle.y.raw() - event.origin.y.raw()),
                        Fixed::from_raw(muzzle.z.raw() - event.origin.z.raw())}).value().raw() <= 64,
                        "WWP-73: projectile release aligns with the drawn banked gun");
                    const auto projectile = std::find_if(snapshot.projectiles().rbegin(), snapshot.projectiles().rend(),
                        [&](const auto& entry) { return entry.shooter == 1 && entry.position == event.origin; });
                    expect(projectile != snapshot.projectiles().rend(), "WWP-74: released projectile begins at banked muzzle");
                    if (projectile != snapshot.projectiles().rend()) {
                        const auto ray = math::Vec3{Fixed::from_raw(event.aim.x.raw() - event.origin.x.raw()),
                            Fixed::from_raw(event.aim.y.raw() - event.origin.y.raw()),
                            Fixed::from_raw(event.aim.z.raw() - event.origin.z.raw())};
                        const auto direction = math::normalize(ray).value();
                        const auto flight = math::normalize(projectile->step).value();
                        expect(math::dot(direction, flight).value() > Fixed::from_raw(one - 64),
                            "WWP-74: launch flies toward the aim point from the banked muzzle");
                    }
                    witnessed = witnessed || std::llabs(roll.raw()) > units(10).raw();
                }
            }
            expect(witnessed, "WWP-73: synthetic gun fires during a substantial bank");
            if (reference.empty()) reference = hashes;
            expect(hashes == reference, "WWP-73/74: banked firing hashes agree on 1/2/4/8 workers");
        }
    }
}

void test_banked_command_and_staged_frames() {
    // WWP-73/74: commanded planetary pulse one, manual admission and staged publication
    // must agree with the same drawn bone. The manual cone is deliberately narrow in pitch:
    // rolling its broadside axis admits the banked target and excludes the level target.
    for (const unsigned route : {0U, 1U, 2U}) {
        const bool planetary = route == 0;
        auto content = table();
        auto& profile = content.profiles.front();
        profile.star_base = !planetary;
        profile.special_weapon = planetary;
        if (planetary) profile.planetary_weapon = tactical::PlanetaryWeaponProfile{0, 1, 200, 2, bomber_bit};
        auto& gun = profile.weapons.front();
        gun.special = true;
        gun.opportunity_when_idle = gun.opportunity_when_targeting = false;
        gun.requires_manual_target = !planetary;
        gun.hardpoint = planetary ? tactical::object_weapon : 0;
        gun.fire_a = at(5, 20, 40); gun.has_fire_b = false;
        if (planetary) gun.muzzles = {gun.fire_a};
        gun.fire_axes = std::array<math::Vec3, 3>{at(0, 1), at(-1, 0), at(0, 0, 1)};
        gun.cone_width = units(360); gun.cone_height = units(planetary ? 180 : 4);
        gun.pulse_count = planetary ? 2 : 1;
        gun.min_recharge_hundredths = gun.max_recharge_hundredths = 10;
        gun.shot.emplace(); gun.shot->speed = units(25); gun.shot->damage = units(1);
        gun.shot->max_travel = units(1000);
        auto motion = turning_motion();
        motion.profiles.front().bank_angle = units(20);
        motion.profiles.front().roll_rate = Fixed::from_raw(48 * one / 100);
        tactical::DurabilityTable health;
        health.damage.emplace(); health.damage->shield_recharge_frames = 1;
        tactical::EconomyRules economy;
        economy.players = {{1, units(100), 20, false}, {2, units(100), 20, false}};
        tactical::BuildOption option{90, tactical::BuildKind::upgrade, tactical::BuildQueue::upgrades,
            units(1), 1, 1, 0, true};
        economy.menus = {{shooter_type, 1, {option}}};
        tactical::UpgradeProfile unlock;
        unlock.type = 90; unlock.special_unlocks = {{false, false}};
        auto bonus_option = option;
        bonus_option.type = 91;
        economy.menus.front().options.push_back(bonus_option);
        tactical::UpgradeProfile upgrade;
        upgrade.type = 91;
        tactical::UpgradeBonus bonus;
        bonus.applicable = {shooter_type}; bonus.percentages[1] = Fixed::from_raw(one / 4);
        upgrade.bonuses = {bonus};
        economy.upgrades = {unlock, upgrade};
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto initial = setup({unit(1, shooter_type, 1, at(0, 0)),
                unit(3, transport_type, 1, at(-500, 0))});
            auto created = tactical::TacticalSession::create(initial, sensors(), health, motion,
                std::nullopt, content, {}, {}, economy);
            expect(static_cast<bool>(created), "WWP-73: banked command fixture creates");
            if (!created) {
                std::cerr << "WWP-73 banked command setup: " << created.error().message << '\n';
                continue;
            }
            auto& value = created.value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::BuyPayload{90}})),
                "WWP-73: banked command unlock submits");
            expect(static_cast<bool>(value.submit({{0, 1, 1}, {1}, tactical::FacePayload{at(0, 10000)}})),
                "WWP-73: banked command turn submits");
            std::vector<std::string> hashes;
            for (unsigned tick = 0; tick < 50; ++tick) {
                const auto stepped = value.step(executor);
                expect(static_cast<bool>(stepped), "WWP-73: banked command warmup steps");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
            }
            expect(std::llabs(value.roll_degrees(1).value_or(Fixed{}).raw()) > units(10).raw(),
                "WWP-73: commanded fixture has a substantial bank");
            const auto source_frame = value.snapshot()->instances().front().fixed_transform;
            const auto source_state = value.units().front();
            const auto level_frame = math::to_matrix(source_state.rotation, source_state.position).value();
            const auto position = math::transform_point(route == 2 ? level_frame : source_frame, at(5, 320, 40)).value();
            const auto target = value.stage_spawn(unit(2, bomber_type, 2, position));
            expect(static_cast<bool>(target), "WWP-73: banked command target stages");
            if (!target) continue;
            expect(value.snapshot()->instances().front().fixed_transform == source_frame,
                "WWP-73: stage add preserves the rolling hull's published frame");
            expect(static_cast<bool>(value.stage_remove(3)), "WWP-73: unrelated staged unit removes");
            expect(value.snapshot()->instances().front().fixed_transform == source_frame,
                "WWP-73: stage remove preserves the rolling hull's published frame");
            // Frozen banked and level admission must disagree, independently of the command result.
            if (!planetary) {
                auto own = value.snapshot()->instances().front();
                const auto& enemy = value.snapshot()->instances().back();
                const auto banked = tactical::manual_special_fire_admission(value.snapshot()->players(), own, enemy, false, 0);
                own.fixed_transform = level_frame;
                const auto level = tactical::manual_special_fire_admission(value.snapshot()->players(), own, enemy, false, 0);
                expect(banked && banked.value().has_value() == (route == 1)
                    && level && level.value().has_value() == (route == 2),
                    "WWP-73: manual fixture distinguishes rolled and level cone admission");
            }
            expect(static_cast<bool>(value.submit({{50, 1, 2}, {1}, tactical::SpecialFirePayload{target.value()}})),
                "WWP-73: banked SPECIAL command submits");
            expect(static_cast<bool>(value.submit({{50, 1, 3}, {1}, tactical::BuyPayload{91}})),
                "WWP-73: rolling hull receives an upgrade-bonus purchase");
            unsigned accepted = 0, rejected = 0, shots = 0, pulses = 0;
            for (unsigned tick = 50; tick < 56; ++tick) {
                const auto stepped = value.step(executor);
                expect(static_cast<bool>(stepped), "WWP-73: banked SPECIAL command steps");
                if (!stepped) break;
                hashes.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
                const auto& snapshot = *stepped.value().snapshot;
                const auto& own = snapshot.instances().front();
                const auto state = value.units().front();
                const auto drawn = math::to_matrix(tactical::banked_rotation(state.rotation,
                    value.roll_degrees(1).value_or(Fixed{})).value(), state.position).value();
                expect(own.fixed_transform == drawn, "WWP-73: next step republishes the same banked rule");
                if (tick == 55) {
                    expect(std::any_of(value.ledgers().front().completed.begin(), value.ledgers().front().completed.end(),
                        [](const auto& held) { return held.type == 91; }),
                        "WWP-73: upgrade-bonus republish is exercised while the hull remains banked");
                    expect(std::llabs(value.roll_degrees(1).value_or(Fixed{}).raw()) > units(10).raw(),
                        "WWP-73: bonus publication occurs during a substantial bank");
                }
                for (const auto& event : snapshot.events()) {
                    if (event.order == tactical::OrderKind::special_fire) {
                        accepted += event.kind == tactical::EventKind::order_accepted ? 1U : 0U;
                        rejected += event.kind == tactical::EventKind::order_rejected ? 1U : 0U;
                    }
                    pulses += event.kind == tactical::EventKind::special_weapon_fired ? 1U : 0U;
                }
                for (const auto& event : snapshot.combat_events()) {
                    if (event.kind != tactical::CombatEventKind::weapon_fired || event.shooter != 1) continue;
                    ++shots;
                    const auto muzzle = math::transform_point(drawn, gun.fire_a).value();
                    expect(math::length(math::Vec3{Fixed::from_raw(muzzle.x.raw() - event.origin.x.raw()),
                        Fixed::from_raw(muzzle.y.raw() - event.origin.y.raw()),
                        Fixed::from_raw(muzzle.z.raw() - event.origin.z.raw())}).value().raw() <= 64,
                        "WWP-73/74: commanded first and later releases leave the drawn banked gun");
                    expect(std::any_of(snapshot.projectiles().begin(), snapshot.projectiles().end(),
                        [&](const auto& projectile) { return projectile.shooter == 1 && projectile.position == event.origin; }),
                        "WWP-74: commanded projectile starts at its banked release");
                }
            }
            expect(accepted == (route == 2 ? 0U : 1U) && rejected == (route == 2 ? 1U : 0U)
                && shots == (planetary ? 2U : route == 1 ? 1U : 0U) && pulses == (planetary ? 2U : 0U),
                "WWP-73: planetary pulses and manual admission agree with banked combat firing");
            if (reference.empty()) reference = hashes;
            expect(hashes == reference, "WWP-73/74: command and staged snapshots agree on 1/2/4/8 workers");
        }
    }
}

void test_attack_turn() {
    test_banked_fire();
    test_banked_command_and_staged_frames();
    const auto fired_at = [](const std::vector<Shot>& shots, const eawr::sim::EntityId target) {
        return std::any_of(shots.begin(), shots.end(), [&](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.target == target;
        });
    };
    // A bomber on the starboard quarter, outside the 45-degree forward cone: -170.54 degrees.
    const auto quarter = [] {
        return setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)});
    };
    const double bearing = -180.0 + 9.462322208025617;
    // Idle (A-05): an acquired target does not turn the unit, and nothing fires.
    auto idle = turning_session(quarter());
    expect(run(idle, 120).empty() && yaw_of(idle, 1) == 0.0, "idle: the unit keeps its heading and holds fire");
    // Auto_Rotate_For_Space_Targeting set (A-05): the scanned target turns the unit as an order would.
    auto rotating = table();
    rotating.auto_rotate_for_targeting = true;
    auto scanned = turning_session(quarter(), rotating);
    const auto scanned_shots = run(scanned, 300);
    expect(std::abs(yaw_of(scanned, 1) - bearing) < 1e-3 && fired_at(scanned_shots, 2),
        "A-05: with Auto_Rotate_For_Space_Targeting an idle unit turns to its scanned target and fires");
    // Ordered (A-04): the order of tick 0 reaches targeting at tick 1; the turn starts at frame 2
    // and first moves the unit at frame 3, the short way at 0.75 degrees per frame.
    auto ordered = turning_session(quarter());
    expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(ordered, 2));
    expect(yaw_of(ordered, 1) == 0.0, "ordered: the heading is unchanged through frame 2");
    const auto plan = ordered.motion_state(1);
    expect(plan && plan->kind == tactical::MotionKind::turn && plan->start_tick == 2, "ordered: a turn in place from frame 2");
    static_cast<void>(run(ordered, 1));
    expect(std::abs(yaw_of(ordered, 1) + 0.75) < 1e-4, "ordered: frame 3 turns 0.75 degrees to starboard");
    const auto shots = run(ordered, 300);
    expect(std::abs(yaw_of(ordered, 1) - bearing) < 1e-3, "ordered: the turn ends on the bearing to the target");
    expect(fired_at(shots, 2), "ordered: the forward weapon fires once the target is ahead");
    expect(ordered.units().front().position == at(0, 0), "ordered: the unit turns without moving");
    // Within 10 degrees (A-07) the unit does not turn; the target is outside the 45-degree cone's
    // half width only when more than 22.5 degrees off, so it also fires.
    auto near = turning_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(300, 50), true)}));
    expect(static_cast<bool>(near.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const auto near_shots = run(near, 120);
    expect(yaw_of(near, 1) == 0.0 && fired_at(near_shots, 2), "within 10 degrees: no turn, the weapon fires");
    // Beyond Targeting_Max_Attack_Distance the unit does not turn in place: it closes on the target
    // (space-orders OR-05, #452) with a move to the slot 900 units short of it.
    auto far = turning_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-1200, -50), true)}));
    expect(static_cast<bool>(far.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(far, 1));
    const auto closing = far.motion_state(1);
    expect(closing && closing->kind == tactical::MotionKind::path && closing->start_tick == 1,
        "out of attack range: a move from frame 1, not a turn in place");
    expect(closing && std::abs(static_cast<double>(closing->target.x.raw()) / one + 300.8) < 0.5
            && std::abs(static_cast<double>(closing->target.y.raw()) / one + 12.5) < 0.5,
        "out of attack range: the slot 900 units short of the target");
    // A-06: a port broadside with more AI combat power than the forward weapon puts the target
    // on the left beam: heading = bearing - 90 = 99.46 degrees.
    auto broadside = table();
    auto port = ion();
    port.hardpoint = 1;
    port.cone_width = units(175);
    port.fire_axes = std::array<math::Vec3, 3>{at(0, 1), at(-1, 0), at(0, 0, 1)};
    port.ai_combat_power = units(2);
    port.shot = tactical::ShotProfile{units(10), tactical::no_type_index, units(20), units(700), true, true, {}};
    broadside.profiles[0].weapons[0].ai_combat_power = units(1);
    broadside.profiles[0].weapons[0].shot = port.shot;
    broadside.profiles[0].weapons.push_back(port);
    broadside.profiles[0].hardpoints.push_back({1, at(0, 5), true});
    auto beam = turning_session(quarter(), broadside);
    expect(static_cast<bool>(beam.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(beam, 200));
    expect(std::abs(yaw_of(beam, 1) - (bearing - 90.0 + 360.0)) < 1e-3, "broadside: the port side turns to the target");
    // A projectile that does no hull damage does not count: the forward weapon wins again.
    auto harmless = broadside;
    harmless.profiles[0].weapons[1].shot->hitpoint_damage = false;
    auto ahead = turning_session(quarter(), harmless);
    expect(static_cast<bool>(ahead.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(ahead, 300));
    expect(std::abs(yaw_of(ahead, 1) - bearing) < 1e-3, "no hull damage: the bow turns to the target");
    // The same turn and fire at 1, 2, 4 and 8 workers.
    std::vector<std::string> finals;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto value = turning_session(quarter(), broadside);
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int tick = 0; tick < 240; ++tick) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "worker step succeeds");
            if (!stepped) break;
        }
        finals.push_back(value.state_sha256());
    }
    expect(finals.size() == 4 && std::all_of(finals.begin(), finals.end(), [&](const std::string& hash) { return hash == finals[0]; }),
        "attack turn: 1, 2, 4 and 8 workers end on the same state");
}

// The turn motion with the bomber (fast, to leave the attack distance within a few frames) and the
// hardpoint-less gunship able to move too.
[[nodiscard]] tactical::MotionTable turning_motion_all() {
    auto motion = turning_motion();
    tactical::MotionProfile bomber;
    bomber.type_id = bomber_type;
    bomber.max_speed = units(80);
    bomber.acceleration = units(80);
    bomber.deceleration = units(80);
    bomber.rate_of_turn = units(90);
    bomber.turn_in_place_slowdown = units(1);
    tactical::MotionProfile gunship = motion.profiles.front();
    gunship.type_id = gunship_type;
    motion.profiles.push_back(bomber);
    motion.profiles.push_back(gunship);
    return motion;
}

[[nodiscard]] tactical::TacticalSession turning_session_all(const tactical::TacticalSetup& value,
    const tactical::CombatTable& combat = table(), const tactical::DurabilityTable& durability = {}) {
    auto created = tactical::TacticalSession::create(value, sensors(), durability, turning_motion_all(), std::nullopt, combat);
    expect(static_cast<bool>(created), "turning session is created");
    return std::move(created).value();
}

// A weapon on hardpoint `hardpoint` whose fire bone's x axis points to `side` (+1 port, -1
// starboard), covering only that beam, with hull damage and the given AI combat power.
[[nodiscard]] tactical::WeaponProfile beam_weapon(const std::uint32_t hardpoint, const std::int64_t side, const std::int64_t power) {
    auto weapon = ion();
    weapon.hardpoint = hardpoint;
    weapon.cone_width = units(175);
    weapon.fire_axes = std::array<math::Vec3, 3>{at(0, side), at(-side, 0), at(0, 0, 1)};
    weapon.ai_combat_power = units(power);
    weapon.shot = tactical::ShotProfile{units(10), tactical::no_type_index, units(20), units(700), true, true, {}};
    return weapon;
}

void test_attack_turn_edges() {
    // --- A-06 tie: nothing ahead, equal port and starboard power. FoC keeps +90 unless turning the
    // left beam onto the target (|90 - d|) is less than facing it (|d|), d the relative bearing.
    auto tie = table();
    tie.profiles[0].weapons = {beam_weapon(0, -1, 1), beam_weapon(1, 1, 1)};
    tie.profiles[0].hardpoints = {{0, at(0, -5), true}, {1, at(0, 5), true}};
    const auto tie_turn = [&](const math::Vec3 target) {
        auto value = turning_session_all(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, target, true)}), tie);
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
        static_cast<void>(run(value, 300));
        return yaw_of(value, 1);
    };
    // 20.07 degrees to the left: 69.93 is not less than 20.07, so +90 (heading 110.07), although
    // the left beam is the nearer one.
    const double left20 = std::atan2(103.0, 282.0) * 180.0 / 3.14159265358979323846;
    expect(std::abs(tie_turn(at(282, 103)) - (left20 + 90.0)) < 1e-3, "tie, target 20 degrees left: +90");
    // 60.02 degrees to the left: 29.98 is less than 60.02, so -90 (heading -29.98).
    const double left60 = std::atan2(260.0, 150.0) * 180.0 / 3.14159265358979323846;
    expect(std::abs(tie_turn(at(150, 260)) - (left60 - 90.0)) < 1e-3, "tie, target 60 degrees left: -90");

    // --- Every weapon hardpoint destroyed: no side has power, the adjustment is 0 and the bow
    // turns to the target (with the port broadside alive it would be -90).
    const double quarter_bearing = -180.0 + 9.462322208025617;
    auto broadside = table();
    broadside.profiles[0].weapons[0].ai_combat_power = units(1);
    broadside.profiles[0].weapons[0].shot = beam_weapon(1, 1, 2).shot;
    broadside.profiles[0].weapons.push_back(beam_weapon(1, 1, 2));
    broadside.profiles[0].hardpoints.push_back({1, at(0, 5), true});
    tactical::DurabilityTable durability;
    tactical::HardpointProfile weapon_hardpoint;
    weapon_hardpoint.role = tactical::HardpointRole::weapon;
    weapon_hardpoint.destroyable = true;
    weapon_hardpoint.max_health = units(100);
    durability.profiles.push_back({shooter_type, units(1000), units(3), false, {weapon_hardpoint, weapon_hardpoint}});
    const auto quarter = [] {
        return setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)});
    };
    auto intact = turning_session_all(quarter(), broadside, durability);
    expect(static_cast<bool>(intact.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(intact, 300));
    expect(std::abs(yaw_of(intact, 1) - (quarter_bearing - 90.0 + 360.0)) < 1e-3, "intact broadside: the port side turns");
    auto wrecked = turning_session_all(quarter(), broadside, durability);
    expect(static_cast<bool>(wrecked.submit({{0, 1, 0}, {1}, tactical::DamagePayload{units(100), 0}})), "damage is queued");
    expect(static_cast<bool>(wrecked.submit({{0, 1, 1}, {1}, tactical::DamagePayload{units(100), 1}})), "damage is queued");
    expect(static_cast<bool>(wrecked.submit({{0, 1, 2}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const auto wrecked_shots = run(wrecked, 300);
    expect(std::abs(yaw_of(wrecked, 1) - quarter_bearing) < 1e-3, "all weapon hardpoints destroyed: the bow turns");
    expect(std::none_of(wrecked_shots.begin(), wrecked_shots.end(), [](const Shot& shot) {
        return shot.kind == tactical::CombatEventKind::weapon_fired;
    }), "all weapon hardpoints destroyed: nothing fires");

    // --- A type without hardpoints (the gunship, an object weapon only) turns toward the target
    // it scans for itself, without an order, straight at the bearing.
    auto gunship = turning_session_all(setup({unit(1, gunship_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)}));
    static_cast<void>(run(gunship, 300));
    expect(gunship.combat_state(1)->attack_target == 2 && !gunship.combat_state(1)->direct,
        "hardpoint-less: the scan's target, not an order");
    expect(std::abs(yaw_of(gunship, 1) - quarter_bearing) < 1e-3, "hardpoint-less: it turns to face the bearing");

    // --- Orders during the turn. The ordered turn to the quarter lasts from frame 3 to about 230.
    const auto ordered = [&](std::vector<tactical::UnitState> extra = {}) {
        std::vector<tactical::UnitState> all{unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(-300, -50), true)};
        all.insert(all.end(), extra.begin(), extra.end());
        auto value = turning_session_all(setup(std::move(all)));
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
        static_cast<void>(run(value, 60));
        expect(yaw_of(value, 1) < -40.0 && yaw_of(value, 1) > -50.0, "mid-turn: about 43 degrees round at frame 60");
        return value;
    };
    // A face order replaces the turn and ends the attack (A-03): the unit ends facing the point.
    auto faced = ordered();
    expect(static_cast<bool>(faced.submit({{60, 1, 1}, {1}, tactical::FacePayload{at(0, 300)}})), "face order is queued");
    static_cast<void>(run(faced, 300));
    expect(std::abs(yaw_of(faced, 1) - 90.0) < 1e-3, "face mid-turn: the unit faces the ordered point");
    expect(!faced.combat_state(1)->direct, "face mid-turn: the attack order ends");
    // A new attack order retargets but the turn under way completes; the unit is checked again
    // at rest and then turns to the new target.
    auto retarget = ordered({unit(3, bomber_type, 2, at(0, 300), true)});
    expect(static_cast<bool>(retarget.submit({{60, 1, 1}, {1}, tactical::AttackPayload{3}})), "second attack order is queued");
    static_cast<void>(run(retarget, 100));
    expect(retarget.combat_state(1)->attack_target == 3 && retarget.combat_state(1)->direct, "attack mid-turn: the new target");
    // Frame 160: 118.5 degrees round, still turning away from the new target (bearing 90).
    expect(std::abs(yaw_of(retarget, 1) + 118.5) < 1e-3, "attack mid-turn: the first turn carries on");
    static_cast<void>(run(retarget, 400));
    expect(std::abs(yaw_of(retarget, 1) - 90.0) < 1e-3, "attack mid-turn: then it turns to the new target");
    // The target dies mid-turn: the target clears, the turn under way completes and nothing more.
    auto killed = ordered();
    expect(static_cast<bool>(killed.stage_remove(2)), "the target is removed");
    static_cast<void>(run(killed, 300));
    expect(killed.combat_state(1)->attack_target == 0, "target dies mid-turn: the target clears");
    expect(std::abs(yaw_of(killed, 1) - quarter_bearing) < 1e-3, "target dies mid-turn: the turn completes");
    // The target leaves the attack distance mid-turn and stays in sensor range (it flies to
    // (-300, 1800)); a target that leaves sensor range is dropped (T-01) and not followed.
    auto fled = ordered();
    expect(static_cast<bool>(fled.submit({{60, 2, 0}, {2}, tactical::MovePayload{at(-300, 1800)}})), "the target's move is queued");
    static_cast<void>(run(fled, 300));
    const auto fled_units = fled.units();
    const auto target_state = std::find_if(fled_units.begin(), fled_units.end(),
        [](const tactical::UnitState& state) { return state.entity_id == 2; });
    expect(target_state != fled_units.end() && target_state->position.y > units(1700), "the target has left the attack distance");
    // Within ten frames of it leaving, the unit plans an approach (space-orders OR-06, #452) and
    // closes to its slot, inside the attack distance.
    const auto chasing = fled.motion_state(1);
    expect(chasing && chasing->kind == tactical::MotionKind::path, "target leaves range: the unit closes on it");
    static_cast<void>(run(fled, 900));
    const auto closed = fled.units();
    const auto shooter_state = std::find_if(closed.begin(), closed.end(),
        [](const tactical::UnitState& state) { return state.entity_id == 1; });
    const auto dx = static_cast<double>(shooter_state->position.x.raw() - target_state->position.x.raw()) / one;
    const auto dy = static_cast<double>(shooter_state->position.y.raw() - target_state->position.y.raw()) / one;
    expect(std::hypot(dx, dy) <= 1000.0 && std::hypot(dx, dy) > 850.0 && fled.motion_state(1)->kind != tactical::MotionKind::path,
        "target leaves range: the unit stops at its slot, inside the attack distance");
}

// WCC-18 (walk 2 G-6): the best hardpoint of a target. With a priority set the candidates are the
// standing destroyable hardpoints, targetable or not, ranked by Hard_Point_Priorities with
// Hard_Point_Exclusions skipped, then strictly nearer (3D, from the height-adjusted attacker), then
// first in HardPoints order. Without a set: the nearest standing targetable hardpoint.
void test_best_hardpoint() {
    constexpr std::uint8_t laser = 1; // units::HardpointType ordinals
    constexpr std::uint8_t shield = 5;
    constexpr std::uint8_t engine = 6;
    const auto unlisted = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    tactical::CombatTable table;
    table.priority_sets.resize(1);
    auto& set = table.priority_sets[0];
    set.unlisted = unlisted;
    set.hardpoint_ranks.assign(14, unlisted);
    set.hardpoint_ranks[shield] = units(1);
    set.hardpoint_ranks[engine] = units(2);
    tactical::CombatProfile attacker;
    attacker.priority_set = 0;
    tactical::CombatProfile target_profile;
    target_profile.hardpoints = {{0, at(-10, 0), true, laser}, {1, at(0, 30), true, engine},
        {2, at(40, 0), false, shield}, {3, at(-20, 0), true, shield}};
    tactical::DurabilityProfile health;
    for (const bool destroyable : {true, true, true, false}) {
        health.hardpoints.push_back({tactical::HardpointRole::other, destroyable, units(100), {}, {}});
    }
    tactical::DurabilityState hit;
    hit.hull = units(100);
    hit.hardpoints = {units(100), units(100), units(100), Fixed{}};
    tactical::detail::CombatUnit target;
    target.id = 2;
    target.position = at(200, 0);
    target.transform = math::to_matrix(math::identity_quat(), target.position).value();
    target.profile = &target_profile;
    target.durability_profile = &health;
    target.durability = &hit;
    const auto best = [&](const tactical::CombatProfile* who) {
        const auto* chosen = tactical::detail::best_hardpoint(&table, who, target, at(0, 0));
        return chosen != nullptr ? chosen->hardpoint : tactical::no_hardpoint;
    };
    expect(best(&attacker) == 2,
        "WCC-18: the first-listed type wins, targetable or not; a non-destroyable one is no candidate");
    expect(tactical::detail::ordered_aim_point(&table, &attacker, target, at(0, 0)) == at(240, 0),
        "WCC-19: the A-04 and approach aim point is the best hardpoint");
    expect(tactical::detail::ordered_aim_point(&table, &attacker, target, at(0, 0), 1) == at(200, 30),
        "OR-25: an ordered standing hardpoint still wins over the best one");
    hit.hardpoints[2] = Fixed{};
    expect(best(&attacker) == 1,
        "WCC-18: a destroyed hardpoint is skipped; the next-listed type wins over a nearer unlisted one");
    set.hardpoint_ranks[engine].reset();
    expect(best(&attacker) == 0, "WCC-18: an excluded type is skipped; an unlisted one ranks after the listed ones");
    set.hardpoint_ranks.assign(14, unlisted);
    hit.hardpoints[2] = units(100);
    expect(best(&attacker) == 0, "WCC-18: a set listing no hardpoints takes the nearest destroyable hardpoint");
    expect(best(nullptr) == 3, "WCC-18: without a priority set, the nearest standing targetable hardpoint");
    const tactical::CombatProfile no_set;
    expect(best(&no_set) == 3, "WCC-18: an attacker type without a set takes the nearest targetable hardpoint");
    set.hardpoint_ranks.clear();
    expect(best(&attacker) == 0, "WCC-18: a type past the rank table ranks unlisted");

    // Ties: equal rank and distance go to the first in HardPoints order; the attacker's
    // Ranged_Target_Z_Adjust raises the point the 3D distance starts from.
    target_profile.hardpoints = {{0, at(0, 10, -5), true, laser}, {1, at(0, -10, 5), true, laser}};
    target.durability_profile = nullptr;
    target.durability = nullptr;
    expect(best(&attacker) == 0, "WCC-18: an exact tie goes to the first hardpoint");
    attacker.ranged_target_z_adjust = units(5);
    expect(best(&attacker) == 1, "WCC-18: the distance starts at the attacker's height-adjusted position");
    attacker.ranged_target_z_adjust = Fixed{};

    // The weapon aim (OR-24, G-W3): an unordered hardpoint is the shooter's best one.
    target_profile.hardpoints = {{0, at(-10, 0), true, laser}, {1, at(0, 30), true, engine},
        {2, at(40, 0), false, shield}, {3, at(-20, 0), true, shield}};
    target.durability_profile = &health;
    target.durability = &hit;
    set.hardpoint_ranks.assign(14, unlisted);
    set.hardpoint_ranks[shield] = units(1);
    tactical::detail::CombatWorld world;
    const std::vector<tactical::Player> owners{{1, 1, 1, tactical::player_flag_commandable},
        {2, 2, 2, tactical::player_flag_commandable}};
    const std::vector<tactical::SnapshotPlayer> relationships{{1, 1, false}, {2, 2, false}};
    world.players = owners;
    world.relationships = relationships;
    world.table = &table;
    auto weapon = ion();
    weapon.hardpoint = tactical::object_weapon;
    weapon.cone_width = weapon.cone_height = units(360);
    weapon.has_fire_b = false;
    weapon.fire_a = {};
    weapon.pulse_count = 1;
    attacker.weapons = {weapon};
    tactical::CombatState state;
    state.attack_target = 2;
    state.direct = true;
    state.weapons.resize(1);
    tactical::detail::CombatUnit self;
    self.id = 1;
    self.owner = self.team = 1;
    self.profile = &attacker;
    self.combat = &state;
    self.transform = math::identity_matrix();
    target.owner = target.team = 2;
    target.visible_to = 1;
    target.previous_position = target.position;
    world.units = {self, target};
    const auto shoot = [&](const std::uint64_t frame) -> std::optional<tactical::CombatEvent> {
        state.weapons[0].countdown = 0;
        state.weapons[0].pulses_left = 1;
        world.frame = frame;
        const auto step = tactical::detail::step_combat(world, world.units.front());
        expect(static_cast<bool>(step), "WCC-18: the attacker's step succeeds");
        if (!step) return std::nullopt;
        state = step.value().state;
        if (step.value().events.empty()) return std::nullopt;
        return step.value().events.front();
    };
    const auto ranked = shoot(10);
    expect(ranked && ranked->target_hardpoint == 2 && ranked->aim == at(240, 0),
        "WCC-18: the weapon aims at the listed shield generator, though it is not targetable");
    attacker.priority_set.reset();
    const auto nearest = shoot(11);
    expect(nearest && nearest->target_hardpoint == 3 && nearest->aim == at(180, 0),
        "WCC-18: without a set the weapon aims at the nearest targetable hardpoint");

    // A-05 + WCC-19: a scanned unit with a weapon hardpoint turns only with auto-rotate,
    // and its facing uses WCC-18 even when the best hardpoint is not the nearest targetable one.
    attacker.priority_set = 0;
    attacker.max_attack_distance = units(1000);
    attacker.weapons[0].hardpoint = 0;
    set.rows = {{target.type_id, units(1)}};
    world.units.front().can_turn = true;
    world.units.back().position = world.units.back().previous_position = at(200, 200);
    world.units.back().transform = math::to_matrix(math::identity_quat(), at(200, 200)).value();
    world.index = tactical::SpaceIndex::build(std::array{
        tactical::SpaceBody{2, 2, at(200, 200)}}).value();
    const auto scanned_turn = [&] {
        state = tactical::CombatState{};
        state.weapons.resize(1);
        world.frame = 20;
        return tactical::detail::step_combat(world, world.units.front());
    };
    table.auto_rotate_for_targeting = false;
    const auto held_heading = scanned_turn();
    expect(held_heading && held_heading.value().state.attack_target == 2
            && !held_heading.value().state.direct && !held_heading.value().face,
        "A-05: a hardpoint unit scans the target but keeps its heading without auto-rotate");
    table.auto_rotate_for_targeting = true;
    const auto best_heading = scanned_turn();
    expect(best_heading && best_heading.value().state.attack_target == 2
            && !best_heading.value().state.direct && best_heading.value().face == at(240, 200),
        "A-05/WCC-19: a scanned hardpoint unit faces the ranked shield, not the nearer hardpoint or centre");
    attacker.priority_set.reset();
    const auto nearest_heading = scanned_turn();
    expect(nearest_heading && nearest_heading.value().face == at(180, 200),
        "A-05/WCC-19: without a priority set the scanned unit faces the nearest targetable hardpoint");
}

// --- Determinism and the golden pin -----------------------------------------------------------


} // namespace combat_test_support
