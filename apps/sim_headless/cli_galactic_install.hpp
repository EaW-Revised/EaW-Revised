#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

// sim_headless --galactic <campaign name> --game-root <install> [--mod-root <leaf;parent;...>] [--ticks <n>]
//     [--workers <n>] [--census-out <report.json>]
// Starts a stock or mod campaign from the installed data, one stage at a time (mount, catalog, galactic tables,
// plan, services, placement, session, ticks). Each stage prints one line; the first failing stage ends the run
// with its diagnostic. The JSON report carries the stages and the galactic loader's issue families with counts.
namespace sim_headless::cli {

struct InstalledCampaignRequest {
    std::string campaign;
    std::string game_root;
    std::filesystem::path mod_root;
    std::uint64_t ticks{};
    std::uint64_t live_ticks{}; // run the viewer's live session with the native AI for this many ticks (0 = off)
    bool live_ai{}; // install the native AI for the live run
    std::string perception_trace; // --live-perception: root equation whose failed leaves are printed
    std::uint64_t goals_every{}; // with live_ai: note the native AI's goals every this many ticks (0 = off)
    std::uint64_t play_ticks{}; // each of the human player's test orders may run this long (0 = no orders)
    std::uint64_t seed{42};
    std::size_t workers{1};
    std::string report_path;
};

[[nodiscard]] int run_installed_campaign(const InstalledCampaignRequest& request);

} // namespace sim_headless::cli
