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
// - after Encyclopedia_Delay, the encyclopedia shows its description, class and matchup icons
//   (P-1..P-4) above the help droid.

#include "ui/font_provider.hpp"
#include "ui/kit.hpp"
#include "ui/portrait_cell.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/vfs/vfs.hpp"

#include "eawr/presentation/ui/fonts.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend {

// Authored popup resources, shared independently of any card or game mode.
struct EncyclopediaPopupStyle {
    godot::Ref<godot::Font> popup_header_font;
    godot::Ref<godot::Font> popup_body_font;
    godot::Ref<godot::Font> popup_cost_font;
    std::int32_t popup_header_points{};
    std::int32_t popup_body_points{};
    godot::Color popup_body_colour;
    std::int32_t popup_cost_points{};
    godot::Color popup_cost_colour;
    data::ui::Vec2 popup_header_size{};
    data::ui::Vec2 popup_icon_scale{};
    float popup_population_offset{};
    float popup_name_offset{};
    float popup_cost_offset{};
    float popup_icon_x_offset{};
    float popup_icon_y_offset{};
    float popup_class_y_offset{};
    // WGUI-166 fleet cells: the encyclopedia_icon caption face and Text_Offset.
    godot::Ref<godot::Font> popup_cell_font;
    std::int32_t popup_cell_points{};
    bool popup_cell_outline{};
    data::ui::Vec2 popup_cell_offset{};
    std::string popup_background;
    std::string popup_frame;
    std::string popup_glow;
    godot::String good_against_text;
    godot::String vulnerable_to_text;
    data::ui::Vec2 tooltip_size{};
    std::int32_t tooltip_point_size{};
    std::function<godot::Ref<godot::Texture2D>(const std::string& texture)> texture;
    std::optional<presentation::ui::EncyclopediaTiming> timing;
};
struct EncyclopediaPopupStatus {
    std::optional<std::int64_t> price;
    std::string disabled_reason;
    bool production{};
};
// Content and status are copied presentation inputs; callers supply placement.
// Unit cards and the standalone Control below share all geometry and drawing.
struct EncyclopediaPopupRenderer final {
    ~EncyclopediaPopupRenderer();
    void initialize(godot::Control& owner);
    void clear();
    void update(const EncyclopediaPopupStyle& setup, const presentation::ui::UnitCardLooks& content,
        const EncyclopediaPopupStatus& status, const godot::Vector2& bottom_left,
        const presentation::ui::ReferenceSpace& space, float scale);
    void draw(godot::Control& owner, const EncyclopediaPopupStyle& setup, float scale, float alpha,
        godot::Vector2 offset = {});
    std::array<KitText, 7> popup_text_;
    godot::Control* popup_text_owner_{};
    godot::RID popup_glow_item_;
    godot::Ref<godot::Material> popup_glow_material_;
    presentation::ui::UnitCardLooks popup_content_;
    std::array<godot::Rect2, 6> popup_icon_rects_;
    std::vector<godot::Rect2> popup_ability_rects_;
    godot::Rect2 popup_portrait_rect_;
    godot::Rect2 popup_title_rect_;
    godot::Rect2 popup_population_rect_;
    std::array<godot::Rect2, 2> popup_matchup_rects_;
    godot::Rect2 popup_name_rect_;
    godot::Rect2 popup_rect_;
    // WGUI-166 fleet cells: icon boxes and their xN captions.
    godot::Rect2 popup_back_rect_;
    std::vector<godot::Rect2> popup_cell_rects_;
    std::vector<std::unique_ptr<KitText>> popup_cell_text_;
};
class EawrEncyclopediaPopup final : public godot::Control {
    GDCLASS(EawrEncyclopediaPopup, godot::Control)
public:
    EawrEncyclopediaPopup();
    void setup(EncyclopediaPopupStyle style);
    void update(const presentation::ui::UnitCardLooks& content, const EncyclopediaPopupStatus& status,
        const godot::Vector2& bottom_left, const presentation::ui::ReferenceSpace& space, float scale,
        std::optional<godot::Rect2> bounds = std::nullopt);
    [[nodiscard]] const presentation::ui::UnitCardLooks& get_content() const { return renderer_.popup_content_; }
    [[nodiscard]] const EncyclopediaPopupStatus& get_status() const { return status_; }
    void _draw() override;
protected:
    static void _bind_methods() {}
private:
    EncyclopediaPopupStyle style_;
    EncyclopediaPopupRenderer renderer_;
    EncyclopediaPopupStatus status_;
    float scale_{1.0F};
};

class EawrUnitCards final : public godot::Control {
    GDCLASS(EawrUnitCards, godot::Control)

public:
    struct Setup final : EncyclopediaPopupStyle {
        // Relative to the host card layer; zero preserves enclosing modal order.
        std::int32_t tooltip_z_index{};
        std::vector<presentation::ui::HudCardSlot> slots;
        std::vector<presentation::ui::HudShellButton> borders;
        // An object type's Icon_Name and display name (Text_ID through the text DB).
        std::function<std::string(const std::string& type)> icon;
        std::function<godot::String(const std::string& type)> name;
        std::function<godot::String(const std::string& type)> description;
        std::function<presentation::ui::UnitCardLooks(const std::string& type)> encyclopedia;
        // The slots' face (UI-F3) and its GDI text cell at a glyph height; the size follows the layout.
        godot::Ref<godot::Font> font;
        std::function<presentation::ui::TextCell(std::int32_t glyph_height)> cell;
        std::int32_t point_size{6};
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
        // #530 (space-purchasing PU-61, PU-62): a build button instead of a unit card: its price as
        // the text, white (alpha 200) while its queue has room, else grey; a disabled button is
        // greyed. No bars.
        std::optional<std::int64_t> price;
        bool room{true};
        bool disabled{};
        std::optional<std::uint32_t> build_frames{};
        std::string disabled_reason{};
        bool pad{};
        double cooldown_progress{1.0};
    };

    EawrUnitCards();
    ~EawrUnitCards() override;
    void setup(Setup setup);
    // The cards to draw; redraws only when they change.
    void show(std::vector<Card> cards, std::vector<presentation::ui::CardBorder> borders);
    // Compare against retained display values before allocating card strings or vectors.
    void show_units(const presentation::ui::CardLayout& layout, std::span<const presentation::ui::CardUnit> units);
    // A retained menu can change price/tint/dial without reconstructing card names.
    bool update_build_states(std::span<const presentation::ui::BuildButton> buttons);
    void set_click(std::function<void(std::size_t slot, bool shift)> click) { click_ = std::move(click); }
    // The clock the hover delay runs on, in seconds (default: the engine's ticks). A driven live
    // battle times it on its presented tick, so a capture run repeats.
    void set_clock(std::function<double()> clock) { clock_ = std::move(clock); }
    // A generic, already observer-filtered hover source. Empty content clears
    // immediately so a hidden/dead object cannot retain an encyclopedia.
    void set_external_hover(std::uint64_t identity, std::optional<presentation::ui::UnitCardLooks> content);

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
    void redraw();
    void draw_tooltip();
    void draw_bar(const presentation::ui::HudBar& bar, std::int32_t level, double percent);
    void update_text();
    void update_popup(const Card& card);
    void act(const godot::Vector2& at, bool shift);

    Setup setup_;
    std::vector<Card> cards_;
    std::uint64_t build_card_batches_{};
    std::uint64_t unit_card_syncs_{};
    std::uint64_t unit_card_batches_{};
    std::uint64_t build_state_updates_{};
    std::vector<presentation::ui::CardBorder> borders_;
    std::vector<std::unique_ptr<PortraitCellRenderer>> cells_;
    godot::Control* tooltip_layer_{};
    KitText tooltip_;
    EncyclopediaPopupRenderer popup_;
    std::uint64_t popup_content_builds_{};
    std::function<void(std::size_t, bool)> click_;
    bool spend_release_{};
    [[nodiscard]] double now() const;

    std::function<double()> clock_;
    std::optional<std::size_t> hovered_;
    double hovered_since_{};
    bool tooltip_shown_{};
    presentation::ui::EncyclopediaTiming timing_;
    double serviced_at_{};
    std::uint64_t external_identity_{};
    std::optional<presentation::ui::UnitCardLooks> external_content_;
    std::optional<Card> shown_card_;
    bool shown_external_{};
    std::optional<presentation::ui::UnitCardLooks> shown_external_content_;
    godot::Rect2 tooltip_rect_;
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t clicks_{};
    std::uint64_t double_clicks_{};
};

// The encyclopedia popup's look from the authored encyclopedia components and
// GameConstants (CARD-19), shared by every host of the popup. Fonts resolve in
// `language` through `fonts`; matchup captions come from `text` when given.
void fill_encyclopedia_popup(EncyclopediaPopupStyle& setup, const data::ui::CommandBarCatalog& catalog,
    const vfs::Vfs& filesystem, FontProvider& fonts, std::string_view language,
    const data::ui::TextDatabase* text, std::vector<core::Diagnostic>& diagnostics);

} // namespace eawr::presentation::godot_backend
