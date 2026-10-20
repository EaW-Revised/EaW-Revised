#include "../support/check.hpp"
#include "../support/result_require.hpp"
#include "eawr/net/gns_transport.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
void expect(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
using eawr::testing::result_code_colon::require;
template<class F> void until(F predicate, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        expect(Clock::now() < deadline, "GNS contract deadline");
        std::this_thread::sleep_for(1ms);
    }
}
GnsOptions options() {
    GnsOptions value; value.build_identity = "gns-contract-build";
    value.keepalive = 20ms; value.timeout = 1000ms;
    return value;
}
struct Pair {
    std::unique_ptr<GnsTransport> host, client;
    Pair(GnsOptions left = options(), GnsOptions right = options()) {
        host = require(GnsTransport::host("127.0.0.1", 0, std::move(left)));
        expect(host->port() != 0, "GNS ephemeral port");
        client = require(GnsTransport::connect("127.0.0.1", host->port(), std::move(right)));
    }
    void open() {
        until([&] { static_cast<void>(host->poll()); static_cast<void>(client->poll());
            return host->link(1) == LinkState::open && client->link(0) == LinkState::open; });
        expect(host->self() == 0 && client->self() == 1 && host->peer_count() == 2, "GNS peer roles");
    }
};
void frames() {
    Pair pair; pair.open();
    std::vector<Bytes> expected{{}, {0}, {1}, {2}, {3}, {4}, {5}, {6}, {255}, Bytes(1024 * 1024, 173)};
    for (unsigned i = 0; i < 1000; ++i) expected.push_back({static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8)});
    for (const auto& bytes : expected) require(pair.host->send(1, bytes));
    const auto queued = pair.host->queue_progress(1);
    expect(queued.native_valid && queued.outbound_frames == expected.size() && !queued.reliable_drained,
        "Queue snapshot observes accepted sends without claiming delivery or pumping them");
    expect(!pair.host->queue_progress(0).native_valid, "Self has no native queue snapshot");
    std::size_t received{};
    until([&] {
        expect(!pair.host->poll(), "GNS control traffic is private");
        while (auto frame = pair.client->poll()) {
            expect(frame->from == 0 && received < expected.size() && frame->frame == expected[received], "Whole ordered GNS frame");
            ++received;
        }
        return received == expected.size();
    }, 10s);
    require(pair.client->send(0, expected[9]));
    std::optional<Received> echoed;
    until([&] { static_cast<void>(pair.client->poll()); echoed = pair.host->poll(); return echoed.has_value(); });
    expect(echoed->from == 1 && echoed->frame == expected[9], "Fragmented 1 MiB GNS echo");
    expect(!pair.host->send(1, Bytes(1024 * 1024 + 1)), "GNS oversize send rejected");
    expect(!pair.host->send(0, {}), "GNS self send rejected");
    expect(pair.host->link(1) == LinkState::open, "Bad send preserves GNS link");
    until([&] { static_cast<void>(pair.host->poll()); static_cast<void>(pair.client->poll());
        return pair.host->connection_rtt(1).has_value(); });
    expect(pair.host->connection_rtt(1)->count() >= 0 && !pair.host->connection_rtt(0), "Measured GNS RTT per remote link");
}
void mismatch(unsigned kind) {
    auto remote = options();
    if (kind == 0) ++remote.protocol_version;
    if (kind == 1) remote.build_identity += "-different";
    if (kind == 2) --remote.max_frame_size;
    Pair pair(options(), remote);
    until([&] { static_cast<void>(pair.host->poll()); static_cast<void>(pair.client->poll());
        return pair.host->diagnostic().has_value() && pair.client->link(0) == LinkState::closed; });
    const auto code = kind == 0 ? "EAWR-NET-1502" : kind == 1 ? "EAWR-NET-1503" : "EAWR-NET-1504";
    expect(pair.host->diagnostic()->code == code, "GNS typed host mismatch");
    expect(pair.client->diagnostic() && pair.client->diagnostic()->code == code, "GNS typed client mismatch");
    const auto diagnostic = *pair.client->diagnostic();
    expect(!pair.client->send(0, {}), "Refused joiner cannot send application frames");
    for (unsigned i = 0; i < 3; ++i) expect(!pair.client->poll(), "Refused joiner emitted data");
    expect(pair.client->closed() && pair.client->diagnostic()->code == diagnostic.code
        && pair.client->diagnostic()->message == diagnostic.message, "Joiner refusal is terminal and stable");
    expect(pair.host->link(1) == LinkState::connecting && !pair.host->closed(), "GNS host listens after failed identity");
    expect(!pair.host->link_stats(1), "Failed GNS attempt cannot publish stats into a retry");
    const auto linger = Clock::now() + 300ms;
    until([&] { static_cast<void>(pair.host->poll()); return Clock::now() >= linger; });
    pair.client.reset();
    pair.client = require(GnsTransport::connect("127.0.0.1", pair.host->port(), options()));
    pair.open();
    expect(!pair.host->diagnostic(), "Valid retry clears old GNS attempt diagnostic");
}
void link_stats() {
    auto config = options(); config.timeout = 5s;
    Pair pair(config, config);
    MessageTransport& host = *pair.host;
    expect(!host.link_stats(1) && !pair.host->connection_rtt(1), "Pre-open GNS stats are unknown");
    pair.open();
    until([&] {
        expect(!pair.host->poll() && !pair.client->poll(), "Stats cannot produce application frames");
        return host.link_stats(1).has_value() && pair.client->link_stats(0).has_value();
    });
    const auto snapshot = *host.link_stats(1);
    expect(snapshot.sample_count > 0 && snapshot.last_rtt.count() >= 0
        && snapshot.last_rtt.count() % 1000 == 0, "Native RTT retains millisecond quantization");
    expect(!host.link_stats(0) && !host.link_stats(2), "Stats belong only to the open remote peer");
    std::this_thread::sleep_for(1100ms);
    for (unsigned i = 0; i < 100; ++i) {
        const auto cached = host.link_stats(1);
        expect(cached && cached->sample_count == snapshot.sample_count && cached->last_rtt == snapshot.last_rtt
            && cached->smoothed_rtt == snapshot.smoothed_rtt && cached->jitter == snapshot.jitter
            && pair.host->connection_rtt(1) == snapshot.last_rtt, "Both GNS getters only read cached stats");
    }
    until([&] {
        expect(!pair.host->poll() && !pair.client->poll(), "Cadence sampling stays private");
        return host.link_stats(1) && host.link_stats(1)->sample_count > snapshot.sample_count;
    });
    pair.host->close();
    expect(!host.link_stats(1) && !pair.host->connection_rtt(1), "Closing GNS stats are absent immediately");
    pair.client->cancel();
    expect(!pair.client->link_stats(0), "Cancelled GNS stats are absent");
}
void lifecycle() {
    Pair pair; pair.open();
    const auto idle = Clock::now() + 2200ms;
    until([&] {
        expect(!pair.host->poll() && !pair.client->poll(), "GNS private idle keepalive");
        expect(pair.host->link(1) == LinkState::open && pair.client->link(0) == LinkState::open, "GNS idle link stays open");
        return Clock::now() >= idle;
    });
    require(pair.client->send(0, {})); require(pair.client->send(0, Bytes(1024 * 1024, 7)));
    pair.client->close();
    std::vector<Bytes> delivered;
    until([&] {
        static_cast<void>(pair.client->poll());
        if (auto frame = pair.host->poll()) delivered.push_back(std::move(frame->frame));
        return pair.host->closed() && pair.client->closed();
    });
    while (auto frame = pair.host->poll()) delivered.push_back(std::move(frame->frame));
    expect(delivered == std::vector<Bytes>{{}, Bytes(1024 * 1024, 7)}, "GNS clean close preserves ordered tail");
    expect(!pair.host->diagnostic() && !pair.client->diagnostic(), "GNS ordered close has no error");
    Pair killed; killed.open(); killed.client.reset();
    until([&] { static_cast<void>(killed.host->poll()); return killed.host->link(1) == LinkState::closed; });
    Pair stalled; stalled.open();
    until([&] { static_cast<void>(stalled.host->poll()); return stalled.host->link(1) == LinkState::closed; }, 2s);
    expect(stalled.host->diagnostic() && stalled.host->diagnostic()->code == "EAWR-NET-1507", "GNS background UDP cannot hide unpumped app timeout");
    Pair no_ack; no_ack.open(); no_ack.host->close();
    until([&] { static_cast<void>(no_ack.host->poll()); return no_ack.host->closed(); }, 2s);
    expect(no_ack.host->diagnostic() && no_ack.host->diagnostic()->code == "EAWR-NET-1507", "GNS close without application acknowledgement is bounded");
    auto listener = require(GnsTransport::host("127.0.0.1", 0, options()));
    listener->cancel(); expect(listener->closed(), "GNS cancel releases pending listener");
}
void capacity() {
    auto config = options(); config.max_frame_size = 64; config.queue_bytes = 128; config.queue_frames = 2;
    Pair pair(config, config); pair.open();
    require(pair.host->send(1, Bytes(64, 3)));
    const auto refused = pair.host->send(1, Bytes(64, 4));
    expect(!refused && refused.error().code == "EAWR-NET-1505", "GNS send backpressure");
    std::optional<Received> frame;
    until([&] { static_cast<void>(pair.host->poll()); frame = pair.client->poll(); return frame.has_value(); });
    require(pair.host->send(1, Bytes(64, 4)));
    auto sender = config; sender.queue_frames = 16;
    Pair burst(sender, config); burst.open();
    for (unsigned i = 0; i < 8; ++i) require(burst.host->send(1, {}));
    static_cast<void>(burst.host->poll()); std::this_thread::sleep_for(30ms);
    until([&] { static_cast<void>(burst.client->poll()); return burst.client->link(0) == LinkState::closed; });
    expect(burst.client->diagnostic() && burst.client->diagnostic()->code == "EAWR-NET-1505", "Empty GNS messages cannot overflow frame quota");
    until([&] { static_cast<void>(burst.host->poll()); return burst.host->link(1) == LinkState::closed; });
    if (!burst.host->diagnostic() || burst.host->diagnostic()->code != "EAWR-NET-1505")
        throw std::runtime_error("GNS capacity reason reaches sender; observed " +
            (burst.host->diagnostic() ? burst.host->diagnostic()->code + ": " + burst.host->diagnostic()->message : "no diagnostic"));
}
void invalid_options() {
    for (const auto peers : {0U, 1U, 3U, 8U}) {
        auto config = options(); config.peers = peers;
        const auto host = GnsTransport::host("127.0.0.1", 0, config);
        const auto client = GnsTransport::connect("127.0.0.1", 12345, config);
        expect(!host && host.error().code == "EAWR-NET-1501"
            && !client && client.error().code == "EAWR-NET-1501", "GNS rejects unsupported peer counts");
    }
    auto value = options(); value.timeout = value.keepalive;
    expect(!GnsTransport::host("127.0.0.1", 0, value), "Invalid GNS timeout");
    value = options(); value.build_identity.clear();
    expect(!GnsTransport::host("127.0.0.1", 0, value), "GNS requires build identity");
    expect(!GnsTransport::host("invalid-address", 0, options()), "GNS requires numeric direct IP");
    expect(!GnsTransport::connect("127.0.0.1", 0, options()), "GNS connect requires remote port");
    expect(!GnsTransport::host_introduced({}, options()), "GNS requires signalling endpoint");
    value = options(); value.stun_servers = "turn:127.0.0.1:3478";
    expect(!GnsTransport::host("127.0.0.1", 0, value), "STUN list must hold stun: URIs");
    value.stun_servers = "stun:127.0.0.1:3478,stun:";
    expect(!GnsTransport::host("127.0.0.1", 0, value), "STUN list entries cannot be empty");
}
// The room factories: one transport per process can still be created against an in-process room, which is enough to
// check the roster-size contract, the incomplete-mesh Start refusal and the closed state without a second GNS identity.
void room_contracts() {
    const auto room_options = [](std::uint32_t peers) { auto value = options(); value.peers = peers; return value; };
    const auto code = [](const auto& result) { return result ? std::string{} : result.error().code; };
    InProcessGnsRoom room(3);
    auto host = require(room.attach(0, "room-host"));
    // Roster size, seat and options are agreed before anything is created.
    expect(code(GnsTransport::host_room(host, room_options(4))) == "EAWR-NET-1765", "Room capacity must equal the agreed roster size");
    expect(code(GnsTransport::host_room(host, room_options(2))) == "EAWR-NET-1765", "A smaller roster is refused too");
    expect(code(GnsTransport::join_room(host, room_options(3))) == "EAWR-NET-1765", "The host seat cannot join as a guest");
    expect(code(GnsTransport::host_room(host, room_options(1))) == "EAWR-NET-1501"
        && code(GnsTransport::host_room(host, room_options(10))) == "EAWR-NET-1501", "Roster size is 2..max_match_seats");
    expect(code(GnsTransport::host_room({}, room_options(3))) == "EAWR-NET-1509", "A room endpoint is required");
    auto timed = room_options(3); timed.pair_timeout = 0ms;
    expect(code(GnsTransport::host_room(host, timed)) == "EAWR-NET-1501", "A pair needs a deadline");
    auto relay = room_options(3); relay.turn_only = true;
    expect(code(GnsTransport::host_room(host, relay)) == "EAWR-NET-1519", "TURN-only needs relay credentials");
    auto stun = room_options(3); stun.stun_servers = "stun:127.0.0.1:3478";
    {
        auto guest = require(room.attach(1, "room-guest"));
        expect(code(GnsTransport::host_room(guest, room_options(3))) == "EAWR-NET-1765", "A guest cannot host the room");
    }
    auto transport = require(GnsTransport::host_room(host, stun));
    expect(transport->mesh() && transport->self() == 0 && transport->peer_count() == 3 && transport->port() == 0, "Room host shape");
    expect(transport->link(1) == LinkState::connecting && transport->link(2) == LinkState::connecting
        && transport->link(0) == LinkState::closed && transport->link(3) == LinkState::closed, "Unfilled slots are still connecting");
    expect(!transport->send(1, {}) && !transport->poll() && !transport->diagnostic(), "No pair, no frames");
    expect(!transport->mesh_complete() && transport->connected_seats() == 0U, "A seat counts as connected only when every pair of the roster is");
    const auto lock = transport->lock_room();
    expect(!lock && lock.error().code == "EAWR-NET-1764", "Start is refused while the mesh is incomplete");
    for (unsigned i = 0; i < 5; ++i) { static_cast<void>(transport->poll()); std::this_thread::sleep_for(2ms); }
    expect(!transport->take_event(), "A lone host has no events");
    expect(transport->signal_counts(1).sent_blobs == 0 && transport->pair(1).phase == GnsPairPhase::absent, "No pair, no signals");
    transport->close();
    until([&] { static_cast<void>(transport->poll()); return transport->closed(); });
    transport.reset();
    // A closed room cannot be joined.
    InProcessGnsRoom closing(2);
    auto first = require(closing.attach(0, "closing-host"));
    require(first->leave());
    expect(code(GnsTransport::host_room(first, room_options(2))) == "EAWR-NET-1760", "A closed room cannot be used");

    // A late joiner's validated commitment arrives on GNS while its room
    // endpoint still reports the pre-Lock roster. No sleeps or TLS race.
    InProcessGnsRoom late(2);
    auto late_host = require(late.attach(0, "late-host"));
    auto late_guest = require(late.attach(1, "late-guest"));
    auto guest_transport = require(GnsTransport::join_room(late_guest, room_options(2)));
    expect(!guest_transport->room_locked(), "Late joiner starts in formation");
    TcpRejoinAuth auth; auth.epoch = 1; auth.match_identity.fill(17);
    auth.seat_tokens.resize(2); auth.seat_tokens[0].fill(23);
    expect(!guest_transport->repair_closed_links(auth), "Staging cannot arm battle repair");
    require(guest_transport->commit_room());
    expect(late_host->roster().state == GnsRoomState::open && guest_transport->room_locked(),
        "Commitment locks the native guest before its roster reports Lock");
    require(guest_transport->repair_closed_links(auth));
    require(guest_transport->commit_room());
    static_cast<void>(guest_transport->poll());
    expect(guest_transport->room_locked(), "A stale open roster cannot undo commitment");
    guest_transport->cancel();
    expect(!guest_transport->commit_room(), "A cancelled mesh cannot commit");
}
class RefusedEndpoint final : public GnsSignallingEndpoint {
public:
    std::optional<GnsTurnCredentials> turn;
    GnsRendezvous rendezvous() const override { return {"refusal-session", "refusal-local", "refusal-remote"}; }
    std::optional<GnsTurnCredentials> turn_credentials() const override { return turn; }
    eawr::core::Result<void> send(std::span<const std::uint8_t>) override {
        throw std::runtime_error("Refused credentials must not send signals");
    }
    eawr::core::Result<std::optional<GnsSignal>> poll() override {
        throw std::runtime_error("Refused credentials must not poll signals");
    }
};
void turn_refusal_logs() {
    const std::string user = "secret-user-4157", password = "secret-password-4157", uri = "turn:secret-host-4157:3478";
    struct Case { std::optional<GnsTurnCredentials> turn; const char* check; };
    std::vector<Case> cases{{{}, "credentials_absent"}};
    auto add = [&](GnsTurnCredentials turn, const char* check) { cases.push_back({std::move(turn), check}); };
    add({"", user, password, 60s}, "uri_empty");
    add({uri, "", password, 60s}, "user_empty");
    add({uri, user, "", 60s}, "cred_empty");
    add({"invalid-secret-uri", user, password, 60s}, "uri_list");
    add({uri + "," + uri, user + ",other", password, 60s}, "user_comma");
    add({uri, user, password + ",other", 60s}, "cred_comma");
    add({uri, user, password, 0s}, "lifetime_nonpositive");
    add({uri, user, password, -7s}, "lifetime_nonpositive");
    std::vector<std::string> lines;
    const auto generation = GnsTransport::set_log_sink([&](std::string_view line) { lines.emplace_back(line); });
    for (const auto& test : cases) {
        for (unsigned factory = 0; factory < 4; ++factory) {
            lines.clear();
            auto config = options(); config.turn_only = true;
            auto result = [&] {
                if (factory < 2) {
                    auto endpoint = std::make_shared<RefusedEndpoint>(); endpoint->turn = test.turn;
                    return factory == 0 ? GnsTransport::host_introduced(endpoint, config)
                        : GnsTransport::connect_introduced(endpoint, config);
                }
                InProcessGnsRoom room(2);
                const auto seat = factory == 2 ? 0U : 1U;
                std::shared_ptr<GnsRoomEndpoint> host;
                if (seat == 1) host = require(room.attach(0, "refusal-host"));
                auto endpoint = require(room.attach(seat, "refusal-member", test.turn));
                return seat == 0 ? GnsTransport::host_room(endpoint, config) : GnsTransport::join_room(endpoint, config);
            }();
            expect(!result && result.error().code == "EAWR-NET-1519", "Refusal retains 1519");
            expect(result.error().message == (test.turn ? "Invalid TURN rendezvous credentials"
                : "TURN-only connection requires relay credentials"), "Refusal retains UI text");
            const auto found = std::find_if(lines.begin(), lines.end(), [](const auto& line) {
                return line.starts_with("EAWR net event=turn-refused ");
            });
            expect(found != lines.end() && std::count_if(lines.begin(), lines.end(), [](const auto& line) {
                return line.starts_with("EAWR net event=turn-refused ");
            }) == 1, "Exactly one refusal line is delivered without poll");
            const auto field = [&](const std::string& text) { return (*found + " ").find(" " + text + " ") != std::string::npos; };
            expect(field(test.turn ? "reason=invalid" : "reason=missing"), "Refusal reason");
            expect(field(std::string("check=") + test.check), "Failed TURN validation check");
            const auto uris = !test.turn || test.turn->uri.empty() ? std::size_t{}
                : static_cast<std::size_t>(std::count(test.turn->uri.begin(), test.turn->uri.end(), ',')) + 1;
            expect(field("uris=" + std::to_string(uris)), "TURN URI count");
            expect(field(!test.turn || test.turn->username.empty() ? "user_empty=1" : "user_empty=0"), "Username empty flag");
            expect(field(!test.turn || test.turn->password.empty() ? "cred_empty=1" : "cred_empty=0"), "Credential empty flag");
            expect(field("expires_in_s=" + (test.turn ? std::to_string(test.turn->lifetime.count()) : "unknown")), "TURN expiry");
            expect(field(factory == 0 || factory == 2 ? "seat=0" : "seat=1"), "Refused seat");
            for (const auto& line : lines) {
                expect(line.find(user) == std::string::npos && line.find(password) == std::string::npos
                    && line.find("secret-host-4157") == std::string::npos && line.find("invalid-secret-uri") == std::string::npos,
                    "No secret or raw URI in diagnostics");
            }
        }
    }
    // Direct TURN-only factories also lack credentials, without a signalling endpoint.
    for (bool hosting : {true, false}) {
        lines.clear(); auto config = options(); config.turn_only = true;
        auto result = hosting ? GnsTransport::host("127.0.0.1", 0, config) : GnsTransport::connect("127.0.0.1", 12345, config);
        expect(!result && result.error().code == "EAWR-NET-1519", "Direct TURN-only refusal");
        expect(lines.size() == 1 && lines.front().find("reason=missing check=credentials_absent uris=0 user_empty=1 cred_empty=1 expires_in_s=unknown")
            != std::string::npos, "Direct refusal is delivered immediately");
    }
    GnsTransport::clear_log_sink(generation);
}
}
int main(int argc, char** argv) {
    try {
        if (!GnsTransport::available()) {
            const auto disabled = GnsTransport::host("127.0.0.1", 0, options());
            expect(!disabled && disabled.error().code == "EAWR-NET-1500", "Typed disabled GNS factory");
            std::cout << "GNS disabled: transport contracts skipped\n"; return 77;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--turn-refusals") {
            turn_refusal_logs(); std::cout << "TURN refusal diagnostics passed\n"; return 0;
        }
        invalid_options(); room_contracts(); frames(); mismatch(0); mismatch(1); mismatch(2); link_stats(); lifecycle(); capacity();
        std::cout << "GNS message/lifecycle contracts passed\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
