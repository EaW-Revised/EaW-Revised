#include "eawr/presentation/ui/input_routing.hpp"

namespace eawr::presentation::ui {
namespace {

constexpr std::uint32_t max_button = 31U;

[[nodiscard]] constexpr std::uint32_t bit(const std::uint32_t button) noexcept {
    return button == 0U || button > max_button ? 0U : 1U << (button - 1U);
}

} // namespace

void WorldPointerCapture::pressed(const std::uint32_t button) noexcept { held_ |= bit(button); }

void WorldPointerCapture::released(const std::uint32_t button) noexcept { held_ &= ~bit(button); }

bool WorldPointerCapture::holds(const std::uint32_t button) const noexcept {
    return (held_ & bit(button)) != 0U;
}

bool world_takes_before_gui(
    const InputClass input, const std::uint32_t button, const WorldPointerCapture& capture) noexcept {
    switch (input) {
    case InputClass::motion: return capture.active();
    case InputClass::button_release: return capture.holds(button);
    case InputClass::key:
    case InputClass::button_press:
    case InputClass::wheel: return false;
    }
    return false;
}

bool world_accepts(const InputClass input, const InputFocus focus) noexcept {
    switch (input) {
    case InputClass::button_release: return true;
    case InputClass::key: return !focus.modal_open && !focus.text_focus;
    case InputClass::button_press:
    case InputClass::wheel:
    case InputClass::motion: return !focus.modal_open;
    }
    return false;
}

} // namespace eawr::presentation::ui
