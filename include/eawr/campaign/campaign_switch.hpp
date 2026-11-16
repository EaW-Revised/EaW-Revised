#pragma once

#include "eawr/core/result.hpp"
#include "eawr/data/galactic/galactic_data.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The LOAD_CAMPAIGN story reward (WST-44, WST-95): a plot asks the host to end the running campaign and start another.
// The simulation never restarts itself. The reward only records a CampaignSwitchRequest in the step's output; the host
// acts on it between ticks by tearing the session down and starting the named campaign through the same path the setup
// screen uses (the solo start: the campaign entry, the human side the entry seats, the difficulty entry of the level).
namespace eawr::campaign {

// The warning code a refused reward (or a refused target) is published under.
inline constexpr std::string_view campaign_switch_warning_code = "EAWR-SCRIPT-0217";

// The reward's parameters as authored or set by a plot script, first token of each slot, trimmed; empty = absent.
//   Reward_Param1 the campaign entry, Reward_Param2 the side (a faction index in the faction table's order, or a faction
//   name), Reward_Param3 the difficulty (0 easy, 1 normal, 2 hard, or the level's name), Reward_Param4 a technology marker
//   (recorded, not applied). The values are not checked against any data when the reward fires.
struct CampaignSwitchRequest {
    std::string campaign, side, difficulty, tech;
    std::uint64_t tick{}; // the completed tick of the step that recorded it
    friend bool operator==(const CampaignSwitchRequest&, const CampaignSwitchRequest&) = default;
};

// A request resolved against the campaign data, ready for the one start path.
struct CampaignSwitchTarget {
    std::string campaign;   // the entry's authored name
    std::string faction;    // the faction the entry seats the human player on, by the data's name
    std::string difficulty; // "easy", "normal" or "hard": the staging dialog's level of the start path
    std::optional<std::int64_t> tech;
    friend bool operator==(const CampaignSwitchTarget&, const CampaignSwitchTarget&) = default;
};

// The side is Reward_Param2 when it is given (an index into data.factions, or a faction name in any case) and the entry's
// own human faction (local_start_faction) when it is absent. The entry must exist, the faction must be a playable,
// non-neutral faction, and it must be the faction the entry seats a human player on: a solo start has one human seat, so
// any other side is refused rather than silently replaced. An absent or unreadable difficulty reads Normal (the staging
// dialog's default level; unverified for retail).
[[nodiscard]] core::Result<CampaignSwitchTarget> resolve_campaign_switch(
    const data::galactic::GalacticData& data, const CampaignSwitchRequest& request);

// The published text of a refused switch: "EAWR-SCRIPT-0217 [warning]: LOAD_CAMPAIGN refused: <reason>".
[[nodiscard]] std::string campaign_switch_warning(std::string_view reason);

} // namespace eawr::campaign
