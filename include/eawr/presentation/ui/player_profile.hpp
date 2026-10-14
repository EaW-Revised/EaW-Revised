#pragma once

// #2935: the player profile (docs/ui/player-profile.md, MNU-46): what the menus remember about
// the player beside the settings record (settings.hpp): the name and lobby colour, the last
// skirmish choices, the tutorials played and the network name. One versioned record, its text
// form (UTF-8; a player name may be non-ASCII) and the clamping that makes a damaged file safe.
//
// The profile is UI state (UI-C2): it never enters the simulation, a replay or a hash. A screen
// reads defaults from it when it opens and writes back when the player starts or accepts; the
// values that reach the match go through the setup seams, not from here.

#include "eawr/platform/ui_files.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

// The text form's first line is "eawr-profile <version>". Another version loads as defaults.
inline constexpr std::uint32_t profile_version = 1;

// A name is at most this many characters (code points); longer is cut.
inline constexpr std::size_t player_name_max_chars = 32;
// The lobby colour is a row of the colour list; the staging screens have eight or nine rows.
inline constexpr int colour_index_maximum = 15;
inline constexpr int team_index_maximum = 15;
// "Use the authored credits" (retail keeps -1 until a match is started).
inline constexpr int credits_authored = -1;
inline constexpr int credits_maximum = 1000000;
inline constexpr int timer_minutes_maximum = 1440;
// The last game type is an integer of the retail setup; 1 is its default.
inline constexpr int game_type_default = 1;
inline constexpr int game_type_maximum = 15;
inline constexpr std::size_t tutorials_played_max = 64;
// The name shown when the platform gives none.
inline constexpr std::string_view fallback_player_name = "Player";

// What one battle kind (space, land) remembers: the map, and the local player's faction,
// colour and team. Maps and factions are the data's own names, never an index into a fixed
// list, so a mod's lists work (an index would point at another faction after a data change).
struct SkirmishSideChoice final {
    std::string map;     // map name; empty: none chosen yet
    std::string faction; // faction name; empty: the screen's own default
    int colour{};
    int team{};

    friend bool operator==(const SkirmishSideChoice&, const SkirmishSideChoice&) = default;
};

// The last skirmish choices (WSS-02, WSS-23): one set per battle kind, and the match options
// that are shared.
struct SkirmishChoices final {
    SkirmishSideChoice space;
    SkirmishSideChoice land;
    // The last scenario (tutorial or scripted) map: a separate field in the retail profile.
    std::string scenario_map;
    int game_type{game_type_default};
    int credits{credits_authored};
    int timer_minutes{};
    std::string game_name;

    friend bool operator==(const SkirmishChoices&, const SkirmishChoices&) = default;
};

struct PlayerProfile final {
    // Empty follows the platform's user name (the retail default); see effective_player_name.
    std::string player_name;
    // The lobby colour (MNU-26's colour combo).
    int lobby_colour{};
    // The name shown in LAN and internet lobbies; empty follows the player name.
    std::string network_name;
    SkirmishChoices skirmish;
    // The tutorials started, by the data's own tutorial name. Empty: none seen.
    std::vector<std::string> tutorials_played;

    [[nodiscard]] bool tutorial_seen() const noexcept { return !tutorials_played.empty(); }
    [[nodiscard]] bool tutorial_played(std::string_view name) const;

    friend bool operator==(const PlayerProfile&, const PlayerProfile&) = default;
};

// The profile of a player who has none yet: FoC's defaults (docs/ui/player-profile.md).
[[nodiscard]] PlayerProfile default_profile();

// --- Reading and writing the fields (what each screen calls) -----------------------------

// A name made safe to store and show: invalid UTF-8 and control characters dropped, outer
// spaces trimmed, cut at player_name_max_chars characters. May be empty.
[[nodiscard]] std::string sanitize_name(std::string_view text);

// Stores the sanitised name. Returns false (profile unchanged) when nothing is left of it.
bool set_player_name(PlayerProfile& profile, std::string_view name);
// An empty network name follows the player name again.
void set_network_name(PlayerProfile& profile, std::string_view name);
void set_lobby_colour(PlayerProfile& profile, int colour);
// The name a screen shows: the player name, else the platform's user name, else "Player".
[[nodiscard]] std::string effective_player_name(const PlayerProfile& profile, std::string_view platform_name);
// The lobby name: the network name, else the effective player name.
[[nodiscard]] std::string effective_network_name(const PlayerProfile& profile, std::string_view platform_name);
// Write-back on Start (skirmish setup): the choices are clamped like a loaded file.
void record_skirmish_choices(PlayerProfile& profile, const SkirmishChoices& choices);
// A tutorial that was started; each name is kept once.
void mark_tutorial_played(PlayerProfile& profile, std::string_view name);

// Brings every field into its bounds. Returns the names of the fields that were repaired.
std::vector<std::string> clamp_profile(PlayerProfile& profile);

struct ProfileLoad final {
    PlayerProfile profile;
    // What was repaired or ignored, one line each; empty for a clean file.
    std::vector<std::string> notes;
    // True when the text was empty, not a profile, or another version: the defaults.
    bool defaulted{};
};

[[nodiscard]] std::string serialize_profile(const PlayerProfile& profile);
// Tolerates anything: a missing value keeps its default, a malformed one is dropped with a note.
[[nodiscard]] ProfileLoad parse_profile(std::string_view text);

// The file forms (engine-free; the viewer's ProfileStore only chooses the path). A missing or
// unreadable file is the defaults with `defaulted` set and no note.
[[nodiscard]] ProfileLoad load_profile_file(const platform::Path& file);
[[nodiscard]] bool save_profile_file(const platform::Path& file, const PlayerProfile& profile,
                                     std::string& error);

} // namespace eawr::presentation::ui
