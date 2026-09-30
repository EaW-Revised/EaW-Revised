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

namespace eawr::presentation::godot_backend {
namespace {

// UI-F4 as a canvas material: the text is drawn white (and its emboss shadow
// and outline black), and the fragment multiplies by the gradient between the
// text block's top and bottom. Colours pass as stored (UI-R1).
constexpr const char* gradient_shader = R"GODOT(
shader_type canvas_item;
uniform vec4 top_color;
uniform vec4 bottom_color;
uniform float text_top;
uniform float text_height;
varying float local_y;
void vertex() { local_y = VERTEX.y; }
void fragment() {
    float t = clamp((local_y - text_top) / max(text_height, 1.0), 0.0, 1.0);
    vec4 tint = mix(top_color, bottom_color, t);
    COLOR = vec4(COLOR.rgb * tint.rgb, COLOR.a * tint.a);
}
)GODOT";

// Project-authored looks the data does not define (ui-layer.md 3.6).
const Color emboss_shadow(0.0F, 0.0F, 0.0F, 0.75F);
const Color outline_colour(0.0F, 0.0F, 0.0F, 1.0F);
// White at 30 % over the list background gives the selection colour of the
// rig's FoC skirmish-setup capture; the hover bar is half that.
const Color selection_colour(1.0F, 1.0F, 1.0F, 0.3F);
const Color hover_colour(1.0F, 1.0F, 1.0F, 0.15F);
constexpr float disabled_alpha = 0.5F;

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
                const Color& modulate = Color(1, 1, 1, 1)) {
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

// The pieces of a 3-piece strip (button, bar): the caps keep their aspect at
// the strip's height and the middle stretches between them.
struct Strip final {
    Rect2 left, middle, right;
};

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

[[nodiscard]] String state_suffix(const KitState state) {
    switch (state) {
    case KitState::hover: return "_Mouse_Over";
    case KitState::pressed: return "_Pressed";
    case KitState::disabled: return "_Disabled";
    default: return "";
    }
}

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

[[nodiscard]] Ref<StyleBoxTexture> texture_box(const Ref<Texture2D>& texture, const bool tile) {
    Ref<StyleBoxTexture> box;
    box.instantiate();
    box->set_texture(texture);
    if (tile) {
        box->set_h_axis_stretch_mode(StyleBoxTexture::AXIS_STRETCH_MODE_TILE);
        box->set_v_axis_stretch_mode(StyleBoxTexture::AXIS_STRETCH_MODE_TILE);
    }
    return box;
}

[[nodiscard]] Ref<StyleBoxEmpty> empty_box() {
    Ref<StyleBoxEmpty> box;
    box.instantiate();
    return box;
}

[[nodiscard]] Ref<StyleBoxFlat> flat_box(const Color& colour) {
    Ref<StyleBoxFlat> box;
    box.instantiate();
    box->set_bg_color(colour);
    return box;
}

[[nodiscard]] Ref<Texture2D> empty_texture() {
    const Ref<Image> image = Image::create_empty(1, 1, false, Image::FORMAT_RGBA8);
    image->fill(Color(0, 0, 0, 0));
    return ImageTexture::create_from_image(image);
}

// The small frame's thickness on each side, from its edge pieces.
struct Border final {
    float left{}, top{}, right{}, bottom{};
};

[[nodiscard]] Border small_border(const Control& control) {
    return {size_of(icon(control, "Small_Frame_Left")).x, size_of(icon(control, "Small_Frame_Top")).y,
            size_of(icon(control, "Small_Frame_Right")).x, size_of(icon(control, "Small_Frame_Bottom")).y};
}

void apply_text_colours(Control& control, const KitTextStyle& style, std::initializer_list<const char*> names) {
    for (const char* name : names) control.add_theme_color_override(name, style.top);
}

} // namespace

KitTextStyle kit_text_style(const Control& control, const std::string_view role) {
    const String base = String::utf8(role.data(), static_cast<int64_t>(role.size()));
    const StringName name(base);
    KitTextStyle style;
    style.font = control.has_theme_font(name) ? control.get_theme_font(name) : control.get_theme_default_font();
    style.size = control.has_theme_font_size(name) ? control.get_theme_font_size(name)
                                                   : control.get_theme_default_font_size();
    const StringName top(base + String("_top"));
    const StringName bottom(base + String("_bottom"));
    style.top = control.has_theme_color(top) ? control.get_theme_color(top) : Color(1, 1, 1, 1);
    style.bottom = control.has_theme_color(bottom) ? control.get_theme_color(bottom) : style.top;
    const StringName emboss(base + String("_emboss"));
    const StringName outline(base + String("_outline"));
    style.emboss = control.has_theme_constant(emboss) && control.get_theme_constant(emboss) != 0;
    style.outline = control.has_theme_constant(outline) && control.get_theme_constant(outline) != 0;
    const StringName cell_ascent(base + String("_cell_ascent"));
    const StringName cell_descent(base + String("_cell_descent"));
    style.cell_ascent = control.has_theme_constant(cell_ascent) ? control.get_theme_constant(cell_ascent) : 0;
    style.cell_descent = control.has_theme_constant(cell_descent) ? control.get_theme_constant(cell_descent) : 0;
    return style;
}

KitText::~KitText() {
    if (item_.is_valid()) RenderingServer::get_singleton()->free_rid(item_);
}

void KitText::draw(CanvasItem& owner, const KitTextStyle& style, const String& text, const Rect2& box,
                   const HorizontalAlignment horizontal, const bool centre_vertically, const bool wrap, const float scale,
                   const float alpha) {
    RenderingServer* rendering = RenderingServer::get_singleton();
    if (!item_.is_valid()) {
        item_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(item_, owner.get_canvas_item());
    }
    rendering->canvas_item_clear(item_);
    if (text.is_empty() || style.font.is_null() || style.size <= 0) return;
    const float width = std::max(1.0F, box.size.x);
    // The line is GDI's text cell when the theme has one, so text sits where
    // the original puts it; otherwise the engine's ascent and descent.
    const bool cell = style.cell_ascent > 0;
    const float ascent = cell ? static_cast<float>(style.cell_ascent) : style.font->get_ascent(style.size);
    const float line = cell ? static_cast<float>(style.cell_ascent + style.cell_descent)
                            : style.font->get_height(style.size);
    const float lines =
        wrap ? std::max(1.0F, std::round(style.font->get_multiline_string_size(text, horizontal, width, style.size).y
                                         / style.font->get_height(style.size)))
             : 1.0F;
    const float block = line * lines;
    const float top = centre_vertically ? box.position.y + (box.size.y - block) / 2.0F : box.position.y;
    const Vector2 origin(box.position.x, std::round(top + ascent));
    const bool gradient = style.top != style.bottom;
    if (gradient) {
        if (material_.is_null()) {
            Ref<Shader> shader;
            shader.instantiate();
            shader->set_code(gradient_shader);
            material_.instantiate();
            material_->set_shader(shader);
        }
        material_->set_shader_parameter("top_color", style.top);
        material_->set_shader_parameter("bottom_color", style.bottom);
        material_->set_shader_parameter("text_top", top);
        material_->set_shader_parameter("text_height", block);
        rendering->canvas_item_set_material(item_, material_->get_rid());
    } else {
        rendering->canvas_item_set_material(item_, RID());
    }
    const Color ink = gradient ? Color(1, 1, 1, alpha) : Color(style.top.r, style.top.g, style.top.b, style.top.a * alpha);
    const auto string = [&](const Vector2& at, const Color& colour) {
        if (wrap) {
            style.font->draw_multiline_string(item_, at, text, horizontal, width, style.size, -1, colour);
        } else {
            style.font->draw_string(item_, at, text, horizontal, width, style.size, colour);
        }
    };
    if (style.outline) {
        const int size = std::max(1, static_cast<int>(std::lround(2.0F * scale)));
        const Color colour(outline_colour.r, outline_colour.g, outline_colour.b, outline_colour.a * alpha);
        if (wrap) {
            style.font->draw_multiline_string_outline(item_, origin, text, horizontal, width, style.size, -1, size, colour);
        } else {
            style.font->draw_string_outline(item_, origin, text, horizontal, width, style.size, size, colour);
        }
    }
    if (style.emboss) {
        const float offset = std::max(1.0F, std::round(scale));
        string(origin + Vector2(offset, offset), Color(emboss_shadow.r, emboss_shadow.g, emboss_shadow.b,
                                                       emboss_shadow.a * alpha));
    }
    string(origin, ink);
}

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

EawrUiLabel::EawrUiLabel() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_mouse_filter(MOUSE_FILTER_IGNORE);
}

void EawrUiLabel::set_text(const String& text) {
    text_ = text;
    queue_redraw();
}

void EawrUiLabel::set_role(const String& role) {
    role_ = role;
    queue_redraw();
}

void EawrUiLabel::set_alignment(const HorizontalAlignment alignment) {
    alignment_ = alignment;
    queue_redraw();
}

void EawrUiLabel::set_wrap(const bool wrap) {
    wrap_ = wrap;
    queue_redraw();
}

void EawrUiLabel::_draw() {
    const CharString role = role_.utf8();
    const KitTextStyle style = kit_text_style(*this, std::string_view(role.get_data(), role.length()));
    caption_.draw(*this, style, text_, Rect2(Vector2(), get_size()), alignment_, false, wrap_, piece_scale(*this),
                  1.0F);
}

// --- Check and radio ---------------------------------------------------------

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

EawrUiSlider::EawrUiSlider() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_focus_mode(FOCUS_NONE);
}

void EawrUiSlider::set_forced_states(const KitState minus, const KitState plus) {
    forced_minus_ = minus;
    forced_plus_ = plus;
    queue_redraw();
}

EawrUiSlider::Layout EawrUiSlider::layout() const {
    const Vector2 size = get_size();
    const Vector2 minus = size_of(icon(*this, "Dial_Minus"));
    const Vector2 plus = size_of(icon(*this, "Dial_Plus"));
    const Vector2 left = size_of(icon(*this, "Dial_Left"));
    const Vector2 right = size_of(icon(*this, "Dial_Right"));
    const Vector2 middle = size_of(icon(*this, "Dial_Middle"));
    const Vector2 tab = size_of(icon(*this, "Dial_Tab"));
    const float centre = size.y / 2.0F;
    const auto centred = [&](const float x, const Vector2& piece) {
        return Rect2(x, std::round(centre - piece.y / 2.0F), piece.x, piece.y);
    };
    Layout result;
    result.minus = centred(0.0F, minus);
    result.plus = centred(size.x - plus.x, plus);
    const float track_height = std::max({left.y, middle.y, right.y});
    result.track = Rect2(minus.x, std::round(centre - track_height / 2.0F), size.x - minus.x - plus.x, track_height);
    const float travel = std::max(0.0F, result.track.size.x - left.x - right.x - tab.x);
    result.tab = centred(result.track.position.x + left.x + std::round(travel * static_cast<float>(get_as_ratio())), tab);
    return result;
}

void EawrUiSlider::_draw() {
    const Layout parts = layout();
    const bool disabled = forced_minus_ == KitState::disabled || forced_plus_ == KitState::disabled;
    const Color modulate(1, 1, 1, disabled ? disabled_alpha : 1.0F);
    const auto button_state = [&](const Part part, const KitState forced) {
        if (disabled) return KitState::normal;
        if (forced != KitState::automatic) return forced;
        if (held_ == part) return KitState::pressed;
        return hovered_ == part ? KitState::hover : KitState::normal;
    };
    draw_piece(*this, state_icon(*this, "Dial_Minus", button_state(Part::minus, forced_minus_)), parts.minus, modulate);
    draw_piece(*this, state_icon(*this, "Dial_Plus", button_state(Part::plus, forced_plus_)), parts.plus, modulate);
    const Ref<Texture2D> left = icon(*this, "Dial_Left");
    const Ref<Texture2D> right = icon(*this, "Dial_Right");
    const Vector2 l = size_of(left);
    const Vector2 r = size_of(right);
    const Rect2& track = parts.track;
    draw_piece(*this, left, Rect2(track.position.x, track.position.y, l.x, track.size.y), modulate);
    draw_piece(*this, icon(*this, "Dial_Middle"),
               Rect2(track.position.x + l.x, track.position.y, track.size.x - l.x - r.x, track.size.y), modulate);
    draw_piece(*this, right, Rect2(track.get_end().x - r.x, track.position.y, r.x, track.size.y), modulate);
    draw_piece(*this, icon(*this, "Dial_Tab"), parts.tab, modulate);
}

EawrUiSlider::Part EawrUiSlider::part_at(const Vector2 position) const {
    const Layout parts = layout();
    if (parts.minus.has_point(position)) return Part::minus;
    if (parts.plus.has_point(position)) return Part::plus;
    if (position.x >= parts.track.position.x && position.x < parts.track.get_end().x) return Part::track;
    return Part::none;
}

void EawrUiSlider::set_from_track(const float x) {
    const Layout parts = layout();
    const float start = parts.track.position.x + size_of(icon(*this, "Dial_Left")).x + parts.tab.size.x / 2.0F;
    const float travel = parts.track.size.x - size_of(icon(*this, "Dial_Left")).x
        - size_of(icon(*this, "Dial_Right")).x - parts.tab.size.x;
    if (travel <= 0.0F) return;
    set_as_ratio(std::clamp((x - start) / travel, 0.0F, 1.0F));
}

void EawrUiSlider::_gui_input(const Ref<InputEvent>& event) {
    const double step = get_step() > 0.0 ? get_step() : (get_max() - get_min()) / 20.0;
    if (const Ref<InputEventMouseMotion> motion = event; motion.is_valid()) {
        const Part part = part_at(motion->get_position());
        if (held_ == Part::track) set_from_track(motion->get_position().x);
        if (part != hovered_) {
            hovered_ = part;
            queue_redraw();
        }
        return;
    }
    const Ref<InputEventMouseButton> button = event;
    if (button.is_null() || button->get_button_index() != MOUSE_BUTTON_LEFT) return;
    if (button->is_pressed()) {
        held_ = part_at(button->get_position());
        if (held_ == Part::minus) set_value(get_value() - step);
        if (held_ == Part::plus) set_value(get_value() + step);
        if (held_ == Part::track) set_from_track(button->get_position().x);
        if (held_ != Part::none) accept_event();
    } else {
        held_ = Part::none;
    }
    queue_redraw();
}

void EawrUiSlider::_notification(const int what) {
    if (what == NOTIFICATION_MOUSE_EXIT) {
        hovered_ = Part::none;
        queue_redraw();
    }
}

// --- Bar ---------------------------------------------------------------------

EawrUiBar::EawrUiBar() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_mouse_filter(MOUSE_FILTER_IGNORE);
}

void EawrUiBar::_draw() {
    const Ref<Texture2D> left = icon(*this, "Progress_Bar_Left");
    const Ref<Texture2D> right = icon(*this, "Progress_Bar_Right");
    const Strip strip = strip_layout(get_size(), left, right);
    draw_piece(*this, left, strip.left);
    draw_piece(*this, right, strip.right);
    const float filled = std::round(strip.middle.size.x * static_cast<float>(get_as_ratio()));
    draw_strip(*this, icon(*this, "Progress_Bar_Middle_On"),
               Rect2(strip.middle.position, Vector2(filled, strip.middle.size.y)), Color(1, 1, 1, 1));
    draw_strip(*this, icon(*this, "Progress_Bar_Middle_Off"),
               Rect2(strip.middle.position.x + filled, 0.0F, strip.middle.size.x - filled, strip.middle.size.y),
               Color(1, 1, 1, 1));
}

// --- List --------------------------------------------------------------------

EawrUiList::EawrUiList() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    if (VScrollBar* bar = get_v_scroll_bar()) bar->connect("draw", callable_mp(this, &EawrUiList::draw_scroll_tab));
}

void EawrUiList::apply_skin() {
    if (applying_) return;
    applying_ = true;
    begin_bulk_theme_override();
    const Border border = small_border(*this);
    Ref<StyleBoxTexture> panel = texture_box(icon(*this, "Small_Frame_Background"), true);
    const float scale = piece_scale(*this);
    panel->set_content_margin(SIDE_LEFT, border.left + std::round(3.0F * scale));
    panel->set_content_margin(SIDE_TOP, border.top + std::round(2.0F * scale));
    panel->set_content_margin(SIDE_RIGHT, border.right);
    panel->set_content_margin(SIDE_BOTTOM, border.bottom + std::round(2.0F * scale));
    add_theme_stylebox_override("panel", panel);
    add_theme_stylebox_override("focus", empty_box());
    for (const char* name : {"selected", "selected_focus", "hovered_selected", "hovered_selected_focus"}) {
        add_theme_stylebox_override(name, flat_box(selection_colour));
    }
    add_theme_stylebox_override("hovered", flat_box(hover_colour));
    add_theme_stylebox_override("cursor", empty_box());
    add_theme_stylebox_override("cursor_unfocused", empty_box());
    const KitTextStyle style = kit_text_style(*this, "List_Box");
    add_theme_font_override("font", style.font);
    add_theme_font_size_override("font_size", style.size);
    apply_text_colours(*this, style, {"font_color", "font_hovered_color", "font_selected_color",
                                      "font_hovered_selected_color"});
    add_theme_color_override("font_outline_color", outline_colour);
    add_theme_constant_override("outline_size", style.outline ? std::max(1, static_cast<int>(scale)) : 0);
    add_theme_constant_override("v_separation", static_cast<int>(std::round(2.0F * scale)));
    add_theme_color_override("guide_color", Color(0, 0, 0, 0));
    end_bulk_theme_override();

    VScrollBar* bar = get_v_scroll_bar();
    if (bar != nullptr) {
        bar->begin_bulk_theme_override();
        for (const auto& [name, slot] : {std::pair{"decrement", "Scroll_Up_Button"},
                                         std::pair{"decrement_highlight", "Scroll_Up_Button_Mouse_Over"},
                                         std::pair{"decrement_pressed", "Scroll_Up_Button_Pressed"},
                                         std::pair{"increment", "Scroll_Down_Button"},
                                         std::pair{"increment_highlight", "Scroll_Down_Button_Mouse_Over"},
                                         std::pair{"increment_pressed", "Scroll_Down_Button_Pressed"}}) {
            if (const Ref<Texture2D> texture = icon(*this, slot); texture.is_valid()) {
                bar->add_theme_icon_override(name, texture);
            }
        }
        const Ref<Texture2D> track = icon(*this, "Scroll_Middle");
        Ref<StyleBoxTexture> scroll = texture_box(track, false);
        scroll->set_content_margin(SIDE_LEFT, size_of(track).x / 2.0F);
        scroll->set_content_margin(SIDE_RIGHT, size_of(track).x / 2.0F);
        bar->add_theme_stylebox_override("scroll", scroll);
        bar->add_theme_stylebox_override("scroll_focus", scroll);
        // The original's tab keeps its size; draw_scroll_tab draws it, and the
        // engine's grabber stays an invisible drag handle at least that long.
        const Vector2 tab = size_of(icon(*this, "Scroll_Tab"));
        Ref<StyleBoxEmpty> grabber = empty_box();
        grabber->set_content_margin(SIDE_TOP, tab.y / 2.0F);
        grabber->set_content_margin(SIDE_BOTTOM, tab.y / 2.0F);
        for (const char* name : {"grabber", "grabber_highlight", "grabber_pressed"}) {
            bar->add_theme_stylebox_override(name, grabber);
        }
        bar->end_bulk_theme_override();
    }
    applying_ = false;
}

void EawrUiList::_notification(const int what) {
    if (what == NOTIFICATION_THEME_CHANGED) apply_skin();
}

void EawrUiList::_draw() {
    draw_small_frame(*this, Rect2(Vector2(), get_size()), false);
}

void EawrUiList::draw_scroll_tab() {
    VScrollBar* bar = get_v_scroll_bar();
    const Ref<Texture2D> tab = icon(*this, "Scroll_Tab");
    if (bar == nullptr || tab.is_null()) return;
    const Vector2 size = bar->get_size();
    const float top = size_of(icon(*this, "Scroll_Up_Button")).y;
    const float bottom = size_of(icon(*this, "Scroll_Down_Button")).y;
    const Vector2 piece = size_of(tab);
    const double range = bar->get_max() - bar->get_min() - bar->get_page();
    const float ratio = range > 0.0 ? static_cast<float>((bar->get_value() - bar->get_min()) / range) : 0.0F;
    const float travel = std::max(0.0F, size.y - top - bottom - piece.y);
    bar->draw_texture_rect(tab, Rect2(std::round((size.x - piece.x) / 2.0F), top + std::round(travel * ratio),
                                      piece.x, piece.y),
                           false);
}

// --- Combo -------------------------------------------------------------------

EawrUiCombo::EawrUiCombo() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
    set_text_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    set_focus_mode(FOCUS_NONE);
}

void EawrUiCombo::set_forced_state(const KitState state) {
    forced_ = state;
    queue_redraw();
}

void EawrUiCombo::apply_skin() {
    if (applying_) return;
    applying_ = true;
    begin_bulk_theme_override();
    const Vector2 cap = size_of(icon(*this, "Combo_Box_Left_Cap"));
    const Vector2 button = size_of(icon(*this, "Combo_Box_Popdown_Button"));
    Ref<StyleBoxTexture> body = texture_box(icon(*this, "Combo_Box_Text_Box"), false);
    body->set_content_margin(SIDE_LEFT, cap.x + 4.0F);
    body->set_content_margin(SIDE_RIGHT, button.x + 4.0F);
    body->set_content_margin(SIDE_TOP, 0.0F);
    body->set_content_margin(SIDE_BOTTOM, 0.0F);
    for (const char* name : {"normal", "hover", "pressed", "hover_pressed", "disabled", "normal_mirrored",
                             "hover_mirrored", "pressed_mirrored", "hover_pressed_mirrored", "disabled_mirrored"}) {
        add_theme_stylebox_override(name, body);
    }
    add_theme_stylebox_override("focus", empty_box());
    add_theme_icon_override("arrow", empty_texture());
    const KitTextStyle style = kit_text_style(*this, "Combo_Box");
    add_theme_font_override("font", style.font);
    add_theme_font_size_override("font_size", style.size);
    apply_text_colours(*this, style, {"font_color", "font_hover_color", "font_pressed_color", "font_focus_color",
                                      "font_hover_pressed_color"});
    add_theme_color_override("font_disabled_color", Color(style.top.r, style.top.g, style.top.b,
                                                         style.top.a * disabled_alpha));
    end_bulk_theme_override();

    if (PopupMenu* popup = get_popup()) {
        popup->begin_bulk_theme_override();
        popup->add_theme_stylebox_override("panel", texture_box(icon(*this, "Small_Frame_Background"), true));
        popup->add_theme_stylebox_override("hover", flat_box(selection_colour));
        popup->add_theme_font_override("font", style.font);
        popup->add_theme_font_size_override("font_size", style.size);
        popup->add_theme_color_override("font_color", style.top);
        popup->add_theme_color_override("font_hover_color", style.top);
        popup->end_bulk_theme_override();
    }
    applying_ = false;
}

void EawrUiCombo::_notification(const int what) {
    if (what == NOTIFICATION_THEME_CHANGED) apply_skin();
}

void EawrUiCombo::_draw() {
    const KitState current = button_state(*this, forced_);
    const float alpha = current == KitState::disabled ? disabled_alpha : 1.0F;
    const Vector2 size = get_size();
    const Ref<Texture2D> cap = icon(*this, "Combo_Box_Left_Cap");
    draw_piece(*this, cap, Rect2(0.0F, 0.0F, size_of(cap).x, size.y), Color(1, 1, 1, alpha));
    const Ref<Texture2D> button =
        state_icon(*this, "Combo_Box_Popdown_Button", current == KitState::disabled ? KitState::normal : current);
    const Vector2 native = size_of(button);
    // The button keeps its aspect at the box's height.
    const float width = native.y > 0.0F ? native.x * size.y / native.y : 0.0F;
    draw_piece(*this, button, Rect2(size.x - width, 0.0F, width, size.y), Color(1, 1, 1, alpha));
}

// --- Edit --------------------------------------------------------------------

EawrUiEdit::EawrUiEdit() {
    set_theme_type_variation("EawrUi");
    set_texture_repeat(CanvasItem::TEXTURE_REPEAT_DISABLED);
}

void EawrUiEdit::set_ime(const bool ime) {
    ime_ = ime;
    apply_skin();
}

void EawrUiEdit::apply_skin() {
    if (applying_ || !is_inside_tree()) return;
    applying_ = true;
    begin_bulk_theme_override();
    const Border border = small_border(*this);
    const float scale = piece_scale(*this);
    Ref<StyleBoxTexture> box = texture_box(icon(*this, "Small_Frame_Background"), true);
    box->set_content_margin(SIDE_LEFT, border.left + std::round(3.0F * scale));
    box->set_content_margin(SIDE_RIGHT, border.right + std::round(3.0F * scale));
    box->set_content_margin(SIDE_TOP, border.top);
    box->set_content_margin(SIDE_BOTTOM, border.bottom);
    add_theme_stylebox_override("normal", box);
    add_theme_stylebox_override("read_only", box);
    add_theme_stylebox_override("focus", empty_box());
    const KitTextStyle style = kit_text_style(*this, ime_ ? "IME_Edit_Box" : "Edit_Box");
    add_theme_font_override("font", style.font);
    add_theme_font_size_override("font_size", style.size);
    apply_text_colours(*this, style, {"font_color", "caret_color", "font_selected_color"});
    add_theme_color_override("font_uneditable_color", Color(style.top.r, style.top.g, style.top.b,
                                                           style.top.a * disabled_alpha));
    add_theme_color_override("font_placeholder_color", Color(style.top.r, style.top.g, style.top.b, 0.4F));
    add_theme_color_override("selection_color", selection_colour);
    end_bulk_theme_override();
    applying_ = false;
}

void EawrUiEdit::_notification(const int what) {
    if (what == NOTIFICATION_THEME_CHANGED) apply_skin();
}

void EawrUiEdit::_draw() {
    draw_small_frame(*this, Rect2(Vector2(), get_size()), false);
}

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
