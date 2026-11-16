#pragma once

#include "eawr/campaign/setup_plan.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/sim/galactic/mp_lobby_catalog.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// GCMP-07 (#4363): the campaigns the multiplayer lobby lists, from the installed galactic data.
namespace eawr::campaign {

// The entry names an AI player type maps Easy, Normal and Hard to (an empty name where it authors none), by type name in
// any case. The lists below ask it for the AI types a campaign uses; absent, no campaign carries a difficulty list.
using LobbyAiLevels = std::function<std::array<std::string, 3>(std::string_view player_type)>;

// The campaign menu's filter (WGP-65) for the multiplayer game type: Is_Listed (absent = listed), Is_Multiplayer
// true and no tutorial flag. Registry order is kept. Each campaign lists the factions its own records name
// (home locations, starting credits and tech, forces, AI control) plus every neutral faction, in the data's
// faction order; a mod that adds or renames factions needs no change here.
[[nodiscard]] std::vector<sim::galactic::MpLobbyCampaign> list_mp_lobby_campaigns(
    const data::galactic::GalacticData& data, const LobbyAiLevels& ai_levels = {});

// The second list, for the lobby's separate co-op tab: listed, non-tutorial, non-story single-player entries
// (single_player_rules) whose Starting_Active_Player names a playable, non-neutral faction of the data. An entry
// whose starter is not playable (a dummy or filler entry that starts as a hostile faction) has no seat for a player
// and stays out; the data itself is not changed. Each keeps its authored entry and only that starter is playable.
[[nodiscard]] std::vector<sim::galactic::MpLobbyCampaign> list_coop_sp_lobby_campaigns(
    const data::galactic::GalacticData& data, const LobbyAiLevels& ai_levels = {});

// Both lists in tab order (multiplayer first), for a lobby that offers both tabs: lobby validation looks a
// campaign up in whatever catalog it is given, so such a lobby passes this one.
[[nodiscard]] std::vector<sim::galactic::MpLobbyCampaign> list_all_lobby_campaigns(
    const data::galactic::GalacticData& data, const LobbyAiLevels& ai_levels = {});

// The difficulty adjustment name a single-player start runs at for the staging dialog's level ("easy", "normal" or
// "hard", any case; empty or anything else reads Normal). It is the entry the campaign's own AI player types map the
// level to (the same pick the lobby offers); a campaign that states none for the level, or no such campaign, falls back
// to the level's stock name (CGS-26). Any campaign entry works, listed or not.
[[nodiscard]] std::string solo_difficulty_entry(const data::galactic::GalacticData& data, std::string_view campaign,
    std::string_view level, const LobbyAiLevels& ai_levels = {});

// One campaign of the list by authored name (case-insensitive), or nullptr.
[[nodiscard]] const sim::galactic::MpLobbyCampaign* find_mp_lobby_campaign(
    const std::vector<sim::galactic::MpLobbyCampaign>& campaigns, std::string_view name) noexcept;

// The first row of a catalog a room can open on (one with a playable faction for a human seat), or nullptr when no
// row can host. The host opens its room on this row; the catalog order (multiplayer rows first) decides which.
[[nodiscard]] const sim::galactic::MpLobbyCampaign* first_hostable_lobby_campaign(
    const std::vector<sim::galactic::MpLobbyCampaign>& campaigns) noexcept;

// The rules a campaign entry runs under when nothing overrides them: a multiplayer entry uses multiplayer rules, a
// single-player entry the authored single-player rules (every peer derives the same value from the agreed campaign).
[[nodiscard]] CampaignRulesFlavour campaign_rules_flavour(const data::galactic::Campaign& campaign) noexcept;

// The faction a start with one local human (no lobby to choose) seats that player on. A single-player entry names it
// (Starting_Active_Player); a multiplayer entry authors none, so it is the first faction the entry's lobby row lets a
// player hold (the row's playable flag, in the data's faction order). Empty when the entry has no such faction.
[[nodiscard]] std::string local_start_faction(const data::galactic::GalacticData& data,
    const data::galactic::Campaign& campaign);

// The caption of a campaign's row in the lobby list: the localized title, and for a co-op row (single-player rules)
// the faction the players take, as "FACTION - Title", since one set is listed once per starting faction (the list cuts a
// long caption at its end, so the faction goes first).
[[nodiscard]] std::string lobby_campaign_row_title(const sim::galactic::MpLobbyCampaign& campaign, std::string_view title);

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
