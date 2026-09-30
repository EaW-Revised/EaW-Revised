#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-09 (#72): hull and hardpoint health, hardpoint loss, unit death and repair
// (docs/behaviour/space-hardpoints.md). The rule cases use FoC values; the session fixture
// comes from the independent oracle generate_tactical_fixture.py (`tactical-durability`), and
// every worker count, storage order and a written-and-parsed replay must reproduce its goldens.
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

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] Fixed decimal(const std::string_view text) { return Fixed::from_decimal(text).value(); }
[[nodiscard]] double real(const Fixed value) { return static_cast<double>(value.raw()) / static_cast<double>(one); }

[[nodiscard]] tactical::HardpointProfile hardpoint(
    const tactical::HardpointRole role, const std::int64_t health, const std::string_view repair = {},
    const std::string_view cost = {}) {
    tactical::HardpointProfile profile;
    profile.role = role;
    profile.destroyable = health > 0;
    profile.max_health = units(health);
    profile.repair_amount_per_frame = repair.empty() ? Fixed{} : decimal(repair);
    profile.repair_cost_per_frame = cost.empty() ? Fixed{} : decimal(cost);
    return profile;
}

// FoC values x Object_Max_Health_Multiplier_Space 1.5 (units::durability_table on the FoC
// data gives the same numbers; tests/units pins them).
[[nodiscard]] tactical::DurabilityTable foc_table() {
    using Role = tactical::HardpointRole;
    tactical::DurabilityTable table;
    table.rules = {decimal("0.2"), decimal("0.4"), decimal("0.33")};
    tactical::DurabilityProfile nebulon{10, units(5400), decimal("2.2"), false, {}};
    for (int index = 0; index < 4; ++index) nebulon.hardpoints.push_back(hardpoint(Role::weapon, 390));
    nebulon.hardpoints.push_back(hardpoint(Role::engine, 390));
    tactical::DurabilityProfile acclamator{20, units(3000), decimal("2.2"), false,
        {hardpoint(Role::weapon, 210), hardpoint(Role::weapon, 210), hardpoint(Role::weapon, 240),
            hardpoint(Role::weapon, 210), hardpoint(Role::weapon, 210), hardpoint(Role::weapon, 240),
            hardpoint(Role::engine, 255), hardpoint(Role::fighter_bay, 150)}};
    tactical::DurabilityProfile corellian{30, units(1125), decimal("3.1"), false, {}};
    for (int index = 0; index < 8; ++index) corellian.hardpoints.push_back(hardpoint(Role::weapon, 0));
    tactical::DurabilityProfile station{40, units(2400), std::nullopt, false,
        {hardpoint(Role::special_ability, 675, ".50", "1.3"), hardpoint(Role::special_ability, 675, ".50", "1.3"),
            hardpoint(Role::weapon, 975, ".50", "1.3"), hardpoint(Role::weapon, 675, ".50", "1.3"),
            hardpoint(Role::weapon, 675, ".50", "1.3"), hardpoint(Role::shield_generator, 975, ".50", "1.5"),
            hardpoint(Role::fighter_bay, 1500, ".50", "1.5")}};
    table.profiles = {nebulon, acclamator, corellian, station};
    return table;
}

void test_validation() {
    expect(static_cast<bool>(tactical::validate_durability(foc_table())), "the FoC-shaped table is valid");
    expect(static_cast<bool>(tactical::validate_durability({})), "an empty table binds nothing");
    auto unordered = foc_table();
    std::swap(unordered.profiles[0], unordered.profiles[1]);
    expect(!tactical::validate_durability(unordered), "type IDs must increase");
    auto no_hull = foc_table();
    no_hull.profiles[0].max_hull = Fixed{};
    expect(!tactical::validate_durability(no_hull), "a hull must be positive");
    auto huge = foc_table();
    huge.profiles[0].max_hull = Fixed::from_raw(tactical::max_durability_health * one + 1);
    expect(!tactical::validate_durability(huge), "a hull above the bound fails");
    auto healthless = foc_table();
    healthless.profiles[0].hardpoints[0].max_health = Fixed{};
    expect(!tactical::validate_durability(healthless), "a destroyable hardpoint needs health");
    auto fast = foc_table();
    fast.rules.engines_disabled_speed = units(2);
    expect(!tactical::validate_durability(fast), "the engines-off modifier may not exceed 1");
    auto setup = tactical::TacticalSetup{};
    expect(!tactical::TacticalSession::create(setup, {}, unordered), "a session rejects an invalid table");
}

void test_rules() {
    const auto table = foc_table();
    const auto& rules = table.rules;
    const auto& nebulon = table.profiles[0];
    auto state = tactical::full_durability(nebulon);
    expect(state.hull == units(5400) && state.hardpoints.size() == 5 && state.hardpoints[4] == units(390),
           "HD-01: a unit starts at full hull and hardpoint health");
    expect(tactical::engines_online(nebulon, state) && tactical::max_speed_factor(nebulon, rules, state) == units(1),
           "engines online at full speed");

    // HD-02, HD-03: hardpoint damage stays on the hardpoint; the excess is lost.
    auto outcome = tactical::apply_damage(nebulon, state, 4, units(400));
    expect(outcome.destroyed_hardpoint == 4U && !outcome.unit_destroyed && state.hull == units(5400)
               && state.hardpoints[4] == Fixed{},
           "a 400 hit destroys the 390 engine and never reaches the hull");
    expect(!tactical::engines_online(nebulon, state) && tactical::max_speed_factor(nebulon, rules, state) == decimal("0.4"),
           "HD-11: without engines the maximum speed is scaled by 0.4");
    outcome = tactical::apply_damage(nebulon, state, 4, units(10));
    expect(!outcome.destroyed_hardpoint && state.hardpoints[4] == Fixed{}, "a destroyed hardpoint takes no more damage");

    // HD-04: damaged strictly below 0.33 of the maximum.
    const auto threshold = Fixed::from_raw((units(390).raw() * decimal("0.33").raw() + one - 1) / one);
    state.hardpoints[0] = threshold;
    expect(tactical::hardpoint_state(nebulon, rules, state, 0) == tactical::HardpointState::intact,
           "at 0.33 of its maximum a hardpoint is intact");
    state.hardpoints[0] = Fixed::from_raw(threshold.raw() - 1);
    expect(tactical::hardpoint_state(nebulon, rules, state, 0) == tactical::HardpointState::damaged,
           "one raw unit below 0.33 it is damaged");
    expect(tactical::weapon_enabled(nebulon, state, 0), "a damaged weapon still fires");
    expect(tactical::hardpoint_state(nebulon, rules, state, 4) == tactical::HardpointState::destroyed
               && !tactical::weapon_enabled(nebulon, state, 4),
           "a destroyed hardpoint is destroyed and disabled");
    const auto& corellian = table.profiles[2];
    const auto corvette = tactical::full_durability(corellian);
    expect(!tactical::damage_target_valid(corellian, 0) && tactical::damage_target_valid(corellian, tactical::hull_target)
               && tactical::hardpoint_state(corellian, rules, corvette, 0) == tactical::HardpointState::intact
               && tactical::weapon_enabled(corellian, corvette, 0),
           "corvette hardpoints are neither targetable for damage nor destroyable");

    // HD-12, HD-13: the station loses its shield with its only generator, launches with its bay.
    const auto& station = table.profiles[3];
    auto base = tactical::full_durability(station);
    expect(tactical::shields_online(station, base) && tactical::launch_ready(station, base), "a full station");
    static_cast<void>(tactical::apply_damage(station, base, 5, units(975)));
    static_cast<void>(tactical::apply_damage(station, base, 6, units(1500)));
    expect(!tactical::shields_online(station, base) && !tactical::launch_ready(station, base),
           "no shield generator and no fighter bay left");
    expect(tactical::shields_online(nebulon, state), "a type without shield generators keeps its shield");
    expect(!tactical::launch_ready(nebulon, state), "a type without a fighter bay cannot launch");

    // HD-20: the hull dies at zero; HD-21: a type that dies with its hardpoints.
    auto hull = tactical::full_durability(nebulon);
    expect(!tactical::apply_damage(nebulon, hull, tactical::hull_target, units(5399)).unit_destroyed
               && tactical::apply_damage(nebulon, hull, tactical::hull_target, units(1)).unit_destroyed
               && hull.hull == Fixed{},
           "the unit dies when its hull reaches zero");
    auto fragile = nebulon;
    fragile.destroyed_with_hardpoints = true;
    auto shell = tactical::full_durability(fragile);
    for (std::uint32_t index = 0; index < 4; ++index) {
        expect(!tactical::apply_damage(fragile, shell, index, units(390)).unit_destroyed, "hardpoints fall one by one");
    }
    const auto last = tactical::apply_damage(fragile, shell, 4, units(390));
    expect(last.destroyed_hardpoint == 4U && last.unit_destroyed && shell.hull == Fixed{},
           "HD-21: the last hardpoint takes the unit with it");
}

void test_service() {
    const auto table = foc_table();
    const auto& rules = table.rules;
    const auto& acclamator = table.profiles[1];

    auto full = tactical::full_durability(acclamator);
    const auto untouched = full;
    auto served = tactical::service_durability(acclamator, rules, full);
    expect(served && served.value().destroyed_hardpoints.empty() && full == untouched,
           "HS-03: a full hull leaves full hardpoints alone");

    // Hull at one third: hardpoints are pulled towards 1/3 + 0.2 of their total.
    auto state = tactical::full_durability(acclamator);
    static_cast<void>(tactical::apply_damage(acclamator, state, 7, units(150)));
    static_cast<void>(tactical::apply_damage(acclamator, state, tactical::hull_target, units(2000)));
    const auto before = state;
    served = tactical::service_durability(acclamator, rules, state);
    expect(served && served.value().destroyed_hardpoints.empty() && state.hull == units(1000), "the hull is unchanged");
    double total = 0;
    double current = 0;
    for (std::size_t index = 0; index < acclamator.hardpoints.size(); ++index) {
        total += real(acclamator.hardpoints[index].max_health);
        current += real(before.hardpoints[index]);
    }
    const double limit = 1000.0 / 3000.0 + real(rules.hull_vs_hardpoints);
    const double excess = current - total * limit;
    for (std::size_t index = 0; index < 7; ++index) {
        const double expected = real(before.hardpoints[index]) - excess * real(before.hardpoints[index]) / total;
        expect(std::abs(real(state.hardpoints[index]) - expected) < 1.0e-6,
               "HS-04: hardpoint " + std::to_string(index) + " loses excess x current / total");
    }
    expect(state.hardpoints[7] == Fixed{}, "a destroyed hardpoint is skipped");

    // HS-02 (types that die with their hardpoints): the hull is capped by the hardpoints.
    auto fragile = table.profiles[0];
    fragile.destroyed_with_hardpoints = true;
    auto shell = tactical::full_durability(fragile);
    for (std::uint32_t index = 0; index < 3; ++index) static_cast<void>(tactical::apply_damage(fragile, shell, index, units(390)));
    served = tactical::service_durability(fragile, rules, shell);
    expect(served && std::abs(real(shell.hull) - 5400.0 * (0.4 + real(rules.hull_vs_hardpoints))) < 1.0e-5,
           "HS-02: two of five hardpoints left caps the hull at 0.4 + 0.2");
}

void test_repair() {
    const auto table = foc_table();
    const auto& station = table.profiles[3];
    auto state = tactical::full_durability(station);
    static_cast<void>(tactical::apply_damage(station, state, 5, units(100)));
    // M2 (SK-30): every player has 0 credits, so a repair stops on its first frame unpaid.
    auto frame = tactical::repair_frame(station, state, 5, Fixed{});
    expect(frame && !frame.value().paid && frame.value().stopped && state.hardpoints[5] == units(875),
           "HR-03: no credits, no repair");
    frame = tactical::repair_frame(station, state, 5, units(10));
    expect(frame && frame.value().paid && !frame.value().stopped && frame.value().cost == decimal("1.5")
               && state.hardpoints[5] == Fixed::from_raw(units(875).raw() + decimal(".50").raw()),
           "HR-01: a paid frame restores Repair_Amount_Per_Frame for Repair_Cost_Per_Frame");
    int frames = 1;
    while (frame && frame.value().paid && !frame.value().stopped && frames < 1000) {
        frame = tactical::repair_frame(station, state, 5, units(10));
        ++frames;
    }
    expect(frames == 200 && state.hardpoints[5] == units(975), "HR-05: 200 half-point frames restore 100 health, then stop");
    static_cast<void>(tactical::apply_damage(station, state, 6, units(1500)));
    frame = tactical::repair_frame(station, state, 6, units(10));
    expect(frame && !frame.value().paid && frame.value().stopped && state.hardpoints[6] == Fixed{},
           "HR-02: a destroyed hardpoint cannot be repaired");
    const auto& nebulon = table.profiles[0];
    auto ship = tactical::full_durability(nebulon);
    static_cast<void>(tactical::apply_damage(nebulon, ship, 0, units(100)));
    frame = tactical::repair_frame(nebulon, ship, 0, units(10));
    expect(frame && !frame.value().paid && frame.value().stopped, "ship hardpoints author no repair");

    // HR-04: a hull below the hardpoints' fraction rises with the repair.
    auto hulled = tactical::full_durability(station);
    static_cast<void>(tactical::apply_damage(station, hulled, 5, units(100)));
    static_cast<void>(tactical::apply_damage(station, hulled, tactical::hull_target, units(1200)));
    frame = tactical::repair_frame(station, hulled, 5, units(10));
    double total = 0;
    double current = 0;
    for (std::size_t index = 0; index < station.hardpoints.size(); ++index) {
        total += real(station.hardpoints[index].max_health);
        current += real(hulled.hardpoints[index]);
    }
    const double expected = 1200.0 * (1.0 + current / total - 0.5);
    expect(frame && std::abs(real(hulled.hull) - expected) < 1.0e-5, "HR-04: the hull rises by the fraction gap");
}

// --- Oracle fixture -------------------------------------------------------------------------

[[nodiscard]] std::vector<std::string> lines(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(stream, line)) {
        // Text goldens may be checked out with CRLF under core.autocrlf.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        result.push_back(line);
    }
    return result;
}

[[nodiscard]] std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) fields.push_back(field);
    return fields;
}

// tactical-durability.table.csv: one row per hardpoint; the rules and each hull are rows too.
[[nodiscard]] std::optional<tactical::DurabilityTable> read_table(const std::filesystem::path& path) {
    const auto rows = lines(path);
    if (rows.empty() || rows[0] != "kind,type_id,role,destroyable,max_raw,repair_amount_raw,repair_cost_raw") {
        return std::nullopt;
    }
    tactical::DurabilityTable table;
    std::map<std::uint64_t, tactical::DurabilityProfile> profiles;
    for (std::size_t index = 1; index < rows.size(); ++index) {
        const auto field = split(rows[index]);
        if (field.size() != 7) return std::nullopt;
        const auto number = [&](const std::size_t column) { return std::stoll(field[column]); };
        if (field[0] == "rules") {
            table.rules = {Fixed::from_raw(number(4)), Fixed::from_raw(number(5)), Fixed::from_raw(number(6))};
        } else if (field[0] == "hull") {
            auto& profile = profiles[std::stoull(field[1])];
            profile.type_id = std::stoull(field[1]);
            profile.max_hull = Fixed::from_raw(number(4));
            if (field[5] != "none") profile.max_speed = Fixed::from_raw(number(5));
            profile.destroyed_with_hardpoints = field[6] == "1";
        } else if (field[0] == "hardpoint") {
            profiles[std::stoull(field[1])].hardpoints.push_back(tactical::HardpointProfile{
                static_cast<tactical::HardpointRole>(number(2)), field[3] == "1", Fixed::from_raw(number(4)),
                Fixed::from_raw(number(5)), Fixed::from_raw(number(6))});
        } else {
            return std::nullopt;
        }
    }
    for (auto& [id, profile] : profiles) {
        static_cast<void>(id);
        table.profiles.push_back(std::move(profile));
    }
    return table;
}


struct Run {
    std::vector<std::string> hashes;    // tick,sha256 rows 1..N
    std::vector<std::string> snapshots; // tick,sha256 rows 0..N
    std::vector<std::string> events;
    std::vector<std::shared_ptr<const tactical::TacticalSnapshot>> published;
};

[[nodiscard]] std::optional<Run> run(tactical::TacticalSession session, const std::uint64_t ticks, const std::size_t workers,
                                     const bool scramble) {
    if (scramble) session.scramble_storage_for_testing();
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Run result;
    result.snapshots.push_back("0," + session.snapshot()->sha256());
    result.published.push_back(session.snapshot());
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        if (!stepped) {
            std::cerr << stepped.error().message << '\n';
            return std::nullopt;
        }
        const auto& value = stepped.value();
        result.hashes.push_back(std::to_string(value.completed_tick) + "," + value.state_sha256);
        result.snapshots.push_back(std::to_string(value.completed_tick) + "," + value.snapshot->sha256());
        result.published.push_back(value.snapshot);
        for (const auto& event : value.snapshot->events()) {
            result.events.push_back(std::to_string(event.tick) + "," + std::to_string(event.player) + ","
                + std::to_string(event.sequence) + "," + std::to_string(event.unit) + "," + std::string(tactical::to_string(event.kind))
                + "," + std::string(tactical::to_string(event.order)) + "," + std::string(tactical::to_string(event.reason))
                + "," + std::to_string(event.hardpoint));
        }
        if (scramble) session.scramble_storage_for_testing();
    }
    return result;
}

[[nodiscard]] const tactical::InstanceDurability* durability_of(
    const tactical::TacticalSnapshot& snapshot, const eawr::sim::EntityId id) {
    for (const auto& instance : snapshot.instances()) {
        if (instance.entity_id == id) return instance.durability ? &*instance.durability : nullptr;
    }
    return nullptr;
}

void check_consequences(const Run& result, const tactical::DurabilityTable& table) {
    using State = tactical::HardpointState;
    const auto& published = result.published;
    expect(published.size() == 7, "six ticks were published");
    if (published.size() != 7) return;
    const auto* start = durability_of(*published[0], 1);
    expect(start != nullptr && start->engines_online && start->max_speed == decimal("2.2") && start->hardpoints.size() == 5,
           "tick 0: the Nebulon-B starts intact at Max_Speed 2.2");
    const auto* squadron = durability_of(*published[0], 5);
    expect(squadron == nullptr, "a squadron has no durability block");

    // Tick 0 destroyed the engine and scratched weapon 0: speed x 0.4, weapons still fire.
    const auto* nebulon = durability_of(*published[1], 1);
    expect(nebulon != nullptr, "the Nebulon-B is durable");
    if (nebulon == nullptr) return;
    const auto slowed = math::multiply(decimal("2.2"), table.rules.engines_disabled_speed).value();
    expect(!nebulon->engines_online && nebulon->max_speed_factor == decimal("0.4") && nebulon->max_speed == slowed,
           "engine destroyed: max speed 2.2 x 0.4");
    expect(nebulon->hull == units(5400), "hardpoint damage never reaches the hull");
    expect(nebulon->hardpoints[4].state == State::destroyed && !nebulon->hardpoints[4].enabled, "the engine is destroyed");
    expect(nebulon->hardpoints[0].state == State::intact && nebulon->hardpoints[0].enabled
               && nebulon->hardpoints[0].health == units(190),
           "weapon 0 at 190 of 390 is intact and enabled");

    nebulon = durability_of(*published[2], 1);
    expect(nebulon != nullptr && nebulon->hardpoints[0].state == State::damaged && nebulon->hardpoints[0].enabled,
           "weapon 0 at 90 of 390 is damaged and still fires");
    const auto* acclamator = durability_of(*published[2], 2);
    expect(acclamator != nullptr && !acclamator->launch_ready && acclamator->hardpoints[7].state == State::destroyed
               && acclamator->engines_online,
           "fighter bay destroyed: the Acclamator can no longer launch");
    const auto* station = durability_of(*published[2], 4);
    expect(station != nullptr && !station->shields_online && station->launch_ready, "shield generator destroyed: shield off");

    // Tick 2 destroyed weapon 0: it may not fire; the other three still may.
    nebulon = durability_of(*published[3], 1);
    expect(nebulon != nullptr && nebulon->hardpoints[0].state == State::destroyed && !nebulon->hardpoints[0].enabled
               && nebulon->hardpoints[1].enabled && nebulon->hardpoints[2].enabled && nebulon->hardpoints[3].enabled,
           "weapon 0 destroyed: firing disabled on it alone");
    const auto* corellian = durability_of(*published[3], 3);
    expect(corellian != nullptr && corellian->hull == units(1125) && corellian->hardpoints[0].enabled,
           "the rejected corvette hit changed nothing");

    // Tick 3 took the Acclamator hull to a third: its live hardpoints are pulled down (HS-03).
    acclamator = durability_of(*published[4], 2);
    expect(acclamator != nullptr && acclamator->hull == units(1000) && acclamator->hardpoints[0].health < units(210)
               && acclamator->hardpoints[0].health > units(100),
           "the hull at a third pulls the hardpoints down");

    // Tick 4 killed it; tick 5's orders for it and on it are rejected.
    expect(durability_of(*published[5], 2) == nullptr && published[5]->instances().size() == 4,
           "the dead Acclamator left the snapshot");
}

void test_fixture(const std::filesystem::path& fixtures) {
    const auto table = read_table(fixtures / "tactical-durability.table.csv");
    expect(table.has_value(), "the durability table reads");
    if (!table) return;
    expect(*table == foc_table(), "the oracle table is the FoC-shaped table");
    std::ifstream file(fixtures / "tactical-durability.eawr-replay", std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto replay = tactical::parse_replay(bytes, "tactical-durability.eawr-replay");
    expect(static_cast<bool>(replay), "the durability replay parses");
    if (!replay) {
        std::cerr << replay.error().message << '\n';
        return;
    }
    const auto written = tactical::write_replay(replay.value());
    expect(written && written.value() == bytes, "the replay writes back byte for byte");

    const auto golden = [&](const std::string& suffix) {
        auto rows = lines(fixtures / ("tactical-durability." + suffix));
        if (!rows.empty()) rows.erase(rows.begin());
        return rows;
    };
    const auto hashes = golden("hashes.csv");
    const auto snapshots = golden("snapshots.csv");
    const auto events = golden("events.csv");
    std::optional<Run> first;
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        for (const bool scramble : {false, true}) {
            auto session = tactical::TacticalSession::from_replay(replay.value(), {}, *table);
            expect(static_cast<bool>(session), "the durability session replays");
            if (!session) return;
            const auto result = run(std::move(session).value(), replay.value().final_tick_count, workers, scramble);
            expect(result.has_value(), "every step succeeds");
            if (!result) return;
            const auto label = " (" + std::to_string(workers) + " workers" + (scramble ? ", scrambled)" : ")");
            expect(result->hashes == hashes, "state hashes match the oracle" + label);
            expect(result->snapshots == snapshots, "snapshot digests match the oracle" + label);
            expect(result->events == events, "events match the oracle" + label);
            if (!first) first = result;
        }
    }
    if (first) check_consequences(*first, *table);

    // A live session fed the same commands records a replay that reproduces it.
    auto live = tactical::TacticalSession::create(replay.value().setup, {}, *table);
    expect(static_cast<bool>(live), "a live durability session starts");
    if (!live) return;
    for (const auto& command : replay.value().commands) {
        expect(static_cast<bool>(live.value().submit(command)), "live submit");
    }
    const eawr::platform::ThreadWorkerAdapter two(2);
    for (std::uint64_t tick = 0; tick < replay.value().final_tick_count; ++tick) {
        expect(static_cast<bool>(live.value().step(two)), "live step");
    }
    const auto recorded = tactical::write_replay(live.value().record());
    expect(recorded && recorded.value() == bytes, "the live recording is the fixture replay");
    expect(!hashes.empty() && hashes.back() == std::to_string(live.value().completed_tick()) + "," + live.value().state_sha256(),
           "the live session ends on the golden hash");
    expect(!live.value().durability_state(2) && live.value().durability_state(1)
               && live.value().durability_state(1)->hardpoints[4] == Fixed{} && !live.value().durability_state(5),
           "durability_state reports live durable units only");

    // Without the table the same replay runs: damage is rejected and every unit lives.
    auto blind = tactical::TacticalSession::from_replay(replay.value());
    expect(static_cast<bool>(blind), "the replay runs without a durability table");
    if (!blind) return;
    const auto blind_run = run(std::move(blind).value(), replay.value().final_tick_count, 1, false);
    expect(blind_run && blind_run->published.back()->instances().size() == 5, "no unit dies without a table");
}

} // namespace

int main(const int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: tactical_durability_tests <fixture directory>\n";
        return 2;
    }
    test_validation();
    test_rules();
    test_service();
    test_repair();
    test_fixture(argv[1]);
    if (failures != 0) {
        std::cerr << failures << " durability contract(s) failed\n";
        return 1;
    }
    std::cout << "durability contracts passed\n";
    return 0;
}
