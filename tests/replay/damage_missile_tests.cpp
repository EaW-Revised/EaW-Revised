#include "damage_support.hpp"

namespace damage_test_support {

// --- Missiles (MS-01 to MS-07, #361) ------------------------------------------------------------

// A corvette that crawls at 3 units per frame, like FoC's corvettes.
[[nodiscard]] tactical::MotionTable crawling_motion() {
    tactical::MotionTable motion;
    motion.rules = {units(15), units(300)};
    tactical::MotionProfile profile;
    profile.type_id = corvette_type;
    profile.max_speed = units(3);
    profile.acceleration = units(3);
    profile.deceleration = units(3);
    profile.rate_of_turn = units(90);
    profile.turn_in_place_slowdown = units(1);
    motion.profiles.push_back(profile);
    return motion;
}

// A concussion-missile-like hardpoint: one shot a second at 7 units per frame, travel 1400, turning
// 3 degrees a frame when it homes.
[[nodiscard]] tactical::CombatTable missile_combat(const bool homing) {
    auto table = combat(1400);
    auto& weapon = table.profiles[0].weapons.front();
    weapon.pulse_count = 1;
    weapon.min_recharge_hundredths = 100;
    weapon.max_recharge_hundredths = 100;
    weapon.shot->speed = units(7);
    weapon.shot->homing = homing;
    weapon.shot->turn_rate = homing ? units(3) : Fixed{};
    return table;
}

struct MissileRun {
    std::size_t shots{};
    std::size_t hits{};
    bool turn_bounded{true};
    bool homing_seen{};
};

// With `weave`, the corvette (unit 2) turns back every 60 ticks between north and south.
[[nodiscard]] MissileRun missile_run(tactical::TacticalSession& value, const std::uint64_t ticks, const bool weave = false) {
    MissileRun result;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        if (weave && tick > 0 && tick % 60 == 0) {
            const auto north = (tick / 60) % 2 == 0;
            expect(static_cast<bool>(value.submit({{tick, 2, tick}, {2}, tactical::MovePayload{at(600, north ? 1500 : -1500)}})),
                "the corvette's turn is queued");
        }
        std::vector<tactical::Projectile> before(value.projectiles().begin(), value.projectiles().end());
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "missile step succeeds");
        if (!stepped) break;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            result.shots += event.kind == tactical::CombatEventKind::weapon_fired;
            result.hits += event.kind == tactical::CombatEventKind::projectile_hit;
        }
        for (const auto& after : value.projectiles()) {
            const auto earlier = std::find_if(before.begin(), before.end(),
                [&](const tactical::Projectile& entry) { return entry.id == after.id; });
            if (earlier == before.end()) continue;
            result.homing_seen = result.homing_seen || after.homing;
            const auto turn = [](const Fixed a, const Fixed b) {
                auto d = std::fmod(static_cast<double>(a.raw() - b.raw()) / static_cast<double>(one), 360.0);
                if (d > 180.0) d -= 360.0;
                if (d <= -180.0) d += 360.0;
                return std::abs(d);
            };
            result.turn_bounded = result.turn_bounded && turn(after.yaw, earlier->yaw) <= 3.0 + 1e-6
                && turn(after.pitch, earlier->pitch) <= 3.0 + 1e-6;
        }
    }
    return result;
}

[[nodiscard]] tactical::TacticalSession crawling_session(const bool homing) {
    auto staged = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(600, 0), true)});
    auto created = tactical::TacticalSession::create(
        staged, sensors(), durability(), crawling_motion(), std::nullopt, missile_combat(homing));
    expect(static_cast<bool>(created), "missile session is created");
    auto value = std::move(created).value();
    expect(static_cast<bool>(value.submit({{0, 2, 0}, {2}, tactical::MovePayload{at(600, 1500)}})),
        "the corvette's move is queued");
    return value;
}

// MS-01 to MS-04: a missile fired at a corvette weaving across its path turns after it, at most 3
// degrees a frame, and hits; the same shots flown straight (MS-06) lead only the corvette's last
// move (W-10), so they miss when it turns back.
void test_missile_homing() {
    auto homing = crawling_session(true);
    const auto steered = missile_run(homing, 300, true);
    auto straight = crawling_session(false);
    const auto flown = missile_run(straight, 300, true);
    std::cout << "MS: homing missiles hit " << steered.hits << " of " << steered.shots << ", straight ones "
              << flown.hits << " of " << flown.shots << '\n';
    expect(steered.homing_seen && steered.turn_bounded, "MS-04: a missile turns at most its Max_Rate_Of_Turn a frame");
    expect(steered.shots > 3 && steered.hits * 2 >= steered.shots,
        "MS-03: homing missiles hit a weaving corvette (" + std::to_string(steered.hits) + " of "
            + std::to_string(steered.shots) + ")");
    expect(flown.hits * 2 < steered.hits, "MS-06: projectiles that do not home miss a turning corvette ("
            + std::to_string(flown.hits) + " hits)");
}

// MS-05: when the target leaves, a missile keeps its facing and never takes another target, even
// with a hostile unit off its bow; it flies out its travel.
void test_missile_lock_loss() {
    auto staged = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, corvette_type, 2, at(600, 0), true),
        unit(3, frigate_type, 2, at(300, 250), true)});
    // The corvette is the intended first lock; the frigate is only the decoy for the lock-loss
    // check, so a priority set picks it over the frigate regardless of the collection order
    // (#469, space-targeting CO-11: an equal-priority tie is order-dependent). The frigate's
    // priority stays off 1.0 (R-09's "no more searching needed" sentinel), or the search could
    // stop on it before ever weighing the corvette.
    auto table = missile_combat(true);
    tactical::PrioritySet priorities;
    priorities.rows = {{frigate_type, decimal("0.5")}, {corvette_type, units(0)}};
    priorities.unlisted = decimal("0.5");
    table.priority_sets.push_back(priorities);
    table.profiles[0].priority_set = static_cast<std::uint32_t>(table.priority_sets.size() - 1);
    auto created = tactical::TacticalSession::create(staged, sensors(), durability(), {}, std::nullopt, table);
    expect(static_cast<bool>(created), "lock-loss session is created");
    if (!created) return;
    auto value = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    std::optional<tactical::Projectile> first;
    for (int tick = 0; tick < 200 && !first; ++tick) {
        static_cast<void>(value.step(executor));
        for (const auto& projectile : value.projectiles()) {
            if (projectile.target == 2) {
                first = projectile;
                break;
            }
        }
    }
    expect(first.has_value() && first->locked, "MS-02: the missile flies locked on the corvette");
    if (!first) return;
    expect(static_cast<bool>(value.stage_remove(2)), "the corvette is removed");
    std::optional<tactical::Projectile> last;
    bool straight = true;
    for (int tick = 0; tick < 60; ++tick) {
        static_cast<void>(value.step(executor));
        const auto flying = value.projectiles();
        const auto found = std::find_if(flying.begin(), flying.end(),
            [&](const tactical::Projectile& entry) { return entry.id == first->id; });
        if (found == flying.end()) break;
        if (last) straight = straight && found->yaw == last->yaw && found->pitch == last->pitch && found->step == last->step;
        last = *found;
    }
    expect(last.has_value() && !last->locked && straight, "MS-05: without its target the missile flies straight on");
}

// --- Determinism and the golden pin -----------------------------------------------------------

} // namespace damage_test_support
