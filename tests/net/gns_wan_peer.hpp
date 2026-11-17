#pragma once

#include "eawr/net/lockstep_session.hpp"
#include "eawr/core/sha256.hpp"

// Real LockstepSession with a tiny deterministic mode: this isolates delivery
// from simulation cost while retaining sealing, completeness and canonical order.
namespace wan_test {
using namespace eawr::net;
using Bytes = std::vector<std::uint8_t>;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
class Buffered final : public MessageTransport {
public:
    explicit Buffered(GnsTransport& value) : native(value) {}
    GnsTransport& native;
    std::deque<Received> early;
    eawr::core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> bytes) override { return native.send(to, bytes); }
    std::optional<Received> poll() override {
        if (early.empty()) return native.poll();
        auto frame = std::move(early.front()); early.pop_front(); return frame;
    }
    LinkState link(PeerIndex peer) const override { return native.link(peer); }
    PeerIndex self() const override { return native.self(); }
    std::uint32_t peer_count() const override { return native.peer_count(); }
};
class Adapter final : public SessionAdapter {
public:
    std::uint64_t tick{};
    Bytes state, input;
    std::uint64_t completed_tick() const override { return tick; }
    std::string state_hash() const override { return eawr::core::sha256_hex(state); }
    Bytes canonical_state() const override { return state; }
    eawr::core::Result<void> validate(const SeatInput&) const override { return eawr::core::Result<void>::success(); }
    eawr::core::Result<void> install(std::span<const SeatInput> inputs) override {
        input.clear();
        for (const auto& command : inputs) {
            input.push_back(static_cast<std::uint8_t>(command.key.seat.value));
            input.insert(input.end(), command.bytes.begin(), command.bytes.end());
        }
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<Bytes> canonical_input() const override { return eawr::core::Result<Bytes>::success(input); }
    eawr::core::Result<CompletedTick> step() override {
        state.insert(state.end(), input.begin(), input.end());
        ++tick;
        return eawr::core::Result<CompletedTick>::success({tick, state_hash(), false});
    }
    bool is_departure(const SeatInput&) const override { return false; }
};
template<class T> T take(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}
inline void require(eawr::core::Result<void> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
}
inline int peer(bool host, std::uint16_t port, std::uint64_t ticks, std::size_t bulk_bytes = 0) {
    const auto sink = GnsTransport::set_log_sink([](std::string_view line) {
        if (line.find("event=gns-stats ") != std::string_view::npos || line.find("event=gns-quality ") != std::string_view::npos)
            std::cout << line << '\n';
    });
    GnsOptions config; config.build_identity = "wan-contract";
    config.timeout = 20s; config.handshake_timeout = 20s;
    auto transport = host ? take(GnsTransport::host("127.0.0.1", port, config))
                          : take(GnsTransport::connect("127.0.0.1", port, config));
    if (host) std::cout << "PORT " << transport->port() << std::endl;
    Buffered buffered(*transport);
    const auto remote = host ? 1U : 0U;
    const auto deadline = Clock::now() + 180s;
    while (transport->link(remote) != LinkState::open) {
        if (auto frame = transport->poll()) buffered.early.push_back(std::move(*frame));
        if (transport->diagnostic()) throw std::runtime_error(transport->diagnostic()->message);
        if (Clock::now() > deadline) throw std::runtime_error("WAN handshake deadline");
        std::this_thread::sleep_for(1ms);
    }
    if (bulk_bytes) {
        Bytes bulk(bulk_bytes, 200);
        require(transport->send(remote, bulk));
        bool received{};
        const auto start = Clock::now();
        while (!received) {
            if (auto frame = buffered.poll()) {
                if (frame->frame != bulk) throw std::runtime_error("WAN bulk bytes differ");
                received = true;
            }
            if (transport->diagnostic()) throw std::runtime_error(transport->diagnostic()->message);
            if (Clock::now() > deadline) throw std::runtime_error("WAN bulk deadline");
            std::this_thread::sleep_for(1ms);
        }
        std::cout << "BULK bytes=" << bulk_bytes << " elapsed_ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count() << std::endl;
    }
    Adapter adapter;
    SessionOptions session_config;
    session_config.input_delay = 8; session_config.final_tick_count = ticks;
    auto session = take(LockstepSession::create(adapter, buffered, {{{1}, true}, {{2}, true}}, session_config));
    const auto start = Clock::now();
    auto next = start;
    std::uint64_t submitted = ticks, stalls{}, wait_ms{}, longest{}, wait_start{};
    std::uint64_t loss_samples{}, last_sample{};
    double loss_sum{}, loss_max{};
    bool waiting{};
    while (adapter.tick < ticks) {
        const auto now = Clock::now();
        if (now > deadline) throw std::runtime_error("WAN lockstep deadline");
        if (now < next) { std::this_thread::sleep_for(1ms); continue; }
        const auto ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count());
        if (submitted != adapter.tick && adapter.tick + session.input_delay() < ticks) {
            take(session.submit([](eawr::sim::SeatCommandKey key) {
                Bytes bytes(16, static_cast<std::uint8_t>(key.tick));
                return eawr::core::Result<Bytes>::success(std::move(bytes));
            }));
            submitted = adapter.tick;
        }
        const auto result = take(session.pump(ms));
        if (const auto stats = transport->link_stats(remote); stats && stats->sample_count != last_sample) {
            last_sample = stats->sample_count; ++loss_samples;
            loss_sum += stats->loss_pct; loss_max = std::max(loss_max, stats->loss_pct);
        }
        if (result.completed) {
            if (waiting) { const auto duration = ms - wait_start; wait_ms += duration; longest = std::max(longest, duration); waiting = false; }
            next = now + 33333us;
        } else {
            if (!waiting) { waiting = true; wait_start = ms; ++stalls; }
            next = now + 1ms;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    std::cout << "RESULT seat=" << transport->self() << " ticks=" << adapter.tick << " elapsed_ms=" << elapsed
              << " stalls=" << stalls << " wait_ms=" << wait_ms << " longest_ms=" << longest
              << " quality_bad_avg_pct=" << (loss_samples ? loss_sum / static_cast<double>(loss_samples) : 0.0)
              << " quality_bad_max_pct=" << loss_max
              << " hash=" << adapter.state_hash() << std::endl;
    // Keep servicing native ACKs and the final reliable originals after the
    // faster peer finishes, so the slower peer can complete the same input log.
    const auto drain = Clock::now() + 3s;
    while (Clock::now() < drain) { static_cast<void>(transport->poll()); std::this_thread::sleep_for(1ms); }
    GnsTransport::clear_log_sink(sink);
    return 0;
}
} // namespace wan_test
