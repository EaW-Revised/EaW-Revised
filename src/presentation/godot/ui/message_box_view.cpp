#include "ui/message_box_view.hpp"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <algorithm>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] String godot_text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

// A key from the text database, or the literal as written.
[[nodiscard]] String resolved(data::ui::TextLookup* text, const std::string& value) {
    if (text == nullptr || value.empty()) return godot_text(value);
    return godot_text(data::ui::to_utf8(text->text(value)));
}

[[nodiscard]] const BuiltGadget* gadget_named(const BuiltDialog& built, const std::string& id) {
    for (const BuiltGadget& gadget : built.gadgets) {
        if (gadget.id == id) return &gadget;
    }
    return nullptr;
}

} // namespace

void EawrMessageBox::_bind_methods() {
    ADD_SIGNAL(MethodInfo("result_chosen", PropertyInfo(Variant::INT, "result")));
}

String EawrMessageBox::line_text(const std::string& control) const {
    const auto* found = gadget_named(built_, control);
    const auto* label = found ? Object::cast_to<EawrUiLabel>(found->control) : nullptr;
    return label ? label->get_text() : String();
}

bool EawrMessageBox::open(Node& parent, const MessageBoxContext& context, const model::MessageBoxRequest& request) {
    plan_ = model::plan_message_box(request);
    if (!plan_.valid()) {
        failure_ = plan_.problem;
        return false;
    }
    if (context.catalog == nullptr || context.theme_model == nullptr) {
        failure_ = "the box has no dialog catalogue or theme";
        return false;
    }
    const data::ui::Dialog* dialog = context.catalog->script.find(plan_.dialog);
    if (dialog == nullptr) {
        failure_ = "dialog " + plan_.dialog + " is not in the catalogue";
        return false;
    }
    built_ = build_dialog(*dialog, *context.theme_model, context.text, model::Placement::centre);

    // The caption and the lines the request carries; a line the request leaves empty is blanked,
    // because the dialog's gadget holds a placeholder.
    const auto set_label = [&](const std::string& id, const std::string& value, const bool literal = false) {
        if (const BuiltGadget* gadget = gadget_named(built_, id)) {
            if (auto* label = Object::cast_to<EawrUiLabel>(gadget->control))
                label->set_text(literal ? godot_text(value) : resolved(context.text, value));
        }
    };
    set_label(plan_.caption_control, plan_.caption);
    for (const std::string_view id : {"IDC_USER_TEXT_STRING_1", "IDC_USER_TEXT_STRING_2", "IDC_USER_TEXT_STRING_3"}) {
        set_label(std::string(id), {});
    }
    for (const auto& [id, value] : plan_.lines) set_label(id, value,
        std::find(plan_.literal_line_controls.begin(), plan_.literal_line_controls.end(), id) != plan_.literal_line_controls.end());

    for (const model::PlannedBoxButton& planned : plan_.buttons) {
        const BuiltGadget* gadget = gadget_named(built_, planned.control);
        auto* button = gadget != nullptr ? Object::cast_to<EawrUiButton>(gadget->control) : nullptr;
        if (button == nullptr) {
            failure_ = "dialog " + plan_.dialog + " has no button " + planned.control;
            return false;
        }
        button->set_text(resolved(context.text, planned.text));
        button->connect("pressed", callable_mp(this, &EawrMessageBox::choose).bind(static_cast<int64_t>(planned.result)));
    }

    set_process_input(true);
    set_layer(layer_index);
    set_name(godot_text(plan_.dialog));
    modal_ = memnew(EawrUiModalLayer);
    modal_->set_position(Vector2());
    modal_->set_size(context.viewport_size);
    modal_->set_theme(context.theme);
    add_child(modal_);
    modal_->add_child(built_.frame);
    parent.add_child(this);
    return true;
}

Control* EawrMessageBox::button(const std::string& control) const {
    const BuiltGadget* gadget = gadget_named(built_, control);
    return gadget != nullptr ? gadget->control : nullptr;
}

void EawrMessageBox::choose(const int64_t result) {
    if (chosen_) return;
    chosen_ = true;
    emit_signal("result_chosen", result);
    queue_free();
}

void EawrMessageBox::_input(const Ref<InputEvent>& event) {
    if (chosen_ || modal_ == nullptr) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (key == nullptr || !key->is_pressed() || key->is_echo() || key->get_keycode() != KEY_ESCAPE) return;
    // Esc is the box's whether or not it has a cancel button: it never reaches the world.
    if (Viewport* viewport = get_viewport()) viewport->set_input_as_handled();
    if (const std::optional<int> result = plan_.escape_result()) choose(*result);
}

void register_message_box_classes() { GDREGISTER_CLASS(EawrMessageBox); }

} // namespace eawr::presentation::godot_backend
