#include "eawr/checkpoint/game.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "encoding_internal.hpp"
#include "game_session_internal.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>
#include <stdexcept>
#include <type_traits>

namespace eawr::checkpoint {
namespace gc = sim::galactic;
namespace {
using Bytes = std::vector<std::uint8_t>;
template <class T> core::Result<T> fail(std::string message, std::string_view path = {},
    std::string_view code = diagnostic_codes::section_invalid) {
    return core::Result<T>::failure(detail::diagnostic(code, std::move(message), path));
}
void blob(detail::Writer& out, std::span<const std::uint8_t> value) { out.u64(value.size()); out.raw(value); }
std::span<const std::uint8_t> blob(detail::Reader& in) {
    const auto size = in.u64();
    return in.take(size > in.remaining() ? in.remaining() + 1 : static_cast<std::size_t>(size));
}
void text(detail::Writer& out, const std::string& value) {
    blob(out, std::span(reinterpret_cast<const std::uint8_t*>(value.data()), value.size()));
}
std::string text(detail::Reader& in) {
    const auto value = blob(in);
    return std::string(value.begin(), value.end());
}
namespace auth = script::authoritative;
void script_value(detail::Writer& out, const auth::Value& value, int depth = 0) {
    if (depth > auth::max_value_list_depth) throw std::length_error("script value nesting exceeds limit");
    out.u8(static_cast<std::uint8_t>(value.data.index()));
    std::visit([&](const auto& item) {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, bool>) out.u8(item ? 1 : 0);
        else if constexpr (std::is_same_v<T, script::numeric::LuaNumber>) out.u64(item.repr);
        else if constexpr (std::is_same_v<T, std::string>) text(out, item);
        else if constexpr (std::is_same_v<T, auth::Handle>) { out.u32(item.kind); out.u64(item.id); }
        else if constexpr (std::is_same_v<T, auth::ValueList>) {
            out.u64(item.size()); for (const auto& child : item) script_value(out, child, depth + 1);
        }
    }, value.data);
}
auth::Value script_value(detail::Reader& in, int depth = 0) {
    const auto reject = [&]() { (void)in.take(in.remaining() + 1); return auth::Value{}; };
    if (depth > auth::max_value_list_depth || in.failed()) return reject();
    switch (in.u8()) {
    case 0: return {};
    case 1: { const auto flag = in.u8(); if (flag > 1) return reject(); return auth::Value{flag != 0}; }
    case 2: return auth::Value::number(script::numeric::LuaNumber::from_repr(in.u64()));
    case 3: return auth::Value::text(text(in));
    case 4: { const auto kind = in.u32(); return auth::Value{auth::Handle{kind, in.u64()}}; }
    case 5: {
        const auto count = in.u64(); if (count > in.remaining()) return reject();
        auth::ValueList list;
        for (std::uint64_t i = 0; i < count && !in.failed(); ++i) list.push_back(script_value(in, depth + 1));
        return auth::Value{std::move(list)};
    }
    default: return reject();
    }
}
core::Result<Bytes> encode_scripts(const std::optional<auth::GalacticScriptSave>& saved) {
    detail::Writer out; out.u32(saved ? 1 : 0);
    if (!saved) return core::Result<Bytes>::success(std::move(out.bytes()));
    const auto& row = *saved;
    out.u64(row.completed_tick); out.u64(row.galactic_frame);
    text(out, row.world_sha256); text(out, row.scripts_sha256); text(out, row.ai); text(out, row.story);
    out.u64(row.pending.size());
    try {
        for (const auto& command : row.pending) {
            out.u64(command.tick); out.u64(command.issuer); out.u64(command.sequence); text(out, command.verb);
            script_value(out, auth::Value{command.arguments});
            if (out.bytes().size() > max_bytes) return fail<Bytes>("section scripts exceeds limit", {}, diagnostic_codes::resource_limit);
        }
    } catch (const std::length_error& error) { return fail<Bytes>(error.what()); }
    out.u64(row.next_sequence.size());
    for (const auto& [seat, sequence] : row.next_sequence) { out.u32(seat.value); out.u64(sequence); }
    out.u64(row.exhausted_seats.size()); for (const auto seat : row.exhausted_seats) out.u32(seat.value);
    if (out.bytes().size() > max_bytes) return fail<Bytes>("section scripts exceeds limit", {}, diagnostic_codes::resource_limit);
    return core::Result<Bytes>::success(std::move(out.bytes()));
}
core::Result<std::optional<auth::GalacticScriptSave>> decode_scripts(std::span<const std::uint8_t> bytes,
    std::string_view path, std::size_t& consumed) {
    using Save = std::optional<auth::GalacticScriptSave>;
    detail::Reader in(bytes); const auto presence = in.u32();
    if (presence > 1 || in.failed()) return fail<Save>("section scripts: invalid presence", path);
    Save saved;
    if (presence == 1) {
        saved.emplace(); auto& row = *saved;
        row.completed_tick = in.u64(); row.galactic_frame = in.u64();
        row.world_sha256 = text(in); row.scripts_sha256 = text(in); row.ai = text(in); row.story = text(in);
        const auto pending = in.u64();
        if (pending > gc::galactic_max_commands || pending > in.remaining() / 33) return fail<Save>("section scripts: invalid pending count", path);
        for (std::uint64_t i = 0; i < pending && !in.failed(); ++i) {
            auth::ScriptCommand command;
            command.tick = in.u64(); command.issuer = in.u64(); command.sequence = in.u64(); command.verb = text(in);
            auto args = script_value(in);
            const auto* list = std::get_if<auth::ValueList>(&args.data);
            if (!list) return fail<Save>("section scripts: arguments must be a list", path);
            command.arguments = *list; row.pending.push_back(std::move(command));
        }
        const auto cursors = in.u64();
        if (cursors > gc::galactic_max_seats || cursors > in.remaining() / 12) return fail<Save>("section scripts: invalid cursor count", path);
        for (std::uint64_t i = 0; i < cursors; ++i) {
            const auto seat = sim::SeatId{in.u32()}; const auto sequence = in.u64();
            if (!row.next_sequence.emplace(seat, sequence).second) return fail<Save>("section scripts: duplicate cursor", path);
        }
        const auto exhausted = in.u64();
        if (exhausted > gc::galactic_max_seats || exhausted > in.remaining() / 4) return fail<Save>("section scripts: invalid exhaustion count", path);
        for (std::uint64_t i = 0; i < exhausted; ++i)
            if (!row.exhausted_seats.insert(sim::SeatId{in.u32()}).second) return fail<Save>("section scripts: duplicate exhausted seat", path);
    }
    if (in.failed()) return fail<Save>("section scripts: malformed lengths", path);
    consumed = bytes.size() - in.remaining();
    auto canonical = encode_scripts(saved);
    if (!canonical || !std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(consumed), canonical.value().begin(), canonical.value().end()))
        return fail<Save>("section scripts: noncanonical encoding", path);
    return core::Result<Save>::success(std::move(saved));
}
// Coordinator-reserved: optional child-host tail in unreleased galaxy section 9 v1.
// With no child the AI/story section keeps its original bytes exactly.
core::Result<Bytes> encode_child_host(const campaign::BattleCheckpoint& saved) {
    detail::Writer out; out.u32(4); out.u32(static_cast<std::uint32_t>(saved.timeline)); out.u64(saved.timeline_tick);
    out.u64(saved.scripts.completed_tick); text(out, saved.scripts.scheduler);
    out.u32(saved.scripts.native ? 1 : 0);
    if (saved.scripts.native) {
        out.u64(saved.scripts.native->size());
        out.bytes().insert(out.bytes().end(), saved.scripts.native->begin(), saved.scripts.native->end());
    }
    out.u64(saved.scripts.pending.size());
    try {
        for (const auto& command : saved.scripts.pending) {
            out.u64(command.tick); out.u64(command.issuer); out.u64(command.sequence); text(out, command.verb);
            script_value(out, auth::Value{command.arguments});
            if (out.bytes().size() > max_bytes) return fail<Bytes>("child host exceeds limit", {}, diagnostic_codes::resource_limit);
        }
    } catch (const std::length_error& error) { return fail<Bytes>(error.what()); }
    out.u64(saved.scripts.last_keys.size());
    for (const auto& [player, key] : saved.scripts.last_keys) { out.u32(player); out.u64(key.first); out.u64(key.second); }
    out.u64(saved.sequences.size());
    for (const auto& [player, sequence] : saved.sequences) { out.u32(player); out.u64(sequence); }
    out.u64(saved.killers.size());
    for (const auto& [id, killer] : saved.killers) { out.u64(id); out.u32(killer); }
    if (out.bytes().size() > max_bytes) return fail<Bytes>("child host exceeds limit", {}, diagnostic_codes::resource_limit);
    return core::Result<Bytes>::success(std::move(out.bytes()));
}
core::Result<std::optional<campaign::BattleCheckpoint>> decode_child_host(std::span<const std::uint8_t> bytes,
    std::string_view path) {
    using Save = std::optional<campaign::BattleCheckpoint>;
    if (bytes.empty()) return core::Result<Save>::success({});
    detail::Reader in(bytes); campaign::BattleCheckpoint saved;
    if (in.u32() != 4) return fail<Save>("section scripts: invalid child host version", path);
    const auto timeline = in.u32();
    if (timeline > 5) return fail<Save>("section scripts: invalid child timeline", path);
    saved.timeline = static_cast<std::int32_t>(timeline);
    saved.timeline_tick = in.u64();
    saved.scripts.completed_tick = in.u64(); saved.scripts.scheduler = text(in);
    const auto native = in.u32();
    if (native > 1) return fail<Save>("section scripts: invalid native child presence", path);
    if (native) {
        const auto size = in.u64();
        if (size > max_bytes || size > in.remaining()) return fail<Save>("section scripts: native child exceeds limit", path);
        const auto payload = in.take(static_cast<std::size_t>(size));
        saved.scripts.native.emplace(payload.begin(), payload.end());
    }
    const auto count = [&](std::uint64_t limit, std::size_t width) {
        const auto value = in.u64();
        if (value > limit || value > in.remaining() / width) { (void)in.take(in.remaining() + 1); return std::uint64_t{}; }
        return value;
    };
    const auto pending = count(gc::galactic_max_commands, 33);
    for (std::uint64_t i = 0; i < pending && !in.failed(); ++i) {
        auth::ScriptCommand command;
        command.tick = in.u64(); command.issuer = in.u64(); command.sequence = in.u64(); command.verb = text(in);
        auto args = script_value(in); const auto* list = std::get_if<auth::ValueList>(&args.data);
        if (!list) return fail<Save>("section scripts: child arguments must be a list", path);
        command.arguments = *list; saved.scripts.pending.push_back(std::move(command));
    }
    const auto keys = count(gc::galactic_max_players, 20);
    for (std::uint64_t i = 0; i < keys; ++i) {
        const auto player = in.u32(); const auto tick = in.u64(); const auto sequence = in.u64();
        if (!saved.scripts.last_keys.emplace(player, std::pair(tick, sequence)).second)
            return fail<Save>("section scripts: duplicate child routing cursor", path);
    }
    const auto sequences = count(gc::galactic_max_players, 12);
    for (std::uint64_t i = 0; i < sequences; ++i) {
        const auto player = in.u32(); const auto sequence = in.u64();
        if (!saved.sequences.emplace(player, sequence).second) return fail<Save>("section scripts: duplicate battle cursor", path);
    }
    const auto killers = count(gc::galactic_max_objects, 12);
    for (std::uint64_t i = 0; i < killers; ++i) {
        const auto id = in.u64(); const auto killer = in.u32();
        if (!saved.killers.emplace(id, killer).second) return fail<Save>("section scripts: duplicate child death", path);
    }
    if (in.failed() || in.remaining() != 0) return fail<Save>("section scripts: malformed child host", path);
    auto canonical = encode_child_host(saved);
    if (!canonical || !std::equal(bytes.begin(), bytes.end(), canonical.value().begin(), canonical.value().end()))
        return fail<Save>("section scripts: noncanonical child host", path);
    return core::Result<Save>::success(std::move(saved));
}
core::Result<Bytes> encode_conflict(const campaign::ConflictRecord& record) {
    sim::tactical::TacticalReplay replay; replay.setup = record.tactical;
    auto tactical = sim::tactical::write_replay(replay);
    if (!tactical) return core::Result<Bytes>::failure(tactical.error());
    detail::Writer out;
    out.u64(record.id); out.u8(record.layer == campaign::BattleLayer::land ? 1 : 0);
    out.u64(record.planet); out.u32(record.invader); out.u32(record.defender);
    for (const auto flag : {record.raid, record.corruption_mission, record.tutorial, record.ai_tactical_credits}) out.u8(flag ? 1 : 0);
    out.u64(record.corruption_request);
    out.u32(static_cast<std::uint32_t>(record.forces.size()));
    for (const auto& row : record.forces) {
        out.u64(row.id); out.u64(row.type); out.u32(row.owner); out.u64(row.parent);
        out.u8(static_cast<std::uint8_t>(row.role));
        out.u8(row.destroy_after_tactical ? 1 : 0); out.u8(row.hero ? 1 : 0);
        out.u64(row.tactical_type); out.u64(row.carrier); out.u64(row.purchase_type);
    }
    out.u32(static_cast<std::uint32_t>(record.seats.size()));
    for (const auto& row : record.seats) { out.u32(row.seat.value); out.u32(row.player); }
    out.u64(record.station); out.u64(record.superweapon); out.u32(record.station_owner); out.u64(record.station_type);
    out.u32(record.station_level); out.u32(record.terrain_index);
    text(out, record.map_identity); text(out, record.story_plot); blob(out, record.linked_mission_options);
    out.u32(static_cast<std::uint32_t>(record.structure_markers.size()));
    for (const auto& [id, marker] : record.structure_markers) { out.u64(id); out.u32(marker); }
    out.u32(static_cast<std::uint32_t>(record.upgrades.size()));
    for (const auto& row : record.upgrades) { out.u64(row.type); out.u64(row.holder); out.u32(row.owner); }
    blob(out, tactical.value());
    if (out.bytes().size() > max_conflict_record_bytes) return fail<Bytes>("section conflicts: record exceeds limit", {}, diagnostic_codes::resource_limit);
    return core::Result<Bytes>::success(std::move(out.bytes()));
}
core::Result<campaign::ConflictRecord> decode_conflict(std::span<const std::uint8_t> bytes, std::string_view path) {
    using Record = campaign::ConflictRecord;
    if (bytes.size() > max_conflict_record_bytes) return fail<Record>("section conflicts: record exceeds limit", path, diagnostic_codes::resource_limit);
    detail::Reader in(bytes); Record record;
    const auto flag = [&]() { const auto value = in.u8(); if (value > 1) (void)in.take(in.remaining() + 1); return value != 0; };
    record.id = in.u64(); record.layer = flag() ? campaign::BattleLayer::land : campaign::BattleLayer::space;
    record.planet = in.u64(); record.invader = in.u32(); record.defender = in.u32();
    record.raid = flag(); record.corruption_mission = flag(); record.tutorial = flag(); record.ai_tactical_credits = flag();
    record.corruption_request = in.u64();
    const auto count = [&](std::size_t limit, std::size_t width) {
        const auto value = in.u32();
        if (value > limit || value > in.remaining() / width) { (void)in.take(in.remaining() + 1); return std::uint32_t{}; }
        return value;
    };
    const auto forces = count(gc::galactic_max_objects, 55);
    for (std::uint32_t i = 0; i < forces; ++i) {
        campaign::ConflictForce row;
        row.id = in.u64(); row.type = in.u64(); row.owner = in.u32(); row.parent = in.u64();
        const auto role = in.u8();
        if (role > static_cast<std::uint8_t>(campaign::ForceRole::orbital)) return fail<Record>("section conflicts: invalid force role", path);
        row.role = static_cast<campaign::ForceRole>(role);
        row.destroy_after_tactical = flag(); row.hero = flag(); row.tactical_type = in.u64(); row.carrier = in.u64();
        row.purchase_type = in.u64();
        record.forces.push_back(row);
    }
    const auto seats = count(gc::galactic_max_seats, 8);
    for (std::uint32_t i = 0; i < seats; ++i) { campaign::BattleSeat row; row.seat = sim::SeatId{in.u32()}; row.player = in.u32(); record.seats.push_back(row); }
    record.station = in.u64(); record.superweapon = in.u64(); record.station_owner = in.u32(); record.station_type = in.u64();
    record.station_level = in.u32(); record.terrain_index = in.u32();
    record.map_identity = text(in); record.story_plot = text(in);
    const auto options = blob(in); record.linked_mission_options.assign(options.begin(), options.end());
    const auto markers = count(gc::galactic_max_objects, 12);
    for (std::uint32_t i = 0; i < markers; ++i) {
        const auto id = in.u64(); const auto marker = in.u32(); record.structure_markers.emplace_back(id, marker);
    }
    const auto upgrades = count(gc::galactic_max_objects, 20);
    for (std::uint32_t i = 0; i < upgrades; ++i) {
        campaign::ReturnedUpgrade row; row.type = in.u64(); row.holder = in.u64(); row.owner = in.u32(); record.upgrades.push_back(row);
    }
    const auto replay_bytes = blob(in);
    if (in.failed() || in.remaining() != 0) return fail<Record>("section conflicts: malformed record", path);
    auto replay = sim::tactical::parse_replay(replay_bytes, path);
    if (!replay || !replay.value().commands.empty() || replay.value().final_tick_count != 0)
        return fail<Record>("section conflicts: invalid tactical entry", path);
    record.tactical = std::move(replay).value().setup;
    auto canonical = encode_conflict(record);
    if (!canonical || !std::equal(bytes.begin(), bytes.end(), canonical.value().begin(), canonical.value().end()))
        return fail<Record>("section conflicts: noncanonical record", path);
    return core::Result<Record>::success(std::move(record));
}

Bytes encode_result(const campaign::BattleResult& result) {
    detail::Writer out;
    out.u64(result.conflict); out.u32(result.winner); out.u8(result.retreat_player ? 1 : 0);
    if (result.retreat_player) out.u32(*result.retreat_player);
    out.u32(static_cast<std::uint32_t>(result.forces.size()));
    for (const auto& row : result.forces) {
        out.u64(row.id); out.u8(static_cast<std::uint8_t>(row.disposition));
        out.u32(row.final_blow_player); out.u8(row.hero_defeated ? 1 : 0);
    }
    out.u32(result.station_level); out.u8(result.superweapon_destroyed ? 1 : 0); out.u32(result.superweapon_killer);
    out.u8(result.corruption_success ? 1 : 0); if (result.corruption_success) out.u8(*result.corruption_success ? 1 : 0);
    out.u32(static_cast<std::uint32_t>(result.credits.size()));
    for (const auto& row : result.credits) { out.u32(row.player); out.u64(static_cast<std::uint64_t>(row.credits.raw())); }
    out.u32(static_cast<std::uint32_t>(result.upgrades.size()));
    for (const auto& row : result.upgrades) { out.u64(row.type); out.u64(row.holder); out.u32(row.owner); }
    out.u32(static_cast<std::uint32_t>(result.casualties.size()));
    for (const auto& row : result.casualties) {
        out.u64(row.id); out.u64(row.tactical_type); out.u64(row.original_type); out.u32(row.owner); out.u32(row.final_blow_player);
    }
    out.u32(static_cast<std::uint32_t>(result.invasion_offers.size()));
    for (const auto& row : result.invasion_offers) {
        out.u64(row.planet); out.u64(row.fleet); out.u32(row.player);
        out.u32(static_cast<std::uint32_t>(row.transports.size()));
        for (const auto id : row.transports) out.u64(id);
    }
    out.u8(result.bounty ? 1 : 0); if (result.bounty) out.u64(static_cast<std::uint64_t>(result.bounty->raw()));
    out.u64(result.tactical_frames); blob(out, result.reserved_health); return std::move(out.bytes());
}
core::Result<campaign::BattleResult> decode_result(std::span<const std::uint8_t> bytes, std::string_view path) {
    using Result = campaign::BattleResult;
    detail::Reader in(bytes); Result result;
    const auto flag = [&] { const auto value = in.u8(); if (value > 1) (void)in.take(in.remaining() + 1); return value != 0; };
    const auto count = [&](std::size_t limit, std::size_t width) {
        const auto value = in.u32();
        if (value > limit || value > in.remaining() / width) { (void)in.take(in.remaining() + 1); return std::uint32_t{}; }
        return value;
    };
    result.conflict = in.u64(); result.winner = in.u32(); if (flag()) result.retreat_player = in.u32();
    const auto forces = count(gc::galactic_max_objects, 14);
    for (std::uint32_t i = 0; i < forces; ++i) {
        campaign::ForceResult row; row.id = in.u64(); const auto disposition = in.u8();
        if (disposition > static_cast<std::uint8_t>(campaign::ForceDisposition::destroy_after_tactical))
            return fail<Result>("section tactical return: invalid disposition", path);
        row.disposition = static_cast<campaign::ForceDisposition>(disposition); row.final_blow_player = in.u32();
        row.hero_defeated = flag(); result.forces.push_back(row);
    }
    result.station_level = in.u32(); result.superweapon_destroyed = flag(); result.superweapon_killer = in.u32();
    if (flag()) result.corruption_success = flag();
    const auto credits = count(gc::galactic_max_players, 12);
    for (std::uint32_t i = 0; i < credits; ++i) {
        const auto player = in.u32(); const auto raw = std::bit_cast<std::int64_t>(in.u64());
        result.credits.push_back({player, sim::math::Fixed::from_raw(raw)});
    }
    const auto upgrades = count(gc::galactic_max_objects, 20);
    for (std::uint32_t i = 0; i < upgrades; ++i) {
        campaign::ReturnedUpgrade row; row.type = in.u64(); row.holder = in.u64(); row.owner = in.u32(); result.upgrades.push_back(row);
    }
    const auto casualties = count(gc::galactic_max_objects, 32);
    for (std::uint32_t i = 0; i < casualties; ++i) {
        campaign::BattleCasualty row; row.id = in.u64(); row.tactical_type = in.u64(); row.original_type = in.u64();
        row.owner = in.u32(); row.final_blow_player = in.u32(); result.casualties.push_back(row);
    }
    const auto offers = count(gc::galactic_max_objects, 20);
    for (std::uint32_t i = 0; i < offers; ++i) {
        campaign::InvasionOffer row; row.planet = in.u64(); row.fleet = in.u64(); row.player = in.u32();
        for (auto j = count(gc::galactic_max_objects, 8); j > 0; --j) row.transports.push_back(in.u64());
        result.invasion_offers.push_back(std::move(row));
    }
    if (flag()) result.bounty = sim::math::Fixed::from_raw(std::bit_cast<std::int64_t>(in.u64()));
    result.tactical_frames = in.u64(); const auto health = blob(in); result.reserved_health.assign(health.begin(), health.end());
    if (in.failed() || in.remaining() != 0) return fail<Result>("section tactical return: malformed result", path);
    const auto canonical = encode_result(result);
    if (!std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end())) return fail<Result>("section tactical return: noncanonical result", path);
    return core::Result<Result>::success(std::move(result));
}
// Reuse the state owner's bytes; never duplicate galaxy field encodings in this adapter.
std::array<Bytes, gc::state_section_count> galaxy_sections(const campaign::GameSession& session) {
    const auto bytes = session.galaxy().canonical_state_bytes(); detail::Reader in(bytes);
    (void)in.take(12); const auto count = in.u32();
    std::array<Bytes, gc::state_section_count> sections;
    for (std::size_t index = 0; index < count; ++index) {
        const auto id = in.u16();
        auto& section = sections.at(id);
        (void)in.u16(); const auto value = blob(in);
        section.assign(value.begin(), value.end());
    }
    return sections;
}
core::Result<Checkpoint> capture(const campaign::GameSession& session, const Identities& build) {
    if (build.content != session.galaxy().setup().content_identity || build.campaign != session.galaxy().setup().campaign_identity)
        return fail<Checkpoint>("save identities disagree with the galaxy", {}, diagnostic_codes::invalid_save);
    if (build.mod_list != Digest{}) return fail<Checkpoint>("mod-list participation is not implemented", {}, diagnostic_codes::invalid_save);
    auto recorded = session.checkpoint_recording();
    if (!recorded) return core::Result<Checkpoint>::failure(recorded.error());
    auto replay = gc::write_replay(recorded.value().commands);
    if (!replay) return core::Result<Checkpoint>::failure(replay.error());
    auto state = galaxy_sections(session);
    Checkpoint out; out.header.flags = flag_galaxy;
    out.header.identities = build; out.header.seed = session.galaxy().setup().seed;
    out.header.completed_tick = session.completed_tick(); out.header.galactic_frame = session.galaxy().clock().galactic_frame;
    // #4058: the digest chains the history base through this window; the key is the last sealed command, even when
    // the window holds none.
    auto sealed = campaign::history_frontier_of(recorded.value(), replay.value(), session.completed_tick());
    if (!sealed) return core::Result<Checkpoint>::failure(sealed.error());
    if (const auto& key = sealed.value().last_key) {
        out.header.flags |= flag_frontier;
        out.header.frontier = {key->tick, key->seat.value, key->sequence, sealed.value().digest};
    }
    const auto add = [&](SectionId id, Bytes bytes, std::uint16_t version = galaxy_section_version) {
        out.sections.push_back({static_cast<std::uint16_t>(id), version, true, std::move(bytes)});
    };
    detail::Writer context;
    context.u64(session.completed_tick()); Digest hash{};
    if (!detail::from_hex(session.state_sha256(), hash)) return fail<Checkpoint>("invalid game digest", {}, diagnostic_codes::invalid_save);
    context.raw(hash); context.u64(static_cast<std::uint64_t>(session.galaxy().clock().strategic_speed.raw()));
    context.u64(static_cast<std::uint64_t>(session.galaxy().clock().tactical_speed.raw()));
    blob(context, state[0]); blob(context, replay.value());
    detail::write_history_base(context, recorded.value().base);
    context.u8(recorded.value().multiplayer_vote_timeout ? 1 : 0);
    if (recorded.value().multiplayer_vote_timeout) context.u64(*recorded.value().multiplayer_vote_timeout);
    context.u32(static_cast<std::uint32_t>(recorded.value().load_fixups.size()));
    for (const auto& command : recorded.value().load_fixups) {
        context.u64(command.key.tick); context.u32(command.key.seat.value); context.u64(command.key.sequence);
        blob(context, gc::encode_payload(command.payload));
    }
    const auto timelines = session.save_timeline();
    context.u32(static_cast<std::uint32_t>(timelines.galaxy));
    context.u8(timelines.historical_child ? 1 : 0);
    if (timelines.historical_child) {
        const auto& row = *timelines.historical_child;
        context.u64(row.mode.conflict); context.u64(row.completed_tick); context.u32(static_cast<std::uint32_t>(row.value));
    }
    add(SectionId::session, std::move(context.bytes()));
    // The v1 seats DTO stays unchanged; explicit human sides are in the galaxy session bytes.
    std::vector<Seat> seats;
    for (const auto& row : session.galaxy().seats())
        seats.push_back({row.seat_id, row.player, row.kind, row.active, row.lobby_slot});
    auto encoded_seats = encode_seats(seats);
    if (!encoded_seats) return core::Result<Checkpoint>::failure(encoded_seats.error());
    add(SectionId::seats, std::move(encoded_seats).value());
    add(SectionId::players, std::move(state[1]));
    detail::Writer planets; blob(planets, state[2]); blob(planets, state[3]);
    if (session.galaxy().graph()) blob(planets, state[5]);
    if (session.galaxy().setup().control) blob(planets, state[9]);
    if (session.galaxy().setup().observer) blob(planets, state[13]);
    if (session.galaxy().setup().economy_corruption) blob(planets, state[17]);
    blob(planets, state[18]); // conditional hashed native invasion queue, always framed
    blob(planets, state[19]);
    add(SectionId::planets, std::move(planets.bytes()));
    if (session.galaxy().setup().fleet_control) {
        detail::Writer fleets; blob(fleets, state[4]); blob(fleets, state[6]);
        if (session.galaxy().setup().movement) blob(fleets, state[7]);
        add(SectionId::fleets, std::move(fleets.bytes()));
    } else add(SectionId::fleets, std::move(state[4]));
    if (session.galaxy().setup().income) {
        detail::Writer production;
        blob(production, session.galaxy().setup().production ? state[8] : Bytes(4, 0));
        if (session.galaxy().setup().census) blob(production, state[11]);
        blob(production, state[15]);
        add(SectionId::production, std::move(production.bytes()));
    } else if (session.galaxy().setup().census) {
        // Opt-in population inputs/totals travel with the existing production DTO.
        detail::Writer production; blob(production, state[8]); blob(production, state[11]);
        add(SectionId::production, std::move(production.bytes()));
    } else add(SectionId::production, session.galaxy().setup().production ? std::move(state[8]) : Bytes(4, 0));
    detail::Writer conflicts; conflicts.u32(static_cast<std::uint32_t>(recorded.value().conflicts.size()));
    for (const auto& entry : recorded.value().conflicts) {
        auto encoded = encode_conflict(entry.conflict);
        if (!encoded) return core::Result<Checkpoint>::failure(encoded.error());
        conflicts.u64(entry.entry_tick); blob(conflicts, encoded.value());
    }
    // Opt-in tail preserves the existing FT-08 conflict section for older recordings.
    if (recorded.value().conflict_services) {
        conflicts.u32(static_cast<std::uint32_t>(recorded.value().admissions.size()));
        for (const auto& entry : recorded.value().admissions) {
            auto encoded = encode_conflict(entry.conflict);
            if (!encoded) return core::Result<Checkpoint>::failure(encoded.error());
            conflicts.u64(entry.entry_tick); blob(conflicts, encoded.value());
            conflicts.u8(static_cast<std::uint8_t>(entry.arbitration.route));
            conflicts.u8(entry.arbitration.forced_tactical ? 1 : 0);
            conflicts.u8(entry.arbitration.overrun ? 1 : 0);
        }
    }
    add(SectionId::conflicts, std::move(conflicts.bytes()));
    add(SectionId::victory, session.galaxy().setup().victory ? std::move(state[16]) : Bytes(4, 0));
    auto script_save = session.save_galactic_scripts();
    if (!script_save) return core::Result<Checkpoint>::failure(script_save.error());
    auto script_bytes = encode_scripts(script_save.value());
    if (!script_bytes) return core::Result<Checkpoint>::failure(script_bytes.error());
    auto child_save = session.save_battle_checkpoint();
    if (!child_save) return core::Result<Checkpoint>::failure(child_save.error());
    if (child_save.value()) {
        auto host = encode_child_host(*child_save.value());
        if (!host) return core::Result<Checkpoint>::failure(host.error());
        if (host.value().size() > max_bytes - script_bytes.value().size())
            return fail<Checkpoint>("section scripts exceeds limit", {}, diagnostic_codes::resource_limit);
        script_bytes.value().insert(script_bytes.value().end(), host.value().begin(), host.value().end());
    }
    add(SectionId::scripts, std::move(script_bytes).value());
    if (session.battle()) {
        out.header.flags |= flag_child;
        auto conflict = encode_conflict(*session.conflict());
        if (!conflict) return core::Result<Checkpoint>::failure(conflict.error());
        ChildLog child; child.completed_tick = session.battle()->completed_tick();
        child.log = *session.battle_record(); child.conflict_record = std::move(conflict).value();
        if (!detail::from_hex(session.battle()->state_sha256(), child.state_sha256)) return fail<Checkpoint>("invalid child hash");
        auto encoded = encode_child_log(child);
        if (!encoded) return core::Result<Checkpoint>::failure(encoded.error());
        add(SectionId::child_log, std::move(encoded).value(), child_log_section_version);
        add(SectionId::child_state, std::move(child_save.value()->state), child_state_section_version);
    }
    add(SectionId::mod_list, Bytes(4, 0));
    auto returned = session.save_return_state();
    if (!returned) return core::Result<Checkpoint>::failure(returned.error());
    detail::Writer return_state;
    blob(return_state, returned.value());
    const auto* result = session.latest_tactical_result();
    const auto* result_context = session.latest_tactical_context();
    if ((result != nullptr) != (result_context != nullptr)) return fail<Checkpoint>("incomplete latest tactical result pair");
    return_state.u8(result ? 1 : 0);
    if (result) {
        auto encoded_context = encode_conflict(*result_context);
        if (!encoded_context) return core::Result<Checkpoint>::failure(encoded_context.error());
        blob(return_state, encoded_context.value()); blob(return_state, encode_result(*result));
    }
    out.sections.push_back({static_cast<std::uint16_t>(SectionId::tactical_return), 1, true, std::move(return_state.bytes())});
    auto ai_save = session.save_galactic_ai();
    if (!ai_save) return core::Result<Checkpoint>::failure(ai_save.error());
    auto ai_bytes = campaign::encode_galactic_ai_save(ai_save.value());
    if (!ai_bytes) return core::Result<Checkpoint>::failure(ai_bytes.error());
    out.sections.push_back({static_cast<std::uint16_t>(SectionId::galactic_ai), 1, true, std::move(ai_bytes).value()});
    add(SectionId::galactic_abilities, std::move(state[20]));
    add(SectionId::campaign_scoring, std::move(state[21]));
    auto owner = session.save_checkpoint_state();
    if (!owner) return core::Result<Checkpoint>::failure(owner.error());
    add(SectionId::galactic_owner, std::move(owner).value());
    // Check total size before the final container allocation.
    std::size_t total = header_size;
    for (const auto& section : out.sections) {
        if (total > max_bytes - section_header_size || section.bytes.size() > max_bytes - total - section_header_size)
            return fail<Checkpoint>("game checkpoint exceeds the byte limit", {}, diagnostic_codes::resource_limit);
        total += section_header_size + section.bytes.size();
    }
    return core::Result<Checkpoint>::success(std::move(out));
}
} // namespace

Identities game_identities(const gc::GalacticSetup& setup) {
    Identities out; out.rules_version = gc::galactic_rules_version; out.state_encoding_version = 15;
    out.content = setup.content_identity; out.campaign = setup.campaign_identity;
    return out;
}
core::Result<Bytes> save_game_checkpoint(const campaign::GameSession& session, const Identities& build,
    std::span<const std::uint8_t> hints) {
    if (hints.size() > max_bytes) return fail<Bytes>("presentation hints exceed the byte limit", {}, diagnostic_codes::resource_limit);
    auto captured = capture(session, build);
    if (!captured) return core::Result<Bytes>::failure(captured.error());
    if (!hints.empty()) captured.value().sections.push_back({static_cast<std::uint16_t>(SectionId::presentation_hints), 1, false, Bytes(hints.begin(), hints.end())});
    return write_checkpoint(captured.value());
}

namespace {

core::Result<void> validate_checkpoint_sections(const Checkpoint& saved, const Identities& build,
    const GameLoadOptions& options, std::string_view path) {
    if (auto identity = check_identities(saved.header, build, path); !identity) return core::Result<void>::failure(identity.error());
    if (!(saved.header.flags & flag_galaxy)) return fail<void>("not a galaxy checkpoint", path);
    if (saved.header.completed_tick > options.max_restore_ticks || saved.header.completed_tick > gc::galactic_max_ticks)
        return fail<void>("restore tick budget exceeded", path, diagnostic_codes::resource_limit);
    for (const auto id : {SectionId::session, SectionId::seats, SectionId::players, SectionId::planets,
        SectionId::fleets, SectionId::production, SectionId::conflicts, SectionId::victory, SectionId::scripts, SectionId::mod_list,
        SectionId::galactic_abilities, SectionId::campaign_scoring, SectionId::galactic_owner}) {
        const auto* section = saved.find(id);
        if (!section || !section->required) return fail<void>("missing required section " + std::string(section_name(static_cast<std::uint16_t>(id))), path);
        if (section->version != galaxy_section_version)
            return fail<void>("unsupported galaxy section version " + std::to_string(section->version) + " (this build reads "
                + std::to_string(galaxy_section_version) + ")", path, diagnostic_codes::version);
    }
    return core::Result<void>::success();
}

core::Result<void> decode_checkpoint_return_result(detail::Reader& returned, const auto& has_result,
    std::optional<campaign::ConflictRecord>& latest_context,
    std::optional<campaign::BattleResult>& latest_result, std::string_view path) {
    if (has_result == 1) {
        auto decoded_context = decode_conflict(blob(returned), path);
        if (!decoded_context) return core::Result<void>::failure(decoded_context.error());
        auto decoded_result = decode_result(blob(returned), path);
        if (!decoded_result) return core::Result<void>::failure(decoded_result.error());
        latest_context = std::move(decoded_context).value(); latest_result = std::move(decoded_result).value();
        if (latest_context->id != latest_result->conflict) return fail<void>("tactical result context differs", path);
    }
    if (has_result > 1 || returned.failed() || returned.remaining()) return fail<void>("malformed tactical return section", path);
    return core::Result<void>::success();
}

core::Result<void> decode_checkpoint_child(const auto* child_state, const auto* child_section, auto& child_save,
    const Checkpoint& saved, std::optional<ChildLog>& child_log,
    std::optional<campaign::ConflictRecord>& active_conflict, std::string_view path) {
    if (!child_state->required || !child_section->required || child_state->version != child_state_section_version
        || child_section->version != child_log_section_version) return fail<void>("unsupported or optional child section", path);
    auto log = decode_child_log(child_section->bytes, path);
    if (!log) return core::Result<void>::failure(log.error());
    auto conflict = decode_conflict(log.value().conflict_record, path);
    if (!conflict) return core::Result<void>::failure(conflict.error());
    if (log.value().completed_tick != child_save.value()->scripts.completed_tick
        || log.value().completed_tick > saved.header.completed_tick)
        return fail<void>("child log and host clocks differ", path);
    child_save.value()->state = child_state->bytes;
    child_log = std::move(log).value(); active_conflict = std::move(conflict).value();
    return core::Result<void>::success();
}

core::Result<void> decode_checkpoint_fixups(detail::Reader& context, campaign::GameRecording& recording, std::string_view path) {
    const auto vote_timeout = context.u8();
    if (vote_timeout > 1) return fail<void>("invalid multiplayer vote timeout flag", path);
    if (vote_timeout) {
        const auto ticks = context.u64();
        if (ticks >= gc::galactic_max_ticks) return fail<void>("invalid multiplayer vote timeout", path);
        recording.multiplayer_vote_timeout = ticks;
    }
    const auto fixups = context.u32();
    if (fixups > gc::galactic_max_commands || fixups > context.remaining() / 35) return fail<void>("section session: invalid prologue count", path);
    for (std::uint32_t i = 0; i < fixups; ++i) {
        gc::GameCommand command; command.key.tick = context.u64(); command.key.seat = sim::SeatId{context.u32()}; command.key.sequence = context.u64();
        const auto payload = blob(context);
        auto decoded = gc::decode_payload(gc::CommandLayer::session, gc::session_opcodes::post_load_fixup, Bytes(payload.begin(), payload.end()));
        if (!decoded) return fail<void>("section session: malformed load command", path);
        command.payload = *decoded; recording.load_fixups.push_back(std::move(command));
    }
    return core::Result<void>::success();
}

core::Result<void> decode_checkpoint_timelines(detail::Reader& context, campaign::ModeTimelineState& timelines,
    const auto& completed, const Checkpoint& saved, campaign::GameRecording& recording,
    const auto& strategic, const auto& tactical, std::string_view path) {
    const auto galaxy_timeline = context.u32(); const auto historical = context.u8();
    if (galaxy_timeline > 5 || historical > 1) return fail<void>("invalid saved timeline", path);
    timelines.galaxy = static_cast<std::int32_t>(galaxy_timeline);
    if (historical) {
        const auto conflict = context.u64(); const auto tick = context.u64(); const auto value = context.u32();
        if (value > 5) return fail<void>("invalid historical timeline", path);
        timelines.historical_child = campaign::ModeTimelineSnapshot{{campaign::SessionMode::battle, conflict}, tick, static_cast<std::int32_t>(value)};
    }
    if (context.failed() || context.remaining() != 0 || completed != saved.header.completed_tick
        || completed > recording.commands.final_tick_count || strategic == 0 || tactical == 0
        || strategic > static_cast<std::uint64_t>(gc::max_speed_factor_raw) || tactical > static_cast<std::uint64_t>(gc::max_speed_factor_raw))
        return fail<void>("section session: inconsistent clocks or frontier", path, diagnostic_codes::inconsistent);
    return core::Result<void>::success();
}

core::Result<void> validate_checkpoint_recording(const Checkpoint& saved, campaign::GameRecording& recording, auto& active_conflict,
    const auto& completed, auto& child_log, const auto& replay_bytes,
    const Identities& build, campaign::HistoryBoundary& boundary, std::string_view path) {
    if (recording.base.at.tick > completed)
        return fail<void>("section session: history base is after the saved tick", path, diagnostic_codes::inconsistent);
    detail::Reader conflicts(saved.find(SectionId::conflicts)->bytes);
    const auto count = conflicts.u32();
    if (count > gc::galactic_max_objects || count > conflicts.remaining() / 16) return fail<void>("section conflicts: invalid count", path);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto tick = conflicts.u64(); auto conflict = decode_conflict(blob(conflicts), path);
        if (!conflict) return core::Result<void>::failure(conflict.error());
        recording.conflicts.push_back({tick, std::move(conflict).value()});
    }
    if (!conflicts.failed() && conflicts.remaining()) {
        const auto admissions = conflicts.u32();
        recording.conflict_services = true;
        if (admissions > gc::galactic_max_objects || admissions > conflicts.remaining() / 19)
            return fail<void>("section conflicts: invalid admission count", path);
        for (std::uint32_t i = 0; i < admissions; ++i) {
            campaign::RecordedAdmission entry; entry.entry_tick = conflicts.u64();
            auto conflict = decode_conflict(blob(conflicts), path);
            if (!conflict) return core::Result<void>::failure(conflict.error());
            entry.conflict = std::move(conflict).value();
            const auto route = conflicts.u8(), forced = conflicts.u8(), overrun = conflicts.u8();
            if (route > static_cast<std::uint8_t>(campaign::ConflictRoute::tactical) || forced > 1 || overrun > 1)
                return fail<void>("section conflicts: invalid admission policy", path);
            entry.arbitration = {static_cast<campaign::ConflictRoute>(route), forced != 0, overrun != 0};
            recording.admissions.push_back(std::move(entry));
        }
    }
    if (conflicts.failed() || conflicts.remaining() != 0) return fail<void>("section conflicts: malformed", path);
    if (active_conflict && std::none_of(recording.conflicts.begin(), recording.conflicts.end(), [&](const auto& entry) {
        // WGS-P09: roles/departures evolve only the effective mapping; history keeps
        // the original entry for replay. Readiness and save-only barriers advance only the parent clock.
        // Final recapture below still compares every effective seat and exact saved section.
        auto initial = *active_conflict;
        const bool roles = recording.commands.setup.seat_control.has_value();
        if (roles) initial.seats = entry.conflict.seats;
        if (entry.conflict != initial || entry.entry_tick >= completed) return false;
        const auto elapsed = completed - entry.entry_tick - 1;
        // Agreed multiplayer pauses advance the parent frontier while holding the child clock.
        // Exact saved sections are still checked after direct owner restoration below.
        const auto& setup = recording.commands.setup;
        // Match GalacticSession::multiplayer() before constructing the restored world.
        const bool multiplayer = setup.victory ? setup.victory->context.multiplayer
            : setup.economy_corruption && setup.economy_corruption->multiplayer;
        if (roles || multiplayer) return elapsed >= child_log->completed_tick;
        // Save barriers before the history base left with their history (#4058): only the upper bound is checkable.
        if (entry.entry_tick < recording.base.at.tick) return elapsed >= child_log->completed_tick;
        std::uint64_t save_barriers{};
        const auto& commands = recording.commands.commands;
        for (auto first = commands.begin(); first != commands.end();) {
            const auto tick = first->key.tick;
            auto last = std::find_if(first, commands.end(), [tick](const auto& command) { return command.key.tick != tick; });
            if (tick > entry.entry_tick && tick < completed && std::all_of(first, last, [](const auto& command) {
                return std::holds_alternative<gc::SaveRequest>(command.payload);
            })) ++save_barriers;
            first = last;
        }
        return elapsed >= save_barriers && elapsed - save_barriers == child_log->completed_tick;
    })) return fail<void>("active child disagrees with conflict history", path);
    // Validate canonical replay and header references before expensive reconstruction.
    auto canonical = gc::write_replay(recording.commands);
    if (!canonical || !std::equal(replay_bytes.begin(), replay_bytes.end(), canonical.value().begin(), canonical.value().end())
        || recording.commands.setup.seed != saved.header.seed || recording.commands.setup.content_identity != build.content
        || recording.commands.setup.campaign_identity != build.campaign)
        return fail<void>("section session: inconsistent replay identity", path, diagnostic_codes::inconsistent);
    auto sealed = campaign::history_frontier_of(recording, replay_bytes, completed);
    if (!sealed) return core::Result<void>::failure(sealed.error());
    InputFrontier frontier{};
    if (const auto& key = sealed.value().last_key) frontier = {key->tick, key->seat.value, key->sequence, sealed.value().digest};
    if (frontier != saved.header.frontier || ((saved.header.flags & flag_frontier) != 0) != sealed.value().last_key.has_value())
        return fail<void>("section session: inconsistent sealed input frontier", path, diagnostic_codes::inconsistent);
    boundary = std::move(sealed).value();
    return core::Result<void>::success();
}

core::Result<void> prepare_checkpoint_runtime(const GameLoadOptions& options, auto& prepared_runtime, std::string_view path) {
    auto prepared = options.prepare_galactic();
    if (!prepared) return core::Result<void>::failure(prepared.error());
    if (!prepared.value().configure_native_ai)
        return fail<void>("prepared galactic runtime has no native installer", path);
    if (static_cast<bool>(prepared.value().child_scripts) != static_cast<bool>(prepared.value().configure_child))
        return fail<void>("prepared galactic runtime has an incomplete child script pair", path);
    prepared_runtime.emplace(std::move(prepared).value());
    return core::Result<void>::success();
}

core::Result<void> bind_checkpoint_services(campaign::GameSession& session, const GameLoadOptions& options,
    auto& prepared_runtime, const bool direct) {
    if (!direct) session.begin_native_reconstruction();
    session.set_result_applier(options.apply_result); session.set_child_configurator(options.configure_child);
    session.set_auto_resolve_outcome_services(options.auto_resolve_outcomes);
    session.set_auto_resolver(options.auto_resolve);
    // A recording may already have installed the canonical MP policy. An absent
    // legacy callback must not erase it before the saved vote owner is imported.
    if (options.vote_progress) session.set_vote_progress(options.vote_progress);
    session.set_conflict_gate(options.conflict_gate);
    if (options.conflict_service) session.set_conflict_service(options.conflict_service, options.content, options.scripts, options.script_requirement);
    if (options.configure_session) {
        auto configured = options.configure_session(session);
        if (!configured) return core::Result<void>::failure(configured.error());
    }
    if (prepared_runtime && prepared_runtime->child_parent_queries) {
        auto bound = session.add_conflict_parent_queries(campaign::BattleLayer::space, prepared_runtime->child_parent_queries);
        if (!bound) return core::Result<void>::failure(bound.error());
    }
    if (prepared_runtime && prepared_runtime->child_scripts) {
        auto configured = session.set_child_script_services(campaign::BattleLayer::space, prepared_runtime->child_scripts, prepared_runtime->configure_child);
        if (!configured) return core::Result<void>::failure(configured.error());
    }
    return core::Result<void>::success();
}

core::Result<void> restore_checkpoint_frontier(campaign::GameSession& session, const Checkpoint& saved, const bool direct,
    const auto& completed, auto& active_conflict, auto& child_save, auto& child_log,
    const sim::PartitionExecutor& executor, std::uint64_t& resimulated,
    bool& child_restored, std::string_view path) {
    if (direct) {
        auto restored = session.load_checkpoint_state(saved.find(SectionId::galactic_owner)->bytes);
        if (!restored) return fail<void>("direct owner restoration failed: " + restored.error().message, path, diagnostic_codes::restore_failed);
        if (session.completed_tick() != completed) return fail<void>("direct owner clock differs", path);
    } else {
    while (session.completed_tick() < completed) {
        if (active_conflict && session.conflict() && session.conflict()->id == active_conflict->id) {
            auto restored = session.restore_battle_checkpoint(*child_save.value(), child_log->log, completed, executor);
            if (!restored) return fail<void>("child restoration failed: " + restored.error().message, path, diagnostic_codes::restore_failed);
            child_restored = true;
            break;
        }
        auto stepped = session.step(executor);
        if (!stepped) return fail<void>("recording reconstruction failed: " + stepped.error().message, path, diagnostic_codes::restore_failed);
        ++resimulated;
    }
    // A save at the activation barrier has a zero-tick child and still needs its host/state.
    if (active_conflict && child_log->completed_tick == 0 && !child_restored) {
        auto restored = session.restore_battle_checkpoint(*child_save.value(), child_log->log, completed, executor);
        if (!restored) return fail<void>("child restoration failed: " + restored.error().message, path, diagnostic_codes::restore_failed);
    }
    if (auto finished = session.finish_native_reconstruction(executor); !finished)
        return fail<void>("native effect reconstruction failed: " + finished.error().message, path, diagnostic_codes::restore_failed);
    // A player-load prologue at the saved barrier has already run and must not run twice.
    if (auto prologue = session.apply_restore_prologue(); !prologue) return core::Result<void>::failure(prologue.error());
    }
    return core::Result<void>::success();
}

core::Result<void> restore_checkpoint_scripts(campaign::GameSession& session, auto& script_save, auto& prepared_runtime,
    const GameLoadOptions& options, auto& prepared_native) {
    if (script_save.value()) {
        auto schedulers = [&]() -> core::Result<GameLoadOptions::GalacticSchedulers> {
            if (!prepared_runtime) return options.galactic_scripts();
            prepared_native = std::move(prepared_runtime->configure_native_ai);
            return core::Result<GameLoadOptions::GalacticSchedulers>::success(std::move(prepared_runtime->schedulers));
        }();
        if (!schedulers) return core::Result<void>::failure(schedulers.error());
        auto installed = session.install_galactic_scripts(std::move(schedulers.value().first), std::move(schedulers.value().second), script_save.value());
        if (!installed) return core::Result<void>::failure(installed.error());
        if (options.configure_galactic) {
            auto configured = options.configure_galactic(*session.galactic_scripts());
            if (!configured) return core::Result<void>::failure(configured.error());
        }
    }
    return core::Result<void>::success();
}

core::Result<void> restore_checkpoint_publication(campaign::GameSession& session, const auto& return_state_bytes,
    auto& latest_context, auto& latest_result, const Checkpoint& saved,
    const bool direct, campaign::ModeTimelineState& timelines, std::string_view path) {
    if (auto restored = session.restore_return_state(return_state_bytes); !restored)
        return core::Result<void>::failure(restored.error());
    if (latest_result) {
        auto restored = session.restore_latest_tactical_result(std::move(*latest_context), std::move(*latest_result));
        if (!restored) return core::Result<void>::failure(restored.error());
    }
    else session.clear_latest_tactical_result();
    // WGA-256: access is restored before any derived AI route/connectivity owner.
    detail::Reader access_reader(saved.find(SectionId::planets)->bytes);
    (void)blob(access_reader); (void)blob(access_reader);
    if (session.galaxy().graph()) (void)blob(access_reader);
    if (session.galaxy().setup().control) (void)blob(access_reader);
    if (session.galaxy().setup().observer) (void)blob(access_reader);
    if (session.galaxy().setup().economy_corruption) (void)blob(access_reader);
    (void)blob(access_reader); // native invasion queue precedes the access component
    const auto access_bytes = blob(access_reader);
    if (access_reader.failed() || access_reader.remaining())
        return fail<void>("malformed planet access boundary", path);
    if (!direct && session.galaxy().snapshot()->planet_access()) {
        auto restored = session.restore_planet_access(access_bytes);
        if (!restored) return core::Result<void>::failure(restored.error());
    } else if (!session.galaxy().snapshot()->planet_access() && !access_bytes.empty()) return fail<void>("unexpected planet access owner", path);
    if (!direct) if (auto restored = session.restore_timeline(timelines); !restored)
        return core::Result<void>::failure(restored.error());
    // WGA-56/139: fixup only after world/child/Lua/return identities exist. No
    // native engine is initialized or serviced while reconstructing the log.
    if (!direct && session.galaxy().setup().census) {
        detail::Reader production(saved.find(SectionId::production)->bytes);
        (void)blob(production);
        const auto census = blob(production);
        if (production.failed()) return fail<void>("malformed saved actor census", path);
        if (auto restored = session.restore_actor_representations(census); !restored)
            return core::Result<void>::failure(restored.error());
    }
    return core::Result<void>::success();
}

core::Result<void> validate_checkpoint_reconstruction(campaign::GameSession& session, const auto& expected_hash, const auto& strategic,
    const auto& tactical, const Checkpoint& saved, const Identities& build,
    const bool direct, std::string_view path) {
    Digest actual{}; (void)detail::from_hex(session.state_sha256(), actual);
    if (actual != expected_hash || session.galaxy().clock().strategic_speed.raw() != static_cast<std::int64_t>(strategic)
        || session.galaxy().clock().tactical_speed.raw() != static_cast<std::int64_t>(tactical)) {
        // Name the sections whose canonical bytes differ so a hash mismatch is actionable.
        std::string detail_text;
        if (auto probe = capture(session, build)) {
            for (const auto& section : saved.sections) {
                const auto* other = probe.value().find(static_cast<SectionId>(section.id));
                if (!other || *other == section) continue;
                std::size_t at = 0;
                while (at < other->bytes.size() && at < section.bytes.size() && other->bytes[at] == section.bytes[at]) ++at;
                detail_text += " " + std::string(section_name(section.id)) + "@" + std::to_string(at) + "("
                    + std::to_string(section.bytes.size()) + "/" + std::to_string(other->bytes.size()) + ")";
            }
        }
        return fail<void>("reconstructed game state differs" + (detail_text.empty() ? std::string{} : ";" + detail_text),
            path, diagnostic_codes::restore_failed);
    }
    auto recaptured = capture(session, build);
    if (!recaptured) return core::Result<void>::failure(recaptured.error());
    if (recaptured.value().header != saved.header) return fail<void>("reconstructed header differs", path, diagnostic_codes::inconsistent);
    for (const auto& section : saved.sections) {
        if (section.id >= 0x8000 || !known_section(section.id)) continue;
        if (!direct && section.id == static_cast<std::uint16_t>(SectionId::galactic_owner)) continue;
        const auto* expected = recaptured.value().find(static_cast<SectionId>(section.id));
        if (!expected || *expected != section) {
            std::size_t at{};
            if (expected) while (at < expected->bytes.size() && at < section.bytes.size() && expected->bytes[at] == section.bytes[at]) ++at;
            return fail<void>("reconstructed section differs: " + std::string(section_name(section.id))
                + "@" + std::to_string(at), path, diagnostic_codes::inconsistent);
        }
    }
    for (const auto& section : recaptured.value().sections)
        if (!saved.find(static_cast<SectionId>(section.id))) return fail<void>("missing child section", path);
    return core::Result<void>::success();
}

core::Result<void> apply_checkpoint_player_load(GameRestore& result, campaign::GameRecording& recording, const auto& completed,
    const GameLoadOptions& options, std::string_view path) {
    const auto& setup = result.session.galaxy().setup();
    const auto live_seats = result.session.galaxy().seats();
    const auto human = std::find_if(live_seats.begin(), live_seats.end(), [](const auto& row) { return row.kind == gc::SeatKind::human && row.active; });
    if (human == live_seats.end()) return fail<void>("player load needs an active human seat", path);
    const auto humans = std::count_if(setup.seats.begin(), setup.seats.end(), [](const auto& row) { return row.kind == gc::SeatKind::human; });
    // WGS-43 differs deliberately from runtime/resync pause preservation.
    const auto depth = result.session.galaxy().clock().pause_depth;
    const auto normalized = depth != 0 && (result.session.mode() == campaign::SessionMode::battle
        || result.session.pending_conflict()) ? 1U : 0U;
    const bool pending = result.session.conflict() || result.session.pending_conflict() || std::any_of(recording.conflicts.begin(), recording.conflicts.end(),
        [&](const auto& entry) { return entry.entry_tick >= completed; });
    gc::GameCommand fixup{{completed, human->seat_id, 0}, gc::PostLoadFixup{normalized, humans == 1 && normalized == 0, pending, options.reserved_autosave}};
    if (auto applied = result.session.apply_load_fixup(fixup); !applied) return core::Result<void>::failure(applied.error());
    result.first_commands.push_back(fixup);
    return core::Result<void>::success();
}

} // namespace

namespace {

// Test oracle for a save whose history starts at an earlier save: import that save's world/game owner (no scripts, AI
// or child; a window is replayed the way a new game's history is) and check it is the boundary the save names.
core::Result<campaign::GameSession> import_oracle_base(std::span<const std::uint8_t> base_bytes, const Identities& build,
    const GameLoadOptions& options, const campaign::GameSession::ScriptFactory& child_scripts, auto& prepared_runtime,
    const campaign::HistoryBoundary& expected, std::string_view path) {
    using Session = campaign::GameSession;
    auto parsed = parse_checkpoint(base_bytes, path);
    if (!parsed) return core::Result<Session>::failure(parsed.error());
    const auto& saved = parsed.value();
    if (auto phase = validate_checkpoint_sections(saved, build, options, path); !phase) return core::Result<Session>::failure(phase.error());
    if ((saved.header.flags & flag_child) != 0) return fail<Session>("the replay oracle cannot start from a save with a battle in progress", path);
    detail::Reader context(saved.find(SectionId::session)->bytes);
    const auto prefix = detail::read_session_prefix(context);
    auto replay = gc::parse_replay(prefix.replay, path);
    if (!replay || context.failed()) return fail<Session>("base save: invalid recording", path);
    campaign::GameRecording recording{std::move(replay).value(), {}, {}};
    recording.base = prefix.base;
    if (auto phase = decode_checkpoint_fixups(context, recording, path); !phase) return core::Result<Session>::failure(phase.error());
    std::optional<ChildLog> no_child;
    std::optional<campaign::ConflictRecord> no_conflict;
    campaign::HistoryBoundary boundary;
    if (auto phase = validate_checkpoint_recording(saved, recording, no_conflict, prefix.completed, no_child, prefix.replay, build, boundary, path); !phase)
        return core::Result<Session>::failure(phase.error());
    if (boundary.tick != expected.tick || boundary.digest != expected.digest || boundary.last_key != expected.last_key)
        return fail<Session>("section session: history base differs from the earlier save it names", path, diagnostic_codes::inconsistent);
    auto created = Session::from_recording(recording, options.content, child_scripts, options.script_requirement);
    if (!created) return created;
    auto session = std::move(created).value();
    if (auto phase = bind_checkpoint_services(session, options, prepared_runtime, true); !phase) return core::Result<Session>::failure(phase.error());
    auto restored = session.load_checkpoint_state(saved.find(SectionId::galactic_owner)->bytes);
    if (!restored) return fail<Session>("base owner restoration failed: " + restored.error().message, path, diagnostic_codes::restore_failed);
    if (session.completed_tick() != prefix.completed) return fail<Session>("base owner clock differs", path);
    session.schedule_history_rebase(std::move(boundary));
    return core::Result<Session>::success(std::move(session));
}

} // namespace

static core::Result<GameRestore> load_game_checkpoint_impl(std::span<const std::uint8_t> bytes, const Identities& build,
    const GameLoadOptions& options, const sim::PartitionExecutor& executor, std::string_view path, bool direct,
    std::span<const std::uint8_t> oracle_base = {}) {
    auto parsed = parse_checkpoint(bytes, path);
    if (!parsed) return core::Result<GameRestore>::failure(parsed.error());
    const auto& saved = parsed.value();
    if (auto phase = validate_checkpoint_sections(saved, build, options, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    const auto& scripts_bytes = saved.find(SectionId::scripts)->bytes;
    const auto* ai_section = saved.find(SectionId::galactic_ai);
    if (!ai_section || !ai_section->required || ai_section->version != 1)
        return fail<GameRestore>("missing or unsupported native AI section", path);
    auto ai_save = campaign::decode_galactic_ai_save(ai_section->bytes);
    if (!ai_save) return core::Result<GameRestore>::failure(ai_save.error());
    if (ai_save.value().completed_tick != saved.header.completed_tick || ai_save.value().galactic_frame != saved.header.galactic_frame)
        return fail<GameRestore>("native AI boundary differs from the whole-game header", path, diagnostic_codes::inconsistent);
    if (ai_save.value().state && !options.configure_native_ai && !options.prepare_galactic)
        return fail<GameRestore>("native AI section needs a fresh state contributor", path);
    const auto* return_section = saved.find(SectionId::tactical_return);
    if (!return_section || !return_section->required || return_section->version != 1)
        return fail<GameRestore>("missing or unsupported tactical return section", path);
    detail::Reader returned(return_section->bytes);
    const auto return_state_bytes = blob(returned);
    const auto has_result = returned.u8();
    std::optional<campaign::ConflictRecord> latest_context;
    std::optional<campaign::BattleResult> latest_result;
    if (auto phase = decode_checkpoint_return_result(returned, has_result, latest_context, latest_result, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    std::size_t script_length{};
    auto script_save = decode_scripts(scripts_bytes, path, script_length);
    if (!script_save) return core::Result<GameRestore>::failure(script_save.error());
    auto child_save = decode_child_host(std::span(scripts_bytes).subspan(script_length), path);
    if (!child_save) return core::Result<GameRestore>::failure(child_save.error());
    const auto* child_state = saved.find(SectionId::child_state);
    const auto* child_section = saved.find(SectionId::child_log);
    const bool has_child = (saved.header.flags & flag_child) != 0;
    if (has_child != child_save.value().has_value() || has_child != (child_state != nullptr) || has_child != (child_section != nullptr))
        return fail<GameRestore>("child flag, host, log and full state must participate together", path);
    std::optional<ChildLog> child_log;
    std::optional<campaign::ConflictRecord> active_conflict;
    if (has_child) {
        if (auto phase = decode_checkpoint_child(child_state, child_section, child_save, saved, child_log, active_conflict, path); !phase)
            return core::Result<GameRestore>::failure(phase.error());
    }
    if (script_save.value() && !options.galactic_scripts && !options.prepare_galactic) return fail<GameRestore>("section scripts needs pinned scheduler resolver", path);
    detail::Reader context(saved.find(SectionId::session)->bytes);
    const auto prefix = detail::read_session_prefix(context);
    const auto completed = prefix.completed; const auto expected_hash = prefix.expected_hash;
    const auto strategic = prefix.strategic; const auto tactical = prefix.tactical;
    const auto replay_bytes = prefix.replay;
    auto replay = gc::parse_replay(replay_bytes, path);
    if (!replay || context.failed()) return fail<GameRestore>("section session: invalid recording", path);
    campaign::GameRecording recording{std::move(replay).value(), {}, {}};
    recording.base = prefix.base;
    if (auto phase = decode_checkpoint_fixups(context, recording, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    campaign::ModeTimelineState timelines;
    if (auto phase = decode_checkpoint_timelines(context, timelines, completed, saved, recording, strategic, tactical, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    campaign::HistoryBoundary boundary;
    if (auto phase = validate_checkpoint_recording(saved, recording, active_conflict, completed, child_log, replay_bytes, build, boundary, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    const bool windowed_oracle = !direct && recording.base.at.tick != 0;
    if (windowed_oracle && (oracle_base.empty() || has_child))
        return fail<GameRestore>(has_child ? "the replay oracle cannot replay a window with a battle in progress"
            : "section session: history starts at an earlier save that the replay oracle needs", path, diagnostic_codes::inconsistent);
    // Prepare parent owners without servicing them. Their child bindings must
    // already exist when reconstructing/restoring a parent-aware Lua graph.
    std::optional<GameLoadOptions::PreparedGalacticScripts> prepared_runtime;
    if (script_save.value() && options.prepare_galactic) {
        if (auto phase = prepare_checkpoint_runtime(options, prepared_runtime, path); !phase)
            return core::Result<GameRestore>::failure(phase.error());
    }
    const auto& child_scripts = prepared_runtime && prepared_runtime->child_scripts ? prepared_runtime->child_scripts : options.scripts;
    auto created = windowed_oracle
        ? import_oracle_base(oracle_base, build, options, child_scripts, prepared_runtime, recording.base.at, path)
        : campaign::GameSession::from_recording(recording, options.content, child_scripts, options.script_requirement);
    if (!created) return core::Result<GameRestore>::failure(created.error());
    auto session = std::move(created).value();
    if (windowed_oracle) {
        // The base is imported; the window now replays on top of it exactly as a new game's history does.
        if (auto appended = session.append_recording_window(recording); !appended) return core::Result<GameRestore>::failure(appended.error());
        session.begin_native_reconstruction();
    } else if (auto phase = bind_checkpoint_services(session, options, prepared_runtime, direct); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    std::uint64_t resimulated{};
    bool child_restored{};
    if (auto phase = restore_checkpoint_frontier(session, saved, direct, completed, active_conflict, child_save, child_log, executor, resimulated, child_restored, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    // WGS-57/173: story rewards mutate the active monitor synchronously rather
    // than issuing ordinary replay commands. Restore its sealed owner before
    // script-host world-hash checks, without replaying registration notices.
    if (!direct && session.galaxy().setup().victory) {
        auto monitor = gc::detail::decode_victory(saved.find(SectionId::victory)->bytes);
        if (!monitor) return fail<GameRestore>("malformed saved victory monitor", path);
        if (auto restored = session.restore_victory_state(*monitor); !restored)
            return core::Result<GameRestore>::failure(restored.error());
    }
    std::function<core::Result<void>(campaign::GameSession&)> prepared_native;
    if (auto phase = restore_checkpoint_scripts(session, script_save, prepared_runtime, options, prepared_native); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    if (auto phase = restore_checkpoint_publication(session, return_state_bytes, latest_context, latest_result, saved, direct, timelines, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    if (prepared_native || options.configure_native_ai) {
        auto configured = prepared_native ? prepared_native(session) : options.configure_native_ai(session);
        if (!configured) return core::Result<GameRestore>::failure(configured.error());
    }
    if (auto restored = session.restore_galactic_ai(ai_save.value()); !restored)
        return core::Result<GameRestore>::failure(restored.error());
    if (auto phase = validate_checkpoint_reconstruction(session, expected_hash, strategic, tactical, saved, build, direct, path); !phase)
        return core::Result<GameRestore>::failure(phase.error());
    // #4058: the loaded save is the new history boundary; the first step drops the window it was restored with.
    session.schedule_history_rebase(std::move(boundary));
    session.clear_auto_resolve_history(); // WAR-63: result reconstruction does not persist the runtime ring.
    GameRestore result{std::move(session), saved.header, {resimulated, bytes.size()}, {}, {}};
    if (const auto* hints = saved.find(SectionId::presentation_hints)) result.presentation_hints = hints->bytes;
    if (options.player_load) {
        if (auto phase = apply_checkpoint_player_load(result, recording, completed, options, path); !phase)
            return core::Result<GameRestore>::failure(phase.error());
    }
    return core::Result<GameRestore>::success(std::move(result));
}
core::Result<GameRestore> load_game_checkpoint(std::span<const std::uint8_t> bytes, const Identities& build,
    const GameLoadOptions& options, const sim::PartitionExecutor& executor, std::string_view path) {
    return load_game_checkpoint_impl(bytes, build, options, executor, path, true);
}
core::Result<GameRestore> replay_game_checkpoint_for_testing(std::span<const std::uint8_t> bytes,
    const Identities& build, const GameLoadOptions& options, const sim::PartitionExecutor& executor,
    std::span<const std::uint8_t> base) {
    return load_game_checkpoint_impl(bytes, build, options, executor, {}, false, base);
}
core::Result<void> restore_game_checkpoint(GameRestore& running, std::span<const std::uint8_t> bytes,
    const Identities& build, const GameLoadOptions& options, const sim::PartitionExecutor& executor, std::string_view path) {
    auto loaded = load_game_checkpoint(bytes, build, options, executor, path);
    if (!loaded) return core::Result<void>::failure(loaded.error());
    running = std::move(loaded).value(); return core::Result<void>::success();
}
std::int64_t local_save_slot(const gc::GalacticEvent& event, const gc::SeatId local_seat) noexcept {
    return event.kind == gc::GalacticEventKind::save_requested && event.producer == local_seat
        ? std::bit_cast<std::int64_t>(event.value) : -1;
}
} // namespace eawr::checkpoint
