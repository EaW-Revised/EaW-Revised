#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-08 (#71): FoC's space path finder and tracking layers (docs/behaviour/space-movement.md,
// AV-01 to AV-15). The rule cases use the FoC corvette and Nebulon-B limits and the footprints
// units::motion_table measures from the FoC data; the session cases check the static layer
// (a mining pad), the layer rule, the partitioned tracking phase and worker equality.
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
constexpr tactical::TypeId pad_type = 2013; // SPACE_OBSTACLE in the static layer, no motion

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) { return {units(x), units(y), units(z)}; }
[[nodiscard]] Vec2 at2(const std::int64_t x, const std::int64_t y) { return {units(x), units(y)}; }
[[nodiscard]] bool near(const Fixed value, const Fixed expected, const std::int64_t raw) {
    const auto difference = value.raw() - expected.raw();
    return difference >= -raw && difference <= raw;
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
        {pad_type, tactical::SpaceLayer::static_object, units(0), units(0), decimal("206.25"), true},
    };
    return table;
}

void test_validation() {
    expect(static_cast<bool>(tactical::validate_motion(foc_table())), "the FoC-shaped avoidance table is valid");
    auto unsorted = foc_table();
    std::swap(unsorted.footprints[0], unsorted.footprints[1]);
    expect(!tactical::validate_motion(unsorted), "footprint type IDs must increase");
    auto negative = foc_table();
    negative.footprints[0].radius = decimal("-1");
    expect(!tactical::validate_motion(negative), "a negative radius is rejected");
    auto untried = foc_table();
    untried.avoidance->tries = 0;
    expect(!tactical::validate_motion(untried), "zero tries are rejected");
    auto windowless = foc_table();
    windowless.avoidance->tracking_windows = 0;
    expect(!tactical::validate_motion(windowless), "zero windows are rejected");
    auto unsearched = foc_table();
    unsearched.avoidance->destination_search_increment = Fixed{};
    expect(!tactical::validate_motion(unsearched), "a zero destination search increment is rejected");
    // The destination search's query square is the radius times 1.2 and ring 39 lies at 1950: a
    // radius whose ring holds more than 2^13 points is rejected (#372 review); zero stays legal.
    auto tiny = foc_table();
    tiny.footprints[0].radius = Fixed::from_raw(1);
    expect(!tactical::validate_motion(tiny), "a one-raw-unit radius is rejected (5.27e9 points on ring 1)");
    tiny.footprints[0].radius = decimal("1.2");
    expect(!tactical::validate_motion(tiny), "a radius of 1.2 is rejected (8509 points on ring 39)");
    tiny.footprints[0].radius = decimal("1.3");
    expect(static_cast<bool>(tactical::validate_motion(tiny)), "a radius of 1.3 is valid (7854 points)");
    tiny.footprints[0].radius = Fixed{};
    expect(static_cast<bool>(tactical::validate_motion(tiny)), "a zero radius is valid (one point per ring)");
    auto far = foc_table();
    far.avoidance->destination_search_increment = units(1024);
    expect(static_cast<bool>(tactical::validate_motion(far)), "the corvette is valid at the largest increment (5000 points)");
    far.footprints[0].radius = units(20);
    expect(!tactical::validate_motion(far), "the bound follows the increment: 20 at 1024 is rejected (10456 points)");
    expect(foc_table().footprint(pad_type) != nullptr && foc_table().footprint(99) == nullptr, "footprints are found by type");
}

[[nodiscard]] tactical::TrackedLeaf leaf(const eawr::sim::EntityId id, const Vec2 start, const Vec2 end, const Fixed x,
    const Fixed y, const std::uint8_t collision) {
    return {id, start, end, {units(1), Fixed{}}, x, y, std::max(x, y), collision};
}

// AV-03, AV-04: windows, clipping, the footprint projection and the ignored searcher.
void test_collision() {
    tactical::TrackingLayerView layer;
    layer.start_frame = 100;
    layer.windows.resize(3);
    // A held ship at (0, 0) in every window; a mover crossing y = 500 during window 1 only.
    for (auto& window : layer.windows) {
        window.push_back(leaf(7, at2(0, 0), at2(0, 0), units(20), units(100), tactical::collision_static));
    }
    layer.windows[1].push_back(leaf(8, at2(-400, 500), at2(400, 500), units(20), units(20), tactical::collision_moving));
    tactical::LinearQuery query;
    query.x_extent = units(20);
    query.y_extent = units(20);
    query.ignore = 9;
    query.facing = {units(1), Fixed{}};
    query.start = at2(-300, 0);
    query.end = at2(300, 0);
    query.start_frame = units(110);
    query.end_frame = units(170);
    const auto blocked = tactical::find_linear_collision(layer, 90, query);
    expect(blocked && blocked.value() == tactical::collision_static, "a move through a held ship hits it as static");
    query.ignore = 7;
    const auto self = tactical::find_linear_collision(layer, 90, query);
    expect(self && self.value() == 0, "the searching ship is ignored");
    query.ignore = 9;
    query.start = at2(-300, 200);
    query.end = at2(300, 200);
    const auto beside = tactical::find_linear_collision(layer, 90, query);
    expect(beside && beside.value() == 0, "a move 200 units beside the held ship (reach 100 + 20) misses it");
    // The reaches come from the directions at the query's two ends (research E71-20): a short
    // pass abeam sees the Y extent, a long pass sees mostly the X extent.
    query.start = at2(-10, 110);
    query.end = at2(10, 110);
    const auto grazing = tactical::find_linear_collision(layer, 90, query);
    expect(grazing && grazing.value() == tactical::collision_static,
        "abeam of the ship its Y extent counts: a short pass 110 units off hits (100 + 20)");
    query.start = at2(-300, 110);
    query.end = at2(300, 110);
    const auto long_pass = tactical::find_linear_collision(layer, 90, query);
    expect(long_pass && long_pass.value() == 0, "a long pass 110 units off blends toward the X extent and misses");
    // The mover in window 1 (frames 190..280): a query along y = 500 then meets it.
    query.start = at2(0, 300);
    query.end = at2(0, 700);
    query.start_frame = units(200);
    query.end_frame = units(260);
    const auto crossing = tactical::find_linear_collision(layer, 90, query);
    expect(crossing && crossing.value() == tactical::collision_moving, "a crossing mover hits as moving");
    query.start_frame = units(10);
    query.end_frame = units(60);
    const auto before = tactical::find_linear_collision(layer, 90, query);
    expect(before && before.value() == 0, "a query before the layer's first window finds nothing");
}

[[nodiscard]] tactical::CollisionWorld empty_world(const std::vector<tactical::TrackedLeaf>* statics = nullptr) {
    tactical::CollisionWorld world;
    world.interval = 90;
    world.statics = statics;
    return world;
}

// AV-10 to AV-12 in open space: the S-10 detour (U-01) and the S-12 shape.
void test_open_space() {
    const auto table = foc_table();
    const auto& footprint = *table.footprint(corvette_type);
    const auto world = empty_world();
    const auto straight = tactical::plan_space_move(
        table, corvette(), footprint, world, 1, 31, at(-1800, -1500, -20), Fixed{}, Fixed{}, at(0, -1500, 0));
    expect(straight && straight.value().kind == tactical::MotionKind::path, "a dead-ahead move plans");
    if (straight) {
        const auto& nodes = straight.value().nodes;
        std::vector<Fixed> yaws;
        for (const auto& node : nodes) yaws.push_back(node.yaw);
        const bool right_then_left = nodes.size() >= 6 && yaws[2] == units(-15) && yaws[3] == Fixed{}
            && near(yaws[4], decimal("0.344"), one / 100);
        expect(right_then_left, "S-10: speed-up, a 15 degree right arc, a left arc back, a 0.34 degree match");
        expect(nodes.back().position.x == Fixed{} && nodes.back().position.y == units(-1500)
                   && nodes.back().position.z == units(-20) && nodes.back().speed == Fixed{},
            "the end leg stops on the target in the unit's layer");
    }
    const auto turning = tactical::plan_space_move(
        table, corvette(), footprint, world, 1, 31, at(-1800, -1500, -20), Fixed{}, Fixed{}, at(-1800, 500, 0));
    const auto direct = tactical::plan_move(corvette(), table.rules, 31, at(-1800, -1500, -20), Fixed{}, Fixed{},
        at(-1800, 500, 0));
    expect(turning && direct && turning.value().nodes.size() == direct.value().nodes.size(),
        "S-12: the search takes the speed-up, arcs, match and end of MV-13 to MV-16");
    if (turning && direct && turning.value().nodes.size() == direct.value().nodes.size()) {
        bool close = true;
        for (std::size_t index = 0; index < direct.value().nodes.size(); ++index) {
            close = close && near(turning.value().nodes[index].position.x, direct.value().nodes[index].position.x, one / 64)
                && near(turning.value().nodes[index].position.y, direct.value().nodes[index].position.y, one / 64);
        }
        expect(close, "S-12: the searched nodes lie within 1/64 unit of the open-space plan");
    }
    const auto tiny = tactical::plan_space_move(
        table, corvette(), footprint, world, 1, 31, at(0, 0, -20), Fixed{}, Fixed{}, at(39, 0, 0));
    expect(tiny && tiny.value().kind == tactical::MotionKind::none, "a move shorter than 40 units plans nothing");
}

// AV-01, AV-13: a static obstacle in the static layer is flown around.
void test_static_layer() {
    const auto table = foc_table();
    const auto& footprint = *table.footprint(corvette_type);
    const std::vector<tactical::TrackedLeaf> pad{{50, at2(-600, -1500), at2(-600, -1500), {units(1), Fixed{}},
        decimal("206.25"), decimal("206.25"), decimal("206.25"), tactical::collision_static}};
    const auto world = empty_world(&pad);
    const auto planned = tactical::plan_space_move(
        table, corvette(), footprint, world, 1, 31, at(-1800, -1500, -20), Fixed{}, Fixed{}, at(0, -1500, 0));
    expect(planned && planned.value().kind == tactical::MotionKind::path, "a move past a mining pad plans");
    if (!planned || planned.value().kind != tactical::MotionKind::path) return;
    Fixed closest = units(100000);
    for (std::uint64_t tick = 31; tick < 700; ++tick) {
        const auto sample = tactical::sample_motion(planned.value(), tick, at(-1800, -1500, -20), Fixed{});
        if (!sample || sample.value().finished) break;
        const auto dx = math::subtract(sample.value().position.x, units(-600)).value();
        const auto dy = math::subtract(sample.value().position.y, units(-1500)).value();
        closest = std::min(closest, math::length(Vec2{dx, dy}).value());
    }
    expect(closest > decimal("206.25"), "the corvette never enters the pad's radius");
}

[[nodiscard]] tactical::TrackedLeaf pad_leaf(const eawr::sim::EntityId id, const Vec2 centre) {
    return {id, centre, centre, {units(1), Fixed{}}, decimal("206.25"), decimal("206.25"), decimal("206.25"),
        tactical::collision_static};
}

[[nodiscard]] bool within(const Vec2 value, const double x, const double y) {
    const auto close = [](const Fixed coordinate, const double expected) {
        const double difference = static_cast<double>(coordinate.raw()) / static_cast<double>(one) - expected;
        return difference > -1.0 / 256 && difference < 1.0 / 256;
    };
    return close(value.x, x) && close(value.y, y);
}

// AV-19, AV-20: Get_Nearest_Open_Position against hand-computed rings. The corvette's query
// square is 41.822 x 1.2 = 50.1864; a pad's square is 206.25, so a point is blocked within
// 256.4364 of a pad's centre. Ring r (0, 50, 100, ...) holds (int)(2 pi r / 50.1864) + 1
// points, counter-clockwise from the unit's side at ((cos a + 1) / 4 + 1 / 2) r.
void test_nearest_open() {
    const auto table = foc_table();
    const auto& rules = *table.avoidance;
    const auto& footprint = *table.footprint(corvette_type);
    const auto search = [&](const tactical::CollisionWorld& world, const Vec2 position, const Vec2 destination,
                            const eawr::sim::EntityId entity = 1) {
        return tactical::nearest_open_position(rules, footprint, world, entity, 31, position, destination);
    };
    const auto open = search(empty_world(), at2(-1000, 0), at2(3, 7));
    expect(open && open.value() == at2(3, 7), "an open destination is kept exactly (ring 0 is the destination)");

    const std::vector<tactical::TrackedLeaf> centred{pad_leaf(50, at2(0, 0))};
    const auto on_pad = search(empty_world(&centred), at2(-1000, 0), at2(0, 0));
    expect(on_pad && on_pad.value() == at2(-300, 0),
        "a move onto a pad's centre ends on ring 6 toward the unit: (-300, 0); ring 5 (250) lies inside 256.44");

    const std::vector<tactical::TrackedLeaf> offset{pad_leaf(50, at2(-150, 0))};
    const auto beside = search(empty_world(&offset), at2(-1000, 0), at2(0, 0));
    expect(beside && within(beside.value(), 62.600230, -151.130325),
        "a pad between unit and destination: ring 5 (32 points), point 10 at 112.5 degrees, (62.60, -151.13)");

    const std::vector<tactical::TrackedLeaf> pair{pad_leaf(50, at2(-150, 0)), pad_leaf(51, at2(150, 0))};
    const auto between = search(empty_world(&pair), at2(-1000, 0), at2(0, 0));
    expect(between && within(between.value(), -19.091804, -230.403808),
        "between two pads: ring 6 (38 points), point 9, (-19.09, -230.40)");

    // The retail S-21 recording (FoC debug build, Coruscant's TED-placed objects): a corvette at
    // (-1800, 50) sent to (-229, 52), the gravity well station's centre (soft radius 500; ring 11
    // at 550 is 0.19 inside), stops at (-828.9965, 51.2362); one at (-1800, -430) sent to
    // (-796, -428), a mining pad's centre, stops at (-1095.9852, -428.5975). The remake's rings
    // put the targets at (-828.9995, 51.2362) and (-1095.9994, -428.5976).
    const std::vector<tactical::TrackedLeaf> coruscant{
        {60, {decimal("-228.943024"), decimal("52.138306")}, {decimal("-228.943024"), decimal("52.138306")},
            {units(1), Fixed{}}, units(500), units(500), units(500), tactical::collision_static},
        {61, {decimal("-795.584778"), decimal("-427.722046")}, {decimal("-795.584778"), decimal("-427.722046")},
            {units(1), Fixed{}}, decimal("206.25"), decimal("206.25"), decimal("206.25"), tactical::collision_static},
        {62, {decimal("-1359.067871"), decimal("313.218842")}, {decimal("-1359.067871"), decimal("313.218842")},
            {units(1), Fixed{}}, units(100), units(100), units(100), tactical::collision_static},
    };
    const auto station = search(empty_world(&coruscant), at2(-1800, 50), at2(-229, 52));
    expect(station && within(station.value(), -828.999514, 51.236156),
        "S-21: onto the gravity well station, ring 12 (76 points), point 0: (-829.00, 51.24) as recorded");
    const auto map_pad = search(empty_world(&coruscant), at2(-1800, -430), at2(-796, -428));
    expect(map_pad && within(map_pad.value(), -1095.999405, -428.597608),
        "S-21: onto a map mining pad, ring 6 (38 points), point 0: (-1096.00, -428.60) as recorded");

    // The unit's own layer: a held corvette (17.755 along its facing, 41.822 across) at the
    // destination; from below, the reach is 50.19 + 41.82 = 92.01, so ring 2 at (0, -100).
    tactical::TrackingLayerView corvettes;
    corvettes.start_frame = 0;
    corvettes.windows.resize(45);
    for (auto& window : corvettes.windows) {
        window.push_back({7, at2(0, 0), at2(0, 0), {units(1), Fixed{}}, decimal("17.755"), decimal("41.822"),
            decimal("41.822"), tactical::collision_static});
    }
    auto world = empty_world();
    world.layers[2] = &corvettes;
    const auto held = search(world, at2(0, -1000), at2(0, 0));
    expect(held && held.value() == at2(0, -100), "a held corvette at the destination: ring 2 toward the unit, (0, -100)");
    const auto itself = search(world, at2(0, -1000), at2(0, 0), 7);
    expect(itself && itself.value() == at2(0, 0), "the searching unit does not block its own destination");
    auto frigate_world = empty_world();
    frigate_world.layers[1] = &corvettes;
    const auto other_layer = search(frigate_world, at2(0, -1000), at2(0, 0));
    expect(other_layer && other_layer.value() == at2(0, 0), "ships of another layer do not block a destination");

    // The query spans every window from now on: a corvette that crosses the destination in a
    // later window blocks it too.
    tactical::TrackingLayerView later;
    later.start_frame = 0;
    later.windows.resize(45);
    later.windows[20].push_back({8, at2(-400, 0), at2(400, 0), {units(1), Fixed{}}, decimal("17.755"),
        decimal("41.822"), decimal("41.822"), tactical::collision_moving});
    auto crossing_world = empty_world();
    crossing_world.layers[2] = &later;
    const auto crossed = search(crossing_world, at2(0, -1000), at2(0, 0));
    expect(crossed && crossed.value() != at2(0, 0), "a destination on another corvette's future path is moved");

    // A unit on its blocked destination has no direction: every point is the destination, and
    // after 40 rings the destination is kept.
    const auto stuck = search(empty_world(&centred), at2(0, 0), at2(0, 0));
    expect(stuck && stuck.value() == at2(0, 0), "no open point: the destination is kept");

    // #372 review: a footprint of one raw unit bypassing validation. On its blocked destination
    // the search ends after ring 0 instead of querying the destination ~5.27e9 times on ring 1.
    auto speck = footprint;
    speck.radius = Fixed::from_raw(1);
    const auto speck_search = [&](const tactical::CollisionWorld& world, const Vec2 position) {
        return tactical::nearest_open_position(rules, speck, world, 1, 31, position, at2(0, 0));
    };
    const auto speck_stuck = speck_search(empty_world(&centred), at2(0, 0));
    expect(speck_stuck && speck_stuck.value() == at2(0, 0), "a tiny footprint on its blocked destination keeps it");
    // With a direction, ring 1 is capped at 2^13 points: a 60-unit square blocks all of ring 1
    // (reach 25 to 50) and ring 2 opens at its first point, toward the unit.
    const std::vector<tactical::TrackedLeaf> small{{52, at2(0, 0), at2(0, 0), {units(1), Fixed{}}, units(60), units(60),
        units(60), tactical::collision_static}};
    const auto speck_moved = speck_search(empty_world(&small), at2(-1000, 0));
    expect(speck_moved && speck_moved.value() == at2(-100, 0), "a tiny footprint's capped ring 1 is blocked; ring 2 opens");

    // The planner moves the target before planning and stops on the moved point.
    const auto planned = tactical::plan_space_move(
        table, corvette(), footprint, empty_world(&centred), 1, 31, at(-1800, 0, -20), Fixed{}, Fixed{}, at(0, 0, 5));
    expect(planned && planned.value().kind == tactical::MotionKind::path, "a move onto a pad plans");
    if (planned && planned.value().kind == tactical::MotionKind::path) {
        const auto& last = planned.value().nodes.back();
        expect(last.position == at(-300, 0, -20) && last.speed == Fixed{}, "the path stops on the clipped point");
        expect(planned.value().target == at(-300, 0, 5), "the plan's target is the clipped point");
    }
    // The 40-unit rule measures the clipped target: a unit 280 units from the pad's centre is
    // sent to (-300, 0), 20 units away, and plans nothing.
    const auto short_move = tactical::plan_space_move(
        table, corvette(), footprint, empty_world(&centred), 1, 31, at(-280, 0, -20), Fixed{}, Fixed{}, at(0, 0, 0));
    expect(short_move && short_move.value().kind == tactical::MotionKind::none,
        "a clipped target nearer than 40 units plans nothing");
}

// --- Sessions ---------------------------------------------------------------------------------

[[nodiscard]] math::Quat yaw(const std::int64_t degrees) { return tactical::yaw_rotation(units(degrees)).value(); }

[[nodiscard]] tactical::PlayerCommand command(const std::uint64_t tick, const std::uint64_t sequence,
    const eawr::sim::EntityId unit, tactical::CommandPayload payload) {
    return {{tick, 1, sequence}, {unit}, std::move(payload)};
}

[[nodiscard]] tactical::TacticalSetup setup(const std::vector<tactical::UnitState>& units_list) {
    tactical::TacticalSetup value;
    value.seed = 0x5eed000000000071ULL;
    value.content_identity.fill(0x71);
    value.players = {{1, 1, 1, tactical::player_flag_commandable}};
    value.units = units_list;
    return value;
}

// The S-14 shape with a mining pad: two corvettes cross, a frigate passes a held frigate, a
// corvette flies through the pad's position.
[[nodiscard]] tactical::TacticalReplay avoidance_replay() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1700, -20), yaw(0), {}},
        {2, corvette_type, 1, at(-1800, -1300, -20), yaw(0), {}},
        {3, frigate_type, 1, at(-2000, -1500, -90), yaw(0), {}},
        {4, frigate_type, 1, at(-800, -1500, -90), yaw(90), {}},
        {5, pad_type, 1, at(-600, 1000, 0), yaw(0), {}},
        {6, corvette_type, 1, at(-1800, 1000, -20), yaw(0), {}},
    });
    replay.final_tick_count = 800;
    replay.commands = {
        command(30, 0, 1, tactical::MovePayload{at(1200, -1300, 0)}),
        command(30, 1, 2, tactical::MovePayload{at(1200, -1700, 0)}),
        command(30, 2, 3, tactical::MovePayload{at(1000, -1500, 0)}),
        command(30, 3, 6, tactical::MovePayload{at(600, 1000, 0)}),
        command(200, 4, 2, tactical::StopPayload{}),
    };
    return replay;
}

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

struct Run {
    std::vector<std::string> hashes;
    std::vector<std::vector<tactical::UnitState>> units;
};

[[nodiscard]] std::optional<Run> run(const tactical::TacticalReplay& replay, const std::size_t workers, const bool scramble,
    const tactical::DurabilityTable& durability = {}) {
    auto created = tactical::TacticalSession::from_replay(replay, {}, durability, foc_table());
    if (!created) std::cerr << created.error().message << '\n';
    expect(static_cast<bool>(created), "the avoidance session replays");
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

void test_session() {
    const auto replay = avoidance_replay();
    const auto reference = run(replay, 1, false);
    expect(reference.has_value(), "the avoidance session runs");
    if (!reference) return;
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8},
             eawr::platform::ThreadWorkerAdapter::hardware_worker_count()}) {
        for (const bool scramble : {false, true}) {
            const auto other = run(replay, workers, scramble);
            expect(other && other->hashes == reference->hashes,
                std::to_string(workers) + " workers" + (scramble ? ", scrambled," : "") + " hash like 1 worker");
        }
    }
    Fixed pad_gap = units(100000);
    Fixed frigate_gap = units(100000);
    bool corvette_moved = false;
    for (const auto& list : reference->units) {
        const auto* pad = unit_of(list, 5);
        const auto* runner = unit_of(list, 6);
        const auto* frigate_ship = unit_of(list, 3);
        const auto* held = unit_of(list, 4);
        if (pad && runner) pad_gap = std::min(pad_gap, distance(pad->position, runner->position));
        if (frigate_ship && held) frigate_gap = std::min(frigate_gap, distance(frigate_ship->position, held->position));
        if (runner && runner->position.x > units(0)) corvette_moved = true;
    }
    expect(corvette_moved, "the corvette behind the pad reaches the far side");
    expect(pad_gap > decimal("206.25"), "the corvette flies around the mining pad (static layer)");
    expect(frigate_gap > decimal("133.43"), "the frigate flies around the held frigate (its own layer)");

    // The tracking phase runs partitioned in the ticks that plan moves.
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
    if (!created) return;
    auto session = std::move(created).value();
    const PhaseRecorder recorder;
    bool tracked_on_order = false;
    bool tracked_elsewhere = false;
    for (std::uint64_t tick = 0; tick < 40; ++tick) {
        recorder.calls.clear();
        expect(static_cast<bool>(session.step(recorder)), "phase map step");
        for (const auto& [name, partitions] : recorder.calls) {
            expect(!name.empty() && partitions == eawr::sim::tick_partition_count, "phase " + name + " is named and partitioned");
            if (name == "tracking") (tick == 30 ? tracked_on_order : tracked_elsewhere) = true;
        }
    }
    expect(tracked_on_order && !tracked_elsewhere, "the tracking phase runs in the tick that plans moves, and only then");
}

// AV-03, AV-15: scripted damage that destroys a tracked unit between two orders of one tick
// takes it out of the layers the later order plans against (PR #343 review). Order A builds
// the static layer and the corvette layer; the pad and the held frigate then die; the
// corvette and the frigate ordered after that fly as if neither had been there.
void test_same_tick_destruction() {
    tactical::DurabilityTable durability;
    durability.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    durability.profiles = {{frigate_type, units(100), std::nullopt, false, {}}, {pad_type, units(100), std::nullopt, false, {}}};
    auto destroyed = avoidance_replay();
    destroyed.final_tick_count = 400;
    destroyed.commands = {
        command(30, 0, 1, tactical::MovePayload{at(1200, -1300, 0)}),
        {{30, 1, 1}, {4, 5}, tactical::DamagePayload{units(1000)}},
        command(30, 2, 6, tactical::MovePayload{at(600, 1000, 0)}),
        command(30, 3, 3, tactical::MovePayload{at(1000, -1500, 0)}),
    };
    auto absent = destroyed;
    std::erase_if(absent.setup.units, [](const tactical::UnitState& unit) { return unit.entity_id == 4 || unit.entity_id == 5; });
    absent.commands.erase(absent.commands.begin() + 1);
    for (std::size_t index = 1; index < absent.commands.size(); ++index) absent.commands[index].key.sequence = index;
    const auto with = run(destroyed, 1, false, durability);
    const auto without = run(absent, 1, false, durability);
    expect(with && without, "the same-tick destruction sessions run");
    if (!with || !without) return;
    bool corvette_same = true;
    bool frigate_same = true;
    bool pad_gone = true;
    for (std::size_t tick = 0; tick < with->units.size(); ++tick) {
        const auto* runner = unit_of(with->units[tick], 6);
        const auto* runner_alone = unit_of(without->units[tick], 6);
        const auto* frigate_ship = unit_of(with->units[tick], 3);
        const auto* frigate_alone = unit_of(without->units[tick], 3);
        corvette_same = corvette_same && runner && runner_alone && runner->position == runner_alone->position
            && runner->rotation == runner_alone->rotation;
        frigate_same = frigate_same && frigate_ship && frigate_alone && frigate_ship->position == frigate_alone->position
            && frigate_ship->rotation == frigate_alone->rotation;
        if (tick >= 30) pad_gone = pad_gone && unit_of(with->units[tick], 5) == nullptr && unit_of(with->units[tick], 4) == nullptr;
    }
    expect(pad_gone, "the damage command destroys the pad and the held frigate on tick 30");
    expect(corvette_same, "a later order that tick plans without the destroyed pad (static layer)");
    expect(frigate_same, "a later order that tick plans without the destroyed frigate (its own layer)");
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const auto other = run(destroyed, workers, true, durability);
        expect(other && other->hashes == with->hashes, std::to_string(workers) + " workers hash like 1 after the destruction");
    }
}

// The clipped-destination fixture (#266, AV-19): a corvette ordered onto a mining pad's centre
// stops at (-900, 1000), ring 6 toward it; another ordered onto a held corvette stops at
// (-100, -1000), ring 2 (the held ship's 17.755 along its facing plus the 50.19 query square).
[[nodiscard]] tactical::TacticalReplay clipped_replay() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, 1000, -20), yaw(0), {}},
        {2, pad_type, 1, at(-600, 1000, 0), yaw(0), {}},
        {3, corvette_type, 1, at(-1800, -1000, -20), yaw(0), {}},
        {4, corvette_type, 1, at(0, -1000, -20), yaw(0), {}},
    });
    replay.final_tick_count = 700;
    replay.commands = {
        command(30, 0, 1, tactical::MovePayload{at(-600, 1000, 0)}),
        command(30, 1, 3, tactical::MovePayload{at(0, -1000, 0)}),
    };
    return replay;
}

[[nodiscard]] std::vector<std::string> read_rows(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back(); // CRLF checkouts
        rows.push_back(line);
    }
    if (!rows.empty()) rows.erase(rows.begin());
    return rows;
}

void test_clipped_fixture(const std::optional<std::filesystem::path>& fixtures, const bool regenerate) {
    const auto replay = clipped_replay();
    const auto reference = run(replay, 1, false);
    expect(reference.has_value(), "the clipped-destination session runs");
    if (!reference) return;
    for (const std::size_t workers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        for (const bool scramble : {false, true}) {
            const auto other = run(replay, workers, scramble);
            expect(other && other->hashes == reference->hashes,
                "clipped: " + std::to_string(workers) + " workers" + (scramble ? ", scrambled," : "") + " hash like 1 worker");
        }
    }
    const auto& last = reference->units.back();
    const auto* runner = unit_of(last, 1);
    const auto* follower = unit_of(last, 3);
    expect(runner && near(runner->position.x, units(-900), one / 64) && near(runner->position.y, units(1000), one / 64),
        "the corvette sent onto the pad stops at (-900, 1000) (MV-32: within 1/64 unit)");
    expect(follower && near(follower->position.x, units(-100), one / 64) && near(follower->position.y, units(-1000), one / 64),
        "the corvette sent onto the held corvette stops at (-100, -1000)");
    const auto& before = reference->units[reference->units.size() - 2];
    expect(runner && follower && unit_of(before, 1)->position == runner->position
               && unit_of(before, 3)->position == follower->position,
        "both corvettes are at rest by the fixture's end");
    if (!fixtures) return;
    const auto encoded = tactical::write_replay(replay);
    expect(static_cast<bool>(encoded), "the clipped fixture writes");
    if (!encoded) return;
    std::vector<std::string> rows;
    for (std::size_t tick = 0; tick < reference->hashes.size(); ++tick) {
        rows.push_back(std::to_string(tick + 1) + "," + reference->hashes[tick]);
    }
    const auto replay_path = *fixtures / "tactical-motion-clipped.eawr-replay";
    const auto hashes_path = *fixtures / "tactical-motion-clipped.hashes.csv";
    if (regenerate) {
        std::ofstream(replay_path, std::ios::binary)
            .write(reinterpret_cast<const char*>(encoded.value().data()), static_cast<std::streamsize>(encoded.value().size()));
        std::ofstream hashes(hashes_path, std::ios::binary);
        hashes << "tick,sha256\n";
        for (const auto& row : rows) hashes << row << '\n';
        std::cout << "regenerated tactical-motion-clipped\n";
        return;
    }
    std::ifstream file(replay_path, std::ios::binary);
    const std::vector<std::uint8_t> committed{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    expect(committed == encoded.value(), "clipped: the committed replay is the fixture's bytes");
    expect(rows == read_rows(hashes_path), "clipped: state hashes match the golden");
}

} // namespace

int main(const int argc, char** argv) {
    const bool regenerate = argc == 3 && std::string_view(argv[2]) == "--regenerate";
    if (argc > 3 || (argc == 3 && !regenerate)) {
        std::cerr << "usage: tactical_pathfind_tests [<fixture directory> [--regenerate]]\n";
        return 2;
    }
    const auto fixtures = argc >= 2 ? std::optional<std::filesystem::path>(argv[1]) : std::nullopt;
    test_validation();
    test_collision();
    test_open_space();
    test_static_layer();
    test_nearest_open();
    test_session();
    test_same_tick_destruction();
    test_clipped_fixture(fixtures, regenerate);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "pathfind tests passed\n";
    return 0;
}
