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
    if (departed_[self_] || agreement_ || held_ || resync_ || (options_.resync && monitor_ && monitor_->frozen())
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

core::Result<void> LockstepSession::receive(Received received) {
    if (!received.frame.empty() && (received.frame.front() == timing_proposal_kind || received.frame.front() == timing_ready_kind)) {
        if (received.from >= seats_.size() || received.from == self_ || received.frame.size() != 17)
            return refused<void>(timing_errors::malformed, "timing frame has invalid sender or size");
        return receive_timing(received);
    }
    if (received.from >= seats_.size() || received.from == self_ || received.frame.empty()
        || received.frame.size() > max_batch_bytes + 5)
        return refused<void>(session_errors::malformed, "frame has invalid transport sender or size");
    const auto kind = received.frame.front();
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
        if (accepted_[sender].at(batch.tick) == digest) return core::Result<void>::success();
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

} // namespace eawr::net
