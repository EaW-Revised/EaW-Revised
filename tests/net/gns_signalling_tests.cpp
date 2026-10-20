#include "../support/check.hpp"
#include "../support/result_require.hpp"
#include "eawr/net/gns_signalling.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
InProcessGnsIntroduction::Clock::time_point time_now{};
InProcessGnsIntroduction::Clock::time_point now() { return time_now; }
void expect(bool value, const char* reason) {
    if (!value) eawr::testing::require(value, reason);
}
using eawr::testing::result_code_colon::require;
void protocol() {
    time_now = {};
    InProcessGnsIntroduction service(now, 8, 2);
    auto host = require(service.publish("host-peer", 1s));
    expect(!host.endpoint->send(Bytes{1}), "Unpaired introduction cannot route");
    expect(!service.join(host.join_code, "host-peer"), "Duplicate peer identity rejected");
    auto peer = require(service.join(host.join_code, "client-peer"));
    expect(host.endpoint->rendezvous().remote_identity == "client-peer"
        && peer->rendezvous().remote_identity == "host-peer"
        && peer->rendezvous().session == host.endpoint->rendezvous().session, "Paired rendezvous identities");
    expect(!service.join(host.join_code, "intruder"), "Consumed join code rejects third peer");
    expect(!service.join("missing-code", "intruder"), "Unknown code rejected");
    Bytes bytes{1, 2, 3, 4}; require(host.endpoint->send(bytes)); bytes[0] = 255;
    require(host.endpoint->send(Bytes{5, 6, 7, 8}));
    const auto full = host.endpoint->send(Bytes{9});
    expect(!full && full.error().code == "EAWR-NET-1513", "Signalling byte/frame capacity");
    const auto first = require(peer->poll());
    expect(first && first->from == "host-peer" && first->bytes == Bytes{1, 2, 3, 4}, "Signals own their opaque payload");
    require(host.endpoint->send(Bytes{9}));
    expect(require(peer->poll())->bytes == Bytes{5, 6, 7, 8}, "Signals preserve endpoint order");
    expect(require(peer->poll())->bytes == Bytes{9}, "Signalling retry after drain");
    expect(!require(peer->poll()), "Signalling empty poll");
    require(peer->send(Bytes{10}));
    expect(require(host.endpoint->poll())->from == "client-peer", "Reverse signalling identity");
    expect(!peer->send({}) && !peer->send(Bytes(65537)), "Signal size validation");
    time_now += 1s;
    expect(!require(peer->poll()), "Join-code expiry does not expire a paired signalling endpoint");
    require(peer->send(Bytes{1}));
    expect(require(host.endpoint->poll())->bytes == Bytes{1}, "Paired signals flow after admission expiry");
    const auto expiring = require(service.publish("another-host", 10ms));
    time_now += 10ms;
    const auto expired = service.join(expiring.join_code, "another-client");
    expect(!expired && expired.error().code == "EAWR-NET-1510", "Join code expires at its deadline");
    expect(!service.publish("", 1s) && !service.publish("host", 0ms), "Invalid introduction options");
    expect(!service.publish(std::string(32, 'x'), 1s), "Introduction rejects IDs beyond GNS generic string capacity");
    auto longest = require(service.publish(std::string(31, 'a'), 1s));
    auto longest_peer = require(service.join(longest.join_code, std::string(31, 'b')));
    expect(longest_peer->rendezvous().local_identity.size() == 31, "Full GNS identity capacity is usable");
}
void threaded_callbacks() {
    time_now = {};
    std::shared_ptr<GnsSignallingEndpoint> host, peer;
    {
        InProcessGnsIntroduction service(now, 4096, 512);
        auto published = require(service.publish("thread-host", 1s));
        host = published.endpoint; peer = require(service.join(published.join_code, "thread-peer"));
    }
    std::atomic<unsigned> failures{};
    const auto producer = [&](std::uint8_t id) {
        for (unsigned i = 0; i < 100; ++i)
            if (!host->send(Bytes{id, static_cast<std::uint8_t>(i)})) ++failures;
    };
    std::thread one(producer, std::uint8_t{1}), two(producer, std::uint8_t{2}); one.join(); two.join();
    expect(failures == 0, "Thread-safe GNS callback enqueue");
    std::array<unsigned, 2> count{};
    while (auto signal = require(peer->poll())) {
        expect(signal->bytes.size() == 2 && signal->bytes[0] >= 1 && signal->bytes[0] <= 2, "Threaded payload intact");
        auto& received = count[signal->bytes[0] - 1];
        expect(signal->bytes[1] == received++, "Each callback thread remains ordered");
    }
    expect(count == std::array<unsigned, 2>{100, 100}, "All concurrent signals drained after service destruction");
}
void concurrent_admission() {
    time_now = {};
    InProcessGnsIntroduction service(now);
    std::array<GnsIntroduction, 16> hosts;
    std::array<std::shared_ptr<GnsSignallingEndpoint>, 16> peers;
    std::array<std::thread, 16> threads;
    std::atomic<unsigned> failures{};
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i] = std::thread([&, i] {
        auto published = service.publish("host-" + std::to_string(i), 1s);
        if (!published) { ++failures; return; }
        hosts[i] = std::move(published).value();
        auto joined = service.join(hosts[i].join_code, "peer-" + std::to_string(i));
        if (!joined) { ++failures; return; }
        peers[i] = std::move(joined).value();
        if (!hosts[i].endpoint->send(Bytes{static_cast<std::uint8_t>(i)})) ++failures;
    });
    for (auto& thread : threads) thread.join();
    expect(failures == 0, "Concurrent publish/join succeeds");
    std::set<std::string> codes;
    for (std::size_t i = 0; i < peers.size(); ++i) {
        codes.insert(hosts[i].join_code);
        expect(require(peers[i]->poll())->bytes == Bytes{static_cast<std::uint8_t>(i)}, "Concurrent admissions retain correct peer routing");
    }
    expect(codes.size() == peers.size(), "Concurrent publishes allocate unique codes");
}
}
int main() {
    try { protocol(); threaded_callbacks(); concurrent_admission(); std::cout << "GNS introduction contracts passed\n"; return 0; }
    catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
