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

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
