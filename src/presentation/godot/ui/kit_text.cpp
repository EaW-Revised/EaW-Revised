#include "kit_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace kit_detail;

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


} // namespace eawr::presentation::godot_backend
