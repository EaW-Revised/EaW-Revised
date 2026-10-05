#include "kit_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace kit_detail;


// --- Frame -------------------------------------------------------------------

EawrUiFrame::EawrUiFrame() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_mouse_filter(MOUSE_FILTER_IGNORE);
}

void EawrUiFrame::set_small(const bool small) {
    small_ = small;
    queue_redraw();
}

void draw_small_frame(Control& control, const Rect2& rect, const bool background) {
    if (background) draw_piece(control, icon(control, "Small_Frame_Background"), rect);
    const Ref<Texture2D> top_left = icon(control, "Small_Frame_Top_Left");
    const Ref<Texture2D> top_right = icon(control, "Small_Frame_Top_Right");
    const Ref<Texture2D> bottom_left = icon(control, "Small_Frame_Bottom_Left");
    const Ref<Texture2D> bottom_right = icon(control, "Small_Frame_Bottom_Right");
    const Vector2 tl = size_of(top_left);
    const Vector2 tr = size_of(top_right);
    const Vector2 bl = size_of(bottom_left);
    const Vector2 br = size_of(bottom_right);
    const Border border = small_border(control);
    const Vector2 end = rect.get_end();
    draw_piece(control, icon(control, "Small_Frame_Top"),
               Rect2(rect.position.x + tl.x, rect.position.y, rect.size.x - tl.x - tr.x, border.top));
    draw_piece(control, icon(control, "Small_Frame_Bottom"),
               Rect2(rect.position.x + bl.x, end.y - border.bottom, rect.size.x - bl.x - br.x, border.bottom));
    draw_piece(control, icon(control, "Small_Frame_Left"),
               Rect2(rect.position.x, rect.position.y + tl.y, border.left, rect.size.y - tl.y - bl.y));
    draw_piece(control, icon(control, "Small_Frame_Right"),
               Rect2(end.x - border.right, rect.position.y + tr.y, border.right, rect.size.y - tr.y - br.y));
    draw_piece(control, top_left, Rect2(rect.position, tl));
    draw_piece(control, top_right, Rect2(end.x - tr.x, rect.position.y, tr.x, tr.y));
    draw_piece(control, bottom_left, Rect2(rect.position.x, end.y - bl.y, bl.x, bl.y));
    draw_piece(control, bottom_right, Rect2(end - br, br));
}

void EawrUiFrame::_draw() {
    const Rect2 rect(Vector2(), get_size());
    if (small_) {
        draw_small_frame(*this, rect, true);
        return;
    }
    // The background has its own rim, so it stretches over the rect (tiled, the
    // rims would grid the dialog).
    draw_piece(*this, icon(*this, "Frame_Background"), rect);
    const float width = rect.size.x;
    const float height = rect.size.y;
    const auto piece = [&](const char* slot) { return icon(*this, slot); };
    // The corners sit outside the rect; transitions run inside the rect's span
    // next to the corners, and the edges stretch between the transitions.
    const Ref<Texture2D> top_left = piece("Frame_Top_Left");
    const Ref<Texture2D> top_right = piece("Frame_Top_Right");
    const Ref<Texture2D> bottom_left = piece("Frame_Bottom_Left");
    const Ref<Texture2D> bottom_right = piece("Frame_Bottom_Right");
    const Vector2 tl = size_of(top_left);
    const Vector2 tr = size_of(top_right);
    const Vector2 bl = size_of(bottom_left);
    const Vector2 br = size_of(bottom_right);
    draw_piece(*this, top_left, Rect2(-tl, tl));
    draw_piece(*this, top_right, Rect2(width, -tr.y, tr.x, tr.y));
    draw_piece(*this, bottom_left, Rect2(-bl.x, height, bl.x, bl.y));
    draw_piece(*this, bottom_right, Rect2(width, height, br.x, br.y));

    const auto horizontal = [&](const char* first, const char* edge, const char* last, const bool top) {
        const Ref<Texture2D> start = piece(first);
        const Ref<Texture2D> middle = piece(edge);
        const Ref<Texture2D> finish = piece(last);
        const Vector2 a = size_of(start);
        const Vector2 m = size_of(middle);
        const Vector2 b = size_of(finish);
        const auto y = [&](const Vector2& size) { return top ? -size.y : height; };
        draw_piece(*this, start, Rect2(0.0F, y(a), std::min(a.x, width / 2.0F), a.y));
        draw_piece(*this, finish, Rect2(width - std::min(b.x, width / 2.0F), y(b), std::min(b.x, width / 2.0F), b.y));
        draw_piece(*this, middle, Rect2(a.x, y(m), width - a.x - b.x, m.y));
    };
    const auto vertical = [&](const char* first, const char* edge, const char* last, const bool left) {
        const Ref<Texture2D> start = piece(first);
        const Ref<Texture2D> middle = piece(edge);
        const Ref<Texture2D> finish = piece(last);
        const Vector2 a = size_of(start);
        const Vector2 m = size_of(middle);
        const Vector2 b = size_of(finish);
        const auto x = [&](const Vector2& size) { return left ? -size.x : width; };
        draw_piece(*this, start, Rect2(x(a), 0.0F, a.x, std::min(a.y, height / 2.0F)));
        draw_piece(*this, finish, Rect2(x(b), height - std::min(b.y, height / 2.0F), b.x, std::min(b.y, height / 2.0F)));
        draw_piece(*this, middle, Rect2(x(m), a.y, m.x, height - a.y - b.y));
    };
    horizontal("Frame_Top_Transition_Left", "Frame_Top", "Frame_Top_Transition_Right", true);
    horizontal("Frame_Bottom_Transition_Left", "Frame_Bottom", "Frame_Bottom_Transition_Right", false);
    vertical("Frame_Left_Transition_Top", "Frame_Left", "Frame_Left_Transition_Bottom", true);
    vertical("Frame_Right_Transition_Top", "Frame_Right", "Frame_Right_Transition_Bottom", false);
}

// --- Button ------------------------------------------------------------------


} // namespace eawr::presentation::godot_backend
