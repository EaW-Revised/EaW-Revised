#pragma once

#include "relay_prefix_budget.hpp"
#include "relay_protocol.hpp"
#include "eawr/net/gns_room.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

// N-peer room scope of the introduction service (slice N3 of docs/net-online-npeer.md), Kind 34..49.
// A room is the signalling counterpart of a directory room (same 128-bit ID). Slot 0 is the host, slots
// 1..capacity-1 are guests; the slot number is the transport PeerIndex. Layouts (all integers big endian):
//   room_open     host  -> service  string room, string ownership, string principal, string identity, u32 public_alias
//   room_attach   guest -> service  string room_or_alias, string capability (empty for an alias), string identity
//   room_reattach member-> service  string room, string capability, string identity           (after Lock only)
//   room_opened / room_attached (service -> member)
//                 u32 slot, string capability, string identity, string alias, string turn_uri, string turn_username,
//                 string turn_password, u32 turn_lifetime_s, u32 lease_remaining_ms, u32 lease_hard_remaining_ms
//   room_roster   service -> attached members   u32 revision, u32 capacity, u32 state (0 open, 1 locked),
//                 u32 count (== capacity), count x { u32 slot_state, string identity }  (no addresses or capabilities)
//   room_signal   member -> service  u32 destination slot, blob (1..65536 bytes, never parsed)
//   room_forwarded service -> member u32 source slot, string source identity, blob   (source set by the service)
//   room_leave / room_lock   empty body
//   room_ack      service -> member  empty: done. With a u32 reason: not (fully) applied and the connection stays
//                 (a repeated lock 1775, a release after Start 1775, a kick whose denylist entry did not fit 1777)
//   room_release / room_kick  host -> service  u32 slot
//   room_closed   service -> member  u32 reason; the connection closes after it
// Ticket E's Kind 13..16 (renew, renewed, renew_refused, expiring) run unchanged on a room member connection.
// Slot states on the wire are GnsRoomSlotState values: vacant 0, reserved 1, attached 2, held 3, left 4.
// Typed reasons (EAWR-NET-1772..1777, Kind::error or room_closed); anything else reuses the 15xx service numbers.
namespace eawr::net::relay {
inline constexpr std::size_t room_slots = max_match_seats;
static_assert(room_slots == 9, "The wire layouts and docs assume nine seats");
inline constexpr std::size_t room_signal_max = 65536;
inline constexpr std::size_t room_denylist_max = 64;
class Directory;

struct RoomOptions {
    std::chrono::milliseconds session_lifetime{7200000}, max_session_lifetime{86400000}, renewal_window{600000};
    std::chrono::milliseconds reservation_timeout{30000}; // unattached directory reservation
    std::chrono::milliseconds hold_timeout{300000};       // held seat after Lock before it is marked left
    std::chrono::milliseconds credential_wait{6000};      // attach/renew waits this long for a TURN provider
    std::size_t operations_per_member{480}, bytes_per_member{2 * 1024 * 1024};
    std::size_t reservations_per_room{2}, unpaired_per_prefix{2}, failed_guesses{100};
    std::chrono::milliseconds renew_cooldown{30000};     // per member, after a renewal attempt it started
    bool aliases{};
};
// What the service needs from its host: queued output, typed failure, TURN credentials and a directory view.
class RoomSink {
public:
    virtual ~RoomSink() = default;
    virtual bool send(std::uint64_t connection, Bytes bytes) = 0;
    // Best effort for forwarded signals: false when the receiver's queue is full. A slow receiver never loses its connection
    // because a sender filled its queue; the blob is dropped and counted.
    virtual bool try_send(std::uint64_t connection, Bytes bytes) = 0;
    virtual void close(std::uint64_t connection, Bytes final_frame) = 0; // queue the frame, then close after the flush
    virtual void fail(std::uint64_t connection, unsigned reason) = 0;
    // Asks the provider for one member credential; the outcome comes back through RoomService::turn_done.
    // false: refused (call cap, queue); the member continues without relay candidates.
    virtual bool request_turn(const std::string& key, std::chrono::seconds lifetime) = 0;
    virtual void revoke(const std::string& username) = 0;
    virtual std::optional<std::string> new_alias() = 0;
    virtual void warn(core::Diagnostic) = 0;
    virtual void directory_state(const std::string& room, bool locked, bool closed) = 0;
};

// Single owner thread, no sockets, no wall clock: the same state machine runs in the contracts with a fake sink and
// clock. Every mutation commits here before the reply or push is queued.
class RoomService {
public:
    using Time = RelayServer::Clock::time_point;
    RoomService(RoomOptions options, RoomSink& sink, Directory& directory, PrefixBudgets& budgets);
    // Directory side (the shared reservation table).
    bool has_scope(const std::string& room) const { return rooms_.contains(room); }
    bool alias_in_use(const std::string& alias) const { return aliases_.contains(alias); }
    std::uint32_t capacity(const std::string& room) const;
    std::uint32_t occupied(const std::string& room) const;
    bool denied(const std::string& room, const std::string& principal) const;
    struct Reservation { unsigned refusal{}; std::uint32_t slot{}; std::string capability; };
    Reservation reserve(const std::string& room, std::uint64_t client, const std::string& principal, const std::string& prefix, Time);
    void release_reservation(std::uint64_t client);
    void directory_closed(const std::string& room);
    // Connection side.
    void request(std::uint64_t connection, const std::string& prefix, const Bytes& bytes, Time);
    void renew(std::uint64_t connection, Time);
    void disconnect(std::uint64_t connection, Time);
    bool member(std::uint64_t connection) const { return bound_.contains(connection); }
    void poll(Time);
    bool owns_turn(const std::string& key) const { return turns_.contains(key); }
    void turn_done(const std::string& key, std::optional<GnsTurnCredentials>, std::optional<core::Diagnostic>, Time);
    std::size_t rooms() const { return rooms_.size(); }
    std::size_t members() const { return bound_.size(); }
    std::size_t reservations() const { return reservers_.size(); }
    // Signals to a seat that is not attached (or whose receiver queue is full): dropped, counted, nobody disconnected.
    std::uint64_t dropped_signals() const { return dropped_; }
private:
    enum class Purpose : std::uint8_t { attach, renew };
    struct Slot {
        GnsRoomSlotState state{GnsRoomSlotState::vacant};
        std::string identity, capability, principal, prefix, turn_key, refresh_key;
        // Every credential username issued to this seat (attach, renewals, reattach): each is revoked when the seat ends.
        std::vector<std::string> issued;
        Time next_renew{};
        std::uint64_t connection{}, reserver{};
        Time deadline{}, turn_deadline{};
        bool attaching{}, counted{};
        Kind reply{Kind::room_attached};
        std::optional<GnsTurnCredentials> refreshed;
        Budget budget;
    };
    struct Room {
        std::string id, alias;
        std::uint32_t capacity{}, revision{};
        GnsRoomState state{GnsRoomState::open};
        std::array<Slot, room_slots> slots{};
        Time opened{};
        std::chrono::milliseconds extension{};
        bool warned{}, renewing{};
        std::uint64_t requester{};
        Time renewal_deadline{};
        Budget budget;
        std::set<std::string> denied_identities, denied_principals;
    };
    struct PendingTurn { std::string room; std::uint32_t slot{}; Purpose purpose{}; Time created{}; };
    RoomOptions options_;
    RoomSink& sink_;
    Directory& directory_;
    PrefixBudgets& budgets_;
    std::uint64_t sequence_{}, dropped_{};
    std::map<std::string, Room> rooms_;
    std::map<std::string, std::string> aliases_;
    std::map<std::uint64_t, std::pair<std::string, std::uint32_t>> bound_, reservers_;
    std::map<std::string, PendingTurn> turns_;

    Time lease_end(const Room& r) const { return r.opened + options_.session_lifetime + r.extension; }
    Time lease_hard_end(const Room& r) const { return r.opened + options_.max_session_lifetime; }
    std::chrono::milliseconds renewal_window() const { return std::min(options_.renewal_window, options_.session_lifetime / 2); }
    void open(std::uint64_t, const std::string& prefix, Reader&, Time);
    void attach(std::uint64_t, const std::string& prefix, Reader&, Time, bool reattach);
    void operate(std::uint64_t, const Bytes&, Reader&, Time, Kind);
    void begin_turn(Room&, std::uint32_t, Time);
    void kick_or_release(std::uint64_t, Room&, std::uint32_t self, std::uint32_t target, bool kick, Time);
    void finalize_attach(Room&, std::uint32_t, std::optional<GnsTurnCredentials>, Time, Kind);
    void announce(Room&);
    void free_slot(Room&, std::uint32_t);
    void issue(Slot&, const std::string& username);
    void revoke_issued(Slot&);
    bool charge(Room&, Slot&, std::size_t bytes, std::uint64_t connection, Time);
    void uncount(Slot&);
    void close_room(const std::string& id, unsigned reason, bool from_directory);
    void refuse_refresh(Room&, Time);
    void settle_renewal(Room&, Time);
    void leave(Room&, std::uint32_t, Time);
    void expire_slot_states(Room&, Time);
    bool duplicate_identity(const Room&, std::uint32_t self, const std::string& identity) const;
    Bytes attached_frame(const Room&, std::uint32_t, const std::optional<GnsTurnCredentials>&, Time, Kind) const;
    Bytes roster_frame(const Room&) const;
    std::optional<std::uint32_t> lowest_vacant(const Room&) const;
    std::size_t outstanding(const Room&) const;
};
} // namespace eawr::net::relay
