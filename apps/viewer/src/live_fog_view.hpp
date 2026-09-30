#pragma once

#include "live_session_view.hpp"

#include "eawr/presentation/camera/controller.hpp"
#include "eawr/presentation/space/fog_field.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/plane_mesh.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

// #494: FoC's space fog of war in the world of the live battle
// (docs/behaviour/space-fog-presentation.md FW-01 to FW-15). A flat plane at SpaceFOWHeight over
// the playable rectangle shows the local player's fog: fogged cells draw the faint tiled grid of
// W_Space_FOW_Grid in SpaceFOWColor over the backdrop, revealed cells are clear, and cells fade in
// and out on the battle's logical-frame clock. It reads the local player's fog cells that the
// session publishes with each tick (#495's grid, the same cells as the minimap's fog, MM-10), or
// in a battle without fog rules the local team's sensor circles; nothing reaches the session, so
// the headless hashes are unchanged.
class LiveFogView final {
public:
    explicit LiveFogView(godot::Node3D& host);
    ~LiveFogView();
    LiveFogView(const LiveFogView&) = delete;
    LiveFogView& operator=(const LiveFogView&) = delete;

    // Reads the GameConstants fog values and the grid texture. `revealed` (--eawr-live-reveal on)
    // draws no fog, as FoC draws none for a player whose map is revealed (FW-14). Never fails the
    // run: what does not resolve is reported and the plane is not drawn.
    // `deploy_overlay` (--eawr-live-deploy-overlay on, #563) draws the deployment overlay instead:
    // fogged cells and the unplayable border in SpaceReinforceFOWColor on W_Space_Reinforce_FOW_Grid
    // (FW-22), the mode FoC shows while placing reinforcements.
    void prepare(const vfs::Vfs& filesystem, bool revealed, bool deploy_overlay = false);

    // One rendered frame, before it is drawn: the local player's fog cells of the newest snapshot
    // (or, without them, the local team's sensors over the camera's target bounds, FW-07),
    // advanced by the presented ticks since the last frame.
    void frame(const LiveSessionView& live, const std::optional<camera::SourceTargetBounds>& bounds);
    void release();
    // The report's "live_fog" member, followed by ",\n".
    void write_report(std::ostream& output) const;

private:
    [[nodiscard]] bool create_plane(const space::FogFieldLayout& layout);

    godot::Node3D* host_{};
    space::FogLooks looks_;
    std::optional<space::FogField> field_;
    bool revealed_{};
    bool prepared_{};
    std::string status_{"not prepared"};
    std::string grid_source_;
    // FW-08: "cells" (the session's fog cells) or "sensors" (the local team's circles, a battle
    // without fog rules); empty before the plane is built.
    std::string source_;
    std::uint64_t cell_tick_{};
    std::vector<std::string> notes_;
    godot::Ref<godot::ImageTexture> grid_;
    godot::Ref<godot::ImageTexture> reinforce_grid_;
    bool deploy_overlay_{};
    std::string reinforce_grid_source_;
    godot::Ref<godot::ImageTexture> cells_;
    godot::Ref<godot::Shader> shader_;
    godot::Ref<godot::ShaderMaterial> material_;
    godot::Ref<godot::PlaneMesh> mesh_;
    godot::RID scenario_;
    godot::RID instance_;
    std::optional<double> last_tick_;
    // Report evidence.
    std::uint64_t frames_{};
    std::uint64_t uploads_{};
    double advanced_frames_{};
    std::size_t revealers_{};
    std::size_t held_cells_{};
    std::size_t fogged_cells_{};
    std::size_t max_held_cells_{};
};

} // namespace eawr::presentation::godot_backend
