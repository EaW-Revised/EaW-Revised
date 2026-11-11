#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/trig.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdlib>
#include <iostream>
#include <new>

static std::atomic<bool> measuring{};
static std::atomic<std::size_t> allocations{};
void* operator new(std::size_t size) {
    if (measuring.load(std::memory_order_relaxed)) allocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* pointer = std::malloc(size ? size : 1)) return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
// Keep the allocation guard's malloc/free pairing behind the matching delete operators.
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void* pointer) noexcept { std::free(pointer); }
#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete[](pointer); }
#endif

namespace {
using namespace eawr;
using namespace sim::tactical;
using namespace land_test;
testing::Checker checks;
void expect(bool ok, const char* message) { checks.check(ok, message, std::cerr); }
const auto dt = Fixed::from_ratio(1, 30).value();

LandMovement movement(const Field& field, std::size_t squads = 1, std::size_t members = 3) {
    LandMovement::Checkpoint saved;
    for (std::size_t i = 0; i < squads; ++i) {
        LandMovingSquad s;
        s.object = static_cast<sim::EntityId>(10000 + i);
        s.position = centre(2, 3); s.maximum_speed = whole(20); s.rear_extent = whole(6);
        s.collision_class = units::LandCollisionGroup::infantry; // WLPA-82: the container's class makes it a body
        auto req = request(*field.terrain, 2, 3, 12, 3);
        LandPathArena arena;
        s.path = LandPathService::plan(field.occupancy, arena, req);
        s.next_node = 1; s.order = LandMoveOrder::following;
        saved.squads.push_back(s);
        for (std::size_t j = 0; j < members; ++j) {
            LandMovingMember m;
            // Reverse IDs versus storage ensures the service actually needs its ordered index.
            m.object = static_cast<sim::EntityId>(1 + i * members + members - 1 - j);
            m.squad = i; m.source.type = "synthetic-member"; m.source.kind = units::LandObjectKind::infantry;
            m.source.locomotor = "synthetic-locomotor"; m.source.movement_class = "walker";
            m.source.max_speed = whole(30);
            m.profile.maximum_speed = *m.source.max_speed;
            m.profile.acceleration = whole(45); m.profile.deceleration = whole(60);
            m.profile.turn_rate = whole(1);
            m.profile.collision_class = units::LandCollisionGroup::infantry; // WLPA-82: the class makes the pair soft
            m.mask = walker;
            m.slot = at(0, static_cast<std::int64_t>(j) * 2 - static_cast<std::int64_t>(members - 1));
            m.position = {s.position.x, Fixed::from_raw(s.position.y.raw() + m.slot.y.raw())};
            saved.members.push_back(m);
        }
    }
    return LandMovement::from_checkpoint(std::move(saved)).value();
}

Field obstacle() {
    return make_field({"...............", ".......#.......", ".......#.......", ".......#.......",
        ".......#.......", ".......#.......", "...............", "...............", "..............."});
}

void motion_inputs() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = movement(field, 1, 1).checkpoint();
    saved.squads[0].generic_team = true;
    saved.squads[0].collision_class = units::LandCollisionGroup::none;
    saved.members[0].facing = Fixed::from_ratio(1, 4).value();
    auto moving = LandMovement::from_checkpoint(saved).value();
    expect(!moving.member_motion_inputs()[0].valid, "transient inputs are unavailable after restore");
    expect(static_cast<bool>(moving.step(field.occupancy, serial, dt)), "motion input step");
    const auto input = moving.member_motion_inputs()[0];
    expect(input.valid && input.turn_fraction_valid, "a generic-team member publishes both inputs");
    expect(input.heading_error == Fixed::from_ratio(-1, 4).value(), "heading error is the exact signed yaw to the target");
    expect(input.turn_fraction == whole(1), "a yaw beyond the budget consumes exactly the whole budget");
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto parallel_field = make_field(std::vector<std::string>(9, std::string(15, '.')));
        auto parallel = LandMovement::from_checkpoint(saved).value();
        platform::ThreadWorkerAdapter executor(workers);
        expect(static_cast<bool>(parallel.step(parallel_field.occupancy, executor, dt)), "parallel motion input step");
        expect(parallel.member_motion_inputs()[0] == input, "motion inputs match across worker counts");
    }
    const auto checkpoint = moving.checkpoint();
    auto restored = LandMovement::from_checkpoint(checkpoint).value();
    expect(!restored.member_motion_inputs()[0].valid && restored.checkpoint() == checkpoint,
        "restoring drops only transient presentation inputs");
}

// A formation member (stock vehicle) has a heading error but no turn fraction: the viewer keeps its estimate for it.
// A generic-team member held by the body-pair guard does not move, so it consumes no turn budget.
void motion_input_formation_and_revert() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto saved = movement(field, 1, 1).checkpoint();
    saved.members[0].facing = Fixed::from_ratio(1, 4).value();
    auto formation = LandMovement::from_checkpoint(saved).value();
    expect(static_cast<bool>(formation.step(field.occupancy, serial, dt)), "formation motion input step");
    const auto formation_input = formation.member_motion_inputs()[0];
    expect(formation_input.valid && !formation_input.turn_fraction_valid && formation_input.turn_fraction == Fixed{},
        "a formation member publishes the heading error but no turn fraction");

    const auto blocker_gap = [&](Fixed gap) {
        auto two = movement(field, 2, 1).checkpoint();
        const auto start = centre(3, 3);
        for (auto& squad : two.squads) {
            squad.generic_team = true;
            squad.collision_class = units::LandCollisionGroup::none;
        }
        auto& mover = two.squads[0]; auto& blocker = two.squads[1];
        mover.position = start; mover.order = LandMoveOrder::following; mover.next_node = 1;
        mover.path = plan(field, request(*field.terrain, 3, 3, 12, 3));
        blocker.position = {Fixed::from_raw(start.x.raw() + gap.raw()), start.y};
        blocker.order = LandMoveOrder::idle; blocker.next_node = blocker.path.nodes.size();
        auto& m = two.members[0]; auto& other = two.members[1];
        m.position = start; m.next_node = 1; m.facing = Fixed::from_ratio(1, 20).value(); m.speed = whole(30);
        m.profile.maximum_speed = whole(30); m.profile.acceleration = whole(45); m.profile.deceleration = whole(60);
        m.profile.turn_rate = whole(1); m.profile.radius = whole(3);
        m.profile.collision_class = units::LandCollisionGroup::vehicle;
        m.slot = {};
        other.position = blocker.position; other.facing = Fixed{}; other.speed = Fixed{}; other.slot = {};
        other.next_node = blocker.next_node; other.profile = m.profile;
        return LandMovement::from_checkpoint(two).value();
    };
    auto free_run = blocker_gap(whole(200));
    expect(static_cast<bool>(free_run.step(field.occupancy, serial, dt)), "unblocked generic step");
    expect(free_run.members()[0].speed > Fixed{} && free_run.member_motion_inputs()[0].turn_fraction == whole(1),
        "a moving generic member that turns at the limit publishes a full turn fraction");
    auto held = blocker_gap(Fixed::from_ratio(602, 100).value());
    expect(static_cast<bool>(held.step(field.occupancy, serial, dt)), "blocked generic step");
    expect(held.members()[0].speed == Fixed{} && held.members()[0].state == LandWalkState::stopping,
        "the body-pair guard stops the member");
    const auto held_input = held.member_motion_inputs()[0];
    expect(held_input.turn_fraction_valid && held_input.turn_fraction == Fixed{},
        "a reverted member publishes a zero turn fraction that agrees with its zero speed");
}

void speed_rules() {
    expect(land_member_desired_speed(whole(10), whole(20), whole(1), false) == whole(10), "LR3-15 inclusive +1 deadband");
    expect(land_member_desired_speed(whole(10), whole(20), whole(-1), false) == whole(10), "LR3-15 inclusive -1 deadband");
    expect(land_member_desired_speed(whole(10), whole(20), whole(5), false) == whole(15), "lag blends toward member maximum");
    expect(land_member_desired_speed(whole(10), whole(20), whole(-5), false) == Fixed::from_ratio(15, 2).value(), "overshoot uses squared fraction");
    expect(land_member_desired_speed(whole(10), whole(20), whole(-10), false) == Fixed{}, "overshoot clamp reaches zero");
    expect(land_member_desired_speed(whole(10), whole(20), whole(100), true) == Fixed{}, "turn requests zero speed");
}

LandMovement::Checkpoint straggler_wait_guard(const sim::PartitionExecutor& executor, bool restore) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto initial = movement(field, 1, 1).checkpoint();
    initial.squads[0].position = centre(5, 3);
    initial.squads[0].path = plan(field, request(*field.terrain, 5, 3, 12, 3));
    initial.members[0].position = centre(2, 3);
    initial.members[0].profile.maximum_speed = Fixed{};
    auto moving = LandMovement::from_checkpoint(initial).value();
    const auto end = initial.squads[0].path.nodes.back();
    int arrival = -1;
    for (int i = 0; i < 330; ++i) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "straggler guard step");
        if (arrival < 0 && moving.squads()[0].position == end) arrival = i;
        if (i == 14 && restore) {
            const auto saved = moving.checkpoint();
            expect(saved.squads[0].straggler_wait_remaining < land_straggler_wait_visits,
                "straggler wait has retained qualified visits");
            expect(saved.members[0].straggler_best_lag > Fixed{}, "straggler best lag is retained");
            moving = LandMovement::from_checkpoint(saved).value();
        }
    }
    expect(moving.squads()[0].position == end, "stalled straggler releases leader through path end");
    // 30 waits, then 140 units at 20/s (210 ticks), then the end-point braking at 60/s^2 (about 5 more ticks): the walk is
    // not slowed by more than that.
    expect(arrival >= 0 && arrival <= 262, "the end-point slowdown adds only the braking tail to the released walk");
    return moving.checkpoint();
}

void straggler_near_hold_paths() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    for (const bool corner : {false, true}) {
        auto state = movement(field, 1, 1).checkpoint();
        state.squads[0].position = centre(5, 3);
        const auto end = corner ? centre(10, 7) : centre(12, 3);
        state.squads[0].path = {LandPathStatus::found,
            corner ? std::vector<sim::math::Vec2>{centre(5, 3), centre(10, 3), end}
                   : std::vector<sim::math::Vec2>{centre(5, 3), end}, whole(200)};
        state.members[0].position = state.squads[0].position;
        state.members[0].profile.maximum_speed = Fixed{};
        auto moving = LandMovement::from_checkpoint(state).value();
        for (int i = 0; i < 500; ++i)
            expect(static_cast<bool>(moving.step(field.occupancy, serial, dt)), "near-hold straggler step");
        expect(moving.squads()[0].position == end, "near-hold straggler releases through path end, including a corner");
    }
}

void straggler_progress_guard() {
    sim::InlineExecutor serial;
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto state = movement(field, 1, 1).checkpoint();
    state.squads[0].position = centre(5, 3);
    state.squads[0].path = plan(field, request(*field.terrain, 5, 3, 12, 3));
    state.members[0].position = centre(2, 3);
    state.members[0].profile.maximum_speed = Fixed{};
    const auto start = state.squads[0].position;
    for (int i = 0; i < 45; ++i) {
        state.members[0].position.x = Fixed::from_raw(centre(2, 3).x.raw() + i * (Fixed::scale / 30));
        auto moving = LandMovement::from_checkpoint(state).value();
        expect(static_cast<bool>(moving.step(field.occupancy, serial, dt)), "catching straggler step");
        state = moving.checkpoint();
        expect(state.squads[0].position == start, "leader waits while straggler gains ground");
    }
    state.members[0].position = centre(2, 3);
    state.members[0].straggler_best_lag = Fixed::from_raw(-1);
    state.squads[0].straggler_wait_remaining = land_straggler_wait_visits;
    for (int i = 0; i < 40; ++i) {
        state.members[0].position.x = Fixed::from_raw(centre(2, 3).x.raw() + (i % 2) * (Fixed::scale / 30));
        auto moving = LandMovement::from_checkpoint(state).value();
        expect(static_cast<bool>(moving.step(field.occupancy, serial, dt)), "jittering straggler step");
        state = moving.checkpoint();
    }
    expect(state.squads[0].position != start && state.squads[0].straggler_wait_remaining == 0,
        "jitter without new progress releases the leader");
    auto spliced = LandMovement::from_checkpoint(state).value();
    const auto anchor = spliced.splice_anchor(0);
    expect(anchor.has_value(), "released moving squad has splice anchor");
    if (anchor) {
        LandPath splice{LandPathStatus::found, {*anchor, centre(12, 3)}, whole(100)};
        expect(spliced.splice_path(0, splice) == LandMovement::SpliceResult::installed, "new splice accepted after release");
        expect(spliced.squads()[0].straggler_wait_remaining == land_straggler_wait_visits
            && spliced.members()[0].straggler_best_lag == Fixed::from_raw(-1), "splice re-arms straggler guard");
    }
    auto reordered = LandMovement::from_checkpoint(state).value();
    LandPath path{LandPathStatus::found, {state.squads[0].position, centre(12, 3)}, whole(100)};
    expect(reordered.install_path(0, path, 0), "new order accepted after release");
    expect(reordered.squads()[0].straggler_wait_remaining == land_straggler_wait_visits
        && reordered.members()[0].straggler_best_lag == Fixed::from_raw(-1), "new order re-arms straggler guard");
}

void zero_heading_boundaries() {
    // Land paths are Vec2: a Z-only node is unrepresentable. Coincident planar nodes
    // are consumed before atan2 and must not change the path's eventual heading.
    std::optional<LandMovement::Checkpoint> coincident_reference, steering_reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        auto field = make_field(std::vector<std::string>(8, std::string(15, '.')));
        auto moving = movement(field, 1, 1);
        auto saved = moving.checkpoint();
        auto& squad = saved.squads.front();
        squad.path.nodes.insert(squad.path.nodes.begin() + 1, squad.position);
        moving = LandMovement::from_checkpoint(saved).value();
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "FM-27: coincident land node does not abort");
        expect(moving.squads().front().next_node >= 2, "FM-27: coincident land node is consumed before heading calculation");
        expect(moving.squads().front().facing == Fixed{}, "FM-27: following positive-X land segment has zero heading");
        if (!coincident_reference) coincident_reference = moving.checkpoint();
        else expect(moving.checkpoint() == *coincident_reference, "FM-27: coincident land node agrees on 1/2/4/8 workers");

        // A legal wide map can quantize reciprocal distance to zero. The derived
        // normalized steering vector is then exactly (0,0), despite a nonzero target.
        const auto wide = whole(67108864);
        LandTerrainInput input;
        input.width = 3; input.height = 3; input.spacing = wide;
        input.heights.resize(9); input.surfaces.resize(9); input.passability.assign(9, clear);
        input.movement_masks = {{"walker", walker}};
        const auto terrain = std::make_shared<const LandTerrain>(LandTerrain::build(std::move(input)).value());
        auto grid = LandOccupancy::create(terrain, clear).value();
        const sim::math::Vec2 start{wide, wide}, target{whole(134217728), wide};
        const auto reciprocal = sim::math::divide(whole(1), wide);
        expect(reciprocal && reciprocal.value() == Fixed{}, "FM-27: wide-map steering reciprocal is exactly zero");
        LandMovement::Checkpoint state;
        LandMovingSquad team;
        team.object = 100; team.generic_team = true; team.position = start;
        team.order = LandMoveOrder::following; team.path.status = LandPathStatus::found;
        team.path.nodes = {start, target}; team.next_node = 1;
        state.squads.push_back(team);
        LandMovingMember member;
        member.object = 1; member.position = start; member.next_node = 1; member.mask = walker;
        member.facing = Fixed::from_ratio(1, 4).value();
        member.profile.maximum_speed = whole(1); member.profile.acceleration = whole(1);
        member.profile.deceleration = whole(1); member.profile.turn_rate = whole(1);
        state.members.push_back(member);
        auto zero_steering = LandMovement::from_checkpoint(state).value();
        expect(static_cast<bool>(zero_steering.step(grid, executor, whole(1))), "FM-27: zero land steering direction does not abort");
        expect(zero_steering.members().front().facing == Fixed{}, "FM-27: zero land steering direction selects exact zero heading");
        if (!steering_reference) steering_reference = zero_steering.checkpoint();
        else expect(zero_steering.checkpoint() == *steering_reference, "FM-27: zero land steering agrees on 1/2/4/8 workers");
    }
}

LandMovement::Checkpoint follow(const sim::PartitionExecutor& executor, bool restore) {
    auto field = obstacle();
    auto moving = movement(field);
    expect(walkable(field, moving.squads()[0].path, walker), "L4 provides an admitted detour");
    expect(moving.squads()[0].path.nodes.size() > 2, "obstacle requires a squad path detour");
    for (int tick = 0; tick < 1600; ++tick) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "movement step succeeds");
        {
            // WLPA-36/69: only the container tests its centre point; a soldier has no terrain test and may clip a corner.
            const auto x = land_cell_of(moving.squads()[0].position.x, whole(20), field.columns);
            const auto y = land_cell_of(moving.squads()[0].position.y, whole(20), field.rows);
            expect(x && y && field.occupancy.admits(*x, *y, walker), "the container never commits inside the obstacle");
        }
        if (restore && (tick == 0 || tick == 19 || tick == 290 || tick == 650 || tick == 1250)) {
            const auto saved = moving.checkpoint();
            auto resumed = LandMovement::from_checkpoint(saved);
            expect(resumed && resumed->checkpoint() == saved, "every authoritative field round trips at arbitrary ticks");
            moving = std::move(*resumed);
        }
    }
    expect(moving.squads()[0].position == centre(12, 3), "squad center arrives around the wall");
    expect(moving.squads()[0].order == LandMoveOrder::reached, "arrival requires settled members, not just a completed path");
    for (const auto& m : moving.members()) {
        expect(m.speed == Fixed{} && m.state == LandWalkState::stopped, "members settle and stop");
        expect(distance(m.position, moving.squads()[0].position) <= whole(3), "members retain their formation slots");
        expect(m.source.locomotor == "synthetic-locomotor", "L2 per-member locomotor identity survives following");
    }
    return moving.checkpoint();
}

// LR13-17 (#6115): a Walk_Transition member plays its MOVESTART clip from standing and walks once the clip is almost finished.
// Nothing published a start gate in a live session, so a stock walker waited in `starting` for ever; the adapter now reads
// the clip and the movement keeps the clip clock itself. The result is the tick of the first displacement (0: none).
struct StartClipRun { int first_move{0}; int started_at{0}; bool arrived{}; LandMovement::Checkpoint end; };
StartClipRun start_clip_run(const sim::PartitionExecutor& executor, bool restore, bool clock, std::uint32_t ticks) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto moving = movement(field, 1, 1);
    auto saved = moving.checkpoint();
    auto& profile = saved.members[0].profile;
    profile.walk_transition = true; profile.start_clock = clock;
    profile.start_clip_ticks = ticks;
    moving = LandMovement::from_checkpoint(saved).value();
    const auto origin = moving.squads()[0].position;
    StartClipRun run;
    for (int tick = 1; tick <= 400; ++tick) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "start clip step");
        if (run.first_move == 0 && moving.squads()[0].position != origin) run.first_move = tick;
        if (run.started_at == 0 && moving.members()[0].state != LandWalkState::stopped && moving.members()[0].state != LandWalkState::starting)
            run.started_at = tick;
        if (restore && tick % 11 == 0) {
            const auto before = moving.checkpoint();
            auto resumed = LandMovement::from_checkpoint(before);
            expect(resumed && resumed->checkpoint() == before, "the start clip clock round trips");
            if (resumed) moving = std::move(*resumed);
        }
    }
    run.arrived = moving.squads()[0].order == LandMoveOrder::reached;
    run.end = moving.checkpoint();
    return run;
}

void start_clip_clock() {
    sim::InlineExecutor serial;
    // A wait of 29 logical frames (a one second clip, one frame short): the first displacement is on the 30th step.
    const auto clip = start_clip_run(serial, false, true, 29);
    expect(clip.first_move >= 29 && clip.first_move <= 31,
        "a Walk_Transition member holds the walk while its start clip plays, then walks (first displacement near tick 30)");
    expect(clip.started_at >= 29 && clip.started_at <= 31 && clip.arrived, "the member leaves starting when the clip is almost finished and the walk completes");
    const auto fast = start_clip_run(serial, false, true, 9);
    expect(fast.first_move >= 9 && fast.first_move <= 11, "a shorter wait (a faster playback) starts earlier");
    const auto none = start_clip_run(serial, false, true, 0);
    expect(none.first_move >= 1 && none.first_move <= 2, "a model without a MOVESTART clip has nothing to wait for");
    const auto held = start_clip_run(serial, false, false, 29);
    expect(held.first_move == 0 && !held.arrived, "without the adapter's clock only a published gate opens the start (hand-built profile)");
    expect(start_clip_run(serial, true, true, 29).end == clip.end, "save/restore through the start clip continues identically");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        expect(start_clip_run(threads, true, true, 29).end == clip.end, "the start clip walk equals on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(start_clip_run(reverse, true, true, 29).end == clip.end, "the start clip walk equals with reverse partitions");
}

void lifecycle_and_gates() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto moving = movement(field, 1, 1);
    sim::InlineExecutor executor;
    auto saved = moving.checkpoint();
    saved.squads[0].following_start = 3;
    saved.members[0].profile.walk_transition = true;
    saved.members[0].profile.turn_rate = Fixed::from_ratio(1, 4).value();
    moving = LandMovement::from_checkpoint(saved).value();
    for (int i = 0; i < 3; ++i) expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "future start step");
    expect(moving.members()[0].position == saved.members[0].position, "LR1-18 future following-start prevents movement");
    expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "start equality");
    expect(moving.members()[0].state == LandWalkState::starting && moving.members()[0].speed == Fixed{}, "walk start retained without a fabricated duration");
    LandMotionGates gates;
    gates.start_almost_finished = true;
    expect(moving.set_gates(1, gates), "publish start-clip admission");
    expect(moving.set_animation_progress(1, whole(7), whole(9)), "retain animation phase inputs");
    expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "start admitted");
    expect(moving.members()[0].speed > Fixed{} && moving.members()[0].speed < whole(20), "LR3-14 acceleration does not copy parent speed");
    expect(moving.yield(0, 99, 20), "explicit yield admission");
    for (int i = 0; i < 50; ++i) expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "yielded visits");
    expect(moving.squads()[0].order == LandMoveOrder::yielded, "no fixed-tick stall timeout");
    expect(!moving.observe_progress(0, 98, 21) && !moving.observe_progress(0, 99, 20), "wrong blocker or no progress cannot recover");
    expect(moving.observe_progress(0, 99, 21), "observed blocker progress admits recovery");
    const auto recovered = moving.checkpoint();
    moving = LandMovement::from_checkpoint(recovered).value();
    expect(moving.checkpoint() == recovered && moving.squads()[0].order == LandMoveOrder::recovering, "yield identities/recovery round trip");
    expect(moving.drop_order(0), "explicit contact order-drop seam");
    for (int i = 0; i < 50; ++i) expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "dropped visits");
    expect(moving.squads()[0].order == LandMoveOrder::dropped, "dropped order never auto-reissues");
    auto replacement = request(*field.terrain, 2, 3, 12, 3);
    replacement.start = moving.squads()[0].position;
    expect(moving.install_path(0, plan(field, replacement), 0), "replacement path explicitly restores following");
    expect(moving.set_walk_state(1, LandWalkState::deploying), "deploy state seam");
    const auto position = moving.members()[0].position;
    expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "deploy gate");
    expect(moving.members()[0].position == position && moving.members()[0].state == LandWalkState::deploying, "deployment waits for authoritative animation completion");
    gates.deployment_almost_finished = true; gates.turret_aligned = false;
    expect(moving.set_gates(1, gates), "turret gate input");
    expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "turret blocked");
    expect(moving.members()[0].state == LandWalkState::deploying, "unaligned turret keeps deployment blocked");
    gates.turret_aligned = true;
    expect(moving.set_gates(1, gates), "turret aligned");
    expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "deployment completion");
    expect(moving.members()[0].state == LandWalkState::deployed, "deployment completion is explicit");
    saved = moving.checkpoint(); saved.members[0].object = saved.squads[0].object;
    expect(!LandMovement::from_checkpoint(saved), "duplicate object IDs refused");
    saved = moving.checkpoint(); saved.squads[0].next_node = saved.squads[0].path.nodes.size() + 1;
    expect(!LandMovement::from_checkpoint(saved), "out-of-range restored tracker refused");
}

void neighbour_contract() {
    LandNeighbours index;
    constexpr std::size_t count = 800;
    index.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto x = static_cast<std::int64_t>(i % 40) * 16 - 320;
        const auto y = static_cast<std::int64_t>(i / 40) * 16 - 160;
        // Negative coordinates, cell-edge crossings, unequal radii and reversed sweeps.
        index.bodies[i] = {at(x, y), at(x + static_cast<std::int64_t>(i % 13) - 6,
            y + static_cast<std::int64_t>(i % 7) - 3), whole(static_cast<std::int64_t>(i % 4))};
    }
    index.build(whole(1));
    expect(index.safe(), "ordinary signed swept bounds use the neighbour index");
    std::vector<std::size_t> candidates;
    std::size_t work = 0;
    for (std::size_t i = 0; i < count; ++i) {
        index.query(i, candidates);
        work += candidates.size();
        expect(std::is_sorted(candidates.begin(), candidates.end())
            && std::adjacent_find(candidates.begin(), candidates.end()) == candidates.end(),
            "neighbour queries return unique ascending ranks");
        const auto& a = index.bodies[i];
        for (std::size_t j = 0; j < count; ++j) {
            if (i == j) continue;
            const auto& b = index.bodies[j];
            const auto overlap = [](Fixed a0, Fixed a1, Fixed ar, Fixed b0, Fixed b1, Fixed br) {
                return std::min(a0.raw(), a1.raw()) - ar.raw() <= std::max(b0.raw(), b1.raw()) + br.raw()
                    && std::min(b0.raw(), b1.raw()) - br.raw() <= std::max(a0.raw(), a1.raw()) + ar.raw();
            };
            if (overlap(a.from.x, a.to.x, a.radius, b.from.x, b.to.x, b.radius)
                && overlap(a.from.y, a.to.y, a.radius, b.from.y, b.to.y, b.radius))
                expect(std::binary_search(candidates.begin(), candidates.end(), j), "no intersecting swept boxes lost at bucket boundaries");
        }
    }
    expect(work < count * 8, "sparse 800-member fixture keeps candidate work linear within budget");
    // Tiny geometry must retain the full scan's rounded-to-zero squared distances.
    index.resize(2);
    index.bodies[0] = {at(0, 0), at(0, 0), Fixed::from_raw(1)};
    index.bodies[1] = {{Fixed::from_raw(1000), {}}, {Fixed::from_raw(1000), {}}, Fixed::from_raw(1)};
    index.build(whole(1)); index.query(0, candidates);
    expect(candidates == std::vector<std::size_t>{1}, "broad phase preserves sub-square-root-quantum contacts");
    index.bodies[1].present = false;
    index.build(whole(1)); index.query(0, candidates);
    expect(candidates.empty(), "discovery excludes members absent from committed occupancy");
    index.bodies[1].from.x = Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
    index.build(whole(1));
    expect(!index.safe(), "extreme arithmetic keeps the original full-scan diagnostic path");
}

// WLPA-88/89: a squad that is walking keeps walking through a re-order. The new path (searched from the node the squad
// finishes its segment at) is spliced in behind it; a first corner beyond the smooth-start gate is refused untouched.
LandMovement::Checkpoint reorder_splice(const sim::PartitionExecutor& executor, bool restore) {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto moving = movement(field, 1, 3);
    const auto reload = [&] { if (restore) moving = LandMovement::from_checkpoint(moving.checkpoint()).value(); };
    for (int i = 0; i < 10; ++i) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk before the re-order");
        reload();
    }
    expect(moving.squads()[0].speed > Fixed{}, "the squad is translating before the re-order");
    const auto anchor = moving.bounded_splice_anchor(0);
    if (anchor) moving.cut_splice_anchor(0, *anchor);
    expect(anchor && *anchor == moving.squads()[0].path.nodes[moving.squads()[0].next_node],
        "WLPA-88 the anchor is the node that ends the current segment");
    if (!anchor) return moving.checkpoint();
    // The search runs from the anchor while the squad keeps walking; the anchor holds until it is reached.
    auto replacement = request(*field.terrain, 12, 3, 12, 7);
    replacement.start = *anchor;
    const auto path = plan(field, replacement);
    expect(path.status == LandPathStatus::found && path.nodes.front() == *anchor, "the replacement starts at the anchor");
    for (int i = 0; i < 5; ++i) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk during the search");
        expect(moving.squads()[0].speed > Fixed{} && moving.splice_anchor(0) == anchor, "no stop while the re-order is searched");
        reload();
    }
    // A path that does not start at the anchor, or one for a squad with no anchor, is refused with no change.
    const auto before = moving.checkpoint();
    auto stale = path; stale.nodes.front() = at(1, 1);
    expect(moving.splice_path(0, stale) == LandMovement::SpliceResult::rejected && moving.checkpoint() == before,
        "a stale anchor is refused untouched");
    expect(moving.splice_path(0, path) == LandMovement::SpliceResult::installed, "WLPA-88 the search result splices in");
    const auto& squad = moving.squads()[0];
    expect(squad.order == LandMoveOrder::following && !squad.turning && squad.next_node == 1
        && squad.path.nodes.size() == path.nodes.size() + 1 && squad.path.nodes.front() == squad.position
        && squad.path.nodes[1] == *anchor, "the squad finishes the current segment, then follows the new path");
    int ticks = 0;
    while (moving.squads()[0].order == LandMoveOrder::following && ticks < 900) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk the spliced path");
        if (moving.squads()[0].order == LandMoveOrder::following)
            expect(moving.squads()[0].speed > Fixed{} && !moving.squads()[0].turning, "never zero speed across the splice window");
        ++ticks;
        if (ticks % 7 == 0) reload();
    }
    expect(moving.squads()[0].order == LandMoveOrder::reached && moving.squads()[0].position == path.nodes.back(),
        "the spliced order completes at the new destination");
    return moving.checkpoint();
}

void reorder_gate() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    sim::InlineExecutor executor;
    const auto slow = [&](Fixed rate, Fixed minimum = {}) {
        auto moving = movement(field, 1, 3);
        auto saved = moving.checkpoint();
        for (auto& member : saved.members) {
            member.profile.turn_rate = rate;
            member.profile.min_speed_fraction_for_turn = minimum;
        }
        moving = LandMovement::from_checkpoint(saved).value();
        for (int i = 0; i < 10; ++i) expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk");
        return moving;
    };
    const auto from_anchor = [&](const LandMovement& moving, std::int64_t to_x, std::int64_t to_y) {
        auto req = request(*field.terrain, 12, 3, to_x, to_y);
        req.start = *moving.splice_anchor(0);
        return plan(field, req);
    };
    // WLPA-89/90: a corner keeps the walk while it has turn speed; only a corner whose turn speed falls below the authored
    // minimum fraction (here a fifth of the top speed; this quarter turn gets about an eighth) falls back to the stop.
    auto sharp = slow(Fixed::from_ratio(1, 100).value(), Fixed::from_ratio(1, 5).value());
    const auto before = sharp.checkpoint();
    expect(sharp.splice_path(0, from_anchor(sharp, 12, 7)) == LandMovement::SpliceResult::sharp && sharp.checkpoint() == before,
        "a corner below the minimum turn speed fraction falls back and changes nothing");
    expect(sharp.splice_path(0, from_anchor(sharp, 14, 3)) == LandMovement::SpliceResult::installed,
        "a collinear continuation is smooth at any turn rate");
    auto lenient = slow(Fixed::from_ratio(1, 100).value(), Fixed::from_ratio(1, 10).value());
    expect(lenient.splice_path(0, from_anchor(lenient, 12, 7)) == LandMovement::SpliceResult::installed,
        "the same slow corner is smooth when the minimum fraction is below its turn speed");
    auto slowest = slow(Fixed::from_ratio(1, 100).value());
    expect(slowest.splice_path(0, from_anchor(slowest, 12, 7)) == LandMovement::SpliceResult::installed,
        "no authored minimum: even a very slow turn rate keeps the squad walking (WLPA-89 retail rule)");
    auto smooth = slow(whole(1));
    expect(smooth.splice_path(0, from_anchor(smooth, 12, 7)) == LandMovement::SpliceResult::installed,
        "a quarter turn is smooth when the slowest member turns a full turn per second");
    auto fixed = slow(Fixed{});
    expect(fixed.splice_path(0, from_anchor(fixed, 12, 7)) == LandMovement::SpliceResult::installed,
        "a squad with a non-turning member never turns in place");
    // Only a squad translating on its own path has an anchor: a dropped, yielded or still-turning squad keeps the old drop.
    auto moving = slow(whole(1));
    expect(moving.yield(0, 99, 1) && !moving.splice_anchor(0), "a yielded squad has no anchor");
    expect(moving.drop_order(0) && !moving.splice_anchor(0), "a dropped squad has no anchor");
    // Standing start unchanged (WLPA-68): an accepted path from a non-moving squad turns in place first.
    auto replacement = request(*field.terrain, 12, 3, 12, 7);
    replacement.start = moving.squads()[0].position;
    expect(moving.install_path(0, plan(field, replacement), 0) && moving.squads()[0].turning,
        "a standing start still turns in place before walking");
    // WLPA-91: only a team container always turns in place first. A single object (a hero, a lone vehicle) walks off at
    // once when its heading error is within one frame's turn or within the move-start angle, and turns first beyond it.
    const auto standing_start = [&](bool formation, Fixed close_enough, std::int64_t to_x, std::int64_t to_y) {
        auto saved = movement(field, 1, 1).checkpoint();
        saved.squads[0].has_formation = formation; saved.squads[0].order = LandMoveOrder::idle; saved.squads[0].speed = Fixed{};
        saved.members[0].profile.close_enough_angle = close_enough;
        auto idle = LandMovement::from_checkpoint(std::move(saved)).value();
        const LandPath path{LandPathStatus::found, {centre(2, 3), centre(to_x, to_y)}, whole(80)};
        const bool accepted = idle.install_path(0, path, 0);
        return accepted && idle.squads()[0].turning;
    };
    const auto quarter = Fixed::from_ratio(9, 32).value(); // a little over a quarter turn
    const auto eighth = Fixed::from_ratio(1, 8).value();
    expect(standing_start(true, quarter, 2, 7), "a team container turns in place first whatever its move-start angle");
    expect(!standing_start(false, quarter, 2, 7), "a single object walks off a quarter turn from its heading when the move-start angle allows it");
    expect(standing_start(false, eighth, 2, 7), "a single object turns in place first when the heading error is beyond its move-start angle");
    expect(!standing_start(false, Fixed{}, 6, 3), "a single object already on its first heading never turns in place");
    expect(standing_start(false, Fixed{}, 2, 7), "no move-start angle: only a heading error within one frame's turn walks off");
    auto turning_saved = slow(whole(1)).checkpoint();
    turning_saved.squads[0].turning = true;
    expect(!LandMovement::from_checkpoint(turning_saved).value().splice_anchor(0), "a squad still turning has no anchor");
    auto stopped_saved = slow(whole(1)).checkpoint();
    stopped_saved.squads[0].speed = Fixed{};
    expect(!LandMovement::from_checkpoint(stopped_saved).value().splice_anchor(0), "a stopped squad has no anchor");
    auto short_leg = slow(whole(1));
    while (short_leg.squads()[0].order == LandMoveOrder::following
        && short_leg.bounded_splice_anchor(0) != short_leg.splice_anchor(0))
        expect(static_cast<bool>(short_leg.step(field.occupancy, executor, dt)), "approach the end of the short leg");
    const auto short_anchor = short_leg.bounded_splice_anchor(0);
    expect(short_anchor && short_anchor == short_leg.splice_anchor(0), "a short remaining leg keeps its endpoint");
    if (short_anchor) {
        const auto path = from_anchor(short_leg, 12, 7);
        expect(short_leg.splice_path(0, path) == LandMovement::SpliceResult::installed, "short leg replacement installs");
        for (int i = 0; i < 900 && short_leg.squads()[0].order == LandMoveOrder::following; ++i)
            expect(static_cast<bool>(short_leg.step(field.occupancy, executor, dt)), "finish short leg replacement");
        expect(short_leg.squads()[0].order == LandMoveOrder::reached && short_leg.squads()[0].position == path.nodes.back(),
            "short leg replacement reaches its destination");
    }
}

// WLPA-90/91: corner speed, start gate and approach slowdown. A squad of one soldier walks a two-leg path on open ground at
// 20 units/second with a deceleration of 60; its turn rate is a twentieth of a turn per second (18 degrees per second) so
// that the corner caps matter.
double real(Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(Fixed::scale); }
const auto slow_turn = Fixed::from_ratio(1, 20).value();

LandTurnLimits corner_limits(Fixed rate, Fixed minimum = {}) {
    LandTurnLimits limits;
    limits.maximum_speed = whole(20); limits.turn_rate = rate; limits.deceleration = whole(60);
    limits.minimum_fraction = minimum; limits.turns = true;
    return limits;
}

void corner_fractions() {
    const sim::math::Vec2 east{whole(1), Fixed{}};
    const auto limits = corner_limits(slow_turn);
    const auto straight = land_corner_fraction(east, at(100, 0), limits);
    const auto degrees45 = land_corner_fraction(east, at(71, 71), limits);
    const auto degrees90 = land_corner_fraction(east, at(0, 100), limits);
    const auto degrees180 = land_corner_fraction(east, at(-100, 0), limits);
    expect(straight == whole(1) && degrees45 == whole(1), "a straight continuation and a 45 degree corner keep full speed at this rate");
    expect(degrees90 > Fixed::from_ratio(78, 100).value() && degrees90 < Fixed::from_ratio(79, 100).value(),
        "a quarter turn at 18 degrees/second over 100 units keeps about 78.5 percent of the top speed (w d / 2)");
    expect(degrees180 > Fixed::from_ratio(39, 100).value() && degrees180 < Fixed::from_ratio(40, 100).value(),
        "a U-turn is limited by the deviation rule to about 39 percent");
    expect(degrees45 > degrees90 && degrees90 > degrees180, "a sharper corner never keeps more speed");
    expect(land_corner_fraction(east, at(-100, 0), corner_limits(slow_turn, Fixed::from_ratio(1, 2).value())) == Fixed{},
        "a corner below the minimum turn speed fraction stops to turn");
    expect(land_corner_fraction(east, at(0, 100), corner_limits(slow_turn, Fixed::from_ratio(1, 2).value())) == degrees90,
        "a corner above the minimum keeps its turn speed");
    auto fixed = limits; fixed.turns = false;
    expect(land_corner_fraction(east, at(-100, 0), fixed) == whole(1), "a body that does not turn is never limited");
    expect(land_corner_fraction(east, at(0, 0), limits) == whole(1), "a zero-length segment has no corner");
    expect(land_corner_fraction(east, at(0, 100), corner_limits(whole(1))) == whole(1), "a fast turn rate takes the corner at full speed");
}

LandMovement corner_squad(const Field& field, const std::vector<sim::math::Vec2>& nodes, Fixed rate, Fixed minimum = {}) {
    auto saved = movement(field, 1, 1).checkpoint();
    auto& squad = saved.squads[0];
    squad.position = nodes.front(); squad.facing = Fixed{}; squad.turn_start_heading = Fixed{};
    squad.maximum_speed = whole(20);
    squad.path = LandPath{LandPathStatus::found, nodes, whole(400)};
    squad.next_node = 1; squad.order = LandMoveOrder::following;
    saved.members[0].position = nodes.front();
    saved.members[0].profile.turn_rate = rate;
    saved.members[0].profile.min_speed_fraction_for_turn = minimum;
    return LandMovement::from_checkpoint(std::move(saved)).value();
}

struct CornerRun {
    LandMovement::Checkpoint end;
    Fixed fastest_after{}, final_speed{};
    double corner_cap{0}; // the largest speed / cos(error) while the heading is unmet near the corner: the corner's turn speed
    bool turned{}, stopped_at_corner{}, arrived{}, speed_held{true}, cosine_held{true};
};

CornerRun corner_run(const sim::PartitionExecutor& executor, bool restore, const std::vector<sim::math::Vec2>& nodes, Fixed rate,
    Fixed minimum = {}) {
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    auto moving = corner_squad(field, nodes, rate, minimum);
    CornerRun run;
    const auto corner = nodes[1];
    const auto last = nodes.back();
    int ticks = 0;
    while (moving.squads()[0].order == LandMoveOrder::following && ticks < 2500) {
        const auto position = moving.squads()[0].position;
        const auto left = real(distance(position, last));
        // The heading error of this tick against the segment the squad walks, before it steps.
        double error_turns = 0;
        {
            const auto& before = moving.squads()[0];
            auto ahead = before.next_node;
            while (ahead < nodes.size() && nodes[ahead] == before.position) ++ahead;
            if (ahead < nodes.size()) {
                const auto heading = sim::math::atan2_turn(Fixed::from_raw(nodes[ahead].y.raw() - before.position.y.raw()),
                    Fixed::from_raw(nodes[ahead].x.raw() - before.position.x.raw())).value();
                error_turns = std::fabs(real(sim::math::wrap_turn(Fixed::from_raw(heading.raw() - before.facing.raw()))));
            }
        }
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "corner walk step");
        const auto& squad = moving.squads()[0];
        run.turned = run.turned || squad.turning;
        // Position stays on the path, so the body never runs along it against its facing: while the heading error is beyond
        // one frame's turn the speed is at most the top speed times the cosine of the error (zero at a quarter turn).
        if (error_turns > real(rate) / 30.0 + 1e-6 && !squad.turning)
            run.cosine_held = run.cosine_held
                && real(squad.speed) <= 20.0 * std::max(0.0, std::cos(6.283185307179586 * error_turns)) + 1e-3;
        {
            // The corner's turn speed is the cap before the cosine of the heading error scales it.
            const auto cosine = std::cos(6.283185307179586 * error_turns);
            if (error_turns > real(rate) / 30.0 + 1e-6 && cosine > 0.2 && squad.next_node >= 2 && !squad.turning
                && distance(squad.position, corner) < whole(60))
                run.corner_cap = std::max(run.corner_cap, real(squad.speed) / cosine);
        }
        if (squad.next_node >= 2 && distance(squad.position, corner) > whole(85)) run.fastest_after = std::max(run.fastest_after, squad.speed);
        run.stopped_at_corner = run.stopped_at_corner || (distance(squad.position, corner) < whole(1) && squad.speed == Fixed{});
        // WLPA-91: within the last stretch the speed never exceeds the speed that brakes to a stop at the path end.
        if (squad.next_node + 1 >= nodes.size() && left < 3.0)
            run.speed_held = run.speed_held && real(squad.speed) <= std::sqrt(2.0 * 60.0 * left) + 0.001;
        run.final_speed = squad.speed;
        ++ticks;
        if (restore && ticks % 11 == 0) moving = LandMovement::from_checkpoint(moving.checkpoint()).value();
    }
    run.arrived = moving.squads()[0].order == LandMoveOrder::reached && moving.squads()[0].position == last;
    run.end = moving.checkpoint();
    return run;
}

void corner_trajectories() {
    sim::InlineExecutor serial;
    const std::vector<sim::math::Vec2> d45{at(60, 100), at(260, 100), at(331, 171)};
    const std::vector<sim::math::Vec2> d90{at(60, 100), at(260, 100), at(260, 200)};
    const std::vector<sim::math::Vec2> d180{at(60, 100), at(260, 100), at(160, 100)};
    const auto run45 = corner_run(serial, false, d45, slow_turn);
    const auto run90 = corner_run(serial, false, d90, slow_turn);
    const auto run180 = corner_run(serial, false, d180, slow_turn);
    expect(run45.arrived && run90.arrived && run180.arrived, "the squad arrives at the end of a 45, 90 and 180 degree path");
    expect(!run45.turned && !run90.turned && !run180.turned, "a corner never stops the walk to turn in place without a minimum fraction");
    expect(run45.corner_cap > 19.9 && run45.corner_cap < 20.1, "a 45 degree corner keeps the top speed as its turn speed");
    expect(run90.corner_cap < 17.0 && run90.corner_cap > 14.0,
        "a 90 degree corner's turn speed is near 78.5 percent of the top speed");
    expect(run180.corner_cap < 9.0 && run180.corner_cap > 6.0, "a 180 degree corner's turn speed is near 39 percent");
    expect(run180.corner_cap < run90.corner_cap && run90.corner_cap < run45.corner_cap, "a sharper corner is taken slower");
    expect(run90.fastest_after == whole(20) && run180.fastest_after == whole(20),
        "once the heading is met the squad is back at the top speed");
    expect(run45.speed_held && run90.speed_held && run180.speed_held, "the approach never exceeds the braking speed to the end point");
    expect(run45.cosine_held && run90.cosine_held && run180.cosine_held,
        "while the heading is unmet the speed never exceeds the top speed times the cosine of the heading error");
    expect(run45.final_speed < whole(5) && run90.final_speed < whole(5) && run180.final_speed < whole(5),
        "the squad reaches the end point slowly, not at the top speed");
    for (const auto* run : {&run45, &run90, &run180}) {
        const auto& squad = run->end.squads[0];
        const auto& nodes = squad.path.nodes;
        const auto out = sim::math::Vec2{Fixed::from_raw(nodes.back().x.raw() - nodes[nodes.size() - 2].x.raw()),
            Fixed::from_raw(nodes.back().y.raw() - nodes[nodes.size() - 2].y.raw())};
        const auto heading = sim::math::atan2_turn(out.y, out.x).value();
        const auto error = sim::math::wrap_turn(Fixed::from_raw(squad.facing.raw() - heading.raw())).raw();
        expect(std::llabs(error) <= Fixed::scale / 500, "the facing swings to the heading of the last leg");
    }
    // A corner with no turn speed (minimum fraction above it) stops at the corner, turns in place, then walks on.
    const auto stopping = corner_run(serial, false, d90, slow_turn, Fixed::from_ratio(9, 10).value());
    expect(stopping.arrived && stopping.turned && stopping.stopped_at_corner,
        "a corner below the minimum turn speed fraction brakes to a stop, turns in place and arrives");
    // Save/restore in the middle of the turn and every worker count give the same end state.
    expect(corner_run(serial, true, d90, slow_turn).end == run90.end, "save/restore across the corner continues identically");
    expect(corner_run(serial, true, d180, slow_turn).end == run180.end, "save/restore across a U-turn continues identically");
    expect(corner_run(serial, true, d90, slow_turn, Fixed::from_ratio(9, 10).value()).end == stopping.end,
        "save/restore across a stop-and-turn corner continues identically");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(corner_run(executor, true, d90, slow_turn).end == run90.end, "the corner walk equals on 1/2/4/8 workers");
        expect(corner_run(executor, false, d180, slow_turn).end == run180.end, "the U-turn walk equals on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(corner_run(reverse, true, d90, slow_turn).end == run90.end, "the corner walk equals with reverse partitions");
}

// WLPA-91: a single object may walk off at once whatever its heading error, but it never runs against its facing: ordered
// straight behind itself it rotates at zero speed until its error is under a quarter turn, then walks and arrives.
void reverse_start(const sim::PartitionExecutor& executor) {
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    const std::vector<sim::math::Vec2> nodes{at(260, 100), at(160, 100)};
    auto saved = corner_squad(field, nodes, slow_turn).checkpoint();
    saved.squads[0].has_formation = false;
    saved.squads[0].facing = Fixed{}; // east; the path leads west
    auto moving = LandMovement::from_checkpoint(std::move(saved)).value();
    const auto start = moving.squads()[0].position;
    bool held_while_behind = true, rotated = false;
    int ticks = 0;
    while (moving.squads()[0].order == LandMoveOrder::following && ticks < 2000) {
        const auto before = moving.squads()[0];
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "reverse start step");
        const auto error = std::fabs(real(sim::math::wrap_turn(Fixed::from_raw(Fixed::scale / 2 - before.facing.raw()))));
        if (error > 0.25 + 1e-3) held_while_behind = held_while_behind && moving.squads()[0].position == start;
        rotated = rotated || moving.squads()[0].facing != before.facing;
        ++ticks;
    }
    expect(held_while_behind && rotated, "a unit ordered straight behind itself rotates at zero speed instead of walking backwards");
    expect(moving.squads()[0].order == LandMoveOrder::reached && moving.squads()[0].position == nodes.back(),
        "the unit that rotated first still arrives");
}

// A re-order while the squad is turning through a corner: the new path splices in behind the squad, which never stops.
LandMovement::Checkpoint cornering_reorder(const sim::PartitionExecutor& executor, bool restore) {
    auto field = make_field(std::vector<std::string>(20, std::string(30, '.')));
    auto moving = corner_squad(field, {at(60, 100), at(260, 100), at(260, 200)}, slow_turn);
    const auto reload = [&] { if (restore) moving = LandMovement::from_checkpoint(moving.checkpoint()).value(); };
    int ticks = 0;
    while (moving.squads()[0].next_node < 2 && ticks < 900) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk to the corner");
        ++ticks;
    }
    for (int i = 0; i < 40; ++i) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "turn through the corner");
        reload();
    }
    expect(moving.squads()[0].speed > Fixed{} && moving.squads()[0].speed < whole(20) && !moving.squads()[0].turning
        && moving.squads()[0].facing != Fixed{} && moving.squads()[0].facing < Fixed::from_ratio(1, 4).value(),
        "the squad is slowed and half way through its turn when it is re-ordered");
    const auto anchor = moving.bounded_splice_anchor(0);
    expect(anchor.has_value(), "a squad cornering has a splice anchor");
    if (!anchor) return moving.checkpoint();
    moving.cut_splice_anchor(0, *anchor);
    expect(moving.squads()[0].awaiting_splice, "the cut path ends at a splice anchor, not a destination");
    const LandPath replacement{LandPathStatus::found, {*anchor, {whole(460), anchor->y}}, whole(200)};
    const auto before_install = moving.squads()[0].facing;
    expect(moving.splice_path(0, replacement) == LandMovement::SpliceResult::installed,
        "a squad that is cornering splices the new path in without stopping");
    expect(!moving.squads()[0].awaiting_splice && moving.squads()[0].turn_start_heading == before_install,
        "the spliced path measures its first corner from the current facing");
    int walked = 0, zero_visits = 0;
    while (moving.squads()[0].order == LandMoveOrder::following && walked < 2500) {
        // The replacement turns a quarter turn at the anchor: the cosine cap holds the speed at zero for the visit that
        // starts with exactly that heading error, and for no other visit.
        double error_turns = 0;
        {
            const auto& before = moving.squads()[0];
            const auto target = before.path.nodes[std::min<std::size_t>(before.next_node, before.path.nodes.size() - 1)];
            const auto heading = sim::math::atan2_turn(Fixed::from_raw(target.y.raw() - before.position.y.raw()),
                Fixed::from_raw(target.x.raw() - before.position.x.raw())).value();
            error_turns = std::fabs(real(sim::math::wrap_turn(Fixed::from_raw(heading.raw() - before.facing.raw()))));
        }
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk the re-ordered path");
        if (!(moving.squads()[0].speed > Fixed{})) ++zero_visits;
        expect(!moving.squads()[0].turning && (moving.squads()[0].speed > Fixed{} || error_turns > 0.249),
            "never zero speed through the re-order except at the cosine cap of a quarter turn");
        ++walked;
        if (walked % 7 == 0) reload();
    }
    expect(zero_visits <= 1, "the re-order stops for one visit at most, at the quarter turn of the anchor");
    expect(moving.squads()[0].order == LandMoveOrder::reached && moving.squads()[0].position == replacement.nodes.back(),
        "the re-ordered squad arrives at the new destination");
    return moving.checkpoint();
}

void cornering_reorder_contracts() {
    sim::InlineExecutor serial;
    const auto reference = cornering_reorder(serial, false);
    expect(cornering_reorder(serial, true) == reference, "save/restore through a corner re-order continues identically");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(cornering_reorder(executor, true) == reference, "the corner re-order equals on 1/2/4/8 workers");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(cornering_reorder(reverse, true) == reference, "the corner re-order equals with reverse partitions");
}

void allocation_and_stress() {
    auto field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    sim::InlineExecutor executor;
    auto moving = movement(field, 1, 1);
    // Warm across several cell boundaries as well as the initial occupancy snapshot. L1's
    // old/new touched-cell union reaches its steady capacity only after a member changes cells.
    for (int i = 0; i < 120; ++i) expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "warm movement scratch");
    allocations = 0; measuring = true;
    bool ok = true;
    for (int i = 0; i < 12; ++i) ok &= static_cast<bool>(moving.step(field.occupancy, executor, dt));
    measuring = false;
    expect(ok && allocations == 0, "warmed movement hot path allocates no heap storage");
    // Exercise the new retained stopped/stopped dispatcher while measuring the warmed late hot path.
    auto retained_field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto retained_seed = movement(retained_field, 2, 1).checkpoint();
    for (auto& s : retained_seed.squads) s.order = LandMoveOrder::idle;
    // Deployed soldiers cannot be sent away, so the overlapping idle containers keep one retained stopped/stopped row.
    for (auto& m : retained_seed.members) { m.profile.radius = whole(6); m.state = LandWalkState::deployed; }
    auto retained = LandMovement::from_checkpoint(std::move(retained_seed)).value();
    for (std::uint64_t frame = 0; frame < 8; ++frame) {
        expect(static_cast<bool>(retained.step(retained_field.occupancy, executor, dt)), "warm retained contact motion");
        expect(static_cast<bool>(retained.service_contacts(retained_field.occupancy, executor, frame)), "warm retained contact service");
    }
    expect(!retained.contacts().empty(), "allocation fixture actually has retained stopped/stopped contacts");
    allocations = 0; measuring = true; ok = true;
    for (std::uint64_t frame = 8; frame < 20; ++frame) {
        ok &= static_cast<bool>(retained.step(retained_field.occupancy, executor, dt));
        ok &= static_cast<bool>(retained.service_contacts(retained_field.occupancy, executor, frame));
    }
    measuring = false;
    expect(ok && allocations == 0, "warmed movement plus retained contact/discovery service allocates no heap storage");
    std::optional<LandMovement::Checkpoint> reference;
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter threads(workers);
        // Synthetic: 49 squads x 18 members = 882 members; no land session setup is wired yet.
        auto stress_field = make_field(std::vector<std::string>(9, std::string(15, '.')));
        auto stress = movement(stress_field, 49, 18);
        for (int i = 0; i < 10; ++i) expect(static_cast<bool>(stress.step(stress_field.occupancy, threads, dt)), "stress warmup");
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 150; ++i) expect(static_cast<bool>(stress.step(stress_field.occupancy, threads, dt)), "stress step");
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
        std::cout << "synthetic-land workers=" << workers << " squads=49 members=882 ticks=150 ms_per_tick=" << elapsed / 150 << '\n';
        const auto saved = stress.checkpoint();
        if (workers == 1U) std::cout << "land contacts bodies before=882 after=" << stress.body_count() << '\n';
        if (!reference) reference = saved;
        else expect(saved == *reference, "multi-squad 882-member final checkpoint equals across worker counts");
    }
    auto reverse_field = make_field(std::vector<std::string>(9, std::string(15, '.')));
    auto reverse_stress = movement(reverse_field, 49, 18);
    test_support::ReverseOrderExecutor reverse(8);
    for (int i = 0; i < 160; ++i)
        expect(static_cast<bool>(reverse_stress.step(reverse_field.occupancy, reverse, dt)), "reverse multi-squad stress step");
    expect(reverse_stress.checkpoint() == *reference, "multi-squad 882-member checkpoint equals in reverse partition order");
}
}

int main() {
    motion_inputs(); motion_input_formation_and_revert(); speed_rules(); straggler_progress_guard(); straggler_near_hold_paths(); zero_heading_boundaries(); lifecycle_and_gates(); start_clip_clock(); neighbour_contract(); reorder_gate();
    corner_fractions(); corner_trajectories(); cornering_reorder_contracts();
    { sim::InlineExecutor reverse_serial; reverse_start(reverse_serial); }
    {
        sim::InlineExecutor spliced;
        const auto reference = reorder_splice(spliced, false);
        expect(reorder_splice(spliced, true) == reference, "WLPA-88 save/restore mid-splice continues identically");
        for (std::size_t workers : {1U, 2U, 4U, 8U}) {
            platform::ThreadWorkerAdapter executor(workers);
            expect(reorder_splice(executor, true) == reference, "WLPA-88 1/2/4/8 workers match across the splice");
        }
        test_support::ReverseOrderExecutor reverse(8);
        expect(reorder_splice(reverse, true) == reference, "WLPA-88 reverse partition order matches across the splice");
    }
    sim::InlineExecutor serial;
    const auto stalled = straggler_wait_guard(serial, false);
    expect(straggler_wait_guard(serial, true) == stalled, "straggler guard restores mid-count");
    const auto reference = follow(serial, false);
    expect(follow(serial, true) == reference, "save/restore continues identically");
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(straggler_wait_guard(executor, true) == stalled, "straggler guard matches at 1/2/4/8 workers");
        expect(follow(executor, true) == reference, "1/2/4/8 workers and arbitrary checkpoint ticks match");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(follow(reverse, true) == reference, "reverse partition traversal matches");
    allocation_and_stress();
    return checks.finish();
}
