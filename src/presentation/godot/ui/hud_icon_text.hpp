#pragma once

#include "ui/kit.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/fonts.hpp"
#include <godot_cpp/classes/texture2d.hpp>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace eawr::presentation::godot_backend {

// Shared HUD readout adapter: the caller supplies an already prepared value.
// Both tactical and galactic populations retain their own accounting rules.
struct HudIconTextRenderer final {
    KitText text;
    godot::Ref<godot::Font> font;
    godot::Ref<godot::Texture2D> icon;
    std::function<presentation::ui::TextCell(std::int32_t)> cell;
    godot::Rect2 text_rect;
    godot::Rect2 icon_rect;

    // Draw an authored text cell into a caller-prepared band. The caller owns
    // placement and content; this shares font metrics and rendering with HUD
    // readouts whose placement is computed below.
    void draw_cell(godot::Control& parent, const presentation::ui::HudIconText& look,
                   const godot::String& value, const godot::Rect2& box, const int pixels,
                   const godot::HorizontalAlignment alignment, const float scale,
                   const float outline_pixels = 2.0F, const float alpha = 1.0F) {
        KitTextStyle style;
        style.font = font;
        style.size = pixels;
        const auto& c = look.text.colour;
        style.top = style.bottom = godot::Color(c.r / 255.0F, c.g / 255.0F, c.b / 255.0F, c.a / 255.0F);
        style.outline = look.text.outline;
        style.outline_pixels = outline_pixels;
        style.emboss = look.text.emboss;
        if (cell) {
            const auto metrics = cell(style.size);
            style.cell_ascent = metrics.ascent;
            style.cell_descent = metrics.descent;
        }
        text.draw(parent, style, value, box, alignment, true, false, scale, alpha);
    }

    void update(godot::Control& parent, const presentation::ui::HudIconText& look,
                const godot::String& value, const presentation::ui::ShellPlacement& placement,
                const presentation::ui::ReferenceSpace& space, const float alpha = 1.0F,
                const bool centred = false) {
        namespace model = presentation::ui;
        const auto screen = [&](const data::ui::ReferenceRect& rect) {
            const auto p = model::shell_to_screen(rect, placement);
            return godot::Rect2(static_cast<float>(p.x), static_cast<float>(p.y),
                static_cast<float>(p.width), static_cast<float>(p.height));
        };
        const float y = look.origin.y + look.text_offset.y;
        const float right = look.origin.x + look.text_offset.x;
        // Presenter clipping box in shell units; authored origin and offsets
        // locate the readout (HUD-POP-01), independently of this text budget.
        const int pixels = model::font_pixels({look.text.point_size, false, 1.0}, model::font_screen_height(space)).glyph_height;
        const float text_budget_width = centred && font.is_valid() && placement.scale > 0
            ? std::max(1.0F, font->get_string_size(value, godot::HORIZONTAL_ALIGNMENT_LEFT, -1, pixels).x
                / static_cast<float>(placement.scale)) : 120.0F;
        constexpr float text_budget_height = 16.0F;
        const float left = look.left_justified ? right
            : right - text_budget_width * (centred && !look.right_justified ? 0.5F : 1.0F);
        text_rect = screen({left, y - text_budget_height / 2.0F,
                            text_budget_width, text_budget_height});
        draw_cell(parent, look, value, text_rect, pixels,
            look.right_justified ? godot::HORIZONTAL_ALIGNMENT_RIGHT : godot::HORIZONTAL_ALIGNMENT_LEFT,
            static_cast<float>(placement.scale), 2.0F, alpha);
        icon_rect = {};
        if (icon.is_valid() && !value.is_empty()) {
            const auto size = icon->get_size();
            icon_rect = screen({look.origin.x + look.icon_offset.x - size.x / 2.0F,
                look.origin.y + look.icon_offset.y - size.y / 2.0F, size.x, size.y});
        }
    }
    void draw_icon(godot::Control& parent) const {
        if (icon.is_valid() && icon_rect.has_area()) parent.draw_texture_rect(icon, icon_rect, false);
    }
};

// A prepared HUD text band with ordinary Control hover routing. It contains no
// accounting or layout policy; GC readouts, card prices and queue percentages
// supply their own values and authored bands.
class EawrHudText final : public godot::Control {
    GDCLASS(EawrHudText, godot::Control)
public:
    EawrHudText() { set_mouse_filter(MOUSE_FILTER_IGNORE); }
    void set_text(const godot::String& value) { value_ = value; update_minimum_size(); queue_redraw(); }
    [[nodiscard]] godot::String get_text() const { return value_; }
    [[nodiscard]] int glyph_pixels() const { return pixels_; }
    [[nodiscard]] const presentation::ui::HudIconText& style() const { return look_; }
    [[nodiscard]] godot::Ref<godot::Font> font() const { return renderer_.font; }
    void set_style(presentation::ui::HudIconText look, godot::Ref<godot::Font> font,
                   std::function<presentation::ui::TextCell(std::int32_t)> cell,
                   const int pixels, const float scale, const float outline_pixels) {
        look_ = std::move(look);
        renderer_.font = std::move(font);
        renderer_.cell = std::move(cell);
        pixels_ = pixels; scale_ = scale; outline_pixels_ = outline_pixels;
        update_minimum_size();
        queue_redraw();
    }
    void set_horizontal_alignment(const godot::HorizontalAlignment value) { alignment_ = value; queue_redraw(); }
    [[nodiscard]] godot::HorizontalAlignment get_horizontal_alignment() const { return alignment_; }
    void set_colour(const godot::Color& colour) {
        look_.text.colour = {static_cast<std::uint8_t>(std::lround(colour.r * 255)),
            static_cast<std::uint8_t>(std::lround(colour.g * 255)),
            static_cast<std::uint8_t>(std::lround(colour.b * 255)),
            static_cast<std::uint8_t>(std::lround(colour.a * 255))};
        queue_redraw();
    }
    [[nodiscard]] godot::Color colour() const {
        const auto& c = look_.text.colour;
        return godot::Color(c.r / 255.0F, c.g / 255.0F, c.b / 255.0F, c.a / 255.0F);
    }
    [[nodiscard]] godot::Vector2 _get_minimum_size() const override {
        // Height is the authored band, even when the glyph cell is taller.
        return renderer_.font.is_valid() && pixels_ > 0
            ? godot::Vector2(renderer_.font->get_string_size(value_, godot::HORIZONTAL_ALIGNMENT_LEFT, -1, pixels_).x, 0)
            : godot::Vector2{};
    }
    void _draw() override {
        renderer_.draw_cell(*this, look_, value_, godot::Rect2(godot::Vector2{}, get_size()),
            pixels_, alignment_, scale_, outline_pixels_);
    }
protected:
    static void _bind_methods() {}
private:
    HudIconTextRenderer renderer_;
    presentation::ui::HudIconText look_;
    godot::String value_;
    int pixels_{};
    float scale_{1.0F}, outline_pixels_{2.0F};
    godot::HorizontalAlignment alignment_{godot::HORIZONTAL_ALIGNMENT_LEFT};
};

} // namespace eawr::presentation::godot_backend
