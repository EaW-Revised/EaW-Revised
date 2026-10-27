#include "eawr/checkpoint/tactical.hpp"

#include "encoding_internal.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/seat.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>

namespace eawr::checkpoint {

namespace tactical = sim::tactical;
using detail::diagnostic;

namespace {

template <typename T>
[[nodiscard]] core::Result<T> fail(const std::string_view code, std::string message, const std::string_view path = {}) {
    return core::Result<T>::failure(diagnostic(code, std::move(message), path));
}

// A section's own failure, named for the section it came from.
template <typename T>
[[nodiscard]] core::Result<T> fail_in(const std::string_view code, const SectionId section, const core::Diagnostic& inner,
    const std::string_view path) {
    return fail<T>(code, "section " + std::string(section_name(static_cast<std::uint16_t>(section))) + ": "
        + inner.message + (inner.code.empty() ? "" : " (" + inner.code + ")"), path);
}

[[nodiscard]] std::uint64_t canonical_final_tick(const std::uint64_t completed, const std::vector<tactical::PlayerCommand>& commands) {
    return commands.empty() ? completed : std::max(completed, commands.back().key.tick + 1U);
}

[[nodiscard]] std::uint64_t queued_count(const ChildLog& child) {
    return static_cast<std::uint64_t>(std::count_if(child.log.commands.begin(), child.log.commands.end(),
        [&](const tactical::PlayerCommand& command) { return command.key.tick >= child.completed_tick; }));
}

// The tactical layer has no seats (ADR-020: the game session forwards a seat's command as its
// player's), so a battle outside a galaxy seals its last command under its player, and its input
// digest is the SHA-256 of the child log's embedded replay: setup and every accepted command.
[[nodiscard]] InputFrontier tactical_frontier(const ChildLog& child, const std::span<const std::uint8_t> replay_bytes) {
    InputFrontier frontier;
    if (child.log.commands.empty()) return frontier;
    const auto& last = child.log.commands.back().key;
    frontier.tick = last.tick;
    frontier.seat = last.player_id;
    frontier.sequence = last.sequence;
    frontier.input_digest = core::sha256(replay_bytes);
    return frontier;
}

[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_with_replay(const ChildLog& child,
    const std::vector<std::uint8_t>& replay) {
    using Bytes = std::vector<std::uint8_t>;
    if (child.conflict_record.size() > max_conflict_record_bytes) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "the conflict record exceeds "
            + std::to_string(max_conflict_record_bytes) + " bytes");
    }
    if (child.log.final_tick_count != canonical_final_tick(child.completed_tick, child.log.commands)) {
        return fail<Bytes>(diagnostic_codes::invalid_save, "the child log's final tick is not the canonical one");
    }
    if (replay.size() > max_bytes - 52U - child.conflict_record.size()) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "section child-log exceeds the checkpoint limit");
    }
    detail::Writer out;
    out.u64(child.completed_tick);
    out.raw(child.state_sha256);
    out.u32(static_cast<std::uint32_t>(child.conflict_record.size()));
    out.raw(child.conflict_record);
    out.u64(replay.size());
    out.raw(replay);
    return core::Result<Bytes>::success(std::move(out.bytes()));
}

[[nodiscard]] core::Result<std::vector<tactical::PlayerCommand>> checked_queue(const tactical::TacticalSession& session,
    const std::span<const tactical::PlayerCommand> queued) {
    using Commands = std::vector<tactical::PlayerCommand>;
    if (queued.size() != session.pending_command_count()) {
        return fail<Commands>(diagnostic_codes::invalid_save, std::to_string(queued.size())
            + " queued commands given but the battle holds " + std::to_string(session.pending_command_count()));
    }
    for (std::size_t index = 0; index < queued.size(); ++index) {
        if (queued[index].key.tick < session.completed_tick()) {
            return fail<Commands>(diagnostic_codes::invalid_save, "a queued command is keyed to tick "
                + std::to_string(queued[index].key.tick) + ", which has already executed");
        }
        if (index > 0 && !(queued[index - 1].key < queued[index].key)) {
            return fail<Commands>(diagnostic_codes::invalid_save, "queued commands are not in canonical order");
        }
    }
    const auto actual = session.pending_commands();
    if (!std::equal(queued.begin(), queued.end(), actual.begin(), actual.end())) {
        return fail<Commands>(diagnostic_codes::invalid_save, "the supplied commands are not the battle's pending queue");
    }
    return core::Result<Commands>::success(Commands(queued.begin(), queued.end()));
}

[[nodiscard]] core::Result<void> check_tactical_seats(const std::span<const Seat> seats,
    const tactical::TacticalSetup& setup, const std::string_view code, const std::string_view path = {}) {
    std::size_t humans = 0;
    std::set<tactical::PlayerId> human_sides;
    for (const auto& seat : seats) {
        const auto player = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const tactical::Player& value) { return value.player_id == seat.player_id; });
        if (player == setup.players.end()) {
            return fail<void>(code, "section seats: seat " + std::to_string(seat.seat_id.value)
                + " names a player who is not in the battle", path);
        }
        if (seat.kind == SeatKind::human) {
            ++humans;
            // Campaign children also carry SKSU map metadata; campaign ownership
            // retains shared controllers (ADR-020).
            if (!human_sides.insert(player->player_id).second && setup.skirmish && !setup.campaign)
                return fail<void>(code, "section seats: skirmish human seats must have distinct owners", path);
        }
    }
    // ADR-023: command seats are distinct from loaded faction/team rows.
    if (humans > sim::max_match_seats) {
        return fail<void>(code, "section seats: at most nine human seats are supported", path);
    }
    return core::Result<void>::success();
}

} // namespace

core::Result<std::vector<std::uint8_t>> encode_seats(const std::span<const Seat> seats) {
    using Bytes = std::vector<std::uint8_t>;
    if (seats.size() > max_seats) {
        return fail<Bytes>(diagnostic_codes::resource_limit, std::to_string(seats.size()) + " seats exceed the limit of "
            + std::to_string(max_seats));
    }
    detail::Writer out;
    out.u32(static_cast<std::uint32_t>(seats.size()));
    for (std::size_t index = 0; index < seats.size(); ++index) {
        const auto& seat = seats[index];
        if (index > 0 && seats[index - 1].seat_id >= seat.seat_id) {
            return fail<Bytes>(diagnostic_codes::invalid_save, "seat IDs do not strictly increase");
        }
        if (seat.kind != SeatKind::human && seat.kind != SeatKind::ai && seat.kind != SeatKind::script) {
            return fail<Bytes>(diagnostic_codes::invalid_save, "seat " + std::to_string(seat.seat_id.value) + " has an unknown kind");
        }
        out.u32(seat.seat_id.value);
        out.u32(seat.player_id);
        out.u8(static_cast<std::uint8_t>(seat.kind));
        out.u8(seat.active ? 1U : 0U);
        out.u16(0); // reserved
        out.u32(seat.lobby_slot);
    }
    return core::Result<Bytes>::success(std::move(out.bytes()));
}

core::Result<std::vector<Seat>> decode_seats(const std::span<const std::uint8_t> bytes, const std::string_view path) {
    using Seats = std::vector<Seat>;
    detail::Reader in(bytes);
    const auto count = in.u32();
    if (in.failed()) return fail<Seats>(diagnostic_codes::section_invalid, "section seats: truncated count", path);
    if (count > max_seats) {
        return fail<Seats>(diagnostic_codes::resource_limit, "section seats: " + std::to_string(count)
            + " seats exceed the limit of " + std::to_string(max_seats), path);
    }
    if (in.remaining() != static_cast<std::size_t>(count) * 16U) {
        return fail<Seats>(diagnostic_codes::section_invalid, "section seats: length does not match " + std::to_string(count)
            + " seats", path);
    }
    Seats result;
    result.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        Seat seat;
        seat.seat_id = SeatId{in.u32()};
        seat.player_id = in.u32();
        const auto kind = in.u8();
        const auto active = in.u8();
        const auto reserved = in.u16();
        seat.lobby_slot = in.u32();
        if (kind < 1U || kind > 3U || active > 1U || reserved != 0U) {
            return fail<Seats>(diagnostic_codes::section_invalid, "section seats: seat " + std::to_string(seat.seat_id.value)
                + " has an unknown kind, active flag or reserved field", path);
        }
        if (!result.empty() && result.back().seat_id >= seat.seat_id) {
            return fail<Seats>(diagnostic_codes::section_invalid, "section seats: seat IDs do not strictly increase", path);
        }
        seat.kind = static_cast<SeatKind>(kind);
        seat.active = active == 1U;
        result.push_back(seat);
    }
    return core::Result<Seats>::success(std::move(result));
}

std::vector<Seat> default_seats(const tactical::TacticalSetup& setup) {
    std::vector<Seat> result;
    for (const auto& player : setup.players) {
        if (!player.commandable()) continue;
        Seat seat;
        seat.seat_id = SeatId{player.player_id};
        seat.player_id = player.player_id;
        seat.kind = SeatKind::ai;
        if (setup.skirmish) {
            const auto& slots = setup.skirmish->slots;
            const auto slot = std::find_if(slots.begin(), slots.end(),
                [&](const tactical::ReplayLobbySlot& value) { return value.player == player.player_id; });
            if (slot != slots.end()) {
                seat.kind = slot->human ? SeatKind::human : SeatKind::ai;
                seat.lobby_slot = static_cast<std::uint32_t>(slot - slots.begin());
            }
        }
        result.push_back(seat);
    }
    return result;
}

core::Result<std::vector<std::uint8_t>> encode_child_log(const ChildLog& child) {
    auto replay = tactical::write_replay(child.log);
    if (!replay) return fail_in<std::vector<std::uint8_t>>(diagnostic_codes::invalid_save, SectionId::child_log, replay.error(), {});
    return encode_with_replay(child, replay.value());
}

core::Result<ChildLog> decode_child_log(const std::span<const std::uint8_t> bytes, const std::string_view path) {
    if (bytes.size() > max_bytes) {
        return fail<ChildLog>(diagnostic_codes::resource_limit, "section child-log exceeds the checkpoint limit", path);
    }
    const auto invalid = [&](std::string message) {
        return fail<ChildLog>(diagnostic_codes::section_invalid, "section child-log: " + std::move(message), path);
    };
    detail::Reader in(bytes);
    ChildLog child;
    child.completed_tick = in.u64();
    child.state_sha256 = in.digest();
    const auto conflict_length = in.u32();
    if (in.failed()) return invalid("truncated");
    if (conflict_length > max_conflict_record_bytes) {
        return fail<ChildLog>(diagnostic_codes::resource_limit, "section child-log: the conflict record exceeds "
            + std::to_string(max_conflict_record_bytes) + " bytes", path);
    }
    const auto conflict = in.take(conflict_length);
    const auto replay_length = in.u64();
    if (in.failed() || replay_length != in.remaining()) return invalid("the embedded replay's length does not match the section");
    const auto replay_bytes = in.take(static_cast<std::size_t>(replay_length));
    child.conflict_record.assign(conflict.begin(), conflict.end());
    auto replay = tactical::parse_replay(replay_bytes, path);
    if (!replay) return fail_in<ChildLog>(diagnostic_codes::section_invalid, SectionId::child_log, replay.error(), path);
    child.log = std::move(replay).value();
    if (child.log.final_tick_count != canonical_final_tick(child.completed_tick, child.log.commands)) {
        return invalid("the embedded replay's final tick " + std::to_string(child.log.final_tick_count)
            + " is not the canonical one for completed tick " + std::to_string(child.completed_tick));
    }
    // One encoding per battle: a replay that parses but re-encodes differently is refused.
    const auto canonical = tactical::write_replay(child.log);
    if (!canonical || !std::equal(canonical.value().begin(), canonical.value().end(), replay_bytes.begin(), replay_bytes.end())) {
        return invalid("the embedded replay is not canonically encoded");
    }
    return core::Result<ChildLog>::success(std::move(child));
}

core::Result<ChildLog> capture_child_log(const tactical::TacticalSession& session,
    const std::span<const tactical::PlayerCommand> queued, std::vector<std::uint8_t> conflict_record) {
    auto pending = checked_queue(session, queued);
    if (!pending) return core::Result<ChildLog>::failure(pending.error());
    ChildLog child;
    child.conflict_record = std::move(conflict_record);
    child.completed_tick = session.completed_tick();
    if (!detail::from_hex(session.state_sha256(), child.state_sha256)) {
        return fail<ChildLog>(diagnostic_codes::invalid_save, "the battle's state hash is not a SHA-256 digest");
    }
    child.log = session.record();
    auto& commands = child.log.commands;
    commands.insert(commands.end(), pending.value().begin(), pending.value().end());
    child.log.final_tick_count = canonical_final_tick(child.completed_tick, commands);
    return core::Result<ChildLog>::success(std::move(child));
}

TacticalSessionFactory content_free_factory() {
    return [](const tactical::TacticalReplay& replay) { return tactical::TacticalSession::from_replay(replay); };
}

core::Result<tactical::TacticalSession> resimulate_child_log(const ChildLog& child, const TacticalSessionFactory& factory,
    const sim::PartitionExecutor& executor, RestoreWork* work, const std::string_view path) {
    using SessionResult = core::Result<tactical::TacticalSession>;
    auto created = factory(child.log);
    if (!created) return fail_in<tactical::TacticalSession>(diagnostic_codes::restore_failed, SectionId::child_log, created.error(), path);
    auto session = std::move(created).value();
    std::uint64_t stepped = 0;
    while (session.completed_tick() < child.completed_tick) {
        const auto tick = session.step(executor);
        if (!tick) return fail_in<tactical::TacticalSession>(diagnostic_codes::restore_failed, SectionId::child_log, tick.error(), path);
        ++stepped;
    }
    if (session.completed_tick() != child.completed_tick) {
        return fail<tactical::TacticalSession>(diagnostic_codes::restore_failed, "section child-log: the battle starts after tick "
            + std::to_string(child.completed_tick), path);
    }
    if (session.state_sha256() != detail::hex(child.state_sha256)) {
        return fail<tactical::TacticalSession>(diagnostic_codes::restore_failed, "section child-log: the re-simulated battle's state "
            "at tick " + std::to_string(child.completed_tick) + " differs from the saved state (other content or rules)", path);
    }
    if (session.pending_command_count() != queued_count(child)) {
        return fail<tactical::TacticalSession>(diagnostic_codes::restore_failed, "section child-log: the re-simulated battle "
            "queues " + std::to_string(session.pending_command_count()) + " commands, the save " + std::to_string(queued_count(child)), path);
    }
    if (work) work->resimulated_ticks += stepped;
    return SessionResult::success(std::move(session));
}

Identities tactical_identities(const Digest& content) {
    Identities result;
    result.rules_version = tactical::tactical_rules_version;
    result.state_encoding_version = tactical::tactical_state_encoding_version;
    result.content = content;
    return result;
}

core::Result<std::vector<std::uint8_t>> save_tactical_checkpoint(const tactical::TacticalSession& session,
    const std::span<const tactical::PlayerCommand> queued, const std::span<const Seat> seats, const Identities& build) {
    using Bytes = std::vector<std::uint8_t>;
    auto child = capture_child_log(session, queued);
    if (!child) return core::Result<Bytes>::failure(child.error());
    const auto& setup = child.value().log.setup;
    if (build.content != setup.content_identity) {
        return fail<Bytes>(diagnostic_codes::invalid_save, "the build's content identity is not the battle's");
    }
    if (auto valid = check_tactical_seats(seats, setup, diagnostic_codes::invalid_save); !valid)
        return core::Result<Bytes>::failure(valid.error());
    auto seat_bytes = encode_seats(seats);
    if (!seat_bytes) return core::Result<Bytes>::failure(seat_bytes.error());
    auto replay = tactical::write_replay(child.value().log);
    if (!replay) return fail_in<Bytes>(diagnostic_codes::invalid_save, SectionId::child_log, replay.error(), {});
    auto child_bytes = encode_with_replay(child.value(), replay.value());
    if (!child_bytes) return core::Result<Bytes>::failure(child_bytes.error());
    auto state_bytes = session.save_checkpoint_state();
    if (!state_bytes) return fail_in<Bytes>(diagnostic_codes::invalid_save, SectionId::child_state, state_bytes.error(), {});

    Checkpoint checkpoint;
    auto& header = checkpoint.header;
    header.identities = build;
    header.seed = setup.seed;
    header.completed_tick = child.value().completed_tick;
    header.frontier = tactical_frontier(child.value(), replay.value());
    header.flags = static_cast<std::uint16_t>(flag_child | (child.value().log.commands.empty() ? 0U : flag_frontier));
    checkpoint.sections.push_back({static_cast<std::uint16_t>(SectionId::seats), seats_section_version, true,
        std::move(seat_bytes).value()});
    checkpoint.sections.push_back({static_cast<std::uint16_t>(SectionId::child_log), child_log_section_version, true,
        std::move(child_bytes).value()});
    checkpoint.sections.push_back({static_cast<std::uint16_t>(SectionId::child_state), child_state_section_version, true,
        std::move(state_bytes).value()});
    if (session.domain() == tactical::BattleDomain::land) {
        auto land = session.save_land_checkpoint_state();
        if (!land) return fail_in<Bytes>(diagnostic_codes::invalid_save, SectionId::land_state, land.error(), {});
        checkpoint.sections.push_back({static_cast<std::uint16_t>(SectionId::land_state), land_state_section_version,
            false, std::move(land).value()});
    }
    return write_checkpoint(checkpoint);
}

core::Result<TacticalRestore> load_tactical_checkpoint(const std::span<const std::uint8_t> bytes, const Identities& build,
    const TacticalSessionFactory& factory, const sim::PartitionExecutor& executor, const std::string_view path) {
    auto parsed = parse_checkpoint(bytes, path);
    if (!parsed) return core::Result<TacticalRestore>::failure(parsed.error());
    const auto& checkpoint = parsed.value();
    const auto& header = checkpoint.header;
    if (auto identities = check_identities(header, build, path); !identities) {
        return core::Result<TacticalRestore>::failure(identities.error());
    }
    const auto inconsistent = [&](std::string message) {
        return fail<TacticalRestore>(diagnostic_codes::inconsistent, std::move(message), path);
    };
    if ((header.flags & flag_galaxy) != 0U || (header.flags & flag_child) == 0U) {
        return inconsistent("a tactical checkpoint holds exactly one battle and no galaxy");
    }
    for (const auto& section : checkpoint.sections) {
        const auto id = static_cast<SectionId>(section.id);
        if (known_section(section.id) && id != SectionId::seats && id != SectionId::child_log
            && id != SectionId::child_state && id != SectionId::land_state && id != SectionId::presentation_hints) {
            return inconsistent("section " + std::string(section_name(section.id)) + " belongs to a galaxy, not a tactical checkpoint");
        }
    }
    const auto* seats_section = checkpoint.find(SectionId::seats);
    const auto* child_section = checkpoint.find(SectionId::child_log);
    if (!seats_section || !child_section) return inconsistent("a tactical checkpoint needs the seats and child-log sections");
    for (const auto* section : {seats_section, child_section}) {
        const auto expected = section == seats_section ? seats_section_version : child_log_section_version;
        if (section->version != expected) {
            return fail<TacticalRestore>(diagnostic_codes::version, "section " + std::string(section_name(section->id))
                + " version " + std::to_string(section->version) + " is not this build's version " + std::to_string(expected), path);
        }
    }
    auto seats = decode_seats(seats_section->bytes, path);
    if (!seats) return core::Result<TacticalRestore>::failure(seats.error());
    auto child = decode_child_log(child_section->bytes, path);
    if (!child) return core::Result<TacticalRestore>::failure(child.error());
    const auto& setup = child.value().log.setup;
    if (header.seed != setup.seed || header.identities.content != setup.content_identity || header.galactic_frame != 0U
        || header.completed_tick != child.value().completed_tick) {
        return inconsistent("the header's seed, content, frame or completed tick is not the battle's");
    }
    const auto replay = tactical::write_replay(child.value().log);
    const auto frontier = replay ? tactical_frontier(child.value(), replay.value()) : InputFrontier{};
    if (header.frontier != frontier || ((header.flags & flag_frontier) != 0U) != !child.value().log.commands.empty()) {
        return inconsistent("the header's input frontier is not the battle's last command");
    }
    if (auto valid = check_tactical_seats(seats.value(), setup, diagnostic_codes::inconsistent, path); !valid)
        return core::Result<TacticalRestore>::failure(valid.error());
    RestoreWork work;
    work.checkpoint_bytes = bytes.size();
    const auto* state_section = checkpoint.find(SectionId::child_state);
    const auto* land_section = checkpoint.find(SectionId::land_state);
    if ((setup.domain == tactical::BattleDomain::land) != (land_section != nullptr))
        return inconsistent("section land-state and battle domain disagree");
    if (land_section && (land_section->version != land_state_section_version || !state_section))
        return inconsistent("section land-state requires a supported version and child-state");
    auto session = [&]() -> core::Result<tactical::TacticalSession> {
        if (!state_section) return resimulate_child_log(child.value(), factory, executor, &work, path);
        if (state_section->version != child_state_section_version)
            return fail<tactical::TacticalSession>(diagnostic_codes::version, "section child-state: unsupported version", path);
        if (!factory) return fail<tactical::TacticalSession>(diagnostic_codes::restore_failed, "section child-state: no content factory", path);
        auto restored = factory(child.value().log);
        if (!restored) return fail_in<tactical::TacticalSession>(diagnostic_codes::restore_failed, SectionId::child_state, restored.error(), path);
        if (restored.value().completed_tick() != 0 || restored.value().record().setup != child.value().log.setup
            || restored.value().pending_commands() != child.value().log.commands)
            return fail<tactical::TacticalSession>(diagnostic_codes::restore_failed,
                "section child-state: factory must return the saved setup and sealed commands at tick zero", path);
        auto loaded = restored.value().load_checkpoint_state(state_section->bytes);
        if (!loaded) return fail_in<tactical::TacticalSession>(diagnostic_codes::section_invalid, SectionId::child_state, loaded.error(), path);
        if (land_section) {
            auto land = restored.value().load_land_checkpoint_state(land_section->bytes);
            if (!land) return fail_in<tactical::TacticalSession>(diagnostic_codes::section_invalid, SectionId::land_state, land.error(), path);
            auto encoded_land = restored.value().save_land_checkpoint_state();
            if (!encoded_land || encoded_land.value() != land_section->bytes)
                return fail<tactical::TacticalSession>(diagnostic_codes::section_invalid,
                    "section land-state: noncanonical restored state", path);
        }
        if (restored.value().completed_tick() != child.value().completed_tick
            || core::sha256(restored.value().canonical_state_bytes()) != child.value().state_sha256
            || restored.value().pending_command_count() != queued_count(child.value()))
            return fail<tactical::TacticalSession>(diagnostic_codes::restore_failed, "section child-state: restored state disagrees with child-log", path);
        auto encoded = restored.value().save_checkpoint_state();
        if (!encoded || encoded.value() != state_section->bytes)
            return fail<tactical::TacticalSession>(diagnostic_codes::section_invalid, "section child-state: noncanonical restored state", path);
        return restored;
    }();
    if (!session) return core::Result<TacticalRestore>::failure(session.error());
    return core::Result<TacticalRestore>::success(
        TacticalRestore{std::move(session).value(), std::move(seats).value(), header, work});
}

core::Result<void> restore_tactical_checkpoint(TacticalRestore& running, const std::span<const std::uint8_t> bytes,
    const Identities& build, const TacticalSessionFactory& factory, const sim::PartitionExecutor& executor,
    const std::string_view path) {
    auto loaded = load_tactical_checkpoint(bytes, build, factory, executor, path);
    if (!loaded) return core::Result<void>::failure(loaded.error());
    running = std::move(loaded).value();
    return core::Result<void>::success();
}

} // namespace eawr::checkpoint
