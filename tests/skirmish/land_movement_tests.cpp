#include "eawr/sim/tactical/land_movement.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "land_test_support.hpp"
#include "../support/reverse_partition_executor.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
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
    for (int i = 0; i < 250; ++i) {
        expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "straggler guard step");
        if (i == 14 && restore) {
            const auto saved = moving.checkpoint();
            expect(saved.squads[0].straggler_wait_remaining < land_straggler_wait_visits,
                "straggler wait has retained qualified visits");
            expect(saved.members[0].straggler_best_lag > Fixed{}, "straggler best lag is retained");
            moving = LandMovement::from_checkpoint(saved).value();
        }
    }
    expect(moving.squads()[0].position == end, "stalled straggler releases leader through path end");
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
    const auto slow = [&](Fixed rate) {
        auto moving = movement(field, 1, 3);
        auto saved = moving.checkpoint();
        for (auto& member : saved.members) member.profile.turn_rate = rate;
        moving = LandMovement::from_checkpoint(saved).value();
        for (int i = 0; i < 10; ++i) expect(static_cast<bool>(moving.step(field.occupancy, executor, dt)), "walk");
        return moving;
    };
    const auto from_anchor = [&](const LandMovement& moving, std::int64_t to_x, std::int64_t to_y) {
        auto req = request(*field.terrain, 12, 3, to_x, to_y);
        req.start = *moving.splice_anchor(0);
        return plan(field, req);
    };
    // WLPA-89 interim gate: smooth when the slowest member turns through the corner within one second (a quarter turn here).
    auto sharp = slow(Fixed::from_ratio(1, 10).value());
    const auto before = sharp.checkpoint();
    expect(sharp.splice_path(0, from_anchor(sharp, 12, 7)) == LandMovement::SpliceResult::sharp && sharp.checkpoint() == before,
        "a corner the slowest member cannot turn in a second falls back and changes nothing");
    expect(sharp.splice_path(0, from_anchor(sharp, 14, 3)) == LandMovement::SpliceResult::installed,
        "a collinear continuation is smooth at any turn rate");
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
    speed_rules(); straggler_progress_guard(); straggler_near_hold_paths(); zero_heading_boundaries(); lifecycle_and_gates(); neighbour_contract(); reorder_gate();
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
