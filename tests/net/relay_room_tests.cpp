// Contracts of the N-peer room scope (slice N3, docs/net-online-npeer.md section 2 and 4).
// Part A drives RoomService and the real Directory with a fake sink and a fake clock (no sockets, no wall time).
// Part B runs the same service behind the TLS RelayServer on loopback with NetworkOnlineLobby and NetworkGnsRoom.
#include "eawr/net/network_introduction.hpp"
#include "eawr/net/relay_server.hpp"
#include "relay_directory.hpp"
#include "relay_prefix_budget.hpp"
#include "relay_renewal.hpp"
#include "relay_room.hpp"
#include "relay_tls.hpp"
#include "relay_turn_refresh.hpp"

#include <atomic>
#include <deque>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace {
using namespace eawr::net;
using namespace eawr::net::relay;
using namespace std::chrono_literals;
using Time = RoomService::Time;
void expect(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
template<class F> void until(F predicate, const char* reason, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(reason);
        std::this_thread::sleep_for(2ms);
    }
}
Kind kind_of(const Bytes& bytes) { expect(bytes.size() >= 2 && bytes[0] == 2, "Frame header"); return static_cast<Kind>(bytes[1]); }
unsigned number_of(const Bytes& bytes) {
    expect(bytes.size() == 6 && kind_of(bytes) == Kind::error, "Typed error frame");
    return read_u32(std::span(bytes).subspan(2));
}
unsigned closed_reason(const Bytes& bytes) {
    expect(bytes.size() == 6 && kind_of(bytes) == Kind::room_closed, "room_closed frame");
    return read_u32(std::span(bytes).subspan(2));
}

// ---- wire helpers --------------------------------------------------------------------------------------------------------
OnlineRoomMetadata info(std::uint32_t capacity) {
    OnlineRoomMetadata value; value.name = "Guest room"; value.host_name = "Host";
    value.build = "build-hash"; value.protocol = "protocol-tag"; value.content = "content-tag";
    value.mod = "mod-tag"; value.mode = "mode-from-data"; value.map = "map-from-data"; value.capacity = capacity; return value;
}
Bytes open_principal(const std::string& resume = {}) { auto bytes = message(Kind::directory_open); string(bytes, resume); return bytes; }
Bytes publish_request(OnlineRoomMetadata value, std::string password = {}) {
    auto bytes = message(Kind::directory_publish); string(bytes, ""); metadata(bytes, value); string(bytes, password); string(bytes, ""); string(bytes, "");
    return bytes;
}
Bytes heartbeat_request(const std::string& room, const std::string& ownership, OnlineRoomMetadata value) {
    auto bytes = message(Kind::directory_heartbeat); string(bytes, room); string(bytes, ownership); metadata(bytes, value); return bytes;
}
Bytes join_request(const std::string& room, std::string password, std::string name) {
    auto bytes = message(Kind::directory_join); string(bytes, room); string(bytes, password); string(bytes, name); return bytes;
}
Bytes room_open_request(const std::string& room, const std::string& ownership, const std::string& principal, const std::string& identity, std::uint32_t alias = 1) {
    auto bytes = message(Kind::room_open); string(bytes, room); string(bytes, ownership); string(bytes, principal); string(bytes, identity); u32(bytes, alias); return bytes;
}
Bytes attach_request(Kind kind, const std::string& first, const std::string& capability, const std::string& identity) {
    auto bytes = message(kind); string(bytes, first); string(bytes, capability); string(bytes, identity); return bytes;
}
Bytes signal_request(std::uint32_t destination, const Bytes& blob) {
    auto bytes = message(Kind::room_signal); u32(bytes, destination); bytes.insert(bytes.end(), blob.begin(), blob.end()); return bytes;
}
Bytes slot_request(Kind kind, std::uint32_t slot) { auto bytes = message(kind); u32(bytes, slot); return bytes; }
struct Attached { std::uint32_t slot{}; std::string capability, identity, alias; GnsTurnCredentials turn; std::uint32_t remaining{}, hard{}; };
Attached attached_of(const Bytes& bytes) {
    expect(kind_of(bytes) == Kind::room_opened || kind_of(bytes) == Kind::room_attached, "Attached frame kind");
    Reader reader{std::span(bytes).subspan(2)}; Attached a;
    a.slot = reader.u32(); a.capability = reader.string(32); a.identity = reader.string(31); a.alias = reader.string(4);
    a.turn.uri = reader.string(512); a.turn.username = reader.string(256); a.turn.password = reader.string(256); a.turn.lifetime = std::chrono::seconds(reader.u32());
    a.remaining = reader.u32(); a.hard = reader.u32();
    expect(reader.done() && code_valid(a.capability), "Attached layout");
    return a;
}
struct RosterView { std::uint32_t revision{}, capacity{}, state{}; std::vector<std::pair<std::uint32_t, std::string>> slots; };
RosterView roster_of(const Bytes& bytes) {
    expect(kind_of(bytes) == Kind::room_roster, "Roster frame kind");
    Reader reader{std::span(bytes).subspan(2)}; RosterView r;
    r.revision = reader.u32(); r.capacity = reader.u32(); r.state = reader.u32(); const auto count = reader.u32();
    expect(count == r.capacity && count <= 9, "Roster lists exactly the capacity");
    for (std::uint32_t i = 0; i < count; ++i) { const auto state = reader.u32(); r.slots.emplace_back(state, reader.string(31)); }
    expect(reader.done(), "Roster layout");
    return r;
}
struct Forwarded { std::uint32_t slot{}; std::string identity; Bytes blob; };
Bytes got(std::optional<Bytes> value) { expect(value.has_value(), "Expected frame was not sent"); return std::move(*value); }
std::optional<Refused> refused_in(const Bytes& bytes) { return parse_refused(Reader{std::span(bytes).subspan(2)}); }
std::optional<Expiring> expiring_in(const Bytes& bytes) { return parse_expiring(Reader{std::span(bytes).subspan(2)}); }
std::optional<Renewed> renewed_in(const Bytes& bytes) { return parse_renewed(Reader{std::span(bytes).subspan(2)}); }
Forwarded forwarded_of(const Bytes& bytes) {
    expect(kind_of(bytes) == Kind::room_forwarded, "Forwarded frame kind");
    Reader reader{std::span(bytes).subspan(2)}; Forwarded f;
    f.slot = reader.u32(); f.identity = reader.string(31); f.blob.assign(reader.bytes.begin(), reader.bytes.end());
    return f;
}

// ---- Part A: the state machine with a fake sink --------------------------------------------------------------------------
struct Sink final : RoomSink {
    std::map<std::uint64_t, std::deque<Bytes>> out;
    std::map<std::uint64_t, unsigned> failed;
    std::map<std::uint64_t, Bytes> closed;
    std::set<std::uint64_t> dead;
    std::vector<std::string> revoked, pending, warnings;
    std::map<std::string, std::chrono::seconds> ttl;
    std::vector<std::tuple<std::string, bool, bool>> states;
    std::vector<std::string> alias_pool{"AB12", "CD34", "EF56", "GH78", "JK90", "LM12", "NP34", "QR56", "ST78", "UV90", "WX12", "YZ34"};
    bool refuse_turn{}, manual{};
    Directory* directory{};
    std::set<std::uint64_t> full; // connections whose output queue is full
    bool send(std::uint64_t id, Bytes bytes) override {
        if (dead.contains(id)) return false;
        out[id].push_back(std::move(bytes)); return true;
    }
    bool try_send(std::uint64_t id, Bytes bytes) override {
        if (dead.contains(id) || full.contains(id)) return false;
        out[id].push_back(std::move(bytes)); return true;
    }
    void close(std::uint64_t id, Bytes frame) override { closed[id] = std::move(frame); dead.insert(id); }
    void fail(std::uint64_t id, unsigned reason) override { failed[id] = reason; dead.insert(id); }
    bool request_turn(const std::string& key, std::chrono::seconds lifetime) override {
        if (refuse_turn) return false;
        pending.push_back(key); ttl[key] = lifetime; return true;
    }
    void revoke(const std::string& username) override { revoked.push_back(username); }
    std::optional<std::string> new_alias() override {
        if (alias_pool.empty()) return {};
        auto alias = alias_pool.front(); alias_pool.erase(alias_pool.begin()); return alias;
    }
    void warn(eawr::core::Diagnostic diagnostic) override { warnings.push_back(diagnostic.code); }
    void directory_state(const std::string& room, bool locked, bool closed_state) override {
        states.emplace_back(room, locked, closed_state);
        if (directory) directory->scope_state(room, locked, closed_state);
    }
    std::optional<Bytes> take(std::uint64_t id, Kind kind) {
        auto& queue = out[id];
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if (kind_of(*it) == kind) { auto frame = std::move(*it); queue.erase(it); return frame; }
        return {};
    }
    std::size_t count(std::uint64_t id, Kind kind) {
        std::size_t total{};
        for (const auto& frame : out[id]) if (kind_of(frame) == kind) ++total;
        return total;
    }
    bool warned(const char* code) const { return std::find(warnings.begin(), warnings.end(), code) != warnings.end(); }
};
struct Host {
    std::uint64_t dir{}, conn{};
    std::string principal, room, ownership, prefix, password;
    std::uint32_t capacity{};
    Attached opened;
    bool beating{true};
};
struct Guest {
    std::uint64_t dir{}, conn{};
    std::string principal, capability, prefix, name;
    std::uint32_t slot{}, seat{};
    unsigned refusal{};
    Attached attached;
};
struct World {
    Directory directory{64, 256};
    PrefixBudgets budgets{256};
    Sink sink;
    std::unique_ptr<RoomService> rooms;
    Time now{};
    std::uint64_t next{};
    std::map<std::uint64_t, std::string> prefix_of;
    std::map<std::string, Host> hosts;
    unsigned prefixes{};
    explicit World(RoomOptions options = {}) {
        options.aliases = true;
        sink.directory = &directory;
        rooms = std::make_unique<RoomService>(options, sink, directory, budgets);
        directory.set_room_scope(rooms.get());
    }
    std::string fresh_prefix() { return "198.51.100." + std::to_string(++prefixes); }
    std::uint64_t add(const std::string& prefix) {
        auto* budget = budgets.admit(prefix, now, 100000, 100000);
        expect(budget != nullptr, "Prefix admitted");
        ++budget->live; // as the server does on accept: a prefix with live connections is never evicted
        const auto id = ++next; prefix_of[id] = prefix; return id;
    }
    Bytes dir(std::uint64_t id, const Bytes& bytes) { return directory.request(id, prefix_of.at(id), bytes, now, budgets); }
    std::string principal_of(std::uint64_t id, const std::string& resume = {}) {
        const auto reply = dir(id, open_principal(resume));
        expect(kind_of(reply) == Kind::directory_opened, "Directory principal opened");
        Reader reader{std::span(reply).subspan(2)}; return reader.string(32);
    }
    // A completed provider call: a distinct credential per request key.
    void settle() {
        for (int round = 0; round < 4 && !sink.manual && !sink.pending.empty(); ++round) {
            auto keys = std::move(sink.pending); sink.pending.clear();
            for (const auto& key : keys) {
                GnsTurnCredentials credentials{"turn:203.0.113.5:3478", "user-" + key, "pass-" + key, sink.ttl.at(key)};
                rooms->turn_done(key, credentials, {}, now);
            }
        }
    }
    void reap() {
        auto dead = std::move(sink.dead); sink.dead.clear();
        for (const auto id : dead) rooms->disconnect(id, now);
    }
    void settle_all() { reap(); settle(); reap(); }
    // Time moves in steps below the directory lease; the hosts' staging screens keep their rooms alive.
    void tick(std::chrono::milliseconds duration, bool poll = true) {
        while (duration.count() > 0) {
            const auto step = std::min(duration, std::chrono::milliseconds(10000));
            now += step; duration -= step;
            for (auto& [room, host] : hosts) {
                if (!host.beating) continue;
                auto value = info(host.capacity);
                const auto reply = dir(host.dir, heartbeat_request(host.room, host.ownership, value));
                (void)reply; // a locked or closed scope answers like any heartbeat; flags are the service's
            }
            directory.expire(now); budgets.refresh(now);
            if (poll) rooms->poll(now);
            settle_all();
        }
    }
    Host& host(std::uint32_t capacity = 4, const std::string& password = {}, std::uint32_t alias = 1, const std::string& identity = "host-id") {
        Host h; h.prefix = fresh_prefix(); h.capacity = capacity; h.password = password;
        h.dir = add(h.prefix); h.principal = principal_of(h.dir);
        const auto published = dir(h.dir, publish_request(info(capacity), password));
        expect(kind_of(published) == Kind::directory_published, "Room published");
        Reader reader{std::span(published).subspan(2)}; h.room = reader.string(32); h.ownership = reader.string(32);
        h.conn = add(h.prefix);
        rooms->request(h.conn, h.prefix, room_open_request(h.room, h.ownership, h.principal, identity, alias), now);
        settle_all();
        if (auto frame = sink.take(h.conn, Kind::room_opened)) h.opened = attached_of(*frame);
        auto& stored = hosts[h.room]; stored = h; return stored;
    }
    Guest guest(const Host& h, const std::string& name, std::string prefix = {}) {
        Guest g; g.name = name; g.prefix = prefix.empty() ? fresh_prefix() : prefix;
        g.dir = add(g.prefix); g.principal = principal_of(g.dir);
        const auto reply = dir(g.dir, join_request(h.room, h.password, name));
        if (kind_of(reply) == Kind::error) { g.refusal = number_of(reply); return g; }
        expect(kind_of(reply) == Kind::directory_admitted, "Admission");
        Reader reader{std::span(reply).subspan(2)}; reader.string(32); g.seat = reader.u32(); reader.string(32);
        g.slot = reader.u32(); g.capability = reader.string(32);
        expect(reader.done() && code_valid(g.capability), "Admission carries slot and member capability");
        return g;
    }
    // Attaches with the directory reservation; the connection id of the member is kept in g.conn.
    bool attach(Guest& g, const Host& h, const std::string& identity) {
        g.conn = add(g.prefix);
        rooms->request(g.conn, g.prefix, attach_request(Kind::room_attach, h.room, g.capability, identity), now);
        settle_all();
        if (auto frame = sink.take(g.conn, Kind::room_attached)) { g.attached = attached_of(*frame); return true; }
        return false;
    }
    Guest member(const Host& h, const std::string& name, const std::string& identity) {
        auto g = guest(h, name); expect(g.refusal == 0, "Reservation granted");
        expect(attach(g, h, identity), "Member attached"); return g;
    }
    std::optional<Attached> alias_attach(const std::string& alias, const std::string& identity, std::uint64_t* conn = nullptr, const std::string& prefix = {}) {
        const auto p = prefix.empty() ? fresh_prefix() : prefix;
        const auto id = add(p); if (conn) *conn = id;
        rooms->request(id, p, attach_request(Kind::room_attach, alias, "", identity), now);
        settle_all();
        if (auto frame = sink.take(id, Kind::room_attached)) return attached_of(*frame);
        return {};
    }
    RosterView roster(std::uint64_t id) {
        auto& queue = sink.out[id];
        for (auto it = queue.rbegin(); it != queue.rend(); ++it) if (kind_of(*it) == Kind::room_roster) return roster_of(*it);
        throw std::runtime_error("No roster was pushed");
    }
    void send(std::uint64_t id, const std::string& prefix, const Bytes& bytes) { rooms->request(id, prefix, bytes, now); }
    unsigned failure_of(std::uint64_t id) { const auto it = sink.failed.find(id); return it == sink.failed.end() ? 0 : it->second; }
    bool dead(std::uint64_t id) const { return sink.failed.contains(id) || sink.closed.contains(id); }
};

void formation_and_routing() {
    World w;
    auto& h = w.host(5);
    expect(h.opened.slot == 0 && h.opened.identity == "host-id" && alias_valid(h.opened.alias), "The host holds slot 0 and a public alias");
    expect(h.opened.turn.uri == "turn:203.0.113.5:3478" && !h.opened.turn.username.empty(), "The host gets its own TURN credential");
    // F2: the credential covers the room's hard maximum (24 h) plus the allocation margin.
    expect(h.opened.turn.lifetime == 86400s + 60s, "Credential lifetime covers the hard maximum plus the margin");
    expect(h.opened.remaining == 7200000 && h.opened.hard == 86400000, "The lease starts at room open");
    auto first = w.roster(h.conn);
    expect(first.capacity == 5 && first.state == 0 && first.slots[0] == std::pair<std::uint32_t, std::string>{2, "host-id"}
        && first.slots[1].first == 0 && first.slots[1].second.empty(), "Roster shows the host attached and the rest vacant, no addresses");
    auto g1 = w.guest(h, "Guest1");
    expect(g1.refusal == 0 && g1.slot == 1 && g1.capability != h.opened.capability, "The directory admission reserves the lowest slot and hands out a capability");
    expect(w.roster(h.conn).slots[1].first == 1 && w.roster(h.conn).slots[1].second.empty(), "A reservation is visible without an identity");
    auto g2 = w.guest(h, "Guest2");
    expect(g2.slot == 2, "Slots follow admission order");
    expect(w.attach(g1, h, "peer-1") && w.attach(g2, h, "peer-2"), "Both guests attach");
    expect(g1.attached.slot == 1 && g2.attached.slot == 2 && g1.attached.identity == "peer-1" && g1.attached.alias.empty(), "Attach reply carries the slot");
    const std::set<std::string> users{h.opened.turn.username, g1.attached.turn.username, g2.attached.turn.username};
    expect(users.size() == 3 && !g1.attached.turn.password.empty() && w.sink.ttl.size() == 3, "One TURN credential per member");
    const auto settled = w.roster(g1.conn);
    expect(settled.slots[0].second == "host-id" && settled.slots[1].second == "peer-1" && settled.slots[2].second == "peer-2"
        && settled.slots[2].first == 2, "Every member sees the same attached roster");
    expect(w.roster(h.conn).revision >= settled.revision && settled.revision > first.revision, "Revisions only grow");
    // Source slot and identity are the service's, never the sender's.
    const Bytes blob{9, 8, 7, 6};
    w.send(g1.conn, g1.prefix, signal_request(2, blob));
    auto delivered = forwarded_of(*w.sink.take(g2.conn, Kind::room_forwarded));
    expect(delivered.slot == 1 && delivered.identity == "peer-1" && delivered.blob == blob, "A signal arrives with the authoritative source");
    expect(w.sink.count(h.conn, Kind::room_forwarded) == 0 && w.sink.count(g1.conn, Kind::room_forwarded) == 0, "Only the addressed member receives it");
    Bytes large(65536, 0x5a);
    w.send(h.conn, h.prefix, signal_request(1, large));
    expect(forwarded_of(*w.sink.take(g1.conn, Kind::room_forwarded)).blob.size() == 65536, "The largest blob is forwarded whole");
    // Forbidden destinations close the sender's connection and free a pre-Start seat.
    const std::vector<std::pair<const char*, Bytes>> forbidden{
        {"itself", signal_request(3, blob)}, {"a slot beyond the room", signal_request(9, blob)},
        {"an empty blob", signal_request(1, {})}, {"an oversize blob", signal_request(1, Bytes(65537, 1))},
        {"a malformed destination", [] { auto b = message(Kind::room_signal); b.push_back(0); return b; }()}};
    for (const auto& [name, bytes] : forbidden) {
        auto f = w.member(h, "Probe", "probe-id");
        expect(f.attached.slot == 3, "The probe takes the free slot");
        w.send(f.conn, f.prefix, bytes);
        expect(w.failure_of(f.conn) == 1521, name);
        w.settle_all();
        expect(w.roster(h.conn).slots[3].first == 0, "A closed member frees its pre-Start seat");
        w.directory.disconnect(f.dir, w.now);
    }
    auto stranger = w.add("203.0.113.99");
    w.send(stranger, "203.0.113.99", signal_request(0, blob));
    expect(w.failure_of(stranger) == room_capability, "A connection that is not a member cannot signal");
    auto late = w.add("203.0.113.98");
    w.send(late, "203.0.113.98", message(Kind::room_lock));
    expect(w.failure_of(late) == room_capability, "Nor lock");
    w.send(g1.conn, g1.prefix, message(Kind::room_lock));
    expect(w.failure_of(g1.conn) == room_capability, "Only the host locks");
}

void admission_races() {
    {
        World w; auto& h = w.host(2);
        auto g = w.guest(h, "Guest1");
        expect(g.refusal == 0 && g.slot == 1, "The only guest slot is reserved");
        std::uint64_t conn{};
        expect(!w.alias_attach(h.opened.alias, "alias-guest", &conn), "A reserved last seat is not available to the alias path");
        expect(w.failure_of(conn) == 1545, "The alias path sees the room full");
        expect(w.attach(g, h, "dir-guest"), "The reservation holder attaches");
    }
    {
        World w; auto& h = w.host(2);
        auto taken = w.alias_attach(h.opened.alias, "alias-guest");
        expect(taken && taken->slot == 1 && taken->identity == "alias-guest" && taken->alias.empty(), "The alias path reserves and attaches in one step");
        expect(w.guest(h, "Guest1").refusal == 1545, "The directory path then finds the room full: one winner for the last seat");
    }
    {
        World w; auto& h = w.host(9);
        auto g1 = w.guest(h, "Guest1"), g2 = w.guest(h, "Guest2"), g3 = w.guest(h, "Guest3");
        expect(g1.refusal == 0 && g2.refusal == 0 && g3.refusal == room_unavailable, "At most two reservations are outstanding per room");
        expect(w.attach(g1, h, "peer-1"), "Attaching frees the outstanding count");
        expect(w.guest(h, "Guest4").refusal == 0, "A third reservation is possible once one attached");
        std::uint64_t conn{};
        expect(!w.alias_attach("ZZZZ", "nobody", &conn) && w.failure_of(conn) == 1523, "An unknown alias is the existing unknown-code refusal");
    }
    {
        // Reservations count toward the reserving prefix's unpaired budget (default 2), across rooms.
        World w; auto& a = w.host(4); auto& b = w.host(4); const std::string prefix = "192.0.2.50";
        auto first = w.guest(a, "Guest1", prefix), second = w.guest(b, "Guest2", prefix);
        expect(first.refusal == 0 && second.refusal == 0 && w.budgets.at(prefix).unpaired == 2, "Reservations count as unpaired sessions");
        expect(w.guest(a, "Guest3", prefix).refusal == 1527, "A third outstanding reservation from one prefix is refused");
        expect(w.attach(first, a, "peer-1") && w.budgets.at(prefix).unpaired == 1, "Attaching releases the prefix budget");
        w.directory.disconnect(second.dir, w.now);
        expect(w.budgets.at(prefix).unpaired == 0, "Leaving the directory releases an unattached reservation");
        expect(w.roster(b.conn).slots[1].first == 0, "The released seat is free again");
    }
}

void reservation_expiry() {
    World w; auto& h = w.host(4);
    auto g = w.guest(h, "Guest1", "198.51.100.200");
    expect(w.budgets.at(g.prefix).unpaired == 1, "An outstanding reservation is counted");
    w.tick(29s);
    expect(w.roster(h.conn).slots[1].first == 1, "Still reserved just before the deadline");
    w.tick(2s);
    expect(w.roster(h.conn).slots[1].first == 0 && w.budgets.at(g.prefix).unpaired == 0, "Expiry frees the seat and the prefix budget");
    expect(!w.attach(g, h, "late-peer") && w.failure_of(g.conn) == room_capability, "An expired capability opens nothing");
    // Expired but not yet polled: the attach itself notices.
    auto slow = w.guest(h, "Guest2");
    w.tick(31s, false);
    expect(!w.attach(slow, h, "slow-peer") && w.failure_of(slow.conn) == room_unavailable, "An attach after the deadline is refused");
    expect(w.roster(h.conn).slots[slow.slot].first == 0, "...and frees the seat");
    w.settle_all();
    // Expired reservations spend the prefix's failed-guess budget and have a window of their own: with two outstanding
    // reservations per prefix a squatter makes four expiries a minute, so the third one starts the cooldown.
    const std::string squatter = "192.0.2.77";
    std::string principal;
    for (int cycle = 0; cycle < 2; ++cycle) {
        auto one = w.guest(h, "Squat" + std::to_string(cycle * 2), squatter), two = w.guest(h, "Squat" + std::to_string(cycle * 2 + 1), squatter);
        expect(one.refusal == 0 && two.refusal == 0, "Two outstanding reservations per prefix");
        principal = one.principal;
        w.tick(30s);
        if (cycle == 0) expect(w.budgets.guess_allowed(squatter, w.now, 100, false), "Two expiries are not yet a cooldown");
    }
    expect(!w.budgets.guess_allowed(squatter, w.now, 100, false), "The third unattached expiry in a minute starts the cooldown");
    const auto again = w.add(squatter); w.principal_of(again, principal);
    expect(number_of(w.dir(again, join_request(h.room, "", "Squat9"))) == 1547, "The directory admission refuses a cooled-down prefix");
    w.tick(61s);
    expect(kind_of(w.dir(again, join_request(h.room, "", "Squat10"))) == Kind::directory_admitted, "The cooldown ends");
}

void moderation_and_refill() {
    World w; auto& h = w.host(4);
    auto g1 = w.member(h, "Guest1", "peer-1"), g2 = w.member(h, "Guest2", "peer-2");
    auto probe = w.member(h, "Probe", "probe-id");
    w.send(probe.conn, probe.prefix, slot_request(Kind::room_release, 2));
    expect(w.failure_of(probe.conn) == room_capability, "A guest cannot release");
    w.settle_all();
    w.send(h.conn, h.prefix, slot_request(Kind::room_release, 2));
    expect(w.sink.take(h.conn, Kind::room_ack) && closed_reason(w.sink.closed.at(g2.conn)) == room_denied, "Release frees the seat and tells the member");
    expect(w.roster(h.conn).slots[2].first == 0, "The released seat is vacant");
    auto g3 = w.member(h, "Guest3", "peer-3");
    expect(g3.attached.slot == 2, "A new member refills the released seat");
    w.send(h.conn, h.prefix, slot_request(Kind::room_kick, 1));
    expect(w.sink.take(h.conn, Kind::room_ack) && closed_reason(w.sink.closed.at(g1.conn)) == room_denied, "Kick removes the member");
    w.settle_all();
    // The kicked identity and the admission principal stay out for the room's lifetime.
    const auto again = w.add(w.fresh_prefix());
    const auto principal = w.principal_of(again, g1.principal);
    expect(principal == g1.principal, "The principal resumes");
    expect(number_of(w.dir(again, join_request(h.room, "", "Returning"))) == room_denied, "The kicked principal cannot reserve again");
    std::uint64_t conn{};
    expect(!w.alias_attach(h.opened.alias, "peer-1", &conn) && w.failure_of(conn) == room_denied, "...nor attach by alias under the kicked identity");
    auto fresh = w.alias_attach(h.opened.alias, "peer-new");
    expect(fresh && fresh->slot == 1, "The kicked seat is refilled by someone else");
    // Out of range and self targets are protocol violations.
    w.send(h.conn, h.prefix, slot_request(Kind::room_kick, 0));
    expect(w.failure_of(h.conn) == 1521, "The host cannot kick itself");
}

void lock_and_battle() {
    RoomOptions options; options.hold_timeout = 60s;
    World w(options); auto& h = w.host(5);
    auto g1 = w.member(h, "Guest1", "peer-1"), g2 = w.member(h, "Guest2", "peer-2");
    auto pending = w.guest(h, "Guest3");
    expect(pending.refusal == 0, "A pending reservation exists at Start");
    w.send(h.conn, h.prefix, message(Kind::room_lock));
    expect(w.sink.take(h.conn, Kind::room_ack) && w.roster(h.conn).state == 1 && w.roster(g1.conn).state == 1, "Lock is acknowledged and announced");
    expect(w.roster(h.conn).slots[pending.slot].first == 0, "Pending reservations are dropped");
    expect(w.sink.states.back() == std::make_tuple(h.room, true, false), "The directory is told to hide the room");
    w.tick(1s);
    {
        const auto reader = w.add(w.fresh_prefix()); w.principal_of(reader);
        auto list = message(Kind::directory_list); string(list, ""); u32(list, 50);
        expect(require(room_page(w.dir(reader, list))).rooms.empty(), "A started room is hidden from the directory");
    }
    expect(!w.attach(pending, h, "peer-3") && w.failure_of(pending.conn) == room_locked, "A dropped reservation attaches nothing");
    std::uint64_t conn{};
    expect(!w.alias_attach(h.opened.alias, "peer-4", &conn) && w.failure_of(conn) == room_locked, "No new attachment after Start");
    expect(w.guest(h, "Guest5").refusal == 1544, "...nor a new reservation: the row is hidden");
    // A lost member connection holds the seat.
    w.rooms->disconnect(g1.conn, w.now);
    auto held = w.roster(h.conn);
    expect(held.slots[1].first == 3 && held.slots[1].second == "peer-1" && held.slots[2].first == 2, "A lost seat is held, the others stay");
    // Repair: wrong proof of capability or identity opens nothing.
    auto bad = w.add(g1.prefix);
    w.send(bad, g1.prefix, attach_request(Kind::room_reattach, h.room, std::string(32, 'a'), "peer-1"));
    expect(w.failure_of(bad) == room_capability, "A wrong capability cannot take the seat");
    auto bad2 = w.add(g1.prefix);
    w.send(bad2, g1.prefix, attach_request(Kind::room_reattach, h.room, g1.attached.capability, "peer-2"));
    expect(w.failure_of(bad2) == room_capability, "A wrong identity cannot take the seat");
    const auto old_user = g1.attached.turn.username;
    const auto before = w.sink.revoked.size();
    auto back = w.add(g1.prefix);
    w.send(back, g1.prefix, attach_request(Kind::room_reattach, h.room, g1.attached.capability, "peer-1"));
    w.settle_all();
    auto again = attached_of(*w.sink.take(back, Kind::room_attached));
    expect(again.slot == 1 && again.capability == g1.attached.capability && again.turn.username != old_user, "Reattach binds a new connection with fresh credentials");
    expect(std::find(w.sink.revoked.begin() + static_cast<std::ptrdiff_t>(before), w.sink.revoked.end(), old_user) != w.sink.revoked.end(),
        "The replaced credential is revoked");
    expect(w.roster(h.conn).slots[1].first == 2, "The seat is attached again");
    // A half-open old connection is taken over by the capability holder.
    auto second = w.add(g2.prefix);
    w.send(second, g2.prefix, attach_request(Kind::room_reattach, h.room, g2.attached.capability, "peer-2"));
    w.settle_all();
    expect(w.sink.take(second, Kind::room_attached) && closed_reason(w.sink.closed.at(g2.conn)) == room_capability,
        "Reattach over a live old connection takes the seat over and closes the old one");
    // Signals flow between reattached members.
    w.send(back, g1.prefix, signal_request(2, Bytes{1, 2, 3}));
    expect(w.sink.take(second, Kind::room_forwarded).has_value(), "Signals reach the reattached member");
    // Host loss after Start does not end the room.
    w.rooms->disconnect(h.conn, w.now);
    expect(w.rooms->rooms() == 1 && w.roster(back).slots[0].first == 3, "The host is held like any seat");
    // A held seat expires into left; its capability stops working.
    w.rooms->disconnect(back, w.now);
    w.tick(61s);
    expect(w.roster(second).slots[1].first == 4 && w.roster(second).slots[1].second == "peer-1", "A seat nobody repairs is marked left");
    auto late = w.add(g1.prefix);
    w.send(late, g1.prefix, attach_request(Kind::room_reattach, h.room, g1.attached.capability, "peer-1"));
    expect(w.failure_of(late) == room_capability, "A left seat cannot be reattached");
    // Everyone gone: the started room is released.
    w.send(second, g2.prefix, message(Kind::room_leave));
    w.tick(1ms);
    expect(w.rooms->rooms() == 0 && w.rooms->members() == 0, "A started room with nobody attached or held ends");
}

void pre_start_rules() {
    {
        World w; auto& h = w.host(4);
        auto g = w.member(h, "Guest1", "peer-1");
        auto back = w.add(g.prefix);
        w.send(back, g.prefix, attach_request(Kind::room_reattach, h.room, g.attached.capability, "peer-1"));
        expect(w.failure_of(back) == room_unavailable, "Reattach exists only after Start");
        w.send(g.conn, g.prefix, message(Kind::room_leave));
        expect(kind_of(w.sink.closed.at(g.conn)) == Kind::room_ack && w.roster(h.conn).slots[1].first == 0, "Leave frees a pre-Start seat and is acknowledged");
        auto refill = w.alias_attach(h.opened.alias, "peer-2");
        expect(refill && refill->slot == 1, "A later newcomer fills it");
    }
    {
        // Host loss before Start closes staging for everyone, hides the row and releases every reservation.
        World w; auto& h = w.host(4);
        auto g = w.member(h, "Guest1", "peer-1"); auto pending = w.guest(h, "Guest2", "192.0.2.10");
        w.rooms->disconnect(h.conn, w.now);
        expect(closed_reason(w.sink.closed.at(g.conn)) == 1528, "Members learn that the host left");
        expect(w.rooms->rooms() == 0 && w.rooms->reservations() == 0 && w.budgets.at("192.0.2.10").unpaired == 0, "No state survives the room");
        expect(w.sink.states.back() == std::make_tuple(h.room, false, true), "The directory row is closed");
        expect(!w.alias_attach(h.opened.alias, "peer-3"), "The alias dies with the room");
        const auto refused = w.dir(pending.dir, join_request(h.room, "", "Guest3"));
        expect(kind_of(refused) == Kind::error, "The closed row takes no new seat");
    }
    {
        // The host's explicit leave behaves the same.
        World w; auto& h = w.host(3); auto g = w.member(h, "Guest1", "peer-1");
        w.send(h.conn, h.prefix, message(Kind::room_leave));
        expect(closed_reason(w.sink.closed.at(g.conn)) == 1528 && w.rooms->rooms() == 0, "A host leaving ends the room");
    }
    {
        // A directory row that stops heartbeating ends an open scope but not a started one.
        World w; auto& open = w.host(3); auto g = w.member(open, "Guest1", "peer-1");
        w.hosts.at(open.room).beating = false;
        w.tick(31s);
        expect(w.rooms->rooms() == 0 && closed_reason(w.sink.closed.at(g.conn)) == room_unknown, "A staging room whose host stopped heartbeating ends");
        World v; auto& started = v.host(3); auto m = v.member(started, "Guest1", "peer-1");
        v.send(started.conn, started.prefix, message(Kind::room_lock));
        v.hosts.at(started.room).beating = false;
        v.tick(31s);
        expect(v.rooms->rooms() == 1 && !v.dead(m.conn), "A started match outlives its directory row");
        const auto hidden = v.dir(started.dir, heartbeat_request(started.room, started.ownership, info(3)));
        expect(number_of(hidden) == 1541, "The expired row cannot be resurrected");
    }
    {
        // Heartbeats keep the scope's flags and capacity; the occupancy is the seat table's.
        World w; auto& h = w.host(4); w.member(h, "Guest1", "peer-1");
        auto wrong = info(3);
        expect(number_of(w.dir(h.dir, heartbeat_request(h.room, h.ownership, wrong))) == 1540, "A scoped room keeps its capacity");
        expect(w.dir(h.dir, heartbeat_request(h.room, h.ownership, info(4))) == message(Kind::directory_ok), "A matching heartbeat is accepted");
        const auto reader = w.add(w.fresh_prefix()); w.principal_of(reader);
        auto list = message(Kind::directory_list); string(list, ""); u32(list, 50);
        auto page = require(room_page(w.dir(reader, list)));
        expect(page.rooms.size() == 1 && page.rooms[0].metadata.occupancy == 2, "The listed occupancy comes from the seat table");
        w.send(h.conn, h.prefix, message(Kind::room_lock));
        expect(w.dir(h.dir, heartbeat_request(h.room, h.ownership, info(4))) == message(Kind::directory_ok), "The host keeps heartbeating a started room");
        w.tick(1s);
        expect(require(room_page(w.dir(reader, list))).rooms.empty(), "A heartbeat cannot unhide a started room");
    }
    {
        // Open rules.
        World w;
        const auto prefix = w.fresh_prefix();
        const auto dir = w.add(prefix); const auto principal = w.principal_of(dir);
        for (const std::uint32_t capacity : {1U, 10U}) {
            const auto published = w.dir(dir, publish_request(info(capacity)));
            expect(kind_of(published) == Kind::directory_published, "The directory lists any capacity up to 64");
            Reader reader{std::span(published).subspan(2)}; const auto room = reader.string(32), ownership = reader.string(32);
            const auto conn = w.add(prefix);
            w.send(conn, prefix, room_open_request(room, ownership, principal, "host-id"));
            expect(w.failure_of(conn) == 1521, "A signalling room needs 2..9 seats");
            auto remove = message(Kind::directory_remove); string(remove, room); string(remove, ownership);
            expect(w.dir(dir, remove) == message(Kind::directory_ok), "Cleanup");
        }
        auto& h = w.host(3);
        const auto again = w.add(h.prefix);
        w.send(again, h.prefix, room_open_request(h.room, h.ownership, h.principal, "other-id"));
        expect(w.failure_of(again) == room_unavailable, "A room opens once");
        // A published room nobody has opened yet: only its principal and ownership capability open it.
        const auto prefix3 = w.fresh_prefix(); const auto dir3 = w.add(prefix3); const auto principal3 = w.principal_of(dir3);
        const auto published3 = w.dir(dir3, publish_request(info(3)));
        Reader reader3{std::span(published3).subspan(2)}; const auto room3 = reader3.string(32), ownership3 = reader3.string(32);
        const auto stranger = w.add("203.0.113.40");
        w.send(stranger, "203.0.113.40", room_open_request(room3, std::string(32, 'a'), principal3, "x-id"));
        expect(w.failure_of(stranger) == room_unknown, "A wrong ownership capability is the unknown-room refusal");
        const auto thief = w.add("203.0.113.41");
        w.send(thief, "203.0.113.41", room_open_request(room3, ownership3, std::string(32, 'b'), "x-id"));
        expect(w.failure_of(thief) == room_unknown, "A wrong principal is too");
        const auto malformed = w.add("203.0.113.42");
        w.send(malformed, "203.0.113.42", room_open_request("not-hex", h.ownership, h.principal, "x-id"));
        expect(w.failure_of(malformed) == 1521, "Malformed ids are protocol errors");
        const auto flag = w.add("203.0.113.43");
        w.send(flag, "203.0.113.43", room_open_request(h.room, h.ownership, h.principal, "x-id", 2));
        expect(w.failure_of(flag) == 1521, "The alias flag is 0 or 1");
        auto& secret = w.host(3, "pw");
        expect(secret.opened.alias.empty(), "A password room publishes no alias");
        auto& quiet = w.host(3, "", 0);
        expect(quiet.opened.alias.empty(), "A host may decline the alias");
        w.sink.alias_pool.clear();
        const auto prefix2 = w.fresh_prefix(); const auto dir2 = w.add(prefix2); const auto principal2 = w.principal_of(dir2);
        const auto published = w.dir(dir2, publish_request(info(3)));
        Reader reader{std::span(published).subspan(2)}; const auto room = reader.string(32), ownership = reader.string(32);
        const auto conn = w.add(prefix2);
        w.send(conn, prefix2, room_open_request(room, ownership, principal2, "host-id"));
        expect(w.failure_of(conn) == 1533 && w.rooms->rooms() == 3, "No free alias is the existing alias-exhaustion refusal");
    }
}

void turn_credentials_contracts() {
    {
        World w; auto& h = w.host(4);
        w.sink.refuse_turn = true;
        auto g = w.member(h, "Guest1", "peer-1");
        expect(g.attached.turn.uri.empty() && g.attached.turn.username.empty() && w.sink.warned("EAWR-NET-1559"),
            "A refused provider call lets the member attach without relay candidates");
        w.sink.refuse_turn = false;
    }
    {
        // No relay configured at all (empty URI): members attach, nothing is revoked.
        World w; auto& h = w.host(3);
        auto g = w.guest(h, "Guest1"); g.conn = w.add(g.prefix);
        w.sink.manual = true;
        w.send(g.conn, g.prefix, attach_request(Kind::room_attach, h.room, g.capability, "peer-1"));
        expect(w.sink.pending.size() == 1, "The attach waits for the provider");
        w.sink.manual = false;
        w.rooms->turn_done(w.sink.pending.front(), GnsTurnCredentials{}, {}, w.now); w.sink.pending.clear();
        const auto frame = attached_of(*w.sink.take(g.conn, Kind::room_attached));
        expect(frame.turn.uri.empty() && frame.turn.username.empty() && w.sink.revoked.empty(), "No relay configured is a plain attach");
    }
    {
        // A provider that never answers: the attach continues at the deadline and a late credential is revoked.
        World w; auto& h = w.host(3);
        auto g = w.guest(h, "Guest1"); g.conn = w.add(g.prefix);
        w.sink.manual = true;
        w.send(g.conn, g.prefix, attach_request(Kind::room_attach, h.room, g.capability, "peer-1"));
        const auto key = w.sink.pending.front();
        w.tick(5s);
        expect(w.sink.count(g.conn, Kind::room_attached) == 0, "Still waiting inside the deadline");
        w.tick(2s);
        const auto frame = attached_of(*w.sink.take(g.conn, Kind::room_attached));
        expect(frame.turn.uri.empty() && w.sink.warned("EAWR-NET-1556"), "The deadline lets the member in without relay");
        w.rooms->turn_done(key, GnsTurnCredentials{"turn:203.0.113.5:3478", "late-user", "late-pass", 100s}, {}, w.now);
        expect(w.sink.revoked.back() == "late-user", "A credential that arrives after the deadline is revoked at once");
    }
    {
        // Credentials end with the seat: leave, kick, release and room end all revoke.
        World w; auto& h = w.host(5);
        auto a = w.member(h, "Guest1", "peer-1"), b = w.member(h, "Guest2", "peer-2"), c = w.member(h, "Guest3", "peer-3");
        w.send(a.conn, a.prefix, message(Kind::room_leave));
        w.send(h.conn, h.prefix, slot_request(Kind::room_kick, 2));
        w.send(h.conn, h.prefix, slot_request(Kind::room_release, 3));
        w.settle_all();
        const std::set<std::string> revoked(w.sink.revoked.begin(), w.sink.revoked.end());
        expect(revoked.contains(a.attached.turn.username) && revoked.contains(b.attached.turn.username) && revoked.contains(c.attached.turn.username),
            "Leave, kick and release each revoke the member's credential");
        w.rooms->disconnect(h.conn, w.now);
        expect(w.sink.revoked.size() == 4 && w.rooms->rooms() == 0, "Room end revokes the rest");
    }
    {
        // A late attach gets a credential sized to the time left, not the full maximum.
        RoomOptions options; options.session_lifetime = 20s; options.max_session_lifetime = 25s; options.renewal_window = 5s;
        World w(options); auto& h = w.host(3);
        w.tick(12s);
        auto g = w.member(h, "Guest1", "peer-1");
        expect(g.attached.turn.lifetime == 13s + 60s && g.attached.remaining == 8000 && g.attached.hard == 13000,
            "A late member's credential covers the time left plus the margin, not the full maximum");
    }
}

void lease_contracts() {
    RoomOptions options; options.session_lifetime = 10s; options.max_session_lifetime = 25s; options.renewal_window = 5s;
    {
        World w(options); auto& h = w.host(4);
        auto a = w.member(h, "Guest1", "peer-1"), b = w.member(h, "Guest2", "peer-2");
        w.rooms->renew(a.conn, w.now);
        auto early = refused_in(got(w.sink.take(a.conn, Kind::renew_refused)));
        expect(early && early->reason == renew_refused_early && early->remaining == 10s, "Renewal before the window is refused, lease unchanged");
        w.tick(6s);
        for (const auto conn : {h.conn, a.conn, b.conn}) {
            auto warning = expiring_in(got(w.sink.take(conn, Kind::expiring)));
            expect(warning && warning->remaining == 4s && warning->hard_remaining == 19s && warning->renewable, "Every attached member is warned at the window");
        }
        w.rooms->renew(a.conn, w.now);
        expect(w.sink.pending.size() == 3, "One provider call per attached member");
        w.settle();
        std::set<std::string> users;
        for (const auto conn : {h.conn, a.conn, b.conn}) {
            auto renewed = renewed_in(got(w.sink.take(conn, Kind::renewed)));
            expect(renewed && renewed->remaining == 14s && renewed->hard_remaining == 19s && renewed->turn.uri == "turn:203.0.113.5:3478", "Renewal extends the lease for every member");
            users.insert(renewed->turn.username);
        }
        expect(users.size() == 3, "...with a distinct fresh credential each");
        w.rooms->renew(b.conn, w.now);
        expect(refused_in(got(w.sink.take(b.conn, Kind::renew_refused)))->reason == renew_refused_early,
            "A second member's renewal is a harmless no-op");
        w.tick(10s);
        for (const auto conn : {h.conn, a.conn, b.conn}) expect(expiring_in(got(w.sink.take(conn, Kind::expiring)))->hard_remaining == 9s,
            "Warned again before the second end");
        w.rooms->renew(b.conn, w.now); w.settle();
        auto capped = renewed_in(got(w.sink.take(h.conn, Kind::renewed)));
        expect(capped && capped->remaining == 9s && capped->hard_remaining == 9s, "Renewal is capped at the maximum");
        w.rooms->renew(h.conn, w.now);
        expect(refused_in(got(w.sink.take(h.conn, Kind::renew_refused)))->reason == renew_refused_maximum, "No renewal past the maximum");
        w.tick(9s);
        expect(closed_reason(w.sink.closed.at(h.conn)) == lease_ended_at_maximum && closed_reason(w.sink.closed.at(a.conn)) == lease_ended_at_maximum
            && w.rooms->rooms() == 0, "The room ends at the maximum with its typed reason for everyone");
    }
    {
        // Without a renewal the lease ends with the not-renewed reason.
        World w(options); auto& h = w.host(3); auto a = w.member(h, "Guest1", "peer-1");
        w.tick(10s);
        expect(closed_reason(w.sink.closed.at(a.conn)) == lease_not_renewed, "An unrenewed lease ends with its own reason");
    }
    {
        // A failing provider refuses the renewal and keeps the lease; a slow one is cut off at its deadline.
        RoomOptions slow; slow.session_lifetime = 20s; slow.max_session_lifetime = 40s; slow.renewal_window = 8s; slow.credential_wait = 2s; slow.renew_cooldown = 1s;
        World w(slow); auto& h = w.host(3); auto a = w.member(h, "Guest1", "peer-1");
        w.tick(12s);
        w.sink.refuse_turn = true;
        w.rooms->renew(a.conn, w.now);
        expect(refused_in(got(w.sink.take(a.conn, Kind::renew_refused)))->reason == renew_refused_credentials,
            "A refused provider call refuses the renewal");
        w.sink.refuse_turn = false;
        w.rooms->renew(a.conn, w.now);
        expect(refused_in(got(w.sink.take(a.conn, Kind::renew_refused)))->reason == renew_refused_early, "A member cannot restart a renewal within its cooldown");
        w.tick(2s); // t = 14 s; the cooldown has passed
        w.sink.manual = true;
        w.rooms->renew(a.conn, w.now);
        const auto stuck = w.sink.pending;
        w.sink.pending.clear();
        expect(stuck.size() == 2, "A fresh attempt asks for each member");
        w.tick(3s);
        expect(w.sink.count(a.conn, Kind::renew_refused) == 0, "Still waiting inside the deadline");
        w.tick(2s);
        expect(refused_in(got(w.sink.take(a.conn, Kind::renew_refused)))->reason == renew_refused_credentials,
            "A provider that does not answer in time refuses the renewal");
        const auto before = w.sink.revoked.size();
        w.rooms->turn_done(stuck.front(), GnsTurnCredentials{"turn:203.0.113.5:3478", "too-late", "pw", 50s}, {}, w.now);
        expect(w.sink.revoked.size() == before + 1 && w.sink.revoked.back() == "too-late", "Late renewal credentials are revoked");
        w.sink.manual = false;
        w.rooms->renew(a.conn, w.now); w.settle();
        const auto renewed = renewed_in(got(w.sink.take(a.conn, Kind::renewed)));
        expect(renewed && renewed->remaining == 21s && renewed->hard_remaining == 21s, "The lease renews after the failed attempts");
    }
}

void budget_contracts() {
    {
        RoomOptions options; options.operations_per_member = 4;
        World w(options); auto& h = w.host(4);
        auto a = w.member(h, "Guest1", "peer-1"), b = w.member(h, "Guest2", "peer-2");
        for (int i = 0; i < 4; ++i) w.send(a.conn, a.prefix, signal_request(2, Bytes{1}));
        expect(w.failure_of(a.conn) == 0, "Four operations fit the member budget");
        w.send(a.conn, a.prefix, signal_request(2, Bytes{1}));
        expect(w.failure_of(a.conn) == room_budget && w.failure_of(b.conn) == 0 && w.failure_of(h.conn) == 0, "A member over budget is cut off alone");
        w.settle_all();
        auto c = w.member(h, "Guest3", "peer-3");
        w.send(c.conn, c.prefix, signal_request(2, Bytes{1}));
        expect(w.failure_of(c.conn) == 0, "The refilled seat has a fresh budget");
    }
    {
        RoomOptions options; options.operations_per_member = 4;
        World w(options); auto& h = w.host(2);
        // Churn cannot launder operations through fresh seats: the room as a whole has capacity x member budget.
        bool cut{};
        for (int i = 0; i < 6 && !cut; ++i) {
            auto m = w.member(h, "Churn" + std::to_string(i), "churn-" + std::to_string(i));
            for (int j = 0; j < 4; ++j) w.send(m.conn, m.prefix, signal_request(0, Bytes{1}));
            if (w.failure_of(m.conn) == room_budget) cut = true; else w.send(m.conn, m.prefix, message(Kind::room_leave));
            w.settle_all(); w.directory.disconnect(m.dir, w.now);
        }
        expect(cut, "The room budget bounds churning members");
        w.tick(61s);
        auto m = w.member(h, "After", "after-id");
        w.send(m.conn, m.prefix, signal_request(0, Bytes{1}));
        expect(w.failure_of(m.conn) == 0, "Budgets reset every minute");
    }
    {
        RoomOptions options; options.bytes_per_member = 1000;
        World w(options); auto& h = w.host(3);
        auto a = w.member(h, "Guest1", "peer-1");
        w.send(a.conn, a.prefix, signal_request(0, Bytes(600, 1)));
        expect(w.failure_of(a.conn) == 0, "A signal inside the byte budget passes");
        w.send(a.conn, a.prefix, signal_request(0, Bytes(600, 1)));
        expect(w.failure_of(a.conn) == room_budget, "The byte budget is per member");
    }
}

void bounds_at_max_rooms() {
    // 64 rooms (the directory bound) of nine members each: every structure is released again.
    World w;
    std::vector<Host*> opened;
    for (int r = 0; r < 64; ++r) {
        auto& h = w.host(9, "", 0); // the fake alias pool holds twelve
        for (int m = 0; m < 8; ++m) {
            auto g = w.guest(h, "G" + std::to_string(m));
            expect(g.refusal == 0 && w.attach(g, h, "peer-" + std::to_string(m)), "Full-room member attached");
        }
        opened.push_back(&h);
    }
    expect(w.rooms->rooms() == 64 && w.rooms->members() == 64 * 9 && w.rooms->reservations() == 0, "Sixty-four full rooms hold exactly their members");
    for (auto* h : opened) { h->beating = false; w.rooms->disconnect(h->conn, w.now); }
    w.settle_all();
    expect(w.rooms->rooms() == 0 && w.rooms->members() == 0 && w.rooms->reservations() == 0, "Closing the rooms releases every table");
    expect(w.sink.revoked.size() == 64 * 9, "...and every credential");
}

void benign_races_keep_connections() {
    World w; auto& h = w.host(6);
    auto a = w.member(h, "Guest1", "peer-1"), b = w.member(h, "Guest2", "peer-2"), c = w.member(h, "Guest3", "peer-3");
    const Bytes blob{1, 2, 3};
    // A signal toward a seat that just emptied is dropped and counted; the sender and everyone else stay.
    w.send(c.conn, c.prefix, message(Kind::room_leave));
    w.settle_all();
    w.send(a.conn, a.prefix, signal_request(c.attached.slot, blob));
    expect(w.failure_of(a.conn) == 0 && w.rooms->dropped_signals() == 1, "A signal to a vacated seat is dropped, the sender stays");
    // ...also toward a reserved seat, a held seat and a seat that is still attaching.
    auto reserved = w.guest(h, "Guest4");
    w.send(a.conn, a.prefix, signal_request(reserved.slot, blob));
    expect(w.failure_of(a.conn) == 0 && w.rooms->dropped_signals() == 2, "...to a reserved seat");
    w.directory.disconnect(c.dir, w.now); w.directory.disconnect(reserved.dir, w.now);
    // Out-of-range seats, oneself and malformed frames remain protocol violations.
    w.send(b.conn, b.prefix, signal_request(b.attached.slot, blob));
    expect(w.failure_of(b.conn) == 1521, "A signal to oneself is a violation");
    w.settle_all();
    // A receiver whose queue is full loses the blob, not its connection.
    auto d = w.member(h, "Guest5", "peer-5");
    w.sink.full.insert(d.conn);
    const auto dropped = w.rooms->dropped_signals();
    w.send(a.conn, a.prefix, signal_request(d.attached.slot, blob));
    expect(w.rooms->dropped_signals() == dropped + 1 && w.failure_of(d.conn) == 0 && w.failure_of(a.conn) == 0, "A full receiver queue drops the blob only");
    w.sink.full.clear();
    w.send(a.conn, a.prefix, signal_request(d.attached.slot, blob));
    expect(w.sink.count(d.conn, Kind::room_forwarded) == 1, "...and the receiver gets the next one");
    // Host races: the target left, the lock repeats, Start came first. Nothing closes the host or the room.
    w.send(h.conn, h.prefix, slot_request(Kind::room_release, c.attached.slot));
    expect(w.sink.take(h.conn, Kind::room_ack).has_value() && w.failure_of(h.conn) == 0, "Release of a seat that just emptied is a no-op acknowledgement");
    w.send(h.conn, h.prefix, slot_request(Kind::room_kick, c.attached.slot));
    expect(w.sink.take(h.conn, Kind::room_ack).has_value() && w.failure_of(h.conn) == 0, "...so is a kick");
    w.send(h.conn, h.prefix, message(Kind::room_lock));
    w.sink.take(h.conn, Kind::room_ack);
    w.send(h.conn, h.prefix, message(Kind::room_lock));
    const auto repeated = got(w.sink.take(h.conn, Kind::room_ack));
    expect(repeated.size() == 6 && read_u32(std::span(repeated).subspan(2)) == room_locked && w.failure_of(h.conn) == 0, "A repeated Start is acknowledged with its reason");
    w.send(h.conn, h.prefix, slot_request(Kind::room_release, a.attached.slot));
    const auto late = got(w.sink.take(h.conn, Kind::room_ack));
    expect(late.size() == 6 && read_u32(std::span(late).subspan(2)) == room_locked && w.failure_of(h.conn) == 0 && w.rooms->rooms() == 1,
        "A release after Start is refused with its reason and keeps the room");
}

void denylist_bound() {
    World w; auto& h = w.host(2);
    for (unsigned i = 0; i <= room_denylist_max; ++i) {
        auto member = w.alias_attach(h.opened.alias, "kick-" + std::to_string(i));
        expect(member && member->slot == 1, "A newcomer fills the only guest seat");
        w.send(h.conn, h.prefix, slot_request(Kind::room_kick, 1));
        const auto ack = got(w.sink.take(h.conn, Kind::room_ack));
        if (i < room_denylist_max) expect(ack.size() == 2, "A kick inside the bound is plain");
        else expect(ack.size() == 6 && read_u32(std::span(ack).subspan(2)) == room_budget && w.failure_of(h.conn) == 0,
            "A kick beyond the bound still removes the member and says it was not denied");
        w.settle_all();
        expect(w.roster(h.conn).slots[1].first == 0, "The seat is free again");
    }
    std::uint64_t conn{};
    expect(!w.alias_attach(h.opened.alias, "kick-0", &conn) && w.failure_of(conn) == room_denied, "A denied identity stays out");
    auto again = w.alias_attach(h.opened.alias, "kick-" + std::to_string(room_denylist_max));
    expect(again.has_value(), "The identity that did not fit the list can come back (documented limit)");
}

void seat_and_slot() {
    // The directory seat is a chat label that is never reused; the slot is the transport peer index and is reused.
    World w; auto& h = w.host(3);
    auto first = w.member(h, "Guest1", "peer-1");
    expect(first.slot == 1 && first.seat == 2, "First guest: slot 1, directory seat 2");
    w.send(first.conn, first.prefix, message(Kind::room_leave));
    w.settle_all(); w.directory.disconnect(first.dir, w.now);
    auto second = w.member(h, "Guest2", "peer-2");
    expect(second.slot == 1 && second.seat == 3, "After a leave the slot is refilled, the seat is not");
    expect(second.attached.slot == second.slot, "The attach reply names the reserved slot");
    // Kicking by slot removes the member that holds the slot, whatever its directory seat is.
    w.send(h.conn, h.prefix, slot_request(Kind::room_kick, 1));
    expect(closed_reason(w.sink.closed.at(second.conn)) == room_denied, "The slot, not the seat, names the member to remove");
}

void credential_revocation() {
    RoomOptions options; options.session_lifetime = 20s; options.max_session_lifetime = 60s; options.renewal_window = 8s;
    options.renew_cooldown = 1s; options.hold_timeout = 5s;
    {
        // Every credential issued to a seat is revoked when it ends: attach plus two renewals.
        World w(options); auto& h = w.host(3); auto a = w.member(h, "Guest1", "peer-1");
        std::set<std::string> issued{a.attached.turn.username};
        w.tick(12s);
        w.rooms->renew(a.conn, w.now); w.settle();
        issued.insert(renewed_in(got(w.sink.take(a.conn, Kind::renewed)))->turn.username);
        w.tick(21s);
        w.rooms->renew(h.conn, w.now); w.settle();
        issued.insert(renewed_in(got(w.sink.take(a.conn, Kind::renewed)))->turn.username);
        expect(issued.size() == 3, "A seat was issued three distinct credentials");
        const auto before = w.sink.revoked.size();
        w.send(h.conn, h.prefix, slot_request(Kind::room_kick, 1));
        const std::set<std::string> revoked(w.sink.revoked.begin() + static_cast<std::ptrdiff_t>(before), w.sink.revoked.end());
        expect(revoked == issued, "A kick revokes the attach credential and every renewal credential");
    }
    {
        // A held seat that times out into left, and a reattach, revoke what the seat held.
        World w(options); auto& h = w.host(3); auto a = w.member(h, "Guest1", "peer-1");
        w.send(h.conn, h.prefix, message(Kind::room_lock));
        w.rooms->disconnect(a.conn, w.now);
        w.tick(6s);
        expect(std::find(w.sink.revoked.begin(), w.sink.revoked.end(), a.attached.turn.username) != w.sink.revoked.end(),
            "A held seat that times out into left revokes its credentials");
    }
}

// ---- Part B: the same service behind the TLS server ------------------------------------------------------------------------
std::atomic<std::int64_t> millis{};
RelayServer::Clock::time_point now_fake() { return RelayServer::Clock::time_point{} + std::chrono::milliseconds(millis.load()); }
struct Fixture {
    std::unique_ptr<RelayServer> server;
    NetworkIntroductionOptions client;
    std::atomic<std::size_t> rooms{};
    std::jthread worker;
    Fixture(RelayServerOptions options, const std::string& certificate, const std::string& key, std::shared_ptr<TurnRefresh> refresh = nullptr) {
        millis = 0;
        options.port = 0; options.listen_address = "127.0.0.1"; options.certificate_file = certificate; options.key_file = key;
        server = require(RelayServer::listen(std::move(options), now_fake, nullptr, std::move(refresh)));
        client.address = "127.0.0.1"; client.port = server->port(); client.certificate_name = "localhost";
        client.ca_file = certificate; client.timeout = 3s;
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) { server->poll(); rooms = server->rooms(); std::this_thread::sleep_for(1ms); }
        });
    }
    ~Fixture() { worker.request_stop(); worker.join(); }
};
RelayServerOptions server_options(bool relay = true) {
    RelayServerOptions o; o.room_aliases = true;
    if (relay) { o.turn_uri = "turn:127.0.0.1:3478"; o.turn_secret = std::string(32, 's'); }
    return o;
}
std::size_t attached_count(const GnsRoomRoster& roster) {
    return static_cast<std::size_t>(std::count_if(roster.slots.begin(), roster.slots.end(), [](const GnsRoomSlot& s) { return s.state == GnsRoomSlotState::attached; }));
}
GnsRoomSignal wait_signal(NetworkGnsRoom& room) {
    std::optional<GnsRoomSignal> got;
    until([&] { got = require(room.poll()); return got.has_value(); }, "A signal arrives");
    return std::move(*got);
}

struct Table {
    Fixture& f;
    std::unique_ptr<NetworkOnlineLobby> host_lobby;
    std::shared_ptr<NetworkGnsRoom> host;
    OnlineRoomPublication publication;
    OnlineRoomMetadata meta;
    std::vector<std::unique_ptr<NetworkOnlineLobby>> lobbies;
    std::vector<std::shared_ptr<NetworkGnsRoom>> guests;
    Table(Fixture& fixture, std::uint32_t capacity, bool public_room = true) : f(fixture) {
        host_lobby = std::make_unique<NetworkOnlineLobby>(f.client);
        meta = info(capacity);
        publication = require(host_lobby->publish(meta, ""));
        host = require(NetworkGnsRoom::open(f.client, publication, host_lobby->principal(), "host-peer", public_room));
    }
    // The server runs on a fake clock. Moving it forward in steps below the directory lease, with the host's
    // staging heartbeat in between, lets the per-minute principal-creation quota of one address refill.
    void advance(std::chrono::milliseconds total) {
        while (total.count() > 0) {
            const auto step = std::min(total, std::chrono::milliseconds(25000));
            millis += step.count(); total -= step;
            require(host_lobby->heartbeat(publication, meta));
        }
    }
    std::shared_ptr<NetworkGnsRoom> join(const std::string& name, const std::string& identity) {
        lobbies.push_back(std::make_unique<NetworkOnlineLobby>(f.client));
        auto admission = require(lobbies.back()->join(publication.room, "", name));
        expect(admission.slot == guests.size() + 1 && code_valid(admission.member_capability), "The admission names the slot and the capability");
        guests.push_back(require(NetworkGnsRoom::attach(f.client, admission, identity)));
        return guests.back();
    }
};

void loopback_room(const std::string& certificate, const std::string& key) {
    Fixture f(server_options(), certificate, key);
    Table t(f, 3);
    expect(t.host->local_slot() == 0 && alias_valid(t.host->alias()) && code_valid(t.host->capability()), "The host holds slot 0, an alias and a capability");
    expect(t.host->turn_credentials() && t.host->turn_credentials()->uri == "turn:127.0.0.1:3478" && !t.host->turn_credentials()->username.empty(),
        "The host has relay credentials");
    expect(t.host->lease().expires > std::chrono::steady_clock::now() + 7000s, "The lease is the service's session lifetime");
    auto one = t.join("Guest1", "guest-one");
    auto two = require(NetworkGnsRoom::attach_alias(f.client, t.host->alias(), "guest-two"));
    expect(one->local_slot() == 1 && two->local_slot() == 2, "Directory admission and alias attach take consecutive slots");
    until([&] { return attached_count(t.host->roster()) == 3 && attached_count(two->roster()) == 3 && attached_count(one->roster()) == 3; }, "Every member sees the full roster");
    const auto roster = one->roster();
    expect(roster.capacity == 3 && roster.state == GnsRoomState::open && roster.slots[0].identity == "host-peer" && roster.slots[1].identity == "guest-one"
        && roster.slots[2].identity == "guest-two", "The roster carries identities by slot");
    std::set<std::string> users{t.host->turn_credentials()->username, one->turn_credentials()->username, two->turn_credentials()->username};
    expect(users.size() == 3, "Each member has its own TURN credential");
    // Signals: whole blobs, authoritative source.
    Bytes blob(65536); for (std::size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<std::uint8_t>(i * 7);
    require(one->send(2, blob));
    auto got = wait_signal(*two);
    expect(got.from_slot == 1 && got.from_identity == "guest-one" && got.bytes == blob, "A 64 KiB blob arrives whole with the service's source");
    require(t.host->send(1, Bytes{1, 2, 3}));
    require(two->send(0, Bytes{4}));
    expect(wait_signal(*one).bytes == Bytes({1, 2, 3}) && wait_signal(*t.host).from_slot == 2, "Signals flow in every direction");
    expect(!one->send(1, Bytes{1}) && !one->send(2, {}) , "Local checks: not to itself, not empty");
    expect(!one->release(2) && !one->kick(2) && !one->lock(), "Host-only operations are refused locally from a guest");
    // The lease and heartbeat keep running; Start locks the room.
    require(t.host->lock());
    until([&] { return one->roster().state == GnsRoomState::locked && t.host->roster().state == GnsRoomState::locked; }, "Start reaches every member");
    auto late = NetworkGnsRoom::attach_alias(f.client, t.host->alias(), "latecomer");
    expect(!late && late.error().code == "EAWR-NET-1775", "A started room takes no one");
    // A lost connection holds the seat; reattach with the capability repairs it.
    const auto capability = one->capability();
    const auto old_user = one->turn_credentials()->username;
    one.reset(); t.guests[0].reset();
    until([&] { return t.host->roster().slots[1].state == GnsRoomSlotState::held; }, "The host sees the seat held");
    auto wrong = NetworkGnsRoom::reattach(f.client, t.publication.room, std::string(32, 'c'), "guest-one");
    expect(!wrong && wrong.error().code == "EAWR-NET-1773", "A wrong capability repairs nothing");
    auto repaired = require(NetworkGnsRoom::reattach(f.client, t.publication.room, capability, "guest-one"));
    expect(repaired->local_slot() == 1 && repaired->turn_credentials() && repaired->turn_credentials()->username != old_user, "Reattach returns fresh credentials");
    until([&] { return t.host->roster().slots[1].state == GnsRoomSlotState::attached; }, "The seat is attached again");
    require(two->send(1, Bytes{9}));
    expect(wait_signal(*repaired).from_identity == "guest-two", "Signals reach the repaired member");
    // A forbidden destination ends only the offender.
    require(two->send(8, Bytes{1}));
    until([&] { return !two->status(); }, "The service closes a member that addresses a slot outside the room");
    expect(two->status().error().code == "EAWR-NET-1521", "...with the protocol refusal");
    until([&] { return t.host->roster().slots[2].state == GnsRoomSlotState::held; }, "A started room holds the offender's seat");
    expect(t.host->status() && repaired->status(), "The others are unaffected");
}

void loopback_without_relay(const std::string& certificate, const std::string& key) {
    Fixture f(server_options(false), certificate, key);
    // A room whose host never opened the scope admits like before: slot 0, no capability.
    NetworkOnlineLobby host_lobby(f.client), guest_lobby(f.client);
    auto publication = require(host_lobby.publish(info(3), ""));
    auto legacy = require(guest_lobby.join(publication.room, "", "Guest"));
    expect(legacy.slot == 0 && legacy.member_capability.empty(), "A room without a scope admits as before");
    Table t(f, 3);
    auto one = t.join("Guest1", "guest-one");
    expect(!t.host->turn_credentials() && !one->turn_credentials(), "No relay configured: members attach directly without credentials");
    until([&] { return attached_count(t.host->roster()) == 2; }, "The room works without a relay");
    require(one->send(0, Bytes{1}));
    expect(wait_signal(*t.host).from_identity == "guest-one", "Signals do not need a relay");
}

void loopback_nine_seats(const std::string& certificate, const std::string& key) {
    // Nine members behind one address: two connections each exactly fill the raised per-address cap of 18.
    auto options = server_options(); options.idle_timeout = 30min;
    Fixture f(options, certificate, key);
    Table t(f, 9);
    for (int i = 1; i < 9; ++i) {
        if (i == 4 || i == 8) t.advance(61s); // four new directory principals per address per minute
        t.join("G" + std::to_string(i), "peer-" + std::to_string(i));
    }
    until([&] { return attached_count(t.host->roster()) == 9; }, "Nine members attach through one address");
    for (const auto& guest : t.guests) until([&] { return attached_count(guest->roster()) == 9; }, "Every member sees nine");
    std::set<std::string> users{t.host->turn_credentials()->username};
    for (const auto& guest : t.guests) users.insert(guest->turn_credentials()->username);
    expect(users.size() == 9, "Nine members, nine distinct credentials");
    expect(t.host->roster().capacity == 9 && f.rooms == 1, "One room of nine");
    // Every member reaches every other member through the service.
    std::vector<NetworkGnsRoom*> all{t.host.get()};
    for (const auto& guest : t.guests) all.push_back(guest.get());
    for (std::uint32_t from = 0; from < 9; ++from)
        for (std::uint32_t to = 0; to < 9; ++to)
            if (to != from) require(all[from]->send(to, Bytes{static_cast<std::uint8_t>(from)}));
    for (std::uint32_t to = 0; to < 9; ++to) {
        std::set<std::uint32_t> senders;
        for (int n = 0; n < 8; ++n) {
            const auto signal = wait_signal(*all[to]);
            expect(signal.bytes.size() == 1 && signal.bytes[0] == signal.from_slot && signal.from_slot != to, "The source slot is the service's");
            senders.insert(signal.from_slot);
        }
        expect(senders.size() == 8, "Every other member reached this one");
    }
}

void loopback_renewal(const std::string& certificate, const std::string& key) {
    auto o = server_options(); o.session_lifetime = 10s; o.max_session_lifetime = 25s; o.renewal_window = 5s;
    Fixture f(o, certificate, key);
    Table t(f, 3);
    auto one = t.join("Guest1", "guest-one");
    until([&] { return attached_count(t.host->roster()) == 2; }, "Two members");
    const auto before = t.host->lease().expires;
    const auto first = t.host->turn_credentials()->username;
    millis = 6000;
    until([&] { return t.host->lease().expires > before + 3s && one->lease().expires > before + 3s; }, "The warning makes a member renew; everyone's lease extends");
    until([&] { return t.host->turn_credentials()->username != first; }, "New connections would use the refreshed credential");
    expect(t.host->status() && one->status(), "Renewal is invisible to the members");
}

void loopback_budgets(const std::string& certificate, const std::string& key) {
    auto o = server_options(); o.room_operations_per_member_per_minute = 8;
    Fixture f(o, certificate, key);
    Table t(f, 3);
    auto one = t.join("Guest1", "guest-one"), two = t.join("Guest2", "guest-two");
    until([&] { return attached_count(t.host->roster()) == 3; }, "Three members");
    for (int i = 0; i < 20; ++i) (void)one->send(2, Bytes{1});
    until([&] { return !one->status(); }, "A member over the room budget is closed");
    expect(one->status().error().code == "EAWR-NET-1777", "...with the budget reason");
    expect(t.host->status() && two->status(), "Other members are unaffected");
}

void loopback_overflow(const std::string& certificate, const std::string& key) {
    // A member that does not poll loses blobs to its bounded queue; it keeps its connection and the room.
    Fixture f(server_options(), certificate, key);
    Table t(f, 3);
    auto one = t.join("Guest1", "guest-one");
    until([&] { return attached_count(t.host->roster()) == 2; }, "Two members");
    for (int i = 0; i < 300; ++i) if (!t.host->send(1, Bytes{static_cast<std::uint8_t>(i)})) std::this_thread::sleep_for(5ms);
    until([&] { return one->dropped_signals() > 0; }, "The guest's receive queue overflows and drops");
    expect(one->status() && t.host->status() && attached_count(t.host->roster()) == 2, "Nobody lost a connection to the overflow");
    std::size_t kept{};
    while (require(one->poll())) ++kept;
    expect(kept > 0 && kept <= 64, "The queue held its bound");
    until([&] { return static_cast<bool>(t.host->send(1, Bytes{200})); }, "The sender queue drains");
    // Blobs still in flight from before the drain arrive first; the new one follows them.
    GnsRoomSignal last;
    for (int n = 0; n < 400 && last.bytes != Bytes({200}); ++n) last = wait_signal(*one);
    expect(last.bytes == Bytes({200}), "...and delivers again once drained");
}
void loopback_end(const std::string& certificate, const std::string& key) {
    Fixture f(server_options(), certificate, key);
    Table t(f, 3);
    auto one = t.join("Guest1", "guest-one");
    until([&] { return attached_count(one->roster()) == 2; }, "Two members");
    t.host.reset();
    until([&] { return !one->status(); }, "The host's disconnection ends staging");
    expect(one->status().error().code == "EAWR-NET-1528", "Members learn that the host left");
    until([&] { return f.rooms == 0; }, "The service drops the room");
}
template<class F> void run(const char* name, F test) {
    try { test(); }
    catch (const std::exception& e) { throw std::runtime_error(std::string(name) + ": " + e.what()); }
}
} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "Artifact directory argument");
        run("formation_and_routing", formation_and_routing); run("admission_races", admission_races);
        run("reservation_expiry", reservation_expiry); run("moderation_and_refill", moderation_and_refill);
        run("lock_and_battle", lock_and_battle); run("pre_start_rules", pre_start_rules);
        run("turn_credentials", turn_credentials_contracts); run("lease", lease_contracts); run("budgets", budget_contracts);
        run("max_rooms", bounds_at_max_rooms);
        run("benign_races", benign_races_keep_connections); run("denylist_bound", denylist_bound);
        run("seat_and_slot", seat_and_slot); run("credential_revocation", credential_revocation);
        const auto directory = std::filesystem::path(argv[1]) / require(random_code());
        std::filesystem::create_directories(directory);
        const auto cert = (directory / "certificate.pem").string(), key = (directory / "key.pem").string();
        require(create_certificate(cert, key));
        run("loopback_room", [&] { loopback_room(cert, key); });
        run("loopback_without_relay", [&] { loopback_without_relay(cert, key); });
        run("loopback_nine_seats", [&] { loopback_nine_seats(cert, key); });
        run("loopback_renewal", [&] { loopback_renewal(cert, key); });
        run("loopback_budgets", [&] { loopback_budgets(cert, key); });
        run("loopback_overflow", [&] { loopback_overflow(cert, key); });
        run("loopback_end", [&] { loopback_end(cert, key); });
        std::filesystem::remove_all(directory);
        std::cout << "Room scope contracts passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
