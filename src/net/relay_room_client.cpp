#include "eawr/net/network_introduction.hpp"
#include "relay_client_options.hpp"
#include "relay_directory_codec.hpp"
#include "relay_https.hpp"
#include "relay_renewal.hpp"
#include "relay_tls.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>

// Room member client of slice N3 (docs/net-online-npeer.md section 2/3): a NetworkGnsRoom is one TLS connection to
// the introduction service, owned by one worker thread. Game-thread and GNS-thread calls only touch the copied,
// bounded queues below under the mutex.
namespace eawr::net {
using namespace relay;
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::size_t room_signal_limit = 65536;
// Hostnames in a room credential's TURN list become numeric addresses on the worker thread, as for Paired.
Bytes numeric_room_turn(Bytes bytes, std::stop_token stop) {
    if (bytes.size() < 2 || bytes[0] != 2) return bytes;
    const auto kind = static_cast<Kind>(bytes[1]);
    if (kind != Kind::room_opened && kind != Kind::room_attached && kind != Kind::renewed) return bytes;
    Reader reader{std::span(bytes).subspan(2)};
    auto result = message(kind);
    if (kind == Kind::renewed) {
        const auto remaining = reader.u32(), hard = reader.u32();
        const auto uri = reader.string(512), username = reader.string(256), password = reader.string(256);
        const auto lifetime = reader.u32();
        if (!reader.done() || uri.empty() || !turn_uri_list_valid(uri)) return bytes;
        const auto numeric = numeric_turn_uris(uri, {}, std::chrono::seconds(5), stop);
        if (numeric == uri) return bytes;
        u32(result, remaining); u32(result, hard); string(result, numeric);
        string(result, numeric.empty() ? "" : username); string(result, numeric.empty() ? "" : password);
        u32(result, numeric.empty() ? 0 : lifetime);
        return result;
    }
    const auto slot = reader.u32();
    const auto capability = reader.string(32), identity = reader.string(gns_identity_max_bytes), alias = reader.string(4);
    const auto uri = reader.string(512), username = reader.string(256), password = reader.string(256);
    const auto lifetime = reader.u32(), remaining = reader.u32(), hard = reader.u32();
    if (!reader.done() || uri.empty() || !turn_uri_list_valid(uri)) return bytes;
    const auto numeric = numeric_turn_uris(uri, {}, std::chrono::seconds(5), stop);
    if (numeric == uri) return bytes;
    u32(result, slot); string(result, capability); string(result, identity); string(result, alias); string(result, numeric);
    string(result, numeric.empty() ? "" : username); string(result, numeric.empty() ? "" : password);
    u32(result, numeric.empty() ? 0 : lifetime); u32(result, remaining); u32(result, hard);
    return result;
}
core::Result<std::shared_ptr<NetworkGnsRoom>> room_failure(core::Diagnostic diagnostic) {
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::failure(std::move(diagnostic));
}
} // namespace

struct NetworkGnsRoom::Impl {
    NetworkIntroductionOptions options;
    Bytes request;
    std::string room_reference;
    std::uint64_t repair_requested{}, repair_inflight{}, repair_completed{};
    Kind request_kind{};
    std::string expected_identity;
    std::optional<std::uint32_t> expected_slot; // a directory admission already named the seat
    mutable std::mutex mutex;
    std::condition_variable condition;
    PeerIndex slot{};
    std::string capability, alias;
    GnsRoomRoster roster;
    std::optional<GnsTurnCredentials> turn;
    GnsRoomLease lease;
    Clock::time_point hard_ends{};
    bool renewable{true};
    std::optional<core::Diagnostic> failure;
    bool admitted{}, finished{}, leaving{};
    std::deque<Bytes> outbound;
    std::deque<GnsRoomSignal> inbound;
    std::size_t outbound_bytes{}, inbound_bytes{};
    std::uint64_t dropped{};
    struct RosterLock {
        std::uint32_t seats{}, revision{};
        std::optional<unsigned> reason;
        Clock::time_point deadline;
    };
    std::optional<RosterLock> roster_lock;
    std::string last_refusal;
    std::pair<std::uint64_t, std::string> renewal_failure;
    bool renew_wanted{};
    unsigned renew_attempts{};
    Clock::time_point renew_retry{};
    std::jthread worker;
    Impl(NetworkIntroductionOptions o, Bytes bytes, std::string identity)
        : options(std::move(o)), request(std::move(bytes)), request_kind(static_cast<Kind>(request[1])), expected_identity(std::move(identity)) {
        Reader reader{std::span(request).subspan(2)};
        room_reference = reader.string(32);
    }
    ~Impl() {
        {
            std::lock_guard lock(mutex);
            worker.request_stop();
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
    }
    void start() { worker = std::jthread([this](std::stop_token stop) { run(stop); }); }
    core::Result<void> wait() {
        std::unique_lock lock(mutex);
        if (!condition.wait_for(lock, options.timeout, [&] { return admitted || failure; })) failure = error(1528, "Relay room request timed out");
        if (failure) return core::Result<void>::failure(*failure);
        return core::Result<void>::success();
    }
    void fail(core::Diagnostic diagnostic) {
        std::lock_guard lock(mutex);
        if (!failure) failure = std::move(diagnostic);
        condition.notify_all();
    }
    static bool visible(GnsRoomSlotState state) {
        return state == GnsRoomSlotState::attached || state == GnsRoomSlotState::held || state == GnsRoomSlotState::left;
    }
    // The helpers below run under the mutex.
    void malformed() { if (!failure) failure = error(1521, "Malformed relay room response"); }
    void admission(Reader& reader) {
        const auto assigned = reader.u32();
        auto cap = reader.string(32);
        auto identity = reader.string(gns_identity_max_bytes);
        auto room_alias = reader.string(4);
        GnsTurnCredentials credentials{reader.string(512), reader.string(256), reader.string(256), std::chrono::seconds(reader.u32())};
        const auto remaining = reader.u32(), hard = reader.u32();
        const bool host = request_kind == Kind::room_open;
        if (!reader.done() || assigned >= max_match_seats || !code_valid(cap) || identity != expected_identity
            || (!room_alias.empty() && (!host || !alias_valid(room_alias))) || !turn_uri_list_valid(credentials.uri)
            || (!credentials.uri.empty() && (credentials.username.empty() || credentials.password.empty() || credentials.lifetime.count() <= 0))
            || remaining == 0 || hard < remaining || (host && assigned != 0) || (!host && request_kind == Kind::room_attach && assigned == 0)
            || (expected_slot && *expected_slot != assigned)) {
            malformed(); return;
        }
        slot = assigned; capability = std::move(cap);
        // Reattach deliberately omits the host-only alias in its reply.
        // Retain the code already learned from Open across that refresh.
        if (request_kind == Kind::room_open || !room_alias.empty()) alias = std::move(room_alias);
        turn.reset();
        if (!credentials.uri.empty()) turn = std::move(credentials);
        repair_completed = repair_inflight;
        const auto now = Clock::now();
        lease.expires = now + std::chrono::milliseconds(remaining); hard_ends = now + std::chrono::milliseconds(hard);
        renewable = hard > remaining; admitted = true;
    }
    void roster_frame(Reader& reader) {
        GnsRoomRoster next;
        next.revision = reader.u32(); next.capacity = reader.u32();
        const auto state = reader.u32(), count = reader.u32();
        if (!reader.valid || next.capacity < 2 || next.capacity > max_match_seats || count != next.capacity || state > 1) { malformed(); return; }
        next.state = state == 0 ? GnsRoomState::open : GnsRoomState::locked;
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto raw = reader.u32();
            auto identity = reader.string(gns_identity_max_bytes);
            if (!reader.valid || raw > static_cast<std::uint32_t>(GnsRoomSlotState::left)) { malformed(); return; }
            const auto slot_state = static_cast<GnsRoomSlotState>(raw);
            if (visible(slot_state) != !identity.empty() || (!identity.empty() && !identity_valid(identity))) { malformed(); return; }
            next.slots[i] = {slot_state, std::move(identity)};
        }
        if (!reader.done()) { malformed(); return; }
        if (roster.capacity != 0 && next.revision < roster.revision) return; // a roster never steps back
        roster = std::move(next);
    }
    void response(const Bytes& bytes) {
        std::lock_guard lock(mutex);
        if (bytes.size() < 2 || bytes[0] != 2) { malformed(); condition.notify_all(); return; }
        Reader reader{std::span(bytes).subspan(2)};
        const auto kind = static_cast<Kind>(bytes[1]);
        if (kind == Kind::error || kind == Kind::room_closed) {
            const auto number = reader.u32();
            if (!reader.done() || !room_error_number(number)) malformed();
            else if (!failure) failure = roster_lock && number == 1521
                ? error(1764, "The relay does not support roster-lock protocol 1. Update the relay service before starting this build.")
                : error(number, kind == Kind::error ? "Relay service rejected the room request" : "The room was closed by the service");
        } else if (!admitted) {
            const auto expected = request_kind == Kind::room_open ? Kind::room_opened : Kind::room_attached;
            if (kind == expected) admission(reader); else malformed();
        } else if (kind == Kind::room_roster) {
            roster_frame(reader);
        } else if (kind == Kind::room_roster_locked) {
            const auto revision = reader.u32(), seats = reader.u32(), reason = reader.u32();
            if (!reader.done() || !roster_lock || revision != roster_lock->revision || seats != roster_lock->seats
                || (reason && !room_error_number(reason))) malformed();
            else roster_lock->reason = reason;
        } else if (kind == Kind::room_forwarded) {
            const auto from = reader.u32();
            auto identity = reader.string(gns_identity_max_bytes);
            const auto blob = reader.bytes;
            // The service sets source slot and identity; the roster it pushed first must agree.
            if (!reader.valid || from >= roster.capacity || from == slot || !identity_valid(identity) || blob.empty() || blob.size() > room_signal_limit
                || roster.slots[from].identity != identity) malformed();
            else if (inbound.size() >= options.queue_frames || blob.size() > options.queue_bytes - inbound_bytes) ++dropped; // lost to ICE retries, never the member
            else { inbound_bytes += blob.size(); inbound.push_back({from, std::move(identity), {blob.begin(), blob.end()}}); }
        } else if (kind == Kind::room_ack) {
            // Empty: done. With a reason: the host operation was not (fully) applied; the room and this connection stay.
            if (reader.bytes.empty()) { if (leaving) finished = true; }
            else {
                const auto reason = reader.u32();
                if (!reader.done() || !room_error_number(reason)) malformed(); else last_refusal = "EAWR-NET-" + std::to_string(reason);
            }
        } else if (kind == Kind::expiring) {
            const auto warning = parse_expiring(reader);
            if (!warning) malformed();
            else {
                const auto now = Clock::now();
                lease.expires = now + warning->remaining; hard_ends = now + warning->hard_remaining; renewable = warning->renewable;
                if (warning->renewable && renew_attempts < 6) { renew_wanted = true; renew_retry = now; }
            }
        } else if (kind == Kind::renewed) {
            const auto renewed = parse_renewed(reader);
            if (!renewed) malformed();
            else {
                const auto now = Clock::now();
                lease.expires = now + renewed->remaining; hard_ends = now + renewed->hard_remaining;
                renewable = renewed->hard_remaining > renewed->remaining; renew_wanted = false; renew_attempts = 0;
                // Live GNS connections cannot switch credentials; connections dialed from now on use the fresh ones.
                if (!renewed->turn.uri.empty()) turn = renewed->turn;
            }
        } else if (kind == Kind::renew_refused) {
            const auto refusal = parse_refused(reader);
            if (!refusal) malformed();
            else if (refusal->reason == renew_refused_maximum) {
                ++renewal_failure.first; renewal_failure.second = "EAWR-NET-1732";
                renew_wanted = false; renewable = false;
            }
            else if (refusal->reason == renew_refused_credentials) {
                ++renewal_failure.first; renewal_failure.second = "EAWR-NET-1733";
                renew_wanted = renew_attempts < 6; renew_retry = Clock::now() + options.renewal_retry;
            }
            else renew_wanted = false; // Early: another member's renewal is already in progress.
        } else malformed();
        condition.notify_all();
    }
    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            run_connection(stop);
            // A repair request wakes this same TLS owner, even after an outage.
            // No join, DNS, TLS or provider request runs on the game/GNS thread.
            std::unique_lock lock(mutex);
            condition.wait(lock, [&] { return stop.stop_requested() || repair_requested > repair_inflight; });
            if (stop.stop_requested()) return;
            repair_inflight = repair_requested;
            request = message(Kind::room_reattach);
            string(request, room_reference); string(request, capability); string(request, expected_identity);
            request_kind = Kind::room_reattach; expected_slot = slot;
            failure.reset(); admitted = false; finished = leaving = false;
            turn.reset(); outbound.clear(); inbound.clear(); outbound_bytes = inbound_bytes = 0;
            renew_wanted = false; renew_attempts = 0;
        }
    }
    void run_connection(std::stop_token stop) {
        const auto deadline = Clock::now() + options.timeout;
        auto ctx = client_context(options.ca_file, options.certificate_sha256);
        if (!ctx) { fail(ctx.error()); return; }
        const auto handle = connect_service(options.address, options.port, deadline, stop);
        if (handle == socket::invalid) { fail(error(1519, "Relay service connection failed")); return; }
        TlsStream stream(std::move(ctx).value(), handle, false, options.certificate_name,
            options.certificate_sha256, options.queue_bytes, options.queue_frames);
        if (!stream.queue(request)) { fail(error(1526, "Relay initial queue failed")); return; }
        auto heartbeat = Clock::now();
        while (!stop.stop_requested()) {
            stream.pump();
            if (auto bytes = stream.receive()) response(numeric_room_turn(std::move(*bytes), stop));
            {
                std::lock_guard lock(mutex);
                if (failure || finished || repair_requested > repair_inflight) return;
                if (!admitted && Clock::now() >= deadline) { failure = error(1528, "Relay room request timed out"); condition.notify_all(); return; }
                // One owner performs all TLS I/O; GNS-thread callbacks never block on the network.
                if (admitted && !outbound.empty() && stream.queue(outbound.front())) {
                    outbound_bytes -= outbound.front().size(); outbound.pop_front();
                }
                if (renew_wanted && renew_attempts < 6 && Clock::now() < lease.expires && Clock::now() >= renew_retry && stream.queue(message(Kind::renew))) {
                    renew_wanted = false; ++renew_attempts;
                }
                if (admitted && Clock::now() - heartbeat >= std::chrono::seconds(5) && stream.queue(message(Kind::ping))) heartbeat = Clock::now();
            }
            if (stream.failed()) { fail(stream.failure().value_or(error(stream.ready() ? 1528 : 1519, "Relay TLS connection failed"))); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    core::Result<void> queue(Bytes bytes, bool closing = false) {
        std::lock_guard lock(mutex);
        if (failure) return core::Result<void>::failure(*failure);
        if (!admitted) return core::Result<void>::failure(error(1524, "Relay room is not attached"));
        if (leaving) return core::Result<void>::failure(error(1528, "Relay room member has left"));
        if (outbound.size() >= options.queue_frames || bytes.size() > options.queue_bytes - outbound_bytes)
            return core::Result<void>::failure(error(1526, "Relay client send queue is full"));
        outbound_bytes += bytes.size(); outbound.push_back(std::move(bytes));
        if (closing) leaving = true;
        return core::Result<void>::success();
    }
};

NetworkGnsRoom::NetworkGnsRoom(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NetworkGnsRoom::~NetworkGnsRoom() = default;
bool NetworkGnsRoom::available() { return true; }
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::connect(NetworkIntroductionOptions options, Bytes request,
        std::string identity, std::optional<std::uint32_t> expected_slot) {
    if (!client_options_valid(options, true)) return room_failure(error(1519, "Invalid relay client options"));
    auto impl = std::make_unique<Impl>(std::move(options), std::move(request), std::move(identity));
    impl->expected_slot = expected_slot;
    auto room = std::shared_ptr<NetworkGnsRoom>(new NetworkGnsRoom(std::move(impl)));
    room->impl_->start();
    auto ready = room->impl_->wait();
    if (!ready) return room_failure(ready.error());
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::success(std::move(room));
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::open(NetworkIntroductionOptions options, const OnlineRoomPublication& room,
        std::string principal, std::string identity, bool public_alias) {
    if (!identity_valid(identity) || !code_valid(room.room) || !code_valid(room.ownership) || !code_valid(principal))
        return room_failure(error(1519, "Invalid relay room open request"));
    auto request = message(Kind::room_open);
    string(request, room.room); string(request, room.ownership); string(request, principal); string(request, identity); u32(request, public_alias ? 1 : 0);
    return connect(std::move(options), std::move(request), std::move(identity), 0);
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::attach(NetworkIntroductionOptions options, const OnlineRoomAdmission& admission,
        std::string identity) {
    if (!identity_valid(identity) || !code_valid(admission.room) || !code_valid(admission.member_capability)
        || admission.slot == 0 || admission.slot >= max_match_seats)
        return room_failure(error(1519, "Invalid relay room attach request"));
    auto request = message(Kind::room_attach);
    string(request, admission.room); string(request, admission.member_capability); string(request, identity);
    return connect(std::move(options), std::move(request), std::move(identity), admission.slot);
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::attach_alias(NetworkIntroductionOptions options, std::string alias, std::string identity) {
    if (!identity_valid(identity) || !alias_valid(alias)) return room_failure(error(1519, "Invalid relay room alias request"));
    auto request = message(Kind::room_attach);
    string(request, alias); string(request, ""); string(request, identity);
    return connect(std::move(options), std::move(request), std::move(identity), std::nullopt);
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::reattach(NetworkIntroductionOptions options, std::string room, std::string capability,
        std::string identity) {
    if (!identity_valid(identity) || !code_valid(room) || !code_valid(capability)) return room_failure(error(1519, "Invalid relay room reattach request"));
    auto request = message(Kind::room_reattach);
    string(request, room); string(request, capability); string(request, identity);
    return connect(std::move(options), std::move(request), std::move(identity), std::nullopt);
}
std::uint64_t NetworkGnsRoom::dropped_signals() const { std::lock_guard lock(impl_->mutex); return impl_->dropped; }
std::string NetworkGnsRoom::last_refusal() const { std::lock_guard lock(impl_->mutex); return impl_->last_refusal; }
std::string NetworkGnsRoom::alias() const { std::lock_guard lock(impl_->mutex); return impl_->alias; }
std::string NetworkGnsRoom::capability() const { std::lock_guard lock(impl_->mutex); return impl_->capability; }
PeerIndex NetworkGnsRoom::local_slot() const { std::lock_guard lock(impl_->mutex); return impl_->slot; }
GnsRoomRoster NetworkGnsRoom::roster() const { std::lock_guard lock(impl_->mutex); return impl_->roster; }
std::optional<GnsTurnCredentials> NetworkGnsRoom::turn_credentials() const { std::lock_guard lock(impl_->mutex); return impl_->turn; }
GnsRoomLease NetworkGnsRoom::lease() const { std::lock_guard lock(impl_->mutex); return impl_->lease; }
core::Result<void> NetworkGnsRoom::status() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->failure) return core::Result<void>::failure(*impl_->failure);
    if (impl_->leaving) return core::Result<void>::failure(error(1528, "Relay room member has left"));
    return core::Result<void>::success();
}
core::Result<void> NetworkGnsRoom::send(PeerIndex slot, std::span<const std::uint8_t> blob) {
    if (blob.empty() || blob.size() > room_signal_limit || slot >= max_match_seats || slot == local_slot())
        return core::Result<void>::failure(error(1521, "Invalid relay room signal"));
    auto bytes = message(Kind::room_signal); u32(bytes, slot);
    bytes.insert(bytes.end(), blob.begin(), blob.end());
    return impl_->queue(std::move(bytes));
}
core::Result<std::optional<GnsRoomSignal>> NetworkGnsRoom::poll() {
    std::lock_guard lock(impl_->mutex);
    if (impl_->failure) return core::Result<std::optional<GnsRoomSignal>>::failure(*impl_->failure);
    if (impl_->inbound.empty()) return core::Result<std::optional<GnsRoomSignal>>::success({});
    auto signal = std::move(impl_->inbound.front()); impl_->inbound.pop_front(); impl_->inbound_bytes -= signal.bytes.size();
    return core::Result<std::optional<GnsRoomSignal>>::success(std::move(signal));
}
core::Result<bool> NetworkGnsRoom::repair_attachment(std::uint64_t epoch) {
    std::lock_guard lock(impl_->mutex);
    if (epoch == 0 || epoch < impl_->repair_requested || impl_->capability.empty() || !code_valid(impl_->room_reference)
        || impl_->leaving) {
        return core::Result<bool>::failure(error(1791, "Room cannot reattach for this repair epoch"));
    }
    if (epoch > impl_->repair_requested) {
        // A committed GNS match can precede this TLS owner's Lock roster.
        // Let a live owner finish receiving it before replacing that connection.
        // On an outage its cached roster may stay open forever: reattach then
        // validates the locked room and capability at the service instead.
        if (impl_->roster.state == GnsRoomState::open && !impl_->failure)
            return core::Result<bool>::success(false);
        impl_->repair_requested = epoch; impl_->condition.notify_all();
        return core::Result<bool>::success(false);
    }
    if (impl_->repair_completed == epoch) return core::Result<bool>::success(true);
    if (impl_->repair_inflight == epoch && impl_->failure)
        return core::Result<bool>::failure(*impl_->failure);
    return core::Result<bool>::success(false);
}
core::Result<void> NetworkGnsRoom::leave() { return impl_->queue(message(Kind::room_leave), true); }
core::Result<void> NetworkGnsRoom::release(PeerIndex slot) {
    if (local_slot() != 0 || slot == 0 || slot >= max_match_seats) return core::Result<void>::failure(error(room_capability, "Only the host releases a seat"));
    auto bytes = message(Kind::room_release); u32(bytes, slot); return impl_->queue(std::move(bytes));
}
core::Result<void> NetworkGnsRoom::kick(PeerIndex slot) {
    if (local_slot() != 0 || slot == 0 || slot >= max_match_seats) return core::Result<void>::failure(error(room_capability, "Only the host removes a seat"));
    auto bytes = message(Kind::room_kick); u32(bytes, slot); return impl_->queue(std::move(bytes));
}
core::Result<void> NetworkGnsRoom::lock() {
    if (local_slot() != 0) return core::Result<void>::failure(error(room_capability, "Only the host starts the match"));
    return impl_->queue(message(Kind::room_lock));
}
core::Result<bool> NetworkGnsRoom::lock_roster(std::uint32_t seats, std::uint64_t revision) {
    using Result = core::Result<bool>;
    std::lock_guard lock(impl_->mutex);
    if (impl_->failure) return Result::failure(*impl_->failure);
    if (impl_->slot != 0 || !impl_->admitted || impl_->leaving || revision == 0 || revision > UINT32_MAX)
        return Result::failure(error(room_capability, "Only an attached host can lock the roster."));
    if (!impl_->roster_lock) {
        auto bytes = message(Kind::room_lock_roster); u32(bytes, static_cast<std::uint32_t>(revision)); u32(bytes, seats);
        if (impl_->outbound.size() >= impl_->options.queue_frames || bytes.size() > impl_->options.queue_bytes - impl_->outbound_bytes)
            return Result::failure(error(1526, "Relay client send queue is full"));
        impl_->roster_lock = Impl::RosterLock{seats, static_cast<std::uint32_t>(revision), {}, Clock::now() + impl_->options.timeout};
        impl_->outbound_bytes += bytes.size(); impl_->outbound.push_back(std::move(bytes));
    }
    auto& pending = *impl_->roster_lock;
    if (pending.seats != seats || pending.revision != revision)
        return Result::failure(error(1764, "A different roster lock is still pending."));
    if (pending.reason) {
        if (*pending.reason) {
            const auto reason = *pending.reason;
            impl_->roster_lock.reset();
            return Result::failure(error(reason, "Start raced a player joining or leaving. Wait for the player list and connections, then accept again."));
        }
        std::uint32_t attached{};
        for (PeerIndex p = 0; p < impl_->roster.capacity; ++p)
            if (impl_->roster.slots[p].state == GnsRoomSlotState::attached) attached |= 1U << p;
        if (impl_->roster.state != GnsRoomState::locked || attached != seats || impl_->roster.revision <= revision)
            return Result::failure(error(1764, "The relay confirmed a different player list for Start."));
        return Result::success(true);
    }
    if (Clock::now() >= pending.deadline) {
        impl_->failure = error(1764, "Timed out confirming the locked player list with the relay.");
        return Result::failure(*impl_->failure);
    }
    return Result::success(false);
}
std::pair<std::uint64_t, std::string> NetworkGnsRoom::renewal_failure() const {
    std::lock_guard lock(impl_->mutex); return impl_->renewal_failure;
}
} // namespace eawr::net
