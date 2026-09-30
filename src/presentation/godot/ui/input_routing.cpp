#include "input_routing.hpp"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>

namespace eawr::presentation::godot_backend {

using namespace godot;
using presentation::ui::InputClass;

EawrUiHitMask::EawrUiHitMask() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    // Godot passes wheel events on through STOP controls by default; over the HUD they stop.
    set_force_pass_scroll_events(false);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

bool EawrUiHitMask::_has_point(const Vector2& point) const {
    return hit_test_ && hit_test_(point.x, point.y);
}

EawrUiModalLayer::EawrUiModalLayer() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_force_pass_scroll_events(false);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
    add_to_group(group);
}

std::optional<InputClass> input_class(const Ref<InputEvent>& event, std::uint32_t& button) {
    button = 0U;
    if (event.is_null()) return std::nullopt;
    if (Object::cast_to<InputEventKey>(event.ptr()) != nullptr) return InputClass::key;
    if (const auto* mouse = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        switch (mouse->get_button_index()) {
        case MOUSE_BUTTON_WHEEL_UP:
        case MOUSE_BUTTON_WHEEL_DOWN:
        case MOUSE_BUTTON_WHEEL_LEFT:
        case MOUSE_BUTTON_WHEEL_RIGHT: return InputClass::wheel;
        default: break;
        }
        button = static_cast<std::uint32_t>(mouse->get_button_index());
        return mouse->is_pressed() ? InputClass::button_press : InputClass::button_release;
    }
    if (Object::cast_to<InputEventMouseMotion>(event.ptr()) != nullptr) return InputClass::motion;
    return std::nullopt;
}

bool is_text_control(const Control* control) {
    return control != nullptr
        && (Object::cast_to<LineEdit>(const_cast<Control*>(control)) != nullptr
            || Object::cast_to<TextEdit>(const_cast<Control*>(control)) != nullptr);
}

presentation::ui::InputFocus input_focus(Viewport& viewport) {
    presentation::ui::InputFocus focus;
    focus.text_focus = is_text_control(viewport.gui_get_focus_owner());
    if (SceneTree* tree = viewport.get_tree()) {
        const TypedArray<Node> modals = tree->get_nodes_in_group(EawrUiModalLayer::group);
        for (int64_t index = 0; index < modals.size() && !focus.modal_open; ++index) {
            const auto* layer = Object::cast_to<CanvasItem>(modals[index]);
            focus.modal_open = layer != nullptr && layer->is_visible_in_tree();
        }
    }
    return focus;
}

void register_ui_input_classes() {
    GDREGISTER_CLASS(EawrUiHitMask);
    GDREGISTER_CLASS(EawrUiModalLayer);
}

} // namespace eawr::presentation::godot_backend
