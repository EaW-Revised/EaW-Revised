#pragma once

// #453 and #459: the battle's screen messages over the tactical HUD.
// - the win/lose message (docs/behaviour/battle-end.md BE-02, BE-03): one line in the
//   Win_Lose_Message_Font, centred, its top at 0.4 of the screen height;
// - the pause banner (docs/behaviour/tactical-time-controls.md TM-09, TP-07): the pause text and
//   a Resume Game button in the mounted shell's final show pose;
// - the fullscreen mounted battle result dialog (WBF-44/45), with lifetime loss panes and Exit.
// The overlay covers the viewport and ignores the pointer; only its buttons and the end panel
// (which keeps clicks off the finished battle) take it.

#include "ui/kit.hpp"
#include "ui/dialog_builder.hpp"
#include "eawr/presentation/ui/battle_results.hpp"

#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include <godot_cpp/classes/texture_rect.hpp>

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/canvas_item_material.hpp>
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
    void activate();
    void set_action(std::function<void()> action) { action_ = std::move(action); }
    // The caption's look and text; the control keeps its rect, the caption is centred in it.
    void set_caption(const KitTextStyle& style, const godot::String& text, float scale);
    void set_rollover(const godot::Ref<godot::Texture2D>& texture);
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    [[nodiscard]] int presses() const { return presses_; }

protected:
    static void _bind_methods() {}

private:
    KitText text_;
    godot::TextureRect* rollover_{};
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
        Font native_button;
        bool button_emboss{};
        bool button_outline{};
        struct Mesh final {
            presentation::ui::ShellBlend blend{};
            godot::Ref<godot::Texture2D> texture;
            std::vector<data::ui::ShellTriangle> triangles;
        };
        std::vector<Mesh> pause_meshes;
        std::optional<data::ui::ReferenceRect> pause_text;
        std::optional<data::ui::ReferenceRect> pause_button;
        godot::Ref<godot::Texture2D> pause_rollover;
        godot::Color button_colour;
        godot::String win_text;
        godot::String lose_text;
        godot::String victory_title;
        godot::String defeat_title;
        godot::String paused_text;
        godot::String resume_text;
        godot::String quit_text;
        godot::String begin_text;
        godot::String loading_title;
        // The layout's reference space for UI-F1 font sizes.
        std::function<presentation::ui::ReferenceSpace()> space;
        std::function<BuiltDialog(godot::Vector2)> results_dialog;
        std::function<godot::Ref<godot::Texture2D>(const std::string&)> result_icon;
        std::function<godot::String(const std::string&)> result_name;
        godot::String battle_time;
        godot::String exit_text;
    };

    EawrBattleOverlay();
    ~EawrBattleOverlay();
    void setup(Style style, std::function<void()> resume, std::function<void()> quit);
    // BE-02: the local player's result (true: victory); nullopt hides the message.
    void show_message(std::optional<bool> won);
    void show_paused(bool paused);
    void show_begin(bool ready);
    void _input(const godot::Ref<godot::InputEvent>& event) override;
    [[nodiscard]] bool paused_shown() const { return paused_; }
    // BEP-03: the end panel for the result; nullopt hides it.
    void show_end(std::optional<bool> won);
    void set_results(const presentation::ui::BattleResults& results);
    void _notification(int what);

    // A shown control's rect in viewport pixels: "message", "resume", "quit" or "end_panel".
    [[nodiscard]] std::optional<godot::Rect2> control_rect(std::string_view name) const;
    [[nodiscard]] std::string report_json() const;

protected:
    static void _bind_methods() {}

private:
    void relayout();
    void layout_results();
    void scroll_your(double value);
    void scroll_enemy(double value);
    [[nodiscard]] KitTextStyle text_style(const Font& font, const godot::Color& colour, int& pixels) const;
    [[nodiscard]] float text_width(const Font& font, int pixels, const godot::String& text) const;

    Style style_;
    std::optional<bool> message_;
    bool paused_{};
    bool ready_{};
    std::optional<bool> ended_;
    KitText message_text_;
    KitText banner_text_;
    KitText title_text_;
    godot::Rect2 message_rect_;
    godot::Rect2 banner_rect_;
    godot::Rect2 panel_rect_;
    int message_pixels_{};
    EawrOverlayButton* resume_{};
    std::function<void()> begin_action_;
    EawrOverlayButton* quit_{};
    godot::Control* panel_{};
    std::optional<BuiltDialog> result_dialog_;
    presentation::ui::BattleResults results_;
    std::array<std::size_t, 2> result_scroll_{};
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::vector<godot::RID> pause_items_;
    godot::Ref<godot::CanvasItemMaterial> pause_additive_;
};

} // namespace eawr::presentation::godot_backend
