#pragma once

// #425 (P2-20b, #83): the unit cards of the tactical command bar in Godot. The engine-free
// presentation::ui::layout_unit_cards says which card fills which slot; this draws them in the
// HUD shell's card slots and turns the pointer on a card into a card click
// (docs/behaviour/foc-unit-cards.md):
//
// - the column borders (special_border_NN, the full, left, centre or right piece), then per card the
//   type's portrait (Icon_Name, i_button_temporary.tga without one) at its texture's size around the
//   slot's bone, its health and shield bars, and "x<n>" on a stacked card;
// - a left release on a card is its click (Shift deselects); a double click runs it again and spends
//   the release that follows, as FoC's command bar does. Only the slot meshes take the pointer.
// - after Encyclopedia_Delay on one card, the card's type name shows on the header line of FoC's
//   encyclopedia popup above the help droid (the rest of the popup is on the fidelity list).

#include "ui/kit.hpp"

#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrUnitCards final : public godot::Control {
    GDCLASS(EawrUnitCards, godot::Control)

public:
    struct Setup final {
        std::vector<presentation::ui::HudCardSlot> slots;
        std::vector<presentation::ui::HudShellButton> borders;
        // A texture of the command bar (MT_CommandBar, then files) at texel size; null when missing.
        std::function<godot::Ref<godot::Texture2D>(const std::string& texture)> texture;
        // An object type's Icon_Name and display name (Text_ID through the text DB).
        std::function<std::string(const std::string& type)> icon;
        std::function<godot::String(const std::string& type)> name;
        // The slots' face (UI-F3) and its GDI text cell at a glyph height; the size follows the layout.
        godot::Ref<godot::Font> font;
        std::function<presentation::ui::TextCell(std::int32_t glyph_height)> cell;
        std::int32_t point_size{6};
        // GameConstants Encyclopedia_Delay (750 ms in FoC).
        double hover_delay_seconds{0.75};
        std::function<presentation::ui::ShellPlacement()> placement;
        std::function<presentation::ui::ReferenceSpace()> space;
    };
    struct Card final {
        std::size_t slot{};
        std::string type;
        std::uint32_t count{1};
        bool stacked{};
        std::int32_t health_level{};
        std::optional<double> shield;
    };

    EawrUnitCards();
    ~EawrUnitCards() override;
    void setup(Setup setup);
    // The cards to draw; redraws only when they change.
    void show(std::vector<Card> cards, std::vector<presentation::ui::CardBorder> borders);
    void set_click(std::function<void(std::size_t slot, bool shift)> click) { click_ = std::move(click); }
    // The clock the hover delay runs on, in seconds (default: the engine's ticks). A driven live
    // battle times it on its presented tick, so a capture run repeats.
    void set_clock(std::function<double()> clock) { clock_ = std::move(clock); }

    [[nodiscard]] std::size_t slot_count() const noexcept { return setup_.slots.size(); }
    // A slot's mesh on the screen (its hit rect), or nothing for an unknown slot.
    [[nodiscard]] std::optional<godot::Rect2> slot_rect(std::size_t slot) const;
    [[nodiscard]] std::optional<std::size_t> slot_at(const godot::Vector2& point) const;
    // The "unit_cards" object of the HUD report.
    [[nodiscard]] std::string report_json() const;

    bool _has_point(const godot::Vector2& point) const override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);
    void _draw() override;

protected:
    static void _bind_methods() {}

private:
    [[nodiscard]] godot::Rect2 screen(const data::ui::ReferenceRect& rect) const;
    [[nodiscard]] godot::Ref<godot::Texture2D> portrait(const std::string& type, std::string* name = nullptr) const;
    void draw_bar(const presentation::ui::HudBar& bar, std::int32_t level, double percent);
    void update_text();
    void act(const godot::Vector2& at, bool shift);

    Setup setup_;
    std::vector<Card> cards_;
    std::vector<presentation::ui::CardBorder> borders_;
    std::vector<std::unique_ptr<KitText>> counts_;
    KitText tooltip_;
    std::function<void(std::size_t, bool)> click_;
    bool spend_release_{};
    [[nodiscard]] double now() const;

    std::function<double()> clock_;
    std::optional<std::size_t> hovered_;
    double hovered_since_{};
    bool tooltip_shown_{};
    godot::Rect2 tooltip_rect_;
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t clicks_{};
    std::uint64_t double_clicks_{};
};

} // namespace eawr::presentation::godot_backend
