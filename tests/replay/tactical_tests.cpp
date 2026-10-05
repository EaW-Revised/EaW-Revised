#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class FailingExecutor final : public eawr::sim::PartitionExecutor {
public:
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 2; }

    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t,
        const std::function<void(std::size_t)>&) const override {
        return eawr::core::Result<void>::failure(eawr::core::Diagnostic{
            .code = std::string(tactical::diagnostic_codes::worker_failure),
            .severity = eawr::core::Severity::error,
            .message = "synthetic executor failure",
            .logical_path = std::nullopt,
            .line = std::nullopt,
            .column = std::nullopt,
            .source_id = std::string("tactical-test"),
        });
    }
};

// Reports any worker count and runs the partitions inline, last partition first.
class ReverseInline final : public eawr::sim::PartitionExecutor {
public:
    explicit ReverseInline(const std::size_t workers) : workers_(workers) {}

    [[nodiscard]] std::size_t worker_count() const noexcept override { return workers_; }

    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        for (std::size_t index = count; index != 0; --index) {
            partition(index - 1);
        }
        return eawr::core::Result<void>::success();
    }

private:
    std::size_t workers_;
};

// Records every phase a tick hands to the executor, named or not.
class PhaseRecorder final : public eawr::sim::PartitionExecutor {
public:
    struct Call {
        std::string phase; // empty for an unnamed execute()
        std::size_t partitions{};
    };

    [[nodiscard]] std::size_t worker_count() const noexcept override { return 4; }

    [[nodiscard]] eawr::core::Result<void> execute(
        const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        calls.push_back(Call{{}, count});
        return inline_executor.execute(count, partition);
    }

    [[nodiscard]] eawr::core::Result<void> execute_phase(
        const std::string_view phase,
        const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        calls.push_back(Call{std::string(phase), count});
        return inline_executor.execute(count, partition);
    }

    mutable std::vector<Call> calls;

private:
    eawr::sim::InlineExecutor inline_executor;
};

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// Golden CSV rows after the header; tolerant of a CRLF checkout.
[[nodiscard]] std::vector<std::string> read_rows(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<std::string> rows;
    std::string line;
    bool header = true;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (header) {
            header = false;
            continue;
        }
        rows.push_back(line);
    }
    return rows;
}

[[nodiscard]] std::string event_row(const tactical::Event& event) {
    std::ostringstream row;
    row << event.tick << ',' << event.player << ',' << event.sequence << ',' << event.unit << ','
        << tactical::to_string(event.kind) << ',' << tactical::to_string(event.order) << ','
        << tactical::to_string(event.reason);
    return row.str();
}

struct Golden {
    std::vector<std::string> hashes;
    std::vector<std::string> snapshots;
    std::vector<std::string> events;
};

struct RunOutput {
    std::vector<std::string> hashes;
    std::vector<std::string> snapshots;
    std::vector<std::string> events;
    std::vector<std::string> warnings;
    friend bool operator==(const RunOutput&, const RunOutput&) = default;
};

void record_initial(RunOutput& output, const tactical::TacticalSession& session) {
    output.snapshots.push_back(std::to_string(session.completed_tick()) + "," + session.snapshot()->sha256());
}

void record_tick(RunOutput& output, const tactical::TacticalTick& tick, const bool hashed_off_thread) {
    // #637: off the stepping thread the tick's hash is only in state_hash; on it, in both.
    expect(hashed_off_thread ? tick.state_sha256.empty() : tick.state_hash.get() == tick.state_sha256,
        "the tick's hash is where the hashing mode puts it");
    output.hashes.push_back(std::to_string(tick.completed_tick) + "," + tick.state_hash.get());
    output.snapshots.push_back(std::to_string(tick.completed_tick) + "," + tick.snapshot->sha256());
    for (const auto& event : tick.snapshot->events()) {
        output.events.push_back(event_row(event));
    }
    for (const auto& diagnostic : tick.diagnostics) {
        output.warnings.push_back(diagnostic.code);
    }
}

[[nodiscard]] RunOutput run_replay(
    const tactical::TacticalReplay& replay,
    const eawr::sim::PartitionExecutor& executor,
    const bool scramble,
    std::shared_ptr<eawr::sim::StateHasher> hasher = nullptr,
    const tactical::EconomyRules& economy = {}) {
    RunOutput output;
    auto created = tactical::TacticalSession::from_replay(replay, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
    expect(static_cast<bool>(created), "fixture session is created from replay");
    if (!created) {
        return output;
    }
    auto session = std::move(created).value();
    const bool hashed_off_thread = hasher != nullptr;
    session.set_state_hasher(std::move(hasher));
    record_initial(output, session);
    while (session.completed_tick() < replay.final_tick_count) {
        if (scramble) {
            session.scramble_storage_for_testing();
        }
        const auto tick = session.step(executor);
        expect(static_cast<bool>(tick), "fixture tick executes");
        if (!tick) {
            break;
        }
        record_tick(output, tick.value(), hashed_off_thread);
    }
    return output;
}

[[nodiscard]] tactical::TacticalReplay load_fixture(const std::string& fixtures) {
    const auto path = fixtures + "/tactical-v2.eawr-replay";
    const auto parsed = tactical::parse_replay(read_bytes(path), path);
    expect(static_cast<bool>(parsed), "tactical fixture parses");
    return parsed ? parsed.value() : tactical::TacticalReplay{};
}

void test_format(const std::string& fixtures) {
    const auto bytes = read_bytes(fixtures + "/tactical-v2.eawr-replay");
    const auto v1_bytes = read_bytes(fixtures + "/original-v1.eawr-replay");
    expect(tactical::peek_replay_format_version(bytes) == 2, "v2 fixture reports format version 2");
    expect(tactical::peek_replay_format_version(v1_bytes) == 1, "v1 fixture reports format version 1");
    expect(!tactical::peek_replay_format_version(std::vector<std::uint8_t>{'E', 'A', 'W', 'R'}),
        "short input reports no format version");

    const auto parsed = tactical::parse_replay(bytes, "tactical-v2");
    expect(static_cast<bool>(parsed), "v2 fixture parses");
    if (parsed) {
        const auto& replay = parsed.value();
        expect(replay.setup.players.size() == 3 && replay.setup.units.size() == 9
                && replay.commands.size() == 10 && replay.final_tick_count == 6,
            "v2 fixture table sizes");
        const auto written = tactical::write_replay(replay);
        expect(written && written.value() == bytes, "write(parse(v2 fixture)) preserves exact bytes");
    }
    expect(bytes[24] == tactical::tick_numerator && bytes[28] == tactical::tick_denominator
            && tactical::logical_frames_per_second == 30,
        "replay-v2 header carries the pinned 1/30 s tick");

    const auto v1_as_v2 = tactical::parse_replay(v1_bytes, "original-v1");
    expect(!v1_as_v2 && v1_as_v2.error().code == tactical::diagnostic_codes::version,
        "v2 reader rejects a v1 file as an unsupported version");
    const auto v2_as_v1 = eawr::sim::parse_replay(bytes, "tactical-v2");
    expect(!v2_as_v1 && v2_as_v1.error().code == eawr::sim::diagnostic_codes::replay_version,
        "v1 reader keeps rejecting format version 2");

    constexpr std::array mutations{
        std::pair{"count", tactical::diagnostic_codes::malformed},
        std::pair{"issuer", tactical::diagnostic_codes::invalid_issuer},
        std::pair{"length", tactical::diagnostic_codes::malformed},
        std::pair{"order", tactical::diagnostic_codes::order},
        std::pair{"owner", tactical::diagnostic_codes::invalid_setup},
        std::pair{"player-flags", tactical::diagnostic_codes::version},
        std::pair{"rotation", tactical::diagnostic_codes::invalid_setup},
        std::pair{"rules", tactical::diagnostic_codes::version},
        std::pair{"tick-rate", tactical::diagnostic_codes::version},
        std::pair{"trailing", tactical::diagnostic_codes::malformed},
        std::pair{"unit-order", tactical::diagnostic_codes::invalid_command},
    };
    for (const auto& [name, code] : mutations) {
        const auto file = std::string("tactical-v2-mutated-") + name + ".eawr-replay";
        const auto result = tactical::parse_replay(read_bytes(fixtures + "/" + file), file);
        expect(!result && result.error().code == code, "mutation rejected with its code: " + file);
    }
    auto truncated = bytes;
    truncated.resize(tactical::replay_header_size - 1);
    const auto truncated_result = tactical::parse_replay(truncated, "truncated");
    expect(!truncated_result && truncated_result.error().code == tactical::diagnostic_codes::malformed,
        "truncated header is malformed");
}

void test_match_policy_format(const std::string& fixtures) {
    const auto legacy_bytes = read_bytes(fixtures + "/tactical-v2.eawr-replay");
    const auto legacy = load_fixture(fixtures);
    const auto reference = run_replay(legacy, eawr::sim::InlineExecutor{}, false);
    for (std::uint32_t flags = 1; flags <= 15; ++flags) {
        auto replay = legacy;
        replay.setup.match_policy = tactical::SkirmishMatchPolicy{
            (flags & 1U) == 0, (flags & 2U) == 0, (flags & 4U) == 0, (flags & 8U) == 0};
        // Independent byte construction from the unchanged v2 oracle fixture.
        auto expected = legacy_bytes;
        expected[8] = 4;
        expected[10] = 116;
        const std::array<std::uint8_t, 12> extension{
            1, 0, 0, 0, 1, 0, 4, 0, static_cast<std::uint8_t>(flags), 0, 0, 0};
        expected.insert(expected.begin() + 104, extension.begin(), extension.end());
        const auto written = tactical::write_replay(replay);
        expect(written && written.value() == expected, "v4 tagged policy bytes match independent encoding");
        const auto parsed = tactical::parse_replay(expected);
        expect(parsed && parsed.value() == replay, "every non-default policy combination round-trips");
        const auto wrong = tactical::TacticalSession::from_replay(replay);
        expect(!wrong && wrong.error().message.find("match policy") != std::string::npos,
            "default content cannot reproduce a non-default policy silently");
        tactical::EconomyRules bound;
        bound.match_policy = *replay.setup.match_policy;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            expect(run_replay(replay, ReverseInline(workers), true, nullptr, bound) == reference,
                "policy binding preserves the command stream, hashes and snapshots across workers");
        }
        auto session = tactical::TacticalSession::from_replay(replay, {}, {}, {}, std::nullopt, {}, {}, {}, bound);
        if (session) {
            while (session.value().completed_tick() < replay.final_tick_count) {
                if (!session.value().step(eawr::sim::InlineExecutor{})) break;
            }
            const auto recorded = tactical::write_replay(session.value().record());
            expect(recorded && recorded.value() == expected, "a reproduced policy replay records byte-for-byte");
        }
    }
    auto replay = legacy;
    replay.setup.match_policy = tactical::SkirmishMatchPolicy{false, true, true, true};
    const auto written = tactical::write_replay(replay);
    if (!written) return;
    const auto rejects = [&](const std::size_t offset, const std::uint8_t value, const std::string_view label) {
        auto bytes = written.value();
        bytes[offset] = value;
        expect(!tactical::parse_replay(bytes), label);
    };
    rejects(104, 0, "empty extension list rejected");
    rejects(104, 255, "oversized extension count rejected before allocation");
    rejects(108, 99, "unknown header extension tag rejected");
    rejects(110, 8, "policy body length rejected");
    rejects(112, 0, "redundant all-enabled v4 policy rejected");
    rejects(112, 16, "unsupported policy flag rejected");
    rejects(10, 104, "extension outside declared header rejected");
    auto truncated = written.value(); truncated.resize(115);
    expect(!tactical::parse_replay(truncated), "truncated extension rejected");
    auto duplicate = written.value();
    duplicate[10] = 124; duplicate[104] = 2;
    duplicate.insert(duplicate.begin() + 116, written.value().begin() + 108, written.value().begin() + 116);
    expect(!tactical::parse_replay(duplicate), "duplicate policy record rejected");
    replay.setup.match_policy = tactical::SkirmishMatchPolicy{};
    expect(!tactical::write_replay(replay), "all-enabled policy has only the legacy encoding");
    replay.setup.match_policy = tactical::SkirmishMatchPolicy{true, false, true, true};
    replay.setup.players = {{1, 0, 1, tactical::player_flag_commandable}};
    replay.setup.units = {{1, 101, 1}, {2, 102, 1}};
    replay.setup.squadrons = {{1, {2}}};
    replay.commands.clear();
    const auto squadron_bytes = tactical::write_replay(replay);
    const auto squadron = squadron_bytes ? tactical::parse_replay(squadron_bytes.value())
        : eawr::core::Result<tactical::TacticalReplay>::failure(squadron_bytes.error());
    expect(squadron_bytes && squadron_bytes.value()[8] == 5 && squadron && squadron.value() == replay,
        "v5 retains the squadron table beside tagged policy records");
}

void test_skirmish_setup_format(const std::string& fixtures) {
    const auto legacy = load_fixture(fixtures);
    auto replay = legacy;
    tactical::ReplaySkirmishSetup metadata;
    metadata.map = "data/art/maps/recorded.ted";
    metadata.map_sha256 = std::string(64, 'a');
    metadata.victory_condition = 2;
    metadata.match.credits = eawr::sim::math::Fixed::from_raw(123);
    metadata.match.start_tech = 3;
    metadata.match.max_tech = 5;
    metadata.match.game_timer = -7;
    metadata.match.win_integer = 11;
    metadata.match.auto_resolve = -9;
    metadata.match.win_float = eawr::sim::math::Fixed::from_raw(-456);
    metadata.match.allow_random_events = true;
    metadata.match.space_win_condition = "SKIRMISH_ALL_ENEMY_UNITS_DESTROYED";
    for (const auto& player : replay.setup.players) if (player.commandable())
        metadata.slots.push_back({player.player_id, player.player_id != 1U, 2U, {"Test_Fleet"}});
    replay.setup.skirmish = metadata;
    const auto written = tactical::write_replay(replay);
    expect(written && written.value()[8] == 4, "SKSU alone selects v4 without a redundant default policy");
    if (!written) return;
    const auto parsed = tactical::parse_replay(written.value());
    expect(parsed && parsed.value() == replay, "SKSU round-trips roles, map, palette, fleet and every match field");
    const auto reference = run_replay(legacy, eawr::sim::InlineExecutor{}, false);
    for (const auto workers : {1U, 2U, 4U, 8U})
        expect(run_replay(replay, ReverseInline(workers), true) == reference,
            "load-time metadata does not change canonical hashes, snapshots or events across workers");
    auto session = tactical::TacticalSession::from_replay(replay);
    if (session) {
        while (session.value().completed_tick() < replay.final_tick_count)
            if (!session.value().step(eawr::sim::InlineExecutor{})) break;
        const auto recorded = tactical::write_replay(session.value().record());
        expect(recorded && recorded.value() == written.value(), "session recording preserves SKSU bytes");
    }
    const auto rejects = [&](const std::size_t offset, const std::uint8_t value, const std::string_view label) {
        auto bytes = written.value(); bytes[offset] = value;
        expect(!tactical::parse_replay(bytes), label);
    };
    const std::size_t flags = 112U + 2U + metadata.map.size() + 2U + metadata.map_sha256.size();
    rejects(112, 255, "oversized SKSU string rejects before allocation");
    rejects(flags, 32, "unknown SKSU match flag rejects");
    rejects(flags + 16U, 127, "out-of-range signed tech value rejects");
    const std::size_t victory = flags + 60U + 2U + metadata.match.win_condition.size()
        + 2U + metadata.match.space_win_condition.size();
    rejects(victory, 3, "non-lobby victory condition rejects");
    rejects(victory + 4U, 65, "oversized SKSU slot count rejects");
    rejects(victory + 12U, 2, "non-boolean role rejects");
    auto truncated = written.value(); truncated.resize(115);
    expect(!tactical::parse_replay(truncated), "truncated SKSU rejects");
    const auto header = static_cast<std::size_t>(written.value()[10]) + 256U * written.value()[11];
    auto duplicate = written.value();
    std::vector<std::uint8_t> record(duplicate.begin() + 108, duplicate.begin() + static_cast<std::ptrdiff_t>(header));
    duplicate.insert(duplicate.begin() + static_cast<std::ptrdiff_t>(header), record.begin(), record.end());
    duplicate[104] = 2;
    const auto larger = header + record.size();
    duplicate[10] = static_cast<std::uint8_t>(larger); duplicate[11] = static_cast<std::uint8_t>(larger >> 8U);
    expect(!tactical::parse_replay(duplicate), "duplicate SKSU rejects");
    auto invalid = replay;
    invalid.setup.skirmish->slots.push_back(metadata.slots.front());
    expect(!tactical::write_replay(invalid), "extra or duplicate recorded player rejects");
    invalid = replay; invalid.setup.skirmish->map = "../unrecorded.ted";
    expect(!tactical::write_replay(invalid), "nonlogical map path rejects");
    replay.setup.match_policy = tactical::SkirmishMatchPolicy{false, true, true, true};
    expect(!tactical::write_replay(replay), "contradictory SKSU and policy reject");
    replay.setup.skirmish->match.allow_heroes = false;
    replay.setup.units = {{1, 101, replay.setup.players.front().player_id}, {2, 102, replay.setup.players.front().player_id}};
    replay.setup.squadrons = {{1, {2}}}; replay.commands.clear();
    const auto both = tactical::write_replay(replay);
    const auto squadron = both ? tactical::parse_replay(both.value())
        : eawr::core::Result<tactical::TacticalReplay>::failure(both.error());
    expect(both && both.value()[8] == 5 && squadron && squadron.value() == replay,
        "v5 stores SKSU, policy and squadron tables together");
    if (both) {
        auto reversed = both.value();
        const auto end = static_cast<std::size_t>(reversed[10]) + 256U * reversed[11];
        std::rotate(reversed.begin() + 108, reversed.begin() + 116,
            reversed.begin() + static_cast<std::ptrdiff_t>(end));
        expect(!tactical::parse_replay(reversed), "header extensions have one canonical tag order");
    }
}

void test_golden_across_workers(const std::string& fixtures, const Golden& golden) {
    const auto replay = load_fixture(fixtures);
    const eawr::sim::InlineExecutor inline_executor;
    const auto reference = run_replay(replay, inline_executor, false);
    expect(reference.hashes == golden.hashes, "state hashes match the independent oracle");
    expect(reference.snapshots == golden.snapshots, "snapshot digests match the independent oracle");
    expect(reference.events == golden.events, "event stream matches the independent oracle");
    expect(reference.warnings
            == std::vector<std::string>{
                std::string(tactical::diagnostic_codes::command_rejected),
                std::string(tactical::diagnostic_codes::command_rejected),
                std::string(tactical::diagnostic_codes::command_rejected),
                std::string(tactical::diagnostic_codes::command_rejected)},
        "each command with a rejected unit order returns one warning diagnostic");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(run_replay(replay, executor, false) == reference,
            "hash, events and snapshots match with " + std::to_string(workers) + " workers");
        expect(run_replay(replay, executor, true) == reference,
            "storage scrambling keeps every output with " + std::to_string(workers) + " workers");
    }
    // #637: the live game's dispatch and its hashing thread change no output either.
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter executor(workers, eawr::platform::ThreadWorkerAdapter::Dispatch::by_cost);
        expect(run_replay(replay, executor, false, std::make_shared<eawr::platform::ThreadStateHasher>()) == reference,
            "by-cost dispatch and off-thread hashes match with " + std::to_string(workers) + " workers");
    }
    const eawr::platform::ThreadWorkerAdapter three(3);
    expect(run_replay(replay, three, false) == reference, "an odd worker count matches the reference");
    expect(run_replay(replay, ReverseInline(3), false) == reference,
        "partitions run last-first on one thread match the reference");
}

// CI guard of the phase map (docs/simulation.md): every tick hands its per-unit phases to the
// executor under their names, in order, with deterministic partition counts. A phase that turns
// serial, loses its name or changes its partition count fails here. A new partitioned phase
// adds its name to `required` and to the phase map.
void test_phase_map(const std::string& fixtures) {
    const std::vector<std::string_view> required{"gather", "movement", "targeting", "unit-systems", "visibility"};
    const auto replay = load_fixture(fixtures);
    auto created = tactical::TacticalSession::from_replay(replay);
    expect(static_cast<bool>(created), "phase map fixture session is created");
    if (!created) {
        return;
    }
    auto session = std::move(created).value();
    const PhaseRecorder recorder;
    while (session.completed_tick() < replay.final_tick_count) {
        recorder.calls.clear();
        const auto tick = std::to_string(session.completed_tick());
        expect(static_cast<bool>(session.step(recorder)), "phase map tick " + tick + " succeeds");
        auto next = required.begin();
        for (const auto& call : recorder.calls) {
            expect(!call.phase.empty(), "tick " + tick + ": every partitioned phase is named");
            const auto expected_partitions = call.phase == "gather" ? 1U : eawr::sim::tick_partition_count;
            expect(call.partitions == expected_partitions,
                "tick " + tick + ": phase '" + call.phase + "' uses its deterministic partition count");
            if (next != required.end() && call.phase == *next) {
                ++next;
            }
        }
        expect(next == required.end(), "tick " + tick + ": the movement, targeting, unit-systems and visibility phases run partitioned, in order");
    }

    // The script service (#247) runs its instances as one named phase.
    namespace script = eawr::script::authoritative;
    script::ModuleManifest manifest;
    expect(static_cast<bool>(manifest.add("Main.lua", "x = 1\n")),"phase map script module is added");
    auto scheduler = script::ScriptScheduler::create(script::SessionConfig{}, std::move(manifest));
    expect(static_cast<bool>(scheduler), "phase map script scheduler is created");
    if (!scheduler) {
        return;
    }
    expect(static_cast<bool>(scheduler.value().create_instance(1, "Main.lua")), "phase map script instance is created");
    recorder.calls.clear();
    expect(static_cast<bool>(scheduler.value().service(recorder)), "phase map script service succeeds");
    const bool script_phase = std::any_of(recorder.calls.begin(), recorder.calls.end(), [](const PhaseRecorder::Call& call) {
        return call.phase == script::script_phase_name && call.partitions == eawr::sim::tick_partition_count;
    });
    expect(script_phase && recorder.calls.size() == 1, "the script-instances phase runs partitioned");
}

void test_live_submission_and_recording(const std::string& fixtures, const Golden& golden) {
    const auto bytes = read_bytes(fixtures + "/tactical-v2.eawr-replay");
    const auto replay = load_fixture(fixtures);
    auto created = tactical::TacticalSession::create(replay.setup);
    expect(static_cast<bool>(created), "live session is created from the fixture setup");
    if (!created) {
        return;
    }
    auto session = std::move(created).value();
    const eawr::platform::ThreadWorkerAdapter executor(4);
    RunOutput output;
    record_initial(output, session);
    while (session.completed_tick() < replay.final_tick_count) {
        // Players deliver their commands just in time and in reverse player order.
        std::vector<tactical::PlayerCommand> due;
        for (const auto& command : replay.commands) {
            if (command.key.tick == session.completed_tick()) {
                due.push_back(command);
            }
        }
        std::stable_sort(due.begin(), due.end(), [](const auto& left, const auto& right) {
            return left.key.player_id > right.key.player_id;
        });
        for (const auto& command : due) {
            expect(static_cast<bool>(session.submit(command)), "just-in-time command is accepted");
        }
        // #615: the replay through the next tick, taken before the step, is the step's record.
        const auto through_next = session.record_through_next_tick();
        const auto tick = session.step(executor);
        expect(static_cast<bool>(tick), "live tick executes");
        if (!tick) {
            return;
        }
        expect(through_next == session.record(), "the replay through the next tick equals the record after it");
        record_tick(output, tick.value(), false);
    }
    expect(output.hashes == golden.hashes,
        "live just-in-time submission reproduces the replay state hashes (pending commands are not state)");
    expect(output.snapshots == golden.snapshots && output.events == golden.events,
        "live submission reproduces the replay snapshots and events");
    const auto recorded = session.record();
    expect(recorded == replay, "recording equals the fixture replay");
    const auto written = tactical::write_replay(recorded);
    expect(written && written.value() == bytes, "recorded replay encodes to the exact fixture bytes");

    const auto units = session.units();
    expect(units.size() == 9, "rules v1 creates and destroys no units");
    const auto order_of = [&units](const eawr::sim::EntityId id) {
        return std::find_if(units.begin(), units.end(), [id](const auto& unit) {
            return unit.entity_id == id;
        })->order;
    };
    expect(order_of(2).kind == tactical::OrderKind::stop && order_of(2).issued_tick == 3,
        "a later stop replaces an attack order");
    expect(order_of(3).kind == tactical::OrderKind::attack && order_of(3).target == 5
            && order_of(3).issued_tick == 1,
        "attack order records its target and tick");
    expect(order_of(4).kind == tactical::OrderKind::move
            && order_of(4).destination.x.raw() == -2067791872,
        "move order records its Q24 destination");
    expect(order_of(5).kind == tactical::OrderKind::none, "a rejected order leaves the unit unchanged");
    expect(order_of(1).kind == tactical::OrderKind::none, "an uncommanded unit has no order");
}

void test_submit_diagnostics(const std::string& fixtures) {
    const auto replay = load_fixture(fixtures);
    auto created = tactical::TacticalSession::create(replay.setup);
    auto session = std::move(created).value();
    const auto move = [](const std::uint64_t tick, const std::uint32_t player, const std::uint64_t sequence,
                          std::vector<eawr::sim::EntityId> units) {
        return tactical::PlayerCommand{{tick, player, sequence}, std::move(units), tactical::MovePayload{}};
    };
    const auto before_hash = session.state_sha256();
    const auto rejected = [&](const tactical::PlayerCommand& command, const std::string_view code,
                              const std::string_view what) {
        const auto result = session.submit(command);
        expect(!result && result.error().code == code, what);
        expect(session.pending_command_count() == 0 && session.state_sha256() == before_hash,
            std::string(what) + " leaves the session unchanged");
    };
    rejected(move(0, 9, 1, {2}), tactical::diagnostic_codes::invalid_issuer, "unknown issuer");
    rejected(move(0, 3, 1, {9}), tactical::diagnostic_codes::invalid_issuer, "non-commandable issuer");
    rejected(move(0, 1, 1, {}), tactical::diagnostic_codes::invalid_command, "empty unit list");
    rejected(move(0, 1, 1, {3, 2}), tactical::diagnostic_codes::invalid_command, "unsorted unit list");
    rejected(move(0, 1, 1, {0, 2}), tactical::diagnostic_codes::invalid_command, "unit ID zero");
    rejected(tactical::PlayerCommand{{0, 1, 1}, {2}, tactical::AttackPayload{0}},
        tactical::diagnostic_codes::invalid_command, "attack target zero");
    std::vector<eawr::sim::EntityId> many(tactical::max_units_per_command + 1);
    for (std::size_t index = 0; index < many.size(); ++index) {
        many[index] = index + 1;
    }
    rejected(move(0, 1, 1, many), tactical::diagnostic_codes::resource_limit, "oversized unit list");
    rejected(move(tactical::max_ticks, 1, 1, {2}), tactical::diagnostic_codes::resource_limit, "tick limit");

    expect(static_cast<bool>(session.submit(move(4, 1, 7, {2}))), "future command is accepted");
    const auto out_of_order = session.submit(move(4, 1, 6, {2}));
    expect(!out_of_order && out_of_order.error().code == tactical::diagnostic_codes::order,
        "lower sequence from the same issuer on the same tick is out of order");
    const auto earlier_tick = session.submit(move(3, 1, 9, {2}));
    expect(!earlier_tick && earlier_tick.error().code == tactical::diagnostic_codes::order,
        "earlier tick from the same issuer after a later one is out of order");
    const auto duplicate = session.submit(move(4, 1, 7, {3}));
    expect(!duplicate && duplicate.error().code == tactical::diagnostic_codes::order,
        "duplicate (tick, player, sequence) is out of order");
    expect(static_cast<bool>(session.submit(move(2, 2, 1, {6}))),
        "another issuer may submit an earlier tick");
    expect(session.pending_command_count() == 2, "only accepted commands are queued");

    const eawr::sim::InlineExecutor executor;
    for (int index = 0; index < 3; ++index) {
        expect(static_cast<bool>(session.step(executor)), "tick with queued future commands executes");
    }
    const auto late = session.submit(move(2, 2, 2, {6}));
    expect(!late && late.error().code == tactical::diagnostic_codes::late_command
            && late.error().message.find("next tick to execute is 3") != std::string::npos,
        "command for an executed tick is late with context");
    expect(static_cast<bool>(session.submit(move(3, 2, 2, {6}))), "command for the next tick is on time");
}

void test_atomicity_and_snapshots(const std::string& fixtures) {
    const auto replay = load_fixture(fixtures);
    auto created = tactical::TacticalSession::from_replay(replay);
    auto session = std::move(created).value();
    const auto hash = session.state_sha256();
    const auto snapshot = session.snapshot();
    const auto pending = session.pending_command_count();
    const FailingExecutor failing;
    const auto failed = session.step(failing);
    expect(!failed && failed.error().code == tactical::diagnostic_codes::worker_failure,
        "executor failure is propagated");
    expect(session.completed_tick() == 0 && session.state_sha256() == hash
            && session.snapshot() == snapshot && session.pending_command_count() == pending,
        "executor failure leaves tick, state, snapshot and queue unchanged");
    const auto wrong = session.step(ReverseInline(0));
    expect(!wrong && wrong.error().code == tactical::diagnostic_codes::worker_failure,
        "an executor without workers is rejected");
    const eawr::platform::ThreadWorkerAdapter no_workers(0);
    const auto pool_without_workers = session.step(no_workers);
    expect(!pool_without_workers && pool_without_workers.error().code == tactical::diagnostic_codes::worker_failure
            && session.completed_tick() == 0 && session.state_sha256() == hash,
        "a pool without workers is rejected and leaves the session unchanged");

    expect(snapshot->completed_tick() == 0 && snapshot->events().empty()
            && snapshot->instances().size() == 9,
        "tick-zero snapshot holds every setup unit and no events");
    const auto& flipped = snapshot->instances()[6];
    const auto one = eawr::sim::math::Fixed::scale;
    expect(flipped.entity_id == 7 && flipped.team == 1 && flipped.fixed_transform.rows[0][0].raw() == -one
            && flipped.fixed_transform.rows[1][1].raw() == -one
            && flipped.fixed_transform.rows[2][2].raw() == one
            && flipped.fixed_transform.rows[0][1].raw() == 0,
        "half-turn yaw converts to an exact Q24 matrix");
    const auto& neutral = snapshot->instances()[8];
    expect(neutral.owner == 3 && neutral.team == 3
            && neutral.fixed_transform == eawr::sim::math::identity_matrix(),
        "identity rotation at the origin is the identity matrix");

    const eawr::sim::InlineExecutor executor;
    const auto first = session.step(executor);
    const auto held = first.value().snapshot;
    const std::vector<tactical::Event> held_events(held->events().begin(), held->events().end());
    expect(static_cast<bool>(session.step(executor)), "second tick executes");
    expect(held->completed_tick() == 1
            && std::vector<tactical::Event>(held->events().begin(), held->events().end()) == held_events
            && held_events.size() == 4,
        "held snapshot and its events stay immutable");
    expect(held != session.snapshot(), "each tick publishes a new snapshot");
}

void test_gripper_metadata_is_not_canonical() {
    tactical::TacticalInstance instance;
    instance.entity_id = 1;
    instance.type_id = 10;
    instance.owner = 1;
    instance.team = 1;
    instance.fixed_transform = eawr::sim::math::identity_matrix();
    instance.visible_to = 1;
    const tactical::TacticalSnapshot plain(0, {{1, 1}}, {instance}, {});
    instance.craft_velocity_per_frame = eawr::sim::math::Vec3{
        eawr::sim::math::Fixed::from_raw(100), eawr::sim::math::Fixed::from_raw(200),
        eawr::sim::math::Fixed::from_raw(300)};
    instance.squadron_in_idle_grid = true;
    const tactical::TacticalSnapshot presented(0, {{1, 1}}, {instance}, {});
    expect(plain.canonical_bytes() == presented.canonical_bytes() && plain.sha256() == presented.sha256(),
        "WSU-34: icon velocity and idle-grid metadata do not change canonical bytes or hashes");
}

// An empty session steps cheaply, so this walks the real tick limit.
void test_tick_limit() {
    auto created = tactical::TacticalSession::create(tactical::TacticalSetup{});
    auto session = std::move(created).value();
    const eawr::sim::InlineExecutor executor;
    bool stepped = true;
    while (stepped && session.completed_tick() < tactical::max_ticks) {
        stepped = static_cast<bool>(session.step(executor));
    }
    expect(stepped && session.completed_tick() == tactical::max_ticks, "a session steps up to the tick limit");
    const auto hash = session.state_sha256();
    const auto snapshot = session.snapshot();
    const auto beyond = session.step(executor);
    expect(!beyond && beyond.error().code == tactical::diagnostic_codes::resource_limit,
        "a step past the tick limit is a resource-limit error");
    expect(session.completed_tick() == tactical::max_ticks && session.state_sha256() == hash
            && session.snapshot() == snapshot,
        "a refused step leaves tick, state and snapshot unchanged");
    const auto recorded = session.record();
    expect(recorded.final_tick_count == tactical::max_ticks && static_cast<bool>(tactical::write_replay(recorded)),
        "a session at the tick limit still records a writable replay");
}

void test_setup_validation() {
    tactical::TacticalSetup setup;
    setup.players = {{1, 0, 1, tactical::player_flag_commandable}};
    tactical::UnitState unit;
    unit.entity_id = 1;
    unit.type_id = 1;
    unit.owner = 1;
    setup.units = {unit};
    expect(static_cast<bool>(tactical::TacticalSession::create(setup)), "minimal setup is valid");
    auto ordered = setup;
    ordered.units[0].order.kind = tactical::OrderKind::move;
    expect(!tactical::TacticalSession::create(ordered), "setup units cannot carry an order");
    auto duplicate = setup;
    duplicate.players.push_back(duplicate.players.front());
    const auto duplicate_result = tactical::TacticalSession::create(duplicate);
    expect(!duplicate_result && duplicate_result.error().code == tactical::diagnostic_codes::order,
        "duplicate player IDs are an ordering error");
    auto empty = tactical::TacticalSetup{};
    auto empty_session = tactical::TacticalSession::create(empty);
    expect(empty_session && empty_session.value().next_entity_id() == 1,
        "an empty setup issues stable ID 1 next");
}

void test_sparse_registry_commit() {
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        for (const bool scramble : {false, true}) {
            tactical::TacticalSetup setup;
            setup.players = {{1, 0, 1, tactical::player_flag_commandable}};
            for (eawr::sim::EntityId id = 1; id <= 2048; ++id) {
                tactical::UnitState unit;
                unit.entity_id = id;
                unit.type_id = 1;
                unit.owner = 1;
                setup.units.push_back(unit);
            }
            auto created = tactical::TacticalSession::create(setup);
            expect(static_cast<bool>(created), "sparse registry setup creates");
            if (!created) return;
            auto session = std::move(created).value();
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (std::uint64_t frame = 0; frame < 4; ++frame) {
                if (scramble) session.scramble_storage_for_testing();
                if (frame == 1) {
                    tactical::PlayerCommand command;
                    command.key = {frame, 1, 0};
                    command.units = {1024};
                    command.payload = tactical::StopPayload{};
                    expect(static_cast<bool>(session.submit(command)), "one sparse command submits");
                }
                const auto stepped = session.step(executor);
                expect(static_cast<bool>(stepped), "sparse registry tick executes");
                if (!stepped) return;
                expect(stepped.value().registry_emplacements == 0,
                    "steady registry never recreates an existing unit's components");
                expect(stepped.value().registry_component_writes == (frame == 1 ? 1U : 0U),
                    "registry writes depend on changed components, not the 2048 live units");
                expect(stepped.value().staged_map_copies == 0, "absent flight state has no journal copies");
                hashes.push_back(stepped.value().state_sha256);
            }
            if (reference.empty()) reference = hashes;
            else expect(hashes == reference, "sparse commits agree at 1/2/4/8 workers and scrambled storage");
        }
    }
}

} // namespace

int main(const int argc, const char* const argv[]) {
    if (argc != 2) {
        std::cerr << "usage: tactical_contract_tests <fixture-dir>\n";
        return 2;
    }
    const std::string fixtures = argv[1];
    const Golden golden{
        read_rows(fixtures + "/tactical-v2.hashes.csv"),
        read_rows(fixtures + "/tactical-v2.snapshots.csv"),
        read_rows(fixtures + "/tactical-v2.events.csv"),
    };
    expect(golden.hashes.size() == 6 && golden.snapshots.size() == 7 && golden.events.size() == 18,
        "golden oracle files are complete");
    expect(read_bytes(fixtures + "/tactical-v2.eawr-replay").size() > tactical::replay_header_size,
        "tactical fixture is present");
    if (failures != 0) {
        std::cerr << "tactical fixtures are missing from " << fixtures << '\n';
        return 1;
    }
    test_format(fixtures);
    test_match_policy_format(fixtures);
    test_skirmish_setup_format(fixtures);
    test_golden_across_workers(fixtures, golden);
    test_phase_map(fixtures);
    test_live_submission_and_recording(fixtures, golden);
    test_submit_diagnostics(fixtures);
    test_atomicity_and_snapshots(fixtures);
    test_gripper_metadata_is_not_canonical();
    test_tick_limit();
    test_setup_validation();
    test_sparse_registry_commit();
    if (failures != 0) {
        std::cerr << failures << " tactical contract test(s) failed\n";
        return 1;
    }
    std::cout << "tactical contracts passed\n";
    return 0;
}
