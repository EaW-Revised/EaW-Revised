#include "eawr/net/gns_transport.hpp"
#include "../../src/net/platform_socket.hpp"
#include "../../src/net/gns_frame_codec.hpp"

#if EAWR_HAS_GNS
#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingcustomsignaling.h>
#endif

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Test-only loopback UDP IPC carries registration and opaque signals, never
// game frames. The parent owns the real in-process introduction implementation;
// each child owns a separate standalone GNS identity and real ICE data path.
namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
std::atomic<std::int64_t> broker_time_ms{};
Clock::time_point broker_now() { return Clock::time_point(std::chrono::duration_cast<Clock::duration>(
    std::chrono::milliseconds(broker_time_ms.load()))); }
void expect(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
void expect_clean(const GnsTransport& transport, const char* reason) {
    if (const auto diagnostic = transport.diagnostic())
        throw std::runtime_error(std::string(reason) + " [" + diagnostic->code + ": " + diagnostic->message + "]");
}
template<class T> T require(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}
eawr::core::Diagnostic failure(const char* message) {
    eawr::core::Diagnostic diagnostic; diagnostic.code = "EAWR-NET-1517"; diagnostic.message = message; return diagnostic;
}
struct Udp {
    // LAN discovery intentionally shares ports; private IPC must not. A shared
    // ephemeral binding can deliver registration/replies to another role.
    socket::Open binding{eawr::net::socket::udp_open("127.0.0.1", 0, false)};
    Udp() { expect(binding.handle != eawr::net::socket::invalid && binding.port != 0, "IPC UDP open"); }
    ~Udp() { eawr::net::socket::close(binding.handle); }
    bool send(std::uint16_t port, std::span<const std::uint8_t> bytes) const {
        return eawr::net::socket::udp_send(binding.handle, eawr::net::socket::ipv4_loopback, port, bytes);
    }
    std::optional<Bytes> poll() const {
        std::array<std::uint8_t, 64000> bytes{};
        const auto read = eawr::net::socket::udp_receive(binding.handle, bytes);
        if (read.status == eawr::net::socket::Status::pending) return {};
        expect(read.status == eawr::net::socket::Status::ready && read.from == eawr::net::socket::ipv4_loopback, "IPC receive");
        return Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(read.count));
    }
};
void text(Bytes& bytes, const std::string& value) {
    expect(value.size() < 128, "IPC string capacity"); bytes.push_back(static_cast<std::uint8_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}
std::string text(std::span<const std::uint8_t>& bytes) {
    expect(!bytes.empty() && bytes.front() < 128, "IPC string prefix");
    const auto size = bytes.front(); bytes = bytes.subspan(1); expect(bytes.size() >= size, "IPC string length");
    std::string value(reinterpret_cast<const char*>(bytes.data()), size); bytes = bytes.subspan(size); return value;
}
void broker() {
    Udp udp;
    const auto duplicate = socket::udp_open("127.0.0.1", udp.binding.port, false);
    if (duplicate.handle != socket::invalid) socket::close(duplicate.handle);
    expect(duplicate.handle == socket::invalid, "IPC binding must reject duplicate endpoints");
    InProcessGnsIntroduction service(broker_now);
    std::array<std::uint16_t, 2> ports{};
    std::array<std::shared_ptr<GnsSignallingEndpoint>, 2> endpoints;
    std::array<bool, 2> finished{};
    std::array<unsigned, 2> forwarded{};
    std::cout << udp.binding.port << '\n' << std::flush;
    const auto deadline = Clock::now() + 100s;
    while (!finished[0] || !finished[1]) {
        expect(Clock::now() < deadline, "Introduced broker deadline");
        while (auto packet = udp.poll()) {
            expect(packet->size() >= 2 && (*packet)[1] < 2, "IPC role");
            const auto role = (*packet)[1];
            if ((*packet)[0] == 0) {
                expect(packet->size() == 4, "IPC registration");
                ports[role] = static_cast<std::uint16_t>((*packet)[2] | (static_cast<unsigned>((*packet)[3]) << 8));
                expect(ports[role] != udp.binding.port && (!ports[1 - role] || ports[role] != ports[1 - role]),
                    "IPC endpoints must have distinct ports");
                if (ports[0] && ports[1] && !endpoints[0]) {
                    auto published = require(service.publish("introduced-host", 1s));
                    endpoints[0] = published.endpoint;
                    endpoints[1] = require(service.join(published.join_code, "introduced-client"));
                }
                if (endpoints[0]) for (std::uint8_t peer = 0; peer < 2; ++peer) {
                    const auto rendezvous = endpoints[peer]->rendezvous();
                    Bytes reply{1, peer}; text(reply, rendezvous.session); text(reply, rendezvous.local_identity);
                    text(reply, rendezvous.remote_identity); expect(udp.send(ports[peer], reply), "IPC rendezvous reply");
                }
            } else if ((*packet)[0] == 2) {
                expect(endpoints[role] && packet->size() > 2, "IPC signal registration");
                require(endpoints[role]->send(std::span<const std::uint8_t>(*packet).subspan(2)));
            } else if ((*packet)[0] == 3) finished[role] = true;
            else if ((*packet)[0] == 4) {
                // Raw client received host acceptance but deliberately withholds
                // its ACK until host poll observes the injected expiry failure.
                expect(role == 1, "Expiry trigger role");
                expect(udp.send(ports[0], Bytes{5, 0}), "IPC expiry trigger");
            } else if ((*packet)[0] == 6) {
                expect(role == 0, "Expiry observation role");
                expect(udp.send(ports[1], Bytes{6, 1}), "IPC expiry observed");
            } else if ((*packet)[0] == 8) {
                expect(role == 0, "Retry observation role");
                expect(udp.send(ports[1], Bytes{8, 1}), "IPC retry observed");
            } else if ((*packet)[0] == 9) {
                expect(role == 0 && packet->size() == 2, "Verified host refusal role");
                expect(udp.send(ports[1], Bytes{9, 1}), "IPC verified host refusal");
            } else if ((*packet)[0] == 10) {
                expect(role == 1, "Native close observation role");
                expect(udp.send(ports[0], Bytes{10, 0}), "IPC native close observed");
            }
            else expect(false, "Unknown test IPC envelope");
        }
        if (endpoints[0]) for (std::uint8_t target = 0; target < 2; ++target) {
            while (auto signal = require(endpoints[target]->poll())) {
                Bytes wire{2, static_cast<std::uint8_t>(1 - target)};
                wire.insert(wire.end(), signal->bytes.begin(), signal->bytes.end());
                expect(udp.send(ports[target], wire), "IPC signal forwarding"); ++forwarded[target];
            }
        }
        std::this_thread::sleep_for(1ms);
    }
    expect(forwarded[0] > 0 && forwarded[1] > 0, "Both GNS peers used custom signalling");
    std::cout << "Introduced ICE signals exchanged through paired endpoints\n";
}
class ProxyEndpoint final : public GnsSignallingEndpoint {
public:
    ProxyEndpoint(std::uint8_t role, std::uint16_t broker_port, bool expiry_outage = true)
        : role_(role), broker_port_(broker_port), expiry_outage_(expiry_outage) {
        const auto deadline = Clock::now() + 10s;
        auto next_register = Clock::time_point{};
        while (rendezvous_.session.empty()) {
            expect(Clock::now() < deadline, "IPC introduction deadline");
            if (Clock::now() >= next_register) {
                const Bytes registration{0, role_, static_cast<std::uint8_t>(udp_.binding.port),
                    static_cast<std::uint8_t>(udp_.binding.port >> 8)};
                expect(udp_.send(broker_port_, registration), "IPC register"); next_register = Clock::now() + 100ms;
            }
            if (auto packet = udp_.poll()) {
                if (packet->size() < 2 || (*packet)[0] != 1 || (*packet)[1] != role_)
                    throw std::runtime_error("IPC rendezvous role: expected " + std::to_string(role_)
                        + " on port " + std::to_string(udp_.binding.port) + ", broker " + std::to_string(broker_port_)
                        + ", received type " + (packet->empty() ? "empty" : std::to_string((*packet)[0]))
                        + ", role " + (packet->size() < 2 ? "missing" : std::to_string((*packet)[1])));
                auto body = std::span<const std::uint8_t>(*packet).subspan(2);
                rendezvous_.session = text(body); rendezvous_.local_identity = text(body); rendezvous_.remote_identity = text(body);
                expect(body.empty(), "IPC rendezvous trailing bytes");
            }
            std::this_thread::sleep_for(1ms);
        }
    }
    GnsRendezvous rendezvous() const override { return rendezvous_; }
    std::optional<GnsTurnCredentials> turn_credentials() const override { return turn_; }
    void use_turn(GnsTurnCredentials turn) { turn_ = std::move(turn); }
    eawr::core::Result<void> send(std::span<const std::uint8_t> signal) override {
        if (outage_.load()) return eawr::core::Result<void>::failure(failure("Injected established signalling outage"));
        if (signal.empty() || signal.size() > 60000)
            return eawr::core::Result<void>::failure(failure("IPC signal capacity"));
        Bytes packet{2, role_}; packet.insert(packet.end(), signal.begin(), signal.end());
        std::lock_guard lock(send_mutex_);
        if (!udp_.send(broker_port_, packet)) return eawr::core::Result<void>::failure(failure("IPC signal send"));
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<std::optional<GnsSignal>> poll() override {
        ++poll_calls_;
        if (outage_.load()) return eawr::core::Result<std::optional<GnsSignal>>::failure(failure("Injected established signalling outage"));
        for (unsigned i = 0; i < 32; ++i) {
            auto packet = udp_.poll();
            if (!packet) return eawr::core::Result<std::optional<GnsSignal>>::success({});
            expect(packet->size() >= 2, "IPC signal header");
            if ((*packet)[0] == 1) continue; // a retried registration's reply
            if ((*packet)[0] == 5) {
                expect(role_ == 0, "Expiry target");
                expect(udp_.send(broker_port_, Bytes{6, role_}), "IPC expiry observed");
                if (!expiry_outage_) return eawr::core::Result<std::optional<GnsSignal>>::success(
                    GnsSignal{"unauthorized-test-peer", Bytes{1}});
                outage_.store(true);
                return eawr::core::Result<std::optional<GnsSignal>>::failure(failure("Injected endpoint expiry before application ACK"));
            }
            if ((*packet)[0] == 6) { expiry_observed_ = true; continue; }
            if ((*packet)[0] == 8) { retry_observed_ = true; continue; }
            if ((*packet)[0] == 9) { host_refused_ = true; continue; }
            if ((*packet)[0] == 10) { native_close_observed_ = true; continue; }
            expect(packet->size() > 2 && (*packet)[0] == 2 && (*packet)[1] == 1 - role_, "IPC signal peer");
            return eawr::core::Result<std::optional<GnsSignal>>::success(
                GnsSignal{rendezvous_.remote_identity, Bytes(packet->begin() + 2, packet->end())});
        }
        return eawr::core::Result<std::optional<GnsSignal>>::success({});
    }
    void outage() { outage_.store(true); }
    bool outage_observed() const { return outage_.load(); }
    void expire_host() { expect(udp_.send(broker_port_, Bytes{4, role_}), "IPC request expiry"); }
    bool expiry_observed() const { return expiry_observed_; }
    void retry_ready() { expect(udp_.send(broker_port_, Bytes{8, role_}), "IPC retry ready"); }
    bool retry_observed() const { return retry_observed_; }
    void verify_refusal() {
        for (unsigned i = 0; i < 3; ++i) {
            expect(udp_.send(broker_port_, Bytes{9, role_}), "IPC publish verified refusal");
            std::this_thread::sleep_for(1ms);
        }
    }
    bool host_refused() const { return host_refused_; }
    std::size_t poll_calls() const { return poll_calls_; }
    void note_native_close() {
        for (unsigned i = 0; i < 3; ++i) {
            expect(udp_.send(broker_port_, Bytes{10, role_}), "IPC native close observation");
            std::this_thread::sleep_for(1ms);
        }
    }
    bool native_close_observed() const { return native_close_observed_; }
    void finish() {
        for (unsigned i = 0; i < 3; ++i) { expect(udp_.send(broker_port_, Bytes{3, role_}), "IPC finish"); std::this_thread::sleep_for(1ms); }
    }
private:
    Udp udp_;
    std::uint8_t role_{};
    std::uint16_t broker_port_{};
    GnsRendezvous rendezvous_;
    std::optional<GnsTurnCredentials> turn_;
    std::mutex send_mutex_;
    std::atomic<bool> outage_{};
    bool expiry_observed_{};
    bool retry_observed_{};
    bool host_refused_{};
    bool expiry_outage_{};
    std::size_t poll_calls_{};
    bool native_close_observed_{};
};
void closure_refusal_host(std::uint16_t port) {
    auto endpoint = std::make_shared<ProxyEndpoint>(std::uint8_t{0}, port);
    GnsOptions options; options.build_identity = "introduced-process-test";
    options.handshake_timeout = 10s;
    auto transport = require(GnsTransport::host_introduced(endpoint, options));
    const auto deadline = Clock::now() + 10s;
    while (!transport->diagnostic()) {
        expect(!transport->poll(), "Refusal drain emits no application frame");
        expect(Clock::now() < deadline, "Closure refusal deadline"); std::this_thread::sleep_for(1ms);
    }
    expect(endpoint->outage_observed() && transport->diagnostic()->code == "EAWR-NET-1503",
        "Typed peer refusal survives earlier service closure");
    const auto close_deadline = Clock::now() + 1s;
    while (!transport->closed()) {
        expect(!transport->poll(), "Terminal refusal emits no application frame");
        expect(Clock::now() < close_deadline, "Service closure drain is bounded"); std::this_thread::sleep_for(1ms);
    }
    expect(transport->diagnostic()->code == "EAWR-NET-1503", "Generic closure never replaces the typed refusal");
    endpoint->verify_refusal(); endpoint->finish();
    std::cout << "Typed peer refusal after service closure and bounded terminal drain passed\n";
}
void retry_host(std::uint16_t port, bool native_reason = false) {
    auto endpoint = std::make_shared<ProxyEndpoint>(std::uint8_t{0}, port, !native_reason);
    GnsOptions options; options.build_identity = "introduced-process-test";
    options.keepalive = 20ms; options.timeout = 5s; options.handshake_timeout = 15s;
    auto transport = require(GnsTransport::host_introduced(endpoint, options));
    const auto deadline = Clock::now() + 40s;
    bool retried = false;
    std::optional<std::size_t> refusal_polls;
    std::optional<Clock::time_point> refusal_started;
    bool signalling_checked = false;
    bool refusal_published = false;
    while (true) {
        expect(!transport->poll(), "Retry host received application data before admission");
        expect(transport->link(1) != LinkState::open, "Retry host admitted an expired attempt");
        if (transport->diagnostic()) {
            const auto& code = transport->diagnostic()->code;
            if (code == "EAWR-NET-1506" && !retried) {
                expect(transport->link(1) == LinkState::connecting, "Host did not retain listener on pre-ACK departure");
                endpoint->retry_ready(); retried = true;
            } else if (code == "EAWR-NET-1517") {
                expect(retried, "Expiry occurred before retry");
                if (!refusal_polls) { refusal_polls = endpoint->poll_calls(); refusal_started = Clock::now(); }
                if (native_reason && Clock::now() - *refusal_started >= 50ms && !signalling_checked) {
                    expect(endpoint->poll_calls() > *refusal_polls, "Refusal stopped consuming healthy signalling input");
                    signalling_checked = true;
                }
                if (native_reason && Clock::now() - *refusal_started >= 400ms) {
                    // Keep the SDK and healthy signalling endpoint alive until
                    // the raw peer confirms the native close. Fixture teardown
                    // must not kill GNS's asynchronous close notification.
                    if (!refusal_published) { endpoint->verify_refusal(); refusal_published = true; }
                    if (endpoint->native_close_observed()) { transport->cancel(); break; }
                }
                if (transport->closed()) break; // bounded in-band refusal linger
            } else expect(code == "EAWR-NET-1506", "Unexpected retry host diagnostic");
        }
        expect(Clock::now() < deadline, "Retry admission deadline"); std::this_thread::sleep_for(1ms);
    }
    if (native_reason) {
        expect(signalling_checked, "Healthy signalling drain was not exercised");
        expect(endpoint->native_close_observed(), "Native close was not verified before fixture teardown");
    }
    if (!refusal_published) endpoint->verify_refusal();
    endpoint->finish();
    std::cout << "Host reset admission after pre-ACK departure and rejected retry expiry\n";
}
void peer(std::uint8_t role, std::uint16_t port) {
    auto endpoint = std::make_shared<ProxyEndpoint>(role, port);
    GnsOptions options; options.build_identity = "introduced-process-test";
    options.keepalive = 20ms; options.timeout = 5s; options.handshake_timeout = 15s;
    auto transport = require(role == 0 ? GnsTransport::host_introduced(endpoint, options)
        : GnsTransport::connect_introduced(endpoint, options));
    const auto deadline = Clock::now() + 40s;
    std::vector<Bytes> received;
    while (transport->link(1 - role) != LinkState::open) {
        if (auto frame = transport->poll()) received.push_back(std::move(frame->frame));
        if (transport->diagnostic()) throw std::runtime_error(transport->diagnostic()->message);
        expect(Clock::now() < deadline, "Introduced ICE connection deadline"); std::this_thread::sleep_for(1ms);
    }
    // The established data path must survive both inbound signalling failure
    // and any later outbound GNS signal failing during its graceful drain.
    endpoint->outage();
    const Bytes sent(1024 * 1024, static_cast<std::uint8_t>(role + 31));
    require(transport->send(1 - role, {})); require(transport->send(1 - role, sent));
    while (received.size() < 2) {
        if (auto frame = transport->poll()) received.push_back(std::move(frame->frame));
        expect(!transport->diagnostic() && transport->link(1 - role) == LinkState::open,
            "Signalling outage terminated an established data path");
        expect(Clock::now() < deadline, "Introduced transfer deadline"); std::this_thread::sleep_for(1ms);
    }
    expect(received == std::vector<Bytes>{{}, Bytes(1024 * 1024, static_cast<std::uint8_t>(32 - role))},
        "Introduced reliable ordered frames differ");
    require(transport->send(1 - role, Bytes(1024 * 1024, static_cast<std::uint8_t>(role + 71))));
    std::optional<Bytes> tail;
    while (!tail) {
        if (auto frame = transport->poll()) tail = std::move(frame->frame);
        expect(!transport->diagnostic(), "Signalling outage lost the ordered tail");
        expect(Clock::now() < deadline, "Introduced tail deadline"); std::this_thread::sleep_for(1ms);
    }
    // Host closes only after the client's tail arrived. The client therefore
    // queued its own tail before close, and receives the host's tail ahead of
    // the ordered close marker even if it is scheduled later.
    if (role == 0) transport->close();
    while (!transport->closed()) {
        if (auto frame = transport->poll()) tail = std::move(frame->frame);
        expect_clean(*transport, "Signalling outage truncated a clean close");
        expect(Clock::now() < deadline, "Introduced close deadline"); std::this_thread::sleep_for(1ms);
    }
    while (auto frame = transport->poll()) tail = std::move(frame->frame);
    expect(tail == std::optional<Bytes>{Bytes(1024 * 1024, static_cast<std::uint8_t>(72 - role))}, "Introduced close lost accepted tail");
    endpoint->finish();
    std::cout << "Peer " << static_cast<unsigned>(role) << ": ICE, post-admission outage, ordered frames and close tail passed\n";
}
// #3912: a relayed pair through a TURN server that drops a permission 300 s after it was last created or refreshed
// (here: a few seconds). The pair exchanges a counter frame every 100 ms for `seconds`, and no direction may go quiet
// for longer than a permission lifetime. GNS keeps saying Connected while the relay silently drops, so the gap check
// is the only thing that notices.
//
// With `silent` the server instead stops forwarding for good a few seconds in, and the link must say so within the
// acknowledgement-silence timeout, far below GNS's own connection timeout (and the 30 s lockstep completeness wait).
//
// #4021: the same run also proves the diagnostics reach the log sink: a TURN reply line (and the repeat line when the server
// rotates its nonce) from GNS, and the 1507 stop with this seat and its peer when the path goes silent.
void turn_peer(std::uint8_t role, std::uint16_t port, const std::string& turn_address, unsigned seconds, bool silent,
    bool nonce = false) {
    std::vector<std::string> log_lines;
    struct SinkGuard { ~SinkGuard() { GnsTransport::set_log_sink({}); } } sink_guard;
    GnsTransport::set_log_sink([&log_lines](std::string_view line) { log_lines.emplace_back(line); });
    const auto turn_host = turn_address.substr(0, turn_address.rfind(':'));
    const auto logged = [&](std::string_view text) {
        return std::any_of(log_lines.begin(), log_lines.end(), [&](const std::string& line) {
            return line.starts_with("EAWR net event=gns utc=") && line.find(text) != std::string::npos; });
    };
    auto endpoint = std::make_shared<ProxyEndpoint>(role, port);
    endpoint->use_turn(GnsTurnCredentials{"turn:" + turn_address, "eawr-turn-test", "eawr-turn-secret", 3600s});
    GnsOptions options; options.build_identity = "introduced-process-test";
    options.turn_only = true; options.timeout = 80s; options.handshake_timeout = 30s;
    // The shipped default: the silent case below must be named inside 30 s with it, not with a shortened test value.
    expect(options.ack_silence_timeout == 15s, "The acknowledgement-silence default is 15 s");
    auto transport = require(role == 0 ? GnsTransport::host_introduced(endpoint, options)
        : GnsTransport::connect_introduced(endpoint, options));
    const auto connect_deadline = Clock::now() + 30s;
    while (transport->link(1 - role) != LinkState::open) {
        transport->poll();
        if (transport->diagnostic()) throw std::runtime_error("TURN pair did not open: " + transport->diagnostic()->message);
        expect(Clock::now() < connect_deadline, "Relayed ICE connection deadline"); std::this_thread::sleep_for(1ms);
    }
    const auto begin = Clock::now(), end = begin + std::chrono::seconds(silent ? 30u : seconds);
    auto last_send = begin, last_receive = begin;
    std::uint64_t sent = 0, received = 0;
    std::chrono::milliseconds worst_gap{0};
    while (Clock::now() < end) {
        const auto now = Clock::now();
        if (now - last_send >= 100ms) {
            Bytes frame(8); for (unsigned i = 0; i < 8; ++i) frame[i] = static_cast<std::uint8_t>(sent >> (8 * i));
            require(transport->send(1 - role, frame)); ++sent; last_send = now;
        }
        while (auto frame = transport->poll()) {
            expect(frame->frame.size() == 8, "Relayed frame size");
            std::uint64_t counter = 0; for (unsigned i = 0; i < 8; ++i) counter |= static_cast<std::uint64_t>(frame->frame[i]) << (8 * i);
            expect(counter == received, "Relayed frames arrive complete and in order"); ++received; last_receive = Clock::now();
        }
        const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - last_receive);
        worst_gap = std::max(worst_gap, gap);
        if (silent && transport->diagnostic()) {
            const auto& diagnostic = *transport->diagnostic();
            expect(diagnostic.code == "EAWR-NET-1507" && diagnostic.message.find("GNS link silent") != std::string::npos
                && diagnostic.message.find("relay") != std::string::npos, "Silent relay path is named as such");
            std::cout << "Peer " << static_cast<unsigned>(role) << ": " << diagnostic.code << ' ' << diagnostic.message << '\n';
            while (transport->poll()) {}
            expect(logged("seat=" + std::to_string(role) + " peer=" + std::to_string(1 - role)
                + " link stops: EAWR-NET-1507 GNS link silent"), "The 1507 stop reaches the log sink with seat and peer");
            for (const auto& line : log_lines) std::cout << line << '\n';
            GnsTransport::set_log_sink({});
            endpoint->finish();
            return;
        }
        if (transport->diagnostic()) throw std::runtime_error("Relayed link failed after "
            + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - begin).count()) + " s: "
            + transport->diagnostic()->code + ": " + transport->diagnostic()->message);
        if (!silent && gap > 4s) throw std::runtime_error("Relayed link went silent for " + std::to_string(gap.count()) + " ms at "
            + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - begin).count())
            + " s while GNS still reports Connected (TURN permission not refreshed)");
        std::this_thread::sleep_for(1ms);
    }
    expect(!silent, "A silent relay path was not reported within 30 s");
    expect(received > 10ull * seconds / 2, "Relayed frames kept flowing");
    while (transport->poll()) {}
    expect(logged("ICE: TURN server <addr> accepted a request"), "A TURN refresh reply reaches the log sink, address redacted");
    if (nonce) expect(logged("answered 438, repeating the request"), "The nonce repeat reaches the log sink");
    expect(!turn_host.empty() && std::none_of(log_lines.begin(), log_lines.end(), [&](const std::string& line) {
        return line.find(turn_host) != std::string::npos || line.find("127.0.0.1") != std::string::npos; }), "The log carries no address");
    std::cout << "Peer " << static_cast<unsigned>(role) << ": " << log_lines.size() << " log lines, first: "
              << (log_lines.empty() ? std::string{} : log_lines.front()) << '\n';
    GnsTransport::set_log_sink({});
    // Closing needs the peer's frames drained too; the host leaves first.
    if (role == 0) transport->close();
    const auto close_deadline = Clock::now() + 20s;
    while (!transport->closed()) {
        while (transport->poll()) {}
        expect(Clock::now() < close_deadline, "Relayed close deadline"); std::this_thread::sleep_for(1ms);
    }
    endpoint->finish();
    std::cout << "Peer " << static_cast<unsigned>(role) << ": " << sent << " frames sent, " << received
              << " received over " << seconds << " s through a permission-expiring TURN relay, worst gap "
              << worst_gap.count() << " ms\n";
}
#if EAWR_HAS_GNS
void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}
// A native SDK client controls the ACK boundary that two fully automatic
// adapters cannot expose deterministically. All game envelopes remain existing
// TcpMessage kinds; IPC markers above belong solely to this test fixture.
void delayed_ack_peer(std::uint16_t port, bool retry_case, bool native_reason = false, bool closure_refusal = false) {
    auto endpoint = std::make_shared<ProxyEndpoint>(std::uint8_t{1}, port);
    SteamNetworkingIdentity identity{}; identity.SetGenericString(endpoint->rendezvous().local_identity.c_str());
    SteamNetworkingErrMsg reason{}; expect(GameNetworkingSockets_Init(&identity, reason), "Raw GNS initialization");
    struct Shutdown { ~Shutdown() { GameNetworkingSockets_Kill(); } } shutdown;
    struct Signal final : ISteamNetworkingConnectionSignaling {
        std::shared_ptr<ProxyEndpoint> endpoint;
        explicit Signal(std::shared_ptr<ProxyEndpoint> value) : endpoint(std::move(value)) {}
        bool SendSignal(HSteamNetConnection, const SteamNetConnectionInfo_t&, const void* bytes, int size) override {
            return size > 0 && static_cast<bool>(endpoint->send({static_cast<const std::uint8_t*>(bytes), static_cast<std::size_t>(size)}));
        }
        void Release() override { delete this; }
    };
    struct Context final : ISteamNetworkingSignalingRecvContext {
        ISteamNetworkingConnectionSignaling* OnConnectRequest(HSteamNetConnection, const SteamNetworkingIdentity&, int) override { return nullptr; }
        void SendRejectionSignal(const SteamNetworkingIdentity&, const void*, int) override {}
    } context;
    auto& sdk = *SteamNetworkingSockets();
    SteamNetworkingIdentity remote{}; remote.SetGenericString(endpoint->rendezvous().remote_identity.c_str());
    SteamNetworkingConfigValue_t buffer{}; buffer.SetInt32(k_ESteamNetworkingConfig_SendBufferSize, 8 * 1024 * 1024);
    auto connection = sdk.ConnectP2PCustomSignaling(new Signal(endpoint), &remote, 0, 1, &buffer);
    expect(connection != k_HSteamNetConnection_Invalid, "Raw GNS connect");
    if (closure_refusal) {
        bool expiry_requested{}, refusal_sent{};
        const auto deadline = Clock::now() + 10s;
        while (!endpoint->host_refused()) {
            expect(Clock::now() < deadline, "Raw closure-refusal deadline");
            while (auto signal = require(endpoint->poll()))
                expect(sdk.ReceivedP2PCustomSignal(signal->bytes.data(), static_cast<int>(signal->bytes.size()), &context), "Raw closure signal");
            sdk.RunCallbacks();
            SteamNetworkingMessage_t* message{};
            while (sdk.ReceiveMessagesOnConnection(connection, &message, 1) > 0) {
                const bool hello = message->m_cbSize > 0 && static_cast<const std::uint8_t*>(message->m_pData)[0] == static_cast<std::uint8_t>(TcpMessage::hello);
                message->Release();
                // Receiving the host hello proves its ICE path is ready. Send
                // no application hello, so service closure precedes admission.
                if (hello && !expiry_requested) { endpoint->expire_host(); expiry_requested = true; }
            }
            if (endpoint->expiry_observed() && !refusal_sent) {
                // Only hosts send refusal envelopes. This guest sends a valid
                // mismatched hello, which must produce 1503 on the receiving
                // host even though signalling has already closed.
                GnsOptions mismatch; mismatch.build_identity = "introduced-process-test-different";
                Bytes packet{static_cast<std::uint8_t>(TcpMessage::hello), 'E', 'A', 'W', 'R'};
                u32(packet, mismatch.protocol_version); u32(packet, static_cast<std::uint32_t>(mismatch.build_identity.size()));
                packet.insert(packet.end(), mismatch.build_identity.begin(), mismatch.build_identity.end());
                u32(packet, mismatch.max_frame_size); u32(packet, 1); u32(packet, mismatch.peers); packet.push_back(1);
                expect(sdk.SendMessageToConnection(connection, packet.data(), static_cast<uint32>(packet.size()),
                    k_nSteamNetworkingSend_ReliableNoNagle, nullptr) == k_EResultOK, "Raw mismatched hello send");
                refusal_sent = true;
            }
            std::this_thread::sleep_for(1ms);
        }
        expect(expiry_requested && refusal_sent && endpoint->expiry_observed(), "Service closed before native refusal was sent");
        sdk.CloseConnection(connection, 0, nullptr, false); endpoint->finish(); return;
    }
    GnsOptions options; options.build_identity = "introduced-process-test";
    GnsFrameDecoder decoder(options, false);
    bool hello_sent = false, hello_seen = false, accepted = false, ack_sent = false, close_seen = false, tail_sent = false;
    bool first_departed = false, retry_connected = false;
    std::optional<eawr::core::Diagnostic> refusal;
    std::vector<Bytes> received;
    const auto deadline = Clock::now() + 40s;
    auto send = [&](const Bytes& packet) {
        expect(sdk.SendMessageToConnection(connection, packet.data(), static_cast<uint32>(packet.size()),
            k_nSteamNetworkingSend_Reliable, nullptr) == k_EResultOK, "Raw reliable send");
    };
    auto frame = [&](const Bytes& bytes) {
        std::size_t offset = 0;
        do {
            const auto count = std::min<std::size_t>(64 * 1024, bytes.size() - offset);
            Bytes packet{static_cast<std::uint8_t>(TcpMessage::data)};
            packet.reserve(9 + count);
            u32(packet, static_cast<std::uint32_t>(bytes.size())); u32(packet, static_cast<std::uint32_t>(offset));
            packet.insert(packet.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.begin() + static_cast<std::ptrdiff_t>(offset + count));
            send(packet); offset += count;
        } while (offset < bytes.size());
    };
    while (true) {
        if (Clock::now() >= deadline) throw std::runtime_error("Delayed ACK peer deadline: refusal="
            + std::to_string(refusal.has_value()) + " expiry=" + std::to_string(endpoint->expiry_observed())
            + " host-refused=" + std::to_string(endpoint->host_refused()));
        while (auto signal = require(endpoint->poll())) expect(sdk.ReceivedP2PCustomSignal(signal->bytes.data(),
            static_cast<int>(signal->bytes.size()), &context), "Raw custom signal");
        sdk.RunCallbacks();
        if (connection == k_HSteamNetConnection_Invalid) {
            if (endpoint->retry_observed()) {
                connection = sdk.ConnectP2PCustomSignaling(new Signal(endpoint), &remote, 0, 1, &buffer);
                expect(connection != k_HSteamNetConnection_Invalid, "Raw retry connect");
            }
            std::this_thread::sleep_for(1ms); continue;
        }
        SteamNetConnectionInfo_t info{}; expect(sdk.GetConnectionInfo(connection, &info), "Raw connection info");
        if (info.m_eState == k_ESteamNetworkingConnectionState_Connected && !hello_sent) {
            if (first_departed) {
                // A second native route is established, but this attempt has
                // sent no application hello when signalling expires.
                endpoint->expire_host(); retry_connected = true;
            } else {
                Bytes packet; packet.reserve(18 + options.build_identity.size());
                packet.insert(packet.end(), {static_cast<std::uint8_t>(TcpMessage::hello), 'E', 'A', 'W', 'R'});
                u32(packet, options.protocol_version); u32(packet, static_cast<std::uint32_t>(options.build_identity.size()));
                packet.insert(packet.end(), options.build_identity.begin(), options.build_identity.end());
                u32(packet, options.max_frame_size); u32(packet, 1); u32(packet, options.peers); packet.push_back(1); send(packet);
            }
            hello_sent = true;
        }
        SteamNetworkingMessage_t* message{};
        while (sdk.ReceiveMessagesOnConnection(connection, &message, 1) > 0) {
            auto decoded = decoder.receive({static_cast<const std::uint8_t*>(message->m_pData), static_cast<std::size_t>(message->m_cbSize)},
                {hello_seen, accepted, close_seen, 0, 0}, (message->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0);
            message->Release();
            if (!decoded && retry_case && decoded.error().code == "EAWR-NET-1517") {
                refusal = decoded.error();
                expect(refusal->message == (native_reason ? "Invalid or unauthorized GNS signal" : "GNS introduction service failed"),
                    "Joiner lost host refusal text");
                break;
            }
            auto value = require(std::move(decoded));
            if (value.kind == TcpMessage::hello) hello_seen = true;
            else if (value.kind == TcpMessage::accept) {
                if (retry_case) {
                    expect(!first_departed, "Retry accepted without a new hello");
                    sdk.CloseConnection(connection, 1506, "Injected departure before ACK", false);
                    connection = k_HSteamNetConnection_Invalid; first_departed = true;
                    hello_sent = hello_seen = accepted = false; decoder.reset(); break;
                }
                accepted = true; endpoint->expire_host();
            }
            else if (value.frame) received.push_back(std::move(*value.frame));
            else if (value.kind == TcpMessage::close) { close_seen = true; send({static_cast<std::uint8_t>(TcpMessage::close)}); }
        }
        if (accepted && endpoint->expiry_observed() && !ack_sent) {
            Bytes packet; packet.reserve(5); packet.push_back(static_cast<std::uint8_t>(TcpMessage::accept));
            u32(packet, 1); send(packet); ack_sent = true;
            frame({}); frame(Bytes(1024 * 1024, 32));
        }
        if (received.size() >= 2 && !tail_sent) { frame(Bytes(1024 * 1024, 72)); tail_sent = true; }
        if (!retry_case && close_seen) {
            // The host's native P2P close notification uses the deliberately
            // failed signalling endpoint. Like the adapter, finish from the
            // ordered application close and acknowledged native sends instead.
            SteamNetConnectionRealTimeStatus_t status{};
            if (sdk.GetConnectionRealTimeStatus(connection, &status, 0, nullptr) == k_EResultOK
                && status.m_cbPendingReliable == 0 && status.m_cbSentUnackedReliable == 0) break;
        }
        if (retry_case && refusal && endpoint->expiry_observed() && endpoint->host_refused()) {
            // The joiner must decode the host's in-band diagnostic despite
            // failed signalling. Broker telemetry only confirms host teardown.
            expect(retry_connected && first_departed && !ack_sent && received.empty(), "Retry admission/refusal boundary differs");
            if (native_reason) {
                if (info.m_eState != k_ESteamNetworkingConnectionState_ClosedByPeer) {
                    std::this_thread::sleep_for(1ms); continue;
                }
                expect(info.m_eEndReason == 1517, "Native refusal fallback lost the typed host reason");
                endpoint->note_native_close();
            }
            break;
        }
        if (!retry_case && info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer) {
            expect(close_seen, "Host aborted at pre-ACK signalling expiry"); break;
        }
        if (!retry_case) expect(info.m_eState != k_ESteamNetworkingConnectionState_ProblemDetectedLocally, "Raw native connection failed");
        std::this_thread::sleep_for(1ms);
    }
    expect(endpoint->expiry_observed(), "Injected endpoint expiry was not observed");
    if (retry_case) expect(first_departed && retry_connected && refusal && refusal->code == "EAWR-NET-1517"
        && endpoint->host_refused() && !ack_sent && received.empty(), "Retry test lost the joiner admission diagnostic");
    else expect(ack_sent && received == std::vector<Bytes>{{}, Bytes(1024 * 1024, 31), Bytes(1024 * 1024, 71)}, "Pre-ACK expiry lost ordered frames/tail");
    sdk.CloseConnection(connection, 0, nullptr, false); endpoint->finish();
    std::cout << (retry_case ? "Retry rejected at injected endpoint expiry before hello\n"
        : "Injected host endpoint expiry after acceptance and before ACK; frames and close tail passed\n");
}
#endif
}
int main(int argc, char** argv) {
    try {
        if (!GnsTransport::available()) { std::cout << "GNS disabled: introduced test skipped\n"; return 77; }
        expect(argc >= 2, "introduced test mode required");
        if (std::string_view(argv[1]) == "broker") broker();
        else {
            expect(argc == 4 || ((argc == 6 || argc == 7) && std::string_view(argv[1]) == "turn-peer"),
                "introduced peer role and broker port required");
            const auto role = std::stoul(argv[2]), port = std::stoul(argv[3]);
            expect(role < 2 && port > 0 && port <= 65535, "introduced arguments");
            if (std::string_view(argv[1]) == "turn-peer") {
                expect(argc >= 6, "TURN peer address and duration required");
                turn_peer(static_cast<std::uint8_t>(role), static_cast<std::uint16_t>(port), argv[4],
                    static_cast<unsigned>(std::stoul(argv[5])), argc == 7 && std::string_view(argv[6]) == "silent",
                    argc == 7 && std::string_view(argv[6]) == "nonce");
            } else if (std::string_view(argv[1]) == "closure-refusal-host") {
                expect(role == 0, "Closure refusal host role"); closure_refusal_host(static_cast<std::uint16_t>(port));
            } else if (std::string_view(argv[1]) == "closure-refusal-peer") {
#if EAWR_HAS_GNS
                expect(role == 1, "Raw closure refusal role"); delayed_ack_peer(static_cast<std::uint16_t>(port), false, false, true);
#endif
            } else if (std::string_view(argv[1]) == "retry-host" || std::string_view(argv[1]) == "retry-native-host") {
                expect(role == 0, "Retry host role"); retry_host(static_cast<std::uint16_t>(port), std::string_view(argv[1]) == "retry-native-host");
            } else if (std::string_view(argv[1]) == "delayed-ack" || std::string_view(argv[1]) == "retry-expiry"
                || std::string_view(argv[1]) == "retry-native") {
#if EAWR_HAS_GNS
                expect(role == 1, "Raw peer role"); delayed_ack_peer(static_cast<std::uint16_t>(port),
                    std::string_view(argv[1]) != "delayed-ack", std::string_view(argv[1]) == "retry-native");
#endif
            } else peer(static_cast<std::uint8_t>(role), static_cast<std::uint16_t>(port));
        }
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
