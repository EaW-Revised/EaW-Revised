#include "eawr/net/gns_room.hpp"
#include "eawr/net/mesh_policy.hpp"

#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using namespace eawr::net;
using namespace std::chrono_literals;
using Pair = std::pair<PeerIndex, PeerIndex>;
using Reports = std::array<MeshLinkReport, max_match_seats>;
void expect(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
void require(eawr::core::Result<void> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
}
MeshSeats seats(std::uint32_t count) { return static_cast<MeshSeats>((1U << count) - 1U); }
Reports make_reports(std::uint32_t count, const std::vector<Pair>& lost, bool relay_failed = true) {
    Reports result{};
    for (PeerIndex p = 0; p < count; ++p) {
        result[p].incident = 7; result[p].revision = 1;
        result[p].open = static_cast<MeshSeats>(seats(count) & ~mesh_seat(p));
    }
    for (const auto& [a, b] : lost) {
        result[a].open &= static_cast<MeshSeats>(~mesh_seat(b));
        result[b].open &= static_cast<MeshSeats>(~mesh_seat(a));
        if (relay_failed) {
            result[a].relay_failed = static_cast<MeshSeats>(result[a].relay_failed | mesh_seat(b));
            result[b].relay_failed = static_cast<MeshSeats>(result[b].relay_failed | mesh_seat(a));
        }
    }
    return result;
}
std::vector<MeshLossPolicy> local_views(std::uint32_t count, const Reports& reports) {
    std::vector<MeshLossPolicy> policies;
    for (PeerIndex self = 0; self < count; ++self) {
        policies.emplace_back(self, seats(count), 7, 0ms, 40s);
        // No omniscient receive side: only its own row and rows arriving over
        // this seat's still-open pairs. Lost-edge reports are never forwarded.
        for (PeerIndex from = 0; from < count; ++from)
            if (from == self || ((reports[self].open & mesh_seat(from)) && (reports[from].open & mesh_seat(self))))
                expect(policies.back().report(from, reports[from], 30s), "Deliver report over open pair");
    }
    return policies;
}
std::optional<MeshLossDecision> announce(std::vector<MeshLossPolicy>& policies) {
    std::optional<MeshLossDecision> decision;
    for (auto& policy : policies) {
        const auto candidate = policy.decide(32s);
        if (candidate) { expect(!decision, "Exactly one decider announces"); decision = candidate; }
    }
    return decision;
}
MeshSeats agreed_case(std::uint32_t count, const std::vector<Pair>& lost) {
    const auto reports = make_reports(count, lost);
    auto policies = local_views(count, reports);
    const auto decision = announce(policies);
    PeerIndex expected_decider = count;
    for (PeerIndex p = 0; p < count; ++p)
        if (reports[p].open == (seats(count) & ~mesh_seat(p))) { expected_decider = p; break; }
    if (expected_decider == count) {
        expect(!decision, "No all-open seat cannot decide");
        for (auto& policy : policies) { policy.advance(40500ms); expect(policy.state() == MeshLossState::leave, "No decider times out closed within receiver tolerance"); }
        return 0;
    }
    expect(decision.has_value() && decision->decider == expected_decider, "Lowest all-open seat selected");
    expect(decision->rejoin_allowed, "Removed seats can rejoin");
    // Independent adjacency-table implementation of fewer-reachable selection.
    MeshSeats expected = seats(count);
    while (true) {
        PeerIndex victim = count;
        std::size_t fewest = max_match_seats;
        for (PeerIndex p = 0; p < count; ++p) {
            if (!(expected & mesh_seat(p))) continue;
            std::size_t reachable = 0, possible = 0;
            for (PeerIndex q = 0; q < count; ++q) {
                if (q == p || !(expected & mesh_seat(q))) continue;
                ++possible;
                if ((reports[p].open & mesh_seat(q)) && (reports[q].open & mesh_seat(p))) ++reachable;
            }
            if (reachable < possible && reachable <= fewest) { victim = p; fewest = reachable; }
        }
        if (victim == count) break;
        expected &= static_cast<MeshSeats>(~mesh_seat(victim));
    }
    expect(decision->survivors == expected, "Fewer reachable peers, later-seat tie");
    for (PeerIndex p = 0; p < count; ++p) {
        expect(policies[p].accept(decision->decider, *decision, 32s), "Every seat accepts identical announcement");
        expect(policies[p].decision() == decision, "Every continuing seat holds identical decision");
        expect(policies[p].state() == MeshLossState::agreeing, "No removal or settlement before echoes");
        expect(policies[decision->decider].acknowledge(p, *decision, 32s), "Every active seat pledges the same proposal");
    }
    const auto confirmed = policies[decision->decider].confirmation(32s);
    expect(confirmed && confirmed->proposal == *decision && confirmed->survivors == expected
        && confirmed->pledges == seats(count), "Decider confirms only after all active echoes before cutoff");
    for (PeerIndex p = 0; p < count; ++p) {
        expect(policies[p].commit(decision->decider, *confirmed, 32s), "Every seat commits the same confirmation");
        const bool survives = (expected & mesh_seat(p)) != 0;
        const auto continuing = expected == seats(count) ? MeshLossState::restored : MeshLossState::settling;
        expect(policies[p].state() == (survives ? continuing : MeshLossState::leave), "Removed seats leave; restoration is agreed");
        if (survives) {
            for (PeerIndex q = p + 1; q < count; ++q)
                if (expected & mesh_seat(q)) expect((reports[p].open & mesh_seat(q)) != 0, "Survivors form a clique");
            if (continuing == MeshLossState::settling) {
                policies[p].settled(); expect(policies[p].state() == MeshLossState::settled, "Agreement is settled only explicitly");
            }
        }
    }
    return expected;
}
void loss_agreement() {
    expect(agreed_case(4, {{0, 1}, {0, 2}, {1, 2}}) == (mesh_seat(0) | mesh_seat(3)), "Partitioned ab/ac/bc keeps a,d");
    expect(agreed_case(4, {{0, 1}, {2, 3}}) == 0, "No decider ends match");
    expect(agreed_case(5, {{0, 1}}) == (seats(5) & ~mesh_seat(1)), "Multiple all-open seats defer to c");
    expect(agreed_case(5, {{0, 1}, {0, 2}}) == (seats(5) & ~mesh_seat(0)), "Fewer-reachable host removed without election");
    auto policies = local_views(5, make_reports(5, {{0, 1}}));
    const auto decision = announce(policies);
    expect(decision.has_value(), "Forge fixture has decision");
    auto forged = *decision; forged.decider = 3;
    expect(!policies[0].accept(3, forged, 32s), "Higher all-open announcer refused");
    forged = *decision; forged.decider = 1;
    expect(!policies[0].accept(1, forged, 32s), "Non-all-open announcer refused");
    forged = *decision; forged.survivors = seats(5);
    expect(!policies[0].accept(2, forged, 32s), "Forged survivor set refused");
    forged = *decision; forged.incident = 8;
    expect(!policies[0].accept(2, forged, 32s), "Stale incident refused");
    expect(!policies[0].accept(4, *decision, 32s), "Authenticated sender must match decision");
    for (PeerIndex p = 0; p < 5; ++p) {
        expect(policies[p].accept(2, *decision, 32s), "Real announcement after forged one");
        expect(policies[2].acknowledge(p, *decision, 32s), "Real proposal echoed");
    }
    const auto confirmation = policies[2].confirmation(32s);
    expect(confirmation && confirmation->proposal == *decision, "Proposal confirmed after all echoes");
    for (PeerIndex p = 0; p < 5; ++p) {
        expect(policies[p].commit(2, *confirmation, 32s), "Confirmed proposal commits before departure");
        policies[p].pair_lost(0, 1); // intended closure to a removed seat
        if (p != 1) expect(policies[p].state() == MeshLossState::settling, "Expected removed-pair closures do not overlap");
        policies[p].pair_lost(0, 3);
        expect(policies[p].state() == MeshLossState::leave, "Second loss during settle fails closed");
    }
    auto unrelayed = make_reports(4, {{1, 2}}, false);
    auto repair = local_views(4, unrelayed);
    expect(!announce(repair), "No removal before relay attempt fails");
    unrelayed[1].relay_failed = mesh_seat(2); unrelayed[1].revision = 2;
    expect(repair[0].report(1, unrelayed[1], 32s), "One end tried relay");
    for (PeerIndex p = 0; p < 4; ++p) expect(repair[0].report(p, unrelayed[p], 33s), "All rows fresh in one-sided relay case");
    expect(!repair[0].decide(34s), "Both relay endpoints must fail");
    unrelayed[2].relay_failed = mesh_seat(1); unrelayed[2].revision = 2;
    for (PeerIndex p = 0; p < 4; ++p) expect(repair[0].report(p, unrelayed[p], 35s), "Refresh relay-failure reports");
    expect(!repair[0].decide(36s), "Two-second stable reports required");
    expect(repair[0].decide(37s).has_value(), "Relay failure allows selection and rejoin outcome");
    const auto early_rows = make_reports(4, {{1, 2}});
    MeshLossPolicy early(0, seats(4), 7, 0ms, 40s);
    for (PeerIndex p = 0; p < 4; ++p) expect(early.report(p, early_rows[p], 0ms), "Initial removal topology");
    for (PeerIndex p = 0; p < 4; ++p) expect(early.report(p, early_rows[p], 28s), "Stable removal rows fresh before grace end");
    expect(!early.decide(29s), "Only the repair grace prevents early removal");
    auto stale = local_views(4, make_reports(4, {{1, 2}}));
    expect(!stale[0].decide(34s), "Old reports cannot elect a decider");
    const auto invalid = MeshLossPolicy(9, seats(4), 7, 0ms, 40s);
    expect(invalid.state() == MeshLossState::leave, "Invalid local seat fails closed");
}
void exhaustive_losses() {
    std::vector<Pair> pairs;
    for (PeerIndex a = 0; a < 9; ++a) for (PeerIndex b = a + 1; b < 9; ++b) pairs.emplace_back(a, b);
    std::size_t cases = 0;
    for (std::size_t a = 0; a < pairs.size(); ++a) {
        agreed_case(9, {pairs[a]}); ++cases;
        for (std::size_t b = a + 1; b < pairs.size(); ++b) {
            agreed_case(9, {pairs[a], pairs[b]}); ++cases;
            for (std::size_t c = b + 1; c < pairs.size(); ++c) { agreed_case(9, {pairs[a], pairs[b], pairs[c]}); ++cases; }
        }
    }
    expect(cases == 7806, "All nine-seat loss sets of one through three pairs");
    expect(agreed_case(9, {}) == seats(9), "Zero losses produce agreed restoration with no removal");
    std::cout << "nine-seat loss sets: " << cases + 1 << '\n';
}
void review_regressions() {
    const auto rows = make_reports(4, {{0, 1}});
    MeshLossPolicy decider(2, seats(4), 7, 0ms, 40s);
    expect(decider.report(2, rows[2], 0ms), "Initial all-open local truth");
    for (const PeerIndex p : {0U, 1U, 3U}) expect(decider.report(p, rows[p], 28s), "Fresh remote reports, unchanged local row");
    const auto proposal = decider.decide(30s);
    expect(proposal.has_value(), "Decider's own row does not expire at 3 seconds");
    expect(!decider.confirmation(30s) && !decider.commit(2,
        MeshLossConfirmation{*proposal, mesh_seat(2), proposal->survivors}, 30s), "No effective decision before all echoes");
    MeshLossPolicy earlier(0, seats(4), 7, 0ms, 40s);
    expect(earlier.report(0, rows[0], 0ms), "Receiver own truth can also be old");
    expect(earlier.report(2, rows[2], 28s) && earlier.report(3, rows[3], 28s), "Receiver open-pair reports");
    expect(earlier.accept(2, *proposal, 30s), "Receiver's unchanged own row does not expire");
    MeshLossPolicy detected_late(0, seats(4), 7, 5s, 40s);
    expect(detected_late.report(0, rows[0], 5s) && detected_late.report(2, rows[2], 28s)
        && detected_late.report(3, rows[3], 28s), "Receiver detects loss five seconds late");
    expect(detected_late.state(29s) == MeshLossState::repairing, "Receiver still within agreed repair grace");
    expect(detected_late.accept(2, *proposal, 29s), "Content-valid proposal accepted while receiver still repairing");
    MeshLossPolicy late_decider(2, seats(4), 7, 10s, 40s);
    expect(late_decider.report(2, rows[2], 10s), "Late decider starts at 10 seconds");
    for (const PeerIndex p : {0U, 1U, 3U}) expect(late_decider.report(p, rows[p], 27500ms), "Initial stable remote reports for late decider");
    for (const PeerIndex p : {0U, 1U, 3U}) expect(late_decider.report(p, rows[p], 28s), "Stable remote reports fresh at agreed grace end");
    const auto late_proposal = late_decider.decide(30s);
    expect(late_proposal.has_value(), "Start at 10 seconds still decides at agreed 30-second grace end, before 40-second cutoff");
    MeshLossPolicy early_receiver(0, seats(4), 7, 0ms, 40s);
    expect(early_receiver.report(0, rows[0], 0ms) && early_receiver.report(2, rows[2], 28s)
        && early_receiver.report(3, rows[3], 28s), "Earlier receiver shares the design's 40-second cutoff");
    expect(early_receiver.accept(2, *late_proposal, 30s), "Detection skew cannot create a private grace or deadline");
    MeshLossPolicy expired(0, seats(4), 7, 0ms, 40s);
    expect(expired.report(0, rows[0], 0ms) && expired.report(2, rows[2], 39s), "Expired receiver has otherwise valid content");
    expect(expired.state(40500ms) == MeshLossState::leave && !expired.accept(2, *late_proposal, 40500ms), "state(now) advances deadline plus tolerance; late proposal refused");
    expect(decider.state(40s) == MeshLossState::leave && !decider.confirmation(40s), "Missing acknowledgements fail closed at agreed deadline");

    // Review's one-link-latency race: d still holds ab/ac lost, while a/c
    // have repaired ac and c becomes the lower all-open candidate.
    const auto old = make_reports(4, {{0, 1}, {0, 2}});
    auto repaired = rows; repaired[0].revision = 2; repaired[2].revision = 2;
    std::vector<MeshLossPolicy> race;
    for (PeerIndex p = 0; p < 4; ++p) {
        race.emplace_back(p, seats(4), 7, 0ms, 40s);
        expect(race[p].report(p, old[p], 0ms), "Initial local truth in stale-view race");
    }
    for (PeerIndex from = 0; from < 3; ++from) expect(race[3].report(from, old[from], 27500ms), "d receives old content");
    for (PeerIndex from = 0; from < 3; ++from) expect(race[3].report(from, old[from], 29800ms), "d refreshes old periodic content");
    for (PeerIndex self = 0; self < 3; ++self) for (PeerIndex from = 0; from < 4; ++from)
        if (self == from || (repaired[self].open & mesh_seat(from)))
            expect(race[self].report(from, repaired[from], 29900ms), "Other seats receive current repaired topology");
    const auto d = race[3].decide(30s);
    expect(d && d->survivors == (mesh_seat(1) | mesh_seat(2) | mesh_seat(3)), "Stale d proposes b,c,d");
    for (PeerIndex p = 0; p < 3; ++p) expect(!race[p].accept(3, *d, 30s), "Current seats refuse stale d proposal");
    expect(!race[3].confirmation(30s), "Stale proposal cannot become effective");
    const auto c = race[2].decide(31900ms);
    expect(c && c->survivors == (mesh_seat(0) | mesh_seat(2) | mesh_seat(3)), "Current c proposes a,c,d");
    expect(race[0].accept(2, *c, 31900ms) && race[1].accept(2, *c, 31900ms), "Fresh topology proposal accepted by a,b");
    expect(!race[3].accept(2, *c, 31900ms), "d cannot pledge a second candidate in the same incident");
    expect(race[2].acknowledge(0, *c, 31900ms) && race[2].acknowledge(1, *c, 31900ms), "c collects matching a,b echoes");
    expect(!race[2].confirmation(31900ms) && !race[2].commit(2,
        MeshLossConfirmation{*c, static_cast<MeshSeats>(seats(4) & ~mesh_seat(3)), static_cast<MeshSeats>(c->survivors & ~mesh_seat(3))},
        31900ms), "Missing d pledge prevents early competing commit");
    expect(!race[3].acknowledge(2, *c, 31900ms) && race[3].state() == MeshLossState::leave, "Mismatched echo fails closed");
    expect(race[3].state(40s) == MeshLossState::leave, "Stale minority candidate cannot survive cutoff");
    const auto majority = race[2].confirmation(40s);
    expect(majority && majority->survivors == (mesh_seat(0) | mesh_seat(2)), "Current majority confirms at cutoff without stale d");
    for (PeerIndex p = 0; p < 3; ++p) {
        expect(race[p].commit(2, *majority, 40200ms), "Current majority's confirmation reaches every pledged seat");
        expect(race[p].decision()->survivors == majority->survivors, "Every continuing seat holds identical effective survivors");
    }
    expect(!race[2].acknowledge(3, *c, 40200ms), "Late pledge cannot revise a committed majority result");

    auto rtt_only = rows; rtt_only[0].rtt_ms[2] = 999;
    MeshLossPolicy stable(2, seats(4), 7, 0ms, 40s);
    for (PeerIndex p = 0; p < 4; ++p) expect(stable.report(p, rows[p], 28s), "Stable topology before RTT change");
    expect(stable.report(0, rtt_only[0], 29s), "Same topology revision permits a new RTT sample");
    const auto topology_only = stable.decide(30s);
    expect(topology_only && topology_only->reports[0].rtt_ms[2] == 0, "RTT samples are excluded from the agreed topology snapshot");
}
void missing_pledges() {
    auto policies = local_views(4, make_reports(4, {{0, 1}}));
    const auto proposal = announce(policies);
    expect(proposal && proposal->decider == 2, "Missing-pledge fixture selects c");
    for (PeerIndex p = 0; p < 3; ++p) {
        expect(policies[p].accept(2, *proposal, 32s), "a,b,c pledge; d does not");
        expect(policies[2].acknowledge(p, *proposal, 32s), "c receives original-set majority");
    }
    expect(!policies[2].confirmation(39s), "Missing pledge waits until agreed cutoff");
    expect(policies[3].state(40500ms) == MeshLossState::leave, "Unpledged seat leaves within fixed cutoff tolerance");
    const auto confirmed = policies[2].confirmation(40s);
    expect(confirmed && confirmed->pledges == (seats(4) & ~mesh_seat(3))
        && confirmed->survivors == (mesh_seat(0) | mesh_seat(2)), "Cutoff fixes survivor set excluding missing pledger");
    expect(!policies[3].accept(2, *proposal, 40500ms) && !policies[2].acknowledge(3, *proposal, 40s), "Expired proposal and post-cutoff pledge refused");
    auto forged = *confirmed; forged.pledges = static_cast<MeshSeats>(mesh_seat(0) | mesh_seat(2));
    expect(!policies[0].commit(2, forged, 40200ms), "Minority cannot certify a survivor set");
    forged = *confirmed; forged.pledges = static_cast<MeshSeats>(forged.pledges | mesh_seat(8));
    expect(!policies[0].commit(2, forged, 40200ms), "Out-of-roster pledge cannot certify a survivor set");
    for (PeerIndex p = 0; p < 3; ++p) {
        expect(policies[p].commit(2, *confirmed, 40200ms), "Confirmation delivered within bounded post-cutoff window");
        expect(policies[p].decision()->survivors == confirmed->survivors, "Every continuing seat sees same missing-pledge removal");
    }
    expect(policies[0].state() == MeshLossState::settling && policies[2].state() == MeshLossState::settling
        && policies[1].state() == MeshLossState::leave, "Remaining a,c settle; removed b and missing d leave");
    expect(policies[2].confirmation(41s) == confirmed && !policies[2].acknowledge(3, *proposal, 41s), "Late pledge cannot change the confirmed decision");
    auto absent = local_views(4, make_reports(4, {{0, 1}}));
    const auto unconfirmed = announce(absent);
    expect(unconfirmed && !absent[2].confirmation(40s) && absent[2].state() == MeshLossState::leave, "No majority ends incident instead of deadlocking");
    auto no_delivery = local_views(4, make_reports(4, {{0, 1}}));
    const auto delivery_proposal = announce(no_delivery);
    expect(delivery_proposal && no_delivery[0].accept(2, *delivery_proposal, 32s), "Receiver pledges but confirmation is lost");
    expect(no_delivery[0].state(50500ms) == MeshLossState::leave, "Missing confirmation cannot wait beyond bounded delivery window plus tolerance");
}
void agreed_restoration() {
    auto policies = local_views(4, make_reports(4, {{1, 2}}, false));
    auto healed = make_reports(4, {});
    for (auto& row : healed) row.revision = 2;
    for (auto& policy : policies) {
        for (PeerIndex p = 0; p < 4; ++p) expect(policy.report(p, healed[p], 31s), "Repaired mesh delivers all current rows");
        expect(policy.state(33s) == MeshLossState::awaiting_decision, "All-open local view is never terminal restoration");
    }
    const auto restored = policies[0].decide(33s);
    expect(restored && restored->outcome == MeshLossOutcome::restored && restored->survivors == seats(4), "Lowest seat proposes no removal");
    for (PeerIndex p = 0; p < 4; ++p) {
        expect(policies[p].accept(0, *restored, 33s) && policies[p].state() == MeshLossState::agreeing, "No-removal also needs a pledge");
        expect(policies[0].acknowledge(p, *restored, 33s), "No-removal pledges collected");
    }
    const auto confirmed = policies[0].confirmation(33s);
    expect(confirmed.has_value(), "All-active no-removal confirmation");
    for (auto& policy : policies) {
        expect(policy.commit(0, *confirmed, 33s) && policy.state() == MeshLossState::restored, "Only confirmation permits restoration");
        expect(policy.state(60s) == MeshLossState::restored, "Agreed restoration completes the incident without timeout");
    }
    expect(!policies[0].confirmation(60s), "Confirmation resend ends after bounded delivery window");

    auto before_loss = make_reports(3, {});
    MeshLossPolicy delayed(2, seats(3), 7, 0ms, 40s);
    expect(delayed.report(2, before_loss[2], 0ms) && delayed.report(0, before_loss[0], 28s)
        && delayed.report(1, before_loss[1], 28s), "c receives delayed pre-loss all-open rows");
    expect(delayed.state(30s) == MeshLossState::awaiting_decision && !delayed.decide(30s), "Stale healthy view cannot continue on its own");
    expect(delayed.state(40500ms) == MeshLossState::leave, "Without agreement even all-open local view leaves within cutoff tolerance");
    MeshLossPolicy updated(2, seats(3), 7, 0ms, 40s);
    expect(updated.report(2, before_loss[2], 0ms) && updated.report(0, before_loss[0], 28s)
        && updated.report(1, before_loss[1], 28s), "Another stale healthy observer");
    expect(!updated.decide(30s), "Healthy observer does not invent restoration");
    auto lost = make_reports(3, {{0, 1}}); lost[0].revision = 2; lost[1].revision = 2;
    expect(updated.report(0, lost[0], 32s) && updated.report(1, lost[1], 32s), "Delayed loss rows remain processable");
    const auto removal = updated.decide(34s);
    expect(removal && removal->outcome == MeshLossOutcome::departure, "Stale healthy observer can later decide actual loss");

    auto old_views = local_views(4, make_reports(4, {{0, 1}, {0, 2}, {1, 2}}));
    const auto old_proposal = announce(old_views);
    expect(old_proposal && old_proposal->decider == 3, "d proposes from delayed loss snapshot");
    for (const PeerIndex p : {0U, 1U, 3U}) {
        expect(old_views[p].accept(3, *old_proposal, 32s), "a,b,d pledge one snapshot");
        expect(old_views[3].acknowledge(p, *old_proposal, 32s), "d collects original-set majority");
    }
    const auto excludes_c = old_views[3].confirmation(40s);
    expect(excludes_c && !(excludes_c->survivors & mesh_seat(2)), "Missing c is excluded by verified majority confirmation");
    MeshLossPolicy unpledged(2, seats(4), 7, 0ms, 40s);
    const auto healthy = make_reports(4, {});
    for (PeerIndex p = 0; p < 4; ++p) expect(unpledged.report(p, healthy[p], 38s), "c still has an all-open local view");
    expect(unpledged.commit(3, *excludes_c, 40s) && unpledged.state() == MeshLossState::leave, "Verified exclusion stops a seat that never accepted the proposal");
    // Defensive certificate handling: even a completed local restore cannot
    // override an authenticated, structurally verified exclusion certificate.
    // Two honest quorums cannot produce these certificates in one execution.
    expect(policies[2].commit(3, *excludes_c, 40s) && policies[2].state() == MeshLossState::leave,
        "A restored seat excluded by verified confirmation leaves");
}
std::vector<Pair> cuts(std::uint32_t count, const std::vector<MeshSeats>& groups) {
    std::vector<Pair> lost;
    for (PeerIndex a = 0; a < count; ++a) for (PeerIndex b = a + 1; b < count; ++b) {
        bool connected = false;
        for (const auto group : groups) if ((group & mesh_seat(a)) && (group & mesh_seat(b))) connected = true;
        if (!connected) lost.emplace_back(a, b);
    }
    return lost;
}
void partition_case(std::uint32_t count, const std::vector<MeshSeats>& groups, MeshSeats expected) {
    const auto reports = make_reports(count, cuts(count, groups));
    auto policies = local_views(count, reports);
    const auto proposal = announce(policies);
    if (expected == 0) {
        expect(!proposal, "No majority component can propose");
        for (auto& policy : policies) expect(policy.state(40500ms) == MeshLossState::leave, "Minority components leave within cutoff tolerance");
        return;
    }
    PeerIndex decider = 0;
    while (!(expected & mesh_seat(decider))) ++decider;
    expect(proposal && proposal->decider == decider && proposal->survivors == expected
        && proposal->reported == expected && proposal->rule == MeshLossRule::majority_clique, "Largest majority clique and its lowest seat decide");
    for (PeerIndex p = 0; p < count; ++p) {
        if (!(expected & mesh_seat(p))) {
            expect(proposal->reports[p] == MeshLinkReport{}, "No invented or forwarded report crosses cut");
            continue;
        }
        expect(p == decider || (reports[p].open & mesh_seat(decider)), "Proposal delivered only on open pair");
        expect(policies[p].accept(decider, *proposal, 32s), "Majority clique receives same proposal");
        expect(policies[decider].acknowledge(p, *proposal, 32s), "Majority clique's direct pledges reach decider");
    }
    expect(!policies[decider].confirmation(39s), "Partition waits until common cutoff for unreachable pledgers");
    const auto confirmed = policies[decider].confirmation(40s);
    expect(confirmed && confirmed->survivors == expected, "Original-set majority confirms one clique");
    for (PeerIndex p = 0; p < count; ++p) {
        if (expected & mesh_seat(p)) {
            expect(policies[p].commit(decider, *confirmed, 40200ms) && policies[p].state() == MeshLossState::settling,
                "Majority group commits while stalled for departure settle");
            expect(policies[p].decision()->survivors == expected, "Every continuing seat holds identical survivor set");
        } else {
            expect(policies[p].state(40500ms) == MeshLossState::leave && policies[p].rejoin_allowed(), "Unreachable outside seat leaves with rejoin permitted");
            // A repaired, authenticated pair may later deliver the certificate;
            // no live proposal or pledge was sent across the original cut.
            expect(policies[p].commit(decider, *confirmed, 41s) && policies[p].state() == MeshLossState::leave,
                "Delayed authenticated exclusion cannot revive the departed outsider");
        }
    }
}
void majority_cliques() {
    partition_case(4, {seats(3), mesh_seat(3)}, seats(3));
    partition_case(5, {seats(3), mesh_seat(3), mesh_seat(4)}, seats(3));
    partition_case(4, {seats(2), mesh_seat(2), mesh_seat(3)}, 0);
    partition_case(8, {seats(4), static_cast<MeshSeats>(seats(8) & ~seats(4))}, 0);
    partition_case(9, {seats(5), static_cast<MeshSeats>(seats(9) & ~seats(5))}, seats(5));
    const auto later_five = static_cast<MeshSeats>(seats(9) & ~seats(4));
    partition_case(9, {seats(4), later_five}, later_five);

    const auto minority = static_cast<MeshSeats>(seats(9) & ~seats(5));
    const auto split_rows = make_reports(9, cuts(9, {seats(5), minority}));
    auto split = local_views(9, split_rows);
    MeshLossDecision forged;
    forged.incident = 7; forged.decider = 5; forged.active = seats(9);
    forged.reported = minority; forged.survivors = minority; forged.rule = MeshLossRule::majority_clique;
    for (PeerIndex p = 5; p < 9; ++p) forged.reports[p] = split_rows[p];
    for (PeerIndex p = 5; p < 9; ++p) {
        expect(!split[p].decide(32s) && !split[p].confirmation(32s), "Minority side never proposes or confirms");
        expect(!split[p].commit(5, MeshLossConfirmation{forged, minority, minority}, 32s)
            && !split[p].decision() && split[p].state(40500ms) == MeshLossState::leave, "Minority side cannot commit a fabricated minority outcome");
    }

    const MeshSeats earlier = static_cast<MeshSeats>(mesh_seat(0) | mesh_seat(3) | mesh_seat(4) | mesh_seat(5));
    const MeshSeats later = static_cast<MeshSeats>(mesh_seat(1) | mesh_seat(2) | mesh_seat(4) | mesh_seat(5));
    auto policies = local_views(7, make_reports(7, cuts(7, {earlier, later, mesh_seat(6)})));
    const auto a = policies[0].decide(32s), b = policies[1].decide(32s);
    expect(a && b && a->survivors == earlier && b->survivors == later, "Overlapping partial views can nominate two equal-size cliques");
    expect(!policies[4].accept(1, *b, 32s) && !policies[5].accept(1, *b, 32s), "Shared seats reject a worse tie choice they know");
    for (const PeerIndex p : {0U, 3U, 4U, 5U}) {
        expect(policies[p].accept(0, *a, 32s), "Lexicographically earlier clique pledges");
        expect(policies[0].acknowledge(p, *a, 32s), "Earlier clique gets original-set majority");
    }
    expect(policies[2].accept(1, *b, 32s) && policies[1].acknowledge(2, *b, 32s), "Later clique gets only its private seats");
    expect(!policies[1].confirmation(40s) && policies[1].state() == MeshLossState::leave, "Later tie candidate cannot confirm without majority");
    const auto confirmed = policies[0].confirmation(40s);
    expect(confirmed && confirmed->survivors == earlier, "Equal-size clique ties follow lowest seat order, not numeric mask value");
    for (const PeerIndex p : {0U, 3U, 4U, 5U}) expect(policies[p].commit(0, *confirmed, 40200ms), "Winning clique commits one survivor set");
}
void minimum_survivors() {
    auto policies = local_views(5, make_reports(5, {{1, 2}, {1, 3}, {1, 4}, {2, 3}, {3, 4}}));
    const auto proposal = announce(policies);
    const auto base = static_cast<MeshSeats>(mesh_seat(0) | mesh_seat(2) | mesh_seat(4));
    expect(proposal && proposal->decider == 0 && proposal->survivors == base, "One-seat-result fixture starts with three survivors");
    const auto votes = static_cast<MeshSeats>(mesh_seat(0) | mesh_seat(1) | mesh_seat(3));
    for (const PeerIndex p : {0U, 1U, 3U}) {
        expect(policies[p].accept(0, *proposal, 32s) && policies[0].acknowledge(p, *proposal, 32s), "Original-set majority votes but only one would survive");
    }
    expect(!policies[0].confirmation(40s) && policies[0].state() == MeshLossState::leave, "One-seat confirmation is never emitted");
    expect(!policies[1].commit(0, MeshLossConfirmation{*proposal, votes, mesh_seat(0)}, 40s)
        && policies[1].state() == MeshLossState::leave, "One-seat commit fails closed");
}
void architect_regressions() {
    const auto group = seats(5);
    const auto outside = static_cast<MeshSeats>(seats(9) & ~group);
    const auto split_rows = make_reports(9, cuts(9, {group, outside}));
    auto split = local_views(9, split_rows);
    const auto proposal = announce(split);
    expect(proposal.has_value(), "Heal-during-pledge majority proposal");
    for (PeerIndex p = 0; p < 5; ++p) {
        expect(split[p].accept(0, *proposal, 32s) && split[0].acknowledge(p, *proposal, 32s), "Five-seat clique pledges");
    }
    auto healed_one = split_rows[1]; healed_one.revision = 2;
    healed_one.open = static_cast<MeshSeats>(healed_one.open | mesh_seat(5));
    healed_one.relay_failed &= static_cast<MeshSeats>(~mesh_seat(5));
    for (PeerIndex p = 0; p < 5; ++p) expect(split[p].report(1, healed_one, 35s)
        && split[p].state() == MeshLossState::agreeing, "Reopening an outside pair cannot abort pledged clique");
    const auto confirmed_split = split[0].confirmation(40s);
    expect(confirmed_split && confirmed_split->survivors == group, "Healed partition edge leaves frozen clique selection unchanged");
    for (PeerIndex p = 0; p < 5; ++p) expect(split[p].commit(0, *confirmed_split, 40200ms), "Five seats still settle after heal");

    const auto rows = make_reports(4, {{0, 1}});
    auto normal = local_views(4, rows);
    const auto p = announce(normal);
    expect(p.has_value(), "All-open decider with one missing pledge");
    for (PeerIndex seat = 0; seat < 3; ++seat)
        expect(normal[seat].accept(2, *p, 32s) && normal[2].acknowledge(seat, *p, 32s), "Original-set majority pledged");
    auto repaired_zero = rows[0]; repaired_zero.revision = 2;
    repaired_zero.open = static_cast<MeshSeats>(repaired_zero.open | mesh_seat(1)); repaired_zero.relay_failed = 0;
    for (const PeerIndex seat : {0U, 2U, 3U}) expect(normal[seat].report(0, repaired_zero, 36s), "Lost pair heals after pledge");
    const auto first = normal[2].confirmation(40s);
    expect(first && first->survivors == (mesh_seat(0) | mesh_seat(2)), "Heal cannot change pledged removal or missing-pledger exclusion");
    expect(!normal[2].acknowledge(3, *p, 39980ms), "Backdated echo cannot expand issued confirmation");
    expect(normal[2].confirmation(40100ms) == first, "First issued confirmation is immutable");
    expect(normal[0].commit(2, *first, 39900ms), "Receiver 100 ms behind accepts frozen majority certificate");
    expect(normal[2].commit(2, *first, 40100ms), "Decider can still commit first issued certificate");
    expect(normal[2].confirmation(49s) == first && !normal[2].confirmation(51s), "Confirmation retransmits within a bounded window only");

    auto loss = local_views(4, rows);
    const auto loss_proposal = announce(loss);
    expect(loss_proposal.has_value(), "Additional-loss fixture");
    for (PeerIndex seat = 0; seat < 4; ++seat) expect(loss[seat].accept(2, *loss_proposal, 32s), "Survivors agree before second loss");
    auto dropped = rows[0]; dropped.revision = 2; dropped.open &= static_cast<MeshSeats>(~mesh_seat(3));
    for (PeerIndex seat = 0; seat < 4; ++seat) expect(loss[seat].report(0, dropped, 35s)
        && loss[seat].state() == MeshLossState::leave, "A survivor pair loss still aborts agreement");

    auto current = split_rows; current[4].revision = 2;
    std::vector<MeshLossPolicy> lagging;
    for (PeerIndex self = 0; self < 9; ++self) {
        lagging.emplace_back(self, seats(9), 7, 0ms, 40s);
        for (PeerIndex from = 0; from < 9; ++from) {
            if (from != self && !(current[self].open & mesh_seat(from))) continue;
            auto row = current[from];
            if (self == 3 && from == 4) { row.revision = 1; row.relay_failed &= static_cast<MeshSeats>(~mesh_seat(7)); }
            expect(lagging.back().report(from, row, 30s), "Lagging member's direct report delivery");
        }
    }
    const auto lag_proposal = lagging[0].decide(32s);
    expect(lag_proposal && !lagging[3].accept(0, *lag_proposal, 32s), "Lagging row initially refuses clique proposal");
    for (const PeerIndex seat : {0U, 1U, 2U, 4U}) {
        expect(lagging[seat].accept(0, *lag_proposal, 32s) && lagging[0].acknowledge(seat, *lag_proposal, 32s), "Only four initial pledges");
    }
    expect(lagging[3].report(4, current[4], 33s) && lagging[3].report(0, current[0], 33s), "Delayed row and sender heartbeat arrive");
    expect(lagging[0].decision() == lag_proposal && lagging[3].accept(0, *lagging[0].decision(), 34s), "Unchanged proposal re-delivered after view catches up");
    expect(lagging[0].acknowledge(3, *lag_proposal, 34s), "Echo only after successful later acceptance");
    const auto lag_confirmed = lagging[0].confirmation(40s);
    expect(lag_confirmed && lag_confirmed->survivors == group, "Resend preserves healthy exact-majority clique");
    for (PeerIndex seat = 0; seat < 5; ++seat) expect(lagging[seat].commit(0, *lag_confirmed, 40200ms), "Every caught-up clique member commits");

    std::vector<MeshLossPolicy> quick;
    auto healthy = make_reports(4, {});
    for (auto& row : healthy) row.revision = 2;
    // This fixture starts at the actual short-blip times, not local_views' 30 s.
    for (PeerIndex self = 0; self < 4; ++self) {
        quick.emplace_back(self, seats(4), 7, 0ms, 40s);
        for (PeerIndex from = 0; from < 4; ++from) expect(quick.back().report(from, healthy[from], 3s), "Short repair completes at three seconds");
    }
    const auto restore = quick[0].decide(5500ms);
    expect(restore && restore->outcome == MeshLossOutcome::restored, "Restoration eligible after quiet period without 30-second wait");
    for (PeerIndex seat = 0; seat < 4; ++seat) expect(quick[seat].accept(0, *restore, 5500ms)
        && quick[0].acknowledge(seat, *restore, 5500ms), "Short repair's no-removal pledges");
    const auto restored = quick[0].confirmation(6s);
    expect(restored.has_value(), "Early no-removal confirmation");
    for (auto& policy : quick) expect(policy.commit(0, *restored, 6s)
        && policy.state() == MeshLossState::restored, "Repaired match resumes by six seconds");
}
void formation() {
    for (const std::uint32_t count : {3U, 4U, 9U}) {
        MeshFormation mesh(count);
        expect(mesh.attach(0, 1), "Host attached");
        for (PeerIndex k = 1; k < count; ++k) {
            expect(mesh.attach(k, k + 1), "Incremental newcomer");
            expect(!mesh.complete(), "Start blocked until complete");
            for (PeerIndex j = 0; j < k; ++j) expect(mesh.pair_open(k, j), "New pair opens");
            expect(mesh.connected(k), "Newcomer is connected to every occupied seat");
        }
        expect(mesh.complete(), "Mesh complete at fixed capacity");
        expect(mesh.pair_failed(1, 2, {false, false}).action == MeshFormationAction::none && !mesh.complete(), "Direct-only loss clears readiness without regrant");
        expect(mesh.pair_failed(1, 2, {true, false}).action == MeshFormationAction::none, "One relay endpoint cannot regrant");
        const auto retry = mesh.pair_failed(1, 2, {true, true});
        expect(retry.action == MeshFormationAction::regrant && retry.seat == 2, "One regrant for admitted guest pair");
        expect(!mesh.complete(), "Loss clears ready");
        const auto refused = mesh.pair_failed(1, 2, {true, true});
        expect(refused.action == MeshFormationAction::refuse && refused.seat == 2 && refused.unreachable == 1, "Later admission refused on regrant failure");
        expect(mesh.attach(2, 100), "Released slot refilled");
        for (PeerIndex j = 0; j < count; ++j) if (j != 2) expect(mesh.pair_open(2, j), "Refill gets fresh pairs");
        expect(mesh.complete(), "Refilled room starts");
        mesh.vacate(1); expect(!mesh.complete(), "Leave or kick clears completeness");
        expect(mesh.attach(1, 101), "Kicked slot can be reused by admission");
        for (PeerIndex j = 0; j < count; ++j) if (j != 1) expect(mesh.pair_open(1, j), "Replacement pairs");
        expect(mesh.complete(), "Room ready after leave/kick replacement");
        mesh.vacate(0); expect(!mesh.complete() && !mesh.attach(0, 102), "Host loss closes staging");
        for (PeerIndex k = 1; k < count; ++k) for (PeerIndex j = 0; j < k; ++j) {
            MeshFormation partial(count);
            for (PeerIndex p = 0; p < k; ++p) {
                expect(partial.attach(p, p + 1), "Existing admission");
                for (PeerIndex q = 0; q < p; ++q) expect(partial.pair_open(p, q), "Existing mesh");
            }
            expect(partial.attach(k, k + 1), "Failing newcomer attached");
            expect(partial.pair_failed(k, j, {false, false}).action == MeshFormationAction::none
                && (partial.occupied() & mesh_seat(k)), "Direct-only newcomer failure retains the seat");
            expect(partial.pair_failed(k, j, {true, false}).action == MeshFormationAction::none
                && (partial.occupied() & mesh_seat(k)), "One-sided relay failure cannot refuse newcomer");
            const auto failure = partial.pair_failed(k, j, {true, true});
            expect(failure.action == MeshFormationAction::refuse && failure.seat == k && failure.unreachable == j
                && !(partial.occupied() & mesh_seat(k)), "Every failing newcomer pair refuses only newcomer");
        }
    }
    MeshFormation admission_order(3);
    expect(admission_order.attach(0, 1) && admission_order.attach(2, 2) && admission_order.attach(1, 3), "Admission order differs from slot order");
    expect(admission_order.pair_open(0, 1) && admission_order.pair_open(0, 2) && admission_order.pair_open(1, 2), "Out-of-order mesh complete");
    expect(admission_order.pair_failed(1, 2, {true, true}).action == MeshFormationAction::regrant, "Out-of-order pair regrant");
    expect(admission_order.pair_failed(1, 2, {true, true}).seat == 1, "Pre-Start refusal uses later admission, not higher slot");
    expect(mesh_newcomer_refusal_text("Player Blue").find("Player Blue") != std::string::npos, "Curated refusal names unreachable player");
    expect(!MeshFormation(10).attach(0, 1), "Capacity bound enforced");
    // A silent end of a failed pair counts as failed after the grace: five seconds, at most half the pair deadline.
    expect(mesh_report_grace(20s) == 5s && mesh_report_grace(8s) == 4s && mesh_report_grace(100ms) == 50ms, "Report grace bounds");
    const auto silent = [](MeshRelayFailure reported, std::chrono::milliseconds waited, std::chrono::milliseconds deadline = 20s) {
        return mesh_silent_end_failed(reported, waited, deadline);
    };
    expect(silent({true, false}, 4999ms).b_failed == false && silent({true, false}, 4999ms).a_failed, "Within the grace the silent end is not failed");
    expect(silent({true, false}, 5s).b_failed && silent({false, true}, 5s).a_failed, "After the grace the silent end counts as failed");
    expect(silent({true, false}, 4s, 8s).b_failed && !silent({true, false}, 3999ms, 8s).b_failed, "The grace is half a short pair deadline");
    expect(!silent({false, false}, 60s).a_failed && !silent({false, false}, 60s).b_failed, "A pair nobody reported on is never inferred failed");
    expect(silent({true, true}, 0ms).a_failed && silent({true, true}, 0ms).b_failed, "Both reported stays failed");
    MeshFormation silence(3);
    expect(silence.attach(0, 1) && silence.attach(1, 2) && silence.attach(2, 3), "Silence formation");
    expect(silence.pair_open(0, 1) && silence.pair_open(0, 2) && silence.pair_open(1, 2), "Silence formation complete");
    expect(silence.pair_failed(1, 2, silent({true, false}, 1s)).action == MeshFormationAction::none, "One report inside the grace waits");
    expect(silence.pair_failed(1, 2, silent({true, false}, 6s)).action == MeshFormationAction::regrant, "A silent end after the grace regrants once");
    expect(silence.pair_failed(1, 2, silent({true, false}, 6s)).action == MeshFormationAction::refuse, "Then the silent pair refuses the newcomer");
}
InProcessGnsRoom::Clock::time_point time_now{};
InProcessGnsRoom::Clock::time_point now() { return time_now; }
void room_fixture() {
    time_now = {};
    InProcessGnsRoom room(3, now, 4, 2, 60s);
    const auto host = require(room.attach(0, "host"));
    const auto a = require(room.attach(1, "a"));
    expect(!host->lock() && !a->release(0), "Only complete host membership can lock");
    const auto b = require(room.attach(2, "b", GnsTurnCredentials{"turn:test", "user-b", "password", 60s}));
    expect(b->turn_credentials()->username == "user-b", "Per-member credentials exposed");
    expect(!room.attach(2, "replacement") && !room.attach(9, "outside") && !room.attach(1, "host"), "Attachment bounds and uniqueness");
    std::vector<std::uint8_t> blob{1, 2}; require(a->send(2, blob)); blob[0] = 99;
    require(host->send(2, std::vector<std::uint8_t>{3, 4}));
    expect(!a->send(2, std::vector<std::uint8_t>{5}), "Bounded aggregate member queue");
    const auto first = require(b->poll());
    expect(first && first->from_slot == 1 && first->from_identity == "a" && first->bytes[0] == 1, "Authoritative source and owned blob");
    expect(require(b->poll())->from_slot == 0 && !require(b->poll()), "Ordered queue drains");
    expect(!a->send(1, blob) && !a->send(3, blob) && !a->send(2, {}) && !a->send(2, std::vector<std::uint8_t>(65537)), "Invalid signalling refused");
    require(a->send(2, blob)); require(host->release(1));
    expect(!a->status() && !require(b->poll()), "Release purges old source messages and attachment");
    const auto replacement = require(room.attach(1, "refill"));
    expect(!a->send(2, blob), "Old endpoint cannot impersonate refill");
    require(host->kick(1)); expect(!room.attach(1, "refill"), "Kick denies identity for room lifetime");
    const auto refill = require(room.attach(1, "second"));
    expect(!replacement->status(), "Kicked endpoint invalidated");
    require(host->lock()); expect(!room.attach(1, "third") && !host->release(1), "Locked room refuses new admissions and releases");
    require(room.disconnect(0));
    expect(b->roster().state == GnsRoomState::locked && b->roster().slots[0].state == GnsRoomSlotState::held, "Locked host loss preserves room");
    require(refill->send(2, blob)); expect(require(b->poll()).has_value(), "Survivor signalling continues without host");
    expect(!room.attach(0, "forged-host"), "Held slot requires same identity");
    const auto returned = require(room.attach(0, "host"));
    expect(!host->status() && returned->status().has_value(), "Returning host has fresh attachment");
    time_now += 60s; expect(!b->status() && !returned->send(2, blob), "Room lease expires");
    InProcessGnsRoom staging(2, now);
    const auto staged = require(staging.attach(0, "staging"));
    const auto guest = require(staging.attach(1, "guest"));
    require(staged->leave()); expect(!guest->status(), "Pre-Start host departure closes room");
    expect(!InProcessGnsRoom(10, now).attach(0, "invalid"), "Invalid room capacity");
    std::shared_ptr<GnsRoomEndpoint> retained;
    { InProcessGnsRoom temporary(2, now); retained = require(temporary.attach(0, "retained")); }
    expect(retained->status().has_value(), "Endpoint retains fixture state");
}
void threaded_signals() {
    InProcessGnsRoom room(3, now, 1024, 256);
    const auto host = require(room.attach(0, "thread-host"));
    const auto a = require(room.attach(1, "thread-a"));
    const auto b = require(room.attach(2, "thread-b"));
    std::atomic<unsigned> failures{};
    const auto producer = [&](const std::shared_ptr<GnsRoomEndpoint>& endpoint) {
        for (std::uint8_t i = 0; i < 100; ++i) if (!endpoint->send(2, std::vector<std::uint8_t>{i})) ++failures;
    };
    std::thread first(producer, host), second(producer, a);
    first.join(); second.join();
    expect(failures == 0, "Concurrent callbacks fit bounded inbox");
    std::array<unsigned, 2> counts{};
    while (const auto signal = require(b->poll())) {
        expect(signal->from_slot < 2 && signal->bytes[0] == counts[signal->from_slot], "Per-source callback order");
        ++counts[signal->from_slot];
    }
    expect(counts[0] == 100 && counts[1] == 100, "All concurrent signals retained");
}
void timing() {
    std::array<std::optional<MeshLinkReport>, max_match_seats> reports{};
    for (PeerIndex p = 0; p < 4; ++p) reports[p] = MeshLinkReport{};
    reports[0]->rtt_ms[1] = 20; reports[0]->rtt_ms[2] = 30; reports[0]->rtt_ms[3] = 40;
    reports[1]->rtt_ms[0] = 9999; reports[1]->rtt_ms[2] = 9999; reports[2]->rtt_ms[1] = 60;
    reports[2]->rtt_ms[3] = 50; reports[3]->rtt_ms[2] = 55;
    expect(mesh_rtt_ms(seats(4), reports) == 60U, "Lying high reporter cannot raise other pairs; host uses own RTT");
    reports[1]->rtt_ms[2] = 1;
    expect(mesh_rtt_ms(seats(4), reports) == 50U, "Per-pair minimum, then worst pair");
    reports[3].reset(); expect(!mesh_rtt_ms(seats(4), reports), "Missing reports remain unknown");
    expect(mesh_timing_frozen(seats(4), LinkState::closed) && mesh_timing_frozen(seats(4), LinkState::connecting)
        && mesh_timing_frozen(static_cast<MeshSeats>(seats(4) & ~mesh_seat(0)), LinkState::open), "Held or departed host freezes proposals");
    expect(!mesh_timing_frozen(seats(4), LinkState::open), "Returning host resumes proposal authority");
}
}
int main() {
    try {
        formation(); loss_agreement(); exhaustive_losses(); review_regressions(); missing_pledges(); agreed_restoration();
        majority_cliques(); minimum_survivors(); architect_regressions(); room_fixture(); threaded_signals(); timing();
        std::cout << "mesh policy and room fixture contracts passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
