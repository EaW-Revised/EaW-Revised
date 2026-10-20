#pragma once

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/core/result.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/story_engine.hpp"
#include "eawr/vfs/vfs.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace eawr::campaign {

// The plots of one campaign start (WGS-07, WST-01..08): the plot list and the sub-plot files in
// table order (plot order, then file order inside the plot). `notes` carries what the original
// logs or asserts on (missing files, unknown event types, duplicate names), without failing the load.
struct StoryPlots {
    std::vector<sim::galactic::story::PlotRecord> plots;
    std::vector<sim::galactic::story::SubPlotDef> subplots;
    std::vector<std::string> notes;
};

// One plot file named by the campaign for `owner` (WST-01..03). An empty name loads nothing.
core::Result<void> load_story_plot(const vfs::Vfs&, sim::galactic::story::Player owner, std::string_view plot_file, StoryPlots& out);

// The unit types the plots create (SPAWN_HERO, UNIQUE_UNIT; reward parameter 1), in first-use order without repeats (#683).
// Setup binds them into the creation inputs so a plot can place them whenever it fires (WST-40).
[[nodiscard]] std::vector<std::string> story_unit_names(const StoryPlots&);

// WGS-07: for each active player, the campaign's `*_Story_Name` that matches the player's side
// (GameConstants Good/Evil/Corrupt_Side_Name -> Rebel/Empire/Underworld plot), in that order.
[[nodiscard]] core::Result<StoryPlots> load_story_plots(const vfs::Vfs&, const data::galactic::GalacticData&,
    const CampaignSetupPlan&);

} // namespace eawr::campaign
