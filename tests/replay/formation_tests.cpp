#include "formation_support.hpp"
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
namespace formation_test_support {


int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}


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


[[nodiscard]] std::optional<Run> run(const tactical::TacticalReplay& replay, const std::size_t workers, const bool scramble,
    const tactical::MotionTable& table) {
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

} // namespace

// `--trace <file.csv>` also writes the mixed group's positions (C-12).
using namespace formation_test_support;

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
