#include "lockstep_session_internal.hpp"

namespace eawr::net {
using namespace lockstep_detail;

MeshSeats LockstepSession::mesh_voters() const noexcept {
    MeshSeats voters{};
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if (timing_voter(peer)) voters = static_cast<MeshSeats>(voters | mesh_seat(peer));
    return voters;
}

MeshSeats LockstepSession::mesh_active() const noexcept {
    MeshSeats active{};
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (!timing_voter(peer)) continue;
        // A seat never leaves its own set; the exclusions below are reported to the others in every link report.
        if (peer != self_) {
            if (quit_seen_[peer]) continue;
            if (agreement_ && agreement_->target == peer && !agreement_->pair_loss) continue;
            if (held_ && held_->target == peer) continue;
        }
        active = static_cast<MeshSeats>(active | mesh_seat(peer));
    }
    return active;
}

core::Result<void> LockstepSession::queue_mesh_departure(const Received& received, bool& queued) {
    queued = false;
    if (!mesh_) return core::Result<void>::success();
    auto bytes = std::span<const std::uint8_t>(received.frame).subspan(1);
    std::uint64_t target{}, reason{};
    // A frame the departure handler refuses outright (no target, the receiver itself or its own sender) is not held back.
    if (!read(bytes, target, 4) || target >= seats_.size() || target == self_ || target == received.from) return core::Result<void>::success();
    // Settled votes are replay validation, not an early proposal for a future
    // mesh decision. Let receive_departure compare their exact retained vote.
    if (agreed_departures_.contains(static_cast<PeerIndex>(target))) return core::Result<void>::success();
    const bool ordered = mesh_early_senders_.contains(received.from);
    const bool proposal = received.frame.front() == departure_proposal_kind && read(bytes, reason, 1)
        && reason == static_cast<std::uint64_t>(DepartureReason::pair_loss);
    // A survivor that already holds the confirmation can start the removal before ours arrives (different streams).
    // Its controls wait, in their arrival order, for the accepted decision; nothing else is held back.
    if (!ordered && (!proposal || mesh_->removal_confirmed(static_cast<PeerIndex>(target))))
        return core::Result<void>::success();
    if (mesh_early_.size() >= departure_frame_limit(input_horizon(), seats_.size() - 1)
        || received.frame.size() > max_batch_bytes - mesh_early_bytes_)
        return refused<void>(session_errors::resource_limit, "queued pair-loss departure controls exceed budget");
    if (mesh_early_.empty()) mesh_early_since_ms_ = last_now_.value_or(0);
    mesh_early_bytes_ += received.frame.size();
    mesh_early_.push_back(received);
    mesh_early_senders_.insert(received.from);
    queued = true;
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::send_mesh_control(PeerIndex peer, const Bytes& frame) {
    if (!(mesh_ && mesh_->owns_closed_link(peer))) return transport_->send(peer, frame);
    auto [entry, inserted] = mesh_replay_.try_emplace(peer, transport_->edge_identity(peer), next_seal_);
    static_cast<void>(inserted);
    auto& replay = entry->second;
    if (replay.controls.empty() && transport_->link(peer) == LinkState::open) {
        if (auto sent = transport_->send(peer, frame); sent) return sent;
    }
    if (frame.size() > max_batch_bytes - replay.bytes) {
        transport_->fail_repair(peer, 1794); replay.controls.clear(); replay.bytes = 0;
        return core::Result<void>::success();
    }
    replay.controls.push_back(frame); replay.bytes += frame.size();
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::restore_mesh_controls(PeerIndex peer) {
    auto& replay = mesh_replay_.at(peer);
    replay.controls.clear(); replay.bytes = 0;
    const auto add = [&](Bytes frame) {
        if (frame.size() > max_batch_bytes - replay.bytes) return false;
        replay.bytes += frame.size(); replay.controls.push_back(std::move(frame)); return true;
    };
    const auto overflow = [&] {
        replay.controls.clear(); replay.bytes = 0; transport_->fail_repair(peer, 1794);
        return core::Result<void>::success();
    };
    // A peer can lag only the most recently completed vote: completing the
    // next one would require its ACK. Replay that last ACK before a new vote.
    if (mesh_departure_ack_ && mesh_departure_ack_->first != peer
        && (!agreement_ || agreement_->target != mesh_departure_ack_->first)) {
        Bytes ack{departure_ack_kind}; append(ack, mesh_departure_ack_->first, 4); append(ack, mesh_departure_ack_->second, 8);
        if (!add(std::move(ack))) return overflow();
    }
    if (agreement_ && agreement_->target != peer) {
        const auto& vote = *agreement_;
        Bytes proposal{departure_proposal_kind}; append(proposal, vote.target, 4);
        append(proposal, static_cast<std::uint8_t>(vote.reasons[self_]), 1); append(proposal, *vote.frontiers[self_], 8);
        if (!add(std::move(proposal))) return overflow();
        for (const auto& [tick, batch] : recovery_[vote.target]) {
            if (tick >= *vote.frontiers[self_]) continue;
            auto encoded = encode_batch(batch);
            if (!encoded) return core::Result<void>::failure(encoded.error());
            Bytes frame{departure_recovery_kind}; append(frame, vote.target, 4);
            frame.insert(frame.end(), encoded.value().begin(), encoded.value().end());
            if (!add(std::move(frame))) return overflow();
        }
        if (vote.ready[self_]) { Bytes ready{departure_ready_kind}; append(ready, vote.target, 4); if (!add(std::move(ready))) return overflow(); }
        if (vote.acknowledgements[self_]) {
            Bytes ack{departure_ack_kind}; append(ack, vote.target, 4); append(ack, *vote.acknowledgements[self_], 8);
            if (!add(std::move(ack))) return overflow();
        }
    }
    const auto timing_frame = [](std::uint8_t kind, const TimingEvent& event) {
        Bytes frame{kind}; append(frame, event.tick, 8); append(frame, event.delay, 8); return frame;
    };
    if (options_.adaptive_timing) {
        if (!timing_ && last_timing_ && !add(timing_frame(timing_ready_kind, *last_timing_))) return overflow();
        if (timing_) {
            if (self_ == options_.adaptive_timing->host && timing_->proposed
                && !add(timing_frame(timing_proposal_kind, timing_->event))) return overflow();
            if (timing_->ready[self_] && !add(timing_frame(timing_ready_kind, timing_->event))) return overflow();
        }
    }
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::service_mesh(std::uint64_t now_ms, bool receive_drained) {
    if (!mesh_) return core::Result<void>::success();
    // Setup pumps tick zero behind its monitor gate before the committed game
    // installs departure encoding. Pair loss there still aborts setup itself.
    if (!encode_departure_ && adapter_->completed_tick() == 0 && monitor_ && !monitor_->may_step())
        return core::Result<void>::success();
    if (!encode_departure_) return refused<void>(mesh_errors::options, "room mesh pair-loss agreement requires enabled departures");
    mesh_->set_active(mesh_active());
    MeshSeats owned{};
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if (mesh_->owns_closed_link(peer)) owned = static_cast<MeshSeats>(owned | mesh_seat(peer));
    const auto ownership = [owned](PeerIndex peer) { return peer < max_match_seats && (owned & mesh_seat(peer)) != 0; };
    if (monitor_) if (auto configured = monitor_->on_link_repair(self_, ownership); !configured) return configured;
    // Resend only this sender's original sealed tail on a newly authenticated
    // edge. The input horizon bounds retention; ordinary duplicate validation
    // checks every replay against the receiver's accepted digest.
    const bool retry_monitor = monitor_ && !monitor_->may_step() && now_ms >= mesh_monitor_retry_ms_;
    if (retry_monitor) mesh_monitor_retry_ms_ = now_ms + 1000;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || departed_[peer] || mesh_excluded(peer) || transport_->link(peer) != LinkState::open) continue;
        const auto generation = transport_->edge_identity(peer);
        if (generation == 0) continue;
        auto [it, inserted] = mesh_replay_.try_emplace(peer, generation, next_seal_);
        auto& replay = it->second;
        if (replay.initial_generation == 0) replay.initial_generation = generation;
        auto& known = replay.generation; auto& cursor = replay.cursor;
        if (!inserted && known != generation) {
            if (mesh_tail_unavailable_) { transport_->fail_repair(peer, 1794); continue; }
            known = generation;
            cursor = recovery_[self_].empty() ? next_seal_ : recovery_[self_].begin()->first;
            if (auto restored = restore_mesh_controls(peer); !restored) return restored;
        }
        unsigned budget = 256;
        while (!replay.controls.empty() && budget > 0) {
            if (auto sent = transport_->send(peer, replay.controls.front()); !sent) break;
            replay.bytes -= replay.controls.front().size(); replay.controls.pop_front(); --budget;
        }
        if (!replay.controls.empty() || transport_->link(peer) != LinkState::open) continue;
        for (auto batch = recovery_[self_].lower_bound(cursor); batch != recovery_[self_].end() && budget > 0; ++batch, --budget) {
            auto frame = encode_batch(batch->second);
            if (!frame) return core::Result<void>::failure(frame.error());
            if (auto sent = transport_->send(peer, frame.value()); !sent) break;
            last_sent_batch_[peer] = batch->first;
            cursor = batch->first + 1;
        }
        if (monitor_ && cursor == next_seal_ && (retry_monitor || mesh_monitor_generation_[peer] != generation)) {
            if (auto sent = monitor_->on_link_repair(peer, ownership); sent) mesh_monitor_generation_[peer] = generation;
        }
    }

    if (auto serviced = mesh_->service(std::chrono::milliseconds(static_cast<std::int64_t>(now_ms)), receive_drained); !serviced) return serviced;
    while (auto result = mesh_->take_result()) {
        if (mesh_results_.size() >= 64) mesh_results_.erase(mesh_results_.begin());
        mesh_results_.push_back(*result);
        if (result->kind == MeshIncidentResult::Kind::leave) return refused<void>(result->code, result->message);
        if (result->kind == MeshIncidentResult::Kind::restored) {
            mesh_early_.clear(); mesh_early_senders_.clear(); mesh_early_bytes_ = 0;
            continue;
        }
        mesh_removed_ = result->removed;
        for (PeerIndex seat = 0; seat < seats_.size(); ++seat)
            if ((result->removed & mesh_seat(seat)) && !departed_[seat] && !agreed_departures_.contains(seat)) mesh_pending_.push_back(seat);
    }
    // Pair-loss controls that never meet an accepted decision (a forged proposal, or a decision that died with its decider)
    // expire with the incident's own deadlines instead of waiting forever.
    if (mesh_removed_ == 0 && !mesh_early_.empty() && !mesh_->running()
        && now_ms - mesh_early_since_ms_ > static_cast<std::uint64_t>((options_.mesh->repair_grace + std::chrono::seconds(25)).count()))
        return refused<void>(departure_errors::agreement, "pair-loss departure controls arrived without an accepted decision");
    if (mesh_removed_ == 0) return core::Result<void>::success();
    if (!mesh_early_.empty()) {
        auto frames = std::move(mesh_early_);
        mesh_early_.clear(); mesh_early_senders_.clear(); mesh_early_bytes_ = 0;
        for (const auto& frame : frames)
            if (auto handled = receive_departure(frame); !handled) return handled;
    }
    while (!mesh_pending_.empty() && (departed_[mesh_pending_.front()] || agreed_departures_.contains(mesh_pending_.front())))
        mesh_pending_.pop_front();
    if (mesh_pending_.empty() && !agreement_) {
        mesh_removed_ = 0;
        mesh_->settled();
    }
    return core::Result<void>::success();
}

} // namespace eawr::net
