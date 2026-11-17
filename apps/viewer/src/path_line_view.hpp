#pragma once

#include "eawr/presentation/ui/path_lines.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <cstdint>
#include <span>

namespace eawr::presentation::godot_backend {

// PL-01..PL-08 (docs/behaviour/space-path-lines.md): the selected-ship path line. One mesh with one
// fixed-capacity line surface for the whole fleet, one unshaded alpha-blended material and one draw
// call, whatever the number of selected ships. The surface is created once; a rebuild writes only the
// used vertex range in place (no mesh rebuild on the render thread). The line shortens as the ship flies
// through one material parameter, the presented plan frame, so a frame without a plan or selection
// change touches no buffer. It reads the shared plans of the latest snapshot; nothing reaches the session.
class PathLineView final {
public:
    explicit PathLineView(godot::Node3D& host);
    ~PathLineView();
    PathLineView(const PathLineView&) = delete;
    PathLineView& operator=(const PathLineView&) = delete;

    // The constant colour of the line (PL-04) times the line texture's first texel.
    void set_colour(godot::Color colour);
    // One presented frame. `ships` are the local player's selected ships with a plan, earliest selected
    // first. `enabled` is the GameConstants flag AND a visible world UI.
    void frame(bool enabled, std::span<const ui::PathLineShip> ships, double presented_tick);

    // Report evidence.
    [[nodiscard]] const ui::PathLineStats& stats() const noexcept { return builder_.stats(); }
    [[nodiscard]] std::uint32_t drawn_segments() const noexcept { return drawn_segments_; }
    [[nodiscard]] std::uint32_t uploads() const noexcept { return uploads_; }
    [[nodiscard]] bool usable() const noexcept { return usable_; }

private:
    bool create_surface();
    void upload();
    void release();

    ui::PathLineBuilder builder_;
    godot::RID scenario_, shader_, material_, mesh_, instance_;
    godot::Color colour_{128.0F / 255.0F, 1.0F, 128.0F / 255.0F, 128.0F / 255.0F};
    std::uint32_t capacity_{};          // vertices in the surface
    std::uint32_t vertex_stride_{};
    std::uint32_t attribute_stride_{};
    std::uint32_t attribute_offset_{};
    std::uint32_t written_{};            // vertices currently visible
    std::uint64_t uploaded_generation_{};
    std::uint32_t drawn_segments_{};
    std::uint32_t uploads_{};
    float presented_set_{-1.0e9F};
    bool colour_dirty_{true};
    bool created_{};
    bool usable_{true};
};

} // namespace eawr::presentation::godot_backend
