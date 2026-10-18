#pragma once

#include "fake_transport.hpp"

#include <map>
#include <vector>

namespace eawr::testing {

// An N-peer room mesh where single pairs can be cut. Each seat sees only its own pairs: a cut pair reads closed at both
// ends (after an optional per-end detection delay), sends over it fail, and frames still in flight over it are dropped.
// One round is one fake millisecond, as in FakeNetwork. Nothing here is a global view: a seat cannot learn about a pair it
// is not part of except through frames delivered over its own open pairs.
class PairNetwork final : public FakeNetwork {
public:
    explicit PairNetwork(std::uint32_t count)
        : FakeNetwork(count), cut_(count, std::vector<bool>(count)), since_(count, std::vector<std::uint64_t>(count)),
          delay_(count, std::vector<std::uint64_t>(count)), silent_(count, std::vector<bool>(count)) {}
    // The pair closes at both ends; `late_a` / `late_b` delay when each end notices.
    void cut(net::PeerIndex a, net::PeerIndex b, std::uint64_t late_a = 0, std::uint64_t late_b = 0) {
        cut_[a][b] = cut_[b][a] = true;
        since_[a][b] = since_[b][a] = round;
        delay_[a][b] = late_a; delay_[b][a] = late_b;
    }
    std::map<std::pair<net::PeerIndex, net::PeerIndex>, std::uint64_t> generations;
    std::map<std::pair<net::PeerIndex, net::PeerIndex>, unsigned> failed_repairs;
    void heal(net::PeerIndex a, net::PeerIndex b) { ++generations[{a, b}]; ++generations[{b, a}]; cut_[a][b] = cut_[b][a] = false; silent_[a][b] = silent_[b][a] = false; }
    // The pair reads closed at `closed_end` at once and stays open at `open_end`, whose sends succeed but never arrive.
    void half_open(net::PeerIndex closed_end, net::PeerIndex open_end) {
        cut(closed_end, open_end, 0, 100'000'000);
        silent_[open_end][closed_end] = true;
    }
    // Both directions of the pair swallow every frame while both ends still read it open and their sends succeed: the relay
    // silence of #4021 before a watchdog notices. heal() ends it; cut() is what the watchdog does afterwards.
    void go_silent(net::PeerIndex a, net::PeerIndex b) { silent_[a][b] = silent_[b][a] = true; }
    [[nodiscard]] bool is_silent(net::PeerIndex from, net::PeerIndex to) const { return silent_[from][to]; }
    void isolate(net::PeerIndex seat) {
        for (net::PeerIndex other = 0; other < cut_.size(); ++other) if (other != seat) cut(seat, other);
    }
    [[nodiscard]] bool is_cut(net::PeerIndex a, net::PeerIndex b) const { return cut_[a][b]; }
    [[nodiscard]] bool seen_closed(net::PeerIndex seat, net::PeerIndex peer) const {
        return cut_[seat][peer] && round >= since_[seat][peer] + delay_[seat][peer];
    }
    // Frames sent per (sender, kind), for assertions about who announced what.
    std::map<std::pair<net::PeerIndex, std::uint8_t>, std::size_t> sent;
    // Called with every frame that was delivered into a seat's inbox; may cut pairs (e.g. when a proposal is sent).
    std::function<void(net::PeerIndex, net::PeerIndex, std::span<const std::uint8_t>)> on_send;
private:
    std::vector<std::vector<bool>> cut_;
    std::vector<std::vector<std::uint64_t>> since_, delay_;
    std::vector<std::vector<bool>> silent_;
};

class PairTransport final : public net::MessageTransport {
public:
    PairTransport(PairNetwork& network, net::PeerIndex index) : network_(&network), self_(index) {}
    core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> bytes) override {
        if (to < network_->inbox.size() && to != self_ && network_->is_silent(self_, to)) return core::Result<void>::success();
        if (to == self_ || to >= network_->inbox.size() || network_->is_cut(self_, to)) {
            core::Diagnostic error;
            error.code = "EAWR-TEST-PAIR-CUT";
            error.message = "the pair is cut";
            return core::Result<void>::failure(std::move(error));
        }
        if (!bytes.empty()) ++network_->sent[{self_, bytes.front()}];
        network_->deliver(self_, to, bytes);
        if (network_->on_send) network_->on_send(self_, to, bytes);
        return core::Result<void>::success();
    }
    std::optional<net::Received> poll() override {
        auto& inbox = network_->inbox[self_];
        for (;;) {
            const auto entry = std::min_element(inbox.begin(), inbox.end(),
                [](const auto& left, const auto& right) { return left.due < right.due; });
            if (entry == inbox.end() || entry->due > network_->round) return std::nullopt;
            auto received = std::move(entry->received);
            inbox.erase(entry);
            // A frame in flight over a pair that was cut meanwhile never arrives.
            if (network_->is_cut(self_, received.from)) continue;
            return received;
        }
    }
    net::LinkState link(net::PeerIndex peer) const override {
        return peer == self_ || network_->seen_closed(self_, peer) ? net::LinkState::closed : net::LinkState::open;
    }
    net::PeerIndex self() const override { return self_; }
    void fail_repair(net::PeerIndex peer, unsigned code) override {
        network_->failed_repairs[{self_, peer}] = code; network_->cut(self_, peer);
    }
    std::uint64_t edge_identity(net::PeerIndex peer) const override {
        const auto found = network_->generations.find({self_, peer});
        return found == network_->generations.end() ? 1 : found->second + 1;
    }
    std::uint32_t peer_count() const override { return static_cast<std::uint32_t>(network_->inbox.size()); }
private:
    PairNetwork* network_{};
    net::PeerIndex self_{};
};

} // namespace eawr::testing
