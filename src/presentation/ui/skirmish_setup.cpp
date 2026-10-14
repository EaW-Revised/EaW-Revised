#include "eawr/presentation/ui/skirmish_setup.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <map>

namespace eawr::presentation::ui {

RcRect staging_status_band(const RcRect& last_name, const RcRect& previous_name,
                          const RcRect& last_team, const std::int32_t footer_top) noexcept {
    // Project spacing in resource units; the loaded rectangles supply every anchor.
    const std::int64_t left = static_cast<std::int64_t>(last_name.x) + 3;
    const std::int64_t top = static_cast<std::int64_t>(last_name.y) * 2 - previous_name.y - 4;
    const std::int64_t right = static_cast<std::int64_t>(last_team.x) + last_team.width - 4;
    const auto bounded = [](const std::int64_t value) {
        return static_cast<std::int32_t>(std::clamp(value,
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
    };
    return {bounded(left), bounded(top), bounded(std::max<std::int64_t>(0, right - left)),
        bounded(std::max<std::int64_t>(0, static_cast<std::int64_t>(footer_top) - top - 6))};
}

namespace {

// The keys are the ones the debug build's string table names for these screens, with the retail
// capture's displayed words (R-SETUP-01): the game-type list, the Open and AI rows, the team word, the
// start checks and the LAN panel's buttons. Values come from the installed text database.
constexpr std::array<SetupTextKey, static_cast<std::size_t>(SetupText::count_)> text_keys{{
    {SetupText::game_type_land, "TEXT_GAME_TYPE_TEAM_LAND"},
    {SetupText::game_type_space, "TEXT_GAME_TYPE_TEAM_SPACE"},
    {SetupText::game_type_land_control, "TEXT_GAME_TYPE_CAPTURE_CONTROL_POINTS"},
    {SetupText::open_player, "TEXT_OPEN_PLAYER"},
    // The AI's difficulty is Normal until the selected level reaches the battle (WSS-36); its caption is
    // the medium one.
    {SetupText::ai_player, "TEXT_AI_MEDIUM_PLAYER_NAME"},
    // The local player's own name stands in the first row; this is the fallback when the host gives none.
    {SetupText::host_player, "TEXT_HOST_PREFIX"},
    {SetupText::team_number, "TEXT_MP_TEAM_NUMBER"},
    {SetupText::too_few_players, "TEXT_LAN_HOST_MIN_PLAYERS"},
    {SetupText::too_few_teams, "TEXT_LAN_HOST_NOT_ENOUGH_TEAMS"},
    {SetupText::mixed_factions, "TEXT_LAN_HOST_MISMATCHED_FACTIONS"},
    {SetupText::unavailable, "TEXT_TOOLTIP_GUI_UNAVAILABLE"},
    {SetupText::single_player, "TEXT_BUTTON_SINGLE_PLAYER_GAMES"},
    {SetupText::lan, "TEXT_BUTTON_MULTIPLAYER_LAN"},
    {SetupText::lan_host, "TEXT_BUTTON_HOST_GAME"},
    {SetupText::lan_join, "TEXT_BUTTON_JOIN_GAME"},
    {SetupText::lan_cancel, "TEXT_BUTTON_CANCEL"},
    {SetupText::accept, "TEXT_BUTTON_ACCEPT"},
    {SetupText::start, "TEXT_BUTTON_START_GAME"},
    {SetupText::host_address, "TEXT_HOST_IP"},
}};

constexpr std::array<SetupGameTypeRow, 3> game_types{{
    {SetupGameType::land, SetupText::game_type_land, false},
    {SetupGameType::space, SetupText::game_type_space, true},
    {SetupGameType::land_control, SetupText::game_type_land_control, false},
}};

constexpr std::array<std::string_view, static_cast<std::size_t>(SetupNotice::count_)> notices{{
    "Land battles are not part of this build yet.",
    "The starting credits are not applied to a battle yet; every player starts with the data's default.",
    "Map router port",
    "Opt in to a temporary TCP router mapping for Internet hosting. CGNAT cannot be bypassed.",
}};

// WSS-31: all enemy units, then the enemy starbase. The captions are the database's win-condition keys, which
// the debug build forms from the condition's name without its SKIRMISH_ prefix.
constexpr std::array<SetupWinCondition, 2> win_conditions{{
    {"SKIRMISH_ALL_ENEMY_UNITS_DESTROYED", "TEXT_WIN_CONDITION_ALL_ENEMY_UNITS_DESTROYED"},
    {"SKIRMISH_SPACE_ENEMY_STARBASE_DESTROYED", "TEXT_WIN_CONDITION_SPACE_ENEMY_STARBASE_DESTROYED"},
}};

} // namespace

std::span<const SetupTextKey> skirmish_setup_text_keys() noexcept { return text_keys; }

std::string_view skirmish_setup_text_key(const SetupText text) noexcept {
    const auto index = static_cast<std::size_t>(text);
    return index < text_keys.size() ? text_keys[index].key : std::string_view{};
}

std::string setup_team_caption(const std::string_view team_word, const std::uint32_t team_index) {
    std::string caption(team_word);
    if (!caption.empty()) caption += ' ';
    caption += std::to_string(team_index + 1U);
    return caption;
}

std::span<const SetupGameTypeRow> setup_game_types() noexcept { return game_types; }

std::string_view setup_notice(const SetupNotice notice) noexcept {
    const auto index = static_cast<std::size_t>(notice);
    return index < notices.size() ? notices[index] : std::string_view{};
}

std::span<const SetupWinCondition> setup_win_conditions() noexcept { return win_conditions; }

std::optional<std::size_t> setup_win_condition_index(const std::string_view name) noexcept {
    for (std::size_t i = 0; i < win_conditions.size(); ++i) {
        if (win_conditions[i].name == name) return i;
    }
    return std::nullopt;
}

std::int64_t setup_credits_clamp(const SetupCreditsRange range, const std::int64_t value) noexcept {
    if (range.maximum < range.minimum) return range.minimum;
    return std::clamp(value, range.minimum, range.maximum);
}

SetupBlocker setup_blocker(const std::span<const SetupRow> occupied) {
    if (occupied.size() < 2U) return SetupBlocker::too_few_players;
    std::map<std::uint32_t, std::set<std::string>> factions;
    for (const auto& row : occupied) factions[row.team].insert(row.faction);
    if (factions.size() < 2U) return SetupBlocker::too_few_teams;
    const bool mixed = std::any_of(factions.begin(), factions.end(),
        [](const auto& team) { return team.second.size() > 1U; });
    return mixed ? SetupBlocker::mixed_factions : SetupBlocker::none;
}

std::optional<SetupText> setup_blocker_text(const SetupBlocker blocker) noexcept {
    switch (blocker) {
    case SetupBlocker::too_few_players: return SetupText::too_few_players;
    case SetupBlocker::too_few_teams: return SetupText::too_few_teams;
    case SetupBlocker::mixed_factions: return SetupText::mixed_factions;
    default: return std::nullopt;
    }
}

} // namespace eawr::presentation::ui
