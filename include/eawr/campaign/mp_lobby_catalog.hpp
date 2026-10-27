#pragma once

#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/mp_lobby_catalog.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GCMP-07 (#4363): the campaigns the multiplayer lobby lists, from the installed galactic data.
namespace eawr::campaign {

// The campaign menu's filter (WGP-65) for the multiplayer game type: Is_Listed (absent = listed), Is_Multiplayer
// true and no tutorial flag. Registry order is kept. Each campaign lists the factions its own records name
// (home locations, starting credits and tech, forces, AI control) plus every neutral faction, in the data's
// faction order; a mod that adds or renames factions needs no change here.
[[nodiscard]] std::vector<sim::galactic::MpLobbyCampaign> list_mp_lobby_campaigns(const data::galactic::GalacticData& data);

// One campaign of the list by authored name (case-insensitive), or nullptr.
[[nodiscard]] const sim::galactic::MpLobbyCampaign* find_mp_lobby_campaign(
    const std::vector<sim::galactic::MpLobbyCampaign>& campaigns, std::string_view name) noexcept;

// What the lobby shows for one campaign (#4553): the localized name and description. `resolve` returns the text of a
// key, or nullopt when the text database has none. A missing Text_ID falls back to the campaign's ID and a missing
// Description_Text to no description; each missing key is named in `missing` for the caller's log line.
struct MpLobbyCampaignLabels {
    std::string title, description;
    std::vector<std::string> missing;
};
using MpLobbyTextResolver = std::function<std::optional<std::string>(std::string_view key)>;
[[nodiscard]] MpLobbyCampaignLabels label_mp_lobby_campaign(
    const sim::galactic::MpLobbyCampaign& campaign, const MpLobbyTextResolver& resolve);

} // namespace eawr::campaign
