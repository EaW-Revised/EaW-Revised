#pragma once

#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/mp_lobby_catalog.hpp"

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

} // namespace eawr::campaign
