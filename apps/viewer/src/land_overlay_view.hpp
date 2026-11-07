#pragma once

#include "eawr/sim/tactical/land_debug_overlay.hpp"
#include "eawr/sim/tactical/land_terrain.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>

namespace eawr::presentation::godot_backend {

// Land movement debug overlay (default OFF; F4 toggles it in a land battle). Per squad: the order target as a cross, the path
// polyline with the next node marked, a dot per member at its slot target (parent + axis * slot) joined to the member, and a
// colour per squad state (turning, yielded, lagging, moving). It draws a read-only copy
// (platform::LiveSession::land_overlay); nothing reaches the session, so state, hash and replay are unchanged.
class LandOverlayView final {
public:
    explicit LandOverlayView(godot::Node3D& host);
    ~LandOverlayView();
    LandOverlayView(const LandOverlayView&) = delete;
    LandOverlayView& operator=(const LandOverlayView&) = delete;

    void set_shown(bool shown);
    [[nodiscard]] bool shown() const noexcept { return shown_; }
    // Rebuilds the line mesh from `data` (null keeps the previous lines; the same tick is not rebuilt).
    void frame(const sim::tactical::LandOverlayData* data, const sim::tactical::LandTerrain& terrain);
    void release();

private:
    godot::RID scenario_, shader_, material_, mesh_, instance_;
    bool shown_{};
    std::uint64_t drawn_tick_{~std::uint64_t{0}};
};

} // namespace eawr::presentation::godot_backend
