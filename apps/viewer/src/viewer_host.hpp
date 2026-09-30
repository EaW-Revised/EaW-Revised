#pragma once

#include "camera_input.hpp"
#include "effect_mode.hpp"
#include "font_mode.hpp"
#include "input_routing_mode.hpp"
#include "map_mode.hpp"
#include "ui_gallery_mode.hpp"
#include "unit_mode.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/camera/camera.hpp"
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/ui/input_routing.hpp"
#include "eawr/sim/snapshot.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/rid.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

class ViewerHost final : public godot::Node3D {
    GDCLASS(ViewerHost, godot::Node3D)

public:
    ViewerHost();
    ~ViewerHost() override;

    void _ready() override;
    void _process(double delta) override;
    // Engine input and lifecycle callbacks (UI-07 #304, docs/ui/ui-layer.md
    // §3.3). The GUI sees every event before the world: _input only takes the
    // pointer while the world holds a button (a drag keeps its motion and
    // release over the HUD), and _unhandled_input hands what the GUI left to
    // the world through the routing policy (modal dialogs and a focused edit
    // box block it). The world routes only to the explicit opt-in tactical
    // interaction (`--eawr-camera-interactive`), a map mode's camera or the
    // routing self-test; every other mode, including the frozen #22 fixed
    // capture, drops them unread.
    void _input(const godot::Ref<godot::InputEvent>& event) override;
    void _unhandled_input(const godot::Ref<godot::InputEvent>& event) override;
    void _notification(int what);

protected:
    static void _bind_methods();

private:
    struct Options;
    // Presentation-only screen-space atlas overlay. It owns its own canvas
    // RIDs and never contributes to the 3D scenario or the material routes.
    struct AtlasOverlay;
    // Presentation-only tactical camera exercise. It resolves the XML-sourced
    // camera model at one zoom and applies it to this run's capture camera. It
    // deliberately never touches the frozen P1-01 fixed-capture framing.
    struct TacticalCameraRun;
    // Opt-in P1-09 interaction on top of the tactical camera run. It owns a
    // separate interactive camera and never writes capture_camera_. It also
    // hosts the project-authored free-flight camera, entered by toggle.
    struct CameraInteraction;
    // Opt-in exploratory MC-50 Hull preview only: selection identity,
    // bounds-fitted framing and capture evidence.
    struct ModelPreview;
    bool load_scene();
    bool load_replay();
    bool start_renderer_runtime_exercise();
    bool start_atlas_overlay();
    bool start_tactical_camera();
    bool start_camera_interaction();
    void publish_camera_viewport(float width, float height);
    void on_viewport_size_changed();
    void on_map_viewport_size_changed();
    // The world layer, then the camera: #82's selection and orders go before
    // the camera path here.
    void route_world_input(const godot::Ref<godot::InputEvent>& event);
    // A text control took keyboard focus: held camera keys are cancelled, as
    // on a window focus loss, so none keeps moving while the user types.
    void on_gui_focus_changed(godot::Control* control);
    // A modal dialog opened or closed since the last call: the world's pointer capture is
    // released and its held input cancelled (ModalHoldGuard). Runs before every event and
    // every frame, so a hold never outlives the edge by an event or a frame.
    void sync_modal_holds();
    void cancel_held_world_input();
    [[nodiscard]] bool step_camera_interaction(double delta);
    // Free flight inside the opt-in interaction (schema v2 `free` context).
    [[nodiscard]] bool toggle_free_camera();
    [[nodiscard]] bool step_free_camera(
        const eawr::viewer::camera_input::StepIntent& intent, float seconds);
    [[nodiscard]] bool advance_camera_selftest();
    [[nodiscard]] bool verify_atlas_capture(const CaptureResult& capture);
    void release_atlas_overlay();
    bool apply_animation_pose(const std::shared_ptr<const sim::RenderSnapshot>& snapshot);
    [[nodiscard]] bool verify_runtime_capture(const CaptureResult& capture);
    [[nodiscard]] bool verify_draw_capture(const CaptureResult& capture);
    [[nodiscard]] bool verify_preview_capture(const CaptureResult& capture);
    [[nodiscard]] bool write_report(std::string_view status);
    void stop(int exit_code);

    std::unique_ptr<Options> options_;
    std::string active_content_profile_{"remake"};
    // `--eawr-map` is a self-contained mode that owns its own renderer and
    // frame loop; this host only parses the option and hands over.
    std::unique_ptr<MapMode> map_mode_;
    // `--eawr-effect` is likewise self-contained and parses its own options.
    std::unique_ptr<EffectMode> effect_mode_;
    // `--eawr-unit` (P1-03 whole-unit animation strips) is self-contained too.
    std::unique_ptr<UnitMode> unit_mode_;
    // `--eawr-fonts` (UI-05 font cache and UI-F3 samples) is self-contained too.
    std::unique_ptr<FontMode> font_mode_;
    // `--eawr-ui-gallery` (UI-06 kit controls and catalogue dialogs) is self-contained too.
    std::unique_ptr<UiGalleryMode> ui_gallery_mode_;
    // `--eawr-input-routing` (UI-07 routing self-test) is self-contained too.
    std::unique_ptr<InputRoutingMode> input_routing_mode_;
    // Mouse buttons the world pressed and has not released yet.
    presentation::ui::WorldPointerCapture world_capture_;
    presentation::ui::ModalHoldGuard modal_holds_;
    std::unique_ptr<AtlasOverlay> atlas_overlay_;
    std::unique_ptr<TacticalCameraRun> tactical_camera_;
    std::unique_ptr<CameraInteraction> camera_interaction_;
    std::unique_ptr<ModelPreview> model_preview_;
    std::unique_ptr<GodotRenderer> renderer_;
    assets::Model model_;
    assets::Animation animation_;
    std::optional<animation::Player> animation_player_;
    std::optional<animation::Pose> animation_pose_;
    assets::Texture texture_;
    MaterialDescription material_;
    FixedCamera capture_camera_;
    std::vector<std::shared_ptr<const sim::RenderSnapshot>> snapshots_;
    std::shared_ptr<const sim::RenderSnapshot> fixed_capture_snapshot_;
    std::vector<std::shared_ptr<const sim::RenderSnapshot>> runtime_lifecycle_snapshots_;
    std::string scene_hash_;
    std::string replay_hash_;
    std::string capture_hash_;
    std::string selected_model_path_;
    std::string selected_model_hash_;
    std::string selected_texture_path_;
    std::string selected_texture_hash_;
    std::string selected_animation_path_;
    std::string selected_animation_hash_;
    std::string selected_program_;
    float animation_time_seconds_{};
    std::string status_message_;
    std::uint64_t frame_{};
    std::uint64_t runtime_scene_switches_{};
    bool completed_{};
    bool runtime_exercise_{};
    bool runtime_capture_verified_{};
    bool runtime_overlap_verified_{};
    bool runtime_post_dependency_verified_{};
    bool runtime_failed_upload_clean_{};
    bool runtime_shutdown_resources_empty_{};
    bool animation_capture_verified_{};
    bool atlas_overlay_verified_{};
    bool tactical_camera_verified_{};
    bool skin_palette_bound_{};
};

} // namespace eawr::presentation::godot_backend
