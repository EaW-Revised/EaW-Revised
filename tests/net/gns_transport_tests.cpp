#include "eawr/net/gns_transport.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
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
}
}
int main() {
    try {
        if (!GnsTransport::available()) {
            const auto disabled = GnsTransport::host("127.0.0.1", 0, options());
            expect(!disabled && disabled.error().code == "EAWR-NET-1500", "Typed disabled GNS factory");
            std::cout << "GNS disabled: transport contracts skipped\n"; return 77;
        }
        invalid_options(); frames(); mismatch(0); mismatch(1); mismatch(2); link_stats(); lifecycle(); capacity();
        std::cout << "GNS message/lifecycle contracts passed\n"; return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
