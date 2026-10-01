#pragma once

#include "camera_input.hpp"
#include "eawr/presentation/camera/constants_source.hpp"
#include "eawr/presentation/camera/controller.hpp"
#include "eawr/presentation/camera/free_camera.hpp"
#include "eawr/skirmish/start.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::viewer {

// Explicit, project-authored source-space target rectangle. No TED volume or
// terrain footprint is interpreted as a legal camera bound.
//
// A land config is schema `eawr-map-camera` version 1 (tactical) or 2
// (tactical plus free flight). A space config is schema
// `eawr-space-map-camera` version 1: tactical space bindings only, because no
// free-flight policy is defined for a space map.
struct MapCameraConfig final {
    presentation::camera::SourceTargetBounds bounds;
    std::string map_path;
    std::string map_sha256;
    std::string bindings_path;
    float target_x{};
    float target_y{};
    float target_height{};
    float zoom{};
    float yaw_degrees{};
    std::uint32_t version{1};
    presentation::camera::Mode mode{presentation::camera::Mode::land};
    // Optional `<constant_overrides schema="eawr-map-camera-overrides"
    // version="1">` block, accepted after `<bindings>` in every config
    // version. Empty when the block is absent, which leaves the effective-VFS
    // XML constants untouched. Applied by resolve_map_constants.
    std::string overrides_source_id;
    std::string overrides_authority;
    std::vector<presentation::camera::ConstantOverride> constant_overrides;
    // Project override for the space tactical overview only. FoC's XML value
    // remains the baseline for other maps and for land.
    std::optional<std::uint32_t> overview_clicks;
};

// `mode` selects the schema: land accepts only `eawr-map-camera`, space only
// `eawr-space-map-camera`. Unlocked has no map camera and is rejected.
[[nodiscard]] core::Result<MapCameraConfig> parse_map_camera_config(
    std::string_view xml, std::string_view map_path, std::string_view map_sha256,
    presentation::camera::Mode mode = presentation::camera::Mode::land);

// #908 SC-02: project live-camera policy, centred on the fleet placed around
// the local spawn by PL-01/PL-03 and bounded by the declared map extents.
[[nodiscard]] core::Result<MapCameraConfig> skirmish_camera_config(
    const assets::Map& map, const skirmish::SkirmishStart& start, sim::tactical::PlayerId local_player);

// Applies the config's constant overrides over the XML layer (see
// camera::apply_map_overrides for precedence and rejection rules). Without an
// override block `xml` is returned unchanged and nothing else is checked, so
// default runs keep their exact behaviour. With a block, the layered constants
// are also dry-run through BoundedTacticalController::create with the config's
// bounds and initial pose; a refusal there is EAWR-CAMERA-0005 naming the
// controller's own code. The function is pure: nothing is activated or kept.
//
// `config_name` is recorded as each override's `file`. The viewer reads the
// config from a host path, so it passes only the file name (never a directory);
// the config is identified by `config_sha256`, which equals the report's
// `config_sha256`, not by that name.
[[nodiscard]] core::Result<presentation::camera::LoadedConstants> resolve_map_constants(
    const MapCameraConfig& config, presentation::camera::LoadedConstants xml,
    std::string_view config_name, std::string_view config_sha256);

// Why an overridden scalar has no effect on the bounded map camera path, or
// empty when the controller, solver or adapter reads it. Local project policy
// about this implementation, not a statement about the original game.
[[nodiscard]] std::string_view map_override_inert_reason(std::string_view tag, bool use_splines);

// Report members shared by the land and space map camera sections, starting
// with ", ": the constant precedence statement, every applied override with
// its replaced value, both provenances and its effect on this run
// (`consumed`; `unbound` for an input rate no bound action or edge scrolling
// reads; `inert` with map_override_inert_reason), and the input-default
// ledger for the active binding table (XML rate tags per bound action;
// physical controls are project-authored because no original binding source
// exists), with the table's pan speed scale. `constants` are the active
// layered constants.
[[nodiscard]] std::string map_camera_source_members(
    std::span<const presentation::camera::AppliedOverride> overrides,
    const presentation::camera::Constants& constants,
    const camera_input::BindingTable* table);

// Source-basis terrain heightfield a land map camera follows when the mode's
// Location_Follows_Terrain is nonzero. Heights are row-major (row = source Y
// index) in source units, `spacing` apart from the source origin; each cell is
// the two triangles split along its (x, y)-(x+1, y+1) diagonal, like the
// rendered terrain mesh. Empty means no ground source.
struct TerrainGround final {
    std::uint32_t width{};
    std::uint32_t height{};
    float spacing{};
    std::vector<float> heights;
    // Height at source (x, y), clamped onto the grid edge outside it. nullopt
    // for an empty or inconsistent grid or a non-finite point.
    [[nodiscard]] std::optional<float> at(float x, float y) const;
};

// Everything the host read and hashed for one map camera activation. The
// constants come from the effective VFS through camera::load_constants for the
// config's own mode; no host filesystem path is retained.
struct MapCameraSource final {
    MapCameraConfig config;
    std::string config_file;
    std::optional<std::uint32_t> overview_base_clicks;
    presentation::camera::Constants constants;
    std::vector<presentation::camera::FieldProvenance> provenance;
    std::vector<presentation::camera::AppliedOverride> overrides;
    std::string bindings_json;
    std::string config_sha256;
    std::string bindings_sha256;
    std::string tactical_xml_sha256;
    std::string gameconstants_xml_sha256;
};

// The space overview keeps its XML constants separate from the bounded camera
// constants. Report the effective click count and both sources in one place.
[[nodiscard]] std::string map_overview_source_members(const MapCameraSource& source);

// One consumed tactical step. Movement is local project policy computed from
// the XML constants; nothing here is an original-game measurement.
struct MapCameraTrace final {
    std::uint64_t step{};
    float seconds{};
    float pan_x{};
    float pan_y{};
    float drag_x{};
    float drag_y{};
    bool push_scroll{};
    float zoom_detents{};
    float rotate_units{};
    float orbit_pitch_units{};
    float translate_x{};
    float translate_y{};
    std::uint32_t resets{};
    std::uint32_t view_resets{};
    std::array<float, 3> target_before{};
    std::array<float, 3> target_after{};
    float zoom_before{};
    float zoom_after{};
    float yaw_before{};
    float yaw_after{};
    float pitch_before{};
    float pitch_after{};
};

// Bridges the viewer's input adapter to the bounded tactical controller for a
// land or space map. The context is fixed at construction; a space bridge
// accepts only space-context tactical bindings and has no free flight.
class MapCameraBridge final {
public:
    // Upper bound on retained trace entries; later consumed steps are counted.
    static constexpr std::size_t max_trace_entries = 64;

    explicit MapCameraBridge(bool focused,
        camera_input::Context context = camera_input::Context::land)
        : adapter_(context, focused), context_(context) {}

    // Land terrain to follow; set before activate. The target snaps onto the
    // ground at activation and reset, then eases with the Location_Height_*
    // smooth times; the eye stays Min_Height_Above_Terrain above the ground.
    void set_ground(TerrainGround ground) { ground_ = std::move(ground); }
    [[nodiscard]] bool follows_ground() const noexcept {
        return !ground_.heights.empty() && constants_.location_follows_terrain != 0.0F;
    }

    // All-or-nothing: on failure the bridge (adapter, controller, config,
    // constants) is exactly as before the call.
    [[nodiscard]] core::Result<void> activate(
        MapCameraConfig config, presentation::camera::Constants constants,
        std::string_view bindings_json, std::uint32_t width, std::uint32_t height,
        std::optional<presentation::camera::TacticalFrame> fixed_capture);
    [[nodiscard]] core::Result<void> step(float seconds);
    [[nodiscard]] core::Result<void> set_viewport(float width, float height);
    void set_focus(bool focused) noexcept {
        adapter_.set_focus(focused);
        if (!focused && controller_) controller_->stop_pan();
    }
    void pointer_left() noexcept {
        adapter_.pointer_left();
        if (controller_) controller_->stop_pan();
    }
    [[nodiscard]] core::Result<void> handle(const camera_input::RawEvent& event);
    // #82: the tactical camera looks at render (x, z) (a control group's focus).
    [[nodiscard]] core::Result<void> focus(const float x, const float z) {
        if (!controller_) return core::Result<void>::success();
        return controller_->set_target(x, z);
    }
    // #82: while FoC's tactical overview is on, the yaw it draws at (see
    // TacticalOverview::view_yaw): pan reads its view axes at that yaw, and
    // rotate and orbit input is dropped, as FoC's Tilt_Pan_Camera does
    // nothing in the overview. nullopt: the overview is off.
    void set_overview_view_yaw(const std::optional<float> yaw_degrees) noexcept { overview_view_yaw_ = yaw_degrees; }
    [[nodiscard]] bool active() const noexcept { return controller_.has_value(); }
    [[nodiscard]] camera_input::Context context() const noexcept { return context_; }
    // Space clamps the orbit to the XML pitch range, which may pass under the
    // plane; on land the project range keeps it above the terrain.
    [[nodiscard]] presentation::camera::OrbitPitchRange orbit_pitch_range() const noexcept {
        return context_ == camera_input::Context::space
            ? presentation::camera::space_orbit_pitch_range(constants_)
            : presentation::camera::land_orbit_pitch_range;
    }
    // Degrees one orbit_pitch mouse unit tilts: the XML Pitch_Per_Mouse_Unit,
    // except that a land rate of 0 (FoC's Land_Mode) becomes the project land
    // rate (#348 owner deviation), so land tilts as space does.
    [[nodiscard]] float orbit_pitch_per_mouse_unit() const noexcept {
        return context_ == camera_input::Context::land && constants_.pitch_per_mouse_unit == 0.0F
            ? presentation::camera::land_project_pitch_per_mouse_unit
            : constants_.pitch_per_mouse_unit;
    }
    [[nodiscard]] const std::vector<MapCameraTrace>& trace() const noexcept { return trace_; }
    [[nodiscard]] std::uint64_t trace_dropped() const noexcept { return trace_dropped_; }
    [[nodiscard]] const presentation::camera::TacticalFrame& frame() const noexcept {
        return free_controller_ ? free_frame_ : controller_->frame();
    }
    [[nodiscard]] const presentation::camera::BoundedTacticalController& controller() const noexcept {
        return *controller_;
    }
    [[nodiscard]] const camera_input::Adapter& adapter() const noexcept { return adapter_; }
    [[nodiscard]] const MapCameraConfig& config() const noexcept { return config_; }
    [[nodiscard]] const presentation::camera::Constants& constants() const noexcept { return constants_; }
    [[nodiscard]] std::uint64_t steps() const noexcept { return steps_; }
    [[nodiscard]] std::uint64_t resets() const noexcept { return resets_; }
    // Middle-click view resets: zoom and yaw to the config's initial values,
    // the orbit cleared, the target kept (FoC's click reset keeps it too).
    [[nodiscard]] std::uint64_t view_resets() const noexcept { return view_resets_; }
    [[nodiscard]] std::uint64_t input_callbacks() const noexcept { return input_callbacks_; }
    [[nodiscard]] const std::string& rejected_event() const noexcept { return rejected_event_; }
    [[nodiscard]] bool free_active() const noexcept { return free_controller_.has_value(); }
    [[nodiscard]] const std::optional<presentation::camera::FreeCameraPose>& free_pose() const noexcept {
        return free_pose_;
    }
    [[nodiscard]] const std::optional<presentation::camera::FreeCameraSettings>& free_settings() const noexcept {
        return free_settings_;
    }
    [[nodiscard]] const presentation::camera::TacticalFrame& saved_tactical_frame() const noexcept {
        return saved_tactical_frame_;
    }
    [[nodiscard]] std::uint64_t free_entries() const noexcept { return free_entries_; }
    [[nodiscard]] std::uint64_t free_exits() const noexcept { return free_exits_; }
    [[nodiscard]] std::uint64_t free_rejections() const noexcept { return free_rejections_; }
    [[nodiscard]] std::uint64_t free_steps() const noexcept { return free_steps_; }
    [[nodiscard]] const std::string& free_rejection() const noexcept { return free_rejection_; }
    [[nodiscard]] const std::vector<std::string>& free_transitions() const noexcept { return free_transitions_; }

private:
    camera_input::Adapter adapter_;
    camera_input::Context context_;
    std::vector<MapCameraTrace> trace_;
    std::uint64_t trace_dropped_{};
    std::optional<presentation::camera::BoundedTacticalController> controller_;
    presentation::camera::Constants constants_;
    MapCameraConfig config_;
    std::uint64_t steps_{};
    std::uint64_t resets_{};
    std::uint64_t view_resets_{};
    std::uint64_t input_callbacks_{};
    std::string rejected_event_;
    std::optional<presentation::camera::FreeCameraSettings> free_settings_;
    std::optional<presentation::camera::FreeCameraController> free_controller_;
    std::optional<presentation::camera::FreeCameraPose> free_pose_;
    presentation::camera::TacticalFrame saved_tactical_frame_;
    presentation::camera::TacticalFrame free_frame_;
    std::uint64_t free_entries_{};
    std::uint64_t free_exits_{};
    std::uint64_t free_rejections_{};
    std::uint64_t free_steps_{};
    std::string free_rejection_;
    std::vector<std::string> free_transitions_;
    TerrainGround ground_;
    std::optional<float> overview_view_yaw_;
    bool focus_held_{};
    [[nodiscard]] core::Result<void> toggle_free();
};

} // namespace eawr::viewer
