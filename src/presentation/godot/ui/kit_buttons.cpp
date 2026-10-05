#include "kit_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace kit_detail;

EawrUiButton::EawrUiButton() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_focus_mode(FOCUS_NONE);
}

void EawrUiButton::set_text(const String& text) {
    text_ = text;
    update_minimum_size();
    queue_redraw();
}

void EawrUiButton::set_forced_state(const KitState state) {
    forced_ = state;
    queue_redraw();
}

KitState EawrUiButton::state() const {
    return button_state(*this, forced_);
}

void EawrUiButton::_draw() {
    const KitState current = state();
    const Ref<Texture2D> left = state_icon(*this, "Button_Left", current);
    const Ref<Texture2D> middle = state_icon(*this, "Button_Middle", current);
    const Ref<Texture2D> right = state_icon(*this, "Button_Right", current);
    // As retail draws it: the pieces keep their (scaled) texture height from
    // the rect's top, whatever the `.rc` height (FoC in-game menu, 1280x720).
    const float height = std::max({size_of(left).y, size_of(middle).y, size_of(right).y});
    const Strip strip = strip_layout(Vector2(get_size().x, height > 0.0F ? height : get_size().y), left, right);
    draw_piece(*this, left, strip.left);
    draw_piece(*this, middle, strip.middle);
    draw_piece(*this, right, strip.right);
    const KitTextStyle style = kit_text_style(*this, "Push_Button");
    // Centred over the whole button, caps included, as the original does.
    caption_.draw(*this, style, text_, Rect2(0.0F, 0.0F, get_size().x, strip.middle.size.y),
                  HORIZONTAL_ALIGNMENT_CENTER, true, false, piece_scale(*this),
                  current == KitState::disabled ? disabled_alpha : 1.0F);
}

Vector2 EawrUiButton::_get_minimum_size() const {
    const float height = size_of(icon(*this, "Button_Middle")).y;
    const KitTextStyle style = kit_text_style(*this, "Push_Button");
    if (style.font.is_null()) return {0.0F, height};
    const Vector2 text = style.font->get_string_size(text_, HORIZONTAL_ALIGNMENT_LEFT, -1.0F, style.size);
    return {text.x + 2.0F * text.y, std::max(text.y, height)};
}

// --- Label -------------------------------------------------------------------


EawrUiCheck::EawrUiCheck() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_toggle_mode(true);
    set_focus_mode(FOCUS_NONE);
}

void EawrUiCheck::set_text(const String& text) {
    text_ = text;
    update_minimum_size();
    queue_redraw();
}

void EawrUiCheck::set_radio(const bool radio) {
    radio_ = radio;
    queue_redraw();
}

void EawrUiCheck::set_forced_state(const KitState state) {
    forced_ = state;
    queue_redraw();
}

KitState EawrUiCheck::state() const {
    if (forced_ != KitState::automatic) return forced_;
    if (is_disabled()) return KitState::disabled;
    return is_hovered() ? KitState::hover : KitState::normal;
}

void EawrUiCheck::_draw() {
    const KitState current = state();
    Ref<Texture2D> box;
    if (radio_) {
        box = icon(*this, is_pressed() ? "Radio_On" : current == KitState::hover ? "Radio_Mouse_Over" : "Radio_Off");
        if (box.is_null()) box = icon(*this, is_pressed() ? "Radio_On" : "Radio_Off");
    } else {
        box = icon(*this, is_pressed() ? "Check_On" : "Check_Off");
    }
    const float alpha = current == KitState::disabled ? disabled_alpha : 1.0F;
    const Vector2 size = size_of(box);
    const float height = get_size().y;
    draw_piece(*this, box, Rect2(0.0F, std::round((height - size.y) / 2.0F), size.x, size.y), Color(1, 1, 1, alpha));
    const float gap = std::round(size.x * 0.3F);
    const KitTextStyle style = kit_text_style(*this, "Global_Default");
    caption_.draw(*this, style, text_, Rect2(size.x + gap, 0.0F, get_size().x - size.x - gap, height),
                  HORIZONTAL_ALIGNMENT_LEFT, true, false, piece_scale(*this), alpha);
}

Vector2 EawrUiCheck::_get_minimum_size() const {
    const Vector2 box = size_of(icon(*this, radio_ ? "Radio_Off" : "Check_Off"));
    const KitTextStyle style = kit_text_style(*this, "Global_Default");
    if (style.font.is_null()) return box;
    const Vector2 text = style.font->get_string_size(text_, HORIZONTAL_ALIGNMENT_LEFT, -1.0F, style.size);
    return {box.x * 1.3F + text.x, std::max(box.y, text.y)};
}

// --- Slider ------------------------------------------------------------------


} // namespace eawr::presentation::godot_backend
