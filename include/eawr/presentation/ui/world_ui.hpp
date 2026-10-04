#pragma once

#include "eawr/sim/commands.hpp"
#include "eawr/presentation/ui/ability_buttons.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

// #424 (P2-19/P2-20): FoC's battle UI drawn in the world over the units: the selection circle under
// a selected own unit, the shield and health bars over a unit, the squadron icon, and the hardpoint
// reticles over a hovered ship (docs/behaviour/foc-battle-world-ui.md). Every rule and number here
// was read in the FoC debug build (selection, command bar, bar, reticle and team code) or in the
// FoC data (GameConstants.xml, Factions.xml,
// CommandBarComponents.xml, the MT_CommandBar atlas); what the remake decides is marked project
// policy. Presentation only: nothing here enters a command, the replay or a hash.
//
// Sizes are in pixels of the 1024 x 768 UI reference (layout.hpp); the host multiplies them by
// its UI scale (screen height / 768 at 4:3 and wider).
namespace eawr::presentation::ui {

using Rgb = std::array<std::uint8_t, 3>;

// WU-43..WU-46: the world uses the cards' state, but only an active ability shows here.
// Squadron icons take the first active slot; a bracket can show both. No heap storage.
struct WorldOverlayUnit final {
    sim::EntityId entity{};
    std::array<std::uint32_t, 2> abilities{};
    bool ally{};
    bool visible{};
    bool on_screen{};
    bool squadron{};
    bool bracket{};
};
[[nodiscard]] std::array<std::uint32_t, 2> world_ability_overlays(
    const WorldOverlayUnit& unit, const AbilityState& state);
// WU-44: icon centre above the bracket, with a three-reference-pixel gap from the bar.
[[nodiscard]] float bracket_ability_y(float bracket_y, float bar_height_pixels,
                                      float icon_height_pixels, float ui_scale) noexcept;
// WU-43, WU-44: a separate component effect quad, native size at its own scale.
// Authored offsets are reference pixels with +Y up; screen coordinates have +Y down.
struct WorldAbilityRect final {
    float x{};
    float y{};
    float width{};
    float height{};
};
[[nodiscard]] WorldAbilityRect world_ability_rect(std::array<float, 2> centre,
    std::array<float, 2> native_size, std::array<float, 2> effect_offset,
    float component_scale, float ui_scale) noexcept;

// --- Selection circle (WU-01 to WU-03) -----------------------------------------------------------

// WU-02: the circle is a square blob lying flat at the unit's position, its side
// Select_Box_Scale x Scale_Factor world units; none when Select_Box_Scale is not positive.
[[nodiscard]] std::optional<float> selection_circle_side(float select_box_scale, float scale_factor) noexcept;

// --- Shield and health bars (WU-10 to WU-19) -----------------------------------------------------

// WU-11: the bar set a unit gets. GUI_Bracket_Size (0, 1, 2) wins;
// without it a fighter or bomber is small, a frigate or capital ship large, anything else medium.
enum class BarSize : std::uint8_t { small, medium, large };
[[nodiscard]] BarSize bar_size(std::string_view ship_class, std::optional<int> gui_bracket_size) noexcept;
// WU-12: the bar's width and height in reference pixels (I_BAR_CONTROL_WIDE*, I_BAR_HEALTH_*).
[[nodiscard]] float bar_width(BarSize size) noexcept;
inline constexpr float bar_height = 2.0F;

// GameConstants.xml: Health_Bar_Scale, Min_Health_Bar_Scale, Health_Bar_Spacing,
// Team_Healthbar_Offset.
inline constexpr float health_bar_scale = 1500.0F;
inline constexpr float min_health_bar_scale = 1.0F;
inline constexpr float health_bar_spacing = 2.0F;
inline constexpr float team_healthbar_offset = 20.0F;

// WU-13: bars grow as the camera closes in: Health_Bar_Scale over the camera's distance to the
// unit, never below Min_Health_Bar_Scale.
[[nodiscard]] float bar_scale(float camera_distance) noexcept;

// WU-14: a bar shows level ceil(10 x fraction) of 0..10; its filled part is fraction x width from
// the left, over a black back, with a one-pixel black outline.
[[nodiscard]] int bar_level(float fraction) noexcept;
// WU-15: the health bar's colour per level (I_BAR_HEALTH_<size><level>): black, red at 1 to 3,
// orange at 4 and 5, yellow at 6 and 7, yellow-green at 8 and 9, green at 10.
[[nodiscard]] Rgb health_bar_colour(int level) noexcept;
// WU-15: the shield bar is always I_BAR_SHIELD_<size>10.
inline constexpr Rgb shield_bar_colour{0, 190, 255};

// WU-16, WU-17: which bars a unit shows this frame.
struct BarUnit {
    bool selected{};          // in the local player's selection (a squadron craft never is)
    bool hovered{};           // the unit under the pointer
    bool squadron_member{};   // a craft of a squadron
    bool fogged{};            // the local player does not see it
    bool has_health{};        // a positive maximum hull
    float health{};           // the displayed hull fraction
    bool shielded{};          // SHIELDED behaviour
    bool hide_health_bar{};   // GUI_Hide_Health_Bar
    bool neutral{};           // WSU-63: neutral props have no health or shield bars
    bool admitted{true};       // WSU-50: selectable/hero/special admission before bar rules
};
struct BarVisibility {
    bool health{};
    bool shield{};
};
// WU-16 (#502): FoC considers only the unit under the pointer and the local player's selection;
// hovering a squadron's icon shows only the icon's own bar (WU-22), never its craft's. Any other
// unit gets no bars, whatever its health (the critical-health rule applies within these).
[[nodiscard]] bool bar_candidate(bool hovered, bool selected) noexcept;
[[nodiscard]] BarVisibility bar_visibility(const BarUnit& unit) noexcept;

// WU-18: where the bars sit, in the view: the unit's world-space bounds centre moved along the
// camera's up axis by the bounds' largest extent along it (GUI_Bounds_Scale 1), or by the length
// of the bounds' half extent (any other GUI_Bounds_Scale), times GUI_Bounds_Scale. The shield bar
// is drawn there; the health bar below it by the shield bar's height x Health_Bar_Spacing.
[[nodiscard]] float bar_anchor_lift(const std::array<float, 3>& half_extent, const std::array<float, 3>& camera_up,
                                    float gui_bounds_scale) noexcept;

// --- Squadron icon (WU-20 to WU-24) --------------------------------------------------------------

// WU-21: the icon's frame (I_BUTTON_UNIT_FRAME_GRIPPER at Scale 0.6) and its health bar (the small
// bar at Scale 0.8, 16 reference pixels below the frame's centre).
inline constexpr float squadron_frame_side = 60.0F * 0.6F;
inline constexpr float squadron_bar_scale = 0.8F;
inline constexpr float squadron_bar_offset = 16.0F;
// WU-37 to WU-39 (#632): a hangar-launched squadron's icon carries the flag (I_GARRISON_FLAG, 12 x 14
// pixels at scale 1.0): its centre 15 reference pixels right of and 15 below the frame's centre.
inline constexpr float garrison_flag_offset_x = 15.0F;
inline constexpr float garrison_flag_offset_y = 15.0F;
inline constexpr float garrison_flag_width = 12.0F;
inline constexpr float garrison_flag_height = 14.0F;
// WU-24 (#500): FoC's gripper placement always asks for this screen offset when it places a
// team's icon (debug build): after projecting the world point to screen space, FoC adds this
// share of the screen height to Y (+Y down), so the icon hovers below the squadron's projected
// centre instead of covering it. A raw pixel offset (a share of the screen height, not a
// reference-pixel size the UI scale would grow at a close camera). The dogfight grid (WU-25 to
// WU-27) reads its icon's world point through the same gripper placement (WU-26), so the offset
// applies there too, on top of the grid's own within-cell layout.
inline constexpr float squadron_icon_screen_offset_fraction = 0.048F;

// WU-49: an arriving squadron's icon waits at its landing point; the craft fly to it.
struct SquadronIconAnchor final {
    std::array<float, 3> position{};
    float speed{};
    bool arriving{};
};
// Returns true while arrival or its first ordinary frame owns the anchor.
[[nodiscard]] bool place_squadron_arrival_icon(SquadronIconAnchor& anchor,
    std::optional<std::array<float, 3>> landing, std::array<float, 3> presented) noexcept;

// WU-47: named heroes and explicit heads share the world identity frame.
[[nodiscard]] constexpr bool hero_world_identity(const bool named_hero, const bool show_hero_head) noexcept {
    return named_hero || show_hero_head;
}
// WU-24: the pixel offset itself, for a `screen_height` pixel viewport.
[[nodiscard]] float squadron_icon_screen_offset(float screen_height) noexcept;
// WU-22: the icon's health level is ceil(10 x the squadron's health).
[[nodiscard]] float squadron_health(float health_sum, float max_health_sum) noexcept;

// --- Dogfight grid (WU-25 to WU-27) ---------------------------------------------------------------

// WU-25: the combat grid's cells are 400 world units square, every odd row shifted by half a
// cell.
inline constexpr float combat_cell_size = 400.0F;
// WU-26: the icons of one cell are 30 reference pixels apart.
inline constexpr float combat_grid_step = 30.0F;

struct CombatCell {
    int x{};
    int y{};
    friend constexpr bool operator==(const CombatCell&, const CombatCell&) noexcept = default;
};
// WU-25: the cell of a point, `origin` the grid's low corner.
[[nodiscard]] CombatCell combat_cell_of(std::array<float, 2> point, std::array<float, 2> origin) noexcept;
// WU-25: the cell's point (its centre, odd rows half a cell further along x).
[[nodiscard]] std::array<float, 2> combat_cell_point(CombatCell cell, std::array<float, 2> origin) noexcept;

// WU-26: where icon `index` of the `count` icons of one cell sits,
// in reference pixels from the cell point's screen position (+y down): ceil(sqrt(count)) columns
// 30 apart from half the first row's width left of the point, rows 30 apart downward.
[[nodiscard]] std::array<float, 2> combat_grid_slot(std::size_t index, std::size_t count) noexcept;

// WU-25: squadrons dogfighting other squadrons hold combat cells. Every frame, in service order,
// a squadron whose target is a squadron and whose leader is inside its strafe reach (or which
// already records its target's cell) either joins the cell its target records, or, when the
// target records none, records the WU-25a search's cell without joining it. A squadron that
// closes from beyond the reach, or has no squadron target, leaves its cell and forgets it. Only
// joined squadrons are drawn (WU-26). Presentation state only.
class CombatGrid final {
public:
    struct Fight final {
        sim::EntityId squadron{};
        sim::EntityId target_squadron{};  // the target's squadron; invalid when the target is a ship
        std::array<float, 2> target{};     // the target squadron's position in the plane
        bool closing{};                    // FA-01: its leader is beyond the strafe reach
    };
    struct Occupied final {
        CombatCell cell;
        std::vector<sim::EntityId> squadrons;  // in the order they joined this frame
    };
    // `fights` in ascending squadron ID (the remake's service order, project).
    void update(std::span<const Fight> fights, std::array<float, 2> origin);
    // #457: the cells the sim's squadrons record (space-fighters FD-01 to FD-03), in ascending
    // squadron ID; the joined ones are drawn in their cell in that order.
    struct Record final {
        sim::EntityId squadron{};
        CombatCell cell;
        bool joined{};
    };
    void adopt(std::span<const Record> records);
    [[nodiscard]] const std::vector<Occupied>& cells() const noexcept { return cells_; }
    // The cell a squadron joined (drawn in), if any.
    [[nodiscard]] std::optional<CombatCell> cell_of(sim::EntityId squadron) const noexcept;
    // The cell a squadron records, joined or not.
    [[nodiscard]] std::optional<CombatCell> record_of(sim::EntityId squadron) const noexcept;
    // WU-25a: the cell a dogfight around `target` settles on: of the nine cells around the target's
    // own cell, row by row from the low corner, a cell some squadron has joined scores 0 and any
    // other the squared distance from its point to the target; the first strictly lowest wins.
    [[nodiscard]] CombatCell search(std::array<float, 2> target, std::array<float, 2> origin) const noexcept;

private:
    void lift(sim::EntityId squadron);
    std::vector<Occupied> cells_;
    std::vector<std::pair<sim::EntityId, CombatCell>> records_;  // ascending squadron ID
};

// --- Hardpoint reticles (WU-30 to WU-36) ---------------------------------------------------------

// GameConstants.xml HardPoint_Target_Reticle_{Enemy,Friendly}_Screen_Size: the reticle's width as a
// share of the screen width; its height is 4/3 of that share of the screen height.
inline constexpr float hardpoint_reticle_screen_size = 0.03F;

// A reticle's place on the screen in pixels: its top-left corner and its size.
struct ReticleRect {
    float x{};
    float y{};
    float width{};
    float height{};
};

// WU-31: the reticle over a hardpoint whose screen point is `centre` (pixels) on a screen of
// `viewport` pixels. Its size is a share of the screen alone, so it keeps its pixel size at every
// camera distance and for every ship.
[[nodiscard]] ReticleRect hardpoint_reticle_rect(std::array<float, 2> centre, std::array<float, 2> viewport) noexcept;

// WU-34: the world point a reticle is centred on: the hardpoint's attachment point `attachment`
// (the unit frame: scaled, turned by the model's quarter turn, +X forward) placed by the unit's
// drawn pose, Rz(yaw) Ry(pitch) Rx(roll) about `position` (R-ROT-01), the same pose the model is
// drawn with.
[[nodiscard]] std::array<float, 3> hardpoint_reticle_anchor(const std::array<float, 3>& position,
                                                            float yaw_degrees, float pitch_degrees, float roll_degrees,
                                                            const std::array<float, 3>& attachment) noexcept;

// WU-33: grey when disabled, else
// green from 66 % health, yellow from 33 %, red below.
[[nodiscard]] Rgb hardpoint_reticle_tint(float health_fraction, bool disabled) noexcept;

// WU-32: the reticle art of a hardpoint type (HardPoint_Target_Reticle_*_Texture): the texture name
// without extension, its "_tracked" variant for the hardpoint under the pointer. Empty for a type
// with no reticle.
[[nodiscard]] std::string_view hardpoint_reticle_texture(std::string_view hardpoint_type) noexcept;

} // namespace eawr::presentation::ui
