#include "fighter_support.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #75 fighter squadrons (docs/behaviour/space-fighters.md, cases C-01 to C-06): the hangar
// (FL-01 to FL-08) on a synthetic spawner, a launch from a bay, the squadron's priority-set
// scan (FT-02, FT-03) and worker-count hash equality of a launch-and-attack session.
namespace fighter_test_support {


int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}


[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {units(x), units(y), units(z)};
}

// Entries A (1 alive, reserve 2) and B (1 alive, no reserve), a 150-frame delay, one bay.
[[nodiscard]] tactical::SpawnerProfile spawner() {
    tactical::SpawnerProfile profile;
    profile.type_id = carrier_type;
    profile.entries = {{squadron_a, 1, 2}, {squadron_b, 1, 0}};
    profile.delay_frames = 150;
    profile.bays = {{0, at(20, 0, -10), at(1, 0, -1)}};
    return profile;
}

[[nodiscard]] tactical::MotionTable motion() {
    tactical::MotionTable table;
    auto& squadrons = table.squadrons;
    tactical::CraftProfile craft;
    craft.type_id = craft_type;
    craft.max_speed = Fixed::from_raw(one * 54 / 10);
    craft.min_speed = Fixed::from_raw(one * 18 / 10);
    craft.rate_of_turn = units(6);
    craft.lift = units(6);
    craft.thrust = Fixed::from_raw(one / 5);
    craft.roll_rate = units(6);
    craft.bank_angle = units(70);
    craft.strafe_distance = units(200);
    squadrons.craft = {craft};
    squadrons.squadrons = {
        {squadron_a, {craft_type, craft_type}, {at(0, 0), at(-10, 10)}, units(1000), units(200), units(300), units(20)},
        {squadron_b, {craft_type}, {at(0, 0)}, units(1000), units(200), units(300), units(20)}};
    squadrons.spawners = {spawner()};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    table.profiles = {tactical::DurabilityProfile{carrier_type, units(5000), std::nullopt, false, {}},
        tactical::DurabilityProfile{craft_type, units(90), units(5), false, {}},
        tactical::DurabilityProfile{frigate_type, units(3000), units(3), false, {}},
        tactical::DurabilityProfile{corvette_type, units(1000), units(3), false, {}}};
    return table;
}

// Craft attack by the set (frigate 1, corvette 3); a ship-level object weapon so they fire.
[[nodiscard]] tactical::CombatTable combat() {
    tactical::CombatTable table;
    tactical::PrioritySet set;
    set.unlisted = units(1000);
    set.rows = {{frigate_type, units(1)}, {corvette_type, units(3)}};
    table.priority_sets = {set};
    tactical::CombatProfile craft;
    craft.type_id = craft_type;
    craft.category_bits = 1;
    craft.priority_set = 0;
    craft.max_attack_distance = units(500);
    tactical::WeaponProfile gun;
    gun.range = units(500);
    gun.min_recharge_hundredths = 100;
    gun.max_recharge_hundredths = 100;
    craft.weapons = {gun};
    table.profiles = {tactical::CombatProfile{carrier_type, 2, std::nullopt, std::nullopt, {}, {}, {}}, craft,
        tactical::CombatProfile{frigate_type, 4, std::nullopt, std::nullopt, {}, {}, {}},
        tactical::CombatProfile{corvette_type, 4, std::nullopt, std::nullopt, {}, {}, {}}};
    return table;
}

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    return {{carrier_type, units(8000)}, {craft_type, units(8000)}, {frigate_type, units(8000)}, {corvette_type, units(8000)}};
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3& position) {
    return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
}

[[nodiscard]] std::vector<tactical::Player> players() {
    return {{empire, 1, 1, tactical::player_flag_commandable}, {rebel, 2, 2, tactical::player_flag_commandable}};
}

[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSnapshot& snapshot,
    const eawr::sim::EntityId id) {
    for (const auto& entry : snapshot.instances()) {
        if (entry.entity_id == id) return &entry;
    }
    return nullptr;
}

[[nodiscard]] math::Vec3 position(const tactical::TacticalInstance& found) {
    return {found.fixed_transform.rows[0][3], found.fixed_transform.rows[1][3], found.fixed_transform.rows[2][3]};
}

} // namespace

using namespace fighter_test_support;

int main() {
    test_hangar();
    test_destroyed_bay();
    test_validation();
    test_finite_reserve_boundary();
    test_formation_catch_up();
    test_coordinate_limit();
    test_launch_state();
    test_out_of_combat();
    test_approach();
    test_launch();
    test_launched_squadron();
    test_priority_scan();
    test_launch_idle();
    test_worker_equality();
    test_squadron_orders();
    test_squadron_attack_move();
    test_squadron_guard();
    test_idle_cell_claim();
    test_idle_grid();
    test_attack_order_approach();
    test_team_attack();
    test_team_retarget();
    test_squadron_hardpoint_attack();
    test_empty_table();
    test_staged_flight_rollback();
    if (failures != 0) {
        std::cerr << failures << " fighter contract failure(s)\n";
        return 1;
    }
    std::cout << "fighter contracts passed\n";
    return 0;
}
