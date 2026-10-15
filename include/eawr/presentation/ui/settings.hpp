#pragma once

// #2927: the player's settings (docs/ui/ui-settings.md): the option set of FoC's options
// dialogs (audio, video, gameplay, network, keyboard) as one versioned record, its text
// form and the clamping that makes a hand-edited or damaged file safe. Settings are UI
// state (UI-C2): they never enter a replay or a hash, and an option that changes a game's
// rules goes through the existing setup and command seams, never into the simulation
// from here. Defaults are FoC's own (docs/ui/ui-settings.md, source column).
//
// The record keeps integers (percent, steps, permille) so the text form round-trips
// exactly and a slider's stop is the stored value.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "eawr/core/diagnostic.hpp"

namespace eawr::presentation::ui {

// The text form's first line is "eawr-settings <version>". A file of another version
// loads as the defaults (no compatibility is kept before 1.0).
inline constexpr std::uint32_t settings_version = 1;

// The volume sliders and their FoC default of 0.75 (ST-A1).
inline constexpr int volume_default_percent = 75;
// Game speed has five steps (TM-01); scroll speed has ten stops over 0..3 (ST-G2).
inline constexpr int speed_step_count = 5;
inline constexpr int speed_step_default = 2;
inline constexpr int scroll_stop_count = 10;
inline constexpr int scroll_stop_default = 3; // 3/9 of 3.0 = 1.0
inline constexpr double scroll_speed_maximum = 3.0;
// The basic detail slider is 0..3 (ST-V3).
inline constexpr int detail_level_maximum = 3;
inline constexpr int detail_level_default = 2;
// The advanced page's eleven detail values; -1 follows the basic level (ST-V4).
inline constexpr std::size_t detail_slot_count = 11;
inline constexpr int detail_automatic = -1;
// The network port FoC treats as "use the default" (ST-N1).
inline constexpr int default_network_port = 1234;
// A window or mode size outside this is not a size (the viewer's own window-size bound).
inline constexpr int display_size_maximum = 16384;
inline constexpr int display_size_minimum = 320;

// The render profile, a project-authored setting (ST-P1): the retail look or the enhanced
// look (anti-aliasing and anisotropic filtering). `automatic` is the run's own choice.
enum class RenderProfileSetting : std::uint8_t { automatic, retail, enhanced };

struct AudioSettings final {
    int master_percent{volume_default_percent};
    int sfx_percent{volume_default_percent};
    int speech_percent{volume_default_percent};
    int music_percent{volume_default_percent};
    bool subtitles{true};

    friend bool operator==(const AudioSettings&, const AudioSettings&) = default;
};

struct VideoSettings final {
    // Full-screen mode and windowed size are separate choices, as in FoC (ST-V1).
    int fullscreen_width{800};
    int fullscreen_height{600};
    int window_width{800};
    int window_height{600};
    bool windowed{false};
    bool vsync{false};
    int detail_level{detail_level_default};
    // GM-04: project texture detail floor, independent of the retail detail presets.
    int texture_mip_drop{};
    int gamma_permille{1000};
    bool widescreen{false};
    bool hardware_cursor{true};
    std::array<int, detail_slot_count> detail{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    RenderProfileSetting render_profile{RenderProfileSetting::automatic};

    friend bool operator==(const VideoSettings&, const VideoSettings&) = default;
};

struct GameplaySettings final {
    int tactical_speed{speed_step_default};
    int strategic_speed{speed_step_default};
    int scroll_stop{scroll_stop_default};
    bool absolute_scrolling{true};
    bool delay_tooltips{false};
    bool right_button_scroll{false};
    bool both_buttons_rotate{false};
    bool alternate_mouse{false};
    bool simplified_faction_colours{false};
    bool abilities_autofire{true};
    bool ask_auto_resolve{false};

    [[nodiscard]] double scroll_speed() const noexcept;

    friend bool operator==(const GameplaySettings&, const GameplaySettings&) = default;
};

struct NetworkSettings final {
    int port{default_network_port};
    bool allow_nat{true};
    bool allow_firewall{true};
    bool allow_upnp{true};
    int quickmatch_default{1}; // 0 campaign, 1 land, 2 space
    int preferred_faction{0};  // 0 random, then the playable factions in data order
    int connection_speed{0};   // 0 modem, 1 broadband
    int lobby_locale{-1};      // -1 follows the game language
    // Project online service. Empty ships unconfigured; LAN retains its own settings.
    std::string online_host;
    int online_port{27020};
    std::string online_trust{"pin"}; // pin, ca-file, or operating-system trust
    std::string online_pin;
    std::string online_ca_file;
    bool online_relay_only{};

    [[nodiscard]] bool uses_default_port() const noexcept { return port == default_network_port; }

    friend bool operator==(const NetworkSettings&, const NetworkSettings&) = default;
};

// Curated diagnostics contain no entered host, certificate path or pin. An empty host
// is valid for storage but cannot open Online. Malformed trust never falls back.
[[nodiscard]] std::optional<core::Diagnostic> validate_online_settings(const NetworkSettings&, bool require_service = false);

struct PlayerSettings final {
    AudioSettings audio;
    VideoSettings video;
    GameplaySettings gameplay;
    NetworkSettings network;
    // Command name -> key text overrides of the keyboard pages. Empty keeps the project's
    // keys; rebinding is not built yet (ST-K1), so nothing writes here today.
    std::map<std::string, std::string> keys;

    friend bool operator==(const PlayerSettings&, const PlayerSettings&) = default;
};

// Brings numeric fields into their bounds (NaN-free by construction). Online
// service fields use validate_online_settings and fail closed instead of clamping.
// Returns the names of the fields that were out of range.
std::vector<std::string> clamp_settings(PlayerSettings& settings);

struct SettingsLoad final {
    PlayerSettings settings;
    // What was repaired or ignored, one line each (unknown keys, bad values, a version
    // mismatch); empty for a clean file.
    std::vector<std::string> notes;
    // True when the text was empty, not a settings file, or another version: the defaults.
    bool defaulted{};
};

[[nodiscard]] std::string serialize_settings(const PlayerSettings& settings);
// Tolerates anything: a missing value keeps its default, a malformed one is dropped with a
// note, an out-of-range one is clamped with a note.
[[nodiscard]] SettingsLoad parse_settings(std::string_view text);

// The 0..1 gain of a percent slider.
[[nodiscard]] constexpr double volume_gain(const int percent) noexcept {
    return percent <= 0 ? 0.0 : percent >= 100 ? 1.0 : static_cast<double>(percent) / 100.0;
}

// What a battle takes from the settings: the four bus gains (0..1) and the tactical speed step.
// The viewer's audio buses and live session read these, so the mapping has one definition.
struct SessionValues final {
    double master_gain{};
    double sfx_gain{};
    double speech_gain{};
    double music_gain{};
    int speed_step{};
};

[[nodiscard]] constexpr SessionValues session_values(const PlayerSettings& settings) noexcept {
    return {volume_gain(settings.audio.master_percent), volume_gain(settings.audio.sfx_percent),
            volume_gain(settings.audio.speech_percent), volume_gain(settings.audio.music_percent),
            settings.gameplay.tactical_speed};
}

// --- Resolution list ------------------------------------------------------------------------

struct DisplaySize final {
    int width{};
    int height{};

    friend bool operator==(const DisplaySize&, const DisplaySize&) = default;
    friend bool operator<(const DisplaySize& a, const DisplaySize& b) {
        return a.width != b.width ? a.width < b.width : a.height < b.height;
    }
};

// The sizes the video page lists (ST-V2): the project's table of common sizes that fit the
// screen, then the screen's own size and `extras` (the window's current size and the sizes the
// settings hold), each once, ascending. The list is project-authored: the window system, not
// the game, owns the display modes here.
[[nodiscard]] std::vector<DisplaySize> resolution_choices(DisplaySize screen, std::span<const DisplaySize> extras);

// --- What each retail control does today -----------------------------------------------------

// A retail options control (by its resource id) and whether this build honours it. A control
// that is not honoured shows disabled in the screen, with `reason` as its tooltip.
struct OptionStatus final {
    std::string_view control;
    bool honoured{};
    std::string_view reason; // empty when honoured
};

// Null for a control the options pages do not list (labels, groups, buttons).
[[nodiscard]] const OptionStatus* find_option_status(std::string_view control) noexcept;
[[nodiscard]] std::span<const OptionStatus> option_statuses() noexcept;

// The key the project binds to a retail keyboard-page command, shown read-only on the
// keyboard pages. Empty when the project has no key for the command (ST-K2).
[[nodiscard]] std::string_view project_key_for(std::string_view edit_control) noexcept;

} // namespace eawr::presentation::ui
