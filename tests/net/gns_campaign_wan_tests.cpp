#include "eawr/net/gns_transport.hpp"
#include "eawr/net/desync_monitor.hpp"
#include "eawr/net/lockstep_session.hpp"
#include "eawr/core/sha256.hpp"
#include "../support/process_cpu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

// Two real native transports and the same hash/cadence gates as campaign play.
namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using Bytes = std::vector<std::uint8_t>;
template<class T> T take(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}

class Adapter final : public SessionAdapter {
public:
    std::uint64_t tick{};
    Bytes state, input;
    std::uint64_t completed_tick() const override { return tick; }
    std::string state_hash() const override { return eawr::core::sha256_hex(state); }
    Bytes canonical_state() const override { return state; }
    eawr::core::Result<void> validate(const SeatInput&) const override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> install(std::span<const SeatInput> commands) override {
        input.clear();
        for (const auto& command : commands) {
            input.push_back(static_cast<std::uint8_t>(command.key.seat.value));
            input.insert(input.end(), command.bytes.begin(), command.bytes.end());
        }
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<Bytes> canonical_input() const override { return eawr::core::Result<Bytes>::success(input); }
    eawr::core::Result<CompletedTick> step() override {
        state.insert(state.end(), input.begin(), input.end());
        return eawr::core::Result<CompletedTick>::success({++tick, state_hash(), false});
    }
    bool is_departure(const SeatInput&) const override { return false; }
};

class Transport final : public MessageTransport {
public:
    explicit Transport(GnsTransport& value) : native(value) {}
    GnsTransport& native;
    std::deque<Received> early;
    Clock::time_point last_poll{};
    double max_poll_ms{};
    eawr::core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> bytes) override { return native.send(to, bytes); }
    std::optional<Received> poll() override {
        const auto now = Clock::now();
        if (last_poll != Clock::time_point{}) max_poll_ms = std::max(max_poll_ms,
            std::chrono::duration<double, std::milli>(now - last_poll).count());
        last_poll = now;
        if (early.empty()) return native.poll();
        auto frame = std::move(early.front()); early.pop_front(); return frame;
    }
    LinkState link(PeerIndex peer) const override { return native.link(peer); }
    PeerIndex self() const override { return native.self(); }
    std::uint32_t peer_count() const override { return native.peer_count(); }
    std::optional<LinkStats> link_stats(PeerIndex peer) const override { return native.link_stats(peer); }
};

class Gate final : public SessionMonitor {
public:
    Gate(MessageTransport& transport, std::uint64_t every, bool deferred, std::uint64_t final) : monitor(transport, options(every, deferred, final)) {}
    static DesyncOptions options(std::uint64_t every, bool deferred, std::uint64_t final) {
        DesyncOptions value; value.exchange_every = every; value.deferred_compare = deferred; value.final_tick = final; return value;
    }
    DesyncMonitor monitor;
    std::uint64_t exchange_points{};
    bool due{true};
    bool needs_encoding(std::uint64_t tick, bool end) const override { return monitor.needs_encoding(tick, end); }
    eawr::core::Result<void> record(std::uint64_t tick, std::string_view hash, std::span<const std::uint8_t> state,
        std::span<const std::uint8_t> input, bool end) override {
        if (monitor.needs_encoding(tick, end)) ++exchange_points;
        return monitor.record(tick, hash, state, input, end);
    }
    eawr::core::Result<void> receive(const Received& frame) override { return monitor.receive(frame); }
    eawr::core::Result<void> on_input_delay(std::uint64_t delay, std::uint64_t retained) override {
        return monitor.on_input_delay(delay, retained);
    }
    eawr::core::Result<void> on_activated_input_delay(std::uint64_t delay, std::uint64_t retained) override {
        return monitor.on_activated_input_delay(delay, retained);
    }
    std::vector<PeerIndex> waiting_for_peers() const override { return monitor.waiting_for_peers(); }
    bool frozen() const override { return monitor.frozen(); }
    bool may_step() const override { return due && monitor.may_step(); }
};

template<class Timing> void responsive(Timing& timing) {
    // The identical fixture also compiles against the original baseline policy.
    if constexpr (requires { timing.responsive; }) timing.responsive = true;
}

Clock::time_point shared_start(Transport& transport, bool host, PeerIndex remote) {
    // Same-host processes share the steady clock. A future common epoch removes
    // connection-handshake skew from the deliberately offset pacing deadlines.
    Bytes frame{250};
    if (host) {
        const auto epoch = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            (Clock::now() + 1500ms).time_since_epoch()).count());
        for (unsigned shift = 0; shift < 64; shift += 8) frame.push_back(static_cast<std::uint8_t>(epoch >> shift));
        if (!transport.send(remote, frame)) throw std::runtime_error("WAN shared epoch send failed");
    }
    const auto timeout = Clock::now() + 5s;
    while (true) {
        if (auto received = transport.poll()) {
            if (received->frame.size() != 9 || received->frame.front() != 250)
                throw std::runtime_error("WAN shared epoch frame differs");
            if (host && received->frame != frame) throw std::runtime_error("WAN shared epoch acknowledgement differs");
            frame = std::move(received->frame);
            if (!host && !transport.send(remote, frame)) throw std::runtime_error("WAN shared epoch acknowledgement failed");
            std::uint64_t epoch{};
            for (unsigned byte = 0; byte < 8; ++byte) epoch |= std::uint64_t{frame[byte + 1]} << (byte * 8);
            const auto start = Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(epoch)));
            if (start <= Clock::now() || start > Clock::now() + 2s) throw std::runtime_error("WAN processes do not share a future steady epoch");
            return start;
        }
        if (Clock::now() > timeout) throw std::runtime_error("WAN shared epoch deadline");
        std::this_thread::sleep_for(1ms);
    }
}

int peer(bool host, std::uint16_t port, std::uint64_t ticks, std::string_view policy, bool continuous,
    std::uint64_t every, std::uint64_t phase_lag_ms, bool deferred) {
    GnsOptions config; config.build_identity = "campaign-wan-timing"; config.timeout = 20s; config.handshake_timeout = 20s;
    auto native = host ? take(GnsTransport::host("127.0.0.1", port, config))
                       : take(GnsTransport::connect("127.0.0.1", port, config));
    if (host) std::cout << "PORT " << native->port() << std::endl;
    Transport transport(*native);
    const auto remote = host ? 1U : 0U;
    const auto deadline = Clock::now() + 120s;
    while (native->link(remote) != LinkState::open) {
        if (auto frame = native->poll()) transport.early.push_back(std::move(*frame));
        if (native->diagnostic()) throw std::runtime_error(native->diagnostic()->message);
        if (Clock::now() > deadline) throw std::runtime_error("campaign WAN handshake deadline");
        std::this_thread::sleep_for(1ms);
    }
    const auto epoch = shared_start(transport, host, remote);
    Adapter adapter;
    Gate gate(transport, every, deferred, ticks);
    SessionOptions options;
    options.input_delay = policy == "skirmish" ? 4 : 3;
    // Phase qualification needs lead for 100 ms phase + 60 ms one-way delay.
    // Hold D8 in both interval arms so input waits do not collapse the phase.
    if (phase_lag_ms) options.input_delay = 8;
    options.final_tick_count = ticks; options.keep_admission_tick_open = true;
    if (policy != "fixed" && !phase_lag_ms) {
        options.non_lan = true;
        options.adaptive_timing.emplace();
        options.adaptive_timing->hold_on_expiry = true;
        if (policy == "adaptive") responsive(*options.adaptive_timing);
    }
    std::uint64_t retained{};
    options.retain_timing_event = [&](auto, auto) { ++retained; return eawr::core::Result<void>::success(); };
    auto session = take(LockstepSession::create(adapter, transport, {{{1}, true}, {{2}, true}}, options, &gate));
    // Same inputs at the same initial delay in both arms; the remaining sealed
    // batches are noops, so the canonical hash compares policy independently.
    take(session.submit([](auto) { return eawr::core::Result<Bytes>::success({42}); }));
    const auto start = Clock::now();
    const auto cpu_start = eawr::testing::process_cpu_ms();
    auto next = epoch + 33333us + (host ? 0ms : std::chrono::milliseconds(phase_lag_ms));
    auto last_completion = start;
    double pacing_sum{}, pacing_squared{}, pacing_max{};
    std::uint64_t pacing_samples{}, hash_due_ms{}, hash_due_stalls{}, hash_due_at{}, hash_due_tick{}, previous_ms{};
    bool hash_due_wait{};
    std::uint64_t stalls{}, stalled_ms{}, longest{}, wait_start{};
    std::array<std::uint64_t, 5> class_stalls{}, class_ms{};
    std::optional<std::size_t> wait_class;
    std::uint64_t class_at{};
    bool waiting{};
    while (adapter.tick < ticks) {
        const auto now = Clock::now();
        if (now > deadline) throw std::runtime_error("campaign WAN tick deadline");
        const auto ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        if (hash_due_wait) hash_due_ms += ms - previous_ms;
        previous_ms = ms;
        gate.due = now >= next;
        if (!gate.due && !continuous) { std::this_thread::sleep_for(1ms); continue; }
        const auto result = take(session.pump(ms));
        const bool hash_blocked = gate.due && adapter.tick != 0 && !result.completed
            && !gate.monitor.waiting_for_peers().empty();
        if (hash_blocked && !hash_due_wait) { ++hash_due_stalls; hash_due_at = ms; hash_due_tick = adapter.tick; }
        if (!hash_blocked && hash_due_wait)
            std::cout << "HASH_DUE tick=" << hash_due_tick << " blocked_ms=" << ms - hash_due_at << '\n';
        hash_due_wait = hash_blocked;
        std::optional<std::size_t> reason;
        if (!gate.monitor.waiting_for_peers().empty()) reason = 2;
        else if (gate.due && session.waiting_for() && !session.waiting_for()->peers.empty())
            reason = native->queue_progress(remote).inbound_frames ? 1 : 0;
        else if (gate.due && !result.completed) reason = session.transition_pending() ? 3 : 4;
        if (reason != wait_class) {
            if (wait_class) class_ms[*wait_class] += ms - class_at;
            if (reason) ++class_stalls[*reason];
            wait_class = reason; class_at = ms;
        }
        const bool blocked = reason.has_value();
        if (blocked && !waiting) { waiting = true; wait_start = ms; ++stalls; }
        if (!blocked && waiting) {
            const auto duration = ms - wait_start;
            stalled_ms += duration; longest = std::max(longest, duration); waiting = false;
        }
        if (result.completed) {
            std::cout << "TICK tick=" << adapter.tick << " epoch_ns="
                << std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count() << '\n';
            if (adapter.tick > 1) {
                const auto gap = std::chrono::duration<double, std::milli>(now - last_completion).count();
                ++pacing_samples; pacing_sum += gap; pacing_squared += gap * gap; pacing_max = std::max(pacing_max, gap);
            }
            last_completion = now;
            next += 33333us;
            if (now > next + 4 * 33333us) next = now;
        }
        std::this_thread::sleep_for(2ms);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - epoch).count();
    std::cout << "RESULT seat=" << transport.self() << " ticks=" << adapter.tick << " elapsed_ms=" << elapsed
        << " stalls=" << stalls << " wait_ms=" << stalled_ms << " longest_ms=" << longest
        << " delay=" << session.input_delay() << " timing_events=" << retained
        << " cpu_ms=" << eawr::testing::process_cpu_ms() - cpu_start << " hash=" << adapter.state_hash() << std::endl;
    constexpr std::array names{"batch", "queued", "hash", "timing", "other"};
    std::cout << "CLASSES max_poll_ms=" << transport.max_poll_ms;
    const auto pacing_mean = pacing_samples ? pacing_sum / static_cast<double>(pacing_samples) : 0.0;
    const auto pacing_variance = pacing_samples ? std::max(0.0, pacing_squared / static_cast<double>(pacing_samples)
        - pacing_mean * pacing_mean) : 0.0;
    std::cout << " interval=" << every << " phase_lag_ms=" << phase_lag_ms
        << " exchange_points=" << gate.exchange_points
        << " hash_due_ms=" << hash_due_ms << " hash_due_stalls=" << hash_due_stalls
        << " pacing_mean_ms=" << pacing_mean << " pacing_stddev_ms=" << std::sqrt(pacing_variance)
        << " pacing_max_ms=" << pacing_max;
    for (std::size_t index = 0; index < names.size(); ++index)
        std::cout << ' ' << names[index] << "_stalls=" << class_stalls[index] << ' ' << names[index] << "_ms=" << class_ms[index];
    std::cout << std::endl;
    const auto drain = Clock::now() + 2s;
    while (Clock::now() < drain) { static_cast<void>(native->poll()); std::this_thread::sleep_for(1ms); }
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (!GnsTransport::available()) { std::cout << "GNS disabled" << std::endl; return 77; }
        if (argc < 5 || argc > 10) throw std::runtime_error("expected host/client port ticks [policy] [polling] [interval] [phase-lag-ms] [deferred]");
        return peer(std::string_view(argv[2]) == "host", static_cast<std::uint16_t>(std::stoul(argv[3])),
            std::stoull(argv[4]), argc == 5 ? "adaptive" : std::string_view(argv[5]), argc >= 7 && std::string_view(argv[6]) == "continuous",
            argc >= 8 && std::stoull(argv[7]) != 0 ? std::stoull(argv[7]) : default_hash_exchange_every,
            argc >= 9 ? std::stoull(argv[8]) : 0,
            argc >= 10 && std::string_view(argv[9]) != "default" ? std::string_view(argv[9]) == "on" : default_deferred_compare);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
