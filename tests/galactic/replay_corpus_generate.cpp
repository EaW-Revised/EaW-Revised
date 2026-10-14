#include "galactic_support.hpp"

#include "eawr/campaign/game_session.hpp"
#include "eawr/campaign/synthetic.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace {
namespace galactic = eawr::sim::galactic;

void generate(const std::filesystem::path& directory, const std::string& name,
    const std::uint32_t planets, const std::uint32_t players, const bool coop,
    const std::uint64_t ticks, const bool commands, const std::string& fiscal) {
    eawr::campaign::SyntheticCampaign campaign;
    campaign.name = name;
    campaign.galaxy.seed = 11;
    campaign.galaxy.planets = planets;
    campaign.galaxy.players = players;
    campaign.galaxy.coop_seat = coop;
    campaign.galaxy.debug_commands = commands;
    campaign.galaxy.fleets_per_player = 3;
    campaign.galaxy.members_per_fleet = 4;
    if (!fiscal.empty()) campaign.fiscal_cycle_seconds = eawr::sim::math::Fixed::from_decimal(fiscal).value();
    const auto setup = eawr::campaign::build_synthetic_campaign(campaign).value();
    auto orders = commands ? galactic_test::script(setup, ticks) : std::vector<galactic::GameCommand>{};
    if (commands) {
        // WGM-05/06: nested pause holds logical frames; rejected excess resume stays recorded.
        const auto seat = setup.seats.back().seat_id;
        orders.push_back({{2, seat, 1}, galactic::PauseGalaxy{}});
        orders.push_back({{3, seat, 2}, galactic::PauseGalaxy{}});
        orders.push_back({{6, seat, 3}, galactic::ResumeGalaxy{}});
        orders.push_back({{9, seat, 4}, galactic::ResumeGalaxy{}});
        orders.push_back({{10, seat, 5}, galactic::ResumeGalaxy{}});
        orders.push_back({{11, seat, 6}, galactic::SetSpeed{false, eawr::sim::math::Fixed::from_integer(2).value()}});
        // Sort indices rather than variant payloads, as in the shared command script helper.
        std::vector<std::size_t> order;
        for (std::size_t index = 0; index < orders.size(); ++index) order.push_back(index);
        std::sort(order.begin(), order.end(), [&](const auto left, const auto right) { return orders[left].key < orders[right].key; });
        std::vector<galactic::GameCommand> canonical;
        for (const auto index : order) canonical.push_back(orders[index]);
        orders = std::move(canonical);
    }
    auto session = eawr::campaign::GameSession::create(setup).value();
    const eawr::sim::InlineExecutor executor;
    std::ostringstream hashes;
    hashes << "tick,sha256\n";
    std::size_t next = 0;
    for (std::uint64_t tick = 0; tick < ticks; ++tick) {
        while (next < orders.size() && orders[next].key.tick == tick) {
            const auto admitted = session.submit(orders[next++]);
            if (!admitted) throw std::runtime_error(eawr::core::format_diagnostic(admitted.error()));
        }
        const auto stepped = session.step(executor);
        if (!stepped) throw std::runtime_error(eawr::core::format_diagnostic(stepped.error()));
        hashes << stepped.value().completed_tick << ',' << stepped.value().state_hash.get() << '\n';
    }
    const auto bytes = galactic::write_replay(session.record()).value();
    std::ofstream replay(directory / (name + ".eawr-gcreplay"), std::ios::binary);
    replay.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    std::ofstream pin(directory / (name + ".hashes.csv"), std::ios::binary);
    pin << hashes.str();
    if (!replay || !pin) throw std::runtime_error("could not write corpus fixture");
}
} // namespace

int main(const int argc, const char* const argv[]) {
    if (argc != 2) { std::cerr << "expected output directory\n"; return 2; }
    try {
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        generate(directory, "quiet-minimal", 2, 1, false, 16, false, "");
        generate(directory, "fiscal-boundary", 12, 2, false, 1352, false, "45");
        generate(directory, "coop-orders", 40, 3, true, 160, true, "2.5");
        generate(directory, "partitioned-orders", 257, 8, true, 96, true, "0.5");
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
