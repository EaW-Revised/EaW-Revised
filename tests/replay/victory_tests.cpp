#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// P2-14 (#77): fixed-force victory and defeat (docs/behaviour/space-victory.md). The rule cases
// run the pure evaluation on small line-ups; the session fixtures destroy star bases with scripted
// damage and must decide the same outcome, with the same hashes, at 1, 2 and 4 workers, with
// scrambled storage and through a written-and-parsed replay. Their final hashes are pinned in
// tactical-victory.hashes.csv: `victory_tests <fixtures> --update` rewrites it.
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

constexpr tactical::TypeId ship_type = 10;
constexpr tactical::TypeId station_type = 40;
constexpr tactical::TypeId container_type = 50;

// --- The evaluation ----------------------------------------------------------------------------

[[nodiscard]] tactical::VictoryRules rules(std::vector<tactical::PlayerId> contenders,
    std::vector<tactical::PlayerId> humans) {
    tactical::VictoryRules result;
    result.condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    result.starbase_types = {station_type};
    result.contenders = std::move(contenders);
    result.humans = std::move(humans);
    return result;
}

[[nodiscard]] tactical::Player player(const tactical::PlayerId id, const tactical::TeamId team, const bool lobby = true) {
    return {id, team, id, lobby ? tactical::player_flag_commandable : 0U};
}

using Bases = std::vector<tactical::StarbaseEntry>;

void test_rules() {
    // VC-01: one human against one AI, the M2 line-up.
    const std::vector<tactical::Player> duel{player(1, 0), player(2, 1), player(3, 2, false)};
    const auto m2 = rules({1, 2}, {1});
    expect(tactical::starbase_destroyed_winner(m2, duel, 2, Bases{{1, 1}}) == 1U,
        "VC-01: the AI's star base falls, the human wins (VT-05)");
    expect(tactical::starbase_destroyed_winner(m2, duel, 1, Bases{{3, 2}}) == 2U,
        "VC-01: the human's star base falls, the AI wins (VT-05)");
    // VC-02: non-playable players are no contenders: their loss decides nothing and they never win.
    expect(!tactical::starbase_destroyed_winner(m2, duel, 3, Bases{{1, 1}, {3, 2}}),
        "VC-02: a non-playable player's loss decides nothing (VT-03)");
    expect(!tactical::starbase_destroyed_winner(m2, duel, 3, Bases{{1, 1}}),
        "VC-02: a non-contender's loss decides nothing even when only a contender's base remains");
    expect(!tactical::starbase_destroyed_winner(rules({1, 3}, {1}), duel, 1, Bases{{3, 3}}),
        "VC-02: a non-commandable player cannot win even if caller supplied invalid rules");
    expect(tactical::starbase_destroyed_winner(m2, duel, 1, Bases{{3, 2}, {9, 2}}) == 2U,
        "VC-02: the AI wins while only its own star bases stand");
    // VC-03: free-for-all of a human and two AIs.
    const std::vector<tactical::Player> three{player(1, 0), player(2, 1), player(3, 2)};
    const auto ffa = rules({1, 2, 3}, {1});
    expect(!tactical::starbase_destroyed_winner(ffa, three, 2, Bases{{1, 1}, {3, 3}}),
        "VC-03: with a human's and an AI's star base standing, nobody wins yet (VT-05, VT-06)");
    expect(tactical::starbase_destroyed_winner(ffa, three, 3, Bases{{1, 1}}) == 1U,
        "VC-03: the human wins once every other star base has fallen");
    expect(tactical::starbase_destroyed_winner(ffa, three, 1, Bases{{2, 2}, {3, 3}}) == 2U,
        "VC-03: once the human's star base falls the lowest AI enemy wins at once (VT-06)");
    // VC-04: without humans an AI wins on the first enemy loss (VT-06, the retail rule as read).
    const auto machines = rules({1, 2, 3}, {});
    expect(tactical::starbase_destroyed_winner(machines, three, 2, Bases{{1, 1}, {3, 3}}) == 1U,
        "VC-04: no human: the lowest enemy AI wins on the first loss");
    // VC-05: teams. A star base the candidate does not own counts even when an ally owns it (VT-05).
    const std::vector<tactical::Player> teams{player(1, 0), player(2, 0), player(3, 1), player(4, 1)};
    const auto two_two = rules({1, 2, 3, 4}, {1});
    expect(!tactical::starbase_destroyed_winner(two_two, teams, 4, Bases{{1, 1}, {2, 2}}),
        "VC-05: human and AI ally both standing: neither wins (unverified ally rule, G-V3)");
    expect(!tactical::starbase_destroyed_winner(two_two, teams, 1, Bases{{2, 2}, {3, 3}, {4, 4}}),
        "VC-05: the human's loss decides nothing while the human's AI ally stands (VT-06)");
    expect(tactical::starbase_destroyed_winner(two_two, teams, 2, Bases{{3, 3}, {4, 4}}) == 3U,
        "VC-05: once no star base of the human's team stands the lowest enemy AI wins");
    expect(tactical::starbase_destroyed_winner(two_two, teams, 4, Bases{{1, 1}}) == 1U,
        "VC-05: the human alone standing wins");
    // Rules off: never.
    expect(!tactical::starbase_destroyed_winner(tactical::VictoryRules{}, duel, 2, {}), "no condition, no winner");
}

void test_validation() {
    const std::vector<tactical::Player> duel{player(1, 0), player(2, 1), player(3, 2, false)};
    expect(static_cast<bool>(tactical::validate_victory(rules({1, 2}, {1}), duel)), "the M2 rules are valid");
    expect(static_cast<bool>(tactical::validate_victory({}, duel)), "no rules are valid");
    expect(!tactical::validate_victory(rules({2, 1}, {}), duel), "contenders must increase");
    expect(!tactical::validate_victory(rules({1, 2}, {3}), duel), "a human must be a contender");
    expect(!tactical::validate_victory(rules({1, 5}, {}), duel), "a contender must be a player");
    const auto nonplayable = tactical::validate_victory(rules({1, 3}, {1}), duel);
    expect(!nonplayable && nonplayable.error().code == tactical::diagnostic_codes::invalid_setup
            && nonplayable.error().message.find("3") != std::string::npos,
        "a non-commandable contender is rejected with a diagnostic naming the player");
    auto types = rules({1, 2}, {});
    types.starbase_types = {40, 40};
    expect(!tactical::validate_victory(types, duel), "star base types must strictly increase");
    auto late = rules({1, 2}, {});
    late.countdown_frames = static_cast<std::uint32_t>(tactical::max_ticks + 1);
    expect(!tactical::validate_victory(late, duel), "the countdown stays within the tick limit");
    tactical::TacticalSetup setup;
    setup.players = duel;
    expect(!tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, rules({1, 4}, {})),
        "a session rejects rules naming an unknown player");
    expect(!tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, rules({1, 3}, {1})),
        "a session rejects rules naming a non-commandable contender");
}

// --- Session fixtures --------------------------------------------------------------------------

[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_decimal("0.2").value(), Fixed::from_decimal("0.4").value(),
        Fixed::from_decimal("0.33").value()};
    table.profiles = {{ship_type, units(600), std::nullopt, false, {}}, {station_type, units(2400), std::nullopt, false, {}},
        {container_type, units(20), std::nullopt, false, {}}};
    return table;
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const std::int64_t x) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = {units(x), Fixed{}, Fixed{}};
    return state;
}

// The M2 shape: 1 Rebel human (team 0), 2 Empire AI (team 1), 3 Pirates (no command flag, its own
// team). Each lobby player has a star base and a ship; the Pirates own a star base type and a
// container, which never count.
[[nodiscard]] tactical::TacticalSetup setup() {
    tactical::TacticalSetup result;
    result.seed = 77;
    result.players = {player(1, 0), player(2, 1), player(3, 2, false)};
    result.units = {unit(1, station_type, 1, -3000), unit(2, ship_type, 1, -2500), unit(3, station_type, 2, 3000),
        unit(4, ship_type, 2, 2500), unit(5, station_type, 3, 0), unit(6, container_type, 3, 100)};
    return result;
}

[[nodiscard]] tactical::VictoryRules m2_rules() { return rules({1, 2}, {1}); }

[[nodiscard]] tactical::PlayerCommand damage(const std::uint64_t tick, const tactical::PlayerId issuer,
    const std::uint64_t sequence, std::vector<eawr::sim::EntityId> targets, const std::int64_t amount = 3000) {
    return {{tick, issuer, sequence}, std::move(targets), tactical::DamagePayload{units(amount)}};
}

struct Fixture {
    std::string name;
    tactical::TacticalReplay replay;
    std::optional<tactical::PlayerId> winner;
    std::uint64_t decided_tick{};
    eawr::sim::EntityId deciding_unit{};
};

[[nodiscard]] std::vector<Fixture> fixtures() {
    std::vector<Fixture> list;
    // VF-1 victory: the Pirates' star base and container, then the Empire's ship, fall first and
    // decide nothing; the Empire's star base decides at tick 5; the Rebel's loss later changes
    // nothing (VT-09).
    list.push_back({"victory",
        {setup(), 12,
            {damage(2, 1, 0, {5, 6}), damage(3, 1, 1, {4}), damage(5, 1, 2, {3}), damage(8, 1, 3, {1})}},
        1, 5, 3});
    // VF-2 defeat: the Rebel's star base falls at tick 4; the Empire wins.
    list.push_back({"defeat", {setup(), 10, {damage(4, 2, 0, {1})}}, 2, 4, 1});
    // VF-3 simultaneous: both star bases fall in one command. Its units are hit in list order, so
    // the Rebel's (1) is destroyed first and the Empire wins (VT-08, VT-09).
    list.push_back({"simultaneous", {setup(), 10, {damage(6, 1, 0, {1, 3})}}, 2, 6, 1});
    // VF-4 simultaneous, other order: two commands in one tick run in (player, sequence) order,
    // so the Empire's star base (hit by player 1) falls first and the Rebel wins.
    list.push_back({"simultaneous-ordered", {setup(), 10, {damage(6, 1, 0, {3}), damage(6, 2, 0, {1})}}, 1, 6, 3});
    // VF-5 undecided: only ships and non-playable objects fall; no outcome.
    list.push_back({"undecided", {setup(), 10, {damage(2, 1, 0, {4, 5, 6}), damage(3, 2, 0, {2})}}, std::nullopt, 0, 0});
    return list;
}

struct Trace {
    std::vector<std::string> rows; // tick,state,snapshot
    std::optional<tactical::BattleOutcome> outcome;
    std::size_t victory_events{};
};

[[nodiscard]] Trace run(const tactical::TacticalReplay& replay, const eawr::sim::PartitionExecutor& executor,
    const bool scramble, const tactical::VictoryRules& victory) {
    Trace trace;
    auto created = tactical::TacticalSession::from_replay(replay, {}, durability(), {}, std::nullopt, {}, victory);
    expect(static_cast<bool>(created), "victory session is created");
    if (!created) return trace;
    auto session = std::move(created).value();
    while (session.completed_tick() < replay.final_tick_count) {
        if (scramble) session.scramble_storage_for_testing();
        auto stepped = session.step(executor);
        expect(static_cast<bool>(stepped), "victory step succeeds");
        if (!stepped) break;
        const auto& snapshot = *stepped.value().snapshot;
        trace.rows.push_back(std::to_string(stepped.value().completed_tick) + ',' + stepped.value().state_sha256 + ','
            + snapshot.sha256());
        for (const auto& event : snapshot.events()) trace.victory_events += event.kind == tactical::EventKind::victory;
        expect(snapshot.outcome() == session.outcome(), "the snapshot carries the session's outcome");
    }
    trace.outcome = session.outcome();
    return trace;
}

void test_fixtures(const std::filesystem::path& directory, const bool update) {
    const eawr::sim::InlineExecutor inline_executor;
    std::vector<std::string> finals;
    for (const auto& fixture : fixtures()) {
        const auto reference = run(fixture.replay, inline_executor, false, m2_rules());
        const auto& outcome = reference.outcome;
        if (fixture.winner) {
            expect(outcome && outcome->winner == *fixture.winner && outcome->decided_tick == fixture.decided_tick
                    && outcome->deciding_unit == fixture.deciding_unit
                    && outcome->end_tick == fixture.decided_tick + tactical::victory_countdown_frames
                    && outcome->condition == tactical::VictoryCondition::enemy_starbase_destroyed,
                fixture.name + ": the pinned winner, tick and star base");
            expect(outcome && outcome->winner != 3U, fixture.name + ": a non-playable player never wins");
            expect(reference.victory_events == 1, fixture.name + ": exactly one victory event");
        } else {
            expect(!outcome && reference.victory_events == 0, fixture.name + ": no outcome");
        }
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(run(fixture.replay, executor, false, m2_rules()).rows == reference.rows,
                fixture.name + ": " + std::to_string(workers) + " workers match");
        }
        const eawr::platform::ThreadWorkerAdapter four(4);
        expect(run(fixture.replay, four, true, m2_rules()).rows == reference.rows, fixture.name + ": scrambled storage matches");
        bool reparsed = false;
        if (const auto written = tactical::write_replay(fixture.replay)) {
            const auto parsed = tactical::parse_replay(written.value());
            reparsed = parsed && run(parsed.value(), inline_executor, false, m2_rules()).rows == reference.rows;
        }
        expect(reparsed, fixture.name + ": a written-and-parsed replay matches");
        // Old hashes stay: before the outcome (and throughout an undecided battle) the state and
        // snapshot hashes equal those of a session without victory rules.
        const auto blind = run(fixture.replay, inline_executor, false, tactical::VictoryRules{});
        expect(!blind.outcome && blind.victory_events == 0, fixture.name + ": without rules nothing is decided");
        const auto undecided = fixture.winner ? fixture.decided_tick : fixture.replay.final_tick_count;
        for (std::size_t index = 0; index < undecided && index < reference.rows.size(); ++index) {
            expect(blind.rows[index] == reference.rows[index],
                fixture.name + ": tick " + std::to_string(index + 1) + " hashes as before #77");
        }
        if (fixture.winner && fixture.decided_tick < reference.rows.size()) {
            expect(blind.rows[fixture.decided_tick] != reference.rows[fixture.decided_tick],
                fixture.name + ": the deciding tick's hashes include the outcome");
        }
        if (!reference.rows.empty()) finals.push_back(fixture.name + ',' + reference.rows.back());
    }
    const auto golden = directory / "tactical-victory.hashes.csv";
    if (update) {
        std::ofstream output(golden, std::ios::binary);
        output << "fixture,tick,state_sha256,snapshot_sha256\n";
        for (const auto& row : finals) output << row << '\n';
        std::cout << "wrote " << golden.string() << '\n';
        return;
    }
    std::ifstream input(golden, std::ios::binary);
    std::string line;
    std::getline(input, line);
    std::vector<std::string> pinned;
    while (std::getline(input, line)) pinned.push_back(line);
    expect(pinned == finals, "the fixtures' final hashes match tactical-victory.hashes.csv");
}

void test_nonplayable_base_remaining() {
    auto battle = setup();
    battle.units = {unit(1, station_type, 1, -3000), unit(3, station_type, 3, 3000)};
    const tactical::TacticalReplay replay{battle, 2, {damage(0, 1, 0, {1})}};
    const eawr::sim::InlineExecutor executor;
    const auto trace = run(replay, executor, false, rules({1}, {1}));
    expect(!trace.outcome && trace.victory_events == 0,
        "destroying the last contender base leaves only a non-playable base and awards no winner");
}

// VT-12: a staged removal is the recorder's deletion, not a destruction.
void test_staging() {
    auto created = tactical::TacticalSession::create(setup(), {}, durability(), {}, std::nullopt, {}, m2_rules());
    expect(static_cast<bool>(created), "staging session is created");
    if (!created) return;
    auto session = std::move(created).value();
    expect(static_cast<bool>(session.stage_remove(3)), "the Empire star base is removed by staging");
    const eawr::sim::InlineExecutor executor;
    expect(session.step(executor) && !session.outcome(), "VT-12: a staged removal decides nothing");
    // A staged star base counts: a second Rebel star base keeps the battle open when the first
    // falls, and its own loss lets the Empire win.
    const auto spawned = session.stage_spawn(unit(0, station_type, 1, -3500));
    expect(static_cast<bool>(spawned), "a star base is staged");
    if (!spawned) return;
    expect(static_cast<bool>(session.submit(damage(1, 2, 0, {1}))), "the first Rebel star base is hit");
    expect(session.step(executor) && !session.outcome(), "the staged Rebel star base still stands: undecided");
    expect(static_cast<bool>(session.submit(damage(2, 2, 1, {spawned.value()}))), "the staged star base is hit");
    expect(session.step(executor) && session.outcome() && session.outcome()->winner == 2U
            && session.outcome()->deciding_unit == spawned.value(),
        "a staged star base counts like a starting one");
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: victory_tests <fixtures directory> [--update]\n";
        return 2;
    }
    const bool update = argc > 2 && std::string_view(argv[2]) == "--update";
    test_rules();
    test_validation();
    test_fixtures(argv[1], update);
    test_nonplayable_base_remaining();
    test_staging();
    if (failures != 0) {
        std::cerr << failures << " victory check(s) failed\n";
        return 1;
    }
    std::cout << "victory contracts passed\n";
    return 0;
}
