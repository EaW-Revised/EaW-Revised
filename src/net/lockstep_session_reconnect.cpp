#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif
#include "lockstep_session_internal.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>
#include <limits>
#include <cstdlib>
#ifndef _WIN32
#include <cerrno>
#include <sys/random.h>
#endif

namespace eawr::net {
using namespace lockstep_detail;

namespace {
core::Result<RejoinNonce> fresh_nonce() {
    RejoinNonce nonce{};
#ifdef _WIN32
    // The CRT uses the OS cryptographic random generator, independent of rand().
    for (std::size_t offset = 0; offset < nonce.size(); offset += 4) {
        unsigned int word{};
        if (rand_s(&word) != 0) return refused<RejoinNonce>(reconnect_errors::nonce, "OS nonce generation failed");
        for (unsigned byte = 0; byte < 4; ++byte)
            nonce[offset + byte] = static_cast<std::uint8_t>(word >> (byte * 8U));
    }
#else
    std::size_t offset{};
    while (offset < nonce.size()) {
        const auto count = getrandom(nonce.data() + offset, nonce.size() - offset, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return refused<RejoinNonce>(reconnect_errors::nonce, "OS nonce generation failed");
        offset += static_cast<std::size_t>(count);
    }
#endif
    return core::Result<RejoinNonce>::success(nonce);
}

bool constant_equal(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right) {
    if (left.size() != right.size()) return false;
    std::uint8_t difference{};
    for (std::size_t index = 0; index < left.size(); ++index) difference |= left[index] ^ right[index];
    return difference == 0;
}

Bytes hello_frame(std::uint64_t epoch, std::uint8_t phase, std::uint32_t seat,
    const std::array<std::uint8_t, 32>& identity) {
    Bytes frame{rejoin_hello_kind}; append(frame, epoch, 8); append(frame, phase, 1); append(frame, seat, 4);
    frame.insert(frame.end(), identity.begin(), identity.end());
    return frame;
}
} // namespace

core::Result<RejoinNonce> make_rejoin_nonce() { return fresh_nonce(); }

bool LockstepSession::can_replay_control(std::span<const std::uint8_t> bytes) const noexcept {
    return options_.reconnect && (options_.reconnect->replay_control
        || (options_.adaptive_timing && bytes.size() == 17 && bytes.front() == timing_proposal_kind));
}

core::Result<void> LockstepSession::apply_reconnect_control(std::span<const std::uint8_t> bytes) {
    if (options_.adaptive_timing && !bytes.empty() && bytes.front() == timing_proposal_kind)
        return replay_timing_control(bytes);
    if (options_.reconnect && options_.reconnect->replay_control) return options_.reconnect->replay_control(bytes);
    return refused<void>(reconnect_errors::options, "unsupported retained session control");
}

ReconnectStatus LockstepSession::reconnect_status() const noexcept {
    if (options_.reconnect && options_.reconnect->rejoin_source)
        return {rejoin_restored_ ? ReconnectPhase::proving : ReconnectPhase::catching_up,
            self_, options_.reconnect->rejoin_epoch, held_ ? held_->cutoff : 0};
    if (held_) return {held_->offered ? ReconnectPhase::proving : ReconnectPhase::held,
        held_->target, held_->epoch, held_->cutoff};
    if (agreement_ && options_.reconnect)
        return {ReconnectPhase::agreeing, agreement_->target, agreement_->reconnect_epoch, 0};
    return {};
}

std::optional<std::uint64_t> LockstepSession::reconnect_grace_remaining_ms() const noexcept {
    if (!options_.reconnect || !held_ || held_->progress_started) return std::nullopt;
    const auto elapsed = last_now_.value_or(held_->since_ms) - held_->since_ms;
    const auto grace = options_.reconnect->grace_ms;
    return elapsed >= grace ? 0 : grace - elapsed;
}

core::Result<void> LockstepSession::retain_reconnect_control(std::uint64_t tick, std::span<const std::uint8_t> bytes) {
    if (!options_.reconnect || !reconnect_available_) return core::Result<void>::success();
    if (tick < adapter_->completed_tick() || tick >= options_.final_tick_count || bytes.empty()
        || bytes.size() > max_batch_bytes || !can_replay_control(bytes))
        return refused<void>(reconnect_errors::options, "invalid retained session control");
    std::size_t queued{};
    for (const auto& [at, controls] : reconnect_controls_) {
        static_cast<void>(at);
        for (const auto& control : controls) queued += control.size() + 4;
    }
    if (queued + bytes.size() + 4 > options_.reconnect->max_log_bytes - reconnect_bytes_) {
        disable_reconnect("history budget exhausted; play continues without reconnect");
        return core::Result<void>::success();
    }
    reconnect_controls_[tick].emplace_back(bytes.begin(), bytes.end());
    return core::Result<void>::success();
}

void LockstepSession::reconnect_notice(std::string_view code, std::string message) {
    // Keep the persistent unavailable notice even if delayed packets also arrive.
    if (reconnect_notice_ && reconnect_notice_->code == reconnect_errors::unavailable) return;
    reconnect_notice_ = diagnostic(code, std::move(message));
    reconnect_notice_->severity = core::Severity::warning;
}

void LockstepSession::disable_reconnect(std::string message) {
    reconnect_available_ = false;
    reconnect_notice(reconnect_errors::unavailable, std::move(message));
    if (!held_) {
        std::vector<Bytes>{}.swap(reconnect_log_); reconnect_bytes_ = 0;
        std::erase_if(reconnect_controls_, [&](const auto& row) {
            return std::find(recovered_control_ticks_.begin(), recovered_control_ticks_.end(), row.first) == recovered_control_ticks_.end();
        });
    }
}

core::Result<void> LockstepSession::report_replay_progress(std::uint64_t tick) {
    Bytes frame{rejoin_barrier_kind}; append(frame, held_->epoch, 8);
    append(frame, 2, 1); append(frame, *options_.reconnect->rejoin_source, 4); append(frame, tick, 8);
    if (auto sent = transport_->send(*options_.reconnect->rejoin_source, frame); !sent) return sent;
    // Drive a bounded amount of I/O between completed replay ticks. This flushes
    // progress on TCP too; the viewer must run this work off its render thread.
    for (std::size_t count = 0; count < options_.max_frames_per_pump; ++count) {
        auto received = transport_->poll(); if (!received) break;
        if (auto accepted = receive(std::move(*received)); !accepted) return accepted;
    }
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::check_held_survivors() const {
    if (held_ && !options_.reconnect->rejoin_source)
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
            if (peer != self_ && peer != held_->target && !departed_[peer]
                && !agreed_departures_.contains(peer) && transport_->link(peer) == LinkState::closed)
                return refused<void>(departure_errors::reconnect_hold, "survivor lost during held-seat recovery");
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::reconnect_broadcast(std::span<const std::uint8_t> frame) {
    // Receive callbacks can relay progress before pump finishes draining frames.
    // Apply the same held-survivor loss rule before any broadcast side effect.
    if (auto checked = check_held_survivors(); !checked) return checked;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || departed_[peer]) continue;
        // Expiry needs only the old surviving roster; the dropped link stays closed.
        if (held_ && peer == held_->target && (frame.front() == rejoin_expire_kind
            || (frame.front() == rejoin_barrier_kind && frame.size() == 22))) continue;
        auto sent = transport_->send(peer, frame);
        if (!sent) return sent;
    }
    return core::Result<void>::success();
}

core::Result<Bytes> LockstepSession::reconnect_package() const {
    Bytes bytes;
    const auto& options = *options_.reconnect;
    append(bytes, options.setup.size(), 4);
    bytes.insert(bytes.end(), options.setup.begin(), options.setup.end());
    append(bytes, options_.input_delay, 8);
    append(bytes, reconnect_log_.size(), 8);
    for (const auto& row : reconnect_log_) {
        append(bytes, row.size(), 4); bytes.insert(bytes.end(), row.begin(), row.end());
    }
    // The open frontier matters as much as the executed prefix: every immutable
    // future batch and accepted sequence cursor survives the transport replacement.
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        append(bytes, peer == self_ ? next_seal_ : received_through_[peer], 8);
        append(bytes, command_cursors_[peer].has_value(), 1);
        append(bytes, command_cursors_[peer].value_or(0), 8);
        append(bytes, departed_[peer], 1);
    }
    std::size_t count{};
    for (const auto& [tick, batches] : ready_) { static_cast<void>(tick); count += batches.size(); }
    append(bytes, count, 4);
    for (const auto& [tick, batches] : ready_) for (const auto& [peer, batch] : batches) {
        static_cast<void>(tick); static_cast<void>(peer);
        auto frame = encode_batch(batch);
        if (!frame) return core::Result<Bytes>::failure(frame.error());
        append(bytes, frame.value().size(), 4);
        bytes.insert(bytes.end(), frame.value().begin(), frame.value().end());
    }
    // Retain controls already agreed for future activation as well.
    append(bytes, reconnect_controls_.size(), 4);
    for (const auto& [tick, controls] : reconnect_controls_) {
        append(bytes, tick, 8); append(bytes, controls.size(), 4);
        for (const auto& control : controls) {
            append(bytes, control.size(), 4); bytes.insert(bytes.end(), control.begin(), control.end());
        }
    }
    if (bytes.size() > options.max_log_bytes)
        return refused<Bytes>(reconnect_errors::limit, "setup and reconnect log exceed agreed transfer budget");
    return core::Result<Bytes>::success(std::move(bytes));
}

core::Result<void> LockstepSession::install_reconnect_package() {
    if (core::sha256(incoming_package_) != incoming_digest_)
        return refused<void>(reconnect_errors::log, "reconnect transfer digest mismatch");
    auto bytes = std::span<const std::uint8_t>(incoming_package_);
    std::uint64_t size{}, delay{}, count{};
    if (!read(bytes, size, 4) || size > bytes.size()
        || size != options_.reconnect->setup.size()
        || !std::equal(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size), options_.reconnect->setup.begin()))
        return refused<void>(reconnect_errors::identity, "transferred setup differs from locally verified setup");
    bytes = bytes.subspan(static_cast<std::size_t>(size));
    if (!read(bytes, delay, 8) || delay == 0 || delay > max_input_delay || !read(bytes, count, 8)
        || count != held_->cutoff || count > bytes.size() / 20)
        return refused<void>(reconnect_errors::log, "invalid reconnect prefix or input delay");
    for (std::uint64_t tick = 0; tick < count; ++tick) {
        if (!read(bytes, size, 4) || size > bytes.size())
            return refused<void>(reconnect_errors::log, "truncated replay row");
        Bytes retained(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        auto row = bytes.first(static_cast<std::size_t>(size)); bytes = bytes.subspan(static_cast<std::size_t>(size));
        std::uint64_t at{}, controls{}, commands{};
        if (!read(row, at, 8) || at != tick || !read(row, controls, 4) || controls > row.size() / 5)
            return refused<void>(reconnect_errors::log, "noncontiguous replay row");
        for (std::uint64_t index = 0; index < controls; ++index) {
            if (!read(row, size, 4) || size == 0 || size > row.size() || !can_replay_control(row.first(static_cast<std::size_t>(size))))
                return refused<void>(reconnect_errors::log, "unsupported or truncated replay control");
            auto applied = apply_reconnect_control(row.first(static_cast<std::size_t>(size)));
            if (!applied) return applied;
            row = row.subspan(static_cast<std::size_t>(size));
        }
        if (!read(row, commands, 4) || commands > max_batch_commands * seats_.size() || commands > row.size() / 17)
            return refused<void>(reconnect_errors::log, "invalid replay command count");
        std::vector<SeatInput> inputs;
        for (std::uint64_t index = 0; index < commands; ++index) {
            std::uint64_t seat{}, sequence{};
            if (!read(row, seat, 4) || !read(row, sequence, 8) || !read(row, size, 4) || size == 0 || size > row.size())
                return refused<void>(reconnect_errors::log, "truncated replay input");
            SeatInput input{{tick, sim::SeatId{static_cast<std::uint32_t>(seat)}, sequence},
                Bytes(row.begin(), row.begin() + static_cast<std::ptrdiff_t>(size))};
            row = row.subspan(static_cast<std::size_t>(size));
            if (!inputs.empty() && !(inputs.back().key < input.key))
                return refused<void>(reconnect_errors::log, "replay commands are not canonical");
            const auto bound = std::find_if(seats_.begin(), seats_.end(), [&](const auto& value) { return value.seat == input.key.seat; });
            if (bound == seats_.end() || !bound->may_submit)
                return refused<void>(reconnect_errors::log, "replay input has no authorized seat");
            auto valid = adapter_->validate(input); if (!valid) return valid;
            inputs.push_back(std::move(input));
        }
        if (!row.empty()) return refused<void>(reconnect_errors::log, "trailing replay row bytes");
        auto installed = adapter_->install(inputs); if (!installed) return installed;
        auto step = adapter_->step(); if (!step) return core::Result<void>::failure(step.error());
        if (adapter_->completed_tick() != tick + 1 || step.value().completed_tick != tick + 1)
            return refused<void>(reconnect_errors::log, "catch-up did not advance one tick");
        reconnect_bytes_ += retained.size() + 4;
        reconnect_log_.push_back(std::move(retained));
        if (!resync_staging_) if (auto progress = report_replay_progress(tick + 1); !progress) return progress;
    }
    if (options_.input_delay != delay)
        return refused<void>(reconnect_errors::log, "transferred delay disagrees with locally replayed timing policy");
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        std::uint64_t frontier{}, present{}, cursor{}, departed{};
        if (!read(bytes, frontier, 8) || frontier > count + input_horizon()
            || !read(bytes, present, 1) || present > 1 || !read(bytes, cursor, 8)
            || !read(bytes, departed, 1) || departed > 1 || (!departed && frontier < count))
            return refused<void>(reconnect_errors::log, "invalid reconnect sequence frontier");
        received_through_[peer] = frontier;
        command_cursors_[peer] = present ? std::optional<std::uint64_t>(cursor) : std::nullopt;
        departed_[peer] = departed != 0;
        // Ancient batches on a replaced stream are rejected, never accepted as
        // unverified duplicates. Only retained future batches get digest entries.
        accepted_[peer].resize(static_cast<std::size_t>(frontier));
    }
    if (departed_[self_] || (!resync_staging_ && received_through_[self_] != count))
        return refused<void>(reconnect_errors::log, "rejoining seat is not held at the barrier");
    next_seal_ = resync_staging_ ? received_through_[self_] : count;
    if (command_cursors_[self_] && *command_cursors_[self_] == std::numeric_limits<std::uint64_t>::max())
        return refused<void>(reconnect_errors::log, "rejoining command sequence exhausted");
    next_sequence_ = command_cursors_[self_] ? *command_cursors_[self_] + 1 : 0;
    std::uint64_t batches{};
    if (!read(bytes, batches, 4) || batches > input_horizon() * seats_.size())
        return refused<void>(reconnect_errors::log, "too many retained future batches");
    for (std::uint64_t index = 0; index < batches; ++index) {
        if (!read(bytes, size, 4) || size > bytes.size()) return refused<void>(reconnect_errors::log, "truncated future batch");
        const auto frame = bytes.first(static_cast<std::size_t>(size)); bytes = bytes.subspan(static_cast<std::size_t>(size));
        auto decoded = decode_batch(frame); if (!decoded) return core::Result<void>::failure(decoded.error());
        auto batch = std::move(decoded).value();
        if (batch.sender >= seats_.size() || (!resync_staging_ && batch.sender == self_) || batch.tick < count
            || batch.tick >= received_through_[batch.sender] || ready_[batch.tick].contains(batch.sender))
            return refused<void>(reconnect_errors::log, "invalid retained future frontier");
        for (const auto& input : batch.commands) {
            if (input.key.seat != seats_[batch.sender].seat || !seats_[batch.sender].may_submit)
                return refused<void>(reconnect_errors::log, "future batch seat mismatch");
            auto valid = adapter_->validate(input); if (!valid) return valid;
        }
        accepted_[batch.sender][batch.tick] = core::sha256(frame);
        recovery_[batch.sender].emplace(batch.tick, batch);
        ready_[batch.tick].emplace(batch.sender, std::move(batch));
    }
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if ((resync_staging_ || peer != self_) && !departed_[peer]) for (auto tick = count; tick < received_through_[peer]; ++tick)
            if (!ready_[tick].contains(peer)) return refused<void>(reconnect_errors::log, "gap in future sealed log");
    std::uint64_t control_ticks{};
    if (!read(bytes, control_ticks, 4) || control_ticks > bytes.size() / 17)
        return refused<void>(reconnect_errors::log, "invalid future control count");
    for (std::uint64_t index = 0; index < control_ticks; ++index) {
        std::uint64_t tick{}, controls{};
        if (!read(bytes, tick, 8) || tick < count || tick >= options_.final_tick_count
            || reconnect_controls_.contains(tick) || !read(bytes, controls, 4) || controls == 0 || controls > bytes.size() / 5)
            return refused<void>(reconnect_errors::log, "invalid future control tick");
        for (std::uint64_t control = 0; control < controls; ++control) {
            if (!read(bytes, size, 4) || size == 0 || size > bytes.size() || !can_replay_control(bytes.first(static_cast<std::size_t>(size))))
                return refused<void>(reconnect_errors::log, "invalid future control");
            if (auto restored = restore_future_timing(bytes.first(static_cast<std::size_t>(size))); !restored) return restored;
            reconnect_controls_[tick].emplace_back(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
            bytes = bytes.subspan(static_cast<std::size_t>(size));
        }
        recovered_control_ticks_.push_back(tick);
    }
    if (!bytes.empty()) return refused<void>(reconnect_errors::log, "trailing reconnect package bytes");
    Bytes{}.swap(incoming_package_);
    recorded_zero_ = true;
    reconnect_epoch_ = held_->epoch;
    rejoin_restored_ = true;
    return core::Result<void>::success();
}

core::Result<void> LockstepSession::offer_reconnect(PeerIndex peer, std::uint64_t epoch) {
    if (adapter_->completed_tick() != held_->cutoff)
        return refused<void>(reconnect_errors::epoch, "rejoin is stale or outside its held barrier");
    if (monitor_ && monitor_->frozen()) return refused<void>(reconnect_errors::hash, "reconnect cannot conceal a desync freeze");
    auto package = reconnect_package();
    if (!package && package.error().code == reconnect_errors::limit) {
        disable_reconnect("transfer budget exhausted; play continues without reconnect");
        held_->unavailable = true;
        return core::Result<void>::success();
    }
    if (!package) return core::Result<void>::failure(package.error());
    held_->outgoing = std::move(package).value(); held_->offered = true;
    held_->source = self_; held_->progress_started = true; held_->progress_since_ms = last_now_.value_or(0);
    Bytes start{rejoin_barrier_kind}; append(start, epoch, 8); append(start, 0, 1); append(start, self_, 4); append(start, 0, 8);
    if (auto sent = reconnect_broadcast(start); !sent) return sent;
    Bytes offer{rejoin_offer_kind}; append(offer, epoch, 8); append(offer, held_->cutoff, 8);
    append(offer, held_->outgoing.size(), 8);
    const auto digest = core::sha256(held_->outgoing); offer.insert(offer.end(), digest.begin(), digest.end());
    return transport_->send(peer, offer);
}

core::Result<void> LockstepSession::receive_reconnect(const Received& received) {
    if (!options_.reconnect) return refused<void>(reconnect_errors::options, "reconnect was not agreed");
    if (!options_.reconnect->allow_rejoin) {
        reconnect_notice(reconnect_errors::unavailable, "stream rejoin was not agreed");
        return core::Result<void>::success();
    }
    if (options_.reconnect->seat_tokens.size() != seats_.size()) {
        reconnect_notice(reconnect_errors::unavailable, "reconnect credentials have been released");
        return core::Result<void>::success();
    }
    const auto kind = received.frame.front();
    auto& options = *options_.reconnect;
    const auto guarded_send = [this](PeerIndex peer, std::span<const std::uint8_t> frame) {
        if (auto checked = check_held_survivors(); !checked) return checked;
        return transport_->send(peer, frame);
    };
    if (kind != rejoin_hello_kind && held_ && !options.rejoin_source && received.from == held_->target) {
        bool authenticated{};
        for (const bool transfer : {false, true}) {
            const auto found = issued_challenges_.find({received.from, transfer});
            authenticated = authenticated || (found != issued_challenges_.end()
                && found->second.epoch == held_->epoch && found->second.consumed);
        }
        if (!authenticated) {
            reconnect_notice(reconnect_errors::auth, "ignored control frame on unauthenticated returning stream");
            return core::Result<void>::success();
        }
    }
    auto bytes = std::span<const std::uint8_t>(received.frame).subspan(1);
    std::uint64_t epoch{};
    if (!read(bytes, epoch, 8)) return refused<void>(reconnect_errors::malformed, "truncated reconnect epoch");
    // Expiry permanently rejects this epoch. An in-flight returning peer must
    // not be able to stop the survivors' already agreed ordinary departure.
    if (epoch != 0 && (epoch <= completed_reconnect_epoch_
        || (expired_reconnect_epoch_ && epoch <= *expired_reconnect_epoch_))) {
        reconnect_notice(reconnect_errors::stale, "ignored a completed or expired reconnect epoch");
        return core::Result<void>::success();
    }
    // Ready/resume packets can still be queued on a slower peer's stream when
    // this endpoint has resumed. They settle only the already proved epoch.
    if (!held_ && !options.rejoin_source && epoch == reconnect_epoch_ && !last_reconnect_hash_.empty()) {
        if ((kind == rejoin_ready_kind || kind == rejoin_resume_kind) && bytes.empty()) return core::Result<void>::success();
        if (kind == rejoin_proof_kind && std::string(bytes.begin(), bytes.end()) == last_reconnect_hash_)
            return core::Result<void>::success();
    }
    if (kind == rejoin_hello_kind) {
        if (transport_->link(received.from) != LinkState::open) {
            reconnect_notice(reconnect_errors::stale, "ignored handshake on closed replacement link");
            return core::Result<void>::success();
        }
        std::uint64_t phase{}, seat{};
        if (!read(bytes, phase, 1) || phase > rejoin_link_response || !read(bytes, seat, 4)
            || bytes.size() != (phase % 3 == rejoin_request ? 32U : phase % 3 == rejoin_challenge ? 64U : 96U)
            || !std::equal(bytes.begin(), bytes.begin() + 32, options.match_identity.begin()))
            return refused<void>(reconnect_errors::identity, "invalid rejoin identity or envelope");
        bytes = bytes.subspan(32);
        const bool transfer = phase < rejoin_link_request;
        const auto step = phase % 3;
        if (step == rejoin_challenge) {
            if (!options.rejoin_source || seat != seats_[self_].seat.value || epoch != options.rejoin_epoch
                || (transfer ? received.from != *options.rejoin_source || rejoin_offer_received_
                    : !rejoin_restored_ || departed_[received.from] || received.from == *options.rejoin_source)) {
                reconnect_notice(reconnect_errors::nonce, "ignored challenge outside returning seat incident");
                return core::Result<void>::success();
            }
            RejoinNonce nonce{}; std::copy(bytes.begin(), bytes.end(), nonce.begin());
            const auto previous = returning_nonces_.find(received.from);
            if (previous != returning_nonces_.end() && previous->second != nonce) {
                reconnect_notice(reconnect_errors::nonce, "ignored conflicting nonce on returning stream");
                return core::Result<void>::success();
            }
            returning_nonces_[received.from] = nonce;
            auto response = hello_frame(epoch, transfer ? rejoin_response : rejoin_link_response, seats_[self_].seat.value, options.match_identity);
            response.insert(response.end(), nonce.begin(), nonce.end());
            const auto tag = rejoin_authenticator(options.seat_tokens[received.from], options.match_identity,
                seats_[self_].seat.value, epoch, nonce, transfer);
            response.insert(response.end(), tag.begin(), tag.end());
            if (auto sent = guarded_send(received.from, response); !sent) return sent;
            // A previous proof may have been discarded while its response was lost.
            // Resend on this same ordered stream after answering the retry challenge.
            if (!transfer && held_ && held_->sent_proof) {
                Bytes proof{rejoin_proof_kind}; append(proof, held_->epoch, 8);
                const auto hash = adapter_->state_hash(); proof.insert(proof.end(), hash.begin(), hash.end());
                return guarded_send(received.from, proof);
            }
            return core::Result<void>::success();
        }
        if (options.rejoin_source || seat != seats_[received.from].seat.value)
            return refused<void>(reconnect_errors::identity, "rejoin seat or survivor role mismatch");
        const auto expected = held_ ? held_->epoch : (agreement_ ? agreement_->reconnect_epoch : reconnect_epoch_ + 1);
        if (departed_[received.from] || agreed_departures_.contains(received.from)
            || epoch != expected || (held_ && held_->target != received.from)
            || (agreement_ && (agreement_->target != received.from || agreement_->reconnect_expired))) {
            reconnect_notice(reconnect_errors::stale, "ignored a rejoin request outside its seat incident");
            return core::Result<void>::success();
        }
        if (held_ && held_->expiry_sent) {
            reconnect_notice(reconnect_errors::unavailable, "reconnect is unavailable for this incident");
            return core::Result<void>::success();
        }
        if (!transfer && (!held_ || held_->target != received.from)) {
            reconnect_notice(reconnect_errors::stale, "survivor link authentication waits for the held barrier");
            return core::Result<void>::success();
        }
        const auto challenge_key = std::pair{received.from, transfer};
        auto found = issued_challenges_.find(challenge_key);
        if (step == rejoin_request) {
            if (found != issued_challenges_.end() && found->second.epoch != epoch) {
                issued_challenges_.erase(found);
                found = issued_challenges_.end();
            }
            if (found == issued_challenges_.end()) {
                auto nonce = fresh_nonce();
                if (!nonce) return core::Result<void>::failure(nonce.error());
                found = issued_challenges_.emplace(challenge_key,
                    RejoinChallenge{epoch, nonce.value(), false, transfer}).first;
            }
            if ((found->second.consumed && transfer) || found->second.transfer != transfer) {
                reconnect_notice(reconnect_errors::nonce, "ignored consumed nonce request");
                return core::Result<void>::success();
            }
            auto challenge = hello_frame(epoch, transfer ? rejoin_challenge : rejoin_link_challenge, static_cast<std::uint32_t>(seat), options.match_identity);
            challenge.insert(challenge.end(), found->second.nonce.begin(), found->second.nonce.end());
            return guarded_send(received.from, challenge);
        }
        if (found == issued_challenges_.end() || found->second.epoch != epoch
            || found->second.consumed || found->second.transfer != transfer || !constant_equal(bytes.first(32), found->second.nonce)) {
            reconnect_notice(reconnect_errors::nonce, "ignored missing, replayed or incorrect nonce");
            return core::Result<void>::success();
        }
        const auto tag = rejoin_authenticator(options.seat_tokens[received.from], options.match_identity,
            static_cast<std::uint32_t>(seat), epoch, found->second.nonce, transfer);
        if (!constant_equal(bytes.subspan(32), tag)) {
            reconnect_notice(reconnect_errors::auth, "rejoin authentication failed");
            return core::Result<void>::success();
        }
        found->second.consumed = true; // Consume before any gameplay or transfer side effect.
        if (!held_ && !agreement_) {
            if (auto begun = begin_departure(received.from, DepartureReason::disconnect); !begun) return begun;
        }
        if (!reconnect_available_) {
            reconnect_notice(reconnect_errors::unavailable, "reconnect is unavailable; ordinary departure applies");
            return core::Result<void>::success();
        }
        if (!transfer) return core::Result<void>::success();
        if (!held_ || adapter_->completed_tick() < held_->cutoff) {
            pending_rejoin_ = received;
            return core::Result<void>::success();
        }
        return offer_reconnect(received.from, epoch);
    }
    if (kind == rejoin_offer_kind) {
        std::uint64_t tick{}, total{};
        if (!options.rejoin_source || !returning_nonces_.contains(received.from) || received.from != *options.rejoin_source || rejoin_offer_received_
            || epoch != options.rejoin_epoch || !read(bytes, tick, 8) || tick >= options_.final_tick_count
            || !read(bytes, total, 8) || total == 0 || total > options.max_log_bytes || bytes.size() != 32)
            return refused<void>(reconnect_errors::malformed, "invalid rejoin offer or source");
        held_ = HeldSeat{self_, tick, last_now_.value_or(0), epoch,
            std::vector<bool>(seats_.size()), std::vector<bool>(seats_.size()), std::vector<bool>(seats_.size()),
            true, false, false, false, false, {}, 0};
        held_->since_ms = waiting_ ? waiting_->since_ms : last_now_.value_or(0);
        held_->progress_since_ms = last_now_.value_or(0); held_->progress_started = true;
        held_->source = options.rejoin_source;
        incoming_size_ = static_cast<std::size_t>(total);
        std::copy(bytes.begin(), bytes.end(), incoming_digest_.begin());
        rejoin_offer_received_ = true;
        return core::Result<void>::success();
    }
    if (!held_ || epoch != held_->epoch || departed_[received.from])
        return refused<void>(reconnect_errors::epoch, "message is not bound to the active reconnect incident");
    if (kind == rejoin_chunk_kind) {
        std::uint64_t offset{};
        if (!options.rejoin_source || received.from != *options.rejoin_source || rejoin_restored_
            || !read(bytes, offset, 8) || offset != incoming_package_.size() || bytes.empty()
            || bytes.size() > 64U * 1024U || bytes.size() > incoming_size_ - incoming_package_.size())
            return refused<void>(reconnect_errors::log, "reconnect chunk is not contiguous or exceeds bounds");
        incoming_package_.insert(incoming_package_.end(), bytes.begin(), bytes.end());
        held_->progress_since_ms = last_now_.value_or(0);
        Bytes progress{rejoin_barrier_kind}; append(progress, epoch, 8); append(progress, 1, 1);
        append(progress, *options.rejoin_source, 4); append(progress, incoming_package_.size(), 8);
        return guarded_send(*options.rejoin_source, progress);
    }
    if (kind == rejoin_barrier_kind) {
        if (bytes.size() == 13) {
            std::uint64_t phase{}, source{}, value{};
            if (options.rejoin_source || !read(bytes, phase, 1) || phase > 2 || !read(bytes, source, 4)
                || source >= seats_.size() || source == held_->target || !read(bytes, value, 8)
                || (phase == 0 ? received.from != source || value != 0
                    : received.from != held_->target && received.from != source)
                || (held_->source && *held_->source != source))
                return refused<void>(reconnect_errors::malformed, "invalid reconnect progress source");
            if (held_->expiry_sent) return core::Result<void>::success();
            if (phase != 0 && !held_->source) {
                reconnect_notice(reconnect_errors::auth, "ignored progress before survivor authentication announcement");
                return core::Result<void>::success();
            }
            held_->source = static_cast<PeerIndex>(source);
            bool progressed = !held_->progress_started;
            if (phase == 1) {
                if (value > options.max_log_bytes || (source == self_ && value > held_->sent_bytes))
                    return refused<void>(reconnect_errors::log, "progress exceeds transmitted prefix");
                progressed = progressed || value > held_->progress_bytes;
                held_->progress_bytes = std::max(held_->progress_bytes, value);
            } else if (phase == 2) {
                if (value > held_->cutoff) return refused<void>(reconnect_errors::log, "progress exceeds replay barrier");
                progressed = progressed || !held_->replay_started || value > held_->progress_tick;
                held_->replay_started = true; held_->progress_tick = std::max(held_->progress_tick, value);
            }
            held_->progress_started = true;
            if (progressed) held_->progress_since_ms = last_now_.value_or(0);
            // The source relays verified progress to the old surviving roster.
            // The fresh peer has not installed that membership until replay ends.
            if (source == self_ && phase != 0 && progressed) return reconnect_broadcast(received.frame);
            return core::Result<void>::success();
        }
        if (replay_in_progress_) return core::Result<void>::success();
        if (!options.rejoin_source || received.from != *options.rejoin_source || rejoin_restored_
            || incoming_package_.size() != incoming_size_ || bytes.size() != 64)
            return refused<void>(reconnect_errors::log, "incomplete reconnect prefix at barrier");
        const std::string hash(bytes.begin(), bytes.end());
        held_->progress_since_ms = last_now_.value_or(0);
        replay_in_progress_ = true;
        auto restored = install_reconnect_package();
        replay_in_progress_ = false;
        if (!restored) return restored;
        if (adapter_->state_hash() != hash) return refused<void>(reconnect_errors::hash, "headless catch-up hash differs");
        return core::Result<void>::success();
    }
    if (kind == rejoin_expire_kind) {
        if (options.rejoin_source || received.from == held_->target || bytes.size() != 65
            || adapter_->completed_tick() > held_->cutoff)
            return refused<void>(reconnect_errors::malformed, "invalid expiry voter");
        const auto unavailable = bytes.back();
        if (unavailable > 1) return refused<void>(reconnect_errors::malformed, "invalid expiry policy flag");
        if (unavailable) { disable_reconnect("peer history budget exhausted; play continues without reconnect"); held_->unavailable = true; }
        const std::string hash(bytes.begin(), bytes.begin() + 64);
        if (adapter_->completed_tick() < held_->cutoff) {
            if (const auto found = pending_expiry_proofs_.find(received.from);
                found != pending_expiry_proofs_.end() && found->second != hash)
                return refused<void>(reconnect_errors::hash, "voter changed its expiry proof");
            pending_expiry_proofs_[received.from] = hash;
            return core::Result<void>::success();
        }
        if (hash != adapter_->state_hash()) return refused<void>(reconnect_errors::hash, "expiry barrier hashes disagree");
        held_->expired[received.from] = true;
        return core::Result<void>::success();
    }
    if (kind == rejoin_proof_kind) {
        if (bytes.size() != 64 || adapter_->completed_tick() > held_->cutoff
            || (options.rejoin_source && !rejoin_restored_))
            return refused<void>(reconnect_errors::malformed, "invalid rejoin proof");
        if (adapter_->completed_tick() < held_->cutoff) {
            const std::string hash(bytes.begin(), bytes.end());
            if (const auto found = pending_rejoin_proofs_.find(received.from);
                found != pending_rejoin_proofs_.end() && found->second != hash)
                return refused<void>(reconnect_errors::hash, "peer changed its queued rejoin proof");
            pending_rejoin_proofs_[received.from] = hash;
            return core::Result<void>::success();
        }
        if (std::string(bytes.begin(), bytes.end()) != adapter_->state_hash())
            return refused<void>(reconnect_errors::hash, "rejoin barrier hashes disagree");
        held_->proofs[received.from] = true;
        held_->progress_since_ms = last_now_.value_or(0);
        if (!options.rejoin_source && received.from == held_->target && held_->sent_proof) {
            Bytes proof{rejoin_proof_kind}; append(proof, held_->epoch, 8);
            const auto hash = adapter_->state_hash(); proof.insert(proof.end(), hash.begin(), hash.end());
            return guarded_send(received.from, proof);
        }
        return core::Result<void>::success();
    }
    if ((kind == rejoin_ready_kind || kind == rejoin_resume_kind) && bytes.empty()) {
        if (kind == rejoin_ready_kind) held_->ready[received.from] = true;
        else {
            if (!held_->sent_ready || !held_->ready[received.from])
                return refused<void>(reconnect_errors::epoch, "resume arrived before proof/ready agreement");
            held_->resuming = true;
        }
        return core::Result<void>::success();
    }
    return refused<void>(reconnect_errors::malformed, "unknown reconnect envelope");
}

core::Result<void> LockstepSession::finish_reconnect() {
    if (monitor_) {
        std::vector<PeerIndex> inactive;
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer) if (departed_[peer]) inactive.push_back(peer);
        auto reset = monitor_->on_reconnect(held_->cutoff, adapter_->state_hash(), inactive);
        if (!reset) return reset;
    }
    last_reconnect_hash_ = adapter_->state_hash();
    completed_reconnect_epoch_ = std::max(completed_reconnect_epoch_, held_->epoch);
    pending_rejoin_proofs_.clear();
    pending_expiry_proofs_.clear();
    held_.reset(); options_.reconnect->rejoin_source.reset();
    waiting_.reset(); monitor_wait_.reset();
    if (!reconnect_available_) disable_reconnect("history budget exhausted; play continues without reconnect");
    return core::Result<void>::success();
}

core::Result<bool> LockstepSession::service_reconnect() {
    auto& options = *options_.reconnect;
    const auto now = last_now_.value_or(0);
    if (options.rejoin_source && !rejoin_offer_received_ && !waiting_)
        waiting_ = WaitingFor{0, {*options.rejoin_source}, now};
    if (options.rejoin_source && !rejoin_offer_received_ && hello_attempts_ < options.max_hello_attempts
        && transport_->link(*options.rejoin_source) == LinkState::open
        && (!rejoin_hello_sent_ || now - last_hello_ms_ >= options.hello_retry_ms)) {
        auto hello = hello_frame(options.rejoin_epoch, rejoin_request, seats_[self_].seat.value, options.match_identity);
        auto sent = transport_->send(*options.rejoin_source, hello);
        if (!sent) return core::Result<bool>::failure(sent.error());
        rejoin_hello_sent_ = true; last_hello_ms_ = now; ++hello_attempts_;
    }
    if (!held_) {
        if (waiting_ && now - waiting_->since_ms >= options.grace_ms)
            return refused<bool>(reconnect_errors::timeout, "rejoin offer deadline expired");
        return core::Result<bool>::success(true);
    }
    auto& state = *held_;
    if (adapter_->completed_tick() < state.cutoff && !options.rejoin_source) return core::Result<bool>::success(false);
    if (adapter_->completed_tick() > state.cutoff)
        return refused<bool>(reconnect_errors::log, "live peer advanced past reconnect barrier");
    for (auto entry = pending_rejoin_proofs_.begin(); entry != pending_rejoin_proofs_.end();) {
        const auto& [peer, hash] = *entry;
        if (hash != adapter_->state_hash()) return refused<bool>(reconnect_errors::hash, "queued rejoin proof differs at barrier");
        state.proofs[peer] = true;
        entry = pending_rejoin_proofs_.erase(entry);
    }
    for (const auto& [peer, hash] : pending_expiry_proofs_) {
        if (hash != adapter_->state_hash()) return refused<bool>(reconnect_errors::hash, "queued expiry proof differs at barrier");
        state.expired[peer] = true;
    }
    pending_expiry_proofs_.clear();
    if (pending_rejoin_) {
        auto hello = std::move(*pending_rejoin_); pending_rejoin_.reset();
        if (hello.from != state.target || !reconnect_available_)
            reconnect_notice(reconnect_errors::stale, "discarded a handshake not eligible for this held seat");
        else {
            auto offered = offer_reconnect(hello.from, state.epoch); if (!offered) return core::Result<bool>::failure(offered.error());
        }
    }
    const auto idle_limit = state.progress_started ? options.progress_timeout_ms : options.grace_ms;
    const auto idle_since = state.progress_started ? state.progress_since_ms : state.since_ms;
    const bool deadline = now - state.since_ms >= options.hard_cap_ms || now - idle_since >= idle_limit;
    if (options.rejoin_source && !rejoin_restored_) {
        if (deadline) return refused<bool>(reconnect_errors::timeout, "rejoin progress or hard-cap deadline expired");
        return core::Result<bool>::success(true);
    }
    if (options.rejoin_source && !state.sent_ready && now - state.progress_since_ms >= options.progress_timeout_ms)
        return refused<bool>(reconnect_errors::timeout, "rejoin proof deadline expired");
    // After READY this survivor no longer originates expiry on its deadline.
    // It still honors an uncommitted survivor's expiry below. An expiring voter
    // never sends READY, so all-peer resume cannot race the agreed departure.
    if (!options.rejoin_source && (deadline || !reconnect_available_) && !state.sent_ready) {
        if (!state.expiry_sent) {
            Bytes expired{rejoin_expire_kind}; append(expired, state.epoch, 8);
            const auto hash = adapter_->state_hash(); expired.insert(expired.end(), hash.begin(), hash.end());
            append(expired, !reconnect_available_, 1);
            auto sent = reconnect_broadcast(expired); if (!sent) return core::Result<bool>::failure(sent.error());
            state.expiry_sent = true; state.expired[self_] = true;
        }
    }
    // Any survivor may expire the incident. All survivors acknowledge that same
    // epoch before the existing departure vote is allowed to mutate the roster.
    const bool expiring = std::any_of(state.expired.begin(), state.expired.end(), [](bool value) { return value; });
    if (expiring && !options.rejoin_source) {
        if (!state.expiry_sent) {
            Bytes expired{rejoin_expire_kind}; append(expired, state.epoch, 8);
            const auto hash = adapter_->state_hash(); expired.insert(expired.end(), hash.begin(), hash.end());
            append(expired, state.unavailable || !reconnect_available_, 1);
            auto sent = reconnect_broadcast(expired); if (!sent) return core::Result<bool>::failure(sent.error());
            state.expiry_sent = true; state.expired[self_] = true;
        }
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
            if (peer != state.target && !departed_[peer] && !state.expired[peer]) return core::Result<bool>::success(true);
        const auto target = state.target;
        const auto reason = state.reason;
        expired_reconnect_epoch_ = state.epoch;
        completed_reconnect_epoch_ = std::max(completed_reconnect_epoch_, state.epoch);
        if (monitor_) {
            // Every survivor proved the same recovered prefix. The absent target
            // leaves the monitor roster here; the subsequent ordinary departure
            // remains replayable and records its next tick contiguously.
            std::vector<PeerIndex> inactive{target};
            for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
                if (departed_[peer]) inactive.push_back(peer);
            auto reset = monitor_->on_reconnect(state.cutoff, adapter_->state_hash(), inactive);
            if (!reset) return core::Result<bool>::failure(reset.error());
        }
        held_.reset();
        pending_rejoin_.reset(); pending_rejoin_proofs_.clear(); pending_expiry_proofs_.clear();
        if (!reconnect_available_) disable_reconnect("history budget exhausted; play continues without reconnect");
        auto begun = begin_departure(target, reason, true);
        if (!begun) return core::Result<bool>::failure(begun.error());
        agreement_->reconnect_expired = true;
        return core::Result<bool>::success(true);
    }
    if (!state.outgoing.empty()) {
        // Bounded per-pump transfer; MessageTransport retains framing/backpressure.
        if (state.sent_bytes < state.outgoing.size()) {
            const auto length = std::min<std::size_t>(64U * 1024U, state.outgoing.size() - state.sent_bytes);
            Bytes chunk{rejoin_chunk_kind}; append(chunk, state.epoch, 8); append(chunk, state.sent_bytes, 8);
            chunk.insert(chunk.end(), state.outgoing.begin() + static_cast<std::ptrdiff_t>(state.sent_bytes),
                state.outgoing.begin() + static_cast<std::ptrdiff_t>(state.sent_bytes + length));
            auto sent = transport_->send(state.target, chunk); if (!sent) return core::Result<bool>::failure(sent.error());
            state.sent_bytes += length;
            return core::Result<bool>::success(true);
        }
        Bytes barrier{rejoin_barrier_kind}; append(barrier, state.epoch, 8);
        const auto hash = adapter_->state_hash(); barrier.insert(barrier.end(), hash.begin(), hash.end());
        auto sent = transport_->send(state.target, barrier); if (!sent) return core::Result<bool>::failure(sent.error());
        Bytes{}.swap(state.outgoing);
    }
    bool links_authenticated = true;
    if (options.rejoin_source && rejoin_restored_) {
        const bool retry = link_hello_attempts_ < options.max_hello_attempts
            && (link_hello_attempts_ == 0 || now - link_hello_ms_ >= options.hello_retry_ms);
        bool sent_request{};
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
            if (peer == self_ || departed_[peer] || peer == *options.rejoin_source) continue;
            links_authenticated = links_authenticated && returning_nonces_.contains(peer);
            if (state.proofs[peer]) continue; // This is the authenticated acknowledgement.
            if (retry && transport_->link(peer) == LinkState::open) {
                auto request = hello_frame(state.epoch, rejoin_link_request, seats_[self_].seat.value, options.match_identity);
                auto sent = transport_->send(peer, request);
                if (!sent) return core::Result<bool>::failure(sent.error());
                sent_request = true;
            }
        }
        if (sent_request) { link_hello_ms_ = now; ++link_hello_attempts_; }
    }
    // The returning seat sends proof first. This tells other survivors the
    // rebuilt endpoint is ready, so no gameplay/hash frame races its log transfer.
    if (!state.sent_proof && ((options.rejoin_source && links_authenticated) || (!options.rejoin_source && state.proofs[state.target]))) {
        Bytes proof{rejoin_proof_kind}; append(proof, state.epoch, 8);
        const auto hash = adapter_->state_hash(); proof.insert(proof.end(), hash.begin(), hash.end());
        auto sent = reconnect_broadcast(proof); if (!sent) return core::Result<bool>::failure(sent.error());
        state.sent_proof = true; state.proofs[self_] = true;
        if (!options.rejoin_source) for (const auto& [tick, batches] : ready_) {
            static_cast<void>(tick);
            if (const auto own = batches.find(self_); own != batches.end()) {
                auto encoded = encode_batch(own->second); if (!encoded) return core::Result<bool>::failure(encoded.error());
                auto resent = transport_->send(state.target, encoded.value());
                if (resent) last_sent_batch_[state.target] = tick;
                if (!resent) return core::Result<bool>::failure(resent.error());
            }
        }
    }
    bool proved = true, ready = true;
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) if (!departed_[peer]) {
        proved = proved && state.proofs[peer]; ready = ready && state.ready[peer];
    }
    if (proved && !state.sent_ready) {
        Bytes frame{rejoin_ready_kind}; append(frame, state.epoch, 8);
        auto sent = reconnect_broadcast(frame); if (!sent) return core::Result<bool>::failure(sent.error());
        state.sent_ready = true; state.ready[self_] = true;
    }
    if (ready && !state.resuming) {
        Bytes frame{rejoin_resume_kind}; append(frame, state.epoch, 8);
        auto sent = reconnect_broadcast(frame); if (!sent) return core::Result<bool>::failure(sent.error());
        state.resuming = true;
    }
    if (state.resuming && ready) {
        auto finished = finish_reconnect(); if (!finished) return core::Result<bool>::failure(finished.error());
        return core::Result<bool>::success(false);
    }
    if (state.sent_ready && now - idle_since >= idle_limit
        && now - idle_since - idle_limit >= options_.completeness_timeout_ms)
        return refused<bool>(reconnect_errors::timeout, "committed rejoin lost a proof/ready voter");
    return core::Result<bool>::success(true);
}

} // namespace eawr::net
