#pragma once

// #454 (P2 gap 3, #83): the command bar's ability buttons and the ability marks on the unit cards in
// Godot. The engine-free presentation::ui::ability_bar says what to draw
// (docs/behaviour/foc-ability-buttons.md); this draws it over the shell and the cards and turns the
// pointer on a button into a click:
//
// - AB-07: per button its base, the ability icon, the mouse-over art, the fading press flash, the
//   disabled art and the autofire outline frames, each at its texture's size around the button's
//   bone (14 shell units left when the group has two buttons), then the recharge dial additively;
// - AB-08: on a card the ability icon at 0.75 scale, its dial and the autofire box;
// - AB-09: a left or right release over a shown button's mesh is its click. Only those meshes take
//   the pointer.

#include "eawr/presentation/ui/ability_buttons.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrAbilityButtons final : public godot::Control {
    GDCLASS(EawrAbilityButtons, godot::Control)

public:
    struct Setup final {
        std::vector<presentation::ui::HudAbilityButton> buttons;
        std::vector<presentation::ui::HudCardSlot> slots;
        std::function<godot::Ref<godot::Texture2D>(const std::string& texture)> texture;
        std::function<presentation::ui::ShellPlacement()> placement;
    };

    EawrAbilityButtons();
    ~EawrAbilityButtons() override;
    void setup(Setup setup);
    void show(presentation::ui::AbilityBar bar);
    // A release on the shown button at `index` of the bar (right: the right button).
    void set_click(std::function<void(std::size_t index, bool right)> click) { click_ = std::move(click); }
    // The clock the press flash and the autofire frames run on, in seconds (default: the engine's).
    void set_clock(std::function<double()> clock) { clock_ = std::move(clock); }

    [[nodiscard]] const presentation::ui::AbilityBar& bar() const noexcept { return bar_; }
    // A shown button's mesh on the screen, by its index in the bar.
    [[nodiscard]] std::optional<godot::Rect2> button_rect(std::size_t index) const;
    [[nodiscard]] std::optional<std::size_t> button_at(const godot::Vector2& point) const;
    // The "ability_buttons" object of the HUD report.
    [[nodiscard]] std::string report_json() const;

    bool _has_point(const godot::Vector2& point) const override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);
    void _draw() override;

protected:
    static void _bind_methods() {}

private:
    [[nodiscard]] godot::Rect2 screen(const data::ui::ReferenceRect& rect) const;
    [[nodiscard]] double now() const;
    [[nodiscard]] const presentation::ui::HudAbilityButton* look(const presentation::ui::AbilityButton& button) const;
    // A quad of the texture's size times `scale` centred on `centre` (shell units), on the screen.
    void draw_centred(const godot::Ref<godot::Texture2D>& texture, assets::Vec2f centre, float scale,
                      const godot::Color& modulate = godot::Color(1, 1, 1, 1));
    // AB-05: the texture cut to the clock sweep from `completion` to the full turn, additively.
    void draw_dial(const godot::Ref<godot::Texture2D>& texture, assets::Vec2f centre, float scale, double completion);

    Setup setup_;
    presentation::ui::AbilityBar bar_;
    std::function<void(std::size_t, bool)> click_;
    std::function<double()> clock_;
    godot::RID dial_item_;
    godot::Ref<godot::Material> additive_;
    std::optional<std::size_t> hovered_;
    std::optional<std::size_t> pressed_;
    bool pressed_right_{};
    std::optional<std::size_t> flash_;
    double flash_start_{};
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t clicks_{};
    std::uint64_t right_clicks_{};
    std::size_t textures_missing_{};
    std::size_t dials_drawn_{};
    std::size_t autofire_frame_{};
};

} // namespace eawr::presentation::godot_backend
