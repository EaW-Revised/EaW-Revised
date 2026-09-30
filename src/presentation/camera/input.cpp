#include "eawr/presentation/camera/input.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace eawr::presentation::camera {
namespace {

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool valid_event_type(const EventType type) noexcept {
    switch (type) {
    case EventType::press:
    case EventType::release:
    case EventType::repeat:
        return true;
    }
    return false;
}

} // namespace

void InputReducer::cancel_held_state() noexcept {
    pressed_controls_.clear();
    held_actions_.clear();
}

void InputReducer::clear() noexcept {
    cancel_held_state();
}

void InputReducer::set_focus_eligible(const bool eligible) noexcept {
    if (focus_eligible_ == eligible) return;
    focus_eligible_ = eligible;
    if (!eligible) cancel_held_state();
}

void InputReducer::set_capture_locked(const bool locked) noexcept {
    if (capture_locked_ == locked) return;
    capture_locked_ = locked;
    if (locked) cancel_held_state();
}

core::Result<void> InputReducer::process(const Event& event) {
    if (event.control_id == invalid_control_id || event.action_id == invalid_action_id
        || !valid_event_type(event.type)) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_input_event,
            "camera input events need nonzero control/action IDs and a known event type"));
    }

    // Lifecycle cancellation is authoritative.  Ineligible events are valid
    // normalized input but cannot recreate state until a fresh press follows
    // focus regain or capture unlock.
    if (!input_eligible()) return core::Result<void>::success();

    const auto control_position = std::lower_bound(
        pressed_controls_.begin(), pressed_controls_.end(), event.control_id,
        [](const PressedControl& pressed, const ControlId control_id) {
            return pressed.control_id < control_id;
        });
    const bool control_is_pressed = control_position != pressed_controls_.end()
        && control_position->control_id == event.control_id;

    switch (event.type) {
    case EventType::press:
        if (control_is_pressed) {
            if (control_position->action_id != event.action_id) {
                return core::Result<void>::failure(failure(
                    diagnostic_codes::input_binding_mismatch,
                    "a pressed control cannot change its action until release"));
            }
            // Duplicate presses are idempotent.  OS key-repeat is represented
            // separately, but treating a duplicate press as inert is equally
            // important for deterministic held-action counts.
            return core::Result<void>::success();
        }

        pressed_controls_.insert(control_position,
            PressedControl{event.control_id, event.action_id});

        {
            const auto action_position = std::lower_bound(
                held_actions_.begin(), held_actions_.end(), event.action_id,
                [](const HeldAction& held, const ActionId action_id) {
                    return held.action_id < action_id;
                });
            if (action_position != held_actions_.end()
                && action_position->action_id == event.action_id) {
                ++action_position->control_count;
            } else {
                held_actions_.insert(action_position, HeldAction{event.action_id, 1U});
            }
        }
        return core::Result<void>::success();

    case EventType::release: {
        // A missing release is inert.  This is intentional: focus loss and
        // capture lock already clear all controls, so a late key-up must not
        // produce an error or resurrect a contribution.
        if (!control_is_pressed) return core::Result<void>::success();
        if (control_position->action_id != event.action_id) {
            return core::Result<void>::failure(failure(
                diagnostic_codes::input_binding_mismatch,
                "a release action does not match the pressed control"));
        }

        pressed_controls_.erase(control_position);
        const auto action_position = std::lower_bound(
            held_actions_.begin(), held_actions_.end(), event.action_id,
            [](const HeldAction& held, const ActionId action_id) {
                return held.action_id < action_id;
            });
        if (action_position != held_actions_.end()
            && action_position->action_id == event.action_id) {
            if (action_position->control_count > 1U) {
                --action_position->control_count;
            } else {
                held_actions_.erase(action_position);
            }
        }
        return core::Result<void>::success();
    }

    case EventType::repeat: {
        // Repeat never adds another contribution.  A repeat arriving after a
        // cancellation is simply stale input and remains inert.
        if (!control_is_pressed) return core::Result<void>::success();
        if (control_position->action_id != event.action_id) {
            return core::Result<void>::failure(failure(
                diagnostic_codes::input_binding_mismatch,
                "a repeat action does not match the pressed control"));
        }
        return core::Result<void>::success();
    }
    }

    // The validation switch above makes this unreachable, but retaining an
    // error return keeps the function total if EventType gains a new value.
    return core::Result<void>::failure(failure(diagnostic_codes::invalid_input_event,
        "unknown camera input event type"));
}

bool InputReducer::is_held(const ActionId action_id) const noexcept {
    if (action_id == invalid_action_id) return false;
    const auto action_position = std::lower_bound(
        held_actions_.begin(), held_actions_.end(), action_id,
        [](const HeldAction& held, const ActionId candidate) {
            return held.action_id < candidate;
        });
    return action_position != held_actions_.end() && action_position->action_id == action_id;
}

} // namespace eawr::presentation::camera
