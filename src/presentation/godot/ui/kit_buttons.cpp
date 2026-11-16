#include "kit_internal.hpp"

#include "eawr/presentation/ui/layout.hpp"

namespace eawr::presentation::godot_backend {
using namespace kit_detail;

EawrUiButton::EawrUiButton() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_focus_mode(FOCUS_NONE);
    track_oversampling(*this);
}

void EawrUiButton::set_text(const String& text) {
    text_ = text;
    update_minimum_size();
    queue_redraw();
}

void EawrUiButton::set_fit_to_rect(const bool enabled) {
    if (fit_to_rect_ == enabled) return;
    fit_to_rect_ = enabled;
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
    // Most controls retain their texture height. Bounded dialogs scale the strip
    // into the assigned rectangle, including the end caps' aspect ratio.
    const auto tallest = [](const Ref<Texture2D>& a, const Ref<Texture2D>& b, const Ref<Texture2D>& c) {
        return std::max({size_of(a).y, size_of(b).y, size_of(c).y});
    };
    // UI-B1: the rectangle is as high as the resting art; a taller art of another state (a mod's glow round a
    // hovered, pressed or disabled button) grows round the rectangle's centre line instead of pushing the
    // caption and the frame down over the next row.
    const float resting = fit_to_rect_ ? get_size().y
        : tallest(icon(*this, "Button_Left"), icon(*this, "Button_Middle"), icon(*this, "Button_Right"));
    const auto band = eawr::presentation::ui::button_art_band(resting, fit_to_rect_ ? 0.0F : tallest(left, middle, right));
    const float height = static_cast<float>(band.height);
    const float top = static_cast<float>(band.top);
    const Strip strip = strip_layout(Vector2(get_size().x, height > 0.0F ? height : get_size().y), left, right);
    const auto lowered = [top](Rect2 rect) { rect.position.y += top; return rect; };
    draw_piece(*this, left, lowered(strip.left));
    draw_piece(*this, middle, lowered(strip.middle));
    draw_piece(*this, right, lowered(strip.right));
    const KitTextStyle style = kit_text_style(*this, "Push_Button");
    // Centred over the whole button, caps included, as the original does.
    caption_.draw(*this, style, text_, Rect2(0.0F, 0.0F, get_size().x, static_cast<float>(band.caption_height)),
                  HORIZONTAL_ALIGNMENT_CENTER, true, false, piece_scale(*this),
                  current == KitState::disabled ? disabled_alpha : 1.0F);
}

Vector2 EawrUiButton::_get_minimum_size() const {
    const float height = fit_to_rect_ ? 0.0F : size_of(icon(*this, "Button_Middle")).y;
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
    track_oversampling(*this);
}

void EawrUiCheck::set_text(const String& text) {
    text_ = text;
    update_minimum_size();
    queue_redraw();
}

void EawrUiCheck::set_radio(const bool radio) {
    radio_ = radio;
    update_minimum_size();
    queue_redraw();
}

void EawrUiCheck::set_authored_size(const Vector2& size) {
    authored_size_ = {size.x, size.y};
    update_minimum_size();
    set_size(size);
}

void EawrUiCheck::set_pushlike(const bool pushlike) {
    pushlike_ = pushlike;
    update_minimum_size();
    queue_redraw();
}

void EawrUiCheck::set_forced_state(const KitState state) {
    forced_ = state;
    queue_redraw();
}

presentation::ui::ToggleBox EawrUiCheck::glyph_box() const {
    // The glyph is sized from the OFF art alone (docs/behaviour/walks/dialog-toggles.md DTG-1).
    const Vector2 off = size_of(icon(*this, radio_ ? "Radio_Off" : "Check_Off"));
    return presentation::ui::toggle_glyph_box({off.x, off.y});
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
    if (pushlike_ && !radio_) {
        // A push-style check box: no rule is sourced for it, it keeps filling its authored rectangle.
        const Rect2 whole(Vector2(), get_size());
        draw_piece(*this, box, whole, Color(1, 1, 1, alpha));
        const KitTextStyle tab_style = kit_text_style(*this, "Global_Default");
        caption_.draw(*this, tab_style, text_, whole, HORIZONTAL_ALIGNMENT_CENTER, true, false, piece_scale(*this),
                      alpha);
        return;
    }
    // #5345 (DTG-1..4): the glyph is the OFF art's size at the control's top-left, every state draws into it; the caption is
    // centred on the glyph (push-style) or starts one gap after it, vertically on the glyph's centre.
    const presentation::ui::ToggleBox glyph = glyph_box();
    draw_piece(*this, box, Rect2(glyph.x, glyph.y, glyph.width, glyph.height), Color(1, 1, 1, alpha));
    const presentation::ui::ToggleBox place = presentation::ui::toggle_caption_box(
        glyph, pushlike_, get_size().x, presentation::ui::toggle_caption_gap(glyph));
    const KitTextStyle style = kit_text_style(*this, "Global_Default");
    caption_.draw(*this, style, text_, Rect2(place.x, place.y, place.width, place.height),
                  pushlike_ ? HORIZONTAL_ALIGNMENT_CENTER : HORIZONTAL_ALIGNMENT_LEFT, true, false, piece_scale(*this), alpha);
}

Vector2 EawrUiCheck::_get_minimum_size() const {
    // Positive resource dimensions remain authoritative. A zero dimension still needs a hit area
    // for the native radio glyph drawn outside that empty rectangle (DTG-5).
    if (pushlike_) {
        if (!radio_) return {};
        const auto glyph = glyph_box();
        const auto minimum = presentation::ui::toggle_minimum_size(authored_size_, {glyph.width, glyph.height});
        return {minimum.width, minimum.height};
    }
    const presentation::ui::ToggleBox glyph = glyph_box();
    const Vector2 box(glyph.width, glyph.height);
    const KitTextStyle style = kit_text_style(*this, "Global_Default");
    if (style.font.is_null()) return box;
    const Vector2 text = style.font->get_string_size(text_, HORIZONTAL_ALIGNMENT_LEFT, -1.0F, style.size);
    return {box.x * 1.3F + text.x, std::max(box.y, text.y)};
}

// --- Slider ------------------------------------------------------------------


} // namespace eawr::presentation::godot_backend
