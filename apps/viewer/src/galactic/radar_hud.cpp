#include "radar_hud.hpp"
#include <godot_cpp/classes/input_event_mouse_button.hpp>

using namespace godot;
namespace eawr::presentation::godot_backend {
EawrGalacticRadar::EawrGalacticRadar() {
    set_mouse_filter(MOUSE_FILTER_STOP); set_focus_mode(FOCUS_NONE); set_clip_contents(true);
}
void EawrGalacticRadar::set_actions(Click click, std::function<void(bool)> wheel, std::function<void(bool)> release) {
    click_ = std::move(click); wheel_ = std::move(wheel); release_ = std::move(release);
}
void EawrGalacticRadar::_gui_input(const Ref<InputEvent>& event) {
    // WGUI-51/56: blocked actions still belong to the HUD and cannot leak into
    // world picking, including their release/motion/wheel events.
    accept_event();
    if (event.is_null()) return;
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (!button) return;
    const auto index = button->get_button_index();
    if (!button->is_pressed()) {
        if (release_ && (index == MOUSE_BUTTON_RIGHT || index == MOUSE_BUTTON_MIDDLE))
            release_(index == MOUSE_BUTTON_RIGHT);
        return;
    }
    if (!enabled_) return;
    if (index == MOUSE_BUTTON_LEFT && click_) {
        const auto size = get_size(); const auto point = button->get_position();
        // WGUI-90: native canvas Y goes down; radar-local/source Y goes up.
        static_cast<void>(click_({point.x - size.x * 0.5F, size.y * 0.5F - point.y}, size.x, size.y));
    } else if (wheel_ && (index == MOUSE_BUTTON_WHEEL_UP || index == MOUSE_BUTTON_WHEEL_DOWN)) {
        wheel_(index == MOUSE_BUTTON_WHEEL_UP);
    }
}
} // namespace eawr::presentation::godot_backend
