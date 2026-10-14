#include "eawr/net/lobby_session.hpp"
#include "eawr/net/lan_directory.hpp"

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
        && limits.chat_window_ms > 0 && limits.denied_identities > 0 && limits.denied_identities <= 4096;
}
}

core::Diagnostic lobby_diagnostic(LobbyReason reason, std::string message) {
    core::Diagnostic value;
    value.code = "EAWR-NET-" + std::to_string(1600U + static_cast<unsigned>(reason));
    value.message = sanitize_lobby_text(message, 512);
    return value;
}
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
        out.u8(static_cast<std::uint8_t>(lobby_snapshot_kind + frame.index()));
        out.u32(1); out.text(room, 64);
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
                out.u64(value.revision); out.blob(value.frame, max_lobby_payload_bytes);
            } else {
                if (!value.revision || static_cast<unsigned>(value.reason) > static_cast<unsigned>(LobbyReason::recovery_unavailable))
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
        if (in.u32() != 1) throw std::runtime_error("unsupported room protocol version");
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
            if (value.revision == 0 || value.frame.empty()) throw std::runtime_error("invalid setup envelope");
            result = std::move(value); break;
        }
        case lobby_cancel_kind: case lobby_closed_kind: case lobby_rejected_kind: {
            LobbyNotice value;
            value.room = room; value.revision = in.u64(); const auto reason = in.u8(); value.message = in.text(512);
            if (!value.revision || reason > static_cast<unsigned>(LobbyReason::recovery_unavailable)) throw std::runtime_error("invalid room notice");
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
    std::vector<PeerIndex> peers;
    std::deque<Received> inbox;
    std::size_t bytes{};
    bool transferred{};
    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) override {
        if (to >= peers.size()) return failure<void>(LobbyReason::authority, "invalid setup destination");
        if (frame.size() > max_lobby_payload_bytes) return failure<void>(LobbyReason::resource_limit, "setup/game frame exceeds envelope bound");
        auto encoded = encode_lobby_frame(LobbySetupFrame{room, revision, Bytes(frame.begin(), frame.end())});
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
                if (frame && frame->room == room && frame->revision == revision && found != peers.end())
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
    bool awaiting_snapshot{};
    std::uint64_t snapshot_query_time{}, dropped{};
    std::unique_ptr<SetupStream> stream;
    std::optional<LobbySetupContext> context;
    std::optional<SetupSession> setup;

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
        return core::Result<void>::success();
    }
    void reset_setup() { setup.reset(); context.reset(); stream.reset(); }
    core::Result<void> advance() {
        if (state.revision == std::numeric_limits<std::uint64_t>::max())
            return failure<void>(LobbyReason::resource_limit, "room revision exhausted");
        ++state.revision; return core::Result<void>::success();
    }
    void clear_ready() { for (auto& value : state.seats) value.ready = false; }
    core::Result<void> terminal(LobbyReason reason, std::string message) {
        reset_setup(); state.phase = LobbyPhase::closed; state.frozen_revision = 0; clear_ready();
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
        const auto frozen = state.frozen_revision;
        reset_setup(); state.phase = LobbyPhase::staging; state.frozen_revision = 0; clear_ready();
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
        auto old_selection = previous->selection; old_selection.name = updated.selection.name;
        const bool consequential = old_selection != updated.selection || previous->player != player;
        if (consequential) for (auto& value : candidate.seats) value.ready = false;
        if (auto valid = validate(candidate); !valid) return valid;
        if (auto advanced = advance(); !advanced) return advanced;
        candidate.revision = state.revision; state = std::move(candidate); return broadcast(state);
    }
    core::Result<void> drop(PeerIndex peer, LobbyReason reason, bool deny) {
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
        static_cast<void>(send(peer, LobbyClosed{{state.room, state.revision, reason, "seat removed from room"}}));
        return core::Result<void>::success();
    }
    core::Result<void> remove(PeerIndex peer, LobbyReason reason, bool deny) {
        const bool starting = state.phase == LobbyPhase::starting;
        if (auto removed = drop(peer, reason, deny); !removed) return removed;
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
            if (local->ready) return core::Result<void>::success();
            local->ready = true; return broadcast(state);
        case LobbyAction::unready:
            if (state.phase == LobbyPhase::starting) return abort(LobbyReason::not_ready, "player withdrew readiness during Start");
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
    core::Result<void> post(PeerIndex peer, std::string text) {
        if (state.phase == LobbyPhase::closed || state.phase == LobbyPhase::committed)
            return failure<void>(LobbyReason::frozen, "room chat is closed");
        const auto* sender = seat(peer);
        if (!sender) return failure<void>(LobbyReason::authority, "chat sender has no admitted seat");
        text = sanitize_lobby_text(text, chat_limit);
        if (text.empty() || std::all_of(text.begin(), text.end(), [](char c) { return c == ' '; }))
            return failure<void>(LobbyReason::invalid_choice, "empty room chat");
        if (!budget(chat_times, peer)) return failure<void>(LobbyReason::chat_limit, "room chat rate exceeded");
        if (chat_sequence == std::numeric_limits<std::uint64_t>::max()) return failure<void>(LobbyReason::resource_limit, "room chat sequence exhausted");
        LobbyChat value{state.room, ++chat_sequence, sender->seat, sender->selection.name, std::move(text)};
        state.chat_sequence = chat_sequence;
        retain(value); return broadcast(value);
    }
    core::Result<void> create_setup() {
        stream = std::make_unique<SetupStream>(); stream->outer = transport; stream->room = state.room; stream->revision = state.frozen_revision;
        for (const auto& value : state.seats) stream->peers.push_back(value.peer);
        auto created = hooks.setup(*stream, state);
        if (!created) { reset_setup(); return core::Result<void>::failure(created.error()); }
        context.emplace(std::move(created).value()); context->options.auto_accept = false; context->options.auto_start = false;
        std::vector<SetupSeat> roster;
        for (const auto& value : state.seats) roster.push_back({static_cast<PeerIndex>(roster.size()), value.seat, value.player});
        auto expected = hooks.offer(state, roster);
        if (!expected) { reset_setup(); return core::Result<void>::failure(expected.error()); }
        auto identify = context->providers.identify;
        if (!identify) { reset_setup(); return failure<void>(LobbyReason::setup_failed, "missing local content identifier"); }
        context->providers.identify = [identify = std::move(identify), expected = std::move(expected).value()](const SetupOffer& offered) {
            if (offered.seed != expected.seed || offered.domain != expected.domain || offered.setup != expected.setup
                || offered.seats != expected.seats || offered.input_delay != expected.input_delay || offered.final_tick_count != expected.final_tick_count)
                return failure<sim::MatchAgreement>(LobbyReason::setup_failed, "host offer differs from frozen room choices");
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
        if (!decoded) return core::Result<void>::failure(decoded.error());
        auto& frame = decoded.value();
        if (room_of(frame) != state.room) return failure<void>(LobbyReason::authority, "message belongs to another room");
        if (auto* envelope = std::get_if<LobbySetupFrame>(&frame)) {
            if (!initialized || !seat(raw.from)) return failure<void>(LobbyReason::authority, "unadmitted setup sender");
            if (envelope->revision > state.revision) return failure<void>(LobbyReason::stale_revision, "future setup attempt");
            if (envelope->revision < state.revision || state.phase == LobbyPhase::staging) return core::Result<void>::success();
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
            if (snapshot->phase == LobbyPhase::committed && (!setup || state.frozen_revision != snapshot->frozen_revision))
                return failure<void>(LobbyReason::setup_failed, "commit without frozen setup attempt");
            const bool new_start = snapshot->phase == LobbyPhase::starting && (!setup || state.frozen_revision != snapshot->frozen_revision);
            if (snapshot->phase == LobbyPhase::staging) reset_setup();
            state = *snapshot; chat_sequence = state.chat_sequence; initialized = true; awaiting_snapshot = false;
            if (new_start) {
                reset_setup();
                if (auto created = create_setup(); !created) {
                    notice = LobbyNotice{state.room, state.frozen_revision, LobbyReason::setup_failed, created.error().message};
                    // A typed setup refusal reaches the host even if local provider construction failed.
                    SetupRefusal refusal{SetupReason::local_build, {}, created.error().message, {}, {}};
                    auto encoded = encode_setup_refusal(refusal);
                    if (!encoded) return core::Result<void>::failure(encoded.error());
                    return send(state.host, LobbySetupFrame{state.room, state.frozen_revision, std::move(encoded).value()});
                }
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
            notice = *cancel; reset_setup(); state.phase = LobbyPhase::staging; state.frozen_revision = 0; clear_ready();
            return core::Result<void>::success();
        }
        if (const auto* closed = std::get_if<LobbyClosed>(&frame)) {
            notice = *closed; reset_setup(); state.phase = LobbyPhase::closed; state.frozen_revision = 0; clear_ready(); return core::Result<void>::success();
        }
        if (const auto* rejected = std::get_if<LobbyRejected>(&frame)) { notice = *rejected; return core::Result<void>::success(); }
        return failure<void>(LobbyReason::authority, "host sent a request instead of authoritative room state");
    }
};

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
core::Result<void> LobbySession::close() {
    auto& value = *runtime_; if (auto valid = value.allowed(value.state.revision, true, true); !valid) return valid;
    return value.terminal(LobbyReason::host_closed, "host closed the room");
}
core::Result<void> LobbySession::start(std::uint64_t revision) {
    auto& value = *runtime_; if (auto valid = value.allowed(revision); !valid) return valid;
    if (value.state.seats.size() < 2 || !std::all_of(value.state.seats.begin(), value.state.seats.end(), [](const auto& seat) { return seat.ready; }))
        return failure<void>(LobbyReason::not_ready, "every admitted seat, including host, must be ready");
    for (const auto& seat : value.state.seats) if (seat.peer != value.transport->self() && value.transport->link(seat.peer) != LinkState::open)
        return failure<void>(LobbyReason::link_closed, "Start requires connected seats");
    if (auto valid = value.validate(value.state, true); !valid) return valid;
    std::vector<SetupSeat> roster;
    for (const auto& seat : value.state.seats) roster.push_back({static_cast<PeerIndex>(roster.size()), seat.seat, seat.player});
    auto frozen = value.state; frozen.phase = LobbyPhase::starting; frozen.frozen_revision = frozen.revision;
    auto offer = value.hooks.offer(frozen, roster);
    if (!offer) return core::Result<void>::failure(offer.error());
    if (offer.value().seats != roster) return failure<void>(LobbyReason::setup_failed, "builder changed the frozen seat mapping");
    // Preflight setup capacity and loaded owner/team bindings before publishing Start.
    auto encoded = encode_setup_offer(offer.value());
    if (!encoded) return core::Result<void>::failure(encoded.error());
    if (auto wrapped = encode_lobby_frame(LobbySetupFrame{frozen.room, frozen.revision, encoded.value()}); !wrapped)
        return core::Result<void>::failure(wrapped.error());
    value.state = std::move(frozen);
    value.stream = std::make_unique<SetupStream>(); value.stream->outer = value.transport;
    value.stream->room = value.state.room; value.stream->revision = value.state.frozen_revision;
    for (const auto& seat : value.state.seats) value.stream->peers.push_back(seat.peer);
    if (auto sent = value.broadcast(value.state); !sent) {
        if (value.state.phase == LobbyPhase::starting) static_cast<void>(value.abort(LobbyReason::link_closed, sent.error().message));
        return sent;
    }
    auto context = value.hooks.setup(*value.stream, value.state);
    if (!context) { auto restored = value.abort(LobbyReason::setup_failed, context.error().message); return restored ? core::Result<void>::failure(context.error()) : restored; }
    value.context.emplace(std::move(context).value()); value.context->options.auto_accept = false; value.context->options.auto_start = false;
    auto session = SetupSession::host(*value.stream, std::move(offer).value(), value.context->providers, value.context->options);
    if (!session) { auto restored = value.abort(LobbyReason::setup_failed, session.error().message); return restored ? core::Result<void>::failure(session.error()) : restored; }
    value.setup.emplace(std::move(session).value()); return value.setup->start();
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
        value.reset_setup(); value.state.phase = LobbyPhase::closed; value.state.frozen_revision = 0; value.clear_ready(); return core::Result<void>::success();
    }
    if (value.is_host && value.state.phase != LobbyPhase::committed) {
        std::vector<PeerIndex> lost;
        for (const auto& seat : value.state.seats) if (seat.peer != value.state.host && value.transport->link(seat.peer) == LinkState::closed) lost.push_back(seat.peer);
        for (const auto peer : lost) if (auto removed = value.remove(peer, LobbyReason::link_closed, false); !removed) return removed;
    }
    // Drain a bounded batch before setup can publish commitment. If the batch
    // fills, defer setup/commit to the next pump so queued Unready/Leave wins.
    std::size_t count{};
    for (; count < value.limits.frames_per_pump; ++count) {
        auto raw = value.transport->poll(); if (!raw) break;
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
    if (value.setup) {
        if (auto pumped = value.setup->pump(now); !pumped) {
            if (value.is_host) {
                if (value.state.phase == LobbyPhase::committed) {
                    static_cast<void>(value.terminal(LobbyReason::setup_failed, pumped.error().message));
                    return pumped;
                }
                return value.abort(LobbyReason::setup_failed, pumped.error().message);
            }
            value.notice = LobbyNotice{value.state.room, value.state.frozen_revision, LobbyReason::setup_failed, pumped.error().message};
            return core::Result<void>::success(); // Host serializes the abort/new revision.
        }
        if (value.is_host && value.state.phase == LobbyPhase::starting && value.setup->phase() == SetupPhase::ready) {
            auto committed = value.state; committed.phase = LobbyPhase::committed;
            if (auto sent = value.broadcast(committed); !sent) return sent;
            value.state = std::move(committed); return core::Result<void>::success();
        }
    }
    return core::Result<void>::success();
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

} // namespace eawr::net
