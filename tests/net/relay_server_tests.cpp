#include "eawr/net/network_introduction.hpp"
#include "eawr/net/relay_server.hpp"
#include "relay_tls.hpp"
#include "relay_prefix_budget.hpp"
#include "relay_directory.hpp"
#include "relay_hash_worker.hpp"
#include "relay_renewal.hpp"
#include "relay_turn_refresh.hpp"
#include <deque>
#include <mutex>
#include <barrier>
#include <map>

#include <atomic>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#endif

namespace {
using namespace eawr::net;
using namespace eawr::net::relay;
using namespace std::chrono_literals;
std::atomic<std::int64_t> millis{};
RelayServer::Clock::time_point now() { return RelayServer::Clock::time_point{} + std::chrono::milliseconds(millis.load()); }
void expect(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
template<class F> void until(F predicate, const char* reason) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(reason);
        std::this_thread::sleep_for(2ms);
    }
}
struct Fixture {
    std::unique_ptr<RelayServer> server;
    NetworkIntroductionOptions client;
    std::atomic<std::size_t> live{}, sessions{};
    std::atomic<bool> paused{};
    std::jthread worker;
    Fixture(RelayServerOptions options, const std::string& certificate, const std::string& key, bool token_required = true, RelayServer::PasswordHash hasher = nullptr,
        std::shared_ptr<TurnRefresh> refresh = nullptr) {
        millis = 0; options.require_access_token = token_required;
        options.port = 0; options.listen_address = "127.0.0.1";
        options.certificate_file = certificate; options.key_file = key; options.access_token = std::string(64, 'a');
        server = require(RelayServer::listen(std::move(options), now, hasher, std::move(refresh)));
        client.address = "127.0.0.1"; client.port = server->port(); client.certificate_name = "localhost";
        client.ca_file = certificate; client.access_token = std::string(64, 'a'); client.timeout = 2s;
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                if (paused) { std::this_thread::sleep_for(1ms); continue; }
                server->poll(); live = server->connections(); sessions = server->sessions();
                std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~Fixture() { worker.request_stop(); worker.join(); }
};
struct Raw {
    std::unique_ptr<TlsStream> tls;
    explicit Raw(const NetworkIntroductionOptions& options) {
        auto ctx = require(client_context(options.ca_file, options.certificate_sha256));
        const auto handle = socket::connect(options.address, options.port);
        expect(handle != socket::invalid, "Raw TCP connect");
        tls = std::make_unique<TlsStream>(std::move(ctx), handle, false, options.certificate_name,
            options.certificate_sha256, options.queue_bytes, options.queue_frames);
    }
    Bytes exchange(Bytes bytes) {
        expect(tls->queue(std::move(bytes)), "Raw request queued");
        return receive();
    }
    Bytes receive() {
        std::optional<Bytes> reply;
        until([&] { tls->pump(); reply = tls->receive(); return reply.has_value() || tls->failed(); }, "Raw request timeout");
        expect(reply.has_value(), "Raw request received a typed response"); return std::move(*reply);
    }
};
unsigned number(const Bytes& bytes) {
    expect(bytes.size() == 6 && bytes[0] == 2 && bytes[1] == static_cast<std::uint8_t>(Kind::error), "Typed error wire contract");
    return read_u32(std::span(bytes).subspan(2));
}
Bytes publish(std::string token = std::string(64, 'a'), std::string identity = "raw-host") {
    auto bytes = message(Kind::publish); string(bytes, token); string(bytes, identity); u32(bytes, 1000); u32(bytes, 1); return bytes;
}

struct DirectoryFixture {
    Directory directory{64, 64}; PrefixBudgets budgets{64};
    RelayServer::Clock::time_point time{};
    std::uint64_t next{};
    std::string prefix{"directory-test"};
    std::map<std::uint64_t, std::string> prefixes;
    std::map<std::string, bool> admitted_prefixes{{prefix, true}};
    struct BridgeSession { std::string owner; bool bound{}, used{}, revoked{}; };
    std::map<std::string, BridgeSession> bridges;
    DirectoryFixture() {
        expect(budgets.admit(prefix, time, 128, 10000) != nullptr, "Directory prefix admitted");
        directory.set_bridge_hooks({
            [this](const auto& code, const auto& owner, bool, bool publishing) {
                const auto found = bridges.find(code);
                return found != bridges.end() && found->second.owner == owner && !found->second.used && !found->second.revoked
                    && (publishing ? !found->second.bound : found->second.bound);
            },
            [this](const auto& code) { bridges.at(code).bound = true; },
            [this](const auto& code) {
                auto& session = bridges.at(code);
                if (!session.bound || session.used || session.revoked) return false;
                session.used = true; return true;
            },
            [this](const auto& code) { bridges.at(code).revoked = true; }
        });
    }
    void bridge(const std::string& code) { bridges.emplace(code, BridgeSession{std::string(32, 'b')}); }
    Bytes call(std::uint64_t id, Bytes bytes) { return directory.request(id, prefixes.contains(id) ? prefixes.at(id) : prefix, bytes, time, budgets); }
    std::pair<std::uint64_t, std::string> open(std::string resume = {}) {
        if (!admitted_prefixes.contains(prefix)) {
            expect(budgets.admit(prefix, time, 128, 10000) != nullptr, "Additional directory prefix admitted"); admitted_prefixes[prefix] = true;
        }
        const auto id = ++next; prefixes[id] = prefix; auto request = message(Kind::directory_open); string(request, resume);
        auto response = call(id, std::move(request)); expect(response[1] == static_cast<std::uint8_t>(Kind::directory_opened), "Directory principal created");
        Reader reader{std::span(response).subspan(2)}; auto principal = reader.string(32);
        expect(reader.done() && code_valid(principal), "Principal is a long private capability"); return {id, principal};
    }
};
OnlineRoomMetadata info() {
    OnlineRoomMetadata value; value.name = "Guest room"; value.host_name = "Host";
    value.build = "build-hash"; value.protocol = "protocol-tag"; value.content = "content-tag";
    value.mod = "mod-tag"; value.mode = "mode-from-data"; value.map = "map-from-data"; value.capacity = 3; return value;
}
Bytes room_publish(OnlineRoomMetadata value = info(), std::string password = {}, std::string invitation = {}) {
    auto bytes = message(Kind::directory_publish); string(bytes, ""); metadata(bytes, value); string(bytes, password); string(bytes, invitation);
    string(bytes, invitation.empty() ? std::string{} : std::string(32, 'b')); return bytes;
}
OnlineRoomPublication publication(const Bytes& bytes) {
    expect(bytes[1] == static_cast<std::uint8_t>(Kind::directory_published), "Room published");
    Reader reader{std::span(bytes).subspan(2)}; OnlineRoomPublication result{reader.string(32), reader.string(32), reader.u32()};
    expect(reader.done() && result.lease_ms == 30000 && result.room != result.ownership, "Listing and ownership are distinct"); return result;
}
Bytes heartbeat(const OnlineRoomPublication& room, OnlineRoomMetadata value = info()) {
    auto bytes = message(Kind::directory_heartbeat); string(bytes, room.room); string(bytes, room.ownership); metadata(bytes, value); return bytes;
}
Bytes room_join(const OnlineRoomPublication& room, std::string password = {}, std::string name = "Guest") {
    auto bytes = message(Kind::directory_join); string(bytes, room.room); string(bytes, password); string(bytes, name); return bytes;
}
Bytes listing(std::string after = {}, std::uint32_t limit = 50) {
    auto bytes = message(Kind::directory_list); string(bytes, after); u32(bytes, limit); return bytes;
}
void directory_contracts() {
    DirectoryFixture f; const auto [host, principal] = f.open(); const auto room = publication(f.call(host, room_publish()));
    const auto [reader, reader_principal] = f.open(); (void)reader_principal;
    auto list_bytes = f.call(reader, listing()); auto page = require(room_page(list_bytes));
    expect(page.rooms.size() == 1 && page.rooms[0].room == room.room && page.rooms[0].metadata == info(), "All compatibility/display tags survive listing");
    const std::string listed(list_bytes.begin(), list_bytes.end());
    expect(listed.find(principal) == std::string::npos && listed.find(room.ownership) == std::string::npos, "Listing has no principal or ownership capability");
    expect(number(f.call(reader, listing())) == 1542, "One query per second per principal");
    const auto [resumed, same_principal] = f.open(principal); expect(same_principal == principal, "Principal resume capability preserved");
    expect(number(f.call(resumed, room_publish())) == 1543, "One room per principal across connections");
    expect(number(f.call(reader, heartbeat(room))) == 1541, "Another principal cannot renew room even with ownership credential");
    f.time += 20s; expect(f.call(host, heartbeat(room)) == message(Kind::directory_ok), "Heartbeat renews lease");
    f.time += 11s; expect(require(room_page(f.call(reader, listing()))).rooms.size() == 1, "Renewed room survives original expiry");
    f.directory.disconnect(host, f.time);
    expect(number(f.call(reader, room_join(room, {}, "host"))) == 1553, "Disconnected host name stays reserved until lease expiry");
    expect(number(f.call(resumed, room_join(room, {}, "Guest"))) == 1543, "Host principal cannot consume a guest reservation in its own room");
    expect(f.call(resumed, heartbeat(room)) == message(Kind::directory_ok), "Disconnected host can resume room with both capabilities");
    auto starting = info(); starting.starting = true; expect(f.call(resumed, heartbeat(room, starting)) == message(Kind::directory_ok), "Mark starting");
    f.time += 1s; expect(require(room_page(f.call(reader, listing()))).rooms.empty(), "Starting rooms excluded");
    expect(number(f.call(reader, room_join(room))) == 1544, "Starting rooms refuse new seats");
    starting.starting = false; starting.closed = true; expect(f.call(resumed, heartbeat(room, starting)) == message(Kind::directory_ok), "Mark closed");
    f.time += 1s; expect(require(room_page(f.call(reader, listing()))).rooms.empty(), "Closed rooms excluded");
    f.time += 30s; f.directory.expire(f.time); expect(f.directory.rooms() == 0, "Lease expires at exact deadline");
    expect(number(f.call(resumed, heartbeat(room))) == 1541, "Expired capability cannot resurrect room");
    const auto replacement = publication(f.call(resumed, room_publish()));
    auto remove = message(Kind::directory_remove); string(remove, replacement.room); string(remove, replacement.ownership);
    expect(f.call(resumed, std::move(remove)) == message(Kind::directory_ok) && f.directory.rooms() == 0, "Owner removal clears directory and principal binding");
    f.directory.disconnect(reader, f.time); f.directory.disconnect(resumed, f.time);
    f.time += 60s; f.directory.expire(f.time); expect(f.directory.principals() == 0, "Disconnected principal history expires with bounded retention");
    DirectoryFixture pages;
    for (unsigned i = 0; i < 51; ++i) { pages.prefix = "page-" + std::to_string(i); const auto [id, credential] = pages.open(); (void)credential; publication(pages.call(id, room_publish())); }
    const auto [query, credential] = pages.open(); (void)credential;
    auto first = require(room_page(pages.call(query, listing())));
    expect(first.rooms.size() == 50 && !first.next.empty(), "Page never exceeds fifty");
    pages.time += 1s; auto second = require(room_page(pages.call(query, listing(first.next))));
    expect(second.rooms.size() == 1 && second.next.empty() && second.rooms[0].room > first.next, "Stable keyset pagination");
    expect(number(pages.call(query, listing({}, 51))) == 1540, "Oversize page refused before allocation");
    Directory tiny(1, 1); PrefixBudgets tiny_budget(1);
    expect(tiny_budget.admit("tiny", {}, 8, 30) != nullptr, "Tiny prefix admitted");
    for (std::uint64_t i = 1; i <= 4; ++i) { auto request = message(Kind::directory_open); string(request, ""); expect(tiny.request(i, "tiny", request, {}, tiny_budget)[1] == 30, "Bounded principal slots admitted"); }
    auto request = message(Kind::directory_open); string(request, ""); expect(number(tiny.request(5, "tiny", request, {}, tiny_budget)) == 1549, "Principal history capacity enforced");
}
void password_seat_chat_contracts() {
    const std::string password = "only-in-admission";
    DirectoryFixture admission; const auto [owner, owner_principal] = admission.open(); (void)owner_principal;
    auto capacity = info(); capacity.capacity = 2;
    const auto limited = publication(admission.call(owner, room_publish(capacity, password)));
    const auto [a, ap] = admission.open(); const auto [b, bp] = admission.open(); (void)ap; (void)bp;
    admission.budgets.failed_guess(admission.prefix, admission.time, true);
    expect(!admission.budgets.guess_allowed(admission.prefix, admission.time, 1), "Global alias cap exhausted");
    expect(number(admission.call(a, room_join(limited, password, "hOsT"))) == 1553, "Guest names are unique under ASCII case folding");
    auto winner = admission.call(a, room_join(limited, password, " Guest\n\x01 "));
    expect(winner[1] == static_cast<std::uint8_t>(Kind::directory_admitted), "Full room ID/password unaffected by global alias cap");
    expect(number(admission.call(b, room_join(limited, password))) == 1545, "Exactly one last-seat reservation wins in owner order");
    Reader seat_reader{std::span(winner).subspan(2)}; seat_reader.string(32); const auto seat = seat_reader.u32(); const auto unbound = seat_reader.string(32);
    expect(seat == 2 && unbound.empty() && seat_reader.done(), "Typed reservation returns a stable seat");
    auto chat = message(Kind::directory_chat); string(chat, " hi\n\x01there ");
    expect(admission.call(a, chat) == message(Kind::directory_ok), "Sanitized chat admitted");
    expect(number(admission.call(a, chat)) == 1551, "Chat throttle per principal");
    expect(number(admission.call(b, chat)) == 1548, "Unadmitted client cannot send room chat");
    auto poll = message(Kind::directory_poll); u32(poll, 0);
    auto chat_bytes = admission.call(owner, poll); auto lines = require(chat_page(chat_bytes, 0));
    expect(lines.lines.size() == 1 && lines.lines[0].name == "Guest" && lines.lines[0].text == "hithere", "Names and text sanitized without spoofing sender");
    expect(lines.members.size() == 2, "Room chat roster contains only safe seat/name pairs");
    const std::string transcript(chat_bytes.begin(), chat_bytes.end());
    expect(transcript.find(password) == std::string::npos && transcript.find(limited.ownership) == std::string::npos
        && transcript.find(ap) == std::string::npos, "Chat contains no password, ownership or principal capability");
    for (unsigned i = 0; i < 205; ++i) {
        admission.time += 1s;
        if (i % 10 == 0) expect(admission.call(owner, heartbeat(limited, capacity)) == message(Kind::directory_ok), "Lease renewed during chat history test");
        expect(admission.call(a, chat) == message(Kind::directory_ok), "Chat history append");
    }
    lines = require(chat_page(admission.call(owner, poll), 0));
    expect(lines.lines.size() == 200 && lines.lines.front().sequence == 7 && lines.through == 206, "Room history is a bounded two-hundred-line ring");
    auto too_long = message(Kind::directory_chat); string(too_long, std::string(129, 'x'));
    expect(number(admission.call(a, too_long)) == 1540, "Chat length cap enforced");
    auto kick = message(Kind::directory_kick); string(kick, limited.room); string(kick, limited.ownership); u32(kick, seat);
    expect(number(admission.call(b, kick)) == 1541, "Only owner can remove chat membership");
    expect(admission.call(owner, kick) == message(Kind::directory_ok), "Host can remove a guest from chat");
    expect(number(admission.call(a, chat)) == 1548 && number(admission.call(a, poll)) == 1548, "Removed guest cannot send or read chat");
    admission.directory.disconnect(a, admission.time);
    expect(admission.call(b, room_join(limited, password))[1] == static_cast<std::uint8_t>(Kind::directory_admitted), "Disconnect releases reservation");
    expect(admission.directory.operator_remove(limited.room, 3), "Operator can remove a guest from chat");
    expect(number(admission.call(b, chat)) == 1548 && !admission.directory.operator_remove(limited.room, 1), "Operator removal enforced and host seat protected");
    expect(require(password_hash(password, std::string(32, 'a'))) != require(password_hash(password, std::string(32, 'b'))), "Independent salts produce different slow hashes");
    expect(!password_hash(std::string(129, 'x'), std::string(32, 'a')), "Slow hash has bounded input");

    DirectoryFixture cooldown; cooldown.prefix = "password-owner"; const auto [h, hp] = cooldown.open(); (void)hp;
    const auto private_room = publication(cooldown.call(h, room_publish(info(), password)));
    cooldown.prefix = "password-guests"; const auto [g, gp] = cooldown.open(); (void)gp;
    for (unsigned i = 0; i < 2; ++i) { cooldown.time += 20s; cooldown.call(h, heartbeat(private_room)); }
    cooldown.time += 15s; cooldown.call(h, heartbeat(private_room));
    for (unsigned i = 0; i < 4; ++i) expect(number(cooldown.call(g, room_join(private_room, "wrong"))) == 1546, "Four password failures charged to prefix");
    expect(number(cooldown.call(g, room_join(private_room, password))) == 1547, "Fifth slow hash refused by prefix quota");
    cooldown.time += 5s; cooldown.call(h, heartbeat(private_room));
    expect(number(cooldown.call(g, room_join(private_room, "wrong"))) == 1546, "Fifth rolling guess enters shared cooldown across hash windows");
    expect(!cooldown.budgets.guess_allowed(cooldown.prefix, cooldown.time, 100), "Password failures share alias cooldown");
    for (unsigned i = 0; i < 2; ++i) { cooldown.time += 20s; cooldown.call(h, heartbeat(private_room)); }
    cooldown.time += 19999ms; cooldown.call(h, heartbeat(private_room));
    expect(number(cooldown.call(g, room_join(private_room, password))) == 1547, "Cooldown lasts full sixty seconds");
    cooldown.time += 1ms; expect(cooldown.call(g, room_join(private_room, password))[1] == 33, "Correct password allowed after cooldown");

}

Bytes removal(const OnlineRoomPublication& room) { auto bytes = message(Kind::directory_remove); string(bytes, room.room); string(bytes, room.ownership); return bytes; }
void directory_quota_contracts() {
    {
        DirectoryFixture empty; empty.prefix = "empty-password-owner"; const auto [owner, credential] = empty.open(); (void)credential;
        const auto room = publication(empty.call(owner, room_publish(info(), "private")));
        empty.prefix = "empty-password-guest"; const auto [guest, guest_credential] = empty.open(); (void)guest_credential;
        expect(number(empty.call(guest, room_join(room))) == 1546, "Empty password refused before any slow-hash charge");
        for (unsigned i = 0; i < 4; ++i) {
            expect(empty.call(guest, room_join(room, "private"))[1] == 33, "Empty input leaves all four real hash attempts available");
            empty.call(guest, message(Kind::directory_leave));
        }
        expect(number(empty.call(guest, room_join(room, "private"))) == 1547, "Fifth real hash reaches the prefix cap");
    }
    DirectoryFixture f; const auto [a, ap] = f.open(); (void)ap;
    for (unsigned i = 0; i < 128; ++i) {
        const auto reply = f.call(a, room_publish(info(), "private"));
        if (i < 4) { const auto room = publication(reply); expect(f.call(a, removal(room)) == message(Kind::directory_ok), "Password publication removed"); }
        else expect(number(reply) == 1547, "One prefix cannot consume global hash capacity");
    }
    f.prefix = "independent-prefix"; const auto [b, bp] = f.open(); (void)bp;
    publication(f.call(b, room_publish(info(), "private")));
    DirectoryFixture global;
    for (unsigned prefix = 0; prefix < 32; ++prefix) {
        global.prefix = "global-hash-" + std::to_string(prefix); const auto [id, credential] = global.open(); (void)credential;
        for (unsigned i = 0; i < 4; ++i) { const auto room = publication(global.call(id, room_publish(info(), "private"))); global.call(id, removal(room)); }
    }
    global.prefix = "global-overflow"; const auto [over, op] = global.open(); (void)op;
    expect(number(global.call(over, room_publish(info(), "private"))) == 1547, "Global slow-hash cap is enforced across prefixes");
    global.time += 60s; expect(global.call(over, room_publish(info(), "private"))[1] == 32, "Hash budget renews after its window");
    DirectoryFixture room_cap; room_cap.prefix = "room-owner"; const auto [owner, principal] = room_cap.open(); (void)principal;
    const auto room = publication(room_cap.call(owner, room_publish(info(), "private")));
    unsigned attempts{};
    for (unsigned prefix = 0; prefix < 8; ++prefix) {
        room_cap.prefix = "room-guess-" + std::to_string(prefix); const auto [guest, credential] = room_cap.open(); (void)credential;
        for (unsigned i = 0; i < 4; ++i) {
            auto reply = room_cap.call(guest, room_join(room, "private"));
            if (++attempts <= 30) { expect(reply[1] == 33, "Room hash budget admits its first thirty attempts"); room_cap.call(guest, message(Kind::directory_leave)); }
            else expect(number(reply) == 1547, "Per-room hash cap enforced");
        }
    }
    DirectoryFixture principal_cap;
    for (unsigned i = 0; i < 4; ++i) principal_cap.open();
    auto open = message(Kind::directory_open); string(open, "");
    expect(number(principal_cap.call(99, open)) == 1549, "Only four new principals per prefix per minute");
    expect(require(room_page(principal_cap.call(1, listing()))).rooms.empty(), "Existing principal can still list at creation quota");
    auto resume = message(Kind::directory_open);
    // Obtain a separate fixture's resume capability while its creation quota is full.
    DirectoryFixture rooms; const auto [first, credential] = rooms.open(); const auto [second, second_credential] = rooms.open();
    const auto [third, third_credential] = rooms.open(); (void)second_credential; (void)third_credential;
    publication(rooms.call(first, room_publish())); publication(rooms.call(second, room_publish()));
    expect(number(rooms.call(third, room_publish())) == 1549, "Only two concurrent rooms per prefix");
    rooms.open(); string(resume, credential); expect(rooms.call(100, resume)[1] == 30, "Capability resumes remain available at creation quota");
}
void directory_codec_contracts() {
    auto page = message(Kind::directory_page); string(page, ""); u32(page, 51);
    expect(!room_page(page), "Malformed list count rejected before row allocation");
    page = message(Kind::directory_page); string(page, ""); u32(page, 1);
    string(page, std::string(32, 'a')); auto value = info(); value.name = "bad\nname"; metadata(page, value); u32(page, 0);
    expect(!room_page(page), "Unsanitized service row rejected");
    value = info(); page = message(Kind::directory_page); string(page, ""); u32(page, 1); string(page, std::string(32, 'a')); metadata(page, value); u32(page, 2);
    expect(!room_page(page), "Nonboolean password flag rejected");
    auto chat = message(Kind::directory_chat_page); u32(chat, 1); u32(chat, 1); u32(chat, 1); u32(chat, 2); string(chat, "Guest"); string(chat, "Hi"); u32(chat, 0);
    expect(chat_page(chat, 0).has_value() && !chat_page(chat, 1), "Chat codec enforces monotonically increasing cursor");
    chat.push_back(0); expect(!chat_page(chat, 0), "Chat codec rejects trailing input");
    expect(clean_text("\xf0\x80\x80\x80X", 20) == "X" && clean_text("\xe2\x80\xaeX", 20) == "X", "Overlong UTF-8 and bidi overrides removed");
    expect(clean_text("\xc3\xa9", 1).empty(), "Text cap never splits UTF-8");
}

std::atomic<bool> hash_entered{}, hash_release{};
eawr::core::Result<std::string> gated_hash(std::string_view password, std::string_view salt) {
    hash_entered = true;
    while (!hash_release) std::this_thread::sleep_for(1ms);
    return password_hash(password, salt);
}
eawr::core::Result<std::string> gated_queue_hash(std::string_view password, std::string_view salt) {
    hash_entered = true;
    while (!hash_release) std::this_thread::sleep_for(1ms);
    if (password != "private" || salt != std::string(32, 'a'))
        return eawr::core::Result<std::string>::failure(error(1540, "Queue fixture input was not copied correctly"));
    return eawr::core::Result<std::string>::success(std::string(64, 'a'));
}
struct ReleaseHash { ~ReleaseHash() { hash_release = true; } };

void directory_bridge_contracts() {
    DirectoryFixture unhooked; const auto [unhooked_host, unhooked_principal] = unhooked.open(); (void)unhooked_principal;
    unhooked.directory.set_bridge_hooks({});
    expect(number(unhooked.call(unhooked_host, room_publish(info(), {}, std::string(32, 'a')))) == 1544,
        "directory without bridge hooks refuses a capability binding");
    DirectoryFixture f; const auto [host, principal] = f.open(); (void)principal;
    const std::string secret(32, 'a'); f.bridge(secret);
    expect(number(f.call(host, room_publish(info(), {}, "ABCD"))) == 1540, "a guessable alias cannot authorize a room binding");
    auto room = publication(f.call(host, room_publish(info(), "private", secret)));
    const auto [guest, guest_principal] = f.open(); (void)guest_principal;
    expect(number(f.call(guest, room_join(room, "wrong"))) == 1546, "wrong password never releases the game capability");
    auto list = message(Kind::directory_list); string(list, ""); u32(list, 50);
    const auto listed = f.call(guest, list);
    expect(std::search(listed.begin(), listed.end(), secret.begin(), secret.end()) == listed.end(), "listing contains no bridge invitation");
    const auto admitted = f.call(guest, room_join(room, "private"));
    Reader reader{std::span(admitted).subspan(2)};
    expect(admitted[1] == static_cast<std::uint8_t>(Kind::directory_admitted) && reader.string(32) == room.room && reader.u32() == 2
        && reader.string(32) == secret && reader.done(), "only admitted guest receives the binding");
    f.call(guest, message(Kind::directory_leave));
    expect(number(f.call(guest, room_join(room, "private"))) == 1544, "binding cannot be released twice after a guest leaves");
    f.call(host, removal(room));
    expect(number(f.call(guest, room_join(room, "private"))) == 1544, "host removal destroys its binding");
    expect(number(f.call(host, room_publish(info(), {}, secret))) == 1544, "retired fixture session cannot be rebound");
    const std::string replacement(32, 'c'); f.bridge(replacement);
    room = publication(f.call(host, room_publish(info(), {}, replacement)));
    f.directory.set_bridge_hooks({});
    expect(number(f.call(guest, room_join(room))) == 1544, "a bound room refuses admission when hooks disappear");
    f.directory.invalidate_bridge(replacement);
    expect(number(f.call(guest, room_join(room))) == 1544 && f.directory.rooms() == 0, "host signalling session end destroys its room binding");
}

void hash_worker_contracts(const std::string& certificate, const std::string& key) {
    for (unsigned i = 0; i < 32; ++i) { PasswordHashWorker idle; }
    hash_entered = false; hash_release = false;
    {
        // This is a work/queue contract, not a KDF throughput budget. Password and
        // quota contracts plus the real-server gate below retain real PBKDF2.
        PasswordHashWorker worker(gated_queue_hash); ReleaseHash release;
        for (std::uint64_t i = 0; i < 16; ++i)
            expect(worker.submit({i, {"private", std::string(32, 'a')}}), "Bounded worker slot admitted");
        until([] { return hash_entered.load(); }, "Hash worker reached deterministic gate");
        expect(!worker.submit({17, {"private", std::string(32, 'a')}}), "Hash worker queue bounds all pending/completed jobs");
        expect(!worker.take(), "Owner can poll without waiting for a gated hash");
        hash_release = true; unsigned completed{};
        until([&] {
            if (auto result = worker.take()) {
                expect(result->output.value.has_value() && result->output.value.value() == std::string(64, 'a'), "Copied queue inputs reached the injected provider");
                ++completed;
            }
            return completed == 16;
        }, "All copied hash jobs finish");
    }
    hash_entered = false; hash_release = false;
    RelayServerOptions options; Fixture f(options, certificate, key, false, gated_hash); ReleaseHash release;
    auto host = require(NetworkGnsIntroduction(f.client).publish("signal-host", 10s));
    auto peer = require(NetworkGnsIntroduction(f.client).join(host.join_code, "signal-guest"));
    until([&] { return !host.endpoint->rendezvous().remote_identity.empty(); }, "Signalling pair ready before gated password request");
    NetworkOnlineLobby directory(f.client);
    std::atomic<bool> published{}; std::optional<eawr::core::Result<OnlineRoomPublication>> result;
    std::jthread publisher([&] { result = directory.publish(info(), "private"); published = true; });
    until([] { return hash_entered.load(); }, "Server password job blocked on its hash worker");
    require(host.endpoint->send(Bytes{42})); std::optional<GnsSignal> signal;
    until([&] { signal = require(peer->poll()); return signal.has_value(); }, "Relay owner continues serving admitted session during hash");
    expect(signal->bytes == Bytes{42} && !published, "Signalling progressed before the blocked password hash returned");
    hash_release = true; publisher.join(); expect(result.has_value() && result->has_value(), "Owner commits completed password publication");
}
void reconnect_and_race_contracts(const std::string& certificate, const std::string& key) {
    {
        Fixture f({}, certificate, key, false); auto options = f.client; options.timeout = 250ms;
        NetworkOnlineLobby host(options); auto value = info(); value.name = "Skirmish night at the Core Worlds"; value.host_name = "Host with trailing  ";
        const auto room = require(host.publish(value)); require(host.heartbeat(room, value));
        const auto principal = host.principal();
        f.paused = true;
        std::atomic<bool> begun{}, finished{}; std::optional<eawr::core::Result<void>> failed;
        std::jthread requester([&] { begun = true; failed = host.heartbeat(room, value); finished = true; });
        until([&] { return begun.load(); }, "Client exchange started"); std::this_thread::sleep_for(20ms);
        expect(host.principal() == principal && !finished, "Principal snapshot does not wait for network I/O mutex");
        requester.join(); expect(failed && !*failed && failed->error().code == "EAWR-NET-1552", "Forced transport timeout returned");
        f.paused = false; until([&] { return f.live == 0; }, "Timed-out client connection released");
        require(host.heartbeat(room, value)); expect(host.principal() == principal, "Same client reconnects and resumes its principal/room");
        f.paused = true; auto lost_chat = host.chat(0);
        expect(!lost_chat && lost_chat.error().code == "EAWR-NET-1552", "Chat request forced to time out");
        f.paused = false; until([&] { return f.live == 0; }, "Lost chat connection released before resume");
        require(host.chat(0)); require(host.send_chat("Reattached"));
        const auto listed = require(host.list());
        expect(listed.rooms.size() == 1 && listed.rooms[0].room == room.room && host.principal() == principal,
            "Resumed host chat keeps its own room identity and does not require a new publish");
        require(host.remove(room));
    }
    {
        Fixture f({}, certificate, key, false); NetworkOnlineLobby host(f.client), a(f.client), b(f.client);
        auto value = info(); value.capacity = 2; const auto room = require(host.publish(value, "private"));
        // Establish both principals first; the barrier starts both independent
        // TLS exchanges on the same deterministic last-seat state.
        require(a.list()); require(b.list()); std::barrier gate(3);
        std::optional<eawr::core::Result<OnlineRoomAdmission>> first, second;
        std::jthread one([&] { gate.arrive_and_wait(); first = a.join(room.room, "private", "Guest A"); });
        std::jthread two([&] { gate.arrive_and_wait(); second = b.join(room.room, "private", "Guest B"); });
        gate.arrive_and_wait(); one.join(); two.join();
        expect(first && second && first->has_value() != second->has_value(), "Exactly one simultaneous client reserves the last seat");
        expect((!*first ? first->error().code : second->error().code) == "EAWR-NET-1545", "Concurrent loser gets the typed full-room refusal");
        if (*first) require(a.leave()); else require(b.leave()); require(host.remove(room));
    }
}

void bridge_loopback(const std::string& certificate, const std::string& key) {
    std::mutex diagnostics_mutex; std::vector<eawr::core::Diagnostic> diagnostics;
    RelayServerOptions options;
    options.diagnostics = [&](const auto& diagnostic) { std::lock_guard lock(diagnostics_mutex); diagnostics.push_back(diagnostic); };
    options.room_aliases = true; options.idle_timeout = 5min;
    Fixture f(options, certificate, key, false);
    f.client.public_alias = false;
    auto signal = require(NetworkGnsIntroduction(f.client).publish("private-host", 2min));
    expect(signal.room_alias.empty() && code_valid(signal.publisher_ownership), "private publication returns host-only ownership and no alias");
    auto early = NetworkGnsIntroduction(f.client).join("ABCD", "early-outsider");
    expect(!early && early.error().code == "EAWR-NET-1523", "alias join refuses before password binding exists");
    NetworkOnlineLobby owner(f.client), guest(f.client);
    auto forged = owner.publish_bridge(info(), "private", signal.join_code, std::string(32, 'a'));
    expect(!forged && forged.error().code == "EAWR-NET-1544", "possession of a guest invitation cannot bind another owner's session");
    expect(require(owner.list()).rooms.empty(), "recoverable binding refusal leaves the directory client usable");
    const auto room = require(owner.publish_bridge(info(), "private", signal.join_code, signal.publisher_ownership));
    auto guessed = NetworkGnsIntroduction(f.client).join("ABCD", "outsider");
    expect(!guessed && guessed.error().code == "EAWR-NET-1523", "alias join refuses throughout private publication");
    auto wrong = guest.join(room.room, "wrong", "Guest");
    expect(!wrong && wrong.error().code == "EAWR-NET-1546", "password refusal returns no capability");
    const auto admitted = require(guest.join(room.room, "private", "Guest"));
    expect(admitted.bridge_invitation == signal.join_code, "only admitted guest receives the private binding");
    require(guest.leave());
    auto repeated = guest.join(room.room, "private", "Guest");
    expect(!repeated && repeated.error().code == "EAWR-NET-1544", "leaving cannot restore consumption");
    require(owner.remove(room));
    auto held = NetworkGnsIntroduction(f.client).join(admitted.bridge_invitation, "late-guest");
    expect(!held && held.error().code == "EAWR-NET-1523", "room removal revokes a capability already held by a guest");
    auto rebound = guest.publish_bridge(info(), {}, signal.join_code, std::string(32, 'a'));
    expect(!rebound && rebound.error().code == "EAWR-NET-1544", "removed binding cannot be rebound by an admitted guest");
    auto stale = owner.publish_bridge(info(), {}, signal.join_code, signal.publisher_ownership);
    expect(!stale && stale.error().code == "EAWR-NET-1544", "removed session cannot be rearmed by its publisher either");
    millis = 1000;
    expect(require(owner.list()).rooms.empty(), "all refused publishes remain non-latching");

    auto next = require(NetworkGnsIntroduction(f.client).publish("new-host", 2min));
    const auto replacement = require(owner.publish_bridge(info(), {}, next.join_code, next.publisher_ownership));
    require(next.endpoint->close_publication());
    millis = 2000;
    expect(require(owner.list()).rooms.empty(), "signalling revocation removes the directory row, independently of admission checks");
    auto ended = guest.join(replacement.room, {}, "Guest");
    expect(!ended && ended.error().code == "EAWR-NET-1544", "revoked signalling session has no binding");

    millis = 62001; // advance beyond the shared-prefix guess cooldown before the fresh session
    auto lapse = require(NetworkGnsIntroduction(f.client).publish("lapse-host", 2min));
    const auto leased = require(owner.publish_bridge(info(), {}, lapse.join_code, lapse.publisher_ownership));
    const auto before_lapse = require(guest.join(leased.room, {}, "Guest")); require(guest.leave());
    millis = 94002;
    expect(require(owner.list()).rooms.empty(), "lease expiry removes its consumed row");
    auto rearmed = owner.publish_bridge(info(), {}, lapse.join_code, lapse.publisher_ownership);
    expect(!rearmed && rearmed.error().code == "EAWR-NET-1544", "a consumed session cannot be rearmed by a new room after lapse");
    auto expired_bearer = NetworkGnsIntroduction(f.client).join(before_lapse.bridge_invitation, "expired-guest");
    expect(!expired_bearer && expired_bearer.error().code == "EAWR-NET-1523", "lease expiry revokes a previously delivered capability");
    auto paired_host = require(NetworkGnsIntroduction(f.client).publish("paired-host", 2min));
    const auto paired_room = require(owner.publish_bridge(info(), {}, paired_host.join_code, paired_host.publisher_ownership));
    const auto pair_admission = require(guest.join(paired_room.room, {}, "Guest"));
    auto paired_guest = require(NetworkGnsIntroduction(f.client).join(pair_admission.bridge_invitation, "paired-guest"));
    until([&] { return !paired_host.endpoint->rendezvous().remote_identity.empty(); }, "Paired game begins before directory removal");
    require(owner.remove(paired_room));
    require(paired_guest->send(Bytes{77}));
    std::optional<GnsSignal> live_signal;
    until([&] { live_signal = require(paired_host.endpoint->poll()); return live_signal.has_value(); }, "Directory removal preserves a paired game's signalling");
    expect(live_signal->bytes == Bytes{77}, "revocation applies to admission, not an already-paired game");
    {
        const std::lock_guard lock(diagnostics_mutex);
        for (const auto& diagnostic : diagnostics) {
            const auto logged = diagnostic.code + " " + diagnostic.message;
            for (const auto& secret : {signal.join_code, signal.publisher_ownership, next.join_code, next.publisher_ownership,
                    lapse.join_code, lapse.publisher_ownership, paired_host.join_code, paired_host.publisher_ownership,
                    std::string("private"), std::string("wrong")})
                expect(logged.find(secret) == std::string::npos, "server diagnostic sink contains no bridge credentials or passwords");
        }
    }
    for (const auto& diagnostic : {wrong.error(), repeated.error(), forged.error(), stale.error(), rearmed.error()}) {
        const auto logged = diagnostic.code + " " + diagnostic.message;
        expect(logged.find(signal.join_code) == std::string::npos && logged.find(signal.publisher_ownership) == std::string::npos
            && logged.find("private") == std::string::npos, "binding/password diagnostics contain no private credentials");
    }
}
void directory_loopback(const std::string& certificate, const std::string& key) {
    RelayServerOptions options; Fixture f(options, certificate, key, false);
    f.client.address = "localhost";
    NetworkOnlineLobby host(f.client), guest(f.client), outsider(f.client);
    const auto room = require(host.publish(info(), "private"));
    auto page = require(guest.list()); expect(page.rooms.size() == 1 && page.rooms[0].passworded, "Directory client list over TLS");
    auto wrong = guest.join(room.room, "bad", "Guest"); expect(!wrong && wrong.error().code == "EAWR-NET-1546", "Client typed password refusal");
    const auto seat = require(guest.join(room.room, "private", "Guest")); expect(seat.seat == 2, "Client password admission and reservation");
    require(guest.send_chat("Hello")); auto chat = require(host.chat(0)); expect(chat.lines.size() == 1 && chat.lines[0].text == "Hello", "Client room chat over TLS");
    auto refused = outsider.chat(0); expect(!refused && refused.error().code == "EAWR-NET-1548", "Client room isolation");
    require(host.heartbeat(room, info())); require(host.kick(room, seat.seat));
    refused = guest.chat(0); expect(!refused && refused.error().code == "EAWR-NET-1548", "Client host removal over TLS");
    require(guest.leave()); require(host.remove(room));
}
void loopback(const std::string& certificate, const std::string& key) {
    RelayServerOptions options;
    options.turn_uri = "turn:127.0.0.1:3478"; options.turn_secret = std::string(64, 'b');
    Fixture fixture(options, certificate, key);
    fixture.client.address = "localhost";
    require(NetworkGnsIntroduction::health(fixture.client));
    std::shared_ptr<GnsSignallingEndpoint> host, client;
    std::string code;
    {
        NetworkGnsIntroduction service(fixture.client);
        auto published = require(service.publish("host-peer", 1s)); code = published.join_code; host = published.endpoint;
        expect(code_valid(code), "Join code contains 128 bits as 32 hex digits");
        expect(!host->send(Bytes{1}), "Unpaired endpoint cannot signal");
        auto duplicate = service.join(code, "host-peer");
        expect(!duplicate && duplicate.error().code == "EAWR-NET-1524", "Duplicate identity rejected");
        auto guest_options = fixture.client; guest_options.access_token.clear();
        client = require(NetworkGnsIntroduction(guest_options).join(code, "client-peer"));
        auto third = service.join(code, "third-peer");
        expect(!third && third.error().code == "EAWR-NET-1524", "Consumed code cannot admit a third peer");
    }
    until([&] { return host->rendezvous().remote_identity == "client-peer"; }, "Host receives paired identity");
    expect(client->rendezvous().remote_identity == "host-peer" && client->rendezvous().session == code, "Paired rendezvous contract");
    const auto turn = client->turn_credentials(); const auto host_turn = host->turn_credentials();
    expect(turn && host_turn && turn->uri == options.turn_uri && turn->username != host_turn->username
        && turn->lifetime == 86460s && turn->password.size() == 28, "Bounded per-peer TURN REST credentials");
    expect(turn->password != options.turn_secret, "Server-only TURN secret never sent to peers");
    Bytes payload{0, 1, 255, 42}; require(host->send(payload)); payload[0] = 99;
    std::optional<GnsSignal> signal;
    until([&] { signal = require(client->poll()); return signal.has_value(); }, "Forwarded opaque signal arrives");
    expect(signal->from == "host-peer" && signal->bytes == Bytes({0, 1, 255, 42}), "Send copies payload and authorizes sender");
    require(client->send(Bytes(65536, 37)));
    until([&] { signal = require(host->poll()); return signal.has_value(); }, "Maximum reverse signal arrives");
    expect(signal->from == "client-peer" && signal->bytes == Bytes(65536, 37), "Full 64 KiB signal survives framing/TLS");
    expect(!host->send({}) && !host->send(Bytes(65537)), "Signal bounds reject empty/oversize sends");
    std::atomic<bool> successful{true};
    std::jthread sender([&] { for (unsigned i = 0; i < 16; ++i) if (!host->send(Bytes{static_cast<std::uint8_t>(i)})) successful = false; });
    sender.join();
    for (unsigned i = 0; i < 16; ++i) {
        until([&] { signal = require(client->poll()); return signal.has_value(); }, "GNS-thread copied send arrives");
        expect(signal->bytes == Bytes{static_cast<std::uint8_t>(i)}, "Signalling preserves callback order");
    }
    expect(successful, "Threaded sends succeed");
    millis = 1000;
    require(client->send(Bytes{5}));
    until([&] { signal = require(host->poll()); return signal.has_value(); }, "Paired service survives join deadline");
    expect(signal->bytes == Bytes{5}, "Admission expiry does not end a paired endpoint");
    client.reset();
    until([&] { return !host->poll(); }, "Peer disconnect ends signalling session");
    host.reset();
    until([&] { return fixture.live == 0 && fixture.sessions == 0; }, "Disconnect releases bounded server state");
}
void prefix_guess_contracts() {
    const auto zero = RelayServer::Clock::time_point{};
    PrefixBudgets budgets(1);
    expect(budgets.admit("protected", zero, 1, 30) != nullptr, "Guess prefix admitted");
    for (unsigned i = 0; i < 4; ++i) budgets.failed_guess("protected", zero);
    const auto fifth = zero + 59s;
    expect(budgets.guess_allowed("protected", fifth, 100), "Four recent failures leave one attempt");
    budgets.failed_guess("protected", fifth);
    budgets.refresh(zero + 60s);
    expect(!budgets.guess_allowed("protected", zero + 60s, 100), "Accept-window rollover cannot clear guess cooldown");
    for (const auto* prefix : {"idle-a", "idle-b", "idle-c", "replacement"})
        expect(budgets.admit(prefix, zero + 60s, 1, 30) != nullptr, "Inactive map pressure still admits other prefixes");
    expect(!budgets.guess_allowed("protected", zero + 118999ms, 100), "Map pressure cannot evict protected failure history");
    expect(budgets.guess_allowed("protected", zero + 119s, 100), "Cooldown expires sixty seconds after fifth failure");
    PrefixBudgets global(1);
    expect(global.admit("one", zero, 1, 30) && global.admit("two", zero, 1, 30), "Distinct guess prefixes admitted");
    global.failed_guess("one", zero, false);
    expect(global.guess_allowed("two", zero, 1), "Full-code misses do not consume the global alias budget");
    global.failed_guess("one", zero);
    expect(!global.guess_allowed("two", zero + 59s, 1), "Global guess budget spans distinct prefixes");
    expect(global.guess_allowed("two", zero + 60s, 1), "Global guess budget recovers at the rolling deadline");
}
void invitations(const std::string& certificate, const std::string& key) {
    {
        RelayServerOptions options; options.require_access_token = true;
        options.certificate_file = certificate; options.key_file = key;
        expect(!RelayServer::listen(options), "Token ON requires a configured strong publisher secret");
    }
    {
        Fixture f({}, certificate, key, false);
        auto host = require(NetworkGnsIntroduction(f.client).publish("revoked", 1s));
        require(host.endpoint->close_publication());
        auto revoked = NetworkGnsIntroduction(f.client).join(host.join_code, "guest");
        expect(!revoked && revoked.error().code == "EAWR-NET-1523", "Confirmed publication close invalidates the full invitation before acknowledgement");
        host.endpoint.reset();
    }
    {
        RelayServerOptions options; options.room_aliases = true; options.max_join_lifetime = 1s;
        Fixture f(options, certificate, key, false);
        auto guest = f.client; guest.access_token.clear();
        auto host = require(NetworkGnsIntroduction(guest).publish("public-host", 15min));
        expect(alias_valid(host.room_alias) && code_valid(host.join_code), "Server generates separate four-character alias and full code");
        expect(host.expires > std::chrono::steady_clock::now() && host.expires <= std::chrono::steady_clock::now() + 1s, "Accepted capped TTL reaches client");
        auto client = require(NetworkGnsIntroduction(guest).join(host.room_alias, "public-guest"));
        expect(client->rendezvous().session == host.join_code, "Short alias resolves the original full session code");
        auto consumed = NetworkGnsIntroduction(guest).join(host.room_alias, "third");
        expect(!consumed && consumed.error().code == "EAWR-NET-1524", "Alias shares single-use admission");
    }
    {
        RelayServerOptions options; options.room_aliases = true;
        Fixture f(options, certificate, key, false);
        auto host = require(NetworkGnsIntroduction(f.client).publish("expires", 1s));
        const auto alias = host.room_alias; millis = 1000;
        until([&] { return !host.endpoint->poll(); }, "Alias publication expires");
        auto expired = NetworkGnsIntroduction(f.client).join(alias, "guest");
        expect(!expired && expired.error().code == "EAWR-NET-1523", "Expired and absent aliases share unavailable refusal");
    }
    {
        Fixture f({}, certificate, key, false);
        for (unsigned i = 0; i < 5; ++i) {
            auto result = NetworkGnsIntroduction(f.client).join("ZZZZ", "guesser");
            expect(!result && result.error().code == "EAWR-NET-1523", "Five wrong invitations receive ordinary unavailable refusal");
        }
        auto limited = NetworkGnsIntroduction(f.client).join(std::string(32, '0'), "guesser");
        expect(!limited && limited.error().code == "EAWR-NET-1532", "Guess budget spans short and full codes");
        millis = 59999;
        auto still_limited = NetworkGnsIntroduction(f.client).join("ZZZZ", "guesser");
        expect(!still_limited && still_limited.error().code == "EAWR-NET-1532", "Cooldown remains in force at its last millisecond");
        millis = 60000;
        auto restored = NetworkGnsIntroduction(f.client).join("ZZZZ", "guesser");
        expect(!restored && restored.error().code == "EAWR-NET-1523", "Cooldown survives connection teardown and expires after sixty seconds");
    }
    {
        RelayServerOptions options; options.failed_guesses_per_minute = 1;
        Fixture f(options, certificate, key, false);
        expect(!NetworkGnsIntroduction(f.client).join("ZZZZ", "guesser"), "Global guess budget records a failure");
        auto limited = NetworkGnsIntroduction(f.client).join("ZZZZ", "guesser");
        expect(!limited && limited.error().code == "EAWR-NET-1532", "Global alias guess cap applies before resolution");
        auto host = require(NetworkGnsIntroduction(f.client).publish("full-code-host", 1s));
        auto joined = require(NetworkGnsIntroduction(f.client).join(host.join_code, "full-code-guest"));
        expect(joined->rendezvous().session == host.join_code, "Valid full codes bypass an exhausted global alias budget");
    }
}
void authentication(const std::string& certificate, const std::string& key) {
    Fixture f({}, certificate, key);
    f.client.address = "localhost";
    auto unresolved = f.client; unresolved.address = "service.invalid"; unresolved.timeout = 500ms;
    const auto absent_signal = NetworkGnsIntroduction::health(unresolved);
    expect(!absent_signal && absent_signal.error().code == "EAWR-NET-1519"
        && absent_signal.error().message.find(unresolved.address) == std::string::npos, "unresolved signalling host fails with a curated typed error");
    NetworkOnlineLobby absent_lobby(unresolved); const auto absent_list = absent_lobby.list();
    expect(!absent_list && absent_list.error().code == "EAWR-NET-1519"
        && absent_list.error().message.find(unresolved.address) == std::string::npos, "unresolved directory host fails with a curated typed error");
    auto bad = f.client; bad.access_token = std::string(64, 'z');
    const auto denied = NetworkGnsIntroduction(bad).publish("host", 1s);
    expect(!denied && denied.error().code == "EAWR-NET-1520", "Unauthorized token rejected inside TLS");
    auto pin = f.client; pin.ca_file.clear(); pin.certificate_name.clear(); pin.certificate_sha256 = require(certificate_fingerprint(certificate));
    require(NetworkGnsIntroduction::health(pin));
    NetworkOnlineLobby pinned_lobby(pin); expect(require(pinned_lobby.list()).rooms.empty(), "pinned directory connects through localhost");
    auto precedence = pin; precedence.ca_file = "missing-ca-file.pem"; precedence.certificate_name = "wrong.invalid";
    require(NetworkGnsIntroduction::health(precedence));
    auto system = f.client; system.ca_file.clear();
#ifdef _WIN32
    const auto system_context = require(client_context({}, {}));
    expect(system_root_count(system_context) > 0, "Windows system context loads at least one ROOT anchor");
#endif
    for (const auto* literal : {"127.0.0.1", "::1", "[::1]", "fe80::1%1"}) {
        auto ip_name = system; ip_name.certificate_name = literal;
        const auto invalid_name = NetworkGnsIntroduction::health(ip_name);
        expect(!invalid_name && invalid_name.error().code == "EAWR-NET-1519"
            && invalid_name.error().message == "Invalid relay client options", "System trust rejects IP-literal certificate names before TLS");
        NetworkOnlineLobby invalid_lobby(ip_name);
        const auto invalid_list = invalid_lobby.list();
        expect(!invalid_list && invalid_list.error().code == "EAWR-NET-1519", "Lobby rejects IP-literal system names too");
    }
    // Accepted offline-fixture gap: a system-mode 1741 against a trusted chain
    // is not exercised here. We neither mutate OS roots nor require a public
    // server; the explicit CA-file contract below exercises name mismatch.
    const auto untrusted = NetworkGnsIntroduction::health(system);
    expect(!untrusted && untrusted.error().code == "EAWR-NET-1740"
        && untrusted.error().message == "Relay certificate is not trusted", "System trust rejects a self-signed loopback certificate with a curated typed error");
    NetworkOnlineLobby untrusted_lobby(system);
    const auto untrusted_list = untrusted_lobby.list();
    expect(!untrusted_list && untrusted_list.error().code == "EAWR-NET-1740", "Lobby preserves the typed system trust refusal");
    pin.certificate_sha256[0] = pin.certificate_sha256[0] == '0' ? '1' : '0';
    expect(!NetworkGnsIntroduction::health(pin), "Wrong pinned certificate fails closed");
    bad = f.client; bad.certificate_name = "wrong.invalid";
    const auto mismatch = NetworkGnsIntroduction::health(bad);
    expect(!mismatch && mismatch.error().code == "EAWR-NET-1741"
        && mismatch.error().message == "Relay certificate name does not match", "Wrong certificate name has a curated typed refusal");
    bad.ca_file = "missing-ca-file.pem";
    expect(!NetworkGnsIntroduction::health(bad), "Missing trust file rejected");
    const auto other_cert = (std::filesystem::path(certificate).parent_path() / "other-cert.pem").string();
    const auto other_key = (std::filesystem::path(certificate).parent_path() / "other-key.pem").string();
    require(create_certificate(other_cert, other_key));
    bad = f.client; bad.ca_file = other_cert;
    expect(!NetworkGnsIntroduction::health(bad), "A different trusted CA cannot authenticate this service");
    std::filesystem::remove(other_key); std::filesystem::remove(other_cert);
    bad = f.client; bad.ca_file.clear(); bad.certificate_name.clear();
    const auto missing_name = NetworkGnsIntroduction::health(bad);
    expect(!missing_name && missing_name.error().code == "EAWR-NET-1519", "System trust requires a certificate name");
    bad.ca_file = certificate;
    expect(!NetworkGnsIntroduction::health(bad), "CA-file trust still requires a certificate name");
    const auto unknown = NetworkGnsIntroduction(f.client).join(std::string(32, '0'), "client");
    expect(!unknown && unknown.error().code == "EAWR-NET-1523", "Unknown valid-shaped code rejected");
}
void spoofed_sender(const std::string& certificate, const std::string& key) {
    const auto listener = socket::listen("127.0.0.1", 0);
    expect(listener.handle != socket::invalid, "Scripted TLS server listens");
    auto context = require(server_context(certificate, key));
    std::jthread worker([&](std::stop_token stop) {
        std::unique_ptr<TlsStream> stream;
        bool sent{};
        while (!stop.stop_requested()) {
            if (!stream) {
                const auto handle = socket::accept(listener.handle);
                if (handle != socket::invalid) stream = std::make_unique<TlsStream>(context, handle, true, "", "", 262144, 64);
            }
            if (stream) {
                stream->pump();
                if (!sent && stream->receive()) {
                    auto paired = message(Kind::paired); string(paired, std::string(32, 'a'));
                    string(paired, "client"); string(paired, "host"); string(paired, ""); string(paired, ""); string(paired, ""); u32(paired, 0);
                    stream->queue(std::move(paired));
                    auto forwarded = message(Kind::forwarded); string(forwarded, "intruder"); forwarded.push_back(7);
                    stream->queue(std::move(forwarded)); sent = true;
                }
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    NetworkIntroductionOptions options;
    options.address = "127.0.0.1"; options.port = listener.port; options.ca_file = certificate; options.certificate_name = "localhost";
    auto result = NetworkGnsIntroduction(options).join(std::string(32, 'a'), "client");
    if (!result) expect(result.error().code == "EAWR-NET-1521", "Spoofed sender rejected during admission response drain");
    else {
        until([&] { return !result.value()->poll(); }, "Spoofed forwarded sender rejected by network client");
        expect(result.value()->poll().error().code == "EAWR-NET-1521", "Spoofed forwarded sender has malformed-response diagnostic");
    }
    worker.request_stop(); worker.join(); socket::close(listener.handle);
}
void queue_overflow(const std::string& certificate, const std::string& key) {
    RelayServerOptions options;
    options.queue_frames = 1; options.queue_bytes = max_body + 4;
    options.operations_per_minute = 100000; options.bytes_per_minute = 1024ULL * 1024 * 1024;
    Fixture fixture(options, certificate, key);
    Raw host(fixture.client), peer(fixture.client);
    auto published = host.exchange(publish());
    Reader reader{std::span(published).subspan(2)}; const auto code = reader.string(32);
    auto join = message(Kind::join); string(join, code); string(join, "slow-reader");
    expect(peer.exchange(std::move(join))[1] == static_cast<std::uint8_t>(Kind::paired), "Slow reader admitted");
    std::optional<Bytes> response;
    until([&] { host.tls->pump(); response = host.tls->receive(); return response.has_value(); }, "Publisher pairing arrives");
    expect((*response)[1] == static_cast<std::uint8_t>(Kind::paired), "Publisher paired before queue pressure");
    response.reset();
    auto signal = message(Kind::signal); signal.resize(65538, 1);
    unsigned sends{};
    until([&] {
        host.tls->pump(); response = host.tls->receive();
        if (response || host.tls->failed()) return true;
        if (host.tls->drained() && sends < 1024 && host.tls->queue(signal)) ++sends;
        return false;
    }, "A non-reading TLS peer cannot grow an unbounded server queue");
    if (!response) throw std::runtime_error("Output overflow lost the final response after " + std::to_string(sends)
        + " sends; TLS failed=" + std::to_string(host.tls->failed()));
    const auto diagnostic = number(*response);
    if (diagnostic != 1526) throw std::runtime_error("Output overflow returned diagnostic " + std::to_string(diagnostic) + " instead of 1526");
}
void prefix_contracts() {
    expect(address_prefix("127.0.0.1") == "127.0.0.1", "IPv4 budget key");
    expect(address_prefix("::ffff:127.0.0.1") == "127.0.0.1", "Mapped IPv4 shares its budget key");
    expect(address_prefix("2001:db8:1234:5678::1") == address_prefix("2001:db8:1234:5678:ffff:abcd:1234:5678"),
        "Rotating IPv6 addresses within a /64 cannot reset quotas");
    expect(address_prefix("2001:db8:1234:5678::1") != address_prefix("2001:db8:1234:5679::1"), "Distinct IPv6 /64 budgets");
    expect(address_prefix("fe80::1234%5") == "fe80::/64" && address_prefix("invalid").empty(), "Scoped IPv6 masks canonically without DNS");
}
void prefix_pressure() {
    const auto time = RelayServer::Clock::time_point{};
    PrefixBudgets prefixes(2); // Eight stored keys, sixteen new admissions/minute.
    const auto active = address_prefix("2001:db8:1::1");
    auto* live = prefixes.admit(active, time, 2, 2);
    expect(live != nullptr, "First source admitted");
    live->live = 1; live->unpaired = 1;
    live->operations = 7; live->bytes = 1024;
    for (unsigned i = 2; i <= 8; ++i) {
        const auto source = address_prefix("2001:db8:" + std::to_string(i) + "::1");
        expect(prefixes.admit(source, time, 2, 2) != nullptr, "Distinct sources fill bounded history");
    }
    expect(prefixes.size() == 8, "Prefix map reaches its exact bound");
    for (unsigned i = 9; i <= 16; ++i) {
        const auto source = address_prefix("2001:db8:" + std::to_string(i) + "::1");
        expect(prefixes.admit(source, time, 2, 2) != nullptr, "Zero-live histories evicted for new sources");
        expect(prefixes.size() == 8, "Eviction never exceeds map bound");
        const auto& kept = prefixes.at(active);
        expect(kept.live == 1 && kept.unpaired == 1 && kept.operations == 7 && kept.bytes == 1024,
            "Multi-source churn preserves active session and rate counters");
    }
    expect(!prefixes.admit(address_prefix("2001:db8:17::1"), time, 2, 2),
        "Global new-key quota cannot reset through eviction");
    expect(prefixes.admit(active, time, 2, 2) != nullptr, "Known active prefix remains usable at new-key quota");
    expect(!prefixes.admit(active, time, 2, 2), "Existing prefix retains accept quota under churn");
    prefixes.refresh(time + 60s);
    expect(prefixes.admit(address_prefix("2001:db8:17::1"), time + 60s, 2, 2) != nullptr,
        "New source admissions recover at minute boundary");
    expect(prefixes.at(active).live == 1 && prefixes.at(active).unpaired == 1,
        "Minute rollover preserves live counters");
    // Directly exercise the defensive full-map branch (production also has a
    // smaller global connection cap), then release exactly one history.
    PrefixBudgets full(1);
    for (unsigned i = 1; i <= 4; ++i) {
        auto* entry = full.admit(std::to_string(i), time, 1, 2);
        expect(entry != nullptr, "Fill all-live map"); entry->live = 1;
    }
    expect(!full.admit("new", time, 1, 2) && full.size() == 4, "All-live map refuses new keys");
    full.at("2").live = 0;
    expect(full.admit("new", time, 1, 2) != nullptr && full.size() == 4,
        "Released prefix is evicted before live entries");
    expect(!full.admit("1", time, 1, 2), "Live prefix connection cap remains enforced");
}
void pinned_expired_certificate(const std::filesystem::path& directory) {
    const auto cert = (directory / "expired-cert.pem").string();
    const auto key = (directory / "expired-key.pem").string();
    require(create_certificate(cert, key, -1s));
    {
        Fixture f({}, cert, key);
        auto pin = f.client; pin.ca_file.clear(); pin.certificate_name.clear();
        pin.certificate_sha256 = require(certificate_fingerprint(cert));
        {
            Raw peer(pin); // TCP connect only; TLS has not been pumped yet.
            until([&] { return f.live == 1; }, "Expired-certificate client consumes a server slot");
            until([&] { peer.tls->pump(); return peer.tls->failed() || peer.tls->ready(); },
                "Pinned certificate handshake completes or fails");
            expect(peer.tls->failed() && !peer.tls->ready(), "Correct certificate pin cannot bypass expiration");
        }
        until([&] { return f.live == 0; }, "Expired pinned handshake releases its connection");
    }
    std::filesystem::remove(key); std::filesystem::remove(cert);
}

void closing_deadline(const std::string& certificate, const std::string& key) {
    Fixture fixture({}, certificate, key);
    Raw peer(fixture.client);
    expect(number(peer.exchange(publish("wrong"))) == 1520, "Typed final refusal arrives before the write half-close");
    millis = 249;
    std::this_thread::sleep_for(5ms);
    expect(fixture.live == 1, "A ready refusal drains until its fixed deadline or peer EOF");
    // Raw retains the TCP write side after receiving the final response. This
    // extra complete frame must be discarded, and must not extend the deadline.
    auto extra = message(Kind::signal); extra.push_back(1);
    expect(peer.tls->queue(std::move(extra)), "Peer can still send while server writes are half-closed");
    peer.tls->pump();
    millis = 250;
    until([&] { return fixture.live == 0; }, "The refusal drain ends at 250 ms when the peer never sends EOF");
}
void key_permissions(const std::string& path) {
#ifdef _WIN32
    auto wide = std::filesystem::path(path).wstring();
    PSECURITY_DESCRIPTOR descriptor{}; PACL acl{};
    expect(GetNamedSecurityInfoW(wide.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS,
        "Read generated private-key ACL");
    const std::unique_ptr<void, decltype(&LocalFree)> guard(descriptor, LocalFree);
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision{};
    expect(GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) && acl && acl->AceCount == 1,
        "Windows key has one explicit owner ACE and no inherited grants");
#else
    const auto permissions = std::filesystem::status(path).permissions();
    expect((permissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none,
        "POSIX key denies group and other permissions");
#endif
}
void malformed(const std::string& certificate, const std::string& key) {
    Fixture f({}, certificate, key);
    expect(number(Raw(f.client).exchange(Bytes{1, 4})) == 1521, "Obsolete wire version rejected");
    expect(number(Raw(f.client).exchange(Bytes{3, 4})) == 1521, "Unsupported wire version");
    expect(number(Raw(f.client).exchange(Bytes{2, 255})) == 1521, "Unknown wire operation");
    expect(number(Raw(f.client).exchange(Bytes{2, 4, 0})) == 1521, "Trailing health fields rejected");
    expect(number(Raw(f.client).exchange(Bytes{2, 1, 255, 255})) == 1520, "Truncated authorization string rejected");
    auto extra = publish(); extra.push_back(0);
    expect(number(Raw(f.client).exchange(std::move(extra))) == 1521, "Trailing publish fields rejected");
    expect(number(Raw(f.client).exchange(publish(std::string(64, 'a'), std::string(32, 'x')))) == 1521, "Identity respects GNS capacity");
    Raw raw(f.client);
    expect(raw.exchange(publish())[1] == static_cast<std::uint8_t>(Kind::published), "Raw publish contract");
    expect(number(raw.exchange(publish())) == 1521, "Connection cannot switch sessions or identity");
    auto ctx = require(client_context(f.client.ca_file, ""));
    TlsStream queues(ctx, socket::connect("127.0.0.1", f.client.port), false, "localhost", "", max_body + 4, 1);
    expect(queues.queue(Bytes(max_body, 1)) && !queues.queue(message(Kind::health)), "TLS output byte/frame quota enforced before I/O");
    expect(!queues.queue(Bytes(max_body + 1, 1)), "TLS body size capped");
}
void expiry_limits(const std::string& certificate, const std::string& key) {
    {
        Fixture f({}, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("expiry-host", 100ms));
        millis = 100;
        until([&] { return !host.endpoint->poll(); }, "Host expiry observed at exact monotonic deadline");
        const auto expired = host.endpoint->poll();
        expect(!expired && expired.error().code == "EAWR-NET-1522", "Expiry has its reserved diagnostic");
        expect(!NetworkGnsIntroduction(f.client).join(host.join_code, "client"), "Expired join code cannot pair");
    }
    {
        RelayServerOptions o; o.max_sessions = 1;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        const auto full = NetworkGnsIntroduction(f.client).publish("second", 1s);
        expect(!full && full.error().code == "EAWR-NET-1527", "Session capacity bounded");
    }
    {
        RelayServerOptions o; o.unpaired_sessions_per_ip = 1;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        const auto full = NetworkGnsIntroduction(f.client).publish("second", 1s);
        expect(!full && full.error().code == "EAWR-NET-1527", "Aggregated-prefix unpaired session cap enforced");
        auto guest_options = f.client; guest_options.access_token.clear();
        auto guest = require(NetworkGnsIntroduction(guest_options).join(host.join_code, "guest"));
        auto next = require(NetworkGnsIntroduction(f.client).publish("next", 1s));
        expect(!next.join_code.empty(), "Pairing releases the prefix's unpaired slot");
    }
    {
        RelayServerOptions o; o.idle_timeout = 10s;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("idle-host", 300s));
        millis = 10000;
        until([&] { return !host.endpoint->poll(); }, "An admitted unpaired host has the same idle deadline");
    }
    {
        RelayServerOptions o; o.idle_timeout = 10s;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        auto guest = require(NetworkGnsIntroduction(f.client).join(host.join_code, "guest"));
        millis = 10000;
        until([&] { return !guest->poll(); }, "Paired idle deadline is enforced independently of maximum session age");
    }
    {
        RelayServerOptions o; o.accepts_per_minute = 2;
        Fixture f(o, certificate, key);
        require(NetworkGnsIntroduction::health(f.client));
        until([&] { return f.live == 0; }, "Health connection reclaimed");
        require(NetworkGnsIntroduction::health(f.client));
        until([&] { return f.live == 0; }, "Second health reclaimed");
        expect(!NetworkGnsIntroduction::health(f.client), "Per-IP accept quota retained after disconnect");
        millis = 60000;
        require(NetworkGnsIntroduction::health(f.client));
    }
    {
        RelayServerOptions o; o.operations_per_minute = 4;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        auto client = require(NetworkGnsIntroduction(f.client).join(host.join_code, "client"));
        until([&] { return !host.endpoint->rendezvous().remote_identity.empty(); }, "Pair before rate check");
        require(host.endpoint->send(Bytes{1})); require(host.endpoint->send(Bytes{2})); require(client->send(Bytes{3}));
        until([&] { return !host.endpoint->poll() || !client->poll(); }, "Per-IP protocol operation quota closes session");
    }
    {
        RelayServerOptions o; o.session_lifetime = 1s;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        auto client = require(NetworkGnsIntroduction(f.client).join(host.join_code, "client"));
        millis = 1000;
        until([&] { return !client->poll(); }, "Maximum paired session age bounded");
    }
    {
        RelayServerOptions o; o.max_connections = 2; o.connections_per_ip = 2;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        auto client = require(NetworkGnsIntroduction(f.client).join(host.join_code, "client"));
        expect(!NetworkGnsIntroduction::health(f.client), "Live per-IP/global connection capacity cannot be bypassed");
        expect(f.live == 2, "Rejected TLS attempt consumes no extra server slot");
    }
    {
        RelayServerOptions o; o.bytes_per_minute = 8192;
        Fixture f(o, certificate, key);
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        auto client = require(NetworkGnsIntroduction(f.client).join(host.join_code, "client"));
        until([&] { return !host.endpoint->rendezvous().remote_identity.empty(); }, "Pair before byte quota check");
        require(host.endpoint->send(Bytes(65536, 1)));
        until([&] { return !host.endpoint->poll(); }, "TLS incoming byte quota bounds partial large signals");
        expect(host.endpoint->poll().error().code == "EAWR-NET-1525", "Byte quota preserves typed service refusal");
    }
    {
        RelayServerOptions o; o.admission_timeout = 10ms;
        Fixture f(o, certificate, key);
        const auto handle = socket::connect("127.0.0.1", f.client.port);
        expect(handle != socket::invalid, "Slow-handshake TCP connection created");
        until([&] { return f.live == 1; }, "Unfinished TLS handshake consumes one bounded slot");
        millis = 10;
        until([&] { return f.live == 0; }, "Incomplete TLS handshake expires");
        socket::close(handle);
    }
}

// Ticket E: lease renewal. Raw peers drive the exact wire contract with the server's injected clock; the endpoint
// scenario then checks the real client renews on its own.
Bytes next_frame(Raw& raw) {
    std::optional<Bytes> reply;
    until([&] { raw.tls->pump(); reply = raw.tls->receive(); return reply.has_value() || raw.tls->failed(); }, "Raw frame timeout");
    expect(reply.has_value(), "Raw frame received"); return std::move(*reply);
}
Kind kind_of(const Bytes& bytes) { expect(bytes.size() >= 2 && bytes[0] == 2, "Relay frame envelope"); return static_cast<Kind>(bytes[1]); }
Reader body(const Bytes& bytes) { return Reader{std::span(bytes).subspan(2)}; }
struct RawPair {
    Raw host, guest;
    explicit RawPair(Fixture& f) : host(f.client), guest(f.client) {
        auto published = host.exchange(publish());
        expect(kind_of(published) == Kind::published, "Raw host published");
        auto reader = body(published); const auto code = reader.string(32);
        auto join = message(Kind::join); string(join, code); string(join, "raw-guest");
        expect(kind_of(guest.exchange(std::move(join))) == Kind::paired, "Raw guest paired");
        expect(kind_of(next_frame(host)) == Kind::paired, "Raw host paired");
    }
};
RelayServerOptions renewal_options() {
    RelayServerOptions o;
    o.session_lifetime = 10s; o.max_session_lifetime = 25s; o.renewal_window = 4s;
    o.turn_uri = "turn:127.0.0.1:3478"; o.turn_secret = std::string(64, 'b');
    return o;
}
Renewed renewed_of(const Bytes& bytes) {
    expect(kind_of(bytes) == Kind::renewed, "Renewed frame"); auto parsed = parse_renewed(body(bytes));
    expect(parsed.has_value(), "Renewed frame layout"); return *parsed;
}
Refused refused_of(const Bytes& bytes) {
    expect(kind_of(bytes) == Kind::renew_refused, "Renewal refusal frame"); auto parsed = parse_refused(body(bytes));
    expect(parsed.has_value(), "Renewal refusal layout"); return *parsed;
}
Expiring expiring_of(const Bytes& bytes) {
    expect(kind_of(bytes) == Kind::expiring, "Expiring frame"); auto parsed = parse_expiring(body(bytes));
    expect(parsed.has_value(), "Expiring frame layout"); return *parsed;
}
// Scripted credential provider: inline success, failure, refusal, or a held job released from the test thread.
class ScriptedRefresh final : public TurnRefresh {
public:
    enum class Mode { ok, fail, refuse, hold };
    bool submit(TurnRefreshJob job) override {
        const std::lock_guard lock(mutex_);
        ++submitted_;
        if (mode_ == Mode::refuse) return false;
        pending_.push_back(std::move(job)); return true;
    }
    std::optional<TurnRefreshResult> take() override {
        const std::lock_guard lock(mutex_);
        if (pending_.empty() || mode_ == Mode::hold) return {};
        TurnRefreshResult result; result.session = std::move(pending_.front().session); pending_.pop_front();
        if (mode_ == Mode::fail) result.failure = relay::error(1529, "Provider unavailable");
        else result.credentials = GnsTurnCredentials{"turn:127.0.0.1:3478", "refreshed-user-" + result.session, "refreshed-password", 900s};
        return result;
    }
    void mode(Mode value) { const std::lock_guard lock(mutex_); mode_ = value; }
    std::size_t submitted() const { const std::lock_guard lock(mutex_); return submitted_; }
private:
    mutable std::mutex mutex_;
    Mode mode_{Mode::ok};
    std::deque<TurnRefreshJob> pending_;
    std::size_t submitted_{};
};
void renewal_contracts(const std::string& certificate, const std::string& key) {
    {
        // Before the window a renewal is refused, inside it the lease extends once for both peers, and the
        // extension stops at the maximum: the lease then ends with its own typed reason (EAWR-NET-1730).
        auto o = renewal_options();
        Fixture f(o, certificate, key);
        RawPair pair(f);
        auto early = refused_of(pair.host.exchange(message(Kind::renew)));
        expect(early.reason == relay::renew_refused_early && early.remaining == 10s, "Renewal before the window is refused, lease unchanged");
        millis = 6000;
        for (auto* peer : {&pair.host, &pair.guest}) {
            const auto warning = expiring_of(next_frame(*peer));
            expect(warning.remaining == 4s && warning.hard_remaining == 19s && warning.renewable, "Both peers are warned at the window");
        }
        auto first = renewed_of(pair.host.exchange(message(Kind::renew)));
        auto second = renewed_of(next_frame(pair.guest));
        expect(first.remaining == 14s && first.hard_remaining == 19s && second.remaining == 14s, "Renewal extends the lease for both peers");
        expect(first.turn.uri == o.turn_uri && !first.turn.username.empty() && first.turn.password != o.turn_secret
            && first.turn.lifetime == 14s + 60s && first.turn.username != second.turn.username, "Refreshed TURN credentials cover the new lease and never carry the secret");
        expect(refused_of(pair.guest.exchange(message(Kind::renew))).reason == relay::renew_refused_early, "The second peer's renewal is a harmless no-op");
        millis = 16000;
        for (auto* peer : {&pair.host, &pair.guest}) expect(expiring_of(next_frame(*peer)).hard_remaining == 9s, "Warned again before the second end");
        auto capped = renewed_of(pair.guest.exchange(message(Kind::renew)));
        expect(capped.remaining == 9s && capped.hard_remaining == 9s, "Renewal is capped at the configured maximum");
        next_frame(pair.host);
        expect(refused_of(pair.host.exchange(message(Kind::renew))).reason == relay::renew_refused_maximum,
            "A final lease reports maximum even before its renewal window");
        millis = 21000;
        for (auto* peer : {&pair.host, &pair.guest}) expect(!expiring_of(next_frame(*peer)).renewable, "The final warning says renewal is exhausted");
        auto last = refused_of(pair.host.exchange(message(Kind::renew)));
        expect(last.reason == relay::renew_refused_maximum && last.remaining == 4s, "No renewal past the maximum");
        millis = 25000;
        expect(number(next_frame(pair.host)) == relay::lease_ended_at_maximum && number(next_frame(pair.guest)) == relay::lease_ended_at_maximum,
            "The lease ends at the maximum with its typed reason");
    }
    {
        // Without a renewal the lease ends at its own time, with the not-renewed reason; a late renewal races nothing.
        Fixture f(renewal_options(), certificate, key);
        RawPair pair(f);
        f.paused = true; millis = 10000;
        expect(pair.host.tls->queue(message(Kind::renew)), "Late renewal queued");
        f.paused = false;
        expect(number(next_frame(pair.host)) == relay::lease_not_renewed, "A renewal after expiry is refused with the not-renewed reason");
        expect(number(next_frame(pair.guest)) == relay::lease_not_renewed, "The peer is told the same reason");
    }
    {
        // Only a paired session can renew, and a connection without a session cannot.
        Fixture f(renewal_options(), certificate, key);
        Raw alone(f.client);
        expect(kind_of(alone.exchange(publish())) == Kind::published, "Unpaired publication");
        expect(number(alone.exchange(message(Kind::renew))) == 1524, "An unpaired session cannot renew");
        Raw lobby(f.client);
        expect(number(lobby.exchange(message(Kind::renew))) == 1521, "A connection without a session cannot renew");
    }
    {
        // Renewal requests spend the same per-session and per-address budget as every other operation.
        auto o = renewal_options(); o.operations_per_minute = 4;
        Fixture f(o, certificate, key);
        RawPair pair(f);
        bool refused_by_budget{};
        for (unsigned i = 0; i < 6 && !refused_by_budget; ++i) {
            auto reply = pair.host.exchange(message(Kind::renew));
            if (kind_of(reply) == Kind::error) { expect(number(reply) == 1525, "Renewal flood is refused by the operation budget"); refused_by_budget = true; }
        }
        expect(refused_by_budget, "The operation budget bounds renewal requests");
    }
    {
        // Provider seam: failures and refusals keep the lease, a held job answers later, success delivers credentials.
        auto provider = std::make_shared<ScriptedRefresh>();
        auto o = renewal_options();
        Fixture f(o, certificate, key, true, nullptr, provider);
        RawPair pair(f);
        millis = 6000;
        for (auto* peer : {&pair.host, &pair.guest}) next_frame(*peer);
        provider->mode(ScriptedRefresh::Mode::refuse);
        auto refused = refused_of(pair.host.exchange(message(Kind::renew)));
        expect(refused.reason == relay::renew_refused_credentials && refused.remaining == 4s, "A provider that refuses the job leaves the lease unchanged");
        provider->mode(ScriptedRefresh::Mode::fail);
        expect(refused_of(pair.host.exchange(message(Kind::renew))).reason == relay::renew_refused_credentials, "A failed credential refresh refuses the renewal");
        provider->mode(ScriptedRefresh::Mode::hold);
        expect(pair.host.tls->queue(message(Kind::renew)), "Renewal queued while the provider holds the job");
        until([&] { pair.host.tls->pump(); return provider->submitted() == 5; }, "Provider received the held job");
        expect(refused_of(pair.guest.exchange(message(Kind::renew))).reason == relay::renew_refused_early, "A renewal in progress is not repeated");
        provider->mode(ScriptedRefresh::Mode::ok);
        const auto host_reply = renewed_of(next_frame(pair.host)), guest_reply = renewed_of(next_frame(pair.guest));
        expect(host_reply.remaining == 14s && host_reply.turn.username.starts_with("refreshed-user-") && guest_reply.turn.username.starts_with("refreshed-user-")
            && host_reply.turn.username != guest_reply.turn.username,
            "Each peer receives its own provider credential once both jobs complete");
    }
    {
        // A provider that never completes must release renewing before expiry, and late completions cannot
        // satisfy the retry. The two attempts use different opaque keys.
        auto provider = std::make_shared<ScriptedRefresh>(); provider->mode(ScriptedRefresh::Mode::hold);
        auto o = renewal_options(); o.cloudflare_timeout = 500ms;
        Fixture f(o, certificate, key, true, nullptr, provider); RawPair pair(f);
        millis = 6000;
        for (auto* peer : {&pair.host, &pair.guest}) next_frame(*peer);
        expect(pair.host.tls->queue(message(Kind::renew)), "Held renewal queued");
        until([&] { pair.host.tls->pump(); return provider->submitted() == 2; }, "Both held jobs submitted");
        millis = 9000;
        const auto timed_out = refused_of(next_frame(pair.host));
        expect(timed_out.reason == relay::renew_refused_credentials && timed_out.remaining == 1s, "Held refresh has a deadline and leaves its lease unchanged");
        expect(pair.host.tls->queue(message(Kind::renew)), "Retry queued before old completions arrive");
        until([&] { pair.host.tls->pump(); return provider->submitted() == 4; }, "Retry has fresh job keys");
        provider->mode(ScriptedRefresh::Mode::ok);
        const auto host = renewed_of(next_frame(pair.host)), guest = renewed_of(next_frame(pair.guest));
        expect(host.remaining == 11s && host.turn.username.ends_with(":3") && guest.turn.username.ends_with(":4"),
            "Only the retry's per-peer completions extend the lease");
    }
    {
        auto o = renewal_options(); o.turn_uri.clear(); o.turn_secret.clear();
        Fixture f(o, certificate, key); RawPair pair(f); millis = 6000;
        for (auto* peer : {&pair.host, &pair.guest}) next_frame(*peer);
        const auto host = renewed_of(pair.host.exchange(message(Kind::renew))), guest = renewed_of(next_frame(pair.guest));
        expect(host.remaining == 14s && host.turn.uri.empty() && host.turn.username.empty() && guest.turn.uri.empty(),
            "A direct-only session renews without TURN credentials");
    }
    for (const auto kind : {Kind::renew, Kind::renewed, Kind::renew_refused, Kind::expiring}) {
        Fixture f(renewal_options(), certificate, key); RawPair pair(f);
        auto request = message(kind);
        if (kind == Kind::renew) request.push_back(0);
        expect(number(pair.host.exchange(std::move(request))) == 1521,
            "Trailing renewal bytes and service-only renewal Kinds from peers are rejected");
    }
    {
        auto provider = std::make_shared<ScriptedRefresh>(); provider->mode(ScriptedRefresh::Mode::refuse);
        Fixture f(renewal_options(), certificate, key, true, nullptr, provider); f.client.renewal_retry = 50ms;
        auto host = require(NetworkGnsIntroduction(f.client).publish("retry-host", 1s));
        auto guest = require(NetworkGnsIntroduction(f.client).join(host.join_code, "retry-guest"));
        until([&] { return !host.endpoint->rendezvous().remote_identity.empty(); }, "Retry pair admitted");
        millis = 6000;
        until([&] { const auto lease = host.endpoint->lease(); return lease && lease->last_refusal == "EAWR-NET-1733"; }, "Real client records credential refusal");
        provider->mode(ScriptedRefresh::Mode::ok);
        until([&] { const auto h = host.endpoint->lease(), g = guest->lease(); return h && g && h->renewals == 1 && g->renewals == 1; }, "Real client retries and both peers renew");
        expect(host.endpoint->lease()->last_refusal.empty() && guest->take_refreshed_turn_credentials().has_value(), "Successful retry clears refusal and delivers credentials");
    }
    {
        auto provider = std::make_shared<ScriptedRefresh>(); provider->mode(ScriptedRefresh::Mode::refuse);
        Fixture f(renewal_options(), certificate, key, true, nullptr, provider); f.client.renewal_retry = 25ms;
        auto host = require(NetworkGnsIntroduction(f.client).publish("bounded-host", 1s));
        auto guest = require(NetworkGnsIntroduction(f.client).join(host.join_code, "bounded-guest"));
        until([&] { return !host.endpoint->rendezvous().remote_identity.empty(); }, "Bounded retry pair admitted");
        millis = 6000;
        until([&] { return provider->submitted() == 12; }, "Each peer makes at most six attempts");
        std::this_thread::sleep_for(150ms);
        expect(provider->submitted() == 12 && host.endpoint->lease()->last_refusal == "EAWR-NET-1733"
            && guest->lease()->last_refusal == "EAWR-NET-1733", "Persistent failure stops retrying with its refusal retained");
    }
    {
        Fixture f(renewal_options(), certificate, key);
        for (const auto retry : {0ms, -1ms, 60001ms}) {
            auto options = f.client; options.renewal_retry = retry;
            const auto invalid = NetworkGnsIntroduction::health(options);
            expect(!invalid && invalid.error().code == "EAWR-NET-1519", "Invalid retry intervals are rejected before network I/O");
        }
        for (const bool short_maximum : {false, true}) {
            auto options = renewal_options(); options.certificate_file = certificate; options.key_file = key;
            if (short_maximum) options.max_session_lifetime = options.session_lifetime - 1ms;
            else options.renewal_window = options.max_session_lifetime + 1ms;
            const auto invalid = RelayServer::listen(options, now);
            expect(!invalid && invalid.error().code == "EAWR-NET-1519", "Inconsistent session maximum/window is rejected before listening");
        }
        expect(service_error_number(1730) && service_error_number(1731) && !service_error_number(1732)
            && !service_error_number(1735) && !service_error_number(1738), "Only service lease-ending errors are accepted on the wire");
    }
    {
        // The real client renews on its own and hands over the refreshed credentials once.
        auto o = renewal_options();
        Fixture f(o, certificate, key);
        f.client.renewal_retry = 50ms;
        auto host = require(NetworkGnsIntroduction(f.client).publish("host", 1s));
        auto client = require(NetworkGnsIntroduction(f.client).join(host.join_code, "client"));
        until([&] { return !host.endpoint->rendezvous().remote_identity.empty(); }, "Pair before renewal");
        expect(!host.endpoint->lease() && !client->lease(), "No lease is reported before the service sends one");
        expect(host.endpoint->turn_credentials()->lifetime == 85s && client->turn_credentials()->lifetime == 85s,
            "Pairing credentials cover the session maximum plus margin, not just the first lease");
        millis = 6000;
        for (auto* endpoint : {host.endpoint.get(), client.get()}) {
            until([&] { const auto lease = endpoint->lease(); return lease && lease->renewals == 1; }, "Endpoint renewed automatically");
            const auto lease = *endpoint->lease();
            const auto remaining = lease.ends - std::chrono::steady_clock::now();
            expect(lease.renewable && remaining > 13s && remaining <= 14s && lease.hard_ends - lease.ends == 5s, "Endpoint lease follows the service");
            const auto refreshed = endpoint->take_refreshed_turn_credentials();
            expect(refreshed && refreshed->uri == o.turn_uri && refreshed->lifetime == 74s, "Refreshed credentials are delivered");
            expect(!endpoint->take_refreshed_turn_credentials(), "Refreshed credentials are delivered once");
        }
        millis = 16000;
        for (auto* endpoint : {host.endpoint.get(), client.get()})
            until([&] { const auto lease = endpoint->lease(); return lease && lease->renewals == 2 && !lease->renewable; }, "Second renewal reaches the maximum");
        millis = 25000;
        until([&] { return !host.endpoint->poll() && !client->poll(); }, "Lease end closes the signalling session");
        expect(host.endpoint->poll().error().code == "EAWR-NET-1730", "Lease end is a typed client failure");
    }
}
}
// Room kinds (34..49) belong to room member connections only, and a room member connection answers nothing else.
void room_routing_contracts(const std::string& certificate, const std::string& key) {
    RelayServerOptions o;
    Fixture f(o, certificate, key, false);
    Raw session(f.client);
    expect(session.exchange(publish())[1] == static_cast<std::uint8_t>(Kind::published), "Invitation published");
    expect(number(session.exchange(message(Kind::room_leave))) == 1540, "A room kind on an invitation connection is refused");
    Raw lobby(f.client);
    auto opened = lobby.exchange([] { auto b = message(Kind::directory_open); string(b, ""); return b; }());
    Reader principal_reader{std::span(opened).subspan(2)}; const auto principal = principal_reader.string(32);
    const auto room = publication(lobby.exchange(room_publish()));
    expect(number(lobby.exchange(message(Kind::room_lock))) == 1540, "A room kind on a directory connection is refused");
    Raw member(f.client);
    auto open = message(Kind::room_open); string(open, room.room); string(open, room.ownership); string(open, principal); string(open, "host-id"); u32(open, 0);
    expect(member.exchange(open)[1] == static_cast<std::uint8_t>(Kind::room_opened), "The host opens the scope on its own connection");
    expect(member.receive()[1] == static_cast<std::uint8_t>(Kind::room_roster), "The first roster follows the reply");
    expect(number(member.exchange(publish())) == 1521, "An invitation kind on a room connection is refused");
    Raw second(f.client);
    expect(second.exchange(open)[1] == static_cast<std::uint8_t>(Kind::error), "A room opens once");
    Raw directory_on_room(f.client);
    auto again = message(Kind::room_open); string(again, room.room); string(again, room.ownership); string(again, principal); string(again, "other-id"); u32(again, 0);
    expect(number(directory_on_room.exchange(again)) == room_unavailable, "...with the room-unavailable reason");
}

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "Artifact directory argument");
        const auto directory = std::filesystem::path(argv[1]) / require(random_code());
        std::filesystem::create_directories(directory);
        const auto cert = (directory / "certificate.pem").string(), key = (directory / "key.pem").string();
        require(create_certificate(cert, key));
        key_permissions(key);
        expect(!create_certificate(cert, key), "Certificate generator refuses overwrite");
        directory_contracts(); directory_bridge_contracts(); password_seat_chat_contracts(); directory_quota_contracts(); directory_codec_contracts();
        hash_worker_contracts(cert, key); reconnect_and_race_contracts(cert, key); directory_loopback(cert, key); bridge_loopback(cert, key);
        loopback(cert, key); invitations(cert, key); prefix_guess_contracts(); prefix_contracts(); prefix_pressure(); pinned_expired_certificate(directory); authentication(cert, key); malformed(cert, key);
        closing_deadline(cert, key); spoofed_sender(cert, key); queue_overflow(cert, key); expiry_limits(cert, key); renewal_contracts(cert, key);
        room_routing_contracts(cert, key);
        std::filesystem::remove(std::filesystem::path(key)); std::filesystem::remove(std::filesystem::path(cert));
        std::filesystem::remove(directory);
        std::cout << "TLS relay protocol and loopback contracts passed\n"; return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
