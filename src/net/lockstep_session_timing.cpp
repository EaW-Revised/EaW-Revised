#include "lockstep_session_internal.hpp"
#include <algorithm>

namespace eawr::net {
using namespace lockstep_detail;

std::uint64_t adaptive_input_delay(std::uint64_t worst_rtt_ms, bool broadband) noexcept {
    // MP-05 at the fixed logical 30 Hz rate. Beyond this bound the lead is capped;
    // bounding before multiplication also handles arbitrary transport counters.
    const auto ping = std::min<std::uint64_t>(worst_rtt_ms, 2640);
    const auto latency = std::max<std::uint64_t>(2 * ((120 * ping / 100) / (1000 / 30)) / 3,
        broadband ? 4U : 6U);
    return 2 * std::min<std::uint64_t>(latency / 2, 32);
}

std::uint64_t LockstepSession::input_horizon() const noexcept {
    auto delay = options_.input_delay;
    if (adapter_->completed_tick() < retain_until_) delay = std::max(delay, retained_delay_);
    if (timing_) delay = std::max(delay, timing_->event.delay);
    return sealed_input_horizon(delay);
}

core::Result<void> LockstepSession::notify_timing() {
    if (monitor_) return monitor_->on_input_delay(options_.input_delay, (input_horizon() - 2) / 2);
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::replay_timing_control(std::span<const std::uint8_t> bytes) {
    if (failure_) return core::Result<void>::failure(*failure_);
    if (!options_.adaptive_timing || !options_.non_lan || bytes.empty() || bytes.front() != timing_proposal_kind)
        return refused<void>(timing_errors::agreement, "timing replay outside adaptive catch-up policy");
    bytes = bytes.subspan(1);
    TimingEvent event;
    if (!read(bytes, event.tick, 8) || !read(bytes, event.delay, 8) || !bytes.empty()
        || event.tick != adapter_->completed_tick() || event.tick >= options_.final_tick_count
        || event.delay < 4 || event.delay > 64 || event.delay % 2 != 0)
        return refused<void>(timing_errors::malformed, "timing replay does not match original activation tick");
    if (last_timing_ && *last_timing_ == event) return notify_timing();
    if ((last_timing_ && last_timing_->tick >= event.tick) || (timing_ && timing_->event != event))
        return refused<void>(timing_errors::agreement, "timing replay conflicts with retained event");
    retained_delay_ = options_.input_delay;
    retain_until_ = event.tick + sealed_input_horizon(retained_delay_);
    options_.input_delay = event.delay;
    const auto cadence = 64 * std::max<std::uint64_t>(1, event.delay / 2);
    next_policy_tick_ = (event.tick / cadence + 1) * cadence;
    last_timing_ = event;
    timing_.reset();
    return notify_timing();
}

bool LockstepSession::timing_voter(PeerIndex peer) const noexcept {
    return !departed_[peer] && !agreed_departures_.contains(peer) && !mesh_excluded(peer);
}

bool LockstepSession::timing_frozen() const noexcept {
    if (!options_.adaptive_timing) return false;
    const auto host = options_.adaptive_timing->host;
    const auto link = host == self_ ? LinkState::open : transport_->link(host);
    return mesh_timing_frozen(mesh_voters(), link) || (mesh_ && mesh_->running());
}

core::Result<void> LockstepSession::restore_future_timing(std::span<const std::uint8_t> bytes) {
    if (!options_.adaptive_timing || bytes.empty() || bytes.front() != timing_proposal_kind) return core::Result<void>::success();
    bytes = bytes.subspan(1);
    TimingEvent event;
    if (!read(bytes, event.tick, 8) || !read(bytes, event.delay, 8) || !bytes.empty()
        || event.tick < adapter_->completed_tick() || event.tick < retain_until_ || event.tick >= options_.final_tick_count
        || event.delay < 4 || event.delay > 64 || event.delay % 2 != 0)
        return refused<void>(timing_errors::malformed, "invalid future timing control in verified reconnect log");
    if (timing_ && timing_->event != event) return refused<void>(timing_errors::agreement, "conflicting future reconnect timing");
    // This log contains already agreed controls, so restoring it needs no new votes.
    timing_ = TimingAgreement{event, std::vector<bool>(seats_.size(), true), true, last_now_.value_or(0), true};
    return notify_timing();
}

core::Result<void> LockstepSession::broadcast_timing(const Bytes& frame) {
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || !timing_voter(peer)) continue;
        if (auto sent = send_mesh_control(peer, frame); !sent) return sent;
    }
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::receive_timing(const Received& received) {
    if (!options_.adaptive_timing)
        return refused<void>(timing_errors::agreement, "timing message outside adaptive roster");
    if (!timing_voter(received.from) || (agreement_ && agreement_->target == received.from))
        return core::Result<void>::success();
    auto bytes = std::span<const std::uint8_t>(received.frame).subspan(1);
    TimingEvent event;
    if (!read(bytes, event.tick, 8) || !read(bytes, event.delay, 8) || !bytes.empty()
        || event.delay < 4 || event.delay > 64 || event.delay % 2 != 0
        || event.tick >= options_.final_tick_count)
        return refused<void>(timing_errors::malformed, "invalid sealed timing event");
    const bool proposal = received.frame.front() == timing_proposal_kind;
    if (proposal && received.from != options_.adaptive_timing->host)
        return refused<void>(timing_errors::agreement, "timing proposal is not from the host");
    if (last_timing_ && event == *last_timing_) return core::Result<void>::success();
    if (agreement_) {
        // An ordered departure ack can immediately precede the host proposal.
        // Settle completed votes now; a different stream may still hold one ack.
        if (auto advanced = advance_departure(); !advanced) return advanced;
        if (agreement_ && !agreement_->acknowledgements[received.from])
            return refused<void>(timing_errors::agreement, "timing control precedes sender departure acknowledgement");
    }
    if (!timing_) {
        const auto tick = adapter_->completed_tick();
        if (event.tick <= tick + sealed_input_horizon(options_.input_delay) || event.tick <= next_seal_
            || event.tick - tick > 4 * sealed_input_horizon(64)
            || event.tick < retain_until_)
            return refused<void>(timing_errors::agreement, "timing event overlaps immutable horizon or agreement");
        timing_ = TimingAgreement{event, std::vector<bool>(seats_.size()), false, last_now_.value_or(0)};
    }
    if (timing_->event != event)
        return refused<void>(timing_errors::agreement, "conflicting timing event");
    if (!proposal) timing_->ready[received.from] = true;
    if (proposal) timing_->proposed = true;
    if (auto acknowledged = acknowledge_timing(); !acknowledged) return acknowledged;
    return notify_timing();
}

core::Result<void> LockstepSession::acknowledge_timing() {
    if (!timing_ || agreement_ || !timing_->proposed || timing_->ready[self_]) return core::Result<void>::success();
    timing_->ready[self_] = true;
    Bytes ready{timing_ready_kind};
    append(ready, timing_->event.tick, 8); append(ready, timing_->event.delay, 8);
    return broadcast_timing(ready);
}

core::Result<void> LockstepSession::advance_timing() {
    if (!options_.adaptive_timing) return core::Result<void>::success();
    const auto tick = adapter_->completed_tick();
    if (timing_) {
        if (auto acknowledged = acknowledge_timing(); !acknowledged) return acknowledged;
        bool ready = timing_->proposed;
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
            if (!timing_voter(peer)) continue;
            if (held_ && held_->target == peer && timing_->retained) continue;
            if (peer != self_ && transport_->link(peer) == LinkState::closed && !encode_departure_)
                return refused<void>(timing_errors::agreement, "timing voter stream closed");
            ready = ready && timing_->ready[peer];
        }
        if (!ready && mesh_ && mesh_->running() && mesh_->state() != MeshLossState::settling)
            timing_->since_ms = last_now_.value_or(0);
        if (!ready && last_now_.value_or(0) - timing_->since_ms >= options_.completeness_timeout_ms)
            return refused<void>(timing_errors::timeout, "timing agreement expired");
        if (ready && !timing_->retained) {
            Bytes control{timing_proposal_kind};
            append(control, timing_->event.tick, 8); append(control, timing_->event.delay, 8);
            if (options_.retain_timing_event) {
                auto retained = options_.retain_timing_event(timing_->event.tick, control);
                if (!retained) return retained;
            }
            if (auto retained = retain_reconnect_control(timing_->event.tick, control); !retained) return retained;
            timing_->retained = true;
        }
        if (tick == timing_->event.tick && ready) {
            retained_delay_ = options_.input_delay;
            retain_until_ = tick + sealed_input_horizon(retained_delay_);
            options_.input_delay = timing_->event.delay;
            const auto cadence = 64 * std::max<std::uint64_t>(1, options_.input_delay / 2);
            next_policy_tick_ = (tick / cadence + 1) * cadence;
            last_timing_ = timing_->event;
            timing_.reset();
        }
        return notify_timing();
    }
    if (self_ != options_.adaptive_timing->host || tick < next_policy_tick_ || tick < retain_until_ || agreement_ || held_
        || !deferred_departures_.empty())
        return notify_timing();
    const auto cadence = 64 * std::max<std::uint64_t>(1, options_.input_delay / 2);
    // Catch-up and a retained transition can pass a policy boundary. Rejoin on
    // the next global 64R boundary, rather than inventing a wall-clock policy pass.
    if (tick % cadence != 0) {
        next_policy_tick_ = (tick / cadence + 1) * cadence;
        return notify_timing();
    }
    next_policy_tick_ = tick + cadence;
    std::uint64_t worst{};
    if (mesh_) {
        // The host measures only its own pairs: for the others it takes the smaller of the two ends' reports, then the
        // largest over pairs, so one liar can only lower its own pairs (docs/net-online-npeer.md, section 3).
        if (timing_frozen()) return notify_timing();
        const auto rtt = mesh_rtt_ms(mesh_voters(), mesh_->rtt_reports(std::chrono::milliseconds(last_now_.value_or(0))));
        if (!rtt || *rtt >= mesh_rtt_unmeasured) return notify_timing();
        worst = *rtt;
    } else {
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
            if (peer == self_ || !timing_voter(peer)) continue;
            std::optional<std::uint64_t> sample;
            if (options_.adaptive_timing->rtt_ms) sample = options_.adaptive_timing->rtt_ms(peer);
            else if (const auto stats = transport_->link_stats(peer);
                stats && stats->sample_count != 0 && stats->smoothed_rtt.count() >= 0) {
                const auto micros = static_cast<std::uint64_t>(stats->smoothed_rtt.count());
                sample = micros / 1000 + (micros % 1000 != 0 ? 1U : 0U);
            }
            if (!sample) return notify_timing();
            worst = std::max(worst, *sample);
        }
    }
    const auto delay = adaptive_input_delay(worst, options_.adaptive_timing->broadband);
    if (delay == options_.input_delay) return notify_timing();
    const auto effective = tick + 2 * sealed_input_horizon(std::max(delay, options_.input_delay)) + 1;
    if (effective >= options_.final_tick_count) return notify_timing();
    timing_ = TimingAgreement{{effective, delay}, std::vector<bool>(seats_.size()), true, last_now_.value_or(0)};
    timing_->ready[self_] = true;
    Bytes proposal{timing_proposal_kind}; append(proposal, effective, 8); append(proposal, delay, 8);
    if (auto sent = broadcast_timing(proposal); !sent) return sent;
    // Host readiness also traverses every stream, so a peer applying the event knows
    // that every other peer has received the proposal, even across skewed links.
    Bytes ready{timing_ready_kind}; append(ready, effective, 8); append(ready, delay, 8);
    if (auto sent = broadcast_timing(ready); !sent) return sent;
    return notify_timing();
}

} // namespace eawr::net
