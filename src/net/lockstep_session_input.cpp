#include "lockstep_session_internal.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <limits>

namespace eawr::net {
using namespace lockstep_detail;

namespace {
core::Result<void> validate_batch(const SealedBatch& batch) {
    if (batch.sender >= max_match_seats || batch.tick >= max_session_ticks || batch.sequence != batch.tick
        || batch.commands.size() > max_batch_commands)
        return refused<void>(session_errors::malformed, "invalid sealed batch envelope");
    std::optional<sim::SeatCommandKey> previous;
    std::size_t size = 25;
    for (const auto& command : batch.commands) {
        if (command.key.tick != batch.tick || command.key.seat.value == 0 || command.bytes.empty()
            || (previous && (previous->seat != command.key.seat || previous->sequence >= command.key.sequence)))
            return refused<void>(session_errors::malformed, "sealed command key disagrees with envelope or order");
        if (size > max_batch_bytes - 16 || command.bytes.size() > max_batch_bytes - size - 16)
            return refused<void>(session_errors::resource_limit, "sealed batch exceeds byte limit");
        size += 16 + command.bytes.size();
        previous = command.key;
    }
    return core::Result<void>::success();
}
} // namespace

core::Result<Bytes> encode_batch(const SealedBatch& batch) {
    if (auto valid = validate_batch(batch); !valid) return core::Result<Bytes>::failure(valid.error());
    Bytes frame{sealed_batch_kind};
    append(frame, batch.sender, 4);
    append(frame, batch.tick, 8);
    append(frame, batch.sequence, 8);
    append(frame, batch.commands.size(), 4);
    for (const auto& command : batch.commands) {
        append(frame, command.key.seat.value, 4);
        append(frame, command.key.sequence, 8);
        append(frame, command.bytes.size(), 4);
        frame.insert(frame.end(), command.bytes.begin(), command.bytes.end());
    }
    return core::Result<Bytes>::success(std::move(frame));
}

core::Result<SealedBatch> decode_batch(std::span<const std::uint8_t> frame) {
    if (frame.empty() || frame.size() > max_batch_bytes || frame.front() != sealed_batch_kind)
        return refused<SealedBatch>(session_errors::malformed, "invalid batch kind or frame size");
    frame = frame.subspan(1);
    std::uint64_t sender{}, tick{}, sequence{}, count{};
    if (!read(frame, sender, 4) || !read(frame, tick, 8) || !read(frame, sequence, 8)
        || !read(frame, count, 4) || count > max_batch_commands || count > frame.size() / 16)
        return refused<SealedBatch>(session_errors::malformed, "truncated batch or invalid command count");
    SealedBatch batch{static_cast<PeerIndex>(sender), tick, sequence, {}};
    for (std::uint64_t index = 0; index < count; ++index) {
        std::uint64_t seat{}, command_sequence{}, length{};
        if (!read(frame, seat, 4) || !read(frame, command_sequence, 8) || !read(frame, length, 4)
            || length == 0 || length > frame.size())
            return refused<SealedBatch>(session_errors::malformed, "truncated command or invalid payload length");
        batch.commands.push_back({{tick, sim::SeatId{static_cast<std::uint32_t>(seat)}, command_sequence},
            Bytes(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(length))});
        frame = frame.subspan(static_cast<std::size_t>(length));
    }
    if (!frame.empty()) return refused<SealedBatch>(session_errors::malformed, "trailing batch bytes");
    if (auto valid = validate_batch(batch); !valid) return core::Result<SealedBatch>::failure(valid.error());
    return core::Result<SealedBatch>::success(std::move(batch));
}

core::Result<sim::SeatCommandKey> LockstepSession::submit(
    const EncodeIntent& encode, const std::optional<std::uint64_t> input_epoch) {
    if (failure_) return core::Result<sim::SeatCommandKey>::failure(*failure_);
    if (checkpoint_ || recovery_rejoin_ || departed_[self_] || agreement_ || held_ || resync_
        || (options_.resync && monitor_ && monitor_->frozen())
        || (options_.reconnect && options_.reconnect->rejoin_source))
        return refused<sim::SeatCommandKey>(session_errors::admission, "seat departed or agreement pending");
    const auto completed = adapter_->completed_tick();
    if (input_epoch && (!options_.keep_admission_tick_open || *input_epoch > completed))
        return refused<sim::SeatCommandKey>(session_errors::admission, "invalid LAN producer epoch");
    auto tick = std::max(next_seal_, input_epoch.value_or(completed) + options_.input_delay);
    // A decrease must not put a newer seat sequence before an older pending command.
    if (options_.adaptive_timing && !pending_.empty()) tick = std::max(tick, pending_.rbegin()->first);
    if (!seats_[self_].may_submit || !encode || tick >= options_.final_tick_count
        || next_sequence_ == std::numeric_limits<std::uint64_t>::max())
        return refused<sim::SeatCommandKey>(session_errors::admission, "no authorized open input tick or sequence");
    const sim::SeatCommandKey key{tick, seats_[self_].seat, next_sequence_};
    auto encoded = encode(key);
    if (!encoded) return core::Result<sim::SeatCommandKey>::failure(encoded.error());
    SeatInput input{key, std::move(encoded).value()};
    if (auto valid = adapter_->validate(input); !valid) return core::Result<sim::SeatCommandKey>::failure(valid.error());
    auto candidate = pending_[tick];
    candidate.push_back(std::move(input));
    auto frame = encode_batch({self_, tick, tick, candidate});
    if (!frame) return core::Result<sim::SeatCommandKey>::failure(frame.error());
    pending_[tick] = std::move(candidate);
    ++next_sequence_;
    return core::Result<sim::SeatCommandKey>::success(key);
}

core::Result<void> LockstepSession::seal() {
    const auto completed = adapter_->completed_tick();
    auto through = completed + options_.input_delay
        - (options_.keep_admission_tick_open ? 1U : 0U);
    if (options_.keep_admission_tick_open && options_.pending_input_epoch)
        if (const auto epoch = options_.pending_input_epoch(); epoch && *epoch <= completed)
            through = std::min(through, *epoch + options_.input_delay - 1U);
    if (checkpoint_) {
        const auto limit = checkpoint_->cutoff.value_or(*checkpoint_->drains[self_]);
        if (limit == 0) return refused<void>(session_errors::options, "invalid save cutoff");
        through = std::min(through, limit - 1);
    }
    while (next_seal_ < options_.final_tick_count && next_seal_ <= through) {
        SealedBatch batch{self_, next_seal_, next_seal_, {}};
        if (auto found = pending_.find(next_seal_); found != pending_.end()) {
            batch.commands = std::move(found->second);
            pending_.erase(found);
        }
        auto frame = encode_batch(batch);
        if (!frame) return core::Result<void>::failure(frame.error());
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
            if (peer == self_ || departed_[peer] || agreed_departures_.contains(peer) || mesh_excluded(peer)
                || (agreement_ && agreement_->target == peer) || (held_ && held_->target == peer)
                || transport_->link(peer) == LinkState::closed) continue;
            if (mesh_) {
                const auto replay = mesh_replay_.find(peer);
                if (replay != mesh_replay_.end() && (replay->second.cursor < next_seal_ || !replay->second.controls.empty())) continue;
            }
            auto sent = transport_->send(peer, frame.value());
            if (sent) last_sent_batch_[peer] = next_seal_;
            if (sent && mesh_) {
                const auto replay = mesh_replay_.find(peer);
                if (replay != mesh_replay_.end()) replay->second.cursor = next_seal_ + 1;
            }
            // A stream can close during send. Already received complete ticks remain usable;
            // a missing batch from this peer is diagnosed by completeness below.
            if (!sent && transport_->link(peer) != LinkState::closed && !(mesh_ && mesh_->owns_closed_link(peer))) return sent;
        }
        if (stage_observer_) for (const auto& input : batch.commands)
            stage_observer_("seal", input, adapter_->completed_tick());
        if (mesh_ && !mesh_tail_unavailable_) {
            constexpr std::size_t tail_limit = 8 * max_batch_bytes;
            if (frame.value().size() > tail_limit - mesh_tail_bytes_) {
                mesh_tail_unavailable_ = true; recovery_[self_].clear(); mesh_tail_bytes_ = 0;
                for (auto& [peer, replay] : mesh_replay_) {
                    if (replay.cursor >= next_seal_ && replay.controls.empty()) continue;
                    transport_->fail_repair(peer, 1794); replay.controls.clear(); replay.bytes = 0;
                }
            } else {
                recovery_[self_].emplace(next_seal_, batch); mesh_tail_bytes_ += frame.value().size();
            }
        }
        ready_[next_seal_].emplace(self_, std::move(batch));
        const auto& sealed = ready_[next_seal_].at(self_);
        if (!sealed.commands.empty()) command_cursors_[self_] = sealed.commands.back().key.sequence;
        ++next_seal_;
    }
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::pump_galactic_recovery(bool obsolete_owner, bool published_owner) {
    if (!galactic_recovery_handler_ || (!obsolete_owner && !published_owner && !failure_ && (!encode_departure_ || !departed_[self_])))
        return core::Result<void>::success();
    // Preserve the old active owner on a pre-commit refusal, and give the
    // published owner a boundary at release. Both must retain gameplay frames.
    const auto budget = obsolete_owner || published_owner ? std::size_t{1} : options_.max_frames_per_pump;
    for (std::size_t count = 0; count < budget; ++count) {
        auto frame = transport_->poll();
        if (!frame) break;
        if (frame->from < seats_.size() && frame->from != self_ && !frame->frame.empty()
            && frame->frame.front() >= 90 && frame->frame.front() <= 98)
            galactic_recovery_handler_(std::move(*frame));
        else if (published_owner || (obsolete_owner && !failure_ && (!encode_departure_ || !departed_[self_]))) {
            constexpr std::size_t byte_limit = 8 * max_batch_bytes;
            const auto frame_limit = departure_frame_limit(input_horizon(), seats_.size() - 1);
            if (frame->frame.size() > max_batch_bytes + 5 || frame->frame.size() > byte_limit - galactic_recovery_input_bytes_
                || galactic_recovery_input_.size() >= frame_limit)
                return refused<void>(session_errors::resource_limit, "retained recovery input exceeds its bound");
            galactic_recovery_input_bytes_ += frame->frame.size();
            galactic_recovery_input_.push_back(std::move(*frame));
        }
    }
    return core::Result<void>::success();
}
core::Result<void> LockstepSession::receive(Received received) {
    if (galactic_recovery_handler_ && !received.frame.empty() && received.frame.front() >= 90 && received.frame.front() <= 98) {
        galactic_recovery_handler_(std::move(received));
        return core::Result<void>::success();
    }
    if (!received.frame.empty() && (received.frame.front() == timing_proposal_kind || received.frame.front() == timing_ready_kind)) {
        if (received.from >= seats_.size() || received.from == self_ || received.frame.size() != 17)
            return refused<void>(timing_errors::malformed, "timing frame has invalid sender or size");
        return receive_timing(received);
    }
    if (received.from >= seats_.size() || received.from == self_ || received.frame.empty()
        || received.frame.size() > max_batch_bytes + 5)
        return refused<void>(session_errors::malformed, "frame has invalid transport sender or size");
    const auto kind = received.frame.front();
    if (kind >= checkpoint_drain_kind && kind <= checkpoint_release_kind) return receive_checkpoint(received);
    if (kind >= mesh_report_kind && kind <= mesh_confirm_kind) {
        if (!mesh_) return refused<void>(mesh_errors::malformed, "pair-loss control without a room mesh");
        return mesh_->receive(received, std::chrono::milliseconds(last_now_.value_or(0)));
    }
    if (kind >= resync_vote_kind && kind <= resync_abort_kind) return receive_resync(received);
    if (options_.reconnect && !options_.reconnect->allow_rejoin && kind >= rejoin_hello_kind && kind <= rejoin_expire_kind)
        return receive_reconnect(received);
    if (resync_ && kind >= rejoin_hello_kind && kind <= rejoin_expire_kind)
        return refused<void>(resync_errors::overlap, "reconnect overlaps desync recovery");
    if (kind >= rejoin_hello_kind && kind <= rejoin_expire_kind) return receive_reconnect(received);
    // A replacement stream cannot submit gameplay before its barrier proof.
    if (options_.reconnect && options_.reconnect->rejoin_source && !rejoin_restored_) {
        reconnect_notice(reconnect_errors::stale, "ignored pre-restoration traffic on the replacement stream");
        return core::Result<void>::success();
    }
    if (held_ && received.from == held_->target && kind == sealed_batch_kind && !held_->resuming) {
        reconnect_notice(reconnect_errors::stale, "ignored delayed gameplay from a held seat");
        return core::Result<void>::success();
    }
    if (kind >= departure_proposal_kind && kind <= 55) {
        bool queued{};
        if (auto early = queue_mesh_departure(received, queued); !early) return early;
        if (queued) return core::Result<void>::success();
        return receive_departure(received);
    }
    if (kind != sealed_batch_kind) {
        // The old monitor window ends at the independently proved rejoin barrier.
        // Its in-flight exchanges cannot gate headless advancement to that barrier.
        if (resync_ && kind >= 32 && kind <= 39 && received.frame.size() >= 17) {
            auto envelope = std::span<const std::uint8_t>(received.frame).subspan(1);
            std::uint64_t first{}, last{};
            if (read(envelope, first, 8) && read(envelope, last, 8) && last > resync_->tick) {
                if (resync_->monitor_frames.size() >= 2 * seats_.size())
                    return refused<void>(resync_errors::malformed, "resync monitor tail exceeds its bound");
                resync_->monitor_frames.push_back(std::move(received));
                return core::Result<void>::success();
            }
        }
        if (held_ && kind >= 32 && kind <= 39) return core::Result<void>::success();
        if (kind >= 32 && kind <= 39 && monitor_) return monitor_->receive(std::move(received));
        if (handler_) return handler_(std::move(received));
        return refused<void>(session_errors::malformed, "unhandled session message kind");
    }
    const auto sender = received.from;
    if (departed_[sender] || agreed_departures_.contains(sender)) return core::Result<void>::success();
    if (agreement_ && agreement_->target == sender && !receiving_recovery_) return core::Result<void>::success();
    auto decoded = decode_batch(received.frame);
    if (!decoded) return core::Result<void>::failure(decoded.error());
    const auto& batch = decoded.value();
    if (batch.sender != sender)
        return refused<void>(session_errors::admission, "batch sender does not match authenticated stream");
    const auto digest = core::sha256(received.frame);
    const auto expected = received_through_[sender];
    if (batch.tick < expected) {
        if (batch.tick >= initial_tick_ && accepted_[sender].at(batch.tick - initial_tick_) == digest) return core::Result<void>::success();
        return refused<void>(session_errors::conflicting_duplicate, "late batch contradicts accepted sealed input");
    }
    if (batch.tick != expected)
        return refused<void>(session_errors::stream_gap, "reliable stream lost or reordered a sealed batch");
    if (batch.tick >= options_.final_tick_count || batch.tick > adapter_->completed_tick() + input_horizon() - 1)
        return refused<void>(session_errors::resource_limit, "batch exceeds reachable input horizon");
    auto cursor = command_cursors_[sender];
    for (const auto& input : batch.commands) {
        if (!seats_[sender].may_submit || input.key.seat != seats_[sender].seat
            || (cursor && input.key.sequence <= *cursor))
            return refused<void>(session_errors::admission, "seat not bound to sender or command sequence does not advance");
        if (auto valid = adapter_->validate(input); !valid) return valid;
        cursor = input.key.sequence;
        if (adapter_->is_departure(input)) quit_seen_[sender] = true;
    }
    command_cursors_[sender] = cursor;
    accepted_[sender].push_back(digest);
    recovery_[sender].emplace(batch.tick, batch);
    if (stage_observer_) for (const auto& input : batch.commands)
        stage_observer_("receive", input, adapter_->completed_tick());
    ready_[batch.tick].emplace(sender, std::move(decoded).value());
    ++received_through_[sender];
    return core::Result<void>::success();
}


core::Result<void> LockstepSession::broadcast_checkpoint(const Bytes& frame) {
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || !(checkpoint_ ? checkpoint_->voters[peer] : timing_voter(peer))) continue;
        if (transport_->link(peer) == LinkState::closed) continue;
        auto sent = transport_->send(peer, frame);
        if (!sent && transport_->link(peer) != LinkState::closed) return sent;
    }
    return core::Result<void>::success();
}
core::Result<void> LockstepSession::abort_checkpoint(std::string message, bool broadcast) {
    if (!checkpoint_) return core::Result<void>::success();
    const auto epoch = checkpoint_->epoch;
    if (broadcast && self_ != checkpoint_callbacks_->host) {
        // A peer can request refusal, but cannot withdraw its proof or decide
        // locally: the host may already have committed and sent its release.
        if (!checkpoint_->refusal_requested) {
            checkpoint_->refusal_requested = true;
            Bytes frame{checkpoint_release_kind}; append(frame, epoch, 8); append(frame, 0, 1);
            const auto host = checkpoint_callbacks_->host;
            if (transport_->link(host) != LinkState::closed) {
                auto sent = transport_->send(host, frame);
                if (!sent && transport_->link(host) != LinkState::closed) return sent;
            }
        }
        return core::Result<void>::success();
    }
    if (broadcast) {
        Bytes frame{checkpoint_release_kind}; append(frame, epoch, 8); append(frame, 0, 1);
        if (auto sent = broadcast_checkpoint(frame); !sent) return sent;
    }
    completed_checkpoint_ = epoch; checkpoint_.reset(); early_checkpoint_.clear();
    if (checkpoint_callbacks_->refused)
        checkpoint_callbacks_->refused(diagnostic(session_errors::admission, "Save failed: " + message));
    return core::Result<void>::success();
}
core::Result<void> LockstepSession::begin_checkpoint() {
    if (!checkpoint_callbacks_ || !checkpoint_callbacks_->capture || !checkpoint_callbacks_->commit
        || checkpoint_callbacks_->host >= seats_.size())
        return refused<void>(session_errors::options, "save barrier callbacks are incomplete");
    const auto epoch = adapter_->completed_tick();
    if (!epoch) return refused<void>(session_errors::admission, "save barrier requires a completed trigger tick");
    const auto drain = std::max({epoch, next_seal_, pending_.empty() ? epoch : pending_.rbegin()->first + 1});
    CheckpointBarrier barrier;
    barrier.epoch = epoch; barrier.since_ms = last_now_.value_or(0); barrier.drains.resize(seats_.size());
    barrier.voters.resize(seats_.size());
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        barrier.voters[peer] = timing_voter(peer);
        if (!barrier.voters[peer]) barrier.drains[peer] = epoch;
    }
    barrier.drains[self_] = drain; checkpoint_ = std::move(barrier);
    if (agreement_ || held_ || resync_ || timing_ || (mesh_ && mesh_->running()))
        return abort_checkpoint("a roster or timing transition overlaps capture");
    Bytes frame{checkpoint_drain_kind}; append(frame, epoch, 8); append(frame, drain, 8);
    if (auto sent = broadcast_checkpoint(frame); !sent) return sent;
    auto early = std::exchange(early_checkpoint_, {});
    for (const auto& message : early) {
        if (auto accepted = receive_checkpoint(message); !accepted) return accepted;
    }
    return core::Result<void>::success();
}
core::Result<void> LockstepSession::receive_checkpoint(const Received& received) {
    if (!checkpoint_callbacks_ || received.from >= seats_.size() || received.from == self_)
        return refused<void>(session_errors::malformed, "save control without an agreed save service");
    if (!(checkpoint_ ? checkpoint_->voters[received.from] : timing_voter(received.from)))
        return core::Result<void>::success();
    const auto kind = received.frame.front();
    const std::size_t expected_size = kind == checkpoint_proof_kind ? 105U : (kind == checkpoint_release_kind ? 10U : 17U);
    if (received.frame.size() != expected_size)
        return refused<void>(session_errors::malformed, "invalid save control size");
    auto body = std::span<const std::uint8_t>(received.frame).subspan(1);
    std::uint64_t epoch{};
    if (!read(body, epoch, 8) || !epoch)
        return refused<void>(session_errors::malformed, "truncated save epoch");
    if (epoch <= completed_checkpoint_) return core::Result<void>::success();
    if (!checkpoint_) {
        // Another peer can finish the trigger tick before this peer. Controls
        // cannot activate a barrier: only the sealed SaveRequest can do that.
        // The bounded queue is consumed on begin and discarded on refusal.
        if (epoch <= adapter_->completed_tick() || epoch > adapter_->completed_tick() + input_horizon()
            || early_checkpoint_.size() >= 4 * seats_.size())
            return refused<void>(session_errors::malformed, "save control outside the pending sealed horizon");
        early_checkpoint_.push_back(received);
        return core::Result<void>::success();
    }
    auto& barrier = *checkpoint_;
    if (epoch != barrier.epoch) return refused<void>(session_errors::malformed, "overlapping save epochs");
    if (kind == checkpoint_drain_kind) {
        std::uint64_t drain{};
        if (!read(body, drain, 8) || !body.empty() || drain < epoch
            || drain > options_.final_tick_count || drain > epoch + input_horizon())
            return refused<void>(session_errors::malformed, "invalid save drain frontier");
        auto& previous = barrier.drains[received.from];
        if (previous && *previous != drain) return refused<void>(session_errors::conflicting_duplicate, "save drain frontier changed");
        previous = drain;
    } else if (kind == checkpoint_cutoff_kind) {
        std::uint64_t cutoff{};
        if (received.from != checkpoint_callbacks_->host || !read(body, cutoff, 8) || !body.empty()
            || cutoff < *barrier.drains[self_] || cutoff > epoch + input_horizon()
            || cutoff > options_.final_tick_count || (barrier.cutoff && *barrier.cutoff != cutoff))
            return refused<void>(session_errors::malformed, "invalid host save cutoff");
        barrier.cutoff = cutoff;
    } else if (kind == checkpoint_proof_kind) {
        if (body.size() != 96) return refused<void>(session_errors::malformed, "invalid save proof size");
        CheckpointProof proof{};
        for (auto& digest : proof) { std::copy_n(body.begin(), digest.size(), digest.begin()); body = body.subspan(digest.size()); }
        const auto [at, inserted] = barrier.proofs.emplace(received.from, proof);
        if (!inserted && at->second != proof) return refused<void>(session_errors::conflicting_duplicate, "save proof changed");
    } else if (kind == checkpoint_release_kind) {
        std::uint64_t success{};
        if (!read(body, success, 1) || success > 1 || !body.empty()
            || (success && received.from != checkpoint_callbacks_->host))
            return refused<void>(session_errors::malformed, "invalid save release");
        if (!success) {
            if (received.from == checkpoint_callbacks_->host)
                return abort_checkpoint("the host refused capture agreement", false);
            if (self_ != checkpoint_callbacks_->host)
                return refused<void>(session_errors::malformed, "save refusal request is not addressed to the host");
            return abort_checkpoint("a peer could not capture or agree");
        }
        barrier.release = true;
    }
    return core::Result<void>::success();
}
core::Result<bool> LockstepSession::service_checkpoint() {
    const auto refuse_save = [&](std::string message) -> core::Result<bool> {
        auto aborted = abort_checkpoint(std::move(message));
        if (!aborted) return core::Result<bool>::failure(aborted.error());
        return core::Result<bool>::success(checkpoint_.has_value());
    };
    auto& barrier = *checkpoint_;
    const auto finish = [&]() -> core::Result<bool> {
        if (self_ == checkpoint_callbacks_->host && !barrier.release) {
            Bytes frame{checkpoint_release_kind}; append(frame, barrier.epoch, 8); append(frame, 1, 1);
            if (auto sent = broadcast_checkpoint(frame); !sent) return core::Result<bool>::failure(sent.error());
            barrier.release = true;
        }
        if (!barrier.release) return core::Result<bool>::success(true);
        auto committed = checkpoint_callbacks_->commit();
        if (!committed) return core::Result<bool>::failure(committed.error());
        completed_checkpoint_ = barrier.epoch; checkpoint_.reset();
        return core::Result<bool>::success(false);
    };
    // Only a host release resolves the barrier. An unresolvable hold ends the
    // session via its normal connection-loss route, without a history decision.
    const auto host = checkpoint_callbacks_->host;
    if (self_ != host && !barrier.release
        && (!timing_voter(host) || transport_->link(host) == LinkState::closed))
        return core::Result<bool>::failure(connection_loss(host, "save barrier unresolved: host connection lost"));
    if (!barrier.proofs_matched && barrier.captured
        && barrier.proofs.size() == static_cast<std::size_t>(std::count(barrier.voters.begin(), barrier.voters.end(), true))) {
        barrier.proofs_matched = std::all_of(barrier.proofs.begin(), barrier.proofs.end(), [&](const auto& row) {
            return row.second == barrier.proofs.at(self_);
        });
    }
    if (barrier.proofs_matched && !barrier.decision_since_ms) barrier.decision_since_ms = last_now_.value_or(0);
    if (barrier.proofs_matched && barrier.release) return finish();
    // Two minutes permits delayed release beyond ordinary completeness timeout.
    // Wall-clock expiry terminates participation, never aborts or commits history.
    constexpr std::uint64_t decision_timeout_ms = 120'000;
    if (self_ != host
        && last_now_.value_or(0) - barrier.decision_since_ms.value_or(barrier.since_ms) >= decision_timeout_ms)
        return core::Result<bool>::failure(connection_loss(host, "save barrier unresolved: host connection lost"));
    if (barrier.proofs_matched) return finish();
    if (barrier.refusal_requested) return core::Result<bool>::success(true);
    if (self_ == checkpoint_callbacks_->host
        && last_now_.value_or(0) - barrier.since_ms >= options_.completeness_timeout_ms)
        return refuse_save("capture agreement timed out");
    if (agreement_ || held_ || resync_ || timing_ || (mesh_ && mesh_->running()))
        return refuse_save("a roster or timing transition interrupted capture");
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (barrier.voters[peer] != timing_voter(peer)) return refuse_save("the save voter roster changed");
        if (peer != self_ && barrier.voters[peer] && transport_->link(peer) == LinkState::closed)
            return refuse_save("a required peer stream closed");
    }
    if (self_ == checkpoint_callbacks_->host && !barrier.cutoff
        && std::all_of(barrier.drains.begin(), barrier.drains.end(), [](const auto& value) { return value.has_value(); })) {
        std::uint64_t cutoff = barrier.epoch;
        for (const auto& drain : barrier.drains) cutoff = std::max(cutoff, *drain);
        barrier.cutoff = cutoff;
        Bytes frame{checkpoint_cutoff_kind}; append(frame, barrier.epoch, 8); append(frame, cutoff, 8);
        if (auto sent = broadcast_checkpoint(frame); !sent) return core::Result<bool>::failure(sent.error());
    }
    const auto limit = barrier.cutoff.value_or(*barrier.drains[self_]);
    if (adapter_->completed_tick() < limit) return core::Result<bool>::success(false);
    if (!barrier.cutoff) return core::Result<bool>::success(true);
    if (adapter_->completed_tick() != limit || !pending_.empty()) return refuse_save("accepted input remains beyond the save barrier");
    if (!barrier.captured) {
        std::map<sim::SeatId, std::uint64_t> sequences;
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
            if (barrier.voters[peer] && command_cursors_[peer]) sequences.emplace(seats_[peer].seat, *command_cursors_[peer]);
        auto captured = checkpoint_callbacks_->capture(sequences);
        if (!captured) return refuse_save(captured.error().message);
        barrier.proofs.emplace(self_, captured.value()); barrier.captured = true;
        Bytes frame{checkpoint_proof_kind}; append(frame, barrier.epoch, 8);
        for (const auto& digest : captured.value()) frame.insert(frame.end(), digest.begin(), digest.end());
        if (auto sent = broadcast_checkpoint(frame); !sent) return core::Result<bool>::failure(sent.error());
    }
    if (barrier.proofs.size() != static_cast<std::size_t>(std::count(barrier.voters.begin(), barrier.voters.end(), true)))
        return core::Result<bool>::success(true);
    for (const auto& [peer, proof] : barrier.proofs) {
        (void)peer;
        if (proof != barrier.proofs.at(self_)) return refuse_save("canonical capture, state or frontier digests differ");
    }
    barrier.proofs_matched = true;
    barrier.decision_since_ms = last_now_.value_or(0);
    return finish();
}

} // namespace eawr::net
