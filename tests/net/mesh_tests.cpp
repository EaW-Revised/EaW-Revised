#include "setup_test_support.hpp"
#include "eawr/net/tcp_transport.hpp"
#include "eawr/net/lan_credentials.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "fake_transport.hpp"
#include "../../src/net/platform_socket.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>

namespace {
using namespace setup_test;
using Clock = std::chrono::steady_clock;
template<class F> void until(F predicate) {
    const auto deadline = Clock::now() + std::chrono::seconds(15);
    while (!predicate()) {
        expect(Clock::now() < deadline, "mesh test deadline expired");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
// Delay a raw TCP byte stream, preserving order without interpreting controls.
// This reproduces different WAN delivery times for the host's roster streams.
struct ByteDelayRelay {
    struct Chunk { Bytes bytes; std::size_t sent{}; Clock::time_point due; };
    net::socket::Open listener;
    net::socket::Handle upstream{net::socket::invalid}, downstream{net::socket::invalid};
    std::deque<Chunk> to_host, to_client;
    std::uint16_t advertised_port{};
    Bytes bootstrap_bytes;
    std::chrono::milliseconds delay;
    ByteDelayRelay(std::uint16_t port, std::chrono::milliseconds duration)
        : listener(net::socket::listen("127.0.0.1", 0)), upstream(net::socket::connect("127.0.0.1", port)), delay(duration) {
        expect(listener.handle != net::socket::invalid && upstream != net::socket::invalid, "open delayed TCP roster relay");
    }
    ~ByteDelayRelay() { net::socket::close(downstream); net::socket::close(upstream); net::socket::close(listener.handle); }
    void read(net::socket::Handle stream, std::deque<Chunk>& queue, std::chrono::milliseconds latency) {
        if (stream == net::socket::invalid) return;
        std::array<std::uint8_t, 64 * 1024> bytes{};
        const auto received = net::socket::read(stream, bytes);
        if (received.status != net::socket::Status::ready) return;
        Bytes chunk(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(received.count));
        if (!advertised_port || stream != downstream) {
            queue.push_back({std::move(chunk), 0, Clock::now() + latency}); return;
        }
        // Route the synthetic seat's pair endpoint through a persistent relay.
        // Framing is retained across reads, including split bootstrap messages.
        bootstrap_bytes.insert(bootstrap_bytes.end(), chunk.begin(), chunk.end());
        while (bootstrap_bytes.size() >= 4) {
            std::uint32_t size{};
            for (unsigned byte = 0; byte < 4; ++byte) size |= static_cast<std::uint32_t>(bootstrap_bytes[byte]) << (byte * 8);
            expect(size <= 1024 * 1024 + 1, "relay frame bound");
            if (bootstrap_bytes.size() < size + 4) return;
            Bytes framed(bootstrap_bytes.begin(), bootstrap_bytes.begin() + size + 4);
            bootstrap_bytes.erase(bootstrap_bytes.begin(), bootstrap_bytes.begin() + size + 4);
            if (size >= 13 && framed[4] == static_cast<std::uint8_t>(net::TcpMessage::mesh_join))
                for (unsigned byte = 0; byte < 4; ++byte) framed[13 + byte] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(advertised_port) >> (byte * 8));
            queue.push_back({std::move(framed), 0, Clock::now() + latency});
        }
    }
    void write(net::socket::Handle stream, std::deque<Chunk>& queue) {
        if (stream == net::socket::invalid || queue.empty() || queue.front().due > Clock::now()) return;
        auto& chunk = queue.front();
        const auto written = net::socket::write(stream, std::span(chunk.bytes).subspan(chunk.sent));
        if (written.status == net::socket::Status::ready) {
            chunk.sent += written.count;
            if (chunk.sent == chunk.bytes.size()) queue.pop_front();
        }
    }
    void pump() {
        if (downstream == net::socket::invalid) downstream = net::socket::accept(listener.handle);
        read(upstream, to_client, delay); read(downstream, to_host, std::chrono::milliseconds(0));
        write(upstream, to_host); write(downstream, to_client);
    }
    void drop_streams() {
        net::socket::close(downstream); downstream = net::socket::invalid;
        net::socket::close(upstream); upstream = net::socket::invalid;
        to_host.clear(); to_client.clear(); bootstrap_bytes.clear();
    }
};
struct Mesh {
    std::vector<std::unique_ptr<net::TcpTransport>> peers;
    explicit Mesh(std::uint32_t count, bool reverse = false, bool wildcard = false) : peers(count) {
        net::TcpOptions options; options.build_identity = identity().build; options.peers = count;
        options.keepalive = std::chrono::milliseconds(20);
        peers[0] = require(net::TcpTransport::host(wildcard ? "0.0.0.0" : "127.0.0.1", 0, options));
        for (net::PeerIndex position = 1; position < count; ++position) {
            const auto peer = reverse ? count - position : position;
            options.seat = peer;
            peers[peer] = require(net::TcpTransport::connect("127.0.0.1", peers[0]->port(), options));
        }
        until([&] {
            bool open = true;
            for (auto& transport : peers) {
                expect(!transport->poll(), "bootstrap controls never become application frames");
                if (transport->diagnostic()) throw std::runtime_error(transport->diagnostic()->message);
                for (net::PeerIndex peer = 0; peer < count; ++peer)
                    open = open && transport->link(peer) == net::LinkState::open;
            }
            return open;
        });
        for (net::PeerIndex peer = 0; peer < count; ++peer)
            expect(peers[peer]->self() == peer && peers[peer]->peer_count() == count,
                "seat indices do not depend on connection arrival order");
    }
};
void transport(std::uint32_t count, bool wildcard = false) {
    Mesh mesh(count, true, wildcard);
    std::vector<std::vector<Bytes>> delivered(count);
    for (net::PeerIndex from = 0; from < count; ++from) {
        expect(!mesh.peers[from]->send(from, {}), "mesh rejects self destination");
        expect(!mesh.peers[from]->send(count, {}), "mesh rejects out-of-range destination");
        for (net::PeerIndex to = 0; to < count; ++to) if (from != to)
            for (std::uint8_t sequence = 0; sequence < 3; ++sequence)
                require(mesh.peers[from]->send(to, Bytes{static_cast<std::uint8_t>(net::TcpMessage::mesh_join),
                    static_cast<std::uint8_t>(from), static_cast<std::uint8_t>(to), sequence}));
    }
    until([&] {
        std::size_t total{};
        for (net::PeerIndex to = 0; to < count; ++to) {
            while (auto received = mesh.peers[to]->poll()) {
                expect(received->frame.size() == 4 && received->from == received->frame[1]
                    && received->frame[2] == to, "direct stream authenticates application sender index");
                const auto sequence = std::count_if(delivered[to].begin(), delivered[to].end(),
                    [&](const auto& bytes) { return bytes[1] == received->from; });
                expect(received->frame[3] == sequence, "each mesh pair preserves ordered delivery");
                delivered[to].push_back(std::move(received->frame));
            }
            total += delivered[to].size();
        }
        return total == count * (count - 1) * 3;
    });
    until([&] {
        bool measured = true;
        for (auto& peer : mesh.peers) {
            (void)peer->poll();
            for (net::PeerIndex remote = 0; remote < count; ++remote)
                if (remote != peer->self()) measured = measured && peer->link_stats(remote).has_value();
        }
        return measured;
    });
    mesh.peers[0].reset();
    until([&] {
        bool closed = true;
        for (net::PeerIndex peer = 1; peer < count; ++peer) {
            (void)mesh.peers[peer]->poll();
            closed = closed && mesh.peers[peer]->link(0) == net::LinkState::closed;
        }
        return closed;
    });
    require(mesh.peers[1]->send(2, Bytes{42}));
    until([&] {
        (void)mesh.peers[1]->poll();
        const auto frame = mesh.peers[2]->poll();
        return frame && frame->from == 1 && frame->frame == Bytes{42};
    });
    for (net::PeerIndex peer = 1; peer < count; ++peer) mesh.peers[peer]->close();
    until([&] {
        bool closed = true;
        for (net::PeerIndex peer = 1; peer < count; ++peer) {
            (void)mesh.peers[peer]->poll(); closed = closed && mesh.peers[peer]->closed();
        }
        return closed;
    });
}
void admission() {
    net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
    options.seat = 1;
    auto first = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    until([&] { (void)host->poll(); (void)first->poll(); return first->self() == 1; });
    auto duplicate = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    until([&] {
        (void)host->poll(); (void)first->poll(); (void)duplicate->poll();
        return duplicate->link(0) == net::LinkState::closed;
    });
    expect(duplicate->diagnostic() && duplicate->diagnostic()->code == "EAWR-NET-1302",
        "duplicate seat receives a typed refusal without disturbing the existing seat");
    expect(!host->send(1, {}), "host cannot expose a partially formed mesh");
    options.seat = 2;
    auto last = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    until([&] {
        (void)host->poll(); (void)first->poll(); (void)last->poll();
        return host->link(1) == net::LinkState::open && first->link(2) == net::LinkState::open
            && last->link(1) == net::LinkState::open;
    });
    options.peers = 10;
    expect(!net::TcpTransport::host("127.0.0.1", 0, options), "tenth mesh peer is refused");
}
void delayed_roster() {
    net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
    ByteDelayRelay relay(host->port(), std::chrono::milliseconds(300));
    options.seat = 1;
    auto lower = require(net::TcpTransport::connect("127.0.0.1", relay.listener.port, options));
    options.seat = 2;
    auto higher = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    const auto begin = Clock::now();
    until([&] {
        relay.pump();
        (void)host->poll(); (void)lower->poll(); (void)higher->poll();
        for (const auto* peer : {host.get(), lower.get(), higher.get()})
            if (peer->diagnostic()) throw std::runtime_error("delayed roster: " + peer->diagnostic()->message);
        return host->link(1) == net::LinkState::open && lower->link(2) == net::LinkState::open
            && higher->link(1) == net::LinkState::open;
    });
    expect(Clock::now() - begin >= std::chrono::milliseconds(600), "roster relay genuinely holds downstream delivery");
    require(higher->send(1, Bytes{42}));
    until([&] {
        relay.pump(); (void)host->poll(); (void)higher->poll();
        const auto frame = lower->poll(); return frame && frame->from == 2 && frame->frame == Bytes{42};
    });
}
void half_formed_mesh() {
    net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
    options.handshake_timeout = std::chrono::milliseconds(1000);
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
    options.seat = 1; options.mesh_listen = "127.0.0.1"; options.mesh_advertise = "127.0.0.2";
    auto first = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    options.seat = 2; options.mesh_advertise.clear();
    auto last = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    until([&] {
        for (auto* peer : {host.get(), first.get(), last.get()}) {
            expect(!peer->poll(), "half-formed mesh exposes no application frames");
            for (net::PeerIndex remote = 0; remote < 3; ++remote) if (remote != peer->self())
                expect(peer->link(remote) != net::LinkState::open, "one missing pair cannot expose a partially open mesh");
        }
        return host->closed() && first->closed() && last->closed();
    });
    expect(host->diagnostic() && first->diagnostic() && last->diagnostic(), "every participant fails closed and releases its sockets");
    expect(!host->send(1, {}), "failed mesh cannot send application data");
}
void early_hash_bounds() {
    for (const bool bytes_bound : {false, true}) {
        eawr::testing::FakeNetwork network(3);
        eawr::testing::FakeTransport source(network, 1), sink(network, 2);
        sim::InlineExecutor executor;
        net::DesyncMonitor source_monitor(source), sink_monitor(sink);
        auto simulation = require(tactical::TacticalSession::create(setup(), content()));
        require(source_monitor.record(0, simulation.state_sha256(), simulation.canonical_state_bytes(), {}, false));
        const auto hash = network.inbox[2].front().received.frame;
        network.inbox[2].clear();
        Bytes frame = hash;
        // Padding tests the ingress byte budget before an offer makes full hash
        // decoding possible; the kind and sender remain the real hash envelope.
        if (bytes_bound) frame.resize(net::max_batch_bytes);
        const auto count = bytes_bound ? 8U : 8192U;
        int builds{};
        net::SetupOptions options; options.session.max_frames_per_pump = count;
        auto session = require(net::SetupSession::join(sink, providers(sink_monitor, executor, identity(), false, &builds), options));
        for (unsigned index = 0; index < count; ++index) network.inbox[2].push_back({0, {1, frame}});
        require(session.pump(0));
        expect(session.phase() == net::SetupPhase::awaiting_offer && builds == 0, "exact early-hash bound is held without building a world");
        network.inbox[2].push_back({0, {1, hash}});
        const auto refused = session.pump(1);
        expect(!refused && session.phase() == net::SetupPhase::refused && builds == 0,
            "one further early hash exceeds the count or byte bound and fails closed before world construction");
    }
}
void unbound_admission_timeout() {
    net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
    options.handshake_timeout = std::chrono::milliseconds(500);
    options.keepalive = std::chrono::milliseconds(20);
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
    auto legacy = options; legacy.peers = 2;
    std::vector<std::unique_ptr<net::TcpTransport>> unbound;
    for (unsigned attempt = 0; attempt < options.peers; ++attempt)
        unbound.push_back(require(net::TcpTransport::connect("127.0.0.1", host->port(), legacy)));
    until([&] {
        (void)host->poll();
        for (auto& peer : unbound) (void)peer->poll();
        return std::all_of(unbound.begin(), unbound.end(), [](const auto& peer) { return peer->link(0) == net::LinkState::open; });
    });
    until([&] {
        (void)host->poll();
        for (auto& peer : unbound) (void)peer->poll(); // Healthy TCP pings cannot retain an unbound seat forever.
        return std::all_of(unbound.begin(), unbound.end(), [](const auto& peer) { return peer->link(0) == net::LinkState::closed; });
    });
    for (const auto& peer : unbound)
        expect(peer->diagnostic() && peer->diagnostic()->code == "EAWR-NET-1301", "missing mesh admission gets a typed deadline refusal");
    expect(!host->diagnostic() && host->link(1) == net::LinkState::connecting,
        "unbound attempts do not end the waiting host");
    options.seat = 1;
    auto valid = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    until([&] { (void)host->poll(); (void)valid->poll(); return valid->self() == 1; });
    expect(!valid->diagnostic(), "valid admission succeeds after all pending slots were occupied by unbound attempts");
}
void wrong_pair_secret() {
    net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
    options.seat = 1;
    auto first = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    options.seat = 2;
    auto last = require(net::TcpTransport::connect("127.0.0.1", host->port(), options));
    until([&] {
        (void)host->poll(); (void)first->poll(); (void)last->poll();
        return first->self() == 1 && last->self() == 2;
    });
    // Hold seat two before it completes its pair handshake. Seat one has received
    // its roster, so an outsider cannot claim seat two with an unrelated secret.
    for (unsigned round = 0; round < 30; ++round) {
        (void)host->poll(); (void)first->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto raw = net::socket::connect("127.0.0.1", first->port());
    expect(raw != net::socket::invalid, "open an unbound mesh pair attempt");
    struct Guard { net::socket::Handle stream; ~Guard() { net::socket::close(stream); } } guard{raw};
    Bytes wire;
    const auto word = [](Bytes& bytes, std::uint32_t value) {
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto envelope = [&](const Bytes& body) {
        word(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end());
    };
    Bytes hello{static_cast<std::uint8_t>(net::TcpMessage::hello), 'E', 'A', 'W', 'R'};
    word(hello, options.protocol_version); word(hello, static_cast<std::uint32_t>(options.build_identity.size()));
    hello.insert(hello.end(), options.build_identity.begin(), options.build_identity.end());
    word(hello, options.max_frame_size); hello.push_back(1); envelope(hello);
    Bytes accept{static_cast<std::uint8_t>(net::TcpMessage::accept)}; word(accept, 1); envelope(accept);
    Bytes pair{static_cast<std::uint8_t>(net::TcpMessage::mesh_pair)};
    pair.resize(17, 0); word(pair, 2); word(pair, 1); envelope(pair);
    std::size_t sent{};
    Bytes received;
    until([&] {
        (void)host->poll(); (void)first->poll();
        if (sent < wire.size()) {
            const auto written = net::socket::write(raw, std::span(wire).subspan(sent));
            if (written.status == net::socket::Status::ready) sent += written.count;
        }
        std::array<std::uint8_t, 2048> bytes{};
        const auto read = net::socket::read(raw, bytes);
        if (read.status == net::socket::Status::ready) received.insert(received.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read.count));
        const std::string refusal = "EAWR-NET-1302";
        return std::search(received.begin(), received.end(), refusal.begin(), refusal.end()) != received.end();
    });
    expect(!first->diagnostic(), "wrong pair secret does not evict a legitimate assigned seat");
    until([&] {
        (void)host->poll(); (void)first->poll(); (void)last->poll();
        return first->link(2) == net::LinkState::open && last->link(1) == net::LinkState::open;
    });
}
void early_peer_hash() {
    eawr::testing::FakeNetwork network(3);
    sim::InlineExecutor executor;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<net::SetupSession> sessions;
    network.fault = [](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        eawr::testing::FakeDelivery delay;
        if (from == 0 && to == 2 && frame.front() == net::setup_offer_kind) delay.ordered_delay = 10;
        return delay;
    };
    bool early{};
    network.before_send = [&](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        if (from == 1 && to == 2 && frame.front() == net::desync_combined_kind && sessions.size() == 3
            && sessions[2].phase() == net::SetupPhase::awaiting_offer) early = true;
    };
    auto selected = offer(); selected.seats.push_back({2, sim::SeatId{15}, 2});
    for (net::PeerIndex peer = 0; peer < 3; ++peer) {
        transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, peer));
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back()));
        auto provider = providers(*monitors.back(), executor);
        sessions.push_back(peer == 0
            ? require(net::SetupSession::host(*transports.back(), selected, std::move(provider)))
            : require(net::SetupSession::join(*transports.back(), std::move(provider))));
    }
    for (unsigned round = 0; round < 100; ++round) {
        ++network.round;
        for (auto& session : sessions) require(session.pump(network.round));
        if (std::all_of(sessions.begin(), sessions.end(), [](const auto& session) { return session.phase() == net::SetupPhase::ready; })) {
            expect(early, "independent client hash precedes the deliberately delayed host offer");
            return;
        }
    }
    throw std::runtime_error("early mesh peer hash blocked setup agreement");
}
std::vector<std::string> battle(std::uint32_t count, std::size_t configuration, bool coop) {
    Mesh mesh(count);
    constexpr std::array<std::size_t, 4> workers{1, 2, 4, 8};
    std::vector<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>> executors;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::vector<net::SetupSession> setup_sessions;
    auto selected = offer(); selected.seats.clear(); selected.final_tick_count = 50;
    auto initial = setup();
    if (coop) initial.skirmish->slots[1].human = false;
    const auto sides = count == 9 && !coop ? 3U : 2U;
    if (sides == 3) initial.skirmish->slots[2].human = true;
    selected.setup = require(net::encode_skirmish_setup(initial));
    for (net::PeerIndex peer = 0; peer < count; ++peer)
        selected.seats.push_back({peer, sim::SeatId{peer + 1}, coop ? 1U : peer % sides + 1});
    net::SetupOptions options; options.session.final_tick_count = selected.final_tick_count;
    for (net::PeerIndex peer = 0; peer < count; ++peer) {
        executors.push_back(std::make_unique<eawr::platform::ThreadWorkerAdapter>(workers[(configuration + peer) % workers.size()]));
        net::DesyncOptions monitor_options; monitor_options.exchange_every = 2;
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*mesh.peers[peer], monitor_options));
        auto provider = providers(*monitors[peer], *executors[peer]);
        setup_sessions.push_back(peer == 0
            ? require(net::SetupSession::host(*mesh.peers[peer], selected, std::move(provider), options))
            : require(net::SetupSession::join(*mesh.peers[peer], std::move(provider), options)));
    }
    std::uint64_t now{};
    until([&] {
        ++now; bool ready = true;
        for (auto& session : setup_sessions) {
            require(session.pump(now)); ready = ready && session.phase() == net::SetupPhase::ready;
        }
        return ready;
    });
    std::vector<net::SetupResult> sessions;
    std::vector<std::vector<std::string>> hashes(count);
    std::vector<unsigned> departures(count);
    for (net::PeerIndex peer = 0; peer < count; ++peer) {
        sessions.push_back(require(setup_sessions[peer].take_result()));
        const auto seats = selected.seats;
        sessions.back().session().enable_departures([seats](sim::SeatCommandKey key) {
            const auto found = std::find_if(seats.begin(), seats.end(), [&](const auto& seat) { return seat.seat == key.seat; });
            const tactical::SeatBinding binding{found->seat, found->player, true, false};
            return net::encode_tactical_input(key, binding, {{}, tactical::QuitPayload{}});
        });
        const auto bound = selected.seats[peer];
        const tactical::SeatBinding binding{bound.seat, bound.player, true, false};
        require(sessions.back().session().submit([binding, peer](sim::SeatCommandKey key) {
            return net::encode_tactical_input(key, binding,
                {{binding.player}, tactical::MovePayload{{units(10 + peer), units(20), {}}}});
        }));
    }
    until([&] {
        ++now; bool complete = true;
        for (net::PeerIndex peer = 0; peer < count; ++peer) {
            if (sessions[peer].adapter().completed_tick() < 20) {
                auto pumped = require(sessions[peer].session().pump(now));
                if (pumped.completed) hashes[peer].push_back(pumped.completed->state_sha256);
            }
            complete = complete && sessions[peer].adapter().completed_tick() == 20;
        }
        return complete;
    });
    mesh.peers.back()->cancel(); // One controller departs; its owner still has another seat.
    until([&] {
        ++now; bool complete = true;
        for (net::PeerIndex peer = 0; peer + 1 < count; ++peer) {
            auto pumped = require(sessions[peer].session().pump(now));
            if (pumped.completed) hashes[peer].push_back(pumped.completed->state_sha256);
            departures[peer] += static_cast<unsigned>(pumped.departure_events.size());
            complete = complete && monitors[peer]->agreed_end();
        }
        return complete;
    });
    for (net::PeerIndex peer = 0; peer + 1 < count; ++peer) {
        expect(departures[peer] == 1, "survivors agree one seat departure exactly once");
        expect(hashes[peer].size() == 50 && hashes[peer] == hashes[0], "all survivor hashes agree at every tick");
        auto& adapter = dynamic_cast<net::TacticalSessionAdapter&>(sessions[peer].adapter());
        expect(adapter.simulation().record().commands.size() == count, "shared-owner departure preserves the faction's command stream");
        expect(adapter.simulation().departure_controls().empty(), "remaining controller keeps the faction in the battle");
    }
    return hashes[0];
}
void unproved_repair_controls() {
    for (const auto injected : {net::TcpMessage::mesh_open, net::TcpMessage::mesh_pair}) {
        net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
        options.keepalive = std::chrono::milliseconds(20);
        std::array<std::unique_ptr<net::TcpTransport>, 3> peers;
        peers[0] = require(net::TcpTransport::host("127.0.0.1", 0, options));
        ByteDelayRelay bootstrap(peers[0]->port(), std::chrono::milliseconds(0));
        options.seat = 1;
        peers[1] = require(net::TcpTransport::connect("127.0.0.1", bootstrap.listener.port, options));
        ByteDelayRelay pair(peers[1]->port(), std::chrono::milliseconds(0));
        bootstrap.advertised_port = pair.listener.port;
        options.seat = 2;
        peers[2] = require(net::TcpTransport::connect("127.0.0.1", peers[0]->port(), options));
        until([&] {
            bootstrap.pump(); pair.pump();
            bool complete = true;
            for (auto& peer : peers) {
                expect(!peer->poll(), "relayed bootstrap stays below application frames");
                for (net::PeerIndex remote = 0; remote < peers.size(); ++remote) complete = complete && peer->link(remote) == net::LinkState::open;
            }
            return complete;
        });
        net::TcpRejoinAuth auth; auth.match_identity.fill(42); auth.epoch = 1;
        auth.seat_tokens.resize(3);
        auth.seat_tokens = pair_keys(0, 3);
        // The relay keeps its endpoint bound throughout. Only seat one's edge to
        // seat two drops; both existing host edges must survive the attack.
        pair.drop_streams();
        const auto word = [](Bytes& bytes, std::uint32_t value) {
            const auto start = bytes.size(); bytes.resize(start + 4);
            for (unsigned byte = 0; byte < 4; ++byte) bytes.at(start + byte) = static_cast<std::uint8_t>(value >> (byte * 8));
        };
        Bytes wire;
        const auto envelope = [&](const Bytes& body) {
            word(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end());
        };
        Bytes hello{static_cast<std::uint8_t>(net::TcpMessage::hello), 'E', 'A', 'W', 'R'};
        word(hello, options.protocol_version); word(hello, static_cast<std::uint32_t>(options.build_identity.size()));
        hello.insert(hello.end(), options.build_identity.begin(), options.build_identity.end());
        word(hello, options.max_frame_size); hello.push_back(0); envelope(hello);
        Bytes accepted{static_cast<std::uint8_t>(net::TcpMessage::accept)}; word(accepted, 1); envelope(accepted);
        envelope(Bytes{static_cast<std::uint8_t>(injected)});
        std::size_t sent{};
        until([&] {
            bootstrap.pump();
            for (auto& peer : peers) {
                expect(!peer->poll(), "unproved mesh repair exposes no application data");
                auto local = auth; local.seat_tokens = pair_keys(peer->self(), 3);
                require(peer->repair_closed_links(local));
            }
            if (pair.downstream == net::socket::invalid) pair.downstream = net::socket::accept(pair.listener.handle);
            if (pair.downstream != net::socket::invalid && sent < wire.size()) {
                const auto written = net::socket::write(pair.downstream, std::span(wire).subspan(sent));
                if (written.status == net::socket::Status::ready) sent += written.count;
            }
            return sent == wire.size();
        });
        for (unsigned round = 0; round < 30; ++round) {
            bootstrap.pump();
            for (auto& peer : peers) {
                expect(!peer->poll(), "injected bootstrap control stays quarantined");
            }
            for (const auto peer : {1U, 2U})
                expect(peers[0]->link(peer) == net::LinkState::open && peers[peer]->link(0) == net::LinkState::open,
                    "unproved control cannot fail healthy survivor edges");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        expect(peers[2]->link(1) != net::LinkState::open, "physical hello cannot prove the replacement seat");
        const Bytes payload{17, 19}; require(peers[0]->send(2, payload));
        until([&] {
            bootstrap.pump(); (void)peers[0]->poll(); auto frame = peers[2]->poll();
            if (!frame) return false;
            expect(frame->from == 0 && frame->frame == payload, "healthy survivor edge carries data after the injection");
            return true;
        });
        pair.drop_streams();
        pair.upstream = net::socket::connect("127.0.0.1", peers[1]->port());
        expect(pair.upstream != net::socket::invalid, "relay reconnects to the rightful replacement");
        until([&] {
            bootstrap.pump(); pair.pump();
            bool complete = true;
            for (net::PeerIndex peer = 0; peer < peers.size(); ++peer) {
                (void)peers[peer]->poll(); auto local = auth; local.seat_tokens = pair_keys(peer, 3); require(peers[peer]->repair_closed_links(local));
                for (net::PeerIndex other = 0; other < peers.size(); ++other)
                    if (other != peer) complete = complete && peers[peer]->link(other) == net::LinkState::open;
            }
            return complete;
        });
        bootstrap.pump(); pair.pump();
        for (auto& peer : peers) { (void)peer->poll(); expect(!peer->listening(), "rightful recovery closes all listeners after stranger leaves"); }
    }
}
void candidate_queue_refusal() {
    net::TcpOptions options; options.build_identity = identity().build; options.peers = 3;
    options.max_frame_size = 0; options.queue_bytes = 5; options.queue_frames = 4;
    auto host = require(net::TcpTransport::host("127.0.0.1", 0, options));
    struct Raw {
        net::socket::Handle stream{net::socket::invalid};
        ~Raw() { net::socket::close(stream); }
    } first, second;
    first.stream = net::socket::connect("127.0.0.1", host->port());
    second.stream = net::socket::connect("127.0.0.1", host->port());
    expect(first.stream != net::socket::invalid && second.stream != net::socket::invalid, "two raw admission candidates connect");
    const auto word = [](Bytes& bytes, std::uint32_t value) {
        const auto start = bytes.size(); bytes.resize(start + 4);
        for (unsigned byte = 0; byte < 4; ++byte) bytes.at(start + byte) = static_cast<std::uint8_t>(value >> (byte * 8));
    };
    Bytes wire;
    const auto envelope = [&](const Bytes& body) {
        word(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end());
    };
    Bytes hello{static_cast<std::uint8_t>(net::TcpMessage::hello), 'E', 'A', 'W', 'R'};
    word(hello, options.protocol_version); word(hello, static_cast<std::uint32_t>(options.build_identity.size()));
    hello.insert(hello.end(), options.build_identity.begin(), options.build_identity.end());
    word(hello, options.max_frame_size); hello.push_back(1); envelope(hello);
    Bytes accepted{static_cast<std::uint8_t>(net::TcpMessage::accept)}; word(accepted, 1); envelope(accepted);
    const auto write = [&](net::socket::Handle stream) {
        std::size_t sent{};
        until([&] {
            expect(!host->poll(), "raw candidates stay below the application layer");
            const auto result = net::socket::write(stream, std::span(wire).subspan(sent));
            if (result.status == net::socket::Status::ready) sent += result.count;
            return sent == wire.size();
        });
    };
    write(first.stream); write(second.stream);
    // Five bytes allow ordinary tiny application queues, but the mesh assignment
    // needs thirteen. Overflow must refuse only this candidate, keeping the
    // second pending candidate and admission owner alive.
    wire.clear(); Bytes join{static_cast<std::uint8_t>(net::TcpMessage::mesh_join)};
    word(join, 3); word(join, 1); word(join, 43210); word(join, 0); envelope(join); write(first.stream);
    Bytes response;
    const std::string refusal = "Mesh candidate control queue capacity exceeded";
    until([&] {
        expect(!host->poll() && !host->closed() && host->listening() && !host->diagnostic(),
            "candidate control overflow never clears the admission owner or other pending candidates");
        std::array<std::uint8_t, 2048> bytes{};
        const auto read = net::socket::read(first.stream, bytes);
        if (read.status == net::socket::Status::ready) response.insert(response.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read.count));
        return std::search(response.begin(), response.end(), refusal.begin(), refusal.end()) != response.end();
    });
    wire.clear(); envelope(Bytes{static_cast<std::uint8_t>(net::TcpMessage::ping), 90, 91, 92, 93}); write(second.stream);
    response.clear(); const Bytes pong{static_cast<std::uint8_t>(net::TcpMessage::pong), 90, 91, 92, 93};
    until([&] {
        expect(!host->poll() && host->listening() && !host->diagnostic(), "later pending candidate survives the earlier refusal");
        std::array<std::uint8_t, 2048> bytes{};
        const auto read = net::socket::read(second.stream, bytes);
        if (read.status == net::socket::Status::ready) response.insert(response.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read.count));
        return std::search(response.begin(), response.end(), pong.begin(), pong.end()) != response.end();
    });
    host->cancel();
}

void occupied_repair_listener() {
    Mesh mesh(3);
    const auto port = mesh.peers[0]->port();
    mesh.peers[2]->cancel();
    until([&] {
        (void)mesh.peers[0]->poll(); (void)mesh.peers[1]->poll();
        return mesh.peers[0]->link(2) == net::LinkState::closed && !mesh.peers[0]->listening();
    });
    const auto listener = net::socket::listen("127.0.0.1", port);
    expect(listener.handle != net::socket::invalid, "occupy retained mesh repair port");
    struct Guard { net::socket::Handle handle; ~Guard() { net::socket::close(handle); } } guard{listener.handle};
    net::TcpRejoinAuth auth; auth.match_identity.fill(42); auth.epoch = 1;
    auth.seat_tokens.resize(3);
    auth.seat_tokens = pair_keys(0, 3);
    const auto result = mesh.peers[0]->repair_closed_links(auth);
    expect(!result && result.error().code == "EAWR-NET-1014", "mesh-only listener failure pins its reconnect diagnostic");
}
void pairwise_host_repair_isolation() {
    Mesh mesh(3);
    auto agreed = offer(); agreed.recovery.reconnect = true;
    agreed.seats.push_back({2, sim::SeatId{12}, 1});
    std::array<net::LanReconnectCredentials, 3> providers;
    std::array<net::ReconnectTokens, 3> keys;
    keys[0] = require(providers[0].provide(agreed, *mesh.peers[0]));
    until([&] {
        for (net::PeerIndex peer = 0; peer < 3; ++peer) while (auto frame = mesh.peers[peer]->poll())
            expect(require(providers[peer].receive(*frame, *mesh.peers[peer])), "real provider consumes mesh credential frames");
        for (net::PeerIndex peer = 1; peer < 3; ++peer) if (keys[peer].empty()) {
            auto supplied = providers[peer].provide(agreed, *mesh.peers[peer]);
            if (supplied) keys[peer] = std::move(supplied).value();
        }
        return providers[0].delivered();
    });
    expect(keys[1][0] != keys[0][2] && keys[1][2] != keys[0][2], "peer one has neither credential for the host/seat-two pair");
    mesh.peers[2]->cancel();
    until([&] { (void)mesh.peers[0]->poll(); (void)mesh.peers[1]->poll(); return mesh.peers[0]->link(2) == net::LinkState::closed; });
    net::TcpRejoinAuth auth{eawr::core::sha256(require(net::encode_setup_offer(agreed))), keys[0], 1};
    require(mesh.peers[0]->repair_closed_links(auth));
    struct Raw {
        net::socket::Handle stream{net::socket::invalid};
        ~Raw() { net::socket::close(stream); }
    } attacker;
    attacker.stream = net::socket::connect("127.0.0.1", mesh.peers[0]->port());
    expect(attacker.stream != net::socket::invalid, "scripted admitted peer dials seat-two replacement");
    const auto word = [](Bytes& bytes, std::uint32_t value) {
        for (unsigned byte = 0; byte < 4; ++byte) bytes.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    };
    const auto framed = [&](const Bytes& body) { Bytes wire; word(wire, static_cast<std::uint32_t>(body.size())); wire.insert(wire.end(), body.begin(), body.end()); return wire; };
    const auto write = [&](const Bytes& wire) {
        std::size_t sent{};
        until([&] {
            expect(!mesh.peers[0]->poll() && !mesh.peers[1]->poll(), "unproved replacement exposes no application bytes");
            const auto result = net::socket::write(attacker.stream, std::span(wire).subspan(sent));
            expect(result.status == net::socket::Status::ready || result.status == net::socket::Status::pending, "scripted replacement write remains connected");
            if (result.status == net::socket::Status::ready) sent += result.count;
            return sent == wire.size();
        });
    };
    net::TcpOptions options; options.build_identity = identity().build;
    Bytes hello{static_cast<std::uint8_t>(net::TcpMessage::hello), 'E', 'A', 'W', 'R'};
    word(hello, options.protocol_version); word(hello, static_cast<std::uint32_t>(options.build_identity.size()));
    hello.insert(hello.end(), options.build_identity.begin(), options.build_identity.end());
    word(hello, options.max_frame_size); hello.push_back(1); write(framed(hello));
    Bytes accepted{static_cast<std::uint8_t>(net::TcpMessage::accept)}; word(accepted, 1); write(framed(accepted));
    // Reproduce the host's all-zero mesh-pair gate from the review. Its HMAC
    // guard must still refuse peer one claiming seat two with peer one's own key.
    Bytes pair{static_cast<std::uint8_t>(net::TcpMessage::mesh_pair)}; pair.resize(17, 0);
    word(pair, 2); word(pair, 0); write(framed(pair));
    Bytes incoming, challenge;
    bool refused{};
    const auto read = [&] {
        std::array<std::uint8_t, 4096> buffer{};
        const auto result = net::socket::read(attacker.stream, buffer);
        if (result.status == net::socket::Status::ready) incoming.insert(incoming.end(), buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(result.count));
        while (incoming.size() >= 4) {
            std::uint32_t length{};
            for (unsigned byte = 0; byte < 4; ++byte) length |= static_cast<std::uint32_t>(incoming[byte]) << (8U * byte);
            expect(length <= 4096, "bounded scripted replacement response");
            if (incoming.size() < 4U + length) break;
            Bytes body(incoming.begin() + 4, incoming.begin() + static_cast<std::ptrdiff_t>(4U + length));
            incoming.erase(incoming.begin(), incoming.begin() + static_cast<std::ptrdiff_t>(4U + length));
            if (body.size() == 79 && body[0] == static_cast<std::uint8_t>(net::TcpMessage::data) && body[1] == net::rejoin_hello_kind)
                challenge.assign(body.begin() + 1, body.end());
            if (!body.empty() && body[0] == static_cast<std::uint8_t>(net::TcpMessage::refuse)) {
                const std::string code = "EAWR-NET-1016";
                expect(std::search(body.begin(), body.end(), code.begin(), code.end()) != body.end(), "wrong pair key gets the authentication refusal");
                refused = true;
            }
        }
    };
    until([&] { (void)mesh.peers[0]->poll(); (void)mesh.peers[1]->poll(); read(); return !challenge.empty(); });
    expect(challenge[9] == net::rejoin_link_challenge, "host challenges the claimed replacement seat");
    net::RejoinNonce nonce{}, identity_bytes{};
    std::copy_n(challenge.begin() + 14, 32, identity_bytes.begin()); std::copy_n(challenge.begin() + 46, 32, nonce.begin());
    const auto tag = net::rejoin_authenticator(keys[1][2], identity_bytes, 3, 1, nonce, false);
    Bytes response{static_cast<std::uint8_t>(net::TcpMessage::data)};
    challenge[9] = net::rejoin_link_response;
    response.insert(response.end(), challenge.begin(), challenge.end()); response.insert(response.end(), tag.begin(), tag.end());
    write(framed(response));
    until([&] { (void)mesh.peers[0]->poll(); (void)mesh.peers[1]->poll(); read(); return refused; });
    expect(mesh.peers[0]->link(2) != net::LinkState::open && mesh.peers[0]->listening(), "an admitted peer cannot occupy another held seat");
    expect(mesh.peers[0]->link(1) == net::LinkState::open && mesh.peers[1]->link(0) == net::LinkState::open, "impersonation preserves the rightful survivor link");
}
void repaired_mesh() {
    Mesh mesh(3);
    net::TcpRejoinAuth auth; auth.match_identity.fill(42);
    auth.seat_tokens.resize(3); auth.seat_tokens = pair_keys(0, 3);
    for (const auto returning : {2U, 0U}) {
        ++auth.epoch;
        std::cerr << "repair mesh seat " << returning << " epoch " << auth.epoch << std::endl;
        auto returning_auth = auth; returning_auth.seat_tokens = pair_keys(returning, 3);
        require(mesh.peers[returning]->restart_links(returning_auth));
        bool early_closed{}, early_sent{}, early_received{};
        const net::PeerIndex early_source = returning == 2 ? 2U : 1U;
        const net::PeerIndex early_target = returning == 2 ? 1U : 0U;
        const Bytes early_payload{42, static_cast<std::uint8_t>(returning)};
        until([&] {
            bool complete = true;
            // Visit the higher dialer first so it can flush application data
            // beside its final proof before the lower candidate is installed.
            for (const auto peer : {2U, 1U, 0U}) {
                auto& stream = *mesh.peers[peer];
                while (auto received = stream.poll()) {
                    expect(peer == early_target && received->from == early_source && received->frame == early_payload
                        && !early_received, "proof-tail application frame arrives once with the retained sender");
                    early_received = true;
                }
                auto local = auth; local.seat_tokens = pair_keys(peer, 3);
                require(stream.repair_closed_links(local));
                expect(stream.self() == peer, "closed-link repair preserves the original seat");
                if (peer == early_source && stream.link(early_target) != net::LinkState::open) early_closed = true;
                if (early_closed && !early_sent && peer == early_source && stream.link(early_target) == net::LinkState::open) {
                    require(stream.send(early_target, early_payload)); early_sent = true;
                    expect(!stream.poll(), "flush proof and early application data before the target pump");
                }
                for (net::PeerIndex other = 0; other < mesh.peers.size(); ++other)
                    if (other != peer) complete = complete && stream.link(other) == net::LinkState::open;
            }
            return complete && early_received;
        });
        for (auto& stream : mesh.peers) {
            (void)stream->poll();
            expect(!stream->listening(), "completed mesh repair closes admission listeners");
        }
        const Bytes payload{9, 8, 7};
        require(mesh.peers[returning]->send(1, payload));
        until([&] {
            for (auto& stream : mesh.peers) (void)stream->link(stream->self());
            (void)mesh.peers[returning]->poll();
            auto received = mesh.peers[1]->poll();
            if (!received) return false;
            expect(received->from == returning && received->frame == payload, "repaired stream retains its sender identity");
            return true;
        });
    }
}

}
int main() {
    try {
        admission();
        unproved_repair_controls();
        candidate_queue_refusal();
        repaired_mesh();
        occupied_repair_listener();
        pairwise_host_repair_isolation();
        delayed_roster();
        half_formed_mesh();
        early_hash_bounds();
        unbound_admission_timeout();
        wrong_pair_secret();
        early_peer_hash();
        for (const auto peers : {3U, 4U, 8U, 9U}) transport(peers);
        transport(3, true); // Viewer-style wildcard/dual-stack host accepts IPv4 peer endpoints.
        for (const auto peers : {3U, 4U, 9U}) for (const auto coop : {false, true}) {
            std::vector<std::string> baseline;
            for (std::size_t configuration = 0; configuration < 4; ++configuration) {
                auto hashes = battle(peers, configuration, coop);
                if (configuration == 0) baseline = hashes;
                else expect(hashes == baseline, "mesh battles and departure agree across 1/2/4/8 worker assignments");
            }
        }
        std::cout << "Mesh: 3/4/8/9-peer direct streams, stable seats, RTT and host-free delivery; 3/4/9-peer coop and two/three-side versus setup/battle/departure equal on 1/2/4/8 workers\n";
        return 0;
    } catch (const std::exception& failure) { std::cerr << failure.what() << '\n'; return 1; }
}
