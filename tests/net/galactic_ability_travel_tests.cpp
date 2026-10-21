#include "../galactic/ability_travel_fixture.hpp"
#include "../galactic/galactic_support.hpp"
#include "../support/result.hpp"
#include "fake_transport.hpp"
#include "eawr/campaign/game_session.hpp"
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/platform/sim_workers.hpp"
#include <array>
#include <memory>
#include <limits>

int main() {
    using namespace ability_travel_fixture;
    using eawr::testing::take;
    using galactic_test::expect;
    namespace net = eawr::net;
    try {
        ability_fixture::arm_watchdog();
        auto config = setup(); galactic_test::mark_multiplayer(config);
        eawr::testing::FakeNetwork network(2);
        std::array<std::unique_ptr<eawr::campaign::GameSession>, 2> games;
        std::array<std::unique_ptr<eawr::platform::ThreadWorkerAdapter>, 2> workers;
        std::array<std::unique_ptr<net::GalacticSessionAdapter>, 2> adapters;
        std::array<std::unique_ptr<eawr::testing::FakeTransport>, 2> transports;
        std::vector<net::LockstepSession> peers;
        std::array<std::vector<std::string>, 2> hashes;
        const std::vector<net::PeerSeat> seats{{{1}, true}, {{2}, true}};
        for (net::PeerIndex i = 0; i < 2; ++i) {
            games[i] = std::make_unique<eawr::campaign::GameSession>(take(eawr::campaign::GameSession::create(config)));
            workers[i] = std::make_unique<eawr::platform::ThreadWorkerAdapter>(i == 0 ? 1 : 2);
            adapters[i] = std::make_unique<net::GalacticSessionAdapter>(*games[i], *workers[i]);
            transports[i] = std::make_unique<eawr::testing::FakeTransport>(network, i);
            net::SessionOptions options; options.input_delay = 2; options.final_tick_count = 40;
            peers.push_back(take(net::LockstepSession::create(*adapters[i], *transports[i], seats, options)));
        }
        take(peers[1].submit([&](auto key) { return net::encode_galactic_input(key, move(config)); }));
        for (std::uint64_t round = 0; round < 1000 && (hashes[0].size() < 40 || hashes[1].size() < 40); ++round) {
            network.round = round;
            for (std::size_t i = 0; i < 2; ++i) {
                if (hashes[i].size() == 40) continue;
                const auto result = take(peers[i].pump(round));
                if (result.completed) {
                    hashes[i].push_back(result.completed->state_sha256);
                    (void)adapters[i]->take_tick();
                }
            }
        }
        expect(hashes[0].size() == 40 && hashes[0] == hashes[1], "two real lockstep peers agree at every frontier with different worker counts");
        for (const auto& game : games) {
            const auto& menus = game->galaxy().ability_state()->preparations;
            expect(menus.size() == 1 && menus[0].actor == first && menus[0].planet == 1,
                "seat-inbox travel activates the same live bearer on both peers");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::checker.finish();
}
