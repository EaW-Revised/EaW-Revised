#pragma once

// #944 (WHE-44..46, HCB-44): the battle hero tray in Godot. The engine-free presentation::ui::collect_heroes
// says which heroes each side shows; this draws them in the two hero frame shells (`good_hero_frame`,
// `evil_hero_frame`): the frame art of the variant for the hero count, per hero the slot's ring, the
// hero's portrait (Icon_Name) at its texture's size around the slot's bone and its health bar
// (`hero_hb_*`). The local team's tray is at the top right, the hostile team's at the top left (retail
// capture 10-20, U-12; the owner's recollection had them the other way round).
//
// The tray is one control for space and land battles: it knows heroes and icons, nothing about the
// battle domain. Only an icon's rect takes the pointer; this control draws and reports hits, and what a
// click does is routed by the caller (PR 2).

#include "ui/kit.hpp"

#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture2d.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend {

class EawrHeroTray final : public godot::Control {
    GDCLASS(EawrHeroTray, godot::Control)

public:
    enum class Tray : std::uint8_t { friendly, hostile };

    // Where the first icon (slot 0, the newest hero) sits: its centre's distance from the screen edge of its
    // side and from the top, in reference units. WHE-70 measured about 44 px and 28 px at 1280x720, which is
    // 46.9 and 29.9 units (the layout scale there is 720/768); only that resolution was captured.
    struct Placement final {
        float edge_inset{46.9F};
        float top_inset{29.9F};
    };
    struct Setup final {
        // The frame shells by side; `friendly_side` is the one the local team's tray uses.
        std::array<presentation::ui::HudHeroFrame, 2> frames;
        presentation::ui::HeroSide friendly_side{presentation::ui::HeroSide::good};
        Placement placement;
        // A command bar texture by name (the atlas, then the texture files); null when missing.
        std::function<godot::Ref<godot::Texture2D>(const std::string& name)> texture;
        // An object type's Icon_Name (empty without one).
        std::function<std::string(const std::string& type)> icon;
        std::function<presentation::ui::ReferenceSpace()> space;
    };
    // One shown hero (the model's HeroEntry resolved for drawing).
    struct Icon final {
        std::string type;                 // the hero's object type name (the portrait)
        double health{1.0};               // 0..1 as given; show() derives the bar level from it
        std::int32_t health_level{};      // 0..the bar's max level (set by show())
        bool automatic{};                 // WHE-46: the highlight
        std::uint64_t hero{};             // the hero identity
        std::uint64_t represented{};      // the object a click acts on
        // The drawn state: the raw health is not compared, only the bar level it rounds up to, so a
        // sub-level health change costs no redraw.
        friend bool operator==(const Icon& a, const Icon& b) noexcept {
            return a.type == b.type && a.health_level == b.health_level && a.automatic == b.automatic
                && a.hero == b.hero && a.represented == b.represented;
        }
    };

    EawrHeroTray();
    ~EawrHeroTray() override;
    void setup(Setup setup);
    // The icons to draw, in slot order; redraws only when they change.
    void show(std::vector<Icon> friendly, std::vector<Icon> hostile);

    [[nodiscard]] const std::vector<Icon>& icons(Tray tray) const noexcept {
        return icons_[static_cast<std::size_t>(tray)];
    }
    // An occupied slot's rect on the screen (its hit rect), or nothing.
    [[nodiscard]] std::optional<godot::Rect2> slot_rect(Tray tray, std::size_t slot) const;
    [[nodiscard]] std::optional<std::pair<Tray, std::size_t>> slot_at(const godot::Vector2& point) const;
    // The "hero_tray" object of the HUD report.
    [[nodiscard]] std::string report_json() const;

    bool _has_point(const godot::Vector2& point) const override;
    void _gui_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);
    void _draw() override;

protected:
    static void _bind_methods() {}

private:
    [[nodiscard]] const presentation::ui::HudHeroFrame& frame(Tray tray) const noexcept;
    void set_levels(Tray tray, std::vector<Icon>& icons) const;
    [[nodiscard]] presentation::ui::ShellPlacement frame_placement(Tray tray) const;
    // The frame art variant for a hero count (the icons of the first row, clamped to the art).
    [[nodiscard]] std::size_t variant(Tray tray, std::size_t count) const noexcept;
    void draw_frame(Tray tray);
    void draw_bar(const presentation::ui::HudBar& bar, const presentation::ui::ShellPlacement& placement,
                  std::int32_t level, double percent);

    Setup setup_;
    std::array<std::vector<Icon>, 2> icons_;
    std::optional<std::pair<Tray, std::size_t>> hovered_;
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::uint64_t shows_{};
    std::uint64_t changes_{};
    std::uint64_t draws_{};
};

} // namespace eawr::presentation::godot_backend
