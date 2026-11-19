#pragma once

// #2927: the player's settings (docs/ui/ui-settings.md): the option set of FoC's options
// dialogs (audio, video, gameplay, network, keyboard) as one versioned record, its text
// form and the clamping that makes a hand-edited or damaged file safe. Settings are UI
// state (UI-C2): they never enter a replay or a hash, and an option that changes a game's
// rules goes through the existing setup and command seams, never into the simulation
// from here. Defaults and deliberate project choices are in docs/ui/ui-settings.md.
//
// The record keeps integers (percent, steps, permille) so the text form round-trips
// exactly and a slider's stop is the stored value.

#include "eawr/presentation/ui/render_thread_setting.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include "eawr/core/diagnostic.hpp"

namespace eawr::presentation::ui {

// The channel sliders keep FoC's 0.75; ST-A1 lowers the fresh master at the owner's request.
inline constexpr int volume_default_percent = 75;
inline constexpr int master_default_percent = 37;
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
// The advanced page's heat-distortion check and particle detail slider (#6560).
inline constexpr std::size_t detail_slot_heat = 5;
inline constexpr std::size_t detail_slot_particle = 7;
// The network port FoC treats as "use the default" (ST-N1).
inline constexpr int default_network_port = 1234;
// A window or mode size outside this is not a size (the viewer's own window-size bound).
inline constexpr int display_size_maximum = 16384;
inline constexpr int display_size_minimum = 320;

// AA quality (ST-P1): four levels; automatic keeps the run's launch default.
// retail is explicit Off, serialized as "off" to distinguish old saved "retail" values.
enum class RenderProfileSetting : std::uint8_t { automatic, retail, low, medium, high };

inline constexpr std::array supersampling_scales{1.0F, 1.5F, 2.0F};

struct AudioSettings final {
    int master_percent{master_default_percent};
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
    RenderThreadSetting render_thread{RenderThreadSetting::separate}; // Applied at the next launch.
    int particle_threads{}; // 0 selects the existing automatic rule.
    int simulation_threads{}; // 0 selects the existing automatic rule.
    bool depth_prepass{true}; // Read once before renderer startup.
    bool vsync{true}; // ST-V5: owner-requested FIFO default; stored choices stay unchanged.
    // ST-P2: owner-requested render limit; 0 is Off, -1 follows the display.
    int frame_rate_limit{120};
    int detail_level{detail_level_default};
    // GM-04: project texture detail floor, independent of the retail detail presets.
    int texture_mip_drop{};
    int gamma_permille{1000};
    bool widescreen{false};
    bool hardware_cursor{true};
    std::array<int, detail_slot_count> detail{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    // Supersampling is independent of AA: Off (native), 1.5x, 2x.
    int supersampling{};
    RenderProfileSetting render_profile{RenderProfileSetting::automatic};

    friend bool operator==(const VideoSettings&, const VideoSettings&) = default;
};

// #6560: the particle detail slider's levels 0..3 as the global draw quality. This is the
// project's own scale, not a retail mapping (no retail capture backs it). Automatic keeps
// the full look (1.0), whatever the basic detail level is.
inline constexpr std::array<float, 4> particle_detail_levels{0.4F, 0.6F, 0.8F, 1.0F};

[[nodiscard]] constexpr float particle_global_detail(const VideoSettings& video) noexcept {
    const int level = video.detail[detail_slot_particle];
    if (level == detail_automatic) return particle_detail_levels.back();
    return particle_detail_levels[static_cast<std::size_t>(
        level < 0 ? 0 : (level > detail_level_maximum ? detail_level_maximum : level))];
}

// Heat distortions: automatic keeps them on (today's look); a stored value is on when above zero.
[[nodiscard]] constexpr bool particle_heat_enabled(const VideoSettings& video) noexcept {
    const int value = video.detail[detail_slot_heat];
    return value == detail_automatic || value > 0;
}

// What a battle's particle presentation takes at its start. A launch argument that was given
// wins over the saved setting; without one the saved setting decides.
struct ParticleSettingValues final {
    float global{1.0F};
    bool heat{true};
};

[[nodiscard]] constexpr ParticleSettingValues resolve_particle_values(
    const VideoSettings& video, const std::optional<float> launch_global,
    const std::optional<bool> launch_heat) noexcept {
    return {launch_global.value_or(particle_global_detail(video)),
            launch_heat.value_or(particle_heat_enabled(video))};
}

// A launch or profile count wins; zero leaves the automatic rule in charge.
[[nodiscard]] constexpr std::optional<std::size_t> resolve_thread_override(
    const std::optional<std::size_t> chosen, const int saved) noexcept {
    if (chosen || saved <= 0) return chosen;
    return static_cast<std::size_t>(saved);
}

// ST-P2: presentation pacing only. The live simulation keeps its elapsed-time tick clock.
// `refresh_rate` is the display's rate as reported (59.94, 119.88, 143.98 ...), not a whole number: the limiter
// derived from it is rounded UP, never down. A derived limiter below the real refresh makes the engine's sleep and
// the vsync wait beat against each other (one repeated refresh about every 1.1 s at 119 on a 119.88 Hz mode).
// An explicit cap clearly below the refresh is the player's choice and stays. With vsync on there is always a
// limiter when the cap is Off or at the refresh (the AMD frame-cap rule), also when the refresh is unknown (60).
[[nodiscard]] constexpr int frame_rate_cap(const VideoSettings& video, const double refresh_rate,
                                         const bool menu, const bool focused) noexcept {
    const bool known = refresh_rate >= 30.0 && refresh_rate <= 1000.0;
    const double refresh = known ? refresh_rate : 60.0;
    int ceiling = static_cast<int>(refresh);
    if (static_cast<double>(ceiling) < refresh) ++ceiling;
    int cap = video.frame_rate_limit < 0 ? ceiling : video.frame_rate_limit;
    if (cap > 0 && cap < 30) cap = 30;
    // With vsync on, Off rounds up to the refresh (60 when it is unknown), and so does a cap at (or within 3% of) a
    // known refresh; a cap clearly below it stays the player's cap.
    if (video.vsync && (cap == 0 || (known && static_cast<double>(cap) >= 0.97 * refresh))) cap = ceiling;
    const auto lower = [&](const int limit) { if (cap == 0 || limit < cap) cap = limit; };
    if (menu) lower(60);
    if (!focused) lower(30);
    return cap;
}

struct GameplaySettings final {
    int tactical_speed{speed_step_default};
    int strategic_speed{speed_step_default};
    int scroll_stop{scroll_stop_default};
    bool absolute_scrolling{true};
    bool delay_tooltips{false};
    bool reinforcement_areas_only_while_placing{true}; // PD-01: presentation only.
    bool animated_loading_background{true};
    bool fit_radar_to_galaxy{true}; // WGUI-177 / DEV-9: galactic radar expansion, on by default (the original's fixed frame is the opt-out).
    bool space_pauses_galactic{true}; // DEV-13: off restores Space as the cinematic tour.
    int galactic_build_tray{-1}; // GBT-13: -1 follows the layer's default (untouched), 0 off, 1 on (a player's choice).
    int hover_owner_names{1}; // -1 retains the saved mode-dependent choice (MP on, solo off), 0 off, 1 on.
    bool right_button_scroll{false};
    bool both_buttons_rotate{false};
    bool alternate_mouse{false};
    bool simplified_faction_colours{false};
    bool abilities_autofire{true};
    bool ask_auto_resolve{false};

    [[nodiscard]] double scroll_speed() const noexcept;

    friend bool operator==(const GameplaySettings&, const GameplaySettings&) = default;
};

[[nodiscard]] constexpr bool hover_owner_names_enabled(const GameplaySettings& settings, const bool multiplayer) noexcept {
    return settings.hover_owner_names < 0 ? multiplayer : settings.hover_owner_names != 0;
}

// GBT-13: an explicit player choice wins; an untouched value follows the active layer's default.
[[nodiscard]] constexpr bool galactic_build_tray_enabled(const GameplaySettings& settings,
    const bool layer_default) noexcept {
    return settings.galactic_build_tray < 0 ? layer_default : settings.galactic_build_tray != 0;
}

// PD-01: the saved preference can restrict the data rule, never relax it.
[[nodiscard]] constexpr bool reinforcement_areas_visible(const GameplaySettings& settings,
    const bool data_only_while_dragging, const bool pane_open, const bool placing) noexcept {
    return placing || (pane_open && !settings.reinforcement_areas_only_while_placing && !data_only_while_dragging);
}

struct NetworkSettings final {
    int port{default_network_port};
    bool allow_nat{true};
    bool allow_firewall{true};
    bool allow_upnp{true};
    int quickmatch_default{1}; // 0 campaign, 1 land, 2 space
    int preferred_faction{0};  // 0 random, then the playable factions in data order
    int connection_speed{0};   // 0 modem, 1 broadband
    int lobby_locale{-1};      // -1 follows the game language
    // ST-N2: public online service defaults; LAN retains its own settings.
    std::string online_host{"eaw.ernst.work"};
    int online_port{27020};
    std::string online_trust{"system"}; // pin, ca-file, or operating-system trust
    std::string online_pin;
    std::string online_ca_file;
    bool online_relay_only{};
    // Which of the service's relay (TURN) providers online games use: auto tries the operator's own server first and Cloudflare
    // after it (a fallback for a server that does not answer, not for loss); own or cloudflare keep only that provider.
    std::string online_turn_provider{"auto"};

    [[nodiscard]] bool uses_default_port() const noexcept { return port == default_network_port; }

    friend bool operator==(const NetworkSettings&, const NetworkSettings&) = default;
};

// Curated diagnostics contain no entered host, certificate path or pin. An empty host
// is valid for storage but cannot open Online. Malformed trust never falls back.
[[nodiscard]] std::optional<core::Diagnostic> validate_online_settings(const NetworkSettings&, bool require_service = false);

// SH-03: transient layer defaults; only player overrides are serialized.
struct SelectionActionConfig final {
    std::string default_key;
    std::map<std::string, std::string> faction_events;
    std::map<std::string, std::string> default_faction_events;
    std::map<std::string, std::string> advisor_events;
    bool include_heroes{true}; // Include_Heroes: heroes (and team heroes) join this bulk selection
    friend bool operator==(const SelectionActionConfig&, const SelectionActionConfig&) = default;
};
struct SelectionConfig final {
    bool enabled{true};
    std::array<SelectionActionConfig, 2> actions{{{"Ctrl+Q", {}, {}, {}, true}, {"Q", {}, {}, {}, true}}};
    SelectionActionConfig gift{}; // GIFT_UNITS uses the same advisor mapping, without a keyboard binding.
    friend bool operator==(const SelectionConfig&, const SelectionConfig&) = default;
};

struct PlayerSettings final {
    bool revised_layer_enabled{true};
    std::optional<std::string> revised_layer_path;
    AudioSettings audio;
    VideoSettings video;
    GameplaySettings gameplay;
    NetworkSettings network;
    // ST-K3: optional physical WASD pan; the default keeps retail arrow keys.
    bool camera_wasd{};
    SelectionConfig selection_config;
    // GBT-13: the active layer's Enabled_By_Default for the galactic build tray; filled from the layer, never saved or compared.
    bool galactic_build_tray_layer_default{true};
    bool selection_hotkeys_authored{};
    bool selection_hotkeys{true}; // SH-01: player and layer opt-outs still win.
    // Load provenance, consumed by the XML default migration; not an effective preference.
    bool selection_hotkeys_migration_pending{true};
    // Command name -> key text overrides of the keyboard pages. Empty keeps the project's
    // keys; SH-01 writes only its two feature-specific binding overrides.
    std::map<std::string, std::string> keys;

    friend bool operator==(const PlayerSettings& a, const PlayerSettings& b) {
        return a.revised_layer_enabled == b.revised_layer_enabled && a.revised_layer_path == b.revised_layer_path
            && a.audio == b.audio && a.video == b.video && a.gameplay == b.gameplay && a.network == b.network
            && a.camera_wasd == b.camera_wasd && a.selection_config == b.selection_config
            && a.selection_hotkeys_authored == b.selection_hotkeys_authored
            && a.selection_hotkeys == b.selection_hotkeys && a.keys == b.keys;
    }
};

// SH-01: physical letters optionally use Ctrl; Mouse N chords may use Ctrl/Shift/Alt.
// Chords the command, camera and ability maps already use are reserved.
inline constexpr std::array<std::string_view, 2> selection_commands{"SELECT_OWN_ALL", "SELECT_OWN_SCREEN"};
enum class SelectionChordStatus : std::uint8_t { ok, malformed, reserved, duplicate, camera, mouse_reserved };
// Mouse numbering excludes wheel directions: middle is 3, side buttons start at 4.
[[nodiscard]] std::string selection_mouse_chord_for(int button, bool shift, bool ctrl, bool alt);
[[nodiscard]] std::optional<std::size_t> selection_mouse_action(const PlayerSettings&, int button,
    bool shift, bool ctrl, bool alt, bool meta = false, bool free_camera = false);
// The chord text a pressed physical letter stands for ("" when it is not A-Z).
[[nodiscard]] std::string selection_chord_for(char letter, bool ctrl);
// Shape and reservation only (what an XML default must satisfy).
[[nodiscard]] bool selection_chord_usable(std::string_view chord) noexcept;
// Whether `action` may take `chord`: shape, reservation, and the other action's binding.
[[nodiscard]] SelectionChordStatus selection_chord_status(const PlayerSettings&, std::size_t action,
    std::string_view chord) noexcept;
[[nodiscard]] std::string_view selection_key(const PlayerSettings&, std::size_t action) noexcept;
// Reject reserved/duplicate chords without changing the saved binding.
bool set_selection_key(PlayerSettings&, std::size_t action, std::string_view chord);
// No action while disabled, and exact modifier matching (Meta is never admitted).
[[nodiscard]] std::optional<std::size_t> selection_action(const PlayerSettings&, char physical,
    bool shift, bool ctrl, bool alt, bool meta = false, bool free_camera = false) noexcept;

// GBT-07: the galactic build tray's key, a local binding in Options > Controls (default Tab, rebindable). The
// text is the engine's key name without modifiers ("Tab", "B", "F6"). Keys the galaxy view already uses and the
// selection hotkeys' chords are refused.
inline constexpr std::string_view build_tray_command = "GALACTIC_BUILD_TRAY";
inline constexpr std::string_view build_tray_default_key = "Tab";
// The galaxy view's own unmodified keys (the menu, pause, the overlay, camera home and planet stepping, the camera
// pans and the arrow keys, Enter). tests/galactic/test_build_tray_key_pin.py pins this list against the keys
// GalacticMode::input and the camera handle, so a new galaxy key cannot silently share the tray's binding.
inline constexpr std::array<std::string_view, 15> build_tray_reserved_keys{"Escape", "Space", "F3", "Home", "PageUp",
    "PageDown", "Up", "Down", "Left", "Right", "W", "A", "S", "D", "Enter"};
[[nodiscard]] bool build_tray_key_usable(const PlayerSettings&, std::string_view key) noexcept;
[[nodiscard]] std::string_view build_tray_key(const PlayerSettings&) noexcept;
// Rejects an unusable key without changing the saved binding.
bool set_build_tray_key(PlayerSettings&, std::string_view key);

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

// --- Where the window starts ------------------------------------------------------------------

// A monitor's usable area (the desktop minus taskbars) in desktop pixels. The origin can be negative (a monitor
// left of or above the primary one); sizes are the physical pixels the window system works in, so a DPI scale needs
// no correction here.
struct ScreenArea final {
    int x{};
    int y{};
    int width{};
    int height{};

    friend bool operator==(const ScreenArea&, const ScreenArea&) = default;
};

// What the window system adds around the client area: title bar and borders.
struct FrameInsets final {
    int left{};
    int top{};
    int right{};
    int bottom{};

    friend bool operator==(const FrameInsets&, const FrameInsets&) = default;
};

struct WindowPlacement final {
    int x{};  // client area, desktop pixels
    int y{};
    int width{};
    int height{};

    friend bool operator==(const WindowPlacement&, const WindowPlacement&) = default;
};

// Resolve the transient launcher hint; absent, malformed or disconnected screens keep the current screen.
[[nodiscard]] std::size_t startup_window_screen(std::span<const std::string> arguments, int current, int count);

// Where a window of client size `wanted` goes on screen `screen_index` (clamped into the list; no screens gives
// the wanted size at the origin). The size shrinks to the usable area, frame included, and the whole frame ends up
// on the screen. `keep` is the window's current client position: with it the window only moves as far as needed
// to be fully visible; without it (the start of a run) the window is centred on the screen.
[[nodiscard]] WindowPlacement place_window(DisplaySize wanted, FrameInsets frame, std::span<const ScreenArea> screens,
                                           std::size_t screen_index, std::optional<std::pair<int, int>> keep);

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

struct CameraKeyMove final {
    std::string_view control;
    char retail;
    char wasd;
};
// ST-K3: only bare chords move. Modified ability/selection chords stay unchanged.
inline constexpr std::array<CameraKeyMove, 4> camera_key_moves{{
    {"IDC_EDIT_WAYPOINT", 'W', 'U'}, {"IDC_EDIT_ATTACK", 'A', 'X'},
    {"IDC_EDIT_STOP", 'S', 'V'}, {"IDC_EDIT_PREV_UNIT", 'D', 'H'},
}};
[[nodiscard]] std::string_view project_key_for(std::string_view edit_control, bool camera_wasd) noexcept;
// A bare physical letter to the existing command's letter. Zero is reserved for pan.
[[nodiscard]] char camera_command_key(char physical, bool camera_wasd) noexcept;

} // namespace eawr::presentation::ui
