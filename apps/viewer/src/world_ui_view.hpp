#pragma once

#include "live_session_view.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/presentation/camera/controller.hpp"
#include "eawr/presentation/ui/selection.hpp"
#include "eawr/presentation/ui/world_ui.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// #424: FoC's battle UI in the world (docs/behaviour/foc-battle-world-ui.md), drawn for the live
// battle's local player: the selection circle lying flat under each selected own unit (a 3D quad,
// so the ship hides the part of the ring behind it), and on the battle input's canvas the shield
// and health bars, the squadron icons and the hardpoint reticles of the hovered ship. The rules are
// presentation::ui::world_ui; this class resolves the art and the per-type data and draws. It only
// reads the live view (snapshots, the start's squadrons and colours); nothing reaches the session.
class WorldUiView final {
public:
    explicit WorldUiView(godot::Node3D& host);
    ~WorldUiView();
    WorldUiView(const WorldUiView&) = delete;
    WorldUiView& operator=(const WorldUiView&) = delete;

    // Resolves the art (the faction's selection ring, the command bar atlas, the reticle textures)
    // and each unit type's GUI tags. Never fails the view: what does not resolve is reported.
    void prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const LiveSessionView& live);

    struct Frame final {
        const std::vector<ui::BattleUnit>* units{};
        const ui::Selection* selection{};
        std::optional<std::size_t> hovered;      // index into units: the unit (or craft) under the pointer
        std::optional<sim::EntityId> hovered_icon; // the squadron whose icon is under the pointer
        std::optional<std::array<float, 2>> pointer;
        std::optional<camera::TacticalFrame> camera;
        std::array<float, 2> viewport{};
        std::function<std::optional<std::array<float, 2>>(const ui::Vec3f&)> project;
        const LiveSessionView* live{};
    };
    // Poses the circles and draws the rest into `canvas_item` (already cleared by the caller).
    void draw(const Frame& frame, godot::RID canvas_item);
    // The squadron whose icon the last drawn frame put under `point`.
    [[nodiscard]] std::optional<sim::EntityId> icon_at(std::array<float, 2> point) const;
    // The centre of squadron `squadron`'s icon in the last drawn frame.
    [[nodiscard]] std::optional<std::array<float, 2>> icon_centre(sim::EntityId squadron) const;
    // #550: the last drawn frame's icons as selectable units (the squadron, its type, the icon's
    // centre as its screen point), so a double click on an icon selects by icon like a unit's by model.
    [[nodiscard]] std::vector<ui::BattleUnit> icon_units() const;
    // The report's "world_ui" member, followed by ",\n".
    void write_report(std::ostream& output) const;

private:
    struct HardpointUi final {
        std::string texture;  // reticle art stem, empty: none
        bool targetable{};
        std::array<float, 3> local{};
        float max_health{};
    };
    struct TypeUi final {
        std::string name;
        std::optional<float> circle_side;
        ui::BarSize bar{ui::BarSize::medium};
        float bounds_scale{1.0F};
        bool hide_health_bar{};
        bool shielded{};
        std::string icon;  // Icon_Name
        float layer_z{};   // Layer_Z_Adjust: the height of a combat cell point of this craft type (WU-26)
        std::vector<HardpointUi> hardpoints;
    };
    struct Icon final {
        sim::EntityId squadron{};
        sim::tactical::TypeId type{};
        bool own{};
        float min_x{}, min_y{}, max_x{}, max_y{};
    };
    [[nodiscard]] godot::Ref<godot::ImageTexture> standalone(const vfs::Vfs& filesystem, const std::string& stem);
    [[nodiscard]] std::optional<godot::Rect2> atlas_region(const std::string& name) const;
    void place_circles(const Frame& frame);
    void draw_bars(const Frame& frame, godot::RID canvas_item, float ui_scale);
    void draw_icons(const Frame& frame, godot::RID canvas_item, float ui_scale);
    // WU-25: the dogfighting squadrons' combat cells from the latest snapshot.
    void update_grid(const Frame& frame);
    void draw_reticles(const Frame& frame, godot::RID canvas_item);

    godot::Node3D* host_{};
    std::map<sim::tactical::TypeId, TypeUi> types_;
    godot::Ref<godot::ImageTexture> ring_;
    ui::Rgb ring_tint_{0, 255, 0};
    godot::Ref<godot::StandardMaterial3D> ring_material_;
    godot::Ref<godot::PlaneMesh> ring_mesh_;
    godot::RID scenario_;
    std::vector<godot::RID> circles_;  // RenderingServer instances of ring_mesh_
    godot::Ref<godot::ImageTexture> atlas_;
    std::map<std::string, godot::Rect2> atlas_regions_;
    std::map<std::string, godot::Ref<godot::ImageTexture>> reticles_;
    std::vector<Icon> icons_;
    ui::CombatGrid grid_;
    std::size_t grid_icons_{};
    std::size_t max_grid_icons_{};
    std::vector<std::string> unresolved_;
    std::string ring_source_;
    // Report evidence: what the last frame drew, and the most at once.
    std::size_t circles_drawn_{};
    std::size_t health_bars_{};
    std::size_t shield_bars_{};
    std::size_t reticles_drawn_{};
    std::size_t tracked_reticles_{};
    std::vector<std::string> bar_rows_;   // entity:health level:shield level, last frame
    std::vector<std::string> icon_rows_;  // squadron:state:level, last frame
    std::vector<std::string> flag_rows_;  // squadron:x=,y= of a launched squadron's flag, last frame (#632)
    std::set<std::string> flags_seen_;    // every squadron that showed the flag
    std::size_t max_circles_{};
    std::size_t max_reticles_{};
    std::array<float, 2> reticle_size_{};  // the last reticle drawn, pixels (WU-31)
    std::size_t max_health_bars_{};
};

} // namespace eawr::presentation::godot_backend
