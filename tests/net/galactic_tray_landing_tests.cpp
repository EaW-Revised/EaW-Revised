// A land-tray drop on the planet detail screen (GPD-22/23) is three ordinary seat commands submitted in one
// command boundary: launch the pad's occupant, land the dropped unit, set its marker.
//  - lockstep: the exact command list runs through four real lockstep peers (the co-op party of seats 1 and 2 plus
//    seats 3 and 5), once per worker count (1, 2, 4, 8 on every peer), and every peer applies it identically;
//    the digests and the final state also agree between the worker counts. This test submits the three commands one
//    by one through the lockstep session, because that session takes one command per submit.
//  - batch: LiveGameSession::submit_batch (the path the viewer uses) puts all orders of one batch in the same
//    command boundary, in order.
#include "../galactic/galactic_support.hpp"
#include "../presentation/galactic/ground_hud_fixture.hpp"
#include "../support/result.hpp"
#include "fake_transport.hpp"
#include "galactic/galaxy_view.hpp"
#include "galactic/land_tray_hud.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {
namespace net = eawr::net;
namespace gc = eawr::sim::galactic;
namespace view = eawr::viewer::galactic;
using eawr::testing::take;
using galactic_test::expect;
using namespace std::chrono_literals;

gc::GalacticSetup fixture() {
    auto config = view::synthetic_live_setup();
    view::configure_synthetic_hud_clock(config);
    std::vector<gc::FleetDisplayType> traits;
    std::vector<gc::FleetDisplayVisibility> visibility;
    std::map<gc::TypeId, std::string> names;
    view::configure_ground_fixture(config, 1, "ground-launch-move", traits, visibility, names);
    return config;
}

// Seat 1 drops fleet 601's first company (7001) on the pad holding 8001, whose marker record is 5.
view::TrayLanding plan(const gc::GalacticSetup& config) {
    auto planner = take(gc::GalacticSession::create(config));
    const eawr::platform::ThreadWorkerAdapter executor(1);
    take(planner.step(executor));
    const view::LandTrayEntry entry{.type = 1000, .count = 2, .enabled = true, .representative = 7001, .fleet = 601};
    view::DetailPad pad; pad.marker.record = 5; pad.object = 8001; pad.type = 1000;
    return view::plan_tray_landing(*planner.snapshot(), 1, 101, entry, pad, {true, true, true});
}

bool landed(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId id) {
    const auto& rows = snapshot.control()->planets;
    const auto surface = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.planet == 101; });
    return surface != rows.end() && std::any_of(surface->landed.begin(), surface->landed.end(),
        [id](const auto& row) { return row.id == id; });
}
bool remembers(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId id, std::uint32_t marker) {
    const auto& rows = snapshot.control()->planets;
    const auto surface = std::find_if(rows.begin(), rows.end(), [](const auto& row) { return row.planet == 101; });
    return surface != rows.end() && std::any_of(surface->ground_layout.begin(), surface->ground_layout.end(),
        [&](const auto& row) { return row.object == id && row.marker == marker; });
}
bool in_orbit(const gc::GalacticSnapshot& snapshot, eawr::sim::EntityId id) {
    const auto fleets = snapshot.fleets();
    return std::any_of(fleets.begin(), fleets.end(), [id](const auto& fleet) {
        return fleet.orbit == 101 && std::any_of(fleet.members.begin(), fleet.members.end(),
            [id](const auto& member) { return member.id == id; }); });
}

struct Outcome { std::vector<std::string> hashes; std::vector<std::uint8_t> state; };

Outcome lockstep(const gc::GalacticSetup& unmarked, const view::TrayLanding& drop, const std::size_t workers) {
    auto config = unmarked;
    galactic_test::mark_multiplayer(config);
    std::vector<net::PeerSeat> seats;
    for (const auto& seat : config.seats) if (seat.kind == eawr::sim::SeatKind::human) seats.push_back({seat.seat_id, true});
    std::sort(seats.begin(), seats.end(), [](const auto& a, const auto& b) { return a.seat < b.seat; });
    const std::size_t count = seats.size();
    const auto where = "workers=" + std::to_string(workers) + ": ";
    expect(count == 4 && seats.front().seat.value == 1, where + "the fixture has four human peers, the co-op party starting at seat 1");
    eawr::testing::FakeNetwork network(static_cast<std::uint32_t>(count));
    std::vector<std::unique_ptr<eawr::campaign::GameSession>> games;
    std::vector<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>> executors;
    std::vector<std::unique_ptr<net::GalacticSessionAdapter>> adapters;
    std::vector<std::unique_ptr<eawr::testing::FakeTransport>> transports;
    std::vector<net::LockstepSession> peers;
    std::vector<std::vector<std::string>> hashes(count);
    for (net::PeerIndex i = 0; i < count; ++i) {
        games.push_back(std::make_unique<eawr::campaign::GameSession>(take(eawr::campaign::GameSession::create(config))));
        executors.push_back(std::make_unique<eawr::platform::ThreadWorkerAdapter>(workers));
        adapters.push_back(std::make_unique<net::GalacticSessionAdapter>(*games[i], *executors[i]));
        transports.push_back(std::make_unique<eawr::testing::FakeTransport>(network, i));
        net::SessionOptions options; options.input_delay = 2; options.final_tick_count = 30;
        peers.push_back(take(net::LockstepSession::create(*adapters[i], *transports[i], seats, options)));
    }
    std::size_t submitted = 0;
    for (std::uint64_t round = 0; round < 2000; ++round) {
        network.round = round;
        bool all_done = true;
        for (std::size_t i = 0; i < count; ++i) {
            if (hashes[i].size() == 30) continue;
            all_done = false;
            // Seat 1 drops the tray unit once: all three commands at the same tick.
            if (i == 0 && submitted == 0 && adapters[0]->completed_tick() == 3) {
                for (const auto& command : drop.commands) {
                    take(peers[0].submit([&](auto key) { return net::encode_galactic_input(key, command); }));
                    ++submitted;
                }
            }
            const auto result = take(peers[i].pump(round));
            if (result.completed) {
                hashes[i].push_back(result.completed->state_sha256);
                (void)adapters[i]->take_tick();
            }
        }
        if (all_done) break;
    }
    expect(submitted == 3, where + "all three commands were submitted");
    for (std::size_t i = 0; i < count; ++i) {
        expect(hashes[i].size() == 30 && hashes[i] == hashes[0], where + "every peer agrees at every frontier");
        expect(adapters[i]->canonical_state() == adapters[0]->canonical_state(), where + "equal final canonical state on every peer");
        const auto snapshot = games[i]->galaxy().snapshot();
        expect(landed(*snapshot, 7001) && !landed(*snapshot, 8001), where + "the dropped unit is landed and the occupant is gone on every peer");
        expect(remembers(*snapshot, 7001, 5), where + "the dropped unit remembers the pad marker on every peer");
        expect(in_orbit(*snapshot, 8001), where + "the launched occupant is an orbit fleet member on every peer");
    }
    return {hashes[0], adapters[0]->canonical_state()};
}

void batch(const gc::GalacticSetup& config, const view::TrayLanding& drop) {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        for (const bool reversed : {false, true}) {
            const auto where = "batch workers=" + std::to_string(workers) + (reversed ? " reversed: " : ": ");
            auto live = take(eawr::platform::LiveGameSession::start(config,
                {.workers = workers, .pacing = eawr::platform::LiveGameSession::Pacing::driven}));
            std::vector<eawr::platform::LiveGameOrder> orders;
            for (const auto& command : drop.commands) orders.push_back({gc::SeatId{1}, command, {}});
            if (reversed) std::reverse(orders.begin(), orders.end());
            live->submit_batch(std::move(orders));
            live->advance_to(1);
            expect(live->wait_for(1, 10s), where + "the first tick completes");
            const auto snapshot = live->frame().latest;
            // After exactly one tick: every order of the batch was applied in that tick.
            expect(landed(*snapshot, 7001) && !landed(*snapshot, 8001) && in_orbit(*snapshot, 8001),
                where + "the launch and the landing of one batch both applied in the first tick");
            // And in the submitted order: the marker command ran before the landing in the reversed batch, so it was refused.
            expect(remembers(*snapshot, 7001, 5) == !reversed,
                where + (reversed ? "a marker ordered before the landing is refused" : "a marker ordered after the landing is applied"));
            live->stop();
        }
    }
}
} // namespace

int main() {
    try {
        const auto config = fixture();
        const auto drop = plan(config);
        expect(drop.result == view::TrayDropResult::ok && drop.commands.size() == 3, "the tray drop plans launch, land and marker");
        std::vector<Outcome> outcomes;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) outcomes.push_back(lockstep(config, drop, workers));
        for (std::size_t i = 1; i < outcomes.size(); ++i)
            expect(outcomes[i].hashes == outcomes[0].hashes && outcomes[i].state == outcomes[0].state,
                "digests and final state are equal between the worker counts");
        batch(config, drop);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::checker.finish();
}
