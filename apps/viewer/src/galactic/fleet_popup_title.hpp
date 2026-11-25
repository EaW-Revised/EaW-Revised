#pragma once

#include "galactic/notice_text.hpp"

#include "eawr/presentation/ui/hud_shell.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

namespace eawr::viewer::galactic {
// Text keys of the fleet encyclopedia header (WGUI-166).
inline constexpr std::string_view fleet_title_format_key = "TEXT_EAWR_FACTION_FLEET_FORMAT";
inline constexpr std::string_view garrison_title_format_key = "TEXT_EAWR_FACTION_GARRISON_FORMAT";

// The header of the fleet encyclopedia popup: the owner's fleet (space) or garrison (ground) name.
// - A faction that is the GameConstants good, evil or corrupt side, or the Pirates faction, keeps the stock side keys
//   (WGUI-166).
// - Every other faction (any extra faction of a mod) is named by its own display name through a project format key
//   that a mod's text table may override (`%s Fleet`, `%s Garrison` by default). The stock keys have no name for such a
//   faction and read "enemy" for it, which labelled a player's own fleet as the enemy; this is a project presentation
//   rule (the popup never names a faction an enemy). A faction without any display name falls back to the stock pirate
//   key, so the header is never empty.
// `text` is callable with a key and returns its text (the key itself or an empty string when it has none).
template <typename TextOf>
[[nodiscard]] std::string fleet_popup_title(const std::string_view faction,
    const presentation::ui::SideNames& sides, const std::string_view display_name, const bool ground, TextOf&& text) {
    constexpr std::array<std::string_view, 4> space_keys{"TEXT_GOOD_SPACE_FLEET", "TEXT_EVIL_SPACE_FLEET",
        "TEXT_CORRUPT_SPACE_FLEET", "TEXT_PIRATE_SPACE_FLEET"};
    constexpr std::array<std::string_view, 4> ground_keys{"TEXT_GOOD_LAND_GARRISON", "TEXT_EVIL_LAND_GARRISON",
        "TEXT_CORRUPT_LAND_GARRISON", "TEXT_PIRATE_LAND_GARRISON"};
    const auto side = presentation::ui::allegiance_emblem_index(faction, sides);
    if (side < space_keys.size()) return text(ground ? ground_keys[side] : space_keys[side]);
    const std::string name(display_name.empty() ? faction : display_name);
    if (name.empty()) return text(ground ? ground_keys[3] : space_keys[3]);
    const auto key = ground ? garrison_title_format_key : fleet_title_format_key;
    const auto format = text(key);
    // A text source with no project table leaves the key itself: the name alone still never reads "enemy".
    if (format.empty() || format == key) return name;
    const std::array<std::string, 1> arguments{name};
    return expand_printf_notice(format, arguments);
}
} // namespace eawr::viewer::galactic
