#include "battle_overlay.hpp"

#include "eawr/presentation/ui/battle_messages.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>

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

// TP-07: the pause banner's texts sit at the top of the view until the shell's pose is known.
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

void EawrOverlayButton::_gui_input(const Ref<InputEvent>& event) {
    const Ref<InputEventMouseButton> button = event;
    if (button.is_null() || button->get_button_index() != MOUSE_BUTTON_LEFT) return;
    if (button->is_pressed()) {
        armed_ = true;
    } else if (armed_) {
        armed_ = false;
        if (Rect2(Vector2(), get_size()).has_point(button->get_position())) {
            ++presses_;
            if (action_) action_();
        }
    }
    accept_event();
}

EawrBattleOverlay::EawrBattleOverlay() {
    set_mouse_filter(MOUSE_FILTER_IGNORE);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

void EawrBattleOverlay::setup(Style style, std::function<void()> resume, std::function<void()> quit) {
    style_ = std::move(style);
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

void EawrBattleOverlay::_notification(const int what) {
    if (what == NOTIFICATION_RESIZED || what == NOTIFICATION_READY || what == NOTIFICATION_ENTER_TREE) relayout();
    if (what == NOTIFICATION_DRAW && ended_ && panel_rect_.size.x > 0.0F) {
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
    const float scale = size.y / 720.0F;
    // BE-03: centred, its top at 0.4 of the height.
    message_rect_ = Rect2();
    if (message_) {
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
    const bool banner = paused_ && !ended_;
    if (banner) {
        const KitTextStyle style = text_style(style_.banner, style_.paused, banner_pixels);
        const float width = text_width(style_.banner, banner_pixels, style_.paused_text);
        const float height = style.font.is_valid() ? style.font->get_height(style.size) : 0.0F;
        banner_rect_ = Rect2((size.x - width) * 0.5F, size.y * banner_top, width, height);
        banner_text_.draw(*this, style, style_.paused_text, banner_rect_, HORIZONTAL_ALIGNMENT_LEFT, false, false, scale, 1.0F);
        const float button_width = text_width(style_.button, button_pixels, style_.resume_text) + 16.0F * scale;
        resume_->set_position(Vector2((size.x - button_width) * 0.5F, banner_rect_.get_end().y + 4.0F * scale));
        resume_->set_size(Vector2(button_width, button_line + 6.0F * scale));
        resume_->set_caption(button_style, style_.resume_text, scale);
    } else {
        banner_text_.draw(*this, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1.0F, 1.0F);
    }
    resume_->set_visible(banner);
    // BEP-03: the end panel.
    panel_rect_ = Rect2();
    if (ended_) {
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
    } else {
        title_text_.draw(*this, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1.0F, 1.0F);
    }
    panel_->set_visible(ended_.has_value());
    quit_->set_visible(ended_.has_value());
    queue_redraw();
}

std::optional<Rect2> EawrBattleOverlay::control_rect(const std::string_view name) const {
    if (name == "message" && message_) return message_rect_;
    if (name == "resume" && resume_ != nullptr && resume_->is_visible()) return Rect2(resume_->get_position(), resume_->get_size());
    if (name == "quit" && quit_ != nullptr && quit_->is_visible()) return Rect2(quit_->get_position(), quit_->get_size());
    if (name == "end_panel" && ended_) return panel_rect_;
    return std::nullopt;
}

std::string EawrBattleOverlay::report_json() const {
    std::ostringstream output;
    output << "{\"message\": ";
    if (message_) {
        output << "{\"result\": " << json(*message_ ? "victory" : "defeat")
               << ", \"text\": " << json(utf8(*message_ ? style_.win_text : style_.lose_text))
               << ", \"rect\": " << rect_json(message_rect_) << ", \"pixels\": " << message_pixels_ << "}";
    } else {
        output << "null";
    }
    output << ", \"paused_banner\": " << (paused_ && !ended_ ? "{\"text\": " + json(utf8(style_.paused_text))
                                                                 + ", \"rect\": " + rect_json(banner_rect_) + "}"
                                                           : std::string("null"))
           << ", \"end_panel\": ";
    if (ended_) {
        output << "{\"title\": " << json(utf8(*ended_ ? style_.victory_title : style_.defeat_title))
               << ", \"rect\": " << rect_json(panel_rect_)
               << ", \"quit_rect\": " << rect_json(Rect2(quit_->get_position(), quit_->get_size())) << "}";
    } else {
        output << "null";
    }
    output << ", \"resume_presses\": " << (resume_ ? resume_->presses() : 0)
           << ", \"quit_presses\": " << (quit_ ? quit_->presses() : 0) << "}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
