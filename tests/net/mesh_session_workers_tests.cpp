// The pair-loss hand-off over the real tactical simulation, with 1, 2, 4 and 8 sim workers: pairs cut among four seats (the
// all-open decider), a crashed seat among four and three lost pairs among five (the majority-clique decider). The agreed
// cutoff ticks and the survivors' final state hashes must not depend on the worker count (networking stays outside the sim;
// docs/net-online-npeer.md, section 6).

#include "../support/check.hpp"
#include "../support/result_require.hpp"
#include "pair_network.hpp"
#include "eawr/net/tactical_adapter.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace net = eawr::net;
namespace sim = eawr::sim;
namespace tactical = sim::tactical;
using eawr::testing::PairNetwork;
using eawr::testing::PairTransport;

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool value, const std::string& message) {
    if (!value) local_checker.check_before_print(value, message, std::cerr, "FAIL: ");
}
using eawr::testing::result_code_colon::require;

struct Scenario {
    std::string name;
    unsigned peers{};
    std::vector<std::pair<unsigned, unsigned>> cuts;
    std::vector<unsigned> survivors;
    std::vector<std::pair<unsigned, std::string_view>> leavers; // seat, exact code
};

struct Run {
    std::string hash;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> cutoffs; // tick, seat of every agreed pair-loss departure at the first survivor
    std::vector<std::string> failure;
};

Run play(unsigned workers, const Scenario& scenario) {
    const unsigned peers = scenario.peers;
    PairNetwork network(peers);
    eawr::platform::ThreadWorkerAdapter executor(workers);
    tactical::TacticalSetup initial;
    initial.seed = 716;
    initial.departure_policy.emplace();
    std::vector<tactical::SeatBinding> roster;
    for (unsigned peer = 0; peer < peers; ++peer) {
        const auto id = peer + 1;
        initial.players.push_back({id, id, id, tactical::player_flag_commandable});
        initial.departure_policy->humans.push_back(id);
        roster.push_back({sim::SeatId{id}, id, true, false});
        tactical::UnitState unit;
        unit.entity_id = id; unit.type_id = 1; unit.owner = id;
        unit.rotation = sim::math::identity_quat();
        initial.units.push_back(unit);
    }
    net::SessionOptions options;
    options.final_tick_count = 90;
    options.completeness_timeout_ms = 100'000;
    net::MeshIncidentOptions mesh;
    mesh.repair_grace = std::chrono::seconds(2);
    mesh.rounds = 1; // these cases end at the first deadline
    options.mesh = mesh;
    std::vector<std::unique_ptr<PairTransport>> transports;
    std::vector<net::TacticalLockstepSession> sessions;
    for (net::PeerIndex peer = 0; peer < peers; ++peer) {
        transports.push_back(std::make_unique<PairTransport>(network, peer));
        sessions.push_back(require(net::TacticalLockstepSession::create(initial, {}, *transports.back(), executor, roster, options, nullptr)));
    }
    Run run;
    run.failure.resize(peers);
    std::vector<bool> stopped(peers);
    const auto first = scenario.survivors.front();
    const auto done = [&] {
        for (const auto seat : scenario.survivors) if (sessions[seat].simulation().completed_tick() != 90) return false;
        for (const auto& [seat, code] : scenario.leavers) { static_cast<void>(code); if (!stopped[seat]) return false; }
        return true;
    };
    for (std::uint64_t round = 1; round < 150'000; ++round) {
        network.round = round;
        if (round == 30) for (const auto& [a, b] : scenario.cuts) network.cut(a, b);
        if (round == 500 && scenario.name == "repaired pair") network.heal(0, 1);
        for (net::PeerIndex peer = 0; peer < peers; ++peer) {
            if (stopped[peer]) continue;
            auto pumped = sessions[peer].pump(round);
            if (!pumped) { run.failure[peer] = pumped.error().code; stopped[peer] = true; continue; }
            if (peer == first) for (const auto& quit : pumped.value().quits) run.cutoffs.emplace_back(quit.tick, quit.seat.value);
        }
        if (done()) break;
    }
    expect(done(), scenario.name + ": workers " + std::to_string(workers) + ": the survivors finish the match and the removed seats end");
    for (const auto seat : scenario.survivors) {
        expect(run.failure[seat].empty(), scenario.name + ": a survivor never fails: " + run.failure[seat]);
        expect(sessions[seat].simulation().state_sha256() == sessions[first].simulation().state_sha256(), scenario.name + ": survivors hold equal state");
        expect(sessions[seat].departure_outcomes().size() == scenario.leavers.size(), scenario.name + ": every removal reached the policy");
    }
    for (const auto& [seat, code] : scenario.leavers)
        expect(run.failure[seat] == code, scenario.name + ": seat " + std::to_string(seat) + " ends with " + std::string(code) + ", got " + run.failure[seat]);
    run.hash = sessions[first].simulation().state_sha256();
    return run;
}
} // namespace

int main() {
    try {
        const std::vector<Scenario> scenarios{
            {"repaired pair", 4, {{0, 1}}, {0, 1, 2, 3}, {}},
            {"all-open decider", 4, {{0, 1}, {0, 2}, {1, 2}}, {0, 3}, {{1, net::mesh_errors::left}, {2, net::mesh_errors::left}}},
            {"crash of one of four", 4, {{3, 0}, {3, 1}, {3, 2}}, {0, 1, 2}, {{3, net::mesh_errors::no_majority}}},
            {"clique of five", 5, {{0, 1}, {2, 3}, {4, 0}}, {1, 2, 4}, {{0, net::mesh_errors::no_majority}, {3, net::mesh_errors::left}}},
        };
        for (const auto& scenario : scenarios) {
            const auto baseline = play(1, scenario);
            expect(baseline.cutoffs.size() == scenario.leavers.size(), scenario.name + ": one agreed departure per removed seat");
            for (const unsigned workers : {2U, 4U, 8U}) {
                const auto run = play(workers, scenario);
                expect(run.hash == baseline.hash, scenario.name + ": state after the pair-loss departures is identical at " + std::to_string(workers) + " workers");
                expect(run.cutoffs == baseline.cutoffs, scenario.name + ": agreed cutoff ticks are identical at " + std::to_string(workers) + " workers");
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) return 1;
    std::cout << "mesh session worker contracts passed\n";
    return 0;
}
