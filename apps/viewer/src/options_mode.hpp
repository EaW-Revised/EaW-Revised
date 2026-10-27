#pragma once

#include <godot_cpp/classes/node3d.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode (`--eawr-options`, #2927) that shows the options screen
// (options_screen.hpp) over a black backdrop with the FoC data's skin.
//
//   --eawr-options                       open the hub; the window stays until the screen closes
//   --eawr-options-page <page>           start on hub, audio, video, performance, advanced, gameplay, keyboard
//                                        or network
//   --eawr-options-context main|game     the main menu's hub or the in-game menu's (default main)
//   --eawr-options-do <action>           a scripted step, repeatable, one per frame, in order:
//                                          page:<page>            open a page from the saved settings
//                                          set:<IDC_ID>=<number>  a slider value, a check 0/1, a list index
//                                          press:<IDC_ID>         a click on a button
//                                          snapshot               record the page's controls
//   --eawr-settings-file <path>          the settings file (default: the user data folder's)
//   --eawr-ui-rules aspect|retail        the layout rules, as the UI gallery's
//   --eawr-capture <png>, --eawr-report <json>
//
// A scripted or captured run ends by itself after its last step: it writes the PNG and the
// report (the saved settings, the pages' controls, what the engine took) and exits.
class OptionsMode final {
public:
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::filesystem::path font_cache;
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        std::filesystem::path settings_file;
        std::string page{"hub"};
        std::string context{"main"};
        std::string rules{"aspect"};
        std::vector<std::string> actions;
    };

    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit OptionsMode(Options options);
    ~OptionsMode();
    OptionsMode(OptionsMode&&) noexcept;
    OptionsMode& operator=(OptionsMode&&) noexcept;
    OptionsMode(const OptionsMode&) = delete;
    OptionsMode& operator=(const OptionsMode&) = delete;

    // Loads the data and opens the screen. A false return has written the report.
    [[nodiscard]] bool ready(godot::Node3D& host);
    // One frame; returns the exit code once the report has been written.
    [[nodiscard]] std::optional<int> process();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
