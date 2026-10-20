#include "../support/check.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/lan_directory.hpp"
#include "../../src/net/platform_socket.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
void require(bool value, const char* message) {
    if (!value) eawr::testing::require(value, message);
}
template<class F> void until(F predicate, std::chrono::milliseconds timeout = 3000ms) {
    const auto deadline = Clock::now() + timeout;
    while (!predicate()) {
        require(Clock::now() < deadline, "Timed out waiting for transport");
        std::this_thread::sleep_for(1ms);
    }
}
TcpOptions options() {
    TcpOptions result;
    result.build_identity = "tcp-test-build";
    result.keepalive = 20ms; result.timeout = 1000ms;
    return result;
}
struct Pair {
    std::unique_ptr<TcpTransport> host, client;
    Pair(TcpOptions a = options(), TcpOptions b = options()) {
        auto listener = TcpTransport::host("127.0.0.1", 0, std::move(a));
        require(static_cast<bool>(listener), "Create host"); host = std::move(listener).value();
        require(host->port() != 0, "Ephemeral port assigned");
        auto connector = TcpTransport::connect("127.0.0.1", host->port(), std::move(b));
        require(static_cast<bool>(connector), "Create client"); client = std::move(connector).value();
    }
    void open() {
        until([&] { (void)host->poll(); (void)client->poll(); return host->link(1) == LinkState::open && client->link(0) == LinkState::open; });
        require(host->self() == 0 && client->self() == 1 && host->peer_count() == 2, "Host assigns peer indices");
    }
};
void frames() {
    Pair pair; pair.open();
    std::vector<Bytes> expected{{}, {0}, {1}, {6}, {255}, Bytes(1024 * 1024, 173)};
    for (unsigned i = 0; i < 1000; ++i) expected.push_back({static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(i >> 8)});
    for (const auto& bytes : expected) require(static_cast<bool>(pair.host->send(1, bytes)), "Queue host frame");
    std::size_t received = 0;
    until([&] {
        require(!pair.host->poll(), "No application data returned to sender");
        while (auto frame = pair.client->poll()) {
            require(frame->from == 0 && received < expected.size() && frame->frame == expected[received], "Whole ordered host frame");
            ++received;
        }
        return received == expected.size();
    }, 5000ms);
}
void roundtrip() {
    Pair pair; pair.open();
    const std::vector<Bytes> expected{{}, {0, 1, 2, 255}, Bytes(1024 * 1024, 91)};
    for (const auto& bytes : expected) {
        require(static_cast<bool>(pair.host->send(1, bytes)), "Send host message");
        std::optional<Received> remote;
        until([&] { (void)pair.host->poll(); remote = pair.client->poll(); return remote.has_value(); });
        require(remote->frame == bytes && remote->from == 0, "Client receives complete message");
        require(static_cast<bool>(pair.client->send(0, remote->frame)), "Send client echo");
        std::optional<Received> echoed;
        until([&] { (void)pair.client->poll(); echoed = pair.host->poll(); return echoed.has_value(); });
        require(echoed->frame == bytes && echoed->from == 1, "Host receives complete echo");
    }
    require(!pair.host->send(1, Bytes(1024 * 1024 + 1)), "Reject oversized send");
    require(pair.host->link(1) == LinkState::open, "Rejected send does not damage link");
    require(!pair.host->send(0, {}), "Reject self destination");
}
void link_measurement() {
    Pair pair;
    require(!pair.host->link_stats(1) && !pair.client->link_stats(0), "RTT unknown before open");
    pair.open();
    until([&] {
        require(!pair.host->poll() && !pair.client->poll(), "Probes never leak as application frames");
        const auto a = pair.host->link_stats(1), b = pair.client->link_stats(0);
        return a && b && a->sample_count >= 2 && b->sample_count >= 2;
    });
    require(!pair.host->link_stats(0) && !pair.host->link_stats(2), "Self and invalid links have unknown RTT");
    const auto before = *pair.host->link_stats(1);
    std::this_thread::sleep_for(25ms);
    (void)pair.host->poll(); // Sends a probe while the remote application is stalled.
    std::this_thread::sleep_for(60ms);
    until([&] {
        (void)pair.client->poll(); (void)pair.host->poll();
        return pair.host->link_stats(1)->sample_count > before.sample_count;
    });
    const auto after = *pair.host->link_stats(1);
    require(after.last_rtt >= 60ms && after.smoothed_rtt > before.smoothed_rtt && after.jitter > before.jitter,
        "Delayed pong raises measured RTT and jitter");
    const auto target = after.sample_count + 2;
    until([&] {
        require(static_cast<bool>(pair.host->send(1, Bytes{7, 8})), "Busy stream write");
        require(!pair.host->poll(), "Busy stream control stays local");
        while (auto frame = pair.client->poll()) require(frame->frame == Bytes({7, 8}), "Application control-like bytes preserved");
        return pair.host->link_stats(1)->sample_count >= target;
    });
    pair.host->close();
    require(!pair.host->link_stats(1), "Closed links no longer publish stale stats");
}
void mismatch(bool build) {
    auto remote = options();
    if (build) remote.build_identity += "-different"; else ++remote.protocol_version;
    Pair pair(options(), remote);
    until([&] { (void)pair.host->poll(); (void)pair.client->poll(); return pair.host->diagnostic().has_value() && pair.client->link(0) == LinkState::closed; });
    const auto code = build ? "EAWR-NET-0202" : "EAWR-NET-0201";
    require(pair.host->diagnostic() && pair.host->diagnostic()->code == code, "Host has typed mismatch reason");
    require(pair.client->diagnostic() && pair.client->diagnostic()->code == code, "Client has typed mismatch reason");
    require(!pair.client->diagnostic()->message.empty(), "Viewer has readable reason");
}
void probe_under_backpressure() {
    auto config = options(); config.queue_frames = 1;
    config.queue_bytes = static_cast<std::size_t>(config.max_frame_size) + 5;
    Pair pair(config, options()); pair.open();
    std::this_thread::sleep_for(25ms);
    require(!pair.client->poll(), "Client emits a transport-local probe");
    const Bytes expected(config.max_frame_size, 37);
    require(static_cast<bool>(pair.host->send(1, expected)), "Fill application send capacity");
    require(!pair.host->poll() && !pair.host->diagnostic() && pair.host->link(1) == LinkState::open,
        "Pending application frame leaves reserved capacity for the pong");
    bool received = false;
    until([&] {
        (void)pair.host->poll();
        if (auto frame = pair.client->poll()) {
            require(!received && frame->frame == expected, "Congested stream preserves its application frame");
            received = true;
        }
        return received && pair.client->link_stats(0).has_value();
    });
    auto tiny = options(); tiny.max_frame_size = 0; tiny.queue_bytes = 5; tiny.queue_frames = 1;
    Pair empty_frames(tiny, tiny); empty_frames.open();
    until([&] {
        require(!empty_frames.host->poll() && !empty_frames.client->poll(), "Tiny application limit still supports control frames");
        return empty_frames.host->link_stats(1).has_value() && empty_frames.client->link_stats(0).has_value();
    });
}
void crossing_probe_close() {
    for (const bool host_closes : {false, true}) {
        Pair pair; pair.open();
        auto& closer = host_closes ? pair.host : pair.client;
        auto& remote = host_closes ? pair.client : pair.host;
        std::this_thread::sleep_for(25ms);
        require(!remote->poll(), "Remote emits a probe before local close");
        closer->close();
        until([&] {
            require(!pair.host->poll() && !pair.client->poll(), "Crossing controls remain transport-local");
            return pair.host->closed() && pair.client->closed();
        });
        require(!pair.host->diagnostic() && !pair.client->diagnostic(), "A crossing probe cannot turn clean close into refusal");
    }
}
void lifecycle() {
    Pair pair; pair.open();
    const auto idle_until = Clock::now() + 2200ms;
    until([&] {
        require(!pair.host->poll() && !pair.client->poll(), "Keepalive is transport-local");
        require(pair.host->link(1) == LinkState::open && pair.client->link(0) == LinkState::open, "Keepalive retains idle link beyond timeout");
        return Clock::now() >= idle_until;
    });
    pair.client->close();
    until([&] { (void)pair.client->poll(); (void)pair.host->poll(); return pair.host->link(1) == LinkState::closed; });
    require(!pair.host->diagnostic(), "Clean close is not a connection error");
    Pair killed; killed.open(); killed.client.reset();
    until([&] { (void)killed.host->poll(); return killed.host->link(1) == LinkState::closed; }, 1500ms);
    Pair stalled; stalled.open();
    // Stop pumping the client, modelling a hung peer that leaves TCP established.
    until([&] { (void)stalled.host->poll(); return stalled.host->link(1) == LinkState::closed; }, 1500ms);
    require(stalled.host->diagnostic() && stalled.host->diagnostic()->code == "EAWR-NET-0207", "Idle peer produces timeout diagnostic");
    Pair ordered; ordered.open();
    require(static_cast<bool>(ordered.client->send(0, { })), "Queue empty before clean close");
    require(static_cast<bool>(ordered.client->send(0, Bytes(1024 * 1024, 7))), "Queue large before clean close");
    ordered.client->close();
    std::vector<Bytes> delivered;
    until([&] {
        (void)ordered.client->poll();
        if (auto frame = ordered.host->poll()) delivered.push_back(std::move(frame->frame));
        return ordered.host->link(1) == LinkState::closed;
    });
    while (auto frame = ordered.host->poll()) delivered.push_back(std::move(frame->frame));
    require(delivered == std::vector<Bytes>{{}, Bytes(1024 * 1024, 7)}, "Close arrives after every accepted message");
    require(!ordered.host->diagnostic(), "Ordered close is clean");
}
void backpressure() {
    auto config = options(); config.max_frame_size = 64; config.queue_bytes = 128; config.queue_frames = 2;
    Pair pair(config, config); pair.open();
    require(static_cast<bool>(pair.host->send(1, Bytes(64, 3))), "First frame fits queue");
    const auto refused = pair.host->send(1, Bytes(64, 4));
    require(!refused && refused.error().code == "EAWR-NET-0205", "Bounded send queue reports backpressure");
    std::optional<Received> frame;
    until([&] { (void)pair.host->poll(); frame = pair.client->poll(); return frame.has_value(); });
    require(static_cast<bool>(pair.host->send(1, Bytes(64, 4))), "Can retry after pumping");
}
void receive_capacity() {
    auto config = options(); config.max_frame_size = 16; config.queue_bytes = 128; config.queue_frames = 2;
    auto sender = config; sender.queue_frames = 16;
    Pair pair(sender, config); pair.open();
    // Queue a burst while leaving the receiver unpumped, exceeding its frame quota
    // with empty application messages which would bypass a byte-only queue quota.
    for (unsigned i = 0; i < 8; ++i) {
        require(static_cast<bool>(pair.host->send(1, {})), "Queue empty capacity probe");
    }
    (void)pair.host->poll();
    std::this_thread::sleep_for(30ms);
    until([&] { (void)pair.client->poll(); return pair.client->link(0) == LinkState::closed; });
    require(pair.client->diagnostic() && pair.client->diagnostic()->code == "EAWR-NET-0205", "Empty messages cannot overflow receive capacity");
    until([&] { (void)pair.host->poll(); return pair.host->link(1) == LinkState::closed; });
    require(pair.host->diagnostic() && pair.host->diagnostic()->code == "EAWR-NET-0205", "Receive-capacity refusal reaches sender");
    Pair closing(sender, config); closing.open();
    for (unsigned i = 0; i < 8; ++i)
        require(static_cast<bool>(closing.host->send(1, {})), "Queue closing capacity probe");
    (void)closing.host->poll(); std::this_thread::sleep_for(30ms);
    closing.client->close();
    (void)closing.client->poll();
    require(closing.client->diagnostic() && closing.client->diagnostic()->code == "EAWR-NET-0205",
        "Receive quota remains a typed error during graceful shutdown");
    until([&] { (void)closing.client->poll(); (void)closing.host->poll(); return closing.host->diagnostic().has_value() && closing.host->link(1) == LinkState::closed; });
    require(closing.host->diagnostic() && closing.host->diagnostic()->code == "EAWR-NET-0205",
        "Peer receives typed refusal after the close request");
}
void unread_close() {
    Pair pair; pair.open();
    for (unsigned i = 0; i < 100; ++i)
        require(static_cast<bool>(pair.host->send(1, Bytes(32, static_cast<std::uint8_t>(i)))), "Queue unread close traffic");
    (void)pair.host->poll(); std::this_thread::sleep_for(30ms);
    pair.client->close();
    until([&] { (void)pair.client->poll(); (void)pair.host->poll(); return pair.host->link(1) == LinkState::closed; });
    for (unsigned i = 0; i < 10; ++i) { (void)pair.client->poll(); (void)pair.host->poll(); }
    require(!pair.host->diagnostic() && !pair.client->diagnostic(), "Unread incoming traffic does not reset clean close");
}
void host_lifecycle() {
    auto config = options(); config.timeout = 100ms; config.handshake_timeout = 200ms;
    auto result = TcpTransport::host("127.0.0.1", 0, config);
    require(static_cast<bool>(result), "Persistent listener"); auto host = std::move(result).value();
    const auto wait_until = Clock::now() + 350ms;
    until([&] { (void)host->poll(); return Clock::now() >= wait_until; });
    require(host->link(1) == LinkState::connecting && !host->diagnostic(), "Listen phase is not timed out");
    auto wrong = config; wrong.build_identity += "-wrong";
    auto bad_result = TcpTransport::connect("127.0.0.1", host->port(), wrong);
    require(static_cast<bool>(bad_result), "Wrong-build connector"); auto bad = std::move(bad_result).value();
    until([&] { (void)host->poll(); (void)bad->poll(); return host->diagnostic() && host->link(1) == LinkState::connecting && bad->link(0) == LinkState::closed; });
    require(host->diagnostic()->code == "EAWR-NET-0202" && bad->diagnostic() && bad->diagnostic()->code == "EAWR-NET-0202", "Typed refusal reaches both sides before retry");
    const auto dropped = socket::connect("127.0.0.1", host->port());
    require(dropped != socket::invalid, "Dropped connector");
    until([&] { (void)host->poll(); return socket::connected(dropped) == socket::Status::ready && !host->diagnostic(); });
    socket::close(dropped);
    until([&] { (void)host->poll(); return host->link(1) == LinkState::connecting && host->diagnostic() && host->diagnostic()->code == "EAWR-NET-0206"; });
    auto good_result = TcpTransport::connect("127.0.0.1", host->port(), config);
    require(static_cast<bool>(good_result), "Matching connector after rejected/dropped attempts"); auto good = std::move(good_result).value();
    until([&] { (void)host->poll(); (void)good->poll(); return host->link(1) == LinkState::open && good->link(0) == LinkState::open; });
    require(!host->diagnostic(), "Successful retry clears attempt diagnostic");
    auto pending_result = TcpTransport::host("127.0.0.1", 0, config);
    require(static_cast<bool>(pending_result), "Cancellable listener"); auto pending = std::move(pending_result).value();
    const auto closed_port = pending->port(); pending->cancel();
    require(pending->link(1) == LinkState::closed, "Cancel closes pending listener");
    auto refused = TcpTransport::connect("127.0.0.1", closed_port, options());
    if (!refused) require(refused.error().code == "EAWR-NET-0206", "Refused connect has typed factory error");
    else {
        auto connector = std::move(refused).value();
        until([&] { (void)connector->poll(); return connector->link(0) == LinkState::closed; });
        require(connector->diagnostic() && connector->diagnostic()->code == "EAWR-NET-0209", "Refused connect has typed asynchronous error");
    }
}
void close_failed_join() {
    const auto config = options();
    auto listener = TcpTransport::host("127.0.0.1", 0, config);
    require(static_cast<bool>(listener), "Failed-join close host"); auto host = std::move(listener).value();
    const auto raw = socket::connect("127.0.0.1", host->port());
    require(raw != socket::invalid, "Failed-join raw connector");
    struct Guard { socket::Handle handle; ~Guard() { socket::close(handle); } } guard{raw};
    until([&] { (void)host->poll(); return socket::connected(raw) == socket::Status::ready; });
    // A raw joiner deliberately withholds EOF, making the failed-join drain stable.
    Bytes hello{1, 'E', 'A', 'W', 'R'};
    const auto append = [](Bytes& bytes, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    append(hello, config.protocol_version);
    const auto wrong = config.build_identity + "-wrong";
    append(hello, static_cast<std::uint32_t>(wrong.size())); hello.insert(hello.end(), wrong.begin(), wrong.end());
    append(hello, config.max_frame_size); hello.push_back(1);
    Bytes wire; append(wire, static_cast<std::uint32_t>(hello.size())); wire.insert(wire.end(), hello.begin(), hello.end());
    std::size_t sent = 0;
    until([&] { const auto io = socket::write(raw, std::span<const std::uint8_t>(wire).subspan(sent)); sent += io.count; return sent == wire.size(); });
    until([&] { (void)host->poll(); return host->diagnostic().has_value(); });
    require(host->link(1) == LinkState::closed && host->diagnostic()->code == "EAWR-NET-0202", "Rejected host is draining");
    host->close();
    require(socket::shutdown_write(raw), "Release failed-join EOF after host close");
    Bytes drain(4096);
    until([&] { (void)host->poll(); return socket::read(raw, drain).status == socket::Status::closed; });
    for (unsigned i = 0; i < 20; ++i) {
        (void)host->poll();
        std::this_thread::sleep_for(1ms);
    }
    require(host->link(1) == LinkState::closed, "Close during failed join prevents listening again");
}
void fixed_port_rehost() {
#ifndef _WIN32
    auto selected = TcpTransport::host("127.0.0.1", 0, options());
    require(static_cast<bool>(selected), "Select available fixed port");
    const auto port = selected.value()->port(); selected.value().reset();
    auto listener = TcpTransport::host("127.0.0.1", port, options());
    require(static_cast<bool>(listener), "Host on fixed port"); auto host = std::move(listener).value();
    auto connector = TcpTransport::connect("127.0.0.1", port, options());
    require(static_cast<bool>(connector), "Connect to fixed port"); auto client = std::move(connector).value();
    until([&] { (void)host->poll(); (void)client->poll(); return host->link(1) == LinkState::open && client->link(0) == LinkState::open; });
    client->close();
    until([&] { (void)client->poll(); (void)host->poll(); return host->link(1) == LinkState::closed && client->link(0) == LinkState::closed; });
    for (unsigned i = 0; i < 20; ++i) {
        (void)host->poll(); (void)client->poll(); std::this_thread::sleep_for(1ms);
    }
    host.reset(); client.reset();
    auto replacement = TcpTransport::host("127.0.0.1", port, options());
    require(static_cast<bool>(replacement) && replacement.value()->port() == port, "Immediately re-host same fixed port after clean close");
#endif
}
void slow_frame() {
    auto config = options(); config.max_frame_size = 2 * 1024 * 1024; config.queue_bytes = 4 * 1024 * 1024;
    config.timeout = 500ms;
    Pair pair(config, config); pair.open();
    const Bytes expected(config.max_frame_size, 99);
    require(static_cast<bool>(pair.host->send(1, expected)), "Queue slow large frame");
    const auto start = Clock::now(); std::optional<Received> frame;
    until([&] {
        (void)pair.host->poll(); frame = pair.client->poll();
        require(pair.host->link(1) == LinkState::open && pair.client->link(0) == LinkState::open, "Partial bytes count as liveness");
        if (!frame) std::this_thread::sleep_for(100ms);
        return frame.has_value();
    }, 5000ms);
    require(Clock::now() - start > config.timeout && frame->frame == expected, "Slow whole frame arrives beyond idle timeout");
}
void invalid_options() {
    require(!TcpTransport::host("127.0.0.1", 0), "Build identity is required");
    auto config = options(); config.timeout = config.keepalive;
    require(!TcpTransport::host("127.0.0.1", 0, config), "Timeout must exceed keepalive");
    config = options(); config.max_frame_size = 16 * 1024 * 1024 + 1;
    require(!TcpTransport::host("127.0.0.1", 0, config), "Frame allocation ceiling enforced");
    require(!TcpTransport::connect("", 10, options()), "Empty host rejected");
}
void join_endpoints() {
    for (const auto& [text, host, port] : std::vector<std::tuple<std::string, std::string, unsigned>>{
        {"localhost", "localhost", TcpOptions::default_port}, {"localhost:42", "localhost", 42},
        {"127.0.0.1:65535", "127.0.0.1", 65535}, {"[::1]", "::1", TcpOptions::default_port},
        {"[::1]:42", "::1", 42}, {"::1", "::1", TcpOptions::default_port}}) {
        const auto parsed = parse_join_endpoint(text);
        require(parsed && parsed.value().host == host && parsed.value().port == port, "Join endpoint parsed");
    }
    for (const auto* text : {"", ":42", "host:", "host:0", "host:65536", "host:-1", "host:42x", "[::1", "[::1]x", "[::1]:", "[host]", "host name", "host]"}) {
        const auto parsed = parse_join_endpoint(text);
        require(!parsed && parsed.error().code == "EAWR-NET-0211", "Malformed endpoint rejected with useful diagnostic");
    }
}
void resolved_loopback() {
    const auto exchange = [](const std::string& bind, const std::string& destination, bool optional) {
        auto listener = TcpTransport::host(bind, 0, options());
        if (!listener && optional) { std::cout << "IPv6 unavailable: skipping IPv6 loopback\n"; return; }
        require(static_cast<bool>(listener), "Resolved loopback listener"); auto host = std::move(listener).value();
        auto opened = TcpTransport::connect(destination, host->port(), options());
        require(static_cast<bool>(opened), "Asynchronous resolved connector"); auto client = std::move(opened).value();
        require(!client->closed(), "Pending resolution is not closed");
        until([&] { (void)host->poll(); (void)client->poll(); return host->link(1) == LinkState::open && client->link(0) == LinkState::open; });
        const Bytes expected{1, 2, 3}; require(static_cast<bool>(client->send(0, expected)), "Resolved loopback send");
        std::optional<Received> received;
        until([&] { (void)client->poll(); received = host->poll(); return received.has_value(); });
        require(received->frame == expected, "Resolved loopback payload");
    };
    exchange("127.0.0.1", "localhost", false); // Exercises IPv6-first refusal then IPv4 fallback where available.
    const auto candidates = socket::resolve("localhost", 1);
    const auto ipv4 = socket::resolve("127.0.0.1", 1);
    if (!candidates.empty() && !ipv4.empty()) {
        const auto first_family = candidates.front().family;
        const bool other_family = std::any_of(candidates.begin(), candidates.end(),
            [&](const auto& value) { return value.family != first_family; });
        if (other_family) {
            // Bind only the family behind the first candidate, forcing the first attempt to fail.
            exchange(first_family == ipv4.front().family ? "::1" : "127.0.0.1", "localhost", true);
        } else std::cout << "Single-family localhost: skipping ordered family fallback\n";
    }
    exchange("::1", "::1", true);
    exchange("0.0.0.0", "127.0.0.1", false);
    if (TcpTransport::host("::1", 0, options())) exchange("0.0.0.0", "::1", false);
    auto pending = TcpTransport::connect("localhost", 1, options());
    require(static_cast<bool>(pending), "Cancellable DNS request"); pending.value()->cancel();
    (void)pending.value()->poll();
    require(pending.value()->closed() && pending.value()->link(0) == LinkState::closed, "Cancel cannot revive resolved connection");
}
struct RawPeer {
    std::unique_ptr<TcpTransport> host;
    socket::Handle stream{socket::invalid};
    Bytes received;
    std::vector<Bytes> envelopes;
    static void word(Bytes& bytes, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
    static void envelope(Bytes& wire, const Bytes& body) {
        word(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end());
    }
    explicit RawPeer(TcpOptions config = options()) {
        auto opened = TcpTransport::host("127.0.0.1", 0, config);
        require(static_cast<bool>(opened), "Raw peer host"); host = std::move(opened).value();
        stream = socket::connect("127.0.0.1", host->port());
        require(stream != socket::invalid, "Raw peer connector");
        until([&] { (void)host->poll(); return socket::connected(stream) == socket::Status::ready; });
        Bytes hello{1, 'E', 'A', 'W', 'R'}; word(hello, config.protocol_version);
        word(hello, static_cast<std::uint32_t>(config.build_identity.size()));
        hello.insert(hello.end(), config.build_identity.begin(), config.build_identity.end());
        word(hello, config.max_frame_size); hello.push_back(1);
        Bytes wire; envelope(wire, hello); envelope(wire, {2, 1, 0, 0, 0}); write(wire);
        until([&] { (void)host->poll(); return host->link(1) == LinkState::open; });
        require(!host->diagnostic(), "Raw peer handshake opens cleanly");
    }
    ~RawPeer() { socket::close(stream); }
    void write(const Bytes& wire) {
        std::size_t sent = 0;
        until([&] { const auto result = socket::write(stream, std::span<const std::uint8_t>(wire).subspan(sent));
            require(result.status == socket::Status::ready || result.status == socket::Status::pending, "Raw peer write");
            sent += result.count; return sent == wire.size(); });
    }
    void read() {
        std::array<std::uint8_t, 4096> bytes{};
        const auto result = socket::read(stream, bytes);
        received.insert(received.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(result.count));
        while (received.size() >= 4) {
            std::uint32_t length{};
            for (unsigned i = 0; i < 4; ++i) length |= static_cast<std::uint32_t>(received[i]) << (8 * i);
            if (received.size() - 4 < length) break;
            envelopes.emplace_back(received.begin() + 4, received.begin() + 4 + length);
            received.erase(received.begin(), received.begin() + 4 + length);
        }
    }
};
void open_probe_refusals() {
    for (const std::uint8_t kind : {std::uint8_t{0}, std::uint8_t{255}}) {
        RawPeer peer; Bytes wire; RawPeer::envelope(wire, {kind}); peer.write(wire);
        until([&] { require(!peer.host->poll(), "Unknown open-state control is not application data"); return peer.host->diagnostic().has_value(); });
        require(peer.host->diagnostic()->code == "EAWR-NET-0203" && peer.host->link(1) == LinkState::closed,
            "Unknown open-state kind fails closed");
    }
    for (const std::uint8_t kind : {std::uint8_t{7}, std::uint8_t{8}}) {
        for (const unsigned size : {3U, 5U}) {
            RawPeer peer; Bytes body{kind}; body.resize(1 + size, 0);
            Bytes wire; RawPeer::envelope(wire, body); peer.write(wire);
            until([&] { require(!peer.host->poll(), "Malformed controls never reach application"); return peer.host->diagnostic().has_value(); });
            require(peer.host->diagnostic()->code == "EAWR-NET-0800", "Malformed open-state probe has reserved refusal");
        }
    }
    for (const bool byte_bound : {false, true}) {
        auto config = options(); config.queue_frames = byte_bound ? 4096U : 1U;
        config.max_frame_size = byte_bound ? 0U : 64U; config.queue_bytes = byte_bound ? 5U : 69U;
        config.keepalive = 1000ms; config.timeout = 5s;
        RawPeer flood(config); Bytes wire;
        for (unsigned i = 0; i < 64; ++i) RawPeer::envelope(wire, {7, 1, 0, 0, 0});
        flood.write(wire);
        until([&] {
            require(!flood.host->poll(), "Flood controls never reach application"); flood.read();
            return std::any_of(flood.envelopes.begin(), flood.envelopes.end(), [](const auto& body) { return body.front() == 3; });
        });
        require(flood.host->diagnostic() && flood.host->diagnostic()->code == "EAWR-NET-0801", "Ping flood has bounded-capacity refusal");
        require(std::count_if(flood.envelopes.begin(), flood.envelopes.end(), [](const auto& body) { return body.front() == 8; }) == (byte_bound ? 2 : 3),
            "Flood emits only bounded pong frames before refusal");
    }
}
void stale_probe_echoes() {
    RawPeer peer; Bytes unsolicited;
    RawPeer::envelope(unsolicited, {8, 0, 0, 0, 0}); RawPeer::envelope(unsolicited, {6, 41}); peer.write(unsolicited);
    until([&] { auto marker = peer.host->poll(); return marker && marker->frame == Bytes{41}; });
    require(!peer.host->link_stats(1), "Processed unsolicited pong cannot manufacture a sample");
    std::optional<Bytes> ping;
    until([&] {
        require(!peer.host->poll(), "Probe is transport-local"); peer.read();
        for (const auto& body : peer.envelopes) if (body.front() == 7) ping = body;
        return ping.has_value();
    });
    Bytes pong = *ping; pong.front() = 8;
    Bytes echo; RawPeer::envelope(echo, pong); peer.write(echo);
    until([&] { require(!peer.host->poll(), "Pong is transport-local"); return peer.host->link_stats(1).has_value(); });
    const auto measured = *peer.host->link_stats(1);
    RawPeer::envelope(echo, {6, 42}); peer.write(echo);
    until([&] { auto marker = peer.host->poll(); return marker && marker->frame == Bytes{42}; });
    require(peer.host->link_stats(1)->sample_count == measured.sample_count
        && peer.host->link_stats(1)->last_rtt == measured.last_rtt, "Stale duplicate echo cannot change measured stats");
}
void resolution_errors() {
    auto invalid = TcpTransport::connect(":::invalid", 1, options());
    require(static_cast<bool>(invalid), "Resolution failure is asynchronous");
    until([&] { (void)invalid.value()->poll(); return invalid.value()->closed(); });
    require(invalid.value()->diagnostic() && invalid.value()->diagnostic()->code == "EAWR-NET-0208", "Malformed literal has resolution failure code");
    auto config = options(); config.handshake_timeout = 1ms;
    auto delayed = TcpTransport::connect("localhost", 1, config);
    require(static_cast<bool>(delayed), "Resolution deadline connector");
    std::this_thread::sleep_for(5ms); // Deadline checked before collecting even an already-completed mailbox.
    (void)delayed.value()->poll();
    require(delayed.value()->closed() && delayed.value()->diagnostic()->code == "EAWR-NET-0210", "Uncollected DNS request reaches the resolution deadline");
    // Documentation-only destination: both immediate routing refusal and a blackholed SYN
    // must produce the candidate-exhaustion diagnostic, rather than a handshake timeout.
    config = options(); config.handshake_timeout = 500ms;
    auto unreachable = TcpTransport::connect("203.0.113.7", 47624, config);
    require(static_cast<bool>(unreachable), "Connect deadline connector");
    until([&] { (void)unreachable.value()->poll(); return unreachable.value()->closed(); });
    require(unreachable.value()->diagnostic() && unreachable.value()->diagnostic()->code == "EAWR-NET-0209", "Connect deadline retains address-attempt diagnostic");
}
void text_contracts() {
    const std::string replacement = "\xEF\xBF\xBD";
    const std::vector<std::pair<std::string, std::string>> cases{
        {"a\xE2\x82", "a" + replacement}, {"\xC0\xAF", replacement + replacement},
        {"a\x80z", "a" + replacement + "z"}, {"\xED\xA0\x80", replacement + replacement + replacement},
        {std::string("a\0b", 3), "a" + replacement + "b"},
        {"J\xC3\xBCrgen \xF0\x9F\x9A\x80", "J\xC3\xBCrgen \xF0\x9F\x9A\x80"},
        {std::string(1023, 'a') + "\x80", std::string(1023, 'a')},
    };
    const auto word = [](Bytes& bytes, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto text = [&](Bytes& bytes, const std::string& value) {
        word(bytes, static_cast<std::uint32_t>(value.size())); bytes.insert(bytes.end(), value.begin(), value.end());
    };
    for (const auto& [raw_text, clean] : cases) {
        auto local = options(); local.build_identity = raw_text;
        auto remote = options(); remote.build_identity = clean;
        Pair pair(local, remote); pair.open(); // Encoder and local comparison use the same rule.
        for (const bool hello : {false, true}) {
            auto config = options(); config.build_identity = clean;
            auto listener = TcpTransport::host("127.0.0.1", 0, config);
            require(static_cast<bool>(listener), "Text contract listener"); auto host = std::move(listener).value();
            const auto raw = socket::connect("127.0.0.1", host->port());
            require(raw != socket::invalid, "Text contract raw connector");
            struct Guard { socket::Handle handle; ~Guard() { socket::close(handle); } } guard{raw};
            until([&] { (void)host->poll(); return socket::connected(raw) == socket::Status::ready; });
            Bytes body;
            if (hello) {
                body = {1, 'E', 'A', 'W', 'R'}; word(body, config.protocol_version);
                text(body, raw_text); word(body, config.max_frame_size); body.push_back(1);
            } else { body = {3}; text(body, "EAWR-NET-0202"); text(body, raw_text); }
            Bytes wire; word(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end());
            if (hello) { word(wire, 5); wire.insert(wire.end(), {2, 1, 0, 0, 0}); }
            std::size_t sent = 0;
            until([&] {
                (void)host->poll(); const auto io = socket::write(raw, std::span<const std::uint8_t>(wire).subspan(sent));
                sent += io.count; return sent == wire.size();
            });
            until([&] { (void)host->poll(); return hello ? host->link(1) == LinkState::open : host->diagnostic().has_value(); });
            if (hello) require(!host->diagnostic(), "Raw hello UTF-8 is repaired before comparison");
            else require(host->diagnostic()->message == clean, "Raw refusal UTF-8 is repaired before display");
        }
    }
}
void prefix(Bytes bytes, std::uint32_t length) {
    auto listener = TcpTransport::host("127.0.0.1", 0, options());
    require(static_cast<bool>(listener), "Raw test host"); auto host = std::move(listener).value();
    const auto raw = socket::connect("127.0.0.1", host->port());
    require(raw != socket::invalid, "Raw test client");
    struct Guard { socket::Handle handle; ~Guard() { socket::close(handle); } } guard{raw};
    until([&] { (void)host->poll(); return socket::connected(raw) == socket::Status::ready; });
    Bytes wire;
    for (unsigned i = 0; i < 4; ++i) wire.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
    wire.insert(wire.end(), bytes.begin(), bytes.end());
    // One-byte writes force partial-prefix/body intake through the real socket adapter.
    for (const auto byte : wire) {
        until([&] { return socket::write(raw, std::span<const std::uint8_t>(&byte, 1)).status == socket::Status::ready; });
        (void)host->poll();
    }
    until([&] { (void)host->poll(); return host->link(1) == LinkState::closed; });
    const auto code = bytes.empty() ? "EAWR-NET-0204" : (bytes[0] == 7 || bytes[0] == 8) ? "EAWR-NET-0800" : "EAWR-NET-0203";
    require(host->diagnostic() && host->diagnostic()->code == code, "Malformed wire is typed refusal");
}
void authenticated_repair() {
    for (const auto injected : {std::uint8_t{58}, std::uint8_t{48}}) {
        auto config = options(); config.handshake_timeout = 100ms;
        Pair pair(config, config); pair.open();
        pair.client->cancel();
        until([&] { (void)pair.host->poll(); return pair.host->link(1) == LinkState::closed; });
        require(!pair.host->repair_closed_links() && pair.host->link(1) == LinkState::closed,
            "repair with no provider cannot admit a replacement stream");
        eawr::net::TcpRejoinAuth auth; auth.match_identity.fill(42); auth.epoch = 1;
        auth.seat_tokens.resize(2); auth.seat_tokens[0].fill(77); auth.seat_tokens[1].fill(77);
        require(static_cast<bool>(pair.host->repair_closed_links(auth)), "begin authenticated host repair");
        auto stranger_result = TcpTransport::connect("127.0.0.1", pair.host->port(), config);
        require(static_cast<bool>(stranger_result), "stranger connects before the rightful seat");
        auto stranger = std::move(stranger_result).value();
        until([&] {
            require(!pair.host->poll(), "quarantine forwards no unproved application traffic"); (void)stranger->poll();
            return stranger->link(0) == LinkState::open;
        });
        require(pair.host->link(1) != LinkState::open && pair.host->listening(), "stranger cannot occupy the logical seat or close its listener");
        require(static_cast<bool>(stranger->send(0, Bytes{injected})), "stranger injects recovery/departure control");
        until([&] {
            require(!pair.host->poll(), "stranger control cannot reach lockstep"); (void)stranger->poll();
            return stranger->link(0) == LinkState::closed;
        });
        require(pair.host->listening(), "refused stranger preserves rightful admission");
        auto wrong = auth; wrong.seat_tokens.at(0).fill(22); wrong.seat_tokens.at(1).fill(22);
        require(static_cast<bool>(pair.client->repair_closed_links(wrong)), "wrong credential starts a quarantined attempt");
        until([&] {
            require(!pair.host->poll(), "wrong credential forwards no application traffic"); (void)pair.client->poll();
            return pair.client->link(0) == LinkState::closed;
        });
        require(pair.host->link(1) != LinkState::open && pair.host->listening(), "wrong credential cannot close rightful admission");
        require(static_cast<bool>(pair.client->repair_closed_links(auth)), "rightful client repairs with confidential credentials");
        until([&] {
            require(!pair.host->poll() && !pair.client->poll(), "authentication frames stay below the application layer");
            return pair.host->link(1) == LinkState::open && pair.client->link(0) == LinkState::open;
        });
        require(!pair.host->listening(), "proved direct repair closes its listener");
        const Bytes bytes{8, 9}; require(static_cast<bool>(pair.client->send(0, bytes)), "proved seat resumes application traffic");
        until([&] {
            (void)pair.client->poll();
            auto frame = pair.host->poll();
            if (!frame) return false;
            require(frame->from == 1 && frame->frame == bytes, "repaired sender identity is unchanged");
            return true;
        });
    }
}
void close_unproved_repair() {
    auto config = options(); config.handshake_timeout = 1000ms;
    Pair pair(config, config); pair.open();
    pair.client->cancel();
    until([&] { (void)pair.host->poll(); return pair.host->link(1) == LinkState::closed; });
    TcpRejoinAuth auth; auth.match_identity.fill(42); auth.epoch = 1;
    auth.seat_tokens.resize(2); auth.seat_tokens[0].fill(77); auth.seat_tokens[1].fill(77);
    require(static_cast<bool>(pair.host->repair_closed_links(auth)), "begin repair before explicit close");
    auto stranger_result = TcpTransport::connect("127.0.0.1", pair.host->port(), config);
    require(static_cast<bool>(stranger_result), "connect unproved close fixture");
    auto stranger = std::move(stranger_result).value();
    until([&] {
        require(!pair.host->poll(), "unproved close fixture forwards no application frames");
        const auto challenge = stranger->poll();
        return challenge && !challenge->frame.empty() && challenge->frame.front() == rejoin_hello_kind;
    });
    require(pair.host->listening() && pair.host->link(1) != LinkState::open,
        "credential challenge proves physical open while logical admission is held");
    until([&] {
        require(!pair.host->poll(), "pre-proof probes remain below the application layer");
        (void)stranger->poll();
        require(!pair.host->link_stats(1), "quarantined stranger cannot supply public RTT samples");
        return stranger->link_stats(0).has_value();
    });
    for (unsigned round = 0; round < 10; ++round) {
        (void)pair.host->poll(); (void)stranger->poll();
        require(pair.host->link(1) == LinkState::connecting && !pair.host->link_stats(1),
            "crossing pre-proof probes never expose stranger-driven RTT");
        std::this_thread::sleep_for(1ms);
    }
    pair.host->close();
    require(pair.host->closed() && !pair.host->listening(), "explicit close retires unproved stream and listener");
    until([&] { (void)pair.host->poll(); (void)stranger->poll(); return stranger->closed(); });
    auto later_result = TcpTransport::connect("127.0.0.1", pair.host->port(), config);
    require(static_cast<bool>(later_result), "later connector has an asynchronous refusal");
    auto later = std::move(later_result).value();
    until([&] { (void)pair.host->poll(); (void)later->poll(); return later->closed(); });
    require(pair.host->closed() && !pair.host->listening() && pair.host->link(1) == LinkState::closed,
        "explicit close never retries or accepts a later connector");
}
void repair_retry() {
    Pair pair; pair.open(); const auto port = pair.host->port();
    pair.host->cancel(); pair.client->cancel();
    const auto occupied = socket::listen("127.0.0.1", port);
    require(occupied.handle != socket::invalid, "occupy the vacated repair port");
    eawr::net::TcpRejoinAuth auth; auth.match_identity.fill(42); auth.epoch = 1;
    auth.seat_tokens.resize(2); auth.seat_tokens[0].fill(77); auth.seat_tokens[1].fill(77);
    require(!pair.host->repair_closed_links(auth), "transient bind failure remains a retryable repair result");
    require(pair.host->self() == 0 && pair.host->link(1) == LinkState::closed, "failed repair retains the stable transport owner");
    socket::close(occupied.handle);
    require(static_cast<bool>(pair.host->repair_closed_links(auth)) && static_cast<bool>(pair.client->repair_closed_links(auth)),
        "repair retries after the transient bind failure");
    pair.open();
}

}
int main() {
    try {
        authenticated_repair(); close_unproved_repair(); repair_retry();
        join_endpoints(); resolved_loopback(); resolution_errors(); text_contracts(); open_probe_refusals(); stale_probe_echoes(); frames(); roundtrip(); link_measurement(); probe_under_backpressure(); crossing_probe_close(); mismatch(false); mismatch(true); lifecycle(); backpressure(); receive_capacity();
        unread_close(); host_lifecycle(); close_failed_join(); fixed_port_rehost(); slow_frame(); invalid_options();
        prefix({}, 1024 * 1024 + 2); prefix({}, 0); prefix({255}, 1); prefix({1, 'E'}, 2);
        prefix({7}, 1); prefix({8, 0, 0, 0, 0}, 5);
        std::cout << "TCP transport contracts passed\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
