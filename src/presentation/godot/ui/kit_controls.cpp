#include "kit_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace kit_detail;

namespace {
// White at 30 % over the list background gives the selection colour of the
// rig's FoC skirmish-setup capture; the hover bar is half that.
const Color selection_colour(1.0F, 1.0F, 1.0F, 0.3F);
const Color hover_colour(1.0F, 1.0F, 1.0F, 0.15F);
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

void apply_text_colours(Control& control, const KitTextStyle& style, std::initializer_list<const char*> names) {
    for (const char* name : names) control.add_theme_color_override(name, style.top);
}

} // namespace

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


} // namespace eawr::presentation::godot_backend
