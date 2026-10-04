#pragma once
#include "ui/kit.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/popup_menu.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/style_box_empty.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/style_box_texture.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <string>

using namespace godot;

namespace eawr::presentation::godot_backend::kit_detail {
inline constexpr float disabled_alpha = 0.5F;
extern const Color outline_colour;
// The pieces of a 3-piece strip (button, bar): the caps keep their aspect at
// the strip's height and the middle stretches between them.
struct Strip final {
    Rect2 left, middle, right;
};
// The small frame's thickness on each side, from its edge pieces.
struct Border final {
    float left{}, top{}, right{}, bottom{};
};
[[nodiscard]] Ref<Texture2D> icon(const Control& control, const String& slot);
[[nodiscard]] Vector2 size_of(const Ref<Texture2D>& texture);
void draw_piece(CanvasItem& item, const Ref<Texture2D>& texture, const Rect2& rect,
                const Color& modulate = Color(1, 1, 1, 1));
void draw_strip(CanvasItem& item, const Ref<Texture2D>& texture, const Rect2& rect, const Color& modulate);
[[nodiscard]] Strip strip_layout(const Vector2& size, const Ref<Texture2D>& left, const Ref<Texture2D>& right);
[[nodiscard]] KitState button_state(const BaseButton& button, const KitState forced);
[[nodiscard]] Ref<Texture2D> state_icon(const Control& control, const String& slot, const KitState state);
[[nodiscard]] float piece_scale(const Control& control);
[[nodiscard]] Border small_border(const Control& control);
} // namespace eawr::presentation::godot_backend::kit_detail
