#include "galactic_support.hpp"

#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/galactic/replay.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/sim/tactical/admission.hpp"
#include <type_traits>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The command envelope and the galactic replay EAWRGCR v1 (ADR-015, ADR-020,
// docs/galactic-replay-format.md): byte round trip, every refusal diagnostic, out-of-order and
// late submission, and a live run that replays itself headless with identical per-tick hashes.
namespace {

using namespace galactic_test;
using galactic::GalacticReplay;
using galactic::GalacticSession;
namespace codes = galactic::diagnostic_codes;

using Bytes = std::vector<std::uint8_t>;

// Keep every declared galactic opcode here; reservation range bounds are not opcodes.
constexpr std::array galactic_command_opcodes{
    galactic::galactic_opcodes::create_fleet,
    galactic::galactic_opcodes::add_members,
    galactic::galactic_opcodes::remove_members,
    galactic::galactic_opcodes::merge_fleets,
    galactic::galactic_opcodes::move_fleet,
    galactic::galactic_opcodes::set_orbit_slot,
    galactic::galactic_opcodes::production,
    galactic::galactic_opcodes::land_transports,
    galactic::galactic_opcodes::buy,
    galactic::galactic_opcodes::observer_intelligence,
    galactic::galactic_opcodes::black_market_prepare,
    galactic::galactic_opcodes::black_market_buy,
    galactic::galactic_opcodes::special_prepare,
    galactic::galactic_opcodes::special_execute,
    galactic::galactic_opcodes::special_cancel,
    galactic::galactic_opcodes::ability,
    galactic::galactic_opcodes::launch_from_surface,
    galactic::galactic_opcodes::launch_fleet,
    galactic::galactic_opcodes::recheck_orbit,
};
static_assert([] {
    for (std::size_t i = 0; i < galactic_command_opcodes.size(); ++i)
        for (std::size_t j = i + 1; j < galactic_command_opcodes.size(); ++j)
            if (galactic_command_opcodes[i] == galactic_command_opcodes[j]) return false;
    return true;
}(), "galactic command opcodes must be unique");

static_assert(std::is_same_v<galactic::SeatId, eawr::sim::SeatId>);
static_assert(std::is_same_v<decltype(eawr::checkpoint::Seat{}.seat_id), eawr::sim::SeatId>);
static_assert(std::is_same_v<decltype(eawr::sim::tactical::SeatBinding{}.seat), galactic::SeatId>);
static_assert(std::is_same_v<galactic::SeatKind, eawr::checkpoint::SeatKind>);
static_assert(!std::is_convertible_v<galactic::PlayerId, galactic::SeatId>);
static_assert(!std::is_convertible_v<galactic::SeatId, galactic::PlayerId>);

[[nodiscard]] bool parse_fails(const Bytes& bytes, const std::string_view code) {
    const auto parsed = galactic::parse_replay(bytes, "test.eawr-gcreplay");
    return !parsed && parsed.error().code == code && parsed.error().logical_path == std::string("test.eawr-gcreplay");
}

// A live run: each tick's commands are submitted just before that tick steps, in seat-reversed
// order, as a game would receive them. Returns the per-tick hashes and the session's record().
[[nodiscard]] std::pair<std::vector<std::string>, GalacticReplay> live(
    const galactic::GalacticSetup& setup, const eawr::sim::PartitionExecutor& executor, const std::uint64_t ticks) {
    auto session = GalacticSession::create(setup).value();
    const auto commands = script(setup, ticks);
    std::vector<std::string> hashes;
    std::size_t next = 0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        std::vector<galactic::GameCommand> batch;
        while (next < commands.size() && commands[next].key.tick == tick) batch.push_back(commands[next++]);
        std::stable_sort(batch.begin(), batch.end(), [](const galactic::GameCommand& left, const galactic::GameCommand& right) {
            return left.key.seat > right.key.seat;
        });
        for (const auto& command : batch) expect(static_cast<bool>(session.submit(command)), "live command admitted");
        hashes.push_back(session.step(executor).value().state_hash.get());
    }
    return {hashes, session.record()};
}

[[nodiscard]] std::vector<std::string> replayed(const GalacticReplay& replay, const eawr::sim::PartitionExecutor& executor) {
    auto created = GalacticSession::from_replay(replay);
    expect(static_cast<bool>(created), "the recorded replay creates a session");
    if (!created) return {};
    auto session = std::move(created).value();
    std::vector<std::string> hashes;
    while (session.completed_tick() < replay.final_tick_count) hashes.push_back(session.step(executor).value().state_hash.get());
    return hashes;
}

void live_equals_replay() {
    galactic::SyntheticGalaxyOptions options;
    options.planets = 40;
    options.players = 3;
    options.coop_seat = true;
    const auto setup = galactic::synthetic_galaxy(options);
    const eawr::sim::InlineExecutor inline_executor;
    const auto [hashes, record] = live(setup, inline_executor, 60);
    expect(record.final_tick_count == 60 && record.setup == setup, "record() holds the setup and the final tick");
    expect(record.commands == script(setup, 60), "record() holds every executed command in canonical order");
    const auto written = galactic::write_replay(record);
    expect(static_cast<bool>(written), "the record writes");
    if (!written) return;
    const auto parsed = galactic::parse_replay(written.value());
    expect(parsed && parsed.value() == record, "the replay parses back to the record");
    const auto rewritten = parsed ? galactic::write_replay(parsed.value()) : written;
    expect(rewritten && rewritten.value() == written.value(), "write(parse(bytes)) == bytes");
    for (const auto workers : eawr::platform::determinism_worker_counts()) {
        const eawr::platform::ThreadWorkerAdapter pool(workers);
        expect(replayed(parsed.value(), pool) == hashes,
            "the headless replay on " + std::to_string(workers) + " workers reproduces the live per-tick hashes");
    }
}

void human_side_round_trip() {
    struct HumanShape { std::uint32_t count; std::uint32_t first_side; };
    for (const auto shape : {HumanShape{8, 8}, HumanShape{8, 4}, HumanShape{4, 1}}) {
        galactic::SyntheticGalaxyOptions options;
        options.players = 4;
        auto setup = galactic::synthetic_galaxy(options);
        setup.seats.clear();
        for (std::uint32_t id = 1; id <= shape.count; ++id) {
            setup.seats.push_back({{id}, id % 2 + 1, galactic::SeatKind::human, id - 1, true,
                id <= shape.first_side ? 1U : 2U});
        }
        setup.seats.push_back({{shape.count + 1}, 3, galactic::SeatKind::ai, shape.count, true, 0});
        setup.seats.push_back({{shape.count + 2}, 4, galactic::SeatKind::script, shape.count + 1, true, 0});
        const GalacticReplay record{setup, 1, {}};
        const auto written = galactic::write_replay(record);
        expect(static_cast<bool>(written), "coop 8v-AI / versus 4v4 / versus 1v3 replay writes");
        if (!written) continue;
        const auto parsed = galactic::parse_replay(written.value());
        expect(parsed && parsed.value() == record && static_cast<bool>(GalacticSession::from_replay(parsed.value())),
            "replay preserves human-side membership independently of ownership");
        const std::size_t first_seat = 96 + 4 + setup.players.size() * 12 + 4;
        const Bytes first_row{1, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 0, 0, 0};
        expect(std::equal(first_row.begin(), first_row.end(), written.value().begin()
                + static_cast<std::ptrdiff_t>(first_seat)),
            "shared seat types preserve the replay v1 18-byte row");
        auto third_side = written.value();
        third_side[first_seat + (shape.count - 1) * 18 + 14] = 3;
        third_side[first_seat + (shape.count - 2) * 18 + 14] = 2;
        expect(parse_fails(third_side, codes::invalid_setup), "a third explicit human side is refused on load");
        if (shape.count == 8) {
            auto ninth_human = written.value();
            ninth_human[first_seat + 8 * 18 + 8] = static_cast<std::uint8_t>(galactic::SeatKind::human);
            ninth_human[first_seat + 8 * 18 + 14] = 1;
            expect(parse_fails(ninth_human, codes::invalid_setup), "a ninth human is refused on load");
        }
    }
}

void submission_order() {
    const auto setup = galactic::synthetic_galaxy();
    const eawr::sim::InlineExecutor executor;
    auto canonical = GalacticSession::create(setup).value();
    auto reversed = GalacticSession::create(setup).value();
    const auto commands = script(setup, 20);
    for (const auto& command : commands) expect(static_cast<bool>(canonical.submit(command)), "canonical submit");
    // Out of order across seats and ticks: per seat it still follows (tick, sequence).
    auto shuffled = commands;
    std::stable_sort(shuffled.begin(), shuffled.end(), [](const galactic::GameCommand& left, const galactic::GameCommand& right) {
        return left.key.seat > right.key.seat;
    });
    for (const auto& command : shuffled) expect(static_cast<bool>(reversed.submit(command)), "shuffled submit");
    for (int tick = 0; tick < 20; ++tick) {
        expect(canonical.step(executor).value().state_sha256 == reversed.step(executor).value().state_sha256,
            "submission order across seats never changes a hash");
    }
    expect(canonical.record() == reversed.record(), "and both record the same replay");
    expect(has_code(canonical.submit(owner(5, {1}, 1000, setup.planets[0].id, 2)), codes::late_command),
        "a late command is refused");
}

void payloads() {
    using galactic::CommandLayer;
    const galactic::CommandPayload set_owner = galactic::DebugSetPlanetOwner{3, 2};
    const galactic::CommandPayload set_orbit = galactic::DebugSetFleetOrbit{20, 4};
    const galactic::CommandPayload tactical = galactic::TacticalLayerCommand{99, 3, {1, 2, 3}};
    const galactic::CommandPayload leave = galactic::SeatLeave{};
    const galactic::CommandPayload rejoin = galactic::SeatRejoin{};
    const galactic::CommandPayload role = galactic::AssignBattleTeamRole{99, galactic::BattleTeamRole::spectate};
    expect(galactic::layer_of(set_owner) == CommandLayer::session && galactic::opcode_of(set_owner) == 62,
        "debug set-owner is session opcode 62");
    expect(galactic::layer_of(set_orbit) == CommandLayer::session && galactic::opcode_of(set_orbit) == 63,
        "debug set-orbit is session opcode 63");
    expect(galactic::layer_of(tactical) == CommandLayer::tactical && galactic::opcode_of(tactical) == 3,
        "a tactical command keeps its tactical opcode");
    for (const auto& payload : {set_owner, set_orbit, tactical, leave, rejoin, role}) {
        const auto bytes = galactic::encode_payload(payload);
        expect(galactic::decode_payload(galactic::layer_of(payload), galactic::opcode_of(payload), bytes) == payload,
            "a payload decodes from its bytes");
    }
    expect(galactic::encode_payload(set_owner).size() == 12 && galactic::encode_payload(set_orbit).size() == 16,
        "payload sizes as documented");
    auto longer = galactic::encode_payload(set_owner);
    longer.push_back(0);
    expect(!galactic::decode_payload(CommandLayer::session, 62, longer), "a payload with trailing bytes is refused");
    expect(galactic::decode_payload(CommandLayer::session, 1, {}) == galactic::CommandPayload{galactic::PauseGalaxy{}}
            && galactic::decode_payload(CommandLayer::session, 2, {}) == galactic::CommandPayload{galactic::ResumeGalaxy{}},
        "pause and resume are session opcodes 1 and 2 with empty payloads");
    expect(!galactic::decode_payload(CommandLayer::session, 1, Bytes(1, 0)), "a pause with a payload is refused");
    expect(galactic::decode_payload(CommandLayer::session, galactic::session_opcodes::seat_leave, {}) == leave
            && galactic::decode_payload(CommandLayer::session, galactic::session_opcodes::seat_rejoin, {}) == rejoin,
        "leave and rejoin use their assigned opcodes with empty payloads");
    expect(!galactic::decode_payload(CommandLayer::session, galactic::session_opcodes::seat_leave, Bytes(1, 0))
            && !galactic::decode_payload(CommandLayer::session, galactic::session_opcodes::seat_rejoin, Bytes(1, 0)),
        "leave and rejoin reject trailing payload bytes");
    for (std::uint16_t opcode = 5; opcode <= 61; ++opcode) {
        if (opcode == galactic::session_opcodes::seat_leave || opcode == galactic::session_opcodes::seat_rejoin) continue;
        expect(!galactic::decode_payload(CommandLayer::session, opcode, {}), "other session opcodes 5..61 refuse empty payloads");
    }
    for (std::uint16_t opcode = 1; opcode <= 127; ++opcode) {
        expect(!galactic::decode_payload(CommandLayer::galactic, opcode, Bytes(16, 0)), "galactic opcodes are reserved only");
    }
    expect(!galactic::decode_payload(CommandLayer::tactical, 0, Bytes(8, 0)) && !galactic::decode_payload(CommandLayer::tactical, 256, Bytes(8, 0))
            && !galactic::decode_payload(CommandLayer::tactical, 3, Bytes(7, 0)),
        "a tactical command needs a tactical opcode and a conflict");
    expect(galactic::tactical_retreat_opcode == 28, "the tactical retreat keeps its reserved opcode");
}

void refusals() {
    const auto setup = galactic::synthetic_galaxy();
    GalacticReplay replay{setup, 10, {}};
    replay.commands.push_back(owner(1, {1}, 1, setup.planets[0].id, 2));
    replay.commands.push_back(orbit(1, {2}, 1, setup.fleets[2].id, setup.planets[3].id));
    replay.commands.push_back(owner(4, {1}, 2, setup.planets[1].id, 1));
    const auto good = galactic::write_replay(replay).value();

    // Every strict prefix is refused without a crash; the full bytes parse.
    for (std::size_t size = 0; size < good.size(); ++size) {
        const auto parsed = galactic::parse_replay(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(size)));
        expect(!parsed, "a truncated replay (" + std::to_string(size) + " bytes) is refused");
    }
    expect(static_cast<bool>(galactic::parse_replay(good)), "the whole replay parses");

    const auto patched = [&](const std::size_t offset, const std::uint8_t value) {
        auto bytes = good;
        bytes[offset] = value;
        return bytes;
    };
    auto trailing = good;
    trailing.push_back(0);
    expect(parse_fails(trailing, codes::replay_malformed), "trailing bytes: EAWR-SIM-0510");
    expect(parse_fails(patched(0, 'X'), codes::replay_malformed), "bad magic: EAWR-SIM-0510");
    expect(parse_fails(patched(8, 1), codes::replay_version), "old format version: EAWR-SIM-0511");
    expect(parse_fails(patched(8, galactic::galactic_replay_format_version + 1), codes::replay_version),
        "unknown format version: EAWR-SIM-0511");
    expect(parse_fails(patched(12, 9), codes::replay_version), "another rules version: EAWR-SIM-0511");
    expect(parse_fails(patched(10, 4), codes::replay_malformed), "unknown header flags: EAWR-SIM-0510");
    // Header: magic 8, version 2, flags 2, rules 4, identities 64, seed 8, final tick 8 = 96.
    expect(parse_fails(patched(96, 0xff), codes::replay_malformed), "a hostile player count: EAWR-SIM-0510");
    expect(parse_fails(Bytes(galactic::galactic_replay_max_bytes + 1, 0), codes::replay_resource_limit),
        "an oversize replay: EAWR-SIM-0512");

    // The command records start after the fixed tables; find the first one by its tick and seat.
    const auto first_command = good.size() - 3 * 27 - 12 - 16 - 12;
    expect(good[first_command] == 1 && good[first_command + 8] == 1, "located the first command record");
    expect(parse_fails(patched(first_command + 20, 4), codes::replay_command), "an unknown layer: EAWR-SIM-0514");
    expect(parse_fails(patched(first_command + 21, 13), codes::replay_command), "a reserved session opcode: EAWR-SIM-0514");
    expect(parse_fails(patched(first_command + 20, 2), codes::replay_command), "a reserved galactic opcode: EAWR-SIM-0514");
    expect(parse_fails(patched(first_command + 8, 9), codes::replay_command), "an unknown seat: EAWR-SIM-0514");
    expect(parse_fails(patched(first_command, 5), codes::replay_order), "commands out of canonical order: EAWR-SIM-0513");

    const auto invalid = [&](const std::string_view name, const std::string_view code, const std::function<void(GalacticReplay&)>& change) {
        auto copy = replay;
        change(copy);
        const auto written = galactic::write_replay(copy);
        expect(!written && written.error().code == code, std::string(name) + " is refused by write_replay with " + std::string(code));
        expect(!GalacticSession::from_replay(copy), std::string(name) + ": no session from it");
    };
    invalid("a command at the final tick", codes::replay_order, [](GalacticReplay& copy) { copy.final_tick_count = 4; });
    invalid("a final tick past the limit", codes::replay_resource_limit,
        [](GalacticReplay& copy) { copy.final_tick_count = galactic::galactic_max_ticks + 1; });
    invalid("a repeated key", codes::replay_order, [](GalacticReplay& copy) { copy.commands[1].key = copy.commands[0].key; });
    invalid("debug commands without the flag", codes::replay_command,
        [](GalacticReplay& copy) { copy.setup.debug_commands = false; });
    invalid("a tactical command without a conflict", codes::replay_command, [](GalacticReplay& copy) {
        copy.commands.push_back({{5, {1}, 3}, galactic::TacticalLayerCommand{0, 3, {}}});
    });
    invalid("an oversized tactical payload", codes::replay_command, [](GalacticReplay& copy) {
        copy.commands.push_back({{5, {1}, 3},
            galactic::TacticalLayerCommand{7, 3, Bytes(galactic::galactic_replay_max_payload - 7, 0)}});
    });
    invalid("an invalid setup", codes::invalid_setup, [](GalacticReplay& copy) { copy.setup.seats.front().player = 99; });
    invalid("nine humans in a replay", codes::invalid_setup, [](GalacticReplay& copy) {
        copy.setup.seats.clear();
        for (std::uint32_t id = 1; id <= 9; ++id) copy.setup.seats.push_back({{id}, 1, galactic::SeatKind::human, id - 1, true, 1});
    });
    invalid("three human sides in a replay", codes::invalid_setup, [](GalacticReplay& copy) {
        copy.setup.seats.push_back({{3}, 1, galactic::SeatKind::human, 2, true, 3});
    });
    // A tactical command with a conflict is a valid record; the galaxy itself refuses it.
    auto with_battle = replay;
    with_battle.commands.push_back({{5, {1}, 3}, galactic::TacticalLayerCommand{7, 3, {9, 9}}});
    const auto battle_bytes = galactic::write_replay(with_battle);
    expect(battle_bytes && galactic::parse_replay(battle_bytes.value()).value() == with_battle,
        "a tactical-layer record round-trips");
    expect(!GalacticSession::from_replay(with_battle), "but a galaxy alone cannot run it");
}

} // namespace

int main() {
    payloads();
    refusals();
    human_side_round_trip();
    submission_order();
    live_equals_replay();
    if (failures != 0) {
        std::cerr << failures << " galactic replay check(s) failed\n";
        return 1;
    }
    std::cout << "galactic replay contracts passed\n";
    return 0;
}
