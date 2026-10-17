#include "pair_network.hpp"
#include "eawr/net/mesh_incident.hpp"

#include <array>
#include <bit>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Pair-loss agreement over the N-peer room mesh (docs/net-online-npeer.md, section 3; test plan rows "Pair-loss agreement"
// and "Mesh policy"). Every seat runs its own MeshIncident over a PairNetwork, so it sees only the reports its own open pairs
// deliver; nothing here is a global view except the assertions.
namespace {
namespace net = eawr::net;
using eawr::testing::PairNetwork;
using eawr::testing::PairTransport;
using Time = std::chrono::milliseconds;
using Kind = net::MeshIncidentResult::Kind;

int failures{};
void expect(bool value, const std::string& message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
constexpr net::MeshSeats seats_of(std::initializer_list<unsigned> seats) {
    net::MeshSeats mask{};
    for (const auto seat : seats) mask = static_cast<net::MeshSeats>(mask | (1U << seat));
    return mask;
}

net::MeshIncidentOptions options(std::chrono::milliseconds grace = std::chrono::seconds(2)) {
    net::MeshIncidentOptions value;
    value.repair_grace = grace;
    return value;
}

struct Rig {
    explicit Rig(unsigned count, net::MeshIncidentOptions settings = options(), bool settle = true)
        : network(count), auto_settle(settle), results(count), stopped(count), errors(count) {
        const auto all = static_cast<net::MeshSeats>((1U << count) - 1U);
        for (net::PeerIndex seat = 0; seat < count; ++seat) {
            transports.push_back(std::make_unique<PairTransport>(network, seat));
            incidents.push_back(std::make_unique<net::MeshIncident>(*transports.back(), all, settings));
        }
    }
    void step(std::uint64_t dt) {
        now += dt;
        network.round = now;
        for (net::PeerIndex seat = 0; seat < incidents.size(); ++seat) {
            if (stopped[seat]) continue;
            while (auto frame = transports[seat]->poll()) {
                if (frame->frame.empty() || frame->frame.front() < net::mesh_report_kind || frame->frame.front() > net::mesh_confirm_kind) continue;
                if (auto received = incidents[seat]->receive(*frame, Time(static_cast<std::int64_t>(now))); !received) fault(seat, received.error().code);
            }
            if (auto serviced = incidents[seat]->service(Time(static_cast<std::int64_t>(now))); !serviced) fault(seat, serviced.error().code);
            while (auto result = incidents[seat]->take_result()) {
                results[seat].push_back(*result);
                if (result->kind == Kind::leave) stopped[seat] = true;
                else if (result->kind == Kind::departure && auto_settle) incidents[seat]->settled();
            }
        }
    }
    // No test here may swallow a protocol or transport error: the session turns every service error into a failed match.
    void fault(net::PeerIndex seat, const std::string& code) {
        errors[seat] = code;
        expect(false, "seat " + std::to_string(seat) + " returned an error from the incident driver: " + code);
    }
    void run(std::uint64_t ms, std::uint64_t dt = 100) {
        const auto end = now + ms;
        while (now < end) step(dt);
    }
    // Run until a predicate holds or the budget is spent.
    template<class F> bool until(F predicate, std::uint64_t budget_ms, std::uint64_t dt = 100) {
        const auto end = now + budget_ms;
        while (now < end) {
            if (predicate()) return true;
            step(dt);
        }
        return predicate();
    }
    [[nodiscard]] std::size_t count(Kind kind) const {
        std::size_t total{};
        for (const auto& seat : results) for (const auto& result : seat) if (result.kind == kind) ++total;
        return total;
    }
    PairNetwork network;
    bool auto_settle{};
    std::uint64_t now{};
    std::vector<std::unique_ptr<PairTransport>> transports;
    std::vector<std::unique_ptr<net::MeshIncident>> incidents;
    std::vector<std::vector<net::MeshIncidentResult>> results;
    std::vector<bool> stopped;
    std::vector<std::string> errors;
};

std::uint64_t sent_by(const Rig& rig, net::PeerIndex seat, std::uint8_t kind) {
    const auto found = rig.network.sent.find({seat, kind});
    return found == rig.network.sent.end() ? 0 : found->second;
}

net::MeshLinkReport sample_report() {
    net::MeshLinkReport report;
    report.incident = 7; report.revision = 9; report.open = seats_of({0, 2, 5}); report.relay_failed = seats_of({1, 8});
    for (std::size_t seat = 0; seat < report.rtt_ms.size(); ++seat) report.rtt_ms[seat] = static_cast<std::uint32_t>(10 * seat + 3);
    return report;
}
net::MeshLossDecision sample_decision() {
    net::MeshLossDecision decision;
    decision.incident = 3; decision.decider = 2; decision.active = seats_of({0, 1, 2, 3}); decision.reported = decision.active;
    decision.survivors = seats_of({0, 2, 3});
    decision.rule = net::MeshLossRule::all_open; decision.outcome = net::MeshLossOutcome::departure;
    for (net::PeerIndex seat = 0; seat < 4; ++seat) {
        auto& row = decision.reports[seat];
        row.incident = 3; row.revision = seat + 1;
        row.open = static_cast<net::MeshSeats>(decision.active & ~(1U << seat) & ~(seat == 1 ? 1U : seat == 0 ? 2U : 0U));
        row.relay_failed = static_cast<net::MeshSeats>(decision.active & ~(1U << seat) & ~row.open);
    }
    return decision;
}

void wire_roundtrip_and_refusals() {
    const auto report = sample_report();
    constexpr auto everyone = seats_of({0, 1, 2, 3, 4, 5, 6, 7, 8});
    auto frame = require(net::encode_mesh_report(report, 4321, everyone));
    expect(frame.size() == 63 && frame.front() == net::mesh_report_kind, "link report has a fixed 63-byte layout");
    const auto decoded = require(net::decode_mesh_report(frame));
    expect(decoded.report == report && decoded.elapsed_ms == 4321 && decoded.active == everyone,
        "link report roundtrips with the sender's elapsed time and active set");
    auto bad = frame; bad.pop_back();
    expect(!net::decode_mesh_report(bad), "truncated link report is refused");
    bad = frame; bad.push_back(0);
    expect(!net::decode_mesh_report(bad), "trailing link report bytes are refused");
    bad = frame; bad[17] = 0xff; bad[18] = 0xff; // open mask outside nine seats
    expect(!net::decode_mesh_report(bad), "open mask beyond nine seats is refused");
    auto overlap = report; overlap.open = seats_of({1}); overlap.relay_failed = seats_of({1});
    expect(!net::encode_mesh_report(overlap, 0, everyone), "a pair cannot be both open and failed");
    expect(!net::encode_mesh_report(report, 11 * 60 * 1000, everyone), "an implausible elapsed time is refused");
    expect(!net::encode_mesh_report(report, 0, seats_of({0, 1, 2})), "a row about a seat outside the sender's active set is refused");
    expect(!net::encode_mesh_report(report, 0, static_cast<net::MeshSeats>(everyone | 0x200)), "an active set beyond nine seats is refused");
    bad = frame; bad[21] = 0xff; bad[22] = 0xff; // active mask beyond nine seats
    expect(!net::decode_mesh_report(bad), "an active mask beyond nine seats is refused");
    bad = frame; bad[21] = 0x01; bad[22] = 0x00; // rows about seats the active mask lacks
    expect(!net::decode_mesh_report(bad), "a report whose rows leave its own active set is refused");
    expect(net::decode_mesh_report(frame).value().report.rtt_ms[8] == 83, "round trips survive the wire");

    const auto decision = sample_decision();
    auto proposal = require(net::encode_mesh_decision(net::mesh_proposal_kind, decision));
    expect(require(net::decode_mesh_decision(net::mesh_proposal_kind, proposal)) == decision, "proposal roundtrips");
    auto pledge = require(net::encode_mesh_decision(net::mesh_pledge_kind, decision));
    expect(pledge.front() == net::mesh_pledge_kind && require(net::decode_mesh_decision(net::mesh_pledge_kind, pledge)) == decision,
        "a pledge echoes the exact snapshot");
    expect(!net::decode_mesh_decision(net::mesh_pledge_kind, proposal), "a proposal is never read as a pledge");
    expect(!net::encode_mesh_decision(net::mesh_report_kind, decision), "only proposals and pledges carry a decision");
    bad = proposal; bad.pop_back();
    expect(!net::decode_mesh_decision(net::mesh_proposal_kind, bad), "truncated proposal is refused");
    bad = proposal; bad.push_back(1);
    expect(!net::decode_mesh_decision(net::mesh_proposal_kind, bad), "trailing proposal bytes are refused");
    bad = proposal; bad[1 + 8 + 1 + 6 + 1] = 2; // rule
    expect(!net::decode_mesh_decision(net::mesh_proposal_kind, bad), "an unknown rule is refused");
    bad = proposal; bad[1 + 8] = 9; // decider outside the nine seats
    expect(!net::decode_mesh_decision(net::mesh_proposal_kind, bad), "a decider beyond nine seats is refused");

    net::MeshLossConfirmation confirmation{decision, decision.active, decision.survivors};
    auto confirm = require(net::encode_mesh_confirmation(confirmation));
    expect(require(net::decode_mesh_confirmation(confirm)) == confirmation, "confirmation roundtrips");
    bad = confirm; bad.pop_back();
    expect(!net::decode_mesh_confirmation(bad), "truncated confirmation is refused");
}

void partitioned_reports() {
    // Seats a<b<c<d: pairs ab, ac, bc lost. d is the only all-open seat and decides; a never computes its own.
    Rig rig(4);
    rig.network.cut(0, 1); rig.network.cut(0, 2); rig.network.cut(1, 2);
    expect(rig.until([&] { return rig.count(Kind::departure) == 2 && rig.count(Kind::leave) == 2; }, 30'000),
        "partitioned reports end with two survivors and two seats that leave");
    for (const auto seat : {0U, 3U}) {
        expect(rig.results[seat].size() == 1 && rig.results[seat].front().kind == Kind::departure, "a survivor holds one departure result");
        const auto& result = rig.results[seat].front();
        expect(result.survivors == seats_of({0, 3}) && result.removed == seats_of({1, 2}) && result.incident == 1,
            "survivors {a, d}; b and c leave");
    }
    expect(rig.results[0].front().lost[1] == seats_of({0, 2}) && rig.results[0].front().lost[2] == seats_of({0, 1})
        && rig.results[0].front().lost[0] == seats_of({1, 2}) && rig.results[0].front().lost[3] == 0,
        "the result names the seats each one could not reach");
    expect(rig.results[0].front().survivors == rig.results[3].front().survivors && rig.results[0].front().removed == rig.results[3].front().removed
        && rig.results[0].front().lost == rig.results[3].front().lost, "a and d hold the same decision");
    for (const auto seat : {1U, 2U}) {
        expect(rig.results[seat].size() == 1 && rig.results[seat].front().kind == Kind::leave
            && rig.results[seat].front().code == net::mesh_errors::left, "a removed seat leaves with the pair-loss reason");
    }
    expect(sent_by(rig, 0, net::mesh_proposal_kind) == 0 && sent_by(rig, 1, net::mesh_proposal_kind) == 0
        && sent_by(rig, 2, net::mesh_proposal_kind) == 0 && sent_by(rig, 3, net::mesh_proposal_kind) >= 1,
        "only the all-open seat announces; a never computes its own decision");
    expect(sent_by(rig, 0, net::mesh_pledge_kind) >= 1 && sent_by(rig, 3, net::mesh_confirm_kind) >= 1, "a pledges to d and d freezes a confirmation");
    for (const auto& error : rig.errors) expect(error.empty(), "no seat reported a protocol error: " + error);
    expect(rig.incidents[0]->incident() == 2 && rig.incidents[3]->incident() == 2 && rig.incidents[0]->active() == seats_of({0, 3}),
        "the incident ID advances only after the agreed settlement, to the same value on both survivors");
}

void no_decider() {
    Rig rig(4);
    rig.network.cut(0, 1); rig.network.cut(2, 3);
    rig.run(40'000);
    expect(rig.count(Kind::leave) == 4 && rig.count(Kind::departure) == 0 && rig.count(Kind::restored) == 0,
        "pairs ab and cd lost: every seat leaves at the deadline");
    for (const auto& seat : rig.results) expect(seat.size() == 1 && seat.front().code == net::mesh_errors::no_majority, "typed 'nobody could decide' reason");
    for (net::PeerIndex seat = 0; seat < 4; ++seat) expect(sent_by(rig, seat, net::mesh_proposal_kind) == 0, "nobody announces without a decider");
}

void two_all_open_seats() {
    Rig rig(5);
    rig.network.cut(0, 1);
    rig.run(30'000);
    expect(sent_by(rig, 2, net::mesh_proposal_kind) >= 1 && sent_by(rig, 3, net::mesh_proposal_kind) == 0
        && sent_by(rig, 4, net::mesh_proposal_kind) == 0, "five seats, ab lost: only c announces and d/e defer");
    for (const net::PeerIndex seat : {0U, 2U, 3U, 4U}) {
        expect(rig.results[seat].size() == 1 && rig.results[seat].front().kind == Kind::departure
            && rig.results[seat].front().survivors == seats_of({0, 2, 3, 4}), "the higher seat of the lost pair leaves, everyone else continues");
    }
    expect(rig.results[1].size() == 1 && rig.results[1].front().kind == Kind::leave && rig.results[1].front().code == net::mesh_errors::left,
        "seat b leaves");
}

void forged_controls() {
    Rig rig(5);
    std::vector<std::uint8_t> legit;
    std::size_t pledges_to_forger{};
    rig.network.on_send = [&](net::PeerIndex from, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        if (from == 2 && to == 3 && frame.front() == net::mesh_proposal_kind && legit.empty()) legit.assign(frame.begin(), frame.end());
        if (frame.front() == net::mesh_pledge_kind && to == 4) ++pledges_to_forger;
    };
    rig.network.cut(0, 1);
    expect(rig.until([&] { return !legit.empty(); }, 20'000), "the lowest all-open seat announced");
    // d (seat 4) is not the lowest all-open seat: its own announcement and confirmation are refused.
    auto forged = legit;
    forged[1 + 8] = 4;
    const auto decision = require(net::decode_mesh_decision(net::mesh_proposal_kind, forged));
    for (net::PeerIndex to : {0U, 1U, 2U, 3U}) rig.network.deliver(4, to, forged);
    net::MeshLossConfirmation confirmation{decision, decision.active, decision.survivors};
    const auto confirm = require(net::encode_mesh_confirmation(confirmation));
    for (net::PeerIndex to : {0U, 1U, 2U, 3U}) rig.network.deliver(4, to, confirm);
    // A pledge from a seat that has no accepted proposal, and a frame for a different incident.
    auto later = legit;
    for (unsigned i = 0; i < 8; ++i) later[1 + i] = i == 0 ? 9 : 0;
    rig.network.deliver(3, 2, later);
    rig.run(30'000);
    expect(pledges_to_forger == 0, "a forged announcement from a seat that is not the lowest all-open seat is refused");
    for (const net::PeerIndex seat : {0U, 2U, 3U, 4U})
        expect(rig.results[seat].size() == 1 && rig.results[seat].front().kind == Kind::departure
            && rig.results[seat].front().survivors == seats_of({0, 2, 3, 4}), "forgeries change nothing: the legitimate decision settles");
    for (net::PeerIndex seat = 0; seat < 5; ++seat) expect(rig.errors[seat].empty(), "forged controls are ignored, not fatal: " + rig.errors[seat]);
}

void second_loss_fails_closed() {
    Rig rig(4, options(), false);
    rig.network.cut(0, 1); rig.network.cut(0, 2); rig.network.cut(1, 2);
    expect(rig.until([&] { return rig.count(Kind::departure) == 2; }, 30'000), "survivors got their departure results");
    expect(rig.incidents[0]->removal_confirmed(1) && rig.incidents[0]->removal_confirmed(2) && !rig.incidents[0]->removal_confirmed(3),
        "only the confirmed removals permit a pair-loss departure");
    rig.network.cut(0, 3);
    rig.run(5'000);
    expect(rig.results[0].size() == 2 && rig.results[0].back().kind == Kind::leave && rig.results[0].back().code == net::mesh_errors::overlap
        && rig.results[3].size() == 2 && rig.results[3].back().kind == Kind::leave && rig.results[3].back().code == net::mesh_errors::overlap,
        "a loss between survivors before the departures settle fails closed for everyone");
}

// Independent model of the selection rule: the lowest all-open seat decides; repeatedly drop the seat with the most lost
// pairs among those remaining (ties to the higher index) until the survivors form a clique.
net::MeshSeats model_survivors(unsigned count, const std::vector<std::pair<unsigned, unsigned>>& cuts) {
    std::array<net::MeshSeats, net::max_match_seats> lost{};
    for (const auto& [a, b] : cuts) { lost[a] = static_cast<net::MeshSeats>(lost[a] | (1U << b)); lost[b] = static_cast<net::MeshSeats>(lost[b] | (1U << a)); }
    auto survivors = static_cast<net::MeshSeats>((1U << count) - 1U);
    for (;;) {
        int most{};
        unsigned victim{};
        for (unsigned seat = 0; seat < count; ++seat) {
            if (!(survivors & (1U << seat))) continue;
            const int value = std::popcount(static_cast<net::MeshSeats>(lost[seat] & survivors));
            if (value > 0 && value >= most) { most = value; victim = seat; }
        }
        if (most == 0) return survivors;
        survivors &= static_cast<net::MeshSeats>(~(1U << victim));
    }
}

void check_loss_set(unsigned count, const std::vector<std::pair<unsigned, unsigned>>& cuts, std::string_view label) {
    Rig rig(count, options(std::chrono::milliseconds(0)));
    for (const auto& [a, b] : cuts) rig.network.cut(a, b);
    rig.run(9'000, 250);
    const auto expected = model_survivors(count, cuts);
    for (unsigned seat = 0; seat < count; ++seat) {
        const auto& results = rig.results[seat];
        const bool survives = (expected & (1U << seat)) != 0;
        if (results.size() != 1 || (survives && (results.front().kind != Kind::departure || results.front().survivors != expected))
            || (!survives && (results.front().kind != Kind::leave || results.front().code != net::mesh_errors::left))) {
            expect(false, std::string(label) + ": seat " + std::to_string(seat) + " disagrees with the selection rule");
            return;
        }
    }
}

void all_loss_sets_up_to_three_pairs_at_nine_seats() {
    std::vector<std::pair<unsigned, unsigned>> pairs;
    for (unsigned a = 0; a < 9; ++a) for (unsigned b = a + 1; b < 9; ++b) pairs.emplace_back(a, b);
    std::size_t scenarios{};
    const auto before = failures;
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        check_loss_set(9, {pairs[i]}, "one pair"); ++scenarios;
        for (std::size_t j = i + 1; j < pairs.size(); ++j) {
            check_loss_set(9, {pairs[i], pairs[j]}, "two pairs"); ++scenarios;
            for (std::size_t k = j + 1; k < pairs.size(); ++k) {
                check_loss_set(9, {pairs[i], pairs[j], pairs[k]}, "three pairs"); ++scenarios;
                if (failures - before > 5) return; // one report is enough
            }
        }
    }
    expect(scenarios == 36 + 630 + 7140, "every loss set up to three pairs was exercised");
}

void restoration_before_removal() {
    auto settings = options();
    // While the pair is cut the relay path is still being tried: neither end proves failure, so nobody can be removed.
    settings.relay_failed = [&](net::PeerIndex) { return false; };
    Rig rig(3, settings);
    rig.network.cut(0, 1);
    rig.run(3'000);
    expect(rig.count(Kind::departure) == 0 && rig.count(Kind::leave) == 0, "no removal while the repair is still being tried");
    rig.network.heal(0, 1);
    expect(rig.until([&] { return rig.count(Kind::restored) == 3; }, 30'000), "a repaired mesh is restored by the same agreement");
    for (net::PeerIndex seat = 0; seat < 3; ++seat)
        expect(rig.incidents[seat]->incident() == 2 && rig.incidents[seat]->state() == net::MeshLossState::settled && !rig.incidents[seat]->running(),
            "a restoration completes the incident and advances the agreed ID");
    // The next loss is a new incident with the next ID.
    rig.network.cut(1, 2);
    rig.run(1'500);
    expect(rig.incidents[0]->running() && rig.incidents[0]->incident() == 2, "a later loss starts a fresh incident with the next ID");
}

void incident_clock_follows_the_earliest_start() {
    PairNetwork network(3);
    PairTransport transport(network, 2);
    net::MeshIncident incident(transport, seats_of({0, 1, 2}), options());
    net::MeshLinkReport report;
    report.incident = 1; report.revision = 1; report.open = seats_of({2}); report.relay_failed = seats_of({1});
    const auto frame = require(net::encode_mesh_report(report, 6'000, seats_of({0, 1, 2})));
    require(incident.receive({0, frame}, Time(10'000)));
    // Started six seconds before this seat heard of it: the policy's 12 s deadline and 500 ms tolerance end at 16.5 s.
    std::optional<net::MeshIncidentResult> result;
    for (std::int64_t ms = 10'000; ms <= 23'000 && !result; ms += 50) {
        network.round = static_cast<std::uint64_t>(ms);
        require(incident.service(Time(ms)));
        result = incident.take_result();
        if (result) expect(ms >= 16'450 && ms <= 16'700, "the common incident clock starts at the earliest start heard: " + std::to_string(ms));
    }
    expect(result && result->kind == Kind::leave && result->code == net::mesh_errors::no_majority, "an incident nobody can decide ends in the typed leave");
}

void flapping_pair_is_held_lost() {
    for (const bool hold : {true, false}) {
        auto settings = options();
        settings.flap_limit = hold ? 3 : 0;
        Rig rig(3, settings);
        for (unsigned flap = 0; flap < 3; ++flap) {
            rig.network.cut(0, 1); rig.run(400);
            rig.network.heal(0, 1); rig.run(400);
        }
        expect(rig.incidents[0]->flapping() == (hold ? seats_of({1}) : 0) && rig.incidents[1]->flapping() == (hold ? seats_of({0}) : 0),
            "three losses within the window mark the pair at both ends");
        rig.run(40'000);
        if (hold) {
            expect(rig.count(Kind::departure) == 2 && rig.count(Kind::leave) == 1 && rig.count(Kind::restored) == 0
                && rig.results[0].front().survivors == seats_of({0, 2}), "a flapping pair is held lost: the higher seat leaves, no oscillation");
        } else {
            expect(rig.count(Kind::restored) == 3 && rig.count(Kind::departure) == 0, "without the hold-down the same pattern restores");
        }
    }
}

void decider_lost_mid_incident() {
    Rig rig(4);
    bool isolated{};
    unsigned pledges{};
    rig.network.on_send = [&](net::PeerIndex, net::PeerIndex to, std::span<const std::uint8_t> frame) {
        // The decider dies as the last of the three pledges is sent: all of them accepted its proposal, none gets a confirmation.
        if (!isolated && frame.front() == net::mesh_pledge_kind && to == 3 && ++pledges == 3) { isolated = true; rig.network.isolate(3); }
    };
    rig.network.cut(0, 1); rig.network.cut(0, 2); rig.network.cut(1, 2);
    rig.run(60'000);
    expect(isolated, "the decider was reached and then lost");
    expect(rig.count(Kind::departure) == 0 && rig.count(Kind::restored) == 0 && rig.count(Kind::leave) == 4,
        "a decider lost mid-incident ends the incident for everyone: nobody continues on a private survivor set");
    for (unsigned seat = 0; seat < 3; ++seat)
        expect(rig.results[seat].size() == 1 && rig.results[seat].front().code == net::mesh_errors::no_decision,
            "a pledged seat, victim or survivor, ends with 1781 and never with 'removed': seat " + std::to_string(seat));
    expect(rig.results[3].size() == 1, "the isolated decider ends too");
}

void half_open_pair_fails_closed() {
    // Seat a sees its pair to b closed and reports it failed; b still reads the pair open and its sends succeed but never
    // arrive. A remote claim never turns b's open link into a failed one (no mirror), so the pair is not lost at both ends:
    // nobody is removed on a's word alone.
    Rig rig(3);
    rig.network.half_open(0, 1);
    rig.run(60'000);
    expect(rig.count(Kind::departure) == 0 && rig.count(Kind::restored) == 0, "a half-open pair removes nobody");
    expect(rig.count(Kind::leave) == 3, "it fails closed at the agreed deadline for everyone");
    for (const auto& seat : rig.results)
        expect(seat.size() == 1 && seat.front().code == net::mesh_errors::no_majority, "with the typed 'nobody could decide' reason");
}

void failing_sends_on_an_open_link_are_a_pair_failure() {
    // Seat b reads the pair to a open but every send over it fails: that is b's own evidence that the pair is lost. It is not a
    // fatal error, it does not stop b's reports to the others and both ends then agree the pair is lost.
    Rig rig(3);
    rig.network.cut(0, 1, 0, 100'000'000);
    rig.run(60'000);
    expect(rig.count(Kind::departure) == 2 && rig.count(Kind::leave) == 1, "the pair is lost at both ends: one seat leaves, two agree its departure");
    for (const auto& error : rig.errors) expect(error.empty(), "no seat reported an error: " + error);
    std::size_t removed{};
    for (net::PeerIndex seat = 0; seat < 3; ++seat)
        for (const auto& result : rig.results[seat])
            if (result.kind == Kind::leave) { ++removed; expect(result.code == net::mesh_errors::left, "the removed seat leaves with 1778: " + result.code); }
    expect(removed == 1, "exactly one seat was removed");
}

void staggered_restart_still_converges() {
    // Seat 3 leaves by an ordinary departure while the lost pair 0-1 is being agreed. The seats learn of it at different moments
    // (2 first, then 0, then 1): each restarts its policy over {0, 1, 2}, keeps counting its report revision and the seats still
    // agree one removal instead of giving up.
    auto dead = std::make_shared<bool>(false);
    auto settings = options(std::chrono::seconds(20));
    settings.relay_failed = [dead](net::PeerIndex) { return *dead; };
    Rig rig(4, settings);
    rig.network.cut(0, 1);
    rig.run(3'000);
    *dead = true; // the pair's rows move from "closed" to "relay failed": revision 2
    rig.run(1'000);
    rig.stopped[3] = true;
    rig.incidents[2]->set_active(seats_of({0, 1, 2}));
    rig.run(700);
    rig.incidents[0]->set_active(seats_of({0, 1, 2}));
    rig.run(700);
    rig.incidents[1]->set_active(seats_of({0, 1, 2}));
    rig.run(30'000);
    for (net::PeerIndex seat = 0; seat < 3; ++seat)
        for (const auto& result : rig.results[seat])
            expect(result.code != net::mesh_errors::no_majority && result.code != net::mesh_errors::no_decision,
                "no seat gives up after a staggered restart: " + result.code);
    expect(rig.count(Kind::leave) == 1 && rig.count(Kind::departure) == 2, "one of the lost pair is removed and the others agree it");
}

void sets_that_differ_neither_begin_nor_feed() {
    // Seat 2 already excluded seat 3 (an ordinary departure); seat 1 has not. Seat 1's report over four seats must neither start
    // an incident at seat 2 nor reach its policy; the same report over three seats must.
    PairNetwork network(4);
    PairTransport transport(network, 2);
    net::MeshIncident incident(transport, seats_of({0, 1, 2}), options());
    net::MeshLinkReport report;
    report.incident = 1; report.revision = 1; report.open = seats_of({0, 2}); report.relay_failed = seats_of({3});
    require(incident.receive({1, require(net::encode_mesh_report(report, 0, seats_of({0, 1, 2, 3})))}, Time(1'000)));
    expect(!incident.running(), "a report over a different active set does not begin an incident");
    report.relay_failed = 0;
    require(incident.receive({1, require(net::encode_mesh_report(report, 0, seats_of({0, 1, 2})))}, Time(1'000)));
    expect(incident.running(), "a report over the same active set begins it");
}

void a_running_seat_feeds_only_matching_sets() {
    // Seat 0 (the lowest all-open seat, so the decider) runs an incident over {0, 1, 2}. A report from seat 2 over a different
    // active set must not reach its policy: without it the decider has no complete picture and proposes nothing in the window;
    // the same report over the seat's own set completes the picture at once.
    const auto proposals_after_feeding = [&](net::MeshSeats seat2_set) {
        Rig rig(3);
        net::MeshIncident& incident = *rig.incidents[0];
        net::MeshLinkReport first; first.incident = 1; first.revision = 1; first.open = seats_of({0, 2});
        net::MeshLinkReport second; second.incident = 1; second.revision = 1; second.open = seats_of({0, 1});
        require(incident.receive({1, require(net::encode_mesh_report(first, 0, seats_of({0, 1, 2})))}, Time(1'000)));
        expect(incident.running(), "a matching report started the incident");
        require(incident.receive({2, require(net::encode_mesh_report(second, 0, seat2_set))}, Time(1'000)));
        for (std::int64_t ms = 1'000; ms <= 6'000; ms += 100) {
            rig.network.round = static_cast<std::uint64_t>(ms);
            require(incident.service(Time(ms)));
            if (incident.take_result()) return std::uint64_t{1'000'000};
        }
        return sent_by(rig, 0, net::mesh_proposal_kind);
    };
    expect(proposals_after_feeding(seats_of({0, 1, 2})) > 0, "a report over the seat's own set completes the picture: the decider proposes");
    expect(proposals_after_feeding(seats_of({0, 1, 2, 3})) == 0, "a report over another set does not reach a running policy");
}

void malformed_and_unrelated_controls() {
    PairNetwork network(3);
    PairTransport transport(network, 1);
    net::MeshIncident incident(transport, seats_of({0, 1, 2}), options());
    auto frame = require(net::encode_mesh_report(sample_report(), 0, seats_of({0, 1, 2, 3, 4, 5, 6, 7, 8})));
    auto truncated = frame; truncated.pop_back();
    const auto refused = incident.receive({0, truncated}, Time(0));
    expect(!refused && refused.error().code == net::mesh_errors::malformed, "a truncated link report is a typed malformed-control refusal");
    expect(!incident.receive({0, {90, 1, 2}}, Time(0)), "an unknown control kind is refused");
    expect(static_cast<bool>(incident.receive({7, frame}, Time(0))), "a report from a seat outside the roster is ignored");
    expect(incident.receive({0, frame}, Time(0)) && !incident.running(), "a report for a later incident does not start this one");
    auto proposal = require(net::encode_mesh_decision(net::mesh_proposal_kind, sample_decision()));
    expect(incident.receive({0, proposal}, Time(0)) && !incident.running(), "a proposal for another incident or roster is ignored");
    expect(incident.owns_closed_link(0) && incident.owns_closed_link(2) && !incident.left(), "three active seats leave closed links to the incident");
    incident.set_active(seats_of({0, 1}));
    expect(!incident.owns_closed_link(0), "with two seats left the ordinary departure scan owns closed links");
}

void steady_reports_feed_the_host() {
    for (const unsigned liar : {0U, 1U, 2U}) { // 0: none, 1: claims a huge RTT everywhere, 2: claims none
        std::array<std::array<std::uint32_t, 4>, 4> rtt{};
        const std::uint32_t table[4][4] = {{0, 40, 80, 60}, {40, 0, 100, 120}, {80, 100, 0, 90}, {60, 120, 90, 0}};
        for (unsigned a = 0; a < 4; ++a) for (unsigned b = 0; b < 4; ++b) rtt[a][b] = table[a][b];
        auto settings = options();
        settings.rtt_ms = {};
        Rig rig(4, settings);
        // Per-seat measurement functions: seat 2 is the liar.
        std::vector<std::unique_ptr<net::MeshIncident>> replacement;
        const auto all = seats_of({0, 1, 2, 3});
        for (net::PeerIndex seat = 0; seat < 4; ++seat) {
            auto own = settings;
            own.rtt_ms = [&rtt, seat, liar](net::PeerIndex remote) -> std::optional<std::uint32_t> {
                if (seat == 2 && liar == 1) return 5000;
                if (seat == 2 && liar == 2) return 1;
                return rtt[seat][remote];
            };
            rig.incidents[seat] = std::make_unique<net::MeshIncident>(*rig.transports[seat], all, own);
        }
        rig.run(3'000);
        const auto reports = rig.incidents[0]->rtt_reports(Time(static_cast<std::int64_t>(rig.now)));
        const auto measured = net::mesh_rtt_ms(all, reports);
        // The host's own pairs by its own measurement; every other pair is the smaller of its two ends' reports.
        std::uint32_t expected{};
        for (unsigned a = 0; a < 4; ++a) for (unsigned b = a + 1; b < 4; ++b) {
            const auto ra = (a == 2 && liar == 1) ? 5000U : (a == 2 && liar == 2) ? 1U : rtt[a][b];
            const auto rb = (b == 2 && liar == 1) ? 5000U : (b == 2 && liar == 2) ? 1U : rtt[b][a];
            expected = std::max(expected, a == 0 ? rtt[0][b] : std::min(ra, rb));
        }
        expect(measured && *measured == expected, "per-pair minimum at the host, liar mode " + std::to_string(liar) + ": "
            + (measured ? std::to_string(*measured) : std::string("unknown")) + " vs " + std::to_string(expected));
        if (liar == 1) expect(measured && *measured == 120, "one seat inflating its numbers cannot push everyone's delay up");
    }
    // Pairs without a measurement read as the sentinel the session defers on, never as zero.
    Rig rig(3, options());
    rig.run(2'500);
    const auto all = seats_of({0, 1, 2});
    const auto fresh = net::mesh_rtt_ms(all, rig.incidents[0]->rtt_reports(Time(static_cast<std::int64_t>(rig.now))));
    expect(fresh && *fresh >= net::mesh_rtt_unmeasured, "unmeasured pairs read as the unmeasured sentinel (no link statistics in the fake)");
    // A seat that stops reporting makes the host defer (unknown), not assume its last numbers forever.
    rig.stopped[2] = true;
    rig.run(11'000);
    expect(!net::mesh_rtt_ms(all, rig.incidents[0]->rtt_reports(Time(static_cast<std::int64_t>(rig.now)))), "a silent seat's stale report no longer counts");
}

// A seeded schedule of delays and duplicates on the control frames only; streams stay ordered, as TCP and GNS reliable
// streams are. Whatever the schedule, the continuing seats hold one identical survivor set and everyone else leaves.
void randomized_schedules() {
    for (std::uint32_t seed = 1; seed <= 150; ++seed) {
        std::mt19937 random(seed);
        const unsigned count = 4 + random() % 3;
        std::vector<std::pair<unsigned, unsigned>> cuts;
        const unsigned wanted = 1 + random() % 4;
        while (cuts.size() < wanted) {
            const unsigned a = random() % count, b = random() % count;
            if (a == b) continue;
            const std::pair<unsigned, unsigned> pair{std::min(a, b), std::max(a, b)};
            if (std::find(cuts.begin(), cuts.end(), pair) == cuts.end()) cuts.push_back(pair);
        }
        Rig rig(count, options(std::chrono::milliseconds(random() % 3000)));
        rig.network.fault = [&](net::PeerIndex, net::PeerIndex, std::span<const std::uint8_t> frame) {
            eawr::testing::FakeDelivery action;
            if (frame.front() >= net::mesh_report_kind && frame.front() <= net::mesh_confirm_kind) {
                action.ordered_delay = random() % 700;
                action.duplicate = random() % 5 == 0;
                action.duplicate_delay = random() % 300;
            }
            return action;
        };
        for (const auto& [a, b] : cuts) rig.network.cut(a, b);
        rig.run(60'000, 100);
        std::optional<net::MeshSeats> agreed;
        bool consistent = true;
        for (unsigned seat = 0; seat < count; ++seat) {
            if (rig.results[seat].size() > 1) consistent = false;
            if (rig.results[seat].empty()) { consistent = false; continue; }
            const auto& result = rig.results[seat].front();
            if (result.kind == Kind::departure) {
                if (agreed && *agreed != result.survivors) consistent = false;
                agreed = result.survivors;
                if (!(result.survivors & (1U << seat))) consistent = false;
                for (const auto& [a, b] : cuts)
                    if ((result.survivors & (1U << a)) && (result.survivors & (1U << b))) consistent = false; // survivors are a clique
            } else if (result.kind != Kind::leave) consistent = false;
        }
        if (agreed) for (unsigned seat = 0; seat < count; ++seat)
            if (!(*agreed & (1U << seat)) && (rig.results[seat].empty() || rig.results[seat].front().kind != Kind::leave)) consistent = false;
        for (unsigned seat = 0; seat < count; ++seat) if (!rig.errors[seat].empty()) consistent = false;
        if (!consistent) { expect(false, "randomized schedule " + std::to_string(seed) + " broke agreement"); return; }
    }
}
} // namespace

int main() {
    try {
        wire_roundtrip_and_refusals();
        partitioned_reports();
        no_decider();
        two_all_open_seats();
        forged_controls();
        second_loss_fails_closed();
        all_loss_sets_up_to_three_pairs_at_nine_seats();
        restoration_before_removal();
        incident_clock_follows_the_earliest_start();
        flapping_pair_is_held_lost();
        decider_lost_mid_incident();
        half_open_pair_fails_closed();
        failing_sends_on_an_open_link_are_a_pair_failure();
        staggered_restart_still_converges();
        sets_that_differ_neither_begin_nor_feed();
        a_running_seat_feeds_only_matching_sets();
        malformed_and_unrelated_controls();
        steady_reports_feed_the_host();
        randomized_schedules();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) return 1;
    std::cout << "mesh incident contracts passed\n";
    return 0;
}
