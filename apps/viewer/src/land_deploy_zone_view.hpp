#pragma once

#include "eawr/sim/tactical/land_terrain.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace eawr::presentation::godot_backend {

// #7190: the ground a land player may call a company down on, drawn while a pool entry is being dragged: one ring per allied
// reinforcement point at its enabling radius. Presentation only: it reads positions and radii the session already published
// and writes nothing back, so state, hash and replay are unchanged. The ring is a plain line loop; how the retail game draws
// these zones was not captured, so none of its look is claimed here (docs/land-live-view.md).
class LandDeployZoneView final {
public:
    struct Ring {
        double x{}, y{}, radius{};
        friend bool operator==(const Ring&, const Ring&) = default;
    };

    explicit LandDeployZoneView(godot::Node3D& host);
    ~LandDeployZoneView();
    LandDeployZoneView(const LandDeployZoneView&) = delete;
    LandDeployZoneView& operator=(const LandDeployZoneView&) = delete;

    // The rings to show now; an empty list hides the drawing. The mesh is rebuilt only when the list changes.
    void frame(std::span<const Ring> rings, const sim::tactical::LandTerrain& terrain);
    [[nodiscard]] bool shown() const noexcept { return instance_.is_valid(); }
    [[nodiscard]] std::size_t ring_count() const noexcept { return rings_.size(); }
    void release();

private:
    void clear_mesh();

    godot::RID scenario_, shader_, material_, mesh_, instance_;
    std::vector<Ring> rings_;
};

} // namespace eawr::presentation::godot_backend
