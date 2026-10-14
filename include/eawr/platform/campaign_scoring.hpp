#pragma once

#include "eawr/core/result.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/galactic/scoring.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace eawr::platform {

// WGP-97..103, GHUD-S61: everything the authored scoring module needs from a campaign besides
// the ledger. Copied once at preparation, so evaluation never touches live campaign owners.
struct CampaignScoringSource {
    std::shared_ptr<const vfs::Vfs> files;
    std::shared_ptr<const data::Catalog> catalog;
    std::shared_ptr<const units::GalacticTypeTable> types;
    // The campaign's bound original type per galactic type id (the setup's type bindings).
    std::map<sim::galactic::TypeId, std::string> type_names;
    // Authored planet type per galactic planet id (the scoring tables key conquests by it).
    std::map<sim::galactic::PlanetId, std::string> planet_types;
    std::string campaign_name;
};

struct CampaignScoringPlayer {
    sim::galactic::PlayerId player{};
    std::string faction;
};

struct CampaignScoringOutcome {
    std::string title_key;
    std::array<std::optional<double>, 3> efficiencies; // military, conquest, kill (WGP-97 order)
};

// Mounts the module named by the effective Game_Scoring_Script_Name in the existing script
// sandbox, replays the raw ledger into its galactic event handlers and reads the four score
// controls for `local`. Every equation stays in the mounted module, so a mod that replaces it
// keeps its own scoring. The campaign ledger is the galactic one (WGP-99: tactical resets
// never reach it). Absent module or constants file: no outcome, never an error.
[[nodiscard]] core::Result<std::optional<CampaignScoringOutcome>> evaluate_campaign_scoring(
    const CampaignScoringSource& source, const sim::galactic::CampaignScoreLedger& ledger,
    std::span<const CampaignScoringPlayer> players, sim::galactic::PlayerId local);

} // namespace eawr::platform
