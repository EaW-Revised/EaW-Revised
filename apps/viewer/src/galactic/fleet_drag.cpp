#include "fleet_drag.hpp"
#include "ui/tactical_hud_internal.hpp"
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/texture_button.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
namespace eawr::presentation::godot_backend {
// #3072: the drag preview is the source's own art (its button texture and its
// visible icon, ship and count children), grabbed where the pointer took it.
Control* EawrFleetDrag::ghost_of(const Control& source, const Vector2 grab,
    const std::optional<std::size_t> count, const bool base_only, const bool draw_base) {
    auto* root = memnew(Control); root->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    if (count) root->set_meta("drag_count", static_cast<std::int64_t>(*count));
    const auto text = count ? std::optional(count.value() >= 2 ? String("x") + String::num_uint64(*count) : String{})
        : std::optional<String>{};
    auto* card = memnew(Control); card->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    root->add_child(card);
    card->set_position(-grab); card->set_size(source.get_size()); card->set_modulate(source.get_modulate());
    const auto add_art = [card](const Ref<Texture2D>& texture, Rect2 rect, Color self, Color tint) {
        if (texture.is_null()) return;
        auto* art = memnew(TextureRect); art->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        art->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE); art->set_stretch_mode(TextureRect::STRETCH_SCALE);
        art->set_texture(texture); art->set_self_modulate(self); art->set_modulate(tint);
        card->add_child(art); art->set_position(rect.position); art->set_size(rect.size);
    };
    if (const auto* button = Object::cast_to<TextureButton>(&source); button && draw_base) {
        // GHUD-S97: drag backing is the normal quad; hover/selection effects do not travel.
        const auto texture = button->get_texture_normal();
        add_art(texture, Rect2({}, source.get_size()), button->get_self_modulate(), Color(1, 1, 1, 1));
    }
    if (const auto* button = Object::cast_to<EawrHudButton>(&source); button && button->shared_cell()) {
        auto* cell = memnew(EawrHudButton);
        cell->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); cell->set_size(source.get_size());
        card->add_child(cell); button->copy_cell_drawing_to(*cell, text, base_only, draw_base);
    }
    for (std::int32_t index = 0; index < source.get_child_count(); ++index) {
        const auto* child = Object::cast_to<Control>(source.get_child(index));
        if (!child || !child->is_visible() || Object::cast_to<EawrFleetDrag>(child) || base_only) continue;
        if (const auto* image = Object::cast_to<TextureRect>(child)) {
            add_art(image->get_texture(), image->get_rect(), image->get_self_modulate(), image->get_modulate());
        } else if (auto* copy = Object::cast_to<Control>(child->duplicate())) {
            copy->set_mouse_filter(Control::MOUSE_FILTER_IGNORE); card->add_child(copy);
            if (auto* label = Object::cast_to<Label>(copy); label && text) label->set_text(*text);
        }
    }
    return root;
}

EawrFleetDrag::EawrFleetDrag() {
    set_mouse_filter(MOUSE_FILTER_STOP); set_focus_mode(FOCUS_NONE);
}
void EawrFleetDrag::source(Begin begin, std::function<void()> click) {
    begin_ = std::move(begin); click_ = std::move(click);
}
void EawrFleetDrag::receiver(Drop drop, std::function<void()> end, Hover hover) {
    receiver_ = true; drop_ = std::move(drop); end_ = std::move(end); hover_ = std::move(hover);
    // Godot's own drag preview draws on the base canvas, under the HUD panels;
    // the receiver draws the ghost above them instead.
    set_z_as_relative(false); set_z_index(100);
}
void EawrFleetDrag::start(Control* ghost) {
    cancel();
    ghost_ = ghost;
    if (ghost_) {
        ghost_->set_name("fleet_drag_art"); add_child(ghost_);
        ghost_->set_position(get_global_transform().affine_inverse().xform(get_global_mouse_position()));
    }
    active_ = true; pointer_ = Vector2(-1, -1); set_process(true);
}
void EawrFleetDrag::lift_source(Control& source, const std::optional<std::size_t> remainder, const bool base_only) {
    Control* rest{};
    if (remainder || base_only) {
        rest = ghost_of(source, {}, remainder, base_only); add_child(rest);
        rest->set_name("fleet_drag_remainder");
        rest->set_position(get_global_transform().affine_inverse().xform(source.get_global_position()));
    }
    lifted_.emplace_back(source.get_instance_id(), rest);
    source.set_meta("drag_art_vacated", true);
    source.set_meta("drag_rest_count", static_cast<std::int64_t>(remainder.value_or(0)));
    // Hide drawing without changing native input ownership, visibility or layout.
    RenderingServer::get_singleton()->canvas_item_set_visible(source.get_canvas_item(), false);
}
void EawrFleetDrag::cancel() {
    for (const auto& [id, rest] : lifted_) {
        if (auto* source = Object::cast_to<Control>(UtilityFunctions::instance_from_id(static_cast<std::int64_t>(id)))) {
            source->set_meta("drag_art_vacated", false);
            RenderingServer::get_singleton()->canvas_item_set_visible(source->get_canvas_item(), source->is_visible());
        }
        if (rest) rest->queue_free();
    }
    lifted_.clear();
    if (ghost_) { ghost_->queue_free(); ghost_ = nullptr; }
    active_ = false; set_process(false);
}
bool EawrFleetDrag::_has_point(const Vector2& point) const {
    if (!receiver_) return Rect2({}, get_size()).has_point(point);
    return active_ && get_viewport()->gui_is_dragging();
}
Variant EawrFleetDrag::_get_drag_data(const Vector2& point) {
    if (!begin_ || double_release_) return {};
    const auto id = begin_(get_global_transform().xform(point));
    if (!id) return {};
    dragged_ = true;
    return static_cast<std::int64_t>(*id);
}
bool EawrFleetDrag::_can_drop_data(const Vector2& point, const Variant& data) const {
    return active_ && drop_ && data.get_type() == Variant::INT
        && drop_(static_cast<std::uint64_t>(static_cast<std::int64_t>(data)), get_global_transform().xform(point), false);
}
void EawrFleetDrag::_drop_data(const Vector2& point, const Variant& data) {
    if (_can_drop_data(point, data))
        static_cast<void>(drop_(static_cast<std::uint64_t>(static_cast<std::int64_t>(data)), get_global_transform().xform(point), true));
}
void EawrFleetDrag::_gui_input(const Ref<InputEvent>& event) {
    accept_event(); // Rejected drags and clicks belong to the GUI too.
    const auto* button = event.is_valid() ? Object::cast_to<InputEventMouseButton>(event.ptr()) : nullptr;
    if (button && button->get_button_index() == MOUSE_BUTTON_RIGHT) {
        if (button->is_pressed() && right_) right_(get_global_transform().xform(button->get_position()));
        return;
    }
    if (!button || button->get_button_index() != MOUSE_BUTTON_LEFT || !click_) return;
    if (button->is_pressed()) {
        dragged_ = false; double_release_ = button->is_double_click();
        if (double_release_) click_();
    } else {
        if (!double_release_ && !dragged_ && _has_point(button->get_position())) click_();
        double_release_ = false;
    }
}
void EawrFleetDrag::_notification(int what) {
    if (what == NOTIFICATION_EXIT_TREE) {
        cancel();
    } else if (what == NOTIFICATION_DRAG_END) {
        if (receiver_ && end_) end_();
        cancel();
    } else if (what == NOTIFICATION_PROCESS && active_) {
        for (const auto& [id, rest] : lifted_) {
            if (auto* source = Object::cast_to<Control>(UtilityFunctions::instance_from_id(static_cast<std::int64_t>(id)))) {
                RenderingServer::get_singleton()->canvas_item_set_visible(source->get_canvas_item(), false);
                if (rest) {
                    rest->set_visible(source->is_visible_in_tree());
                    rest->set_position(get_global_transform().affine_inverse().xform(source->get_global_position()));
                }
            } else if (rest) rest->hide();
        }
        const auto point = get_global_mouse_position();
        if (point == pointer_) return;
        pointer_ = point;
        if (ghost_) ghost_->set_position(get_global_transform().affine_inverse().xform(point));
        // #3072: the pointer's planet shows its empty stack slots as drop targets.
        if (hover_) hover_(point);
    }
}
} // namespace eawr::presentation::godot_backend
