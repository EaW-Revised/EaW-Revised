#pragma once

#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/math/fixed.hpp"

#include <algorithm>
#include <array>
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
    // Co-op on a single-player campaign entry (design B2): the entry's authored single-player rules apply and its
    // only playable faction is the Starting_Active_Player, held by every human seat. False for multiplayer entries.
    bool single_player_rules{};
    // The difficulty entry each level (Easy, Normal, Hard) takes in this campaign: the adjustment names the campaign's
    // own AI player types map the level to. An empty name is a level none of them authors. All empty: the campaign states
    // no list (no AI types known), and any authored name is accepted as before.
    std::array<std::string, 3> difficulties;
    friend bool operator==(const MpLobbyCampaign&, const MpLobbyCampaign&) = default;
};

// Whether `difficulty` is one of the campaign's entries (any case, as the AI setup compares); true when it states none.
[[nodiscard]] inline bool lobby_difficulty_offered(const MpLobbyCampaign& campaign, const std::string_view difficulty) noexcept {
    bool any = false;
    for (const auto& entry : campaign.difficulties) {
        if (entry.empty()) continue;
        any = true;
        // ASCII fold: the names are XML identifiers, and a sim header takes no locale function.
        const auto fold = [](const char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c | 0x20) : c; };
        if (entry.size() == difficulty.size() && std::equal(entry.begin(), entry.end(), difficulty.begin(), [&](char a, char b) { return fold(a) == fold(b); }))
            return true;
    }
    return !any;
}
// The entry a new room starts at: the campaign's Normal entry; when that level has none, the middle one of the entries it
// has (never silently the easiest); `fallback` when it states none.
[[nodiscard]] inline std::string lobby_default_difficulty(const MpLobbyCampaign& campaign, const std::string_view fallback) {
    if (!campaign.difficulties[1].empty()) return campaign.difficulties[1];
    std::array<const std::string*, 3> valid{};
    std::size_t count = 0;
    for (const auto& entry : campaign.difficulties) if (!entry.empty()) valid[count++] = &entry;
    return count == 0 ? std::string(fallback) : *valid[count / 2];
}

} // namespace eawr::sim::galactic
