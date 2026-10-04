#include "viewer_host_internal.hpp"

#include "ui/input_routing.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>

namespace eawr::presentation::godot_backend {

bool ViewerHost::step_camera_interaction(const double delta) {
    CameraInteraction& run = *camera_interaction_;
    if (run.selftest && !advance_camera_selftest()) return false;
    auto intent = run.adapter.take_step(tactical_camera_->constants);
    if (!intent) {
        status_message_ = core::format_diagnostic(intent.error());
        return false;
    }
    // #22 precedence: nothing about the camera changes while locked, and no
    // free-flight toggle is honoured.
    if (run.adapter.capture_locked()) return true;
    // A step that carries a toggle performs only the transition; the rest of
    // its intent belonged to the context being left.
    if (intent.value().free_toggle_requests > 0U) return toggle_free_camera();
    // The self-test uses a fixed step so its assertions do not depend on the
    // render cadence; interactive use takes the frame delta.
    const float seconds = run.selftest ? 1.0F / 60.0F : static_cast<float>(delta);
    if (run.free_controller) return step_free_camera(intent.value(), seconds);
    auto next = camera_input::advance_pose(
        tactical_camera_->constants, run.pose, intent.value(), run.default_zoom, seconds);
    if (!next) {
        status_message_ = core::format_diagnostic(next.error());
        return false;
    }
    ++run.steps;
    if (next.value() == run.pose) return true;
    ++run.moving_steps;
    run.pose = next.value();
    auto eye = tactical_camera::eye_position(std::span<const float, 3>{run.pose.target},
        run.pose.state.distance, run.pose.state.pitch_degrees, run.pose.state.yaw_degrees);
    if (!eye) {
        status_message_ = core::format_diagnostic(eye.error());
        return false;
    }
    run.camera.eye = eye.value();
    run.camera.target = run.pose.target;
    // #515: the XML angle is FoC's (horizontal on 4:3); the renderer takes the vertical one.
    auto vertical = tactical_camera::vertical_fov_degrees(run.pose.state.fov_degrees);
    if (!vertical) {
        status_message_ = core::format_diagnostic(vertical.error());
        return false;
    }
    run.camera.vertical_fov_degrees = vertical.value();
    renderer_->set_camera(run.camera);
    return true;
}

// Enters free flight from the current tactical camera, or leaves it.
//
// Entry saves the whole tactical state (pose, render camera, context), builds
// the controller from the current eye/yaw/pitch and switches the adapter to
// the `free` context, which cancels every held control, pending delta and
// pointer sample. The render camera is not rewritten on entry, so the first
// free frame is pixel-identical to the last tactical frame. A pose the
// controller refuses (for example a pitch outside the project bounds) leaves
// the tactical camera active and is reported, never clamped.
//
// Exit restores the saved state exactly and switches back to the saved
// context, which cancels again. No map bound is applied: typed #26 bounds are
// not available to this host.
bool ViewerHost::toggle_free_camera() {
    CameraInteraction& run = *camera_interaction_;
    if (run.free_controller) {
        run.free_controller.reset();
        run.pose = run.saved_pose;
        run.camera = run.saved_camera;
        run.adapter.set_context(run.saved_context);
        renderer_->set_camera(run.camera);
        ++run.free_exits;
        run.free_transitions.emplace_back("exit", frame_);
        return true;
    }
    const camera_input::BindingTable* table = run.adapter.table();
    if (!table || !table->free_camera) {
        // Unreachable with a validated table: only v2 binds free_toggle, and
        // v2 requires settings. Kept so a toggle can never enter unconfigured.
        ++run.free_rejections;
        run.free_rejection = "the active binding table has no free_camera settings";
        run.free_transitions.emplace_back("rejected", frame_);
        return true;
    }
    auto created = tactical_camera::FreeCameraController::create(*table->free_camera,
        std::span<const float, 3>{run.camera.eye}, run.pose.state.yaw_degrees,
        run.pose.state.pitch_degrees);
    if (!created) {
        ++run.free_rejections;
        run.free_rejection = core::format_diagnostic(created.error());
        run.free_transitions.emplace_back("rejected", frame_);
        return true;
    }
    run.saved_pose = run.pose;
    run.saved_camera = run.camera;
    run.saved_context = run.adapter.context();
    run.free_controller.emplace(std::move(created).value());
    run.free_pose = run.free_controller->pose();
    run.adapter.set_context(camera_input::Context::free);
    ++run.free_entries;
    run.free_transitions.emplace_back("enter", frame_);
    return true;
}

// One free-flight step. The render camera keeps the entry FOV, clip planes
// and world up; its look-at point sits along the view direction at the saved
// tactical distance, which only orients the camera.
bool ViewerHost::step_free_camera(const camera_input::StepIntent& intent, const float seconds) {
    CameraInteraction& run = *camera_interaction_;
    const tactical_camera::FreeCameraPose before = run.free_controller->pose();
    const tactical_camera::FreeCameraIntent free_intent{intent.free_move_x, intent.free_move_z,
        intent.free_move_y, intent.free_look_yaw_units, intent.free_look_pitch_units};
    if (auto advanced = run.free_controller->advance(free_intent, seconds); !advanced) {
        status_message_ = core::format_diagnostic(advanced.error());
        return false;
    }
    ++run.free_steps;
    const tactical_camera::FreeCameraPose& pose = run.free_controller->pose();
    run.free_pose = pose;
    if (pose == before) return true;
    ++run.free_moving_steps;
    auto forward = tactical_camera::free_camera_forward(pose);
    if (!forward) {
        status_message_ = core::format_diagnostic(forward.error());
        return false;
    }
    const float reach = std::max(run.saved_pose.state.distance, 1.0F);
    run.camera.eye = pose.eye;
    run.camera.target = {pose.eye[0] + forward.value()[0] * reach,
        pose.eye[1] + forward.value()[1] * reach, pose.eye[2] + forward.value()[2] * reach};
    renderer_->set_camera(run.camera);
    return true;
}

// Scripted synthetic-input run. Key/mouse events go through the engine's
// Input::parse_input_event dispatch; focus changes are propagated from the
// scene root as the engine does; zero and restored extents go through the same
// publish function as the size_changed signal, and one real window resize goes
// through the signal itself. Each check compares exact state.

} // namespace eawr::presentation::godot_backend
