#include "eawr/net/lobby_session.hpp"
#include "eawr/net/galactic_setup.hpp"
#include "eawr/net/lan_directory.hpp"
#include "eawr/net/mesh_policy.hpp"
#include "eawr/net/reconnect.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::size_t name_limit = 64, chat_limit = 512, choice_limit = 256;
constexpr std::size_t options_limit = 64U * 1024U;
constexpr std::size_t mod_description_limit = max_lobby_mod_description_bytes;
constexpr std::size_t content_layer_limit = 256;

template<class T> core::Result<T> failure(LobbyReason reason, std::string message) {
    return core::Result<T>::failure(lobby_diagnostic(reason, std::move(message)));
}
bool valid_room(std::string_view room) {
    return !room.empty() && room.size() <= 64 && std::all_of(room.begin(), room.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
struct Writer {
    Bytes bytes;
    void u8(std::uint8_t value) { bytes.push_back(value); }
    void u32(std::uint32_t value) { for (unsigned i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(value >> (8 * i))); }
    void u64(std::uint64_t value) { for (unsigned i = 0; i < 8; ++i) u8(static_cast<std::uint8_t>(value >> (8 * i))); }
    void digest(const LobbyDigest& value) { bytes.insert(bytes.end(), value.begin(), value.end()); }
    void layers(const std::vector<core::ContentLayerIdentity>& values, bool names = true) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) { if (names) text(value.name, 256); digest(value.digest); }
    }
    void domains(const std::vector<sim::IdentityDomain>& values) {
        u32(static_cast<std::uint32_t>(values.size()));
        for (const auto& value : values) { text(value.name, 64); u32(value.version); digest(value.digest); }
    }
    void text(std::string_view value, std::size_t limit) {
        if (value.size() > limit || sanitize_lobby_text(value, limit) != value) throw std::runtime_error("invalid room text");
        u32(static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void blob(std::span<const std::uint8_t> value, std::size_t limit) {
        if (value.size() > limit) throw std::runtime_error("room blob exceeds bound");
        u32(static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void selection(const LobbySelection& value) {
        text(value.name, name_limit); text(value.faction, choice_limit);
        u32(value.colour); u32(value.team); u32(value.slot);
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at{};
    std::uint8_t u8() {
        if (at >= bytes.size()) throw std::runtime_error("truncated room frame");
        return bytes[at++];
    }
    std::uint32_t u32() { std::uint32_t n{}; for (unsigned i = 0; i < 4; ++i) n |= std::uint32_t{u8()} << (8 * i); return n; }
    std::uint64_t u64() { std::uint64_t n{}; for (unsigned i = 0; i < 8; ++i) n |= std::uint64_t{u8()} << (8 * i); return n; }
    LobbyDigest digest() { LobbyDigest value{}; for (auto& byte : value) byte = u8(); return value; }
    std::vector<core::ContentLayerIdentity> layers() {
        const auto count = u32(); if (count > content_layer_limit) throw std::runtime_error("too many preparation layers");
        std::vector<core::ContentLayerIdentity> values;
        for (std::uint32_t i = 0; i < count; ++i) { core::ContentLayerIdentity value; value.name = text(256); value.digest = digest(); values.push_back(std::move(value)); }
        return values;
    }
    std::vector<sim::IdentityDomain> domains() {
        const auto count = u32(); if (count > 32) throw std::runtime_error("too many preparation domains");
        std::vector<sim::IdentityDomain> values;
        for (std::uint32_t i = 0; i < count; ++i) {
            sim::IdentityDomain value; value.name = text(64); value.version = u32(); value.digest = digest();
            values.push_back(std::move(value));
        }
        return values;
    }
    std::span<const std::uint8_t> blob(std::size_t limit) {
        const auto n = u32();
        if (n > limit || n > bytes.size() - at) throw std::runtime_error("invalid room field length");
        auto value = bytes.subspan(at, n); at += n; return value;
    }
    std::string text(std::size_t limit, bool repair = false) {
        const auto raw = blob(limit);
        const std::string value(raw.begin(), raw.end());
        const auto clean = sanitize_lobby_text(value, limit);
        if (!repair && value != clean) throw std::runtime_error("noncanonical room text");
        return clean;
    }
    LobbySelection selection(bool repair = false) {
        LobbySelection value;
        value.name = text(name_limit, repair); value.faction = text(choice_limit);
        value.colour = u32(); value.team = u32(); value.slot = u32(); return value;
    }
    bool boolean() { const auto n = u8(); if (n > 1) throw std::runtime_error("invalid room boolean"); return n != 0; }
};
void validate_snapshot(const LobbySnapshot& value) {
    if (!valid_room(value.room) || value.revision == 0 || value.capacity == 0 || value.capacity > max_lobby_seats
        || value.seats.empty() || value.seats.size() > value.capacity || value.seats.front().peer != value.host
        || static_cast<unsigned>(value.phase) > static_cast<unsigned>(LobbyPhase::closed))
        throw std::runtime_error("invalid room snapshot");
    const bool frozen = value.phase == LobbyPhase::starting || value.phase == LobbyPhase::committed;
    if ((frozen && value.frozen_revision != value.revision) || (!frozen && value.frozen_revision != 0))
        throw std::runtime_error("invalid frozen room revision");
    if (!frozen && value.start_attempt != LobbyDigest{}) throw std::runtime_error("unfrozen room carries a preparation attempt");
    std::set<PeerIndex> peers;
    std::set<std::string> names;
    std::uint32_t last{};
    for (const auto& seat : value.seats) {
        if (!peers.insert(seat.peer).second || seat.seat.value <= last || seat.player == 0 || seat.selection.name.empty()
            || (frozen && !seat.ready)) throw std::runtime_error("invalid room roster");
        last = seat.seat.value;
        auto folded = seat.selection.name;
        for (auto& c : folded) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (!names.insert(std::move(folded)).second) throw std::runtime_error("duplicate guest name");
    }
}
bool valid_mod_description(std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return true;
    if (bytes.size() > mod_description_limit) return false;
    try {
        Reader in{bytes};
        if (in.u8() != 3 || in.u8() > 1) return false; // mod-description format v3
        const auto count = in.u32();
        if (count > 256) return false;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (in.u8() > 3) return false;
            static_cast<void>(in.text(4096)); static_cast<void>(in.text(4096));
            if (in.boolean()) static_cast<void>(in.text(4096));
            if (in.boolean()) for (unsigned j = 0; j < 32; ++j) static_cast<void>(in.u8());
            if (in.boolean()) for (unsigned j = 0; j < 32; ++j) static_cast<void>(in.u8());
        }
        return in.at == bytes.size();
    } catch (const std::runtime_error&) { return false; }
}
LobbySelection clean_selection(LobbySelection value) {
    value.name = sanitize_lobby_text(value.name, name_limit);
    return value;
}
std::string room_of(const LobbyFrame& value) {
    return std::visit([](const auto& frame) { return frame.room; }, value);
}
bool valid_limits(const LobbyLimits& limits) {
    return limits.frames_per_pump > 0 && limits.frames_per_pump <= 4096
        && limits.queued_setup_bytes >= max_lobby_frame_bytes && limits.queued_setup_bytes <= 64U * 1024U * 1024U
        && limits.chat_history > 0 && limits.chat_history <= 4096
        && limits.chat_messages_per_window > 0 && limits.chat_messages_per_window <= 256
        && limits.chat_window_ms > 0 && limits.denied_identities > 0 && limits.denied_identities <= 4096
        && limits.preparation_timeout_ms > 0;
}
bool nonzero(const LobbyDigest& value) { return value != LobbyDigest{}; }
void validate_layers(const std::vector<core::ContentLayerIdentity>& layers) {
    if (layers.size() > content_layer_limit) throw std::runtime_error("too many preparation layers");
    for (const auto& layer : layers) if (!nonzero(layer.digest) || sanitize_lobby_text(layer.name, 256) != layer.name)
        throw std::runtime_error("invalid preparation layer checksum/label");
}
std::string layer_difference(const std::vector<core::ContentLayerIdentity>& expected,
    const std::vector<core::ContentLayerIdentity>& actual) {
    std::size_t at{};
    while (at < expected.size() && at < actual.size() && expected[at].digest == actual[at].digest) ++at;
    if (at == expected.size() && at == actual.size()) return {};
    const auto& name = at < expected.size() ? expected[at].name : actual[at].name;
    return "mod content/order differs at layer " + std::to_string(at + 1) + ": " + (name.empty() ? "unnamed mod" : name);
}
bool same_prepared_claim(const LobbyPrepared& a, const LobbyPrepared& b) {
    return a.room == b.room && a.revision == b.revision && a.attempt == b.attempt && a.manifest == b.manifest
        && a.offer == b.offer && a.domains == b.domains && a.refusal == b.refusal && layer_difference(a.layers, b.layers).empty();
}
sim::MatchIdentity domain_identity(const std::vector<sim::IdentityDomain>& domains) {
    sim::MatchIdentity identity; identity.domains = domains;
    identity.sha256 = core::sha256(sim::canonical_match_identity(identity)); return identity;
}
void validate_domains(const std::vector<sim::IdentityDomain>& domains) {
    if (domains.empty() || domains.size() > 32 || !sim::validate_match_identity(domain_identity(domains)))
        throw std::runtime_error("noncanonical preparation domains");
}
void validate_prepare(const LobbyPrepare& value) {
    if (value.snapshot.empty() || value.snapshot.front() != lobby_snapshot_kind || value.inputs.empty())
        throw std::runtime_error("missing preparation snapshot/inputs");
    auto decoded = decode_lobby_frame(value.snapshot);
    const auto* snapshot = decoded ? std::get_if<LobbySnapshot>(&decoded.value()) : nullptr;
    if (!snapshot || snapshot->room != value.room || snapshot->revision != value.revision
        || snapshot->phase != LobbyPhase::starting || !nonzero(snapshot->start_attempt))
        throw std::runtime_error("invalid preparation snapshot");
    validate_domains(value.inputs);
    validate_layers(value.layers);
}
void validate_prepared(const LobbyPrepared& value) {
    if (!value.revision || !nonzero(value.attempt) || !nonzero(value.manifest) || value.domains.size() > 32)
        throw std::runtime_error("invalid prepared campaign claim");
    if (value.refusal.empty()) {
        if (!nonzero(value.offer) || value.domains.empty()) throw std::runtime_error("incomplete prepared campaign claim");
        validate_domains(value.domains);
    } else if (nonzero(value.offer) || !value.domains.empty() || !value.layers.empty()) throw std::runtime_error("refusal carries success claims");
    validate_layers(value.layers);
}
LobbyDigest manifest_hash(std::span<const std::uint8_t> encoded) {
    Writer out; out.text("eawr-gc-prepare-manifest-v1", 64);
    out.bytes.insert(out.bytes.end(), encoded.begin(), encoded.end()); return core::sha256(out.bytes);
}
LobbyDigest prepared_hash(const LobbyPrepared& value) {
    Writer out; out.text("eawr-gc-prepared-v1", 64); out.text(value.room, 64); out.u64(value.revision);
    out.digest(value.attempt); out.digest(value.manifest); out.digest(value.offer); out.domains(value.domains); out.text(value.refusal, 512);
    out.layers(value.layers, false); // Labels never enter the prepared content claim's checksum.
    return core::sha256(out.bytes);
}
}

bool valid_lobby_mod_description(std::span<const std::uint8_t> bytes) { return valid_mod_description(bytes); }
core::Diagnostic lobby_diagnostic(LobbyReason reason, std::string message) {
    core::Diagnostic value;
    const unsigned code = reason == LobbyReason::pair_unreachable ? 1762U : reason == LobbyReason::mesh_incomplete ? 1764U
        : 1600U + static_cast<unsigned>(reason);
    value.code = "EAWR-NET-" + std::to_string(code);
    value.message = sanitize_lobby_text(message, 512);
    return value;
}
core::Diagnostic start_pending_diagnostic(std::string message) {
    core::Diagnostic value;
    value.code = std::string(start_pending_code);
    value.message = sanitize_lobby_text(message, 512);
    return value;
}
bool is_start_pending(const core::Diagnostic& diagnostic) noexcept { return diagnostic.code == start_pending_code; }
std::string sanitize_lobby_text(std::string_view value, std::size_t limit) {
    auto clean = sanitize_lan_text(value, limit);
    for (auto& c : clean) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
    // Prevent invisible directional overrides in guest labels and chat.
    for (std::size_t i = 0; i < clean.size();) {
        const auto a = static_cast<unsigned char>(clean[i]);
        const auto b = i + 1 < clean.size() ? static_cast<unsigned char>(clean[i + 1]) : 0;
        const auto c = i + 2 < clean.size() ? static_cast<unsigned char>(clean[i + 2]) : 0;
        if (a == 0xD8 && b == 0x9C) clean.erase(i, 2); // U+061C.
        else if (a == 0xE2 && ((b == 0x80 && ((c >= 0xAA && c <= 0xAE) || c == 0x8E || c == 0x8F))
            || (b == 0x81 && c >= 0xA6 && c <= 0xA9))) clean.erase(i, 3);
        else ++i;
    }
    return clean;
}
core::Result<Bytes> encode_lobby_frame(const LobbyFrame& frame) {
    try {
        Writer out;
        const auto room = room_of(frame);
        if (!valid_room(room)) throw std::runtime_error("invalid room identifier");
        out.u8(frame.index() < 8 ? static_cast<std::uint8_t>(lobby_snapshot_kind + frame.index())
            : (std::holds_alternative<LobbyPrepare>(frame) ? lobby_prepare_kind : lobby_prepared_kind));
        out.u32(3); out.text(room, 64);
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, LobbySnapshot>) {
                validate_snapshot(value);
                out.u64(value.revision); out.u64(value.frozen_revision); out.u64(value.chat_sequence); out.u32(value.capacity); out.u32(value.host);
                out.u8(static_cast<std::uint8_t>(value.phase)); out.text(value.game.map, 1024); out.blob(value.game.options, options_limit);
                out.u32(static_cast<std::uint32_t>(value.seats.size()));
                for (const auto& seat : value.seats) {
                    out.u32(seat.peer); out.u32(seat.seat.value); out.u32(seat.player); out.selection(seat.selection); out.u8(seat.ready ? 1 : 0);
                }
                out.digest(value.start_attempt);
                if (!value.mounted_mods.empty() && value.mounted_mods.front() == 1)
                    throw std::runtime_error("mod list version 1, this build needs version 2");

                if (!valid_mod_description(value.mounted_mods)) throw std::runtime_error("invalid mounted mods");
                out.blob(value.mounted_mods, mod_description_limit);
            } else if constexpr (std::is_same_v<T, LobbyRequest>) {
                if (!value.revision || static_cast<unsigned>(value.action) > static_cast<unsigned>(LobbyAction::snapshot)
                    || (value.action != LobbyAction::select && value.selection != LobbySelection{}))
                    throw std::runtime_error("invalid room request");
                out.u64(value.revision); out.u8(static_cast<std::uint8_t>(value.action)); out.selection(value.selection);
            } else if constexpr (std::is_same_v<T, LobbyChatRequest>) {
                out.text(value.text, chat_limit);
            } else if constexpr (std::is_same_v<T, LobbyChat>) {
                if (!value.sequence || !value.seat.value || value.name.empty() || value.text.empty()) throw std::runtime_error("invalid room chat");
                out.u64(value.sequence); out.u32(value.seat.value); out.text(value.name, name_limit); out.text(value.text, chat_limit);
            } else if constexpr (std::is_same_v<T, LobbySetupFrame>) {
                if (!value.revision || value.frame.empty()) throw std::runtime_error("invalid setup envelope");
                out.u64(value.revision); out.blob(value.frame, max_lobby_payload_bytes); out.digest(value.attempt);
            } else if constexpr (std::is_same_v<T, LobbyPrepare>) {
                validate_prepare(value);
                out.u64(value.revision); out.blob(value.snapshot, max_lobby_payload_bytes); out.domains(value.inputs);
                out.layers(value.layers);
            } else if constexpr (std::is_same_v<T, LobbyPrepared>) {
                validate_prepared(value);
                out.u64(value.revision); out.digest(value.attempt); out.digest(value.manifest); out.digest(value.offer);
                out.domains(value.domains);
                out.text(value.refusal, 512);
                out.layers(value.layers);
                out.digest(prepared_hash(value));
            } else {
                if (!value.revision || static_cast<unsigned>(value.reason) > static_cast<unsigned>(LobbyReason::mesh_incomplete))
                    throw std::runtime_error("invalid room notice");
                out.u64(value.revision); out.u8(static_cast<std::uint8_t>(value.reason)); out.text(value.message, 512);
            }
        }, frame);
        if (out.bytes.size() > max_lobby_frame_bytes) throw std::runtime_error("oversized room frame");
        return core::Result<Bytes>::success(std::move(out.bytes));
    } catch (const std::runtime_error& error) { return failure<Bytes>(LobbyReason::malformed, error.what()); }
}
core::Result<LobbyFrame> decode_lobby_frame(std::span<const std::uint8_t> bytes) {
    try {
        if (bytes.size() > max_lobby_frame_bytes) throw std::runtime_error("oversized room frame");
        Reader in{bytes, 0};
        const auto kind = in.u8();
        if (in.u32() != 3) throw std::runtime_error("unsupported room protocol version; Galactic preparation requires version 3");
        auto room = in.text(64);
        if (!valid_room(room)) throw std::runtime_error("invalid room identifier");
        LobbyFrame result;
        switch (kind) {
        case lobby_snapshot_kind: {
            LobbySnapshot value;
            value.room = room; value.revision = in.u64(); value.frozen_revision = in.u64(); value.chat_sequence = in.u64(); value.capacity = in.u32(); value.host = in.u32();
            const auto phase = in.u8();
            if (phase > static_cast<unsigned>(LobbyPhase::closed)) throw std::runtime_error("invalid room phase");
            value.phase = static_cast<LobbyPhase>(phase); value.game.map = in.text(1024);
            const auto options = in.blob(options_limit); value.game.options.assign(options.begin(), options.end());
            const auto count = in.u32();
            if (count > max_lobby_seats) throw std::runtime_error("oversized room roster");
            for (std::uint32_t i = 0; i < count; ++i) {
                LobbySeat seat;
                seat.peer = in.u32(); seat.seat = sim::SeatId{in.u32()}; seat.player = in.u32(); seat.selection = in.selection(); seat.ready = in.boolean();
                value.seats.push_back(std::move(seat));
            }
            value.start_attempt = in.digest();
            const auto mods = in.blob(mod_description_limit);
            if (!mods.empty() && mods.front() == 1) throw std::runtime_error("mod list version 1, this build needs version 2");
            if (!valid_mod_description(mods)) throw std::runtime_error("invalid mounted mods");
            value.mounted_mods.assign(mods.begin(), mods.end());
            validate_snapshot(value); result = std::move(value); break;
        }
        case lobby_request_kind: {
            LobbyRequest value;
            value.room = room; value.revision = in.u64(); const auto action = in.u8();
            if (action > static_cast<unsigned>(LobbyAction::snapshot) || value.revision == 0) throw std::runtime_error("invalid room request");
            value.action = static_cast<LobbyAction>(action); value.selection = in.selection(true);
            if (value.action != LobbyAction::select && value.selection != LobbySelection{}) throw std::runtime_error("unused request fields");
            result = std::move(value); break;
        }
        case lobby_chat_request_kind: result = LobbyChatRequest{room, in.text(chat_limit, true)}; break;
        case lobby_chat_kind: {
            LobbyChat value{room, in.u64(), sim::SeatId{in.u32()}, in.text(name_limit), in.text(chat_limit)};
            if (!value.sequence || !value.seat.value || value.name.empty() || value.text.empty()) throw std::runtime_error("invalid room chat");
            result = std::move(value); break;
        }
        case lobby_setup_kind: {
            LobbySetupFrame value;
            value.room = room; value.revision = in.u64(); const auto raw = in.blob(max_lobby_payload_bytes); value.frame.assign(raw.begin(), raw.end());
            value.attempt = in.digest();
            if (value.revision == 0 || value.frame.empty()) throw std::runtime_error("invalid setup envelope");
            result = std::move(value); break;
        }
        case lobby_prepare_kind: {
            LobbyPrepare value; value.room = room; value.revision = in.u64();
            const auto snapshot = in.blob(max_lobby_payload_bytes); value.snapshot.assign(snapshot.begin(), snapshot.end());
            value.inputs = in.domains();
            value.layers = in.layers();
            validate_prepare(value); result = std::move(value); break;
        }
        case lobby_prepared_kind: {
            LobbyPrepared value; value.room = room; value.revision = in.u64();
            value.attempt = in.digest(); value.manifest = in.digest(); value.offer = in.digest();
            value.domains = in.domains();
            value.refusal = in.text(512); value.layers = in.layers(); validate_prepared(value);
            if (in.digest() != prepared_hash(value)) throw std::runtime_error("prepared campaign digest differs from its claims");
            result = std::move(value); break;
        }
        case lobby_cancel_kind: case lobby_closed_kind: case lobby_rejected_kind: {
            LobbyNotice value;
            value.room = room; value.revision = in.u64(); const auto reason = in.u8(); value.message = in.text(512);
            if (!value.revision || reason > static_cast<unsigned>(LobbyReason::mesh_incomplete)) throw std::runtime_error("invalid room notice");
            value.reason = static_cast<LobbyReason>(reason);
            if (kind == lobby_cancel_kind) result = LobbyCancel{std::move(value)};
            else if (kind == lobby_closed_kind) result = LobbyClosed{std::move(value)};
            else result = LobbyRejected{std::move(value)};
            break;
        }
        default: throw std::runtime_error("unknown room message kind");
        }
        if (in.at != bytes.size()) throw std::runtime_error("trailing room bytes");
        return core::Result<LobbyFrame>::success(std::move(result));
    } catch (const std::runtime_error& error) { return failure<LobbyFrame>(LobbyReason::malformed, error.what()); }
}

struct LobbyRuntime;
namespace {
class SetupStream final : public MessageTransport {
public:
    MessageTransport* outer{};
    std::string room;
    std::uint64_t revision{};
    LobbyDigest attempt{};
    std::vector<PeerIndex> peers;
    std::deque<Received> inbox;
    std::size_t bytes{};
    bool transferred{};
    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) override {
        if (to >= peers.size()) return failure<void>(LobbyReason::authority, "invalid setup destination");
        if (frame.size() > max_lobby_payload_bytes) return failure<void>(LobbyReason::resource_limit, "setup/game frame exceeds envelope bound");
        auto encoded = encode_lobby_frame(LobbySetupFrame{room, revision, Bytes(frame.begin(), frame.end()), attempt});
        if (!encoded) return core::Result<void>::failure(encoded.error());
        return outer->send(peers[to], encoded.value());
    }
    std::optional<Received> poll() override {
        if (!inbox.empty()) {
            auto received = std::move(inbox.front()); inbox.pop_front(); bytes -= received.frame.size(); return received;
        }
        // After transfer the game core owns outer polling. Staging frames cannot
        // be interpreted as simulation input; this room can never start again.
        if (transferred) {
            for (unsigned i = 0; i < 256; ++i) {
                auto raw = outer->poll(); if (!raw) break;
                auto decoded = decode_lobby_frame(raw->frame); if (!decoded) continue;
                auto* frame = std::get_if<LobbySetupFrame>(&decoded.value());
                const auto found = std::find(peers.begin(), peers.end(), raw->from);
                if (frame && frame->room == room && frame->revision == revision && frame->attempt == attempt && found != peers.end())
                    return Received{static_cast<PeerIndex>(found - peers.begin()), std::move(frame->frame)};
            }
        }
        return {};
    }
    LinkState link(PeerIndex peer) const override { return peer < peers.size() ? outer->link(peers[peer]) : LinkState::closed; }
    PeerIndex self() const override { return static_cast<PeerIndex>(std::find(peers.begin(), peers.end(), outer->self()) - peers.begin()); }
    std::uint32_t peer_count() const override { return static_cast<std::uint32_t>(peers.size()); }
    std::optional<LinkStats> link_stats(PeerIndex peer) const override {
        return peer < peers.size() ? outer->link_stats(peers[peer]) : std::nullopt;
    }
    std::uint64_t edge_identity(PeerIndex peer) const override {
        return peer < peers.size() ? outer->edge_identity(peers[peer]) : 0;
    }
    void fail_repair(PeerIndex peer, unsigned code) override {
        if (peer < peers.size()) outer->fail_repair(peers[peer], code);
    }
};
}
struct LobbyRuntime {
    MessageTransport* transport{};
    bool is_host{}, initialized{}, taken{};
    LobbySnapshot state;
    LobbyHooks hooks;
    LobbyLimits limits;
    std::optional<std::uint64_t> now;
    std::optional<LobbyNotice> notice;
    std::deque<LobbyChat> history;
    std::uint64_t chat_sequence{};
    std::uint32_t next_seat{2};
    std::map<PeerIndex, std::string> identities;
    std::set<std::string> denied;
    std::map<PeerIndex, std::deque<std::uint64_t>> chat_times;
    std::map<PeerIndex, std::deque<std::uint64_t>> request_times;
    bool awaiting_snapshot{}, roster_lock_pending{};
    // #6045: the host's Start waits on an asynchronous offer hook (the frozen revision it will publish), or a joiner's frozen
    // setup waits on it. The room stays in staging on the wire while the host waits: guests' setup deadlines start with the offer.
    std::optional<std::uint64_t> start_pending;
    bool setup_pending{};
    std::uint64_t snapshot_query_time{}, dropped{};
    std::unique_ptr<SetupStream> stream;
    std::optional<LobbySetupContext> context;
    std::optional<SetupSession> setup;
    std::optional<LobbyPrepare> preparation;
    LobbyDigest manifest_digest{};
    std::vector<SetupSeat> preparation_seats;
    std::optional<SetupOffer> prepared_offer;
    std::map<PeerIndex, LobbyPrepared> prepared_claims;
    std::uint64_t preparation_since{};
    std::uint64_t retired_preparation_revision{};
    bool preparation_failed{}, preparation_released{};

    LobbySeat* seat(PeerIndex peer) {
        const auto found = std::find_if(state.seats.begin(), state.seats.end(), [peer](const auto& value) { return value.peer == peer; });
        return found == state.seats.end() ? nullptr : &*found;
    }
    core::Result<void> send(PeerIndex to, const LobbyFrame& frame) {
        auto encoded = encode_lobby_frame(frame);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        auto sent = transport->send(to, encoded.value());
        if (!sent && is_host && to != transport->self() && seat(to))
            static_cast<void>(remove(to, LobbyReason::link_closed, false));
        return sent;
    }
    core::Result<void> broadcast(const LobbyFrame& frame) {
        // Encode once and finish this generation for every survivor before
        // publishing a loss/recovery generation. Never interleave old snapshots
        // after the fresh membership snapshot from a failed send.
        auto encoded = encode_lobby_frame(frame);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        const auto* snapshot = std::get_if<LobbySnapshot>(&frame);
        const bool committing = snapshot && snapshot->phase == LobbyPhase::committed;
        std::vector<PeerIndex> lost;
        std::optional<core::Diagnostic> first_error;
        const auto recipients = state.seats;
        for (const auto& value : recipients) if (value.peer != transport->self()) {
            auto sent = transport->link(value.peer) == LinkState::open ? transport->send(value.peer, encoded.value())
                : failure<void>(LobbyReason::link_closed, "room peer stream closed");
            if (!sent) { if (!first_error) first_error = sent.error(); lost.push_back(value.peer); }
        }
        if (!first_error) return core::Result<void>::success();
        for (const auto peer : lost) if (seat(peer)) {
            if (auto removed = drop(peer, LobbyReason::link_closed, false); !removed) return removed;
        }
        if (committing) {
            static_cast<void>(terminal(LobbyReason::link_closed, "Start commitment delivery failed"));
        } else if (state.phase == LobbyPhase::starting) {
            static_cast<void>(abort(LobbyReason::link_closed, "peer lost during Start announcement"));
        } else if (state.phase == LobbyPhase::staging) {
            if (auto advanced = advance(); !advanced) return advanced;
            static_cast<void>(broadcast(state));
        }
        return core::Result<void>::failure(std::move(*first_error));
    }
    core::Result<void> allowed(std::uint64_t revision, bool host_only = true, bool allow_starting = false) const {
        if (taken || state.phase == LobbyPhase::committed || state.phase == LobbyPhase::closed)
            return failure<void>(LobbyReason::frozen, "room controls are locked");
        if (host_only && !is_host) return failure<void>(LobbyReason::authority, "only the host controls this action");
        if (!initialized || revision != state.revision) return failure<void>(LobbyReason::stale_revision, "room revision changed");
        if (!allow_starting && state.phase != LobbyPhase::staging) return failure<void>(LobbyReason::frozen, "room is starting");
        if (!allow_starting && start_pending)
            return failure<void>(LobbyReason::frozen, "The host is preparing the campaign start; the room is locked until it is done or cancelled.");
        return core::Result<void>::success();
    }
    void reset_setup() {
        if (preparation) retired_preparation_revision = std::max(retired_preparation_revision, state.frozen_revision);
        if (preparation && hooks.discard_preparation) hooks.discard_preparation();
        preparation.reset(); prepared_offer.reset(); prepared_claims.clear(); preparation_seats.clear();
        manifest_digest = {}; preparation_failed = false; preparation_released = false;
        roster_lock_pending = false; setup_pending = false; setup.reset(); context.reset(); stream.reset();
    }
    // Ends a pending Start and tells the room why (chat line from the host's seat plus the host's own notice).
    core::Result<void> cancel_pending(LobbyReason reason, std::string message) {
        if (!start_pending) return core::Result<void>::success();
        notice = LobbyNotice{state.room, *start_pending, reason, sanitize_lobby_text(message, 512)};
        start_pending.reset();
        static_cast<void>(post(state.host, "Start was cancelled: " + message, true));
        return core::Result<void>::success();
    }
    core::Result<void> begin_start(std::uint64_t revision);
    core::Result<void> resume_start();
    core::Result<void> poll_preparation();
    core::Result<void> refuse_preparation(std::string message) {
        // A claim with neither an offer nor a refusal cannot be encoded, so a hook's empty (or blank) message must not reach it.
        if (sanitize_lobby_text(message, 512).find_first_not_of(' ') == std::string::npos)
            message = "The campaign could not be prepared on this computer.";
        if (is_host) return abort(LobbyReason::setup_failed, std::move(message));
        preparation_failed = true; setup_pending = true; // Keep servicing the staging room until the host cancels everyone.
        if (hooks.discard_preparation) hooks.discard_preparation();
        notice = LobbyNotice{state.room, state.frozen_revision, LobbyReason::setup_failed, sanitize_lobby_text(message, 512)};
        LobbyPrepared claim;
        claim.room = state.room; claim.revision = state.frozen_revision; claim.attempt = state.start_attempt;
        claim.manifest = manifest_digest; claim.refusal = notice->message;
        return send(state.host, claim);
    }
    core::Result<void> fail_setup_creation(const core::Diagnostic& error);
    core::Result<void> advance() {
        if (state.revision == std::numeric_limits<std::uint64_t>::max())
            return failure<void>(LobbyReason::resource_limit, "room revision exhausted");
        ++state.revision; return core::Result<void>::success();
    }
    void clear_ready() { for (auto& value : state.seats) value.ready = false; }
    core::Result<void> terminal(LobbyReason reason, std::string message) {
        start_pending.reset(); reset_setup(); state.phase = LobbyPhase::closed; state.frozen_revision = 0; state.start_attempt = {}; clear_ready();
        notice = LobbyNotice{state.room, state.revision, reason, sanitize_lobby_text(message, 512)};
        auto sent = broadcast(LobbyClosed{*notice});
        for (const auto& value : state.seats) if (value.peer != state.host) hooks.remove_peer(value.peer, reason);
        return sent;
    }
    core::Result<void> validate(const LobbySnapshot& candidate, bool starting = false) const {
        auto encoded = encode_lobby_frame(candidate);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        return hooks.validate(candidate, starting);
    }
    core::Result<void> abort(LobbyReason reason, std::string message) {
        // A pending remote Lock may already have succeeded. Reopening staging
        // locally would advertise vacancies in a service-locked room.
        if (roster_lock_pending) return terminal(reason,
            std::move(message) + ". Start was being confirmed; create a new room.");
        const auto frozen = state.frozen_revision;
        // A poll inside the Start drain can observe a close after pump's loss
        // scan. Remove every such guest before publishing the cancellation.
        std::vector<PeerIndex> lost;
        for (const auto& value : state.seats)
            if (value.peer != state.host && transport->link(value.peer) == LinkState::closed) lost.push_back(value.peer);
        for (const auto peer : lost)
            if (auto removed = drop(peer, LobbyReason::link_closed, false); !removed) return removed;
        start_pending.reset(); reset_setup(); state.phase = LobbyPhase::staging; state.frozen_revision = 0; state.start_attempt = {}; clear_ready();
        if (auto advanced = advance(); !advanced) return advanced;
        notice = LobbyNotice{state.room, frozen, reason, sanitize_lobby_text(message, 512)};
        if (auto sent = broadcast(LobbyCancel{*notice}); !sent) return sent;
        return broadcast(state);
    }
    core::Result<void> reject(PeerIndex peer, const core::Diagnostic& error, LobbyReason reason) {
        return send(peer, LobbyRejected{{state.room, state.revision, reason, sanitize_lobby_text(error.message, 512)}});
    }
    core::Result<void> edit(PeerIndex peer, LobbySelection selection, sim::tactical::PlayerId player) {
        const auto* previous = seat(peer);
        if (!previous) return failure<void>(LobbyReason::authority, "peer has no admitted seat");
        auto candidate = state;
        const auto index = static_cast<std::size_t>(previous - state.seats.data());
        auto& updated = candidate.seats[index];
        updated.selection = clean_selection(std::move(selection)); updated.player = player;
        if (updated == *previous) return core::Result<void>::success();
        if (hooks.follow_choice) if (auto followed = hooks.follow_choice(state, candidate, peer); !followed) return followed;
        auto old_selection = previous->selection; old_selection.name = updated.selection.name;
        const bool consequential = old_selection != updated.selection || previous->player != player;
        if (consequential) for (auto& value : candidate.seats) value.ready = false;
        if (auto valid = validate(candidate); !valid) return valid;
        if (auto advanced = advance(); !advanced) return advanced;
        candidate.revision = state.revision; state = std::move(candidate); return broadcast(state);
    }
    core::Result<void> drop(PeerIndex peer, LobbyReason reason, bool deny, std::string notice_text = "seat removed from room") {
        if (peer == state.host || !seat(peer)) return failure<void>(LobbyReason::authority, "cannot remove this seat");
        if (deny && denied.size() >= limits.denied_identities)
            return failure<void>(LobbyReason::resource_limit, "room identity denial budget exhausted");
        const auto identity = identities.find(peer);
        if (deny && identity == identities.end()) return failure<void>(LobbyReason::authority, "seat has no admitted identity");
        if (deny) denied.insert(identity->second);
        identities.erase(peer); chat_times.erase(peer); request_times.erase(peer);
        state.seats.erase(std::remove_if(state.seats.begin(), state.seats.end(), [peer](const auto& value) { return value.peer == peer; }), state.seats.end());
        clear_ready();
        hooks.remove_peer(peer, reason);
        static_cast<void>(send(peer, LobbyClosed{{state.room, state.revision, reason, sanitize_lobby_text(notice_text, 512)}}));
        return core::Result<void>::success();
    }
    core::Result<void> remove(PeerIndex peer, LobbyReason reason, bool deny, std::string notice_text = "seat removed from room") {
        const bool starting = state.phase == LobbyPhase::starting;
        if (auto removed = drop(peer, reason, deny, std::move(notice_text)); !removed) return removed;
        if (starting) return abort(reason, "seat removed during Start");
        if (state.phase == LobbyPhase::committed) {
            return terminal(LobbyReason::link_closed, "committed room peer lost before handoff");
        }
        if (state.phase == LobbyPhase::closed) return core::Result<void>::success();
        if (auto advanced = advance(); !advanced) return advanced;
        return broadcast(state);
    }
    bool budget(std::map<PeerIndex, std::deque<std::uint64_t>>& windows, PeerIndex peer) {
        auto& times = windows[peer]; const auto clock = now.value_or(0);
        while (!times.empty() && clock - times.front() >= limits.chat_window_ms) times.pop_front();
        if (times.size() >= limits.chat_messages_per_window) return false;
        times.push_back(clock); return true;
    }
    core::Result<void> apply_request(PeerIndex peer, const LobbyRequest& value) {
        auto* local = seat(peer);
        if (!local) return failure<void>(LobbyReason::authority, "peer has no admitted seat");
        if (!budget(request_times, peer)) return failure<void>(LobbyReason::resource_limit, "room request rate exceeded");
        if (value.action == LobbyAction::snapshot) return peer == transport->self() ? core::Result<void>::success() : send(peer, state);
        if (auto valid = allowed(value.revision, false, value.action == LobbyAction::unready || value.action == LobbyAction::leave); !valid) return valid;
        switch (value.action) {
        case LobbyAction::select: return edit(peer, value.selection, local->player);
        case LobbyAction::ready:
            if (auto valid = validate(state, true); !valid) return valid;
            if (local->ready) return core::Result<void>::success();
            local->ready = true; return broadcast(state);
        case LobbyAction::unready:
            if (state.phase == LobbyPhase::starting) return abort(LobbyReason::not_ready, "player withdrew readiness during Start");
            if (start_pending) static_cast<void>(cancel_pending(LobbyReason::not_ready, "a player withdrew readiness during Start preparation"));
            if (!local->ready) return core::Result<void>::success();
            local->ready = false; return broadcast(state);
        case LobbyAction::leave:
            if (peer == state.host) return failure<void>(LobbyReason::authority, "host must close the room");
            return remove(peer, LobbyReason::left, false);
        case LobbyAction::snapshot: break;
        }
        return failure<void>(LobbyReason::malformed, "unknown room action");
    }
    void retain(LobbyChat value) {
        history.push_back(std::move(value));
        while (history.size() > limits.chat_history) history.pop_front();
    }
    core::Result<void> post(PeerIndex peer, std::string text, bool room_notice = false) {
        if (state.phase == LobbyPhase::closed || state.phase == LobbyPhase::committed)
            return failure<void>(LobbyReason::frozen, "room chat is closed");
        const auto* sender = seat(peer);
        if (!sender) return failure<void>(LobbyReason::authority, "chat sender has no admitted seat");
        text = sanitize_lobby_text(text, chat_limit);
        if (text.empty() || std::all_of(text.begin(), text.end(), [](char c) { return c == ' '; }))
            return failure<void>(LobbyReason::invalid_choice, "empty room chat");
        if (!room_notice && !budget(chat_times, peer)) return failure<void>(LobbyReason::chat_limit, "room chat rate exceeded");
        if (chat_sequence == std::numeric_limits<std::uint64_t>::max()) return failure<void>(LobbyReason::resource_limit, "room chat sequence exhausted");
        LobbyChat value{state.room, ++chat_sequence, sender->seat, sender->selection.name, std::move(text)};
        state.chat_sequence = chat_sequence;
        retain(value); return broadcast(value);
    }
    // A joiner builds its frozen setup here. When the offer hook is asynchronous (#6045) this returns ok with
    // `setup_pending` set, keeps the stream (setup frames queue in it) and is called again by every pump until the hook answers.
    core::Result<void> create_setup() {
        if (!stream) {
            stream = std::make_unique<SetupStream>(); stream->outer = transport; stream->room = state.room; stream->revision = state.frozen_revision;
            stream->attempt = state.start_attempt;
            for (const auto& value : state.seats) stream->peers.push_back(value.peer);
            auto created = hooks.setup(*stream, state);
            if (!created) { reset_setup(); return core::Result<void>::failure(created.error()); }
            context.emplace(std::move(created).value()); context->options.auto_accept = false; context->options.auto_start = false;
        }
        std::vector<SetupSeat> roster;
        for (const auto& value : state.seats) roster.push_back({static_cast<PeerIndex>(roster.size()), value.seat, value.player});
        if (hooks.bind_setup_seats) {
            auto bound = hooks.bind_setup_seats(state, roster);
            if (!bound) { reset_setup(); return core::Result<void>::failure(bound.error()); }
            roster = std::move(bound).value();
        }
        auto expected = prepared_offer ? core::Result<SetupOffer>::success(*prepared_offer) : hooks.offer(state, roster);
        if (!expected) {
            if (is_start_pending(expected.error())) { setup_pending = true; return core::Result<void>::success(); }
            reset_setup(); return core::Result<void>::failure(expected.error());
        }
        setup_pending = false;
        auto identify = context->providers.identify;
        if (!identify) { reset_setup(); return failure<void>(LobbyReason::setup_failed, "missing local content identifier"); }
        context->providers.identify = [identify = std::move(identify), expected = std::move(expected).value()](const SetupOffer& offered) {
            if (offered.seed != expected.seed || offered.domain != expected.domain || offered.setup != expected.setup
                || offered.seats != expected.seats || offered.input_delay != expected.input_delay || offered.final_tick_count != expected.final_tick_count
                || offered.recovery != expected.recovery)
            {
                std::string message{setup_offer_differs_message};
                // Name the differing part so the host can tell the player which input to compare.
                if (offered.domain == galactic_setup_domain && expected.domain == galactic_setup_domain)
                    if (const auto part = galactic_setup_difference(offered.setup, expected.setup); !part.empty())
                        message += ": " + std::string(part);
                return failure<sim::MatchAgreement>(LobbyReason::setup_failed, std::move(message));
            }
            return identify(offered);
        };
        auto session = SetupSession::join(*stream, context->providers, context->options);
        if (!session) { reset_setup(); return core::Result<void>::failure(session.error()); }
        setup.emplace(std::move(session).value()); return core::Result<void>::success();
    }
    core::Result<void> query_snapshot() {
        if (awaiting_snapshot && now.value_or(0) - snapshot_query_time < limits.chat_window_ms) return core::Result<void>::success();
        auto sent = send(state.host, LobbyRequest{state.room, state.revision, LobbyAction::snapshot, {}});
        if (sent) { awaiting_snapshot = true; snapshot_query_time = now.value_or(0); }
        return sent;
    }
    core::Result<void> receive(Received raw) {
        if (raw.from >= transport->peer_count() || raw.from == transport->self()) return failure<void>(LobbyReason::authority, "invalid room sender");
        auto decoded = decode_lobby_frame(raw.frame);
        if (!decoded) {
            if (is_host && preparation && seat(raw.from) && !raw.frame.empty() && raw.frame.front() == lobby_prepared_kind)
                return refuse_preparation("peer " + std::to_string(raw.from) + ": malformed prepared claim");
            return core::Result<void>::failure(decoded.error());
        }
        auto& frame = decoded.value();
        if (room_of(frame) != state.room) return failure<void>(LobbyReason::authority, "message belongs to another room");
        if (const auto* manifest = std::get_if<LobbyPrepare>(&frame)) {
            if (is_host || raw.from != state.host) return failure<void>(LobbyReason::authority, "preparation manifest is host-only");
            if (manifest->revision < state.revision || manifest->revision <= retired_preparation_revision)
                return core::Result<void>::success();
            if (!initialized || manifest->revision != state.revision || state.phase == LobbyPhase::closed || state.phase == LobbyPhase::committed)
                return failure<void>(LobbyReason::stale_revision, "preparation manifest is not for this staging revision");
            if (preparation) {
                const auto encoded = encode_lobby_frame(*manifest);
                if (encoded && manifest_hash(encoded.value()) == manifest_digest) return core::Result<void>::success();
                return refuse_preparation("host changed the frozen preparation manifest");
            }
            const auto frozen = std::get<LobbySnapshot>(decode_lobby_frame(manifest->snapshot).value());
            auto expected = state; expected.phase = LobbyPhase::starting; expected.frozen_revision = state.revision;
            expected.start_attempt = frozen.start_attempt;
            const auto refuse_manifest = [&](std::string message) {
                // Retain our own choices, but bind the refusal to this host attempt so every peer can cancel promptly.
                reset_setup(); state = expected; preparation = *manifest;
                manifest_digest = manifest_hash(encode_lobby_frame(*manifest).value()); preparation_since = now.value_or(0);
                return refuse_preparation(std::move(message));
            };
            if (frozen != expected || !hooks.preparation_inputs || !hooks.preparation_domains)
                return refuse_manifest("preparation manifest differs from frozen room choices or is unsupported");
            if (auto valid = hooks.validate(frozen, true); !valid) return refuse_manifest(valid.error().message);
            std::vector<SetupSeat> roster;
            for (const auto& value : frozen.seats) roster.push_back({static_cast<PeerIndex>(roster.size()), value.seat, value.player});
            if (hooks.bind_setup_seats) {
                auto bound = hooks.bind_setup_seats(frozen, roster);
                if (!bound) return refuse_manifest(bound.error().message);
                roster = std::move(bound).value();
            }
            reset_setup(); state = frozen; preparation = *manifest; preparation_seats = std::move(roster);
            manifest_digest = manifest_hash(encode_lobby_frame(*manifest).value());
            preparation_since = now.value_or(0); setup_pending = true;
            if (hooks.preparation_mounts) {
                auto layers = hooks.preparation_mounts(state, nullptr);
                if (!layers) return refuse_preparation(layers.error().message);
                if (const auto different = layer_difference(manifest->layers, layers.value()); !different.empty()) return refuse_preparation(different);
            } else if (!manifest->layers.empty()) return refuse_preparation("mounted content verification is unavailable");
            const auto local = hooks.preparation_inputs(state, preparation_seats);
            if (!local) return refuse_preparation(local.error().message);
            if (const auto mismatch = sim::describe_mismatch(domain_identity(manifest->inputs), domain_identity(local.value())))
                return refuse_preparation(mismatch->message);
            return core::Result<void>::success();
        }
        if (const auto* claim = std::get_if<LobbyPrepared>(&frame)) {
            if (!seat(raw.from)) return failure<void>(LobbyReason::authority, "prepared claim requires an admitted seat");
            if (!preparation || state.phase != LobbyPhase::starting || claim->revision != state.frozen_revision || claim->attempt != state.start_attempt)
                return core::Result<void>::success(); // stale attempt cannot affect this one
            if (claim->manifest != manifest_digest) return refuse_preparation("peer " + std::to_string(raw.from) + ": preparation manifest differs");
            if (!is_host) {
                if (raw.from != state.host) return failure<void>(LobbyReason::authority, "prepared release is host-only");
                const auto local = prepared_claims.find(transport->self());
                if (!prepared_offer || local == prepared_claims.end() || !same_prepared_claim(local->second, *claim))
                    return refuse_preparation("host released a different or unfinished prepared campaign");
                preparation_released = true;
                return core::Result<void>::success();
            }
            const auto [held, inserted] = prepared_claims.emplace(raw.from, *claim);
            if (!inserted && !same_prepared_claim(held->second, *claim)) return refuse_preparation("peer " + std::to_string(raw.from) + ": conflicting prepared claims");
            if (!claim->refusal.empty()) return refuse_preparation("peer " + std::to_string(raw.from) + ": " + claim->refusal);
            return core::Result<void>::success();
        }
        if (auto* envelope = std::get_if<LobbySetupFrame>(&frame)) {
            if (!initialized || !seat(raw.from)) return failure<void>(LobbyReason::authority, "unadmitted setup sender");
            if (envelope->revision > state.revision) return failure<void>(LobbyReason::stale_revision, "future setup attempt");
            if (envelope->revision < state.revision || state.phase == LobbyPhase::staging) return core::Result<void>::success();
            if (envelope->attempt != state.start_attempt) return core::Result<void>::success();
            if (!stream && notice && notice->reason == LobbyReason::setup_failed && notice->revision == envelope->revision)
                return core::Result<void>::success();
            if (!stream || envelope->revision != state.frozen_revision) return failure<void>(LobbyReason::stale_revision, "unexpected setup attempt");
            const auto found = std::find(stream->peers.begin(), stream->peers.end(), raw.from);
            if (found == stream->peers.end()) return failure<void>(LobbyReason::authority, "setup peer not in frozen roster");
            if (stream->inbox.size() >= 8192 || envelope->frame.size() > limits.queued_setup_bytes - stream->bytes)
                return failure<void>(LobbyReason::resource_limit, "setup queue exceeded bound");
            stream->bytes += envelope->frame.size();
            stream->inbox.push_back({static_cast<PeerIndex>(found - stream->peers.begin()), std::move(envelope->frame)});
            return core::Result<void>::success();
        }
        if (is_host) {
            if (!seat(raw.from)) return failure<void>(LobbyReason::authority, "unadmitted room sender");
            if (const auto* request = std::get_if<LobbyRequest>(&frame)) {
                auto applied = apply_request(raw.from, *request);
                if (!applied) {
                    if (!seat(raw.from)) return core::Result<void>::success();
                    const bool budget_exhausted = applied.error().code == lobby_diagnostic(LobbyReason::resource_limit, "").code;
                    const auto reason = budget_exhausted ? LobbyReason::resource_limit
                        : (request->revision != state.revision ? LobbyReason::stale_revision : LobbyReason::invalid_choice);
                    if (auto rejected = reject(raw.from, applied.error(), reason); !rejected) return rejected;
                    if (reason == LobbyReason::stale_revision && seat(raw.from)) return send(raw.from, state);
                    return core::Result<void>::success();
                }
                return applied;
            }
            if (const auto* request = std::get_if<LobbyChatRequest>(&frame)) {
                auto posted = post(raw.from, request->text);
                if (!posted) return seat(raw.from) ? reject(raw.from, posted.error(), LobbyReason::chat_limit) : core::Result<void>::success();
                return posted;
            }
            return failure<void>(LobbyReason::authority, "joiner sent a host-only message");
        }
        if (raw.from != state.host) return failure<void>(LobbyReason::authority, "room authority is not the host");
        if (const auto* snapshot = std::get_if<LobbySnapshot>(&frame)) {
            if (snapshot->host != state.host || (initialized && snapshot->capacity != state.capacity)
                || snapshot->revision < state.revision || snapshot->phase == LobbyPhase::closed)
                return failure<void>(LobbyReason::stale_revision, "invalid authoritative room transition");
            for (const auto& value : snapshot->seats) if (value.peer >= transport->peer_count()) return failure<void>(LobbyReason::malformed, "roster exceeds transport");
            if (std::none_of(snapshot->seats.begin(), snapshot->seats.end(), [&](const auto& value) { return value.peer == transport->self(); }))
                return failure<void>(LobbyReason::authority, "snapshot omits local admitted seat");
            if (state.phase == LobbyPhase::closed || (state.phase == LobbyPhase::committed
                && (!awaiting_snapshot || snapshot->phase != LobbyPhase::committed)))
                return failure<void>(LobbyReason::frozen, "terminal room cannot be reopened");
            if (initialized && snapshot->chat_sequence != chat_sequence && !awaiting_snapshot) return query_snapshot();
            if ((state.phase == LobbyPhase::starting || state.phase == LobbyPhase::committed) && snapshot->phase != LobbyPhase::staging) {
                auto frozen = state; frozen.phase = snapshot->phase; frozen.chat_sequence = snapshot->chat_sequence;
                if (frozen != *snapshot) return failure<void>(LobbyReason::frozen, "in-flight room choices changed");
            }
            if (auto valid = hooks.validate(*snapshot, snapshot->phase != LobbyPhase::staging); !valid) return valid;
            if (snapshot->phase != LobbyPhase::staging && nonzero(snapshot->start_attempt) && !preparation)
                return failure<void>(LobbyReason::setup_failed, "setup announcement has no agreed preparation manifest");
            if (snapshot->phase == LobbyPhase::committed && (!setup || state.frozen_revision != snapshot->frozen_revision))
                return failure<void>(LobbyReason::setup_failed, "commit without frozen setup attempt");
            const bool new_start = snapshot->phase == LobbyPhase::starting
                && (!preparation || preparation_released)
                && ((!setup && (!setup_pending || preparation.has_value())) || state.frozen_revision != snapshot->frozen_revision);
            if (snapshot->phase == LobbyPhase::staging) reset_setup();
            state = *snapshot; chat_sequence = state.chat_sequence; initialized = true; awaiting_snapshot = false;
            if (new_start) {
                if (!preparation) reset_setup();
                else if (!prepared_offer) return refuse_preparation("host announced setup before local campaign was prepared");
                if (auto created = create_setup(); !created) return fail_setup_creation(created.error());
            }
            return core::Result<void>::success();
        }
        if (const auto* chat = std::get_if<LobbyChat>(&frame)) {
            if (!initialized) return failure<void>(LobbyReason::authority, "room chat before admission");
            if (awaiting_snapshot || chat->sequence != chat_sequence + 1) return query_snapshot();
            const auto found = std::find_if(state.seats.begin(), state.seats.end(), [&](const auto& value) { return value.seat == chat->seat; });
            if (found == state.seats.end() || found->selection.name != chat->name) return failure<void>(LobbyReason::authority, "chat sender differs from room roster");
            chat_sequence = chat->sequence; state.chat_sequence = chat_sequence; retain(*chat); return core::Result<void>::success();
        }
        if (const auto* cancel = std::get_if<LobbyCancel>(&frame)) {
            if (state.phase != LobbyPhase::starting || cancel->revision != state.frozen_revision)
                return failure<void>(LobbyReason::stale_revision, "unexpected Start cancellation");
            notice = *cancel; reset_setup(); state.phase = LobbyPhase::staging; state.frozen_revision = 0; state.start_attempt = {}; clear_ready();
            return core::Result<void>::success();
        }
        if (const auto* closed = std::get_if<LobbyClosed>(&frame)) {
            notice = *closed; reset_setup(); state.phase = LobbyPhase::closed; state.frozen_revision = 0; state.start_attempt = {}; clear_ready(); return core::Result<void>::success();
        }
        if (const auto* rejected = std::get_if<LobbyRejected>(&frame)) { notice = *rejected; return core::Result<void>::success(); }
        return failure<void>(LobbyReason::authority, "host sent a request instead of authoritative room state");
    }
};

core::Result<void> LobbyRuntime::fail_setup_creation(const core::Diagnostic& error) {
    notice = LobbyNotice{state.room, state.frozen_revision, LobbyReason::setup_failed, error.message};
    // A typed setup refusal reaches the host even if local provider construction failed.
    SetupRefusal refusal{SetupReason::local_build, {}, error.message, {}, {}};
    auto encoded = encode_setup_refusal(refusal);
    if (!encoded) return core::Result<void>::failure(encoded.error());
    return send(state.host, LobbySetupFrame{state.room, state.frozen_revision, std::move(encoded).value(), state.start_attempt});
}

namespace {
core::Result<std::shared_ptr<LobbyRuntime>> make_runtime(MessageTransport& transport, std::string room, PeerIndex host,
    LobbyHooks hooks, LobbyLimits limits) {
    if (!valid_room(room) || !valid_limits(limits) || transport.self() >= transport.peer_count() || host >= transport.peer_count()
        || transport.peer_count() > 128 || !hooks.validate || !hooks.setup || !hooks.offer)
        return failure<std::shared_ptr<LobbyRuntime>>(LobbyReason::invalid_choice, "invalid room configuration");
    auto value = std::make_shared<LobbyRuntime>(); value->transport = &transport; value->state.room = std::move(room);
    value->state.host = host; value->is_host = host == transport.self(); value->hooks = std::move(hooks); value->limits = limits;
    return core::Result<std::shared_ptr<LobbyRuntime>>::success(std::move(value));
}
}
core::Result<LobbySession> LobbySession::host(MessageTransport& transport, std::string room, std::uint32_t capacity,
    LobbySelection selection, sim::tactical::PlayerId player, LobbyGame game, LobbyHooks hooks, LobbyLimits limits) {
    if (!hooks.offer || !hooks.remove_peer) return failure<LobbySession>(LobbyReason::invalid_choice, "host requires setup builder and peer removal hooks");
    auto made = make_runtime(transport, std::move(room), transport.self(), std::move(hooks), limits);
    if (!made) return core::Result<LobbySession>::failure(made.error());
    auto value = std::move(made).value(); value->state.capacity = capacity; value->state.game = std::move(game);
    value->state.mounted_mods = value->hooks.mounted_mods;
    value->state.seats.push_back({transport.self(), sim::SeatId{1}, player, clean_selection(std::move(selection)), false});
    if (auto valid = value->validate(value->state); !valid) return core::Result<LobbySession>::failure(valid.error());
    value->initialized = true; return core::Result<LobbySession>::success(LobbySession(std::move(value)));
}
core::Result<LobbySession> LobbySession::join(MessageTransport& transport, std::string room, PeerIndex host, LobbyHooks hooks, LobbyLimits limits) {
    if (host == transport.self() || transport.link(host) != LinkState::open) return failure<LobbySession>(LobbyReason::authority, "join requires an admitted host stream");
    auto made = make_runtime(transport, std::move(room), host, std::move(hooks), limits);
    if (!made) return core::Result<LobbySession>::failure(made.error());
    return core::Result<LobbySession>::success(LobbySession(std::move(made).value()));
}
core::Result<void> LobbySession::admit(PeerIndex peer, std::string identity, LobbySelection selection,
    sim::tactical::PlayerId player, std::uint64_t revision) {
    auto& value = *runtime_;
    if (auto valid = value.allowed(revision); !valid) return valid;
    if (peer >= value.transport->peer_count() || peer == value.state.host || value.seat(peer) || value.transport->link(peer) != LinkState::open)
        return failure<void>(LobbyReason::authority, "admission requires a vacant admitted stream");
    if (identity.empty() || identity.size() > 256 || value.denied.contains(identity)) return failure<void>(LobbyReason::denied, "admitted identity is denied in this room");
    for (const auto& entry : value.identities) if (entry.second == identity) return failure<void>(LobbyReason::denied, "identity already has a seat");
    if (value.state.seats.size() >= value.state.capacity) return failure<void>(LobbyReason::full, "room is full");
    if (value.next_seat == std::numeric_limits<std::uint32_t>::max()) return failure<void>(LobbyReason::resource_limit, "stable seat IDs exhausted");
    auto candidate = value.state;
    candidate.seats.push_back({peer, sim::SeatId{value.next_seat}, player, clean_selection(std::move(selection)), false});
    for (auto& seat : candidate.seats) seat.ready = false;
    if (auto valid = value.validate(candidate); !valid) return valid;
    if (auto advanced = value.advance(); !advanced) return advanced;
    candidate.revision = value.state.revision; value.state = std::move(candidate); ++value.next_seat; value.identities.emplace(peer, std::move(identity));
    return value.broadcast(value.state);
}
core::Result<void> LobbySession::assign(PeerIndex peer, LobbySelection selection, sim::tactical::PlayerId player, std::uint64_t revision) {
    auto& value = *runtime_; if (auto valid = value.allowed(revision); !valid) return valid;
    return value.edit(peer, std::move(selection), player);
}
core::Result<void> LobbySession::configure_game(LobbyGame game, std::vector<LobbySeat> assignments, std::uint64_t revision) {
    auto& value = *runtime_; if (auto valid = value.allowed(revision); !valid) return valid;
    if (assignments.size() != value.state.seats.size()) return failure<void>(LobbyReason::authority, "configuration cannot change room membership");
    auto candidate = value.state; candidate.game = std::move(game);
    bool consequential = candidate.game != value.state.game;
    for (std::size_t i = 0; i < assignments.size(); ++i) {
        const auto& previous = value.state.seats[i]; auto& next = assignments[i];
        if (next.peer != previous.peer || next.seat != previous.seat)
            return failure<void>(LobbyReason::authority, "configuration cannot change admitted identities");
        next.selection = clean_selection(std::move(next.selection)); next.ready = previous.ready;
        auto old = previous.selection; old.name = next.selection.name;
        consequential = consequential || old != next.selection || previous.player != next.player;
    }
    candidate.seats = std::move(assignments);
    if (consequential) for (auto& seat : candidate.seats) seat.ready = false;
    if (auto valid = value.validate(candidate); !valid) return valid;
    if (candidate == value.state) return core::Result<void>::success();
    if (auto advanced = value.advance(); !advanced) return advanced;
    candidate.revision = value.state.revision; value.state = std::move(candidate); return value.broadcast(value.state);
}
core::Result<void> LobbySession::set_game(LobbyGame game, std::uint64_t revision) {
    auto& value = *runtime_; if (auto valid = value.allowed(revision); !valid) return valid;
    if (game == value.state.game) return core::Result<void>::success();
    auto candidate = value.state; candidate.game = std::move(game); for (auto& seat : candidate.seats) seat.ready = false;
    if (auto valid = value.validate(candidate); !valid) return valid;
    if (auto advanced = value.advance(); !advanced) return advanced;
    candidate.revision = value.state.revision; value.state = std::move(candidate); return value.broadcast(value.state);
}
core::Result<void> LobbySession::request(LobbyAction action, std::uint64_t revision, LobbySelection selection) {
    auto& value = *runtime_;
    if (action != LobbyAction::snapshot) {
        if (auto valid = value.allowed(revision, false, action == LobbyAction::unready || action == LobbyAction::leave); !valid) return valid;
    } else if (!value.initialized || value.taken || value.state.phase == LobbyPhase::closed)
        return failure<void>(LobbyReason::frozen, "room snapshot unavailable");
    LobbyRequest request{value.state.room, revision, action, clean_selection(std::move(selection))};
    auto encoded = encode_lobby_frame(request); if (!encoded) return core::Result<void>::failure(encoded.error());
    return value.is_host ? value.apply_request(value.transport->self(), request) : value.transport->send(value.state.host, encoded.value());
}
core::Result<void> LobbySession::chat(std::string text) {
    auto& value = *runtime_;
    if (!value.initialized || value.taken || value.state.phase == LobbyPhase::closed || value.state.phase == LobbyPhase::committed)
        return failure<void>(LobbyReason::frozen, "room chat unavailable");
    text = sanitize_lobby_text(text, chat_limit);
    return value.is_host ? value.post(value.transport->self(), std::move(text)) : value.send(value.state.host, LobbyChatRequest{value.state.room, std::move(text)});
}
core::Result<void> LobbySession::kick(PeerIndex peer, std::uint64_t revision) {
    auto& value = *runtime_; if (auto valid = value.allowed(revision, true, true); !valid) return valid;
    return value.remove(peer, LobbyReason::kicked, true);
}
core::Result<void> LobbySession::refuse_unreachable(PeerIndex newcomer, PeerIndex unreachable, bool relay_configured) {
    auto& value = *runtime_;
    if (!value.is_host) return failure<void>(LobbyReason::authority, "only the host refuses a newcomer");
    if (value.taken || value.state.phase == LobbyPhase::committed || value.state.phase == LobbyPhase::closed)
        return failure<void>(LobbyReason::frozen, "room controls are locked");
    if (newcomer == value.state.host || newcomer == unreachable) return failure<void>(LobbyReason::authority, "cannot refuse this seat");
    const auto* other = value.seat(unreachable);
    const auto* guest = value.seat(newcomer);
    const std::string other_name = other ? other->selection.name : "another player";
    const std::string guest_name = guest ? guest->selection.name : "A player";
    const auto line = guest_name + " (Seat " + std::to_string(newcomer + 1) + ") could not connect to "
        + other_name + " (Seat " + std::to_string(unreachable + 1) + ") and was not admitted.";
    value.notice = LobbyNotice{value.state.room, value.state.revision, LobbyReason::pair_unreachable,
        sanitize_lobby_text(line, 400) + " " + lobby_diagnostic(LobbyReason::pair_unreachable, "").code};
    // The room hears it once, as a line from the host's seat; the newcomer hears why through its closed notice.
    auto posted = value.post(value.state.host, line, true);
    if (!guest) return posted;
    auto removed = value.remove(newcomer, LobbyReason::pair_unreachable, false, mesh_newcomer_refusal_text(other_name, relay_configured));
    return posted ? removed : posted;
}
core::Result<void> LobbySession::close() {
    auto& value = *runtime_; if (auto valid = value.allowed(value.state.revision, true, true); !valid) return valid;
    return value.terminal(LobbyReason::host_closed, "host closed the room");
}
core::Result<void> LobbySession::start(std::uint64_t revision) {
    auto& value = *runtime_; if (auto valid = value.allowed(revision); !valid) return valid;
    return value.begin_start(revision);
}
bool LobbySession::start_pending() const { return runtime_->start_pending.has_value(); }
bool LobbySession::setup_pending() const { return runtime_->setup_pending; }
core::Result<void> LobbySession::cancel_start() {
    auto& value = *runtime_;
    if (!value.is_host || !value.start_pending) return failure<void>(LobbyReason::frozen, "no Start is pending");
    return value.preparation ? value.abort(LobbyReason::left, "the host cancelled it")
        : value.cancel_pending(LobbyReason::left, "the host cancelled it");
}
// Everything Start does once the host may start: the checks, the offer and, when the offer is ready, publication. With an
// asynchronous offer hook (#6045) it ends at the offer with `start_pending` set; `resume_start` runs it again from `pump`.
core::Result<void> LobbyRuntime::begin_start(const std::uint64_t revision) {
    auto& value = *this;
    if (auto valid = value.validate(value.state, true); !valid) return valid;
    if (value.state.seats.size() < 2 || !std::all_of(value.state.seats.begin(), value.state.seats.end(), [](const auto& seat) { return seat.ready; }))
        return failure<void>(LobbyReason::not_ready, "every admitted seat, including host, must be ready");
    for (const auto& seat : value.state.seats) if (seat.peer != value.transport->self() && value.transport->link(seat.peer) != LinkState::open)
        return failure<void>(LobbyReason::link_closed, "Start requires connected seats");
    // Online room mesh: every agreed seat present and every pair verified, not only the host's own pairs.
    if (value.hooks.mesh_complete)
        if (auto complete = value.hooks.mesh_complete(); !complete)
            return failure<void>(LobbyReason::mesh_incomplete, "Every player must be connected to every other player before the game can start.");
    std::vector<SetupSeat> roster;
    for (const auto& seat : value.state.seats) roster.push_back({static_cast<PeerIndex>(roster.size()), seat.seat, seat.player});
    if (value.hooks.bind_setup_seats) {
        auto bound = value.hooks.bind_setup_seats(value.state, roster);
        if (!bound) return core::Result<void>::failure(bound.error());
        roster = std::move(bound).value();
    }
    auto frozen = value.state; frozen.phase = LobbyPhase::starting; frozen.frozen_revision = frozen.revision;
    if (!value.preparation && value.hooks.preparation_inputs) {
        auto inputs = value.hooks.preparation_inputs(frozen, roster);
        if (!inputs) return core::Result<void>::failure(inputs.error());
        if (!inputs.value().empty()) {
            if (!value.hooks.preparation_domains) return failure<void>(LobbyReason::setup_failed, "missing prepared campaign identity provider");
            auto nonce = make_rejoin_nonce();
            if (!nonce) return core::Result<void>::failure(nonce.error());
            frozen.start_attempt = nonce.value();
            auto encoded_snapshot = encode_lobby_frame(frozen);
            if (!encoded_snapshot) return core::Result<void>::failure(encoded_snapshot.error());
            LobbyPrepare manifest{frozen.room, frozen.revision, std::move(encoded_snapshot).value(), std::move(inputs).value()};
            if (value.hooks.preparation_mounts) {
                auto layers = value.hooks.preparation_mounts(frozen, nullptr);
                if (!layers) return core::Result<void>::failure(layers.error());
                manifest.layers = std::move(layers).value();
            }
            auto encoded_manifest = encode_lobby_frame(manifest);
            if (!encoded_manifest) return core::Result<void>::failure(encoded_manifest.error());
            // Capacity TCP may have irreversibly frozen its roster even if Lock confirmation is still pending.
            if (value.hooks.lock_at_start && value.hooks.lock_room_progress) {
                auto locked = value.hooks.lock_room_progress();
                if (!locked) {
                    if (locked.error().code != "EAWR-NET-1774") static_cast<void>(value.terminal(LobbyReason::setup_failed, locked.error().message));
                    return core::Result<void>::failure(locked.error());
                }
                value.roster_lock_pending = true;
            }
            value.state = std::move(frozen); value.preparation = std::move(manifest);
            value.manifest_digest = manifest_hash(encoded_manifest.value()); value.preparation_seats = std::move(roster);
            // value_or(0) never applies: a seat becomes ready only inside pump (guest requests arrive through receive),
            // after pump has set now, and begin_start refuses until every seat is ready.
            value.preparation_since = value.now.value_or(0); value.start_pending = revision;
            if (auto sent = value.broadcast(*value.preparation); !sent) return sent;
            return value.poll_preparation();
        }
    }
    if (value.preparation) roster = value.preparation_seats;
    auto offer = value.prepared_offer ? core::Result<SetupOffer>::success(*value.prepared_offer) : value.hooks.offer(frozen, roster);
    if (!offer) {
        auto error = offer.error();
        if (is_start_pending(error)) {
            if (!value.start_pending) {
                value.start_pending = revision;
                static_cast<void>(value.post(value.state.host,
                    "The host is preparing the campaign for all players. The room is locked until that is done.", true));
            }
            return core::Result<void>::success();
        }
        if (error.code == "EAWR-NET-1022") {
            // A later key-row send may fail after earlier guests received theirs.
            // A fresh staging revision clears that partial attempt everywhere,
            // without accepting duplicate credentials or closing the host room.
            if (auto advanced = value.advance(); !advanced) return advanced;
            value.clear_ready();
            error.message = "Connections are not ready. Everyone must accept the setup again before Start.";
            value.notice = LobbyNotice{value.state.room, value.state.revision, LobbyReason::setup_failed, error.message};
            if (auto sent = value.broadcast(value.state); !sent) {
                auto send_error = sent.error(); send_error.message += " Everyone must accept the setup again before Start.";
                return core::Result<void>::failure(std::move(send_error));
            }
        }
        return core::Result<void>::failure(std::move(error));
    }
    if (offer.value().seats != roster) return failure<void>(LobbyReason::setup_failed, "builder changed the frozen seat mapping");
    // Preflight setup capacity and loaded owner/team bindings before publishing Start.
    auto encoded = encode_setup_offer(offer.value());
    if (!encoded) return core::Result<void>::failure(encoded.error());
    if (auto wrapped = encode_lobby_frame(LobbySetupFrame{frozen.room, frozen.revision, encoded.value()}); !wrapped)
        return core::Result<void>::failure(wrapped.error());
    // The preparation branch already took the lock before broadcasting its manifest; the room stays locked until
    // the attempt ends, so do not ask the service for it a second time after unanimous claims.
    if (value.hooks.lock_at_start && value.hooks.lock_room_progress && !(value.preparation && value.roster_lock_pending)) {
        auto locked = value.hooks.lock_room_progress();
        if (!locked) {
            // A pending Start stays pending here: resume_start turns the refusal into the host's notice (terminal() clears it itself).
            if (locked.error().code != "EAWR-NET-1774")
                static_cast<void>(value.terminal(LobbyReason::setup_failed, locked.error().message));
            return core::Result<void>::failure(locked.error());
        }
        // From here cancellation cannot reopen a transport whose roster froze.
        value.roster_lock_pending = true;
    }
    value.start_pending.reset();
    value.state = std::move(frozen);
    value.stream = std::make_unique<SetupStream>(); value.stream->outer = value.transport;
    value.stream->room = value.state.room; value.stream->revision = value.state.frozen_revision;
    value.stream->attempt = value.state.start_attempt;
    for (const auto& seat : value.state.seats) value.stream->peers.push_back(seat.peer);
    if (auto sent = value.broadcast(value.state); !sent) {
        if (value.state.phase == LobbyPhase::starting) static_cast<void>(value.abort(LobbyReason::link_closed, sent.error().message));
        return sent;
    }
    auto setup_context = value.hooks.setup(*value.stream, value.state);
    if (!setup_context) { auto restored = value.abort(LobbyReason::setup_failed, setup_context.error().message); return restored ? core::Result<void>::failure(setup_context.error()) : restored; }
    value.context.emplace(std::move(setup_context).value()); value.context->options.auto_accept = false; value.context->options.auto_start = false;
    auto session = SetupSession::host(*value.stream, std::move(offer).value(), value.context->providers, value.context->options);
    if (!session) { auto restored = value.abort(LobbyReason::setup_failed, session.error().message); return restored ? core::Result<void>::failure(session.error()) : restored; }
    value.setup.emplace(std::move(session).value()); return value.setup->start();
}
// A pending Start asks the offer hook again. Anything that changed the room since Start was pressed cancels it: the hook's
// worker keeps running but nothing is published for a room that no longer matches.
core::Result<void> LobbyRuntime::resume_start() {
    if (preparation) return poll_preparation();
    const auto revision = *start_pending;
    if (state.phase != LobbyPhase::staging || state.revision != revision)
        return cancel_pending(LobbyReason::stale_revision, "the room changed during the preparation");
    auto resumed = begin_start(revision);
    if (resumed) return resumed;
    // A refusal now (a seat is not ready, a link closed, the room failed validation) ends the wait, not the room.
    const bool noticed = state.revision != revision; // the hook's own failure path already told the room
    auto error = resumed.error();
    if (!noticed) return cancel_pending(LobbyReason::setup_failed, error.message);
    start_pending.reset();
    return core::Result<void>::success();
}
core::Result<void> LobbyRuntime::poll_preparation() {
    if (!preparation || preparation_failed || setup) return core::Result<void>::success();
    if (now.value_or(0) - preparation_since >= limits.preparation_timeout_ms)
        return refuse_preparation("prepared campaign agreement timed out");
    if (!prepared_offer) {
        // Chat cursors may advance while preparation runs. The worker always sees the published immutable inputs.
        const auto frozen = std::get<LobbySnapshot>(decode_lobby_frame(preparation->snapshot).value());
        auto built = hooks.offer(frozen, preparation_seats);
        if (!built) {
            if (is_start_pending(built.error())) return core::Result<void>::success();
            return refuse_preparation(built.error().message);
        }
        if (built.value().seats != preparation_seats) return refuse_preparation("builder changed the frozen seat mapping");
        auto bytes = encode_setup_offer(built.value());
        if (!bytes) return refuse_preparation(bytes.error().message);
        auto domains = hooks.preparation_domains(built.value());
        if (!domains) return refuse_preparation(domains.error().message);
        LobbyPrepared claim;
        claim.room = state.room; claim.revision = state.frozen_revision; claim.attempt = state.start_attempt;
        claim.manifest = manifest_digest; claim.offer = core::sha256(bytes.value()); claim.domains = std::move(domains).value();
        if (hooks.preparation_mounts) {
            auto layers = hooks.preparation_mounts(frozen, &built.value());
            if (!layers) return refuse_preparation(layers.error().message);
            claim.layers = std::move(layers).value();
        }
        auto encoded = encode_lobby_frame(claim);
        if (!encoded) return refuse_preparation(encoded.error().message);
        prepared_offer = std::move(built).value();
        prepared_claims.emplace(transport->self(), claim);
        if (!is_host) return send(state.host, claim);
    }
    if (!is_host) return core::Result<void>::success();
    const auto& local = prepared_claims.at(state.host);
    for (const auto& [peer, claim] : prepared_claims) {
        if (const auto different = layer_difference(local.layers, claim.layers); !different.empty())
            return refuse_preparation("peer " + std::to_string(peer) + ": " + different);
        sim::MatchIdentity expected, actual;
        expected.domains = local.domains; actual.domains = claim.domains;
        expected.sha256 = core::sha256(sim::canonical_match_identity(expected)); actual.sha256 = core::sha256(sim::canonical_match_identity(actual));
        if (const auto mismatch = sim::describe_mismatch(expected, actual))
            return refuse_preparation("peer " + std::to_string(peer) + ": " + mismatch->message);
        if (claim.offer != local.offer) return refuse_preparation("peer " + std::to_string(peer) + ": sealed offer differs");
    }
    if (prepared_claims.size() != state.seats.size()) return core::Result<void>::success();
    for (const auto& value : state.seats) if (!prepared_claims.contains(value.peer)) return core::Result<void>::success();
    if (!preparation_released) {
        // Snapshot queries during preparation may return the frozen starting phase. Only this unanimous
        // release allows guests to interpret the following snapshot as the existing setup announcement.
        preparation_released = true;
        if (auto sent = broadcast(local); !sent) return sent;
    }
    return begin_start(state.revision);
}
core::Result<void> LobbySession::accept_setup() {
    auto& value = *runtime_;
    if (value.is_host || value.state.phase != LobbyPhase::starting || !value.setup)
        return failure<void>(LobbyReason::frozen, "no joiner setup awaiting explicit acceptance");
    return value.setup->accept();
}
core::Result<void> LobbySession::pump(std::uint64_t now) {
    auto& value = *runtime_;
    if (value.taken) return failure<void>(LobbyReason::frozen, "game owns the room transport");
    if (value.now && now < *value.now) return failure<void>(LobbyReason::invalid_choice, "room clock moved backwards");
    value.now = now;
    if (value.state.phase == LobbyPhase::closed) return core::Result<void>::success();
    if (!value.is_host && value.transport->link(value.state.host) == LinkState::closed) {
        value.notice = LobbyNotice{value.state.room, value.state.revision, LobbyReason::host_closed, "host connection lost"};
        value.reset_setup(); value.state.phase = LobbyPhase::closed; value.state.frozen_revision = 0; value.state.start_attempt = {}; value.clear_ready(); return core::Result<void>::success();
    }
    if (value.is_host && value.state.phase != LobbyPhase::committed) {
        std::vector<PeerIndex> lost;
        for (const auto& seat : value.state.seats) if (seat.peer != value.state.host && value.transport->link(seat.peer) == LinkState::closed) lost.push_back(seat.peer);
        // Remove the whole observed loss batch before publishing a new roster.
        // Publishing after the first removal would send to the other closed
        // streams and report a fatal broadcast error for an ordinary vacancy.
        for (const auto peer : lost) if (auto removed = value.drop(peer, LobbyReason::link_closed, false); !removed) return removed;
        if (!lost.empty()) {
            if (value.state.phase == LobbyPhase::starting) {
                if (auto aborted = value.abort(LobbyReason::link_closed, "seat removed during Start"); !aborted) return aborted;
            } else {
                if (auto advanced = value.advance(); !advanced) return advanced;
                if (auto sent = value.broadcast(value.state); !sent) return sent;
            }
        }
    }
    if (value.hooks.application_pump && value.hooks.application_pump(*value.transport, value.state, now)
        && value.is_host && value.state.phase == LobbyPhase::staging) {
        if (auto advanced = value.advance(); !advanced) return advanced;
        value.clear_ready();
        if (auto sent = value.broadcast(value.state); !sent) return sent;
    }
    // Drain a bounded batch before setup can publish commitment. If the batch
    // fills, defer setup/commit to the next pump so queued Unready/Leave wins.
    std::size_t count{};
    for (; count < value.limits.frames_per_pump; ++count) {
        auto raw = value.transport->poll(); if (!raw) break;
        if (value.hooks.application_frame && value.hooks.application_frame(*value.transport, value.state, *raw, now)) continue;
        const auto sender = raw->from;
        const auto count_drop = [&] { if (value.dropped != std::numeric_limits<std::uint64_t>::max()) ++value.dropped; };
        if (!value.is_host && sender != value.state.host && (raw->frame.empty() || raw->frame.front() != lobby_setup_kind)) {
            count_drop(); continue;
        }
        if (auto received = value.receive(std::move(*raw)); !received) {
            if (!value.is_host) {
                if (sender != value.state.host) { count_drop(); continue; }
                return received;
            }
            if (value.seat(sender) && sender != value.transport->self()) {
                if (auto rejected = value.reject(sender, received.error(), LobbyReason::malformed); !rejected) return rejected;
            }
        }
        if (value.state.phase == LobbyPhase::closed) return core::Result<void>::success();
    }
    if (count == value.limits.frames_per_pump) return core::Result<void>::success();
    if (!value.is_host && value.awaiting_snapshot) {
        if (auto queried = value.query_snapshot(); !queried) return queried;
    }
    if (value.is_host && value.start_pending) {
        if (auto resumed = value.resume_start(); !resumed) return resumed;
    }
    if (!value.is_host && value.preparation && !value.setup) {
        if (auto prepared = value.poll_preparation(); !prepared) return prepared;
    } else if (!value.is_host && value.setup_pending) {
        if (auto created = value.create_setup(); !created) return value.fail_setup_creation(created.error());
    }
    if (value.setup) {
        if (auto pumped = value.setup->pump(now); !pumped) {
            if (value.is_host) {
                if (value.state.phase == LobbyPhase::committed) {
                    static_cast<void>(value.terminal(LobbyReason::setup_failed, pumped.error().message));
                    return pumped;
                }
                return value.abort(LobbyReason::setup_failed, pumped.error().message);
            }
            // A committed room has no abort to wait for: the guest's own barrier failed (timeout, closed link), so surface the typed code.
            if (value.state.phase == LobbyPhase::committed) return pumped;
            value.notice = LobbyNotice{value.state.room, value.state.frozen_revision, LobbyReason::setup_failed, pumped.error().message};
            return core::Result<void>::success(); // Host serializes the abort/new revision.
        }
        if (value.is_host && value.state.phase == LobbyPhase::starting && value.setup->phase() == SetupPhase::ready) {
            if (value.hooks.lock_room_progress) {
                auto locked = value.hooks.lock_room_progress();
                if (!locked) {
                    // Only the service's explicit compare refusal proves it stayed open.
                    if (locked.error().code == "EAWR-NET-1774") value.roster_lock_pending = false;
                    return value.abort(LobbyReason::mesh_incomplete, locked.error().message);
                }
                value.roster_lock_pending = !locked.value();
                if (value.roster_lock_pending) return core::Result<void>::success();
            }
            if (value.hooks.lock_room)
                if (auto locked = value.hooks.lock_room(); !locked)
                    return value.abort(LobbyReason::mesh_incomplete, "The room could not be locked for Start; every player must be connected to every other player.");
            auto committed = value.state; committed.phase = LobbyPhase::committed;
            if (auto sent = value.broadcast(committed); !sent) return sent;
            value.state = std::move(committed); return core::Result<void>::success();
        }
    }
    return core::Result<void>::success();
}
bool LobbySession::result_ready() const {
    const auto& value = *runtime_;
    return !value.taken && value.state.phase == LobbyPhase::committed && value.setup && value.setup->phase() == SetupPhase::ready;
}
core::Result<LobbyResult> LobbySession::take_result() {
    auto& value = *runtime_;
    if (value.taken || value.state.phase != LobbyPhase::committed || !value.setup)
        return failure<LobbyResult>(LobbyReason::frozen, "Start is not committed");
    auto result = value.setup->take_result(); if (!result) return core::Result<LobbyResult>::failure(result.error());
    value.taken = true; value.stream->transferred = true;
    return core::Result<LobbyResult>::success(LobbyResult(runtime_, std::move(result).value()));
}
const LobbySnapshot& LobbySession::snapshot() const { return runtime_->state; }
const std::deque<LobbyChat>& LobbySession::chat_history() const { return runtime_->history; }
const std::optional<LobbyNotice>& LobbySession::notice() const { return runtime_->notice; }
std::optional<SetupPhase> LobbySession::setup_phase() const { return runtime_->setup ? std::optional{runtime_->setup->phase()} : std::nullopt; }
std::uint64_t LobbySession::dropped_frames() const { return runtime_->dropped; }
const LobbySnapshot& LobbyResult::snapshot() const { return lifetime_->state; }
MessageTransport& LobbyResult::transport() { return *lifetime_->stream; }

} // namespace eawr::net
