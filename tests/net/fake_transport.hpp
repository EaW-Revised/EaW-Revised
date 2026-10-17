#pragma once

#include "eawr/net/transport.hpp"
#include <algorithm>
#include <functional>

namespace eawr::testing {

struct FakeDelivery {
    bool drop{};
    bool duplicate{};
    std::uint64_t ordered_delay{};
    std::uint64_t duplicate_delay{};
    std::uint64_t reorder_delay{}; // Deliberately violates the stream contract.
};

// One round is one fake millisecond. Loss is recovered by the reliable stream;
// physical arrival reordering is held behind the preceding complete frame.
struct WanDirection {
    std::uint64_t latency_ms{}, jitter_ms{}, loss_every{}, retransmit_ms{}, reorder_every{}, reorder_ms{};
};
struct WanProfile {
    const char* name;
    WanDirection forward, reverse;
};
inline constexpr WanProfile wan_profiles[]{
    {"lan", {1, 0, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 0}},
    {"dsl-80ms", {40, 8, 17, 80, 7, 20}, {40, 4, 19, 80, 11, 16}},
    {"mobile-200ms-jitter", {90, 45, 9, 200, 5, 80}, {110, 60, 13, 240, 7, 90}}
};

class FakeNetwork {
public:
    struct Entry {
        std::uint64_t due{};
        net::Received received;
    };
    explicit FakeNetwork(std::uint32_t count)
        : inbox(count), frontier(count, std::vector<std::uint64_t>(count)), open(count, true) {}
    std::uint64_t round{};
    std::vector<std::vector<Entry>> inbox;
    std::vector<std::vector<std::uint64_t>> frontier;
    std::vector<bool> open;
    std::function<FakeDelivery(net::PeerIndex, net::PeerIndex, std::span<const std::uint8_t>)> fault;
    std::function<void(net::PeerIndex, net::PeerIndex, std::span<const std::uint8_t>)> before_send;
    std::function<bool(net::PeerIndex, net::PeerIndex, std::span<const std::uint8_t>)> fail_send;
    std::function<void(net::PeerIndex)> before_poll;

    void emulate(WanProfile profile) {
        wan_ = profile;
        sequence_.assign(inbox.size(), std::vector<std::uint64_t>(inbox.size()));
    }

    void deliver(net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t> bytes) {
        const auto action = fault ? fault(from, to, bytes) : FakeDelivery{};
        std::uint64_t delay = 1 + action.ordered_delay;
        if (wan_) {
            const auto& direction = from < to ? wan_->forward : wan_->reverse;
            const auto sequence = ++sequence_[from][to];
            // A reproducible sweep across the configured jitter range, different
            // on each direction; no clock, RNG or simulation state is consulted.
            const auto variation = direction.jitter_ms == 0 ? 0 :
                (sequence * 13 + from * 7 + to * 3) % (2 * direction.jitter_ms + 1);
            delay += direction.latency_ms - std::min(direction.latency_ms, direction.jitter_ms) + variation;
            if (direction.loss_every && sequence % direction.loss_every == 0) delay += direction.retransmit_ms;
            if (direction.reorder_every && sequence % direction.reorder_every == 0) delay += direction.reorder_ms;
        }
        auto due = std::max(round + delay, frontier[from][to]);
        frontier[from][to] = due;
        if (action.drop) return;
        net::Received received{from, {bytes.begin(), bytes.end()}};
        inbox[to].push_back({due + action.reorder_delay, received});
        if (action.duplicate) inbox[to].push_back({due + action.duplicate_delay, std::move(received)});
    }
private:
    std::optional<WanProfile> wan_;
    std::vector<std::vector<std::uint64_t>> sequence_;
};

class FakeTransport final : public net::MessageTransport {
public:
    FakeTransport(FakeNetwork& network, net::PeerIndex index) : network_(&network), self_(index) {}
    core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> bytes) override {
        if (network_->before_send) network_->before_send(self_, to, bytes);
        if (network_->fail_send && network_->fail_send(self_, to, bytes)) {
            core::Diagnostic error;
            error.code = "EAWR-TEST-FAKE-SEND";
            error.message = "injected fake send failure";
            return core::Result<void>::failure(std::move(error));
        }
        if (!network_->open[to]) {
            core::Diagnostic error;
            error.code = "EAWR-TEST-FAKE-CLOSED";
            error.message = "fake destination stream is closed";
            return core::Result<void>::failure(std::move(error));
        }
        network_->deliver(self_, to, bytes);
        return core::Result<void>::success();
    }
    std::optional<net::Received> poll() override {
        if (network_->before_poll) network_->before_poll(self_);
        auto& inbox = network_->inbox[self_];
        const auto entry = std::min_element(inbox.begin(), inbox.end(),
            [](const auto& left, const auto& right) { return left.due < right.due; });
        if (entry == inbox.end() || entry->due > network_->round) return std::nullopt;
        auto received = std::move(entry->received);
        inbox.erase(entry);
        return received;
    }
    net::LinkState link(net::PeerIndex peer) const override {
        return network_->open[peer] ? net::LinkState::open : net::LinkState::closed;
    }
    net::PeerIndex self() const override { return self_; }
    std::uint32_t peer_count() const override { return static_cast<std::uint32_t>(network_->inbox.size()); }
private:
    FakeNetwork* network_{};
    net::PeerIndex self_{};
};

} // namespace eawr::testing
