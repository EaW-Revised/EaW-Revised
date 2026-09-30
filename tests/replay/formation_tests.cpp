#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-08b (#344): FoC's group moves (docs/behaviour/space-movement.md FM-01 to FM-12). The slot
// cases are computed by hand from the rules with the FoC corvette and Nebulon-B limits and
// footprints; the session cases check a mixed group crossing open space and passing a ship in
// its way, the staggered planning, the speed matching and worker equality.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using math::Vec2;
using math::Vec3;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId corvette_type = 2011;
constexpr tactical::TypeId frigate_type = 2012;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) { return {units(x), units(y), units(z)}; }
[[nodiscard]] bool near(const Fixed value, const Fixed expected, const Fixed tolerance) {
    const auto difference = value.raw() - expected.raw();
    return difference >= -tolerance.raw() && difference <= tolerance.raw();
}
[[nodiscard]] bool near(const Vec3& value, const Vec3& expected, const Fixed tolerance) {
    return near(value.x, expected.x, tolerance) && near(value.y, expected.y, tolerance) && near(value.z, expected.z, tolerance);
}

[[nodiscard]] tactical::MotionProfile corvette() {
    return {corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"), decimal("1.5"), units(2)};
}
[[nodiscard]] tactical::MotionProfile frigate() {
    return {frigate_type, decimal("2.64"), decimal("0.048"), decimal("0.048"), decimal("0.6"), units(3)};
}

// gameconstants.xml (Patch2) and the footprints units::motion_table measures on the FoC data.
[[nodiscard]] tactical::MotionTable foc_table() {
    tactical::MotionTable table{{units(15), units(300)}, {corvette(), frigate()}, std::nullopt, {}, {}};
    table.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
        decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"), 3500, 6, 90, 45,
        units(50)};
    table.footprints = {
        {corvette_type, tactical::SpaceLayer::corvette, decimal("17.755"), decimal("41.822"), decimal("41.822"), false},
        {frigate_type, tactical::SpaceLayer::frigate, decimal("23.48"), decimal("109.949"), decimal("109.949"), false},
    };
    return table;
}

[[nodiscard]] tactical::FormationMember member(const eawr::sim::EntityId id, const tactical::TypeId type, const Vec3 position) {
    const auto table = foc_table();
    const auto& profile = *table.find(type);
    const auto radius = tactical::occupation_radius(*table.footprint(type), profile.max_speed, profile.rate_of_turn, *table.avoidance);
    return {id, position, Fixed{}, table.footprint(type)->layer, radius.value(), profile.max_speed, profile.rate_of_turn,
        table.footprint(type)->radius};
}

[[nodiscard]] const tactical::FormationSlot* slot_of(const std::vector<tactical::FormationSlot>& slots, const eawr::sim::EntityId id) {
    for (const auto& slot : slots) {
        if (slot.entity == id) return &slot;
    }
    return nullptr;
}

// FM-04: hard radius * 1.2 + turn radius * 0.2 (by hand: the corvette's hard radius
// sqrt(17.755^2 + 41.822^2) = 45.435, turn radius 3.72 / 1.5 degrees = 142.094; the
// Nebulon-B's 112.428 and 2.64 / 0.6 degrees = 252.101).
void test_occupation_radius() {
    const auto table = foc_table();
    const auto corvette_radius = tactical::occupation_radius(
        *table.footprint(corvette_type), decimal("3.72"), decimal("1.5"), *table.avoidance);
    const auto frigate_radius = tactical::occupation_radius(
        *table.footprint(frigate_type), decimal("2.64"), decimal("0.6"), *table.avoidance);
    expect(corvette_radius && near(corvette_radius.value(), decimal("82.9404"), decimal("0.001")),
        "the corvette's occupation radius is 82.9404");
    expect(frigate_radius && near(frigate_radius.value(), decimal("185.3341"), decimal("0.001")),
        "the Nebulon-B's occupation radius is 185.3341");
}

// FM-06: |bearing - yaw| * 0.5 / rate of turn + 3D distance / maximum speed.
void test_time_to_reach() {
    const auto ahead = tactical::time_to_reach(member(1, corvette_type, at(0, 0, -20)), at(2000, 0, 0));
    const auto aside = tactical::time_to_reach(member(2, corvette_type, at(0, -100, -20)), at(2000, 0, 0));
    expect(ahead && near(ahead.value(), decimal("537.6613"), decimal("0.001")), "a corvette facing the target: 537.6613 frames");
    expect(aside && near(aside.value(), decimal("539.2870"), decimal("0.002")), "a corvette 2.862 degrees off: 539.2870 frames");
}

// Three corvettes in a column across the move (FM-02 to FM-09), by hand: the middle one is on
// the centroid, so it maps first along its bearing from the target and sits 82.94 units short
// of it; the outer two keep their directions (-Y, +Y) and step out from the target in 50-unit
// steps until they clear the middle slot by the two radii (165.88): at 150 units, where the
// distance is sqrt(82.94^2 + 150^2) = 171.40. The column is square to the move, so the three
// plan in their centroid order two frames apart; the middle one is slower (537.66 of 539.29).
void test_column_slots() {
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, corvette_type, at(0, 0, -20)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), tactical::CollisionWorld{90, {}, nullptr}, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 3, "the column maps three slots");
    if (!mapped || mapped.value().size() != 3) return;
    const auto& slots = mapped.value();
    expect(slots[0].entity == 2 && slots[1].entity == 1 && slots[2].entity == 3, "the column plans middle, -Y, +Y");
    expect(slots[0].delay == 0 && slots[1].delay == 2 && slots[2].delay == 4, "two frames apart per ship");
    expect(near(slot_of(slots, 2)->destination, Vec3{decimal("1917.0596"), units(0), units(0)}, decimal("0.001")),
        "the middle corvette sits one occupation radius short of the target");
    expect(near(slot_of(slots, 1)->destination, at(2000, -150, 0), decimal("0.001")), "the -Y corvette steps out to -150");
    expect(near(slot_of(slots, 3)->destination, at(2000, 150, 0), decimal("0.001")), "the +Y corvette steps out to +150");
    expect(near(slot_of(slots, 2)->max_speed, decimal("3.7088"), decimal("0.001")), "the middle corvette slows to 3.7088");
    expect(slot_of(slots, 1)->max_speed == decimal("3.72") && slot_of(slots, 3)->max_speed == decimal("3.72"),
        "the slowest ships keep full speed");
}

// A line along the move: the ship ahead maps first and plans first (FM-05, FM-07). By hand:
// (100, 0) is nearer the target; both are 100 from the centroid, so the nearer keeps its
// place: its slot is target + 82.94 along +X; the other steps from the target along -X to
// -100 (182.94 from the first slot). The front ship plans first.
void test_line_slots() {
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(-100, 0, -20)),
        member(2, corvette_type, at(100, 0, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), tactical::CollisionWorld{90, {}, nullptr}, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 2, "the line maps two slots");
    if (!mapped || mapped.value().size() != 2) return;
    const auto& slots = mapped.value();
    expect(slots[0].entity == 2 && slots[0].delay == 0 && slots[1].entity == 1 && slots[1].delay == 2,
        "the front ship plans first");
    expect(near(slot_of(slots, 2)->destination, Vec3{decimal("2082.9404"), units(0), units(0)}, decimal("0.001")),
        "the front slot is one occupation radius past the target");
    expect(near(slot_of(slots, 1)->destination, at(1900, 0, 0), decimal("0.001")), "the rear slot is 100 short of it");
}

// Mixed layers map separately (FM-03): the lone Nebulon-B goes to the target itself, the
// corvettes around it as a group of their own; the frigate layer plans before the corvettes.
void test_mixed_layers() {
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, frigate_type, at(-200, 0, -90)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), tactical::CollisionWorld{90, {}, nullptr}, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 3, "the mixed group maps three slots");
    if (!mapped || mapped.value().size() != 3) return;
    const auto& slots = mapped.value();
    expect(slots[0].entity == 2 && slots[0].delay == 0, "the frigate layer plans first");
    expect(slots[1].delay == 0 && slots[2].delay == 2, "the corvette layer's stagger starts again at 0");
    expect(slot_of(slots, 2)->destination == at(2000, 0, 0), "a layer of one ship goes to the target");
    // The corvettes: centroid (0, 0): both 100 away; 1 is created first (command order on a
    // tie), maps first at target + 82.94 * (0, -1); 3 steps out along +Y to 150 units.
    expect(near(slot_of(slots, 1)->destination, Vec3{units(2000), decimal("-82.9404"), units(0)}, decimal("0.001")),
        "the first corvette sits one radius out along its bearing");
    expect(near(slot_of(slots, 3)->destination, at(2000, 100, 0), decimal("0.001")),
        "the second corvette clears it by the two radii (100 + 82.94 > 165.88)");
    expect(slot_of(slots, 2)->max_speed == decimal("2.64"), "the slowest ship (the frigate) keeps its full speed");
    expect(slot_of(slots, 1)->max_speed < decimal("2.64"), "the corvettes slow to arrive with it");
}

// FM-05: a slot probe hits a ship of the layer outside the group. A held corvette at
// (2000, -150) blocks the -Y corvette's slot there and its next steps until the probe square
// (82.94) plus the held ship's reach across its facing (41.822) clears it: at -300.
void test_slot_avoids_layer() {
    tactical::TrackingLayerView view;
    view.start_frame = 0;
    const tactical::TrackedLeaf held{9, {units(2000), units(-150)}, {units(2000), units(-150)}, {units(1), Fixed{}},
        decimal("17.755"), decimal("41.822"), decimal("41.822"), tactical::collision_static};
    view.windows.assign(45, std::vector<tactical::TrackedLeaf>{held});
    tactical::CollisionWorld world{90, {}, nullptr};
    world.layers[*tactical::dynamic_layer_index(tactical::SpaceLayer::corvette)] = &view;
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, corvette_type, at(0, 0, -20)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    const auto mapped = tactical::map_group_move(members, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 3, "the blocked column maps three slots");
    if (!mapped || mapped.value().size() != 3) return;
    expect(near(slot_of(mapped.value(), 1)->destination, at(2000, -300, 0), decimal("0.001")),
        "the -Y slot steps past the held corvette");
    expect(near(slot_of(mapped.value(), 3)->destination, at(2000, 150, 0), decimal("0.001")),
        "the +Y slot is unaffected");
}

// FM-05a: the column's first slot (the middle corvette's, 82.94 short of the target) goes
// through the plain move's nearest open position with the group ignored. A group member held
// on that point does not move it; a corvette outside the group does, to the point AV-19 gives
// (tactical_pathfind_contracts pins its rings), and the other slots line up from the target
// shifted as the first slot was.
void test_first_slot_open_position() {
    const auto table = foc_table();
    const Vec3 target = at(2000, 0, 0);
    const std::vector<tactical::FormationMember> members{
        member(1, corvette_type, at(0, -100, -20)),
        member(2, corvette_type, at(0, 0, -20)),
        member(3, corvette_type, at(0, 100, -20)),
    };
    // The middle corvette is on the centroid: its direction is from the target, -X.
    const Vec3 base{Fixed::from_raw(units(2000).raw() - members[1].occupation_radius.raw()), units(0), units(0)};
    const auto map_with_held = [&](const eawr::sim::EntityId held_id, tactical::TrackingLayerView& view) {
        view.start_frame = 0;
        const tactical::TrackedLeaf held{held_id, {base.x, base.y}, {base.x, base.y}, {units(1), Fixed{}},
            decimal("17.755"), decimal("41.822"), decimal("41.822"), tactical::collision_static};
        view.windows.assign(45, std::vector<tactical::TrackedLeaf>{held});
        tactical::CollisionWorld world{90, {}, nullptr};
        world.layers[*tactical::dynamic_layer_index(tactical::SpaceLayer::corvette)] = &view;
        return std::pair{tactical::map_group_move(members, target, world, 31, *table.avoidance), world};
    };

    tactical::TrackingLayerView own_view;
    const auto own = map_with_held(1, own_view).first;
    const auto open = tactical::map_group_move(members, target, tactical::CollisionWorld{90, {}, nullptr}, 31, *table.avoidance);
    expect(own && open && own.value() == open.value(), "a group member on the first slot is ignored");

    tactical::TrackingLayerView other_view;
    const auto [moved, world] = map_with_held(9, other_view);
    expect(moved && moved.value().size() == 3, "the column with a held corvette on its first slot maps three slots");
    if (!moved || moved.value().size() != 3) return;
    const std::vector<eawr::sim::EntityId> group{1, 2, 3};
    const auto expected = tactical::nearest_open_position(*table.avoidance, *table.footprint(corvette_type), world, 2, 31,
        Vec2{units(0), units(0)}, Vec2{base.x, base.y}, group);
    expect(expected.has_value(), "the reference search runs");
    if (!expected) return;
    const Vec3 first = slot_of(moved.value(), 2)->destination;
    expect(first == Vec3{expected.value().x, expected.value().y, units(0)}, "the first slot is its nearest open position");
    expect(!(first == base), "the held corvette moves the first slot");
    // The outer corvettes' rays run along Y from the shifted anchor, so they keep its X.
    const Fixed anchor_x = Fixed::from_raw(units(2000).raw() + first.x.raw() - base.x.raw());
    expect(slot_of(moved.value(), 1)->destination.x == anchor_x && slot_of(moved.value(), 3)->destination.x == anchor_x,
        "the later slots line up from the target shifted as the first slot was");
}

// FoC's composition pass leaves a zero-speed object out of the layer formation. The
// remaining ships still share their own largest travel time and slot order.
void test_immobile_member_and_zero_time() {
    auto stopped = member(1, corvette_type, at(-500, 0, 0));
    stopped.max_speed = Fixed{};
    const std::vector<tactical::FormationMember> moving{
        member(2, corvette_type, at(0, -100, 0)), member(3, corvette_type, at(-200, 100, 0))};
    const std::vector<tactical::FormationMember> together{stopped, moving[0], moving[1]};
    const auto world = tactical::CollisionWorld{90, {}, nullptr};
    const auto expected = tactical::map_group_move(moving, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    const auto mapped = tactical::map_group_move(together, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    expect(expected && mapped && mapped.value() == expected.value(),
        "the immobile member has no slot and does not change the movable group's arrival matching");

    const std::vector<tactical::FormationMember> arrived{
        member(2, corvette_type, at(2000, 0, 0)), member(3, corvette_type, at(2000, 0, 0))};
    const auto zero_time = tactical::map_group_move(arrived, at(2000, 0, 0), world, 31, *foc_table().avoidance);
    expect(zero_time && zero_time.value().size() == 2, "a group already at its slots maps without dividing by zero");
    if (zero_time && zero_time.value().size() == 2) {
        for (const auto& slot : zero_time.value()) {
            expect(slot.destination == at(2000, 0, 0) && slot.max_speed == decimal("3.72"),
                "zero-time members keep their position and full planning speed");
        }
    }
}

// --- Sessions ---------------------------------------------------------------------------------

[[nodiscard]] math::Quat yaw(const std::int64_t degrees) { return tactical::yaw_rotation(units(degrees)).value(); }

[[nodiscard]] tactical::TacticalSetup setup(const std::vector<tactical::UnitState>& units_list) {
    tactical::TacticalSetup value;
    value.seed = 0x5eed000000000344ULL;
    value.content_identity.fill(0x44);
    value.players = {{1, 1, 1, tactical::player_flag_commandable}};
    value.units = units_list;
    return value;
}

// The #345 picture: a mixed group (two corvettes, two Nebulon-Bs) ordered across open space, with
// a held Nebulon-B in a Nebulon-B's way and a held corvette in a corvette's way.
[[nodiscard]] tactical::TacticalReplay group_replay() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1650, -20), yaw(0), {}},
        {2, corvette_type, 1, at(-1800, -1350, -20), yaw(0), {}},
        {3, frigate_type, 1, at(-2100, -1700, -90), yaw(0), {}},
        {4, frigate_type, 1, at(-800, -1300, -90), yaw(90), {}},
        {5, corvette_type, 1, at(-300, -1380, -20), yaw(90), {}},
        {6, frigate_type, 1, at(-2100, -1300, -90), yaw(0), {}},
    });
    replay.final_tick_count = 1500;
    replay.commands = {
        {{30, 1, 0}, {1, 2, 3, 6}, tactical::MovePayload{at(1200, -1500, 0)}},
    };
    return replay;
}

struct Run {
    std::vector<std::string> hashes;
    std::vector<std::vector<tactical::UnitState>> units;
    std::vector<std::optional<tactical::MotionState>> motion_at_40; // per unit ID 1..6
    std::vector<std::optional<tactical::MotionState>> motion_at_100; // per unit ID 1..16
};

[[nodiscard]] std::optional<Run> run(const tactical::TacticalReplay& replay, const std::size_t workers, const bool scramble,
    const tactical::MotionTable& table = foc_table()) {
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, table);
    if (!created) std::cerr << created.error().message << '\n';
    expect(static_cast<bool>(created), "the group session replays");
    if (!created) return std::nullopt;
    auto session = std::move(created).value();
    if (scramble) session.scramble_storage_for_testing();
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Run result;
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
        auto stepped = session.step(executor);
        if (!stepped) {
            std::cerr << stepped.error().message << '\n';
            return std::nullopt;
        }
        result.hashes.push_back(stepped.value().state_sha256);
        result.units.push_back(session.units());
        if (tick == 40) {
            for (eawr::sim::EntityId id = 1; id <= 6; ++id) result.motion_at_40.push_back(session.motion_state(id));
        }
        if (tick == 100) {
            for (eawr::sim::EntityId id = 1; id <= 16; ++id) result.motion_at_100.push_back(session.motion_state(id));
        }
        if (scramble) session.scramble_storage_for_testing();
    }
    return result;
}

[[nodiscard]] const tactical::UnitState* unit_of(const std::vector<tactical::UnitState>& list, const eawr::sim::EntityId id) {
    for (const auto& unit : list) {
        if (unit.entity_id == id) return &unit;
    }
    return nullptr;
}

[[nodiscard]] Fixed distance(const Vec3& a, const Vec3& b) {
    return math::length(Vec2{math::subtract(a.x, b.x).value(), math::subtract(a.y, b.y).value()}).value();
}

// `trace`: a CSV of every unit's position per tick (tick,unit,x,y,z), for the eye-check sheet.
void test_group_session(const char* trace) {
    const auto replay = group_replay();
    const auto reference = run(replay, 1, false);
    expect(reference.has_value(), "the group session runs");
    if (!reference) return;
    if (trace != nullptr) {
        std::ofstream file(trace, std::ios::binary | std::ios::trunc);
        file << "tick,unit,x,y,z\n";
        const auto show = [](const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); };
        for (std::size_t tick = 0; tick < reference->units.size(); ++tick) {
            for (const auto& unit : reference->units[tick]) {
                file << tick + 1 << ',' << unit.entity_id << ',' << show(unit.position.x) << ',' << show(unit.position.y)
                     << ',' << show(unit.position.z) << '\n';
            }
        }
        expect(static_cast<bool>(file), "the trace writes");
    }
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        for (const bool scramble : {false, true}) {
            const auto other = run(replay, workers, scramble);
            expect(other && other->hashes == reference->hashes,
                std::to_string(workers) + " workers" + (scramble ? ", scrambled," : "") + " hash like 1 worker");
        }
    }
    // FM-08: in each layer the first ship plans in the order's frame (tick 31), the second two
    // frames later.
    const auto& motions = reference->motion_at_40;
    const bool recorded = motions.size() == 6 && motions[0] && motions[1] && motions[2] && motions[5];
    expect(recorded, "the group's motion is recorded");
    if (recorded) {
        expect(motions[0]->start_tick == 31 && motions[1]->start_tick == 33, "the corvettes plan at 31 and 33");
        expect(motions[2]->start_tick == 31 && motions[5]->start_tick == 33, "the Nebulon-Bs plan at 31 and 33");
        // FM-05 by hand: frigates one radius (185.33) out along -Y, then +Y in 50-unit steps
        // until 370.67 clear (200); corvettes 82.94 out along -Y, then +100.
        expect(near(motions[2]->target, Vec3{units(1200), decimal("-1685.3341"), units(0)}, decimal("0.001"))
                && near(motions[5]->target, at(1200, -1300, 0), decimal("0.001")),
            "the Nebulon-Bs head for their slots");
        expect(near(motions[0]->target, Vec3{units(1200), decimal("-1582.9404"), units(0)}, decimal("0.001"))
                && near(motions[1]->target, at(1200, -1400, 0), decimal("0.001")),
            "the corvettes head for their slots");
    }
    // FM-09 and the #345 picture: the corvettes slow to the Nebulon-Bs' arrival; each ship flies
    // around the held ship of its layer in its way; the larger ships end on the outside.
    Fixed corvette_peak{};
    Fixed frigate_gap = units(100000);
    Fixed corvette_gap = units(100000);
    for (std::size_t tick = 1; tick < reference->units.size(); ++tick) {
        const auto& now = reference->units[tick];
        for (const eawr::sim::EntityId id : {eawr::sim::EntityId{1}, eawr::sim::EntityId{2}}) {
            const auto* before = unit_of(reference->units[tick - 1], id);
            const auto* ship = unit_of(now, id);
            if (before && ship) corvette_peak = std::max(corvette_peak, distance(before->position, ship->position));
        }
        const auto* runner = unit_of(now, 2);
        const auto* held_corvette = unit_of(now, 5);
        if (runner && held_corvette) corvette_gap = std::min(corvette_gap, distance(runner->position, held_corvette->position));
        const auto* frigate_ship = unit_of(now, 6);
        const auto* held_frigate = unit_of(now, 4);
        if (frigate_ship && held_frigate) frigate_gap = std::min(frigate_gap, distance(frigate_ship->position, held_frigate->position));
    }
    expect(corvette_peak < decimal("3.2"), "the corvettes fly slower than their 3.72 to arrive with the Nebulon-Bs");
    expect(frigate_gap > decimal("133.43"), "the Nebulon-B flies around the held Nebulon-B in its way");
    expect(corvette_gap > decimal("59.577"), "the corvette flies around the held corvette in its way");
    const auto& last = reference->units.back();
    const Vec3 target = at(1200, -1500, 0);
    std::vector<Fixed> to_target;
    for (const eawr::sim::EntityId id : {1, 2, 3, 6}) {
        const auto* ship = unit_of(last, id);
        expect(ship != nullptr, "the group is alive at the end");
        if (ship == nullptr) return;
        to_target.push_back(distance(ship->position, target));
    }
    expect(std::max(to_target[0], to_target[1]) < std::min(to_target[2], to_target[3]),
        "the Nebulon-Bs stop outside the corvettes");
    expect(distance(unit_of(last, 1)->position, unit_of(last, 2)->position) > decimal("165.88"),
        "the corvettes stop at least two occupation radii apart");
    expect(distance(unit_of(last, 3)->position, unit_of(last, 6)->position) > decimal("370.66"),
        "the Nebulon-Bs stop at least two occupation radii apart");
}

// Records each phase's name and partition count, and runs it inline.
class PhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        calls.emplace_back(std::string{}, count);
        return inline_executor.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        calls.emplace_back(std::string(phase), count);
        return inline_executor.execute(count, partition);
    }
    mutable std::vector<std::pair<std::string, std::size_t>> calls;

private:
    eawr::sim::InlineExecutor inline_executor;
};

// #503: the path searches due together in different layers (AV-01) run in the partitioned
// `plan-searches` phase: the front corvette and Nebulon-B in the order's tick, the second pair
// two frames later (FM-08), and in no other tick. The hashes stay the serial ones (the C-12
// pins in test_group_session and its worker equality).
void test_plan_searches_phase() {
    const auto replay = group_replay();
    const auto reference = run(replay, 1, false);
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
    expect(created && reference, "the plan-searches session replays");
    if (!created || !reference) return;
    auto session = std::move(created).value();
    const PhaseRecorder recorder;
    std::vector<std::uint64_t> searched;
    for (std::uint64_t tick = 0; tick < 60; ++tick) {
        recorder.calls.clear();
        const auto stepped = session.step(recorder);
        expect(stepped && stepped.value().state_sha256 == reference->hashes[tick], "the recorded step hashes like 1 worker");
        for (const auto& [name, partitions] : recorder.calls) {
            if (name != "plan-searches") continue;
            expect(partitions == eawr::sim::tick_partition_count, "plan-searches is partitioned");
            searched.push_back(tick);
        }
    }
    expect(searched == std::vector<std::uint64_t>{30, 32}, "plan-searches runs in the order's tick and the staggered one");
}

// PC-07 (#520): single moves of ships in different layers, in one tick, search side by side in
// the partitioned `plan-searches` phase (one lane per layer), with the serial hashes.
void test_single_lanes() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {3, frigate_type, 1, at(-2100, -2100, -90), yaw(0), {}},
        {7, corvette_type, 1, at(-1800, -900, -20), yaw(0), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        {{30, 1, 0}, {1}, tactical::MovePayload{at(1200, -1500, 0)}},
        {{30, 1, 1}, {7}, tactical::MovePayload{at(1200, -900, 0)}},
        {{30, 1, 2}, {3}, tactical::MovePayload{at(1200, -2100, 0)}},
    };
    const auto reference = run(replay, 1, false);
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
    expect(created && reference, "the single-lanes session replays");
    if (!created || !reference) return;
    auto session = std::move(created).value();
    const PhaseRecorder recorder;
    std::vector<std::uint64_t> searched;
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
        recorder.calls.clear();
        const auto stepped = session.step(recorder);
        expect(stepped && stepped.value().state_sha256 == reference->hashes[tick], "the lanes step hashes like 1 worker");
        for (const auto& [name, partitions] : recorder.calls) {
            if (name == "plan-searches") searched.push_back(tick);
        }
    }
    // The first corvette and the frigate together, then the second corvette after the first's plan.
    expect(searched == std::vector<std::uint64_t>{30}, "single moves of two layers search in plan-searches in their tick");
    const auto path = [&](const eawr::sim::EntityId id) {
        const auto state = session.motion_state(id);
        return state && state->kind == tactical::MotionKind::path;
    };
    expect(path(1) && path(3) && path(7), "every single move planned in its tick");
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const auto again = run(replay, workers, true);
        expect(again && again->hashes == reference->hashes, "the lanes hash alike on scrambled workers");
    }
}

// PC-07, PC-08 (#520): a layer searches in a tick only within its budget. With a budget of 1
// every search passes it: the first of each layer is given up at the budget and the others do
// not start; each runs in slices from where its unit will be search_delay (4) frames later and
// lands then. Until the landing the ships keep their plan (here: at rest); a stop in between
// drops the search. The slice size changes no result, and neither does the worker count.
void test_search_budget() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {2, corvette_type, 1, at(-1800, -1000, -20), yaw(0), {}},
        {3, frigate_type, 1, at(-2100, -2100, -90), yaw(0), {}},
        {7, corvette_type, 1, at(-1800, -500, -20), yaw(0), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        {{30, 1, 0}, {1}, tactical::MovePayload{at(1200, -1500, 0)}},
        {{30, 1, 1}, {2}, tactical::MovePayload{at(1200, -1000, 0)}},
        {{30, 1, 2}, {7}, tactical::MovePayload{at(1200, -500, 0)}},
        {{30, 1, 3}, {3}, tactical::MovePayload{at(1200, -2100, 0)}},
        {{32, 1, 0}, {7}, tactical::StopPayload{}},
    };
    auto table = foc_table();
    table.avoidance->search_budget = 1;
    // The units with a path after each of ticks 30 to 35, and every tick's hash.
    std::vector<std::size_t> in_flight;
    const auto trace = [&](const tactical::MotionTable& with, std::vector<std::string>& hashes) {
        in_flight.clear();
        std::vector<std::vector<eawr::sim::EntityId>> planned;
        auto created = tactical::TacticalSession::from_replay(replay, {}, {}, with);
        expect(static_cast<bool>(created), "the budget session starts");
        if (!created) return planned;
        auto session = std::move(created).value();
        const eawr::sim::InlineExecutor executor;
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "the budget session steps");
            if (!stepped) return planned;
            hashes.push_back(stepped.value().state_sha256);
            if (tick < 30 || tick > 35) continue;
            // PC-08: a search lives from its start to its landing or its drop, no longer.
            in_flight.push_back(session.sliced_searches().searches);
            planned.emplace_back();
            for (const eawr::sim::EntityId id : {1, 2, 3, 7}) {
                const auto state = session.motion_state(id);
                if (state && state->kind == tactical::MotionKind::path) planned.back().push_back(id);
            }
        }
        return planned;
    };
    std::vector<std::string> hashes;
    const auto planned = trace(table, hashes);
    const std::vector<std::vector<eawr::sim::EntityId>> expected{{}, {}, {}, {}, {1, 2, 3}, {1, 2, 3}};
    expect(planned == expected, "every search lands 4 frames after its order; the stopped ship's is dropped");
    expect(in_flight == std::vector<std::size_t>{4, 4, 3, 3, 0, 0}, "the searches live until their landing or their drop");
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const auto again = run(replay, workers, workers > 1, table);
        expect(again && again->hashes == hashes, "the sliced searches hash alike on every worker count");
    }
    for (const std::uint64_t slice : {std::uint64_t{1}, std::uint64_t{50}}) {
        auto sliced = table;
        sliced.avoidance->search_slice = slice;
        std::vector<std::string> other;
        static_cast<void>(trace(sliced, other));
        expect(other == hashes, "the slice size changes no hash");
    }
    // The default budget plans all four in the order's tick, as FoC does (PC-02).
    std::vector<std::string> plain;
    const auto base = trace(foc_table(), plain);
    const std::vector<eawr::sim::EntityId> all{1, 2, 3, 7};
    expect(base.size() == 6 && base[0] == all && base[1] == all, "the default budget plans every single move in its tick");
}

// FM-10: a new move, face or stop replaces a waiting member's staggered plan.
void test_wait_replaced() {
    auto replay = group_replay();
    replay.final_tick_count = 60;
    replay.commands.push_back({{31, 1, 1}, {1, 2}, tactical::StopPayload{}});
    const auto stopped = run(replay, 1, false);
    expect(stopped.has_value(), "the replaced group session runs");
    if (!stopped) return;
    const auto& list = stopped->units.back();
    const auto* first = unit_of(list, 1);
    const auto* second = unit_of(list, 2);
    const auto* start_first = unit_of(stopped->units[31], 1);
    const auto* start_second = unit_of(stopped->units[31], 2);
    expect(first && second && start_first && start_second && distance(first->position, start_first->position) < units(1)
            && distance(second->position, start_second->position) < units(1),
        "a stop in the next tick leaves both corvettes where they were (the waiting one never plans)");
    const auto again = run(replay, 4, true);
    expect(again && again->hashes == stopped->hashes, "the replaced group hashes alike on 4 scrambled workers");
}

void test_disabled_engine_group_session() {
    tactical::DurabilityTable durability;
    durability.rules.engines_disabled_speed = Fixed{};
    tactical::HardpointProfile engine;
    engine.role = tactical::HardpointRole::engine;
    engine.destroyable = true;
    engine.max_health = units(100);
    durability.profiles.push_back({corvette_type, units(1000), decimal("3.72"), false, {engine}});
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-500, 0, 0), yaw(0), {}},
        {2, corvette_type, 1, at(0, -100, 0), yaw(0), {}},
        {3, corvette_type, 1, at(-200, 100, 0), yaw(0), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        {{0, 1, 0}, {1}, tactical::DamagePayload{units(100), 0}},
        {{1, 1, 1}, {1, 2, 3}, tactical::MovePayload{at(2000, 0, 0)}},
    };
    std::vector<std::string> reference;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto created = tactical::TacticalSession::from_replay(replay, {}, durability, foc_table());
        expect(static_cast<bool>(created), "the disabled-engine group session starts");
        if (!created) return;
        auto session = std::move(created).value();
        const bool scramble = workers > 1;
        if (scramble) session.scramble_storage_for_testing();
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
            const auto stepped = session.step(executor);
            if (!stepped) {
                std::cerr << stepped.error().message << '\n';
                expect(false, "the disabled-engine group finishes every tick");
                return;
            }
            hashes.push_back(stepped.value().state_sha256);
            if (scramble) session.scramble_storage_for_testing();
        }
        if (workers == 1) reference = hashes;
        else {
            expect(hashes == reference,
                "the disabled-engine group hashes alike on " + std::to_string(workers) + " scrambled workers");
        }
        const auto units_list = session.units();
        expect(unit_of(units_list, 1)->position == at(-500, 0, 0), "the engine-disabled ship stays put");
        expect(unit_of(units_list, 2)->position.x > units(0) && unit_of(units_list, 3)->position.x > units(-200),
            "both movable ships travel after the group's order");
        expect(!session.motion_state(1) || session.motion_state(1)->kind == tactical::MotionKind::none,
            "the engine-disabled ship has no motion plan");
    }
}

// --- A block of 16 (#613) -----------------------------------------------------------------------

// The owner's eye-check order in small: a block of four columns by four rows, 400 apart, the
// corvettes in the two northern rows and the Nebulon-Bs in the two southern ones, all facing +X,
// ordered as one group to a point 3,680 units to the south-east.
constexpr std::int64_t block_spacing = 400;
[[nodiscard]] Vec3 block_position(const std::size_t index) {
    const auto column = static_cast<std::int64_t>(index % 4);
    const auto row = static_cast<std::int64_t>(index / 4);
    return at(block_spacing * column - 600, 600 - block_spacing * row, row < 2 ? -20 : -90);
}
[[nodiscard]] tactical::TypeId block_type(const std::size_t index) { return index < 8 ? corvette_type : frigate_type; }
const Vec3 block_target = at(2600, -2600, 0);

[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

// FM-02 to FM-09 on the block, against the rule rather than by hand: each layer maps on its own
// around the target; every ship's slot lies on the ray from the target along its own XY offset
// from its layer's centroid (world space: no turn to the move's heading, no rows); the ship
// nearest the centroid sits one occupation radius out, and each later one at the first 50-unit
// step (max(50, r / 4)) along its ray that clears every other slot of its layer by the two
// radii; the layers plan frigates first, each front first along the centroid's shift to the
// target, two frames apart; each ship plans with its maximum speed times its time over the
// group's time.
void test_block_slots() {
    std::vector<tactical::FormationMember> members;
    for (std::size_t index = 0; index < 16; ++index) {
        members.push_back(member(static_cast<eawr::sim::EntityId>(index + 1), block_type(index), block_position(index)));
    }
    const auto mapped = tactical::map_group_move(members, block_target, tactical::CollisionWorld{90, {}, nullptr}, 31,
        *foc_table().avoidance);
    expect(mapped && mapped.value().size() == 16, "the block maps sixteen slots");
    if (!mapped || mapped.value().size() != 16) return;
    const auto& slots = mapped.value();
    Fixed slowest{};
    for (const auto& ship : members) slowest = std::max(slowest, tactical::time_to_reach(ship, block_target).value());
    for (const auto layer_type : {frigate_type, corvette_type}) {
        double cx = 0;
        double cy = 0;
        for (std::size_t index = 0; index < 16; ++index) {
            if (block_type(index) != layer_type) continue;
            cx += real(members[index].position.x) / 8;
            cy += real(members[index].position.y) / 8;
        }
        const double shift_x = real(block_target.x) - cx;
        const double shift_y = real(block_target.y) - cy;
        std::vector<const tactical::FormationSlot*> layer_slots;
        for (const auto& slot : slots) {
            if (block_type(slot.entity - 1) == layer_type) layer_slots.push_back(&slot);
        }
        expect(layer_slots.size() == 8, "each layer maps its eight ships");
        double previous_ahead = 1e18;
        double nearest = 1e18;
        for (std::size_t order = 0; order < layer_slots.size(); ++order) {
            const auto& slot = *layer_slots[order];
            const auto& ship = members[slot.entity - 1];
            const double ox = real(ship.position.x) - cx;
            const double oy = real(ship.position.y) - cy;
            nearest = std::min(nearest, std::hypot(ox, oy));
            const double sx = real(slot.destination.x) - real(block_target.x);
            const double sy = real(slot.destination.y) - real(block_target.y);
            const std::string name = "ship " + std::to_string(slot.entity);
            // FM-03, FM-05: on its own ray, in world space.
            const double along = (sx * ox + sy * oy) / std::hypot(ox, oy);
            expect(std::abs(sx * oy - sy * ox) / std::hypot(ox, oy) < 0.01 && along > 0,
                name + "'s slot lies on the ray along its offset from its layer's centroid");
            expect(slot.destination.z == block_target.z, name + "'s slot keeps the target's height");
            // FM-05: slots of a layer clear each other by the two radii; the step before a slot
            // on its ray (if any) would not.
            for (const auto* other : layer_slots) {
                if (other == &slot) continue;
                const auto& other_ship = members[other->entity - 1];
                const double gap = std::hypot(real(slot.destination.x) - real(other->destination.x),
                    real(slot.destination.y) - real(other->destination.y));
                expect(gap >= real(ship.occupation_radius) + real(other_ship.occupation_radius) - 0.01,
                    name + " and ship " + std::to_string(other->entity) + " sit two radii apart");
            }
            const double step = std::max(50.0, real(ship.occupation_radius) / 4);
            const double steps = along / step;
            const bool first_mapped = std::abs(along - real(ship.occupation_radius)) < 0.01;
            expect(first_mapped || std::abs(steps - std::round(steps)) < 0.001,
                name + "'s slot is one radius out or a whole number of steps along its ray");
            if (!first_mapped && along > step / 2) {
                const double back_x = real(block_target.x) + sx * (along - step) / along;
                const double back_y = real(block_target.y) + sy * (along - step) / along;
                bool blocked = false;
                for (const auto* other : layer_slots) {
                    if (other == &slot) continue;
                    const double gap = std::hypot(back_x - real(other->destination.x), back_y - real(other->destination.y));
                    blocked = blocked || gap < real(ship.occupation_radius) + real(members[other->entity - 1].occupation_radius);
                }
                expect(blocked, name + " takes the first free step on its ray");
            }
            // FM-07, FM-08: front first along the shift, two frames apart.
            const double ahead = (ox * shift_x + oy * shift_y) / std::hypot(shift_x, shift_y);
            expect(ahead <= previous_ahead + 0.001, name + " plans after every ship ahead of it");
            previous_ahead = ahead;
            expect(slot.delay == 2 * order, name + " plans two frames after the ship before it");
            // FM-09: the group arrives together.
            const auto time = tactical::time_to_reach(ship, block_target).value();
            const auto expected = time < slowest ? math::multiply(ship.max_speed, math::divide(time, slowest).value()).value()
                                                 : ship.max_speed;
            expect(near(slot.max_speed, expected, decimal("0.0001")), name + " plans with its speed matched to the group");
        }
        for (const auto* slot : layer_slots) {
            const auto& ship = members[slot->entity - 1];
            const double along = std::hypot(real(slot->destination.x) - real(block_target.x),
                real(slot->destination.y) - real(block_target.y));
            if (std::abs(along - real(ship.occupation_radius)) < 0.01) {
                expect(std::abs(std::hypot(real(ship.position.x) - cx, real(ship.position.y) - cy) - nearest) < 0.001,
                    "the ship one radius out is one of those nearest the centroid");
            }
        }
    }
    expect(block_type(slots.front().entity - 1) == frigate_type && block_type(slots[8].entity - 1) == corvette_type,
        "the frigate layer plans before the corvettes (FM-02)");
}

// The block's approach in a session (FM-10, AV-19, PC-04, PC-08, PC-09): every ship flies to the
// end of its plan, none falls back behind where it started, measured along the order's direction,
// and none moves away from its slot on the way. With the default budget the searches plan in their frames as in FoC
// (PC-02); with a budget of 1 each runs sliced and lands 4 frames later (PC-08), which must not
// change the route a later ship of the layer takes around one whose search has not landed yet.
// Hash-identical on 1, 2, 4 and 8 workers.
void test_block_approach() {
    tactical::TacticalReplay replay;
    std::vector<tactical::UnitState> ships;
    std::vector<eawr::sim::EntityId> group;
    for (std::size_t index = 0; index < 16; ++index) {
        const auto id = static_cast<eawr::sim::EntityId>(index + 1);
        ships.push_back({id, block_type(index), 1, block_position(index), yaw(0), {}});
        group.push_back(id);
    }
    replay.setup = setup(ships);
    replay.final_tick_count = 1900;
    replay.commands = {{{30, 1, 0}, group, tactical::MovePayload{block_target}}};
    std::vector<tactical::FormationMember> members;
    for (std::size_t index = 0; index < 16; ++index) {
        members.push_back(member(static_cast<eawr::sim::EntityId>(index + 1), block_type(index), block_position(index)));
    }
    const auto mapped = tactical::map_group_move(members, block_target, tactical::CollisionWorld{90, {}, nullptr}, 31,
        *foc_table().avoidance);
    expect(mapped.has_value(), "the block maps");
    if (!mapped) return;
    const double dx = real(block_target.x) / std::hypot(real(block_target.x), real(block_target.y));
    const double dy = real(block_target.y) / std::hypot(real(block_target.x), real(block_target.y));
    for (const std::uint64_t budget : {std::uint64_t{1000}, std::uint64_t{1}}) {
        auto table = foc_table();
        table.avoidance->search_budget = budget;
        const std::string config = budget == 1 ? " (every search sliced)" : "";
        const auto reference = run(replay, 1, false, table);
        expect(reference.has_value(), "the block session runs" + config);
        if (!reference) continue;
        for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
            const auto other = run(replay, workers, true, table);
            expect(other && other->hashes == reference->hashes,
                "the block hashes alike on " + std::to_string(workers) + " scrambled workers" + config);
        }
        for (const auto& slot : mapped.value()) {
            const auto& start = members[slot.entity - 1].position;
            const double start_gap = std::hypot(real(slot.destination.x) - real(start.x), real(slot.destination.y) - real(start.y));
            double worst_back = 0;
            double worst_away = 0;
            for (std::size_t tick = 30; tick < reference->units.size(); ++tick) {
                const auto* ship = unit_of(reference->units[tick], slot.entity);
                if (ship == nullptr) break;
                const double px = real(ship->position.x) - real(start.x);
                const double py = real(ship->position.y) - real(start.y);
                worst_back = std::min(worst_back, px * dx + py * dy);
                const double gap = std::hypot(real(slot.destination.x) - real(ship->position.x),
                    real(slot.destination.y) - real(ship->position.y));
                worst_away = std::max(worst_away, gap - start_gap);
            }
            const std::string name = "ship " + std::to_string(slot.entity);
            expect(worst_back > -25, name + " never falls back along the order (worst " + std::to_string(worst_back) + ")" + config);
            expect(worst_away < 25, name + " never moves away from its slot (worst +" + std::to_string(worst_away) + ")" + config);
            // AV-19: the ship's own search moves its slot to the nearest point no ship of its layer
            // occupies or will cross, so the ships behind stop short of slots the front ships'
            // routes pass through, and a later try of its search may finish on a step short of
            // that point (AV-14). The ship stops where its path ends.
            const auto& plan = reference->motion_at_100[slot.entity - 1];
            const bool heads = plan && plan->kind == tactical::MotionKind::path && !plan->nodes.empty();
            expect(heads, name + " plans a path" + config);
            const auto* last = unit_of(reference->units.back(), slot.entity);
            if (!heads || last == nullptr) continue;
            const auto& end = plan->nodes.back().position;
            const double end_gap = std::hypot(real(end.x) - real(last->position.x), real(end.y) - real(last->position.y));
            expect(end_gap < 1, name + " stops where its path ends (off by " + std::to_string(end_gap) + ")" + config);
        }
    }
}

// PC-09 (#613): two Nebulon-Bs in a line along the move, the rear one 400 behind the front one.
// The rear one has further to go, so it plans faster (FM-09), closes on the front one and passes
// it 63 units aside. With every search sliced (a budget of 1) the front ship's search starts in
// the order's frame, ends in the next and lands 4 frames after the order; the rear ship searches
// 2 frames after the order (FM-08), before that landing. It reads the front ship's published plan
// and passes it as it does when every search plans in its frame (FoC's timing, PC-02), instead
// of steering around it as if it would stay where it stands.
void test_published_plan() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, frigate_type, 1, at(0, 0, -90), yaw(0), {}},
        {2, frigate_type, 1, at(-400, 0, -90), yaw(0), {}},
    });
    replay.final_tick_count = 400;
    replay.commands = {{{30, 1, 0}, {1, 2}, tactical::MovePayload{at(3000, 0, 0)}}};
    const auto sideways = [&](const std::uint64_t budget) {
        auto table = foc_table();
        table.avoidance->search_budget = budget;
        const auto result = run(replay, 1, false, table);
        expect(result.has_value(), "the line session runs");
        double worst = 0;
        if (!result) return worst;
        for (const auto& list : result->units) {
            if (const auto* rear = unit_of(list, 2)) worst = std::max(worst, std::abs(real(rear->position.y)));
        }
        if (budget == 1) {
            for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
                const auto again = run(replay, workers, true, table);
                expect(again && again->hashes == result->hashes, "the sliced line hashes alike on scrambled workers");
            }
        }
        return worst;
    };
    const double exact = sideways(1'000'000);
    const double sliced = sideways(1);
    expect(exact > 50 && exact < 75, "with FoC's timing the rear ship passes the front one about 63 aside ("
            + std::to_string(exact) + ")");
    expect(std::abs(sliced - exact) < 2, "sliced, the rear ship reads the front one's published plan and passes it alike ("
            + std::to_string(sliced) + " against " + std::to_string(exact) + ")");
}

} // namespace

// `--trace <file.csv>` also writes the mixed group's positions (C-12).
int main(int argc, char** argv) {
    const char* trace = argc == 3 && std::strcmp(argv[1], "--trace") == 0 ? argv[2] : nullptr;
    test_occupation_radius();
    test_time_to_reach();
    test_column_slots();
    test_line_slots();
    test_mixed_layers();
    test_slot_avoids_layer();
    test_first_slot_open_position();
    test_immobile_member_and_zero_time();
    test_group_session(trace);
    test_plan_searches_phase();
    test_single_lanes();
    test_search_budget();
    test_wait_replaced();
    test_disabled_engine_group_session();
    test_block_slots();
    test_block_approach();
    test_published_plan();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "formation tests passed\n";
    return 0;
}
