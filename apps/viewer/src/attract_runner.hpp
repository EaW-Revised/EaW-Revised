#pragma once

#include "battle_content.hpp"
#include "map_mode.hpp"

#include "eawr/skirmish/attract.hpp"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class GodotShaderCache;

// The main-menu demo attract battle (docs/behaviour/main-menu-attract.md, MAT-01 to MAT-21; legacy
// #2937): an AI-only space battle on a demo map, run through the same live session, battle
// effects and camera path a skirmish uses, and cycled by the GameConstants timers. The front end
// owns one:
//
//   attract.start(parent);            // when the main menu activates (MAT-01)
//   attract.process(delta, frame);    // every frame, with what the front end shows (MAT-04, MAT-05)
//   attract.key(...) / pointer_moved  // the front end's input (MAT-06, MAT-07)
//   attract.stop();                   // game start, credits, exit (MAT-13): frees the session
//
// The battle draws no HUD and takes no world input (MAT-10, MAT-19), plays no battle sound
// (MAT-18) and shows the map's saved camera, kept slowly alive by AttractCamera (a placeholder
// for FoC's battle camera, #3005; presentation only). With
// Main_Menu_Demo_Attract_Mode off, or no map listed, start() runs nothing: the plain backdrop.
//
// Standalone, `--eawr-attract` hosts it in the viewer on its own (the front end's start and stop
// are then scripted):
//
//   --eawr-attract                      run the attract until the window closes
//   --eawr-attract off                  the plain backdrop, whatever GameConstants says (frame-time
//                                       baseline)
//   --eawr-attract-frames <n>           end after n frames: stop(), report, exit
//   --eawr-attract-do <frame>:<step>    a scripted step, repeatable: key:<backspace|enter|escape|
//                                       minus|other>, click, stop, start, dialog:<on|off>
//   --eawr-report <json>, --eawr-capture <png>, --eawr-game-root, --eawr-mod-root, --eawr-profile
class AttractRunner final {
public:
    struct Options final {
        std::filesystem::path game_root;
        std::filesystem::path mod_root;
        std::string profile;
        // The front end's installed-content snapshot; absent, the runner mounts the installation.
        std::shared_ptr<const BattleContent> content;
        std::shared_ptr<GodotShaderCache> shaders;
        // Standalone runs.
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        bool forced_off{};
        std::uint32_t frames{};
        std::vector<std::string> steps;
    };

    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit AttractRunner(Options options);
    ~AttractRunner();
    AttractRunner(const AttractRunner&) = delete;
    AttractRunner& operator=(const AttractRunner&) = delete;

    // MAT-01, MAT-02, MAT-03: reads GameConstants, initialises the clocks and starts the first map
    // as a child of `parent` (which must outlive the runner or be stop()ped before it goes). True
    // when a battle runs; false for the plain backdrop (attract off, no map) or a failed start,
    // which failure() describes. An attract already running is left as it is.
    [[nodiscard]] bool start(godot::Node& parent);
    // MAT-13: frees the battle and its session, then forgets the clocks. Safe to call twice.
    void stop();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const std::string& failure() const noexcept;

    // MAT-04, MAT-05: one frame. `dialog_active` and `main_menu_active` say what the front end
    // shows; `foreground` whether the window is in front.
    void process(double delta, bool dialog_active = false, bool main_menu_active = false, bool foreground = true);

    // MAT-06, MAT-07: the front end's input. True when the attract took the key.
    [[nodiscard]] bool key(skirmish::AttractKey pressed, bool dialog_active = false);
    void pointer_moved(bool main_menu_active = false);
    void window_activated();
    // What the front end shows, for the events input() gets (MAT-07: with a dialog up the keys are not
    // the attract's). process() and key() take it per call; input() reads it from here.
    void set_front_end(bool dialog_active, bool main_menu_active) noexcept;
    // Any other event reaches the battle's world, which rejects it (MAT-10).
    void input(const godot::Ref<godot::InputEvent>& event);

    // Standalone: one frame with the scripted steps. Returns the exit code once the run has ended
    // and its report is written.
    [[nodiscard]] std::optional<int> standalone_frame(double delta, godot::Node& parent);

    [[nodiscard]] MapMode::AttractStatus status();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
