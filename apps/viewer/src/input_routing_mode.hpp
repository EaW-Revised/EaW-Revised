#pragma once

#include "eawr/presentation/ui/input_routing.hpp"

#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>

#include <filesystem>
#include <memory>
#include <optional>

namespace eawr::presentation::godot_backend {

// A self-contained viewer mode (`--eawr-input-routing`, UI-07 #304) that checks input routing
// through Godot's real event dispatch. It builds a synthetic HUD (a faceplate whose left half
// is opaque, two component buttons on its transparent half), an edit box and a modal layer,
// injects scripted pointer and key events with Input::parse_input_event, and records what
// reached the world layer: the host's _unhandled_input after the routing policy. World and HUD
// orders go through one OrderInput and CommandSink, so the report carries both command
// streams. The world layer keeps the keys and buttons it holds as the camera adapter does, so
// the report shows whether a hold survives a modal. No game data is read. `--eawr-report
// <path>` writes the report; the exit code is 0 when every rule held.
class InputRoutingMode final {
public:
    struct Options final {
        std::filesystem::path report_path;
    };

    [[nodiscard]] static bool requested();
    [[nodiscard]] static Options from_command_line();

    explicit InputRoutingMode(Options options);
    ~InputRoutingMode();
    InputRoutingMode(InputRoutingMode&&) noexcept;
    InputRoutingMode& operator=(InputRoutingMode&&) noexcept;
    InputRoutingMode(const InputRoutingMode&) = delete;
    InputRoutingMode& operator=(const InputRoutingMode&) = delete;

    [[nodiscard]] bool ready(godot::Node3D& host);
    [[nodiscard]] std::optional<int> process();

    // The world layer. The host calls it for every event the policy let through, before the
    // camera; a true return consumes the event.
    bool world_input(const godot::Ref<godot::InputEvent>& event);
    // The host dropped an event under the policy (modal or text focus).
    void world_dropped(presentation::ui::InputClass input);
    // The host cancelled held world input (a modal opened or closed, or an edit box took
    // focus): the world releases every key and button it holds, as the camera adapter does.
    void world_cancelled();

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
