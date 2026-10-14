#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode (`--eawr-ui-gallery`, UI-06 #229) that builds
// the UI kit theme from the FoC data (GUIDialogs.xml skin, the MT_CommandBar
// atlas, fonts from the UI-05 cache), draws every kit control in its states,
// and writes one capture plus a report. With `--eawr-ui-dialog <IDD_NAME>` it
// builds that catalogue dialog instead, centred as UI-L4 places it; with
// `--eawr-ui-rules retail` it lays out by the retail rules, for side-by-side
// captures against the original game; with `--eawr-mod-root <mod>` a mod's
// data mounts over FoC; with `--eawr-ui-movie <name> --eawr-movie-cache <dir>`
// it plays that HUD movie's converted Theora entry (#237) instead; with
// `--eawr-ui-hud <empire|rebel|underworld>` it draws the tactical HUD shell
// (P2-20a, #83) at the window's size, its planet named by
// `--eawr-ui-hud-map <logical path>` (default _mp_space_coruscant). The font
// cache is found as in `--eawr-fonts`: `--eawr-font-cache <dir>`,
// EAWR_FONT_CACHE, then the checkout's out/fonts.
class UiGalleryMode final {
public:
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::filesystem::path font_cache;
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        std::string dialog;
        std::string movie;
        std::filesystem::path movie_cache;
        std::string rules{"aspect"};
        std::string hud;
        std::string hud_map{"data/art/maps/_mp_space_coruscant.ted"};
        // #425 --eawr-ui-hud-cards: a made-up selection for the HUD page's unit cards, comma-separated
        // <type>[*<count>][:<ABILITY>][@<health>][~<shield>] (for example TIE_Fighter*30:HUNT@0.5).
        std::string hud_cards;
        std::string hud_readouts; // HCB-61/62: bounded prepared-state presentation probes.
        std::string hud_flytext; // HCB-64: caller-prepared bounty feedback states.
        // MNU-42 `--eawr-message-box <none|one|two|three|small|quit>[-centred]`: the runtime message box and
        // its scripted clicks (ui_gallery_box.cpp).
        std::string message_box;
        // MNU-39/41 `--eawr-load-game <script|saves|live>`: the Load Game screen, scripted over the
        // recordings (a self-checking run), scripted over the saved games, or left open until Cancel
        // or Play. `--eawr-replay-store <dir>` names the replay store (default: the user data
        // directory's replays folder), `--eawr-save-dir <dir>` the save directory (default: its saves
        // folder) and `--eawr-load-launcher <none|accept|refuse>` the stand-in for the restore hand-off
        // (none: Load on a saved game stays off; accept: records the request; refuse: answers with a
        // fixed reason).
        std::string load_game;
        std::filesystem::path replay_store;
        // The finished recording fixture for the focused MNU-41 GPU flow.
        std::filesystem::path save_replay_fixture;
        std::filesystem::path save_directory;
        std::string load_launcher{"none"};
        // MNU-07 `--eawr-credits <script|script-click|live>`: the credits screen over the movie in
        // `--eawr-movie-cache`; script skips with Esc (or a click) after a few seconds, live stays open
        // until it closes.
        std::string credits;
        // Set by the host when a Play was refused: the page opens with a box that says why.
        std::string notice;
    };

    // A recording the Load Game screen chose to play: the host starts the replay mode with it.
    struct ReplayRequest final {
        std::filesystem::path path;
        std::string map; // logical map path, empty when the recording has none
    };

    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit UiGalleryMode(Options options);
    ~UiGalleryMode();
    UiGalleryMode(UiGalleryMode&&) noexcept;
    UiGalleryMode& operator=(UiGalleryMode&&) noexcept;
    UiGalleryMode(const UiGalleryMode&) = delete;
    UiGalleryMode& operator=(const UiGalleryMode&) = delete;

    // Loads the data and builds the page. A false return has written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once the report has been written.
    [[nodiscard]] std::optional<int> process();
    // The recording Play chose; once taken the page is closed and the host owns what comes next.
    [[nodiscard]] std::optional<ReplayRequest> take_replay_request();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
