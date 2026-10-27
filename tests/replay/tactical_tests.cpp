#include "../support/check.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include "../../src/sim/tactical/checkpoint_access_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <ios>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace tactical = eawr::sim::tactical;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}

// #3644: a provider whose object turn is due every tick and asks for nothing.
class EveryTurn final : public tactical::ObjectTurnService {
public:
    [[nodiscard]] bool due(const std::uint64_t) override { return true; }
    [[nodiscard]] eawr::core::Result<std::vector<tactical::ObjectTurnUse>> run(
        const eawr::sim::PartitionExecutor&, const tactical::ObjectTurnView& view) override {
        rows = view.rows.size();
        return eawr::core::Result<std::vector<tactical::ObjectTurnUse>>::success({});
    }
    std::size_t rows{};
};

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

// Fail only the read-only publication accelerator, after producing partial output.
class FailingCanonicalExecutor final : public eawr::sim::PartitionExecutor {
public:
    explicit FailingCanonicalExecutor(std::string_view phase = "canonical-records") : phase_(phase) {}
    [[nodiscard]] std::size_t worker_count() const noexcept override { return 2; }
    [[nodiscard]] eawr::core::Result<void> execute(const std::size_t count,
        const std::function<void(std::size_t)>& partition) const override {
        return eawr::sim::InlineExecutor{}.execute(count, partition);
    }
    [[nodiscard]] eawr::core::Result<void> execute_phase(const std::string_view phase,
        const std::size_t count, const std::function<void(std::size_t)>& partition) const override {
        if (phase != phase_) return execute(count, partition);
        reached = true;
        for (std::size_t index = 0; index < count / 2; ++index) partition(index);
        return FailingExecutor{}.execute(count, partition);
    }
    mutable bool reached{};
private:
    std::string_view phase_;
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

void test_special_fire_format(const std::string& fixtures) {
    auto replay = load_fixture(fixtures);
    replay.commands.clear();
    for (const auto admission : {tactical::SpecialFireAdmission::player,
            tactical::SpecialFireAdmission::ai_object, tactical::SpecialFireAdmission::ai_taskforce}) {
        replay.commands.push_back({{0, 1, replay.commands.size()}, {1}, tactical::SpecialFirePayload{2, admission}});
    }
    const auto encoded = tactical::write_replay(replay);
    expect(static_cast<bool>(encoded), "special fire admission routes serialize");
    if (encoded) {
        const auto parsed = tactical::parse_replay(encoded.value(), "special-fire");
        expect(parsed && parsed.value() == replay, "special fire source, target and admission survive replay");
        const auto rewritten = parsed ? tactical::write_replay(parsed.value()) : encoded;
        expect(rewritten && rewritten.value() == encoded.value(), "special fire replay bytes are stable");
    }
    auto invalid = replay;
    invalid.commands.front().units.push_back(2);
    expect(!tactical::write_replay(invalid), "special fire cannot name multiple sources");
    invalid = replay;
    std::get<tactical::SpecialFirePayload>(invalid.commands.front().payload).target = 0;
    expect(!tactical::write_replay(invalid), "special fire rejects a zero target");
    invalid = replay;
    std::get<tactical::SpecialFirePayload>(invalid.commands.front().payload).admission =
        static_cast<tactical::SpecialFireAdmission>(3);
    expect(!tactical::write_replay(invalid), "special fire rejects an unknown admission route");
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

void test_battle_domain(const std::string& fixtures) {
    auto replay = load_fixture(fixtures);
    const auto legacy = read_bytes(fixtures + "/tactical-v2.eawr-replay");
    expect(replay.setup.domain == tactical::BattleDomain::space, "absent battle domain is space");
    const auto space = tactical::write_replay(replay);
    expect(space && space.value() == legacy, "default space keeps every fixture byte");
    tactical::TacticalContent content;
    auto world = tactical::TacticalSession::create(replay.setup, content);
    expect(world && world.value().domain() == tactical::BattleDomain::space, "space content remains the default");
    if (world) {
        const auto recorded = tactical::write_replay(world.value().record());
        auto initial = replay;
        initial.commands.clear();
        initial.final_tick_count = 0;
        expect(recorded && recorded.value() == tactical::write_replay(initial).value(), "space session recording keeps its domain implicit");
    }
    replay.setup.domain = tactical::BattleDomain::land;
    auto expected = legacy;
    expected[8] = 4;
    expected[10] = 120;
    const auto tag = tactical::replay_extension_battle_domain;
    const std::array<std::uint8_t, 16> extension{
        1, 0, 0, 0, static_cast<std::uint8_t>(tag), static_cast<std::uint8_t>(tag >> 8),
        8, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    expected.insert(expected.begin() + 104, extension.begin(), extension.end());
    const auto land = tactical::write_replay(replay);
    const auto parsed = tactical::parse_replay(expected);
    expect(land && land.value() == expected, "land domain uses the independently encoded versioned extension");
    expect(parsed && parsed.value() == replay, "land replay parses without creating a land world");
    if (parsed) {
        const auto rewritten = tactical::write_replay(parsed.value());
        expect(rewritten && rewritten.value() == expected, "land replay has one canonical round-trip encoding");
    }
    const auto refused = [&](const auto& result) {
        return !result && result.error().code == tactical::diagnostic_codes::land_provider_unavailable;
    };
    expect(refused(tactical::TacticalSession::create(replay.setup)), "unbundled land create refuses missing provider");
    expect(refused(tactical::TacticalSession::from_replay(replay)), "unbundled land replay cannot bypass provider refusal");
    content.domain = tactical::BattleDomain::land;
    expect(refused(tactical::TacticalSession::create(replay.setup, content)), "bundled land create refuses missing provider");
    expect(refused(tactical::TacticalSession::from_replay(replay, content)), "bundled land replay refuses missing provider");
    replay.setup.domain = tactical::BattleDomain::space;
    const auto mismatch = tactical::TacticalSession::create(replay.setup, content);
    expect(!mismatch && mismatch.error().code == tactical::diagnostic_codes::invalid_setup,
        "space setup cannot bind land content");
    content.domain = tactical::BattleDomain::space;
    replay.setup.domain = tactical::BattleDomain::land;
    const auto reverse_mismatch = tactical::TacticalSession::from_replay(replay, content);
    expect(!reverse_mismatch && reverse_mismatch.error().code == tactical::diagnostic_codes::invalid_setup,
        "land replay cannot bind space content");
    replay.setup.domain = static_cast<tactical::BattleDomain>(2);
    expect(!tactical::write_replay(replay), "unknown setup domain cannot be written");
    expect(!tactical::TacticalSession::create(replay.setup), "unknown setup domain cannot create a world");
    const auto rejects = [&](std::size_t offset, std::uint8_t value, std::string_view label) {
        auto bytes = expected;
        bytes[offset] = value;
        expect(!tactical::parse_replay(bytes), label);
    };
    rejects(110, 4, "domain extension length must be exact");
    rejects(112, 2, "unknown domain extension version refuses");
    rejects(116, 0, "explicit space extension is noncanonical");
    rejects(116, 2, "unknown wire domain refuses");
    auto truncated = expected;
    truncated.resize(119);
    expect(!tactical::parse_replay(truncated), "truncated domain body refuses");
    auto duplicate = expected;
    duplicate[10] = 132;
    duplicate[104] = 2;
    duplicate.insert(duplicate.begin() + 120, expected.begin() + 108, expected.begin() + 120);
    expect(!tactical::parse_replay(duplicate), "duplicate domain extension refuses");
    replay.setup.domain = tactical::BattleDomain::land;
    replay.setup.match_policy = tactical::SkirmishMatchPolicy{false, true, true, true};
    replay.setup.units = {{1, 101, 1}, {2, 102, 1}};
    replay.setup.players = {{1, 0, 1, tactical::player_flag_commandable}};
    replay.setup.squadrons = {{1, {2}}};
    replay.commands.clear();
    const auto combined = tactical::write_replay(replay);
    expect(combined && combined.value()[8] == 5, "land domain composes with squadron extensions");
    if (combined) {
        const auto round_trip = tactical::parse_replay(combined.value());
        expect(round_trip && round_trip.value() == replay, "domain composes with other ordered extensions");
    }
}

void test_native_scene_format(const std::string& fixtures) {
    auto replay = load_fixture(fixtures);
    tactical::ReplaySkirmishSetup map;
    map.map = "data/art/maps/renamed_campaign.ted";
    map.map_sha256 = std::string(64, 'b'); map.victory_condition = 2;
    for (const auto& player : replay.setup.players) if (player.commandable())
        map.slots.push_back({player.player_id, player.player_id == 1, {}, {}});
    replay.setup.skirmish = map;
    replay.setup.first_free_entity_id = replay.setup.units.back().entity_id + 1;
    const auto without_native = tactical::write_replay(replay);
    replay.setup.native_scene = tactical::ReplayNativeScene{true,
        {"Mod_Carrier", "Mod_Station"}, {"Mod_Nebula"}, {"Mod_Carrier", "Mod_Fighter"}};
    const auto encoded = tactical::write_replay(replay);
    expect(encoded && tactical::parse_replay(encoded.value()).value() == replay,
        "native campaign metadata round-trips renamed loader roots and multiplayer policy");
    if (!encoded || !without_native) return;
    const auto reference = run_replay(replay, eawr::sim::InlineExecutor{}, false);
    for (const auto workers : {1U, 2U, 4U, 8U})
        expect(run_replay(replay, ReverseInline(workers), true) == reference,
            "native load metadata preserves deterministic tactical state across workers");
    auto plain = replay; plain.setup.native_scene.reset();
    expect(tactical::write_replay(plain).value() == without_native.value(),
        "omitted native scene leaves ordinary tactical replay bytes unchanged");
    auto invalid = replay; invalid.setup.skirmish.reset();
    expect(!tactical::write_replay(invalid), "native scene cannot omit native map identity");
    invalid = replay; invalid.setup.first_free_entity_id.reset();
    expect(!tactical::write_replay(invalid), "native scene requires campaign allocator context");
    invalid = replay; invalid.setup.native_scene->battle_types.clear();
    expect(!tactical::write_replay(invalid), "native scene refuses empty battle closure");
    invalid = replay; invalid.setup.native_scene->source_types.push_back("mod_carrier");
    expect(!tactical::write_replay(invalid), "native scene refuses case-insensitive duplicate roots");
    invalid = replay; invalid.setup.native_scene->source_types.front() = std::string(65536, 'x');
    expect(static_cast<bool>(tactical::validate_setup(invalid.setup)), "file metadata limits never refuse a live campaign setup");
    expect(!tactical::write_replay(invalid), "native scene refuses oversized names");
    invalid = replay;
    invalid.setup.native_scene->source_types = {std::string(65535, 'x')};
    expect(static_cast<bool>(tactical::write_replay(invalid)), "maximum u16 name fits wide native framing");
    {
        auto boundary = replay;
        auto& roots = boundary.setup.native_scene->source_types;
        roots.clear();
        for (unsigned i = 0; i < 4095; ++i) {
            auto name = std::to_string(i);
            name.resize(65535, 'x');
            roots.push_back(std::move(name));
        }
        expect(static_cast<bool>(tactical::validate_replay(boundary)), "native body below 256 MiB remains admissible");
        auto name = std::string("4095"); name.resize(65535, 'x'); roots.push_back(std::move(name));
        const auto refused = tactical::validate_replay(boundary);
        const auto body_size = 20U + roots.size() * 65537U + 12U + 13U + 13U;
        expect(!refused && refused.error().message.find(std::to_string(body_size)) != std::string::npos,
            "body past 256 MiB refuses with its actual byte count");
    }
    {
        auto narrow_body = replay;
        narrow_body.setup.units.clear();
        narrow_body.commands.clear();
        for (eawr::sim::EntityId id = 1; id <= 8191; ++id) {
            narrow_body.setup.units.push_back({id, 101, 1});
            narrow_body.setup.units.back().garrison_enabled = false;
        }
        narrow_body.setup.first_free_entity_id = 8193;
        const auto fits = tactical::write_replay(narrow_body);
        expect(fits && tactical::parse_replay(fits.value()).value() == narrow_body,
            "wide header retains narrow garrison body below 65535 bytes");
        narrow_body.setup.units.push_back({8192, 101, 1});
        narrow_body.setup.units.back().garrison_enabled = false;
        const auto refused = tactical::write_replay(narrow_body);
        expect(!refused && refused.error().code == tactical::diagnostic_codes::resource_limit
            && refused.error().message.find("extension 3") != std::string::npos
            && refused.error().message.find("65536") != std::string::npos,
            "wide header refuses a narrow garrison body above 65535 bytes with tag and size");
    }
    auto large = replay;
    for (unsigned i = 0; i < 3000; ++i)
        large.setup.native_scene->source_types.push_back("Synthetic_Catalog_Type_" + std::to_string(i));
    const auto large_bytes = tactical::write_replay(large);
    expect(large_bytes && large_bytes.value().size() > 65535, "native metadata exceeds the former u16 header limit");
    if (large_bytes) {
        expect(large_bytes.value() == read_bytes(fixtures + "/native-large.bin"), "large native replay matches independent fixture");
        const auto parsed = tactical::parse_replay(large_bytes.value());
        expect(parsed && parsed.value() == large, "large native metadata round-trips without truncation");
        if (!parsed) return;
        expect(tactical::write_replay(parsed.value()).value() == large_bytes.value(), "large native bytes are canonical");
        const auto expected = run_replay(replay, eawr::sim::InlineExecutor{}, false);
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            expect(run_replay(parsed.value(), ReverseInline(workers), true) == expected,
                "large metadata keeps simulation and snapshots equal across workers");
            auto session = tactical::TacticalSession::from_replay(parsed.value());
            expect(static_cast<bool>(session), "large metadata creates checkpoint session");
            if (!session) continue;
            expect(static_cast<bool>(session.value().step(ReverseInline(workers))), "large metadata advances before checkpoint");
            const auto checkpoint = session.value().save_checkpoint_state();
            expect(static_cast<bool>(checkpoint), "large native metadata saves checkpoint");
            if (!checkpoint) continue;
            auto restored = tactical::TacticalSession::from_replay(parsed.value());
            expect(static_cast<bool>(restored), "large metadata creates restored session");
            if (!restored) continue;
            const auto loaded = restored.value().load_checkpoint_state(checkpoint.value());
            expect(static_cast<bool>(loaded), "large metadata restores checkpoint");
            if (!loaded) continue;
            expect(restored.value().record().setup.native_scene == large.setup.native_scene, "checkpoint preserves every native root");
            while (session.value().completed_tick() < large.final_tick_count) {
                const auto a = session.value().step(ReverseInline(workers));
                const auto b = restored.value().step(ReverseInline(workers));
                expect(a && b && a.value().state_hash.get() == b.value().state_hash.get(), "checkpoint continuation matches across workers");
                if (!a || !b) break;
            }
        }
        auto truncated = large_bytes.value();
        truncated[12] = 255; truncated[13] = 255;
        expect(!tactical::parse_replay(truncated), "oversized u32 header refuses before allocation");
    }
    auto squadron_native = replay;
    squadron_native.setup.units = {{1, 101, 1}, {2, 102, 1}};
    squadron_native.setup.squadrons = {{1, {2}}};
    squadron_native.setup.first_free_entity_id = 3;
    squadron_native.commands.clear();
    const auto squadron_bytes = tactical::write_replay(squadron_native);
    expect(squadron_bytes && squadron_bytes.value()[8] == tactical::replay_format_version_squadron_native
        && tactical::parse_replay(squadron_bytes.value()).value() == squadron_native, "wide native format retains squadron tables");
    const auto header = static_cast<std::size_t>(encoded.value()[10]) + 256U * encoded.value()[11];
    std::size_t record = 110;
    while (record < header && (encoded.value()[record] != tactical::replay_extension_native_scene
        || encoded.value()[record + 1] != 0)) {
        record += 4U + encoded.value()[record + 2] + 256U * encoded.value()[record + 3];
    }
    expect(record < header, "native scene extension has its reserved tag");
    if (record >= header) return;
    auto bytes = encoded.value(); bytes[record + 6] = 2;
    expect(!tactical::parse_replay(bytes), "native scene refuses unknown body version");
    bytes = encoded.value(); bytes[record + 10] = 2;
    expect(!tactical::parse_replay(bytes), "native scene refuses unknown flags");
    bytes = encoded.value();
    for (std::size_t i = 0; i < 4; ++i) bytes[record + 14 + i] = 255;
    expect(!tactical::parse_replay(bytes), "native scene refuses oversized counts before allocation");
    bytes = encoded.value(); bytes[record + 18] = 0; bytes[record + 19] = 0;
    expect(!tactical::parse_replay(bytes), "native scene refuses empty encoded names");
    bytes = encoded.value();
    for (unsigned i = 0; i < 4; ++i) bytes[record + 2 + i] = 255;
    expect(!tactical::parse_replay(bytes), "native u32 length beyond file refuses before allocation");
    bytes = encoded.value(); bytes[8] = 4;
    expect(!tactical::parse_replay(bytes), "native framing cannot be relabeled as a narrow format");
    bytes = encoded.value(); bytes.resize(header - 1);
    expect(!tactical::parse_replay(bytes), "native scene refuses truncated header bodies");
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
    metadata.match.tactical_win_condition = "SKIRMISH_ALL_ENEMY_UNITS_DESTROYED";
    for (const auto& player : replay.setup.players) if (player.commandable())
        metadata.slots.push_back({player.player_id, player.player_id != 1U, 2U, {"Test_Fleet"}});
    replay.setup.skirmish = metadata;
    const auto written = tactical::write_replay(replay);
    expect(written && written.value()[8] == 4, "SKSU alone selects v4 without a redundant default policy");
    if (!written) return;
    const auto parsed = tactical::parse_replay(written.value());
    expect(parsed && parsed.value() == replay, "SKSU round-trips roles, map, palette, fleet and every match field");
    auto custom = replay;
    custom.setup.skirmish->map = "data/custommaps/recorded.ted";
    expect(static_cast<bool>(tactical::validate_setup(custom.setup)), "custom-map SKSU passes shared setup validation for LAN handoff");
    const auto custom_bytes = tactical::write_replay(custom);
    expect(static_cast<bool>(custom_bytes), "custom-map SKSU encodes");
    if (custom_bytes) {
        const auto decoded_custom = tactical::parse_replay(custom_bytes.value());
        expect(decoded_custom && decoded_custom.value() == custom, "custom-map SKSU round-trips the complete setup");
        const auto map_begin = std::search(custom_bytes.value().begin(), custom_bytes.value().end(),
            custom.setup.skirmish->map.begin(), custom.setup.skirmish->map.end());
        expect(map_begin != custom_bytes.value().end(), "encoded SKSU contains the authored custom-map directory name");
        if (map_begin != custom_bytes.value().end()) {
            auto invalid_bytes = custom_bytes.value();
            invalid_bytes[static_cast<std::size_t>(map_begin - custom_bytes.value().begin())] = 'x';
            expect(!tactical::parse_replay(invalid_bytes), "SKSU decoder rejects a custom-map path outside the admitted directories");
        }
    }
    for (const auto* invalid_path : {"data/other/recorded.ted", "data/custommaps/nested/recorded.ted",
        "data/custommaps/../recorded.ted", "data/custommaps/nested\\recorded.ted", "data/custommaps/recorded.xml"}) {
        auto invalid_custom = custom;
        invalid_custom.setup.skirmish->map = invalid_path;
        expect(!tactical::validate_setup(invalid_custom.setup) && !tactical::write_replay(invalid_custom),
            "custom-map SKSU retains directory, nesting, traversal, separator and extension guards");
    }
    const auto reference = run_replay(legacy, eawr::sim::InlineExecutor{}, false);
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        expect(run_replay(replay, ReverseInline(workers), true) == reference,
            "load-time metadata does not change canonical hashes, snapshots or events across workers");
        expect(run_replay(custom, ReverseInline(workers), true) == reference,
            "custom-map metadata preserves replay hashes, snapshots and events across workers");
    }
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
        + 2U + metadata.match.tactical_win_condition.size();
    rejects(victory, 3, "non-lobby victory condition rejects");
    rejects(victory + 4U, 65, "oversized SKSU slot count rejects");
    rejects(victory + 12U, 3, "human role with an AI level rejects");
    rejects(victory + 12U, 6, "out-of-range role rejects");
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
    const std::vector<std::string_view> required{"gather", "movement", "hull-service", "combat-input-refresh", "targeting", "unit-systems", "visibility", "canonical-records"};
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
        expect(next == required.end(), "tick " + tick + ": the movement, targeting, unit-systems, visibility and canonical-records phases run partitioned, in order");
        expect(std::none_of(recorder.calls.begin(), recorder.calls.end(),
                   [](const PhaseRecorder::Call& call) { return call.phase == "object-turn-rows"; }),
            "tick " + tick + ": without an object-turn provider the object-turn-rows phase never runs");
    }

    // The object turn (#3644): with a provider whose turn is due, its staged rows are one named partitioned
    // phase after the targeting and ability service and before the unit systems.
    auto turn_created = tactical::TacticalSession::from_replay(replay);
    expect(static_cast<bool>(turn_created), "object-turn phase fixture session is created");
    if (!turn_created) {
        return;
    }
    auto turn_session = std::move(turn_created).value();
    const auto provider = std::make_shared<EveryTurn>();
    expect(static_cast<bool>(turn_session.set_object_turn_service(provider)), "the phase map provider installs");
    while (turn_session.completed_tick() < replay.final_tick_count) {
        recorder.calls.clear();
        const auto tick = std::to_string(turn_session.completed_tick());
        expect(static_cast<bool>(turn_session.step(recorder)), "object-turn phase tick " + tick + " succeeds");
        const auto position = [&](const std::string_view name) {
            return std::find_if(recorder.calls.begin(), recorder.calls.end(),
                [&](const PhaseRecorder::Call& call) { return call.phase == name; }) - recorder.calls.begin();
        };
        const auto rows = position("object-turn-rows");
        expect(static_cast<std::size_t>(rows) < recorder.calls.size()
                && recorder.calls[static_cast<std::size_t>(rows)].partitions == eawr::sim::tick_partition_count
                && std::count_if(recorder.calls.begin(), recorder.calls.end(),
                       [](const PhaseRecorder::Call& call) { return call.phase == "object-turn-rows"; }) == 1,
            "tick " + tick + ": the object-turn-rows phase runs once and partitioned");
        expect(position("targeting") < rows && rows < position("unit-systems"),
            "tick " + tick + ": object-turn-rows runs after targeting and before the unit systems");
        expect(provider->rows >= turn_session.units().size() && provider->rows != 0, "the provider saw the staged rows");
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

    // Hash preparation follows authoritative commit. Even partial worker failure
    // must publish that completed tick through the identical serial encoder.
    auto recovered_created = tactical::TacticalSession::from_replay(replay);
    auto reference_created = tactical::TacticalSession::from_replay(replay);
    expect(recovered_created && reference_created, "canonical failure fixtures are created");
    if (!recovered_created || !reference_created) return;
    auto recovered = std::move(recovered_created).value();
    auto reference = std::move(reference_created).value();
    const FailingCanonicalExecutor encoding_failure;
    const ReverseInline reversed(4);
    for (std::uint64_t index = 0; index < 3 && index < replay.final_tick_count; ++index) {
        encoding_failure.reached = false;
        const auto expected = reference.step(reversed);
        const auto actual = recovered.step(encoding_failure);
        expect(encoding_failure.reached && expected && actual,
            "partial canonical worker failure completes publication");
        if (!expected || !actual) break;
        expect(actual.value().state_hash.get() == expected.value().state_hash.get()
                && recovered.canonical_state_bytes() == reference.canonical_state_bytes(),
            "canonical fallback preserves the exact state bytes and hash");
        expect(recovered.snapshot()->completed_tick() == recovered.completed_tick()
                && recovered.snapshot()->canonical_bytes() == reference.snapshot()->canonical_bytes()
                && recovered.pending_command_count() == reference.pending_command_count(),
            "canonical fallback publishes the completed snapshot and command queue");
    }
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
    instance.squadron_idle_anchor = eawr::sim::math::Vec3{
        eawr::sim::math::Fixed::from_raw(400), eawr::sim::math::Fixed::from_raw(500),
        eawr::sim::math::Fixed::from_raw(600)};
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

namespace {
void test_human_seat_admission() {
    static_assert(!std::is_same_v<eawr::sim::SeatId, tactical::PlayerId>);
    static_assert(sizeof(eawr::sim::SeatId) == sizeof(std::uint32_t));
    const auto classify = []<std::size_t... Index>(std::index_sequence<Index...>) {
        unsigned engine_count = 0;
        ((engine_count += tactical::payload_authority(tactical::CommandPayload{
            std::variant_alternative_t<Index, tactical::CommandPayload>{}}) == tactical::PayloadAuthority::engine_only), ...);
        return engine_count;
    };
    expect(classify(std::make_index_sequence<std::variant_size_v<tactical::CommandPayload>>{}) == 6U,
        "CP-1: every payload is classified; damage, credits, reveal, reservation debit, campaign releases and battle control are engine-only");
    tactical::SeatBinding binding{{1}, 2};
    tactical::SeatCommand intent{{0, {1}, 0}, 2, {10}, tactical::StopPayload{}};
    tactical::SeatAdmission admission;
    auto admitted = admission.admit(binding, intent);
    expect(admitted && admitted.value().issuer == intent.key && admitted.value().command.key.player_id == 2,
        "ADR-020: admitted command retains seat provenance and authorizes its owning faction");
    expect(!admission.admit(binding, intent), "a duplicate seat cursor is refused");
    auto other_seat = binding;
    other_seat.seat = {2};
    auto other_intent = intent;
    other_intent.key.seat = {2};
    expect(static_cast<bool>(admission.admit(other_seat, other_intent)),
        "same-faction seats have independent sequence cursors");
    expect(!tactical::admit_human_command(binding, other_intent), "CP-2: payload cannot spoof authenticated seat");
    auto wrong_faction = intent;
    wrong_faction.player = 1;
    expect(!tactical::admit_human_command(binding, wrong_faction), "CP-6: seat cannot claim another faction");
    auto observer = binding;
    observer.observer = true;
    expect(!tactical::admit_human_command(observer, intent), "read-only battle observer cannot issue orders");
    observer.observer = false;
    observer.active = false;
    expect(!tactical::admit_human_command(observer, intent), "inactive seat cannot issue orders");
    for (const auto& payload : {tactical::CommandPayload{tactical::DamagePayload{}},
        tactical::CommandPayload{tactical::CreditGrantPayload{}}, tactical::CommandPayload{tactical::RevealAllPayload{}},
        tactical::CommandPayload{tactical::AiReservationDebitPayload{}}, tactical::CommandPayload{tactical::BuyPayload{1, true}},
        tactical::CommandPayload{tactical::ReinforcePayload{1, {}, 99}},
        tactical::CommandPayload{tactical::SpecialFirePayload{10, tactical::SpecialFireAdmission::ai_object}},
        tactical::CommandPayload{tactical::SpecialFirePayload{10, tactical::SpecialFireAdmission::ai_taskforce}}}) {
        auto privileged = intent;
        privileged.payload = payload;
        expect(!tactical::admit_human_command(binding, privileged), "human admission refuses every engine-only entry route");
    }
    tactical::TacticalSetup setup;
    setup.players = {{1, 0, 1, tactical::player_flag_commandable}, {2, 0, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 1, 2}};
    auto created = tactical::TacticalSession::create(setup);
    expect(static_cast<bool>(created), "seat admission fixture creates");
    if (!created) return;
    auto& world = created.value();
    auto routed = world.submit_human(binding, intent, 7);
    expect(routed && routed.value().issuer == intent.key && routed.value().command.key.sequence == 7,
        "session boundary binds issuer and returns original seat key beside derived child sequence");
    auto privileged = intent;
    privileged.key.sequence = 1;
    privileged.payload = tactical::DamagePayload{};
    expect(!world.submit_human(binding, privileged, 8), "session boundary rejects privileged human order");
    const auto stepped = world.step(eawr::sim::InlineExecutor{});
    expect(static_cast<bool>(stepped), "admitted faction command executes");
    expect(world.record().commands.size() == 1, "refused human command never enters replay stream");
    tactical::PlayerCommand trusted{{1, 2, 8}, {10}, tactical::DamagePayload{}};
    expect(static_cast<bool>(world.submit(trusted)), "engine/replay path remains available for privileged payloads");
}
} // namespace

namespace {
void test_beacons() {
    using eawr::sim::math::Fixed;
    tactical::TacticalSetup setup;
    setup.players = {{1, 7, 1, tactical::player_flag_commandable},
        {2, 7, 1, tactical::player_flag_commandable}, {3, 8, 2, tactical::player_flag_commandable}};
    setup.beacon_rules = {{1, 101, 3}, {2, 102, 5}};
    const eawr::sim::math::Vec3 first{Fixed::from_raw(Fixed::scale), {}, {}};
    const eawr::sim::math::Vec3 replacement{Fixed::from_raw(5 * Fixed::scale), {}, {}};
    tactical::TacticalReplay replay{setup, 8, {
        {{0, 1, 0}, {}, tactical::BeaconPayload{first}},
        {{1, 2, 0}, {}, tactical::BeaconPayload{}},
        {{2, 1, 1}, {}, tactical::BeaconPayload{replacement}}}};
    const auto bytes = tactical::write_replay(replay);
    expect(static_cast<bool>(bytes), "HCB-12: beacon rules and position commands encode");
    if (!bytes) return;
    const auto decoded = tactical::parse_replay(bytes.value());
    expect(decoded && decoded.value() == replay, "HCB-12: beacon setup and commands round-trip exactly");
    if (!decoded) return;
    auto bad_extension = bytes.value();
    bad_extension[112] = 2; // This fixture has only extension 9; its body begins here.
    expect(!tactical::parse_replay(bad_extension), "unknown beacon rules versions reject");
    bad_extension = bytes.value();
    bad_extension[116] = 65;
    expect(!tactical::parse_replay(bad_extension), "beacon rules cannot exceed the player bound");
    auto bad_units = replay;
    bad_units.commands.front().units = {1};
    expect(!tactical::write_replay(bad_units), "beacons never accept a forged unit list");
    auto bad_position = replay;
    std::get<tactical::BeaconPayload>(bad_position.commands.front().payload).position.x = Fixed::from_raw(std::int64_t{1} << 43);
    expect(!tactical::write_replay(bad_position), "beacon points retain command coordinate bounds");
    auto bad_rules = setup;
    bad_rules.beacon_rules.front().lifetime_frames = 0;
    expect(!tactical::validate_setup(bad_rules), "beacon lifetime must be positive");
    bad_rules = setup;
    bad_rules.beacon_rules.push_back(bad_rules.beacon_rules.front());
    expect(!tactical::validate_setup(bad_rules), "beacon rules cannot duplicate an issuer");
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto made = tactical::TacticalSession::from_replay(decoded.value());
        expect(static_cast<bool>(made), "beacon replay creates a session");
        if (!made) continue;
        auto world = std::move(made).value();
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) {
            const auto advanced = world.step(ReverseInline{workers});
            expect(static_cast<bool>(advanced), "beacons step on every worker count");
            if (!advanced) break;
            hashes.push_back(world.state_sha256());
            const auto own = world.snapshot()->visible_beacons(1);
            const auto ally = world.snapshot()->visible_beacons(2);
            expect(own == ally && world.snapshot()->visible_beacons(3).empty()
                && world.snapshot()->visible_beacons(99).empty(), "HCB-12c: only known allies see beacon records");
            if (tick == 0) expect(own.size() == 1 && own.front().expires_tick == 3, "authored lifetime begins at the placement tick");
            if (tick == 2) expect(own.size() == 2 && own.front().position == replacement
                && own.front().sequence == 1 && own.front().expires_tick == 5,
                "HCB-12b: replacement retains the other issuer and refreshes lifetime");
            if (tick == 5) expect(own.size() == 1 && own.front().player == 2, "replacement expires at its exact command-tick deadline");
            if (tick == 6) expect(own.empty(), "independent issuer deadline expires independently");
        }
        if (workers == 1) reference = hashes;
        else expect(hashes == reference, "beacon hashes equal on 1/2/4/8 workers");
        expect(world.record() == replay, "beacons remain in canonical replay command order");
    }
    auto made = tactical::TacticalSession::create(setup);
    if (!made) return;
    auto world = std::move(made).value();
    tactical::SeatBinding binding{{1}, 1};
    tactical::SeatCommand seat{{0, {1}, 0}, 1, {}, tactical::BeaconPayload{first}};
    expect(static_cast<bool>(world.submit_human(binding, seat, 0)), "beacons use trusted human seat admission");
    const auto before = world.canonical_state_bytes();
    expect(!world.step(FailingExecutor{}) && world.canonical_state_bytes() == before,
        "a failed tick commits no beacon state");
    expect(static_cast<bool>(world.step(eawr::sim::InlineExecutor{})), "failed beacon tick can be retried");
    expect(world.snapshot()->visible_beacons(1).size() == 1, "retry publishes one beacon");
    auto different = tactical::TacticalSession::create(setup);
    if (different) {
        auto other = std::move(different).value();
        auto different_seat = seat;
        different_seat.payload = tactical::BeaconPayload{replacement};
        expect(static_cast<bool>(other.submit_human(binding, different_seat, 0)),
            "alternate beacon position admits");
        expect(static_cast<bool>(other.step(eawr::sim::InlineExecutor{})), "alternate beacon position steps");
        expect(other.state_sha256() != world.state_sha256(), "canonical state includes active beacon position");
    }
    auto unavailable = tactical::TacticalSession::create(setup);
    if (unavailable) {
        auto other = std::move(unavailable).value();
        expect(static_cast<bool>(other.submit({{0, 3, 0}, {}, tactical::BeaconPayload{first}})),
            "wire admits a declared player before rules reject unavailable beacons");
        const auto advanced = other.step(eawr::sim::InlineExecutor{});
        expect(advanced && std::any_of(advanced.value().snapshot->events().begin(), advanced.value().snapshot->events().end(),
            [](const auto& event) { return event.kind == tactical::EventKind::order_rejected
                && event.reason == tactical::RejectReason::beacon_unavailable; }),
            "a faction without a beacon rule gets the allocated rejection");
        expect(other.snapshot()->visible_beacons(3).empty(), "rejected beacons create no presentation record");
    }
}
void test_controller_split_and_gates() {
    const auto population_rules = [] {
        tactical::EconomyRules rules;
        rules.campaign = true;
        rules.campaign_population = {{11, 8}, {12, 8}, {13, 2}, {14, 0}, {15, 0}, {16, 1}, {17, 3},
            {18, 100}, {19, 100}, {20, 0}, {99, 9}};
        return rules;
    };
    for (const auto humans : {2U, 3U, 4U}) {
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, 1}, {2, 2, 2, 1}, {3, 3, 3, 0}};
        setup.first_free_entity_id = 50;
        setup.campaign.emplace();
        setup.controllers.emplace();
        for (const auto player : {1U, 2U}) {
            tactical::EconomyPlayer account; account.player = player; account.credit_cap = eawr::sim::math::Fixed{};
            setup.campaign->players.push_back(account);
            setup.controllers->factions.push_back({player, tactical::FactionController::humans});
            for (std::uint32_t seat = 1; seat <= humans; ++seat)
                setup.controllers->seats.push_back({{(player - 1) * humans + seat}, player, eawr::sim::SeatKind::human, true});
            for (std::uint64_t id = 1; id <= 10; ++id)
                setup.units.push_back({id + (player - 1) * 20, 10 + id, player});
            setup.units.back().control = tactical::UnitControl::shared_structure;
            setup.units[setup.units.size() - 5].purchase_type = 99; // transformed hero sorts by loaded purchase population
            setup.squadrons.push_back({7 + (player - 1) * 20, {8 + (player - 1) * 20, 9 + (player - 1) * 20}});
        }
        setup.units.push_back({45, 11, 3}); // scenery is not a commandable side
        auto rules = population_rules(); rules.players = setup.campaign->players;
        const auto initial = setup;
        expect(static_cast<bool>(tactical::split_initial_controllers(setup, rules)), "initial split succeeds for 2/3/4 humans on each side");
        const std::array<std::uint64_t, 7> rank{6, 1, 2, 7, 3, 4, 5};
        for (const auto player : {1U, 2U}) {
            for (std::size_t i = 0; i < rank.size(); ++i) {
                const auto id = rank[i] + (player - 1) * 20;
                const auto unit = std::find_if(setup.units.begin(), setup.units.end(), [id](const auto& row) { return row.entity_id == id; });
                expect(unit->controller_seat.value == (player - 1) * humans + 1 + i % humans,
                    "population descending, stable-ID ties and zero-population tail are dealt round-robin");
            }
            const auto offset = (player - 1) * 10;
            expect(setup.units[offset + 6].controller_seat == setup.units[offset + 7].controller_seat
                && setup.units[offset + 6].controller_seat == setup.units[offset + 8].controller_seat,
                "authored root and attached craft are indivisible despite member populations");
            expect(setup.units[offset + 9].control == tactical::UnitControl::shared_structure
                && setup.units[offset + 9].controller_seat.value == 0, "structures do not consume a deal slot");
        }
        expect(setup.units.back().control == tactical::UnitControl::none, "neutral scenery is excluded");
        auto scrambled = initial; std::reverse(scrambled.units.begin(), scrambled.units.end());
        expect(tactical::split_initial_controllers(scrambled, rules) && scrambled == setup,
            "initial storage order does not affect final controller setup");
        auto disabled = initial; disabled.controllers.reset();
        for (auto& unit : disabled.units) { unit.control = tactical::UnitControl::none; unit.controller_seat = {}; }
        const auto before = tactical::write_replay({disabled, 0, {}});
        expect(tactical::split_initial_controllers(disabled, rules)
            && tactical::write_replay({disabled, 0, {}}).value() == before.value(),
            "disabled policy keeps setup and replay bytes unchanged");
    }
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
    setup.units = {{1, 11, 1}, {2, 11, 1}, {3, 11, 2}, {4, 12, 1}};
    setup.first_free_entity_id = 5; setup.campaign.emplace();
    for (const auto player : {1U, 2U}) { tactical::EconomyPlayer account; account.player = player; account.credit_cap = eawr::sim::math::Fixed{}; setup.campaign->players.push_back(account); }
    setup.controllers = tactical::BattleControllerPolicy{
        {{{1}, 1, eawr::sim::SeatKind::human, true}, {{2}, 1, eawr::sim::SeatKind::human, true},
            {{3}, 2, eawr::sim::SeatKind::ai, true}},
        {{1, tactical::FactionController::humans}, {2, tactical::FactionController::ai}}};
    for (std::size_t i = 0; i < 3; ++i) { setup.units[i].control = tactical::UnitControl::mobile; setup.units[i].controller_seat = {static_cast<std::uint32_t>(i + 1)}; }
    setup.units.back().control = tactical::UnitControl::shared_structure;
    auto economy = population_rules(); economy.players = setup.campaign->players;
    auto passive = setup;
    passive.controllers->seats.pop_back();
    passive.controllers->factions.back().mode = tactical::FactionController::passive;
    passive.units[2].control = tactical::UnitControl::shared_structure; passive.units[2].controller_seat = {};
    expect(static_cast<bool>(tactical::split_initial_controllers(passive, economy)),
        "a passive side containing only shared structures needs no mobile controller seat");
    passive.units[2].control = tactical::UnitControl::mobile;
    expect(!tactical::split_initial_controllers(passive, economy),
        "a passive side with a mobile root refuses a missing configured controller");
    auto installed = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
    expect(static_cast<bool>(installed), "initial role installation fixture creates");
    if (installed) {
        auto policy = *setup.controllers; policy.seats[1].active = false;
        expect(static_cast<bool>(installed.value().set_initial_controller_policy(policy)), "held tick-zero role installs its effective roster");
        auto replayed = tactical::TacticalSession::from_replay(installed.value().record(), {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(replayed && replayed.value().state_sha256() == installed.value().state_sha256(),
            "role installation refreshes the campaign setup binding used by standalone replay");
        const auto saved = installed.value().save_checkpoint_state();
        expect(saved && replayed && replayed.value().load_checkpoint_state(saved.value())
            && replayed.value().state_sha256() == installed.value().state_sha256()
            && replayed.value().snapshot()->canonical_bytes() == installed.value().snapshot()->canonical_bytes(),
            "role installation and checkpoint reconstruction agree at tick zero without a second split");
    }
    const std::vector<tactical::CommandPayload> families{
        tactical::StopPayload{}, tactical::MovePayload{}, tactical::FacePayload{}, tactical::AttackPayload{3},
        tactical::AttackMovePayload{{}, 3}, tactical::GuardPayload{{}, 2},
        tactical::AbilityPayload{tactical::AbilityKind::defend},
        tactical::AreaAbilityPayload{tactical::AbilityKind::barrage}, tactical::ManualTargetPayload{3, 0},
        tactical::SpecialFirePayload{3}, tactical::BuyPayload{11}, tactical::PadBuildPayload{11}, tactical::PadSellPayload{}};
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool scramble : {false, true}) {
        std::string hashes;
        for (std::size_t family = 0; family < families.size(); ++family) {
            auto made = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "controller gate fixture creates");
            if (!made) continue;
            auto world = std::move(made).value();
            if (scramble) world.scramble_storage_for_testing();
            tactical::PlayerCommand forged{{0, 1, 1}, {2}, families[family]};
            forged.origin = tactical::CommandOrigin::seat; forged.issuing_seat = {1};
            auto own = forged; own.key.sequence = 2; own.units.front() = 1;
            expect(world.submit(forged) && world.submit(own), "well-shaped same-faction orders reach execution authority");
            const auto tick = world.step(ReverseInline(workers));
            expect(static_cast<bool>(tick), "controller gate family advances");
            if (!tick) continue;
            const auto& events = tick.value().snapshot->events();
            const auto rejected = std::find_if(events.begin(), events.end(), [](const auto& row) { return row.sequence == 1 && row.kind == tactical::EventKind::order_rejected; });
            expect(rejected != events.end() && rejected->reason == tactical::RejectReason::unit_not_controlled,
                "every mobile opcode family rejects a forged teammate source");
            const auto accepted = std::find_if(events.begin(), events.end(), [](const auto& row) {
                return row.sequence == 2 && (row.kind == tactical::EventKind::order_accepted || row.kind == tactical::EventKind::order_rejected);
            });
            expect(accepted != events.end() && accepted->reason != tactical::RejectReason::unit_not_controlled,
                "own source keeps the opcode's existing admission");
            if (family < 6) expect(accepted != events.end() && accepted->kind == tactical::EventKind::order_accepted,
                "own movement, attack and allied guard orders are accepted");
            hashes += tick.value().state_hash.get() + tick.value().snapshot->sha256();
        }
        auto world = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        if (!world) continue;
        tactical::PlayerCommand engine_forgery{{0, 1, 1}, {2}, tactical::DamagePayload{eawr::sim::math::Fixed::from_integer(1).value()}};
        engine_forgery.origin = tactical::CommandOrigin::seat; engine_forgery.issuing_seat = {1};
        expect(!world.value().submit(engine_forgery), "human provenance cannot turn an engine opcode into a controller bypass");
        std::uint64_t sequence{};
        for (const auto seat : {1U, 2U}) {
            tactical::PlayerCommand command{{0, 1, ++sequence}, {4}, tactical::StopPayload{}};
            command.origin = tactical::CommandOrigin::seat; command.issuing_seat = {seat};
            expect(static_cast<bool>(world.value().submit(command)), "every teammate may order the shared structure");
        }
        tactical::PlayerCommand mixed{{0, 1, ++sequence}, {1, 2}, tactical::StopPayload{}};
        mixed.origin = tactical::CommandOrigin::seat; mixed.issuing_seat = {1};
        expect(static_cast<bool>(world.value().submit(mixed)), "mixed own/teammate selection admits per-unit execution");
        const auto tick = world.value().step(ReverseInline(workers));
        expect(tick && std::count_if(tick.value().snapshot->events().begin(), tick.value().snapshot->events().end(), [](const auto& row) {
            return row.kind == tactical::EventKind::order_accepted;
        }) == 3, "both shared orders and own mixed member execute");
        expect(tick && std::count_if(tick.value().snapshot->events().begin(), tick.value().snapshot->events().end(), [](const auto& row) {
            return row.reason == tactical::RejectReason::unit_not_controlled;
        }) == 1, "only the teammate member of a mixed selection is refused");
        auto policy = *setup.controllers;
        expect(!world.value().set_initial_controller_policy(policy), "a running child can never re-split");
        hashes += world.value().state_sha256();
        if (reference.empty()) reference = hashes;
        expect(hashes == reference, "all command gate hashes and snapshots match at 1/2/4/8 workers and scrambled storage");
    }
}

void test_controller_shared_special_fire() {
    const auto whole = [](const std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, 1}, {2, 2, 2, 1}, {3, 1, 3, 0}};
    setup.units = {{1, 11, 1}, {2, 11, 2, {whole(100), {}, {}}}, {3, 12, 1}, {4, 12, 3}};
    setup.first_free_entity_id = 5; setup.campaign.emplace();
    setup.controllers = tactical::BattleControllerPolicy{
        {{{1}, 1, eawr::sim::SeatKind::human, true}, {{2}, 1, eawr::sim::SeatKind::human, true},
            {{3}, 2, eawr::sim::SeatKind::ai, true}},
        {{1, tactical::FactionController::humans}, {2, tactical::FactionController::ai}}};
    setup.units[0].control = tactical::UnitControl::mobile; setup.units[0].controller_seat = {1};
    setup.units[1].control = tactical::UnitControl::mobile; setup.units[1].controller_seat = {3};
    setup.units[2].control = tactical::UnitControl::shared_structure;
    for (const auto player : {1U, 2U}) {
        tactical::EconomyPlayer account; account.player = player; account.credit_cap = whole(0);
        setup.campaign->players.push_back(account);
    }
    setup.campaign->upgrades = {{90, 3, 1}};
    tactical::EconomyRules economy; economy.campaign = true; economy.players = setup.campaign->players;
    tactical::UpgradeProfile unlock; unlock.type = 90;
    tactical::UpgradeProfile::SpecialUnlock enabled; enabled.all_allies = true;
    unlock.special_unlocks.push_back(enabled);
    economy.upgrades.push_back(unlock);
    tactical::CombatTable combat;
    tactical::CombatProfile target; target.type_id = 11; target.category_bits = 1;
    tactical::CombatProfile station; station.type_id = 12; station.star_base = true;
    tactical::WeaponProfile gun; gun.hardpoint = 0; gun.range = whole(1000);
    gun.cone_width = whole(360); gun.cone_height = whole(180); gun.requires_manual_target = gun.special = true;
    gun.shot.emplace(); gun.shot->speed = whole(1); gun.shot->max_travel = whole(1000);
    station.weapons.push_back(gun); combat.profiles = {target, station};
    const std::vector<tactical::SensorProfile> sensors{{12, whole(1000)}};
    for (const bool controllers : {false, true}) {
        for (const bool permission : {false, true}) {
            auto fixture = setup;
            if (!controllers) {
                fixture.controllers.reset();
                for (auto& unit : fixture.units) { unit.control = tactical::UnitControl::none; unit.controller_seat = {}; }
            }
            auto rules = economy;
            rules.upgrades.front().special_unlocks.front().reverse = !permission;
            std::string reference;
            for (const auto workers : {1U, 2U, 4U, 8U}) {
                std::string hashes;
                for (const auto seat : {1U, 2U}) {
                    auto made = tactical::TacticalSession::create(fixture, sensors, {}, {}, std::nullopt, combat, {}, {}, rules);
                    expect(static_cast<bool>(made), "shared special-fire fixture creates");
                    if (!made) continue;
                    auto world = std::move(made).value();
                    expect(world.snapshot()->economy().front().special_weapons_enabled == permission,
                    "only a non-reversed held unlock enables shared special fire with controllers on and off");
                    std::uint64_t sequence{};
                    for (const auto source : {3ULL, 4ULL}) {
                        tactical::PlayerCommand command{{0, 1, ++sequence}, {source}, tactical::SpecialFirePayload{2}};
                        if (controllers) { command.origin = tactical::CommandOrigin::seat; command.issuing_seat = {seat}; }
                        expect(static_cast<bool>(world.submit(command)), "both teammates submit own and allied shared-structure special fire");
                    }
                    const auto tick = world.step(ReverseInline(workers));
                    expect(tick && std::count_if(tick.value().snapshot->events().begin(), tick.value().snapshot->events().end(), [](const auto& event) {
                        return event.order == tactical::OrderKind::special_fire && event.kind == tactical::EventKind::order_accepted;
                    }) == (permission ? 2 : 0), "only an enabled unlock accepts own and allied shared-structure special fire with controllers on and off");
                    expect(tick && std::count_if(tick.value().snapshot->events().begin(), tick.value().snapshot->events().end(), [](const auto& event) {
                        return event.order == tactical::OrderKind::special_fire && event.kind == tactical::EventKind::order_rejected
                        && event.reason == tactical::RejectReason::ability_unavailable;
                    }) == (permission ? 0 : 2), "a reversed unlock refuses own and allied shared-structure special fire with controllers on and off");
                    hashes += world.state_sha256();
                }
                if (reference.empty()) reference = hashes;
                expect(hashes == reference, "shared and allied special-fire controllers agree on 1/2/4/8 workers");
            }
        }
    }
}

void test_controller_gifts() {
    using eawr::sim::SeatId;
    using eawr::sim::SeatKind;
    for (const auto humans : {2U, 3U, 4U}) {
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
        setup.units = {{1, 11, 1}, {2, 12, 1}, {3, 13, 1}, {4, 14, 1}, {5, 11, 2}, {6, 11, 1}};
        setup.first_free_entity_id = 7;
        setup.squadrons = {{1, {2, 3}}};
        setup.campaign.emplace(); setup.controllers.emplace();
        for (const auto owner : {1U, 2U}) {
            tactical::EconomyPlayer account; account.player = owner;
            account.credit_cap = eawr::sim::math::Fixed{}; account.population_cap = 20;
            setup.campaign->players.push_back(account);
        }
        for (std::uint32_t seat = 1; seat <= humans; ++seat)
            setup.controllers->seats.push_back({{seat}, 1, SeatKind::human, true});
        setup.controllers->seats.push_back({{humans + 1}, 2, SeatKind::human, true});
        setup.controllers->seats.push_back({{humans + 2}, 1, SeatKind::human, false});
        setup.controllers->seats.push_back({{humans + 3}, 1, SeatKind::ai, true});
        setup.controllers->factions = {{1, tactical::FactionController::humans}, {2, tactical::FactionController::humans}};
        for (auto& unit : setup.units) { unit.control = tactical::UnitControl::mobile; unit.controller_seat = {1}; }
        setup.units[3].control = tactical::UnitControl::shared_structure; setup.units[3].controller_seat = {};
        setup.units[4].controller_seat = {humans + 1};
        setup.units[5].controller_seat = {2};
        tactical::EconomyRules economy; economy.campaign = true; economy.players = setup.campaign->players;
        const auto command = [](const std::uint64_t tick, const std::uint64_t sequence, const std::uint32_t sender,
                                std::vector<eawr::sim::EntityId> units, tactical::CommandPayload payload) {
            tactical::PlayerCommand result{{tick, 1, sequence}, std::move(units), std::move(payload)};
            result.origin = tactical::CommandOrigin::seat; result.issuing_seat = {sender};
            return result;
        };
        const auto gift = command(0, 2, 1, {1, 2, 3}, tactical::GiftPayload{{2}});
        const auto encoded = tactical::encode_command_body(gift, setup);
        expect(encoded && encoded.value().size() == 80 && encoded.value()[20] == 32,
            "gift body retains common header and uses opcode 32 with recipient, list and provenance");
        const auto decoded = encoded ? tactical::decode_command_body(encoded.value(), setup)
            : eawr::core::Result<tactical::PlayerCommand>::failure(encoded.error());
        expect(decoded && decoded.value() == gift, "gift body round-trips without losing authenticated sender");
        if (encoded) {
            for (std::size_t length = 0; length < encoded.value().size(); ++length)
                expect(!tactical::decode_command_body(std::span(encoded.value()).first(length), setup),
                    "every truncated gift body fails, including truncated provenance");
            auto corrupt = encoded.value(); corrupt[28] = 1;
            expect(!tactical::decode_command_body(corrupt, setup), "gift reserved word is validated");
            corrupt = encoded.value(); corrupt[24] = 0;
            expect(!tactical::decode_command_body(corrupt, setup), "gift zero recipient is invalid");
            corrupt = encoded.value(); corrupt[32] = 1; corrupt[33] = 4;
            expect(!tactical::decode_command_body(corrupt, setup), "gift excessive unit count is bounded before allocation");
        }
        auto invalid_shape = gift; invalid_shape.units.clear();
        expect(!tactical::encode_command_body(invalid_shape, setup), "gift empty selection is refused");
        invalid_shape = gift; invalid_shape.units = {1, 1};
        expect(!tactical::encode_command_body(invalid_shape, setup), "gift duplicate roots are noncanonical");
        invalid_shape = gift; invalid_shape.units = {3, 1};
        expect(!tactical::encode_command_body(invalid_shape, setup), "gift unsorted list is noncanonical");
        auto engine = gift; engine.origin = tactical::CommandOrigin::engine; engine.issuing_seat = {};
        expect(!tactical::encode_command_body(engine, setup), "engine authority cannot invent human gifts");
        auto disabled = setup; disabled.controllers.reset();
        for (auto& unit : disabled.units) { unit.control = tactical::UnitControl::none; unit.controller_seat = {}; }
        auto ordinary = gift; ordinary.origin = tactical::CommandOrigin::ordinary; ordinary.issuing_seat = {};
        expect(!tactical::encode_command_body(ordinary, disabled), "gift requires explicitly enabled policy");
        if (encoded) expect(!tactical::decode_command_body(encoded.value(), disabled), "disabled child rejects gift bytes");
        std::vector<std::string> reference;
        for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool scramble : {false, true}) {
            auto made = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "gift fixture enables campaign controller policy");
            if (!made) continue;
            auto world = std::move(made).value();
            const auto accounts = world.ledgers();
            const auto before_economy = std::vector<tactical::PlayerEconomy>(accounts.begin(), accounts.end());
            const auto before_instances = world.snapshot()->instances();
            std::vector<std::uint64_t> visibility;
            for (const auto& instance : before_instances) visibility.push_back(instance.visible_to);
            expect(static_cast<bool>(world.submit(command(0, 1, 1, {1}, tactical::StopPayload{}))), "order before gift submits");
            auto selected_gift = gift;
            if (scramble) selected_gift.units = {2};
            expect(static_cast<bool>(world.submit(selected_gift)), "root selections and member-only selections submit");
            expect(static_cast<bool>(world.submit(command(0, 3, 1, {1}, tactical::MovePayload{}))), "former owner order queues for execution check");
            const auto final_seat = humans >= 3 ? 3U : 1U;
            expect(static_cast<bool>(world.submit(command(1, 4, 2, {1}, tactical::StopPayload{}))), "recipient orders before gifting onward");
            expect(static_cast<bool>(world.submit(command(1, 5, 2, {1}, tactical::GiftPayload{{final_seat}}))), "recipient can gift onward without history");
            if (humans >= 3) expect(static_cast<bool>(world.submit(command(2, 6, 3, {1}, tactical::GiftPayload{{1}}))), "third recipient can gift to original sender");
            expect(static_cast<bool>(world.submit(command(3, 7, 1, {1}, tactical::StopPayload{}))), "original sender can command returned gift without cooldown");
            const ReverseInline executor(workers);
            if (scramble) world.scramble_storage_for_testing();
            auto tick = world.step(executor);
            expect(static_cast<bool>(tick), "ordered gift tick advances");
            if (!tick) { std::cerr << tick.error().message << '\n'; continue; }
            const auto instances = world.snapshot()->instances();
            expect(instances.size() == setup.units.size() && instances[0].controller_seat == SeatId{2}
                && instances[1].controller_seat == SeatId{2} && instances[2].controller_seat == SeatId{2},
                "immutable snapshot switches complete root/member closure once");
            const auto after_economy = world.ledgers();
            expect(std::equal(before_economy.begin(), before_economy.end(), after_economy.begin(), after_economy.end()),
                "gift preserves shared faction credits, queues, reserves and population accounts");
            for (std::size_t i = 0; i < instances.size() && i < visibility.size() && i < setup.units.size(); ++i)
                expect(instances[i].visible_to == visibility[i] && instances[i].owner == setup.units[i].owner,
                    "gift leaves faction ownership and faction visibility unchanged");
            const auto live = world.units();
            expect(!live.empty() && live.front().order.kind == tactical::OrderKind::stop && live.front().order.issued_tick == 0,
                "gift preserves the accepted current order and refuses the former controller's later move");
            const auto events = world.snapshot()->events();
            expect(std::count_if(events.begin(), events.end(), [](const auto& event) {
                return event.kind == tactical::EventKind::controller_changed && event.previous_controller == SeatId{1}
                    && event.controller == SeatId{2};
            }) == 3, "controller events include previous and current seats for each transferred live identity");
            expect(std::count_if(events.begin(), events.end(), [](const auto& event) {
                return event.kind == tactical::EventKind::order_rejected && event.reason == tactical::RejectReason::unit_not_controlled;
            }) == 1, "same-tick gift revokes former controller immediately");
            const auto changed = std::find_if(events.begin(), events.end(), [](const auto& row) { return row.kind == tactical::EventKind::controller_changed; });
            if (changed != events.end()) {
                auto event = *changed; event.controller = {1};
                expect(event != *changed, "controller event equality includes the recipient seat");
            }
            const auto saved = world.save_checkpoint_state();
            auto log = world.record(); const auto pending = world.pending_commands();
            log.commands.insert(log.commands.end(), pending.begin(), pending.end()); log.final_tick_count = 4;
            const auto bytes = tactical::write_replay(log);
            const auto parsed = bytes ? tactical::parse_replay(bytes.value())
                : eawr::core::Result<tactical::TacticalReplay>::failure(bytes.error());
            expect(parsed && parsed.value() == log, "standalone child log retains executed and pending gifts");
            if (!saved || !parsed) continue;
            auto fresh = tactical::TacticalSession::from_replay(parsed.value(), {}, {}, {}, std::nullopt, {}, {}, {}, economy);
            expect(static_cast<bool>(fresh), "gift checkpoint factory accepts the sealed child log");
            if (!fresh) continue;
            const auto loaded = fresh.value().load_checkpoint_state(saved.value());
            expect(static_cast<bool>(loaded), "gift checkpoint restores current controller and pending onward gifts");
            if (!loaded) continue;
            expect(fresh.value().state_sha256() == world.state_sha256(), "restored gift state is immediately equal");
            std::vector<std::string> hashes{world.state_sha256() + world.snapshot()->sha256()};
            for (unsigned frame = 1; frame < 4; ++frame) {
                if (scramble) world.scramble_storage_for_testing();
                const auto advanced = world.step(executor); const auto restored = fresh.value().step(executor);
                expect(advanced && restored && advanced.value().state_sha256 == restored.value().state_sha256
                    && advanced.value().snapshot->canonical_bytes() == restored.value().snapshot->canonical_bytes(),
                    "gift continuation equals live, including all events, after checkpoint restore");
                if (!advanced || !restored) break;
                hashes.push_back(world.state_sha256() + world.snapshot()->sha256());
            }
            const auto final_units = world.units();
            expect(!final_units.empty() && final_units.front().controller_seat == SeatId{1}, "A to B to C to A needs no reclaim or transfer history");
            auto child = tactical::TacticalSession::from_replay(world.record(), {}, {}, {}, std::nullopt, {}, {}, {}, economy);
            expect(static_cast<bool>(child), "completed gift child replay creates");
            if (child && hashes.size() == 4) {
                for (unsigned frame = 0; frame < 4; ++frame) {
                    const auto advanced = child.value().step(executor);
                    expect(advanced && child.value().state_sha256() + child.value().snapshot()->sha256() == hashes[frame],
                        "standalone child replay equals live gift state and snapshot at every tick");
                    if (!advanced) break;
                }
            }
            if (reference.empty()) reference = hashes;
            expect(hashes == reference, "2/3/4-human gifts match at 1/2/4/8 workers with scrambled storage");
            // Invalid recipients and mixed source lists never transfer a valid subset.
            const std::vector<tactical::PlayerCommand> refused{
                command(0, 1, 1, {1}, tactical::GiftPayload{{1}}),
                command(0, 1, 1, {1}, tactical::GiftPayload{{humans + 1}}),
                command(0, 1, 1, {1}, tactical::GiftPayload{{humans + 2}}),
                command(0, 1, 1, {1}, tactical::GiftPayload{{humans + 3}}),
                command(0, 1, 1, {1}, tactical::GiftPayload{{humans + 4}}),
                command(0, 1, 1, {1, 4}, tactical::GiftPayload{{2}}),
                command(0, 1, 1, {1, 5}, tactical::GiftPayload{{2}}),
                command(0, 1, 1, {1, 6}, tactical::GiftPayload{{2}}),
                command(0, 1, 1, {1, 7}, tactical::GiftPayload{{2}})};
            for (const auto& request : refused) {
                auto rejected = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
                expect(static_cast<bool>(rejected), "invalid gift fixture creates with enabled controller policy");
                if (!rejected) continue;
                expect(static_cast<bool>(rejected.value().submit(request)), "dynamic gift eligibility is evaluated in command order");
                const auto denied = rejected.value().step(executor);
                expect(denied && denied.value().snapshot->events().size() == 1
                    && denied.value().snapshot->events().front().kind == tactical::EventKind::order_rejected,
                    "self, enemy, inactive, AI, absent spectator, structure, foreign, teammate and stale source gifts reject atomically");
                const auto units = rejected.value().units();
                expect(units.size() == setup.units.size() && std::equal(units.begin(), units.end(), setup.units.begin(),
                    [](const auto& a, const auto& b) { return a.owner == b.owner && a.controller_seat == b.controller_seat
                        && a.control == b.control && a.order == b.order; }), "invalid gift leaves every source and its orders unchanged");
            }
        }
    }
}


void test_controller_departures() {
    tactical::TacticalReplay log;
    log.final_tick_count = 4;
    auto& setup = log.setup;
    setup.first_free_entity_id = 8;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.campaign.emplace();
    for (const auto player : {1U, 2U}) {
        tactical::EconomyPlayer account; account.player = player;
        account.credit_cap = eawr::sim::math::Fixed{};
        setup.campaign->players.push_back(account);
    }
    setup.controllers = tactical::BattleControllerPolicy{
        {{{1}, 1, eawr::sim::SeatKind::human, true}, {{2}, 1, eawr::sim::SeatKind::human, true},
         {{3}, 1, eawr::sim::SeatKind::human, true}, {{10}, 1, eawr::sim::SeatKind::ai, false},
         {{20}, 2, eawr::sim::SeatKind::ai, true}},
        {{1, tactical::FactionController::humans}, {2, tactical::FactionController::ai}}};
    for (std::uint32_t id = 1; id <= 7; ++id) {
        tactical::UnitState unit{id, 11, 1};
        unit.control = id == 7 ? tactical::UnitControl::shared_structure : tactical::UnitControl::mobile;
        unit.controller_seat = {id <= 3 ? 1U : id == 4 ? 2U : id <= 6 ? 3U : 0U};
        setup.units.push_back(unit);
    }
    const auto seat_order = [](std::uint64_t tick, std::uint64_t seq, std::uint32_t seat,
        std::vector<eawr::sim::EntityId> units, tactical::CommandPayload payload) {
        tactical::PlayerCommand command{{tick, 1, seq}, std::move(units), std::move(payload)};
        command.origin = tactical::CommandOrigin::seat; command.issuing_seat = {seat}; return command;
    };
    auto roster = *setup.controllers;
    const auto leave = [&](std::uint64_t tick, std::uint64_t seq, std::uint32_t seat, bool last) {
        tactical::BattleControlPayload change; change.departed = {seat};
        for (auto& row : roster.seats) if (row.player == 1) {
            if (row.seat.value == seat) row.active = false;
            if (row.kind == eawr::sim::SeatKind::ai) row.active = last;
            change.policy.seats.push_back(row);
        }
        change.policy.factions.push_back({1, last ? tactical::FactionController::passive : tactical::FactionController::humans});
        tactical::PlayerCommand command{{tick, 1, seq}, {}, std::move(change)};
        command.origin = tactical::CommandOrigin::engine; return command;
    };
    log.commands.push_back(seat_order(0, 1, 1, {1}, tactical::StopPayload{}));
    log.commands.push_back(seat_order(0, 2, 1, {2}, tactical::GiftPayload{{2}}));
    log.commands.push_back(leave(0, 3, 1, false));
    log.commands.push_back(seat_order(0, 4, 1, {3}, tactical::StopPayload{}));
    log.commands.push_back(seat_order(0, 5, 3, {3}, tactical::StopPayload{}));
    log.commands.push_back(leave(1, 6, 2, false));
    log.commands.push_back(leave(2, 7, 3, true));
    auto bytes = tactical::write_replay(log);
    expect(bytes && tactical::parse_replay(bytes.value()).value() == log, "departure codec preserves effective side roster");
    auto forged = log; forged.commands[2].origin = tactical::CommandOrigin::seat; forged.commands[2].issuing_seat = {1};
    expect(!tactical::validate_replay(forged), "human provenance cannot forge an engine departure");
    auto duplicate = log; duplicate.commands[5] = log.commands[2]; duplicate.commands[5].key = {1, 1, 6};
    expect(!tactical::validate_replay(duplicate), "repeated departure cannot reactivate or reclaim a seat");
    const auto body = tactical::encode_command_body(log.commands[2], setup);
    expect(static_cast<bool>(body), "bounded departure body encodes");
    if (body) {
        auto truncated = body.value(); truncated.pop_back();
        expect(!tactical::decode_command_body(truncated, setup), "truncated departure roster/provenance rejects");
        auto excessive = body.value(); excessive[32] = 255; excessive[33] = 255;
        expect(!tactical::decode_command_body(excessive, setup), "oversized departure roster rejects before allocation");
    }
    tactical::EconomyRules economy; economy.campaign = true; economy.players = setup.campaign->players;
    std::string expected;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto session = tactical::TacticalSession::from_replay(log, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(session), "departure world creates"); if (!session) continue;
        const auto first = session.value().step(ReverseInline(workers));
        expect(static_cast<bool>(first), "ordered departure tick executes"); if (!first) continue;
        const auto units = session.value().units();
        expect(units[0].controller_seat.value == 2 && units[1].controller_seat.value == 2
            && units[2].controller_seat.value == 3, "gift then departure counts current roots and seat-ID ties");
        expect(units[6].control == tactical::UnitControl::shared_structure && units[6].controller_seat.value == 0,
            "departure preserves shared structures");
        const auto& events = first.value().snapshot->events();
        expect(std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.sequence == 1 && event.kind == tactical::EventKind::order_accepted;
        }) && std::any_of(events.begin(), events.end(), [](const auto& event) {
            return event.sequence == 4 && event.kind == tactical::EventKind::order_rejected;
        }), "leave preserves earlier orders and rejects later stale authority");
        auto saved = session.value().save_checkpoint_state();
        auto restored = tactical::TacticalSession::from_replay(log, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(saved && restored && restored.value().load_checkpoint_state(saved.value()), "departure checkpoint restores effective policy");
        if (!saved || !restored) continue;
        expect(restored.value().state_sha256() == session.value().state_sha256(), "restore never re-splits departed units");
        for (int tick = 1; tick < 4; ++tick) {
            auto live = session.value().step(ReverseInline(workers));
            auto resumed = restored.value().step(ReverseInline(workers));
            expect(live && resumed && live.value().state_hash.get() == resumed.value().state_hash.get(),
                "departure checkpoint continuation equals replay");
        }
        expect(session.value().controller_policy()->factions.front().mode == tactical::FactionController::passive
            && session.value().units().front().controller_seat.value == 10,
            "last human transfers to configured passive AI seat without faction quit");
        expect(session.value().record().setup == setup, "departures never rewrite sealed entry roster");
        const auto hash = session.value().state_sha256();
        if (expected.empty()) expected = hash; else expect(expected == hash, "departures equal on 1/2/4/8 workers");
    }
}

void test_controller_foundation() {
    tactical::TacticalReplay replay;
    replay.final_tick_count = 4;
    auto& setup = replay.setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{1, 11, 1}, {2, 12, 1}, {3, 13, 2}};
    setup.first_free_entity_id = 4;
    setup.campaign.emplace();
    for (const auto id : {1U, 2U}) {
        tactical::EconomyPlayer account;
        account.player = id;
        account.credit_cap = eawr::sim::math::Fixed{};
        setup.campaign->players.push_back(account);
    }
    setup.controllers = tactical::BattleControllerPolicy{
        {{{1}, 1, eawr::sim::SeatKind::human, true}, {{2}, 1, eawr::sim::SeatKind::human, true},
            {{3}, 2, eawr::sim::SeatKind::ai, true}, {{4}, 1, eawr::sim::SeatKind::human, true}},
        {{1, tactical::FactionController::humans}, {2, tactical::FactionController::ai}}};
    for (std::size_t i = 0; i < setup.units.size(); ++i) {
        setup.units[i].control = tactical::UnitControl::mobile;
        setup.units[i].controller_seat.value = static_cast<std::uint32_t>(i + 1);
    }
    tactical::PlayerCommand command{{0, 1, 1}, {1}, tactical::MovePayload{}};
    command.origin = tactical::CommandOrigin::seat;
    command.issuing_seat = {1};
    replay.commands = {command};
    auto queued = command; queued.key = {2, 1, 2}; queued.issuing_seat = {2};
    replay.commands.push_back(queued);
    expect(static_cast<bool>(tactical::validate_replay(replay)), "controller fixture is valid");
    const auto encoded = tactical::write_replay(replay);
    expect(static_cast<bool>(encoded), "controller setup encodes");
    if (!encoded) return;
    const auto decoded = tactical::parse_replay(encoded.value());
    expect(decoded && decoded.value() == replay, "controller roster, unit state and provenance round trip");
    const auto header_end = static_cast<std::size_t>(encoded.value()[10])
        | (static_cast<std::size_t>(encoded.value()[11]) << 8U);
    const auto controller_bytes = 16U + 16U * setup.controllers->seats.size()
        + 8U * setup.controllers->factions.size() + 16U * setup.units.size();
    auto malformed = encoded.value(); malformed[header_end - controller_bytes] = 2;
    expect(!tactical::parse_replay(malformed), "unsupported controller extension version fails");
    malformed = encoded.value(); malformed[header_end - controller_bytes - 2] ^= 1;
    expect(!tactical::parse_replay(malformed), "incorrect controller extension length fails");
    const auto body = tactical::encode_command_body(command, setup);
    expect(static_cast<bool>(body), "checked command body encodes");
    if (body) {
        auto parsed = tactical::decode_command_body(body.value(), setup);
        expect(parsed && parsed.value() == command, "body codec preserves authenticated seat and producer key");
        auto bad = body.value(); bad.pop_back();
        expect(!tactical::decode_command_body(bad, setup), "truncated provenance fails");
        bad = body.value(); bad[bad.size() - 12] = 2;
        expect(!tactical::decode_command_body(bad, setup), "unsupported provenance version fails");
        bad = body.value(); bad.back() = 1;
        expect(!tactical::decode_command_body(bad, setup), "malformed origin fails");
        bad = body.value(); bad[bad.size() - 8] = 0;
        expect(!tactical::decode_command_body(bad, setup), "seat origin with zero seat fails");
    }
    auto oversized = command; oversized.units.resize(tactical::max_units_per_command + 1U);
    expect(!tactical::encode_command_body(oversized, setup), "oversized controller command fails before encoding");
    auto unstamped = command; unstamped.issuing_seat = {}; unstamped.origin = tactical::CommandOrigin::ordinary;
    expect(!tactical::encode_command_body(unstamped, setup), "enabled policy requires explicit provenance");
    auto waypoint = command; std::get<tactical::MovePayload>(waypoint.payload).append = true;
    const auto waypoint_body = tactical::encode_command_body(waypoint, setup);
    expect(waypoint_body && tactical::decode_command_body(waypoint_body.value(), setup)
        && tactical::decode_command_body(waypoint_body.value(), setup).value() == waypoint,
        "waypoint and provenance tails coexist canonically");
    auto invalid = setup;
    invalid.units.front().controller_seat = {3};
    expect(!tactical::validate_setup(invalid), "other faction controller fails");
    invalid = setup; invalid.controllers->seats.front().active = false;
    expect(!tactical::validate_setup(invalid), "inactive mobile controller fails");
    invalid = setup; invalid.controllers.reset();
    expect(!tactical::validate_setup(invalid), "orphan controller data fails");
    invalid = setup; invalid.units.front().controller_seat = {};
    expect(!tactical::validate_setup(invalid), "zero cannot grant shared mobile control");
    invalid = setup; invalid.controllers->seats[1].seat = {1};
    expect(!tactical::validate_setup(invalid), "duplicate roster seat fails");
    invalid = setup; invalid.squadrons = {{1, {2}}};
    expect(!tactical::validate_setup(invalid), "attached squadron members retain root control");
    auto native = replay;
    native.setup.skirmish.emplace();
    native.setup.skirmish->map = "data/art/maps/controller_fixture.ted";
    native.setup.skirmish->map_sha256 = std::string(64, '0');
    native.setup.skirmish->victory_condition = 1;
    native.setup.skirmish->slots = {{1, true, {}, {}, {}}, {2, false, {}, {}, {}}};
    native.setup.native_scene = tactical::ReplayNativeScene{true, {"fixture"}, {}, {"fixture"}};
    const auto native_bytes = tactical::write_replay(native);
    expect(native_bytes && tactical::parse_replay(native_bytes.value())
        && tactical::parse_replay(native_bytes.value()).value() == native,
        "native campaign SKSU loader metadata supports controller policy");
    auto large_roster = replay;
    large_roster.setup.controllers->seats.clear();
    for (std::uint32_t seat = 1; seat <= 65; ++seat)
        large_roster.setup.controllers->seats.push_back({{seat}, 1, eawr::sim::SeatKind::human, true});
    large_roster.setup.controllers->seats.push_back({{66}, 2, eawr::sim::SeatKind::ai, true});
    large_roster.setup.units[2].controller_seat = {66};
    const auto roster_bytes = tactical::write_replay(large_roster);
    expect(roster_bytes && tactical::parse_replay(roster_bytes.value())
        && tactical::parse_replay(roster_bytes.value()).value() == large_roster,
        "controller roster bound is independent of tactical faction count");
    auto shared = setup;
    shared.units[1].control = tactical::UnitControl::shared_structure;
    shared.units[1].controller_seat = {};
    expect(static_cast<bool>(tactical::validate_setup(shared)), "explicit shared structure uses zero controller");
    shared.units[1].controller_seat = {2};
    expect(!tactical::validate_setup(shared), "shared structure cannot carry a private controller");
    auto foreign = command; foreign.issuing_seat = {3};
    expect(!tactical::validate_command_provenance(foreign, setup), "mismatched command seat fails");
    tactical::EconomyRules economy;
    economy.campaign = true; economy.players = setup.campaign->players;
    std::string reference_hash, reference_snapshot;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
      for (const bool scramble : {false, true}) {
        auto created = tactical::TacticalSession::from_replay(replay, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(created), "controller session creates");
        if (!created) continue;
        auto session = std::move(created).value();
        expect(session.snapshot()->instances().front().controller_seat.value == 1, "immutable instance publishes controller");
        const auto start_hash = session.state_sha256();
        const auto start_snapshot = session.snapshot()->sha256();
        if (scramble) session.scramble_storage_for_testing();
        expect(session.state_sha256() == start_hash, "ECS rebuild preserves controller identity");
        auto changed = setup;
        changed.units.front().controller_seat = {2};
        const auto other = tactical::TacticalSession::create(changed, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(other && other.value().state_sha256() != start_hash, "state hash includes mobile controller");
        expect(other && other.value().snapshot()->sha256() != start_snapshot, "snapshot hash includes mobile controller");
        changed = setup; changed.controllers->seats[3].active = false;
        const auto roster = tactical::TacticalSession::create(changed, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(roster && roster.value().snapshot()->sha256() != start_snapshot, "snapshot hash includes effective roster");
        const auto advanced = session.step(ReverseInline(workers));
        expect(static_cast<bool>(advanced), "controller command advances");
        expect(advanced && advanced.value().state_hash.get() == session.state_sha256(),
            "partitioned controller encoding equals the serial canonical encoder");
        const auto saved = session.save_checkpoint_state();
        expect(static_cast<bool>(saved), "controller checkpoint saves");
        auto log = session.record();
        const auto pending = session.pending_commands();
        log.commands.insert(log.commands.end(), pending.begin(), pending.end());
        if (!pending.empty()) log.final_tick_count = pending.back().key.tick + 1;
        auto restored = tactical::TacticalSession::from_replay(log, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(restored), "sealed controller log restores");
        if (!saved || !restored) continue;
        const auto loaded = restored.value().load_checkpoint_state(saved.value());
        expect(static_cast<bool>(loaded), "controller checkpoint loads");
        if (!loaded) { std::cerr << loaded.error().message << '\n'; continue; }
        expect(restored.value().state_sha256() == session.state_sha256(), "checkpoint restores controller state hash");
        expect(restored.value().pending_commands() == pending, "checkpoint retains pending caller provenance");
        expect(restored.value().record().commands == session.record().commands, "checkpoint restores executed caller provenance");
        expect(restored.value().snapshot()->controller_policy() == session.snapshot()->controller_policy()
            && std::equal(restored.value().snapshot()->instances().begin(), restored.value().snapshot()->instances().end(),
                session.snapshot()->instances().begin(), session.snapshot()->instances().end()),
            "checkpoint immediately restores controller policy and complete instances");
        // Restore publishes empty tick-event lists (session_state.cpp), so the immediate snapshot
        // cannot match a live snapshot containing its previous tick's events. Like the existing
        // native-checkpoint test, compare immediate state and then continuation; compare full
        // snapshots, including events, after every resumed step through pending execution.
        const std::array<std::uint8_t, 8> controller_tag{'C', 'T', 'R', 'L', 4, 0, 0, 0};
        const auto tail = std::find_end(saved.value().begin(), saved.value().end(),
            controller_tag.begin(), controller_tag.end());
        expect(tail != saved.value().end(), "controller continuation uses the required version-4 domain");
        if (tail == saved.value().end()) continue;
        const auto offset = static_cast<std::size_t>(tail - saved.value().begin());
        const auto policy_archive_bytes = 16U + 10U * setup.controllers->seats.size() + 8U * setup.controllers->factions.size();
        auto corrupt = saved.value(); corrupt[offset + policy_archive_bytes + 24] = 3;
        expect(!restored.value().load_checkpoint_state(corrupt), "checkpoint rejects foreign controller seat");
        corrupt = saved.value(); corrupt[offset + 4] = 2;
        expect(!restored.value().load_checkpoint_state(corrupt), "checkpoint rejects the replaced version-2 controller tail");
        corrupt = saved.value(); corrupt[offset + policy_archive_bytes + 32] = 1;
        expect(!restored.value().load_checkpoint_state(corrupt), "checkpoint rejects a pending caller without a pending ability request");
        auto fresh = tactical::TacticalSession::from_replay(log, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        if (!fresh || !fresh.value().load_checkpoint_state(saved.value())) continue;
        while (session.completed_tick() < log.final_tick_count) {
            const auto resumed_tick = fresh.value().step(ReverseInline(workers));
            const auto live_tick = session.step(ReverseInline(workers));
            expect(resumed_tick && live_tick && resumed_tick.value().state_hash.get() == live_tick.value().state_hash.get(),
                "checkpoint continuation equals live through pending command execution");
            if (!resumed_tick || !live_tick) break;
            expect(fresh.value().snapshot()->canonical_bytes() == session.snapshot()->canonical_bytes(),
                "checkpoint full snapshot equals live after each continuation step");
        }
        expect(fresh.value().record().commands == log.commands, "restored pending seat provenance reaches executed recording");
        expect(fresh.value().state_sha256() == session.state_sha256(), "checkpoint continuation equals live");
        if (workers == 1 && !scramble) { reference_hash = session.state_sha256(); reference_snapshot = session.snapshot()->sha256(); }
        expect(reference_hash == session.state_sha256() && reference_snapshot == session.snapshot()->sha256(),
            "controller state and snapshot equal on 1/2/4/8 workers");
      }
    }
}
#include "canonical_encoder_oracle.hpp"
}

int main(const int argc, const char* const argv[]) {
    if (argc == 2 && std::string_view(argv[1]) == "--beacon-only") {
        test_beacons();
        return failures == 0 ? 0 : 1;
    }
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
    test_controller_shared_special_fire();
    test_controller_gifts();
    test_controller_split_and_gates();
    test_controller_foundation();
    test_controller_departures();
    test_format(fixtures);
    test_human_seat_admission();
    test_special_fire_format(fixtures);
    test_match_policy_format(fixtures);
    test_skirmish_setup_format(fixtures);
    test_native_scene_format(fixtures);
    test_battle_domain(fixtures);
    test_golden_across_workers(fixtures, golden);
    test_phase_map(fixtures);
    test_canonical_encoder_oracle(fixtures);
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
