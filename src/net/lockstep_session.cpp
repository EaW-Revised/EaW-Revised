#ifdef _WIN32
#ifndef _CRT_RAND_S
#define _CRT_RAND_S
#endif
#endif
#include "eawr/net/lockstep_session.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <cstdlib>
#ifndef _WIN32
#include <cerrno>
#include <sys/random.h>
#endif

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::array<std::string_view, 8> resync_codes{resync_errors::options, resync_errors::malformed,
    resync_errors::agreement, resync_errors::replay, resync_errors::hash, resync_errors::timeout,
    resync_errors::overlap, resync_errors::unavailable};
core::Diagnostic diagnostic(std::string_view code, std::string message) {
    core::Diagnostic result;
    result.code = code;
    result.message = std::move(message);
    return result;
}
template<class T> core::Result<T> refused(std::string_view code, std::string message) {
    return core::Result<T>::failure(diagnostic(code, std::move(message)));
}
void append(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
}
bool read(std::span<const std::uint8_t>& bytes, std::uint64_t& value, unsigned width) {
    if (bytes.size() < width) return false;
    value = 0;
    for (unsigned index = 0; index < width; ++index)
        value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    bytes = bytes.subspan(width);
    return true;
}
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
}

std::uint64_t adaptive_input_delay(std::uint64_t worst_rtt_ms, bool broadband) noexcept {
    // MP-05 at the fixed logical 30 Hz rate. Beyond this bound the lead is capped;
    // bounding before multiplication also handles arbitrary transport counters.
    const auto ping = std::min<std::uint64_t>(worst_rtt_ms, 2640);
    const auto latency = std::max<std::uint64_t>(2 * ((120 * ping / 100) / (1000 / 30)) / 3,
        broadband ? 4U : 6U);
    return 2 * std::min<std::uint64_t>(latency / 2, 32);
}

core::Result<RejoinNonce> make_rejoin_nonce() { return fresh_nonce(); }

std::array<std::uint8_t, 32> rejoin_authenticator(const std::array<std::uint8_t, 32>& token,
    const std::array<std::uint8_t, 32>& identity, std::uint32_t seat, std::uint64_t epoch, const RejoinNonce& nonce, bool transfer) {
    // RFC 2104: SHA256((K xor opad) || SHA256((K xor ipad) || text)).
    Bytes inner(64, 0x36), outer(64, 0x5c);
    inner.reserve(192); outer.reserve(96); // Allocate before adding key material.
    for (std::size_t index = 0; index < token.size(); ++index) {
        inner[index] ^= token[index]; outer[index] ^= token[index];
    }
    const std::string_view domain = transfer ? "EAWR-rejoin-v1" : "EAWR-rejoin-link-v1";
    inner.insert(inner.end(), domain.begin(), domain.end());
    inner.insert(inner.end(), identity.begin(), identity.end());
    append(inner, seat, 4); append(inner, epoch, 8);
    inner.insert(inner.end(), nonce.begin(), nonce.end());
    auto digest = core::sha256(inner);
    outer.insert(outer.end(), digest.begin(), digest.end());
    const auto result = core::sha256(outer);
    erase_reconnect_secret(inner); erase_reconnect_secret(outer); erase_reconnect_secret(digest);
    return result;
}

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

LockstepSession::LockstepSession(SessionAdapter& adapter, MessageTransport& transport,
    std::vector<PeerSeat> seats, SessionOptions options, SessionMonitor* monitor)
    : adapter_(&adapter), transport_(&transport), seats_(std::move(seats)), options_(options), self_(transport.self()),
      received_through_(seats_.size()), command_cursors_(seats_.size()), accepted_(seats_.size()), monitor_(monitor), departed_(seats_.size()), quit_in_batch_(seats_.size()), quit_seen_(seats_.size()), recovery_(seats_.size()),
      next_policy_tick_(64 * std::max<std::uint64_t>(1, options.input_delay / 2)), initial_input_delay_(options.input_delay) {
    last_sent_batch_.resize(seats_.size());
    if (options_.mesh)
        mesh_ = std::make_unique<MeshIncident>(transport, static_cast<MeshSeats>((1U << seats_.size()) - 1U), *options_.mesh);
}

core::Result<LockstepSession> LockstepSession::create(SessionAdapter& adapter, MessageTransport& transport,
    std::vector<PeerSeat> seats, SessionOptions options, SessionMonitor* monitor) {
    const auto count = transport.peer_count();
    if (count < 2 || count > max_match_seats || transport.self() >= count || seats.size() != count || adapter.completed_tick() != 0
        || options.input_delay == 0 || options.input_delay > max_input_delay || options.final_tick_count > max_session_ticks
        || options.completeness_timeout_ms == 0 || options.max_frames_per_pump == 0)
        return refused<LockstepSession>(session_errors::options, "invalid roster, initial tick or session options");
    if (options.adaptive_timing && (!options.non_lan || options.adaptive_timing->host >= count || options.input_delay > 64))
        return refused<LockstepSession>(timing_errors::options, "invalid adaptive host or initial delay");
    std::set<sim::SeatId> unique;
    for (const auto& seat : seats)
        if (seat.seat.value == 0 || !unique.insert(seat.seat).second)
            return refused<LockstepSession>(session_errors::options, "unbound or repeated seat");
    if (options.mesh) {
        const auto& mesh = *options.mesh;
        if (mesh.host != 0 || count < 2 || mesh.repair_grace.count() < 0 || mesh.repair_grace > std::chrono::minutes(10)
            || mesh.report_interval.count() <= 0 || mesh.report_interval > std::chrono::seconds(10) || mesh.report_freshness.count() <= 0
            || (options.adaptive_timing && options.adaptive_timing->host != mesh.host))
            return refused<LockstepSession>(mesh_errors::options, "invalid room mesh roster or incident options");
    }
    if (options.reconnect) {
        const auto& reconnect = *options.reconnect;
        const auto nonzero = [](const auto& bytes) { return std::any_of(bytes.begin(), bytes.end(), [](auto byte) { return byte != 0; }); };
        if (!nonzero(reconnect.match_identity) || (reconnect.allow_rejoin && !valid_reconnect_tokens(reconnect.seat_tokens, transport.self(), count))
            || (!reconnect.allow_rejoin && (reconnect.rejoin_source || !reconnect.seat_tokens.empty()))
            || reconnect.grace_ms == 0 || reconnect.grace_ms > max_recovery_timeout_ms
            || reconnect.progress_timeout_ms == 0 || reconnect.progress_timeout_ms > max_recovery_timeout_ms
            || reconnect.hard_cap_ms > max_recovery_timeout_ms
            || reconnect.hard_cap_ms < reconnect.grace_ms || reconnect.hard_cap_ms < reconnect.progress_timeout_ms
            || reconnect.hello_retry_ms == 0 || reconnect.hello_retry_ms > max_recovery_timeout_ms
            || reconnect.max_hello_attempts == 0 || reconnect.max_hello_attempts > 64
            || reconnect.max_log_bytes < reconnect.setup.size() + 1024
            || reconnect.max_log_bytes > 256U * 1024U * 1024U || reconnect.setup.empty()
            || reconnect.setup.size() > max_batch_bytes
            || (reconnect.rejoin_source && (*reconnect.rejoin_source >= count
                || *reconnect.rejoin_source == transport.self() || reconnect.rejoin_epoch == 0)))
            return refused<LockstepSession>(reconnect_errors::options, "invalid agreed reconnect envelope");
    }
    if (options.resync && (!options.reconnect || !monitor || !options.resync->rebuild
        || options.resync->timeout_ms == 0 || options.resync->timeout_ms > max_recovery_timeout_ms
        || (options.reconnect->replay_control && !options.resync->replay_control)))
        return refused<LockstepSession>(resync_errors::options, "resync requires a monitor, retained setup/log and staging factory/control replay");
    if (monitor && options.mesh) {
        if (auto enabled = monitor->on_link_repair(transport.self(), [count](PeerIndex peer) {
            return count >= 3 && peer < count;
        }); !enabled)
            return core::Result<LockstepSession>::failure(enabled.error());
    }
    return core::Result<LockstepSession>::success(LockstepSession(adapter, transport, std::move(seats), options, monitor));
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

bool LockstepSession::timing_frozen() const noexcept {
    if (!options_.adaptive_timing) return false;
    const auto host = options_.adaptive_timing->host;
    const auto link = host == self_ ? LinkState::open : transport_->link(host);
    return mesh_timing_frozen(mesh_voters(), link) || (mesh_ && mesh_->running());
}

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
    // MP-34/36 permit timeout processing; an unconfirmed loss cannot invent a winner.
    if (!sealed_quit && !state.pair_loss && (reason == DepartureReason::timeout || !received_survivor_vote))
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

core::Result<void> LockstepSession::resync_broadcast(std::span<const std::uint8_t> frame) {
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer) {
        if (peer == self_ || departed_[peer]) continue;
        if (auto sent = transport_->send(peer, frame); !sent) return sent;
    }
    return core::Result<void>::success();
}

std::string LockstepSession::resync_log_hash() const {
    Bytes bytes;
    const std::string_view domain = "eawr-resync-input-v1";
    bytes.insert(bytes.end(), domain.begin(), domain.end());
    const auto& options = *options_.reconnect;
    bytes.insert(bytes.end(), options.match_identity.begin(), options.match_identity.end());
    append(bytes, options.setup.size(), 4);
    bytes.insert(bytes.end(), options.setup.begin(), options.setup.end());
    append(bytes, reconnect_log_.size(), 8);
    for (const auto& row : reconnect_log_) {
        append(bytes, row.size(), 4); bytes.insert(bytes.end(), row.begin(), row.end());
    }
    return core::sha256_hex(bytes);
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

core::Diagnostic LockstepSession::connection_loss(PeerIndex peer, std::string_view cause) {
    connection_lost_peer_ = peer;
    return diagnostic(session_errors::stalled, "Connection lost: the match could not continue. "
        + std::string(cause) + " (peer " + std::to_string(peer) + ").");
}

core::Result<PumpResult> LockstepSession::fail(core::Diagnostic error) {
    const auto code = std::find(resync_codes.begin(), resync_codes.end(), error.code);
    if (options_.resync && (resync_ || refused_resync_epoch_ || code != resync_codes.end())) {
        Bytes abort{resync_abort_kind};
        append(abort, resync_ ? resync_->epoch : refused_resync_epoch_.value_or(std::max(resync_epoch_, reconnect_epoch_) + 1), 8);
        append(abort, code == resync_codes.end() ? 3 : static_cast<std::size_t>(code - resync_codes.begin()), 1);
        // Best effort: the local failure remains authoritative if a link died.
        static_cast<void>(resync_broadcast(abort));
    }
    failure_ = std::move(error);
    return core::Result<PumpResult>::failure(*failure_);
}

core::Result<PumpResult> LockstepSession::pump(const std::uint64_t now_ms) {
    if (failure_) return core::Result<PumpResult>::failure(*failure_);
    if (encode_departure_ && departed_[self_]) return core::Result<PumpResult>::success({});
    if (last_now_ && now_ms < *last_now_) return fail(diagnostic(session_errors::clock, "caller clock went backwards"));
    last_now_ = now_ms;
    if (!recorded_zero_ && !(options_.reconnect && options_.reconnect->rejoin_source)) {
        if (monitor_) {
            const bool end = options_.final_tick_count == 0;
            const auto encoding = monitor_->needs_encoding(0, end) ? adapter_->canonical_state() : Bytes{};
            auto recorded = monitor_->record(0, adapter_->state_hash(), encoding, {}, end);
            if (!recorded) return fail(recorded.error());
        }
        recorded_zero_ = true;
    }
    // Receive even when an exchange/freeze blocks advancement, and after the final tick.
    bool receive_drained = false;
    for (std::size_t count = 0; count < options_.max_frames_per_pump; ++count) {
        auto frame = transport_->poll();
        if (!frame) { receive_drained = true; break; }
        auto accepted = receive(std::move(*frame));
        if (!accepted) return fail(accepted.error());
    }
    if (auto meshed = service_mesh(now_ms, receive_drained); !meshed) return fail(meshed.error());
    const auto tick = adapter_->completed_tick();
    // Diagnose another survivor loss before reconnect service can send to its
    // closed stream or alter the held epoch. The simulation prefix stays frozen.
    if (receive_drained)
        if (auto checked = check_held_survivors(); !checked) return fail(checked.error());
    if (options_.resync && (resync_ || (monitor_ && monitor_->frozen()))) {
        auto serviced = service_resync();
        if (!serviced) return fail(serviced.error());
        if (serviced.value()) return core::Result<PumpResult>::success({});
    }
    if (options_.reconnect && (held_ || options_.reconnect->rejoin_source)) {
        auto serviced = service_reconnect();
        if (!serviced) return fail(serviced.error());
        if (serviced.value()) return core::Result<PumpResult>::success({});
    }
    // A hash gate still requires its voters even if their current input batch
    // arrived. Diagnose a closed default-path voter only after queued frames drain.
    if ((!encode_departure_ || tick == options_.final_tick_count) && monitor_ && !monitor_->may_step() && receive_drained)
        for (const auto peer : monitor_->waiting_for_peers()) {
            if (peer >= seats_.size() || peer == self_)
                return fail(diagnostic(session_errors::options, "monitor supplied an invalid waiting voter"));
            if (!departed_[peer] && transport_->link(peer) == LinkState::closed)
                return fail(diagnostic(session_errors::link_closed, "hash gate requires a closed peer stream"));
        }
    if (tick == options_.final_tick_count) {
        // Replay admission has ended: a silent final hash voter yields a bounded
        // error in either mode rather than inventing a command beyond the replay.
        if (monitor_ && !monitor_->may_step() && !monitor_->frozen()) {
            auto needed = monitor_->waiting_for_peers();
            for (const auto peer : needed)
                if (peer >= seats_.size() || peer == self_)
                    return fail(diagnostic(session_errors::options, "monitor supplied an invalid waiting voter"));
            std::erase_if(needed, [&](PeerIndex peer) { return departed_[peer]; });
            if (needed.empty()) monitor_wait_.reset();
            else {
                if (!monitor_wait_ || monitor_wait_->tick != tick) monitor_wait_ = WaitingFor{tick, {}, now_ms};
                monitor_wait_->peers = std::move(needed);
                if (now_ms - monitor_wait_->since_ms >= options_.completeness_timeout_ms)
                    return fail(connection_loss(monitor_wait_->peers.front(), "Final hash voter timed out"));
            }
        } else monitor_wait_.reset();
        return core::Result<PumpResult>::success({});
    }
    // The opt-in policy also resolves a closed peer while a hash gate is waiting
    // on it. Drain delivered batches first, then recover/ack the common frontier;
    // legacy sessions still diagnose closure only for a missing tick batch.
    if (auto serviced = service_departures(receive_drained); !serviced) return fail(serviced.error());
    if (agreement_) return core::Result<PumpResult>::success({});
    if (held_ && tick >= held_->cutoff) return core::Result<PumpResult>::success({});
    if (!held_ && std::find(recovered_control_ticks_.begin(), recovered_control_ticks_.end(), tick) != recovered_control_ticks_.end()) {
        for (const auto& control : reconnect_controls_.at(tick)) {
            auto applied = apply_reconnect_control(control);
            if (!applied) return fail(applied.error());
        }
        std::erase(recovered_control_ticks_, tick);
    }
    if (encode_departure_ && monitor_ && !held_ && !monitor_->may_step() && !monitor_->frozen()) {
        auto needed = monitor_->waiting_for_peers();
        for (const auto peer : needed)
            if (peer >= seats_.size() || peer == self_)
                return fail(diagnostic(departure_errors::malformed, "monitor supplied an invalid waiting voter"));
        std::erase_if(needed, [&](PeerIndex peer) { return departed_[peer] || agreed_departures_.contains(peer); });
        std::sort(needed.begin(), needed.end());
        needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
        if (needed.empty()) monitor_wait_.reset();
        else {
            if (!monitor_wait_ || monitor_wait_->tick != tick) monitor_wait_ = WaitingFor{tick, {}, now_ms};
            monitor_wait_->peers = std::move(needed);
            if (now_ms - monitor_wait_->since_ms >= options_.completeness_timeout_ms && mesh_ && mesh_->running())
                monitor_wait_->since_ms = now_ms; // a pair-loss incident has its own deadlines
            else if (now_ms - monitor_wait_->since_ms >= options_.completeness_timeout_ms) {
                return fail(connection_loss(monitor_wait_->peers.front(), "Required hash voter timed out"));
            }
        }
    } else monitor_wait_.reset();
    if (auto timing = advance_timing(); !timing) return fail(timing.error());
    if (auto serviced = service_departures(receive_drained); !serviced) return fail(serviced.error());
    if (agreement_ || (held_ && tick >= held_->cutoff)) return core::Result<PumpResult>::success({});
    if (timing_ && !timing_->retained) {
        const bool closed = encode_departure_ && receive_drained && [&] {
            for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
                if (peer != self_ && timing_voter(peer) && transport_->link(peer) == LinkState::closed) return true;
            return false;
        }();
        if (closed || !deferred_departures_.empty()) return core::Result<PumpResult>::success({});
    }
    if (timing_ && tick >= timing_->event.tick) return core::Result<PumpResult>::success({});
    if (auto sealed = seal(); !sealed) return fail(sealed.error());
    // A local input decoder can reserve the current tick. Its unsealed batch
    // is not a missing remote voter; wait for the producer handoff before
    // starting peer completeness/departure clocks or accessing ready_.
    if (next_seal_ <= tick) {
        waiting_.reset();
        return core::Result<PumpResult>::success({});
    }
    std::vector<PeerIndex> missing;
    const auto& batches = ready_.at(tick);
    for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
        if (!departed_[peer] && !(agreed_departures_.contains(peer) && tick >= agreed_departures_.at(peer).key.tick) && !batches.contains(peer)) missing.push_back(peer);
    if (!missing.empty()) {
        for (const auto peer : missing) {
            // At the poll budget there may still be a delivered batch behind duplicates.
            // A closed stream is conclusive only after the currently available queue drains.
            if (receive_drained && transport_->link(peer) == LinkState::closed) {
                waiting_ = WaitingFor{tick, missing, now_ms};
                if (!encode_departure_) return fail(diagnostic(session_errors::link_closed, "missing tick batch from a closed peer stream"));
                // The lost pair may be only ours: the mesh incident decides who leaves, never this seat alone.
                if (mesh_ && mesh_->owns_closed_link(peer)) return core::Result<PumpResult>::success({});
                auto begun = begin_departure(peer, options_.closed_link_reason ? options_.closed_link_reason(peer) : DepartureReason::disconnect);
                if (!begun) return fail(begun.error());
                return core::Result<PumpResult>::success({});
            }
        }
        // MP-33: a monitor exchange/freeze is a separate admission gate. Start a fresh
        // completeness clock on resume so its blocked time never reaches the overlay.
        if (monitor_ && !monitor_->may_step()) {
            waiting_.reset();
            return core::Result<PumpResult>::success({});
        }
        if (!waiting_ || waiting_->tick != tick) waiting_ = WaitingFor{tick, {}, now_ms};
        waiting_->peers = std::move(missing);
        if ((!monitor_ || monitor_->may_step()) && now_ms - waiting_->since_ms >= options_.completeness_timeout_ms && mesh_ && mesh_->running())
            waiting_->since_ms = now_ms; // a pair-loss incident has its own deadlines
        else if ((!monitor_ || monitor_->may_step()) && now_ms - waiting_->since_ms >= options_.completeness_timeout_ms) {
            return fail(connection_loss(waiting_->peers.front(), "Required input batch timed out"));
        }
        return core::Result<PumpResult>::success({});
    }
    waiting_.reset();
    if (monitor_ && !monitor_->may_step() && !held_) return core::Result<PumpResult>::success({});
    // Repaired streams can fill the old gap before the decision arrives. Only
    // the shared confirmation permits continuation; ordinary convergence and
    // confirmed departures above still run while this gate holds the sim.
    if (mesh_ && mesh_->running() && mesh_->state() != MeshLossState::settling)
        return core::Result<PumpResult>::success({});
    std::vector<SeatInput> canonical;
    for (const auto& [peer, batch] : batches) {
        if (departed_[peer]) continue;
        canonical.insert(canonical.end(), batch.commands.begin(), batch.commands.end());
    }
    for (const auto& [peer, departure] : agreed_departures_) {
        static_cast<void>(peer);
        if (departed_[peer] || quit_in_batch_[peer] || departure.key.tick != tick) continue;
        auto bytes = encode_departure_(departure.key);
        if (!bytes) return fail(bytes.error());
        canonical.push_back({departure.key, std::move(bytes).value()});
    }
    std::sort(canonical.begin(), canonical.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    PumpResult result;
    for (const auto& input : canonical)
        if (adapter_->is_departure(input)) {
            result.departures.push_back(input.key);
            DepartureReason reason = DepartureReason::quit;
            for (const auto& [peer, departure] : agreed_departures_) {
                static_cast<void>(peer);
                if (departure.key == input.key) reason = departure.reason;
            }
            result.departure_events.push_back({input.key, reason});
        }
    auto installed = adapter_->install(canonical);
    if (!installed) return fail(installed.error());
    auto stepped = adapter_->step();
    if (!stepped) return fail(stepped.error());
    if (stepped.value().completed_tick != tick + 1 || adapter_->completed_tick() != tick + 1)
        return fail(diagnostic(session_errors::simulation, "adapter did not advance exactly one tick"));
    if (options_.reconnect && reconnect_available_) {
        Bytes row;
        append(row, tick, 8);
        const auto controls = reconnect_controls_.find(tick);
        append(row, controls == reconnect_controls_.end() ? 0 : controls->second.size(), 4);
        if (controls != reconnect_controls_.end()) for (const auto& control : controls->second) {
            append(row, control.size(), 4); row.insert(row.end(), control.begin(), control.end());
        }
        append(row, canonical.size(), 4);
        for (const auto& input : canonical) {
            append(row, input.key.seat.value, 4); append(row, input.key.sequence, 8);
            append(row, input.bytes.size(), 4); row.insert(row.end(), input.bytes.begin(), input.bytes.end());
        }
        if (row.size() + 4 > options_.reconnect->max_log_bytes - reconnect_bytes_)
            disable_reconnect("history budget exhausted; play continues without reconnect");
        else {
            reconnect_bytes_ += row.size() + 4;
            reconnect_log_.push_back(std::move(row));
        }
        reconnect_controls_.erase(tick);
    }
    ready_.erase(tick);
    if (mesh_ && tick > input_horizon()) {
        for (auto row = recovery_[self_].begin(); row != recovery_[self_].lower_bound(tick - input_horizon()); ++row) {
            const auto encoded = encode_batch(row->second);
            if (encoded) mesh_tail_bytes_ -= encoded.value().size();
        }
    }
    for (auto& history : recovery_) {
        const auto window = input_horizon();
        if (tick > window) history.erase(history.begin(), history.lower_bound(tick - window));
    }
    for (const auto& event : result.departure_events)
        for (PeerIndex peer = 0; peer < seats_.size(); ++peer)
            if (seats_[peer].seat == event.key.seat) {
                departed_[peer] = true;
                if (monitor_ && !agreed_departures_.contains(peer)) {
                    auto changed = monitor_->on_departure(peer, tick);
                    if (!changed) return fail(changed.error());
                }
            }
    result.completed = std::move(stepped).value();
    if (monitor_ && !held_) {
        // Engine-side AI can generate inputs inside the same gated step. Capture
        // its executed replay commands too, after all simulation/script commit.
        auto encoded = adapter_->canonical_input();
        if (!encoded) return fail(encoded.error());
        auto input = std::move(encoded).value();
        const auto& completed = *result.completed;
        const bool end = completed.completed_tick == options_.final_tick_count;
        const bool exchange = monitor_->needs_encoding(completed.completed_tick, end);
        const auto encoding = exchange ? adapter_->canonical_state() : Bytes{};
        auto recorded = monitor_->record(completed.completed_tick,
            completed.world_only && exchange ? adapter_->state_hash() : completed.state_sha256, encoding, input, end);
        if (!recorded) return fail(recorded.error());
    }
    if (departure_handler_ && !result.departures.empty()) {
        auto handled = departure_handler_(result.departures);
        if (!handled) return fail(handled.error());
    }
    return core::Result<PumpResult>::success(std::move(result));
}

} // namespace eawr::net
