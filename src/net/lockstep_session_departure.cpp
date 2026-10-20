#include "lockstep_session_internal.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <limits>

namespace eawr::net {
using namespace lockstep_detail;

core::Result<void> LockstepSession::broadcast_departure(const Bytes& frame) {
    // Agreement retires a voter immediately, even while its command awaits cutoff.
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || departed_[peer] || agreed_departures_.contains(peer) || peer == agreement_->target || mesh_excluded(peer)) continue;
        // A second loss can occur after this voter sent its first ack. Drain
        // that ordered proof before settling; never invent an absent vote.
        if (transport_->link(peer) == LinkState::closed && !(mesh_ && mesh_->owns_closed_link(peer))) continue;
        if (auto sent = send_mesh_control(peer, frame); !sent) return sent;
    }
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::begin_departure(PeerIndex target, DepartureReason reason, bool expired) {
    if (resync_ || (options_.resync && monitor_ && monitor_->frozen()))
        return refused<void>(resync_errors::overlap, "departure overlaps desync recovery");
    if (!encode_departure_ || target >= seats_.size() || target == self_
        || (reason != DepartureReason::disconnect && reason != DepartureReason::timeout && reason != DepartureReason::pair_loss)
        || (reason == DepartureReason::pair_loss && !(mesh_ && mesh_->removal_confirmed(target))))
        return refused<void>(departure_errors::agreement, "invalid departing peer or unsupported mode");
    if (held_ && held_->target != target)
        return refused<void>(departure_errors::reconnect_hold, "another departure cannot replace a held-seat recovery");
    if (agreement_) {
        if (agreement_->target == target) {
            if (expired) agreement_->reconnect_expired = true;
            return core::Result<void>::success();
        }
        // Local closure is rechecked after the first agreement settles. Its
        // immutable votes still require every original survivor's ack.
        return core::Result<void>::success();
    }
    if (timing_ && !timing_->retained)
        return core::Result<void>::success(); // Retain the full timing vote, or let its bounded deadline expire.
    Agreement state;
    state.target = target;
    state.frontiers.resize(seats_.size());
    state.reasons.resize(seats_.size(), DepartureReason::timeout);
    state.ready.resize(seats_.size());
    state.acknowledgements.resize(seats_.size());
    state.since_ms = last_now_.value_or(0);
    state.pair_loss = reason == DepartureReason::pair_loss;
    state.reconnect_expired = expired || state.pair_loss || (held_ && held_->target == target && held_->expiry_sent);
    if (options_.reconnect && options_.reconnect->allow_rejoin && !state.reconnect_expired) {
        if (reconnect_epoch_ == std::numeric_limits<std::uint64_t>::max())
            return refused<void>(reconnect_errors::epoch, "reconnect epoch exhausted");
        state.reconnect_epoch = ++reconnect_epoch_;
    }
    state.frontiers[self_] = received_through_[target];
    state.reasons[self_] = reason;
    state.ready[self_] = true;
    agreement_ = std::move(state);
    Bytes proposal{departure_proposal_kind};
    append(proposal, target, 4); append(proposal, static_cast<std::uint8_t>(reason), 1);
    append(proposal, received_through_[target], 8);
    if (auto sent = broadcast_departure(proposal); !sent) return sent;
    for (const auto& [tick, batch] : recovery_[target]) {
        static_cast<void>(tick);
        auto encoded = encode_batch(batch);
        if (!encoded) return core::Result<void>::failure(encoded.error());
        Bytes frame{departure_recovery_kind}; append(frame, target, 4);
        frame.insert(frame.end(), encoded.value().begin(), encoded.value().end());
        if (auto sent = broadcast_departure(frame); !sent) return sent;
    }
    Bytes ready{departure_ready_kind}; append(ready, target, 4);
    return broadcast_departure(ready);
}

core::Result<void> LockstepSession::receive_departure(const Received& received, bool deferred) {
    if (resync_ || (options_.resync && monitor_ && monitor_->frozen()))
        return refused<void>(resync_errors::overlap, "remote departure overlaps desync recovery");
    if (received.frame.front() > departure_ack_kind)
        return refused<void>(departure_errors::malformed, "unknown reserved departure message kind");
    auto bytes = std::span<const std::uint8_t>(received.frame).subspan(1);
    std::uint64_t target_value{};
    if (!read(bytes, target_value, 4) || target_value >= seats_.size()
        || target_value == self_ || target_value == received.from || departed_[received.from])
        return refused<void>(departure_errors::malformed, "invalid departure target or voter");
    const auto target = static_cast<PeerIndex>(target_value);
    // A seat the mesh removed no longer votes; whatever it still sends over a surviving pair is not part of the roster.
    if (mesh_excluded(received.from)) return core::Result<void>::success();
    if (agreed_departures_.contains(target)) {
        // Reliable-stream duplicates cannot change an already agreed decision.
        if (received.frame.front() == departure_ack_kind && bytes.size() == 8) {
            std::uint64_t cutoff{}; read(bytes, cutoff, 8);
            if (cutoff == agreed_departures_.at(target).key.tick) return core::Result<void>::success();
        }
        const auto edge = mesh_replay_.find(received.from);
        const bool replaced = edge != mesh_replay_.end() && edge->second.initial_generation != 0
            && transport_->edge_identity(received.from) != 0
            && transport_->edge_identity(received.from) != edge->second.initial_generation;
        if (mesh_ && replaced) {
            if (received.frame.front() == departure_ready_kind && bytes.empty()) return core::Result<void>::success();
            if (received.frame.front() == departure_proposal_kind) {
                std::uint64_t reason{}, frontier{};
                if (read(bytes, reason, 1) && read(bytes, frontier, 8) && bytes.empty() && reason >= 1 && reason <= 3
                    && frontier < options_.final_tick_count
                    && mesh_settled_votes_.contains(target)) {
                    const auto& votes = mesh_settled_votes_.at(target);
                    if (votes.frontiers[received.from] && *votes.frontiers[received.from] == frontier
                        && votes.reasons[received.from] == static_cast<DepartureReason>(reason))
                        return core::Result<void>::success();
                }
            }
            if (received.frame.front() == departure_recovery_kind) {
                auto batch = decode_batch(bytes);
                if (batch && batch.value().sender == target) {
                    const auto tick = batch.value().tick;
                    if (tick < accepted_[target].size() ? accepted_[target][static_cast<std::size_t>(tick)] == core::sha256(bytes)
                        : tick >= agreed_departures_.at(target).key.tick) return core::Result<void>::success();
                }
            }
        }
        return refused<void>(departure_errors::agreement, "message reopens a settled departure");
    }
    if (held_ && held_->target != target)
        return refused<void>(departure_errors::reconnect_hold, "remote departure overlaps held-seat recovery");
    const bool queued_sender = deferred_departures_.contains(received.from);
    const bool pending_timing = timing_ && !timing_->retained && !agreement_;
    if (!deferred && (queued_sender || pending_timing || (agreement_ && agreement_->target != target))) {
        // Stream order proves this sender finished voting on the first target.
        // Defer its whole second exchange, including recovery, in receive order.
        if (!queued_sender && !pending_timing && !agreement_->acknowledgements[received.from])
            return refused<void>(departure_errors::agreement, "overlapping departure precedes sender acknowledgement");
        // Retained history extends behind completion, and sealed lead extends
        // ahead of it. Two horizons cover both, plus the vote controls. Keep
        // the qualified floor; bytes have a separate hard memory ceiling.
        const auto frame_limit = departure_frame_limit(input_horizon(), seats_.size() - 1);
        if (received.frame.size() > max_batch_bytes - deferred_departure_bytes_
            || deferred_departure_frames_ >= frame_limit)
            return refused<void>(session_errors::resource_limit, "deferred departure controls exceed budget");
        deferred_departure_bytes_ += received.frame.size();
        ++deferred_departure_frames_;
        deferred_departures_[received.from].push_back(received);
        return core::Result<void>::success();
    }
    if (received.frame.front() == departure_proposal_kind) {
        std::uint64_t reason{}, frontier{};
        if (!read(bytes, reason, 1) || !read(bytes, frontier, 8) || !bytes.empty()
            || reason < 1 || reason > 3 || frontier >= options_.final_tick_count)
            return refused<void>(departure_errors::malformed, "invalid departure proposal");
        // A pair-loss proposal needs the matching accepted decision; the other reasons must not join one.
        const bool pair_loss = reason == static_cast<std::uint64_t>(DepartureReason::pair_loss);
        if (pair_loss != (mesh_ && mesh_->removal_confirmed(target)))
            return refused<void>(departure_errors::agreement, "departure reason disagrees with the accepted pair-loss decision");
        if (auto begun = begin_departure(target, static_cast<DepartureReason>(reason)); !begun) return begun;
        if (agreement_ && agreement_->pair_loss != pair_loss)
            return refused<void>(departure_errors::agreement, "voters disagree whether this is a pair-loss departure");
        auto& previous = agreement_->frontiers[received.from];
        if (previous && (*previous != frontier || agreement_->reasons[received.from] != static_cast<DepartureReason>(reason)))
            return refused<void>(departure_errors::agreement, "voter changed its departure proposal");
        previous = frontier;
        agreement_->reasons[received.from] = static_cast<DepartureReason>(reason);
        return core::Result<void>::success();
    }
    if (!agreement_ || agreement_->target != target || !agreement_->frontiers[received.from])
        return refused<void>(departure_errors::agreement, "departure data without voter proposal");
    if (received.frame.front() == departure_recovery_kind) {
        if (agreement_->ready[received.from])
            return refused<void>(departure_errors::agreement, "recovery after voter ready");
        auto decoded = decode_batch(bytes);
        if (!decoded || decoded.value().sender != target
            || decoded.value().tick >= *agreement_->frontiers[received.from])
            return refused<void>(departure_errors::malformed, "invalid recovered batch provenance");
        receiving_recovery_ = true;
        auto accepted = receive({target, Bytes(bytes.begin(), bytes.end())});
        receiving_recovery_ = false;
        return accepted;
    }
    if (received.frame.front() == departure_ready_kind) {
        if (!bytes.empty()) return refused<void>(departure_errors::malformed, "trailing ready bytes");
        agreement_->ready[received.from] = true;
        return core::Result<void>::success();
    }
    std::uint64_t cutoff{};
    if (!read(bytes, cutoff, 8) || !bytes.empty() || cutoff >= options_.final_tick_count
        || !agreement_->ready[received.from])
        return refused<void>(departure_errors::malformed, "invalid departure acknowledgement");
    auto& previous = agreement_->acknowledgements[received.from];
    if (previous && *previous != cutoff) return refused<void>(departure_errors::agreement, "voter changed acknowledgement");
    previous = cutoff;
    return advance_departure();
}

core::Result<void> LockstepSession::advance_departure() {
    // Receive still drains at match end, but cannot finalize another departure
    // after replay admission has ended (the same boundary as pump).
    if (adapter_->completed_tick() == options_.final_tick_count) return core::Result<void>::success();
    if (resync_ || (options_.resync && monitor_ && monitor_->frozen()))
        return refused<void>(resync_errors::overlap, "departure finalization overlaps desync recovery");
    auto& state = *agreement_;
    if (mesh_ && mesh_->running() && !state.pair_loss && mesh_->state() != MeshLossState::settling)
        state.since_ms = last_now_.value_or(0);
    if (last_now_.value_or(0) - state.since_ms >= options_.completeness_timeout_ms)
        return refused<void>(departure_errors::timeout, "departure agreement timed out");
    std::uint64_t cutoff{};
    DepartureReason reason = DepartureReason::timeout;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == state.target || departed_[peer] || agreed_departures_.contains(peer) || mesh_excluded(peer)) continue;
        if (!state.frontiers[peer] || !state.ready[peer]) return core::Result<void>::success();
        cutoff = std::max(cutoff, *state.frontiers[peer]);
        reason = std::min(reason, state.reasons[peer]);
    }
    if (state.pair_loss) reason = DepartureReason::pair_loss;
    std::optional<sim::SeatCommandKey> sealed_quit;
    for (const auto& [tick, batch] : recovery_[state.target]) {
        static_cast<void>(tick);
        for (const auto& command : batch.commands)
            if (adapter_->is_departure(command) && (!sealed_quit || command.key < *sealed_quit)) sealed_quit = command.key;
    }
    if (sealed_quit) { cutoff = sealed_quit->tick; reason = DepartureReason::quit; }
    if (!state.cutoff) {
        if (cutoff >= options_.final_tick_count
            || (sealed_quit ? received_through_[state.target] <= cutoff
                : cutoff < adapter_->completed_tick() || received_through_[state.target] != cutoff))
            return refused<void>(departure_errors::agreement, "recovery does not cover agreed frontier");
        state.cutoff = cutoff;
        state.acknowledgements[self_] = cutoff;
        Bytes ack{departure_ack_kind}; append(ack, state.target, 4); append(ack, cutoff, 8);
        if (auto sent = broadcast_departure(ack); !sent) return sent;
    }
    bool received_survivor_vote{};
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == state.target || departed_[peer] || agreed_departures_.contains(peer) || mesh_excluded(peer)) continue;
        if (!state.acknowledgements[peer]) return core::Result<void>::success();
        if (*state.acknowledgements[peer] != cutoff)
            return refused<void>(departure_errors::agreement, "departure cutoff votes disagree");
        received_survivor_vote = received_survivor_vote || peer != self_;
    }
    if (mesh_) {
        mesh_departure_ack_ = std::make_pair(state.target, cutoff);
        mesh_settled_votes_[state.target] = {state.frontiers, state.reasons};
    }
    // An opted-in diagnosed freeze failed above; it cannot turn the same vote
    // into a locally chosen ordinary departure instead of a reconnect hold.
    if (options_.reconnect && options_.reconnect->allow_rejoin && reconnect_available_ && !state.reconnect_expired && !state.pair_loss && !sealed_quit
        && (options_.resync || !monitor_ || !monitor_->frozen())) {
        held_ = HeldSeat{state.target, cutoff, last_now_.value_or(0), state.reconnect_epoch,
            std::vector<bool>(seats_.size()), std::vector<bool>(seats_.size()), std::vector<bool>(seats_.size()),
            false, false, false, false, false, {}, 0};
        held_->reason = reason;
        held_->progress_since_ms = held_->since_ms;
        agreement_.reset();
        return core::Result<void>::success();
    }
    // GCMP-09 / D5 may continue a campaign alone with fallback control. This never bypasses a required survivor
    // vote: all original survivors proved their frontiers/acks above. MP-34/36 retain connection loss by default.
    const bool continue_alone = !received_survivor_vote
        && options_.unconfirmed_departure == UnconfirmedDeparture::continue_alone;
    if (!sealed_quit && !state.pair_loss && !continue_alone
        && (reason == DepartureReason::timeout || !received_survivor_vote))
        return core::Result<void>::failure(connection_loss(state.target, "Peer departure was not confirmed"));
    const auto cursor = command_cursors_[state.target];
    if (cursor && *cursor == std::numeric_limits<std::uint64_t>::max())
        return refused<void>(departure_errors::agreement, "departure sequence exhausted");
    if (monitor_ && !departed_[state.target]) {
        auto changed = monitor_->on_departure(state.target, cutoff);
        if (!changed) return changed;
    }
    quit_in_batch_[state.target] = sealed_quit.has_value();
    agreed_departures_[state.target] = {sealed_quit.value_or(sim::SeatCommandKey{
        cutoff, seats_[state.target].seat, cursor ? *cursor + 1 : 0}), reason};
    completed_reconnect_epoch_ = std::max(completed_reconnect_epoch_, state.reconnect_epoch);
    pending_rejoin_.reset(); pending_rejoin_proofs_.clear(); pending_expiry_proofs_.clear();
    agreement_.reset();
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::service_departures(bool receive_drained) {
    if (agreement_) {
        if (auto advanced = advance_departure(); !advanced) return advanced;
    }
    // Different streams may be on different decisions. A later decision on
    // one stream cannot obstruct another stream's votes for the current one.
    bool progressed = true;
    while (progressed && !(timing_ && !timing_->retained && !agreement_)) {
        progressed = false;
        for (auto entry = deferred_departures_.begin(); entry != deferred_departures_.end();) {
            const auto& next = entry->second.front();
            std::uint64_t target{};
            auto bytes = std::span<const std::uint8_t>(next.frame).subspan(1);
            if (!read(bytes, target, 4)) return refused<void>(departure_errors::malformed, "invalid deferred target");
            if (agreement_ && agreement_->target != target && !agreed_departures_.contains(static_cast<PeerIndex>(target))) {
                ++entry;
                continue;
            }
            auto frame = std::move(entry->second.front());
            deferred_departure_bytes_ -= frame.frame.size();
            --deferred_departure_frames_;
            entry->second.pop_front();
            if (entry->second.empty()) entry = deferred_departures_.erase(entry);
            else ++entry;
            if (auto accepted = receive_departure(frame, true); !accepted) return accepted;
            progressed = true;
        }
        if (agreement_) {
            if (auto advanced = advance_departure(); !advanced) return advanced;
        }
    }
    // Scan after settling/draining: a previously postponed local loss must
    // begin before any timing service or simulation advancement sees its voter.
    for (std::size_t pass = 0; pass < seats_.size(); ++pass) {
        while (!agreement_ && !mesh_pending_.empty() && receive_drained) {
            const auto target = mesh_pending_.front();
            if (departed_[target] || agreed_departures_.contains(target)) { mesh_pending_.pop_front(); continue; }
            if (auto begun = begin_departure(target, DepartureReason::pair_loss); !begun) return begun;
            break;
        }
        // While three or more seats are active, a closed pair belongs to the mesh incident, not to a single-seat departure.
        if (encode_departure_ && receive_drained)
            for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
                if (peer == self_ || departed_[peer] || agreed_departures_.contains(peer) || (mesh_ && mesh_->owns_closed_link(peer))
                    || (held_ && held_->target == peer) || transport_->link(peer) != LinkState::closed) continue;
                if (auto begun = begin_departure(peer, options_.closed_link_reason ? options_.closed_link_reason(peer)
                    : DepartureReason::disconnect); !begun) return begun;
            }
        if (!agreement_) return core::Result<void>::success();
        if (auto advanced = advance_departure(); !advanced) return advanced;
        if (agreement_ || held_) return core::Result<void>::success();
    }
    return core::Result<void>::success();
}

} // namespace eawr::net
