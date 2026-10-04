#include "ability_buttons_view.hpp"

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

// ABE-5: the press flash (Selected_Texture_Name) fades out over 0.2 s.
constexpr double flash_seconds = 0.2;

[[nodiscard]] String reason_text(const std::string_view reason) {
    return reason.empty() ? String{} : String::utf8(reason.data(), static_cast<int64_t>(reason.size()));
}

[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

[[nodiscard]] assets::Vec2f shifted(const assets::Vec2f origin, const bool shift) {
    return {origin.x - (shift ? model::ability_button_shift : 0.0F), origin.y};
}

} // namespace

EawrAbilityButtons::EawrAbilityButtons() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrAbilityButtons::~EawrAbilityButtons() {
    if (RenderingServer* rendering = RenderingServer::get_singleton(); rendering != nullptr && dial_item_.is_valid()) {
        rendering->free_rid(dial_item_);
    }
}

void EawrAbilityButtons::setup(Setup setup) {
    setup_ = std::move(setup);
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!dial_item_.is_valid()) {
        // AB-05: the dials are added, on their own canvas item above the buttons.
        dial_item_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(dial_item_, get_canvas_item());
        rendering->canvas_item_set_default_texture_filter(dial_item_, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
        Ref<CanvasItemMaterial> material;
        material.instantiate();
        material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
        additive_ = material;
        rendering->canvas_item_set_material(dial_item_, additive_->get_rid());
    }
    set_process(true);
    queue_redraw();
}

void EawrAbilityButtons::show(model::AbilityBar bar) {
    const auto same_button = [](const model::AbilityButton& a, const model::AbilityButton& b) {
        return a.component == b.component && a.ability == b.ability && a.shifted == b.shifted && a.icon == b.icon
            && a.disabled == b.disabled && a.recharge == b.recharge && a.autofire == b.autofire && a.units == b.units
            && a.disabled_reason == b.disabled_reason;
    };
    const auto same_mark = [](const model::CardAbilityMark& a, const model::CardAbilityMark& b) {
        return a.slot == b.slot && a.second == b.second && a.icon == b.icon && a.dial == b.dial && a.autofire == b.autofire;
    };
    const bool same = bar.buttons.size() == bar_.buttons.size() && bar.marks.size() == bar_.marks.size()
        && std::equal(bar.buttons.begin(), bar.buttons.end(), bar_.buttons.begin(), same_button)
        && std::equal(bar.marks.begin(), bar.marks.end(), bar_.marks.begin(), same_mark);
    if (same) return;
    bar_ = std::move(bar);
    if (hovered_ && *hovered_ >= bar_.buttons.size()) hovered_.reset();
    set_tooltip_text(hovered_ ? reason_text(bar_.buttons[*hovered_].disabled_reason) : String());
    queue_redraw();
}

Rect2 EawrAbilityButtons::screen(const data::ui::ReferenceRect& rect) const {
    return rect2(model::shell_to_screen(rect, setup_.placement()));
}

double EawrAbilityButtons::now() const {
    if (clock_) return clock_();
    return static_cast<double>(Time::get_singleton()->get_ticks_usec()) / 1.0e6;
}

const model::HudAbilityButton* EawrAbilityButtons::look(const model::AbilityButton& button) const {
    return button.component < setup_.buttons.size() ? &setup_.buttons[button.component] : nullptr;
}

std::optional<Rect2> EawrAbilityButtons::button_rect(const std::size_t index) const {
    if (index >= bar_.buttons.size() || !setup_.placement) return std::nullopt;
    const model::AbilityButton& button = bar_.buttons[index];
    const model::HudAbilityButton* component = look(button);
    if (component == nullptr) return std::nullopt;
    // FoC picks the component's mesh, which the bone's shift moves with the art.
    data::ui::ReferenceRect rect = component->button.rect;
    if (button.shifted) rect.x -= model::ability_button_shift;
    return screen(rect);
}

std::optional<std::size_t> EawrAbilityButtons::button_at(const Vector2& point) const {
    for (std::size_t index = 0; index < bar_.buttons.size(); ++index) {
        const auto rect = button_rect(index);
        if (rect && rect->has_point(point)) return index;
    }
    return std::nullopt;
}

bool EawrAbilityButtons::_has_point(const Vector2& point) const { return button_at(point).has_value(); }

void EawrAbilityButtons::_gui_input(const Ref<InputEvent>& event) {
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        const auto index = button_at(motion->get_position());
        if (index != hovered_) {
            hovered_ = index;
            set_tooltip_text(index ? reason_text(bar_.buttons[*index].disabled_reason) : String());
            queue_redraw();
        }
        return;
    }
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button == nullptr) return;
    const bool left = button->get_button_index() == MOUSE_BUTTON_LEFT;
    const bool right = button->get_button_index() == MOUSE_BUTTON_RIGHT;
    if (!left && !right) return;
    accept_event();
    const auto index = button_at(button->get_position());
    if (button->is_pressed()) {
        pressed_ = index;
        pressed_right_ = right;
        return;
    }
    // AB-09: the action runs on the release over the button the press was on.
    if (!index || index != pressed_ || pressed_right_ != right) {
        pressed_.reset();
        return;
    }
    pressed_.reset();
    if (right) {
        ++right_clicks_;
    } else {
        ++clicks_;
        flash_ = index;
        flash_start_ = now();
    }
    queue_redraw();
    if (click_) click_(*index, right);
}

void EawrAbilityButtons::_notification(const int what) {
    if (what == NOTIFICATION_MOUSE_EXIT) {
        if (hovered_) {
            hovered_.reset();
            set_tooltip_text(String());
            queue_redraw();
        }
    } else if (what == NOTIFICATION_PROCESS) {
        if (get_size() != laid_out_) {
            laid_out_ = get_size();
            queue_redraw();
        }
        // The press flash fades and the autofire outline animates.
        const bool animating = std::any_of(bar_.buttons.begin(), bar_.buttons.end(),
                                           [](const model::AbilityButton& button) { return button.autofire; });
        if (flash_ || animating) queue_redraw();
    }
}

void EawrAbilityButtons::draw_centred(const Ref<Texture2D>& texture, const assets::Vec2f centre, const float scale,
                                      const Color& modulate) {
    if (texture.is_null()) {
        ++textures_missing_;
        return;
    }
    const Vector2 size = texture->get_size();
    const float width = size.x * scale;
    const float height = size.y * scale;
    draw_texture_rect(texture, screen({centre.x - width / 2.0F, centre.y - height / 2.0F, width, height}), false, modulate);
}

void EawrAbilityButtons::draw_dial(const Ref<Texture2D>& texture, const assets::Vec2f centre, const float scale,
                                   const double completion) {
    if (texture.is_null() || completion >= 1.0) return;
    const Vector2 size = texture->get_size();
    const float width = size.x * scale;
    const float height = size.y * scale;
    const Rect2 rect = screen({centre.x - width / 2.0F, centre.y - height / 2.0F, width, height});
    // ABE-7: the square's rim from twelve o'clock clockwise in eighths; the drawn part runs from the
    // completion's point to the end of the turn, a fan around the centre.
    static constexpr std::array<std::array<float, 2>, 9> rim{
        {{0.5F, 0.0F}, {1.0F, 0.0F}, {1.0F, 0.5F}, {1.0F, 1.0F}, {0.5F, 1.0F}, {0.0F, 1.0F}, {0.0F, 0.5F}, {0.0F, 0.0F}, {0.5F, 0.0F}}};
    const double eighths = std::clamp(completion, 0.0, 1.0) * 8.0;
    const auto first = static_cast<std::size_t>(std::floor(eighths));
    const float part = static_cast<float>(eighths - static_cast<double>(first));
    PackedVector2Array points;
    PackedVector2Array uvs;
    const auto add = [&](const float u, const float v) {
        uvs.push_back(Vector2(u, v));
        points.push_back(Vector2(rect.position.x + u * rect.size.x, rect.position.y + v * rect.size.y));
    };
    add(0.5F, 0.5F);
    add(rim[first][0] + (rim[first + 1][0] - rim[first][0]) * part, rim[first][1] + (rim[first + 1][1] - rim[first][1]) * part);
    for (std::size_t index = first + 1; index < rim.size(); ++index) add(rim[index][0], rim[index][1]);
    if (points.size() < 3) return;
    PackedColorArray colours;
    colours.push_back(Color(1, 1, 1, 1));
    RenderingServer::get_singleton()->canvas_item_add_polygon(dial_item_, points, colours, uvs, texture->get_rid());
    ++dials_drawn_;
}

void EawrAbilityButtons::_draw() {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (dial_item_.is_valid()) rendering->canvas_item_clear(dial_item_);
    textures_missing_ = 0;
    dials_drawn_ = 0;
    if (!setup_.placement || !setup_.texture) return;
    const double time = now();
    // AB-08: the card marks, over the cards.
    for (const model::CardAbilityMark& mark : bar_.marks) {
        if (mark.slot >= setup_.slots.size()) continue;
        const model::HudCardSlot& slot = setup_.slots[mark.slot];
        const model::HudCardMarks& at = slot.marks;
        const assets::Vec2f bone = slot.card.origin;
        const data::ui::Vec2 icon = mark.second ? at.second_icon : at.icon;
        const data::ui::Vec2 dial = mark.second ? at.second_dial : at.dial;
        const data::ui::Vec2 overlay = mark.second ? at.second_overlay : at.overlay;
        if (!mark.icon.empty()) {
            draw_centred(setup_.texture(mark.icon), {bone.x + icon.x, bone.y + icon.y}, slot.card.scale * model::card_mark_scale);
        }
        if (mark.autofire) {
            const std::string& box = mark.second ? at.overlay2_texture : at.overlay_texture;
            if (!box.empty()) draw_centred(setup_.texture(box), {bone.x + overlay.x, bone.y + overlay.y}, slot.card.scale);
        }
        if (mark.dial && !at.build.empty()) {
            draw_dial(setup_.texture(at.build), {bone.x + dial.x, bone.y + dial.y}, slot.card.scale, *mark.dial);
        }
    }
    for (std::size_t index = 0; index < bar_.buttons.size(); ++index) {
        const model::AbilityButton& button = bar_.buttons[index];
        const model::HudAbilityButton* component = look(button);
        if (component == nullptr) continue;
        const model::HudShellButton& art = component->button;
        const assets::Vec2f centre = shifted(art.origin, button.shifted);
        if (!component->blank.empty()) draw_centred(setup_.texture(component->blank), centre, art.scale);
        if (!button.icon.empty()) draw_centred(setup_.texture(button.icon), centre, art.scale);
        if (hovered_ == index && !button.disabled && !art.mouse_over.empty()) {
            draw_centred(setup_.texture(art.mouse_over), centre, art.scale);
        }
        if (flash_ == index && !art.pressed.empty()) {
            const double left = flash_seconds - (time - flash_start_);
            if (left > 0.0) {
                const float level = static_cast<float>(left / flash_seconds);
                draw_centred(setup_.texture(art.pressed), centre, art.scale, Color(level, level, level, 1.0F));
            } else {
                flash_.reset();
            }
        }
        if (button.disabled && !art.disabled.empty()) draw_centred(setup_.texture(art.disabled), centre, art.scale);
        // AB-06: the autofire outline cycles its frames while every unit is on autofire.
        if (button.autofire && !art.alternates.empty()) {
            const auto frame = static_cast<std::size_t>(std::floor(std::max(0.0, time) * component->anim_fps))
                % art.alternates.size();
            autofire_frame_ = frame;
            draw_centred(setup_.texture(art.alternates[frame]), centre, art.scale);
        }
        if (button.recharge < 1.0 && !component->build.empty()) {
            draw_dial(setup_.texture(component->build), centre, art.scale, button.recharge);
        }
    }
}

std::string EawrAbilityButtons::report_json() const {
    std::ostringstream output;
    output << "{\"components\": " << setup_.buttons.size() << ", \"clicks\": " << clicks_
           << ", \"right_clicks\": " << right_clicks_ << ", \"textures_missing\": " << textures_missing_
           << ", \"dials_drawn\": " << dials_drawn_ << ", \"buttons\": [";
    for (std::size_t index = 0; index < bar_.buttons.size(); ++index) {
        const model::AbilityButton& button = bar_.buttons[index];
        output << (index ? ", " : "") << "{\"component\": " << button.component << ", \"ability\": " << button.ability
               << ", \"name\": \"" << model::ability_name(button.ability) << "\", \"icon\": \"" << button.icon
               << "\", \"second\": " << (button.second ? "true" : "false") << ", \"shifted\": "
               << (button.shifted ? "true" : "false") << ", \"disabled\": " << (button.disabled ? "true" : "false")
               << ", \"recharge\": " << button.recharge << ", \"autofire\": " << (button.autofire ? "true" : "false")
               << ", \"units\": [";
        for (std::size_t unit = 0; unit < button.units.size(); ++unit) output << (unit ? ", " : "") << button.units[unit];
        output << "]";
        if (const auto rect = button_rect(index)) output << ", \"rect\": " << rect_json(*rect);
        output << "}";
    }
    output << "], \"marks\": [";
    for (std::size_t index = 0; index < bar_.marks.size(); ++index) {
        const model::CardAbilityMark& mark = bar_.marks[index];
        output << (index ? ", " : "") << "{\"slot\": " << mark.slot << ", \"second\": " << (mark.second ? "true" : "false")
               << ", \"icon\": \"" << mark.icon << "\", \"dial\": ";
        if (mark.dial) output << *mark.dial;
        else output << "null";
        output << ", \"autofire\": " << (mark.autofire ? "true" : "false") << "}";
    }
    output << "]}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
