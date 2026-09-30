#include "eawr/sim/tactical/fighters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

// #479 flight feel (docs/behaviour/space-fighters.md, cases C-18 to C-21): a squadron flown by the
// fighter locomotor as the session flies it (every craft reads the frame's copied views, the
// leader first) with FoC's X-wing values. No craft slides off its nose (FM-07), the bank leans
// into every turn and follows the turn's size (FM-03), a reversal is a loop through the vertical
// and a roll out, never a jump (FM-06), and the formation banks with its leader (FM-14).
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr double degrees_per_radian = 180.0 / std::numbers::pi;

[[nodiscard]] Fixed tenths(const std::int64_t value) { return Fixed::from_raw(one * value / 10); }
[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z = 0) {
    return {units(x), units(y), units(z)};
}
[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

// FoC's X-wing (SPACEUNITSFIGHTERS.XML) with FM-01's space multiplier of 1.2 on the speeds and
// rates, the thrust unscaled.
[[nodiscard]] tactical::CraftProfile x_wing() {
    tactical::CraftProfile craft;
    craft.type_id = 200;
    craft.max_speed = tenths(48);
    craft.min_speed = tenths(30);
    craft.rate_of_turn = tenths(36);
    craft.lift = tenths(48);
    craft.thrust = units(1);
    craft.roll_rate = units(6);
    craft.bank_angle = units(40);
    craft.strafe_distance = units(500);
    return craft;
}

using Vec = std::array<double, 3>;
using Matrix = std::array<Vec, 3>;

// Rz(yaw) Ry(pitch) Rx(roll), the craft's drawn frame (craft_rotation, FM-02).
[[nodiscard]] Matrix frame_of(const tactical::CraftState& state) {
    const double y = real(state.yaw) / degrees_per_radian;
    const double p = real(state.pitch) / degrees_per_radian;
    const double r = real(state.roll) / degrees_per_radian;
    const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p), cr = std::cos(r),
                 sr = std::sin(r);
    return {{{cy * cp, cy * sp * sr - sy * cr, cy * sp * cr + sy * sr},
             {sy * cp, sy * sp * sr + cy * cr, sy * sp * cr - cy * sr},
             {-sp, cp * sr, cp * cr}}};
}

[[nodiscard]] Vec nose(const tactical::CraftState& state) {
    const auto m = frame_of(state);
    return {m[0][0], m[1][0], m[2][0]};
}

[[nodiscard]] double angle_between(const Vec& a, const Vec& b) {
    const double dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    const double lengths = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2])
        * std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    return lengths == 0.0 ? 0.0 : std::acos(std::clamp(dot / lengths, -1.0, 1.0)) * degrees_per_radian;
}

// The angle of the rotation taking one frame to the other.
[[nodiscard]] double turned(const tactical::CraftState& from, const tactical::CraftState& to) {
    const auto a = frame_of(from);
    const auto b = frame_of(to);
    double trace = 0.0;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) trace += a[row][column] * b[row][column];
    }
    return std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0)) * degrees_per_radian;
}

[[nodiscard]] double wrap(double degrees) {
    degrees = std::fmod(degrees, 360.0);
    if (degrees > 180.0) degrees -= 360.0;
    if (degrees <= -180.0) degrees += 360.0;
    return degrees;
}

[[nodiscard]] Vec vec(const math::Vec3& value) { return {real(value.x), real(value.y), real(value.z)}; }

// A squadron flying a player move (FO-01) the way the session steps it.
struct Squadron {
    tactical::CraftProfile profile = x_wing();
    std::vector<tactical::CraftView> craft;
    std::vector<math::Vec3> offsets;
    Fixed speed_factor = units(1); // AB-24: TURBO scales the maximum speed

    // One frame: every craft reads the views as they stood before it, the leader is craft 0.
    [[nodiscard]] std::vector<tactical::CraftView> step(const math::Vec3& destination) {
        const std::vector<tactical::CraftView> before = craft;
        for (std::size_t index = 0; index < craft.size(); ++index) {
            tactical::CraftFrame frame;
            frame.self = &before[index];
            frame.leader = &before[0];
            frame.own_offset = offsets[index];
            frame.leader_offset = offsets[0];
            frame.formation_tolerance = units(25);
            frame.moving = true;
            frame.hold = destination;
            frame.speed_factor = speed_factor;
            const auto stepped = tactical::step_craft(frame);
            expect(static_cast<bool>(stepped), "the craft steps");
            if (!stepped) continue;
            craft[index].position = stepped.value().position;
            craft[index].state = stepped.value().state;
        }
        return before;
    }
};

// FoC's X-wing squadron (squadrons.xml Squadron_Offsets) flying east at full speed in formation.
[[nodiscard]] Squadron x_wings() {
    Squadron squadron;
    squadron.offsets = {at(30, 0), at(0, 15), at(0, -15), at(-30, 30), at(-30, -30)};
    for (std::size_t index = 0; index < squadron.offsets.size(); ++index) {
        tactical::CraftView view{static_cast<eawr::sim::EntityId>(index + 1), {}, {}, &squadron.profile};
        const auto& slot = squadron.offsets[index];
        view.position = {Fixed::from_raw(slot.x.raw() - one * 30), slot.y, slot.z};
        view.state.velocity = {squadron.profile.max_speed, Fixed{}, Fixed{}};
        squadron.craft.push_back(view);
    }
    return squadron;
}

// C-18 (FM-07) and C-19 (FM-03): a squadron flies east, reverses (a turn over 170 degrees: FM-06)
// and then curves north, at its own speed and with TURBO (AB-24: twice the maximum speed). On
// every frame of every craft the move is its velocity and the velocity lies along the nose;
// every yaw step leans into the turn; the roll moves at most its rate a frame outside the loop's
// flip; the leader banks to the full 40 degrees in the curve.
void test_nose_bank_and_turn(const std::int64_t turbo) {
    Squadron squadron = x_wings();
    squadron.speed_factor = units(turbo);
    const std::string label = turbo == 1 ? "" : " (TURBO)";
    const tactical::CraftProfile& profile = squadron.profile;
    double worst_slip = 0.0;
    double worst_roll_step = 0.0;
    double steepest_bank = 0.0;
    double formation_tilt = 0.0;
    std::size_t turning_frames = 0;
    for (int tick = 0; tick < 600; ++tick) {
        const math::Vec3 destination = tick < 60 ? at(3000, 0) : tick < 300 ? at(-3000, 0) : at(-1500, 3000);
        const auto before = squadron.step(destination);
        double low = 1.0e9;
        double high = -1.0e9;
        for (std::size_t index = 0; index < squadron.craft.size(); ++index) {
            const auto& old = before[index];
            const auto& now = squadron.craft[index];
            low = std::min(low, real(now.position.z));
            high = std::max(high, real(now.position.z));
            const math::Vec3 moved{Fixed::from_raw(now.position.x.raw() - old.position.x.raw()),
                Fixed::from_raw(now.position.y.raw() - old.position.y.raw()),
                Fixed::from_raw(now.position.z.raw() - old.position.z.raw())};
            expect(moved == now.state.velocity, "FM-07: tick " + std::to_string(tick) + " craft " + std::to_string(now.id)
                + " moves by exactly its velocity");
            const double slip = angle_between(vec(now.state.velocity), nose(now.state));
            worst_slip = std::max(worst_slip, slip);
            const double yaw_step = wrap(real(now.state.yaw) - real(old.state.yaw));
            const double roll_step = wrap(real(now.state.roll) - real(old.state.roll));
            const bool flip = std::abs(yaw_step) > 90.0; // FM-06: the loop's pitch over the vertical
            if (flip) continue;
            if (yaw_step != 0.0) {
                ++turning_frames;
                const double roll = real(now.state.roll);
                expect(roll == 0.0 || (roll < 0.0) == (yaw_step > 0.0), "FM-03: tick " + std::to_string(tick) + " craft "
                    + std::to_string(now.id) + " yaws " + std::to_string(yaw_step) + " leaning " + std::to_string(roll));
            }
            // FM-12 scales a catching-up follower's rates; the leader flies at its own.
            if (index != 0) continue;
            worst_roll_step = std::max(worst_roll_step, std::abs(roll_step));
            expect(std::abs(yaw_step) <= real(profile.rate_of_turn) + 1.0e-6, "FM-03: the leader yaws at most its rate");
            if (tick >= 300) steepest_bank = std::max(steepest_bank, std::abs(real(now.state.roll)));
        }
        if (tick >= 300 && tick < 360) formation_tilt = std::max(formation_tilt, high - low);
    }
    expect(worst_slip < 0.01, "FM-07" + label + ": no craft slides off its nose (worst " + std::to_string(worst_slip)
        + " degrees)");
    expect(worst_roll_step <= real(profile.roll_rate) + 1.0e-6, "FM-03" + label
        + ": the leader's roll moves at most its rate a frame (worst " + std::to_string(worst_roll_step) + ")");
    expect(turning_frames > 100, "the squadron turns" + label + " (" + std::to_string(turning_frames) + " turning frames)");
    expect(steepest_bank == real(profile.bank_angle), "FM-03" + label
        + ": in the curve the leader banks to its full Bank_Turn_Angle (" + std::to_string(steepest_bank) + ")");
    expect(formation_tilt > 5.0, "FM-14" + label + ": the formation banks with its leader in the curve (heights span "
        + std::to_string(formation_tilt) + ")");
    std::cout << "flight" << label << ": worst slip " << worst_slip << ", worst roll step " << worst_roll_step << ", bank "
              << steepest_bank << ", formation tilt " << formation_tilt << '\n';
}

// C-19 (FM-03): the wanted bank is the heading change still to go, up to Bank_Turn_Angle, so a
// small correction banks a little and a large turn banks fully; the roll gets there at its rate.
void test_bank_follows_the_turn() {
    const tactical::CraftProfile profile = x_wing();
    // The destination keeps its world bearing (x, y) from the craft, so the heading change still
    // to go is that bearing less the turn so far.
    const auto bank_after = [&](const std::int64_t x, const std::int64_t y, const int frames) {
        tactical::CraftView craft{1, at(0, 0), {}, &profile};
        craft.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
        for (int frame = 0; frame < frames; ++frame) {
            tactical::CraftFrame step;
            step.self = &craft;
            step.leader = &craft;
            step.moving = true;
            step.hold = {Fixed::from_raw(craft.position.x.raw() + x * one), Fixed::from_raw(craft.position.y.raw() + y * one),
                Fixed{}};
            const auto stepped = tactical::step_craft(step);
            expect(static_cast<bool>(stepped), "the craft steps");
            if (!stepped) break;
            craft.position = stepped.value().position;
            craft.state = stepped.value().state;
        }
        return craft.state;
    };
    const auto first = bank_after(-1000, 1000, 1); // 135 degrees to the left
    expect(real(first.roll) == -6.0 && std::abs(real(first.yaw) - 3.6) < 1.0e-6,
        "FM-03: the first frame of a wide left turn rolls 6 degrees (Max_Rate_Of_Roll) and yaws 3.6 (Max_Rate_Of_Turn)");
    const auto wide = bank_after(-1000, 1000, 7);
    expect(real(wide.roll) == -40.0, "FM-03: a turn of more than 40 degrees still to go banks the full 40 degrees left, got "
        + std::to_string(real(wide.roll)));
    const auto right = bank_after(-1000, -1000, 7);
    expect(real(right.roll) == 40.0 && real(right.yaw) < 0.0, "FM-03: a wide right turn banks 40 degrees right");
    const auto small = bank_after(1000, 87, 1); // a 5-degree bearing: the bank is the gap
    expect(real(small.roll) < -4.9 && real(small.roll) > -5.0,
        "FM-03: a 5-degree correction banks 5 degrees, not 40, got " + std::to_string(real(small.roll)));
}

// C-20 (FM-06): a craft flying east sent to a point behind it pitches up through the vertical,
// turns about (the flip keeps the same rotation), comes out upside down heading west and rolls
// upright: no frame turns it more than two lift steps plus a roll and a yaw step.
void test_reversal_is_a_loop() {
    const tactical::CraftProfile profile = x_wing();
    tactical::CraftView craft{1, at(0, 0), {}, &profile};
    craft.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
    const double bound = 2.0 * real(profile.lift) + real(profile.roll_rate) + real(profile.rate_of_turn);
    double worst_turn = 0.0;
    double highest = 0.0;
    double steepest = 0.0;
    int upside_down = 0;
    int reversed_at = -1;
    int upright_at = -1;
    for (int frame = 0; frame < 150; ++frame) {
        tactical::CraftFrame step;
        step.self = &craft;
        step.leader = &craft;
        step.moving = true;
        step.hold = at(-2000, 0);
        const auto stepped = tactical::step_craft(step);
        expect(static_cast<bool>(stepped), "the craft steps");
        if (!stepped) return;
        const auto previous = craft.state;
        craft.position = stepped.value().position;
        craft.state = stepped.value().state;
        worst_turn = std::max(worst_turn, turned(previous, craft.state));
        highest = std::max(highest, real(craft.position.z));
        const Vec heading = nose(craft.state);
        steepest = std::max(steepest, heading[2]);
        if (std::abs(real(craft.state.roll)) > 90.0) ++upside_down;
        if (reversed_at < 0 && heading[0] < -0.9) reversed_at = frame;
        if (reversed_at >= 0 && upright_at < 0 && std::abs(real(craft.state.roll)) < 1.0) upright_at = frame;
    }
    expect(worst_turn <= bound + 1.0e-6, "FM-06: no frame turns the craft more than " + std::to_string(bound)
        + " degrees (worst " + std::to_string(worst_turn) + ")");
    expect(steepest > 0.99, "FM-06: the nose passes through the vertical (" + std::to_string(steepest) + ")");
    expect(highest > 80.0, "FM-06: the loop climbs (" + std::to_string(highest) + " units)");
    expect(upside_down >= 10, "FM-06: the craft comes out of the half loop upside down (" + std::to_string(upside_down)
        + " frames rolled past 90 degrees)");
    expect(reversed_at >= 30 && reversed_at <= 60, "FM-06: the half loop takes over a second at 4.8 degrees a frame (frame "
        + std::to_string(reversed_at) + ")");
    expect(upright_at > reversed_at && upright_at <= reversed_at + 40,
        "FM-06: it then rolls upright at Max_Rate_Of_Roll (frame " + std::to_string(upright_at) + ")");
    std::cout << "reversal: worst turn " << worst_turn << ", highest " << highest << ", west at " << reversed_at
              << ", upright at " << upright_at << '\n';
}

// C-21 (FM-14): the slot of a follower on the right of a leader banked 40 degrees left rises
// with the bank, so the follower climbs toward it; beside a level leader it stays level.
void test_formation_banks_with_leader() {
    const tactical::CraftProfile profile = x_wing();
    const auto follower_pitch = [&](const std::int64_t leader_roll) {
        tactical::CraftView leader{1, at(0, 0), {}, &profile};
        leader.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
        leader.state.roll = units(leader_roll);
        // The follower flies level 50 units behind its level slot (FM-11: a turn radius ahead of
        // the leader plus its slot (0, -15) less the leader's (30, 0)), on the slot's line.
        tactical::CraftView follower{2, at(-80, -15), leader.state, &profile};
        follower.state.roll = Fixed{};
        tactical::CraftFrame frame;
        frame.self = &follower;
        frame.leader = &leader;
        frame.leader_offset = at(30, 0);
        frame.own_offset = at(0, -15);
        frame.formation_tolerance = units(25);
        frame.moving = true;
        frame.hold = at(3000, 0);
        const auto stepped = tactical::step_craft(frame);
        expect(static_cast<bool>(stepped), "the follower steps");
        return stepped ? real(stepped.value().state.pitch) : 0.0;
    };
    expect(follower_pitch(0) == 0.0, "FM-14: beside a level leader the follower stays level");
    expect(follower_pitch(-40) < 0.0, "FM-14: its slot rises with a leader banked left, so it climbs (nose up)");
    expect(follower_pitch(40) > 0.0, "FM-14: its slot sinks with a leader banked right, so it dives");
}

// #615 (FD-10): a leader all but stopped beside a ship. Its look-ahead is a few raw units, so
// the Q24 square of it rounds to zero though the exact one is not; the share of the look-ahead
// comes from the exact products, as FoC's float division by the tiny square gives it (the huge
// ratio clamped), and the craft steps. Moving away, or toward the ship from outside its radius,
// the nearest point stays outside, so it does not steer away.
void test_stopped_craft_beside_a_ship() {
    const tactical::CraftProfile profile = x_wing();
    const std::array<tactical::CraftObstacle, 1> ship{tactical::CraftObstacle{9, at(0, 0), units(50)}};
    for (const std::int64_t sign : {std::int64_t{-1}, std::int64_t{1}}) {
        tactical::CraftView craft{1, at(40, 40), {}, &profile};
        // 20 raw units a frame: about 1300 raw of look-ahead, whose square is under half a Q24 unit.
        craft.state.velocity = {Fixed::from_raw(sign * 20), Fixed::from_raw(sign * 20), Fixed{}};
        tactical::CraftFrame frame;
        frame.self = &craft;
        frame.leader = &craft;
        frame.moving = true;
        frame.hold = at(40, 40);
        frame.obstacles = ship;
        const auto stepped = tactical::step_craft(frame);
        expect(static_cast<bool>(stepped), std::string("#615: a craft all but stopped beside a ship steps (")
            + (stepped ? std::string(sign < 0 ? "toward it" : "away") : stepped.error().message) + ")");
        if (stepped) {
            expect(stepped.value().state.relaxation == 0, "#615: its nearest point stays outside, so it does not steer away");
        }
    }
}

// #615 (AB-24, FM-11): a speed factor that rounds a follower's maximum speed to zero in Q24.
// FoC's float speed stays positive, so the budget ratio of a follower slower than its formation
// speed is huge, not a trap: the follower steps with its budgets scaled up.
void test_speed_factor_rounding_to_zero() {
    tactical::CraftProfile profile = x_wing();
    profile.max_speed = Fixed::from_raw(one / 4);
    tactical::CraftView leader{1, at(0, 0), {}, &profile};
    leader.state.velocity = {units(4), Fixed{}, Fixed{}};
    // Behind its slot and fast, so FM-12's share is small and the formation speed is near the leader's.
    tactical::CraftView follower{2, at(-200, -15), {}, &profile};
    follower.state.velocity = {units(40), Fixed{}, Fixed{}};
    tactical::CraftFrame frame;
    frame.self = &follower;
    frame.leader = &leader;
    frame.leader_offset = at(30, 0);
    frame.own_offset = at(0, -15);
    frame.formation_tolerance = units(25);
    frame.moving = true;
    frame.hold = at(3000, 0);
    frame.speed_factor = Fixed::from_raw(1);
    const auto stepped = tactical::step_craft(frame);
    expect(static_cast<bool>(stepped), std::string("#615: a follower whose scaled maximum speed rounds to zero steps")
        + (stepped ? std::string() : " (" + stepped.error().message + ")"));
}

// #615: a failed calculation names the craft, the rule step and the raw operands.
void test_failure_names_the_site() {
    const tactical::CraftProfile profile = x_wing();
    const math::Vec3 edge{Fixed::from_raw(std::numeric_limits<std::int64_t>::max() - 10), Fixed{}, Fixed{}};
    tactical::CraftView craft{7, edge, {}, &profile};
    craft.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
    tactical::CraftFrame frame;
    frame.self = &craft;
    frame.leader = &craft;
    frame.moving = true;
    frame.hold = edge;
    const auto stepped = tactical::step_craft(frame);
    const std::string message = stepped ? std::string() : stepped.error().message;
    expect(message.starts_with("craft 7: FM-01 position: ") && message.find("(raw 9223372036854775797 + raw ") != std::string::npos,
        "#615: the diagnostic names the step and its operands (" + message + ")");
}

} // namespace

int main() {
    test_nose_bank_and_turn(1);
    test_nose_bank_and_turn(2);
    test_bank_follows_the_turn();
    test_reversal_is_a_loop();
    test_formation_banks_with_leader();
    test_stopped_craft_beside_a_ship();
    test_speed_factor_rounding_to_zero();
    test_failure_names_the_site();
    if (failures != 0) {
        std::cerr << failures << " flight check(s) failed\n";
        return 1;
    }
    std::cout << "flight contracts passed\n";
    return 0;
}
