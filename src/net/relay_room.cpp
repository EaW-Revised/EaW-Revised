#include "relay_room.hpp"
#include "relay_directory.hpp"
#include "relay_renewal.hpp"
#include "relay_tls.hpp"

namespace eawr::net::relay {
namespace {
using State = GnsRoomSlotState;
using Time = RoomService::Time;
Bytes ack_frame(unsigned reason = 0) { auto bytes = message(Kind::room_ack); if (reason != 0) u32(bytes, reason); return bytes; }
Bytes closed_frame(unsigned reason) { auto bytes = message(Kind::room_closed); u32(bytes, reason); return bytes; }
std::chrono::milliseconds until(Time to, Time from) {
    return to > from ? std::chrono::ceil<std::chrono::milliseconds>(to - from) : std::chrono::milliseconds{};
}
// The identity of a seat is visible once the member has attached; reserved seats show only their state.
bool identity_visible(State state) { return state == State::attached || state == State::held || state == State::left; }
}

RoomService::RoomService(RoomOptions options, RoomSink& sink, Directory& directory, PrefixBudgets& budgets)
    : options_(std::move(options)), sink_(sink), directory_(directory), budgets_(budgets) {}

std::uint32_t RoomService::capacity(const std::string& room) const {
    const auto found = rooms_.find(room); return found == rooms_.end() ? 0 : found->second.capacity;
}
std::uint32_t RoomService::occupied(const std::string& room) const {
    const auto found = rooms_.find(room);
    if (found == rooms_.end()) return 0;
    return static_cast<std::uint32_t>(std::count_if(found->second.slots.begin(), found->second.slots.end(),
        [](const Slot& slot) { return slot.state != State::vacant; }));
}
bool RoomService::denied(const std::string& room, const std::string& principal) const {
    const auto found = rooms_.find(room);
    return found != rooms_.end() && found->second.denied_principals.contains(principal);
}
std::optional<std::uint32_t> RoomService::lowest_vacant(const Room& room) const {
    for (std::uint32_t i = 1; i < room.capacity; ++i) if (room.slots[i].state == State::vacant) return i;
    return {};
}
std::size_t RoomService::outstanding(const Room& room) const {
    return static_cast<std::size_t>(std::count_if(room.slots.begin(), room.slots.end(), [](const Slot& slot) {
        return slot.state == State::reserved && !slot.attaching && slot.reserver != 0;
    }));
}
bool RoomService::duplicate_identity(const Room& room, std::uint32_t self, const std::string& identity) const {
    for (std::uint32_t i = 0; i < room.capacity; ++i)
        if (i != self && !room.slots[i].identity.empty() && room.slots[i].identity == identity) return true;
    return false;
}
void RoomService::uncount(Slot& slot) {
    if (!slot.counted) return;
    if (auto* budget = budgets_.find(slot.prefix); budget && budget->unpaired > 0) --budget->unpaired;
    slot.counted = false;
}
Bytes RoomService::roster_frame(const Room& room) const {
    auto bytes = message(Kind::room_roster);
    u32(bytes, room.revision); u32(bytes, room.capacity); u32(bytes, room.state == GnsRoomState::open ? 0 : 1); u32(bytes, room.capacity);
    for (std::uint32_t i = 0; i < room.capacity; ++i) {
        const auto& slot = room.slots[i];
        u32(bytes, static_cast<std::uint32_t>(slot.state));
        string(bytes, identity_visible(slot.state) ? slot.identity : std::string{});
    }
    return bytes;
}
// One snapshot per revision to every attached member (no deltas, under 1 KiB).
void RoomService::announce(Room& room) {
    ++room.revision;
    const auto frame = roster_frame(room);
    for (std::uint32_t i = 0; i < room.capacity; ++i)
        if (room.slots[i].state == State::attached && !room.slots[i].attaching && room.slots[i].connection) sink_.send(room.slots[i].connection, frame);
}
Bytes RoomService::attached_frame(const Room& room, std::uint32_t index, const std::optional<GnsTurnCredentials>& turn, Time now, Kind kind) const {
    const auto& slot = room.slots[index];
    auto bytes = message(kind);
    // Only the open reply carries the alias: a reattaching host already has it.
    u32(bytes, index); string(bytes, slot.capability); string(bytes, slot.identity);
    string(bytes, kind == Kind::room_opened ? room.alias : std::string{});
    if (turn) { string(bytes, turn->uri); string(bytes, turn->username); string(bytes, turn->password); u32(bytes, static_cast<std::uint32_t>(turn->lifetime.count())); }
    else { string(bytes, ""); string(bytes, ""); string(bytes, ""); u32(bytes, 0); }
    u32(bytes, clamp_ms(until(lease_end(room), now))); u32(bytes, clamp_ms(until(lease_hard_end(room), now)));
    return bytes;
}

// ---- Directory side: the shared reservation table -------------------------------------------------------------------
RoomService::Reservation RoomService::reserve(const std::string& id, std::uint64_t client, const std::string& principal,
        const std::string& prefix, Time now) {
    const auto found = rooms_.find(id);
    if (found == rooms_.end()) return {room_unknown, 0, {}};
    auto& room = found->second;
    if (room.state != GnsRoomState::open) return {room_locked, 0, {}};
    if (room.denied_principals.contains(principal)) return {room_denied, 0, {}};
    for (std::uint32_t i = 0; i < room.capacity; ++i)
        if (room.slots[i].state != State::vacant && !room.slots[i].principal.empty() && room.slots[i].principal == principal) return {1543, 0, {}};
    // Two outstanding reservations per room, and the reserving prefix's unpaired budget (default 2).
    if (outstanding(room) >= options_.reservations_per_room) return {room_unavailable, 0, {}};
    auto* budget = budgets_.find(prefix);
    if (budget && budget->unpaired >= options_.unpaired_per_prefix) return {1527, 0, {}};
    const auto index = lowest_vacant(room);
    if (!index) return {1545, 0, {}};
    auto capability = random_code();
    if (!capability) return {1550, 0, {}};
    Slot slot;
    slot.state = State::reserved; slot.capability = capability.value(); slot.principal = principal; slot.prefix = prefix;
    slot.reserver = client; slot.deadline = now + options_.reservation_timeout; slot.counted = budget != nullptr;
    if (budget) ++budget->unpaired;
    room.slots[*index] = std::move(slot);
    reservers_[client] = {id, *index};
    announce(room);
    return {0, *index, room.slots[*index].capability};
}
void RoomService::release_reservation(std::uint64_t client) {
    const auto found = reservers_.find(client);
    if (found == reservers_.end()) return;
    const auto [id, index] = found->second;
    reservers_.erase(found);
    const auto room = rooms_.find(id);
    if (room == rooms_.end()) return;
    auto& slot = room->second.slots[index];
    if (slot.state != State::reserved || slot.attaching || slot.reserver != client) return;
    free_slot(room->second, index); announce(room->second);
}
void RoomService::directory_closed(const std::string& id) {
    // A started match outlives its directory row; before Start the host is gone from the lobby, so the room ends.
    const auto found = rooms_.find(id);
    if (found != rooms_.end() && found->second.state == GnsRoomState::open) close_room(id, room_unknown, true);
}

// ---- Slot and room lifetime -----------------------------------------------------------------------------------------
void RoomService::issue(Slot& slot, const std::string& username) {
    if (username.empty()) return;
    // Bounded: about one per renewal over the hard maximum. Beyond the bound the oldest is revoked at once.
    if (slot.issued.size() >= 64) { sink_.revoke(slot.issued.front()); slot.issued.erase(slot.issued.begin()); }
    slot.issued.push_back(username);
}
void RoomService::revoke_issued(Slot& slot) {
    for (const auto& name : slot.issued) sink_.revoke(name);
    slot.issued.clear();
}
// Member operations and renewals spend the same per-member and per-room budgets.
bool RoomService::charge(Room& room, Slot& slot, std::size_t bytes, std::uint64_t connection, Time now) {
    slot.budget.refresh(now); room.budget.refresh(now);
    if (++slot.budget.operations > options_.operations_per_member || !slot.budget.add_bytes(bytes, options_.bytes_per_member)
        || ++room.budget.operations > options_.operations_per_member * room.capacity
        || !room.budget.add_bytes(bytes, options_.bytes_per_member * room.capacity)) { sink_.fail(connection, room_budget); return false; }
    return true;
}
void RoomService::free_slot(Room& room, std::uint32_t index) {
    auto& slot = room.slots[index];
    revoke_issued(slot);
    if (slot.refreshed && !slot.refreshed->username.empty()) sink_.revoke(slot.refreshed->username);
    uncount(slot);
    if (slot.connection) bound_.erase(slot.connection);
    if (slot.reserver) reservers_.erase(slot.reserver);
    slot = Slot{};
}
void RoomService::close_room(const std::string& id, unsigned reason, bool from_directory) {
    const auto found = rooms_.find(id);
    if (found == rooms_.end()) return;
    auto& room = found->second;
    const auto frame = closed_frame(reason);
    for (std::uint32_t i = 0; i < room.capacity; ++i) {
        auto& slot = room.slots[i];
        const auto connection = slot.connection;
        free_slot(room, i);
        if (connection) sink_.close(connection, frame);
    }
    if (!room.alias.empty()) aliases_.erase(room.alias);
    const auto locked = room.state != GnsRoomState::open;
    rooms_.erase(found);
    if (!from_directory) sink_.directory_state(id, locked, true);
}
void RoomService::begin_turn(Room& room, std::uint32_t index, Time now) {
    auto& slot = room.slots[index];
    const auto lifetime = std::min(std::chrono::ceil<std::chrono::seconds>(until(lease_hard_end(room), now)) + std::chrono::seconds(60),
        std::chrono::seconds(172800));
    auto key = "room:" + room.id + ":" + std::to_string(index) + ":a:" + std::to_string(++sequence_);
    slot.turn_key = key; slot.turn_deadline = now + options_.credential_wait;
    turns_[key] = {room.id, index, Purpose::attach, now};
    if (!sink_.request_turn(key, lifetime)) {
        turns_.erase(key);
        sink_.warn(error(1559, "TURN credential call cap or queue reached; room member continues without relay candidates"));
        finalize_attach(room, index, {}, now, slot.reply);
    }
}
void RoomService::finalize_attach(Room& room, std::uint32_t index, std::optional<GnsTurnCredentials> turn, Time now, Kind kind) {
    auto& slot = room.slots[index];
    slot.attaching = false; slot.turn_key.clear();
    slot.state = State::attached; slot.deadline = {}; slot.budget = Budget{}; slot.budget.window = now;
    if (turn && !turn->uri.empty()) issue(slot, turn->username); else turn.reset();
    sink_.send(slot.connection, attached_frame(room, index, turn, now, kind));
    announce(room);
}

// ---- Connection side ----------------------------------------------------------------------------------------------
void RoomService::request(std::uint64_t connection, const std::string& prefix, const Bytes& bytes, Time now) {
    if (bytes.size() < 2 || bytes[0] != 2) { sink_.fail(connection, 1521); return; }
    const auto kind = static_cast<Kind>(bytes[1]);
    Reader reader{std::span(bytes).subspan(2)};
    if (kind == Kind::room_open) { open(connection, prefix, reader, now); return; }
    if (kind == Kind::room_attach) { attach(connection, prefix, reader, now, false); return; }
    if (kind == Kind::room_reattach) { attach(connection, prefix, reader, now, true); return; }
    operate(connection, bytes, reader, now, kind);
}
void RoomService::open(std::uint64_t connection, const std::string&, Reader& reader, Time now) {
    const auto id = reader.string(32), ownership = reader.string(32), principal = reader.string(32);
    const auto identity = reader.string(gns_identity_max_bytes);
    const auto alias_flag = reader.u32();
    if (!reader.done() || !code_valid(id) || !code_valid(ownership) || !code_valid(principal) || !identity_valid(identity)
        || alias_flag > 1 || bound_.contains(connection)) { sink_.fail(connection, 1521); return; }
    if (rooms_.contains(id)) { sink_.fail(connection, room_unavailable); return; }
    const auto info = directory_.scope_info(id, ownership, principal);
    if (!info || info->bridged) { sink_.fail(connection, room_unknown); return; }
    if (info->capacity < 2 || info->capacity > room_slots) { sink_.fail(connection, 1521); return; }
    auto capability = random_code();
    if (!capability) { sink_.fail(connection, 1529); return; }
    Room room;
    room.id = id; room.capacity = info->capacity; room.opened = now; room.budget.window = now;
    if (options_.aliases && alias_flag != 0 && !info->passworded) {
        auto alias = sink_.new_alias();
        if (!alias) { sink_.fail(connection, 1533); return; }
        room.alias = std::move(*alias); aliases_[room.alias] = id;
    }
    auto& slot = room.slots[0];
    slot.state = State::reserved; slot.attaching = true; slot.identity = identity; slot.capability = capability.value();
    slot.connection = connection; slot.reply = Kind::room_opened; slot.budget.window = now;
    bound_[connection] = {id, 0};
    auto& stored = rooms_.emplace(id, std::move(room)).first->second;
    begin_turn(stored, 0, now);
}
void RoomService::attach(std::uint64_t connection, const std::string& prefix, Reader& reader, Time now, bool reattach) {
    const auto first = reader.string(32), capability = reader.string(32);
    const auto identity = reader.string(gns_identity_max_bytes);
    const bool alias_form = alias_valid(first);
    if (!reader.done() || !identity_valid(identity) || bound_.contains(connection)
        || (alias_form ? (reattach || !capability.empty()) : (!code_valid(first) || !code_valid(capability)))) {
        sink_.fail(connection, 1521); return;
    }
    std::string id = first;
    if (alias_form) {
        if (!budgets_.guess_allowed(prefix, now, options_.failed_guesses, true)) { sink_.fail(connection, 1532); return; }
        const auto alias = aliases_.find(first);
        if (alias == aliases_.end()) { budgets_.failed_guess(prefix, now, true); sink_.fail(connection, 1523); return; }
        id = alias->second;
    }
    const auto found = rooms_.find(id);
    if (found == rooms_.end()) { sink_.fail(connection, room_unknown); return; }
    auto& room = found->second;
    if (room.denied_identities.contains(identity)) { sink_.fail(connection, room_denied); return; }
    if (reattach) {
        // A started match only: the capability opens the signalling path, never proves the seat (reconnect proof does).
        if (room.state == GnsRoomState::open) { sink_.fail(connection, room_unavailable); return; }
        for (std::uint32_t i = 0; i < room.capacity; ++i) {
            auto& slot = room.slots[i];
            if ((slot.state != State::held && slot.state != State::attached) || slot.capability.empty()
                || !secret_equal(slot.capability, capability) || slot.identity != identity) continue;
            if (slot.attaching) { sink_.fail(connection, room_unavailable); return; }
            if (slot.connection) { // The old connection may be half open: the capability holder takes the seat over.
                const auto old = slot.connection; bound_.erase(old); slot.connection = 0;
                sink_.close(old, closed_frame(room_capability));
            }
            // The credentials issued so far stay valid until the seat ends (they are all revoked then, and expire on their
            // own): this seat's pair links keep refreshing their TURN allocation and permissions with them, and a provider
            // that stops forwarding for a revoked credential would cut every other live link of a re-attaching seat (#4021).
            if (slot.refreshed && !slot.refreshed->username.empty()) sink_.revoke(slot.refreshed->username);
            slot.refresh_key.clear(); slot.refreshed.reset();
            slot.state = State::held; slot.deadline = now + options_.hold_timeout;
            slot.connection = connection; slot.attaching = true; slot.reply = Kind::room_attached;
            bound_[connection] = {id, i};
            begin_turn(room, i, now);
            return;
        }
        sink_.fail(connection, room_capability); return;
    }
    if (room.state != GnsRoomState::open) {
        if (alias_form) budgets_.failed_guess(prefix, now, true);
        sink_.fail(connection, room_locked); return;
    }
    std::uint32_t index = room.capacity;
    if (alias_form) {
        if (const auto vacant = lowest_vacant(room)) index = *vacant;
        else { sink_.fail(connection, 1545); return; }
    } else {
        for (std::uint32_t i = 1; i < room.capacity; ++i) {
            const auto& slot = room.slots[i];
            if (slot.state == State::reserved && !slot.attaching && slot.reserver != 0 && secret_equal(slot.capability, capability)) { index = i; break; }
        }
        if (index == room.capacity) { sink_.fail(connection, room_capability); return; }
        if (now >= room.slots[index].deadline) {
            if (budgets_.find(room.slots[index].prefix)) budgets_.failed_guess(room.slots[index].prefix, now, false);
            free_slot(room, index); announce(room); sink_.fail(connection, room_unavailable); return;
        }
    }
    if (duplicate_identity(room, index, identity)) { sink_.fail(connection, room_unavailable); return; }
    auto& slot = room.slots[index];
    if (alias_form) {
        auto made = random_code();
        if (!made) { sink_.fail(connection, 1529); return; }
        slot = Slot{}; slot.capability = made.value(); slot.prefix = prefix;
    } else {
        uncount(slot);
        reservers_.erase(slot.reserver); slot.reserver = 0;
    }
    slot.state = State::reserved; slot.identity = identity; slot.connection = connection; slot.attaching = true; slot.reply = Kind::room_attached;
    bound_[connection] = {id, index};
    begin_turn(room, index, now);
}
void RoomService::disconnect(std::uint64_t connection, Time now) {
    const auto bound = bound_.find(connection);
    if (bound == bound_.end()) return;
    const auto [id, index] = bound->second;
    bound_.erase(bound);
    const auto found = rooms_.find(id);
    if (found == rooms_.end()) return;
    auto& room = found->second;
    auto& slot = room.slots[index];
    slot.connection = 0;
    if (slot.refreshed && !slot.refreshed->username.empty()) sink_.revoke(slot.refreshed->username);
    slot.refreshed.reset(); slot.refresh_key.clear();
    if (slot.attaching) {
        slot.attaching = false; slot.turn_key.clear();
        if (slot.state == State::held) return; // an aborted repair keeps the seat held
        if (index == 0) { close_room(id, 1528, false); return; }
        free_slot(room, index); announce(room); return;
    }
    if (slot.state != State::attached) return;
    if (room.state == GnsRoomState::open) {
        if (index == 0) { close_room(id, 1528, false); return; } // host loss before Start closes staging
        free_slot(room, index); announce(room); return;
    }
    slot.state = State::held; slot.deadline = now + options_.hold_timeout;
    announce(room);
}
void RoomService::leave(Room& room, std::uint32_t index, Time now) {
    auto& slot = room.slots[index];
    const auto connection = slot.connection;
    if (room.state == GnsRoomState::open) {
        if (index == 0) { close_room(room.id, 1528, false); return; }
        free_slot(room, index);
    } else {
        revoke_issued(slot);
        if (slot.refreshed && !slot.refreshed->username.empty()) sink_.revoke(slot.refreshed->username);
        slot.refreshed.reset(); slot.refresh_key.clear();
        bound_.erase(connection); slot.connection = 0; slot.capability.clear(); slot.state = State::left;
    }
    (void)now;
    sink_.close(connection, message(Kind::room_ack));
    announce(room);
}
void RoomService::kick_or_release(std::uint64_t connection, Room& room, std::uint32_t self, std::uint32_t target, bool kick, Time) {
    if (self != 0) { sink_.fail(connection, room_capability); return; }
    // A host cannot remove itself or a seat outside the room (a protocol violation). Everything else is a benign race
    // with the member leaving, or with Start, and keeps every connection: an empty seat is a no-op acknowledgement.
    if (target == 0 || target >= room.capacity) { sink_.fail(connection, 1521); return; }
    if (room.slots[target].state == State::vacant) { sink_.send(connection, ack_frame()); return; }
    // Release frees a seat for a refill before Start; kick also works in battle but never frees the seat again.
    if (!kick && room.state != GnsRoomState::open) { sink_.send(connection, ack_frame(room_locked)); return; }
    auto& slot = room.slots[target];
    unsigned outcome = 0;
    if (kick) {
        // The denylist is bounded. A full list still removes the member, but cannot deny it: the host is told.
        if (room.denied_identities.size() >= room_denylist_max || room.denied_principals.size() >= room_denylist_max) {
            outcome = room_budget; sink_.warn(error(room_budget, "Room denylist is full; the kicked member is not denied"));
        } else {
            if (!slot.identity.empty()) room.denied_identities.insert(slot.identity);
            if (!slot.principal.empty()) room.denied_principals.insert(slot.principal);
        }
    }
    const auto target_connection = slot.connection;
    if (kick && room.state != GnsRoomState::open) {
        const auto identity = slot.identity;
        free_slot(room, target); room.slots[target].state = State::left; room.slots[target].identity = identity;
    } else free_slot(room, target);
    if (target_connection) sink_.close(target_connection, closed_frame(room_denied));
    sink_.send(connection, ack_frame(outcome));
    announce(room);
}
void RoomService::operate(std::uint64_t connection, const Bytes& bytes, Reader& reader, Time now, Kind kind) {
    const auto bound = bound_.find(connection);
    if (bound == bound_.end()) { sink_.fail(connection, room_capability); return; }
    const auto [id, index] = bound->second;
    auto& room = rooms_.at(id);
    auto& slot = room.slots[index];
    if (slot.attaching || slot.state != State::attached) { sink_.fail(connection, 1521); return; }
    if (!charge(room, slot, bytes.size(), connection, now)) return;
    if (kind == Kind::room_signal) {
        const auto destination = reader.u32();
        const auto blob = reader.bytes;
        // Malformed frames, oneself and seats outside the room are protocol violations.
        if (!reader.valid || blob.empty() || blob.size() > room_signal_max || destination >= room.capacity || destination == index) {
            sink_.fail(connection, 1521); return;
        }
        // A seat that is vacant, reserved, attaching, held or left is a benign race (a member just left, a repair is
        // under way, ICE retries): the blob is dropped and counted, the sender stays.
        const auto& target = room.slots[destination];
        if (target.state != State::attached || target.attaching) { ++dropped_; return; }
        // The source slot and identity are set here, never taken from the sender.
        auto forwarded = message(Kind::room_forwarded); u32(forwarded, index); string(forwarded, slot.identity);
        forwarded.insert(forwarded.end(), blob.begin(), blob.end());
        if (!sink_.try_send(target.connection, std::move(forwarded))) ++dropped_; // a full receiver queue loses the blob, not the connection
        return;
    }
    if (kind == Kind::room_leave) {
        if (!reader.done()) { sink_.fail(connection, 1521); return; }
        leave(room, index, now); return;
    }
    if (kind == Kind::room_release || kind == Kind::room_kick) {
        const auto target = reader.u32();
        if (!reader.done()) { sink_.fail(connection, 1521); return; }
        kick_or_release(connection, room, index, target, kind == Kind::room_kick, now); return;
    }
    if (kind == Kind::room_lock) {
        if (!reader.done() || index != 0) { sink_.fail(connection, reader.done() ? room_capability : 1521); return; }
        if (room.state != GnsRoomState::open) { sink_.send(connection, ack_frame(room_locked)); return; } // a repeated Start
        // Start: pending reservations are dropped, nobody new may attach, and the directory hides the room.
        for (std::uint32_t i = 1; i < room.capacity; ++i)
            if (room.slots[i].state == State::reserved && !room.slots[i].attaching && room.slots[i].reserver != 0) free_slot(room, i);
        room.state = GnsRoomState::locked;
        sink_.directory_state(id, true, false);
        sink_.send(connection, ack_frame());
        announce(room); return;
    }
    sink_.fail(connection, 1521);
}

// ---- Lease renewal (ticket E, per member) ---------------------------------------------------------------------------
void RoomService::renew(std::uint64_t connection, Time now) {
    const auto bound = bound_.find(connection);
    if (bound == bound_.end()) { sink_.fail(connection, room_capability); return; }
    auto& room = rooms_.at(bound->second.first);
    auto& requester = room.slots[bound->second.second];
    if (requester.state != State::attached || requester.attaching) { sink_.fail(connection, 1524); return; }
    if (!charge(room, requester, 2, connection, now)) return;
    const auto end = lease_end(room), hard = lease_hard_end(room);
    if (now >= end) return; // poll closes the room with the typed reason
    const auto remaining = until(end, now);
    if (end >= hard) { sink_.send(connection, refused_message(renew_refused_maximum, remaining)); return; }
    // A member may start a renewal at most every renew_cooldown: a failing provider must not be hammered by one seat.
    if (room.renewing || remaining > renewal_window() || now < requester.next_renew) {
        sink_.send(connection, refused_message(renew_refused_early, remaining)); return;
    }
    requester.next_renew = now + options_.renew_cooldown;
    const auto next = std::min(end + options_.session_lifetime, hard);
    const auto lifetime = std::min(std::chrono::ceil<std::chrono::seconds>(next - now) + std::chrono::seconds(60), std::chrono::seconds(172800));
    room.renewing = true; room.requester = connection;
    std::size_t members = 0;
    for (std::uint32_t i = 0; i < room.capacity; ++i) {
        auto& slot = room.slots[i];
        if (slot.state != State::attached || slot.attaching) continue;
        ++members;
        auto key = "room:" + room.id + ":" + std::to_string(i) + ":r:" + std::to_string(++sequence_);
        slot.refresh_key = key; turns_[key] = {room.id, i, Purpose::renew, now};
        if (!sink_.request_turn(key, lifetime)) { refuse_refresh(room, now); return; }
    }
    // One provider call per attached member, each allowed its own wait.
    room.renewal_deadline = std::min(end, now + options_.credential_wait * static_cast<int>(std::max<std::size_t>(members, 1)));
}
void RoomService::refuse_refresh(Room& room, Time now) {
    for (auto& slot : room.slots) {
        slot.refresh_key.clear();
        if (slot.refreshed && !slot.refreshed->username.empty()) sink_.revoke(slot.refreshed->username);
        slot.refreshed.reset();
    }
    room.renewing = false;
    if (room.requester && bound_.contains(room.requester))
        sink_.send(room.requester, refused_message(renew_refused_credentials, until(lease_end(room), now)));
}
void RoomService::settle_renewal(Room& room, Time now) {
    if (!room.renewing) return;
    for (const auto& slot : room.slots) if (!slot.refresh_key.empty()) return;
    const auto end = lease_end(room), hard = lease_hard_end(room);
    room.extension += std::min(options_.session_lifetime, std::chrono::duration_cast<std::chrono::milliseconds>(hard - end));
    room.warned = false; room.renewing = false;
    for (auto& slot : room.slots) {
        if (!slot.refreshed) continue;
        if (slot.state == State::attached && slot.connection) {
            sink_.send(slot.connection, renewed_message(until(lease_end(room), now), until(hard, now), *slot.refreshed));
            // The live allocation keeps its pairing credential; only the latest refresh is kept for cleanup.
            issue(slot, slot.refreshed->username);
        } else if (!slot.refreshed->username.empty()) sink_.revoke(slot.refreshed->username);
        slot.refreshed.reset();
    }
}
void RoomService::turn_done(const std::string& key, std::optional<GnsTurnCredentials> credentials, std::optional<core::Diagnostic> failure, Time now) {
    const auto pending = turns_.find(key);
    const auto drop = [&] { if (credentials && !credentials->username.empty()) sink_.revoke(credentials->username); };
    if (pending == turns_.end()) { drop(); return; }
    const auto job = pending->second;
    turns_.erase(pending);
    if (failure) sink_.warn(*failure);
    const auto found = rooms_.find(job.room);
    if (found == rooms_.end()) { drop(); return; }
    auto& room = found->second;
    auto& slot = room.slots[job.slot];
    if (job.purpose == Purpose::attach) {
        if (slot.turn_key != key || !slot.attaching) { drop(); return; }
        finalize_attach(room, job.slot, std::move(credentials), now, slot.reply);
        return;
    }
    if (slot.refresh_key != key || !room.renewing) { drop(); return; }
    if (!credentials) { refuse_refresh(room, now); return; }
    slot.refreshed = std::move(credentials); slot.refresh_key.clear();
    settle_renewal(room, now);
}

// ---- Time-driven transitions ----------------------------------------------------------------------------------------
void RoomService::poll(Time now) {
    for (auto it = turns_.begin(); it != turns_.end();) {
        if (now - it->second.created >= std::chrono::minutes(10)) it = turns_.erase(it); else ++it;
    }
    std::vector<std::string> ids;
    for (const auto& [id, room] : rooms_) { (void)room; ids.push_back(id); }
    for (const auto& id : ids) {
        const auto found = rooms_.find(id);
        if (found == rooms_.end()) continue;
        auto& room = found->second;
        if (now >= lease_end(room)) { close_room(id, lease_end(room) >= lease_hard_end(room) ? lease_ended_at_maximum : lease_not_renewed, false); continue; }
        bool changed = false;
        for (std::uint32_t i = 0; i < room.capacity; ++i) {
            auto& slot = room.slots[i];
            if (slot.state == State::reserved && !slot.attaching && slot.reserver != 0 && now >= slot.deadline) {
                // An expired reservation counts as a failed guess for its prefix: rolling reservations reach the cooldown.
                if (budgets_.find(slot.prefix)) { budgets_.failed_guess(slot.prefix, now, false); budgets_.reservation_expired(slot.prefix, now); }
                free_slot(room, i); changed = true;
            } else if (slot.attaching && !slot.turn_key.empty() && now >= slot.turn_deadline) {
                sink_.warn(error(1556, "TURN credentials were not ready in time; room member continues without relay candidates"));
                finalize_attach(room, i, {}, now, slot.reply);
            } else if (slot.state == State::held && !slot.attaching && now >= slot.deadline) {
                revoke_issued(slot);
                slot.state = State::left; slot.capability.clear(); changed = true;
            }
        }
        if (changed) announce(room);
        if (room.renewing && now >= room.renewal_deadline) refuse_refresh(room, now);
        settle_renewal(room, now);
        if (!room.warned && !room.renewing) {
            const auto end = lease_end(room), hard = lease_hard_end(room);
            if (until(end, now) <= renewal_window()) {
                room.warned = true;
                const auto frame = expiring_message(until(end, now), until(hard, now), end < hard);
                for (const auto& slot : room.slots) if (slot.state == State::attached && !slot.attaching && slot.connection) sink_.send(slot.connection, frame);
            }
        }
        if (room.state != GnsRoomState::open && std::none_of(room.slots.begin(), room.slots.end(), [](const Slot& slot) {
            return slot.state == State::attached || slot.state == State::held || slot.attaching; })) close_room(id, 1528, false);
    }
}
} // namespace eawr::net::relay
