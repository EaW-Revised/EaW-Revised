#include "combat_support.hpp"

namespace combat_test_support {

void test_order_without_attack_distance() {
    // T-01: a unit without Targeting_Max_Attack_Distance still drops an ordered target that has
    // left the session, and chooses none of its own afterwards.
    auto combat = table();
    combat.profiles[0].max_attack_distance.reset();
    auto value = session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, fighter_type, 2, at(300, 20), true),
                             unit(3, bomber_type, 2, at(420, -30), true)}),
        combat);
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(value, 30));
    const auto ordered = value.combat_state(1);
    expect(ordered && ordered->attack_target == 2 && ordered->direct,
        "no attack distance: the attack order still sets the direct target");
    expect(static_cast<bool>(value.stage_remove(2)), "the ordered target is removed");
    static_cast<void>(run(value, 2));
    const auto dropped = value.combat_state(1);
    expect(dropped && dropped->attack_target == 0 && !dropped->direct,
        "no attack distance: a removed ordered target is dropped");
    static_cast<void>(run(value, 90));
    expect(value.combat_state(1)->attack_target == 0, "no attack distance: no ship-level target of its own");
}

// --- Orders: closing on a target, attack-move and guard (#452, docs/behaviour/space-orders.md) ---

// The shooter (top speed 3, attack distance `attack`) and the bomber and gunship with motion; the
// sensors reach 5000 so a far target stays visible (T-01 drops a fogged one, OR-08).
[[nodiscard]] tactical::TacticalSession orders_session(
    const tactical::TacticalSetup& value, const std::int64_t attack = 1000, const std::int64_t sensor_range = 5000) {
    auto combat = table();
    combat.profiles[0].max_attack_distance = units(attack);
    auto created = tactical::TacticalSession::create(value, sensors(sensor_range), {}, turning_motion_all(), std::nullopt, combat);
    expect(static_cast<bool>(created), "orders session is created");
    return std::move(created).value();
}

[[nodiscard]] tactical::UnitState state_of(const tactical::TacticalSession& value, const eawr::sim::EntityId id) {
    for (const auto& state : value.units()) {
        if (state.entity_id == id) return state;
    }
    return {};
}

[[nodiscard]] double planar(const math::Vec3& a, const math::Vec3& b) {
    return std::hypot(static_cast<double>(a.x.raw() - b.x.raw()) / one, static_cast<double>(a.y.raw() - b.y.raw()) / one);
}

[[nodiscard]] double coordinate(const Fixed value) { return static_cast<double>(value.raw()) / one; }

// Steps `ticks` frames and returns every order event.
std::vector<tactical::Event> run_events(tactical::TacticalSession& value, const std::uint64_t ticks) {
    std::vector<tactical::Event> result;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t index = 0; index < ticks; ++index) {
        auto stepped = value.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        const auto events = stepped.value().snapshot->events();
        result.insert(result.end(), events.begin(), events.end());
    }
    return result;
}

void test_orders_approach() {
    // AT-10 / OR-03 / W-05: centre admission uses the facing-selected hard extent;
    // a hardpoint aim uses the soft radius. Test the inclusive boundary and one raw
    // unit outside it, separately from the shorter mapping slot.
    const auto footprint_session = [](const math::Vec3 position, const bool hardpoint) {
        auto combat = table();
        if (hardpoint) combat.profiles[2].hardpoints = {{0, at(-50, 0), true}};
        auto motion = turning_motion_all();
        tactical::Footprint footprint;
        footprint.type_id = bomber_type;
        footprint.x_extent = units(200);
        footprint.y_extent = units(50);
        footprint.radius = units(100);
        motion.footprints = {footprint};
        auto created = tactical::TacticalSession::create(
            setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, position)}),
            sensors(5000), {}, motion, std::nullopt, combat);
        expect(static_cast<bool>(created), "range fixture builds");
        return std::move(created).value();
    };
    for (const bool hardpoint : {false, true}) {
        for (const bool outside : {false, true}) {
            auto point = at(hardpoint ? 1150 : 1200, 0);
            if (outside) point.x = Fixed::from_raw(point.x.raw() + 1);
            auto value = footprint_session(point, hardpoint);
            expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "range order queued");
            static_cast<void>(run(value, 1));
            expect(value.motion_state(1)->kind == (outside ? tactical::MotionKind::path : tactical::MotionKind::none),
                "AT-10: bow hard extent and hardpoint soft radius are inclusive to one raw unit");
        }
    }
    auto beam = footprint_session(at(0, 1050), false);
    expect(static_cast<bool>(beam.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "beam order queued");
    static_cast<void>(run(beam, 1));
    expect(beam.motion_state(1)->kind == tactical::MotionKind::none, "AT-10: beam admission adds Y, not X");
    for (const bool outside : {false, true}) {
        auto point = at(805, 0); // fire midpoint X=5, range=700, target soft radius=100
        if (outside) point.x = Fixed::from_raw(point.x.raw() + 1);
        auto value = footprint_session(point, false);
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "fire range order queued");
        const auto shots = run(value, 90);
        const auto fired = std::any_of(shots.begin(), shots.end(), [](const Shot& shot) {
            return shot.kind == tactical::CombatEventKind::weapon_fired && shot.shooter == 1 && shot.target == 2;
        });
        expect(fired == !outside, "W-05: final firing admission includes the soft radius, inclusively");
    }

    // C-01 (OR-03, OR-05): attack distance 600 (inside the 700-unit ion weapon), a stationary enemy
    // 3000 ahead. The order at tick 0 plans a move from frame 1 to the slot 0.9 x 600 = 540 short.
    auto closing = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}), 600);
    expect(static_cast<bool>(closing.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(closing, 1));
    const auto plan = closing.motion_state(1);
    expect(plan && plan->kind == tactical::MotionKind::path && plan->start_tick == 1
            && std::abs(coordinate(plan->target.x) - 2460.0) < 1e-3 && plan->target.y.raw() == 0,
        "C-01: a move from frame 1 to the slot (2460, 0)");
    std::uint64_t arrived = 0;
    bool fired_after = false;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 1; tick < 1400; ++tick) {
        auto stepped = closing.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        const auto motion = closing.motion_state(1);
        if (arrived == 0 && motion && motion->kind != tactical::MotionKind::path) arrived = tick;
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            fired_after = fired_after || (arrived != 0 && event.kind == tactical::CombatEventKind::weapon_fired
                && event.shooter == 1 && event.target == 2);
        }
    }
    const auto stopped = state_of(closing, 1);
    expect(arrived != 0 && std::abs(coordinate(stopped.position.x) - 2460.0) < 1.0 && std::abs(coordinate(stopped.position.y)) < 1e-3,
        "C-01: it stops on the slot, inside its attack distance");
    expect(fired_after, "C-01: once stopped it fires at its target");
    expect(closing.combat_state(1)->attack_target == 2 && closing.combat_state(1)->direct, "C-01: the order stands");

    // C-02 (OR-05): in range at the order, the unit holds: no plan, no approach mapping.
    auto near = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(500, 0), true)}));
    const auto before = near.state_sha256();
    expect(static_cast<bool>(near.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(near, 30));
    expect(near.motion_state(1)->kind == tactical::MotionKind::none && state_of(near, 1).position == at(0, 0),
        "C-02: in range, the unit holds");
    static_cast<void>(before);

    // C-03 (OR-06, OR-07): the stopped unit's target flies away along +X; within ten frames of
    // it leaving the attack distance the unit plans a new approach, and it stops in range again.
    auto chase = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}));
    expect(static_cast<bool>(chase.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    static_cast<void>(run(chase, 900));
    expect(chase.motion_state(1)->kind != tactical::MotionKind::path, "C-03: the unit has stopped at its slot");
    expect(static_cast<bool>(chase.submit({{900, 2, 0}, {2}, tactical::MovePayload{at(4500, 0)}})), "the target's move is queued");
    std::uint64_t left = 0;
    std::uint64_t replanned = 0;
    for (std::uint64_t tick = 900; tick < 1700; ++tick) {
        auto stepped = chase.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        if (left == 0 && planar(state_of(chase, 1).position, state_of(chase, 2).position) > 1000.0) left = tick;
        const auto motion = chase.motion_state(1);
        if (left != 0 && replanned == 0 && motion && motion->kind == tactical::MotionKind::path) replanned = tick;
    }
    expect(left != 0 && replanned >= left && replanned <= left + 10, "C-03: a new approach within ten frames of leaving range");
    const auto gap = planar(state_of(chase, 1).position, state_of(chase, 2).position);
    expect(gap <= 1000.0 && gap > 850.0 && chase.motion_state(1)->kind != tactical::MotionKind::path,
        "C-03: it stops in range of the target again");
}

void test_orders_attack_move() {
    // C-05 (OR-11): an attack-move to a point plans exactly the move to that point.
    const auto start = [] { return setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(2500, 2500), true)}); };
    auto moved = orders_session(start());
    auto attack_moved = orders_session(start());
    expect(static_cast<bool>(moved.submit({{0, 1, 0}, {1}, tactical::MovePayload{at(3000, 0)}})), "move is queued");
    expect(static_cast<bool>(attack_moved.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(3000, 0), 0}})), "attack-move is queued");
    static_cast<void>(run(moved, 1));
    static_cast<void>(run(attack_moved, 1));
    expect(moved.motion_state(1) == attack_moved.motion_state(1), "C-05: the attack-move plans the move's path");
    expect(state_of(attack_moved, 1).order.kind == tactical::OrderKind::attack_move, "C-05: the order is an attack-move");

    // Engage while moving (OR-11): an enemy parked beside the route is fired at as the unit passes,
    // and the unit does not stop; it arrives at its point.
    auto passing = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(1500, 150), true)}));
    expect(static_cast<bool>(passing.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(3000, 0), 0}})), "attack-move is queued");
    bool fired_moving = false;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < 1400; ++tick) {
        auto stepped = passing.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        const auto motion = passing.motion_state(1);
        for (const auto& event : stepped.value().snapshot->combat_events()) {
            fired_moving = fired_moving || (event.kind == tactical::CombatEventKind::weapon_fired && event.shooter == 1
                && event.target == 2 && motion && motion->kind == tactical::MotionKind::path);
        }
    }
    expect(fired_moving, "attack-move: it fires at the enemy beside its route while it moves");
    expect(planar(state_of(passing, 1).position, at(3000, 0)) < 1.0, "attack-move: it does not stop; it reaches its point");

    // OR-12: an attack-move towards a unit approaches it like an attack, without making it the target.
    auto toward = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}));
    expect(static_cast<bool>(toward.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(0, 0), 2}})), "attack-move is queued");
    static_cast<void>(run(toward, 1));
    const auto toward_plan = toward.motion_state(1);
    expect(toward_plan && toward_plan->kind == tactical::MotionKind::path && std::abs(coordinate(toward_plan->target.x) - 2100.0) < 1e-3,
        "OR-12: the attack-move approaches the unit's slot");
    expect(!toward.combat_state(1)->direct, "OR-12: the unit is not its ordered target");

    // A friendly guard destination controls facing even when the combat scan chooses
    // an enemy in another direction. Removing the destination ends that influence.
    auto guarded = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)),
        unit(2, bomber_type, 1, at(0, 500)), unit(3, fighter_type, 2, at(-500, 0))}));
    expect(static_cast<bool>(guarded.submit({{0, 1, 0}, {1}, tactical::GuardPayload{at(0, 0), 2}})), "guard destination queued");
    static_cast<void>(run(guarded, 240));
    const auto guarded_state = state_of(guarded, 1);
    const auto guard_yaw = 2.0 * std::atan2(coordinate(guarded_state.rotation.z), coordinate(guarded_state.rotation.w))
        * 180.0 / 3.14159265358979323846;
    expect(std::abs(guard_yaw - 90.0) < 1e-3 && guarded.combat_state(1)->attack_target == 3,
        "WMV-20: friendly destination centre controls facing independently of an enemy combat target");
    expect(static_cast<bool>(guarded.stage_remove(2)), "guard destination removed");
    static_cast<void>(run(guarded, 30));
    expect(state_of(guarded, 1).rotation == guarded_state.rotation, "WMV-20: a removed destination supplies no turn");

    auto point_hold = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(0, 500))}));
    expect(static_cast<bool>(point_hold.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(0, 0), 0}})), "point hold queued");
    static_cast<void>(run(point_hold, 240));
    expect(state_of(point_hold, 1).rotation == math::identity_quat(), "WMV-20: a point destination supplies no unit-facing turn");

    // WMV-20: a unit destination turns an at-rest ship independently of its scanned
    // combat target. This is the attack-move used by space AI plans, not a direct attack.
    std::vector<std::string> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto held = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)),
            unit(2, bomber_type, 2, at(0, 500), true)}));
        expect(static_cast<bool>(held.submit({{0, 1, 0}, {1}, tactical::AttackMovePayload{at(0, 0), 2}})),
            "WMV-20: unit attack-move is queued");
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        std::vector<std::string> hashes;
        bool fired = false;
        for (std::uint64_t tick = 0; tick < 240; ++tick) {
            const auto stepped = held.step(threaded);
            expect(static_cast<bool>(stepped), "WMV-20: unit-destination step succeeds");
            if (!stepped) break;
            hashes.push_back(held.state_sha256());
            for (const auto& event : stepped.value().snapshot->combat_events()) {
                fired = fired || (event.kind == tactical::CombatEventKind::weapon_fired
                    && event.shooter == 1 && event.target == 2);
            }
        }
        const auto yaw = tactical::yaw_degrees(state_of(held, 1).rotation);
        expect(yaw && std::abs(coordinate(yaw.value()) - 90.0) < 1e-3,
            "WMV-20: a held ship faces its unit destination");
        expect(state_of(held, 1).position == at(0, 0) && fired,
            "WMV-20: the narrow forward weapon fires after turning in place");
        expect(!held.combat_state(1)->direct, "WMV-20: facing does not promote an attack-move to a direct attack");
        if (reference.empty()) reference = hashes;
        else expect(hashes == reference, "WMV-20: every tick matches on 1/2/4/8 workers");
    }
}

void test_orders_guard() {
    // C-04 (OR-14): a guard within 750 of the guarded unit holds; when the guarded unit moves 3000
    // away the guard follows and stops 675 short of it (0.9 x min(1000, 750)).
    auto escort = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(500, 0)),
        unit(3, bomber_type, 2, at(-4000, -4000), true)}));
    expect(static_cast<bool>(escort.submit({{0, 1, 0}, {1}, tactical::GuardPayload{at(0, 0), 2}})), "guard is queued");
    static_cast<void>(run(escort, 60));
    expect(escort.motion_state(1)->kind == tactical::MotionKind::none && state_of(escort, 1).position == at(0, 0),
        "C-04: within the guard range the guard holds");
    expect(static_cast<bool>(escort.submit({{60, 1, 1}, {2}, tactical::MovePayload{at(3500, 0)}})), "the guarded unit's move is queued");
    double widest = 0.0;
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 60; tick < 2400; ++tick) {
        auto stepped = escort.step(executor);
        expect(static_cast<bool>(stepped), "orders step succeeds");
        if (!stepped) break;
        widest = std::max(widest, planar(state_of(escort, 1).position, state_of(escort, 2).position));
    }
    const auto gap = planar(state_of(escort, 1).position, state_of(escort, 2).position);
    expect(gap <= 750.0 && gap >= 674.0 && escort.motion_state(1)->kind != tactical::MotionKind::path,
        "C-04: the guard stops within the guard range, at least 675 short of the guarded unit");
    expect(widest > 750.0, "C-04: the guarded unit outran the leash before the guard caught up");
    expect(!escort.combat_state(1)->direct, "OR-15: a guard gives no attack order");

    // OR-16: a guard of a point is the move to it.
    auto point = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(2500, 2500), true)}));
    auto moved = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(2500, 2500), true)}));
    expect(static_cast<bool>(point.submit({{0, 1, 0}, {1}, tactical::GuardPayload{at(1000, 500), 0}})), "point guard is queued");
    expect(static_cast<bool>(moved.submit({{0, 1, 0}, {1}, tactical::MovePayload{at(1000, 500)}})), "move is queued");
    static_cast<void>(run(point, 1));
    static_cast<void>(run(moved, 1));
    expect(point.motion_state(1) == moved.motion_state(1), "OR-16: a point guard plans the move's path");

    // C-06 (OR-17): naming the unit itself is rejected with target_is_unit; a unit that is not live
    // rejects every listed unit with target_not_live.
    auto rejected = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(500, 0))}));
    expect(static_cast<bool>(rejected.submit({{0, 1, 0}, {1, 2}, tactical::GuardPayload{at(0, 0), 2}})), "guard is queued");
    expect(static_cast<bool>(rejected.submit({{0, 1, 1}, {1}, tactical::AttackMovePayload{at(0, 0), 9}})), "attack-move is queued");
    const auto events = run_events(rejected, 1);
    const auto reason_of = [&](const std::uint64_t sequence, const eawr::sim::EntityId id) {
        for (const auto& event : events) {
            if (event.sequence == sequence && event.unit == id) return event.reason;
        }
        return tactical::RejectReason::none;
    };
    expect(reason_of(0, 1) == tactical::RejectReason::none && reason_of(0, 2) == tactical::RejectReason::target_is_unit,
        "C-06: the guarded unit cannot guard itself");
    expect(reason_of(1, 1) == tactical::RejectReason::target_not_live, "C-06: a unit that is not live is rejected");
    expect(state_of(rejected, 1).order.kind == tactical::OrderKind::guard && state_of(rejected, 2).order.kind == tactical::OrderKind::none,
        "C-06: only the accepted unit takes the guard");
}

// Records every partitioned phase a step runs (docs/simulation.md, the phase map).
class OrdersPhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        return inline_executor.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        if (phase == "orders") orders.push_back(count);
        if (phase == "combat-world") worlds.push_back(count);
        return inline_executor.execute(count, partition);
    }
    mutable std::vector<std::size_t> orders;
    mutable std::vector<std::size_t> worlds;

private:
    eawr::sim::InlineExecutor inline_executor;
};

void test_orders_phase() {
    // OP-03: the approach checks run as the partitioned `orders` phase, in the ticks an approach is
    // due (every 10 frames after the order's tick), and only then.
    auto value = orders_session(setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, bomber_type, 2, at(3000, 0), true)}));
    expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "attack order is queued");
    const OrdersPhaseRecorder recorder;
    std::vector<std::uint64_t> ran;
    for (std::uint64_t tick = 0; tick < 35; ++tick) {
        const auto before = recorder.orders.size();
        expect(static_cast<bool>(value.step(recorder)), "orders phase step succeeds");
        if (recorder.orders.size() != before) ran.push_back(tick);
    }
    expect(ran == std::vector<std::uint64_t>{10, 20, 30}, "the orders phase runs every 10 ticks after the order");
    expect(recorder.worlds.size() == 35 && std::all_of(recorder.worlds.begin(), recorder.worlds.end(),
        [](const std::size_t count) { return count == eawr::sim::tick_partition_count; }),
        "every combat world fills disjoint slots in the named phase with the fixed partition count");
    expect(std::all_of(recorder.orders.begin(), recorder.orders.end(),
               [](const std::size_t count) { return count == eawr::sim::tick_partition_count; }),
        "the orders phase uses the fixed partition count");
}

void test_orders_workers_and_replay() {
    // The approach, attack-move and guard at 1, 2, 4 and 8 workers end on the same state, and the
    // recording replays to it (opcodes 6 and 7).
    const auto start = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(0, 600)),
        unit(3, bomber_type, 2, at(3000, 0), true), unit(4, shooter_type, 2, at(-2500, 1500), true)});
    const auto orders = [](tactical::TacticalSession& value) {
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{3}})), "attack is queued");
        expect(static_cast<bool>(value.submit({{0, 1, 1}, {2}, tactical::GuardPayload{at(0, 0), 1}})), "guard is queued");
        expect(static_cast<bool>(value.submit({{0, 2, 0}, {4}, tactical::AttackMovePayload{at(0, 0), 1}})), "attack-move is queued");
        expect(static_cast<bool>(value.submit({{200, 2, 1}, {3}, tactical::MovePayload{at(3800, 900)}})), "move is queued");
    };
    std::vector<std::string> finals;
    std::optional<tactical::TacticalReplay> recorded;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto value = orders_session(start);
        orders(value);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int tick = 0; tick < 700; ++tick) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "worker step succeeds");
            if (!stepped) break;
        }
        finals.push_back(value.state_sha256());
        if (!recorded) recorded = value.record();
    }
    expect(std::all_of(finals.begin(), finals.end(), [&](const std::string& hash) { return hash == finals[0]; }),
        "orders: 1, 2, 4 and 8 workers end on the same state");
    const auto bytes = tactical::write_replay(*recorded);
    expect(static_cast<bool>(bytes), "orders: the recording writes");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == *recorded, "orders: attack-move and guard commands round-trip through replay v2");
    if (!parsed) return;
    auto combat = table();
    combat.profiles[0].max_attack_distance = units(1000);
    auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors(5000), {}, turning_motion_all(), std::nullopt, combat);
    expect(static_cast<bool>(replayed), "orders: the recording replays");
    if (!replayed) return;
    static_cast<void>(run(replayed.value(), 700));
    expect(replayed.value().state_sha256() == finals[0], "orders: the replay ends on the same state");
}

// --- Attack orders on one hardpoint (#531, docs/behaviour/space-orders.md OR-20 to OR-26) -------

constexpr tactical::TypeId capital_type = 6;

// A target with four targetable hardpoints: 0 and 1 abeam, 2 amidships (the nearest to a shooter
// dead ahead), 3 far off to one side (outside the shooter's cone).
[[nodiscard]] tactical::CombatTable hardpoint_table() {
    auto result = table();
    result.profiles.push_back(tactical::CombatProfile{capital_type, bomber_bit, std::nullopt, std::nullopt, {}, {},
        {{0, at(0, -60), true}, {1, at(0, 60), true}, {2, at(0, 0), true}, {3, at(0, -400), true}}});
    return result;
}

[[nodiscard]] tactical::DurabilityTable hardpoint_durability() {
    tactical::HardpointProfile hardpoint;
    hardpoint.role = tactical::HardpointRole::weapon;
    hardpoint.destroyable = true;
    hardpoint.max_health = units(100);
    tactical::DurabilityTable durability;
    durability.profiles.push_back(
        {capital_type, units(100000), std::nullopt, false, {hardpoint, hardpoint, hardpoint, hardpoint}});
    return durability;
}

[[nodiscard]] std::vector<tactical::SensorProfile> hardpoint_sensors() {
    auto result = sensors();
    result.push_back({capital_type, units(2000)});
    return result;
}

[[nodiscard]] tactical::TacticalSession hardpoint_session(const tactical::TypeId target_type = capital_type) {
    auto created = tactical::TacticalSession::create(
        setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, target_type, 2, at(300, 0))}),
        hardpoint_sensors(), hardpoint_durability(), {}, std::nullopt, hardpoint_table());
    expect(static_cast<bool>(created), "hardpoint session is created");
    return std::move(created).value();
}

[[nodiscard]] std::vector<Shot> fired(const std::vector<Shot>& shots, const std::uint64_t after = 0) {
    std::vector<Shot> result;
    for (const auto& shot : shots) {
        if (shot.kind == tactical::CombatEventKind::weapon_fired && shot.tick > after) result.push_back(shot);
    }
    return result;
}

[[nodiscard]] bool all_at(const std::vector<Shot>& shots, const std::uint32_t hardpoint) {
    return !shots.empty() && std::all_of(shots.begin(), shots.end(), [hardpoint](const Shot& shot) {
        return shot.target == 2 && shot.hardpoint == hardpoint;
    });
}

void test_hardpoint_orders() {
    // OR-24: an attack on the unit picks its nearest standing hardpoint, the one amidships.
    auto hull = hardpoint_session();
    expect(static_cast<bool>(hull.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2}})), "hull attack is queued");
    expect(all_at(fired(run(hull, 300)), 2), "an attack on the unit fires at its nearest hardpoint");
    expect(hull.combat_state(1)->attack_hardpoint == tactical::no_hardpoint, "a hull attack orders no hardpoint");

    // OR-20, OR-25: an attack on hardpoint 0 fires at hardpoint 0 alone.
    auto ordered = hardpoint_session();
    expect(static_cast<bool>(ordered.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}})), "hardpoint attack is queued");
    expect(all_at(fired(run(ordered, 300)), 0), "an attack on a hardpoint fires at that hardpoint");
    const auto state = ordered.combat_state(1);
    expect(state && state->direct && state->attack_target == 2 && state->attack_hardpoint == 0,
        "the order keeps the target and its hardpoint");
    expect(state_of(ordered, 1).order.kind == tactical::OrderKind::attack && state_of(ordered, 1).order.hardpoint == 0,
        "the unit's order records the hardpoint");

    // OR-23: when the ordered hardpoint dies the unit goes back to the target's nearest hardpoint and
    // keeps attacking the unit.
    auto killed = hardpoint_session();
    expect(static_cast<bool>(killed.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}})), "hardpoint attack is queued");
    expect(all_at(fired(run(killed, 60)), 0), "before its death the ordered hardpoint is fired at");
    expect(static_cast<bool>(killed.submit({{60, 1, 1}, {2}, tactical::DamagePayload{units(100), 0}})),
        "the ordered hardpoint's destruction is queued");
    const auto after = fired(run(killed, 400), 62);
    expect(all_at(after, 2), "after the ordered hardpoint dies the nearest one is fired at");
    const auto survivor = killed.combat_state(1);
    expect(survivor && survivor->direct && survivor->attack_target == 2 && survivor->attack_hardpoint == tactical::no_hardpoint,
        "the attack on the unit goes on with no ordered hardpoint");

    // OR-25: a hardpoint no weapon can point at is not fired at, and no other point is picked.
    auto arc = hardpoint_session();
    expect(static_cast<bool>(arc.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 3}})), "out-of-arc attack is queued");
    expect(fired(run(arc, 400)).empty(), "a hardpoint outside every weapon's arc is not fired at, nor is another");
    expect(arc.combat_state(1)->attack_hardpoint == 3, "the order stays while the hardpoint stands");

    // OR-21: the order names a targetable, standing hardpoint of the target's type.
    const auto rejection = [](tactical::TacticalSession value, const tactical::PlayerCommand& command) {
        expect(static_cast<bool>(value.submit(command)), "the attack is queued");
        const auto events = run_events(value, 2);
        return std::any_of(events.begin(), events.end(), [](const tactical::Event& event) {
            return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::hardpoint_invalid;
        });
    };
    expect(rejection(hardpoint_session(), {{0, 1, 0}, {1}, tactical::AttackPayload{2, 9}}),
        "a hardpoint the type does not have is rejected");
    expect(rejection(hardpoint_session(fighter_type), {{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}}),
        "a type without hardpoints has none to attack");
    auto gone = hardpoint_session();
    expect(static_cast<bool>(gone.submit({{0, 1, 0}, {2}, tactical::DamagePayload{units(100), 1}})), "damage is queued");
    static_cast<void>(run(gone, 5));
    expect(static_cast<bool>(gone.submit({{5, 1, 1}, {1}, tactical::AttackPayload{2, 1}})), "attack is queued");
    const auto gone_events = run_events(gone, 2);
    expect(std::any_of(gone_events.begin(), gone_events.end(), [](const tactical::Event& event) {
        return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::hardpoint_invalid;
    }), "a destroyed hardpoint is rejected");

    // Another order ends the hardpoint order with the attack (OR-23).
    auto stopped = hardpoint_session();
    expect(static_cast<bool>(stopped.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 0}})), "hardpoint attack is queued");
    static_cast<void>(run(stopped, 20));
    expect(static_cast<bool>(stopped.submit({{20, 1, 1}, {1}, tactical::StopPayload{}})), "stop is queued");
    static_cast<void>(run(stopped, 2));
    expect(stopped.combat_state(1)->attack_hardpoint == tactical::no_hardpoint && !stopped.combat_state(1)->direct,
        "a stop clears the ordered hardpoint");
    expect(state_of(stopped, 1).order.hardpoint == tactical::attack_hull, "a stop leaves no ordered hardpoint");
}

void test_hardpoint_orders_replay() {
    // An attack on a unit keeps opcode 3 and its bytes; an attack on a hardpoint is opcode 12. Both
    // round-trip, and a recording replays to the state the live session ended on, at every worker count.
    const tactical::PlayerCommand hull_command{{0, 1, 0}, {1}, tactical::AttackPayload{2}};
    const tactical::PlayerCommand hardpoint_command{{0, 1, 0}, {1}, tactical::AttackPayload{2, 1}};
    tactical::TacticalReplay replay;
    replay.setup = setup({unit(1, shooter_type, 1, at(0, 0)), unit(2, capital_type, 2, at(300, 0))});
    replay.final_tick_count = 10;
    replay.commands = {hull_command};
    const auto hull_bytes = tactical::write_replay(replay);
    replay.commands = {hardpoint_command};
    const auto hardpoint_bytes = tactical::write_replay(replay);
    expect(hull_bytes && hardpoint_bytes, "both replays write");
    if (!hull_bytes || !hardpoint_bytes) return;
    // The command table follows the setup: a hardpoint attack is 8 bytes longer, and its opcode is 12.
    expect(hardpoint_bytes.value().size() == hull_bytes.value().size() + 8, "opcode 12 adds a hardpoint index and a reserved word");
    const auto parsed_hull = tactical::parse_replay(hull_bytes.value());
    const auto parsed_hardpoint = tactical::parse_replay(hardpoint_bytes.value());
    expect(parsed_hull && parsed_hull.value().commands == std::vector<tactical::PlayerCommand>{hull_command},
        "a hull attack round-trips as opcode 3");
    expect(parsed_hardpoint && parsed_hardpoint.value().commands == std::vector<tactical::PlayerCommand>{hardpoint_command},
        "a hardpoint attack round-trips as opcode 12");
    // The hardpoint index is the u32 before the unit list (header 8 bytes, one unit 8 bytes) and the
    // reserved word after it; opcode 12 naming the hull is not a valid spelling of an attack on the unit.
    auto forged = hardpoint_bytes.value();
    std::fill(forged.end() - 24, forged.end() - 20, std::uint8_t{0xff});
    expect(!tactical::parse_replay(forged), "opcode 12 naming the hull is refused");
    // The target is the u64 before the index: zero is refused (docs/replay-format.md).
    auto no_target = hardpoint_bytes.value();
    std::fill(no_target.end() - 32, no_target.end() - 24, std::uint8_t{0});
    expect(!tactical::parse_replay(no_target), "opcode 12 without a target is refused");
    // The opcode byte sits 36 bytes from the end. 9 to 11 belong to the purchasing commands and are
    // not this parser's yet; 13 is past the last opcode.
    for (const std::uint8_t opcode : {std::uint8_t{9}, std::uint8_t{11}, std::uint8_t{13}}) {
        auto unknown = hardpoint_bytes.value();
        expect(unknown[unknown.size() - 36] == 12, "the fixture's opcode byte is where the test expects it");
        unknown[unknown.size() - 36] = opcode;
        expect(!tactical::parse_replay(unknown), "an opcode nothing handles is refused: " + std::to_string(opcode));
    }

    std::vector<std::string> finals;
    std::optional<tactical::TacticalReplay> recorded;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto value = hardpoint_session();
        expect(static_cast<bool>(value.submit({{0, 1, 0}, {1}, tactical::AttackPayload{2, 1}})), "hardpoint attack is queued");
        expect(static_cast<bool>(value.submit({{80, 1, 1}, {2}, tactical::DamagePayload{units(100), 1}})), "damage is queued");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (int tick = 0; tick < 300; ++tick) {
            const auto stepped = value.step(executor);
            expect(static_cast<bool>(stepped), "worker step succeeds");
            if (!stepped) break;
        }
        finals.push_back(value.state_sha256());
        if (!recorded) recorded = value.record();
    }
    expect(std::all_of(finals.begin(), finals.end(), [&](const std::string& hash) { return hash == finals[0]; }),
        "hardpoint orders: 1, 2, 4 and 8 workers end on the same state");
    const auto bytes = tactical::write_replay(*recorded);
    expect(static_cast<bool>(bytes), "the recording writes");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == *recorded, "the recording round-trips");
    if (!parsed) return;
    auto replayed = tactical::TacticalSession::from_replay(
        parsed.value(), hardpoint_sensors(), hardpoint_durability(), {}, std::nullopt, hardpoint_table());
    expect(static_cast<bool>(replayed), "the recording replays");
    if (!replayed) return;
    static_cast<void>(run(replayed.value(), 300));
    expect(replayed.value().state_sha256() == finals[0], "the replay ends on the same state");
}


} // namespace combat_test_support
