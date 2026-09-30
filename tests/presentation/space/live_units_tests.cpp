// #80 part A: where the viewer draws a live session's units between two published ticks
// (presentation::space::interpolate_units). Synthetic snapshots only.
#include "eawr/presentation/space/live_units.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;
namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;

int failures{};

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

[[nodiscard]] bool near(const double a, const double b) { return std::abs(a - b) <= 1.0e-3; }

// A yaw-only transform at (x, y, 0): forward column (cos, sin).
[[nodiscard]] math::Mat3x4 pose(const double x, const double y, const double yaw_degrees) {
    const auto fixed = [](const double value) {
        return math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * 16777216.0)));
    };
    const double radians = yaw_degrees * 3.14159265358979323846 / 180.0;
    math::Mat3x4 matrix{};
    matrix.rows[0] = {fixed(std::cos(radians)), fixed(-std::sin(radians)), fixed(0.0), fixed(x)};
    matrix.rows[1] = {fixed(std::sin(radians)), fixed(std::cos(radians)), fixed(0.0), fixed(y)};
    matrix.rows[2] = {fixed(0.0), fixed(0.0), fixed(1.0), fixed(0.0)};
    return matrix;
}

// Rz(yaw) Rx(roll) at the origin (#351, space-movement BK-05).
[[nodiscard]] math::Mat3x4 banked(const double yaw_degrees, const double roll_degrees) {
    const auto fixed = [](const double value) {
        return math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * 16777216.0)));
    };
    const double y = yaw_degrees * 3.14159265358979323846 / 180.0;
    const double r = roll_degrees * 3.14159265358979323846 / 180.0;
    math::Mat3x4 matrix{};
    matrix.rows[0] = {fixed(std::cos(y)), fixed(-std::sin(y) * std::cos(r)), fixed(std::sin(y) * std::sin(r)), fixed(0.0)};
    matrix.rows[1] = {fixed(std::sin(y)), fixed(std::cos(y) * std::cos(r)), fixed(-std::cos(y) * std::sin(r)), fixed(0.0)};
    matrix.rows[2] = {fixed(0.0), fixed(std::sin(r)), fixed(std::cos(r)), fixed(0.0)};
    return matrix;
}

// Players 1 (team 1) and 2 (team 2); visible_to bit 0 is player 1.
[[nodiscard]] tactical::TacticalSnapshot snapshot(const std::uint64_t tick, std::vector<tactical::TacticalInstance> units) {
    return {tick, {{1, 1}, {2, 2}}, std::move(units), {}};
}

void test_interpolation() {
    const auto previous = snapshot(10, {
        {1, 100, 1, 1, pose(0.0, 0.0, 170.0), 0b01, {}, {}},
        {2, 100, 2, 2, pose(50.0, 0.0, 0.0), 0b11, {}, {}},
        {3, 100, 1, 1, pose(0.0, 0.0, 0.0), 0b01, {}, {}},
    });
    const auto latest = snapshot(11, {
        {1, 100, 1, 1, pose(10.0, -20.0, -170.0), 0b01, {}, {}},
        {2, 100, 2, 2, pose(60.0, 0.0, 0.0), 0b10, {}, {}},
        {4, 100, 1, 1, pose(5.0, 5.0, 90.0), 0b01, {}, {}},
    });
    const auto half = space::interpolate_units(previous, latest, 0.5, 1);
    expect(half.size() == 2, "player 1 draws its own units and not the enemy it no longer sees");
    if (half.size() == 2) {
        expect(half[0].entity == 1 && near(half[0].position[0], 5.0) && near(half[0].position[1], -10.0),
               "a unit in both ticks moves half way");
        expect(near(std::abs(half[0].yaw_degrees), 180.0), "the turn from 170 to -170 goes the short way through 180");
        expect(half[1].entity == 4 && near(half[1].position[0], 5.0) && near(half[1].yaw_degrees, 90.0),
               "a new unit stands at its latest pose");
        expect(half[0].instance == &latest.instances()[0], "the pose points at the newer instance");
    }
    const auto enemy = space::interpolate_units(previous, latest, 0.25, 2);
    expect(enemy.size() == 1 && enemy[0].entity == 2 && near(enemy[0].position[0], 52.5),
           "player 2 sees only its own unit");
    const auto clamped = space::interpolate_units(previous, latest, 7.0, 1);
    expect(!clamped.empty() && near(clamped[0].position[0], 10.0), "alpha above 1 holds the latest pose");
    expect(space::interpolate_units(previous, latest, 0.5, 9).empty(), "an unknown player sees nothing");
    expect(near(space::instance_yaw_degrees(pose(0.0, 0.0, -90.0)), -90.0), "yaw is read from the forward column");
    expect(near(space::instance_yaw_degrees(pose(0.0, 0.0, 180.0)), 180.0), "yaw 180 stays in (-180, 180]");
    expect(!half.empty() && near(half[0].roll_degrees, 0.0), "a level unit draws no roll");
}

// #351: the bank roll is read from the transform and eases between ticks like the position.
void test_roll() {
    expect(near(space::instance_roll_degrees(banked(40.0, -12.0)), -12.0), "roll is read about the forward axis");
    expect(near(space::instance_yaw_degrees(banked(40.0, -12.0)), 40.0), "a roll leaves the yaw");
    const auto previous = snapshot(10, {{1, 100, 1, 1, banked(40.0, -4.0), 0b01, {}, {}}});
    const auto latest = snapshot(11, {{1, 100, 1, 1, banked(41.0, -6.0), 0b01, {}, {}}});
    const auto poses = space::interpolate_units(previous, latest, 0.25, 1);
    expect(poses.size() == 1 && near(poses[0].roll_degrees, -4.5) && near(poses[0].yaw_degrees, 40.25),
           "roll and yaw ease a quarter of the way");
}

// #535: `fading` keeps drawing an entity `viewer` no longer sees, at its true (still moving)
// position, without duplicating one the visibility test already includes.
void test_fading_entities() {
    const auto previous = snapshot(10, {
        {1, 100, 1, 1, pose(0.0, 0.0, 0.0), 0b01, {}, {}},
        {2, 100, 2, 2, pose(0.0, 0.0, 0.0), 0b00, {}, {}},
    });
    const auto latest = snapshot(11, {
        {1, 100, 1, 1, pose(10.0, 0.0, 0.0), 0b01, {}, {}},
        {2, 100, 2, 2, pose(10.0, 0.0, 0.0), 0b00, {}, {}},
    });
    const std::vector<eawr::sim::EntityId> fading{2};
    const auto poses = space::interpolate_units(previous, latest, 0.5, 1, false, fading);
    expect(poses.size() == 2, "a fading entity outside visible_entities is drawn alongside it");
    if (poses.size() == 2) {
        expect(poses[1].entity == 2 && near(poses[1].position[0], 5.0),
               "the fading entity still eases its true position between ticks");
    }
    const std::vector<eawr::sim::EntityId> fading_own{1};
    expect(space::interpolate_units(previous, latest, 0.5, 1, false, fading_own).size() == 1,
           "an entity already visible is not drawn twice when it is also listed as fading");
}

// #80 (--eawr-live-reveal, a viewer debug aid): the reveal flag bypasses TacticalSnapshot
// visibility entirely, a presentation-only draw-path choice with no effect on the snapshots.
void test_reveal() {
    const auto previous = snapshot(10, {
        {1, 100, 1, 1, pose(0.0, 0.0, 170.0), 0b01, {}, {}},
        {2, 100, 2, 2, pose(50.0, 0.0, 0.0), 0b11, {}, {}},
    });
    const auto latest = snapshot(11, {
        {1, 100, 1, 1, pose(10.0, -20.0, -170.0), 0b01, {}, {}},
        {2, 100, 2, 2, pose(60.0, 0.0, 0.0), 0b10, {}, {}},
        {4, 100, 1, 1, pose(5.0, 5.0, 90.0), 0b01, {}, {}},
    });
    const auto hidden = space::interpolate_units(previous, latest, 0.5, 2);
    expect(hidden.size() == 1 && hidden[0].entity == 2, "without reveal player 2 sees only its own unit");
    const auto revealed = space::interpolate_units(previous, latest, 0.5, 2, true);
    expect(revealed.size() == 3, "reveal draws every instance of the latest snapshot");
    if (revealed.size() == 3) {
        expect(revealed[0].entity == 1 && revealed[1].entity == 2 && revealed[2].entity == 4,
               "reveal keeps ascending entity order");
        expect(near(revealed[0].position[0], 5.0) && near(revealed[0].position[1], -10.0),
               "a revealed unit still interpolates between its two ticks");
    }
    expect(space::interpolate_units(previous, latest, 0.5, 9, true).size() == 3,
           "reveal draws every instance even for a viewer the snapshot never names");
}

// Rz(yaw) Ry(pitch) Rx(roll) at (x, 0, 0) (space-fighters FM-02; positive pitch lowers the nose).
[[nodiscard]] math::Mat3x4 craft(const double x, const double yaw_degrees, const double pitch_degrees,
                                 const double roll_degrees) {
    const auto fixed = [](const double value) {
        return math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * 16777216.0)));
    };
    constexpr double radians = 3.14159265358979323846 / 180.0;
    const double cy = std::cos(yaw_degrees * radians), sy = std::sin(yaw_degrees * radians);
    const double cp = std::cos(pitch_degrees * radians), sp = std::sin(pitch_degrees * radians);
    const double cr = std::cos(roll_degrees * radians), sr = std::sin(roll_degrees * radians);
    math::Mat3x4 matrix{};
    matrix.rows[0] = {fixed(cy * cp), fixed(cy * sp * sr - sy * cr), fixed(cy * sp * cr + sy * sr), fixed(x)};
    matrix.rows[1] = {fixed(sy * cp), fixed(sy * sp * sr + cy * cr), fixed(sy * sp * cr - cy * sr), fixed(0.0)};
    matrix.rows[2] = {fixed(-sp), fixed(cp * sr), fixed(cp * cr), fixed(0.0)};
    return matrix;
}

[[nodiscard]] tactical::TacticalSnapshot spinning(const std::uint64_t tick, std::vector<tactical::TacticalInstance> units,
                                                  std::vector<tactical::SpinningCraft> spins) {
    return {tick, {{1, 1}, {2, 2}}, std::move(units), {}, {}, {}, std::nullopt, std::move(spins)};
}

[[nodiscard]] tactical::SpinningCraft spin(const double x, const double pitch_degrees, const double roll_degrees) {
    const auto fixed = [](const double value) {
        return math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * 16777216.0)));
    };
    return {5, 100, 1, craft(x, 0.0, pitch_degrees, roll_degrees), fixed(roll_degrees), fixed(pitch_degrees), fixed(0.0),
            0b01};
}

// #447 with #506: a killed craft spinning away eases from its live pitched pose in its death tick,
// then from spin pose to spin pose, along the shortest arc between the two rotations.
void test_spinning() {
    const auto died = spinning(10, {{5, 100, 1, 1, craft(0.0, 0.0, 30.0, 0.0), 0b01, {}, {}}}, {});
    const auto first = spinning(11, {}, {spin(10.0, 30.0, 20.0)});
    const auto half = space::interpolate_spinning(died, first, 0.5, 1);
    expect(half.size() == 1, "the owner sees its spinning craft");
    if (half.size() == 1) {
        expect(half[0].spinning && half[0].instance == nullptr, "a spinning craft is no instance");
        expect(near(half[0].position[0], 5.0), "it moves half way from where the craft died");
        expect(near(half[0].pitch_degrees, 30.0) && near(half[0].roll_degrees, 10.0) && near(half[0].yaw_degrees, 0.0),
               "it keeps the live craft's pitch and rolls half way");
    }
    expect(space::interpolate_spinning(died, first, 0.5, 2).empty(), "the enemy does not see it");
    expect(space::interpolate_spinning(died, first, 0.5, 2, true).size() == 1, "reveal draws it for the enemy too");
    const auto second = spinning(12, {}, {spin(20.0, 30.0, 40.0)});
    const auto quarter = space::interpolate_spinning(first, second, 0.25, 1);
    expect(quarter.size() == 1 && near(quarter[0].position[0], 12.5) && near(quarter[0].roll_degrees, 25.0)
               && near(quarter[0].pitch_degrees, 30.0),
           "between two spin poses it rolls a quarter of the way");
}

} // namespace

int main() {
    test_interpolation();
    test_roll();
    test_reveal();
    test_fading_entities();
    test_spinning();
    if (failures != 0) {
        std::cerr << failures << " live unit check(s) failed\n";
        return 1;
    }
    std::cout << "live unit presentation contracts passed\n";
    return 0;
}
