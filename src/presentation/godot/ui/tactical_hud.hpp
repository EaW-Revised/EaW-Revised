#pragma once

// The tactical space HUD shell in Godot (ticket P2-20a, #83; docs/ui/ui-layer.md
// sections 1.3, 3.3 and 3.4). The engine-free presentation::ui::HudShell says
// what to draw; this draws it on a canvas layer over the 3D view:
//
// - the shell art (faceplate, help droid) as textured canvas triangles with
//   their repeating UVs, MeshAlpha alpha-blended and the radar's MeshAdditive
//   scan lines added, farthest first;
// - the options button as a state-textured button (a stub: it counts presses
//   and opens nothing yet; P2-20e adds the in-game menu);
// - the time panel: help and holocron as inert normal-state art, pause and
//   fast forward as toggle buttons (#459, docs/behaviour/tactical-time-controls.md);
// - the battle overlay (#453, #459): the win/lose message, the pause banner and
//   the end panel (battle_overlay.hpp);
// Button art is drawn at its texture's size around the component's bone
// (HudShell button_quad), as the original does, not stretched to its mesh.
// - the planet name in its component font (UI-F1 size, GDI text cell, outline).
//
// The root is an EawrUiHitMask whose hit test is HudViewModel::hit_test_screen
// (UI-I2), so the pointer stops only on component rects and opaque faceplate
// texels. Placement follows UI-L1/UI-L2 for the viewport's current size, so a
// resize re-lays the HUD out.

#include "ui/ability_buttons_view.hpp"
#include "ui/battle_overlay.hpp"
#include "ui/font_provider.hpp"
#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include "ui/minimap_view.hpp"
#include "ui/production_view.hpp"
#include "ui/unit_cards_view.hpp"

#include "eawr/presentation/ui/hud.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/presentation/ui/unit_cards.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture_button.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace eawr::data {
class Catalog;
}

namespace eawr::presentation::godot_backend {

// The HUD's drawing surface: shell art, then the options button and the planet
// name as children. It covers the viewport and ignores the pointer; the
// EawrUiHitMask above it decides where the HUD stops input.
class EawrTacticalHud final : public godot::Control {
    GDCLASS(EawrTacticalHud, godot::Control)

public:
    struct Mesh final {
        presentation::ui::ShellBlend blend{presentation::ui::ShellBlend::alpha};
        godot::Ref<godot::Texture2D> texture;
        std::vector<data::ui::ShellTriangle> triangles;
    };
    struct Setup final {
        presentation::ui::LayoutRules rules{presentation::ui::LayoutRules::aspect_correct};
        presentation::ui::HudViewModel view;
        std::vector<Mesh> meshes;
        // The options button's quad in shell units (button_quad).
        std::optional<data::ui::ReferenceRect> options_quad;
        // Its interactive rect: the component's mesh extent, the same rect the
        // hit mask (HudViewModel) holds; FoC picks the mesh, not the art.
        std::optional<data::ui::ReferenceRect> options_hit;
        struct Art final {
            godot::Ref<godot::Texture2D> texture;
            data::ui::ReferenceRect quad;
        };
        std::vector<Art> panel;
        // #459: the pause and fast-forward buttons, placed like the options button.
        struct Placed final {
            godot::TextureButton* button{};
            data::ui::ReferenceRect quad;
            data::ui::ReferenceRect hit;
        };
        std::vector<Placed> time_buttons;
        std::optional<presentation::ui::HudShellText> planet;
        godot::String planet_text;
        // The planet name's face (UI-F3) and its GDI text cell at a glyph
        // height; the size follows the layout (UI-F1).
        godot::Ref<godot::Font> planet_font;
        std::function<presentation::ui::TextCell(std::int32_t glyph_height)> planet_cell;
    };

    EawrTacticalHud();
    ~EawrTacticalHud() override;
    // Takes the model; `options` (already carrying its textures) becomes a child.
    void setup(Setup setup, godot::TextureButton* options);
    void _notification(int what);
    void _draw() override;

    [[nodiscard]] presentation::ui::ReferenceSpace space() const;
    [[nodiscard]] presentation::ui::ShellPlacement placement() const;
    [[nodiscard]] bool hit(double x, double y) const;
    [[nodiscard]] godot::TextureButton* options_button() const { return options_; }
    [[nodiscard]] godot::Rect2 planet_rect() const { return planet_rect_; }
    [[nodiscard]] int planet_pixels() const { return planet_pixels_; }
    // Runs `probe` once, on the first frame after the HUD is laid out.
    void set_probe(std::function<void()> probe);

protected:
    static void _bind_methods() {}

private:
    void relayout();

    Setup setup_;
    godot::TextureButton* options_{};
    std::vector<godot::RID> items_;
    godot::RID panel_item_;
    godot::Ref<godot::Material> additive_;
    KitText planet_;
    godot::Rect2 planet_rect_;
    int planet_pixels_{};
    godot::Vector2 laid_out_{-1.0F, -1.0F};
    std::function<void()> probe_;
};

// Owns the HUD: loads the shell, its textures, fonts and the planet name from
// the mounted game (and mod) data and builds the node tree.
class TacticalHud final {
public:
    struct Options final {
        presentation::ui::HudFaction faction{presentation::ui::HudFaction::rebel};
        presentation::ui::LayoutRules rules{presentation::ui::LayoutRules::aspect_correct};
        // The UI-05 font cache (the viewer mounts out/fonts), and how its directory was chosen.
        presentation::ui::FontCache font_cache;
        std::string font_cache_source;
        std::string language{"ENGLISH"};
        // --eawr-hud-probe: once laid out, pushes left clicks at named points
        // through the viewport (GUI first, then the world, UI-I1) and reports
        // where each one went.
        bool probe{};
    };

    explicit TacticalHud(Options options);
    ~TacticalHud();
    TacticalHud(const TacticalHud&) = delete;
    TacticalHud& operator=(const TacticalHud&) = delete;

    // Loads the data and adds the HUD (a CanvasLayer) under `parent`. `objects`
    // and `context_name` (the map's root field 0x09) name the planet; either may
    // be absent. The unit cards (#425) keep reading `objects` (Icon_Name, Text_ID),
    // so a given catalogue must outlive the HUD. A false return means the shell itself could not be read;
    // failure() says why and nothing was added. Missing parts, textures and
    // fonts are diagnosed and drawn without.
    [[nodiscard]] bool build(const vfs::Vfs& filesystem, const data::Catalog* objects,
                             const std::optional<std::string>& context_name, godot::Node& parent);

    [[nodiscard]] const std::string& failure() const noexcept;
    [[nodiscard]] int options_presses() const noexcept;
    [[nodiscard]] EawrTacticalHud* hud() const noexcept;
    // The host saw `event` reach the world (it passed the GUI and the policy).
    void world_input(const godot::Ref<godot::InputEvent>& event);
    // #425: the command bar's unit cards; null before build() or when the shell has no card slots.
    [[nodiscard]] EawrUnitCards* unit_cards() const noexcept;
    // WSU-33, WSU-55: world group text shares the command bar's cached font provider.
    [[nodiscard]] godot::Ref<godot::Font> world_group_font(bool squadron) const;
    // #530: the build queue, the credits and the reinforcement pane; null before build() or when
    // the shell has none of them.
    [[nodiscard]] EawrProductionPanel* production() const noexcept;
    // #530 PU-62: a type's Tactical_Build_Cost_Multiplayer from the object catalog, for a build
    // button whose type the session does not build (price 0 in the menu).
    [[nodiscard]] std::optional<std::int64_t> listed_build_cost(const std::string& type) const;
    // The selection's cards (unit_cards.hpp) to draw; the HUD redraws only when they change.
    void set_unit_cards(const presentation::ui::CardLayout& layout, std::span<const presentation::ui::CardUnit> units);
    // #454 (docs/behaviour/foc-ability-buttons.md): the ability buttons and the cards' ability marks;
    // null before build() or when the shell has no ability buttons.
    [[nodiscard]] EawrAbilityButtons* ability_buttons() const noexcept;
    void set_ability_bar(const presentation::ui::AbilityBar& bar);
    // #455 (docs/behaviour/foc-minimap.md): the command bar's minimap; null before build() or when the
    // shell has no radar mesh. Each frame the live battle hands it what the local player sees; the
    // blips and the camera outline follow every frame, the fog layer a few rows a frame (MM-04).
    [[nodiscard]] EawrMinimap* minimap() const noexcept;
    struct MinimapView final {
        presentation::ui::MinimapExtents extents;
        std::vector<presentation::ui::MinimapUnit> units;
        std::vector<presentation::ui::MinimapHazard> hazards;
        std::optional<std::array<std::array<double, 2>, 4>> ground; // MM-09, source X/Y
        std::vector<presentation::ui::MinimapRevealer> revealers;
        // #494: the local player's fog cells; when set, the fog layer reads them, not `revealers`.
        std::optional<presentation::ui::MinimapFogCells> cells;
        bool fog{true};
    };
    void set_minimap(const MinimapView& view);
    // What a left press or drag (look) and a right click (move) on the minimap do, in source X/Y.
    void set_minimap_handlers(std::function<void(double, double)> look, std::function<void(double, double)> move);
    // MM-07: a faction's colour from Factions.xml, for owners without a lobby colour.
    [[nodiscard]] std::optional<data::ui::Rgba8> faction_colour(std::string_view faction) const;
    // MM-06: a type's minimap looks from its XML (cached).
    [[nodiscard]] const presentation::ui::MinimapTypeLooks& minimap_looks(std::string_view type);
    // A minimap point (x, y from -1 to 1) in viewport pixels while the minimap is shown.
    [[nodiscard]] std::optional<std::array<float, 2>> minimap_point(double x, double y) const;
    // #459: what the time panel's buttons and the pause banner's Resume Game do, and #453's
    // Quit Game. Without handlers the buttons stay inert.
    struct TimeHandlers final {
        std::function<void()> pause;
        std::function<void()> fast_forward;
        std::function<void()> resume;
        std::function<void()> quit;
    };
    void set_time_handlers(TimeHandlers handlers);
    // The panel's state each frame (TM-05, TM-06, TM-08, TM-09).
    struct TimeView final {
        bool paused{};
        bool fast_forward{};
        bool pause_enabled{true};
        bool fast_forward_enabled{true};
    };
    void set_time_view(const TimeView& view);
    // #453: the local player's result (true: victory) once decided, and whether the battle ended.
    void set_battle(std::optional<bool> won, bool ended);
    void set_results(const presentation::ui::BattleResults& results);
    void set_begin(bool ready);
    // #848 (docs/behaviour/foc-battle-selection.md V-5b): while either overview level is on, the
    // tactical shell with everything on it and, while paused, the pause banner hide; the win/lose
    // message and the end panel stay (V-5e).
    void set_overview(bool on);
    // Whether the shell and the pause banner draw this frame.
    [[nodiscard]] bool shell_shown() const;
    [[nodiscard]] bool pause_banner_shown() const;
    // A control's centre in viewport pixels while it is shown: pause, fast_forward, resume, quit.
    [[nodiscard]] std::optional<std::array<float, 2>> control_point(const std::string& name) const;
    // The "hud" object of a viewer report: what was drawn, from where, and the
    // pixel rects at the current viewport size.
    [[nodiscard]] std::string report_json() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Registers the HUD classes with ClassDB (scene initialisation level).
void register_tactical_hud_classes();

} // namespace eawr::presentation::godot_backend
