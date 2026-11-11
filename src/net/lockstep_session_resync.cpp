#include "lockstep_session_internal.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/types.hpp"
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
    if (!reconnect_available_ || tick < initial_tick_ || reconnect_log_.size() != tick - initial_tick_)
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
    if (!adapter || adapter->completed_tick() != initial_tick_)
        return refused<void>(resync_errors::replay, "recovery factory must return a adapter at the agreed checkpoint tick");
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

// Checkpoint-rooted GC recovery is deliberately separate from the tactical
// held-seat incident above: staging owns no live adapter or transport.
#include "eawr/net/galactic_recovery.hpp"
#include <chrono>
#include <thread>

namespace eawr::net {
namespace {
std::optional<std::size_t> recovery_tick_bytes(std::span<const SeatInput> inputs) {
    std::size_t bytes = 8 + 32 + 4;
    if (inputs.size() > max_batch_commands * max_match_seats) return {};
    for (const auto& input : inputs) {
        if (input.bytes.empty() || input.bytes.size() > max_batch_bytes
            || bytes > 256U * 1024U * 1024U - 24 - input.bytes.size()) return {};
        bytes += 24 + input.bytes.size();
    }
    return bytes;
}
std::string recovery_hex(const RecoveryDigest& digest) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text; text.reserve(64);
    for (const auto byte : digest) { text.push_back(digits[byte >> 4U]); text.push_back(digits[byte & 15U]); }
    return text;
}
bool recovery_nonzero(const RecoveryDigest& digest) {
    return std::any_of(digest.begin(), digest.end(), [](auto byte) { return byte != 0; });
}
}
std::string_view galactic_recovery_reason(GalacticRecoveryRefusal reason) noexcept {
    switch (reason) {
    case GalacticRecoveryRefusal::none: return "none";
    case GalacticRecoveryRefusal::unavailable: return "checkpoint_unavailable";
    case GalacticRecoveryRefusal::divergent_epoch: return "divergent_epoch";
    case GalacticRecoveryRefusal::anchor_hash: return "anchor_hash_mismatch";
    case GalacticRecoveryRefusal::log_too_old: return "log_too_old";
    case GalacticRecoveryRefusal::seat_binding: return "authenticated_seat_mismatch";
    case GalacticRecoveryRefusal::malformed: return "malformed_catchup";
    case GalacticRecoveryRefusal::limit: return "catchup_limit";
    case GalacticRecoveryRefusal::restore: return "private_restore_failed";
    case GalacticRecoveryRefusal::replay: return "private_replay_failed";
    case GalacticRecoveryRefusal::state_hash: return "catchup_state_hash_mismatch";
    case GalacticRecoveryRefusal::stale_barrier: return "stale_publication_barrier";
    case GalacticRecoveryRefusal::busy: return "catchup_in_progress";
    case GalacticRecoveryRefusal::cancelled: return "catchup_cancelled";
    }
    return "malformed_catchup";
}
GalacticRecoveryRefusal GalacticRecoveryLog::checkpoint(GalacticRecoveryAnchor anchor,
    std::shared_ptr<const std::vector<std::uint8_t>> bytes, std::vector<PeerSeat> seats) {
    if (!anchor.epoch || anchor.completed_tick >= sim::galactic::galactic_max_ticks || !recovery_nonzero(anchor.lineage)
        || !recovery_nonzero(anchor.state) || !bytes || bytes->empty() || bytes->size() > save_transfer_limit
        || seats.size() < 2 || seats.size() > max_match_seats || !limit_ || limit_ > 256U * 1024U * 1024U)
        return GalacticRecoveryRefusal::malformed;
    for (std::size_t index = 0; index < seats.size(); ++index) {
        if (!seats[index].seat.value) return GalacticRecoveryRefusal::seat_binding;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (seats[index].seat == seats[previous].seat) return GalacticRecoveryRefusal::seat_binding;
    }
    if (bytes_ && (anchor.epoch < frontier_.epoch || (anchor.epoch == frontier_.epoch
        && (anchor.lineage != frontier_.lineage || anchor.completed_tick < frontier_.completed_tick))))
        return GalacticRecoveryRefusal::divergent_epoch;
    digest_ = core::sha256(*bytes);
    checkpoint_ = frontier_ = anchor; bytes_ = std::move(bytes); seats_ = std::move(seats);
    ticks_.clear(); retained_ = 0; expired_ = false;
    return GalacticRecoveryRefusal::none;
}
GalacticRecoveryRefusal GalacticRecoveryLog::append(std::uint64_t tick,
    std::span<const SeatInput> inputs, RecoveryDigest state) {
    if (!bytes_) return GalacticRecoveryRefusal::unavailable;
    if (tick != frontier_.completed_tick || tick >= sim::galactic::galactic_max_ticks || !recovery_nonzero(state))
        return GalacticRecoveryRefusal::malformed;
    const auto bytes = recovery_tick_bytes(inputs);
    if (!bytes) return GalacticRecoveryRefusal::limit;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto& input = inputs[index];
        if (input.key.tick != tick || (index && !(inputs[index - 1].key < input.key)))
            return GalacticRecoveryRefusal::malformed;
        const auto bound = std::find_if(seats_.begin(), seats_.end(), [&](const auto& row) {
            return row.may_submit && row.seat == input.key.seat;
        });
        if (bound == seats_.end()) return GalacticRecoveryRefusal::seat_binding;
    }
    frontier_.completed_tick = tick + 1; frontier_.state = state;
    if (*bytes > limit_) {
        ticks_.clear(); retained_ = 0; expired_ = true;
        return GalacticRecoveryRefusal::log_too_old;
    }
    while (retained_ > limit_ - *bytes) {
        retained_ -= *recovery_tick_bytes(ticks_.front().inputs);
        ticks_.pop_front(); expired_ = true;
    }
    ticks_.push_back({tick, {inputs.begin(), inputs.end()}, state}); retained_ += *bytes;
    return GalacticRecoveryRefusal::none;
}
GalacticRecoveryRefusal GalacticRecoveryLog::package(PeerIndex peer,
    const GalacticRecoveryRequest& request, GalacticRecoveryPackage& result) const {
    if (!bytes_) return GalacticRecoveryRefusal::unavailable;
    if (peer >= seats_.size() || !seats_[peer].may_submit || seats_[peer].seat != request.seat)
        return GalacticRecoveryRefusal::seat_binding;
    if (request.anchor.epoch != frontier_.epoch || request.anchor.lineage != frontier_.lineage)
        return GalacticRecoveryRefusal::divergent_epoch;
    if (expired_ || request.anchor.completed_tick < checkpoint_.completed_tick)
        return GalacticRecoveryRefusal::log_too_old;
    if (request.anchor.completed_tick > frontier_.completed_tick) return GalacticRecoveryRefusal::stale_barrier;
    const auto expected = request.anchor.completed_tick == checkpoint_.completed_tick ? checkpoint_.state
        : ticks_[static_cast<std::size_t>(request.anchor.completed_tick - checkpoint_.completed_tick - 1)].state;
    if (expected != request.anchor.state) return GalacticRecoveryRefusal::anchor_hash;
    result = {checkpoint_, frontier_, digest_, bytes_, seats_, {ticks_.begin(), ticks_.end()}, {}};
    return GalacticRecoveryRefusal::none;
}
void GalacticRecoveryLog::invalidate() noexcept {
    bytes_.reset(); ticks_.clear(); seats_.clear(); retained_ = 0; expired_ = true;
}

GalacticRecoveryWorker::~GalacticRecoveryWorker() { cancel(); }
void GalacticRecoveryWorker::cancel() noexcept {
    if (cancelled_) cancelled_->store(true, std::memory_order_relaxed);
    // A detached job owns only its immutable package and private services. Future
    // destruction never joins it on the live simulation/render thread.
    // Keep the future until the cancelled job exits: repeated cancel/start
    // requests cannot accumulate private workers or checkpoint allocations.
}
GalacticRecoveryRefusal GalacticRecoveryWorker::start(GalacticRecoveryPackage package, Restore restore) {
    if (busy()) return GalacticRecoveryRefusal::busy;
    if (!restore || !package.bytes || package.bytes->empty() || package.bytes->size() > save_transfer_limit
        || !package.checkpoint.epoch || package.checkpoint.epoch != package.frontier.epoch
        || package.checkpoint.lineage != package.frontier.lineage
        || !recovery_nonzero(package.checkpoint.lineage) || !recovery_nonzero(package.checkpoint.state)
        || package.frontier.completed_tick < package.checkpoint.completed_tick
        || package.frontier.completed_tick > sim::galactic::galactic_max_ticks
        || package.ticks.size() != package.frontier.completed_tick - package.checkpoint.completed_tick
        || package.seats.size() < 2 || package.seats.size() > max_match_seats)
        return GalacticRecoveryRefusal::malformed;
    if (package.session_frontier && (package.session_frontier->split
        || package.session_frontier->completed_tick != package.frontier.completed_tick))
        return GalacticRecoveryRefusal::divergent_epoch;
    for (std::size_t index = 0; index < package.seats.size(); ++index) {
        if (!package.seats[index].seat.value) return GalacticRecoveryRefusal::seat_binding;
        for (std::size_t previous = 0; previous < index; ++previous)
            if (package.seats[index].seat == package.seats[previous].seat) return GalacticRecoveryRefusal::seat_binding;
    }
    // Apply the transport budget before creating a thread or invoking any restore.
    std::size_t total{};
    for (const auto& row : package.ticks) {
        const auto bytes = recovery_tick_bytes(row.inputs);
        if (!bytes || *bytes > 256U * 1024U * 1024U - total) return GalacticRecoveryRefusal::limit;
        total += *bytes;
    }
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    std::promise<Result> promise;
    result_ = promise.get_future();
    try {
        std::thread([package = std::move(package), restore = std::move(restore),
            cancelled = cancelled_, promise = std::move(promise)]() mutable {
            Result result; result.frontier = package.frontier;
            try {
                const auto replay = [&]() -> GalacticRecoveryRefusal {
                    if (cancelled->load(std::memory_order_relaxed)) return GalacticRecoveryRefusal::cancelled;
                    if (core::sha256(*package.bytes) != package.checkpoint_digest) return GalacticRecoveryRefusal::anchor_hash;
                    auto restored = restore(package);
                    if (!restored || !restored.value()) return GalacticRecoveryRefusal::restore;
                    auto owner = std::move(restored).value();
                    if (owner->completed_tick() != package.checkpoint.completed_tick
                        || owner->state_hash() != recovery_hex(package.checkpoint.state))
                        return GalacticRecoveryRefusal::state_hash;
                    for (const auto& row : package.ticks) {
                        if (cancelled->load(std::memory_order_relaxed)) return GalacticRecoveryRefusal::cancelled;
                        if (row.tick != owner->completed_tick()) return GalacticRecoveryRefusal::malformed;
                        for (std::size_t index = 0; index < row.inputs.size(); ++index) {
                            const auto& input = row.inputs[index];
                            if (input.key.tick != row.tick || (index && !(row.inputs[index - 1].key < input.key)))
                                return GalacticRecoveryRefusal::malformed;
                            const auto seat = std::find_if(package.seats.begin(), package.seats.end(), [&](const auto& binding) {
                                return binding.may_submit && binding.seat == input.key.seat;
                            });
                            if (seat == package.seats.end()) return GalacticRecoveryRefusal::seat_binding;
                            if (!owner->validate_recovery_input(input)) return GalacticRecoveryRefusal::replay;
                        }
                        if (!owner->install(row.inputs)) return GalacticRecoveryRefusal::replay;
                        auto tick = owner->step();
                        if (!tick || tick.value().completed_tick != row.tick + 1 || owner->completed_tick() != row.tick + 1)
                            return GalacticRecoveryRefusal::replay;
                        if (owner->state_hash() != recovery_hex(row.state)) return GalacticRecoveryRefusal::state_hash;
                    }
                    if (owner->completed_tick() != package.frontier.completed_tick
                        || owner->state_hash() != recovery_hex(package.frontier.state))
                        return GalacticRecoveryRefusal::state_hash;
                    result.owner = std::move(owner);
                    return GalacticRecoveryRefusal::none;
                };
                result.refusal = replay();
            } catch (...) { result.refusal = GalacticRecoveryRefusal::restore; }
            promise.set_value(std::move(result));
        }).detach();
    } catch (...) {
        cancel(); result_ = {}; cancelled_.reset(); return GalacticRecoveryRefusal::restore;
    }
    return GalacticRecoveryRefusal::none;
}
std::optional<GalacticRecoveryWorker::Result> GalacticRecoveryWorker::take(const GalacticRecoveryAnchor& barrier) {
    if (!result_.valid() || result_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return {};
    auto result = result_.get();
    if (cancelled_ && cancelled_->load(std::memory_order_relaxed)) {
        result.owner.reset(); result.refusal = GalacticRecoveryRefusal::cancelled;
    }
    cancelled_.reset();
    if (result.refusal == GalacticRecoveryRefusal::none && result.frontier != barrier) {
        result.owner.reset(); result.refusal = GalacticRecoveryRefusal::stale_barrier;
    }
    return result;
}
} // namespace eawr::net

namespace eawr::net {
namespace {
using RecoveryBytes = std::vector<std::uint8_t>;
constexpr std::size_t recovery_wire_limit = save_transfer_limit + 192U * 1024U * 1024U;
template<class T> core::Result<T> recovery_bad(GalacticRecoveryRefusal reason) {
    return core::Result<T>::failure(lockstep_detail::diagnostic(reconnect_errors::log,
        std::string(galactic_recovery_reason(reason))));
}
void recovery_anchor_bytes(RecoveryBytes& bytes, const GalacticRecoveryAnchor& anchor) {
    lockstep_detail::append(bytes, anchor.epoch, 8); lockstep_detail::append(bytes, anchor.completed_tick, 8);
    bytes.insert(bytes.end(), anchor.lineage.begin(), anchor.lineage.end());
    bytes.insert(bytes.end(), anchor.state.begin(), anchor.state.end());
}
bool recovery_read_digest(std::span<const std::uint8_t>& bytes, RecoveryDigest& digest) {
    if (bytes.size() < digest.size()) return false;
    std::copy_n(bytes.begin(), digest.size(), digest.begin()); bytes = bytes.subspan(digest.size()); return true;
}
bool recovery_read_anchor(std::span<const std::uint8_t>& bytes, GalacticRecoveryAnchor& anchor) {
    return lockstep_detail::read(bytes, anchor.epoch, 8) && anchor.epoch
        && lockstep_detail::read(bytes, anchor.completed_tick, 8) && anchor.completed_tick <= sim::galactic::galactic_max_ticks
        && recovery_read_digest(bytes, anchor.lineage) && recovery_nonzero(anchor.lineage)
        && recovery_read_digest(bytes, anchor.state) && recovery_nonzero(anchor.state);
}
}
core::Result<RecoveryBytes> encode_galactic_recovery_request(const GalacticRecoveryRequest& request) {
    if (!request.transfer_epoch || !request.seat.value || !request.anchor.epoch || request.anchor.completed_tick > sim::galactic::galactic_max_ticks
        || !recovery_nonzero(request.anchor.lineage) || !recovery_nonzero(request.anchor.state))
        return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::malformed);
    RecoveryBytes bytes{gc_recovery_request_kind}; recovery_anchor_bytes(bytes, request.anchor);
    lockstep_detail::append(bytes, request.seat.value, 4);
    lockstep_detail::append(bytes, request.transfer_epoch, 8);
    return core::Result<RecoveryBytes>::success(std::move(bytes));
}
core::Result<GalacticRecoveryRequest> decode_galactic_recovery_request(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 93 || bytes.front() != gc_recovery_request_kind)
        return recovery_bad<GalacticRecoveryRequest>(GalacticRecoveryRefusal::malformed);
    bytes = bytes.subspan(1); GalacticRecoveryRequest request; std::uint64_t seat{};
    if (!recovery_read_anchor(bytes, request.anchor) || !lockstep_detail::read(bytes, seat, 4) || !seat
        || !lockstep_detail::read(bytes, request.transfer_epoch, 8) || !request.transfer_epoch || !bytes.empty())
        return recovery_bad<GalacticRecoveryRequest>(GalacticRecoveryRefusal::malformed);
    request.seat = sim::SeatId{static_cast<std::uint32_t>(seat)};
    return core::Result<GalacticRecoveryRequest>::success(request);
}
core::Result<RecoveryBytes> encode_galactic_recovery_control(const GalacticRecoveryControl& control) {
    if (!control.transfer_epoch || control.source == control.target || control.source >= max_match_seats
        || control.target >= max_match_seats || control.barrier > sim::galactic::galactic_max_ticks
        || control.stage > GalacticRecoveryStage::release || (control.removed && control.stage != GalacticRecoveryStage::release)
        || !control.frontier.epoch
        || control.frontier.completed_tick > sim::galactic::galactic_max_ticks || !recovery_nonzero(control.frontier.lineage)
        || !recovery_nonzero(control.frontier.state)) return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::malformed);
    RecoveryBytes bytes{gc_recovery_proof_kind};
    lockstep_detail::append(bytes, control.transfer_epoch, 8);
    lockstep_detail::append(bytes, control.barrier, 8);
    lockstep_detail::append(bytes, control.source, 4); lockstep_detail::append(bytes, control.target, 4);
    lockstep_detail::append(bytes, static_cast<std::uint8_t>(control.stage), 1);
    recovery_anchor_bytes(bytes, control.frontier);
    bytes.insert(bytes.end(), control.sealed_log.begin(), control.sealed_log.end());
    lockstep_detail::append(bytes, control.removed, 1);
    return core::Result<RecoveryBytes>::success(std::move(bytes));
}
core::Result<GalacticRecoveryControl> decode_galactic_recovery_control(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 139 || bytes.front() != gc_recovery_proof_kind)
        return recovery_bad<GalacticRecoveryControl>(GalacticRecoveryRefusal::malformed);
    bytes = bytes.subspan(1); GalacticRecoveryControl control; std::uint64_t source{}, target{}, stage{};
    if (!lockstep_detail::read(bytes, control.transfer_epoch, 8) || !lockstep_detail::read(bytes, control.barrier, 8)
        || !lockstep_detail::read(bytes, source, 4) || !lockstep_detail::read(bytes, target, 4)
        || !lockstep_detail::read(bytes, stage, 1) || !recovery_read_anchor(bytes, control.frontier)
        || bytes.size() != control.sealed_log.size() + 1) return recovery_bad<GalacticRecoveryControl>(GalacticRecoveryRefusal::malformed);
    control.source = static_cast<PeerIndex>(source); control.target = static_cast<PeerIndex>(target);
    control.stage = static_cast<GalacticRecoveryStage>(stage);
    std::copy_n(bytes.begin(), control.sealed_log.size(), control.sealed_log.begin());
    control.removed = bytes.back();
    if (!encode_galactic_recovery_control(control)) return recovery_bad<GalacticRecoveryControl>(GalacticRecoveryRefusal::malformed);
    return core::Result<GalacticRecoveryControl>::success(control);
}
core::Result<RecoveryBytes> encode_galactic_recovery_log(const GalacticRecoveryPackage& package) {
    if (!package.checkpoint.epoch || package.checkpoint.epoch != package.frontier.epoch
        || package.checkpoint.lineage != package.frontier.lineage
        || package.checkpoint.completed_tick > package.frontier.completed_tick
        || package.frontier.completed_tick > sim::galactic::galactic_max_ticks
        || package.ticks.size() != package.frontier.completed_tick - package.checkpoint.completed_tick
        || package.seats.size() < 2 || package.seats.size() > max_match_seats)
        return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::malformed);
    RecoveryBytes bytes; recovery_anchor_bytes(bytes, package.checkpoint); recovery_anchor_bytes(bytes, package.frontier);
    bytes.insert(bytes.end(), package.checkpoint_digest.begin(), package.checkpoint_digest.end());
    lockstep_detail::append(bytes, package.seats.size(), 4);
    for (const auto& seat : package.seats) {
        lockstep_detail::append(bytes, seat.seat.value, 4); lockstep_detail::append(bytes, seat.may_submit, 1);
    }
    lockstep_detail::append(bytes, package.ticks.size(), 8);
    for (const auto& row : package.ticks) {
        const auto size = recovery_tick_bytes(row.inputs);
        if (!size || *size > recovery_wire_limit - bytes.size()) return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::limit);
        lockstep_detail::append(bytes, row.tick, 8); bytes.insert(bytes.end(), row.state.begin(), row.state.end());
        lockstep_detail::append(bytes, row.inputs.size(), 4);
        for (const auto& input : row.inputs) {
            lockstep_detail::append(bytes, input.key.tick, 8); lockstep_detail::append(bytes, input.key.seat.value, 4);
            lockstep_detail::append(bytes, input.key.sequence, 8); lockstep_detail::append(bytes, input.bytes.size(), 4);
            bytes.insert(bytes.end(), input.bytes.begin(), input.bytes.end());
        }
    }
    lockstep_detail::append(bytes, package.session_frontier.has_value(), 1);
    if (package.session_frontier) {
        const auto& frontier = *package.session_frontier;
        if (frontier.peers.size() != package.seats.size() || !frontier.input_delay || frontier.input_delay > max_input_delay
            || frontier.completed_tick != package.frontier.completed_tick
            || frontier.future.size() > sealed_input_horizon(frontier.input_delay) * package.seats.size())
            return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::malformed);
        lockstep_detail::append(bytes, frontier.completed_tick, 8); lockstep_detail::append(bytes, frontier.input_delay, 8);
        lockstep_detail::append(bytes, frontier.split, 1);
        lockstep_detail::append(bytes, frontier.mesh_incident, 8);
        for (const auto& peer : frontier.peers) {
            lockstep_detail::append(bytes, peer.next_seal, 8); lockstep_detail::append(bytes, peer.producer_sequence.has_value(), 1);
            lockstep_detail::append(bytes, peer.producer_sequence.value_or(0), 8); lockstep_detail::append(bytes, peer.departed, 1);
        }
        if (bytes.size() > recovery_wire_limit - 4) return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::limit);
        lockstep_detail::append(bytes, frontier.future.size(), 4);
        for (const auto& batch : frontier.future) {
            const auto encoded = encode_batch(batch);
            if (!encoded) return core::Result<RecoveryBytes>::failure(encoded.error());
            if (encoded.value().size() + 4 > recovery_wire_limit - bytes.size()) return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::limit);
            lockstep_detail::append(bytes, encoded.value().size(), 4);
            bytes.insert(bytes.end(), encoded.value().begin(), encoded.value().end());
        }
    }
    return core::Result<RecoveryBytes>::success(std::move(bytes));
}
core::Result<GalacticRecoveryPackage> decode_galactic_recovery_log(std::span<const std::uint8_t> bytes,
    std::shared_ptr<const RecoveryBytes> checkpoint) {
    const auto bad = [] { return recovery_bad<GalacticRecoveryPackage>(GalacticRecoveryRefusal::malformed); };
    if (bytes.size() > recovery_wire_limit || !checkpoint || checkpoint->empty() || checkpoint->size() > save_transfer_limit) return bad();
    GalacticRecoveryPackage package; package.bytes = std::move(checkpoint);
    std::uint64_t count{};
    if (!recovery_read_anchor(bytes, package.checkpoint) || !recovery_read_anchor(bytes, package.frontier)
        || package.checkpoint.epoch != package.frontier.epoch || package.checkpoint.lineage != package.frontier.lineage
        || package.checkpoint.completed_tick > package.frontier.completed_tick
        || !recovery_read_digest(bytes, package.checkpoint_digest)
        || !lockstep_detail::read(bytes, count, 4) || count < 2 || count > max_match_seats) return bad();
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t seat{}, may_submit{};
        if (!lockstep_detail::read(bytes, seat, 4) || !seat || !lockstep_detail::read(bytes, may_submit, 1) || may_submit > 1) return bad();
        const sim::SeatId id{static_cast<std::uint32_t>(seat)};
        if (std::any_of(package.seats.begin(), package.seats.end(), [&](const auto& row) { return row.seat == id; })) return bad();
        package.seats.push_back({id, may_submit != 0});
    }
    if (!lockstep_detail::read(bytes, count, 8) || count != package.frontier.completed_tick - package.checkpoint.completed_tick
        || count > bytes.size() / 44) return bad();
    for (std::uint64_t index = 0; index < count; ++index) {
        GalacticRecoveryTick row; std::uint64_t commands{};
        if (!lockstep_detail::read(bytes, row.tick, 8) || row.tick != package.checkpoint.completed_tick + index
            || !recovery_read_digest(bytes, row.state) || !recovery_nonzero(row.state)
            || !lockstep_detail::read(bytes, commands, 4) || commands > max_batch_commands * package.seats.size()
            || commands > bytes.size() / 25) return bad();
        for (std::uint64_t command = 0; command < commands; ++command) {
            std::uint64_t tick{}, seat{}, sequence{}, size{};
            if (!lockstep_detail::read(bytes, tick, 8) || tick != row.tick || !lockstep_detail::read(bytes, seat, 4) || !seat
                || !lockstep_detail::read(bytes, sequence, 8) || !lockstep_detail::read(bytes, size, 4)
                || !size || size > max_batch_bytes || size > bytes.size()) return bad();
            SeatInput input{{tick, sim::SeatId{static_cast<std::uint32_t>(seat)}, sequence},
                RecoveryBytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size))};
            bytes = bytes.subspan(static_cast<std::size_t>(size));
            if (!row.inputs.empty() && !(row.inputs.back().key < input.key)) return bad();
            if (std::none_of(package.seats.begin(), package.seats.end(), [&](const auto& binding) {
                return binding.may_submit && binding.seat == input.key.seat;
            })) return bad();
            row.inputs.push_back(std::move(input));
        }
        package.ticks.push_back(std::move(row));
    }
    std::uint64_t present{};
    if (!lockstep_detail::read(bytes, present, 1) || present > 1) return bad();
    if (present) {
        RecoverySessionFrontier frontier; std::uint64_t split{};
        if (!lockstep_detail::read(bytes, frontier.completed_tick, 8) || frontier.completed_tick != package.frontier.completed_tick
            || !lockstep_detail::read(bytes, frontier.input_delay, 8) || !frontier.input_delay || frontier.input_delay > max_input_delay
            || !lockstep_detail::read(bytes, split, 1) || split > 1
            || !lockstep_detail::read(bytes, frontier.mesh_incident, 8)) return bad();
        frontier.split = split != 0;
        for (std::size_t peer = 0; peer < package.seats.size(); ++peer) {
            RecoveryPeerFrontier row; std::uint64_t cursor{}, departed{};
            if (!lockstep_detail::read(bytes, row.next_seal, 8) || row.next_seal > frontier.completed_tick + sealed_input_horizon(frontier.input_delay)
                || !lockstep_detail::read(bytes, present, 1) || present > 1 || !lockstep_detail::read(bytes, cursor, 8)
                || (!present && cursor) || !lockstep_detail::read(bytes, departed, 1) || departed > 1) return bad();
            row.producer_sequence = present ? std::optional{cursor} : std::nullopt; row.departed = departed != 0;
            if (!row.departed && row.next_seal < frontier.completed_tick) return bad();
            frontier.peers.push_back(row);
        }
        if (!lockstep_detail::read(bytes, count, 4) || count > sealed_input_horizon(frontier.input_delay) * package.seats.size()
            || count > bytes.size() / 29) return bad();
        for (std::uint64_t index = 0; index < count; ++index) {
            std::uint64_t size{};
            if (!lockstep_detail::read(bytes, size, 4) || !size || size > max_batch_bytes || size > bytes.size()) return bad();
            auto batch = decode_batch(bytes.first(static_cast<std::size_t>(size)));
            if (!batch) return bad();
            bytes = bytes.subspan(static_cast<std::size_t>(size));
            frontier.future.push_back(std::move(batch).value());
        }
        package.session_frontier = std::move(frontier);
    }
    if (!bytes.empty() || core::sha256(*package.bytes) != package.checkpoint_digest) return bad();
    return core::Result<GalacticRecoveryPackage>::success(std::move(package));
}
core::Result<RecoveryBytes> GalacticRecoveryStream::chunk(std::uint64_t epoch,
    std::span<const std::uint8_t> log, std::size_t offset) {
    if (!epoch || log.empty() || log.size() > recovery_wire_limit || offset >= log.size() || offset % save_chunk_bytes)
        return recovery_bad<RecoveryBytes>(GalacticRecoveryRefusal::malformed);
    RecoveryBytes frame{gc_recovery_log_kind};
    lockstep_detail::append(frame, epoch, 8); lockstep_detail::append(frame, offset, 8); lockstep_detail::append(frame, log.size(), 8);
    const auto payload = log.subspan(offset, std::min(save_chunk_bytes, log.size() - offset));
    frame.insert(frame.end(), payload.begin(), payload.end());
    return core::Result<RecoveryBytes>::success(std::move(frame));
}
GalacticRecoveryRefusal GalacticRecoveryStream::receive(std::span<const std::uint8_t> frame) {
    if (frame.empty() || frame.front() != gc_recovery_log_kind || frame.size() > save_chunk_bytes + 25)
        return GalacticRecoveryRefusal::malformed;
    frame = frame.subspan(1); std::uint64_t epoch{}, offset{}, total{};
    if (!lockstep_detail::read(frame, epoch, 8) || epoch != epoch_) return GalacticRecoveryRefusal::divergent_epoch;
    if (!lockstep_detail::read(frame, offset, 8) || !lockstep_detail::read(frame, total, 8)
        || !total || !limit_ || limit_ > recovery_wire_limit || total > limit_ || offset >= total
        || offset % save_chunk_bytes || frame.size() != std::min<std::uint64_t>(save_chunk_bytes, total - offset))
        return GalacticRecoveryRefusal::limit;
    if ((total_ && total != *total_) || offset > bytes_.size()) return GalacticRecoveryRefusal::malformed;
    if (offset < bytes_.size()) {
        if (frame.size() > bytes_.size() - static_cast<std::size_t>(offset)
            || !std::equal(frame.begin(), frame.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset)))
            return GalacticRecoveryRefusal::malformed;
        return GalacticRecoveryRefusal::none;
    }
    total_ = static_cast<std::size_t>(total);
    bytes_.insert(bytes_.end(), frame.begin(), frame.end());
    return GalacticRecoveryRefusal::none;
}
} // namespace eawr::net

namespace eawr::net {
core::Result<RecoverySessionFrontier> LockstepSession::recovery_frontier(bool obsolete_owner) const {
    if ((failure_ && !obsolete_owner) || !pending_.empty() || agreement_ || held_ || resync_ || checkpoint_ || timing_ || (mesh_ && mesh_->running()) || options_.adaptive_timing)
        return lockstep_detail::refused<RecoverySessionFrontier>(reconnect_errors::unavailable, "frontier_agreement_in_progress");
    RecoverySessionFrontier frontier;
    frontier.completed_tick = adapter_->completed_tick(); frontier.input_delay = options_.input_delay; frontier.split = recovery_split_;
    frontier.mesh_incident = mesh_ ? mesh_->incident() : 0;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        auto cursor = command_cursors_[peer];
        if (const auto leave = agreed_departures_.find(peer); leave != agreed_departures_.end()
            && (!cursor || *cursor < leave->second.key.sequence)) cursor = leave->second.key.sequence;
        frontier.peers.push_back({peer == self_ ? next_seal_ : received_through_[peer], cursor, departed_[peer]});
    }
    for (const auto& [tick, batches] : ready_) {
        if (tick < frontier.completed_tick) continue;
        for (const auto& [peer, batch] : batches) if (!departed_[peer]) frontier.future.push_back(batch);
    }
    return core::Result<RecoverySessionFrontier>::success(std::move(frontier));
}
core::Result<void> LockstepSession::restore_recovery_frontier(const RecoverySessionFrontier& frontier,
    std::optional<SeatInput> rejoin) {
    const auto bad = [](std::string reason) {
        return lockstep_detail::refused<void>(reconnect_errors::log, std::move(reason));
    };
    const auto tick = adapter_->completed_tick();
    if (recorded_zero_ || !ready_.empty() || !pending_.empty() || failure_ || options_.adaptive_timing || (mesh_ && mesh_->running())
        || options_.reconnect || options_.resync || frontier.completed_tick != tick || initial_tick_ != tick
        || frontier.input_delay != options_.input_delay || frontier.peers.size() != seats_.size())
        return bad("invalid_private_recovery_frontier");
    if (frontier.split) return bad("divergent_epoch");
    if (static_cast<bool>(mesh_) != (frontier.mesh_incident != 0)) return bad("recovery_mesh_policy_mismatch");
    std::optional<PeerIndex> returning;
    if (rejoin) {
        if (!adapter_->is_rejoin(*rejoin) || rejoin->key.tick != tick || tick >= options_.final_tick_count) return bad("invalid_recorded_rejoin_tick");
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
            if (seats_[peer].seat == rejoin->key.seat) returning = peer;
        if (!returning || !seats_[*returning].may_submit || !frontier.peers[*returning].departed
            || (frontier.peers[*returning].producer_sequence && rejoin->key.sequence <= *frontier.peers[*returning].producer_sequence)
            || rejoin->key.sequence == std::numeric_limits<std::uint64_t>::max()) return bad("invalid_recorded_rejoin_seat");
    }
    if (frontier.peers[self_].departed && returning != self_) return bad("inactive_recovery_owner");
    if (frontier.future.size() > input_horizon() * seats_.size()) return bad("recovery_frontier_limit");
    std::map<std::uint64_t, std::map<PeerIndex, SealedBatch>> ready;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        const auto& row = frontier.peers[peer];
        if ((!row.departed && row.next_seal < tick) || row.next_seal > tick + input_horizon()
            || row.next_seal > options_.final_tick_count) return bad("invalid_recovery_sealed_cursor");
    }
    for (const auto& batch : frontier.future) {
        if (batch.sender >= seats_.size() || frontier.peers[batch.sender].departed || batch.tick < tick
            || batch.tick >= frontier.peers[batch.sender].next_seal || ready[batch.tick].contains(batch.sender))
            return bad("invalid_recovery_future_batch");
        const auto encoded = encode_batch(batch);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        for (const auto& input : batch.commands) {
            if (!seats_[batch.sender].may_submit || input.key.seat != seats_[batch.sender].seat
                || !frontier.peers[batch.sender].producer_sequence
                || input.key.sequence > *frontier.peers[batch.sender].producer_sequence) return bad("invalid_recovery_future_sequence");
            if (auto valid = adapter_->validate(input); !valid) return valid;
        }
        ready[batch.tick].emplace(batch.sender, batch);
    }
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (frontier.peers[peer].departed) continue;
        for (auto at = tick; at < frontier.peers[peer].next_seal; ++at)
            if (!ready[at].contains(peer)) return bad("recovery_frontier_gap");
    }
    // A forged future suffix must refuse before any live publication. Static
    // payload admission alone cannot detect a producer sequence repeated in two
    // different ticks. Start from the restored game's inclusive saved cursors.
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        std::optional<std::uint64_t> previous;
        if (const auto at = options_.producer_sequences.find(seats_[peer].seat); at != options_.producer_sequences.end()) previous = at->second;
        for (const auto& [at, batches] : ready) {
            (void)at;
            const auto batch = batches.find(peer);
            if (batch == batches.end()) continue;
            for (const auto& input : batch->second.commands) {
                if (previous && input.key.sequence <= *previous) return bad("recovery_future_sequence_repeated");
                previous = input.key.sequence;
            }
        }
        if (previous != frontier.peers[peer].producer_sequence) return bad("recovery_producer_cursor_mismatch");
    }
    if (rejoin) if (auto valid = adapter_->validate_recovery_input(*rejoin); !valid) return valid;
    ready_ = std::move(ready);
    std::vector<PeerIndex> inactive;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        const auto& row = frontier.peers[peer];
        departed_[peer] = row.departed && returning != peer;
        received_through_[peer] = row.departed ? tick : row.next_seal;
        command_cursors_[peer] = row.producer_sequence;
        accepted_[peer].resize(static_cast<std::size_t>(received_through_[peer] - tick));
        if (departed_[peer]) inactive.push_back(peer);
    }
    if (returning) command_cursors_[*returning] = rejoin->key.sequence;
    for (const auto& [at, batches] : ready_) for (const auto& [peer, batch] : batches) {
        const auto encoded = encode_batch(batch);
        accepted_[peer][static_cast<std::size_t>(at - tick)] = core::sha256(encoded.value());
        recovery_[peer].emplace(at, batch);
    }
    next_seal_ = received_through_[self_];
    if (command_cursors_[self_] && *command_cursors_[self_] == std::numeric_limits<std::uint64_t>::max()) return bad("recovery_sequence_exhausted");
    next_sequence_ = command_cursors_[self_] ? *command_cursors_[self_] + 1 : 0;
    if (mesh_) {
        if (!mesh_->restore_counter(frontier.mesh_incident)) return bad("recovery_mesh_counter");
        mesh_->set_active(mesh_active());
    }
    if (monitor_) if (auto reset = monitor_->on_reconnect(tick, adapter_->state_hash(), inactive); !reset) return reset;
    recorded_zero_ = true; recovery_rejoin_ = std::move(rejoin);
    return core::Result<void>::success();
}
} // namespace eawr::net
