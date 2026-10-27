#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// GCMP-07 (#4363): what the multiplayer lobby offers for a campaign, read from the installed content. It is
// plain data so the lobby codec and validation in net need no content library: campaign/mp_lobby_catalog.hpp
// fills it from the galactic data (Is_Listed + Is_Multiplayer, WGP-65) and every peer builds its own copy.
namespace eawr::sim::galactic {

// One faction row of a campaign, the lobby's unit of choice. `player` and `faction_id` are the lobby's own
// numbering (the 1-based position in the campaign's faction list); the galaxy derives its real identities
// from the roster these rows become, so they only have to agree between peers.
struct MpLobbyFaction {
    PlayerId player{};
    FactionId faction_id{};
    std::uint32_t flags{};
    std::string name;     // authored faction name; any faction the data declares
    bool playable{};      // Is_Playable: a human may hold it
    bool neutral{};       // Is_Neutral: never gets an AI seat
    friend bool operator==(const MpLobbyFaction&, const MpLobbyFaction&) = default;
};

struct MpLobbyCampaign {
    std::string name;
    // The authored Text_ID and Description_Text keys (#4553); empty when the campaign states none.
    std::string text_id, description_text;
    // The galactic map position of each of the campaign's planets: the staging preview's blips (#4553).
    std::vector<std::pair<math::Fixed, math::Fixed>> planet_points;
    bool supports_custom_settings{};
    // Campaign limits for the custom starting and maximum tech (WGP-69); absent means the data states none.
    std::optional<std::int64_t> minimum_custom_tech, maximum_custom_tech;
    std::vector<MpLobbyFaction> factions; // ascending player ID
    friend bool operator==(const MpLobbyCampaign&, const MpLobbyCampaign&) = default;
};

} // namespace eawr::sim::galactic
