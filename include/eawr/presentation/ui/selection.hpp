#pragma once

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <map>
#include <span>
#include <vector>

// #82 P2-19: the local player's battle selection (docs/behaviour/foc-battle-selection.md). Picking,
// click, box, type and control-group selection follow the FoC debug build's GamePlayUI and Player
// rules; where the remake stands in for an engine query (a model's collision mesh, the render scene's
// rectangle query) the note says so. Presentation only: nothing here enters a command, the replay or a
// hash. Orders leave through OrderInput (command_sink.hpp), which this selection feeds.
//
// Geometry is in the source basis (+Z up, the battle plane is Z = 0) and screen positions are
// viewport pixels with +Y down, as the host's pointer reports them.
namespace eawr::presentation::ui {

// GameConstants.xml (FoC): MinimumDragSelectDistance and MinimumDragDistance, in cursor pixels. A
// left drag whose larger side exceeds the select distance box-selects on release; anything smaller
// is a click. The box is drawn once the drag passes the select distance, or at once while nothing
// is selected.
inline constexpr float minimum_drag_select_distance = 100.0F;
inline constexpr float minimum_drag_distance = 4.0F;
// Player.cpp NUM_CONTROL_GROUPS; keys 1..9 and 0 (group 0).
inline constexpr std::size_t control_group_count = 10;
// A control group selected again within one second (Logical_FPS frames) moves the camera to it.
inline constexpr double control_group_double_tap_seconds = 1.0;

using Vec3f = std::array<float, 3>;

struct PickRay {
    Vec3f origin{};
    Vec3f direction{};
};

// A unit's pick volume: a model-space box and the model-to-world transform (row-major 3x4,
// rotation then translation). The remake's stand-in for FoC's per-model collision test.
struct UnitBox {
    std::array<float, 12> model_to_world{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F};
    Vec3f low{};
    Vec3f high{};
};

// #665 (WSU-11): a type's collidable triangles in the model space of its UnitBox frame, with
// their bounds, which a ray must enter before the triangles are tested.
struct PickMesh {
    std::vector<std::array<Vec3f, 3>> triangles;
    Vec3f low{};
    Vec3f high{};
};
// The mesh with its bounds filled in; empty triangles give an empty mesh.
[[nodiscard]] PickMesh make_pick_mesh(std::vector<std::array<Vec3f, 3>> triangles);

// One unit the local player sees this frame. A squadron is one unit (#424, FoC's team container):
// each of its craft is a pick volume whose `entity` and `type` are the squadron's container and
// squadron type, and whose `part` is the craft, so the squadron selects and orders as one unit
// while the pointer can still point at one craft.
struct BattleUnit {
    sim::EntityId entity{sim::invalid_entity_id};
    sim::tactical::TypeId type{};
    sim::EntityId part{sim::invalid_entity_id}; // the craft of a squadron; invalid for other units
    sim::tactical::TypeId part_type{};          // that craft's own type (WSU-38); `type` is the squadron's
    bool own{};     // owned by the local player: the only selectable units
    bool hostile{}; // ordinary combat relationship: a right click attacks it
    bool selectable{true};
    bool locomotion{true}; // WSU-21: behaviour presence, independent of speed
    bool decoration{};
    UnitBox box{};
    // #665 (WSU-10 to WSU-12): the pick volume. The type's collision mesh in the box's frame
    // (null or empty: the box stands in for it, project policy), and its
    // Mouse_Collide_Override_Sphere_Radius (0: none), a sphere around `position` that the ray hits
    // when it misses the mesh.
    const PickMesh* mesh{};
    float sphere_radius{};
    Vec3f position{};
    // WSU-15: the projected model origin in viewport pixels; empty behind the camera.
    std::optional<std::array<float, 2>> screen{};
    bool neutral{}; // WSU-63: no selection or combat overlays for neutral scenery
    bool mouse_sensitive{true}; // WSU-13: admission before testing the pick geometry
};

struct ScreenRect {
    float min_x{};
    float min_y{};
    float max_x{};
    float max_y{};
    [[nodiscard]] bool contains(const std::array<float, 2>& point) const noexcept {
        return point[0] >= min_x && point[0] <= max_x && point[1] >= min_y && point[1] <= max_y;
    }
    [[nodiscard]] bool contains_origin(const std::array<float, 2>& point) const noexcept {
        return point[0] >= min_x && point[0] < max_x && point[1] >= min_y && point[1] < max_y;
    }
    [[nodiscard]] bool contains_quad(const ScreenRect& quad) const noexcept {
        return quad.min_x >= min_x && quad.min_y >= min_y && quad.max_x <= max_x && quad.max_y <= max_y;
    }
};

struct SquadronIcon {
    sim::EntityId entity{sim::invalid_entity_id};
    bool own{};
    bool selectable{true};
    ScreenRect rect{};
};

// The rectangle a drag from `start` to `end` spans, and its larger side (FoC measures drags by it).
[[nodiscard]] ScreenRect drag_rect(std::array<float, 2> start, std::array<float, 2> end) noexcept;
[[nodiscard]] float drag_extent(std::array<float, 2> start, std::array<float, 2> end) noexcept;

// Where the ray enters the box (world coordinates), or nothing when it misses or the box is empty.
[[nodiscard]] std::optional<Vec3f> ray_box_contact(const PickRay& ray, const UnitBox& box) noexcept;
// Where the ray first meets the unit's pick volume (world coordinates), WSU-10: its collision mesh
// (or its box without one), or else its override sphere.
[[nodiscard]] std::optional<Vec3f> pick_contact(const PickRay& ray, const BattleUnit& unit) noexcept;

// The unit under the ray (WSU-10): FoC keeps the hit with the highest contact Z. Ties keep the
// lower ID (then the lower craft of a squadron), project policy for FoC's first collected.
[[nodiscard]] std::optional<sim::EntityId> pick_unit(const PickRay& ray, std::span<const BattleUnit> units) noexcept;
// The same pick as an index into `units`, which also tells the craft of a squadron (#424 hover).
[[nodiscard]] std::optional<std::size_t> pick_index(const PickRay& ray, std::span<const BattleUnit> units) noexcept;

// Where the ray meets the battle plane Z = 0 (FoC's order point in space); nothing when parallel.
[[nodiscard]] std::optional<Vec3f> battle_plane_point(const PickRay& ray) noexcept;

// A right-button release (O-1, O-2). Only a release whose press was seen is a gesture: `press` is
// empty after the host cancelled it (focus loss, pointer exit) and for a release that arrives alone,
// and such a release does nothing. A press-to-release extent past MinimumDragSelectDistance is the
// compass; anything smaller is a click.
enum class RightRelease : std::uint8_t { ignored, compass, click };
[[nodiscard]] RightRelease right_release(std::optional<std::array<float, 2>> press,
                                         std::array<float, 2> release) noexcept;

// What a right click with a selection does (O-1, O-3), given the armed mode and the unit under the
// pointer (`over`, null on empty space). Attack mode attacks an enemy and is disarmed by any other
// right click; move mode always moves; with no mode a selected own unit or an ally takes no order.
enum class RightClick : std::uint8_t { nothing, disarm, order };
[[nodiscard]] RightClick right_click(OrderMode mode, const BattleUnit* over, bool over_selected) noexcept;

struct Modifiers {
    bool shift{};
    bool ctrl{};
    bool alt{};
};

class Selection final {
public:
    [[nodiscard]] const std::vector<sim::EntityId>& units() const noexcept { return selected_; }
    [[nodiscard]] bool contains(sim::EntityId entity) const noexcept;
    [[nodiscard]] bool empty() const noexcept { return selected_.empty(); }
    void clear() noexcept { selected_.clear(); }

    // A left click (no drag) on `picked`, or on nothing. An own unit: Shift toggles it, Ctrl adds
    // every own unit of its type on screen, otherwise it becomes the selection. Another player's
    // unit leaves the selection as it is; empty space clears it. Returns whether it changed.
    bool click(std::optional<sim::EntityId> picked, Modifiers modifiers, std::span<const BattleUnit> units,
               const ScreenRect& viewport);
    // A left double click on `picked`: adds every own unit of its type on screen; on a squadron's
    // craft, every own squadron with a craft of that craft type on screen (WSU-18).
    bool double_click(std::optional<sim::EntityId> picked, std::span<const BattleUnit> units,
                      const ScreenRect& viewport);
    // WSU-19, WSU-21: add own icon quads first; the first eligible model replaces that
    // selection without Shift. A structure-only box preserves it, like an empty box.
    bool box(const ScreenRect& rect, bool shift, std::span<const BattleUnit> units,
             std::span<const SquadronIcon> icons = {});
    // Adds the own units of `type` on screen (Select_All_Objects_Of_Type_On_Screen).
    bool type_on_screen(sim::tactical::TypeId type, std::span<const BattleUnit> units, const ScreenRect& viewport);
    // WSU-38 (#550): the same test for a craft type: adds every own squadron with a craft of
    // `craft_type` on screen, whether or not its icon is. Returns whether it added any.
    bool craft_type_on_screen(sim::tactical::TypeId craft_type, std::span<const BattleUnit> units, const ScreenRect& viewport);
    // Drops units that no longer stand in the session (destroyed); selection and groups.
    void retain(std::span<const sim::EntityId> alive);
    // #425: the selection becomes `units` (a unit card click, unit_cards.hpp), in that order and
    // without repeats. Returns whether it changed.
    bool replace(std::span<const sim::EntityId> units);
    // WPR-52: transfer membership even when the replaced unit is not selected.
    void replace_entity(sim::EntityId previous, sim::EntityId replacement);

    // Control groups. Ctrl+n: the selection becomes group n. Alt+n: the selection joins group n,
    // which is then selected. A unit belongs to one group at most.
    void assign_group(std::size_t group);
    // Shift+n adds group n to the selection; n alone replaces the selection with it. The same group
    // selected again within control_group_double_tap_seconds returns the point to focus the camera
    // on: the members' mean position. `alive` are the units standing now.
    std::optional<Vec3f> recall_group(std::size_t group, bool add, double now_seconds,
                                      std::span<const BattleUnit> alive);
    std::optional<Vec3f> add_to_group(std::size_t group, double now_seconds, std::span<const BattleUnit> alive);
    [[nodiscard]] const std::vector<sim::EntityId>& group(std::size_t index) const { return groups_.at(index); }
    // WSU-33, WSU-55: maintained on group edits, so drawing never scans group members.
    [[nodiscard]] std::optional<std::size_t> group_of(sim::EntityId entity) const noexcept;

private:
    void add(sim::EntityId entity);
    std::optional<Vec3f> selected_group(std::size_t group, double now_seconds, std::span<const BattleUnit> alive);
    std::vector<sim::EntityId> selected_;
    std::array<std::vector<sim::EntityId>, control_group_count> groups_{};
    std::map<sim::EntityId, std::size_t> group_numbers_;
    std::optional<std::size_t> last_group_;
    double last_group_seconds_{};
};

} // namespace eawr::presentation::ui
