#include "kit_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace kit_detail {
const Color outline_colour(0.0F, 0.0F, 0.0F, 1.0F);

[[nodiscard]] Ref<Texture2D> icon(const Control& control, const String& slot) {
    const StringName name(slot);
    if (!control.has_theme_icon(name)) return {};
    Ref<Texture2D> texture = control.get_theme_icon(name);
    return texture.is_valid() && texture->has_meta(empty_piece_meta) ? Ref<Texture2D>() : texture;
}

[[nodiscard]] Vector2 size_of(const Ref<Texture2D>& texture) {
    return texture.is_valid() ? texture->get_size() : Vector2();
}

void draw_piece(CanvasItem& item, const Ref<Texture2D>& texture, const Rect2& rect,
                const Color& modulate) {
    if (texture.is_null() || rect.size.x <= 0.0F || rect.size.y <= 0.0F) return;
    item.draw_texture_rect(texture, rect, false, modulate);
}

// Tiles `texture` along x only, each tile as high as `rect` and keeping the
// texture's aspect.
void draw_strip(CanvasItem& item, const Ref<Texture2D>& texture, const Rect2& rect, const Color& modulate) {
    const Vector2 size = size_of(texture);
    if (size.x <= 0.0F || size.y <= 0.0F || rect.size.x <= 0.0F || rect.size.y <= 0.0F) return;
    const float tile = size.x * rect.size.y / size.y;
    for (float x = rect.position.x; x < rect.get_end().x - 0.01F; x += tile) {
        const float width = std::min(tile, rect.get_end().x - x);
        item.draw_texture_rect_region(texture, Rect2(x, rect.position.y, width, rect.size.y),
                                      Rect2(0.0F, 0.0F, size.x * width / tile, size.y), modulate);
    }
}

[[nodiscard]] Strip strip_layout(const Vector2& size, const Ref<Texture2D>& left, const Ref<Texture2D>& right) {
    const auto cap = [&](const Ref<Texture2D>& texture) {
        const Vector2 native = size_of(texture);
        return native.y > 0.0F ? native.x * size.y / native.y : 0.0F;
    };
    float left_width = cap(left);
    float right_width = cap(right);
    if (left_width + right_width > size.x && left_width + right_width > 0.0F) {
        const float shrink = size.x / (left_width + right_width);
        left_width *= shrink;
        right_width *= shrink;
    }
    Strip strip;
    strip.left = Rect2(0.0F, 0.0F, left_width, size.y);
    strip.right = Rect2(size.x - right_width, 0.0F, right_width, size.y);
    strip.middle = Rect2(left_width, 0.0F, size.x - left_width - right_width, size.y);
    return strip;
}

namespace {
[[nodiscard]] String state_suffix(const KitState state) {
    switch (state) {
    case KitState::hover: return "_Mouse_Over";
    case KitState::pressed: return "_Pressed";
    case KitState::disabled: return "_Disabled";
    default: return "";
    }
}

} // namespace
[[nodiscard]] KitState button_state(const BaseButton& button, const KitState forced) {
    if (forced != KitState::automatic) return forced;
    switch (button.get_draw_mode()) {
    case BaseButton::DRAW_HOVER: return KitState::hover;
    case BaseButton::DRAW_PRESSED:
    case BaseButton::DRAW_HOVER_PRESSED: return KitState::pressed;
    case BaseButton::DRAW_DISABLED: return KitState::disabled;
    default: return KitState::normal;
    }
}

// The state's texture, else the normal one when the skin has no state texture.
[[nodiscard]] Ref<Texture2D> state_icon(const Control& control, const String& slot, const KitState state) {
    if (state != KitState::normal) {
        if (Ref<Texture2D> texture = icon(control, slot + state_suffix(state)); texture.is_valid()) return texture;
    }
    return icon(control, slot);
}

[[nodiscard]] float piece_scale(const Control& control) {
    const StringName name("scale_permille");
    return control.has_theme_constant(name) ? static_cast<float>(std::max(1, control.get_theme_constant(name))) / 1000.0F
                                            : 1.0F;
}

[[nodiscard]] Border small_border(const Control& control) {
    return {size_of(icon(control, "Small_Frame_Left")).x, size_of(icon(control, "Small_Frame_Top")).y,
            size_of(icon(control, "Small_Frame_Right")).x, size_of(icon(control, "Small_Frame_Bottom")).y};
}

} // namespace kit_detail

void register_ui_kit_classes() {
    GDREGISTER_CLASS(EawrUiFrame);
    GDREGISTER_CLASS(EawrUiButton);
    GDREGISTER_CLASS(EawrUiLabel);
    GDREGISTER_CLASS(EawrUiCheck);
    GDREGISTER_CLASS(EawrUiSlider);
    GDREGISTER_CLASS(EawrUiBar);
    GDREGISTER_CLASS(EawrUiList);
    GDREGISTER_CLASS(EawrUiCombo);
    GDREGISTER_CLASS(EawrUiEdit);
}

} // namespace eawr::presentation::godot_backend
