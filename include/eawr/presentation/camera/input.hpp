#pragma once

#include "eawr/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// Engine-independent input state for the tactical camera.
//
// The adapter that owns engine events supplies plain IDs and event kinds to
// this reducer.  This module deliberately has no binding-table, device,
// viewer, or pose knowledge: it only tracks which controls contribute to
// which held actions and applies lifecycle cancellation deterministically.
namespace eawr::presentation::camera {

using ControlId = std::uint32_t;
using ActionId = std::uint32_t;

// Zero is reserved as the invalid/sentinel ID.  No retail binding is implied
// by any value in this module; IDs are supplied by a future project adapter.
inline constexpr ControlId invalid_control_id = 0U;
inline constexpr ActionId invalid_action_id = 0U;

enum class EventType : std::uint8_t {
    press,
    release,
    repeat,
};

struct Event final {
    ControlId control_id{};
    ActionId action_id{};
    EventType type{EventType::press};

    friend bool operator==(const Event&, const Event&) = default;
};

// A compatibility spelling for adapters that prefer an explicit input name.
using InputEvent = Event;
using InputEventType = EventType;

// One unique action in the reducer output.  `control_count` makes the
// two-controls-for-one-action rule observable without exposing mutable state.
// Held actions are sorted by action_id for deterministic output.
struct HeldAction final {
    ActionId action_id{};
    std::size_t control_count{};

    friend bool operator==(const HeldAction&, const HeldAction&) = default;
};

using HeldActionOutput = std::vector<HeldAction>;

namespace diagnostic_codes {
inline constexpr std::string_view invalid_input_event = "EAWR-CAMERA-0101";
inline constexpr std::string_view input_binding_mismatch = "EAWR-CAMERA-0102";
} // namespace diagnostic_codes

class InputReducer final {
public:
    // A newly-created reducer is focused and unlocked, but contains no held
    // controls.  Passing false starts it in an ineligible focus state.
    InputReducer() = default;
    explicit InputReducer(bool focus_eligible) noexcept
        : focus_eligible_(focus_eligible) {}

    // Valid events received while focus is ineligible or capture is locked
    // are ignored.  Invalid IDs/kinds and a control/action mismatch are
    // rejected without mutating the reducer.
    [[nodiscard]] core::Result<void> process(const Event& event);

    // `apply` is an equivalent verb for adapters that process an event stream.
    [[nodiscard]] core::Result<void> apply(const Event& event) {
        return process(event);
    }

    // Losing focus or entering capture lock immediately cancels every held
    // control.  Regaining focus or unlocking only permits a fresh press; it
    // never resurrects the cancelled state.
    void set_focus_eligible(bool eligible) noexcept;
    void set_capture_locked(bool locked) noexcept;

    // Short aliases for adapter lifecycle code.
    void set_focus(bool focused) noexcept { set_focus_eligible(focused); }
    void set_capture_lock(bool locked) noexcept { set_capture_locked(locked); }

    [[nodiscard]] bool focus_eligible() const noexcept { return focus_eligible_; }
    [[nodiscard]] bool capture_locked() const noexcept { return capture_locked_; }
    [[nodiscard]] bool input_eligible() const noexcept {
        return focus_eligible_ && !capture_locked_;
    }

    // The returned reference remains valid until the next successful press,
    // release, or explicit cancellation.  It is always sorted by action ID.
    [[nodiscard]] const HeldActionOutput& held_actions() const noexcept {
        return held_actions_;
    }

    [[nodiscard]] bool is_held(ActionId action_id) const noexcept;
    [[nodiscard]] std::size_t pressed_control_count() const noexcept {
        return pressed_controls_.size();
    }

    // Explicit cancellation is useful to adapters for scene/mode teardown.
    // Lifecycle flags are preserved.
    void clear() noexcept;

private:
    struct PressedControl final {
        ControlId control_id{};
        ActionId action_id{};
    };

    bool focus_eligible_{true};
    bool capture_locked_{false};
    std::vector<PressedControl> pressed_controls_;
    HeldActionOutput held_actions_;

    void cancel_held_state() noexcept;
};

} // namespace eawr::presentation::camera
