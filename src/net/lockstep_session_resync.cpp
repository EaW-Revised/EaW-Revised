#include "lockstep_session_internal.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <limits>

namespace eawr::net {
using namespace lockstep_detail;

core::Result<void> LockstepSession::resync_broadcast(std::span<const std::uint8_t> frame) {
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || departed_[peer]) continue;
        if (auto sent = transport_->send(peer, frame); !sent) return sent;
    }
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::start_resync(std::uint64_t epoch, std::uint64_t tick) {
    // Keep the refused epoch too: other voters must learn the failure immediately.
    refused_resync_epoch_ = epoch;
    if (!options_.resync || !monitor_ || held_ || agreement_
        || (options_.reconnect && options_.reconnect->rejoin_source))
        return refused<void>(resync_errors::overlap, "desync recovery overlaps a membership incident or is not enabled");
    if (!reconnect_available_ || reconnect_log_.size() != tick)
        return refused<void>(resync_errors::unavailable, "complete recovery history is unavailable");
    if (tick != adapter_->completed_tick() || epoch != std::max(reconnect_epoch_, resync_epoch_) + 1)
        return refused<void>(resync_errors::agreement, "resync epoch or completed barrier differs");
    ResyncIncident incident;
    incident.epoch = epoch; incident.tick = tick; incident.since_ms = last_now_.value_or(0);
    incident.proofs.resize(seats_.size()); incident.ready.resize(seats_.size());
    resync_ = std::move(incident);
    refused_resync_epoch_.reset();
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::receive_resync(const Received& received) {
    if (!options_.resync || departed_[received.from])
        return refused<void>(resync_errors::options, "resync message outside the agreed roster");
    auto bytes = std::span<const std::uint8_t>(received.frame).subspan(1);
    const auto kind = received.frame.front();
    std::uint64_t epoch{};
    if (!read(bytes, epoch, 8) || epoch == 0)
        return refused<void>(resync_errors::malformed, "invalid resync epoch");
    if (epoch <= std::max(resync_epoch_, completed_reconnect_epoch_)) return core::Result<void>::success();
    if (kind == resync_abort_kind) {
        if (bytes.size() != 1 || static_cast<std::size_t>(bytes.front()) >= resync_codes.size())
            return refused<void>(resync_errors::malformed, "invalid resync abort reason");
        if ((resync_ && epoch != resync_->epoch) || (!resync_ && epoch > std::max(resync_epoch_, reconnect_epoch_) + 1))
            return refused<void>(resync_errors::agreement, "abort outside the current or next incident");
        refused_resync_epoch_ = epoch;
        return refused<void>(resync_codes[bytes.front()], "peer refused its single recovery attempt");
    }
    if (kind == resync_vote_kind) {
        std::uint64_t tick{};
        if (!read(bytes, tick, 8) || bytes.size() != 128)
            return refused<void>(resync_errors::malformed, "invalid resync vote");
        const auto valid_hex = [](auto data) {
            return std::all_of(data.begin(), data.end(), [](auto value) {
                return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
            });
        };
        if (!valid_hex(bytes)) return refused<void>(resync_errors::malformed, "invalid resync hash encoding");
        if (!resync_) { if (auto begun = start_resync(epoch, tick); !begun) return begun; }
        if (resync_->epoch != epoch || resync_->tick != tick)
            return refused<void>(resync_errors::agreement, "conflicting resync frontier");
        ResyncVote vote{std::string(bytes.begin(), bytes.begin() + 64), std::string(bytes.begin() + 64, bytes.end())};
        const auto [at, inserted] = resync_->votes.emplace(received.from, vote);
        if (!inserted && at->second != vote)
            return refused<void>(resync_errors::agreement, "peer changed its resync vote");
        return core::Result<void>::success();
    }
    if (!resync_ || epoch != resync_->epoch)
        return refused<void>(resync_errors::agreement, "message outside the active resync incident");
    auto& state = *resync_;
    // Independent ordered streams can deliver the source before the last vote.
    // Bound this tail by the same transfer budget, then interpret after agreement.
    if (!state.source) {
        const auto limit = options_.reconnect->max_log_bytes;
        if (received.frame.size() > limit - state.deferred_bytes
            || state.deferred.size() >= limit / (64U * 1024U) + 4 * seats_.size() + 4)
            return refused<void>(resync_errors::malformed, "early resync traffic exceeds its bound");
        state.deferred.push_back(received); state.deferred_bytes += received.frame.size();
        return core::Result<void>::success();
    }
    if (kind == resync_offer_kind) {
        std::uint64_t tick{}, size{};
        if (received.from != *state.source || state.offered || !read(bytes, tick, 8) || tick != state.tick
            || !read(bytes, size, 8) || size == 0 || size > options_.reconnect->max_log_bytes || bytes.size() != 32)
            return refused<void>(resync_errors::malformed, "invalid resync source offer");
        state.size = static_cast<std::size_t>(size); std::copy(bytes.begin(), bytes.end(), state.digest.begin());
        state.offered = true;
        state.receiving = state.votes.at(self_).state != state.hash;
        return core::Result<void>::success();
    }
    if (kind == resync_chunk_kind) {
        std::uint64_t offset{};
        if (received.from != *state.source || !state.offered || !state.receiving || state.adapter
            || !read(bytes, offset, 8) || offset != state.package.size() || bytes.empty()
            || bytes.size() > 64U * 1024U || bytes.size() > state.size - state.package.size())
            return refused<void>(resync_errors::malformed, "invalid resync chunk prefix");
        state.package.insert(state.package.end(), bytes.begin(), bytes.end());
        return core::Result<void>::success();
    }
    if (kind == resync_proof_kind) {
        if (bytes.size() != 64 || std::string(bytes.begin(), bytes.end()) != state.hash)
            return refused<void>(resync_errors::hash, "resync barrier hashes disagree");
        state.proofs[received.from] = true;
        return core::Result<void>::success();
    }
    if (kind == resync_ready_kind) {
        if (!bytes.empty() || !state.proofs[received.from])
            return refused<void>(resync_errors::agreement, "resync ready precedes peer proof");
        state.ready[received.from] = true;
        return core::Result<void>::success();
    }
    return refused<void>(resync_errors::malformed, "unknown resync envelope");
}

core::Result<void> LockstepSession::stage_resync() {
    auto& state = *resync_;
    auto rebuilt = options_.resync->rebuild(options_.reconnect->setup);
    if (!rebuilt) return refused<void>(resync_errors::replay, "recovery factory failed: " + rebuilt.error().message);
    auto adapter = std::move(rebuilt).value();
    if (!adapter || adapter->completed_tick() != 0)
        return refused<void>(resync_errors::replay, "recovery factory must return a fresh tick-zero adapter");
    auto config = options_;
    config.resync.reset(); config.input_delay = initial_input_delay_;
    config.reconnect->rejoin_source.reset();
    // Avoid callbacks captured from the live session during speculative replay.
    config.reconnect->replay_control = {};
    if (options_.resync->replay_control) {
        config.reconnect->replay_control = [target = adapter.get(), replay = options_.resync->replay_control](auto bytes) {
            return replay(*target, bytes);
        };
    }
    auto created = create(*adapter, *transport_, seats_, config);
    if (!created) return refused<void>(resync_errors::replay, "staging session failed: " + created.error().message);
    auto staging = std::move(created).value();
    staging.resync_staging_ = true;
    staging.held_ = HeldSeat{}; staging.held_->cutoff = state.tick; staging.held_->epoch = state.epoch;
    staging.incoming_package_ = std::move(state.package); staging.incoming_digest_ = state.digest;
    auto installed = staging.install_reconnect_package();
    if (!installed) return refused<void>(resync_errors::replay, "staged recovery failed: " + installed.error().message);
    if (staging.options_.input_delay != options_.input_delay || staging.resync_log_hash() != resync_log_hash())
        return refused<void>(resync_errors::agreement, "staged recovery changed the agreed input history or timing");
    if (adapter->state_hash() != state.hash)
        return refused<void>(resync_errors::hash, "staged replay does not reproduce the authoritative hash");
    state.adapter = std::move(adapter);
    return core::Result<void>::success();
}

core::Result<bool> LockstepSession::service_resync() {
    if (!resync_) {
        if (auto begun = start_resync(std::max(reconnect_epoch_, resync_epoch_) + 1, adapter_->completed_tick()); !begun)
            return core::Result<bool>::failure(begun.error());
    }
    auto& state = *resync_;
    const auto now = last_now_.value_or(0);
    // READY commits this endpoint to completion. A delayed READY from another
    // stream cannot make it expire while that peer publishes. The transport's
    // bounded link-liveness policy, rather than a local incident clock, ends a
    // committed wait whose voter stops responding.
    if (!state.sent_ready && now - state.since_ms >= options_.resync->timeout_ms)
        return refused<bool>(resync_errors::timeout, "single resync attempt expired");
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if (peer != self_ && !departed_[peer] && transport_->link(peer) == LinkState::closed)
            return refused<bool>(resync_errors::overlap, "resync lost an active voter");
    if (!state.sent_vote) {
        if (!monitor_->ready_for_resync()) return core::Result<bool>::success(true);
        const ResyncVote vote{adapter_->state_hash(), resync_log_hash()};
        Bytes frame{resync_vote_kind}; append(frame, state.epoch, 8); append(frame, state.tick, 8);
        frame.insert(frame.end(), vote.state.begin(), vote.state.end()); frame.insert(frame.end(), vote.log.begin(), vote.log.end());
        if (auto sent = resync_broadcast(frame); !sent) return core::Result<bool>::failure(sent.error());
        state.votes[self_] = vote; state.sent_vote = true;
    }
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if (!departed_[peer] && !state.votes.contains(peer)) return core::Result<bool>::success(true);
    if (!state.source) {
        std::size_t best_count{};
        sim::SeatId best_seat{std::numeric_limits<std::uint32_t>::max()};
        for (const auto& [peer, vote] : state.votes) {
            if (departed_[peer]) continue;
            if (vote.log != state.votes.at(self_).log)
                return refused<bool>(resync_errors::agreement, "resync cannot repair conflicting sealed histories");
            const auto count = static_cast<std::size_t>(std::count_if(state.votes.begin(), state.votes.end(),
                [&](const auto& entry) { return !departed_[entry.first] && entry.second.state == vote.state; }));
            if (count > best_count || (count == best_count && seats_[peer].seat < best_seat)) {
                state.source = peer; state.hash = vote.state; best_count = count; best_seat = seats_[peer].seat;
            }
        }
        const auto active_count = static_cast<std::size_t>(std::count(departed_.begin(), departed_.end(), false));
        if (best_count == active_count) return refused<bool>(resync_errors::agreement, "no completed-state divergence to recover");
        if (*state.source == self_) {
            auto package = reconnect_package();
            if (!package) return refused<bool>(resync_errors::unavailable, "recovery envelope failed: " + package.error().message);
            state.package = std::move(package).value(); state.size = state.package.size(); state.digest = core::sha256(state.package);
            Bytes offer{resync_offer_kind}; append(offer, state.epoch, 8); append(offer, state.tick, 8); append(offer, state.size, 8);
            offer.insert(offer.end(), state.digest.begin(), state.digest.end());
            if (auto sent = resync_broadcast(offer); !sent) return core::Result<bool>::failure(sent.error());
            state.offered = true;
        }
        auto deferred = std::move(state.deferred); state.deferred.clear(); state.deferred_bytes = 0;
        for (const auto& received : deferred)
            if (auto accepted = receive_resync(received); !accepted) return core::Result<bool>::failure(accepted.error());
    }
    if (*state.source == self_ && state.sent_bytes < state.size) {
        const auto length = std::min<std::size_t>(64U * 1024U, state.size - state.sent_bytes);
        Bytes chunk{resync_chunk_kind}; append(chunk, state.epoch, 8); append(chunk, state.sent_bytes, 8);
        chunk.insert(chunk.end(), state.package.begin() + static_cast<std::ptrdiff_t>(state.sent_bytes),
            state.package.begin() + static_cast<std::ptrdiff_t>(state.sent_bytes + length));
        for (const auto& [peer, vote] : state.votes) if (!departed_[peer] && vote.state != state.hash) {
            if (auto sent = transport_->send(peer, chunk); !sent) return core::Result<bool>::failure(sent.error());
        }
        state.sent_bytes += length;
        if (state.sent_bytes == state.size) Bytes{}.swap(state.package);
    }
    if (state.receiving && !state.adapter && state.package.size() == state.size) {
        if (auto staged = stage_resync(); !staged) return core::Result<bool>::failure(staged.error());
    }
    if (state.offered && (!state.receiving || state.adapter) && !state.sent_proof) {
        Bytes proof{resync_proof_kind}; append(proof, state.epoch, 8); proof.insert(proof.end(), state.hash.begin(), state.hash.end());
        if (auto sent = resync_broadcast(proof); !sent) return core::Result<bool>::failure(sent.error());
        state.proofs[self_] = true; state.sent_proof = true;
    }
    bool proved = true, ready = true;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) if (!departed_[peer]) {
        proved = proved && state.proofs[peer]; ready = ready && state.ready[peer];
    }
    if (proved && !state.sent_ready) {
        Bytes frame{resync_ready_kind}; append(frame, state.epoch, 8);
        if (auto sent = resync_broadcast(frame); !sent) return core::Result<bool>::failure(sent.error());
        state.ready[self_] = true; state.sent_ready = true;
    }
    if (ready) {
        if (auto reset = monitor_->on_resync(state.tick, state.hash); !reset) return core::Result<bool>::failure(reset.error());
        // Validate the buffered next-window traffic before publishing staging.
        for (const auto& received : state.monitor_frames)
            if (auto accepted = monitor_->receive(received); !accepted) return core::Result<bool>::failure(accepted.error());
        if (state.adapter) { owned_adapter_ = std::move(state.adapter); adapter_ = owned_adapter_.get(); }
        resync_epoch_ = state.epoch; reconnect_epoch_ = state.epoch;
        completed_reconnect_epoch_ = std::max(completed_reconnect_epoch_, state.epoch);
        resync_.reset(); waiting_.reset(); monitor_wait_.reset();
        return core::Result<bool>::success(false);
    }
    return core::Result<bool>::success(true);
}

} // namespace eawr::net
