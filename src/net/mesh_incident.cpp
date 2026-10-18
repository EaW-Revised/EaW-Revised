#include "eawr/net/mesh_incident.hpp"

#include <algorithm>
#include <bit>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr MeshSeats all_seats = static_cast<MeshSeats>((1U << max_match_seats) - 1U);
// The policy's fixed decision window after the repair grace (mesh_policy.hpp).
constexpr std::chrono::milliseconds decision_window{10'000};
// A sender cannot claim an incident older than this: ten minutes is far beyond any pledge deadline.
constexpr std::uint32_t max_elapsed_ms = 10U * 60U * 1000U;
// A smoothed RTT above this is not a measurement of an interactive game link.
constexpr std::uint32_t max_rtt_ms = mesh_rtt_unmeasured;
constexpr std::size_t report_size = 1 + 8 + 8 + 2 + 2 + 2 + 4 + 4 * max_match_seats;

core::Diagnostic diagnostic(std::string_view code, std::string message) {
    core::Diagnostic result;
    result.code = code; result.message = std::move(message);
    return result;
}
template<class T> core::Result<T> refused(std::string message) {
    return core::Result<T>::failure(diagnostic(mesh_errors::malformed, std::move(message)));
}
void put(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
}
bool get(std::span<const std::uint8_t>& bytes, std::uint64_t& value, unsigned width) {
    if (bytes.size() < width) return false;
    value = 0;
    for (unsigned index = 0; index < width; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
    bytes = bytes.subspan(width);
    return true;
}
bool valid_masks(MeshSeats open, MeshSeats failed) {
    return (open & ~all_seats) == 0 && (failed & ~all_seats) == 0 && (open & failed) == 0;
}
void put_decision(Bytes& bytes, const MeshLossDecision& decision) {
    put(bytes, decision.incident, 8); put(bytes, decision.decider, 1);
    put(bytes, decision.active, 2); put(bytes, decision.reported, 2); put(bytes, decision.survivors, 2);
    put(bytes, decision.rejoin_allowed ? 1 : 0, 1);
    put(bytes, static_cast<std::uint8_t>(decision.rule), 1); put(bytes, static_cast<std::uint8_t>(decision.outcome), 1);
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) {
        if (!(decision.reported & mesh_seat(seat))) continue;
        const auto& row = decision.reports[seat];
        put(bytes, row.revision, 8); put(bytes, row.open, 2); put(bytes, row.relay_failed, 2);
    }
}
bool get_decision(std::span<const std::uint8_t>& bytes, MeshLossDecision& decision) {
    std::uint64_t incident{}, decider{}, active{}, reported{}, survivors{}, rejoin{}, rule{}, outcome{};
    if (!get(bytes, incident, 8) || !get(bytes, decider, 1) || !get(bytes, active, 2) || !get(bytes, reported, 2)
        || !get(bytes, survivors, 2) || !get(bytes, rejoin, 1) || !get(bytes, rule, 1) || !get(bytes, outcome, 1)
        || incident == 0 || decider >= max_match_seats || (active & ~all_seats) || (reported & ~active) || (survivors & ~active)
        || rejoin > 1 || rule > 1 || outcome > 1) return false;
    decision = {};
    decision.incident = incident; decision.decider = static_cast<PeerIndex>(decider);
    decision.active = static_cast<MeshSeats>(active); decision.reported = static_cast<MeshSeats>(reported);
    decision.survivors = static_cast<MeshSeats>(survivors); decision.rejoin_allowed = rejoin != 0;
    decision.rule = static_cast<MeshLossRule>(rule); decision.outcome = static_cast<MeshLossOutcome>(outcome);
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) {
        if (!(decision.reported & mesh_seat(seat))) continue;
        std::uint64_t revision{}, open{}, failed{};
        if (!get(bytes, revision, 8) || !get(bytes, open, 2) || !get(bytes, failed, 2) || revision == 0
            || !valid_masks(static_cast<MeshSeats>(open), static_cast<MeshSeats>(failed))) return false;
        auto& row = decision.reports[seat];
        row.incident = incident; row.revision = revision;
        row.open = static_cast<MeshSeats>(open); row.relay_failed = static_cast<MeshSeats>(failed);
    }
    return true;
}
std::uint32_t round_up_ms(std::chrono::microseconds value) {
    if (value.count() < 0) return 0;
    const auto micros = static_cast<std::uint64_t>(value.count());
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(micros / 1000 + (micros % 1000 != 0 ? 1U : 0U), max_rtt_ms));
}
} // namespace

core::Result<Bytes> encode_mesh_report(const MeshLinkReport& report, std::uint32_t elapsed_ms, MeshSeats active) {
    if (!valid_masks(report.open, report.relay_failed) || (active & ~all_seats) || ((report.open | report.relay_failed) & ~active)
        || elapsed_ms > max_elapsed_ms
        || std::any_of(report.rtt_ms.begin(), report.rtt_ms.end(), [](auto rtt) { return rtt > max_rtt_ms; }))
        return refused<Bytes>("invalid link report");
    Bytes frame{mesh_report_kind};
    put(frame, report.incident, 8); put(frame, report.revision, 8); put(frame, report.open, 2); put(frame, report.relay_failed, 2);
    put(frame, active, 2); put(frame, elapsed_ms, 4);
    for (const auto rtt : report.rtt_ms) put(frame, rtt, 4);
    return core::Result<Bytes>::success(std::move(frame));
}
core::Result<MeshReportFrame> decode_mesh_report(std::span<const std::uint8_t> frame) {
    if (frame.size() != report_size || frame.front() != mesh_report_kind) return refused<MeshReportFrame>("link report has an invalid size or kind");
    frame = frame.subspan(1);
    MeshReportFrame result;
    std::uint64_t incident{}, revision{}, open{}, failed{}, active{}, elapsed{};
    if (!get(frame, incident, 8) || !get(frame, revision, 8) || !get(frame, open, 2) || !get(frame, failed, 2) || !get(frame, active, 2)
        || !get(frame, elapsed, 4) || !valid_masks(static_cast<MeshSeats>(open), static_cast<MeshSeats>(failed)) || (active & ~all_seats)
        || ((open | failed) & ~active) || elapsed > max_elapsed_ms)
        return refused<MeshReportFrame>("invalid link report");
    result.report.incident = incident; result.report.revision = revision;
    result.report.open = static_cast<MeshSeats>(open); result.report.relay_failed = static_cast<MeshSeats>(failed);
    result.elapsed_ms = static_cast<std::uint32_t>(elapsed); result.active = static_cast<MeshSeats>(active);
    for (auto& rtt : result.report.rtt_ms) {
        std::uint64_t value{};
        if (!get(frame, value, 4) || value > max_rtt_ms) return refused<MeshReportFrame>("invalid link report round trip");
        rtt = static_cast<std::uint32_t>(value);
    }
    return core::Result<MeshReportFrame>::success(std::move(result));
}
core::Result<Bytes> encode_mesh_decision(std::uint8_t kind, const MeshLossDecision& decision) {
    if ((kind != mesh_proposal_kind && kind != mesh_pledge_kind) || (decision.active & ~all_seats) || decision.decider >= max_match_seats)
        return refused<Bytes>("invalid pair-loss decision");
    Bytes frame{kind};
    put_decision(frame, decision);
    return core::Result<Bytes>::success(std::move(frame));
}
core::Result<MeshLossDecision> decode_mesh_decision(std::uint8_t kind, std::span<const std::uint8_t> frame) {
    MeshLossDecision decision;
    if (frame.empty() || frame.front() != kind || (kind != mesh_proposal_kind && kind != mesh_pledge_kind))
        return refused<MeshLossDecision>("pair-loss decision has an invalid kind");
    frame = frame.subspan(1);
    if (!get_decision(frame, decision) || !frame.empty()) return refused<MeshLossDecision>("invalid pair-loss decision");
    return core::Result<MeshLossDecision>::success(std::move(decision));
}
core::Result<Bytes> encode_mesh_confirmation(const MeshLossConfirmation& confirmation) {
    if ((confirmation.pledges & ~all_seats) || (confirmation.survivors & ~all_seats)) return refused<Bytes>("invalid pair-loss confirmation");
    Bytes frame{mesh_confirm_kind};
    put_decision(frame, confirmation.proposal);
    put(frame, confirmation.pledges, 2); put(frame, confirmation.survivors, 2);
    return core::Result<Bytes>::success(std::move(frame));
}
core::Result<MeshLossConfirmation> decode_mesh_confirmation(std::span<const std::uint8_t> frame) {
    MeshLossConfirmation result;
    std::uint64_t pledges{}, survivors{};
    if (frame.empty() || frame.front() != mesh_confirm_kind) return refused<MeshLossConfirmation>("pair-loss confirmation has an invalid kind");
    frame = frame.subspan(1);
    if (!get_decision(frame, result.proposal) || !get(frame, pledges, 2) || !get(frame, survivors, 2) || !frame.empty()
        || (pledges & ~all_seats) || (survivors & ~all_seats)) return refused<MeshLossConfirmation>("invalid pair-loss confirmation");
    result.pledges = static_cast<MeshSeats>(pledges); result.survivors = static_cast<MeshSeats>(survivors);
    return core::Result<MeshLossConfirmation>::success(std::move(result));
}

MeshIncident::MeshIncident(MessageTransport& transport, MeshSeats active, MeshIncidentOptions options)
    : transport_(&transport), options_(std::move(options)), self_(transport.self()), active_(static_cast<MeshSeats>(active & all_seats)) {
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat)
        if (seat != self_ && (active_ & mesh_seat(seat)) && transport_->link(seat) == LinkState::open)
            was_open_ = static_cast<MeshSeats>(was_open_ | mesh_seat(seat));
}

core::Diagnostic MeshIncident::malformed(std::string message) const { return diagnostic(mesh_errors::malformed, std::move(message)); }

MeshIncident::Time MeshIncident::round_deadline() const {
    return round_ == 0 ? options_.repair_grace + decision_window : decision_window * 2;
}

void MeshIncident::next_round() {
    // No seat can still act on the window that just closed: its decider stopped resending at the deadline and its pledged seats
    // gave up ten seconds later, which is what the caller waited for.
    round_offset_ += round_deadline() + decision_window + mesh_receiver_tolerance;
    ++round_;
    policy_.emplace(self_, active_, incident_, Time{0}, round_deadline());
    have_report_ = false; sent_confirmation_ = false; confirmed_ = false;
    proposal_.reset(); frozen_.reset();
    last_report_ = last_resend_ = Time{0};
}

MeshLossState MeshIncident::state() const { return policy_ ? policy_->state() : MeshLossState::settled; }

bool MeshIncident::owns_closed_link(PeerIndex seat) const {
    return std::popcount(active_) >= 3 && seat < max_match_seats && (active_ & mesh_seat(seat)) != 0;
}

bool MeshIncident::removal_confirmed(PeerIndex seat) const {
    return phase_ == Phase::settling && (removed_ & mesh_seat(seat)) != 0;
}

void MeshIncident::set_active(MeshSeats active) {
    active = static_cast<MeshSeats>(active & all_seats);
    if (active == active_) return;
    if (phase_ == Phase::quiet) {
        active_ = active;
        was_open_ &= active_;
        flapping_ &= active_;
        return;
    }
    if (phase_ != Phase::running || proposal_ || !policy_ || policy_->decision()) return;
    active_ = active;
    was_open_ &= active_;
    flapping_ &= active_;
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) if (!(active_ & mesh_seat(seat))) heard_[seat].reset();
    if (std::popcount(active_) < 3) {
        phase_ = Phase::quiet; policy_.reset(); have_report_ = false;
        return;
    }
    policy_.emplace(self_, active_, incident_, Time{0}, round_deadline());
    // The revision stays monotonic across the restart: peers that kept their policy reject an older revision of this seat.
    have_report_ = false; sent_confirmation_ = false;
}

MeshLinkReport MeshIncident::local_row(Time now) {
    (void)now;
    MeshLinkReport row;
    row.incident = phase_ == Phase::quiet ? 0 : incident_;
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) {
        if (seat == self_ || !(active_ & mesh_seat(seat))) continue;
        const auto link = transport_->link(seat);
        const bool held = phase_ != Phase::quiet && (flapping_ & mesh_seat(seat));
        const bool unsendable = phase_ != Phase::quiet && (send_failed_ & mesh_seat(seat)) != 0;
        // Each end proves its own relay failure (design, section 3); a remote report never turns an open local link into a
        // failed one. A half-open pair therefore blocks the selection and fails closed at the agreed deadline (EAWR-NET-1780).
        // A flapping pair is held lost from this end's own history.
        if (link == LinkState::open && !held && !unsendable) row.open = static_cast<MeshSeats>(row.open | mesh_seat(seat));
        else if (link == LinkState::closed || held || unsendable) {
            if (held || unsendable || !options_.relay_failed || options_.relay_failed(seat))
                row.relay_failed = static_cast<MeshSeats>(row.relay_failed | mesh_seat(seat));
        }
        std::optional<std::uint32_t> rtt;
        if (options_.rtt_ms) rtt = options_.rtt_ms(seat);
        else if (const auto stats = transport_->link_stats(seat); stats && stats->sample_count != 0)
            rtt = round_up_ms(stats->smoothed_rtt);
        row.rtt_ms[seat] = rtt ? std::min(*rtt, max_rtt_ms) : mesh_rtt_unmeasured;
    }
    return row;
}

core::Result<void> MeshIncident::send(PeerIndex to, const Bytes& frame) {
    if (to == self_ || to >= max_match_seats || transport_->link(to) != LinkState::open) return core::Result<void>::success();
    // A failed send on a link that still reads open is this end's evidence that the pair is lost (local_row reports it as failed);
    // it never aborts the session or the broadcast to the other seats. A later successful send clears it.
    if (auto sent = transport_->send(to, frame); !sent) send_failed_ = static_cast<MeshSeats>(send_failed_ | mesh_seat(to));
    else send_failed_ &= static_cast<MeshSeats>(~mesh_seat(to));
    return core::Result<void>::success();
}
core::Result<void> MeshIncident::broadcast(MeshSeats to, const Bytes& frame) {
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat)
        if ((to & mesh_seat(seat)) && seat != self_)
            if (auto sent = send(seat, frame); !sent) return sent;
    return core::Result<void>::success();
}

void MeshIncident::note_losses(Time now) {
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) {
        if (seat == self_ || !(active_ & mesh_seat(seat))) continue;
        auto& history = losses_[seat];
        const bool open = transport_->link(seat) == LinkState::open;
        if (!open && (was_open_ & mesh_seat(seat))) history.push_back(now);
        // Only the newest flap_limit losses can matter: the queue stays bounded however fast a link toggles.
        while (history.size() > std::max<std::size_t>(options_.flap_limit, 1)) history.pop_front();
        if (open) was_open_ = static_cast<MeshSeats>(was_open_ | mesh_seat(seat));
        else was_open_ &= static_cast<MeshSeats>(~mesh_seat(seat));
        while (!history.empty() && now - history.front() > options_.flap_window) history.pop_front();
        if (options_.flap_limit != 0 && history.size() >= options_.flap_limit) flapping_ = static_cast<MeshSeats>(flapping_ | mesh_seat(seat));
        else if (phase_ == Phase::quiet) flapping_ &= static_cast<MeshSeats>(~mesh_seat(seat));
    }
}

core::Result<void> MeshIncident::begin(Time now, Time elapsed) {
    if (phase_ != Phase::quiet || std::popcount(active_) < 3) return core::Result<void>::success();
    base_ = now - std::min(elapsed, Time{max_elapsed_ms});
    round_ = 0; round_offset_ = Time{0};
    policy_.emplace(self_, active_, incident_, Time{0}, round_deadline());
    phase_ = Phase::running;
    revision_ = 0; have_report_ = false; proposal_.reset(); frozen_.reset(); sent_confirmation_ = false; confirmed_ = false;
    send_failed_ = 0;
    last_report_ = last_resend_ = Time{0};
    return core::Result<void>::success();
}

void MeshIncident::finish(MeshIncidentResult result) {
    result_ = std::move(result);
}

void MeshIncident::leave(std::string_view code, std::string message) {
    if (phase_ == Phase::left) return;
    MeshIncidentResult result;
    result.kind = MeshIncidentResult::Kind::leave; result.incident = incident_;
    result.code = std::string(code); result.message = std::move(message);
    if (policy_ && policy_->decision()) {
        result.survivors = policy_->decision()->survivors;
        result.removed = static_cast<MeshSeats>(policy_->decision()->active & ~policy_->decision()->survivors);
        const auto& decision = *policy_->decision();
        for (PeerIndex seat = 0; seat < max_match_seats; ++seat)
            if (decision.reported & mesh_seat(seat))
                result.lost[seat] = static_cast<MeshSeats>(decision.active & ~mesh_seat(seat) & ~decision.reports[seat].open);
    }
    phase_ = Phase::left;
    finish(std::move(result));
}

core::Result<void> MeshIncident::receive(const Received& frame, Time now) {
    if (phase_ == Phase::left || frame.frame.empty()) return core::Result<void>::success();
    const auto kind = frame.frame.front();
    const PeerIndex sender = frame.from;
    if (kind == mesh_report_kind) {
        auto decoded = decode_mesh_report(frame.frame);
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (sender >= max_match_seats || sender == self_ || !(active_ & mesh_seat(sender))) return core::Result<void>::success();
        const auto& report = decoded.value().report;
        // A late duplicate of an older report of the same stream must not undo a newer one.
        const auto& known = heard_[sender];
        const bool newer = !known || (report.incident == 0 ? (known->report.incident != 0 || report.revision >= known->report.revision)
            : (report.incident > known->report.incident || (report.incident == known->report.incident && report.revision >= known->report.revision)));
        if (newer) heard_[sender] = Heard{report, now};
        if (report.incident == 0 || report.incident != incident_) return core::Result<void>::success();
        // Seats take part in one incident only while they agree on who is in the match: a seat whose set differs (a departure
        // it has not learned of yet, or one the sender has not) neither begins nor feeds this seat's policy. Both sides
        // converge through the ordinary departure agreement and restart over the same set; the sender resends every second.
        if (decoded.value().active != active_) return core::Result<void>::success();
        if (phase_ == Phase::quiet) {
            if (auto begun = begin(now, Time{decoded.value().elapsed_ms}); !begun) return begun;
        }
        if (phase_ == Phase::quiet) return core::Result<void>::success(); // fewer than three active seats
        const Time claimed = now - Time{decoded.value().elapsed_ms};
        if (claimed < base_) base_ = claimed; // the incident began earlier than this seat noticed
        policy_->report(sender, report, policy_time(now));
        return core::Result<void>::success();
    }
    if (kind != mesh_proposal_kind && kind != mesh_pledge_kind && kind != mesh_confirm_kind)
        return core::Result<void>::failure(malformed("unknown pair-loss control kind"));
    if (kind == mesh_confirm_kind) {
        auto decoded = decode_mesh_confirmation(frame.frame);
        if (!decoded) return core::Result<void>::failure(decoded.error());
        if (sender >= max_match_seats || sender == self_ || !(active_ & mesh_seat(sender)) || !policy_
            || decoded.value().proposal.incident != incident_) return core::Result<void>::success();
        if (policy_->commit(sender, decoded.value(), policy_time(now))) confirmed_ = true;
        return core::Result<void>::success();
    }
    auto decoded = decode_mesh_decision(kind, frame.frame);
    if (!decoded) return core::Result<void>::failure(decoded.error());
    const auto& decision = decoded.value();
    if (sender >= max_match_seats || sender == self_ || !(active_ & mesh_seat(sender)) || decision.incident != incident_
        || phase_ == Phase::settling) return core::Result<void>::success();
    if (phase_ == Phase::quiet) {
        if (decision.active != active_) return core::Result<void>::success();
        if (auto begun = begin(now, Time{0}); !begun) return begun;
        if (phase_ == Phase::quiet) return core::Result<void>::success();
    }
    if (kind == mesh_pledge_kind) {
        // A pledge that is not the exact proposal is ignored: one bad pledge must not end the decider's incident.
        if (policy_->decision() && decision == *policy_->decision()) policy_->acknowledge(sender, decision, policy_time(now));
        return core::Result<void>::success();
    }
    if (policy_->accept(sender, decision, policy_time(now))) {
        auto pledge = encode_mesh_decision(mesh_pledge_kind, decision);
        if (!pledge) return core::Result<void>::failure(pledge.error());
        return send(decision.decider, pledge.value());
    }
    return core::Result<void>::success();
}

core::Result<void> MeshIncident::service(Time now, bool receive_drained) {
    if (phase_ == Phase::left) return core::Result<void>::success();
    note_losses(now);
    std::erase_if(tails_, [now](const Tail& tail) { return now >= tail.until; });
    for (auto& tail : tails_) {
        if (now - tail.last_sent >= options_.report_interval) {
            auto frame = encode_mesh_confirmation(tail.confirmation);
            if (!frame) return core::Result<void>::failure(frame.error());
            tail.last_sent = now;
            if (auto sent = broadcast(static_cast<MeshSeats>(active_ & ~mesh_seat(self_)), frame.value()); !sent) return sent;
        }
    }
    if (phase_ == Phase::quiet) {
        // A closed stream is conclusive only after the available queue drained (an ordinary quit's batch may still be queued).
        if (receive_drained && std::popcount(active_) >= 3)
            for (PeerIndex seat = 0; seat < max_match_seats && phase_ == Phase::quiet; ++seat)
                if (seat != self_ && (active_ & mesh_seat(seat)) && transport_->link(seat) == LinkState::closed)
                    if (auto begun = begin(now, Time{0}); !begun) return begun;
        if (phase_ == Phase::quiet) {
            // Steady state: every seat gives the host its round trips (unsigned, non-authoritative) once a second.
            if (self_ != options_.host && (active_ & mesh_seat(options_.host)) && now - last_steady_ >= options_.report_interval) {
                auto row = local_row(now);
                row.revision = ++steady_revision_;
                auto frame = encode_mesh_report(row, 0, active_);
                if (!frame) return core::Result<void>::failure(frame.error());
                last_steady_ = now;
                return send(options_.host, frame.value());
            }
            return core::Result<void>::success();
        }
    }
    const auto elapsed = relative(now);
    const auto rel = policy_time(now);
    auto row = local_row(now);
    const bool changed = !have_report_ || row.open != row_.open || row.relay_failed != row_.relay_failed;
    if (changed) ++revision_;
    row.revision = revision_;
    row_ = row; have_report_ = true;
    if (phase_ == Phase::settling) {
        // A further pair loss between confirmed survivors, before their departures settle, fails closed.
        const auto& decision = policy_->decision();
        if (decision) for (PeerIndex seat = 0; seat < max_match_seats; ++seat)
            if (seat != self_ && (decision->survivors & mesh_seat(self_)) && (decision->survivors & mesh_seat(seat))
                && transport_->link(seat) == LinkState::closed) policy_->pair_lost(self_, seat);
    }
    policy_->report(self_, row_, rel);
    if (changed || now - last_report_ >= options_.report_interval) {
        auto frame = encode_mesh_report(row_, static_cast<std::uint32_t>(std::min<std::int64_t>(elapsed.count(), max_elapsed_ms)), active_);
        if (!frame) return core::Result<void>::failure(frame.error());
        last_report_ = now;
        if (auto sent = broadcast(active_, frame.value()); !sent) return sent;
    }
    const auto deadline = round_deadline();
    if (phase_ == Phase::running) {
        if (auto decision = policy_->decide(rel)) {
            proposal_ = *decision;
            auto frame = encode_mesh_decision(mesh_proposal_kind, *decision);
            if (!frame) return core::Result<void>::failure(frame.error());
            last_resend_ = now;
            if (auto sent = broadcast(static_cast<MeshSeats>(decision->reported & ~mesh_seat(self_)), frame.value()); !sent) return sent;
        } else if (proposal_ && !frozen_ && proposal_->decider == self_ && rel < deadline && now - last_resend_ >= options_.report_interval) {
            auto frame = encode_mesh_decision(mesh_proposal_kind, *proposal_);
            if (!frame) return core::Result<void>::failure(frame.error());
            last_resend_ = now;
            if (auto sent = broadcast(static_cast<MeshSeats>(proposal_->reported & ~mesh_seat(self_)), frame.value()); !sent) return sent;
        }
        if (!frozen_ && proposal_ && proposal_->decider == self_) {
            if (auto confirmation = policy_->confirmation(rel)) {
                frozen_ = *confirmation;
                if (!policy_->commit(self_, *confirmation, rel)) { leave(mesh_errors::no_decision, "The agreement could not be committed."); return core::Result<void>::success(); }
                confirmed_ = true;
                auto frame = encode_mesh_confirmation(*frozen_);
                if (!frame) return core::Result<void>::failure(frame.error());
                last_resend_ = now;
                // Everyone, including a seat that will leave, must learn the frozen result.
                if (auto sent = broadcast(static_cast<MeshSeats>(active_ & ~mesh_seat(self_)), frame.value()); !sent) return sent;
            }
        } else if (frozen_ && rel <= deadline + decision_window && now - last_resend_ >= options_.report_interval) {
            auto frame = encode_mesh_confirmation(*frozen_);
            if (!frame) return core::Result<void>::failure(frame.error());
            last_resend_ = now;
            if (auto sent = broadcast(static_cast<MeshSeats>(active_ & ~mesh_seat(self_)), frame.value()); !sent) return sent;
        }
    } else if (frozen_ && rel <= deadline + decision_window && now - last_resend_ >= options_.report_interval) {
        auto frame = encode_mesh_confirmation(*frozen_);
        if (!frame) return core::Result<void>::failure(frame.error());
        last_resend_ = now;
        if (auto sent = broadcast(static_cast<MeshSeats>(active_ & ~mesh_seat(self_)), frame.value()); !sent) return sent;
    }
    switch (policy_->state(rel)) {
    case MeshLossState::restored: {
        MeshIncidentResult result;
        result.kind = MeshIncidentResult::Kind::restored; result.incident = incident_; result.survivors = active_;
        finish(std::move(result));
        ++incident_;
        // The decider keeps resending the frozen confirmation until the pledge window closes (a late seat may still need it).
        if (frozen_) tails_.push_back({*frozen_, base_ + round_offset_ + deadline + decision_window, now});
        phase_ = Phase::quiet; policy_.reset(); proposal_.reset(); frozen_.reset(); confirmed_ = false;
        break;
    }
    case MeshLossState::settling:
        if (phase_ == Phase::running) {
            const auto& decision = *policy_->decision();
            MeshIncidentResult result;
            result.kind = MeshIncidentResult::Kind::departure; result.incident = incident_;
            result.survivors = decision.survivors;
            result.removed = static_cast<MeshSeats>(decision.active & ~decision.survivors);
            for (PeerIndex seat = 0; seat < max_match_seats; ++seat)
                if (decision.reported & mesh_seat(seat))
                    result.lost[seat] = static_cast<MeshSeats>(decision.active & ~mesh_seat(seat) & ~decision.reports[seat].open);
            removed_ = result.removed;
            finish(std::move(result));
            phase_ = Phase::settling;
        }
        break;
    case MeshLossState::leave: {
        const auto& decision = policy_->decision();
        if (phase_ == Phase::settling) leave(mesh_errors::overlap, "A further connection loss happened while a player was being removed.");
        else if (confirmed_ && decision && !(decision->survivors & mesh_seat(self_)) && decision->outcome == MeshLossOutcome::departure)
            leave(mesh_errors::left, "This player was removed because a connection could not be restored.");
        else if (!decision) {
            // Nobody pledged and nothing was confirmed to this seat: the window passed without a decision. While the pairs are
            // still being repaired, the next window is a new chance (every seat counts the same incident time, so all of them
            // start it together); only after the last round does the match end.
            if (round_ + 1 < std::max(1U, options_.rounds) && !confirmed_ && rel >= deadline) {
                if (rel >= deadline + decision_window + mesh_receiver_tolerance) next_round();
            } else leave(mesh_errors::no_majority, "No player could decide which connection to give up.");
        }
        else leave(mesh_errors::no_decision, "The decision on the lost connection did not arrive in time.");
        break;
    }
    default: break;
    }
    return core::Result<void>::success();
}

std::optional<MeshIncidentResult> MeshIncident::take_result() {
    auto result = std::move(result_);
    result_.reset();
    return result;
}

void MeshIncident::survivor_lost(PeerIndex a, PeerIndex b) {
    if (policy_ && phase_ == Phase::settling) policy_->pair_lost(a, b);
}

void MeshIncident::settled() {
    if (phase_ != Phase::settling || !policy_) return;
    policy_->settled();
    const auto survivors = policy_->decision() ? policy_->decision()->survivors : active_;
    ++incident_;
    phase_ = Phase::quiet; policy_.reset(); proposal_.reset(); frozen_.reset(); removed_ = 0; confirmed_ = false;
    active_ = static_cast<MeshSeats>(survivors & active_);
    was_open_ &= active_;
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) if (!(active_ & mesh_seat(seat))) { losses_[seat].clear(); heard_[seat].reset(); }
    flapping_ &= active_;
}

std::array<std::optional<MeshLinkReport>, max_match_seats> MeshIncident::rtt_reports(Time now) const {
    std::array<std::optional<MeshLinkReport>, max_match_seats> reports;
    for (PeerIndex seat = 0; seat < max_match_seats; ++seat) {
        if (!(active_ & mesh_seat(seat))) continue;
        if (seat == self_) {
            MeshLinkReport own;
            for (PeerIndex remote = 0; remote < max_match_seats; ++remote) {
                if (remote == self_ || !(active_ & mesh_seat(remote))) continue;
                std::optional<std::uint32_t> rtt;
                if (options_.rtt_ms) rtt = options_.rtt_ms(remote);
                else if (const auto stats = transport_->link_stats(remote); stats && stats->sample_count != 0) rtt = round_up_ms(stats->smoothed_rtt);
                own.rtt_ms[remote] = rtt ? std::min(*rtt, max_rtt_ms) : mesh_rtt_unmeasured;
            }
            reports[seat] = own;
        } else if (heard_[seat] && now - heard_[seat]->at <= options_.report_freshness) reports[seat] = heard_[seat]->report;
    }
    return reports;
}

} // namespace eawr::net
