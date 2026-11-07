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

inline std::optional<sim::galactic::PlayerId> faction_choice_player(
    const net::GalacticLobbyOptions& options, std::size_t row) {
    if (row >= options.rows.size() || !options.rows[row].human) return std::nullopt;
    return options.rows[row].player;
}
}
