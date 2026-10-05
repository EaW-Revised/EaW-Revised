#include "combat_support.hpp"

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

void test_attack_turn() {
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

// --- Determinism and the golden pin -----------------------------------------------------------


} // namespace combat_test_support
