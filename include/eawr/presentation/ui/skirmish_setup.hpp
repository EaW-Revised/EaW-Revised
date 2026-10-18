#pragma once

// The skirmish staging screen's text and start blockers (docs/ui/menu-screens.md MNU-21 and MNU-22,
// rules R-SETUP-01 and R-SETUP-05 in docs/behaviour/skirmish-setup.md). Engine-free; the viewer draws
// it. The two dialogs' own captions are their resource keys, set by the dialog builder. This lists
// every other caption the screen sets as a text-database key, so mods and other languages show their
// own text, and keeps the few notices the retail database has no key for apart from them.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::ui {

struct RcRect;

// The project status line below a staging dialog's last player row. The authored
// columns, row pitch and first footer control bound its free band; small insets
// retain the existing spacing. A resource with no free band yields zero height.
[[nodiscard]] RcRect staging_status_band(const RcRect& last_name, const RcRect& previous_name,
                                         const RcRect& last_team, std::int32_t footer_top) noexcept;
// MNU-21 and MNU-22: the dialogs the screen builds (the guidialogs resource names).
inline constexpr std::string_view skirmish_staging_dialog = "IDD_SKIRMISH_STAGING_DIALOG";
inline constexpr std::string_view skirmish_options_dialog = "IDD_INTERNET_STAGING_DIALOG_ADVANCED_SKIRMISH_OPTIONS";

// UI-T4: the language of the text database. The viewer has no language setting yet; every screen asks
// for the same one.
inline constexpr std::string_view skirmish_setup_language = "ENGLISH";

// Every caption the screen sets itself, apart from the dialogs' own. Each has one text-database key.
enum class SetupText : std::uint8_t {
    game_type_land,
    game_type_space,
    game_type_land_control,
    open_player,
    ai_player,      // the Normal (medium) AI row
    ai_easy_player,
    ai_hard_player,
    host_player,
    team_number,
    too_few_players,
    too_few_teams,
    mixed_factions,
    unavailable,
    single_player,
    lan,
    lan_host,
    lan_join,
    lan_cancel,
    accept,
    start,
    host_address,
    count_,
};

struct SetupTextKey final {
    SetupText text{};
    std::string_view key;
};

// One entry per SetupText, in enum order.
[[nodiscard]] std::span<const SetupTextKey> skirmish_setup_text_keys() noexcept;
[[nodiscard]] std::string_view skirmish_setup_text_key(SetupText text) noexcept;

// "Team 2": the team word the database holds, a space and the one-based number the rows show.
[[nodiscard]] std::string setup_team_caption(std::string_view team_word, std::uint32_t team_index);

// MNU-21: the game types the list offers, in the retail order. Land battles and land control are not
// built here (the project targets the space skirmish for now): their rows stay visible and disabled.
enum class SetupGameType : std::uint8_t { land, space, land_control };

struct SetupGameTypeRow final {
    SetupGameType type{};
    SetupText text{};
    bool available{};
};

[[nodiscard]] std::span<const SetupGameTypeRow> setup_game_types() noexcept;
// The row the list opens on (the skirmish is a space battle).
[[nodiscard]] constexpr SetupGameType setup_default_game_type() noexcept { return SetupGameType::space; }

// Project text the retail database has no key for: notices and router-mapping controls. They are project
// text, kept here so the captions stay keys only (the tests pin both tables).
enum class SetupNotice : std::uint8_t {
    land_not_built, credits_not_applied, port_mapping, port_mapping_tooltip, count_
};

[[nodiscard]] std::string_view setup_notice(SetupNotice notice) noexcept;

// MNU-22, WSS-31: the advanced dialog's win-condition choices in a space battle (a code list in the debug
// build, so mods do not add to it). `name` is the condition the data and the replay record write
// (MP_Default_Space_Tactical_Win_Condition); `text_key` is the database key of the choice's caption.
struct SetupWinCondition final {
    std::string_view name;
    std::string_view text_key;
};

[[nodiscard]] std::span<const SetupWinCondition> setup_win_conditions() noexcept;
// The index of `name` among setup_win_conditions(), or nothing when the data names another condition.
[[nodiscard]] std::optional<std::size_t> setup_win_condition_index(std::string_view name) noexcept;

// MNU-22, WSS-25: the credits display uses the data's Min_Skirmish_Credits and Max_Skirmish_Credits.

struct SetupCreditsRange final {
    std::int64_t minimum{};
    std::int64_t maximum{};
};

// WSS-25: clamp an existing value into the authored bounds without rounding it to an edit bucket.
[[nodiscard]] std::int64_t setup_credits_clamp(SetupCreditsRange range, std::int64_t value) noexcept;

// One occupied row of the staging: its team (zero-based) and faction name.
struct SetupRow final {
    std::uint32_t team{};
    std::string faction;
};

// R-SETUP-01 and WSS-16 to WSS-18: what keeps Start from working, in the order the retail checks are
// listed (at least two players, at least two teams, one faction per team; the order between checks
// that fail together is not verified).
enum class SetupBlocker : std::uint8_t { none, too_few_players, too_few_teams, mixed_factions };

[[nodiscard]] SetupBlocker setup_blocker(std::span<const SetupRow> occupied);
// The caption the blocker shows, or nothing for SetupBlocker::none.
[[nodiscard]] std::optional<SetupText> setup_blocker_text(SetupBlocker blocker) noexcept;

} // namespace eawr::presentation::ui
