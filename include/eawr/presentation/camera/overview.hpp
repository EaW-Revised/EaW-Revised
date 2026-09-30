#pragma once

#include "eawr/core/result.hpp"
#include "eawr/presentation/camera/camera.hpp"
#include "eawr/presentation/camera/constants_source.hpp"
#include "eawr/presentation/camera/controller.hpp"

#include <cstdint>
#include <optional>

// #82 P2-19: FoC's tactical overview, the zoom levels past Distance_Max
// (docs/behaviour/foc-battle-selection.md#tactical-overview). Read in the FoC debug build's
// tactical camera controller: wheel clicks out at Distance_Max, counted within
// Tactical_Overview_Click_Time, enter the overview (Tactical_Overview_Distance, _Pitch, _FOV); the
// same again enters the map overview (Tactical_Overview_Distance2, _Pitch2, yaw 0); a click in steps
// back. The overview key (Insert) cycles overview, map overview and back. FoC sets the camera's own
// yaw to 0 on entering the map overview, so its pan follows the drawn view; the yaw stays 0 back in
// the overview and the saved tactical yaw returns when the overview is left. No rotation or tilt
// input applies while an overview level is on. The fade between the levels and what the battle UI
// hides are presentation::ui::overview_ui (#848, V-5a to V-5g). Presentation only, like the rest of the camera module.
// The project space map and live-battle configs override the FoC ten-click count to five (#413).
namespace eawr::presentation::camera {

// Tactical_Overview_* of one camera mode. An absent tag keeps the value FoC's
// camera constants constructor sets, which the debug build shows.
struct OverviewConstants final {
    float distance{900.0F};
    float distance2{1800.0F};
    float pitch{62.0F};
    float pitch2{90.0F};
    float fov{55.0F};
    float fov2{70.0F};
    std::uint32_t clicks{99};
    float click_time{0.0F};

    friend bool operator==(const OverviewConstants&, const OverviewConstants&) = default;
};

// Reads the Tactical_Overview_* tags of `mode`'s <TacticalCamera> from tacticalcameras.xml, under
// the scalar policy of load_constants. Clicks must be a whole number of at least one.
[[nodiscard]] core::Result<OverviewConstants> load_overview_constants(XmlSource tactical_cameras, Mode mode);

enum class OverviewLevel : std::uint8_t { off = 0, overview = 1, map = 2 };

// FoC counts overview clicks only after the zoom out that reached Distance_Max has settled for 26
// frames of its frame counter. The remake reads them as logic frames at 30 per second, which is
// unverified.
inline constexpr double overview_settle_seconds = 26.0 / 30.0;

class TacticalOverview final {
public:
    explicit TacticalOverview(OverviewConstants constants) noexcept : constants_(constants) {}

    // One wheel event of `detents` notches, > 0 out. `live_distance` is the tactical camera's current
    // distance; FoC counts clicks once it is within half a unit of `distance_max`. Returns whether
    // the tactical controller should also zoom, which it does only while the overview is off.
    bool wheel(float detents, float live_distance, float distance_max, double now_seconds);
    // The overview key: off -> overview -> map -> off.
    void toggle() noexcept;
    // Runs the click timer (Render_Service): past Tactical_Overview_Click_Time the count restarts.
    void advance(double seconds) noexcept;

    [[nodiscard]] OverviewLevel level() const noexcept { return level_; }
    // The yaw the overview draws and pans at, given the tactical camera's: 0 from the map overview
    // until the overview is left, else the tactical yaw. Absent while the overview is off.
    [[nodiscard]] std::optional<float> view_yaw(float tactical_yaw_degrees) const noexcept;
    [[nodiscard]] const OverviewConstants& constants() const noexcept { return constants_; }
    [[nodiscard]] std::uint32_t pending_clicks() const noexcept { return clicks_; }
    [[nodiscard]] std::uint64_t transitions() const noexcept { return transitions_; }

    // The frame drawn while the overview is on: the tactical frame's target, viewport and clip
    // planes; the level's distance, pitch and field of view; view_yaw; the map overview's distance
    // is also capped so the larger half extent of the map fits Tactical_Overview_FOV2. The far plane grows to keep the target in view. Pitch is
    // capped at 89 degrees so the view is never vertical. Off: the tactical frame unchanged.
    [[nodiscard]] core::Result<TacticalFrame> frame(const TacticalFrame& tactical, float tactical_yaw_degrees,
                                                    float map_half_extent) const;

private:
    void enter(OverviewLevel level) noexcept;
    OverviewConstants constants_;
    OverviewLevel level_{OverviewLevel::off};
    // Set on entering the map overview, cleared on leaving the overview (FoC's yaw 0).
    bool yaw_zeroed_{};
    std::uint32_t clicks_{};
    double click_timer_{};
    // When a zoom out last moved the camera; clicks within overview_settle_seconds do not count.
    std::optional<double> last_zoom_out_{};
    std::uint64_t transitions_{};
};

} // namespace eawr::presentation::camera
