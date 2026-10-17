#pragma once

#include "eawr/presentation/ui/front_end.hpp"
#include "eawr/presentation/ui/load_game.hpp"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace eawr::presentation::godot_backend {

class SkirmishSetupMode;

// The out-of-game front end (`--eawr-front-end`, docs/ui/front-end-shell.md): the main menu, the
// single-player and multiplayer menus, and the navigation between them and the screens drawn
// elsewhere (the skirmish setup screen, the settings screen). The three menus are the catalogue
// dialogs of the loaded data; behaviour follows the gadget ids (presentation/ui/front_end.hpp).
// Presentation only: a battle still starts through the setup screen's existing FixtureOptions.
class FrontEndMode final {
public:
    struct Options final {
        std::filesystem::path capture_path;
        std::filesystem::path report_path;
        std::filesystem::path game_root, mod_root;
        std::string profile;
        // Language of the text database; a start-up setting (UI-T4).
        std::string language{"ENGLISH"};
        // --eawr-front-end-test: the GPU contract's scripted run (clicks, logs, captures, exit).
        bool test{};
        bool online_routes_test{};
        std::filesystem::path settings_file;
        // With `test`: quit from the settings hub without closing it (the shutdown ownership case).
        bool quit_with_options_open{};
        // With `test`: start a battle from the setup screen and, when its end dialog's Continue
        // returns to the setup screen, walk back to the main menu and quit (MNU-45).
        bool battle_round_trip{};
        bool audio{true};
        // `--eawr-attract off`: the plain backdrop whatever GameConstants says.
        bool attract{true};
        // With `test`: press Start on the setup screen, report the attract stopped, and exit.
        bool start_battle{};
        // `--eawr-front-end-test-campaign start`: with `test`, walk the single-player menu to the campaign
        // staging screen, pick the first campaign and start it.
        std::string campaign_test;
        std::filesystem::path save_directory;
        std::function<std::string(const presentation::ui::SavedGameRequest&)> saved_game_launcher;
        // Starts the campaign the staging screen chose (MNU-34). An empty answer accepts it; anything
        // else is the reason it was refused, shown over the screen. Null refuses every start.
        std::function<std::string(const presentation::ui::CampaignStartRequest&)> campaign_launcher;
    };
    [[nodiscard]] static Options from_command_line(const std::filesystem::path& capture_path,
                                                   const std::filesystem::path& game_root,
                                                   const std::filesystem::path& mod_root, const std::string& profile);

    explicit FrontEndMode(Options options);
    ~FrontEndMode();
    FrontEndMode(const FrontEndMode&) = delete;
    FrontEndMode& operator=(const FrontEndMode&) = delete;

    // `setup` must be ready(): the shell draws from its load and opens/closes it.
    [[nodiscard]] bool ready(godot::Node3D& host, SkirmishSetupMode& setup);
    [[nodiscard]] const std::string& failure() const;
    // One frame. An exit code means the viewer should stop with it.
    [[nodiscard]] std::optional<int> process();
    // Persists the final front-end report once; false means a required report could not be written.
    // The host calls this on an explicit stop and on the engine's exit-tree notification.
    [[nodiscard]] bool close();
    // True while a menu, the campaign staging screen or the settings screen is the frame; false while the setup screen (or a
    // battle behind it) is, and its own process() runs.
    [[nodiscard]] bool owns_frame() const;
    // A battle or campaign the player left from the in-game menu is gone: the shell shows its main menu
    // again, whichever screen started the game (the setup screen is left, the campaign shell restored).
    void return_to_main_menu();
    // The host's input and window focus, for the attract's idle clock (MAT-06).
    void input(const godot::Ref<godot::InputEvent>& event);
    void window_activated();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
