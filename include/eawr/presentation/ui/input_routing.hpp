#pragma once

#include <cstdint>

// UI-07 (#304, docs/ui/ui-layer.md §3.3, rules UI-I1 to UI-I3): what the world layer may do
// with an input event. The engine's GUI sees every event first (modal dialog, then HUD); the
// world (selection and orders, #82) and then the camera take only what the GUI left unhandled,
// filtered by this policy. Engine free, so the rules are testable headless; the Godot host
// translates its events into these classes.
namespace eawr::presentation::ui {

enum class InputClass : std::uint8_t {
    key = 0,            // any key press, repeat or release
    button_press = 1,   // a mouse button goes down
    button_release = 2, // a mouse button goes up
    wheel = 3,          // a wheel detent
    motion = 4,         // pointer motion
};

struct InputFocus {
    bool modal_open{}; // a modal dialog is up: it blocks the world
    bool text_focus{}; // an edit box has keyboard focus (UI-I3)
    friend constexpr bool operator==(const InputFocus&, const InputFocus&) noexcept = default;
};

// Mouse buttons the world holds: a press the world took sets its bit; its release clears it.
// While any bit is set the world owns the pointer: motion and the matching release reach the
// world before the GUI, even over the HUD, as a retail drag, scroll or rotate keeps its state
// over the command bar. Buttons are 1-based (Godot's MouseButton values 1 to 9).
class WorldPointerCapture final {
public:
    void pressed(std::uint32_t button) noexcept;
    void released(std::uint32_t button) noexcept;
    // Everything is released: the window lost focus or the pointer left it.
    void clear() noexcept { held_ = 0U; }
    [[nodiscard]] bool active() const noexcept { return held_ != 0U; }
    [[nodiscard]] bool holds(std::uint32_t button) const noexcept;

private:
    std::uint32_t held_{};
};

// A modal dialog opening or closing ends every world hold (UI-I1): the host releases its
// pointer capture and cancels the keys, buttons and edge scrolling the world and camera hold,
// as on a window focus loss. Otherwise a key held when the modal opens keeps the camera moving
// (its key-up is blocked by the modal), and a held button keeps sending motion to the world
// before the modal backdrop sees it. A fresh press after the edge starts a new hold, so no hold
// can stick. The host reports the modal state before every event and every frame; only the
// edge counts.
class ModalHoldGuard final {
public:
    // Whether the world's holds must be cancelled now: the modal state differs from the last
    // one reported (initially closed).
    [[nodiscard]] bool changed(bool modal_open) noexcept {
        const bool edge = modal_open != open_;
        open_ = modal_open;
        return edge;
    }
    [[nodiscard]] bool open() const noexcept { return open_; }

private:
    bool open_{};
};

// Before the GUI (the host's _input): whether the world takes this event first because it
// owns the pointer. Only motion and the release of a held button qualify.
[[nodiscard]] bool world_takes_before_gui(
    InputClass input, std::uint32_t button, const WorldPointerCapture& capture) noexcept;

// After the GUI (the host's _unhandled_input): whether the world and camera may act on an
// event the GUI did not consume.
// - A modal dialog blocks keys, presses, wheel and motion.
// - A focused edit box blocks every key, including unbound ones it did not consume (UI-I3).
// - A release always reaches the world, so a hold the world took ends however the pointer
//   moved; handlers ignore releases they did not press.
[[nodiscard]] bool world_accepts(InputClass input, InputFocus focus) noexcept;

} // namespace eawr::presentation::ui
