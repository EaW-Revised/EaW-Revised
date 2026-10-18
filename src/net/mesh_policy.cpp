#include "eawr/net/mesh_policy.hpp"

#include <algorithm>
#include <bit>

namespace eawr::net {
namespace {
constexpr MeshSeats all_seats = static_cast<MeshSeats>((1U << max_match_seats) - 1U);
bool valid_active(MeshSeats active) { return (active & ~all_seats) == 0 && std::popcount(active) >= 2; }
bool all_open(PeerIndex seat, MeshSeats active, const MeshLinkReport& report) {
    return report.open == (active & ~mesh_seat(seat));
}
bool valid_report(PeerIndex seat, MeshSeats active, std::uint64_t incident, const MeshLinkReport& report) {
    const auto remote = static_cast<MeshSeats>(active & ~mesh_seat(seat));
    return report.incident == incident && report.revision != 0 && (report.open & ~remote) == 0
        && (report.relay_failed & ~remote) == 0 && (report.open & report.relay_failed) == 0;
}
bool same_topology(const MeshLinkReport& a, const MeshLinkReport& b) {
    return a.incident == b.incident && a.revision == b.revision && a.open == b.open && a.relay_failed == b.relay_failed;
}
bool earlier_seats(MeshSeats a, MeshSeats b) {
    for (PeerIndex p = 0; p < max_match_seats; ++p)
        if ((a & mesh_seat(p)) != (b & mesh_seat(p))) return (a & mesh_seat(p)) != 0;
    return false;
}
bool clique(MeshSeats seats, const std::array<MeshLinkReport, max_match_seats>& reports) {
    for (PeerIndex p = 0; p < max_match_seats; ++p)
        if ((seats & mesh_seat(p)) && (reports[p].open & seats) != (seats & ~mesh_seat(p))) return false;
    return true;
}
bool enough_survivors(const MeshLossDecision& proposal, MeshSeats survivors) {
    return std::popcount(survivors) >= 2 && (proposal.rule != MeshLossRule::majority_clique
        || std::popcount(survivors) > std::popcount(proposal.active) / 2);
}
std::optional<MeshLossDecision> selection(MeshSeats active, MeshSeats reported, std::uint64_t incident,
    const std::array<MeshLinkReport, max_match_seats>& reports) {
    if (!valid_active(active) || (reported & ~active) != 0 || std::popcount(reported) <= std::popcount(active) / 2) return {};
    std::optional<PeerIndex> decider;
    std::array<MeshSeats, max_match_seats> losses{};
    for (PeerIndex a = 0; a < max_match_seats; ++a) {
        if (!(reported & mesh_seat(a))) continue;
        if (!valid_report(a, active, incident, reports[a])) return {};
        if (!decider && all_open(a, active, reports[a])) decider = a;
        for (PeerIndex b = 0; b < max_match_seats; ++b) {
            if (a == b || !(active & mesh_seat(b))) continue;
            if (!(reports[a].open & mesh_seat(b))) {
                // Across an unreachable cut only the inside endpoint's report
                // exists. Where both reports exist, both must prove failed TURN.
                if (!(reports[a].relay_failed & mesh_seat(b))
                    || ((reported & mesh_seat(b)) && !(reports[b].relay_failed & mesh_seat(a)))) return {};
                losses[a] = static_cast<MeshSeats>(losses[a] | mesh_seat(b));
                losses[b] = static_cast<MeshSeats>(losses[b] | mesh_seat(a));
            }
        }
    }
    MeshSeats survivors = active;
    MeshLossRule rule = MeshLossRule::all_open;
    if (decider) {
        // A globally open candidate waits for every current report. Other
        // seats cannot replace it with a smaller partial-view selection.
        if (reported != active) return {};
        while (true) {
            int most_lost = 0;
            PeerIndex victim = 0;
            for (PeerIndex a = 0; a < max_match_seats; ++a) {
                if (!(survivors & mesh_seat(a))) continue;
                const int lost = std::popcount(static_cast<MeshSeats>(losses[a] & survivors));
                if (lost > 0 && lost >= most_lost) { most_lost = lost; victim = a; }
            }
            if (most_lost == 0) break;
            survivors &= static_cast<MeshSeats>(~mesh_seat(victim));
        }
    } else {
        rule = MeshLossRule::majority_clique;
        survivors = 0;
        // At nine seats there are only 511 nonempty subsets. This runs once
        // per incident decision outside sim, never per entity or simulation tick.
        for (MeshSeats candidate = 1; candidate <= all_seats; ++candidate) {
            if ((candidate & ~reported) != 0 || std::popcount(candidate) <= std::popcount(active) / 2
                || !clique(candidate, reports)) continue;
            if (std::popcount(candidate) > std::popcount(survivors)
                || (std::popcount(candidate) == std::popcount(survivors) && earlier_seats(candidate, survivors))) survivors = candidate;
        }
        if (survivors == 0) return {};
        for (PeerIndex p = 0; p < max_match_seats; ++p) if (survivors & mesh_seat(p)) { decider = p; break; }
    }
    if (!(survivors & mesh_seat(*decider))) return {};
    auto topology = reports;
    for (PeerIndex p = 0; p < max_match_seats; ++p) {
        if (!(reported & mesh_seat(p))) topology[p] = {};
        topology[p].rtt_ms.fill(0);
    }
    return MeshLossDecision{incident, *decider, active, reported, survivors, topology, true, rule,
        survivors == active ? MeshLossOutcome::restored : MeshLossOutcome::departure};
}
}
std::chrono::milliseconds mesh_report_grace(std::chrono::milliseconds pair_timeout) {
    return std::min(std::chrono::milliseconds{5000}, pair_timeout / 2);
}
MeshRelayFailure mesh_silent_end_failed(MeshRelayFailure reported, std::chrono::milliseconds waited,
    std::chrono::milliseconds pair_timeout) {
    if (reported.a_failed == reported.b_failed || waited < mesh_report_grace(pair_timeout)) return reported;
    return {true, true};
}
std::string mesh_newcomer_refusal_text(std::string_view name, bool relay_configured) {
    return "Could not connect to " + std::string(name) + ". Your network or theirs blocks a direct connection and "
        + (relay_configured ? "the relay could not help." : "no relay is configured on this server.");
}
MeshFormation::MeshFormation(std::uint32_t capacity)
    : capacity_(capacity >= 2 && capacity <= max_match_seats ? capacity : 0) {}
bool MeshFormation::attach(PeerIndex seat, std::uint64_t revision) {
    if (closed_ || seat >= capacity_ || revision == 0 || (occupied_ & mesh_seat(seat))
        || (seat != 0 && !(occupied_ & mesh_seat(0)))) return false;
    occupied_ = static_cast<MeshSeats>(occupied_ | mesh_seat(seat)); admitted_[seat] = revision;
    return true;
}
void MeshFormation::vacate(PeerIndex seat) {
    if (seat >= capacity_) return;
    occupied_ &= static_cast<MeshSeats>(~mesh_seat(seat));
    links_[seat] = 0; regranted_[seat] = 0; admitted_[seat] = 0; established_[seat] = false;
    for (PeerIndex p = 0; p < capacity_; ++p) {
        links_[p] &= static_cast<MeshSeats>(~mesh_seat(seat));
        regranted_[p] &= static_cast<MeshSeats>(~mesh_seat(seat));
    }
    if (seat == 0) closed_ = true;
}
bool MeshFormation::pair_open(PeerIndex a, PeerIndex b) {
    if (closed_ || a == b || a >= capacity_ || b >= capacity_
        || !(occupied_ & mesh_seat(a)) || !(occupied_ & mesh_seat(b))) return false;
    links_[a] = static_cast<MeshSeats>(links_[a] | mesh_seat(b));
    links_[b] = static_cast<MeshSeats>(links_[b] | mesh_seat(a));
    for (PeerIndex p = 0; p < capacity_; ++p) if (connected(p)) established_[p] = true;
    return true;
}
bool MeshFormation::connected(PeerIndex seat) const {
    return !closed_ && seat < capacity_ && (occupied_ & mesh_seat(seat))
        && links_[seat] == (occupied_ & ~mesh_seat(seat));
}
bool MeshFormation::complete() const {
    if (closed_ || capacity_ == 0 || occupied_ != static_cast<MeshSeats>((1U << capacity_) - 1U)) return false;
    for (PeerIndex seat = 0; seat < capacity_; ++seat) if (!connected(seat)) return false;
    return true;
}
MeshFormationResult MeshFormation::pair_failed(PeerIndex a, PeerIndex b, MeshRelayFailure relay) {
    if (closed_ || a == b || a >= capacity_ || b >= capacity_
        || !(occupied_ & mesh_seat(a)) || !(occupied_ & mesh_seat(b))) return {};
    links_[a] &= static_cast<MeshSeats>(~mesh_seat(b)); links_[b] &= static_cast<MeshSeats>(~mesh_seat(a));
    if (!relay.a_failed || !relay.b_failed) return {};
    const PeerIndex later = admitted_[a] > admitted_[b] || (admitted_[a] == admitted_[b] && a > b) ? a : b;
    const PeerIndex other = later == a ? b : a;
    if (a == 0 || b == 0) {
        const PeerIndex guest = a == 0 ? b : a;
        vacate(guest); return {MeshFormationAction::refuse, guest, 0, true};
    }
    if (established_[a] && established_[b] && !(regranted_[a] & mesh_seat(b))) {
        regranted_[a] = static_cast<MeshSeats>(regranted_[a] | mesh_seat(b));
        regranted_[b] = static_cast<MeshSeats>(regranted_[b] | mesh_seat(a));
        return {MeshFormationAction::regrant, later, other, false};
    }
    vacate(later); return {MeshFormationAction::refuse, later, other, true};
}
MeshLossPolicy::MeshLossPolicy(PeerIndex self, MeshSeats active, std::uint64_t incident, Time started, Time deadline)
    : self_(self), active_(active), incident_(incident), started_(started), changed_(started), deadline_(deadline), last_now_(started) {
    if (!valid_active(active) || !(active & mesh_seat(self)) || incident == 0 || deadline <= started || deadline < Time{10000})
        state_ = MeshLossState::leave;
    else rejoin_allowed_ = true;
}
bool MeshLossPolicy::report(PeerIndex from, const MeshLinkReport& value, Time now) {
    now = monotonic(now);
    if (state_ == MeshLossState::leave || state_ == MeshLossState::restored || state_ == MeshLossState::settled
        || from >= max_match_seats || !(active_ & mesh_seat(from))
        || !valid_report(from, active_, incident_, value) || now < started_ || now < changed_) return false;
    if (received_ & mesh_seat(from)) {
        if (value.revision < reports_[from].revision) return false;
        if (value.revision == reports_[from].revision) {
            if (!same_topology(value, reports_[from])) return false;
            reports_[from] = value; seen_[from] = now; return true;
        }
    }
    // A restoration removes nobody, so a pair lost while it is pending is not its problem: it commits and the loss opens the
    // next incident. A departure needs its survivors to stay a clique until they have settled.
    if (decision_ && decision_->outcome == MeshLossOutcome::departure
        && (state_ == MeshLossState::agreeing || state_ == MeshLossState::settling)
        && (decision_->survivors & mesh_seat(from))) {
        const auto remaining = static_cast<MeshSeats>(decision_->survivors & ~mesh_seat(from));
        if ((value.open & remaining) != remaining) state_ = MeshLossState::leave;
    }
    if (!(received_ & mesh_seat(from)) || reports_[from].open != value.open
        || reports_[from].relay_failed != value.relay_failed) changed_ = now;
    reports_[from] = value; seen_[from] = now; received_ = static_cast<MeshSeats>(received_ | mesh_seat(from));
    return true;
}
MeshLossPolicy::Time MeshLossPolicy::monotonic(Time now) {
    last_now_ = std::max(last_now_, now);
    return last_now_;
}
void MeshLossPolicy::advance(Time now) {
    now = monotonic(now);
    // Repair grace is agreed, not restarted by each seat's detection time.
    // Even an all-open local view stays stalled until a confirmed proposal.
    if (state_ == MeshLossState::repairing && now >= deadline_ - Time{10000}) state_ = MeshLossState::awaiting_decision;
    if ((state_ == MeshLossState::repairing || state_ == MeshLossState::awaiting_decision)
        && now >= deadline_ + mesh_receiver_tolerance) state_ = MeshLossState::leave;
    if (state_ == MeshLossState::agreeing && now >= deadline_) {
        const bool decider_without_majority = decision_ && decision_->decider == self_
            && std::popcount(acknowledged_) <= std::popcount(active_) / 2;
        if (decider_without_majority || now >= deadline_ + Time{10000} + mesh_receiver_tolerance) state_ = MeshLossState::leave;
    }
}
std::optional<MeshLossDecision> MeshLossPolicy::decide(Time now) {
    now = monotonic(now);
    advance(now);
    if ((state_ != MeshLossState::awaiting_decision && state_ != MeshLossState::repairing) || now >= deadline_
        || !(received_ & mesh_seat(self_)) || now < changed_ + Time{2000}) return {};
    auto selected = selection(active_, fresh_reports(now), incident_, reports_);
    if (!selected || selected->decider != self_
        || (state_ == MeshLossState::repairing && selected->outcome != MeshLossOutcome::restored)) return {};
    decision_ = selected; acknowledged_ = mesh_seat(self_); state_ = MeshLossState::agreeing;
    return selected;
}
MeshSeats MeshLossPolicy::fresh_reports(Time now) const {
    MeshSeats fresh = received_;
    for (PeerIndex p = 0; p < max_match_seats; ++p)
        if (p != self_ && now > seen_[p] + Time{3000}) fresh &= static_cast<MeshSeats>(~mesh_seat(p));
    return fresh;
}
bool MeshLossPolicy::accept(PeerIndex sender, const MeshLossDecision& value, Time now) {
    now = monotonic(now);
    advance(now);
    if (decision_) return state_ != MeshLossState::leave && sender == value.decider && value == *decision_;
    if ((state_ != MeshLossState::awaiting_decision && state_ != MeshLossState::repairing) || sender >= max_match_seats
        || sender != value.decider || value.incident != incident_ || value.active != active_
        || !(received_ & mesh_seat(sender)) || !(received_ & mesh_seat(self_))
        || !(value.reported & mesh_seat(self_))
        || (sender != self_ && now > seen_[sender] + Time{3000})
        || !value.rejoin_allowed) return false;
    const auto expected = selection(active_, value.reported, incident_, value.reports);
    if (!expected || *expected != value) return false;
    // A restoration is the decider's own snapshot of an all-open mesh and removes nobody. A seat whose view of some pair is
    // older than the snapshot (that pair is silent towards it, the situation that makes restoration necessary) must still be
    // able to pledge: the snapshot above already proved it is exactly what the selection rule yields for its reports.
    if (value.outcome == MeshLossOutcome::restored) {
        decision_ = value;
        acknowledged_ = mesh_seat(self_); state_ = MeshLossState::agreeing;
        return true;
    }
    if (value.rule == MeshLossRule::all_open && !all_open(sender, active_, reports_[sender])) return false;
    if (value.rule == MeshLossRule::majority_clique
        && (reports_[sender].open & value.survivors) != (value.survivors & ~mesh_seat(sender))) return false;
    if (value.rule == MeshLossRule::majority_clique) {
        const auto local = selection(active_, fresh_reports(now), incident_, reports_);
        if (local && (local->rule == MeshLossRule::all_open || std::popcount(local->survivors) > std::popcount(value.survivors)
            || (std::popcount(local->survivors) == std::popcount(value.survivors) && earlier_seats(local->survivors, value.survivors)))) return false;
    }
    for (PeerIndex p = 0; p < max_match_seats; ++p) {
        if (!(received_ & value.reported & mesh_seat(p))) continue;
        if (reports_[p].open != value.reports[p].open || reports_[p].relay_failed != value.reports[p].relay_failed
            || reports_[p].revision > value.reports[p].revision) return false;
        if (value.rule == MeshLossRule::all_open && p < sender && all_open(p, active_, reports_[p])) return false;
    }
    decision_ = value;
    acknowledged_ = mesh_seat(self_); state_ = MeshLossState::agreeing;
    return true;
}
bool MeshLossPolicy::acknowledge(PeerIndex sender, const MeshLossDecision& value, Time now) {
    now = monotonic(now);
    advance(now);
    if (state_ != MeshLossState::agreeing || !decision_ || decision_->decider != self_
        || issued_ || now >= deadline_ || sender >= max_match_seats || !(decision_->reported & mesh_seat(sender))) return false;
    if (value != *decision_) { state_ = MeshLossState::leave; return false; }
    acknowledged_ = static_cast<MeshSeats>(acknowledged_ | mesh_seat(sender));
    return true;
}
std::optional<MeshLossConfirmation> MeshLossPolicy::confirmation(Time now) {
    now = monotonic(now);
    advance(now);
    if (now > deadline_ + Time{10000} || state_ == MeshLossState::leave) return {};
    if (issued_) return issued_;
    if (state_ != MeshLossState::agreeing || !decision_ || decision_->decider != self_
        || (now < deadline_ && acknowledged_ != active_)
        || std::popcount(acknowledged_) <= std::popcount(active_) / 2) return {};
    const auto survivors = static_cast<MeshSeats>(decision_->survivors & acknowledged_);
    if (!enough_survivors(*decision_, survivors)) { state_ = MeshLossState::leave; return {}; }
    issued_ = MeshLossConfirmation{*decision_, acknowledged_, survivors};
    return issued_;
}
bool MeshLossPolicy::commit(PeerIndex sender, const MeshLossConfirmation& value, Time now) {
    now = monotonic(now);
    advance(now);
    const auto expected = selection(active_, value.proposal.reported, incident_, value.proposal.reports);
    if (!rejoin_allowed_ || value.proposal.active != active_ || value.proposal.incident != incident_
        || !expected || *expected != value.proposal || sender != value.proposal.decider
        || (value.pledges & ~value.proposal.reported) != 0 || !(value.pledges & mesh_seat(sender))
        || std::popcount(value.pledges) <= std::popcount(active_) / 2
        || value.survivors != (value.proposal.survivors & value.pledges)
        || (self_ == sender && (!issued_ || value != *issued_))) return false;
    if (!enough_survivors(value.proposal, value.survivors)) { state_ = MeshLossState::leave; return false; }
    // Exclusion is safe regardless of the receiver's stale/unpledged state.
    // It must never keep simulating a local survivor set after valid exclusion.
    if (!(value.survivors & mesh_seat(self_))) {
        confirmed_ = value; decision_ = value.proposal; decision_->survivors = value.survivors;
        decision_->outcome = MeshLossOutcome::departure; state_ = MeshLossState::leave;
        return true;
    }
    if (confirmed_) {
        if (value == *confirmed_) return state_ != MeshLossState::leave;
        state_ = MeshLossState::leave; return false;
    }
    if (state_ != MeshLossState::agreeing || !decision_ || value.proposal != *decision_
        || (self_ == sender && value.pledges != acknowledged_)) return false;
    confirmed_ = value;
    decision_->survivors = value.survivors;
    decision_->outcome = value.survivors == active_ ? MeshLossOutcome::restored : MeshLossOutcome::departure;
    state_ = decision_->outcome == MeshLossOutcome::restored ? MeshLossState::restored : MeshLossState::settling;
    return true;
}
void MeshLossPolicy::pair_lost(PeerIndex a, PeerIndex b) {
    if ((state_ == MeshLossState::settling || state_ == MeshLossState::agreeing) && decision_ && a != b
        && (decision_->survivors & mesh_seat(a)) && (decision_->survivors & mesh_seat(b))) state_ = MeshLossState::leave;
}
void MeshLossPolicy::settled() { if (state_ == MeshLossState::settling) state_ = MeshLossState::settled; }
std::optional<std::uint32_t> mesh_rtt_ms(MeshSeats active,
    const std::array<std::optional<MeshLinkReport>, max_match_seats>& reports) {
    if (!valid_active(active)) return {};
    std::uint32_t worst = 0;
    for (PeerIndex a = 0; a < max_match_seats; ++a) {
        if (!(active & mesh_seat(a))) continue;
        if (!reports[a]) return {};
        for (PeerIndex b = a + 1; b < max_match_seats; ++b) {
            if (!(active & mesh_seat(b))) continue;
            if (!reports[b]) return {};
            const auto rtt = a == 0 ? reports[0]->rtt_ms[b] : std::min(reports[a]->rtt_ms[b], reports[b]->rtt_ms[a]);
            worst = std::max(worst, rtt);
        }
    }
    return worst;
}
} // namespace eawr::net
