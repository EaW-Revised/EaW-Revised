#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/transport.hpp"
#include "eawr/core/sha256.hpp"
#include "../support/lockstep_loopback.hpp"
#include "diagnostic_sink.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace net = eawr::net;
namespace core = eawr::core;
namespace loopback = eawr::testing::lockstep;
namespace tactical = eawr::sim::tactical;

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void good(const core::Result<void>& result) {
    require(static_cast<bool>(result), result ? "" : result.error().message);
}

struct Bus {
    explicit Bus(std::size_t peers = 2) : inbox(peers) {}
    std::vector<std::deque<net::Received>> inbox;
};
class FakeTransport final : public net::MessageTransport {
public:
    FakeTransport(Bus& bus, std::uint32_t self) : bus_(bus), self_(self) {}
    core::Result<void> send(std::uint32_t to, std::span<const std::uint8_t> frame) override {
        bus_.inbox.at(to).push_back({self_, {frame.begin(), frame.end()}});
        return core::Result<void>::success();
    }
    std::optional<net::Received> poll() override {
        auto& queue = bus_.inbox.at(self_);
        if (queue.empty()) return {};
        auto value = std::move(queue.front()); queue.pop_front(); return value;
    }
    net::LinkState link(std::uint32_t) const override { return net::LinkState::open; }
    std::uint32_t self() const override { return self_; }
    std::uint32_t peer_count() const override { return static_cast<std::uint32_t>(bus_.inbox.size()); }
private:
    Bus& bus_;
    std::uint32_t self_{};
};

struct Pair {
    Bus bus;
    FakeTransport a{bus, 0}, b{bus, 1};
    net::DesyncMonitor left, right;
    Pair(const std::filesystem::path& directory, std::size_t budget = 16 * 1024 * 1024,
        std::uint64_t every = 30, bool persist = false)
        : left(a, options(directory, budget, every, persist)), right(b, options(directory, budget, every, persist)) {}
    static net::DesyncOptions options(const std::filesystem::path& directory, std::size_t budget,
        std::uint64_t every = 30, bool persist = false) {
        net::DesyncOptions options;
        if (persist) options.diagnostic_sink = eawr::testing::platform_diagnostic_sink(directory);
        options.match_identity = "scripted-test-match";
        options.match_setup = {1, 2, 3};
        options.build_identity = "test-build";
        options.max_retained_bytes = budget;
        options.exchange_every = every;
        return options;
    }
    void drain() {
        for (unsigned rounds = 0; rounds < 10; ++rounds) {
            bool progress = false;
            for (const auto& [transport, monitor] : std::array<std::pair<FakeTransport*, net::DesyncMonitor*>, 2>{{{&a, &left}, {&b, &right}}}) {
                while (auto received = transport->poll()) { good(monitor->receive(received->from, received->frame)); progress = true; }
            }
            if (!progress) return;
        }
        throw std::runtime_error("diagnostic exchange did not quiesce");
    }
};
std::string digest(std::uint64_t tick, bool fault = false) {
    const auto value = std::to_string(tick) + (fault ? ":fault" : ":agreed");
    return core::sha256_hex({reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
std::string read(const std::filesystem::path& file) {
    std::ifstream input(file, std::ios::binary);
    require(static_cast<bool>(input), "missing bundle file " + file.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string bundle_file(const net::DiagnosticBundle& bundle, const std::string& name) {
    const auto found = std::find_if(bundle.files.begin(), bundle.files.end(), [&](const auto& file) { return file.name == name; });
    require(found != bundle.files.end(), "missing in-memory diagnostic file: " + name);
    return {found->bytes.begin(), found->bytes.end()};
}

void check_report(const Pair& pair, std::uint64_t tick, std::uint64_t detected) {
    require(pair.left.frozen() && pair.right.frozen(), "both peers must freeze");
    require(!pair.left.can_step() && !pair.right.can_step(), "frozen peers may not step");
    for (const auto* monitor : {&pair.left, &pair.right}) {
        require(monitor->report().has_value(), "missing typed report");
        const auto& report = *monitor->report();
        require(report.first_divergent_tick == tick && report.detected_at == detected, "wrong first divergent tick");
        require(report.local_hash != report.remote_hash, "report lost hashes");
        require(!report.bundle_error, "bundle write failed");
        require(!report.bundle.id.empty() && report.bundle.files.size() == 9, "missing typed diagnostic bundle");
        const auto text = bundle_file(report.bundle, "report.txt");
        require(text.find("first_divergent_tick=" + std::to_string(tick) + "\n") != std::string::npos, "bundle tick");
        require(text.find(report.local_hash) != std::string::npos && text.find(report.remote_hash) != std::string::npos, "bundle hashes");
        require(bundle_file(report.bundle, "identity.txt").find("test-build") != std::string::npos, "build identity");
        require(bundle_file(report.bundle, "setup.bin") == std::string("\1\2\3", 3), "setup encoding");
        require(!bundle_file(report.bundle, "session.bin").empty(), "canonical session encoding missing");
        require(bundle_file(report.bundle, "inputs.bin").starts_with("eawr-desync-input-v1\n"), "canonical input framing");
        require(bundle_file(report.bundle, "hashes.csv").find(report.local_hash) != std::string::npos, "hash row contents");
        require(!bundle_file(report.bundle, "README.txt").empty(), "README missing");
        std::size_t size = 0;
        for (const auto& file : report.bundle.files) {
            size += file.bytes.size();
            if (!report.bundle_location.empty()) require(read(std::filesystem::path(report.bundle_location) / file.name)
                == std::string(file.bytes.begin(), file.bytes.end()), "published file differs from typed bundle");
        }
        require(size == report.bundle.size_bytes, "bundle byte count differs");
    }
    require(pair.left.report()->local_hash == pair.right.report()->remote_hash, "reports disagree on peer hashes");
}

void peer_confirmation(const std::filesystem::path& directory) {
    Pair pair(directory, 16 * 1024 * 1024, 10);
    require(!pair.left.peer_confirmed_tick(), "no confirmation before any remote summary");
    good(pair.left.record(0, digest(0)));
    good(pair.right.record(0, digest(0)));
    pair.drain();
    require(pair.left.peer_confirmed_tick() == 0, "tick-zero remote proof");
    for (std::uint64_t tick = 1; tick <= 10; ++tick) {
        good(pair.left.record(tick, digest(tick)));
        good(pair.right.record(tick, digest(tick)));
    }
    require(pair.left.peer_confirmed_tick() == 0, "local tick 10 is not remote confirmation");
    pair.drain();
    require(pair.left.peer_confirmed_tick() == 10 && pair.right.peer_confirmed_tick() == 10, "both summaries confirm tick 10");

    Pair dropped(directory, 16 * 1024 * 1024, 10);
    good(dropped.left.record(0, digest(0))); good(dropped.right.record(0, digest(0))); dropped.drain();
    for (std::uint64_t tick = 1; tick <= 10; ++tick) good(dropped.left.record(tick, digest(tick)));
    good(dropped.left.on_departure(1, 10));
    require(dropped.left.peer_confirmed_tick() != 10, "departure cannot count as remote tick-10 proof");
}

void synthetic(const std::filesystem::path& directory, std::uint64_t fault, std::uint64_t end,
    bool input_fault = false, std::uint64_t every = 30) {
    Pair pair(directory, 16 * 1024 * 1024, every);
    for (std::uint64_t tick = 0; tick <= end; ++tick) {
        const std::vector<std::uint8_t> input{static_cast<std::uint8_t>(tick), 7};
        auto other = input;
        if (input_fault && tick == fault) other.back() = 8;
        const std::vector<std::uint8_t> encoding{static_cast<std::uint8_t>(tick), 9};
        good(pair.left.record(tick, digest(tick), encoding, input, tick == end));
        good(pair.right.record(tick, digest(tick, tick == fault && !input_fault), encoding, other, tick == end));
        if (tick == 0) require(!pair.left.can_step(), "zero barrier must wait for remote setup hash");
        pair.drain();
        if (pair.left.frozen()) break;
    }
    check_report(pair, fault, std::min(end, ((fault + every - 1) / every) * every));
    if (input_fault) require(pair.left.report()->channel == "canonical input", "wrong input divergence channel");
    // Transport remains usable after the diagnostic freeze.
    good(pair.a.send(1, std::array<std::uint8_t, 1>{7}));
    require(pair.b.poll().has_value(), "transport died when simulation froze");
}

struct TraceRow { std::uint64_t tick{}; std::string hash; std::vector<std::uint8_t> input, encoding; };
struct Trace { std::vector<TraceRow> rows; std::vector<std::uint8_t> pending_input; };
struct TraceAdapter : loopback::TacticalAdapter {
    static inline std::map<const Session*, Trace*> traces;
    static std::string state_hash(const Session& session) {
        auto hash = TacticalAdapter::state_hash(session);
        auto& trace = *traces.at(&session);
        if (trace.rows.empty()) trace.rows.push_back({0, hash, {}, encoding(session)});
        return hash;
    }
    static core::Result<std::vector<std::uint8_t>> input_bytes(const Session& session, std::span<const Command> commands) {
        auto bytes = TacticalAdapter::input_bytes(session, commands);
        if (bytes) traces.at(&session)->pending_input = bytes.value();
        return bytes;
    }
    static core::Result<std::string> step(Session& session, const eawr::sim::PartitionExecutor& executor) {
        auto result = TacticalAdapter::step(session, executor);
        if (result) {
            const auto tick = session.completed_tick();
            auto& trace = *traces.at(&session);
            trace.rows.push_back({tick, result.value(), std::move(trace.pending_input),
                tick % 30 == 0 || tick == 2000 ? encoding(session) : std::vector<std::uint8_t>{}});
        }
        return result;
    }
};

void scripted_battle(const std::filesystem::path& fixture, const std::filesystem::path& directory, bool inject = false) {
    std::ifstream input(fixture, std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto replay = tactical::parse_replay(bytes);
    require(static_cast<bool>(replay), "cannot load battle fixture");
    auto split = loopback::split_by_issuer<TraceAdapter>(replay.value().commands, 2);
    eawr::sim::InlineExecutor executor;
    std::array<Trace, 2> traces;
    std::vector<loopback::Peer<TraceAdapter>> peers;
    peers.reserve(2);
    TraceAdapter::traces.clear();
    for (std::size_t index = 0; index < 2; ++index) {
        auto session = tactical::TacticalSession::create(replay.value().setup);
        require(static_cast<bool>(session), "cannot create scripted battle");
        std::erase_if(split[index], [](const auto& command) { return command.key.tick >= 2000; });
        peers.push_back({std::move(session).value(), &executor, split[index], {}, {}});
        TraceAdapter::traces.emplace(&peers.back().session, &traces[index]);
    }
    loopback::Options options;
    options.input_delay = 3;
    options.final_tick = 2000;
    if (inject) peers[1].before_step = [](auto& session, std::uint64_t tick) {
        if (tick == 47) {
            require(!session.units().empty(), "fault injection needs a live unit");
            good(session.stage_remove(session.units().back().entity_id));
        }
    };
    auto result = loopback::run<TraceAdapter>({&peers[0], &peers[1]}, options);
    if (inject) require(result.divergence && result.divergence->completed_tick == 48, result.describe());
    else require(result.agreed() && result.completed_ticks == 2000, result.describe());
    require(result.remote_commands > 0, "scripted battle must carry commands");
    Pair pair(directory);
    for (std::size_t index = 0; index < traces[0].rows.size(); ++index) {
        const auto& a = traces[0].rows[index];
        const auto& b = traces[1].rows[index];
        good(pair.left.record(a.tick, a.hash, a.encoding, a.input, a.tick == 2000));
        // A faster peer waits at the exchange until the remote hash arrives.
        if (a.tick % 30 == 0) require(!pair.left.can_step(), "exchange must wait for remote peer");
        good(pair.right.record(b.tick, b.hash, b.encoding, b.input, b.tick == 2000));
        pair.drain();
        if (pair.left.frozen()) break;
    }
    if (inject) check_report(pair, 48, 60);
    else {
        require(pair.left.agreed_end() && pair.right.agreed_end(), "final partial exchange did not agree");
        require(!pair.left.report() && !pair.right.report(), "agreement generated report");
    }
    TraceAdapter::traces.clear();
}

void malformed_and_bounds(const std::filesystem::path& directory) {
    for (std::size_t size = 0; size < 82; ++size) {
        Pair pair(directory);
        const std::vector<std::uint8_t> truncated(size, net::desync_combined_kind);
        require(!pair.left.receive(1, truncated), "truncated combined message accepted");
        require(!pair.left.can_step(), "protocol failure must gate stepping");
    }
    Pair pair(directory, 512);
    const std::vector<std::uint8_t> encoding(200, 9);
    for (std::uint64_t tick = 0; tick <= 120; ++tick) {
        good(pair.left.record(tick, digest(tick), encoding));
        good(pair.right.record(tick, digest(tick), encoding));
        pair.drain();
        require(pair.left.retained_bytes() <= 512, "retention budget exceeded");
    }
    good(pair.left.finish()); good(pair.right.finish());
    require(pair.left.agreed_end() && pair.right.agreed_end(), "exact-boundary finish");
    Pair partial(directory);
    for (std::uint64_t tick = 0; tick <= 7; ++tick) {
        good(partial.left.record(tick, digest(tick), encoding));
        good(partial.right.record(tick, digest(tick, tick == 4), encoding));
        partial.drain();
    }
    good(partial.left.finish(encoding)); good(partial.right.finish(encoding)); partial.drain();
    check_report(partial, 4, 7);

    // A world-only tail row is replaced by the full hash at finish, so a script-only divergence in
    // the last partial window is still caught.
    Pair script_tail(directory);
    for (std::uint64_t tick = 0; tick <= 7; ++tick) {
        good(script_tail.left.record(tick, digest(tick), encoding));
        good(script_tail.right.record(tick, digest(tick), encoding));
        script_tail.drain();
    }
    good(script_tail.left.finish(encoding, digest(7))); good(script_tail.right.finish(encoding, digest(7, true))); script_tail.drain();
    check_report(script_tail, 7, 7);
    Pair bad_tail(directory);
    good(bad_tail.left.record(0, digest(0), encoding)); good(bad_tail.right.record(0, digest(0), encoding)); bad_tail.drain();
    good(bad_tail.left.record(1, digest(1), encoding));
    require(!bad_tail.left.finish(encoding, "not-a-state-digest"), "invalid finish hash accepted");

    Pair invalid_tick(directory);
    require(!invalid_tick.left.record(1, digest(1)), "nonzero initial tick accepted");
    Pair invalid_hash(directory);
    require(!invalid_hash.left.record(0, "not-a-state-digest"), "invalid state hash accepted");
    Pair zero_interval(directory, 512, 0);
    require(!zero_interval.left.needs_encoding(0) && !zero_interval.left.record(0, digest(0)), "zero interval accepted");
    Pair excessive_interval(directory, 512, 4097);
    require(!excessive_interval.left.record(0, digest(0)), "unbounded interval accepted");
    Pair tiny(directory, 64);
    const std::vector<std::uint8_t> excessive_input(65, 1);
    require(!tiny.left.record(0, digest(0), {}, excessive_input), "oversized input window accepted");

    Pair forged(directory);
    good(forged.left.record(0, digest(0), encoding));
    good(forged.right.record(0, digest(0, true), encoding));
    // Deliver the combined hashes, then corrupt one row in the authenticated detail frame.
    auto from_left = forged.b.poll(); auto from_right = forged.a.poll();
    require(from_left && from_right, "combined messages missing");
    good(forged.right.receive(from_left->from, from_left->frame));
    good(forged.left.receive(from_right->from, from_right->frame));
    auto rows = forged.a.poll();
    require(rows && rows->frame[0] == net::desync_rows_kind, "detail rows missing");
    rows->frame[17] = rows->frame[17] == 'a' ? 'b' : 'a';
    require(!forged.left.receive(rows->from, rows->frame), "forged hash rows accepted");

    // Diagnostics must stay frozen and typed even if the configured destination fails.
    std::filesystem::create_directories(directory);
    const auto bad_root = directory / "not-a-directory";
    { std::ofstream file(bad_root); file << "file"; }
    Pair no_disk(bad_root, 16 * 1024 * 1024, 30, true);
    good(no_disk.left.record(0, digest(0), encoding));
    good(no_disk.right.record(0, digest(0, true), encoding));
    no_disk.drain();
    require(no_disk.left.frozen() && no_disk.right.frozen(), "I/O failure resumed stepping");
    require(no_disk.left.report() && no_disk.left.report()->bundle_error, "I/O failure lost the typed report");
    require(no_disk.left.report()->bundle.files.size() == 9, "sink failure lost the in-memory bundle");

    Pair exported(directory, 16 * 1024 * 1024, 30, true);
    good(exported.left.record(0, digest(0), encoding));
    good(exported.right.record(0, digest(0, true), encoding));
    exported.drain();
    check_report(exported, 0, 0);
    require(!exported.left.report()->bundle_location.empty(), "platform exporter returned no location");

    Pair oversized_checkpoint(directory, 512);
    const std::vector<std::uint8_t> oversized(1024, 8);
    for (std::uint64_t tick = 0; tick <= 30; ++tick) {
        const auto& state = tick == 0 ? encoding : oversized;
        good(oversized_checkpoint.left.record(tick, digest(tick), state));
        good(oversized_checkpoint.right.record(tick, digest(tick, tick == 1), state));
        oversized_checkpoint.drain();
    }
    check_report(oversized_checkpoint, 1, 30);
    require(oversized_checkpoint.left.report()->encoding_tick == 0, "oversized new checkpoint discarded usable evidence");
}

void asynchronous_peers(const std::filesystem::path& directory, bool fault) {
    constexpr std::size_t count = 8;
    Bus bus(count);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    std::array<std::uint64_t, count> ticks{};
    const std::vector<std::uint8_t> encoding{1, 2};
    for (std::uint32_t peer = 0; peer < count; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(bus, peer));
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), Pair::options(directory, 4096)));
        good(monitors.back()->record(0, digest(0), encoding));
    }
    for (unsigned round = 0; round < 4000; ++round) {
        for (std::size_t peer = 0; peer < count; ++peer) {
            // Peer 7 delays receiving: the other peers can enter a subsequent
            // exchange before its previous exchange has collected every summary.
            if (peer != 7 || round % 97 == 0) {
                if (peer == 7) {
                    // Preserve sender 0's order, but read its fast stream before
                    // other streams that still hold the previous agreement.
                    auto& inbox = bus.inbox[peer];
                    while (true) {
                        const auto fast = std::find_if(inbox.begin(), inbox.end(), [](const auto& frame) { return frame.from == 0; });
                        if (fast == inbox.end()) break;
                        auto received = std::move(*fast);
                        inbox.erase(fast);
                        good(monitors[peer]->receive(received));
                    }
                }
                while (auto received = transports[peer]->poll()) good(monitors[peer]->receive(std::move(*received)));
            }
            if (ticks[peer] < 90 && monitors[peer]->can_step()) {
                const auto tick = ++ticks[peer];
                good(monitors[peer]->record(tick, digest(tick, fault && peer == 7 && tick == 48), encoding, {}, tick == 90));
            }
        }
        const bool done = std::all_of(monitors.begin(), monitors.end(), [&](const auto& monitor) {
            return fault ? monitor->report().has_value() : monitor->agreed_end();
        });
        if (done) {
            if (fault) for (const auto& monitor : monitors)
                require(monitor->frozen() && monitor->report()->first_divergent_tick == 48, "asynchronous first divergence");
            return;
        }
    }
    throw std::runtime_error("asynchronous peer exchange did not settle");
}

template<class Base>
core::Result<void> notify_departure(Base& base, net::DesyncMonitor& monitor, net::PeerIndex peer, std::uint64_t tick) {
    // Before LAN-6 lands use the concrete compatibility entry point. As soon as
    // its virtual exists this qualification invokes it through SessionMonitor&.
    if constexpr (requires { base.on_departure(peer, tick); }) return base.on_departure(peer, tick);
    else return monitor.on_departure(peer, tick);
}

void departure(const std::filesystem::path& directory) {
    Pair before_zero(directory);
    good(before_zero.left.on_departure(0, 0));
    good(before_zero.left.record(0, digest(0)));
    good(before_zero.left.finish());
    require(!before_zero.left.can_step() && before_zero.left.agreed_end(), "pre-zero local quit tail contract");
    for (const auto kind : {std::uint8_t{34}, net::desync_combined_kind, net::desync_rows_kind}) {
        Pair invalid_tail(directory);
        good(invalid_tail.left.on_departure(1, 0));
        require(!invalid_tail.left.receive(1, std::array<std::uint8_t, 1>{kind}), "malformed departed tail accepted");
    }
    Pair local_quit(directory);
    good(local_quit.left.record(0, digest(0)));
    good(local_quit.right.record(0, digest(0)));
    local_quit.drain();
    net::SessionMonitor& quitting_interface = local_quit.left;
    good(notify_departure(quitting_interface, local_quit.left, 0, 1));
    good(local_quit.left.record(1, digest(1))); // Completed quit-frame record follows the departure hook.
    require(!local_quit.left.can_step() && !local_quit.left.report(), "local quit must stop without a spurious monitor error");

    for (std::uint64_t cutoff : {3ULL, 4ULL}) {
        Pair pending(directory);
        good(pending.left.record(0, digest(0)));
        require(pending.left.waiting() && !pending.left.can_step(), "missing peer must hold setup barrier");
        require(pending.left.waiting_for_peers() == std::vector<net::PeerIndex>{1}, "missing setup peer not exposed for liveness");
        net::SessionMonitor& base = pending.left;
        good(notify_departure(base, pending.left, 1, cutoff));
        good(notify_departure(base, pending.left, 1, cutoff)); // Idempotent agreed notification.
        require(pending.left.setup_agreed() && pending.left.can_step(), "future cutoff did not release pending hash gate");
        require(pending.left.waiting_for_peers().empty(), "departed peer still exposed for liveness");
        for (std::uint64_t tick = 1; tick <= 55; ++tick) good(pending.left.record(tick, digest(tick), {}, {}, tick == 55));
        require(pending.left.agreed_end() && !pending.left.report(), "sole survivor did not catch up and finish exchanges");
        require(pending.bus.inbox[1].size() == 1, "survivor sent hashes to departed peer");
        good(pending.right.record(0, digest(0)));
        auto tail = pending.a.poll(); require(tail.has_value(), "missing in-flight tail");
        good(pending.left.receive(*tail)); // Well-formed stale summary is harmless.
    }
    Pair past(directory);
    good(past.left.record(0, digest(0))); good(past.right.record(0, digest(0))); past.drain();
    good(past.left.record(1, digest(1)));
    require(!past.left.on_departure(1, 0), "past departure cutoff accepted");
    Pair beyond(directory);
    require(!beyond.left.on_departure(1, 9), "departure beyond the D=3 cutoff horizon accepted");
    Pair boundary(directory / "boundary");
    good(boundary.left.record(0, digest(0)));
    good(boundary.left.on_departure(1, 8));
    require(boundary.left.can_step(), "cutoff at 2D+2 remains within the D=3 horizon");

    Bus bus(3);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    const std::vector<std::uint8_t> encoding{4, 5};
    for (std::uint32_t peer = 0; peer < 3; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(bus, peer));
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), Pair::options(directory, 4096)));
    }
    for (std::uint64_t tick = 0; tick <= 60; ++tick) {
        if (tick == 16) for (auto& monitor : monitors) {
            net::SessionMonitor& interface = *monitor;
            good(notify_departure(interface, *monitor, 1, tick));
        }
        for (std::size_t peer = 0; peer < 3; ++peer) {
            if (tick >= 16 && peer == 1) continue;
            good(monitors[peer]->record(tick, digest(tick, peer == 2 && tick == 48), encoding));
        }
        for (unsigned round = 0; round < 10; ++round) for (std::size_t peer = 0; peer < 3; ++peer) {
            while (auto frame = transports[peer]->poll()) good(monitors[peer]->receive(*frame));
        }
    }
    require(monitors[0]->report() && monitors[2]->report(), "survivors did not complete divergence search");
    require(monitors[0]->report()->first_divergent_tick == 48 && monitors[0]->report()->remote_peer == 2,
        "departure renumbered peer identity or first divergence");
    require(monitors[0]->report()->departures.size() == 1 && monitors[0]->report()->departures[0].input_cutoff_tick == 16,
        "report lost the agreed departure cutoff");
    require(monitors[2]->report()->remote_peer == 0 && monitors[2]->frozen(), "survivor report reference peer");
    good(monitors[0]->on_departure(2, 60));
    require(!monitors[0]->frozen() && monitors[0]->can_step() && monitors[0]->report()->diagnostic_only,
        "sole survivor remained frozen only by a departed peer");
}

void retained_departed_rows(const std::filesystem::path& directory) {
    Bus bus(3);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    for (std::uint32_t peer = 0; peer < 3; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(bus, peer));
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), Pair::options(directory, 4096)));
    }
    for (std::uint64_t tick = 0; tick <= 60; ++tick) {
        for (std::size_t peer = 0; peer < 3; ++peer)
            good(monitors[peer]->record(tick, digest(tick, peer == 1 && tick == 48), std::array<std::uint8_t, 1>{7}));
        if (tick == 60) break;
        for (unsigned round = 0; round < 10; ++round) for (std::size_t peer = 0; peer < 3; ++peer)
            while (auto frame = transports[peer]->poll()) good(monitors[peer]->receive(*frame));
    }
    // Deliver summaries only, then the departing peer's authenticated rows to 0.
    for (std::size_t peer = 0; peer < 3; ++peer) while (!bus.inbox[peer].empty()
        && bus.inbox[peer].front().frame[0] == net::desync_combined_kind) {
        auto frame = transports[peer]->poll(); good(monitors[peer]->receive(*frame));
    }
    auto departed = transports[0]->poll();
    require(departed && departed->from == 1, "departed peer's detail stream missing");
    good(monitors[0]->receive(*departed));
    require(!monitors[0]->report(), "diagnosis should still wait for active peer 2");
    good(monitors[2]->on_departure(1, 64)); // Survivor 2 agrees before any departed rows arrive.
    require(monitors[2]->can_step() && !monitors[2]->frozen(), "early departure did not release survivor");
    net::SessionMonitor& interface = *monitors[0];
    good(notify_departure(interface, *monitors[0], 1, 64));
    auto active = transports[0]->poll();
    require(active && active->from == 2, "active peer's detail stream missing");
    good(monitors[0]->receive(*active));
    require(monitors[0]->report() && monitors[0]->report()->first_divergent_tick == 48
        && monitors[0]->report()->remote_peer == 1 && monitors[0]->report()->diagnostic_only,
        "departure discarded valid diagnostic-only rows");
    for (unsigned round = 0; round < 10; ++round) for (std::size_t peer : {0U, 2U})
        while (auto frame = transports[peer]->poll()) good(monitors[peer]->receive(*frame));
    for (std::size_t peer : {0U, 2U}) {
        require(!monitors[peer]->frozen() && monitors[peer]->can_step(), "departure race stopped a survivor");
        good(monitors[peer]->record(61, digest(61)));
    }
    require(!monitors[0]->report()->departed_divergences.empty()
        && bundle_file(monitors[0]->report()->bundle, "report.txt").find("departed peer 1 diverged at tick 48") != std::string::npos,
        "departed diagnostic note missing hashes/tick");
    require(bundle_file(monitors[0]->report()->bundle, "departures.csv").find("1,64\n") != std::string::npos,
        "bundle lost agreed future cutoff");
}
void remote_freeze_without_local_mismatch(const std::filesystem::path& directory) {
    Bus bus(3);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    for (std::uint32_t peer = 0; peer < 3; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(bus, peer));
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), Pair::options(directory, 4096)));
    }
    for (std::uint64_t tick = 0; tick <= 30; ++tick) {
        for (std::size_t peer = 0; peer < 3; ++peer)
            good(monitors[peer]->record(tick, digest(tick, peer == 2 && tick == 20)));
        if (tick == 30) break;
        for (unsigned round = 0; round < 10; ++round) for (std::size_t peer = 0; peer < 3; ++peer)
            while (auto frame = transports[peer]->poll()) good(monitors[peer]->receive(*frame));
    }
    while (auto frame = transports[1]->poll()) good(monitors[1]->receive(*frame));
    auto& inbox = bus.inbox[0];
    for (const auto kind : {net::desync_combined_kind, net::desync_rows_kind}) {
        const auto item = std::find_if(inbox.begin(), inbox.end(), [&](const auto& frame) { return frame.from == 1 && frame.frame[0] == kind; });
        require(item != inbox.end(), "missing identical peer freeze notice");
        auto frame = std::move(*item); inbox.erase(item); good(monitors[0]->receive(frame));
    }
    require(monitors[0]->frozen() && !monitors[0]->report(), "remote-only freeze notice was not gated");
    for (unsigned round = 0; round < 10; ++round) for (std::size_t peer = 0; peer < 3; ++peer)
        while (auto frame = transports[peer]->poll()) good(monitors[peer]->receive(*frame));
    require(monitors[0]->report() && monitors[0]->report()->first_divergent_tick == 20
        && monitors[0]->report()->remote_peer == 2, "remote-only freeze did not complete diagnosis");
}

std::vector<std::uint8_t> row_frame(std::uint64_t first, std::uint64_t last, std::uint64_t fault) {
    std::vector<std::uint8_t> frame{net::desync_rows_kind};
    for (auto tick : {first, last}) for (unsigned shift = 0; shift < 64; shift += 8)
        frame.push_back(static_cast<std::uint8_t>(tick >> shift));
    const auto input = core::sha256_hex(std::span<const std::uint8_t>{});
    for (std::uint64_t tick = first; ; ++tick) {
        const auto state = digest(tick, tick == fault);
        frame.insert(frame.end(), state.begin(), state.end());
        frame.insert(frame.end(), input.begin(), input.end());
        if (tick == last) break;
    }
    return frame;
}

void liveness_waits(const std::filesystem::path& directory) {
    Pair mismatch(directory);
    good(mismatch.left.record(0, digest(0))); good(mismatch.right.record(0, digest(0, true)));
    auto combined = mismatch.a.poll(); require(combined.has_value(), "missing mismatch summary");
    good(mismatch.left.receive(*combined));
    require(mismatch.left.frozen() && mismatch.left.waiting_for_peers() == std::vector<net::PeerIndex>{1},
        "frozen missing row sender not exposed");
    Pair ending(directory);
    good(ending.left.record(0, digest(0))); good(ending.right.record(0, digest(0))); ending.drain();
    good(ending.left.record(1, digest(1), {}, {}, true));
    require(!ending.left.agreed_end() && ending.left.waiting_for_peers() == std::vector<net::PeerIndex>{1},
        "final missing summary sender not exposed");
}

void repair_replay_crosses_a_retired_window(const std::filesystem::path& directory) {
    for (const std::uint64_t older_end : {0ULL, 20ULL}) for (const unsigned contradicts : {0U, 1U, 2U, 3U, 4U}) {
        Pair pair(directory, 4096, 10);
        good(pair.left.on_link_repair(0)); good(pair.right.on_link_repair(1));
        good(pair.left.record(0, digest(0))); good(pair.right.record(0, digest(0))); pair.drain();
        for (std::uint64_t tick = 1; tick <= older_end; ++tick) {
            good(pair.left.record(tick, digest(tick))); good(pair.right.record(tick, digest(tick)));
            if (tick % 10 == 0) pair.drain();
        }
        const auto older_first = older_end == 0 ? 0 : older_end - 9;
        const auto current = older_end + 10;
        for (std::uint64_t tick = older_end + 1; tick <= current; ++tick) {
            good(pair.left.record(tick, digest(tick))); good(pair.right.record(tick, digest(tick)));
        }
        // Only the fast receiver sees the other summary. The sender still needs
        // the receiver's current summary and must retry its retained older one.
        const auto fast = pair.a.poll(); require(fast.has_value(), "missing fast-side summary"); good(pair.left.receive(*fast));
        require(pair.left.peer_confirmed_tick() == current && pair.right.peer_confirmed_tick() == older_end,
            "fixture requires one completed exchange of skew between receiver and sender");
        good(pair.right.on_link_repair(0));
        auto replay = pair.a.poll(); require(replay.has_value(), "missing prior-window retry");
        const auto original = replay->frame;
        if (contradicts == 1) replay->frame[17] = replay->frame[17] == '0' ? '1' : '0';
        if (contradicts == 2) replay->frame = row_frame(older_first, older_end, older_first);
        if (contradicts == 3) replay->frame.back() = 1;
        if (contradicts == 4) replay->frame[1] = static_cast<std::uint8_t>(older_first + 1);
        std::cout << "REPAIR-WINDOW sender=1 confirmed=" << older_end << " current=" << older_end + 1 << ".." << current
            << " receiver=0 confirmed=" << current << " next=" << current + 1 << " retry=" << older_first << ".." << older_end << '\n';
        const auto received = pair.left.receive(*replay);
        if (contradicts) {
            require(!received && received.error().code == "EAWR-NET-0401", "changed retired-window evidence must fail");
            continue;
        }
        good(received);
        good(pair.left.receive(1, row_frame(older_first, older_end, 999)));
        pair.drain();
        require(pair.left.peer_confirmed_tick() == current && pair.right.peer_confirmed_tick() == current,
            "retry cannot change agreement or hold either monitor");
        for (std::uint64_t tick = current + 1; tick <= current + 10; ++tick) {
            good(pair.left.record(tick, digest(tick))); good(pair.right.record(tick, digest(tick)));
        }
        const auto next = pair.a.poll(); require(next.has_value(), "missing next fast-side summary"); good(pair.left.receive(*next));
        good(pair.right.on_link_repair(0)); pair.drain();
        require(pair.left.peer_confirmed_tick() == current + 10 && pair.right.peer_confirmed_tick() == current + 10,
            "retired receipt advances with agreement");
        good(pair.left.receive(1, row_frame(older_end + 1, current, 999)));
        require(!pair.left.receive(1, original), "older-than-retained retries stay refused; no unbounded replay history");
    }
}

void notice_agrees_without_rows(const std::filesystem::path& directory) {
    Bus bus(3); FakeTransport a(bus, 0), b(bus, 1), c(bus, 2);
    net::DesyncMonitor local(a, Pair::options(directory, 4096));
    net::DesyncMonitor peer1(b), peer2(c);
    good(local.record(0, digest(0))); good(peer1.record(0, digest(0))); good(peer2.record(0, digest(0)));
    auto summary1 = a.poll(); require(summary1 && summary1->from == 1, "missing peer1 summary");
    good(local.receive(*summary1));
    const auto rows = row_frame(0, 0, 999);
    good(local.receive(1, rows));
    require(local.frozen() && !local.report() && local.waiting_for_peers() == std::vector<net::PeerIndex>{2},
        "notice without visible mismatch did not wait for active summary");
    auto summary2 = a.poll(); require(summary2 && summary2->from == 2, "missing peer2 summary");
    good(local.receive(*summary2));
    require(local.can_step() && !local.frozen() && !local.report(), "equal summaries unnecessarily waited for detail rows");
    good(local.receive(2, rows)); // The retained agreed window accepts the late active rows.
    require(local.can_step() && !local.report(), "late agreeing rows changed agreement");
}

void report_window_survives_late_note(const std::filesystem::path& directory) {
    Bus bus(3);
    std::vector<std::unique_ptr<FakeTransport>> transports;
    std::vector<std::unique_ptr<net::DesyncMonitor>> monitors;
    for (std::uint32_t peer = 0; peer < 3; ++peer) {
        transports.push_back(std::make_unique<FakeTransport>(bus, peer));
        monitors.push_back(std::make_unique<net::DesyncMonitor>(*transports.back(), Pair::options(directory, 4096)));
    }
    std::optional<net::Received> departed_summary;
    for (std::uint64_t tick = 0; tick <= 60; ++tick) {
        for (std::size_t peer = 0; peer < 3; ++peer) {
            if (peer == 2 && tick > 30) continue;
            good(monitors[peer]->record(tick, digest(tick, (peer == 2 && tick == 10) || (peer == 1 && tick == 50)),
                std::array<std::uint8_t, 1>{static_cast<std::uint8_t>(tick)}));
        }
        if (tick == 30) {
            for (std::size_t peer : {0U, 1U}) {
                auto& inbox = bus.inbox[peer];
                const auto item = std::find_if(inbox.begin(), inbox.end(), [](const auto& frame) { return frame.from == 2; });
                require(item != inbox.end(), "missing delayed departed summary");
                if (peer == 0) departed_summary = *item;
                inbox.erase(item);
                good(monitors[peer]->on_departure(2, 30));
            }
        }
        for (unsigned round = 0; round < 10; ++round) for (std::size_t peer = 0; peer < (tick < 30 ? 3U : 2U); ++peer)
            while (auto frame = transports[peer]->poll()) good(monitors[peer]->receive(*frame));
    }
    require(monitors[0]->report() && monitors[0]->report()->first_divergent_tick == 50, "missing active incident");
    good(monitors[0]->receive(*departed_summary));
    good(monitors[0]->receive(2, row_frame(1, 30, 10)));
    const auto& report = *monitors[0]->report();
    require(!report.diagnostic_only && report.first_divergent_tick == 50 && report.departed_divergences.size() == 1,
        "late departed note replaced active incident");
    require(bundle_file(report.bundle, "hashes.csv").find(",50,") != std::string::npos,
        "late previous-window note replaced the incident hash rows");
    require(bundle_file(report.bundle, "report.txt").find("departed peer 2 diverged at tick 10") != std::string::npos,
        "late departed note not exported");
}

void adaptive_departure_bounds() {
    for (const auto& delays : {std::pair{8U, 8U}, std::pair{4U, 8U}, std::pair{4U, 4U}}) {
        Bus bus;
        FakeTransport transport(bus, 0);
        net::DesyncMonitor monitor(transport);
        good(monitor.on_input_delay(delays.first, delays.second));
        const auto cutoff = net::sealed_input_horizon(delays.second);
        good(monitor.on_departure(1, cutoff));
        Bus outside_bus;
        FakeTransport outside_transport(outside_bus, 0);
        net::DesyncMonitor outside(outside_transport);
        good(outside.on_input_delay(delays.first, delays.second));
        require(!outside.on_departure(1, cutoff + 1), "adaptive monitor retained horizon rejects unreachable cutoff");
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: desync_monitor_tests fixture artifact-directory");
        const std::filesystem::path directory = argv[2];
        peer_confirmation(directory);
        adaptive_departure_bounds();
        scripted_battle(argv[1], directory);
        scripted_battle(argv[1], directory, true);
        synthetic(directory, 0, 90);
        synthetic(directory, 48, 90); // transient state fault reconverges before tick 60
        synthetic(directory, 48, 55); // last partial window
        synthetic(directory, 48, 90, true); // world agrees, canonical input differs
        synthetic(directory, 48, 90, false, 1);
        synthetic(directory, 48, 90, false, 7);
        synthetic(directory, 48, 55, false, 100);
        malformed_and_bounds(directory);
        asynchronous_peers(directory, false);
        asynchronous_peers(directory, true);
        departure(directory);
        retained_departed_rows(directory);
        remote_freeze_without_local_mismatch(directory);
        liveness_waits(directory);
        repair_replay_crosses_a_retired_window(directory);
        notice_agrees_without_rows(directory);
        report_window_survives_late_note(directory);
        std::cout << "desync monitor contracts passed\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
