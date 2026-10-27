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
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* pointer, std::size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { std::free(pointer); }
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
        for (const auto& m : moving.members()) {
            const auto x = land_cell_of(m.position.x, whole(20), field.columns);
            const auto y = land_cell_of(m.position.y, whole(20), field.rows);
            expect(x && y && field.occupancy.admits(*x, *y, m.mask), "members never commit inside the obstacle");
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
    for (auto& m : retained_seed.members) m.profile.radius = whole(6);
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
    speed_rules(); zero_heading_boundaries(); lifecycle_and_gates(); neighbour_contract();
    sim::InlineExecutor serial;
    const auto reference = follow(serial, false);
    expect(follow(serial, true) == reference, "save/restore continues identically");
    for (std::size_t workers : {1U, 2U, 4U, 8U}) {
        platform::ThreadWorkerAdapter executor(workers);
        expect(follow(executor, true) == reference, "1/2/4/8 workers and arbitrary checkpoint ticks match");
    }
    test_support::ReverseOrderExecutor reverse(8);
    expect(follow(reverse, true) == reference, "reverse partition traversal matches");
    allocation_and_stress();
    return checks.finish();
}
