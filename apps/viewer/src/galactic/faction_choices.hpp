#pragma once

#include "eawr/net/galactic_lobby.hpp"
#include "eawr/presentation/ui/faction_display.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace eawr::viewer::galactic {
struct FactionChoice {
    std::size_t row;
    std::string caption;
};

inline std::vector<FactionChoice> faction_choices(const net::GalacticLobbyOptions& options,
    const data::Catalog& catalog, data::ui::TextLookup* text) {
    std::vector<FactionChoice> result;
    for (std::size_t row = 0; row < options.rows.size(); ++row)
        if (options.rows[row].human)
            result.push_back({row, text
                ? presentation::ui::faction_display_name(catalog, text, options.rows[row].faction)
                : options.rows[row].faction});
    return result;
}

// #6134: what a player's own faction dropdown offers: the planned rows, and every other playable faction row the seat can move to
// (the host's lobby reshapes the rows when it does; two human sides at most, so a row that would make a third is not offered).
inline std::vector<FactionChoice> faction_choices(const net::GalacticLobbyOptions& options,
    const data::Catalog& catalog, data::ui::TextLookup* text, const sim::galactic::MpLobbyCampaign& campaign,
    const net::LobbySnapshot& room, net::PeerIndex peer) {
    std::vector<FactionChoice> result;
    for (std::size_t row = 0; row < options.rows.size(); ++row) {
        if (!options.rows[row].human && !net::galactic_lobby_options_with_seat_on(options, campaign, room, peer, options.rows[row].player)) continue;
        result.push_back({row, text
            ? presentation::ui::faction_display_name(catalog, text, options.rows[row].faction)
            : options.rows[row].faction});
    }
    return result;
}

inline std::optional<sim::galactic::PlayerId> faction_choice_player(
    const net::GalacticLobbyOptions& options, std::size_t row) {
    if (row >= options.rows.size() || !options.rows[row].human) return std::nullopt;
    return options.rows[row].player;
}
// The same for a row the dropdown offered through the overload above: any row of the list may be asked for; the host decides.
inline std::optional<sim::galactic::PlayerId> faction_choice_player(
    const net::GalacticLobbyOptions& options, std::size_t row, const sim::galactic::MpLobbyCampaign& campaign) {
    if (row >= options.rows.size() || row >= campaign.factions.size()) return std::nullopt;
    if (!options.rows[row].human && !campaign.factions[row].playable) return std::nullopt;
    return options.rows[row].player;
}
}
