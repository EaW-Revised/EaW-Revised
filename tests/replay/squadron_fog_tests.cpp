#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/fog_cells.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// #271 squadron sensors and #274 retail fog cells (docs/behaviour/space-visibility.md V-03,
// V-11 to V-17). The fixtures stage the FoC debug-build recordings RO-1 (S-91: an X-Wing
// squadron, enemies 300/700/900 away, a member teleported 1500 units, the leader killed, then
// all but one member) and RO-3 (S-93: a Tartan cruiser at a cell centre, targets at
// 1100/1200/1300/1204/1273) and check the recorded outcomes.
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
constexpr tactical::PlayerId rebel = 11;  // service phase 11: see the linger case
constexpr tactical::PlayerId empire = 12;
constexpr tactical::TypeId container_type = 10; // `Team`, REVEAL 800
constexpr tactical::TypeId craft_type = 20;     // X-Wing: no REVEAL, so no profile
constexpr tactical::TypeId enemy_type = 30;     // staged enemies with no sensor of their own
constexpr tactical::TypeId tartan_type = 40;    // REVEAL 1200
constexpr tactical::TypeId corvette_type = 50;  // REVEAL 1000 (the last of 1200, 1000)

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y) { return {units(x), units(y), Fixed{}}; }

[[nodiscard]] std::vector<tactical::SensorProfile> sensors() {
    return {{container_type, units(800)}, {tartan_type, units(1200)}, {corvette_type, units(1000)}};
}

// The craft have a hull so scripted damage (HD-30) can kill them.
[[nodiscard]] tactical::DurabilityTable durability() {
    tactical::DurabilityTable table;
    table.rules = {Fixed::from_raw(one / 5), Fixed::from_raw(one * 2 / 5), Fixed::from_raw(one / 3)};
    table.profiles.push_back(tactical::DurabilityProfile{craft_type, units(90), units(4), false, {}});
    return table;
}

// The RO-1 staging on Coruscant's grid: 130 x 130 cells of 100 from (-6500, 6500).
[[nodiscard]] tactical::FogRules coruscant() {
    tactical::FogRules rules;
    rules.map_left = units(-6500);
    rules.map_top = units(6500);
    rules.cell_size = units(100);
    rules.cells_wide = 130;
    rules.cells_tall = 130;
    rules.ramp_down_step = tactical::fog_ramp_down_step(units(6)).value();
    return rules;
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3& position) {
    return tactical::UnitState{id, type, owner, position, math::identity_quat(), {}};
}

// IDs of the S-91 staging.
constexpr eawr::sim::EntityId squadron = 1;  // the team container
constexpr eawr::sim::EntityId leader = 2;    // member 1
constexpr eawr::sim::EntityId teleported = 6; // member 5
constexpr eawr::sim::EntityId e300 = 7;
constexpr eawr::sim::EntityId e700 = 8;
constexpr eawr::sim::EntityId e900 = 9;
constexpr eawr::sim::EntityId eaway = 10;
constexpr eawr::sim::EntityId efar = 11;  // 600 from the teleported fighter only
constexpr eawr::sim::EntityId lone = 12;  // a fighter without a squadron
constexpr eawr::sim::EntityId enear = 13; // 100 from the lone fighter

[[nodiscard]] tactical::TacticalSetup s91() {
    tactical::TacticalSetup setup;
    setup.seed = 12345;
    setup.players = {{rebel, 0, 1, tactical::player_flag_commandable}, {empire, 1, 2, tactical::player_flag_commandable}};
    // The squadron spawned at (2500, -2500) with its members at their recorded offsets; the
    // fifth member already sits where RO-1 teleported it.
    setup.units = {
        unit(squadron, container_type, rebel, at(2500, -2500)),
        unit(leader, craft_type, rebel, at(2513, -2493)),
        unit(3, craft_type, rebel, at(2512, -2421)),
        unit(4, craft_type, rebel, at(2446, -2465)),
        unit(5, craft_type, rebel, at(2485, -2559)),
        unit(teleported, craft_type, rebel, at(1000, -2500)),
        unit(e300, enemy_type, empire, at(2800, -2500)),
        unit(e700, enemy_type, empire, at(2500, -1800)),
        unit(e900, enemy_type, empire, at(2500, -3400)),
        unit(eaway, enemy_type, empire, at(1300, -2500)),
        unit(efar, enemy_type, empire, at(400, -2500)),
        unit(lone, craft_type, rebel, at(-4000, 4000)),
        unit(enear, enemy_type, empire, at(-3900, 4000)),
    };
    setup.squadrons = {{squadron, {leader, 3, 4, 5, teleported}}};
    return setup;
}

[[nodiscard]] tactical::PlayerCommand kill(const std::uint64_t tick, const std::uint64_t sequence,
    std::vector<eawr::sim::EntityId> targets) {
    return tactical::PlayerCommand{{tick, rebel, sequence}, std::move(targets), tactical::DamagePayload{units(1000)}};
}

[[nodiscard]] bool rebel_sees(const tactical::TacticalSnapshot& snapshot, const eawr::sim::EntityId id) {
    const auto seen = snapshot.visible_entities(rebel);
    return std::binary_search(seen.begin(), seen.end(), id);
}

[[nodiscard]] const tactical::TacticalInstance* instance(const tactical::TacticalSnapshot& snapshot,
    const eawr::sim::EntityId id) {
    for (const auto& entry : snapshot.instances()) {
        if (entry.entity_id == id) return &entry;
    }
    return nullptr;
}

[[nodiscard]] math::Vec3 position(const tactical::TacticalSnapshot& snapshot, const eawr::sim::EntityId id) {
    const auto* found = instance(snapshot, id);
    if (found == nullptr) return {};
    return {found->fixed_transform.rows[0][3], found->fixed_transform.rows[1][3], found->fixed_transform.rows[2][3]};
}

// The S-91 session: the teleport is staged, the leader dies at tick 2, members 2 to 4 at
// tick 3 (retail kills them together at t240) and the last one at tick 300.
[[nodiscard]] tactical::TacticalSession s91_session(const std::optional<tactical::FogRules>& fog) {
    auto session = tactical::TacticalSession::create(s91(), sensors(), durability(), {}, fog).value();
    for (auto&& command : {kill(2, 1, {leader}), kill(3, 2, {3, 4, 5}), kill(300, 3, {teleported})}) {
        expect(static_cast<bool>(session.submit(command)), "S-91 commands submit");
    }
    return session;
}

struct Trace {
    std::vector<std::string> rows; // tick, state hash, snapshot digest
    std::vector<std::shared_ptr<const tactical::TacticalSnapshot>> snapshots;
};

[[nodiscard]] Trace run(tactical::TacticalSession session, const std::uint64_t ticks, const std::size_t workers,
    const bool scramble) {
    if (scramble) session.scramble_storage_for_testing();
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    Trace trace;
    trace.rows.push_back("0," + session.state_sha256() + "," + session.snapshot()->sha256());
    trace.snapshots.push_back(session.snapshot());
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        auto stepped = session.step(executor);
        if (!stepped) {
            expect(false, "step failed: " + stepped.error().message);
            break;
        }
        trace.rows.push_back(std::to_string(stepped.value().completed_tick) + "," + stepped.value().state_sha256 + ","
            + stepped.value().snapshot->sha256());
        trace.snapshots.push_back(stepped.value().snapshot);
    }
    return trace;
}

void test_validation() {
    auto setup = s91();
    expect(static_cast<bool>(tactical::validate_setup(setup)), "the S-91 setup is valid");
    auto missing = setup;
    missing.squadrons[0].container = 99;
    expect(!tactical::validate_setup(missing), "a container must be a setup unit");
    auto foreign = setup;
    foreign.squadrons[0].members.push_back(e300);
    expect(!tactical::validate_setup(foreign), "craft must share the container's owner");
    auto unordered = setup;
    std::swap(unordered.squadrons[0].members[0], unordered.squadrons[0].members[1]);
    expect(!tactical::validate_setup(unordered), "craft IDs must strictly increase");
    auto twice = setup;
    twice.squadrons.push_back({lone, {leader}});
    expect(!tactical::validate_setup(twice), "no craft belongs to two squadrons");
    auto self = setup;
    self.squadrons[0].members.insert(self.squadrons[0].members.begin(), squadron);
    expect(!tactical::validate_setup(self), "a container is not its own craft");
    auto empty = setup;
    empty.squadrons[0].members.clear();
    expect(!tactical::validate_setup(empty), "a squadron lists at least one craft");

    expect(tactical::fog_ramp_down_step(units(6)).value() == 21, "SpaceFOWRegrowTime 6 s ramps 21 per service");
    expect(tactical::fog_ramp_down_step(units(3)).value() == 42, "3 s ramps 42");
    expect(!tactical::fog_ramp_down_step(Fixed{}), "a regrow time must be positive");
    expect(static_cast<bool>(tactical::validate_fog_rules(coruscant())), "the Coruscant grid is valid");
    auto rules = coruscant();
    rules.cell_size = Fixed{};
    expect(!tactical::validate_fog_rules(rules), "a cell must be positive");
    rules = coruscant();
    rules.cells_wide = 0;
    expect(!tactical::validate_fog_rules(rules), "a grid needs cells");
    rules = coruscant();
    rules.ramp_down_step = 0;
    expect(!tactical::validate_fog_rules(rules), "a zero step never regrows");
    rules = coruscant();
    rules.map_left = Fixed::from_raw(std::numeric_limits<std::int64_t>::max() - 10);
    expect(!tactical::validate_fog_rules(rules), "the grid must fit in Q24");
    expect(!tactical::TacticalSession::create(setup, sensors(), durability(), {}, rules), "a session rejects bad fog rules");
}

// Replay v3 carries the squadron table; a replay without squadrons stays v2 byte for byte.
void test_replay() {
    auto session = s91_session(std::nullopt);
    const eawr::platform::ThreadWorkerAdapter executor(1);
    for (int tick = 0; tick < 5; ++tick) static_cast<void>(session.step(executor));
    const auto replay = session.record();
    const auto bytes = tactical::write_replay(replay);
    expect(bytes && tactical::peek_replay_format_version(bytes.value()) == tactical::replay_format_version_squadrons,
           "a replay with squadrons is written as v3");
    if (!bytes) return;
    const auto parsed = tactical::parse_replay(bytes.value());
    expect(parsed && parsed.value() == replay, "v3 round-trips the squadron table");
    auto plain = replay;
    plain.setup.squadrons.clear();
    const auto plain_bytes = tactical::write_replay(plain).value();
    expect(tactical::peek_replay_format_version(plain_bytes) == tactical::replay_format_version,
           "a replay without squadrons stays v2");
    auto relabelled = plain_bytes;
    relabelled[8] = static_cast<std::uint8_t>(tactical::replay_format_version_squadrons);
    expect(!tactical::parse_replay(relabelled), "v3 without squadrons is rejected: one encoding per replay");
    auto truncated = bytes.value();
    truncated.resize(truncated.size() - 1);
    expect(!tactical::parse_replay(truncated), "a truncated v3 replay is rejected");
}

// RO-1 under the exact rules v1 test (V-05 to V-07).
void test_squadron_exact() {
    const auto trace = run(s91_session(std::nullopt), 301, 1, false);
    if (trace.snapshots.size() != 302) return;
    const auto& zero = *trace.snapshots[0];
    expect(rebel_sees(zero, e300) && rebel_sees(zero, e700) && !rebel_sees(zero, e900) && !rebel_sees(zero, eaway),
           "RO-1 tick 0: the squadron sees the enemies at 300 and 700, not 900 (range 800)");
    expect(!rebel_sees(zero, enear), "RO-1: a fighter without a squadron reveals nothing");
    expect(zero.instances()[0].reveal_range == units(800) && !zero.instances()[1].reveal_range,
           "the container carries the sensor; the craft carry none");

    // After the first step the container sits at the bounding-box centre of its craft.
    const auto& first = *trace.snapshots[1];
    expect(position(first, squadron) == math::Vec3{Fixed::from_raw(1756 * one + one / 2), units(-2490), Fixed{}},
           "V-03: the container moves to the per-axis midpoint of its craft (1756.5, -2490)");
    expect(rebel_sees(first, eaway) && !rebel_sees(first, e300),
           "the single 800 circle moved to the midpoint: eaway seen, e300 now 1044 away");
    expect(!rebel_sees(first, efar), "no circle around the teleported fighter itself");

    // Leader death (tick 2): the container keeps its sensor at the survivors' centre.
    const auto& leaderless = *trace.snapshots[3];
    expect(instance(leaderless, leader) == nullptr && instance(leaderless, squadron) != nullptr
               && position(leaderless, squadron) == at(1756, -2490) && rebel_sees(leaderless, eaway),
           "V-03: the leader's death leaves the container and its reveal in place");

    // One member left (tick 3): the container moves onto it.
    const auto& last = *trace.snapshots[4];
    expect(position(last, squadron) == at(1000, -2500) && rebel_sees(last, eaway) && rebel_sees(last, efar),
           "V-03: with one member left the container sits on it");

    // The last member dies (tick 300): the container leaves with it.
    const auto& gone = *trace.snapshots[301];
    expect(instance(gone, teleported) == nullptr && instance(gone, squadron) == nullptr && !rebel_sees(gone, eaway),
           "the container leaves the session with its last craft");
}

// RO-1 and the linger under the retail fog cells (V-11 to V-17).
void test_squadron_cells() {
    const auto trace = run(s91_session(coruscant()), 400, 1, false);
    if (trace.snapshots.size() != 401) return;
    const auto& zero = *trace.snapshots[0];
    expect(rebel_sees(zero, e300) && rebel_sees(zero, e700) && !rebel_sees(zero, e900) && !rebel_sees(zero, eaway),
           "RO-1 cells, tick 0: 300 and 700 seen, 900 fogged (8-cell circle)");
    expect(!rebel_sees(zero, enear), "RO-1 cells: a lone fighter reveals nothing");
    // Tick 1: the container marks its circle at its new cell; eaway is inside it. e300's cell
    // is released and lingers while it regrows.
    expect(rebel_sees(*trace.snapshots[1], eaway) && rebel_sees(*trace.snapshots[1], e300),
           "tick 1: eaway revealed, e300 lingering");
    // V-15: Rebel's grid is serviced on ticks 11, 27, ...; the released cell reaches zero at
    // the 13th service, tick 203, so e300 is seen through completed tick 203 and fogged from
    // 204: 203 ticks after the release, as in S-91 (released t61, fogged t264).
    expect(rebel_sees(*trace.snapshots[203], e300) && !rebel_sees(*trace.snapshots[204], e300),
           "V-15: e300 fogs exactly 203 ticks after its cell was released");
    expect(rebel_sees(*trace.snapshots[203], e700) && !rebel_sees(*trace.snapshots[204], e700),
           "e700's released cell regrows on the same service");
    expect(!rebel_sees(*trace.snapshots[1], efar) && !rebel_sees(*trace.snapshots[3], efar),
           "no cells around the teleported fighter while the container sits at the midpoint");
    // The leader's death moves the centre by half a unit: no re-mark (V-12), reveal kept.
    expect(position(*trace.snapshots[3], squadron) == at(1756, -2490) && rebel_sees(*trace.snapshots[3], eaway),
           "leader death: the container and its cells stay");
    // One member left: the container moves 756 units and re-marks around it.
    expect(rebel_sees(*trace.snapshots[4], efar), "the container re-marks on the last member");
}

// RO-3 (S-93): a Tartan cruiser (1200, radius 12) held at the centre of cell (35, 40).
void test_cell_circle() {
    tactical::TacticalSetup setup;
    setup.players = {{rebel, 0, 1, tactical::player_flag_commandable}, {empire, 1, 2, tactical::player_flag_commandable}};
    const std::int64_t x = -6500 + 35 * 100 + 50;
    const std::int64_t y = 6500 - 40 * 100 - 50;
    setup.units = {
        unit(1, tartan_type, empire, at(x, y)),
        unit(2, enemy_type, rebel, at(x + 1100, y)),     // one cell inside, east
        unit(3, enemy_type, rebel, at(x, y + 1200)),     // the edge cell, north
        unit(4, enemy_type, rebel, at(x - 1300, y)),     // one cell outside, west
        unit(5, enemy_type, rebel, at(x + 900, y - 800)), // 9 east, 8 south: 1204
        unit(6, enemy_type, rebel, at(x - 900, y + 900)), // 9 west, 9 north: 1273
        unit(7, corvette_type, rebel, at(x + 3500, y - 1000)),
    };
    const auto cells = tactical::TacticalSession::create(setup, sensors(), {}, {}, coruscant()).value();
    const auto exact = tactical::TacticalSession::create(setup, sensors()).value();
    const auto sees = [](const tactical::TacticalSession& session, const eawr::sim::EntityId id) {
        const auto seen = session.snapshot()->visible_entities(empire);
        return std::binary_search(seen.begin(), seen.end(), id);
    };
    expect(sees(cells, 2) && sees(cells, 3) && !sees(cells, 4), "RO-3: 1100 seen, 1200 edge seen, 1300 fogged");
    expect(sees(cells, 5) && !sees(cells, 6), "RO-3: the circle reaches 1204 on the diagonal, not 1273");
    expect(sees(exact, 2) && sees(exact, 3) && !sees(exact, 4) && !sees(exact, 5) && !sees(exact, 6),
           "rules v1: the exact 1200 disc misses the 1204 target");
    const auto* grid = cells.fog_cells();
    expect(grid != nullptr, "a bound session exposes its fog cells");
    if (grid == nullptr) return;
    const auto count = [&](const std::size_t player) {
        const auto values = grid->values(player);
        return std::count_if(values.begin(), values.end(), [](const std::uint8_t value) { return value != 0U; });
    };
    expect(count(1) == 489, "RO-3: the Tartan's radius-12 circle is 489 cells, as recorded");
    expect(count(0) == 349, "RO-3: the corvette's radius-10 circle (range 1000, the last authored) is 349 cells");
}

// V-12: a revealer re-marks only after moving at least one cell width.
void test_quantisation() {
    const auto staged = [](const std::int64_t first, const std::int64_t centre) {
        tactical::TacticalSetup setup;
        setup.players = {{rebel, 0, 1, tactical::player_flag_commandable}, {empire, 1, 2, tactical::player_flag_commandable}};
        setup.units = {unit(1, container_type, rebel, at(centre, 0)), unit(2, craft_type, rebel, at(first, 0)),
            unit(3, craft_type, rebel, at(170, 0)), unit(4, enemy_type, empire, at(850, 0))};
        setup.squadrons = {{1, {2, 3}}};
        auto session = tactical::TacticalSession::create(setup, sensors(), durability(), {}, coruscant()).value();
        static_cast<void>(session.submit(kill(0, 1, {3})));
        return run(std::move(session), 400, 1, false);
    };
    // 80 -> -10: 90 units, the circle stays on column 65 and keeps holding column 73.
    const auto held = staged(-10, 80);
    expect(held.snapshots.size() == 401 && rebel_sees(*held.snapshots[400], 4),
           "V-12: a 90-unit move does not re-mark; the enemy stays revealed");
    // 70 -> -30: exactly one cell width re-marks on column 64; column 73 is released.
    const auto moved = staged(-30, 70);
    expect(moved.snapshots.size() == 401 && rebel_sees(*moved.snapshots[203], 4) && !rebel_sees(*moved.snapshots[204], 4),
           "V-12: a 100-unit move re-marks; the released cell regrows as V-15 says");
}

// Records every phase and runs it inline (the phase map in docs/simulation.md).
class PhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }
    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        calls.emplace_back(std::string{}, count);
        return inline_executor.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        calls.emplace_back(std::string(phase), count);
        return inline_executor.execute(count, partition);
    }
    mutable std::vector<std::pair<std::string, std::size_t>> calls;

private:
    eawr::sim::InlineExecutor inline_executor;
};

// The squadron and fog phases run partitioned, named, in order, before the visibility phase.
void test_phase_map() {
    auto session = s91_session(coruscant());
    const PhaseRecorder recorder;
    const std::vector<std::string> expected{"gather", "movement", "targeting", "unit-systems", "squadrons", "fog-reveal", "fog-cells", "visibility"};
    for (int tick = 0; tick < 5; ++tick) {
        recorder.calls.clear();
        expect(static_cast<bool>(session.step(recorder)), "phase map step");
        std::vector<std::string> names;
        for (const auto& [name, partitions] : recorder.calls) {
            const auto expected_partitions = name == "gather" ? 1U : eawr::sim::tick_partition_count;
            expect(partitions == expected_partitions, "phase " + name + " uses its deterministic partition count");
            names.push_back(name);
        }
        expect(names == expected, "tick " + std::to_string(tick) + ": the phase map is gather, movement, targeting, unit-systems, squadrons, "
            "fog-reveal, fog-cells, visibility");
    }
}

// #495 V-19: a flash sets its cell in the target owner's grid alone, unheld, clamped into the
// grid, and the cell regrows like a released one (V-15: fogged at the 13th service).
void test_flash() {
    const std::vector<tactical::Player> players{
        {rebel, 0, 1, tactical::player_flag_commandable}, {empire, 1, 2, tactical::player_flag_commandable}};
    tactical::FogCells cells(coruscant(), players);
    const eawr::sim::InlineExecutor executor;
    const std::vector<tactical::FogFlash> flashes{{empire, at(0, 0)}, {empire, at(99999, 0)}};
    expect(static_cast<bool>(cells.advance(0, true, {}, executor, flashes)), "a flash applies");
    expect(cells.revealed(1, at(0, 0)) && !cells.revealed(0, at(0, 0)), "V-19: only the target owner's grid");
    expect(!cells.revealed(1, at(100, 0)), "V-19: only the shooter's own cell");
    expect(cells.revealed(1, at(6450, 0)), "V-19: a position outside the grid reveals the clamped edge cell");
    std::uint64_t fogged_at = 0;
    for (std::uint64_t tick = 1; tick < 400 && fogged_at == 0; ++tick) {
        expect(static_cast<bool>(cells.advance(tick, true, {}, executor)), "a service applies");
        if (!cells.revealed(1, at(0, 0))) fogged_at = tick;
    }
    // Empire (ID 12) is serviced on ticks 12, 28, ...: 238 at tick 12, fogged at its 13th service.
    expect(fogged_at == 12 + 12 * 16, "V-19: the flashed cell regrows over 13 services (tick 204)");
}

// Worker counts, storage order and replay round trips reproduce every hash and digest.
// Grid-copy budget: snapshots and unchanged ticks share rows, while a one-cell flash
// copies one value row, independently of the number or ordering of workers.
void test_row_copy_budget() {
    auto rules = coruscant();
    rules.cells_wide = 512;
    rules.cells_tall = 512;
    const std::vector<tactical::Player> players{{rebel, 0, 1, tactical::player_flag_commandable},
        {empire, 1, 2, tactical::player_flag_commandable}};
    for (const auto workers : std::vector<std::size_t>{1, 2, 4, 8,
             eawr::platform::ThreadWorkerAdapter::hardware_worker_count()}) {
        tactical::FogCells cells(rules, players);
        const auto initial = cells.value_rows(0);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(static_cast<bool>(cells.advance(11, true, {}, executor)), "empty grid service succeeds");
        expect(cells.copied_grid_bytes() == 0 && cells.value_rows(0) == initial,
            "servicing empty grids copies no cell bytes");
        auto staged = cells;
        expect(staged.value_rows(0) == initial, "staging a grid copies no cell bytes");
        const std::vector<tactical::FogFlash> flashes{{rebel, at(0, 0)}};
        expect(static_cast<bool>(staged.advance(0, false, {}, executor, flashes)), "one-cell flash succeeds");
        expect(staged.copied_grid_bytes() == rules.cells_wide, "one flash copies exactly one value row, no holds");
        const auto flashed = staged.value_rows(0);
        std::size_t changed_rows = 0;
        for (std::size_t row = 0; row < initial.size(); ++row) changed_rows += initial[row] != flashed[row] ? 1U : 0U;
        expect(changed_rows == 1 && cells.value_rows(0) == initial && !cells.revealed(0, at(0, 0)),
            "only the flashed row changes, leaving the committed grid intact");
        expect(staged.value_rows(1) == cells.value_rows(1), "the other player's grid shares every row");
        expect(static_cast<bool>(staged.advance(1, false, {}, executor)), "unchanged tick succeeds");
        expect(staged.copied_grid_bytes() == 0 && staged.value_rows(0) == flashed,
            "retained snapshots and unchanged ticks copy no cell bytes");
        expect(static_cast<bool>(staged.advance(11, true, {}, executor)), "flash regrows on the player's service");
        expect(staged.copied_grid_bytes() == rules.cells_wide, "regrowth copies only the nonzero row");
        expect((*flashed[65])[65] == 255 && (*staged.value_rows(0)[65])[65] == 238,
            "regrowth leaves the earlier published row immutable");
    }
}

class FailAfterPhase final : public eawr::sim::PartitionExecutor {
public:
    explicit FailAfterPhase(const std::string_view phase) : phase_(phase) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 1; }
    [[nodiscard]] eawr::core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        return inline_.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase, const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        auto result = inline_.execute(count, partition);
        if (!result || phase != phase_) return result;
        eawr::core::Diagnostic error;
        error.code = std::string(tactical::diagnostic_codes::worker_failure);
        error.message = "injected failure after fog staging";
        return eawr::core::Result<void>::failure(std::move(error));
    }
private:
    std::string_view phase_;
    eawr::sim::InlineExecutor inline_;
};

void test_staged_fog_failure() {
    const eawr::sim::InlineExecutor executor;
    for (const auto phase : {"fog-cells", "visibility"}) {
        auto session = s91_session(coruscant());
        auto reference = s91_session(coruscant());
        bool injected = false;
        for (int tick = 0; tick < 400; ++tick) {
            const auto expected = reference.step(executor);
            expect(static_cast<bool>(expected), "reference tick succeeds");
            if (reference.fog_cells()->copied_grid_bytes() == 0) {
                expect(static_cast<bool>(session.step(executor)), "pre-failure tick succeeds");
                continue;
            }
            // Locate an actual grid edit, including the squadron's deferred release.
            const auto completed = session.completed_tick();
            const auto hash = session.state_sha256();
            const auto snapshot = session.snapshot();
            const auto rows = session.fog_cells()->value_rows(0);
            expect(!session.step(FailAfterPhase(phase)), "late phase failure is propagated");
            expect(session.completed_tick() == completed && session.state_sha256() == hash && session.snapshot() == snapshot
                && session.fog_cells()->value_rows(0) == rows, "a failed tick commits no fog rows, anchors or snapshot");
            const auto retried = session.step(executor);
            expect(retried && expected && retried.value().state_sha256 == expected.value().state_sha256,
                "retry commits exactly the reference's edited fog state");
            for (int later = 0; later < 250; ++later) {
                const auto actual = session.step(executor);
                const auto next = reference.step(executor);
                expect(actual && next && actual.value().state_sha256 == next.value().state_sha256,
                    "retry preserves later fog hold counts, regrowth, visibility and hashes");
            }
            injected = true;
            break;
        }
        expect(injected, "failure injection exercised a tick that stages changed rows");
    }
}

void test_determinism() {
    for (const auto& fog : {std::optional<tactical::FogRules>{}, std::optional(coruscant())}) {
        const auto label = std::string(fog ? "cells" : "exact");
        const auto reference = run(s91_session(fog), 310, 1, false);
        for (const auto workers : eawr::platform::determinism_worker_counts()) {
            for (const bool scramble : {false, true}) {
                const auto other = run(s91_session(fog), 310, workers, scramble);
                expect(other.rows == reference.rows, label + ": " + std::to_string(workers) + " workers"
                    + (scramble ? ", scrambled" : "") + " reproduce every hash and digest");
            }
        }
        auto session = s91_session(fog);
        const eawr::platform::ThreadWorkerAdapter executor(1);
        for (int tick = 0; tick < 310; ++tick) static_cast<void>(session.step(executor));
        const auto parsed = tactical::parse_replay(tactical::write_replay(session.record()).value());
        expect(parsed.has_value(), label + ": the recording parses");
        if (!parsed) continue;
        auto replayed = tactical::TacticalSession::from_replay(parsed.value(), sensors(), durability(), {}, fog).value();
        const auto again = run(std::move(replayed), 310, 2, false);
        expect(again.rows == reference.rows, label + ": the written replay reproduces the session");
    }
    // Squadrons and fog cells are state; a session without either hashes as before.
    const auto plain = tactical::TacticalSession::create(s91(), sensors(), durability()).value();
    auto bare = s91();
    bare.squadrons.clear();
    const auto without = tactical::TacticalSession::create(bare, sensors(), durability()).value();
    expect(plain.state_sha256() != without.state_sha256(), "squadron membership is hashed");
    const auto fogged = tactical::TacticalSession::create(s91(), sensors(), durability(), {}, coruscant()).value();
    expect(fogged.state_sha256() != plain.state_sha256(), "fog cells are hashed when bound");
}

} // namespace

int main() {
    test_validation();
    test_replay();
    test_squadron_exact();
    test_squadron_cells();
    test_cell_circle();
    test_quantisation();
    test_phase_map();
    test_row_copy_budget();
    test_staged_fog_failure();
    test_flash();
    test_determinism();
    if (failures != 0) {
        std::cerr << failures << " squadron/fog check(s) failed\n";
        return 1;
    }
    std::cout << "squadron sensor and fog cell contracts passed\n";
    return 0;
}
