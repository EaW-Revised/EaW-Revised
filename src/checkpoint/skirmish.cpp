#include "eawr/checkpoint/skirmish.hpp"

#include "encoding_internal.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace eawr::checkpoint {

namespace auth = script::authoritative;

namespace {

using Bytes = std::vector<std::uint8_t>;

template <class T>
[[nodiscard]] core::Result<T> fail(std::string message, std::string_view path = {},
    std::string_view code = diagnostic_codes::section_invalid) {
    return core::Result<T>::failure(detail::diagnostic(code, std::move(message), path));
}

// The value framing of the galaxy's scripts section (a copy, so the galaxy codec stays its owner's).
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
            out.u64(item.size());
            for (const auto& child : item) script_value(out, child, depth + 1);
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
        const auto count = in.u64();
        if (count > in.remaining()) return reject();
        auth::ValueList list;
        for (std::uint64_t i = 0; i < count && !in.failed(); ++i) list.push_back(script_value(in, depth + 1));
        return auth::Value{std::move(list)};
    }
    default: return reject();
    }
}

inline constexpr std::uint32_t host_tag = 1;

} // namespace

core::Result<Bytes> encode_tactical_host(const auth::TacticalScriptSave& saved) {
    detail::Writer out;
    out.u32(host_tag);
    out.u64(saved.completed_tick);
    text(out, saved.scheduler);
    out.u32(saved.native ? 1 : 0);
    if (saved.native) blob(out, *saved.native);
    out.u64(saved.pending.size());
    try {
        for (const auto& command : saved.pending) {
            out.u64(command.tick); out.u64(command.issuer); out.u64(command.sequence); text(out, command.verb);
            script_value(out, auth::Value{command.arguments});
            if (out.bytes().size() > max_bytes)
                return fail<Bytes>("section tactical-host exceeds the checkpoint limit", {}, diagnostic_codes::resource_limit);
        }
    } catch (const std::length_error& error) { return fail<Bytes>(std::string("section tactical-host: ") + error.what()); }
    out.u64(saved.last_keys.size());
    for (const auto& [player, key] : saved.last_keys) { out.u32(player); out.u64(key.first); out.u64(key.second); }
    if (out.bytes().size() > max_bytes)
        return fail<Bytes>("section tactical-host exceeds the checkpoint limit", {}, diagnostic_codes::resource_limit);
    return core::Result<Bytes>::success(std::move(out.bytes()));
}

core::Result<auth::TacticalScriptSave> decode_tactical_host(const std::span<const std::uint8_t> bytes, const std::string_view path) {
    using Save = auth::TacticalScriptSave;
    detail::Reader in(bytes);
    Save saved;
    if (in.u32() != host_tag) return fail<Save>("section tactical-host: invalid host tag", path);
    saved.completed_tick = in.u64();
    saved.scheduler = text(in);
    const auto native = in.u32();
    if (native > 1) return fail<Save>("section tactical-host: invalid native presence", path);
    if (native) {
        const auto payload = blob(in);
        saved.native.emplace(payload.begin(), payload.end());
    }
    // Every count is bounded by the bytes left: a pending command takes at least 33 bytes, a cursor 20.
    const auto count = [&](std::size_t width) {
        const auto value = in.u64();
        if (value > in.remaining() / width) { (void)in.take(in.remaining() + 1); return std::uint64_t{}; }
        return value;
    };
    const auto pending = count(33);
    for (std::uint64_t i = 0; i < pending && !in.failed(); ++i) {
        auth::ScriptCommand command;
        command.tick = in.u64(); command.issuer = in.u64(); command.sequence = in.u64(); command.verb = text(in);
        auto args = script_value(in);
        const auto* list = std::get_if<auth::ValueList>(&args.data);
        if (!list) return fail<Save>("section tactical-host: command arguments must be a list", path);
        command.arguments = *list;
        saved.pending.push_back(std::move(command));
    }
    const auto keys = count(20);
    for (std::uint64_t i = 0; i < keys && !in.failed(); ++i) {
        const auto player = in.u32(); const auto tick = in.u64(); const auto sequence = in.u64();
        if (!saved.last_keys.emplace(player, std::pair(tick, sequence)).second)
            return fail<Save>("section tactical-host: duplicate routing cursor", path);
    }
    if (in.failed() || in.remaining() != 0) return fail<Save>("section tactical-host: malformed lengths", path);
    auto canonical = encode_tactical_host(saved);
    if (!canonical || canonical.value().size() != bytes.size()
        || !std::equal(bytes.begin(), bytes.end(), canonical.value().begin()))
        return fail<Save>("section tactical-host: noncanonical encoding", path);
    return core::Result<Save>::success(std::move(saved));
}

Digest script_closure_digest(const std::string_view closure_sha256_hex) {
    Digest digest{};
    if (!detail::from_hex(closure_sha256_hex, digest)) return Digest{};
    return digest;
}

Identities skirmish_identities(const Digest& content, const Digest& scripts) {
    auto result = tactical_identities(content);
    result.lua_save = scripts;
    return result;
}

core::Result<Bytes> save_skirmish_checkpoint(auth::ScriptedTacticalSession& session, const std::span<const Seat> seats,
    const Identities& build) {
    const auto& world = session.world();
    const auto queued = world.pending_commands();
    auto host = session.save_scripts();
    if (!host) return fail<Bytes>("section tactical-host: " + host.error().message, {}, diagnostic_codes::invalid_save);
    if (host.value().completed_tick != world.completed_tick())
        return fail<Bytes>("section tactical-host: the script host and the battle are at different ticks", {},
            diagnostic_codes::invalid_save);
    auto host_bytes = encode_tactical_host(host.value());
    if (!host_bytes) return core::Result<Bytes>::failure(host_bytes.error());
    auto battle = save_tactical_checkpoint(world, queued, seats, build);
    if (!battle) return battle;
    // The tactical layer writes the battle's own sections; the host section is added beside them.
    auto checkpoint = parse_checkpoint(battle.value());
    if (!checkpoint) return core::Result<Bytes>::failure(checkpoint.error());
    auto& sections = checkpoint.value().sections;
    const auto id = static_cast<std::uint16_t>(SectionId::tactical_host);
    const auto at = std::find_if(sections.begin(), sections.end(), [&](const Section& section) { return section.id > id; });
    sections.insert(at, Section{id, tactical_host_section_version, true, std::move(host_bytes).value()});
    return write_checkpoint(checkpoint.value());
}

core::Result<SkirmishRestore> load_skirmish_checkpoint(const std::span<const std::uint8_t> bytes, const Identities& build,
    const TacticalSessionFactory& world_factory, const SkirmishHostFactory& host_factory,
    const sim::PartitionExecutor& executor, const std::string_view path) {
    using Restore = core::Result<SkirmishRestore>;
    if (!host_factory) return fail<SkirmishRestore>("section tactical-host: no script host factory", path,
        diagnostic_codes::restore_failed);
    // The container and the identities first, so a save from another build names that before any section.
    auto parsed = parse_checkpoint(bytes, path);
    if (!parsed) return Restore::failure(parsed.error());
    if (auto identities = check_identities(parsed.value().header, build, path); !identities)
        return Restore::failure(identities.error());
    const auto* host_section = parsed.value().find(SectionId::tactical_host);
    if (!host_section) return fail<SkirmishRestore>("a skirmish save needs the tactical-host section", path,
        diagnostic_codes::inconsistent);
    if (host_section->version != tactical_host_section_version)
        return fail<SkirmishRestore>("section tactical-host version " + std::to_string(host_section->version)
            + " is not this build's version " + std::to_string(tactical_host_section_version), path, diagnostic_codes::version);
    auto host = decode_tactical_host(host_section->bytes, path);
    if (!host) return Restore::failure(host.error());
    auto battle = load_tactical_checkpoint(bytes, build, world_factory, executor, path);
    if (!battle) return Restore::failure(battle.error());
    if (host.value().completed_tick != battle.value().session.completed_tick())
        return fail<SkirmishRestore>("section tactical-host: the script host and the battle are at different ticks", path,
            diagnostic_codes::inconsistent);
    const auto log = battle.value().session.record();
    auto prepared = host_factory(log);
    if (!prepared) return fail<SkirmishRestore>("section tactical-host: " + prepared.error().message, path,
        diagnostic_codes::restore_failed);
    // The campaign child's order: restore the scheduler and host beside the world, then bind the engine.
    auto restored = auth::ScriptedTacticalSession::restore(std::move(battle.value().session),
        std::move(prepared.value().scripts), host.value());
    if (!restored) return fail<SkirmishRestore>("section tactical-host: " + restored.error().message, path,
        diagnostic_codes::restore_failed);
    if (prepared.value().bind) {
        if (auto bound = prepared.value().bind(restored.value()); !bound)
            return fail<SkirmishRestore>("section tactical-host: " + bound.error().message, path, diagnostic_codes::restore_failed);
    }
    // The restored host must save back to exactly the saved bytes (scheduler, queue, cursors, native AI).
    auto again = restored.value().save_scripts();
    if (!again) return fail<SkirmishRestore>("section tactical-host: " + again.error().message, path,
        diagnostic_codes::restore_failed);
    auto encoded = encode_tactical_host(again.value());
    if (!encoded || encoded.value() != host_section->bytes)
        return fail<SkirmishRestore>("section tactical-host: the restored host does not save back to the saved bytes", path,
            diagnostic_codes::restore_failed);
    return Restore::success(SkirmishRestore{std::move(restored).value(), std::move(battle.value().seats),
        battle.value().header, battle.value().work});
}

} // namespace eawr::checkpoint
