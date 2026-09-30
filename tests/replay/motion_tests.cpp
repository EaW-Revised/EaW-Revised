#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/math.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-07 (#70): single-ship moves, turns and stops (docs/behaviour/space-movement.md). The rule
// cases use the FoC corvette and Acclamator motion profiles (units::motion_table on the FoC
// data). The `tactical-motion` fixture pins a single-move session and `tactical-motion-blocked`
// a blocked-path session: every worker count and storage order reproduces their golden hashes,
// and a live session records the fixture replay. `--regenerate` rewrites the goldens. #351 adds
// banking in turns (BK-01 to BK-05); its FoC roll values re-pinned the `tactical-motion` hashes.
namespace {

namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using math::Vec3;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId corvette_type = 1011;
constexpr tactical::TypeId acclamator_type = 1012;
constexpr tactical::TypeId station_type = 1013; // no motion profile

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] Vec3 at(const std::int64_t x, const std::int64_t y, const std::int64_t z) { return {units(x), units(y), units(z)}; }
[[nodiscard]] bool near(const Fixed value, const Fixed expected, const std::int64_t raw) {
    const auto difference = value.raw() - expected.raw();
    return difference >= -raw && difference <= raw;
}

// FoC values x Object_Max_Speed_Multiplier_Space 1.2 (units::motion_table on the FoC data).
// Roll rate 0.2 x 1.2 and Bank_Turn_Angle (#351, BK-01).
[[nodiscard]] tactical::MotionProfile corvette() {
    return {corvette_type, decimal("3.72"), decimal("0.06"), decimal("0.06"), decimal("1.5"), units(2), decimal("0.24"),
        units(15)};
}
[[nodiscard]] tactical::MotionProfile acclamator() {
    return {acclamator_type, decimal("2.64"), decimal("0.048"), decimal("0.048"), decimal("0.6"), units(3),
        decimal("0.24"), units(20)};
}
// The Nebulon-B frigate: rate of turn 0.7 x 1.2, frigate slowdown, bank 5.
[[nodiscard]] tactical::MotionProfile nebulon() {
    return {acclamator_type, decimal("2.64"), decimal("0.048"), decimal("0.048"), decimal("0.84"), units(3),
        decimal("0.24"), units(5)};
}
[[nodiscard]] tactical::MotionTable foc_table() {
    return {{units(15), units(300)}, {corvette(), acclamator()}, std::nullopt, {}, {}};
}

void test_validation() {
    expect(static_cast<bool>(tactical::validate_motion(foc_table())), "the FoC-shaped table is valid");
    auto unsorted = foc_table();
    std::swap(unsorted.profiles[0], unsorted.profiles[1]);
    expect(!tactical::validate_motion(unsorted), "type IDs must increase");
    auto still = foc_table();
    still.profiles[0].max_speed = Fixed{};
    expect(!tactical::validate_motion(still), "a zero speed is rejected");
    auto fast = foc_table();
    fast.profiles[0].turn_in_place_slowdown = decimal("0.5");
    expect(!tactical::validate_motion(fast), "a slowdown below 1 is rejected");
    auto flat = foc_table();
    flat.rules.arc_degrees = Fixed{};
    expect(!tactical::validate_motion(flat), "a zero arc angle is rejected");
    auto wide = foc_table();
    wide.rules.arc_degrees = units(181);
    expect(!tactical::validate_motion(wide), "an arc above 180 degrees is rejected");
    auto backwards = foc_table();
    backwards.profiles[0].roll_rate = decimal("-0.1");
    expect(!tactical::validate_motion(backwards), "a negative roll rate is rejected");
    auto steep = foc_table();
    steep.profiles[0].bank_angle = units(91);
    expect(!tactical::validate_motion(steep), "a bank above 90 degrees is rejected");
    auto level = foc_table();
    level.profiles[0].roll_rate = Fixed{};
    level.profiles[0].bank_angle = Fixed{};
    expect(static_cast<bool>(tactical::validate_motion(level)), "a type that never banks is valid");
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
    expect(!tactical::TacticalSession::create(setup, {}, {}, unsorted), "a session rejects an invalid motion table");
}

void test_rules() {
    const auto rules = foc_table().rules;
    // MV-13, MV-16, MV-18: from rest, dead ahead (the S-10 shape).
    const auto straight = tactical::plan_move(corvette(), rules, 100, at(-1800, -1500, -20), Fixed{}, Fixed{}, at(0, -1500, 0));
    expect(straight && straight.value().kind == tactical::MotionKind::path && straight.value().nodes.size() == 4,
           "a straight move: start, full speed, braking point, end");
    if (straight) {
        const auto& nodes = straight.value().nodes;
        // Q24 rounds 3.72 / 0.06 and 3.72^2 / 0.12 once each: a few hundred raw at most.
        expect(near(nodes[1].frame, units(162), 64) && near(nodes[1].position.x, decimal("-1684.68"), 64) &&
                   nodes[1].speed == decimal("3.72"),
               "full speed after 62 frames and v^2/2a = 115.32 units");
        expect(near(nodes[2].position.x, decimal("-115.32"), 1024), "braking starts v^2/2d before the end");
        const auto first = tactical::sample_motion(straight.value(), 101, at(-1800, -1500, -20), Fixed{});
        expect(first && near(first.value().position.x, decimal("-1799.97"), 128) &&
                   first.value().position.y == units(-1500) && first.value().position.z == units(-20),
               "the first frame covers a/2 and keeps the layer height");
        const auto cruise = tactical::sample_motion(straight.value(), 300, at(0, 0, -20), Fixed{});
        expect(cruise && near(cruise.value().speed, decimal("3.72"), 8) && cruise.value().yaw == Fixed{},
               "cruising at the maximum speed along the path");
        const auto last_tick = nodes.back().frame.floor_to_integer();
        const auto arriving = tactical::sample_motion(straight.value(), static_cast<std::uint64_t>(last_tick),
            at(0, 0, -20), Fixed{});
        expect(arriving && !arriving.value().finished && arriving.value().position.x <= Fixed{} &&
                   near(arriving.value().position.x, Fixed{}, one / 10),
               "the last frame before the end is within a tenth of a unit of the target");
        const auto done = tactical::sample_motion(straight.value(), static_cast<std::uint64_t>(last_tick + 1),
            at(-3, -1500, -20), Fixed{});
        expect(done && done.value().finished && done.value().position == at(-3, -1500, -20),
               "a finished path leaves the unit where it is (no snap)");
    }
    // MV-13 to MV-15: a target on the left (the S-12 shape) turns only after full speed.
    const auto turning = tactical::plan_move(corvette(), rules, 100, at(-1800, -1500, -20), Fixed{}, Fixed{}, at(-1800, 500, 0));
    expect(static_cast<bool>(turning), "a move with a turn plans");
    if (turning) {
        const auto sample = [&](const std::uint64_t tick) {
            return tactical::sample_motion(turning.value(), tick, Vec3{}, Fixed{}).value();
        };
        expect(sample(162).yaw == Fixed{}, "no turn before full speed");
        expect(near(sample(163).yaw, decimal("1.5061"), one / 1000), "the first arc frame turns 1.506 degrees");
        expect(near(sample(172).yaw, units(15), one / 1000) && near(sample(222).yaw, units(90), one / 1000),
               "15-degree arc steps every 10 frames");
        const auto& end = turning.value().nodes.back();
        expect(near(end.yaw, decimal("97.9296"), one / 1000), "the tangent match heads at the target (97.93 degrees)");
        expect(end.position.x == units(-1800) && end.position.y == units(500), "the path ends on the target");
    }
    // MV-20, MV-21: turning in place (the S-11 shape) and the frigate slowdown.
    const auto face = tactical::plan_face(corvette(), 100, at(-1800, -1500, -20), Fixed{}, at(-1800, 0, 0));
    expect(face && face.value().kind == tactical::MotionKind::turn && face.value().nodes.back().frame == units(220),
           "a corvette turns 90 degrees in place in 90 / 1.5 x 2 = 120 frames");
    if (face) {
        const auto step = tactical::sample_motion(face.value(), 101, at(-1800, -1500, -20), Fixed{});
        const auto end = tactical::sample_motion(face.value(), 220, at(-1800, -1500, -20), decimal("89.25"));
        expect(step && near(step.value().yaw, decimal("0.75"), 64) && step.value().position == at(-1800, -1500, -20),
               "0.75 degrees per frame without moving");
        expect(end && end.value().finished && end.value().yaw == units(90), "the turn ends exactly on the target yaw");
    }
    const auto slow = tactical::plan_face(acclamator(), 0, at(0, 0, -110), units(90), at(1000, 0, 0));
    expect(slow && near(slow.value().nodes.back().frame, units(450), 1024),
           "an Acclamator turns 90 degrees in place in 90 / 0.6 x 3 = 450 frames");
    // MV-12: a target in the planner's start cell gets a direct two-node path.
    const auto trivial = tactical::plan_move(corvette(), rules, 0, at(0, 0, 0), Fixed{}, Fixed{}, at(20, 20, 0));
    expect(trivial && trivial.value().nodes.size() == 2 && trivial.value().nodes.back().speed == Fixed{},
           "a start-cell target is reached directly");
    // MV-03: no plan for the start position or a target outside the motion range.
    const auto here = tactical::plan_move(corvette(), rules, 0, at(5, 5, 0), Fixed{}, Fixed{}, at(5, 5, 90));
    const auto far = tactical::plan_move(corvette(), rules, 0, at(0, 0, 0), Fixed{}, Fixed{},
        at(tactical::max_motion_coordinate + 1, 0, 0));
    expect(here && here.value().kind == tactical::MotionKind::none, "a move to the unit's own XY plans nothing");
    expect(far && far.value().kind == tactical::MotionKind::none, "a target outside the motion range plans nothing");
    // Yaw helpers round-trip.
    const auto rotation = tactical::yaw_rotation(decimal("-97.5"));
    const auto back = rotation ? tactical::yaw_degrees(rotation.value()) : eawr::core::Result<Fixed>::success(Fixed{});
    expect(back && near(back.value(), decimal("-97.5"), 512), "yaw_rotation and yaw_degrees agree");
}

// #351 BK-02 to BK-05 against values worked by hand from the rule (Q24 rounding: a few raw).
void test_bank_rules() {
    const auto bank = [](const tactical::MotionProfile& profile, const char* roll, const char* before, const char* after) {
        const auto value = tactical::bank_roll(profile, decimal(roll), decimal(before), decimal(after));
        return value ? value.value() : units(999);
    };
    // A full-rate left turn (yaw +1.5) aims at -15: the first frame rolls the whole 0.24, the
    // second 0.24 x 14.76 / 15 = 0.23616.
    expect(near(bank(corvette(), "0", "0", "1.5"), decimal("-0.24"), 4), "BK-02: a left turn rolls negative by the rate");
    expect(near(bank(corvette(), "-0.24", "1.5", "3"), decimal("-0.47616"), 8), "BK-03: the step shrinks with the gap");
    expect(near(bank(corvette(), "0", "0", "-1.5"), decimal("0.24"), 4), "BK-02: a right turn rolls positive");
    expect(near(bank(corvette(), "0", "179", "-179.5"), decimal("-0.24"), 4), "BK-02: the turn wraps through 180");
    // A quarter of the rate of turn (0.375) banks half as deep: target -7.5, step 0.24 x 7.5 / 15.
    expect(near(bank(corvette(), "0", "0", "0.375"), decimal("-0.12"), 4), "BK-02: under half the rate banks less");
    // Turning in place: the corvette's 0.75 per frame is half its rate and banks fully; the
    // Nebulon-B's 0.28 (0.84 / 3) is a third: target -5 x 2/3, step 0.24 x (10/3) / 5 = 0.16.
    expect(near(bank(corvette(), "0", "0", "0.75"), decimal("-0.24"), 4), "BK-02: a corvette turning in place banks fully");
    expect(near(bank(nebulon(), "0", "0", "0.28"), decimal("-0.16"), 16), "BK-02: a Nebulon-B turning in place banks 2/3");
    // Near the target the step is a tenth of the rate (0.024) and stops on the target.
    expect(near(bank(corvette(), "-14.9", "0", "1.5"), decimal("-14.924"), 4), "BK-03: at least a tenth of the rate");
    expect(bank(corvette(), "-14.99", "0", "1.5") == units(-15), "BK-03: the roll stops on the bank angle");
    // A frame without a turn eases back: target 0, step 0.24 x 10 / 15 = 0.16.
    expect(near(bank(corvette(), "-10", "30", "30"), decimal("-9.84"), 4), "BK-03: without a turn the roll eases back");
    auto flat = corvette();
    flat.bank_angle = Fixed{};
    expect(bank(flat, "0", "0", "1.5") == Fixed{}, "BK-01: a zero bank angle never rolls");
    // BK-04: at rest the roll levels by the full rate.
    const auto level = [](const char* roll) {
        const auto value = tactical::level_roll(corvette(), decimal(roll));
        return value ? value.value() : units(999);
    };
    expect(level("-10") == decimal("-9.76") && level("10") == decimal("9.76") && level("0.1") == Fixed{} &&
               level("0") == Fixed{},
           "BK-04: at rest the roll levels by 0.24 per frame and stops on zero");
    // BK-05: the roll turns the unit about its own forward axis; negative lowers its left (+Y) side.
    const auto heading = tactical::yaw_rotation(units(30)).value();
    expect(tactical::banked_rotation(heading, Fixed{}).value() == heading, "BK-05: no roll keeps the heading exactly");
    const auto rolled = tactical::banked_rotation(heading, units(-90));
    const auto flat_matrix = math::to_matrix(heading, Vec3{});
    const auto rolled_matrix = rolled ? math::to_matrix(rolled.value(), Vec3{}) : flat_matrix;
    expect(rolled && rolled_matrix && flat_matrix && near(rolled_matrix.value().rows[0][0], flat_matrix.value().rows[0][0], 64) &&
               near(rolled_matrix.value().rows[1][0], flat_matrix.value().rows[1][0], 64) &&
               near(rolled_matrix.value().rows[2][0], Fixed{}, 64) && near(rolled_matrix.value().rows[2][1], units(-1), 64),
           "BK-05: a -90 degree roll keeps the forward axis and turns the left side straight down");
    const auto banked_yaw = tactical::yaw_degrees(tactical::banked_rotation(heading, units(15)).value());
    expect(banked_yaw && near(banked_yaw.value(), units(30), 512), "BK-05: a banked unit keeps its yaw");
}

// --- Session fixtures ------------------------------------------------------------------------

[[nodiscard]] math::Quat yaw(const std::int64_t degrees) { return tactical::yaw_rotation(units(degrees)).value(); }

[[nodiscard]] tactical::PlayerCommand command(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, const eawr::sim::EntityId unit, tactical::CommandPayload payload) {
    return {{tick, player, sequence}, {unit}, std::move(payload)};
}

[[nodiscard]] tactical::TacticalSetup setup(const std::vector<tactical::UnitState>& units_list) {
    tactical::TacticalSetup value;
    value.seed = 0x5eed000000000070ULL;
    const std::string_view content = "EAWR-P2-07-tactical-motion-foc-shaped-content\n";
    const auto digest = eawr::sim::sha256_hex(
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
    for (std::size_t index = 0; index < value.content_identity.size(); ++index) {
        value.content_identity[index] = static_cast<std::uint8_t>(std::stoi(digest.substr(index * 2, 2), nullptr, 16));
    }
    value.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    value.units = units_list;
    return value;
}

// The single-move fixture: a corvette on the S-10 line, an Acclamator that turns toward its
// target, a station without a motion profile; later a face order and a stop.
[[nodiscard]] tactical::TacticalReplay single_move() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {2, acclamator_type, 2, at(0, 1000, -110), yaw(90), {}},
        {3, station_type, 1, at(1500, -1500, 0), yaw(0), {}},
    });
    replay.final_tick_count = 600;
    replay.commands = {
        command(30, 1, 0, 1, tactical::MovePayload{at(0, -1500, 0)}),
        command(30, 2, 0, 2, tactical::MovePayload{at(-1500, 2500, 0)}),
        command(400, 1, 1, 1, tactical::FacePayload{at(-600, 0, 0)}),
        command(450, 2, 1, 2, tactical::StopPayload{}),
    };
    return replay;
}

// The blocked-path fixture (#70 option B): orders that cannot move a unit are accepted and it
// stays: a station has no motion profile, a corvette is sent to its own position, another to a
// point outside the motion range. #266 adds destinations clipped by occupied space.
[[nodiscard]] tactical::TacticalReplay blocked_path() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {3, station_type, 1, at(1500, -1500, 0), yaw(0), {}},
        {4, corvette_type, 1, at(-1800, 1500, -20), yaw(180), {}},
    });
    replay.final_tick_count = 60;
    replay.commands = {
        command(10, 1, 0, 1, tactical::MovePayload{at(-1800, -1500, 500)}),
        command(10, 1, 1, 3, tactical::MovePayload{at(0, 0, 0)}),
        command(10, 1, 2, 4, tactical::MovePayload{at(tactical::max_motion_coordinate + 1, 1500, 0)}),
        command(20, 1, 3, 3, tactical::FacePayload{at(1500, 0, 0)}),
    };
    return replay;
}

struct Run {
    std::vector<std::string> hashes; // tick,sha256 rows 1..N
    std::vector<std::vector<tactical::UnitState>> units;
    std::vector<std::string> events;
};

[[nodiscard]] std::optional<Run> run(tactical::TacticalSession session, const std::uint64_t ticks,
    const std::size_t workers, const bool scramble) {
    if (scramble) session.scramble_storage_for_testing();
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Run result;
    result.units.push_back(session.units());
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        if (!stepped) {
            std::cerr << stepped.error().message << '\n';
            return std::nullopt;
        }
        result.hashes.push_back(std::to_string(stepped.value().completed_tick) + "," + stepped.value().state_sha256);
        result.units.push_back(session.units());
        for (const auto& event : stepped.value().snapshot->events()) {
            result.events.push_back(std::to_string(event.tick) + "," + std::to_string(event.unit) + ","
                + std::string(tactical::to_string(event.kind)) + "," + std::string(tactical::to_string(event.order)));
        }
        if (scramble) session.scramble_storage_for_testing();
    }
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

[[nodiscard]] std::vector<std::string> read_rows(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<std::string> rows;
    std::string line;
    while (std::getline(file, line)) {
        // Text goldens may be checked out with CRLF under core.autocrlf.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        rows.push_back(line);
    }
    if (!rows.empty()) rows.erase(rows.begin());
    return rows;
}

[[nodiscard]] const tactical::UnitState* unit_of(const std::vector<tactical::UnitState>& units_list, const eawr::sim::EntityId id) {
    for (const auto& unit : units_list) {
        if (unit.entity_id == id) return &unit;
    }
    return nullptr;
}

// Replays one fixture with 1, 2, 4, 8 and the hardware count of workers, plain and scrambled;
// returns the first run.
[[nodiscard]] std::optional<Run> check_fixture(const std::filesystem::path& fixtures, const std::string& name,
    const tactical::TacticalReplay& replay, const bool regenerate) {
    const auto encoded = tactical::write_replay(replay);
    expect(static_cast<bool>(encoded), name + " writes");
    if (!encoded) return std::nullopt;
    const auto replay_path = fixtures / (name + ".eawr-replay");
    const auto hashes_path = fixtures / (name + ".hashes.csv");
    std::optional<Run> first;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8},
             eawr::platform::ThreadWorkerAdapter::hardware_worker_count()}) {
        for (const bool scramble : {false, true}) {
            auto session = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
            expect(static_cast<bool>(session), name + " replays");
            if (!session) return std::nullopt;
            auto result = run(std::move(session).value(), replay.final_tick_count, workers, scramble);
            expect(result.has_value(), name + ": every step succeeds");
            if (!result) return std::nullopt;
            if (!first) {
                first = std::move(result);
            } else {
                expect(result->hashes == first->hashes,
                    name + ": " + std::to_string(workers) + " workers" + (scramble ? ", scrambled," : "")
                        + " hash like 1 worker");
            }
        }
    }
    if (regenerate) {
        std::ofstream(replay_path, std::ios::binary)
            .write(reinterpret_cast<const char*>(encoded.value().data()), static_cast<std::streamsize>(encoded.value().size()));
        std::ofstream hashes(hashes_path, std::ios::binary);
        hashes << "tick,sha256\n";
        for (const auto& row : first->hashes) hashes << row << '\n';
        std::cout << "regenerated " << name << '\n';
        return first;
    }
    const auto committed = read_bytes(replay_path);
    expect(committed == encoded.value(), name + ": the committed replay is the fixture's bytes");
    const auto parsed = tactical::parse_replay(committed, name);
    expect(parsed && parsed.value() == replay, name + ": the committed replay parses to the fixture");
    expect(first->hashes == read_rows(hashes_path), name + ": state hashes match the golden");

    // A live session fed the same commands records the fixture replay and ends on its hash.
    auto live = tactical::TacticalSession::create(replay.setup, {}, {}, foc_table());
    expect(static_cast<bool>(live), name + ": a live session starts");
    if (!live) return first;
    for (const auto& item : replay.commands) expect(static_cast<bool>(live.value().submit(item)), name + ": live submit");
    const eawr::platform::ThreadWorkerAdapter two(2);
    for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
        expect(static_cast<bool>(live.value().step(two)), name + ": live step");
    }
    const auto recorded = tactical::write_replay(live.value().record());
    expect(recorded && recorded.value() == encoded.value(), name + ": the live recording is the fixture replay");
    expect(!first->hashes.empty() && first->hashes.back()
               == std::to_string(live.value().completed_tick()) + "," + live.value().state_sha256(),
           name + ": the live session ends on the golden hash");
    return first;
}

// #351 in a session: the C-02 turn in place (face +Y from rest, facing +X) banks the corvette
// left from tick 32, deepest on the turn's last frame (151), then levels by 0.24 per frame; a
// right turn banks the other way. The instance transform carries the roll, and 1, 2, 4 and 8
// workers agree on every roll and hash.
void test_bank_session() {
    tactical::TacticalReplay replay;
    replay.setup = setup({
        {1, corvette_type, 1, at(-1800, -1500, -20), yaw(0), {}},
        {2, corvette_type, 1, at(1800, -1500, -20), yaw(0), {}},
    });
    replay.final_tick_count = 260;
    replay.commands = {
        command(30, 1, 0, 1, tactical::FacePayload{at(-1800, 0, 0)}),
        command(30, 1, 1, 2, tactical::FacePayload{at(1800, -3000, 0)}),
    };
    std::optional<std::vector<std::string>> first_hashes;
    std::vector<Fixed> left;
    std::vector<Fixed> right;
    for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        auto session = tactical::TacticalSession::from_replay(replay, {}, {}, foc_table());
        expect(static_cast<bool>(session), "bank session starts");
        if (!session) return;
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        std::vector<Fixed> rolls{Fixed{}};
        std::vector<Fixed> others{Fixed{}};
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
            const auto stepped = session.value().step(executor);
            expect(static_cast<bool>(stepped), "bank session steps");
            if (!stepped) return;
            hashes.push_back(stepped.value().state_sha256);
            rolls.push_back(session.value().roll_degrees(1).value_or(units(999)));
            others.push_back(session.value().roll_degrees(2).value_or(units(999)));
            if (stepped.value().completed_tick == 100) {
                const auto& instance = stepped.value().snapshot->instances()[0];
                expect(near(instance.fixed_transform.rows[2][1], math::sin_turn(math::divide(rolls.back(), units(360)).value()), 64)
                           && instance.fixed_transform.rows[2][1].raw() < 0,
                       "BK-05: the instance transform carries the roll (left side down)");
            }
        }
        if (!first_hashes) {
            first_hashes = hashes;
            left = rolls;
            right = others;
        } else {
            expect(hashes == *first_hashes && rolls == left && others == right,
                   "bank session: " + std::to_string(workers) + " workers match 1 worker");
        }
    }
    expect(left[31] == Fixed{} && near(left[32], decimal("-0.24"), 4) && near(left[33], decimal("-0.47616"), 8),
           "BK-02: no roll before the first turning frame, then -0.24 and -0.47616");
    expect(left[151] < left[150] && left[151] > units(-15), "BK-02: the last turning frame still banks, short of -15");
    expect(near(left[152], math::add(left[151], decimal("0.24")).value(), 1) &&
               near(left[153], math::add(left[152], decimal("0.24")).value(), 1),
           "BK-04: at rest the roll levels by 0.24 per frame");
    expect(left[259] == Fixed{} && right[259] == Fixed{}, "BK-04: the roll is level again");
    bool mirrored = true;
    for (std::size_t tick = 0; tick < left.size(); ++tick) mirrored = mirrored && near(right[tick], Fixed::from_raw(-left[tick].raw()), 16);
    expect(mirrored, "BK-02: a right turn banks the mirror of a left turn");
}

void test_fixtures(const std::filesystem::path& fixtures, const bool regenerate) {
    const auto moved = check_fixture(fixtures, "tactical-motion", single_move(), regenerate);
    if (moved) {
        const auto& frames = moved->units;
        const auto corvette_at = [&](const std::size_t tick) { return *unit_of(frames[tick], 1); };
        const auto frigate_at = [&](const std::size_t tick) { return *unit_of(frames[tick], 2); };
        expect(corvette_at(31).position == corvette_at(0).position && corvette_at(32).position != corvette_at(0).position,
               "MV-02: an order at tick 30 moves the unit from tick 32");
        expect(corvette_at(0).position.z == corvette_at(399).position.z, "a move stays in the unit's layer");
        expect(frigate_at(451).position != frigate_at(450).position && frigate_at(452).position == frigate_at(451).position,
               "MV-22: a stop at 450 still moves at 451, then holds");
        expect(corvette_at(402).position == corvette_at(401).position, "MV-20: a face order stops the ship and turns it");
        expect(unit_of(frames[599], 3)->position == unit_of(frames[0], 3)->position &&
                   unit_of(frames[599], 3)->rotation == unit_of(frames[0], 3)->rotation,
               "MV-03: a unit without a motion profile never moves");
        expect(moved->events.size() == 4, "every fixture order is accepted");
    }
    const auto blocked = check_fixture(fixtures, "tactical-motion-blocked", blocked_path(), regenerate);
    if (blocked) {
        bool still = true;
        for (const auto& frame : blocked->units) {
            for (const auto& unit : frame) {
                const auto* start = unit_of(blocked->units.front(), unit.entity_id);
                still = still && start != nullptr && unit.position == start->position && unit.rotation == start->rotation;
            }
        }
        expect(still, "blocked orders move nothing");
        std::size_t accepted = 0;
        for (const auto& event : blocked->events) accepted += event.find("order_accepted") != std::string::npos ? 1U : 0U;
        expect(accepted == 4, "blocked orders are accepted, not rejected");
    }

    // Without a motion table the single-move replay moves nothing: sessions bound to no motion
    // table behave and hash as before #70.
    auto blind = tactical::TacticalSession::from_replay(single_move());
    expect(static_cast<bool>(blind), "the fixture runs without a motion table");
    if (blind) {
        const auto result = run(std::move(blind).value(), 60, 1, false);
        expect(result && unit_of(result->units.back(), 1)->position == unit_of(result->units.front(), 1)->position,
               "no unit moves without a motion table");
    }
    // A motion table that names none of a replay's types leaves the tactical-v2 goldens unchanged.
    const auto bytes = read_bytes(fixtures / "tactical-v2.eawr-replay");
    const auto replay = tactical::parse_replay(bytes, "tactical-v2.eawr-replay");
    expect(static_cast<bool>(replay), "tactical-v2 parses");
    if (replay) {
        auto session = tactical::TacticalSession::from_replay(replay.value(), {}, {}, foc_table());
        expect(static_cast<bool>(session), "tactical-v2 replays with the motion table");
        if (session) {
            const auto result = run(std::move(session).value(), replay.value().final_tick_count, 4, true);
            expect(result && result->hashes == read_rows(fixtures / "tactical-v2.hashes.csv"),
                   "tactical-v2 hashes are unchanged under an unrelated motion table");
        }
    }
}

} // namespace

int main(const int argc, char** argv) {
    const bool regenerate = argc == 3 && std::string_view(argv[2]) == "--regenerate";
    if (argc != 2 && !regenerate) {
        std::cerr << "usage: tactical_motion_tests <fixture directory> [--regenerate]\n";
        return 2;
    }
    test_validation();
    test_rules();
    test_bank_rules();
    test_bank_session();
    test_fixtures(argv[1], regenerate);
    if (failures != 0) {
        std::cerr << failures << " motion contract(s) failed\n";
        return 1;
    }
    std::cout << "motion contracts passed\n";
    return 0;
}
