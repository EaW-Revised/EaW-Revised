#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/victory.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
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

void test_all_units_rules() {
    auto selected = rules({1, 2, 3}, {1});
    selected.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    selected.relevant_types = {ship_type, station_type};
    selected.controlled_players = {1, 2, 3};
    selected.installed_players = {1, 2, 3, 4};
    const std::vector<tactical::Player> players{player(1, 0), player(2, 1), player(3, 0), player(4, 2, false)};
    expect(static_cast<bool>(tactical::validate_victory(selected, players)), "WBF-08: selected all-units rules validate");
    using Counts = std::vector<tactical::OwnerUnitCount>;
    expect(!tactical::all_units_destroyed_winner(selected, players, 2, Counts{{1, 1}, {2, 1}, {3, 1}, {4, 10}}),
        "WBF-35: a surviving enemy relevant ship prevents victory");
    expect(tactical::all_units_destroyed_winner(selected, players, 2, Counts{{1, 1}, {3, 1}, {4, 10}}) == 1U,
        "WBF-32/35: allied units and uncontrolled scenery do not prevent the lowest enemy's win");
    expect(!tactical::all_units_destroyed_winner(selected, players, 1, Counts{{2, 1}, {3, 1}}),
        "WBF-35: all-units has no starbase AI shortcut while a human ally's units remain");
    selected.controlled_players.push_back(4);
    expect(!tactical::all_units_destroyed_winner(selected, players, 2, Counts{{1, 1}, {4, 1}}),
        "WBF-35: an explicitly AI-controlled non-lobby owner still counts");
    selected.controlled_players.pop_back();
    selected.installed_players = {2, 4};
    expect(!tactical::all_units_destroyed_winner(selected, players, 2, Counts{{1, 1}}),
        "WBF-37: an uninstalled condition cannot award victory");
    selected.condition = tactical::VictoryCondition::enemy_starbase_destroyed;
    expect(!tactical::all_units_destroyed_winner(selected, players, 2, {}),
        "WBF-35: starbase-only does not fall back to ship elimination");
    const auto authored = tactical::parse_victory_condition("SKIRMISH_ALL_ENEMY_UNITS_DESTROYED");
    expect(authored && authored.value() == tactical::VictoryCondition::all_enemy_units_destroyed,
        "WBF-08: the authored all-unit selector resolves");
    expect(!tactical::parse_victory_condition("SKIRMISH_COMMAND_HQ_DESTROYED"),
        "WBF-36: unsupported land/shared branches are diagnosed");
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
    // decide nothing; the Empire's star base decides at tick 5; the Rebel's later damage is
    // blocked (VT-09, WCC-40).
    list.push_back({"victory",
        {setup(), 12,
            {damage(2, 1, 0, {5, 6}), damage(3, 1, 1, {4}), damage(5, 1, 2, {3}), damage(8, 1, 3, {1})}},
        1, 5, 3});
    // VF-2 defeat: the Rebel's star base falls at tick 4; the Empire wins.
    list.push_back({"defeat", {setup(), 10, {damage(4, 2, 0, {1})}}, 2, 4, 1});
    // VF-3: the command lists both bases; only the Rebel's (1) falls. Pending victory
    // immediately blocks damage to the Empire's base (VT-02, WCC-40).
    list.push_back({"simultaneous", {setup(), 10, {damage(6, 1, 0, {1, 3})}}, 2, 6, 1});
    // VF-4 simultaneous, other order: two commands in one tick run in (player, sequence) order,
    // so only the Empire's star base (hit by player 1) falls and the Rebel wins.
    list.push_back({"simultaneous-ordered", {setup(), 10, {damage(6, 1, 0, {3}), damage(6, 2, 0, {1})}}, 1, 6, 3});
    // VF-5 undecided: only ships and non-playable objects fall; no outcome.
    list.push_back({"undecided", {setup(), 10, {damage(2, 1, 0, {4, 5, 6}), damage(3, 2, 0, {2})}}, std::nullopt, 0, 0});
    return list;
}

struct Trace {
    std::vector<std::string> rows; // tick,state,snapshot
    std::optional<tactical::BattleOutcome> outcome;
    std::size_t victory_events{};
    std::size_t quit_events{};
    std::vector<tactical::PlayerQuit> quits;
    std::vector<tactical::BattleLoss> losses;
    std::vector<eawr::sim::EntityId> survivors;
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
        for (const auto& event : snapshot.events()) trace.quit_events += event.kind == tactical::EventKind::player_quit;
        expect(snapshot.outcome() == session.outcome(), "the snapshot carries the session's outcome");
    }
    trace.outcome = session.outcome();
    const auto quits = session.snapshot()->quits();
    trace.quits.assign(quits.begin(), quits.end());
    const auto losses = session.snapshot()->losses();
    trace.losses.assign(losses.begin(), losses.end());
    for (const auto& instance : session.snapshot()->instances()) trace.survivors.push_back(instance.entity_id);
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
        if (fixture.name == "simultaneous" || fixture.name == "simultaneous-ordered" || fixture.name == "victory") {
            const auto standing_base = fixture.deciding_unit == 1 ? 3U : 1U;
            expect(std::find(reference.survivors.begin(), reference.survivors.end(), standing_base) != reference.survivors.end(),
                fixture.name + ": WCC-40 keeps the later-hit star base standing");
        }
        for (const std::size_t workers : {std::size_t{1}, std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
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

void test_pending_damage() {
    tactical::WeaponProfile weapon;
    weapon.range = units(500);
    // A sub-frame spawn delay starts both weapons ready; stop orders limit the volley.
    weapon.min_recharge_hundredths = 1;
    weapon.max_recharge_hundredths = 1;
    weapon.category_restrictions = 1;
    weapon.opportunity_when_idle = false;
    weapon.opportunity_when_targeting = true;
    weapon.shot = tactical::ShotProfile{units(3000), tactical::no_type_index, units(100), units(500), true, true, {}};
    tactical::CombatProfile shooter;
    shooter.type_id = ship_type;
    shooter.category_bits = 1;
    shooter.max_attack_distance = units(500);
    shooter.weapons = {weapon};
    tactical::CombatProfile base;
    base.type_id = station_type;
    base.category_bits = 2;
    base.collision = tactical::CollisionBox{{units(-10), units(-10), units(-10)}, {units(10), units(10), units(10)}};
    tactical::CombatTable combat;
    constexpr tactical::TypeId protected_ship_type = 60;
    auto protected_ship = base;
    protected_ship.type_id = protected_ship_type;
    combat.profiles = {shooter, base, protected_ship};
    auto health = durability();
    health.damage = tactical::DamageRules{};
    health.damage->shield_recharge_frames = 90;
    health.profiles[1].max_shields = units(100);
    auto protected_health = health.profiles[0];
    protected_health.type_id = protected_ship_type;
    protected_health.max_shields = units(100);
    health.profiles.push_back(protected_health);
    const std::vector<tactical::SensorProfile> sensors{{ship_type, units(2000)}, {station_type, units(2000)},
        {protected_ship_type, units(2000)}};

    const auto exercise = [&](const bool delayed, const tactical::VictoryRules& selected,
                              const eawr::sim::PartitionExecutor& executor, const bool scramble) {
        auto battle = setup();
        // Shooter 2 launches the lower projectile ID at the Rebel base; shooter 4 aims at the Empire base.
        battle.units = {unit(1, station_type, 1, -100), unit(2, ship_type, 2, -200),
            unit(3, station_type, 2, delayed ? 1000 : 100), unit(4, ship_type, 1, 0)};
        battle.units[2].position.y = units(delayed ? 1000 : 100);
        battle.units[3].position.y = units(100);
        const eawr::sim::EntityId protected_target = delayed ? 5U : 3U;
        if (delayed) {
            battle.units.push_back(unit(5, protected_ship_type, 2, 300));
            battle.units.back().position.y = units(100);
        }
        const tactical::TacticalReplay replay{battle, 8,
            {{{0, 1, 0}, {4}, tactical::AttackPayload{protected_target}}, {{0, 2, 0}, {2}, tactical::AttackPayload{1}},
                {{1, 1, 1}, {4}, tactical::StopPayload{}}, damage(1, 1, 2, {protected_target}, 10),
                {{1, 2, 1}, {2}, tactical::StopPayload{}}, damage(4, 1, 3, {protected_target})}};
        auto created = tactical::TacticalSession::from_replay(replay, sensors, health, {}, {}, combat, selected);
        expect(static_cast<bool>(created), "pending projectile fixture creates");
        std::vector<std::string> hashes;
        if (!created) return hashes;
        auto session = std::move(created).value();
        std::size_t losses = 0;
        bool later_in_flight = false;
        for (std::uint64_t frame = 0; frame < replay.final_tick_count; ++frame) {
            if (scramble) session.scramble_storage_for_testing();
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "pending projectile fixture steps");
            if (!stepped) break;
            hashes.push_back(stepped.value().state_sha256 + ',' + stepped.value().snapshot->sha256());
            const auto& snapshot = *stepped.value().snapshot;
            for (const auto& event : snapshot.events()) losses += event.kind == tactical::EventKind::unit_destroyed;
            if (frame == 0) {
                std::string volley = "two shooters launch in ID order at their opposite targets; actual";
                for (const auto& projectile : session.projectiles()) {
                    volley += " id=" + std::to_string(projectile.id) + " shooter=" + std::to_string(projectile.shooter)
                        + " target=" + std::to_string(projectile.target);
                }
                expect(session.projectiles().size() == 2 && session.projectiles()[0].target == 1
                    && session.projectiles()[1].target == protected_target
                    && session.projectiles()[0].id < session.projectiles()[1].id,
                    volley);
            }
            if (frame == 1 && !delayed) {
                expect(losses == (selected.condition == tactical::VictoryCondition::none ? 2U : 1U),
                    "the two projectiles reach both bases in the same deciding tick");
            }
            if (selected.condition == tactical::VictoryCondition::none) continue;
            if (session.outcome()) {
                expect(session.outcome()->winner == 2 && session.outcome()->deciding_unit == 1
                    && session.outcome()->decided_tick == 1,
                    "VT-02: the first projectile decides for the Empire");
                const auto standing = std::find_if(snapshot.instances().begin(), snapshot.instances().end(),
                    [&](const auto& instance) { return instance.entity_id == protected_target; });
                expect(standing != snapshot.instances().end(), "WCC-40: the later-hit Empire unit stands");
                if (standing != snapshot.instances().end()) {
                    expect(standing->durability && standing->durability->hull == units(delayed ? 600 : 2400)
                        && standing->durability->shields == units(100),
                        "WCC-40: later projectiles and scripted damage leave hull and shields unchanged");
                }
                if (delayed && !session.projectiles().empty()) later_in_flight = true;
            }
        }
        if (selected.condition != tactical::VictoryCondition::none) {
            expect(session.outcome() && losses == 1, "pending victory allows only the deciding station destruction");
            expect(session.projectiles().empty(), "blocked projectiles keep flying and are spent on contact");
            expect(!delayed || later_in_flight, "the countdown case actually has a projectile in flight");
        } else {
            expect(losses == 2, "without victory rules both stations can be damaged and destroyed");
        }
        return hashes;
    };
    const eawr::sim::InlineExecutor inline_executor;
    for (const bool delayed : {false, true}) {
        const auto reference = exercise(delayed, m2_rules(), inline_executor, false);
        exercise(delayed, {}, inline_executor, false);
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(exercise(delayed, m2_rules(), executor, true) == reference,
                "pending damage hashes match at 1/2/4/8 workers with scrambled storage");
        }
    }
}

void test_pending_environment_damage() {
    constexpr tactical::TypeId hero_type = 9;
    constexpr tactical::TypeId protected_type = 60;
    constexpr tactical::TypeId field_type = 70;
    const auto decimal = [](const char* text) { return Fixed::from_decimal(text).value(); };
    for (const bool all_units : {false, true}) {
        const auto relevant_type = all_units ? ship_type : station_type;
        for (const bool routed : {false, true}) {
            for (const bool enabled : {false, true}) {
                std::vector<std::string> reference;
                for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                    for (const bool scramble : {false, true}) {
                        auto battle = setup();
                        battle.units = routed
                            ? std::vector<tactical::UnitState>{unit(1, hero_type, 1, 0), unit(2, container_type, 1, 0),
                                unit(3, relevant_type, 1, 0), unit(5, protected_type, 1, 0),
                                unit(7, relevant_type, 2, 0), unit(10, field_type, 3, 0)}
                            : std::vector<tactical::UnitState>{unit(1, relevant_type, 1, 0),
                                unit(3, relevant_type, 2, 0), unit(10, field_type, 3, 0)};
                        if (routed) battle.squadrons = {{2, {1, 3, 5}}};
                        auto health = durability();
                        health.profiles.insert(health.profiles.begin(), {hero_type, units(600), {}, false, {}});
                        health.profiles.push_back({protected_type, units(600), {}, false, {}});
                        health.damage = tactical::DamageRules{};
                        health.damage->shield_recharge_frames = 30;
                        health.damage->asteroid_damage = units(routed ? 6000 : 3000);
                        health.damage->asteroid_rate = units(1);
                        tactical::MotionTable motion;
                        motion.rules = {units(15), units(300)};
                        motion.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
                            decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"),
                            3500, 6, 90, 45, units(50)};
                        if (routed) {
                            tactical::CraftProfile craft;
                            craft.max_speed = decimal("5.4");
                            craft.min_speed = decimal("1.8");
                            craft.rate_of_turn = craft.lift = craft.roll_rate = units(6);
                            craft.thrust = decimal("0.2");
                            craft.bank_angle = units(70);
                            craft.strafe_distance = units(200);
                            for (const auto type : {hero_type, relevant_type, protected_type}) {
                                craft.type_id = type;
                                motion.squadrons.craft.push_back(craft);
                            }
                            motion.squadrons.squadrons = {{container_type, {hero_type, relevant_type, protected_type},
                                {{}, {}, {}}, units(1000), units(200), units(300), units(20)}};
                        }
                        tactical::Footprint victim;
                        victim.type_id = relevant_type;
                        victim.layer = tactical::SpaceLayer::capital;
                        victim.radius = units(10);
                        victim.asteroid_damage = true;
                        victim.locomotor = false;
                        if (routed) {
                            auto hero = victim;
                            hero.type_id = hero_type;
                            motion.footprints.push_back(hero);
                        }
                        motion.footprints.push_back(victim);
                        tactical::Footprint field;
                        field.type_id = field_type;
                        field.layer = tactical::SpaceLayer::static_object;
                        field.radius = units(100);
                        field.obstacle = true;
                        field.asteroid_field = true;
                        motion.footprints.push_back(field);
                        tactical::CombatTable combat;
                        if (routed) {
                            tactical::CombatProfile hero;
                            hero.type_id = hero_type;
                            hero.redirect_damage_to_teammates = true;
                            combat.profiles.push_back(hero);
                        }
                        tactical::CombatProfile profile;
                        profile.type_id = relevant_type;
                        combat.profiles.push_back(profile);
                        auto selected = m2_rules();
                        if (all_units) {
                            selected.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
                            selected.relevant_types = {ship_type};
                            selected.controlled_players = selected.installed_players = {1, 2};
                        }
                        if (!enabled) selected = {};
                        auto created = tactical::TacticalSession::create(battle, {}, health, motion,
                            std::nullopt, combat, selected);
                        expect(static_cast<bool>(created), "environmental pending-victory fixture validates");
                        if (!created) { std::cerr << created.error().message << '\n'; continue; }
                        auto session = std::move(created).value();
                        const eawr::platform::ThreadWorkerAdapter executor(workers);
                        std::vector<std::string> rows;
                        for (std::size_t frame = 0; frame < 2; ++frame) {
                            if (scramble) session.scramble_storage_for_testing();
                            const auto stepped = session.step(executor);
                            expect(static_cast<bool>(stepped), "environmental pending-victory tick completes");
                            if (!stepped) break;
                            rows.push_back(stepped.value().state_sha256 + stepped.value().snapshot->sha256());
                            if (!enabled) {
                                expect(!session.outcome() && !session.durability_state(routed ? 7 : 3),
                                    "disabled victory rules allow later environmental destruction");
                                if (routed) expect(!session.durability_state(5), "disabled victory permits every routed share");
                                continue;
                            }
                            expect(session.outcome() && session.outcome()->winner == 2
                                && session.outcome()->deciding_unit == (routed ? 3U : 1U)
                                && session.outcome()->decided_tick == 0,
                                "VT-02: first environmental destruction immediately decides victory");
                            const auto standing = session.durability_state(routed ? 7 : 3);
                            expect(standing && standing->hull == units(all_units ? 600 : 2400),
                                "WCC-40: later environmental source leaves the opposing unit unchanged");
                            if (routed) {
                                const auto recipient = session.durability_state(5);
                                expect(recipient && recipient->hull == units(600),
                                    "WCC-40/WHE-64: the next share of the deciding routed hit is blocked");
                            }
                            expect(stepped.value().asteroid_impacts.size() == (frame == 0 ? 1U : 0U),
                                "pending victory publishes only the deciding asteroid impact");
                            expect(stepped.value().snapshot->losses().size() == 1,
                                "the environmental destruction hook records the deciding loss exactly once");
                        }
                        if (reference.empty()) reference = rows;
                        else expect(rows == reference,
                            "environmental damage hashes agree at 1/2/4/8 workers and both storage orders");
                    }
                }
            }
        }
    }
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

void test_all_units_sessions() {
    auto selected = m2_rules();
    selected.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    selected.relevant_types = {ship_type, station_type};
    selected.controlled_players = {1, 2};
    selected.installed_players = {1, 2, 3};
    // The neutral relevant base survives; the enemy ship is the last counted loss.
    const tactical::TacticalReplay battle{setup(), 7,
        {damage(1, 1, 0, {3}), damage(3, 1, 1, {4}), damage(4, 2, 0, {1, 2})}};
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = run(battle, inline_executor, false, selected);
    expect(reference.outcome && reference.outcome->winner == 1 && reference.outcome->decided_tick == 3
        && reference.outcome->deciding_unit == 4 && reference.victory_events == 1,
        "WBF-35/37: final relevant enemy ship decides and later losses cannot replace the winner");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(run(battle, executor, true, selected).rows == reference.rows,
            "all-units hashes agree across workers and scrambled storage");
    }
    auto simultaneous = setup();
    simultaneous.units = {unit(1, ship_type, 1, 0), unit(2, ship_type, 2, 100)};
    const auto first = run({simultaneous, 2, {damage(0, 1, 0, {1, 2})}}, inline_executor, false, selected);
    const auto reversed = run({simultaneous, 2, {damage(0, 1, 0, {2}), damage(0, 2, 0, {1})}}, inline_executor, false, selected);
    expect(first.outcome && first.outcome->winner == 2 && reversed.outcome && reversed.outcome->winner == 1,
        "WBF-32/37: simultaneous last-unit losses retain the first ordered hook's winner");
    auto irrelevant = selected;
    irrelevant.relevant_types = {station_type};
    expect(!run({simultaneous, 2, {damage(0, 1, 0, {2})}}, inline_executor, false, irrelevant).outcome,
        "WBF-31: loss of an irrelevant type does not trigger elimination");
    auto team = setup();
    team.units = {unit(1, ship_type, 1, 0), unit(2, container_type, 2, 100), unit(3, ship_type, 2, 100)};
    team.squadrons = {{2, {3}}};
    selected.relevant_types = {container_type};
    const auto parent = run({team, 2, {damage(0, 1, 0, {3})}}, inline_executor, false, selected);
    expect(parent.outcome && parent.outcome->winner == 1 && parent.outcome->deciding_unit == 2,
        "WBF-31: the relevant team container leaving after its last irrelevant craft decides victory");
}

void test_conversion() {
    auto start = setup();
    start.units = {unit(1, ship_type, 1, 0), unit(2, station_type, 2, 10)};
    tactical::EconomyRules economy;
    economy.players = {{1, units(1000), 0}, {2, units(1000), 0}};
    economy.pads.neutral = 3;
    economy.pads.capture = {{station_type, units(100), Fixed{}, {1, 2, 3}, false, true, true}};
    economy.pads.influence = {{ship_type, true}, {station_type, false}};
    for (const auto condition : {tactical::VictoryCondition::all_enemy_units_destroyed,
             tactical::VictoryCondition::enemy_starbase_destroyed}) {
        std::vector<std::string> reference;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            auto selected = m2_rules();
            selected.condition = condition;
            selected.relevant_types = {ship_type, station_type};
            selected.controlled_players = {1, 2};
            selected.installed_players = {1, 2, 3};
            auto created = tactical::TacticalSession::create(start, {}, durability(), {}, {}, {}, selected, {}, economy);
            expect(static_cast<bool>(created), "ownership-conversion victory fixture validates");
            if (!created) continue;
            auto session = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> rows;
            std::size_t destructions = 0;
            for (std::size_t tick = 0; tick < 12; ++tick) {
                const auto result = session.step(executor);
                expect(static_cast<bool>(result), "ownership-conversion step succeeds");
                if (!result) break;
                rows.push_back(result.value().state_sha256 + result.value().snapshot->sha256());
                for (const auto& event : result.value().snapshot->events()) {
                    destructions += event.kind == tactical::EventKind::unit_destroyed;
                }
            }
            expect(session.outcome() && session.outcome()->winner == 1 && session.outcome()->deciding_unit == 2
                && destructions == 0, "WBF-31: conversion alone reevaluates the old owner's installed elimination condition");
            if (reference.empty()) reference = rows;
            else expect(rows == reference, "ownership conversion hashes agree at 1/2/4/8 workers");
        }
    }
}

void test_elimination_rollback() {
    class VisibilityFailure final : public eawr::sim::PartitionExecutor {
    public:
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            return inline_.execute(count, partition);
        }
        eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
            const std::function<void(std::size_t)>& partition) const override {
            if (phase != "visibility") return execute(count, partition);
            return eawr::core::Result<void>::failure(eawr::core::Diagnostic{
                .code = std::string(tactical::diagnostic_codes::worker_failure),
                .severity = eawr::core::Severity::error,
                .message = "synthetic failure after elimination staging",
                .logical_path = std::nullopt, .line = std::nullopt, .column = std::nullopt,
                .source_id = std::string("victory-test")});
        }
    private:
        eawr::sim::InlineExecutor inline_;
    };
    auto selected = m2_rules();
    selected.condition = tactical::VictoryCondition::all_enemy_units_destroyed;
    selected.relevant_types = {ship_type, station_type};
    selected.controlled_players = {1, 2};
    selected.installed_players = {1, 2, 3};
    const tactical::TacticalReplay battle{setup(), 2, {damage(0, 1, 0, {3, 4})}};
    auto created = tactical::TacticalSession::from_replay(battle, {}, durability(), {}, {}, {}, selected);
    expect(static_cast<bool>(created), "elimination rollback fixture validates");
    if (!created) return;
    auto session = std::move(created).value();
    const auto hash = session.state_sha256();
    const auto snapshot = session.snapshot();
    expect(!session.step(VisibilityFailure{}) && session.completed_tick() == 0 && !session.outcome()
        && session.state_sha256() == hash && session.snapshot() == snapshot,
        "failed tick retains its units, pending commands, outcome and published snapshot");
    const eawr::sim::InlineExecutor executor;
    const auto retried = session.step(executor);
    expect(retried && retried.value().snapshot->losses().size() == 2,
        "WBF-45: retry commits each staged death exactly once after a failed publication");
    const auto reference = run(battle, executor, false, selected);
    expect(retried && reference.outcome && session.outcome() == reference.outcome
        && retried.value().state_sha256 == reference.rows[0].substr(reference.rows[0].find(',') + 1, 64),
        "retry rebuilds elimination counts and preserves the first winner");
    auto quit_created = tactical::TacticalSession::from_replay(
        {setup(), 1, {{{0, 1, 0}, {}, tactical::QuitPayload{}}}}, {}, durability(), {}, {}, {}, selected);
    expect(static_cast<bool>(quit_created), "quit rollback fixture validates");
    if (!quit_created) return;
    auto quit_session = std::move(quit_created).value();
    const auto quit_hash = quit_session.state_sha256();
    expect(!quit_session.step(VisibilityFailure{}) && quit_session.state_sha256() == quit_hash
        && quit_session.snapshot()->quits().empty() && !quit_session.outcome(),
        "failed departure tick records neither quit status nor a fallback outcome");
    expect(quit_session.step(executor) && quit_session.snapshot()->quits().size() == 1
        && quit_session.outcome() && quit_session.outcome()->winner == 2,
        "retry consumes the retained departure exactly once");
}

void test_result_loss_lifetime() {
    const tactical::TacticalReplay battle{setup(), 7, {damage(1, 1, 0, {3}), damage(3, 1, 1, {4})}};
    const eawr::sim::InlineExecutor executor;
    const auto reference = run(battle, executor, false, {});
    expect(reference.losses == std::vector<tactical::BattleLoss>{{2, station_type, 1, 1, 1}, {2, ship_type, 1, 1, 3}},
        "WBF-45/46: lifetime loss history keeps owner, scoring type, final attacker and ordered tick");
    auto created = tactical::TacticalSession::from_replay(battle, {}, durability());
    expect(static_cast<bool>(created), "loss-history sharing fixture validates");
    if (created) {
        auto session = std::move(created).value();
        while (session.completed_tick() < 4) {
            const auto stepped = session.step(executor);
            expect(static_cast<bool>(stepped), "loss-history fixture tick completes");
            if (!stepped) return;
        }
        const auto held = session.snapshot();
        static_cast<void>(session.step(executor));
        expect(session.snapshot()->losses().data() == held->losses().data(),
            "ordinary ticks reuse immutable loss history with no per-history copy");
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter threaded(workers);
        const auto actual = run(battle, threaded, true, {});
        expect(actual.losses == reference.losses && actual.rows == reference.rows,
            "derived loss history and replay hashes agree across workers and scrambled storage");
    }
    auto team = setup();
    team.units = {unit(1, ship_type, 1, 0), unit(2, container_type, 2, 100), unit(3, ship_type, 2, 100)};
    team.squadrons = {{2, {3}}};
    const auto emptied = run({team, 2, {damage(0, 1, 0, {3})}}, executor, false, {});
    expect(emptied.losses == std::vector<tactical::BattleLoss>{{2, ship_type, 1, 1, 0}, {2, container_type, 1, 1, 0}},
        "WBF-45: a last-member death records its squadron scoring type with the final attacker's owner");
}

void test_intentional_quit() {
    const eawr::sim::InlineExecutor executor;
    const auto quit = [](const std::uint64_t tick, const tactical::PlayerId issuer, const std::uint64_t sequence = 0) {
        return tactical::PlayerCommand{{tick, issuer, sequence}, {}, tactical::QuitPayload{}};
    };
    const tactical::TacticalReplay local{setup(), 3, {quit(0, 1)}};
    const auto reference = run(local, executor, false, m2_rules());
    expect(reference.outcome && reference.outcome->condition == tactical::VictoryCondition::intentional_quit
        && reference.outcome->winner == 2 && reference.outcome->end_tick == 1 && reference.outcome->deciding_unit == 0
        && reference.quit_events == 1 && reference.victory_events == 0
        && reference.quits == std::vector<tactical::PlayerQuit>{{1, 0}},
        "WBF-43/48: local intentional quit records status and an immediate enemy result without a destruction");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        expect(run(local, pool, true, m2_rules()).rows == reference.rows,
            "intentional quit hashes agree across workers and scrambled storage");
    }
    const auto opponent = run({setup(), 3, {quit(0, 2)}}, executor, false, m2_rules());
    expect(opponent.outcome && opponent.outcome->winner == 1 && opponent.outcome->end_tick == 1,
        "WBF-48: the last opponent leaving routes the local player to results");
    const auto pending = run({setup(), 4, {damage(0, 1, 0, {3}), quit(1, 1, 1)}}, executor, false, m2_rules());
    expect(pending.outcome && pending.outcome->winner == 2 && pending.outcome->end_tick == 2,
        "WBF-43: quitting before a pending winner matures uses the intentional enemy fallback");
    auto short_countdown = m2_rules();
    short_countdown.countdown_frames = 3;
    const auto matured = run({setup(), 6, {damage(0, 1, 0, {3}), quit(4, 1, 1)}}, executor, false, short_countdown);
    expect(matured.outcome && matured.outcome->winner == 1
        && matured.outcome->condition == tactical::VictoryCondition::enemy_starbase_destroyed
        && matured.outcome->end_tick == 3 && matured.quits.size() == 1,
        "WBF-43: a matured result is retained while intentional status is still recorded");
    const auto duplicate = run({setup(), 3, {quit(0, 1), quit(0, 1, 1), quit(0, 2)}}, executor, false, m2_rules());
    expect(duplicate.outcome && duplicate.outcome->winner == 2 && duplicate.quit_events == 2
        && duplicate.quits.size() == 2,
        "duplicate quit notification is refused and later departure cannot replace the first quit result");
    auto three = setup();
    three.players[2] = player(3, 2);
    auto three_rules = rules({1, 2, 3}, {1});
    const auto departure = run({three, 2, {quit(0, 2)}}, executor, false, three_rules);
    expect(!departure.outcome && departure.quits == std::vector<tactical::PlayerQuit>{{2, 0}},
        "WBF-48: an opponent departure alone leaves a battle with two controlled players running");
    auto created = tactical::TacticalSession::from_replay(
        {setup(), 3, {quit(0, 1), damage(1, 1, 1, {3})}}, {}, durability(), {}, {}, {}, m2_rules());
    expect(static_cast<bool>(created), "departure command-deactivation fixture creates");
    if (created) {
        auto session = std::move(created).value();
        expect(static_cast<bool>(session.step(executor)), "departure tick completes");
        const auto after = session.step(executor);
        expect(after && after.value().snapshot->instances().size() == setup().units.size()
            && after.value().snapshot->events().size() == 1
            && after.value().snapshot->events()[0].kind == tactical::EventKind::order_rejected,
            "after end-frame deactivation, the departed player's next command cannot damage any unit");
    }
    const auto encoded = tactical::write_replay(local);
    const auto parsed = encoded ? tactical::parse_replay(encoded.value())
        : eawr::core::Result<tactical::TacticalReplay>::failure(encoded.error());
    expect(parsed && parsed.value().commands == local.commands && run(parsed.value(), executor, false, m2_rules()).rows == reference.rows,
        "reserved quit opcode round-trips the smallest unitless command and reproduces status/results");
    auto invalid = local;
    invalid.commands[0].units = {1};
    expect(!tactical::write_replay(invalid), "a quit command must list no units");
    invalid = local;
    invalid.commands[0].key.player_id = 3;
    expect(!tactical::write_replay(invalid), "neutral scenery cannot issue an intentional quit");
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
    test_all_units_rules();
    test_fixtures(argv[1], update);
    test_nonplayable_base_remaining();
    test_pending_damage();
    test_pending_environment_damage();
    test_staging();
    test_all_units_sessions();
    test_conversion();
    test_elimination_rollback();
    test_intentional_quit();
    test_result_loss_lifetime();
    if (failures != 0) {
        std::cerr << failures << " victory check(s) failed\n";
        return 1;
    }
    std::cout << "victory contracts passed\n";
    return 0;
}
