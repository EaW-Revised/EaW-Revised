#include "battle_overlay.hpp"

#include "eawr/presentation/ui/battle_messages.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/range.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/classes/style_box_texture.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), static_cast<std::size_t>(converted.length()));
}

[[nodiscard]] std::string json(const std::string& value) {
    std::string output = "\"";
    for (const char character : value) {
        if (character == '"' || character == '\\') output += '\\';
        output += character;
    }
    return output + "\"";
}

// TP-07: diagnostic fallback, also used by the project Begin barrier.
constexpr float banner_top = 0.08F;
// BEP-03: the end panel's box, in screen fractions, below the win/lose message (BE-03 at 0.4).
constexpr float panel_width = 0.36F;
constexpr float panel_top = 0.5F;
constexpr float panel_height = 0.2F;

} // namespace

EawrOverlayButton::EawrOverlayButton() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
}

void EawrOverlayButton::set_caption(const KitTextStyle& style, const String& text, const float scale) {
    text_.draw(*this, style, text, Rect2(Vector2(), get_size()), HORIZONTAL_ALIGNMENT_CENTER, true, false, scale, 1.0F);
}

void EawrOverlayButton::activate() {
    ++presses_;
    if (action_) action_();
}

void EawrOverlayButton::set_rollover(const Ref<Texture2D>& texture) {
    rollover_ = memnew(TextureRect);
    rollover_->set_texture(texture);
    rollover_->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
    rollover_->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
    rollover_->set_mouse_filter(MOUSE_FILTER_IGNORE);
    rollover_->set_draw_behind_parent(true);
    rollover_->hide();
    add_child(rollover_);
    connect("mouse_entered", Callable(rollover_, "show"));
    connect("mouse_exited", Callable(rollover_, "hide"));
}

void EawrOverlayButton::_gui_input(const Ref<InputEvent>& event) {
    const Ref<InputEventMouseButton> button = event;
    if (button.is_null() || button->get_button_index() != MOUSE_BUTTON_LEFT) return;
    if (button->is_pressed()) {
        armed_ = true;
    } else if (armed_) {
        armed_ = false;
        if (Rect2(Vector2(), get_size()).has_point(button->get_position())) {
            activate();
        }
    }
    accept_event();
}

EawrBattleOverlay::EawrBattleOverlay() {
    set_mouse_filter(MOUSE_FILTER_IGNORE);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EawrBattleOverlay::~EawrBattleOverlay() {
    if (auto* rendering = RenderingServer::get_singleton())
        for (const auto& item : pause_items_) rendering->free_rid(item);
}

void EawrBattleOverlay::setup(Style style, std::function<void()> resume, std::function<void()> quit) {
    begin_action_ = resume;
    style_ = std::move(style);
    auto* rendering = RenderingServer::get_singleton();
    for (const auto& mesh : style_.pause_meshes) {
        const auto item = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(item, get_canvas_item());
        rendering->canvas_item_set_default_texture_repeat(item, RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
        rendering->canvas_item_set_default_texture_filter(item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
        if (mesh.blend == model::ShellBlend::additive) {
            if (pause_additive_.is_null()) {
                pause_additive_.instantiate();
                pause_additive_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
            }
            rendering->canvas_item_set_material(item, pause_additive_->get_rid());
        }
        pause_items_.push_back(item);
    }
    // After the end the whole view stops the pointer, so the finished battle takes no orders.
    panel_ = memnew(Control);
    panel_->set_name("EawrBattleEndPanel");
    panel_->set_mouse_filter(MOUSE_FILTER_STOP);
    panel_->set_anchors_and_offsets_preset(PRESET_FULL_RECT);
    panel_->set_visible(false);
    add_child(panel_);
    resume_ = memnew(EawrOverlayButton);
    resume_->set_name("EawrResumeGame");
    resume_->set_action(std::move(resume));
    resume_->set_visible(false);
    add_child(resume_);
    if (style_.pause_rollover.is_valid()) resume_->set_rollover(style_.pause_rollover);
    quit_ = memnew(EawrOverlayButton);
    quit_->set_name("EawrQuitGame");
    quit_->set_action(std::move(quit));
    quit_->set_visible(false);
    add_child(quit_);
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrBattleOverlay::show_message(const std::optional<bool> won) {
    if (message_ == won) return;
    message_ = won;
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrBattleOverlay::show_paused(const bool paused) {
    if (paused_ == paused) return;
    paused_ = paused;
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrBattleOverlay::show_end(const std::optional<bool> won) {
    if (ended_ == won) return;
    ended_ = won;
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrBattleOverlay::set_results(const model::BattleResults& results) {
    if (results_ == results) return;
    results_ = results;
    if (ended_) layout_results();
}

void EawrBattleOverlay::scroll_your(const double value) {
    result_scroll_[0] = static_cast<std::size_t>(std::max(0.0, value));
    layout_results();
}

void EawrBattleOverlay::scroll_enemy(const double value) {
    result_scroll_[1] = static_cast<std::size_t>(std::max(0.0, value));
    layout_results();
}

void EawrBattleOverlay::layout_results() {
    if (!result_dialog_ || !ended_) return;
    for (std::size_t side = 0; side < result_scroll_.size(); ++side) {
        result_scroll_[side] = std::min(result_scroll_[side], model::loss_scroll_max(results_.losses[side].size()));
    }
    for (auto& gadget : result_dialog_->gadgets) {
        if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) {
            if (gadget.id == "IDC_TITLE_STATIC") label->set_text(*ended_ ? style_.victory_title : style_.defeat_title);
            else if (gadget.id == "IDC_TIME_STATIC") label->set_text(style_.battle_time + " "
                + String(model::battle_time_text(results_.elapsed_milliseconds).c_str()));
        }
        for (std::size_t side = 0; side < 2; ++side) {
            const std::string prefix = side == 0 ? "IDC_YOUR_" : "IDC_ENEMY_";
            if (gadget.id == prefix + "LOSS_VAL_STATIC") {
                if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control))
                    label->set_text(String(results_.scores[side].c_str()));
            }
            for (std::size_t slot = 0; slot < model::battle_loss_visible_entries; ++slot) {
                const auto index = result_scroll_[side] + slot;
                const auto* row = index < results_.losses[side].size() ? &results_.losses[side][index] : nullptr;
                if (gadget.id == prefix + "UNIT_NAME" + std::to_string(slot)) {
                    if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) {
                        const String name = row != nullptr && style_.result_name ? style_.result_name(row->name) : String();
                        const String count = row != nullptr && row->count > 1
                            ? " x " + String(std::to_string(row->count).c_str()) : String();
                        label->set_text(row != nullptr ? name + count : String());
                    }
                }
                if (gadget.id == prefix + "UNIT" + std::to_string(slot)) {
                    if (auto* icon = Object::cast_to<TextureRect>(gadget.control)) {
                        icon->set_texture(row != nullptr && style_.result_icon ? style_.result_icon(row->name) : Ref<Texture2D>());
                        icon->set_visible(row != nullptr);
                    }
                }
            }
            if (gadget.id == prefix + "SCROLLBAR") {
                if (auto* range = Object::cast_to<Range>(gadget.control)) {
                    range->set_max(static_cast<double>(model::loss_scroll_max(results_.losses[side].size())));
                    range->set_value_no_signal(static_cast<double>(result_scroll_[side]));
                    range->set_visible(model::loss_scroll_max(results_.losses[side].size()) != 0);
                }
            }
            for (std::size_t slot = 0; slot < 5; ++slot) {
                if (gadget.id != prefix + "HERO" + std::to_string(slot)) continue;
                if (auto* icon = Object::cast_to<TextureRect>(gadget.control)) {
                    const auto& heroes = results_.heroes[side];
                    const auto* hero = slot < heroes.size() ? &heroes[slot] : nullptr;
                    icon->set_texture(hero && style_.result_icon ? style_.result_icon(hero->name) : Ref<Texture2D>());
                    icon->set_visible(hero != nullptr);
                }
            }
        }
    }
}

void EawrBattleOverlay::show_begin(const bool ready) {
    if (ready_ == ready) return;
    ready_ = ready;
    set_mouse_filter(ready ? MOUSE_FILTER_STOP : MOUSE_FILTER_IGNORE);
    set_process_input(ready);
    laid_out_ = Vector2(-1.0F, -1.0F);
    relayout();
}

void EawrBattleOverlay::_input(const Ref<InputEvent>& event) {
    // WBF-10: the local Begin barrier also admits keyboard activation. These
    // bindings are the viewer's input policy, not a claim about retail key routing.
    if (!ready_) return;
    const auto* key = Object::cast_to<InputEventKey>(event.ptr());
    if (key == nullptr || !key->is_pressed() || key->is_echo()) return;
    if (key->get_keycode() == KEY_ENTER || key->get_keycode() == KEY_KP_ENTER || key->get_keycode() == KEY_SPACE) {
        // The same action as the Begin button, without manufacturing a mouse event.
        if (begin_action_) begin_action_();
        get_viewport()->set_input_as_handled();
    }
}

void EawrBattleOverlay::_notification(const int what) {
    if (what == NOTIFICATION_RESIZED || what == NOTIFICATION_READY || what == NOTIFICATION_ENTER_TREE) relayout();
    if (what == NOTIFICATION_DRAW && ended_ && !result_dialog_ && panel_rect_.size.x > 0.0F) {
        // BEP-03: a dark box with a hairline in the result's colour.
        const Color edge = *ended_ ? style_.win : style_.lose;
        draw_rect(panel_rect_, Color(0.0F, 0.0F, 0.0F, 0.75F), true);
        draw_rect(panel_rect_, Color(edge.r, edge.g, edge.b, 0.8F), false, 1.0F);
    }
}

KitTextStyle EawrBattleOverlay::text_style(const Font& font, const Color& colour, int& pixels) const {
    KitTextStyle style;
    style.font = font.font;
    const model::ReferenceSpace space = style_.space ? style_.space() : model::ReferenceSpace{};
    pixels = model::font_pixels({font.point_size, false, 1.0}, model::font_screen_height(space)).glyph_height;
    style.size = pixels;
    style.top = style.bottom = colour;
    return style;
}

float EawrBattleOverlay::text_width(const Font& font, const int pixels, const String& text) const {
    if (font.font.is_null() || pixels <= 0) return 0.0F;
    return font.font->get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, pixels).x;
}

void EawrBattleOverlay::relayout() {
    const Vector2 size = get_size();
    if (size.x <= 0.0F || size.y <= 0.0F || size == laid_out_ || resume_ == nullptr) return;
    laid_out_ = size;
    if (result_dialog_ && result_dialog_->frame != nullptr && result_dialog_->frame->get_size() != size) {
        result_dialog_->frame->queue_free();
        result_dialog_.reset();
    }
    const float scale = size.y / 720.0F;
    // BE-03: centred, its top at 0.4 of the height.
    message_rect_ = Rect2();
    if (message_ && !ended_) {
        const Color colour = *message_ ? style_.win : style_.lose;
        const KitTextStyle style = text_style(style_.message, colour, message_pixels_);
        const String& text = *message_ ? style_.win_text : style_.lose_text;
        const float width = text_width(style_.message, message_pixels_, text);
        const model::MessagePlacement at = model::battle_message_placement(size.x, size.y, width);
        const float height = style.font.is_valid() ? style.font->get_height(style.size) : 0.0F;
        message_rect_ = Rect2(static_cast<float>(at.x), static_cast<float>(at.y), width, height);
        message_text_.draw(*this, style, text, message_rect_, HORIZONTAL_ALIGNMENT_LEFT, false, false, scale, 1.0F);
    } else {
        message_text_.draw(*this, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1.0F, 1.0F);
    }
    // TM-09, TP-07: the pause text, and Resume Game under it.
    int banner_pixels = 0;
    int button_pixels = 0;
    const KitTextStyle button_style = text_style(style_.button, style_.button_colour, button_pixels);
    const float button_line = button_style.font.is_valid() ? button_style.font->get_height(button_style.size) : 0.0F;
    banner_rect_ = Rect2();
    const bool banner = (paused_ || ready_) && !ended_;
    const bool native_pause = paused_ && !ready_ && !ended_ && style_.pause_text && style_.pause_button;
    const model::ReferenceSpace reference = style_.space ? style_.space() : model::ReferenceSpace{};
    const model::ShellPlacement pause_at{static_cast<double>(size.x) / 2.0, 0.0, reference.scale};
    auto* rendering = RenderingServer::get_singleton();
    for (std::size_t index = 0; index < pause_items_.size(); ++index) {
        const auto item = pause_items_[index];
        rendering->canvas_item_set_visible(item, native_pause);
        rendering->canvas_item_clear(item);
        const auto& mesh = style_.pause_meshes[index];
        if (!native_pause || mesh.texture.is_null()) continue;
        PackedVector2Array points, uvs;
        PackedColorArray colours;
        PackedInt32Array indices;
        for (const auto& triangle : mesh.triangles) for (const auto& vertex : triangle.vertices) {
            const auto point = model::shell_point_to_screen(vertex.position.x, vertex.position.y, pause_at);
            indices.push_back(static_cast<int32_t>(points.size()));
            points.push_back(Vector2(static_cast<float>(point.x), static_cast<float>(point.y)));
            uvs.push_back(Vector2(vertex.uv.x, vertex.uv.y));
            colours.push_back(Color(1, 1, 1, 1));
        }
        rendering->canvas_item_add_triangle_array(item, indices, points, colours, uvs, PackedInt32Array(),
            PackedFloat32Array(), mesh.texture->get_rid());
    }
    const String& banner_caption = ready_ ? style_.loading_title : style_.paused_text;
    const String& button_caption = ready_ ? style_.begin_text : style_.resume_text;
    if (banner) {
        const KitTextStyle style = text_style(style_.banner, style_.paused, banner_pixels);
        const float width = text_width(style_.banner, banner_pixels, banner_caption);
        const float height = style.font.is_valid() ? style.font->get_height(style.size) : 0.0F;
        banner_rect_ = Rect2((size.x - width) * 0.5F, size.y * banner_top, width, height);
        if (native_pause) {
            const auto rect = model::shell_to_screen(*style_.pause_text, pause_at);
            banner_rect_ = Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y),
                static_cast<float>(rect.width), static_cast<float>(rect.height));
            if (rect.height == 0.0) {
                banner_rect_.position.y -= height / 2.0F;
                banner_rect_.size.y = height;
            }
        }
        banner_text_.draw(*this, style, banner_caption, banner_rect_,
            native_pause ? HORIZONTAL_ALIGNMENT_CENTER : HORIZONTAL_ALIGNMENT_LEFT, native_pause, false, scale, 1.0F);
        const float button_width = text_width(style_.button, button_pixels, button_caption) + 16.0F * scale;
        resume_->set_position(Vector2((size.x - button_width) * 0.5F, banner_rect_.get_end().y + 4.0F * scale));
        resume_->set_size(Vector2(button_width, button_line + 6.0F * scale));
        KitTextStyle resume_style = button_style;
        if (native_pause) {
            const auto rect = model::shell_to_screen(*style_.pause_button, pause_at);
            resume_->set_position(Vector2(static_cast<float>(rect.x), static_cast<float>(rect.y)));
            resume_->set_size(Vector2(static_cast<float>(rect.width), static_cast<float>(rect.height)));
            int native_pixels = 0;
            resume_style = text_style(style_.native_button, style_.button_colour, native_pixels);
            resume_style.emboss = style_.button_emboss;
            resume_style.outline = style_.button_outline;
        }
        resume_->set_caption(resume_style, button_caption, scale);
        if (paused_ && !ready_) {
            const float quit_width = text_width(style_.button, button_pixels, style_.quit_text) + 16.0F * scale;
            quit_->set_position(Vector2((size.x - quit_width) * 0.5F,
                resume_->get_position().y + resume_->get_size().y + 4.0F * scale));
            quit_->set_size(Vector2(quit_width, button_line + 6.0F * scale));
            quit_->set_caption(button_style, style_.quit_text, scale);
        }
    } else {
        banner_text_.draw(*this, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1.0F, 1.0F);
    }
    resume_->set_visible(banner);
    // BEP-03: the end panel.
    panel_rect_ = Rect2();
    if (ended_) {
        if (!result_dialog_ && style_.results_dialog) {
            result_dialog_.emplace(style_.results_dialog(size));
            if (result_dialog_->frame == nullptr) result_dialog_.reset();
            else {
                panel_->add_child(result_dialog_->frame);
                for (auto& gadget : result_dialog_->gadgets) {
                    if (gadget.class_name == "PETROGLYPH_DIALOG_IMAGE") {
                        if (gadget.control != nullptr) gadget.control->queue_free();
                        auto* icon = memnew(TextureRect);
                        icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
                        icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
                        icon->set_mouse_filter(MOUSE_FILTER_IGNORE);
                        icon->set_position(Vector2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y))
                            - result_dialog_->frame->get_position());
                        icon->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
                        result_dialog_->frame->add_child(icon);
                        gadget.control = icon;
                        icon->hide();
                    } else if (gadget.id == "IDC_CONTINUE_BUTTON" && gadget.control != nullptr) {
                        if (auto* skin = Object::cast_to<EawrUiButton>(gadget.control)) {
                            skin->set_text(style_.exit_text);
                            skin->connect("pressed", callable_mp(quit_, &EawrOverlayButton::activate));
                        }
                        quit_->set_position(gadget.control->get_position() + result_dialog_->frame->get_position());
                        quit_->set_size(gadget.control->get_size());
                        quit_->set_caption(button_style, String(), scale);
                    } else if (gadget.id == "IDC_YOUR_SCROLLBAR" || gadget.id == "IDC_ENEMY_SCROLLBAR") {
                        // The stock resource marks these trackbars vertical. Keep their
                        // authored rect/variation and use the existing vertical scroll skin.
                        if (gadget.control != nullptr) gadget.control->queue_free();
                        auto* bar = memnew(VScrollBar);
                        bar->set_theme_type_variation(String(gadget.variation.c_str()));
                        bar->set_position(Vector2(static_cast<float>(gadget.rect.x), static_cast<float>(gadget.rect.y))
                            - result_dialog_->frame->get_position());
                        bar->set_size(Vector2(static_cast<float>(gadget.rect.width), static_cast<float>(gadget.rect.height)));
                        result_dialog_->frame->add_child(bar);
                        for (const auto& [name, slot] : {std::pair{"decrement", "Scroll_Up_Button"},
                            std::pair{"decrement_highlight", "Scroll_Up_Button_Mouse_Over"},
                            std::pair{"decrement_pressed", "Scroll_Up_Button_Pressed"},
                            std::pair{"increment", "Scroll_Down_Button"},
                            std::pair{"increment_highlight", "Scroll_Down_Button_Mouse_Over"},
                            std::pair{"increment_pressed", "Scroll_Down_Button_Pressed"}}) {
                            bar->add_theme_icon_override(name, bar->get_theme_icon(slot, String(gadget.variation.c_str())));
                        }
                        for (const auto& [name, slot] : {std::pair{"scroll", "Scroll_Middle"},
                            std::pair{"scroll_focus", "Scroll_Middle"}, std::pair{"grabber", "Scroll_Tab"},
                            std::pair{"grabber_highlight", "Scroll_Tab"}, std::pair{"grabber_pressed", "Scroll_Tab"}}) {
                            Ref<StyleBoxTexture> skin;
                            skin.instantiate();
                            const auto texture = bar->get_theme_icon(slot, String(gadget.variation.c_str()));
                            skin->set_texture(texture);
                            if (texture.is_valid()) {
                                skin->set_content_margin(SIDE_TOP, static_cast<float>(texture->get_height()) / 2.0F);
                                skin->set_content_margin(SIDE_BOTTOM, static_cast<float>(texture->get_height()) / 2.0F);
                            }
                            bar->add_theme_stylebox_override(name, skin);
                        }
                        gadget.control = bar;
                        if (auto* range = Object::cast_to<Range>(gadget.control)) {
                            range->set_step(1.0);
                            range->connect("value_changed", gadget.id == "IDC_YOUR_SCROLLBAR"
                                ? callable_mp(this, &EawrBattleOverlay::scroll_your) : callable_mp(this, &EawrBattleOverlay::scroll_enemy));
                        }
                    } else if (gadget.control != nullptr && gadget.id != "IDC_TITLE_STATIC"
                        && gadget.id != "IDC_TIME_STATIC" && gadget.id != "IDC_STATIC_LOSS1" && gadget.id != "IDC_STATIC_LOSS2"
                        && gadget.id != "IDC_YOUR_LOSS_VAL_STATIC" && gadget.id != "IDC_ENEMY_LOSS_VAL_STATIC"
                        && !gadget.id.starts_with("IDC_YOUR_UNIT_NAME") && !gadget.id.starts_with("IDC_ENEMY_UNIT_NAME")) {
                        gadget.control->hide(); // replay/network/land controls require their own contexts (WBF-47)
                    }
                }
            }
        }
        if (result_dialog_) {
            panel_rect_ = Rect2(Vector2(), size);
            layout_results();
        } else {
        panel_rect_ = Rect2(size.x * (1.0F - panel_width) * 0.5F, size.y * panel_top, size.x * panel_width,
                            size.y * panel_height);
        int title_pixels = 0;
        const KitTextStyle title_style = text_style(style_.message, *ended_ ? style_.win : style_.lose, title_pixels);
        const String& title = *ended_ ? style_.victory_title : style_.defeat_title;
        title_text_.draw(*this, title_style, title,
                         Rect2(panel_rect_.position, Vector2(panel_rect_.size.x, panel_rect_.size.y * 0.55F)),
                         HORIZONTAL_ALIGNMENT_CENTER, true, false, scale, 1.0F);
        const float button_width = text_width(style_.button, button_pixels, style_.quit_text) + 24.0F * scale;
        const float button_height = button_line + 8.0F * scale;
        quit_->set_position(Vector2(panel_rect_.get_center().x - button_width * 0.5F,
                                    panel_rect_.get_end().y - button_height - panel_rect_.size.y * 0.15F));
        quit_->set_size(Vector2(button_width, button_height));
        quit_->set_caption(button_style, style_.quit_text, scale);
        }
    } else {
        title_text_.draw(*this, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1.0F, 1.0F);
    }
    panel_->set_visible(ended_.has_value());
    quit_->set_mouse_filter(ended_ && result_dialog_ ? MOUSE_FILTER_IGNORE : MOUSE_FILTER_STOP);
    quit_->set_visible(ended_.has_value() || (paused_ && !ready_));
    queue_redraw();
}

std::optional<Rect2> EawrBattleOverlay::control_rect(const std::string_view name) const {
    if (name == "message" && message_ && !ended_) return message_rect_;
    if ((name == "resume" || (name == "begin" && ready_)) && resume_ != nullptr && resume_->is_visible()) return Rect2(resume_->get_position(), resume_->get_size());
    if ((name == "quit" || name == "exit") && quit_ != nullptr && (quit_->is_visible() || (ended_ && result_dialog_)))
        return Rect2(quit_->get_position(), quit_->get_size());
    if (name == "end_panel" && ended_) return panel_rect_;
    return std::nullopt;
}

std::string EawrBattleOverlay::report_json() const {
    std::ostringstream output;
    output << "{\"message\": ";
    if (message_ && !ended_) {
        output << "{\"result\": " << json(*message_ ? "victory" : "defeat")
               << ", \"text\": " << json(utf8(*message_ ? style_.win_text : style_.lose_text))
               << ", \"rect\": " << rect_json(message_rect_) << ", \"pixels\": " << message_pixels_ << "}";
    } else {
        output << "null";
    }
    output << ", \"begin_visible\": " << (ready_ ? "true" : "false")
           << ", \"quit_visible\": " << (quit_ && quit_->is_visible() ? "true" : "false")
           << ", \"authored_pause_shell\": " << (!style_.pause_meshes.empty() && style_.pause_text && style_.pause_button ? "true" : "false")
           << ", \"resume_rect\": " << rect_json(Rect2(resume_->get_position(), resume_->get_size()))
           << ", \"paused_banner\": " << (paused_ && !ended_ ? "{\"text\": " + json(utf8(style_.paused_text))
                                                                 + ", \"rect\": " + rect_json(banner_rect_) + "}"
                                                           : std::string("null"))
           << ", \"end_panel\": ";
    if (ended_) {
        output << "{\"title\": " << json(utf8(*ended_ ? style_.victory_title : style_.defeat_title))
               << ", \"rect\": " << rect_json(panel_rect_)
               << ", \"authored_dialog\": " << (result_dialog_ ? "true" : "false")
               << ", \"quit_rect\": " << rect_json(Rect2(quit_->get_position(), quit_->get_size())) << "}";
    } else {
        output << "null";
    }
    output << ", \"resume_presses\": " << (resume_ ? resume_->presses() : 0)
           << ", \"quit_presses\": " << (quit_ ? quit_->presses() : 0) << "}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
