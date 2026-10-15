#include "eawr/net/gns_room.hpp"

#include <deque>
#include <mutex>
#include <set>

namespace eawr::net {
namespace {
core::Diagnostic room_error(const char* code, const char* message) {
    core::Diagnostic diagnostic;
    diagnostic.code = code; diagnostic.message = message;
    return diagnostic;
}
bool identity_valid(const std::string& identity) {
    if (identity.empty() || identity.size() > gns_identity_max_bytes) return false;
    for (const unsigned char byte : identity) if (byte < 33 || byte > 126) return false;
    return true;
}
struct Room {
    std::mutex mutex;
    GnsRoomRoster roster;
    InProcessGnsRoom::Now now{};
    GnsRoomLease lease;
    std::size_t max_bytes{}, max_signals{};
    std::array<std::deque<GnsRoomSignal>, max_match_seats> inbox{};
    std::array<std::size_t, max_match_seats> used{};
    std::array<std::uint64_t, max_match_seats> generation{};
    std::array<std::optional<GnsTurnCredentials>, max_match_seats> turn{};
    // Fixed-size lifetime denylist: once full, further kicks close the room
    // rather than dropping old bans or growing fixture memory without bound.
    std::set<std::string> banned;
    bool valid{};
    bool healthy() const { return valid && roster.state != GnsRoomState::closed && now() < lease.expires; }
    void clear(PeerIndex seat, GnsRoomSlotState state) {
        inbox[seat].clear(); used[seat] = 0; turn[seat].reset(); ++generation[seat];
        for (PeerIndex p = 0; p < roster.capacity; ++p) {
            for (auto it = inbox[p].begin(); it != inbox[p].end();) {
                if (it->from_slot == seat) { used[p] -= it->bytes.size(); it = inbox[p].erase(it); }
                else ++it;
            }
        }
        roster.slots[seat].state = state;
        if (state == GnsRoomSlotState::vacant) roster.slots[seat].identity.clear();
        ++roster.revision;
    }
};
class Endpoint final : public GnsRoomEndpoint {
public:
    Endpoint(std::shared_ptr<Room> room, PeerIndex seat) : room_(std::move(room)), seat_(seat), generation_(room_->generation[seat]) {}
    PeerIndex local_slot() const override { return seat_; }
    GnsRoomRoster roster() const override { std::lock_guard guard(room_->mutex); return room_->roster; }
    GnsRoomLease lease() const override { std::lock_guard guard(room_->mutex); return room_->lease; }
    std::optional<GnsTurnCredentials> turn_credentials() const override {
        std::lock_guard guard(room_->mutex); return valid() ? room_->turn[seat_] : std::nullopt;
    }
    core::Result<void> status() const override {
        std::lock_guard guard(room_->mutex); return check();
    }
    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> blob) override {
        std::lock_guard guard(room_->mutex);
        if (auto checked = check(); !checked) return checked;
        if (to == seat_ || to >= room_->roster.capacity || room_->roster.slots[to].state != GnsRoomSlotState::attached)
            return core::Result<void>::failure(room_error("EAWR-NET-1761", "Room destination is not an attached remote member."));
        if (blob.empty() || blob.size() > 65536)
            return core::Result<void>::failure(room_error("EAWR-NET-1761", "Invalid room signal size."));
        if (room_->inbox[to].size() >= room_->max_signals || blob.size() > room_->max_bytes - room_->used[to])
            return core::Result<void>::failure(room_error("EAWR-NET-1763", "Room signal queue is full."));
        room_->inbox[to].push_back({seat_, room_->roster.slots[seat_].identity, {blob.begin(), blob.end()}});
        room_->used[to] += blob.size();
        return core::Result<void>::success();
    }
    core::Result<std::optional<GnsRoomSignal>> poll() override {
        std::lock_guard guard(room_->mutex);
        if (auto checked = check(); !checked) return core::Result<std::optional<GnsRoomSignal>>::failure(checked.error());
        if (room_->inbox[seat_].empty()) return core::Result<std::optional<GnsRoomSignal>>::success({});
        auto value = std::move(room_->inbox[seat_].front()); room_->inbox[seat_].pop_front();
        room_->used[seat_] -= value.bytes.size();
        return core::Result<std::optional<GnsRoomSignal>>::success(std::move(value));
    }
    core::Result<void> leave() override {
        std::lock_guard guard(room_->mutex);
        if (auto checked = check(); !checked) return checked;
        if (seat_ == 0 && room_->roster.state == GnsRoomState::open) room_->roster.state = GnsRoomState::closed;
        room_->clear(seat_, room_->roster.state == GnsRoomState::open ? GnsRoomSlotState::vacant : GnsRoomSlotState::left);
        return core::Result<void>::success();
    }
    core::Result<void> release(PeerIndex seat) override { return remove(seat, false); }
    core::Result<void> kick(PeerIndex seat) override { return remove(seat, true); }
    core::Result<void> lock() override {
        std::lock_guard guard(room_->mutex);
        if (auto checked = host_check(); !checked) return checked;
        if (room_->roster.state == GnsRoomState::locked) return core::Result<void>::success();
        for (PeerIndex p = 0; p < room_->roster.capacity; ++p)
            if (room_->roster.slots[p].state != GnsRoomSlotState::attached)
                return core::Result<void>::failure(room_error("EAWR-NET-1764", "Room membership is incomplete at Start."));
        room_->roster.state = GnsRoomState::locked; ++room_->roster.revision;
        return core::Result<void>::success();
    }
private:
    bool valid() const { return room_->healthy() && generation_ == room_->generation[seat_]
        && room_->roster.slots[seat_].state == GnsRoomSlotState::attached; }
    core::Result<void> check() const {
        return valid() ? core::Result<void>::success()
            : core::Result<void>::failure(room_error("EAWR-NET-1760", "Room attachment is closed or expired."));
    }
    core::Result<void> host_check() const {
        if (auto checked = check(); !checked) return checked;
        if (seat_ != 0) return core::Result<void>::failure(room_error("EAWR-NET-1761", "Room operation requires the host."));
        return core::Result<void>::success();
    }
    core::Result<void> remove(PeerIndex seat, bool ban) {
        std::lock_guard guard(room_->mutex);
        if (auto checked = host_check(); !checked) return checked;
        if (seat == 0 || seat >= room_->roster.capacity || room_->roster.slots[seat].identity.empty()
            || (!ban && room_->roster.state != GnsRoomState::open))
            return core::Result<void>::failure(room_error("EAWR-NET-1761", "Invalid room release or kick."));
        if (ban) {
            if (room_->banned.size() >= 64) { room_->roster.state = GnsRoomState::closed; ++room_->roster.revision;
                return core::Result<void>::failure(room_error("EAWR-NET-1763", "Room kick capacity exceeded.")); }
            room_->banned.insert(room_->roster.slots[seat].identity);
        }
        room_->clear(seat, room_->roster.state == GnsRoomState::open ? GnsRoomSlotState::vacant : GnsRoomSlotState::left);
        return core::Result<void>::success();
    }
    std::shared_ptr<Room> room_;
    PeerIndex seat_{};
    std::uint64_t generation_{};
};
}
struct InProcessGnsRoom::Impl { std::shared_ptr<Room> room{std::make_shared<Room>()}; };
InProcessGnsRoom::InProcessGnsRoom(std::uint32_t capacity, Now now, std::size_t bytes,
    std::size_t signals, std::chrono::seconds lifetime) : impl_(std::make_unique<Impl>()) {
    auto& room = *impl_->room;
    room.valid = capacity >= 2 && capacity <= max_match_seats && now && bytes > 0 && signals > 0
        && lifetime > std::chrono::seconds{0} && lifetime <= std::chrono::hours{24};
    room.roster.capacity = room.valid ? capacity : 0;
    room.now = now; room.max_bytes = bytes; room.max_signals = signals;
    if (room.valid) room.lease.expires = now() + lifetime;
}
InProcessGnsRoom::~InProcessGnsRoom() = default;
core::Result<std::shared_ptr<GnsRoomEndpoint>> InProcessGnsRoom::attach(PeerIndex slot, std::string identity,
    std::optional<GnsTurnCredentials> turn) {
    auto room = impl_->room;
    std::lock_guard guard(room->mutex);
    if (!room->valid) return core::Result<std::shared_ptr<GnsRoomEndpoint>>::failure(room_error("EAWR-NET-1765", "Invalid room options."));
    if (!room->healthy()) return core::Result<std::shared_ptr<GnsRoomEndpoint>>::failure(room_error("EAWR-NET-1760", "Room is closed or expired."));
    if (slot >= room->roster.capacity || !identity_valid(identity) || room->banned.contains(identity))
        return core::Result<std::shared_ptr<GnsRoomEndpoint>>::failure(room_error("EAWR-NET-1761", "Invalid room member."));
    const auto& old = room->roster.slots[slot];
    const bool reattach = room->roster.state == GnsRoomState::locked && old.state == GnsRoomSlotState::held && old.identity == identity;
    if (!reattach && (room->roster.state != GnsRoomState::open || old.state != GnsRoomSlotState::vacant
        || (slot != 0 && room->roster.slots[0].state != GnsRoomSlotState::attached)))
        return core::Result<std::shared_ptr<GnsRoomEndpoint>>::failure(room_error("EAWR-NET-1761", "Room slot cannot be attached."));
    for (PeerIndex p = 0; p < room->roster.capacity; ++p)
        if (p != slot && room->roster.slots[p].identity == identity)
            return core::Result<std::shared_ptr<GnsRoomEndpoint>>::failure(room_error("EAWR-NET-1761", "Room identity is duplicated."));
    room->roster.slots[slot] = {GnsRoomSlotState::attached, std::move(identity)};
    room->turn[slot] = std::move(turn); ++room->generation[slot]; ++room->roster.revision;
    return core::Result<std::shared_ptr<GnsRoomEndpoint>>::success(std::make_shared<Endpoint>(std::move(room), slot));
}
core::Result<void> InProcessGnsRoom::disconnect(PeerIndex slot) {
    auto room = impl_->room;
    std::lock_guard guard(room->mutex);
    if (!room->healthy() || slot >= room->roster.capacity || room->roster.slots[slot].state != GnsRoomSlotState::attached)
        return core::Result<void>::failure(room_error("EAWR-NET-1760", "Room attachment is unavailable."));
    if (slot == 0 && room->roster.state == GnsRoomState::open) room->roster.state = GnsRoomState::closed;
    room->clear(slot, room->roster.state == GnsRoomState::locked ? GnsRoomSlotState::held : GnsRoomSlotState::vacant);
    return core::Result<void>::success();
}
} // namespace eawr::net
