#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/data/xml.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

// FoC's space fog of war as the world draws it (#494, docs/behaviour/space-fog-presentation.md
// FW-01 to FW-15): a flat plane at SpaceFOWHeight whose fogged cells show a faint tiled grid over
// the backdrop. This is the engine-free part: the constants, the grid of cells over the playable
// rectangle, each cell's fade in and out on the logical-frame clock, the one-cell border, the 3x3
// blur and the colour ramp that turn it into the plane's RGBA texture. The viewer draws the plane
// (apps/viewer/src/live_fog_view.cpp). Presentation only: nothing reaches the simulation.
namespace eawr::presentation::space {

namespace diagnostic_codes {
inline constexpr std::string_view fog_looks = "EAWR-FOG-0101"; // a GameConstants fog value falls back
} // namespace diagnostic_codes

// FW-01: the gameconstants.xml values the space fog plane reads, with FoC's values as defaults.
struct FogLooks {
    std::array<std::uint8_t, 4> colour{255, 255, 255, 255};  // SpaceFOWColor (r, g, b, a)
    double height{-80.0};                                   // SpaceFOWHeight (source Z)
    double cell_size{100.0};                                // DesiredSpaceFOWCellSize
    double regrow_seconds{6.0};                             // SpaceFOWRegrowTime
    // FW-22: SpaceReinforceFOWColor, the colour of the blocked-area overlay (#563).
    std::array<std::uint8_t, 4> reinforce_colour{255, 0, 0, 254};
    // Values that were absent or unreadable; their defaults are used.
    std::vector<core::Diagnostic> diagnostics;
};
// The root of data/xml/gameconstants.xml; the caller loads it.
[[nodiscard]] FogLooks fog_looks(const data::XmlNode& game_constants);

// FW-05: how far a fading cell's value drops per logical frame: the regrow ramp's per-service
// step, int(238 / (seconds x 30 / 16)), spread over the 16 frames of a service. 6 s gives 21/16.
[[nodiscard]] double fog_fade_step_per_frame(double regrow_seconds) noexcept;

// FW-07: the grid of cells: column c covers X from left + c x cell, row r covers Y down from
// top - r x cell (row 0 is the top, as in FoC's fog grid and texture).
struct FogFieldLayout {
    double left{};
    double top{};
    double cell{};
    std::uint32_t wide{};
    std::uint32_t tall{};
    [[nodiscard]] double width() const noexcept { return cell * wide; }
    [[nodiscard]] double height() const noexcept { return cell * tall; }
};
// The rectangle [min_x, max_x] x [min_y, max_y] widened to whole cells around the same centre.
// Nullopt for an empty, non-finite or oversized (more than 4,096 cells a side) rectangle.
[[nodiscard]] std::optional<FogFieldLayout> fog_field_layout(
    double min_x, double max_x, double min_y, double max_y, double cell);

// FW-22: whether the world point (source X, Y) is blocked for deployment: the overlay draws it in
// the reinforcement colour instead of clear. Supplied by the caller (enemy reinforcement-blocking
// objects in FoC); an absent function blocks nothing.
using FogBlockedPoint = std::function<bool(double x, double y)>;

// One of the local team's revealers this frame: its position and planar reveal range.
struct FogFieldRevealer {
    double x{};
    double y{};
    double range{};
};

// FW-08 to FW-12: the per-cell fade and the texture of the fog plane.
class FogField final {
public:
    FogField(FogFieldLayout layout, const FogLooks& looks);

    // One rendered frame. `frames` is the logical frames the battle advanced since the last call
    // (0 while paused; fractions are kept). A cell is held while its centre lies within a
    // revealer's range, the same test as the minimap's fog texels (FW-08).
    void advance(std::span<const FogFieldRevealer> revealers, double frames);
    // The same frame driven by the session's fog cells (FW-08, FW-10): the local player's grid
    // after the newest tick, row by row in this layout (255 held, 1 to 238 lingering, 0 fogged),
    // and the logical frames since that grid's last service (0 to 15). Cells of another size
    // leave the field unchanged.
    void advance(std::span<const std::uint8_t> cells, double since_service, double frames);
    // Same cells supplied as immutable rows, without flattening a live snapshot.
    void advance_rows(std::span<const std::shared_ptr<const std::vector<std::uint8_t>>> rows,
        double since_service, double frames);

    [[nodiscard]] const FogFieldLayout& layout() const noexcept { return layout_; }
    // FW-22 (#563): turns the deployment overlay on or off, reusable for any deployment or
    // reinforcement zone. With it on the plane keeps the fog's own shape but draws every fogged
    // cell, the outermost ring (the unplayable border) included, in the reinforcement colour, and
    // `blocked` (may be empty) turns any clear cell whose centre it reports blocked into the same
    // colour. Redraws the texture at once.
    void set_deployment_overlay(bool on, FogBlockedPoint blocked = {});
    [[nodiscard]] bool deployment_overlay() const noexcept { return overlay_; }
    // The plane's texture: RGBA8, wide x tall texels, row 0 the top row (largest Y).
    [[nodiscard]] std::span<const std::uint8_t> texels() const noexcept { return texels_; }
    // The shown intensity of a cell after the blur (0 fogged, 255 clear).
    [[nodiscard]] std::uint8_t intensity(std::uint32_t column, std::uint32_t row) const noexcept;
    // The cell's fade value on FoC's logical scale (0 fogged, 16 and up clear, 239 to 255 fading in).
    [[nodiscard]] double value(std::uint32_t column, std::uint32_t row) const noexcept;
    [[nodiscard]] std::size_t held_cells() const noexcept { return held_count_; }
    // Cells whose shown intensity is 0 (fully fogged).
    [[nodiscard]] std::size_t fogged_cells() const noexcept;
    // Whether the last advance changed any texel.
    [[nodiscard]] bool changed() const noexcept { return changed_; }
    // Deterministic work budget: texels visited by blur/presentation, excluding unchanged frames.
    [[nodiscard]] std::uint64_t presented_cells() const noexcept { return presented_cells_; }

private:
    template <typename ReadCell>
    void advance_cells(std::size_t count, const ReadCell& read, double since_service, double frames);
    // FW-12, FW-11: the intensities, the ring, the blur and the texels from values_.
    void present();

    FogFieldLayout layout_;
    std::array<std::array<std::uint8_t, 4>, 256> ramp_{};
    std::array<std::array<std::uint8_t, 4>, 256> overlay_ramp_{};
    bool overlay_{};
    FogBlockedPoint blocked_;
    double fade_step_{};
    std::vector<double> values_;
    std::vector<std::uint8_t> held_;
    std::vector<std::uint8_t> intensities_;
    std::vector<std::uint8_t> next_intensities_;
    std::vector<std::uint8_t> blurred_;
    std::vector<std::uint8_t> texels_;
    std::size_t held_count_{};
    bool changed_{true};
    bool force_present_{};
    std::size_t fogged_count_{};
    std::uint64_t presented_cells_{};
};

// FW-09: FoC's map from a cell's fade value to its shown intensity (before the blur).
[[nodiscard]] std::uint8_t fog_intensity(double value) noexcept;

} // namespace eawr::presentation::space
