// #506: squadron craft are drawn nose first. FoC's fighter locomotor moves a craft along its
// facing, pitch included (docs/behaviour/space-fighters.md FM-02, FM-05), so the direction the
// viewer draws a craft's nose in (presentation::space::interpolate_units) must follow the
// direction the craft travels. A synthetic carrier launches a squadron nose down (FL-06) at a
// frigate above it, so the craft dive, climb and turn; over 300 ticks every drawn craft keeps its
// nose on its travel.
#include "eawr/platform/sim_workers.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;
namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr double degrees_per_radian = 180.0 / std::numbers::pi;
constexpr tactical::PlayerId empire = 1;
constexpr tactical::PlayerId rebel = 2;
constexpr tactical::TypeId carrier_type = 100;
constexpr tactical::TypeId craft_type = 200;
constexpr tactical::TypeId squadron_type = 300;
constexpr tactical::TypeId frigate_type = 400;

using Vec = std::array<double, 3>;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}

// A TIE-fighter-like craft (FM-01) in a three-craft squadron launched from one bay.
[[nodiscard]] tactical::MotionTable motion() {
    tactical::MotionTable table;
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
    table.squadrons.craft = {craft};
    table.squadrons.squadrons = {{squadron_type, {craft_type, craft_type, craft_type},
        {at(0, 0), at(-10, 10), at(-10, -10)}, units(1000), units(200), units(20)}};
    tactical::SpawnerProfile spawner;
    spawner.type_id = carrier_type;
    spawner.entries = {{squadron_type, 1, 0}};
    spawner.delay_frames = 150;
    spawner.bays = {{0, at(20, 0, -10), at(1, 0, -1)}};
    table.squadrons.spawners = {spawner};
    return table;
}

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    table.profiles = {tactical::DurabilityProfile{carrier_type, units(5000), std::nullopt, false, {}},
        tactical::DurabilityProfile{craft_type, units(90), units(5), false, {}},
        tactical::DurabilityProfile{frigate_type, units(30000), units(3), false, {}}};
    return table;
}

[[nodiscard]] tactical::CombatTable combat() {
    tactical::CombatTable table;
    tactical::PrioritySet set;
    set.unlisted = units(1000);
    set.rows = {{frigate_type, units(1)}};
    table.priority_sets = {set};
    tactical::CombatProfile craft;
    craft.type_id = craft_type;
    craft.category_bits = 1;
    craft.priority_set = 0;
    craft.max_attack_distance = units(500);
    table.profiles = {tactical::CombatProfile{carrier_type, 2, std::nullopt, std::nullopt, {}, {}, {}}, craft,
        tactical::CombatProfile{frigate_type, 4, std::nullopt, std::nullopt, {}, {}, {}}};
    return table;
}

// The nose of Rz(yaw) Ry(pitch) Rx(roll): its +X axis.
[[nodiscard]] Vec nose(const space::LiveUnitPose& pose) {
    const double yaw = pose.yaw_degrees / degrees_per_radian;
    const double pitch = pose.pitch_degrees / degrees_per_radian;
    return {std::cos(yaw) * std::cos(pitch), std::sin(yaw) * std::cos(pitch), -std::sin(pitch)};
}

[[nodiscard]] double angle_between(const Vec& a, const Vec& b) {
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const double lengths = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
        * std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    return std::acos(std::clamp(dot / lengths, -1.0, 1.0)) * degrees_per_radian;
}

[[nodiscard]] const space::LiveUnitPose* find(const std::vector<space::LiveUnitPose>& poses, const eawr::sim::EntityId id) {
    const auto found = std::find_if(poses.begin(), poses.end(), [id](const auto& pose) { return pose.entity == id; });
    return found == poses.end() ? nullptr : &*found;
}

// FM-02, FM-05: a craft's travel over a tick is its velocity, which lies along its facing at the
// end of that tick; the drawn nose at that tick must lie along it, and half way through the tick
// it lies between the two drawn noses.
void test_nose_follows_travel() {
    tactical::TacticalSetup setup;
    setup.seed = 506;
    setup.players = {{empire, 1, 1, tactical::player_flag_commandable}, {rebel, 2, 2, tactical::player_flag_commandable}};
    setup.units = {tactical::UnitState{1, carrier_type, empire, at(0, 0), math::identity_quat(), {}},
        tactical::UnitState{2, frigate_type, rebel, at(900, 300, 250), math::identity_quat(), {}}};
    const std::vector<tactical::SensorProfile> sensors{{carrier_type, units(8000)}, {craft_type, units(8000)},
        {frigate_type, units(8000)}};
    auto created = tactical::TacticalSession::create(setup, sensors, durability(), motion(), std::nullopt, combat());
    expect(static_cast<bool>(created), "the session builds");
    if (!created) return;
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(1);
    std::shared_ptr<const tactical::TacticalSnapshot> previous = session.snapshot();
    std::size_t samples{};
    double steepest{};
    double worst_end{};
    double worst_half{};
    for (int tick = 0; tick < 300; ++tick) {
        const auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "step failed: " + stepped.error().message);
            return;
        }
        const auto latest = stepped.value().snapshot;
        const auto end = space::interpolate_units(*previous, *latest, 1.0, empire);
        const auto half = space::interpolate_units(*previous, *latest, 0.5, empire);
        const auto begin = space::interpolate_units(*previous, *latest, 0.0, empire);
        for (const auto& squadron : session.squadrons()) {
            for (const auto craft : squadron.members) {
                const auto* before = find(begin, craft);
                const auto* after = find(end, craft);
                const auto* middle = find(half, craft);
                if (before == nullptr || after == nullptr || middle == nullptr) continue;
                const Vec travel{after->position[0] - before->position[0], after->position[1] - before->position[1],
                                 after->position[2] - before->position[2]};
                if (std::hypot(travel[0], travel[1], travel[2]) < 0.5) continue;
                ++samples;
                steepest = std::max(steepest, std::abs(after->pitch_degrees));
                const double at_end = angle_between(nose(*after), travel);
                worst_end = std::max(worst_end, at_end);
                if (at_end > 0.5) {
                    expect(false, "tick " + std::to_string(stepped.value().completed_tick) + " craft "
                        + std::to_string(craft) + ": nose " + std::to_string(at_end) + " degrees off its travel");
                }
                // Half way the nose has turned half way: within half the tick's turn of the travel.
                const double turn = angle_between(nose(*before), nose(*after));
                const double at_half = angle_between(nose(*middle), travel);
                worst_half = std::max(worst_half, at_half - turn / 2.0);
                if (at_half > turn / 2.0 + 0.5) {
                    expect(false, "tick " + std::to_string(stepped.value().completed_tick) + " craft "
                        + std::to_string(craft) + ": half-way nose " + std::to_string(at_half) + " degrees off");
                }
            }
        }
        previous = latest;
    }
    expect(samples > 300, "the squadron flies for most of the run (" + std::to_string(samples) + " samples)");
    expect(steepest > 20.0, "the craft climb or dive (steepest " + std::to_string(steepest) + " degrees)");
    std::cout << "fighter heading: " << samples << " samples, steepest pitch " << steepest << ", worst end "
              << worst_end << ", worst half excess " << worst_half << '\n';
}

[[nodiscard]] math::Mat3x4 rotation(const double yaw_degrees, const double pitch_degrees, const double roll_degrees) {
    const auto fixed = [](const double value) {
        return Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * static_cast<double>(one))));
    };
    const double y = yaw_degrees / degrees_per_radian;
    const double p = pitch_degrees / degrees_per_radian;
    const double r = roll_degrees / degrees_per_radian;
    const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r),
                 sr = std::sin(r);
    math::Mat3x4 matrix{};
    matrix.rows[0] = {fixed(cy * cp), fixed(cy * sp * sr - sy * cr), fixed(cy * sp * cr + sy * sr), fixed(0.0)};
    matrix.rows[1] = {fixed(sy * cp), fixed(sy * sp * sr + cy * cr), fixed(sy * sp * cr - cy * sr), fixed(0.0)};
    matrix.rows[2] = {fixed(-sp), fixed(cp * sr), fixed(cp * cr), fixed(0.0)};
    return matrix;
}

// FM-02, FM-06: the pitch is read from the transform, and a craft looping over the vertical
// (pitch 80 to 95, which reads as yaw 180, pitch 85, roll 180) keeps its nose on the loop.
void test_pitch_and_loop() {
    expect(std::abs(space::instance_pitch_degrees(rotation(30.0, -40.0, 10.0)) + 40.0) < 1.0e-3,
           "the pitch is read from the forward column");
    expect(space::instance_pitch_degrees(rotation(30.0, 0.0, 10.0)) == 0.0, "a level unit has no pitch");
    const tactical::TacticalSnapshot previous{10, {{1, 1}}, {{1, craft_type, 1, 1, rotation(0.0, 80.0, 0.0), 0b01, {}, {}}}, {}};
    const tactical::TacticalSnapshot latest{11, {{1, 1}}, {{1, craft_type, 1, 1, rotation(0.0, 95.0, 0.0), 0b01, {}, {}}}, {}};
    const auto end = space::interpolate_units(previous, latest, 1.0, 1);
    const auto half = space::interpolate_units(previous, latest, 0.5, 1);
    expect(end.size() == 1 && half.size() == 1, "the craft is drawn");
    if (end.size() != 1 || half.size() != 1) return;
    expect(std::abs(end[0].pitch_degrees - 85.0) < 1.0e-3 && std::abs(std::abs(end[0].yaw_degrees) - 180.0) < 1.0e-3,
           "past the vertical the pose reads as the same rotation turned about");
    const double wanted = 87.5 / degrees_per_radian;
    expect(angle_between(nose(half[0]), {std::cos(wanted), 0.0, -std::sin(wanted)}) < 1.0e-2,
           "half way through the loop the nose is half way over");
    // A level ship keeps the #351 per-angle easing.
    const tactical::TacticalSnapshot ship_before{10, {{1, 1}}, {{1, craft_type, 1, 1, rotation(40.0, 0.0, -4.0), 0b01, {}, {}}}, {}};
    const tactical::TacticalSnapshot ship_after{11, {{1, 1}}, {{1, craft_type, 1, 1, rotation(41.0, 0.0, -6.0), 0b01, {}, {}}}, {}};
    const auto ship = space::interpolate_units(ship_before, ship_after, 0.25, 1);
    expect(ship.size() == 1 && ship[0].pitch_degrees == 0.0 && std::abs(ship[0].roll_degrees + 4.5) < 1.0e-3,
           "a level unit draws no pitch");
    // #479 (FM-06): a level craft rolling upright out of a loop crosses +-180 between two ticks;
    // half way it is drawn upside down, not rolled a whole turn the long way through level.
    const tactical::TacticalSnapshot inverted_before{10, {{1, 1}}, {{1, craft_type, 1, 1, rotation(180.0, 0.0, 177.0), 0b01, {}, {}}}, {}};
    const tactical::TacticalSnapshot inverted_after{11, {{1, 1}}, {{1, craft_type, 1, 1, rotation(180.0, 0.0, -177.0), 0b01, {}, {}}}, {}};
    const auto rolling = space::interpolate_units(inverted_before, inverted_after, 0.5, 1);
    expect(rolling.size() == 1 && std::abs(std::abs(rolling[0].roll_degrees) - 180.0) < 1.0e-3,
           "a roll across +-180 turns the short way (drawn " + std::to_string(rolling.empty() ? 0.0 : rolling[0].roll_degrees) + ")");
}

} // namespace

int main() {
    test_pitch_and_loop();
    test_nose_follows_travel();
    if (failures != 0) {
        std::cerr << failures << " fighter heading check(s) failed\n";
        return 1;
    }
    std::cout << "fighter heading contracts passed\n";
    return 0;
}
