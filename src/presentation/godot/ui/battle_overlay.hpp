#pragma once

// #453 and #459: the battle's screen messages over the tactical HUD.
// - the win/lose message (docs/behaviour/battle-end.md BE-02, BE-03): one line in the
//   Win_Lose_Message_Font, centred, its top at 0.4 of the screen height;
// - the pause banner (docs/behaviour/tactical-time-controls.md TM-09, TP-07): the pause text and
//   a Resume Game button, drawn as texts only until the banner shell's pose is known;
// - the end panel (battle-end.md BEP-03): Victory!/Defeat! and a Quit Game button, standing in
//   for FoC's battle end dialog.
// The overlay covers the viewport and ignores the pointer; only its buttons and the end panel
// (which keeps clicks off the finished battle) take it.

#include "ui/kit.hpp"

#include "eawr/presentation/ui/layout.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend {

// A line of text that takes a left click: pressed and released over it runs its action.
class EawrOverlayButton final : public godot::Control {
    GDCLASS(EawrOverlayButton, godot::Control)

public:
    EawrOverlayButton();
    void set_action(std::function<void()> action) { action_ = std::move(action); }
    // The caption's look and text; the control keeps its rect, the caption is centred in it.
    void set_caption(const KitTextStyle& style, const godot::String& text, float scale);
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    [[nodiscard]] int presses() const { return presses_; }

protected:
    static void _bind_methods() {}

private:
    KitText text_;
    std::function<void()> action_;
    int presses_{};
    bool armed_{};
};

class EawrBattleOverlay final : public godot::Control {
    GDCLASS(EawrBattleOverlay, godot::Control)

public:
    struct Font final {
        godot::Ref<godot::Font> font;
        std::int32_t point_size{};
    };
    struct Style final {
        Font message;        // BE-03: Win_Lose_Message_Font at Win_Lose_Message_Font_Size
        godot::Color win;    // Win_Message_Color
        godot::Color lose;   // Lose_Message_Color
        Font banner;         // TM-09: the pause text's component font
        godot::Color paused; // Battle_Pending_Message_Color
        Font button;         // the Resume Game and Quit Game captions
        godot::Color button_colour;
        godot::String win_text;
        godot::String lose_text;
        godot::String victory_title;
        godot::String defeat_title;
        godot::String paused_text;
        godot::String resume_text;
        godot::String quit_text;
        // The layout's reference space for UI-F1 font sizes.
        std::function<presentation::ui::ReferenceSpace()> space;
    };

    EawrBattleOverlay();
    void setup(Style style, std::function<void()> resume, std::function<void()> quit);
    // BE-02: the local player's result (true: victory); nullopt hides the message.
    void show_message(std::optional<bool> won);
    void show_paused(bool paused);
    // BEP-03: the end panel for the result; nullopt hides it.
    void show_end(std::optional<bool> won);
    void _notification(int what);

    // A shown control's rect in viewport pixels: "message", "resume", "quit" or "end_panel".
    [[nodiscard]] std::optional<godot::Rect2> control_rect(std::string_view name) const;
    [[nodiscard]] std::string report_json() const;

protected:
    static void _bind_methods() {}

private:
    void relayout();
    [[nodiscard]] KitTextStyle text_style(const Font& font, const godot::Color& colour, int& pixels) const;
    [[nodiscard]] float text_width(const Font& font, int pixels, const godot::String& text) const;

    Style style_;
    std::optional<bool> message_;
    bool paused_{};
    std::optional<bool> ended_;
    KitText message_text_;
    KitText banner_text_;
    KitText title_text_;
    godot::Rect2 message_rect_;
    godot::Rect2 banner_rect_;
    godot::Rect2 panel_rect_;
    int message_pixels_{};
    EawrOverlayButton* resume_{};
    EawrOverlayButton* quit_{};
    godot::Control* panel_{};
    godot::Vector2 laid_out_{-1.0F, -1.0F};
};

} // namespace eawr::presentation::godot_backend
