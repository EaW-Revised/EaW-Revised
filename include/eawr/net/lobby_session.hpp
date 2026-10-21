#pragma once

#include "eawr/net/setup.hpp"

#include <deque>
#include <variant>

namespace eawr::net {

// ADR-023: staging and match entry share one command-seat capacity.
inline constexpr std::uint32_t max_lobby_seats = static_cast<std::uint32_t>(max_match_seats);
inline constexpr std::size_t max_lobby_payload_bytes = max_setup_bytes > max_batch_bytes + 5
    ? max_setup_bytes : max_batch_bytes + 5;
inline constexpr std::size_t max_lobby_frame_bytes = max_lobby_payload_bytes + 256;
static_assert(max_lobby_payload_bytes >= max_batch_bytes + 5);
static_assert(max_lobby_frame_bytes >= max_batch_bytes + 5 + 21 + 64);
inline constexpr std::uint8_t lobby_snapshot_kind = 72, lobby_request_kind = 73;
inline constexpr std::uint8_t lobby_chat_request_kind = 74, lobby_chat_kind = 75;
inline constexpr std::uint8_t lobby_setup_kind = 76, lobby_cancel_kind = 77;
inline constexpr std::uint8_t lobby_closed_kind = 78, lobby_rejected_kind = 79;

enum class LobbyPhase : std::uint8_t { staging, starting, committed, closed };
enum class LobbyReason : std::uint8_t {
    malformed, authority, stale_revision, invalid_choice, full, denied,
    not_ready, frozen, chat_limit, setup_failed, left, kicked, host_closed,
    link_closed, resource_limit, recovery_unavailable,
    // Online room mesh (slice N4): their diagnostics are EAWR-NET-1762 and EAWR-NET-1764 (the 1760..1779 range), not 1600+n.
    pair_unreachable, mesh_incomplete
};
struct LobbySelection {
    std::string name, faction;
    std::uint32_t colour{}, team{}, slot{};
    friend bool operator==(const LobbySelection&, const LobbySelection&) = default;
};
struct LobbySeat {
    PeerIndex peer{};
    sim::SeatId seat{};
    sim::tactical::PlayerId player{}; // Host-owned shared-owner mapping.
    LobbySelection selection;
    bool ready{};
    friend bool operator==(const LobbySeat&, const LobbySeat&) = default;
};
struct LobbyGame {
    std::string map;
    std::vector<std::uint8_t> options; // Canonical application builder options, including AI/open/closed rows.
    friend bool operator==(const LobbyGame&, const LobbyGame&) = default;
};
struct LobbySnapshot {
    std::string room; // Immutable public session identifier, never an invitation/credential.
    std::uint64_t revision{1}, frozen_revision{};
    std::uint64_t chat_sequence{}; // Initial cursor for newly admitted clients; history stays locally bounded.
    std::uint32_t capacity{max_lobby_seats};
    PeerIndex host{};
    LobbyPhase phase{LobbyPhase::staging};
    LobbyGame game;
    std::vector<LobbySeat> seats; // Host first, then ascending stable seat IDs.
    friend bool operator==(const LobbySnapshot&, const LobbySnapshot&) = default;
};
enum class LobbyAction : std::uint8_t { select, ready, unready, leave, snapshot };
struct LobbyRequest {
    std::string room;
    std::uint64_t revision{};
    LobbyAction action{};
    LobbySelection selection; // Only select uses this. No target peer or owner field.
};
struct LobbyChatRequest { std::string room, text; };
struct LobbyChat {
    std::string room;
    std::uint64_t sequence{};
    sim::SeatId seat{};
    std::string name, text; // Host captures the sender label when posting to the chat log.
    friend bool operator==(const LobbyChat&, const LobbyChat&) = default;
};
struct LobbySetupFrame { std::string room; std::uint64_t revision{}; std::vector<std::uint8_t> frame; };
struct LobbyNotice {
    std::string room;
    std::uint64_t revision{};
    LobbyReason reason{};
    std::string message;
};
struct LobbyCancel : LobbyNotice {};
struct LobbyClosed : LobbyNotice {};
struct LobbyRejected : LobbyNotice {};
using LobbyFrame = std::variant<LobbySnapshot, LobbyRequest, LobbyChatRequest, LobbyChat,
    LobbySetupFrame, LobbyCancel, LobbyClosed, LobbyRejected>;

[[nodiscard]] std::string sanitize_lobby_text(std::string_view value, std::size_t limit);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_lobby_frame(const LobbyFrame& frame);
[[nodiscard]] core::Result<LobbyFrame> decode_lobby_frame(std::span<const std::uint8_t> frame);
[[nodiscard]] core::Diagnostic lobby_diagnostic(LobbyReason reason, std::string message);

struct LobbyLimits {
    std::size_t frames_per_pump{256}, queued_setup_bytes{8U * 1024U * 1024U};
    std::size_t chat_history{128}, chat_messages_per_window{4};
    std::uint64_t chat_window_ms{2'000};
    std::size_t denied_identities{128};
};
struct LobbySetupContext {
    SetupProviders providers;
    SetupOptions options;
    std::shared_ptr<void> lifetime; // Optional owner of per-attempt monitors/executors/provider inputs.
};
struct LobbyHooks {
    // Mandatory loaded-data validator. Validate choices/owner consistency, map
    // capacity and canonical options; starting=true checks complete teams at
    // Ready, Start and frozen snapshots. Editable staging may be incomplete.
    std::function<core::Result<void>(const LobbySnapshot&, bool starting)> validate;
    // Build the EXACT frozen revision; returned seats must match the supplied
    // dense setup roster. Never reuse a previously built offer after an edit.
    std::function<core::Result<SetupOffer>(const LobbySnapshot&, const std::vector<SetupSeat>&)> offer;
    // Called once per attempt on every peer. Create fresh monitors here using
    // this transport (dense setup peers, host zero); they must outlive the result.
    std::function<core::Result<LobbySetupContext>(MessageTransport&, const LobbySnapshot&)> setup;
    // Host only: close admitted streams and cancel reservations for this identity.
    // The closed notice is best-effort; must not destroy the room transport.
    std::function<void(PeerIndex, LobbyReason)> remove_peer;
    // Host transport readiness (docs/net-online-npeer.md, section 3). mesh_complete is checked by Start:
    // every agreed seat present (online also verifies every pair). lock_room runs once the setup is agreed, just before the commitment
    // is published; a refusal returns the room to staging. Neither may block.
    std::function<core::Result<void>()> mesh_complete;
    std::function<core::Result<void>()> lock_room;
    // Optional asynchronous lock confirmation. Pending keeps Start frozen;
    // failure returns to staging with its named reason, never commits locally.
    std::function<core::Result<bool>()> lock_room_progress;
    // Capacity-based TCP staging freezes admissions before publishing Start.
    // Legacy/online rooms retain their setup-agreement lock timing.
    bool lock_at_start{};
};
struct LobbyRuntime;
class LobbyResult final {
public:
    [[nodiscard]] SetupResult& setup() { return setup_; }
    [[nodiscard]] const LobbySnapshot& snapshot() const;
private:
    friend class LobbySession;
    LobbyResult(std::shared_ptr<LobbyRuntime> lifetime, SetupResult setup)
        : lifetime_(std::move(lifetime)), setup_(std::move(setup)) {}
    std::shared_ptr<LobbyRuntime> lifetime_;
    SetupResult setup_;
};

// Single-threaded, caller-time-driven room over already authenticated/admitted
// reliable streams. Host serializes requests; changes take effect only on its
// authoritative snapshot. A request reaching the host before commitment may
// abort Start; after commitment the ordinary session departure path owns Leave.
// The outer transport outlives both session and result, and must negotiate a
// frame capacity of at least max_lobby_frame_bytes to carry maximal game frames.
class LobbySession final {
public:
    [[nodiscard]] static core::Result<LobbySession> host(MessageTransport& transport,
        std::string room, std::uint32_t capacity, LobbySelection selection,
        sim::tactical::PlayerId player, LobbyGame game, LobbyHooks hooks, LobbyLimits limits = {});
    [[nodiscard]] static core::Result<LobbySession> join(MessageTransport& transport,
        std::string room, PeerIndex host, LobbyHooks hooks, LobbyLimits limits = {});
    // Admission identity is trusted input from the admission bridge, NOT a guest
    // name. A kicked identity is denied for this room lifetime, even on a new peer.
    [[nodiscard]] core::Result<void> admit(PeerIndex peer, std::string identity,
        LobbySelection selection, sim::tactical::PlayerId player, std::uint64_t revision);
    [[nodiscard]] core::Result<void> assign(PeerIndex peer, LobbySelection selection,
        sim::tactical::PlayerId player, std::uint64_t revision);
    // Host commits repaired map/options and owner choices as one validated revision.
    [[nodiscard]] core::Result<void> configure_game(LobbyGame game, std::vector<LobbySeat> assignments, std::uint64_t revision);
    [[nodiscard]] core::Result<void> set_game(LobbyGame game, std::uint64_t revision);
    [[nodiscard]] core::Result<void> request(LobbyAction action, std::uint64_t revision,
        LobbySelection selection = {});
    [[nodiscard]] core::Result<void> chat(std::string text);
    [[nodiscard]] core::Result<void> kick(PeerIndex peer, std::uint64_t revision);
    // Host only, before Start. Online room mesh: `newcomer` could not reach `unreachable` (a seat of this room, or a seat that
    // already left) through any direct or relayed path, so it is refused, not the room (owner decision 1, #3669). The newcomer
    // gets a closed notice with mesh_newcomer_refusal_text(name, relay_configured); the room gets one chat line naming both
    // players; the host's notice() carries the diagnostic EAWR-NET-1762. No ban: the slot is free for another player.
    [[nodiscard]] core::Result<void> refuse_unreachable(PeerIndex newcomer, PeerIndex unreachable, bool relay_configured);
    [[nodiscard]] core::Result<void> close();
    [[nodiscard]] core::Result<void> start(std::uint64_t revision);
    [[nodiscard]] core::Result<void> accept_setup(); // Separate explicit SetupSession::accept.
    [[nodiscard]] core::Result<void> pump(std::uint64_t now_ms);
    // True once the host's commitment is accepted AND this peer's own setup barrier (tick-zero agreement with every
    // peer) is ready. Callers wait on this before take_result: the host commits on its own readiness, which does not prove
    // this guest has every other guest's tick-zero summary yet (#3929).
    [[nodiscard]] bool result_ready() const;
    [[nodiscard]] core::Result<LobbyResult> take_result();
    [[nodiscard]] const LobbySnapshot& snapshot() const;
    [[nodiscard]] const std::deque<LobbyChat>& chat_history() const;
    [[nodiscard]] const std::optional<LobbyNotice>& notice() const;
    [[nodiscard]] std::optional<SetupPhase> setup_phase() const;
    [[nodiscard]] std::uint64_t dropped_frames() const;
private:
    explicit LobbySession(std::shared_ptr<LobbyRuntime> runtime) : runtime_(std::move(runtime)) {}
    std::shared_ptr<LobbyRuntime> runtime_;
};

} // namespace eawr::net
