#pragma once
#include <utility>

#include "map_camera.hpp"

#include "eawr/assets/map.hpp"
#include "eawr/core/result.hpp"
#include "eawr/presentation/lighting/scene_bloom.hpp"
#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/space/environment_scene.hpp"
#include "eawr/sim/snapshot.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/node3d.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <vector>

namespace eawr::data {
class Catalog;
} // namespace eawr::data

namespace eawr::presentation::godot_backend {

class FogMode;
class GodotRenderer;
class GodotShaderCache;

// #32 hook for the populated space scene. The default environment view calls
// it once, after its own uploads and before the first frame. The hook uploads
// its own assets through `renderer`, using asset and entity ids at or above the
// given bases, and returns the instances to submit with the environment on
// every frame. `environment_records` are the TED placement records the
// environment already composes (planets and nebulae,
// space::environment_family); the hook must skip them. A failure fails the run.
struct SpacePopulateContext final {
    GodotRenderer& renderer;
    const FixedCamera& camera;
    const assets::Map& map;
    // Environment 0's light 0, when its record decodes (source basis).
    const space::EnvironmentLight* light{};
    std::span<const std::uint32_t> environment_records;
    sim::AssetId first_asset{};
    sim::EntityId first_entity{};
};
// One frame of a live population (#80, the live session's units).
struct SpaceLiveUpdate final {
    // This frame's population instances; they replace the previous ones.
    std::optional<std::vector<sim::RenderInstance>> instances;
    // Capture this frame too, to the capture path with this suffix before
    // its extension.
    std::optional<std::string> capture_suffix;
    // Keep running past the timed frames (the live run is not done yet).
    bool hold{};
    // #453 BEP-03: the player quit; the run ends this frame, timed frames or not.
    bool quit{};
    // The live source stopped (the simulation thread failed): the view keeps the last
    // instances and shows this on screen. An interactive view keeps running; a capture run
    // fails with it.
    std::optional<std::string> error;
};
struct SpacePopulateResult final {
    std::vector<sim::RenderInstance> instances;
    std::function<void(GodotRenderer&, std::uint32_t)> tick;
    // Called once per frame after tick with the frame's delta in seconds.
    // Called each frame with the camera the frame renders (#80: the battle effects face it).
    std::function<core::Result<SpaceLiveUpdate>(GodotRenderer&, double, const FixedCamera&)> live;
    std::function<void(GodotRenderer&)> release;
    std::function<void(std::ostream&)> write_report;
};
using SpacePopulateHook = std::function<core::Result<SpacePopulateResult>(const SpacePopulateContext& context)>;

// Viewer-owned attached emitters share the land provider and advance beside
// the environment snapshot. Their resources are released before the renderer.
struct SpaceEffectsHook final {
    // Advances the effects to the frame's presentation tick (the idle clips'
    // 30 Hz clock: frame count in a capture, real time in the live view).
    std::function<bool(const FixedCamera&, std::uint32_t tick)> tick;
    // Called before each tick with the scene renderer, whose lighting the
    // bump-mapped effects follow (#238).
    std::function<void(const GodotRenderer&)> follow_lighting;
    std::function<void()> release;
    std::function<void(std::ostream&)> write_report;
};

// A kind-2 (space) map, reached from MapMode after the map has loaded.
//
// Default (no --eawr-space-control or fog): the
// E-space-environment-v1 view. Environment 0's primary and secondary sky,
// the planet and nebula placements and an approximate sun under the default
// tactical camera (space::default_camera_policy), --eawr-space-camera, or a
// live --eawr-map-camera-config. See space/environment_scene.hpp.
//
// With --eawr-space-control (none included) or fog: the
// E-space-primary-sky-v1 evidence harness below.
//
// E-space-primary-sky-v1 composition for a kind-2 (space) map. It draws environment 0's primary sky
// through the modern spatial route, one renderer asset per accepted surface
// with that surface's own BaseTexture, under an explicit fixed camera. An
// accepted MeshGloss.fx surface draws through the eawr-space-sky-meshgloss-v1
// adapter with its authored Emissive/Diffuse/Specular under the declared
// sky light policy. Land composition (terrain, footprint, placements,
// lighting) is not involved.
//
// The evidence harness also supports the space tactical camera when selected
// explicitly with --eawr-space-control. Its fixed capture path locks the
// bridge to --eawr-space-camera for the harness's comparison phases.
class SpaceEnvironment final {
public:
    struct Options final {
        std::string map_path;
        std::string map_sha256;
        bool semantic_complete{};
        std::string profile;
        std::vector<std::string> layers;
        std::shared_ptr<GodotShaderCache> shaders;
        std::filesystem::path report_path;
        std::filesystem::path capture_path;
        std::uint32_t warmup_frames{30};
        std::uint32_t timed_frames{120};
        // --eawr-camera-interactive: the populate tick is the view's real time
        // in 30 Hz ticks rather than its frame count (#145).
        bool real_time_clock{};
        // A fixed capture's presentation clock holds at `clock_hold_ticks` - 1
        // (the map particle frames), and every clock adds `clock_offset` ticks
        // (--eawr-map-idle-offset), as the idle clips do. The environment
        // effects' TIME follows it (#185).
        std::uint32_t clock_hold_ticks{60};
        std::uint32_t clock_offset{};
        // --eawr-space-camera: twelve render-basis numbers (eye, target, up,
        // fov, near, far). The default environment view uses it instead of
        // its default camera. The evidence harness requires it unless an
        // unlocked map camera draws from its authored pose.
        std::string camera;
        // --eawr-space-control: none, drop-submission, broken-shader,
        // occluder, occluder-full, reload-cycle (live create/destroy),
        // sky-shadow or sky-shadow-cast (lit receiver; the latter is the
        // positive control that lets the sky cast), or meshgloss-light-probe
        // (a labelled light policy that makes the MeshGloss Diffuse and
        // Specular bindings measurable; the shipped policy is unlit).
        std::string control;
        // --eawr-map-camera-config for a space map: a validated space config,
        // its bindings and the Space_Mode constants, all read and hashed by
        // the host. The default environment view uses its authored pose and
        // forwards live input to this bridge.
        std::optional<viewer::MapCameraSource> map_camera;
        // --eawr-map-camera-selftest: real Godot events through the host.
        bool camera_selftest{};
        // Terminal input probes (unlocked only): a fixed-step camera bridge
        // consumes a synthetic pan press at the declared terminal step, with
        // the key then held through capture or released immediately afterward.
        bool camera_terminal_baseline_test{};
        bool camera_terminal_hold_test{};
        bool camera_terminal_release_test{};
        // --eawr-map-camera-unlocked-capture: where the unlocked PNG goes.
        std::filesystem::path unlocked_capture_path;
        // #201: the scene bloom of the default environment view; the evidence
        // harness (a control or fog) never blooms, so its pixel checks hold.
        std::optional<lighting::bloom::SceneBloom> bloom;
        // #307: no bloom because the lighting policy is off (reported as lighting_off).
        bool bloom_skipped_unlit{};
        // P1-07 #28 opt-in synthetic space fog. Null keeps the environment-only
        // mode exactly as before. Otherwise MapMode owns the validated source
        // grids and the catalog; only placements whose XML element type is in
        // `fog_admit` are composed, as fog-consuming units in front of the sky,
        // which is never a fog consumer. `fog_paint_evidence` (no fixed
        // capture) adds painted/restored presentation-override phases.
        FogMode* fog{};
        const data::Catalog* catalog{};
        std::vector<std::string> fog_admit;
        std::filesystem::path fog_paint_evidence;
        // #32: the populated scene, composed by the default environment view
        // only. Empty composes the environment alone.
        SpacePopulateHook populate;
        SpaceEffectsHook effects;
        // P2-20a (#83): writes the report's "hud" line when a HUD is drawn.
        std::function<void(std::ostream&)> write_hud_report;
    };

    explicit SpaceEnvironment(Options options);
    ~SpaceEnvironment();
    SpaceEnvironment(const SpaceEnvironment&) = delete;
    SpaceEnvironment& operator=(const SpaceEnvironment&) = delete;

    // Plans, uploads and submits. A false return has already written the
    // report; the caller exits non-zero.
    [[nodiscard]] bool ready(godot::Node3D& host, const assets::Map& map, const vfs::Vfs& filesystem,
                             const assets::ObjectTypeCatalog& catalog, bool catalog_loaded,
                             const std::string& catalog_failure);
    [[nodiscard]] std::optional<int> process(double delta);
    // The player closed the window (a live view): releases the scene and writes the report now,
    // since the engine quits without another frame. Inert for a run that has finished.
    void close();

    // Host lifecycle routed to the space camera; inert without one.
    void camera_event(const viewer::camera_input::RawEvent& event);
    void camera_focus(bool focused);
    void camera_pointer_left();
    void camera_viewport(float width, float height);

    // #82, the live battle input: the camera frame the view drew last (render
    // basis; nothing before the first frame or without the populated view),
    // a look-at for a control group's double tap (source X/Y; only an
    // interactive space map camera moves), the tactical overview key, and the
    // overview level ("off", "overview", "map" or "unavailable").
    [[nodiscard]] std::optional<presentation::camera::TacticalFrame> live_camera_frame() const;
    [[nodiscard]] std::pair<bool, bool> live_camera_pointer_mode(bool ctrl) const;
    void live_camera_focus(float source_x, float source_y);
    // #455: the space map camera's target bounds (source X/Y), which the minimap spans; nothing
    // without the populated view's map camera.
    [[nodiscard]] std::optional<presentation::camera::SourceTargetBounds> live_camera_bounds() const;
    // #888 perf trace: the last frame's main-thread ms in the snapshot build and renderer submit,
    // and the pieces it submitted; zero without the populated view.
    [[nodiscard]] double live_submit_ms() const;
    [[nodiscard]] std::size_t live_submit_pieces() const;
    // The pieces whose transform that submit sent to the engine (#888: only the moved ones).
    [[nodiscard]] std::uint64_t live_submit_sent() const;
    void live_camera_overview_key();
    [[nodiscard]] std::string live_camera_overview() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace eawr::presentation::godot_backend
