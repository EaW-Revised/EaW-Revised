#include "setup_test_support.hpp"
#include "eawr/net/tcp_transport.hpp"
#if defined(EAWR_TEST_GNS)
#include "eawr/net/gns_transport.hpp"
#include "eawr/platform/sim_workers.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <iostream>
#include <thread>

namespace {
using namespace setup_test;
using Clock = std::chrono::steady_clock;
#if defined(EAWR_TEST_GNS)
using TestTransport = net::GnsTransport;
using TestOptions = net::GnsOptions;
#else
using TestTransport = net::TcpTransport;
using TestOptions = net::TcpOptions;
#endif
template<class F> void until(F predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        expect(Clock::now() < deadline, "TCP setup/battle deadline expired");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
std::string loopback(const std::filesystem::path& directory, std::uint32_t workers = 1) {
    TestOptions tcp;
    tcp.build_identity = identity().build;
    auto host_transport = require(TestTransport::host("127.0.0.1", 0, tcp));
    auto client_transport = require(TestTransport::connect("127.0.0.1", host_transport->port(), tcp));
    until([&] {
        static_cast<void>(host_transport->poll()); static_cast<void>(client_transport->poll());
        return host_transport->link(1) == net::LinkState::open && client_transport->link(0) == net::LinkState::open;
    });
#if defined(EAWR_TEST_GNS)
    eawr::platform::ThreadWorkerAdapter executor(workers), client_executor(1);
#else
    static_cast<void>(workers);
    sim::InlineExecutor executor;
    auto& client_executor = executor;
#endif
    static_cast<void>(directory); // No native publication needed for a matching run.
    net::DesyncOptions a, b;
    a.input_delay = offer().input_delay; b.input_delay = offer().input_delay;
    net::DesyncMonitor host_monitor(*host_transport, a), client_monitor(*client_transport, b);
    net::SetupOptions options; options.session.final_tick_count = 300;
    auto selected = offer(); selected.final_tick_count = options.session.final_tick_count;
    auto host = require(net::SetupSession::host(*host_transport, selected, providers(host_monitor, executor), options));
    auto client = require(net::SetupSession::join(*client_transport, providers(client_monitor, client_executor), options));
    std::uint64_t now{};
    until([&] {
        ++now; require(host.pump(now)); require(client.pump(now));
        return host.phase() == net::SetupPhase::ready && client.phase() == net::SetupPhase::ready;
    });
    auto ready_host = require(host.take_result()), ready_client = require(client.take_result());
    expect(ready_host.adapter().completed_tick() == 0 && ready_client.adapter().completed_tick() == 0,
        "TCP readiness must leave both simulations at tick zero");
    expect(ready_host.adapter().state_hash() == ready_client.adapter().state_hash(), "TCP tick-zero agreement");
    submit_attack(ready_host, 1, 2); submit_attack(ready_client, 2, 1);
    until([&] {
        ++now; require(ready_host.session().pump(now)); require(ready_client.session().pump(now));
        return host_monitor.agreed_end() && client_monitor.agreed_end();
    });
    expect(ready_host.adapter().completed_tick() == 300 && ready_client.adapter().completed_tick() == 300,
        "TCP completes 300 ticks");
    expect(ready_host.adapter().state_hash() == ready_client.adapter().state_hash(), "TCP final battle hashes agree");
    std::cout << "TCP host/join -> offer -> accept -> LAN-4 barrier -> 300 battle ticks agreed\n";
    return ready_host.adapter().state_hash();
}
}
int main(int argc, char** argv) {
    try {
#if defined(EAWR_TEST_GNS)
        if (!TestTransport::available()) { std::cout << "GNS disabled: setup test skipped\n"; return 77; }
        expect(argc == 2, "diagnostic output directory required");
        std::string hash;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const auto result = loopback(argv[1], workers);
            if (hash.empty()) hash = result;
            expect(hash == result, "GNS setup/battle hash differs on 1/2/4/8 workers");
        }
        std::cout << "GNS setup and 300-tick battle equal on 1/2/4/8 workers\n";
#else
        expect(argc == 2, "diagnostic output directory required"); static_cast<void>(loopback(argv[1]));
#endif
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
