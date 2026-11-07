#pragma once

#include "galactic_lan_match.hpp"
#include "eawr/platform/live_game_session.hpp"
#include <array>
#include <deque>
#include <mutex>

namespace eawr::presentation::godot_backend {
// Capture-only paired in-memory transport; each endpoint is driven on its own live thread.
class ClockCapturePair {
    struct Pipes { std::mutex mutex; std::array<std::deque<net::Received>, 2> inbox; };
    class Transport final : public net::MessageTransport {
    public:
        Transport(Pipes& pipes, net::PeerIndex peer) : pipes_(pipes), peer_(peer) {}
        core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> data) override {
            const std::lock_guard lock(pipes_.mutex);
            pipes_.inbox.at(to).push_back({peer_, {data.begin(), data.end()}});
            return core::Result<void>::success();
        }
        std::optional<net::Received> poll() override {
            const std::lock_guard lock(pipes_.mutex);
            auto& queue = pipes_.inbox.at(peer_);
            if (queue.empty()) return {};
            auto value = std::move(queue.front()); queue.pop_front(); return value;
        }
        net::LinkState link(net::PeerIndex) const override { return net::LinkState::open; }
        net::PeerIndex self() const override { return peer_; }
        std::uint32_t peer_count() const override { return 2; }
    private:
        Pipes& pipes_;
        net::PeerIndex peer_;
    };
public:
    ClockCapturePair() : transport_{Transport(pipes_, 0), Transport(pipes_, 1)} {}
    ~ClockCapturePair() { if (remote_) remote_->stop(); }
    bool start(const sim::galactic::GalacticSetup& setup, sim::SeatId other_seat,
               net::PeerIndex local_peer, platform::LiveGameSession::Options& local,
               std::shared_ptr<GalacticLanMatch>& match, std::string& failure) {
        GalacticLanOptions options;
        options.seats = {{{1}, true}, {other_seat, true}};
        options.exchange_every = 100;
        match = std::make_shared<GalacticLanMatch>(transport_[local_peer], options);
        remote_match_ = std::make_shared<GalacticLanMatch>(transport_[1U - local_peer], options);
        local.step_driver = match->driver();
        platform::LiveGameSession::Options remote_options;
        remote_options.pacing = platform::LiveGameSession::Pacing::driven;
        remote_options.step_driver = remote_match_->driver();
        auto result = platform::LiveGameSession::start(setup, std::move(remote_options));
        if (!result) { failure = result.error().message; return false; }
        remote_ = std::move(result).value();
        remote_->advance_to(10);
        return true;
    }
private:
    Pipes pipes_;
    std::array<Transport, 2> transport_;
    std::shared_ptr<GalacticLanMatch> remote_match_;
    std::unique_ptr<platform::LiveGameSession> remote_;
};
}
