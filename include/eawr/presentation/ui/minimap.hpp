#pragma once

// #455 (P2 gap 4, #83): the tactical space minimap of the command bar (docs/behaviour/foc-minimap.md).
// Engine-free and presentation only: it reads what the local player sees in the newest snapshot and
// where the camera looks. A left press or drag on it only moves the camera; a right click hands a
// world point to the battle input, which orders the selection there as a right click on the world
// does. Nothing here reaches the simulation, the replay or a hash.
//
// Coordinates: world X/Y are source units (the simulation's plane). Minimap points are the radar's
// own frame (MM-03): x and y from -1 to 1 across the minimap, +y up, the world square's centre at 0.

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/xml.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::data {
class Catalog;
}

namespace eawr::presentation::ui {

namespace diagnostic_codes {
inline constexpr std::string_view minimap_settings = "EAWR-UI-0330"; // a RadarMap.xml or GameConstants value falls back
} // namespace diagnostic_codes

// MM-01, MM-05: RadarMap.xml's space settings and the GameConstants radar colours.
struct MinimapSettings {
    data::ui::Rgba8 background{12, 30, 51, 255};    // MM-14: <Color name="space"> under RadarMapSettings
    std::string backdrop{"i_radar_map_grid.tga"};   // Space_Backdrop_Texture_Name
    data::ui::Rgba8 fog{25, 66, 120, 100};          // Space_FOW_Color
    bool guide_rectangle{};                         // Space_Is_Guide_Rectangle
    bool colorize_selected{true};                   // Radar_Colorize_Selected_Units
    data::ui::Rgba8 selected{209, 255, 209, 255};   // Radar_Selected_Units_Color
    data::ui::Rgba8 nebula{255, 255, 255, 64};     // Nebula_Effect_Color, WHZ-71
    data::ui::Rgba8 field{103, 130, 139, 127};     // Space_Asteroid_Field_Color, WHZ-72
    data::ui::Rgba8 field_border{174, 171, 200, 127}; // Space_Asteroid_Field_Border_Color
    // MM-07: Factions.xml `Color` per faction name, for owners without a lobby colour.
    std::vector<std::pair<std::string, data::ui::Rgba8>> faction_colours;
    std::vector<std::pair<std::string, data::ui::Rgba8>> faction_no_colorization;
    std::vector<core::Diagnostic> diagnostics;
};
// Any root may be null (the file is missing); its values keep FoC's.
[[nodiscard]] MinimapSettings minimap_settings(const data::XmlNode* radar_map, const data::XmlNode* game_constants,
                                               const data::XmlNode* factions = nullptr);
[[nodiscard]] MinimapSettings minimap_settings(const vfs::Vfs& filesystem);
// A faction's colour (any case), or nothing.
[[nodiscard]] std::optional<data::ui::Rgba8> faction_colour(const MinimapSettings& settings, std::string_view faction,
    bool no_colorization = false);

// MM-06: what an object type shows on the minimap (its XML, with the engine's defaults).
struct MinimapTypeLooks {
    bool visible{};                                  // Is_Visible_On_Radar (default No)
    bool visible_to_enemy{true};                     // Is_Visible_On_Enemy_Radar
    std::string icon{"i_radar_default_blip.tga"};    // Radar_Icon_Name
    std::array<float, 2> size{0.05F, 0.05F};         // Radar_Icon_Size: half extents in minimap units
    bool show_facing{true};                          // Radar_Show_Facing
    bool rotate_icon{};                              // Radar_Rotate_Icon: the texture turns a quarter
    float point_size{2.0F};                          // MM-16: Radar_Blip_Size, truncated before plotting
    bool draw_to_scale{};                            // MM-17: Radar_Draw_To_Scale (default No)
    double space_scale{2.0};                         // Radar_Icon_Scale_Space
    bool hazard{};                                  // WHZ-70: field, storm or nebula
    std::uint8_t hazard_kind{};                      // presentation report: field 1, storm 2, nebula 4
    bool visible_when_fogged{};                     // WNO-41: only the two early presentation gates
    std::optional<data::ui::Rgba8> no_colorization;
};
[[nodiscard]] MinimapTypeLooks minimap_type_looks(std::string_view type, const data::Catalog* objects);

// MM-02: the world square the minimap spans, and the playable rectangle inside it.
struct MinimapExtents {
    double min_x{-1.0};
    double min_y{-1.0};
    double max_x{1.0};
    double max_y{1.0};
    // The camera's bounds: outside them the fog layer stays clear (MM-10).
    double playable_min_x{-1.0};
    double playable_min_y{-1.0};
    double playable_max_x{1.0};
    double playable_max_y{1.0};
};
// The square around the camera bounds' centre whose half side is their larger half extent.
[[nodiscard]] MinimapExtents minimap_extents(double min_x, double max_x, double min_y, double max_y) noexcept;

struct MinimapPoint {
    double x{};
    double y{};
    friend constexpr bool operator==(const MinimapPoint&, const MinimapPoint&) noexcept = default;
};
[[nodiscard]] MinimapPoint minimap_point(const MinimapExtents& extents, double x, double y) noexcept;
[[nodiscard]] std::array<double, 2> minimap_world(const MinimapExtents& extents, MinimapPoint point) noexcept;

// WNO-41: copied presentation admission, independent of selection and targeting.
struct MinimapLiveState {
    bool alive{true};
    bool limbo{};
    bool capital_layer{};
    bool model_hidden{};
    bool radar_faded{};
    bool replay{};
    bool locally_visible{true};
    bool interdicted{};
    bool jammed{};
    bool stealthed{};
    bool display_enemies{true};
};
struct MinimapCaptureColour {
    bool raw_local_fog{};
    data::ui::Rgba8 old_colour{100, 100, 100, 255};
    data::ui::Rgba8 new_colour{100, 100, 100, 255};
    double progress{};
};
struct MinimapPlayerColour {
    sim::tactical::PlayerId player{};
    sim::tactical::TeamId team{};
    bool neutral{};
    data::ui::Rgba8 colour;
};
[[nodiscard]] data::ui::Rgba8 minimap_community_colour(std::span<const MinimapPlayerColour> players,
    sim::tactical::PlayerId owner, sim::tactical::PlayerId local, bool multiplayer,
    data::ui::Rgba8 fallback) noexcept;
[[nodiscard]] data::ui::Rgba8 minimap_capture_colour(const MinimapCaptureColour& capture,
    data::ui::Rgba8 neutral) noexcept;

// A live radar candidate; never a selection or targeting input.
struct MinimapUnit {
    sim::EntityId id{};
    std::string type;
    data::ui::Rgba8 owner_colour{255, 255, 255, 255}; // the owner's player colour (MM-07)
    bool hostile{};
    bool selected{};
    double x{};
    double y{};
    double yaw_degrees{}; // facing about +z from +x
    bool in_nebula{};     // WHZ-70/MM-12: suppress hostile blips independently of fog bypass
    std::array<double, 2> world_half_size{}; // model's world bounds, before the radar scale
    bool team{};         // MM-17: a model-free team's extent is the authored radar scale itself
    MinimapLiveState radar{};
    std::optional<MinimapCaptureColour> capture{};
};

// WNO-44: stored identity and transform, with no live admission/selection fields.
struct MinimapMemory {
    sim::EntityId id{};
    std::string type;
    data::ui::Rgba8 owner_colour{255, 255, 255, 255};
    bool hostile{};
    bool capture_point{};
    bool previously_revealed{};
    bool retained_model{};
    bool display_enemies{true};
    double x{}, y{}, yaw_degrees{};
    std::array<double, 2> world_half_size{};
};

// MM-15: live members in team order, including members hidden by fog.
struct MinimapSquadronMember {
    double x{}, y{}, yaw_degrees{};
    bool visible{};
};
struct MinimapSquadronPose {
    double x{}, y{}, yaw_degrees{};
};
// Empty teams and teams whose first live member is unseen have no radar identity.
[[nodiscard]] std::optional<MinimapSquadronPose> minimap_squadron_pose(
    std::span<const MinimapSquadronMember> members) noexcept;

// One textured quad or empty-icon point of the minimap (MM-06, MM-07, MM-08, MM-16).
struct MinimapBlip {
    sim::EntityId id{};
    std::string icon;
    MinimapPoint centre;
    std::array<double, 2> half_size{};
    double rotation_degrees{}; // counter-clockwise in the minimap frame; 0 draws the icon upright
    bool rotate_icon{};
    data::ui::Rgba8 colour{255, 255, 255, 255};
    std::uint32_t point_pixels{1}; // MM-16: only the truncated size 2 produces a 2x2 block
    bool remembered{};
};
struct MinimapPixelRect {
    std::uint32_t x{}, y{}, width{}, height{};
    friend constexpr bool operator==(const MinimapPixelRect&, const MinimapPixelRect&) noexcept = default;
};
// MM-16: texture pixels, top row first. A centre on the right or bottom edge is outside;
// a 2x2 point on the last interior row/column is clipped rather than recentered.
[[nodiscard]] std::optional<MinimapPixelRect> minimap_point_pixels(const MinimapBlip& blip,
    std::uint32_t width, std::uint32_t height) noexcept;
// The blips in draw order: the engine submits its icon list from the back, so the first unit is
// drawn last and sits on top. Units whose type is not visible on the radar, or hostile units whose
// type is hidden from enemy radars, are left out, as is a unit outside the world square.
[[nodiscard]] std::vector<MinimapBlip> minimap_blips(std::span<const MinimapUnit> units,
    const std::function<const MinimapTypeLooks&(std::string_view type)>& looks, const MinimapExtents& extents,
    const MinimapSettings& settings, std::span<const MinimapMemory> memories = {});

struct MinimapHazard {
    double x{}, y{};
    double x_extent{}, y_extent{}; // model bounding-box half extents, independent of soft tracking
    std::uint8_t kind{};
    friend bool operator==(const MinimapHazard&, const MinimapHazard&) = default;
};
// WHZ-72: one shared ellipse mask, two edge orientations and Gaussian fill/border coverage.
[[nodiscard]] std::vector<std::uint8_t> minimap_hazards(std::span<const MinimapHazard> hazards,
    const MinimapExtents& extents, const MinimapSettings& settings, std::uint32_t width, std::uint32_t height);

// MM-09: the camera's view on the minimap. `ground` holds where the rays through the viewport's
// top-left, top-right, bottom-right and bottom-left corners meet the reference plane (world X/Y).
// A rectangle guide draws their bounding box instead. The outline closes back to the first point.
[[nodiscard]] std::array<MinimapPoint, 4> minimap_guide(const std::array<std::array<double, 2>, 4>& ground,
                                                        const MinimapExtents& extents, bool rectangle);
// The part of the segment a-b inside the minimap (-1 to 1 on both axes), or nothing: the outline's
// lines stop at the minimap's edge, as the radar viewport cuts them.
[[nodiscard]] std::optional<std::array<MinimapPoint, 2>> minimap_clip(MinimapPoint a, MinimapPoint b) noexcept;

// MM-10: the fog layer, one texel per minimap pixel, rebuilt a few rows a frame into a back buffer
// that replaces the shown one when it is complete. A texel inside the playable rectangle that no
// revealer of the local player's team covers takes the fog colour; every other texel is clear.
struct MinimapRevealer {
    double x{};
    double y{};
    double range{};
};
// #494: the local player's fog cells (the simulation's grid, row 0 at the top edge `top`); a
// point is fogged when its cell holds zero or it lies outside the grid, as the world's fog plane
// reads the same cells (space-fog-presentation.md FW-08).
struct MinimapFogCells {
    double left{};
    double top{};
    double cell{};
    std::uint32_t wide{};
    std::uint32_t tall{};
    std::shared_ptr<const std::vector<std::uint8_t>> values;
    std::shared_ptr<const std::vector<std::shared_ptr<const std::vector<std::uint8_t>>>> rows{};
    [[nodiscard]] bool revealed(double x, double y) const noexcept;
};
class MinimapFog final {
public:
    // Rows written each frame (the engine's nine).
    static constexpr std::uint32_t rows_per_frame = 9;

    // A new size restarts: the next advance() fills every row at once, as a new fog source does.
    void resize(std::uint32_t width, std::uint32_t height);
    // One frame. False without a size. `show` false (fog off) leaves every texel clear.
    bool advance(const MinimapExtents& extents, std::span<const MinimapRevealer> revealers, data::ui::Rgba8 fog,
                 bool show);
    // The same frame read from the fog cells instead of the revealers' circles.
    bool advance(const MinimapExtents& extents, const MinimapFogCells& cells, data::ui::Rgba8 fog, bool show);
    // The shown texels, RGBA8 row by row, the top row (largest world Y) first.
    [[nodiscard]] std::span<const std::uint8_t> texels() const noexcept { return front_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    // How many complete passes have been shown (the evidence of the update rate).
    [[nodiscard]] std::uint64_t passes() const noexcept { return passes_; }
    [[nodiscard]] std::uint32_t next_row() const noexcept { return next_row_; }
    // Texels of the shown layer that are fogged.
    [[nodiscard]] std::size_t fogged() const noexcept;

private:
    bool advance(const MinimapExtents& extents, const std::function<bool(double, double)>& revealed,
                 data::ui::Rgba8 fog, bool show);

    std::uint32_t width_{};
    std::uint32_t height_{};
    std::uint32_t next_row_{};
    bool full_{true};
    std::uint64_t passes_{};
    std::vector<std::uint8_t> front_;
    std::vector<std::uint8_t> back_;
};

// MM-13: the backdrop texture repeats this many times across the minimap on each axis.
inline constexpr int minimap_backdrop_repeats = 25;

// MM-11: a left drag on the minimap follows the pointer once it has moved this far from the press.
inline constexpr double minimap_drag_pixels = 12.0;

} // namespace eawr::presentation::ui
