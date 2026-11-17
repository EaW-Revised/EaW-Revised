// #479 flight feel (docs/behaviour/space-fighters.md, cases C-18 to C-21): a squadron flown by the
// fighter locomotor as the session flies it (every craft reads the frame's copied views, the
// leader first) with FoC's X-wing values. No craft slides off its nose (FM-07), the bank leans
// into every turn and follows the turn's size (FM-03), a reversal is a loop through the vertical
// and a roll out, never a jump (FM-06), and the formation banks with its leader (FM-14).

#include "../support/check.hpp"
#include "eawr/sim/tactical/fighters.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
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
    [[nodiscard]] std::vector<tactical::CraftView> step(const math::Vec3& destination,
        const std::optional<tactical::LaneFlight>& lane = std::nullopt) {
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
            frame.lane = lane;
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
    tactical::CraftProfile profile = x_wing();
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
    profile.bank_angle = units(360);
    expect(bank_after(-1000, 1000, 7).roll == units(-42),
        "FM-03: a bank of 360 is neither wrapped to zero nor capped at the stock bank");
    profile.bank_angle = units(-40);
    expect(bank_after(-1000, 1000, 1).roll == units(6),
        "FM-03: a negative bank uses the upper limit when the turn passes the lower limit");
    expect(bank_after(-1000, -1000, 1).roll == units(-6),
        "FM-03: a negative bank tests the lower limit first for a right turn");
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

// FM-06/FM-14: a reversal splits the vertical direction of the leader and its trailing slots.
void test_reversal_directions() {
    auto squadron = x_wings();
    std::array<double, 5> low{};
    std::array<double, 5> high{};
    for (int frame = 0; frame < 100; ++frame) {
        static_cast<void>(squadron.step(at(-3000, 0)));
        for (std::size_t index = 0; index < squadron.craft.size(); ++index) {
            const double height = real(squadron.craft[index].position.z);
            low[index] = std::min(low[index], height);
            high[index] = std::max(high[index], height);
        }
    }
    expect(high[0] > 80.0, "FM-06: the level leader reverses upward");
    for (std::size_t index = 1; index < squadron.craft.size(); ++index) {
        expect(low[index] < -20.0, "FM-06/FM-14: follower " + std::to_string(index)
            + " reverses below the layer (low " + std::to_string(low[index]) + ")");
    }
}

// WSQ-10/WSQ-17: after a long level flight, a late reversal still steers one
// turn radius along the retained segment and eases every craft back to its layer.
void test_late_reversal_rejoins() {
    auto squadron = x_wings();
    const std::array<tactical::GroupSquadron, 1> stopped{tactical::GroupSquadron{}};
    const auto empty = tactical::squadron_group_slots(stopped, at(0, 0));
    expect(empty && !empty.value()[0].lane, "FO-02: a zero-length move stays arrived instead of acquiring an arbitrary segment");
    const auto make_lane = [&](const math::Vec3& destination) {
        tactical::GroupSquadron group;
        group.position = squadron.craft[0].position;
        group.max_speed = squadron.profile.max_speed;
        group.min_speed = squadron.profile.min_speed;
        const std::array<tactical::GroupSquadron, 1> groups{group};
        const auto mapped = tactical::squadron_group_slots(groups, destination);
        expect(mapped && mapped.value()[0].lane.has_value(), "WSQ-17: a single squadron retains a path segment");
        return mapped.value()[0].lane.value();
    };
    math::Vec3 destination = at(8000, 0);
    auto lane = make_lane(destination);
    double peak = 0.0;
    int regroup = -1;
    std::array<double, 5> late{};
    for (int tick = 0; tick < 1050; ++tick) {
        if (tick == 750) {
            destination = at(-4000, 0);
            lane = make_lane(destination);
        }
        const tactical::LaneFlight flight{lane.direction, Fixed{}, squadron.profile.max_speed};
        static_cast<void>(squadron.step(destination, flight));
        if (tick < 750) continue;
        peak = std::max(peak, real(squadron.craft[0].position.z));
        double height = 0.0;
        for (std::size_t index = 0; index < squadron.craft.size(); ++index) {
            height = std::max(height, std::abs(real(squadron.craft[index].position.z)));
            if (tick == 900) late[index] = real(squadron.craft[index].position.z);
        }
        if (tick > 800 && height < 1.0 && regroup < 0) regroup = tick;
    }
    expect(peak > 80.0, "WSQ-17: the late reversal still climbs through its half loop");
    // WSQ-17: a five-X-wing native trace reverses at frame 751, peaks at
    // 109.801392, and first samples every craft below one unit at frame 906.
    // Its frame-901 heights are reordered by the authored slots, leader first.
    constexpr std::array<double, 5> native_late{0.158724, -0.469523, -0.663341, -1.253026, -0.843593};
    expect(std::abs(peak - 109.801392) < 0.01, "WSQ-17: the leader's reversal peak matches the native trace");
    // Formation slack is 25 / 4.8 = 5.21 frames; round up and allow the native
    // trace's two-frame sampling interval. Compare elapsed time from reversal.
    expect(std::abs((regroup - 750) - (906 - 751)) <= 8,
        "WSQ-17: regroup time agrees with the native trace within formation slack and sampling");
    for (std::size_t index = 0; index < late.size(); ++index) {
        expect(std::abs(late[index] - native_late[index]) < 1.0,
            "WSQ-17: leader and follower heights agree with the native late sample within one unit");
    }
    expect(regroup >= 801 && regroup <= 900, "WSQ-17: all craft rejoin the layer by tick 900 ("
        + std::to_string(regroup) + ")");
    for (const double height : late) expect(std::abs(height) < 1.0, "WSQ-17: every craft is back at its layer at tick 900");
    std::cout << "late reversal: peak " << peak << ", regroup " << regroup << ", tick900 heights";
    for (const double height : late) std::cout << ' ' << height;
    std::cout << '\n';

    // A leader already above the layer must use the retained direction rather than
    // the distant destination's bearing. Changing only goal distance cannot change its step.
    tactical::CraftView leader{1, at(0, 0, 100), {}, &squadron.profile};
    leader.state.yaw = units(180);
    leader.state.pitch = units(30);
    leader.state.velocity = at(-4, 0, -2);
    tactical::CraftFrame frame;
    frame.self = &leader;
    frame.leader = &leader;
    frame.moving = true;
    frame.lane = tactical::LaneFlight{at(-1, 0), Fixed{}, squadron.profile.max_speed};
    frame.hold = at(-2000, 0);
    const auto near = tactical::step_craft(frame);
    frame.hold = at(-8000, 0);
    const auto far = tactical::step_craft(frame);
    expect(near && far && near.value().position == far.value().position && near.value().state == far.value().state,
        "WSQ-17: the leader's height approach does not flatten toward a farther endpoint");
    leader.position = at(0, 0);
    leader.state = {};
    leader.state.velocity = {squadron.profile.max_speed, Fixed{}, Fixed{}};
    frame.hold = at(8000, 0);
    frame.speed_factor = units(2);
    const tactical::LaneMember member{leader.position,
        tactical::SquadronLane{1, at(0, 0), at(1, 0), Fixed{}, Fixed{}},
        squadron.profile.max_speed, squadron.profile.min_speed};
    const std::array<tactical::LaneMember, 1> members{member};
    const auto flights = tactical::formation_lane_flight(members, Fixed{}, Fixed{});
    expect(flights && flights.value()[0].individual_speed, "AB-24: a single-squadron lane uses current craft speed");
    if (flights) frame.lane = flights.value()[0];
    const auto boosted = tactical::step_craft(frame);
    expect(boosted && boosted.value().state.velocity.x > squadron.profile.max_speed,
        "AB-24: a single-squadron path still accelerates under TURBO");
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

// Fixed synthetic heavy-melee slice: 240 craft inside 44 overlapping ship broad spheres.
// Each hull has 32 separated surface triangles; the look-ahead crosses their bounds but misses
// their surfaces. This deliberately exercises repeated narrow traversal rather than early hits.
void test_avoidance_work_budget() {
    auto profile = x_wing();
    std::vector<tactical::CollisionTriangle> triangles;
    for (std::int64_t face = 0; face < 32; ++face)
        triangles.push_back({at(face * 2, -100, -100), at(face * 2, 100, -100), at(face * 2, -100, 100)});
    tactical::CombatProfile hull;
    hull.meshes = {tactical::collision_mesh(std::move(triangles), tactical::no_hardpoint, tactical::no_hardpoint, false)};
    std::vector<tactical::CraftObstacle> obstacles;
    for (std::uint32_t ship = 0; ship < 44; ++ship)
        obstacles.push_back({1000 + ship, {}, units(150), &hull, math::identity_matrix(), nullptr, nullptr});
    std::vector<std::pair<std::uint64_t, std::uint64_t>> reference;
    std::vector<tactical::CraftView> reference_craft;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        std::vector<tactical::CraftView> craft;
        for (std::uint32_t member = 0; member < 240; ++member) {
            tactical::CraftState initial;
            initial.velocity = {profile.max_speed, Fixed{}, Fixed{}};
            craft.push_back({member + 1, at(-50 - static_cast<std::int64_t>(member % 3), 50, 50), initial, &profile});
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> counts;
        for (std::uint32_t tick = 0; tick < 16; ++tick) {
            const auto before = craft;
            std::vector<std::optional<tactical::CraftStep>> staged(craft.size());
            std::vector<tactical::CraftAvoidanceWork> work(workers);
            {
                std::vector<std::jthread> threads;
                for (std::size_t worker = 0; worker < workers; ++worker) threads.emplace_back([&, worker] {
                    for (auto index = worker; index < craft.size(); index += workers) {
                        tactical::CraftFrame frame;
                        frame.self = frame.leader = &before[index];
                        frame.moving = true; frame.hold = at(1000, 50, 50);
                        frame.obstacles = obstacles; frame.avoidance_work = &work[worker];
                        const auto result = tactical::step_craft(frame);
                        if (result) staged[index] = result.value();
                    }
                });
            }
            tactical::CraftAvoidanceWork total;
            for (const auto& item : work) { total.queries += item.queries; total.triangles += item.triangles; }
            counts.push_back({total.queries, total.triangles});
            std::cout << "FD-10 budget " << workers << " workers tick " << tick << ": "
                      << total.queries << " queries, " << total.triangles << " triangles\n";
            // The fixed slice peaks at 10,560 queries and 337,920 triangle tests per tick
            // (240 craft x 44 hulls x 32 triangles). Budget that baseline plus 5% headroom;
            // wall time is deliberately absent, so slower hosts enforce the same limit.
            expect(total.queries <= 11088 && total.triangles <= 354816,
                "FD-10: heavy-melee narrow work stays within its per-tick budget");
            for (std::size_t index = 0; index < craft.size(); ++index) {
                expect(staged[index].has_value(), "FD-10: heavy-melee slice steps");
                if (!staged[index]) continue;
                craft[index].position = staged[index]->position;
                craft[index].state = staged[index]->state;
            }
        }
        expect(std::any_of(counts.begin(), counts.end(), [](const auto& count) { return count.first > 0 && count.second > 0; }),
            "FD-10: the budget exercises actual narrow queries and triangle tests");
        if (workers == 1) { reference = counts; reference_craft = craft; }
        else {
            expect(counts == reference, "FD-10: per-tick query and triangle counts match on 1/2/4/8 workers");
            for (std::size_t index = 0; index < craft.size(); ++index)
                expect(craft[index].position == reference_craft[index].position && craft[index].state == reference_craft[index].state,
                    "FD-10: counted heavy-melee flight matches on 1/2/4/8 workers");
        }
    }
}

// FD-10/WSQ-35: stock launch coordinates, with an invented hull surface just ahead.
// The broad sphere contains the launch, but the model surface does not. Read copied inputs
// and commit disjoint outputs exactly as the session does; schedules must not separate members.
void test_model_launch_avoidance() {
    const auto fixed = [](const double value) { return Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * one))); };
    tactical::CraftProfile profile = x_wing();
    profile.max_speed = tenths(54);
    const math::Vec3 origin = at(-500, -1500, -110);
    tactical::CombatProfile hull;
    const Fixed face = fixed(16.5);
    hull.meshes = {tactical::collision_mesh({
        {{face, units(-100), units(-100)}, {face, units(100), units(-100)}, {face, units(100), units(100)}},
        {{face, units(-100), units(-100)}, {face, units(100), units(100)}, {face, units(-100), units(100)}}},
        tactical::no_hardpoint, tactical::no_hardpoint, false)};
    auto placement = math::identity_matrix();
    placement.rows[0][3] = origin.x; placement.rows[1][3] = origin.y; placement.rows[2][3] = origin.z;
    const std::array<tactical::CraftObstacle, 1> obstacles{{{9, origin, fixed(148.382), &hull, placement, nullptr, nullptr}}};
    std::vector<tactical::CraftView> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        std::vector<tactical::CraftView> craft;
        tactical::CraftState initial;
        initial.pitch = fixed(44.3701);
        initial.velocity = {math::multiply(profile.max_speed, math::cos_turn(math::divide(initial.pitch, units(360)).value())).value(),
            Fixed{}, Fixed::from_raw(-math::multiply(profile.max_speed,
                math::sin_turn(math::divide(initial.pitch, units(360)).value())).value().raw())};
        for (std::uint32_t member = 0; member < 7; ++member)
            craft.push_back({member + 1, {fixed(-484.5673), units(-1500), fixed(-118.7671)}, initial, &profile});
        const auto hold = craft[0].position;
        for (std::uint32_t age = 1; age <= 16; ++age) {
            const auto before = craft;
            std::array<std::optional<tactical::CraftStep>, 7> staged;
            {
                std::vector<std::jthread> threads;
                for (std::size_t worker = 0; worker < workers; ++worker) threads.emplace_back([&, worker] {
                    for (auto index = worker; index < craft.size(); index += workers) {
                        tactical::CraftFrame frame;
                        frame.self = &before[index]; frame.leader = &before[0];
                        frame.hold = hold; frame.obstacles = obstacles;
                        const auto result = tactical::step_craft(frame);
                        if (result) staged[index] = result.value();
                    }
                });
            }
            for (std::size_t index = 0; index < craft.size(); ++index) {
                expect(staged[index].has_value(), "FD-10: parallel launch step succeeds");
                if (!staged[index]) continue;
                craft[index].position = staged[index]->position; craft[index].state = staged[index]->state;
                expect(craft[index].position == craft[0].position && craft[index].state == craft[0].state,
                    "FD-10: all seven launch members remain coincident through relaxation");
                expect(craft[index].state.relaxation == 16 - age, "FD-10: one avoidance turn then 15 decrements");
                expect(craft[index].state.yaw.raw() == 0 && std::abs(real(craft[index].state.pitch) - 39.5701) < 0.001,
                    "FD-10: the first away pitch and yaw stay fixed throughout relaxation");
            }
            if (age == 1) expect(std::abs(real(craft[0].position.x) + 481.1755) < 0.001
                && real(craft[0].position.y) == -1500 && std::abs(real(craft[0].position.z) + 121.5700) < 0.001,
                "FD-10: equal-age first launch step matches the observed position");
        }
        if (workers == 1) reference = craft;
        else for (std::size_t index = 0; index < craft.size(); ++index)
            expect(craft[index].position == reference[index].position && craft[index].state == reference[index].state,
                "FD-10: 1/2/4/8 schedules produce identical launch flight");
    }
    const tactical::ModelCollisionFilter enabled{};
    // The explicit box takes precedence over a surface and reports containment on its boundary.
    const tactical::CollisionBox box{at(-20, -20, -20), at(20, 20, 20)};
    for (const auto start : {at(0, 0), at(20, 0)}) {
        const auto contact = tactical::segment_hits_model(&box, hull.meshes, enabled, math::identity_matrix(), start, at(100, 0));
        expect(contact && contact.value() && contact.value()->starts_inside,
            "FD-10: a genuine box-interior or boundary start is classified inside");
        hull.model_collision_box = box;
        tactical::CraftView craft{1, start, {}, &profile};
        craft.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
        const std::array<tactical::CraftObstacle, 1> inside{{{9, {}, units(150), &hull, math::identity_matrix(), nullptr, nullptr}}};
        tactical::CraftFrame frame;
        frame.self = frame.leader = &craft; frame.hold = at(100, 0); frame.obstacles = inside;
        const auto step = tactical::step_craft(frame);
        expect(step && step.value().state.relaxation == 0, "FD-10: a genuine shape-inside start skips avoidance");
    }
    const auto exit = tactical::segment_hits_model(nullptr, hull.meshes, enabled, math::identity_matrix(), at(0, 0), at(100, 0));
    expect(exit && exit.value() && !exit.value()->starts_inside,
        "FD-10: mesh surface exits do not infer volume containment");
    const auto endpoint = tactical::segment_hits_model(nullptr, hull.meshes, enabled, math::identity_matrix(), at(0, 0), {face, Fixed{}, Fixed{}});
    expect(endpoint && !endpoint.value(), "FD-10: a mesh contact at the segment endpoint is excluded");
    tactical::MeshCollisionWork work;
    const auto long_look = tactical::segment_hits_model(nullptr, hull.meshes, enabled, math::identity_matrix(),
        at(-50000, 0), at(50000, 0), &work);
    expect(long_look && long_look.value() && !long_look.value()->starts_inside,
        "FD-10: long mod-driven look-aheads are clipped before bounded mesh arithmetic");
    expect(work.meshes == 1 && work.boxes == 1 && work.triangles <= 2,
        "FD-10: the model query traverses one loaded root and at most its two surface triangles");
    const std::array<tactical::CollisionMesh, 1> large{{tactical::collision_mesh({
        {at(40000, -1000, -1000), at(40000, 1000, -1000), at(40000, 0, 1000)}},
        tactical::no_hardpoint, tactical::no_hardpoint, false)}};
    const auto adaptive = tactical::segment_hits_model(nullptr, large, enabled, math::identity_matrix(),
        at(-200000, 0), at(200000, 0));
    expect(large[0].grid_exponent > 0 && adaptive && adaptive.value(),
        "FD-10: oversized authored meshes retain long-look collision on their admitted grid");
    tactical::CraftState rotated;
    rotated.yaw = units(90);
    const auto rotated_frame = math::to_matrix(tactical::craft_rotation(rotated).value(), at(100, 200, 300)).value();
    const auto transformed_inside = tactical::segment_hits_model(&box, {}, enabled, rotated_frame,
        math::transform_point(rotated_frame, at(10, 0)).value(), math::transform_point(rotated_frame, at(100, 0)).value());
    expect(transformed_inside && transformed_inside.value() && transformed_inside.value()->starts_inside,
        "FD-10: explicit box containment follows its translated and rotated model frame");
    hull.model_collision_box.reset();
    tactical::DurabilityProfile health_profile;
    tactical::DurabilityState health;
    const auto eligible_step = [&](const bool visible, const std::uint32_t source, const bool shield) {
        hull.meshes[0].pose_visible = visible; hull.meshes[0].source_hardpoint = source; hull.meshes[0].shield = shield;
        tactical::CraftView craft{1, at(10, 0), {}, &profile};
        craft.state.velocity = {profile.max_speed, Fixed{}, Fixed{}};
        const std::array<tactical::CraftObstacle, 1> obstacle{{{9, {}, units(150), &hull,
            math::identity_matrix(), &health_profile, &health}}};
        tactical::CraftFrame frame;
        frame.self = frame.leader = &craft; frame.hold = at(100, 0); frame.obstacles = obstacle;
        return tactical::step_craft(frame);
    };
    const auto hidden = eligible_step(false, tactical::no_hardpoint, false);
    expect(hidden && hidden.value().state.relaxation == 0, "FD-10: hidden model surfaces are not avoidance hulls");
    health.shields = units(600);
    const auto shield = eligible_step(true, tactical::no_hardpoint, true);
    expect(shield && shield.value().state.relaxation == 0,
        "FD-10: the shield-only collision mask is excluded even while shields are up");
    health_profile.hardpoints.resize(1); health_profile.hardpoints[0].max_health = units(100);
    health.hardpoints = {Fixed{}};
    const auto destroyed = eligible_step(true, 0, false);
    health.hardpoints[0] = units(1);
    const auto standing = eligible_step(true, 0, false);
    expect(destroyed && standing && destroyed.value().state.relaxation == 0 && standing.value().state.relaxation == 15,
        "FD-10: destroyed attached model surfaces are excluded without rebuilding geometry");
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
    expect(message.starts_with("craft 7: FM-11 form up: ") && message.find("(raw 9223372036854775797 + raw ") != std::string::npos,
        "#615: the diagnostic names the step and its operands (" + message + ")");
}

} // namespace

int main() {
    test_nose_bank_and_turn(1);
    test_nose_bank_and_turn(2);
    test_bank_follows_the_turn();
    test_reversal_is_a_loop();
    test_reversal_directions();
    test_late_reversal_rejoins();
    test_formation_banks_with_leader();
    test_stopped_craft_beside_a_ship();
    test_model_launch_avoidance();
    test_avoidance_work_budget();
    test_speed_factor_rounding_to_zero();
    test_failure_names_the_site();
    if (failures != 0) {
        std::cerr << failures << " flight check(s) failed\n";
        return 1;
    }
    std::cout << "flight contracts passed\n";
    return 0;
}
