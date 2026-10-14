// The EAWRCKP container and the child-log section (docs/checkpoint-format.md, ADR-016): round
// trips, every refusal leaving the running battle untouched, and restore-and-continue with
// identical per-tick hashes on 1, 2, 4 and 8 workers.
//
//   checkpoint_tests                       container, refusals and a synthetic combat battle
//   checkpoint_tests --corpus <replay>...  restore and continue at three ticks of each replay

#include "eawr/checkpoint/checkpoint.hpp"
#include "eawr/checkpoint/restore_continue.hpp"
#include "eawr/checkpoint/tactical.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace checkpoint = eawr::checkpoint;
namespace tactical = eawr::sim::tactical;
namespace math = eawr::sim::math;
using math::Fixed;
using Bytes = std::vector<std::uint8_t>;

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename T>
void expect_code(const eawr::core::Result<T>& result, const std::string_view code, const std::string_view message) {
    const bool matched = !result && result.error().code == code;
    if (!matched) {
        std::cerr << "FAIL: " << message << ": expected " << code << ", got "
                  << (result ? std::string("success") : eawr::core::format_diagnostic(result.error())) << '\n';
        ++failures;
    }
}

[[nodiscard]] Bytes read_bytes(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

[[nodiscard]] checkpoint::Digest digest_of(const std::string_view text) {
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    return eawr::core::sha256(bytes);
}

void flip(std::uint8_t& byte, const unsigned mask) { byte = static_cast<std::uint8_t>(byte ^ mask); }

// Recomputes the payload length and SHA-256 after an edit, so a test reaches the check behind them.
void reseal(Bytes& bytes) {
    const std::uint64_t length = bytes.size() - checkpoint::header_size;
    for (std::size_t index = 0; index < 8; ++index) bytes[264 + index] = static_cast<std::uint8_t>(length >> (8U * index));
    const auto digest = eawr::core::sha256(std::span(bytes).subspan(checkpoint::header_size));
    std::copy(digest.begin(), digest.end(), bytes.begin() + 272);
}

// --- Container ---------------------------------------------------------------------------------

[[nodiscard]] checkpoint::Checkpoint sample_container() {
    checkpoint::Checkpoint value;
    auto& header = value.header;
    header.flags = checkpoint::flag_child | checkpoint::flag_frontier;
    header.identities.rules_version = 7;
    header.identities.state_encoding_version = 3;
    header.identities.numeric_abi = digest_of("numeric");
    header.identities.lua_save = digest_of("lua");
    header.identities.content = digest_of("content");
    header.identities.campaign = digest_of("campaign");
    header.identities.mod_list = digest_of("mods");
    header.seed = 0x0102030405060708ULL;
    header.completed_tick = 1234;
    header.galactic_frame = 99;
    header.frontier = {1233, 2, 17, digest_of("inputs")};
    value.sections.push_back({2, 1, true, {1, 2, 3}});
    value.sections.push_back({10, 1, true, {4, 5}});
    value.sections.push_back({0x7000, 4, false, {6}}); // unknown and optional: kept, ignored
    value.sections.push_back({0x8000, 1, false, {}});
    return value;
}

void test_container_round_trip() {
    const auto value = sample_container();
    const auto bytes = checkpoint::write_checkpoint(value);
    expect(static_cast<bool>(bytes), "the container writes");
    if (!bytes) return;
    expect(bytes.value().size() == checkpoint::header_size + 4 * checkpoint::section_header_size + 6,
        "the container is the header, four section headers and six bytes");
    const auto parsed = checkpoint::parse_checkpoint(bytes.value());
    expect(parsed && parsed.value() == value, "the container parses back to the same value");
    if (!parsed) return;
    const auto again = checkpoint::write_checkpoint(parsed.value());
    expect(again && again.value() == bytes.value(), "one encoding: the parsed container writes the same bytes");
    expect(parsed.value().find(checkpoint::SectionId::child_log) != nullptr, "find returns a present section");
    expect(parsed.value().find(checkpoint::SectionId::scripts) == nullptr, "find returns no absent section");
    expect(static_cast<bool>(checkpoint::check_identities(parsed.value().header, value.header.identities)),
        "the same build loads it");
}

void test_container_refusals() {
    const auto value = sample_container();
    const auto bytes = checkpoint::write_checkpoint(value).value();

    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const auto result = checkpoint::parse_checkpoint(std::span(bytes).first(size));
        if (result) {
            expect(false, "truncation to " + std::to_string(size) + " bytes is refused");
            break;
        }
    }
    expect_code(checkpoint::parse_checkpoint(std::span(bytes).first(100)), checkpoint::diagnostic_codes::malformed,
        "a truncated header");
    expect_code(checkpoint::parse_checkpoint(std::span(bytes).first(bytes.size() - 1)), checkpoint::diagnostic_codes::malformed,
        "a truncated payload");
    auto trailing = bytes;
    trailing.push_back(0);
    expect_code(checkpoint::parse_checkpoint(trailing), checkpoint::diagnostic_codes::malformed, "trailing data");

    auto magic = bytes;
    magic[0] = 'X';
    expect_code(checkpoint::parse_checkpoint(magic), checkpoint::diagnostic_codes::malformed, "another magic");
    auto newer = bytes;
    newer[8] = 2;
    expect_code(checkpoint::parse_checkpoint(newer), checkpoint::diagnostic_codes::version, "a newer format version");
    auto zero = bytes;
    zero[8] = 0;
    expect_code(checkpoint::parse_checkpoint(zero), checkpoint::diagnostic_codes::version, "format version zero");
    auto flags = bytes;
    flags[10] = 0x80;
    expect_code(checkpoint::parse_checkpoint(flags), checkpoint::diagnostic_codes::malformed, "an unknown header flag");

    auto damaged = bytes;
    flip(damaged.back(), 0x01U);
    expect_code(checkpoint::parse_checkpoint(damaged), checkpoint::diagnostic_codes::integrity, "a damaged payload byte");
    auto damaged_digest = bytes;
    flip(damaged_digest[280], 0x01U);
    expect_code(checkpoint::parse_checkpoint(damaged_digest), checkpoint::diagnostic_codes::integrity, "a damaged digest");

    auto count = bytes;
    count[220] = 200;
    expect_code(checkpoint::parse_checkpoint(count), checkpoint::diagnostic_codes::resource_limit, "too many sections");
    auto fewer = bytes;
    fewer[220] = 3;
    expect_code(checkpoint::parse_checkpoint(fewer), checkpoint::diagnostic_codes::malformed, "bytes after the last section");

    // Section framing behind a valid digest.
    auto order = bytes;
    order[checkpoint::header_size] = 11; // the first section's ID 2 becomes 11, after which 10 comes
    reseal(order);
    expect_code(checkpoint::parse_checkpoint(order), checkpoint::diagnostic_codes::section_order, "descending section IDs");
    auto unknown = value;
    unknown.sections[2].required = true;
    const auto unknown_bytes = checkpoint::write_checkpoint(unknown);
    expect(static_cast<bool>(unknown_bytes), "an unknown required section writes");
    if (unknown_bytes) {
        expect_code(checkpoint::parse_checkpoint(unknown_bytes.value()), checkpoint::diagnostic_codes::unknown_required_section,
            "an unknown required section");
    }
    auto overrun = bytes;
    overrun[checkpoint::header_size + 8] = 0xff; // first section length
    reseal(overrun);
    expect_code(checkpoint::parse_checkpoint(overrun), checkpoint::diagnostic_codes::malformed, "a section past the payload");
    auto section_flag = bytes;
    section_flag[checkpoint::header_size + 4] = 0x02;
    reseal(section_flag);
    expect_code(checkpoint::parse_checkpoint(section_flag), checkpoint::diagnostic_codes::malformed, "an unknown section flag");

    // Never truncated: over the bound fails the save and the load.
    auto huge = value;
    huge.sections[1].bytes.assign(checkpoint::max_bytes, 0);
    expect_code(checkpoint::write_checkpoint(huge), checkpoint::diagnostic_codes::resource_limit, "an oversize save");
    Bytes oversize(checkpoint::max_bytes + 1U, 0);
    std::copy(bytes.begin(), bytes.end(), oversize.begin());
    expect_code(checkpoint::parse_checkpoint(oversize), checkpoint::diagnostic_codes::resource_limit, "an oversize load");
    auto unordered = value;
    std::swap(unordered.sections[0], unordered.sections[1]);
    expect_code(checkpoint::write_checkpoint(unordered), checkpoint::diagnostic_codes::section_order, "an unordered save");
    auto frontierless = value;
    frontierless.header.flags = checkpoint::flag_child;
    expect_code(checkpoint::write_checkpoint(frontierless), checkpoint::diagnostic_codes::invalid_save,
        "a frontier without its flag");

    // Saved by another build: every identity is checked.
    const auto header = checkpoint::parse_checkpoint(bytes).value().header;
    const auto build = header.identities;
    const std::array<void (*)(checkpoint::Identities&), 7> edits{
        [](checkpoint::Identities& ids) { ++ids.rules_version; },
        [](checkpoint::Identities& ids) { ++ids.state_encoding_version; },
        [](checkpoint::Identities& ids) { flip(ids.numeric_abi[0], 1U); },
        [](checkpoint::Identities& ids) { flip(ids.lua_save[0], 1U); },
        [](checkpoint::Identities& ids) { flip(ids.content[0], 1U); },
        [](checkpoint::Identities& ids) { flip(ids.campaign[0], 1U); },
        [](checkpoint::Identities& ids) { flip(ids.mod_list[0], 1U); },
    };
    for (const auto edit : edits) {
        auto other = build;
        edit(other);
        expect_code(checkpoint::check_identities(header, other), checkpoint::diagnostic_codes::other_build,
            "an identity differs");
    }
}

void test_seats() {
    const std::vector<checkpoint::Seat> seats{
        {{1}, 1, checkpoint::SeatKind::human, true, 0},
        {{2}, 1, checkpoint::SeatKind::human, false, 1}, // coop: two seats, one player
        {{3}, 2, checkpoint::SeatKind::ai, true, checkpoint::no_lobby_slot},
        {{9}, 2, checkpoint::SeatKind::script, true, checkpoint::no_lobby_slot},
    };
    const auto bytes = checkpoint::encode_seats(seats);
    expect(bytes && bytes.value().size() == 4 + 16 * seats.size(), "seats encode at 16 bytes each");
    if (!bytes) return;
    const std::vector<std::uint8_t> expected{
        4, 0, 0, 0,
        1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0, 0,
        2, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0,
        3, 0, 0, 0, 2, 0, 0, 0, 2, 1, 0, 0, 255, 255, 255, 255,
        9, 0, 0, 0, 2, 0, 0, 0, 3, 1, 0, 0, 255, 255, 255, 255,
    };
    expect(bytes.value() == expected, "shared seat types preserve all checkpoint v1 seat bytes");
    const auto decoded = checkpoint::decode_seats(bytes.value());
    expect(decoded && decoded.value() == seats, "seats round trip");
    auto unordered = seats;
    std::swap(unordered[0], unordered[1]);
    expect_code(checkpoint::encode_seats(unordered), checkpoint::diagnostic_codes::invalid_save, "unordered seats");
    auto kind = bytes.value();
    kind[4 + 8] = 7;
    expect_code(checkpoint::decode_seats(kind), checkpoint::diagnostic_codes::section_invalid, "an unknown seat kind");
    auto active = bytes.value();
    active[4 + 9] = 2;
    expect_code(checkpoint::decode_seats(active), checkpoint::diagnostic_codes::section_invalid, "an active flag of 2");
    expect_code(checkpoint::decode_seats(std::span(bytes.value()).first(bytes.value().size() - 1)),
        checkpoint::diagnostic_codes::section_invalid, "truncated seats");
    std::vector<checkpoint::Seat> controllers(128);
    for (std::size_t index = 0; index < controllers.size(); ++index)
        controllers[index] = {{static_cast<std::uint32_t>(index)}, static_cast<std::uint32_t>(index),
            checkpoint::SeatKind::ai, true, checkpoint::no_lobby_slot};
    const auto controller_bytes = checkpoint::encode_seats(controllers);
    expect(controller_bytes && checkpoint::decode_seats(controller_bytes.value()).value() == controllers,
        "the generic format does not cap faction/controller seats at 64");
    std::vector<checkpoint::Seat> many(checkpoint::max_seats + 1U);
    for (std::size_t index = 0; index < many.size(); ++index) many[index].seat_id = {static_cast<std::uint32_t>(index)};
    expect_code(checkpoint::encode_seats(many), checkpoint::diagnostic_codes::resource_limit, "too many seats");
}

// --- A synthetic combat battle -----------------------------------------------------------------

constexpr std::int64_t one = std::int64_t{1} << 24;
constexpr tactical::TypeId gunship_type = 1;
constexpr tactical::TypeId fighter_type = 2;

[[nodiscard]] Fixed units(const std::int64_t value) { return Fixed::from_raw(value * one); }
[[nodiscard]] math::Vec3 at(const std::int64_t x, const std::int64_t y) { return {units(x), units(y), units(0)}; }

struct BattleContent {
    std::vector<tactical::SensorProfile> sensors;
    tactical::DurabilityTable durability;
    tactical::MotionTable motion;
    tactical::CombatTable combat;
};

[[nodiscard]] BattleContent battle_content() {
    BattleContent content;
    content.sensors = {{gunship_type, units(2000)}, {fighter_type, units(2000)}};

    tactical::DurabilityProfile gunship_hull;
    gunship_hull.type_id = gunship_type;
    gunship_hull.max_hull = units(400);
    tactical::DurabilityProfile fighter_hull;
    fighter_hull.type_id = fighter_type;
    fighter_hull.max_hull = units(100);
    content.durability.profiles = {gunship_hull, fighter_hull};

    content.motion.rules.arc_degrees = units(15);
    content.motion.rules.expansion_distance = units(300);
    tactical::MotionProfile gunship_motion;
    gunship_motion.type_id = gunship_type;
    gunship_motion.max_speed = units(3);
    gunship_motion.acceleration = Fixed::from_raw(one / 20);
    gunship_motion.deceleration = Fixed::from_raw(one / 20);
    gunship_motion.rate_of_turn = Fixed::from_raw(3 * one / 2);
    gunship_motion.turn_in_place_slowdown = units(2);
    auto fighter_motion = gunship_motion;
    fighter_motion.type_id = fighter_type;
    fighter_motion.max_speed = units(6);
    fighter_motion.rate_of_turn = units(4);
    content.motion.profiles = {gunship_motion, fighter_motion};

    tactical::PrioritySet priorities;
    priorities.unlisted = units(1000);
    content.combat.priority_sets.push_back(priorities);
    tactical::WeaponProfile cannon;
    cannon.range = units(700);
    cannon.min_recharge_hundredths = 50;
    cannon.max_recharge_hundredths = 350; // a random recharge: the battle draws from its RNG
    cannon.pulse_count = 2;
    cannon.pulse_delay_frames = 15;
    cannon.cone_width = units(45);
    cannon.cone_height = units(45);
    cannon.opportunity_when_idle = true;
    cannon.opportunity_when_targeting = true;
    cannon.fire_a = at(5, 2);
    tactical::CombatProfile gunship;
    gunship.type_id = gunship_type;
    gunship.category_bits = 1U;
    gunship.priority_set = 0;
    gunship.max_attack_distance = units(1000);
    gunship.weapons = {cannon};
    gunship.hardpoints = {{0, at(5, 0), true}};
    auto fighter = gunship;
    fighter.type_id = fighter_type;
    content.combat.profiles = {gunship, fighter};
    return content;
}

[[nodiscard]] tactical::UnitState unit(const eawr::sim::EntityId id, const tactical::TypeId type,
    const tactical::PlayerId owner, const math::Vec3 position) {
    tactical::UnitState state;
    state.entity_id = id;
    state.type_id = type;
    state.owner = owner;
    state.position = position;
    return state;
}

[[nodiscard]] tactical::PlayerCommand command(const std::uint64_t tick, const tactical::PlayerId player,
    const std::uint64_t sequence, std::vector<eawr::sim::EntityId> members, tactical::CommandPayload payload) {
    return {{tick, player, sequence}, std::move(members), std::move(payload)};
}

[[nodiscard]] tactical::TacticalReplay battle_replay() {
    tactical::TacticalReplay replay;
    auto& setup = replay.setup;
    setup.seed = 4242;
    setup.content_identity = digest_of("checkpoint-test-content");
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {unit(1, gunship_type, 1, at(0, 0)), unit(2, gunship_type, 1, at(0, 200)),
        unit(3, fighter_type, 2, at(600, 0)), unit(4, fighter_type, 2, at(650, 100)), unit(5, fighter_type, 2, at(700, -100))};
    replay.final_tick_count = 240;
    replay.commands = {
        command(0, 1, 0, {1}, tactical::MovePayload{at(300, 0)}),
        command(10, 2, 0, {3, 4}, tactical::MovePayload{at(100, 50)}),
        command(40, 1, 1, {1, 2}, tactical::AttackPayload{3}),
        command(70, 2, 1, {4}, tactical::DamagePayload{units(150)}),
        command(100, 1, 2, {2}, tactical::MovePayload{at(500, 0)}),
        command(150, 2, 2, {5}, tactical::MovePayload{at(0, 0)}),
    };
    return replay;
}

[[nodiscard]] checkpoint::TacticalSessionFactory battle_factory() {
    return [content = battle_content()](const tactical::TacticalReplay& replay) {
        return tactical::TacticalSession::from_replay(replay, content.sensors, content.durability, content.motion,
            std::nullopt, content.combat);
    };
}

[[nodiscard]] std::vector<tactical::PlayerCommand> queued_after(const tactical::TacticalReplay& replay, const std::uint64_t tick) {
    std::vector<tactical::PlayerCommand> result;
    std::copy_if(replay.commands.begin(), replay.commands.end(), std::back_inserter(result),
        [tick](const tactical::PlayerCommand& value) { return value.key.tick >= tick; });
    return result;
}

void test_battle_is_not_trivial() {
    const auto replay = battle_replay();
    auto session = battle_factory()(replay);
    expect(static_cast<bool>(session), "the synthetic battle starts");
    if (!session) return;
    const eawr::sim::InlineExecutor executor;
    std::size_t combat_events = 0;
    while (session.value().completed_tick() < replay.final_tick_count) {
        const auto tick = session.value().step(executor);
        if (!tick) {
            expect(false, eawr::core::format_diagnostic(tick.error()));
            return;
        }
        combat_events += tick.value().snapshot->combat_events().size();
    }
    expect(combat_events > 0, "the synthetic battle fires");
    expect(session.value().units().size() < replay.setup.units.size(), "a unit of the synthetic battle dies");
    const auto units = session.value().units();
    const auto first = std::find_if(units.begin(), units.end(), [](const auto& value) { return value.entity_id == 1; });
    expect(first != units.end() && first->position != replay.setup.units[0].position, "the synthetic battle moves");
}

void test_skirmish_state_checkpoint_union() {
    auto replay = battle_replay();
    replay.commands = {
        command(0, 1, 0, {1}, tactical::MovePayload{at(300, 0)}),
        command(1, 1, 1, {1}, tactical::MovePayload{at(300, 200), false, true}),
        command(2, 1, 2, {}, tactical::BeaconPayload{at(100, 100)}),
    };
    replay.setup.beacon_rules = {{1, gunship_type, 1000}};
    replay.setup.first_free_entity_id = 100;
    replay.setup.cinematic_seats = {{1, true}, {2, false}};
    const auto encoded = tactical::write_replay(replay).value();
    expect(tactical::parse_replay(encoded).value() == replay,
        "union replay sorts shared allocator, beacon and campaign cinematic header tags");
    auto content = battle_content();
    content.motion.rules.max_waypoints = 4;
    const auto factory = [&content](const tactical::TacticalReplay& input) {
        return tactical::TacticalSession::from_replay(input, content.sensors, content.durability,
            content.motion, std::nullopt, content.combat);
    };
    std::string baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto source = factory(replay).value();
        while (source.completed_tick() < 20) static_cast<void>(source.step(executor).value());
        expect(source.snapshot()->visible_beacons(1).size() == 1, "union checkpoint has a live beacon");
        const auto bytes = source.save_checkpoint_state().value();
        auto restored = factory(source.record()).value();
        expect(static_cast<bool>(restored.load_checkpoint_state(bytes)), "union checkpoint restores queued waypoints and beacon");
        expect(restored.save_checkpoint_state().value() == bytes, "union checkpoint recaptures canonical raw bytes");
        expect(restored.state_sha256() == source.state_sha256(), "union checkpoint preserves the saved state hash");
        while (source.completed_tick() < replay.final_tick_count) {
            const auto a = source.step(executor).value();
            const auto b = restored.step(executor).value();
            expect(a.state_sha256 == b.state_sha256, "union waypoint/beacon continuation matches every tick");
        }
        if (workers == 1) baseline = source.state_sha256();
        else expect(source.state_sha256() == baseline, "union checkpoint continuation agrees on 1/2/4/8 workers");
    }
}

void test_tactical_round_trip_and_refusals() {
    const auto replay = battle_replay();
    const auto factory = battle_factory();
    const auto build = checkpoint::tactical_identities(replay.setup.content_identity);
    const eawr::platform::ThreadWorkerAdapter executor(2);
    constexpr std::uint64_t save_tick = 60;

    auto source = factory(replay).value();
    while (source.completed_tick() < save_tick) static_cast<void>(source.step(executor).value());
    const auto queued = queued_after(replay, save_tick);
    const auto seats = checkpoint::default_seats(replay.setup);
    expect(seats.size() == 2 && seats[0].kind == checkpoint::SeatKind::ai, "a battle without a lobby seats each player as AI");

    // Capture refuses a queue that is not the battle's.
    expect_code(checkpoint::capture_child_log(source, std::span(queued).first(1)), checkpoint::diagnostic_codes::invalid_save,
        "a short queue");
    auto stale = queued;
    stale[0].key.tick = 10;
    expect_code(checkpoint::capture_child_log(source, stale), checkpoint::diagnostic_codes::invalid_save,
        "a queued command keyed to an executed tick");
    auto swapped = queued;
    std::swap(swapped[0], swapped[1]);
    expect_code(checkpoint::capture_child_log(source, swapped), checkpoint::diagnostic_codes::invalid_save, "an unordered queue");
    auto wrong_queue = queued;
    wrong_queue.back().payload = tactical::MovePayload{at(99, 99)};
    expect_code(checkpoint::capture_child_log(source, wrong_queue), checkpoint::diagnostic_codes::invalid_save,
        "an equal-sized queue with a different payload");
    expect(source.pending_commands() == queued, "capture leaves every pending command untouched");
    expect_code(checkpoint::save_tactical_checkpoint(source, queued, seats, checkpoint::tactical_identities(digest_of("other"))),
        checkpoint::diagnostic_codes::invalid_save, "a save under other content");
    auto stranger = seats;
    stranger.push_back({{7}, 9, checkpoint::SeatKind::human, true, checkpoint::no_lobby_slot});
    expect_code(checkpoint::save_tactical_checkpoint(source, queued, stranger, build), checkpoint::diagnostic_codes::invalid_save,
        "a seat in a player who is not in the battle");

    const auto saved = checkpoint::save_tactical_checkpoint(source, queued, seats, build);
    expect(static_cast<bool>(saved), "the battle saves");
    if (!saved) {
        std::cerr << eawr::core::format_diagnostic(saved.error()) << '\n';
        return;
    }
    const auto& bytes = saved.value();
    const auto parsed = checkpoint::parse_checkpoint(bytes).value();
    expect(parsed.header.completed_tick == save_tick && parsed.header.seed == replay.setup.seed
        && (parsed.header.flags & checkpoint::flag_frontier) != 0U && parsed.header.frontier.tick == 150,
        "the header names the battle's tick, seed and last sealed command");
    class NoTickExecutor final : public eawr::sim::PartitionExecutor {
    public:
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(std::size_t, const std::function<void(std::size_t)>&) const override {
            return eawr::core::Result<void>::failure(eawr::core::Diagnostic{"EAWR-TEST-0001",
                eawr::core::Severity::error, "restore must not execute battle ticks", {}, {}, {}, {}});
        }
    };
    const NoTickExecutor no_ticks;
    expect(static_cast<bool>(checkpoint::load_tactical_checkpoint(bytes, build, factory, no_ticks)),
        "full-state load succeeds even when battle ticking is disabled");

    auto loaded = checkpoint::load_tactical_checkpoint(bytes, build, factory, executor);
    expect(static_cast<bool>(loaded), "the battle loads");
    if (!loaded) {
        std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n';
        return;
    }
    auto running = std::move(loaded).value();
    expect(running.session.state_sha256() == source.state_sha256(), "the loaded battle has the saved state");
    expect(running.session.record() == source.record(), "the loaded battle has the saved record");
    expect(running.seats == seats, "the loaded battle has the saved seats");
    expect(running.work.resimulated_ticks == 0 && running.work.checkpoint_bytes == bytes.size(),
        "full-state restore reads bytes without re-simulating completed ticks");
    auto legacy = parsed;
    legacy.sections.pop_back();
    const auto legacy_loaded = checkpoint::load_tactical_checkpoint(checkpoint::write_checkpoint(legacy).value(), build, factory, executor);
    expect(legacy_loaded && legacy_loaded.value().work.resimulated_ticks == save_tick
        && legacy_loaded.value().session.state_sha256() == source.state_sha256(), "legacy child-log-only saves still load");
    const auto resaved = checkpoint::save_tactical_checkpoint(running.session, queued, running.seats, build);
    expect(resaved && resaved.value() == bytes, "a loaded battle saves the same bytes");

    // Every refusal leaves the running battle untouched.
    const auto before_hash = running.session.state_sha256();
    const auto before_record = running.session.record();
    const auto refused = [&](const Bytes& input, const std::string_view code, const std::string_view what,
                             const checkpoint::Identities& with = checkpoint::Identities{}, bool other_build = false,
                             const checkpoint::TacticalSessionFactory* other_factory = nullptr) {
        const auto result = checkpoint::restore_tactical_checkpoint(running, input, other_build ? with : build,
            other_factory ? *other_factory : factory, executor, "save.eawr-ckp");
        expect_code(result, code, what);
        expect(running.session.state_sha256() == before_hash && running.session.record() == before_record
            && running.seats == seats && running.session.completed_tick() == save_tick,
            std::string(what) + " leaves the running battle untouched");
        if (!result) expect(result.error().logical_path == std::string("save.eawr-ckp"), "the refusal names the file");
    };
    refused(Bytes(bytes.begin(), bytes.begin() + 200), checkpoint::diagnostic_codes::malformed, "a truncated save");
    refused(Bytes(bytes.begin(), bytes.end() - 3), checkpoint::diagnostic_codes::malformed, "a save missing its tail");
    auto damaged = bytes;
    flip(damaged[damaged.size() / 2], 0x10U);
    refused(damaged, checkpoint::diagnostic_codes::integrity, "a damaged save");
    Bytes oversize(checkpoint::max_bytes + 1U, 0);
    refused(oversize, checkpoint::diagnostic_codes::resource_limit, "an oversize save");
    refused(bytes, checkpoint::diagnostic_codes::other_build, "a save from other content",
        checkpoint::tactical_identities(digest_of("other")), true);
    auto newer_rules = build;
    ++newer_rules.rules_version;
    refused(bytes, checkpoint::diagnostic_codes::other_build, "a save from other rules", newer_rules, true);
    const auto content_free = checkpoint::content_free_factory();
    refused(bytes, checkpoint::diagnostic_codes::section_invalid, "a battle restored with other content tables", {}, false,
        &content_free);
    const checkpoint::TacticalSessionFactory wrong_seed = [factory](auto log) {
        ++log.setup.seed;
        return factory(log);
    };
    refused(bytes, checkpoint::diagnostic_codes::restore_failed, "a factory that changes the saved setup", {}, false, &wrong_seed);

    // Section-level defects behind a valid container.
    const auto rewrite = [&](auto&& edit) {
        auto value = parsed;
        edit(value);
        return checkpoint::write_checkpoint(value).value();
    };
    const auto child_index = std::size_t{1};
    refused(rewrite([](checkpoint::Checkpoint& value) { value.header.seed ^= 1U; }),
        checkpoint::diagnostic_codes::inconsistent, "a header seed that is not the battle's");
    refused(rewrite([](checkpoint::Checkpoint& value) { ++value.header.completed_tick; }),
        checkpoint::diagnostic_codes::inconsistent, "a header tick that is not the battle's");
    refused(rewrite([](checkpoint::Checkpoint& value) { ++value.header.frontier.sequence; }),
        checkpoint::diagnostic_codes::inconsistent, "a frontier that is not the battle's last command");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.header.flags |= checkpoint::flag_galaxy; }),
        checkpoint::diagnostic_codes::inconsistent, "a galaxy flag in a tactical save");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.header.flags &= static_cast<std::uint16_t>(~checkpoint::flag_child); }),
        checkpoint::diagnostic_codes::inconsistent, "a battle without the child flag");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.sections.erase(value.sections.begin()); }),
        checkpoint::diagnostic_codes::inconsistent, "a save without seats");
    refused(rewrite([](checkpoint::Checkpoint& value) {
        value.sections.insert(value.sections.begin() + 1, checkpoint::Section{4, 1, true, {}});
    }), checkpoint::diagnostic_codes::inconsistent, "a galaxy section in a tactical save");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.sections.back().bytes.clear(); }),
        checkpoint::diagnostic_codes::section_invalid, "an empty child-state section");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.sections.back().version = 2; }),
        checkpoint::diagnostic_codes::version, "a newer child-state version");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.sections.back().bytes.push_back(0); }),
        checkpoint::diagnostic_codes::section_invalid, "trailing child-state bytes");
    refused(rewrite([](checkpoint::Checkpoint& value) { value.sections.back().bytes.resize(8); }),
        checkpoint::diagnostic_codes::section_invalid, "truncated child-state fields");
    refused(rewrite([](checkpoint::Checkpoint& value) {
        // Four u64 cursors and four u64 tracking anchors precede the cursor-map count.
        std::fill(value.sections.back().bytes.begin() + 64, value.sections.back().bytes.begin() + 72, std::uint8_t{0xff});
    }), checkpoint::diagnostic_codes::section_invalid, "a child-state count larger than its byte budget");
    refused(rewrite([&](checkpoint::Checkpoint& value) { value.sections[child_index].version = 2; }),
        checkpoint::diagnostic_codes::version, "a newer child-log version");
    refused(rewrite([&](checkpoint::Checkpoint& value) { value.sections[child_index].bytes.resize(30); }),
        checkpoint::diagnostic_codes::section_invalid, "a truncated child-log");
    refused(rewrite([&](checkpoint::Checkpoint& value) { flip(value.sections[child_index].bytes[8], 1U); }),
        checkpoint::diagnostic_codes::restore_failed, "a saved state hash the battle does not reach");
    refused(rewrite([&](checkpoint::Checkpoint& value) { flip(value.sections[child_index].bytes[52], 1U); }),
        checkpoint::diagnostic_codes::section_invalid, "a corrupted embedded replay");
    refused(rewrite([&](checkpoint::Checkpoint& value) { value.sections[0].bytes[4 + 4] = 9; }),
        checkpoint::diagnostic_codes::inconsistent, "a seat in a player who is not in the battle");
    refused(rewrite([&](checkpoint::Checkpoint& value) { value.sections[0].bytes[4 + 8] = 0; }),
        checkpoint::diagnostic_codes::section_invalid, "a seat of no kind");

    // A child log whose embedded replay was re-encoded with another final tick.
    auto child = checkpoint::decode_child_log(parsed.sections[child_index].bytes);
    expect(child && child.value().completed_tick == save_tick && child.value().log.final_tick_count == 151,
        "the child log ends one past its last queued command");
    if (child) {
        auto longer = child.value();
        longer.log.final_tick_count = 200;
        expect_code(checkpoint::encode_child_log(longer), checkpoint::diagnostic_codes::invalid_save, "a non-canonical final tick");
        // Another valid command log than the header's frontier sealed.
        auto edited = child.value();
        edited.log.commands.back().payload = tactical::MovePayload{at(10, 10)};
        const auto edited_bytes = checkpoint::encode_child_log(edited);
        expect(static_cast<bool>(edited_bytes), "an edited child log encodes");
        if (edited_bytes) {
            refused(rewrite([&](checkpoint::Checkpoint& value) { value.sections[child_index].bytes = edited_bytes.value(); }),
                checkpoint::diagnostic_codes::inconsistent, "a command log the frontier did not seal");
        }
        expect_code(checkpoint::resimulate_child_log(child.value(), [](const tactical::TacticalReplay&) {
            return eawr::core::Result<tactical::TacticalSession>::failure(eawr::core::Diagnostic{"EAWR-TEST-0001",
                eawr::core::Severity::error, "no content", std::nullopt, std::nullopt, std::nullopt, std::nullopt});
        }, executor), checkpoint::diagnostic_codes::restore_failed, "a factory that cannot build the battle");
    }

    // The valid save still swaps in.
    auto other = factory(replay).value();
    checkpoint::TacticalRestore fresh{std::move(other), {}, {}, {}};
    expect(static_cast<bool>(checkpoint::restore_tactical_checkpoint(fresh, bytes, build, factory, executor))
        && fresh.session.completed_tick() == save_tick && fresh.session.state_sha256() == before_hash,
        "a valid save swaps in");
}

void test_battle_restore_and_continue() {
    const auto replay = battle_replay();
    const auto factory = battle_factory();
    const eawr::platform::ThreadWorkerAdapter reference(1);
    const auto ticks = checkpoint::sample_ticks(replay.final_tick_count);
    expect(ticks == std::vector<std::uint64_t>{60, 120, 180}, "three sample ticks");
    std::vector<std::size_t> sizes;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        for (const bool scramble : {false, true}) {
            const eawr::platform::ThreadWorkerAdapter continued(workers);
            checkpoint::TacticalContinueOptions options;
            options.scramble_after_restore = scramble;
            const auto subject = checkpoint::tactical_continue_case(replay, factory, reference, continued, options);
            const auto samples = checkpoint::run_restore_and_continue(subject, ticks);
            expect(static_cast<bool>(samples), "restore and continue runs");
            if (!samples) {
                std::cerr << eawr::core::format_diagnostic(samples.error()) << '\n';
                continue;
            }
            for (const auto& sample : samples.value()) {
                expect(!sample.first_divergent_tick, "the battle continued from tick " + std::to_string(sample.tick) + " on "
                    + std::to_string(workers) + " workers" + (scramble ? " (scrambled)" : "") + " matches");
                if (workers == 1 && !scramble) sizes.push_back(sample.checkpoint_bytes);
            }
        }
    }
    expect(sizes.size() == 3 && std::all_of(sizes.begin(), sizes.end(), [](const auto size) { return size < 16384; }),
        "full-state saves of the 240-tick battle stay under 16 KiB");
}

void test_harness_reports_divergence() {
    using Hashes = std::vector<std::string>;
    checkpoint::ContinueCase subject;
    subject.final_tick = 3;
    subject.reference = [] { return eawr::core::Result<Hashes>::success({"a", "b", "c"}); };
    subject.save_at = [](std::uint64_t) { return eawr::core::Result<Bytes>::success({1}); };
    subject.restore_and_run = [](std::span<const std::uint8_t>, std::uint64_t) {
        return eawr::core::Result<Hashes>::success({"b", "changed"});
    };
    const std::array<std::uint64_t, 1> ticks{1};
    const auto samples = checkpoint::run_restore_and_continue(subject, ticks);
    expect(samples && samples.value().size() == 1 && samples.value()[0].first_divergent_tick == 3,
        "the harness reports the first changed tick after restore");
}

void test_full_state_staged_and_fog() {
    auto replay = battle_replay();
    const auto battle = battle_content();
    tactical::TacticalContent content;
    content.sensors = battle.sensors;
    content.durability = battle.durability;
    content.motion = battle.motion;
    content.combat = battle.combat;
    tactical::FogRules fog;
    fog.map_left = units(-2000);
    fog.map_top = units(2000);
    fog.cell_size = units(100);
    fog.cells_wide = 40;
    fog.cells_tall = 40;
    content.fog = fog;
    const checkpoint::TacticalSessionFactory factory = [content](const auto& log) {
        return tactical::TacticalSession::from_replay(log, content);
    };
    const eawr::sim::InlineExecutor executor;
    auto source = factory(replay).value();
    for (unsigned i = 0; i < 60; ++i) static_cast<void>(source.step(executor).value());
    auto staged = unit(0, gunship_type, 1, at(0, 500));
    expect(static_cast<bool>(source.stage_spawn(staged)), "scenario-seam spawn enters the full-state save");
    const auto seats = checkpoint::default_seats(replay.setup);
    const auto build = checkpoint::tactical_identities(replay.setup.content_identity);
    const auto bytes = checkpoint::save_tactical_checkpoint(source, source.pending_commands(), seats, build).value();
    auto loaded = checkpoint::load_tactical_checkpoint(bytes, build, factory, executor);
    expect(loaded && loaded.value().work.resimulated_ticks == 0, "staged battle with fog restores without replay");
    if (!loaded) { std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n'; return; }
    expect(loaded.value().session.canonical_state_bytes() == source.canonical_state_bytes(), "fog and staged identities round trip");
    loaded.value().session.scramble_storage_for_testing();
    for (unsigned i = 0; i < 120; ++i) {
        auto expected = source.step(executor);
        auto actual = loaded.value().session.step(executor);
        expect(expected && actual && expected.value().state_sha256 == actual.value().state_sha256,
            "staged battle with fog continues identically");
        if (!expected || !actual) break;
    }
}

void test_projectiles_restore_and_continue() {
    auto replay = battle_replay();
    replay.setup.units = {unit(1, gunship_type, 1, at(0, 0)), unit(3, fighter_type, 2, at(600, 0))};
    replay.commands = {command(0, 1, 1, {1}, tactical::AttackPayload{3})};
    const auto battle = battle_content();
    tactical::TacticalContent content;
    content.sensors = battle.sensors;
    content.durability = battle.durability;
    content.durability.damage = tactical::DamageRules{};
    content.durability.damage->shield_recharge_frames = 90;
    content.durability.damage->energy_recharge_frames = 150;
    content.combat = battle.combat;
    for (auto& profile : content.combat.profiles) {
        profile.collision = tactical::CollisionBox{at(-20, -20), at(20, 20)};
        auto& weapon = profile.weapons.front();
        weapon.cone_width = units(360);
        weapon.cone_height = units(180);
        tactical::ShotProfile shot;
        shot.speed = units(6);
        shot.max_travel = units(2000);
        shot.damage = units(3);
        weapon.shot = shot;
    }
    const checkpoint::TacticalSessionFactory factory = [content](const auto& log) {
        return tactical::TacticalSession::from_replay(log, content);
    };
    const eawr::sim::InlineExecutor reference;
    auto source = factory(replay).value();
    for (unsigned i = 0; i < 30; ++i) static_cast<void>(source.step(reference).value());
    expect(!source.projectiles().empty(), "checkpoint fixture contains projectiles in flight");
    const std::array<std::uint64_t, 3> ticks{30, 100, 150};
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto samples = checkpoint::run_restore_and_continue(
            checkpoint::tactical_continue_case(replay, factory, reference, executor, {true, {}}), ticks);
        expect(static_cast<bool>(samples), "projectile battle restores");
        if (!samples) { std::cerr << eawr::core::format_diagnostic(samples.error()) << '\n'; continue; }
        for (const auto& sample : samples.value()) expect(!sample.first_divergent_tick,
            "projectile continuation matches after restore on " + std::to_string(workers) + " workers");
    }
}

// WCC-82: damage tracking moved to durability state independently of ability binding.

// Compare the publication at the save barrier, before any step can rebuild derived flags.
void test_published_flags_at_restore_barrier() {
    auto replay = battle_replay();
    replay.setup.units = {unit(1, gunship_type, 1, at(0, 0)), unit(2, fighter_type, 1, at(-2000, 0)),
        unit(3, fighter_type, 2, at(100, 0))};
    replay.commands = {command(0, 1, 1, {1}, tactical::AbilityPayload{
        tactical::AbilityKind::tractor_beam, tactical::AbilityAction::activate, 3}),
        command(0, 1, 2, {2}, tactical::AbilityPayload{
            tactical::AbilityKind::stealth, tactical::AbilityAction::activate})};
    tactical::TacticalContent content;
    content.sensors = battle_content().sensors;
    content.durability = battle_content().durability;
    content.motion = battle_content().motion;
    tactical::AbilityProfile beam;
    beam.kind = tactical::AbilityKind::tractor_beam;
    beam.recharge_frames = 20;
    beam.gui_activated_ability_name = "Checkpoint_Beam";
    tactical::SpecialAbilityProfile handler;
    handler.name = beam.gui_activated_ability_name;
    handler.kind = tactical::SpecialAbilityKind::tractor_beam;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = 1;
    handler.beam_min_range = units(5);
    handler.beam_max_range = units(300);
    handler.filter.applicable_categories = 2;
    tactical::UnitAbilityProfile profile;
    profile.type_id = gunship_type;
    profile.abilities = {beam};
    profile.special = {handler};
    tactical::AbilityProfile cloak;
    cloak.kind = tactical::AbilityKind::stealth;
    cloak.expiration_frames = 90;
    cloak.expiration_seconds = units(3);
    cloak.recharge_frames = 90;
    cloak.gui_activated_ability_name = "Checkpoint_Cloak";
    tactical::SpecialAbilityProfile cloak_handler;
    cloak_handler.name = cloak.gui_activated_ability_name;
    cloak_handler.kind = tactical::SpecialAbilityKind::stealth;
    cloak_handler.style = tactical::SpecialActivationStyle::user_input;
    cloak_handler.service_interval = tactical::special_service_interval(cloak_handler.kind);
    cloak_handler.stealth_transition = Fixed::from_raw(one / 2);
    tactical::UnitAbilityProfile cloak_profile;
    cloak_profile.type_id = fighter_type;
    cloak_profile.abilities = {cloak};
    cloak_profile.special = {cloak_handler};
    content.abilities.profiles = {profile, cloak_profile};
    tactical::CombatProfile source_profile;
    source_profile.type_id = gunship_type;
    source_profile.category_bits = 1;
    tactical::CombatProfile target_profile;
    target_profile.type_id = fighter_type;
    target_profile.category_bits = 2;
    content.combat.profiles = {source_profile, target_profile};
    const checkpoint::TacticalSessionFactory factory = [content](const auto& log) {
        return tactical::TacticalSession::from_replay(log, content);
    };
    const auto build = checkpoint::tactical_identities(replay.setup.content_identity);
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto created = factory(replay);
        expect(static_cast<bool>(created), "barrier snapshot fixture creates");
        if (!created) { std::cerr << created.error().message << '\n'; continue; }
        auto source = std::move(created).value();
        for (unsigned tick = 0; tick < 50; ++tick) static_cast<void>(source.step(executor).value());
        const auto expected = source.snapshot();
        const auto target = std::find_if(expected->instances().begin(), expected->instances().end(),
            [](const auto& instance) { return instance.entity_id == 3; });
        expect(target != expected->instances().end() && target->in_tractor_beam,
            "barrier fixture publishes an active tractor target on " + std::to_string(workers) + " workers");
        const auto cloaked = std::find_if(expected->instances().begin(), expected->instances().end(),
            [](const auto& instance) { return instance.entity_id == 2; });
        expect(cloaked != expected->instances().end() && cloaked->stealth_on && cloaked->stealth_invisible,
            "barrier fixture exercises positive published cloak flags");
        const auto saved = checkpoint::save_tactical_checkpoint(source, source.pending_commands(),
            checkpoint::default_seats(replay.setup), build).value();
        auto restored = checkpoint::load_tactical_checkpoint(saved, build, factory, executor);
        expect(static_cast<bool>(restored), "barrier fixture restores");
        if (!restored) { std::cerr << restored.error().message << '\n'; continue; }
        const auto actual = restored.value().session.snapshot();
        expect(actual->completed_tick() == expected->completed_tick(), "restore publishes the saved tick");
        // Full instance equality also checks visibility/reveal, cloak, hazard, movement,
        // targeting and other presentation-only flags, rather than just hashed bytes.
        expect(std::equal(expected->instances().begin(), expected->instances().end(),
            actual->instances().begin(), actual->instances().end()),
            "barrier snapshot instances match immediately after restore on " + std::to_string(workers) + " workers");
        expect(source.step(executor).value().state_sha256 == restored.value().session.step(executor).value().state_sha256,
            "barrier flag retention preserves next-tick state");
    }
}

void test_damage_tracking_restore_and_continue() {
    auto replay = battle_replay();
    replay.setup.units = {unit(1, gunship_type, 1, at(0, 0)), unit(3, fighter_type, 2, at(2000, 0))};
    replay.commands = {command(40, 1, 1, {1}, tactical::DamagePayload{units(3)})};
    tactical::TacticalContent content;
    content.durability = battle_content().durability;
    content.durability.profiles[0].damage_tracking = true;
    const checkpoint::TacticalSessionFactory factory = [content](const auto& log) {
        return tactical::TacticalSession::from_replay(log, content);
    };
    const eawr::sim::InlineExecutor reference;
    auto source = factory(replay).value();
    for (unsigned i = 0; i < 50; ++i) static_cast<void>(source.step(reference).value());
    const auto state = source.durability_state(1);
    expect(state && (state->window_damage.raw() != 0 || state->damage_rate.raw() != 0),
        "damage-tracking fixture contains a live damage window or rate without abilities");
    const std::array<std::uint64_t, 3> ticks{50, 90, 120};
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto samples = checkpoint::run_restore_and_continue(
            checkpoint::tactical_continue_case(replay, factory, reference, executor, {true, {}}), ticks);
        expect(static_cast<bool>(samples), "damage window and rate restore");
        if (!samples) { std::cerr << eawr::core::format_diagnostic(samples.error()) << '\n'; continue; }
        for (const auto& sample : samples.value()) expect(!sample.first_divergent_tick,
            "damage tracking continues identically on " + std::to_string(workers) + " workers");
    }
}

// WST-03/04: Stealth_Duration_Multiplier makes the cloak deadline outlive the slot.
void test_mid_cloak_restore_and_continue() {
    auto replay = battle_replay();
    replay.setup.units = {unit(1, gunship_type, 1, at(0, 0)), unit(2, fighter_type, 1, at(-2000, 0)),
        unit(3, fighter_type, 2, at(2000, 0))};
    replay.final_tick_count = 180;
    replay.commands = {command(0, 1, 1, {2}, tactical::BuyPayload{90}),
        command(20, 1, 2, {1}, tactical::AbilityPayload{tactical::AbilityKind::stealth, tactical::AbilityAction::activate})};
    tactical::TacticalContent content;
    content.durability = battle_content().durability;
    tactical::AbilityProfile cloak;
    cloak.kind = tactical::AbilityKind::stealth;
    cloak.expiration_frames = 90;
    cloak.expiration_seconds = units(3);
    cloak.recharge_frames = 90;
    cloak.gui_activated_ability_name = "Checkpoint_Stealth";
    tactical::SpecialAbilityProfile handler;
    handler.name = cloak.gui_activated_ability_name;
    handler.kind = tactical::SpecialAbilityKind::stealth;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = tactical::special_service_interval(handler.kind);
    handler.stealth_transition = Fixed::from_raw(one / 2);
    tactical::UnitAbilityProfile profile;
    profile.type_id = gunship_type;
    profile.abilities = {cloak};
    profile.special = {handler};
    content.abilities.profiles = {profile};
    content.economy.players = {{1, units(100), 25, false, {}, 1, 3}, {2, units(100), 25, false, {}, 1, 3}};
    tactical::BuildOption option;
    option.type = 90;
    option.kind = tactical::BuildKind::upgrade;
    option.queue = tactical::BuildQueue::upgrades;
    option.price = units(10);
    option.build_frames = 2;
    option.ai_build_frames = 2;
    option.available = true;
    content.economy.menus = {{fighter_type, 1, {option}}};
    tactical::UpgradeProfile upgrade;
    upgrade.type = option.type;
    upgrade.stealth_duration = {{3, Fixed::from_raw(one / 2), false}};
    content.economy.upgrades = {upgrade};
    const checkpoint::TacticalSessionFactory factory = [content](const auto& log) {
        return tactical::TacticalSession::from_replay(log, content);
    };
    const eawr::sim::InlineExecutor reference;
    auto created = factory(replay);
    expect(static_cast<bool>(created), "mid-cloak fixture creates");
    if (!created) { std::cerr << created.error().message << '\n'; return; }
    auto source = std::move(created).value();
    for (unsigned i = 0; i < 50; ++i) static_cast<void>(source.step(reference).value());
    const auto state = source.ability_state(1);
    expect(state && state->stealth_on && state->stealth_end == 155 && state->slots[0].expires_tick == 110,
        "save occurs mid-cloak with independent multiplied deadline");
    const std::array<std::uint64_t, 2> ticks{50, 125};
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto samples = checkpoint::run_restore_and_continue(
            checkpoint::tactical_continue_case(replay, factory, reference, executor, {true, {}}), ticks);
        expect(static_cast<bool>(samples), "multiplied cloak restores before and after slot expiry");
        if (!samples) { std::cerr << eawr::core::format_diagnostic(samples.error()) << '\n'; continue; }
        for (const auto& sample : samples.value()) expect(!sample.first_divergent_tick,
            "multiplied cloak continues identically through its deadline on " + std::to_string(workers) + " workers");
    }
    // Perturb the one independently verified deadline in the child-state payload, then reseal
    // the container: restore must reach and fail the canonical-state integrity check.
    const auto build = checkpoint::tactical_identities(replay.setup.content_identity);
    const auto saved = checkpoint::save_tactical_checkpoint(source, source.pending_commands(),
        checkpoint::default_seats(replay.setup), build).value();
    auto container = checkpoint::parse_checkpoint(saved).value();
    for (auto& section : container.sections) {
        if (section.id != static_cast<std::uint32_t>(checkpoint::SectionId::child_state)) continue;
        const std::array<std::uint8_t, 8> deadline{155, 0, 0, 0, 0, 0, 0, 0};
        const auto found = std::search(section.bytes.begin(), section.bytes.end(), deadline.begin(), deadline.end());
        expect(found != section.bytes.end(), "stealth_end is encoded in the state payload");
        if (found == section.bytes.end()) return;
        const auto second = std::search(found + 8, section.bytes.end(), deadline.begin(), deadline.end());
        expect(second == section.bytes.end(), "the perturbation uniquely identifies stealth_end");
        if (second != section.bytes.end()) return;
        std::fill(found, found + 8, std::uint8_t{0});
    }
    expect_code(checkpoint::load_tactical_checkpoint(checkpoint::write_checkpoint(container).value(), build, factory, reference),
        checkpoint::diagnostic_codes::restore_failed, "zeroing stealth_end fails restore integrity");
}

void test_sliced_search_checkpoint() {
    const auto decimal = [](const std::string_view text) { return Fixed::from_decimal(text).value(); };
    auto table = battle_content().motion;
    table.avoidance = tactical::AvoidanceRules{units(24), decimal("0.2"), units(100), decimal("0.8"), units(15),
        decimal("0.66"), decimal("1.2"), decimal("0.25"), decimal("1.7"), decimal("0.5"), decimal("0.5"),
        3500, 6, 90, 45, units(50)};
    tactical::Footprint footprint;
    footprint.type_id = gunship_type;
    footprint.layer = tactical::SpaceLayer::frigate;
    footprint.x_extent = units(20);
    footprint.y_extent = units(40);
    footprint.radius = units(40);
    std::vector<tactical::TrackedLeaf> statics{{10, {units(0), units(0)}, {units(0), units(0)},
        {units(1), units(0)}, {}, {}, units(300), tactical::collision_static}};
    tactical::TrackingLayerView layer;
    layer.windows.resize(45);
    tactical::CollisionWorld world;
    world.interval = 90;
    world.layers[1] = &layer;
    world.statics = &statics;
    tactical::SlicedPathSearch source(table, table.profiles.front(), footprint, world, 1, 0,
        at(-1800, 0), units(0), units(0), at(1800, 0));
    static_cast<void>(source.run(32));
    expect(!source.ended(), "search checkpoint fixture has unfinished path work");
    const auto saved = source.checkpoint();
    auto loaded = tactical::SlicedPathSearch::from_checkpoint(table, saved);
    expect(loaded && loaded.value().expansions() == source.expansions() && loaded.value().ended() == source.ended(),
        "sliced search restores its expansion cursor");
    if (!loaded) { std::cerr << eawr::core::format_diagnostic(loaded.error()) << '\n'; return; }
    // Alter the caller's world: the restored search must read its saved copied inputs.
    statics.clear();
    for (unsigned i = 0; i < 10000 && !source.ended(); ++i) {
        const auto expected = source.run(32);
        const auto actual = loaded.value().run(32);
        expect(expected == actual && source.expansions() == loaded.value().expansions(), "sliced search continues at the same cursor");
    }
    expect(source.ended() && loaded.value().ended() && source.result() && loaded.value().result()
        && source.result().value() == loaded.value().result().value(), "sliced search restores the same final path");
}

void test_tactical_seat_limits() {
    auto replay = battle_replay();
    replay.setup.players.push_back({3, 3, 3, tactical::player_flag_commandable});
    replay.setup.players.push_back({4, 3, 4, tactical::player_flag_commandable});
    const auto factory = battle_factory();
    auto session = factory(replay).value();
    const auto build = checkpoint::tactical_identities(replay.setup.content_identity);
    const eawr::sim::InlineExecutor executor;
    std::vector<checkpoint::Seat> humans;
    for (std::uint32_t id = 1; id <= 9; ++id)
        humans.push_back({{id}, (id - 1) / 3 + 1, checkpoint::SeatKind::human, true, id - 1});
    const auto nine = checkpoint::save_tactical_checkpoint(session, replay.commands, humans, build);
    expect(nine && checkpoint::load_tactical_checkpoint(nine.value(), build, factory, executor),
        "nine human command seats across three loaded sides round trip");
    auto container = checkpoint::parse_checkpoint(nine.value()).value();
    humans.push_back({{10}, 1, checkpoint::SeatKind::human, true, 9});
    expect_code(checkpoint::save_tactical_checkpoint(session, replay.commands, humans, build),
        checkpoint::diagnostic_codes::invalid_save, "ten human seats are refused at save");
    container.sections[0].bytes = checkpoint::encode_seats(humans).value();
    expect_code(checkpoint::load_tactical_checkpoint(checkpoint::write_checkpoint(container).value(), build, factory, executor),
        checkpoint::diagnostic_codes::inconsistent, "ten human seats are refused at load");
    auto seats = checkpoint::default_seats(replay.setup);
    for (auto& seat : seats) seat.kind = checkpoint::SeatKind::human;
    expect_code(checkpoint::save_tactical_checkpoint(session, replay.commands, seats, build),
        checkpoint::diagnostic_codes::invalid_save, "four human owners on three teams are refused at save");
    container.sections[0].bytes = checkpoint::encode_seats(seats).value();
    expect_code(checkpoint::load_tactical_checkpoint(checkpoint::write_checkpoint(container).value(), build, factory, executor),
        checkpoint::diagnostic_codes::inconsistent, "four human owners on three teams are refused at load");
}

// --- Corpus ------------------------------------------------------------------------------------

int run_corpus(const std::vector<std::string>& paths) {
    const eawr::platform::ThreadWorkerAdapter reference(1);
    const auto factory = checkpoint::content_free_factory();
    for (const auto& path : paths) {
        const auto bytes = read_bytes(path);
        const auto replay = tactical::parse_replay(bytes, path);
        expect(static_cast<bool>(replay), "corpus replay parses: " + path);
        if (!replay) continue;
        const auto ticks = checkpoint::sample_ticks(replay.value().final_tick_count);
        std::size_t largest = 0;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter continued(workers);
            checkpoint::TacticalContinueOptions options;
            options.scramble_after_restore = workers == 4;
            const auto subject = checkpoint::tactical_continue_case(replay.value(), factory, reference, continued, options);
            const auto samples = checkpoint::run_restore_and_continue(subject, ticks);
            if (!samples) {
                expect(false, path + ": " + eawr::core::format_diagnostic(samples.error()));
                break;
            }
            for (const auto& sample : samples.value()) {
                largest = std::max(largest, sample.checkpoint_bytes);
                expect(!sample.first_divergent_tick, path + " continued from tick " + std::to_string(sample.tick) + " on "
                    + std::to_string(workers) + " workers diverges at tick "
                    + std::to_string(sample.first_divergent_tick.value_or(0)));
            }
        }
        std::cout << "restore-and-continue " << path.substr(path.find_last_of("/\\") + 1) << " final "
                  << replay.value().final_tick_count << " ticks";
        for (const auto tick : ticks) std::cout << ' ' << tick;
        std::cout << " largest " << largest << " bytes\n";
    }
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--corpus") {
        const auto result = run_corpus(std::vector<std::string>(argv + 2, argv + argc));
        if (failures != 0) std::cerr << failures << " checkpoint corpus check(s) failed\n";
        return result;
    }
    test_container_round_trip();
    test_container_refusals();
    test_seats();
    test_battle_is_not_trivial();
    test_skirmish_state_checkpoint_union();
    test_tactical_round_trip_and_refusals();
    test_battle_restore_and_continue();
    test_tactical_seat_limits();
    test_harness_reports_divergence();
    test_full_state_staged_and_fog();
    test_projectiles_restore_and_continue();
    test_sliced_search_checkpoint();
    test_published_flags_at_restore_barrier();
    test_mid_cloak_restore_and_continue();
    test_damage_tracking_restore_and_continue();
    if (failures != 0) {
        std::cerr << failures << " checkpoint check(s) failed\n";
        return 1;
    }
    std::cout << "checkpoint contracts passed\n";
    return 0;
}
