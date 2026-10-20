// #391: breakoff props (presentation::space debris rule, docs/behaviour/battle-presentation.md
// BP-30 to BP-36). Values are the FoC Nebulon-B's front-left breakoff prop.

#include "../../support/check.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/space/shield_hits.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace {

namespace space = eawr::presentation::space;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string& message) {
    if (!condition) local_checker.check_before_print(condition, message, std::cerr, "FAIL: ");
}

[[nodiscard]] bool near(const double a, const double b) { return std::abs(a - b) <= 1.0e-9; }

[[nodiscard]] space::DebrisMotion nebulon_front_left() {
    return {{0.2, 0.0, -0.5}, {0.5, 0.1, 0.9}, 15, 25};
}

void test_parsing() {
    const auto vector = space::debris_vector(" 0.2, 0.0, -0.5 ");
    expect(vector && near((*vector)[0], 0.2) && near((*vector)[1], 0.0) && near((*vector)[2], -0.5),
           "a vector reads its three numbers");
    expect(!space::debris_vector("0.2, 0.0"), "two numbers are no vector");
    expect(!space::debris_vector("0.2, 0.0, -0.5, 1"), "four numbers are no vector");
    expect(!space::debris_vector("0.2, x, -0.5"), "a word is no number");
    expect(space::debris_seconds(" 15.0 ") == 15, "15.0 seconds are 15");
    expect(space::debris_seconds("24.9") == 24, "a fraction of a second is cut, not rounded");
    expect(space::debris_seconds("-0.5") == 0, "cut towards zero");
    expect(!space::debris_seconds("soon"), "a word is no lifetime");
}

void test_lifetime() {
    const space::DebrisMotion motion = nebulon_front_left();
    std::set<std::uint64_t> seen;
    bool in_range = true;
    for (std::uint64_t tick = 1; tick <= 400; ++tick) {
        const auto frames = space::debris_lifetime_frames(motion, 7, 0, tick, 30);
        in_range = in_range && frames && *frames % 30 == 0 && *frames >= 15 * 30 && *frames <= 25 * 30;
        if (frames) seen.insert(*frames / 30);
    }
    expect(in_range, "every draw is whole seconds within [15, 25] at 30 frames per second");
    expect(seen.size() == 11, "both ends and everything between are drawn");
    expect(space::debris_lifetime_frames(motion, 7, 2, 100, 30) == space::debris_lifetime_frames(motion, 7, 2, 100, 30),
           "the same event draws the same lifetime");
    space::DebrisMotion fixed = motion;
    fixed.min_lifetime_seconds = fixed.max_lifetime_seconds = 20;
    expect(space::debris_lifetime_frames(fixed, 3, 1, 9, 30) == 600U, "equal ends give that lifetime");
    space::DebrisMotion reversed = motion;
    reversed.min_lifetime_seconds = 25;
    reversed.max_lifetime_seconds = 15;
    const auto swapped = space::debris_lifetime_frames(reversed, 3, 1, 9, 30);
    expect(swapped && *swapped >= 450U && *swapped <= 750U, "reversed ends swap");
    space::DebrisMotion endless = motion;
    endless.max_lifetime_seconds = 0;
    expect(!space::debris_lifetime_frames(endless, 3, 1, 9, 30), "a maximum of zero never expires");
    space::DebrisMotion short_draw = motion;
    short_draw.min_lifetime_seconds = short_draw.max_lifetime_seconds = 0;
    expect(!space::debris_lifetime_frames(short_draw, 3, 1, 9, 30), "a draw below one second never expires");
}

void test_motion() {
    const space::DebrisMotion motion = nebulon_front_left();
    // A ship at (100, 200, 10) facing +Y (yaw 90), level: its unit-frame +X is world +Y, its +Y
    // is world -X.
    const auto spawn = space::debris_spawn({100.0, 200.0, 10.0}, 90.0, 0.0, {30.0, 5.0, 2.0});
    expect(std::abs(spawn.position[0] - 95.0) < 1.0e-9 && std::abs(spawn.position[1] - 230.0) < 1.0e-9
               && near(spawn.position[2], 12.0),
           "the attachment point is placed by the ship's facing");
    expect(near(spawn.facing_degrees[0], 0.0) && near(spawn.facing_degrees[1], 0.0)
               && near(spawn.facing_degrees[2], 90.0),
           "the prop starts with its ship's facing");
    for (const double yaw : {0.0, 37.0, -120.0}) {
        for (const double roll : {0.0, -12.5, 45.0}) {
            const std::array<double, 3> position{100.0, -20.0, 7.0}, point{5.0, 12.0, 3.0};
            const double y = yaw * 3.14159265358979323846 / 180.0;
            const double r = roll * 3.14159265358979323846 / 180.0;
            const double cy = std::cos(y), sy = std::sin(y), cr = std::cos(r), sr = std::sin(r);
            const double side = cr * point[1] - sr * point[2];
            const std::array<double, 3> old{position[0] + cy * point[0] - sy * side,
                position[1] + sy * point[0] + cy * side, position[2] + sr * point[1] + cr * point[2]};
            const auto level = space::debris_spawn(position, yaw, roll, point);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                expect(std::bit_cast<std::uint64_t>(level.position[axis]) == std::bit_cast<std::uint64_t>(old[axis]),
                       "non-pitched debris position is bit-identical");
            }
            for (const double pitch : {-90.0, -40.0, 35.0, 90.0, 110.0}) {
                const auto pitched = space::debris_spawn(position, yaw, roll, point, pitch);
                const auto axes = space::live_unit_axes(yaw, roll, pitch);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const double expected = position[axis] + axes.x[axis] * point[0]
                        + axes.y[axis] * point[1] + axes.z[axis] * point[2];
                    expect(near(pitched.position[axis], expected), "FM-02/BP-30: pitched breakoff and explosion agree");
                }
                expect(near(pitched.facing_degrees[1], pitch < 0.0 ? pitch + 360.0 : pitch),
                       "FM-02: breakoff retains the ship pitch");
            }
        }
    }
    const auto banked = space::debris_spawn({0.0, 0.0, 0.0}, 0.0, -90.0, {0.0, 10.0, 0.0});
    expect(std::abs(banked.position[2] + 10.0) < 1.0e-9 && std::abs(banked.position[1]) < 1.0e-9,
           "a -90 bank lowers the unit frame's left side");
    expect(near(banked.facing_degrees[0], 270.0), "a negative roll wraps into [0, 360)");

    const auto start = space::debris_pose(spawn, motion, 0.0);
    expect(start.position == spawn.position && start.facing_degrees == spawn.facing_degrees, "frame 0 is the spawn");
    const auto later = space::debris_pose(spawn, motion, 30.0);
    expect(std::abs(later.position[0] - 101.0) < 1.0e-9 && std::abs(later.position[1] - 230.0) < 1.0e-9
               && std::abs(later.position[2] + 3.0) < 1.0e-9,
           "30 frames move 30 times the vector in world axes, whatever the facing");
    expect(std::abs(later.facing_degrees[0] - 15.0) < 1.0e-9 && std::abs(later.facing_degrees[1] - 3.0) < 1.0e-9
               && std::abs(later.facing_degrees[2] - 117.0) < 1.0e-9,
           "the facing triple grows by the rotate vector per frame");
    const auto half = space::debris_pose(spawn, motion, 0.5);
    expect(std::abs(half.position[2] - 11.75) < 1.0e-9, "between frames the prop is between their poses");
    const auto long_life = space::debris_pose(spawn, motion, 750.0);
    expect(long_life.facing_degrees[2] >= 0.0 && long_life.facing_degrees[2] < 360.0
               && std::abs(long_life.facing_degrees[2] - std::fmod(90.0 + 0.9 * 750.0, 360.0)) < 1.0e-6,
           "the facing stays wrapped over a whole lifetime");
}

// A yaw-only transform at (x, y, 0).
[[nodiscard]] eawr::sim::math::Mat3x4 placed(const double x, const double y, const double yaw_degrees) {
    const auto fixed = [](const double value) {
        return eawr::sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * 16777216.0)));
    };
    const double radians = yaw_degrees * 3.14159265358979323846 / 180.0;
    eawr::sim::math::Mat3x4 matrix{};
    matrix.rows[0] = {fixed(std::cos(radians)), fixed(-std::sin(radians)), fixed(0.0), fixed(x)};
    matrix.rows[1] = {fixed(std::sin(radians)), fixed(std::cos(radians)), fixed(0.0), fixed(y)};
    matrix.rows[2] = {fixed(0.0), fixed(0.0), fixed(1.0), fixed(0.0)};
    return matrix;
}

void test_ship_at_the_event_tick() {
    // #401 review 1. Players 1 (team 1) and 2 (team 2); visible_to bit 0 is player 1.
    const eawr::sim::tactical::TacticalSnapshot snapshot{20, {{1, 1}, {2, 2}}, {
        {6, 100, 1, 1, placed(100.0, 200.0, 90.0), 0b01, {}, {}},
        {9, 100, 2, 2, placed(-50.0, 0.0, 0.0), 0b10, {}, {}},
    }, {}};
    const auto seen = space::debris_ship_at(&snapshot, 6, 1);
    expect(seen && seen->entity == 6 && std::abs(seen->position[0] - 100.0) < 1.0e-3
               && std::abs(seen->yaw_degrees - 90.0) < 1.0e-3,
           "a ship the player saw at the event's tick throws its prop from that tick's pose");
    expect(!space::debris_ship_at(&snapshot, 9, 1), "a ship hidden at the event's tick throws none");
    expect(space::debris_ship_at(&snapshot, 9, 2).has_value(), "its own player sees it");
    expect(!space::debris_ship_at(nullptr, 6, 1),
           "without the tick's snapshot what the player saw is unknown and nothing is thrown");
}

void test_self_destruct_at_the_kill() {
    // WCC-71 (BP-37): a ship with five hardpoints dies at tick 21. Hardpoint 1 is no destroyable
    // one (maximum health 0); hardpoint 2 was destroyed before; hardpoint 3 stood at tick 20 and
    // a hit destroyed it in the kill tick; 0 and 4 stood until the kill.
    namespace t = eawr::sim::tactical;
    const auto health = [](const double value) {
        return eawr::sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(value * 16777216.0)));
    };
    const auto status = [&](const double now, const double maximum) {
        t::HardpointStatus row;
        row.health = health(now);
        row.max_health = health(maximum);
        row.state = now <= 0.0 && maximum > 0.0 ? t::HardpointState::destroyed : t::HardpointState::intact;
        return row;
    };
    t::InstanceDurability durability;
    durability.hardpoints = {status(300.0, 300.0), status(0.0, 0.0), status(0.0, 300.0), status(40.0, 300.0),
                             status(1.0, 300.0)};
    t::TacticalInstance ship{6, 100, 1, 1, placed(100.0, 200.0, 90.0), 0b01, {}, durability};
    t::TacticalInstance plain{9, 100, 2, 2, placed(-50.0, 0.0, 0.0), 0b10, {}, {}};
    const t::TacticalSnapshot before{20, {{1, 1}, {2, 2}}, {ship, plain}, {}};
    const std::vector<t::Event> kill_tick{
        {21, t::EventKind::hardpoint_destroyed, 2, 0, 9, t::OrderKind::none, t::RejectReason::none, 3},
        {21, t::EventKind::hardpoint_destroyed, 1, 0, 6, t::OrderKind::none, t::RejectReason::none, 3},
        {21, t::EventKind::unit_destroyed, 1, 0, 6, t::OrderKind::none, t::RejectReason::none, 0},
    };
    const auto hardpoints = space::self_destruct_hardpoints(&before, 6, kill_tick);
    const std::vector<space::SelfDestructHardpoint> expected{{0, true}, {2, false}, {3, false}, {4, true}};
    expect(hardpoints == expected,
           "every destroyable hardpoint self-destructs in HardPoints order; only those standing at the kill "
           "are destroyed again, the rest replay their explosion");
    expect(space::self_destruct_hardpoints(&before, 6, {})
               == std::vector<space::SelfDestructHardpoint>{{0, true}, {2, false}, {3, true}, {4, true}},
           "without a hit in the kill tick, hardpoint 3 still stood");
    expect(space::self_destruct_hardpoints(&before, 9, kill_tick).empty(), "a unit without durability has none");
    expect(space::self_destruct_hardpoints(&before, 7, kill_tick).empty(),
           "a unit the tick before does not list: what stood is unknown, nothing is shown");
    expect(space::self_destruct_hardpoints(nullptr, 6, kill_tick).empty(),
           "without the snapshot before the kill nothing is shown");
}

void test_effect_clock() {
    // #401 review 2: the first frame at presented tick 200 reaches an event of tick 15.
    expect(near(space::debris_clock_start(200.0, std::nullopt), 200.0), "without events the clock starts at the frame");
    expect(near(space::debris_clock_start(200.0, 15), 14.0), "the oldest reached event's birth starts the clock");
    expect(near(space::debris_clock_start(10.0, 15), 10.0), "never after the frame");
    // A 30-frame explosion born at sample 450, first reached at sample 931.
    expect(space::debris_effect_ended(450, 30, 931), "an explosion that ended before its frame is not shown");
    expect(space::debris_effect_ended(450, 30, 480), "it ends after its lifetime");
    expect(!space::debris_effect_ended(450, 30, 479), "it shows through its last sample");
    expect(!space::debris_effect_ended(450, 30, 450), "a new one shows");
    expect(!space::debris_effect_ended(460, 30, 450), "one born later shows");
    expect(!space::debris_effect_ended(0, 0, 931), "no authored lifetime never counts as ended");
}

void test_flights_in_tick_order() {
    // #401 review 3: a station hardpoint destroyed at tick 20 (450 frames, dead at presented
    // tick 469), repaired, and destroyed again at tick 800, both reached by one frame.
    space::DebrisFlights flights;
    std::vector<space::DebrisFlights::Ended> ended;
    const auto first = flights.launch({0, 20, {}, 450U}, ended);
    expect(first && ended.empty(), "the first destruction throws its prop");
    expect(!flights.launch({0, 300, {}, 450U}, ended) && ended.empty(),
           "destroyed again while the first prop flies: none");
    const auto other = flights.launch({1, 400, {}, std::nullopt}, ended);
    expect(other.has_value(), "another hardpoint's prop flies alongside");
    const auto second = flights.launch({0, 800, {}, 600U}, ended);
    expect(second && *second != *first, "destroyed again after the first prop died: thrown again");
    expect(ended.size() == 1 && ended[0].serial == *first && near(ended[0].death_tick, 469.0),
           "the first prop ends at its death, before the second is born");
    ended.clear();
    flights.expire(1398.0, ended);
    expect(ended.empty() && flights.flights().size() == 2, "one frame before its death the second prop flies");
    flights.expire(1399.0, ended);
    expect(ended.size() == 1 && ended[0].serial == *second && flights.flights().size() == 1,
           "it ends at tick 799 + 600; the prop without a lifetime never ends");
    const auto death = space::debris_death_tick({0, 20, {}, 450U});
    expect(death && near(*death, 469.0), "a flight dies at its birth plus its lifetime");
    expect(!space::debris_death_tick({0, 20, {}, std::nullopt}), "one without a lifetime never dies");
}

} // namespace

int main() {
    test_parsing();
    test_lifetime();
    test_motion();
    test_ship_at_the_event_tick();
    test_self_destruct_at_the_kill();
    test_effect_clock();
    test_flights_in_tick_order();
    if (failures != 0) {
        std::cerr << failures << " debris check(s) failed\n";
        return 1;
    }
    std::cout << "debris presentation contracts passed\n";
    return 0;
}
