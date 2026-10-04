#include "space_environment.hpp"
#include "space_environment_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] bool orbit_focus_at_centre(const tactical::TacticalFrame& frame,
    const std::array<float, 3>& focus, const float radius) {
    const Vector3 eye(frame.eye[0], frame.eye[1], frame.eye[2]);
    const Vector3 target(frame.target[0], frame.target[1], frame.target[2]);
    const Vector3 point(focus[0], focus[1], focus[2]);
    const Vector3 up(frame.up[0], frame.up[1], frame.up[2]);
    Transform3D view;
    view.origin = eye;
    view = view.looking_at(target, up);
    const Vector3 local = view.affine_inverse().xform(point);
    return point.distance_to(target) < 0.01F && std::abs(local.x) < 0.01F
        && std::abs(local.y) < 0.01F && local.z < 0.0F
        && std::abs(eye.distance_to(point) - radius) < 0.01F;
}

// Synthetic self-test events enter Godot's own input pipeline, so they reach
// the camera through the host's real _input callback like a device event.
void inject_key(const Key code, const bool pressed) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(code);
    event->set_physical_keycode(code);
    event->set_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

// The Ctrl key's own events, as the platform sends them around a Ctrl gesture: the press and
// its auto-repeat carry the Ctrl bit, the release does not.
void inject_ctrl(const bool pressed, const bool echo = false) {
    Ref<InputEventKey> event;
    event.instantiate();
    event->set_keycode(KEY_CTRL);
    event->set_physical_keycode(KEY_CTRL);
    event->set_pressed(pressed);
    event->set_echo(echo);
    event->set_ctrl_pressed(pressed);
    Input::get_singleton()->parse_input_event(event);
}

void inject_button(const MouseButton button, const Vector2 position, const bool pressed = true,
                   const bool ctrl = false) {
    Ref<InputEventMouseButton> event;
    event.instantiate();
    event->set_button_index(button);
    event->set_pressed(pressed);
    event->set_factor(1.0F);
    event->set_position(position);
    event->set_ctrl_pressed(ctrl);
    Input::get_singleton()->parse_input_event(event);
}

void inject_motion(const Vector2 position, const Vector2 relative, const bool alt = false,
                   const bool ctrl = false) {
    Ref<InputEventMouseMotion> event;
    event.instantiate();
    event->set_position(position);
    event->set_relative(relative);
    event->set_alt_pressed(alt);
    event->set_ctrl_pressed(ctrl);
    Input::get_singleton()->parse_input_event(event);
}

} // namespace

bool SpaceEnvironment::State::activate_camera(Node3D& node) {
    const viewer::MapCameraSource& source = *options.map_camera;
    // --eawr-capture locks the bridge to the parsed fixed camera; otherwise
    // the authored initial pose in the current host viewport is the camera.
    const bool locked = !options.capture_path.empty();
    const Vector2 viewport = node.get_viewport()->get_visible_rect().size;
    const std::uint32_t width = locked ? camera.width : static_cast<std::uint32_t>(viewport.x);
    const std::uint32_t height = locked ? camera.height : static_cast<std::uint32_t>(viewport.y);
    auto* display = DisplayServer::get_singleton();
    const bool terminal_probe = options.camera_terminal_baseline_test
        || options.camera_terminal_hold_test || options.camera_terminal_release_test;
    auto created = std::make_unique<viewer::MapCameraBridge>(
        terminal_probe || (display && display->window_is_focused()), camera_input::Context::space);
    const std::optional<tactical::TacticalFrame> fixed = locked
        ? std::optional<tactical::TacticalFrame>(tactical_frame(camera)) : std::nullopt;
    if (auto active = created->activate(source.config, source.constants, source.bindings_json,
            width, height, fixed); !active) {
        return give_up(core::format_diagnostic(active.error()));
    }
    bridge = std::move(created);
    host = &node;
    if (locked || !(options.real_time_clock || options.camera_selftest)) {
        // Keep the render target at the capture identity while the real host
        // window is resized: by the locked graphical probe, or by a window
        // manager (FancyZones snaps a new window into its zone) under an
        // unlocked probe, which then never sees a resize (a terminal probe
        // ignores resizes anyway). Only an interactive run and an unlocked
        // self-test, whose subject is following real resizes, draw at the
        // window size.
        pin_capture_viewport(*node.get_window(), width, height);
    }
    if (!locked) {
        camera = fixed_camera(bridge->frame());
        camera_status = space::CameraStatus::valid;
        camera_source = "interactive space map camera (project-authored config, Space_Mode constants)";
    }
    camera_initial = camera;
    window_size = node.get_window()->get_size();
    node.set_process_input(true);
    node.set_process_unhandled_input(true);
    return true;
}

void SpaceEnvironment::State::record_submissions() {
    // Observed from the renderer's own ordered work list, not restated from
    // the plan.
    observed = renderer->submission_evidence();
    bool matched = true;
    std::vector<std::pair<sim::EntityId, sim::AssetId>> seen;
    for (const auto& item : observed) seen.emplace_back(item.entity_id, item.asset_id);
    for (const Uploaded& item : uploaded) {
        if (std::find(seen.begin(), seen.end(), std::make_pair(item.entity, item.asset)) == seen.end()) {
            matched = false;
            missing_submissions.push_back("entity " + std::to_string(item.entity) + " asset "
                + std::to_string(item.asset));
        }
    }
    // Every surface is in the opaque pass except a MeshAdditive one, which is
    // in the transparent pass; the occluder control is opaque.
    for (const auto& item : observed) {
        const auto sky = std::find_if(uploaded.begin(), uploaded.end(),
            [&](const Uploaded& entry) { return entry.asset == item.asset_id; });
        const bool additive_asset = sky != uploaded.end()
            && plan.surfaces[sky->surface].material == space::SkyMaterial::meshadditive;
        if (item.pass != (additive_asset ? RenderPass::transparent : RenderPass::opaque)) matched = false;
    }
    const std::size_t expected = uploaded.size() + (occluder_uploaded ? 1U : 0U);
    if (observed.size() != expected) matched = false;
    submission_status = matched ? "matched" : "mismatch";
}

void SpaceEnvironment::State::selftest_tick(const std::uint32_t tick) {
    if (!options.camera_selftest || !bridge || !host) return;
    viewer::MapCameraBridge& camera_bridge = *bridge;
    const bool locked = camera_bridge.controller().capture_locked();
    const Vector2 centre = host->get_viewport()->get_visible_rect().size * 0.5F;
    const auto& controller = camera_bridge.controller();
    const auto unchanged = [&] {
        return tactical_frame(fixed_camera(camera_bridge.frame())) == tactical_frame(camera_initial);
    };
    const auto check = [this](std::string name, const bool passed) { checks.emplace_back(std::move(name), passed); };
    const auto notify = [&](const int what) { host->get_tree()->get_root()->propagate_notification(what); };
    const auto remember = [&] {
        mark = camera_bridge.frame().target;
        zoom_mark = controller.state().zoom;
        target_zoom_mark = controller.target_zoom();
        yaw_mark = controller.yaw_degrees();
        pitch_mark = controller.state().pitch_degrees;
        const auto& eye = camera_bridge.frame().eye;
        orbit_radius_mark = std::hypot(eye[0] - mark[0], eye[1] - mark[1], eye[2] - mark[2]);
    };
    switch (tick) {
    case 1:
        notify(Node::NOTIFICATION_APPLICATION_FOCUS_IN);
        check("focus established before input", camera_bridge.adapter().focused() && focus_notifications > 0);
        inject_key(KEY_D, true);
        break;
    case 2:
        // A declared one-second step makes the clamp independent of how
        // quickly the graphical runner schedules frames.
        if (auto moved = camera_bridge.step(1.0F); !moved) failure = core::format_diagnostic(moved.error());
        break;
    case 3: inject_key(KEY_D, false); break;
    case 4:
        check("pan reaches authored X clamp", locked ? unchanged()
            : camera_bridge.frame().target[0] == controller.render_bounds().max_x);
        break;
    case 5:
        remember();
        inject_button(MOUSE_BUTTON_MIDDLE, centre);
        break;
    // A middle drag without Ctrl translates (FoC); left and down, away from the X clamp.
    case 6: inject_motion(centre, Vector2(-20.0F, 20.0F)); break;
    case 8:
        check("grabbed drag translates without turning", locked ? unchanged()
            : controller.yaw_degrees() == yaw_mark && controller.state().zoom == zoom_mark
                && camera_bridge.frame().target[0] < mark[0]
                && camera_bridge.frame().target[2] > mark[2]);
        inject_button(MOUSE_BUTTON_MIDDLE, centre, false);
        break;
    case 10:
        remember();
        distance_mark = controller.state().distance;
        inject_button(MOUSE_BUTTON_WHEEL_DOWN, centre);
        break;
    case 12:
        check("wheel changes only zoom", locked ? unchanged()
            : controller.state().zoom > zoom_mark && controller.yaw_degrees() == yaw_mark
                && camera_bridge.frame().target == mark);
        distance_step = controller.state().distance;
        if (const auto target = tactical::solve(camera_bridge.constants(), controller.target_zoom())) {
            check("wheel distance eases toward target", locked ? unchanged()
                : distance_step > distance_mark && distance_step < target.value().distance);
        } else check("wheel distance eases toward target", false);
        break;
    case 13:
        check("wheel distance continues on idle frames", locked ? unchanged()
            : controller.state().distance > distance_step);
        inject_motion(centre, Vector2(-20.0F, 0.0F), true);
        break;
    case 14:
        check("Alt mouse motion pans target", locked ? unchanged()
            : camera_bridge.frame().target != mark);
        remember();
        inject_key(KEY_A, true);
        break;
    case 15:
        pan_moved = camera_bridge.frame().target != mark;
        notify(Node::NOTIFICATION_APPLICATION_FOCUS_OUT);
        break;
    case 16: remember(); break;
    case 18: notify(Node::NOTIFICATION_APPLICATION_FOCUS_IN); break;
    case 20:
        // A is still physically down; the regained focus must not revive it.
        check("focus loss cancels held pan", locked ? (unchanged() && focus_notifications >= 3)
            : pan_moved && camera_bridge.frame().target == mark && camera_bridge.adapter().focused()
                && !camera_bridge.adapter().is_held(camera_input::Action::pan_left));
        inject_key(KEY_A, false);
        break;
    case 21:
        remember();
        inject_key(KEY_A, true);
        break;
    case 23:
        check("fresh press after focus regain pans", locked ? unchanged()
            : camera_bridge.frame().target != mark);
        inject_key(KEY_A, false);
        break;
    case 24:
        remember();
        inject_key(KEY_A, true);
        break;
    case 26:
        pan_moved = camera_bridge.frame().target != mark;
        window_size = host->get_window()->get_size();
        generation_mark = camera_bridge.adapter().counters().viewport_generation;
        host->get_window()->set_size(window_size - Vector2i(16, 16));
        break;
    case 28: remember(); break;
    case 31:
        check("real resize cancels held pan", locked ? (unchanged()
            && host->get_window()->get_size() != window_size
            && camera_bridge.adapter().counters().viewport_generation == generation_mark)
            : pan_moved && resize_notifications > 0
                && camera_bridge.adapter().counters().viewport_generation > generation_mark
                && camera_bridge.frame().target == mark);
        if (!locked) host->get_window()->set_size(window_size);
        inject_key(KEY_A, false);
        break;
    case 33:
        remember();
        inject_motion(Vector2(0.0F, centre.y), Vector2());
        break;
    case 34:
        edge_changed_only_target = camera_bridge.frame().target != mark
            && controller.target_zoom() == target_zoom_mark && controller.yaw_degrees() == yaw_mark;
        notify(Node::NOTIFICATION_WM_MOUSE_EXIT);
        remember();
        break;
    case 35:
        check("edge pan changes only target", locked ? unchanged() : edge_changed_only_target);
        check("pointer exit cancels edge pan", locked ? (unchanged() && !camera_bridge.adapter().pointer_valid())
            : !camera_bridge.adapter().pointer_valid() && camera_bridge.frame().target == mark);
        break;
    case 37: inject_key(KEY_HOME, true); break;
    case 40:
        check("reset restores authored pose", locked ? unchanged()
            : camera_bridge.resets() == 1U
                && camera_bridge.frame().target[0] == camera_bridge.config().target_x
                && camera_bridge.frame().target[1] == camera_bridge.config().target_height
                && camera_bridge.frame().target[2] == -camera_bridge.config().target_y
                && controller.yaw_degrees() == camera_bridge.config().yaw_degrees
                && controller.state().zoom == camera_bridge.config().zoom);
        inject_key(KEY_HOME, false);
        break;
    case 42:
        check("input callbacks reached space adapter", camera_bridge.input_callbacks() >= 8U);
        check("capture lock isolates input", !locked
            || (unchanged() && camera_bridge.steps() == 0U
                && camera_bridge.adapter().counters().ignored_ineligible > 0));
        break;
    case 44:
        remember();
        inject_ctrl(true);
        inject_ctrl(true, true);
        inject_button(MOUSE_BUTTON_MIDDLE, centre, true, true);
        break;
    // Ctrl + middle-drag right and up by 40% of the height: 40 mouse units at
    // the fixture's Pitch_Per_Mouse_Unit -1.25 tilt 50 degrees toward the
    // horizon, so from the 45-degree default the orbit passes under the
    // battle plane, above the XML Pitch_Min of -20.
    case 45:
        inject_motion(centre, Vector2(8.0F, -0.8F * centre.y), false, true);
        inject_ctrl(true, true);
        break;
    case 47:
        check("Ctrl grabbed drag orbits under the plane", locked ? unchanged()
            : controller.yaw_degrees() < yaw_mark && controller.state().pitch_degrees < 0.0F
                && controller.state().pitch_degrees < pitch_mark
                && controller.state().pitch_degrees >= camera_bridge.orbit_pitch_range().min_degrees
                && camera_bridge.frame().eye[1] < camera_bridge.frame().target[1]
                && controller.state().zoom == zoom_mark
                && orbit_focus_at_centre(camera_bridge.frame(), mark, orbit_radius_mark));
        inject_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_ctrl(false);
        break;
    // A Ctrl click does not reset; a plain middle click resets the view in place.
    case 49:
        inject_ctrl(true);
        inject_button(MOUSE_BUTTON_MIDDLE, centre, true, true);
        break;
    case 50:
        inject_button(MOUSE_BUTTON_MIDDLE, centre, false, true);
        inject_ctrl(false);
        break;
    case 52:
        check("Ctrl click keeps the view", locked ? unchanged()
            : camera_bridge.view_resets() == 0U && controller.orbit_pitch_offset() != 0.0F);
        remember();
        inject_button(MOUSE_BUTTON_MIDDLE, centre);
        break;
    case 53: inject_button(MOUSE_BUTTON_MIDDLE, centre, false); break;
    case 55:
        check("middle click resets the view around the target", locked ? unchanged()
            : camera_bridge.view_resets() == 1U
                && controller.yaw_degrees() == camera_bridge.config().yaw_degrees
                && controller.state().zoom == camera_bridge.config().zoom
                && controller.orbit_pitch_offset() == 0.0F
                && camera_bridge.frame().target[0] == mark[0]
                && camera_bridge.frame().target[2] == mark[2]);
        break;
    default: break;
    }
}

std::optional<int> SpaceEnvironment::State::interactive_process(const double delta) {
    const std::uint32_t terminal = options.warmup_frames + options.timed_frames;
    if (compare_started) {
        // A frozen-pose drawn check, not sky fidelity: the same pose with the
        // sky submission removed must differ from the interactive capture.
        renderer->submit(disabled_snapshot);
        if (++compare_frames < 4) return std::nullopt;
        auto disabled_capture = renderer->capture(camera);
        if (!disabled_capture) return fail(core::format_diagnostic(disabled_capture.error()));
        capture_hashes["sky_disabled"] = hash_bytes(disabled_capture.value().png_bytes);
        const auto drawn = rgb_of(interactive_capture);
        const auto empty = rgb_of(disabled_capture.value().png_bytes);
        if (!drawn || !empty || drawn->width != empty->width || drawn->height != empty->height) {
            drawn_status = "failed";
            return fail("interactive capture or its sky-disabled control could not be decoded");
        }
        for (std::size_t offset = 0; offset + 2 < drawn->rgb.size(); offset += 3) {
            ++drawn_sampled_pixels;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int difference = static_cast<int>(drawn->rgb[offset + channel])
                    - static_cast<int>(empty->rgb[offset + channel]);
                if (std::abs(difference) > static_cast<int>(space::changed_threshold)) {
                    ++drawn_changed_pixels;
                    break;
                }
            }
        }
        return finish_interactive();
    }
    if (!settle_started) {
        if (!std::isfinite(delta) || delta < 0.0 || delta > static_cast<double>(std::numeric_limits<float>::max())) {
            return fail("space camera process delta is invalid");
        }
        const std::uint32_t tick = frame + 1U;
        const bool terminal_probe = options.camera_terminal_baseline_test
            || options.camera_terminal_hold_test || options.camera_terminal_release_test;
        const bool terminal_pan = options.camera_terminal_hold_test || options.camera_terminal_release_test;
        if (terminal_pan && tick == terminal) {
            if (!bridge->adapter().is_held(camera_input::Action::pan_right)) {
                return fail("terminal pan input did not reach the space adapter before the terminal step");
            }
            terminal_held_at_step = true;
        }
        const float seconds = terminal_probe
            ? (tick == terminal ? terminal_step_seconds : 1.0F / 60.0F) : static_cast<float>(delta);
        if (auto stepped = bridge->step(seconds); !stepped) return fail(core::format_diagnostic(stepped.error()));
        selftest_tick(tick);
        if (terminal_pan) {
            // Route the declared key edges through the adapter at fixed frame
            // points. Godot's event queue can lag a process callback under load.
            if (tick == terminal - 1U || (tick == terminal && options.camera_terminal_release_test)) {
                const bool pressed = tick == terminal - 1U;
                if (auto handled = bridge->handle(camera_input::RawEvent{
                        .kind = camera_input::RawKind::key,
                        .code = *camera_input::key_code("D"),
                        .pressed = pressed}); !handled) {
                    return fail(core::format_diagnostic(handled.error()));
                }
            }
        }
        if (!failure.empty()) return fail(failure);
        camera = fixed_camera(bridge->frame());
        renderer->set_camera(camera);
    }
    ++frame;
    renderer->submit(configured_snapshot);
    if (frame == 1) record_submissions();
    if (frame == options.warmup_frames) {
        timing_start = std::chrono::steady_clock::now();
        return std::nullopt;
    }
    if (frame < terminal) return std::nullopt;
    if (!settle_started) {
        // Advancement stops at the declared terminal frame; the pose is
        // frozen through settling and readback.
        timed_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - timing_start).count();
        settle_started = true;
        steps_at_freeze = bridge->steps();
        return std::nullopt;
    }
    if (++settle_frames < settle_frame_count) return std::nullopt;
    if (bridge->steps() != steps_at_freeze
        || tactical_frame(fixed_camera(bridge->frame())) != tactical_frame(camera)) {
        return fail("space camera changed during terminal settle");
    }
    auto capture = renderer->capture(camera);
    if (!capture || capture.value().png_bytes.empty()) {
        return fail(capture ? std::string("capture is empty") : core::format_diagnostic(capture.error()));
    }
    if (capture.value().width != camera.width || capture.value().height != camera.height) {
        return fail("space camera capture dimensions differ from camera identity");
    }
    if (!rgb_of(capture.value().png_bytes)) return fail("interactive space capture PNG could not be decoded");
    capture_hashes["configured"] = hash_bytes(capture.value().png_bytes);
    if (!options.unlocked_capture_path.empty()
        && !persist(options.unlocked_capture_path, capture.value().png_bytes)) {
        return fail("requested artifact was not written: " + artifacts_failed.back());
    }
    interactive_capture = std::move(capture.value().png_bytes);
    compare_started = true;
    return std::nullopt;
}

std::optional<int> SpaceEnvironment::State::finish_interactive() {
    completed = true;
    release_all();
    // Sky fidelity is a fixed-camera claim: its masks and comparison phases
    // are never computed for an interactive pose, so none can be inherited.
    pixel_status = "not_evaluated_interactive";
    drawn_status = drawn_changed_pixels >= 64U ? "verified" : "failed";
    const bool checks_ok = selftest_passed();
    const bool passed = load_status == "loaded" && material_status == "compiled"
        && submission_status == "matched" && drawn_status == "verified" && checks_ok
        && failure.empty() && artifacts_failed.empty()
        && teardown_remaining == std::optional<std::size_t>(0);
    if (!passed && failure.empty()) {
        if (!artifacts_failed.empty()) failure = "requested artifact was not written: " + artifacts_failed.front();
        else if (submission_status != "matched") failure = "observed submissions disagree with the plan";
        else if (drawn_status != "verified") failure = "the frozen interactive pose drew no measurable sky";
        else if (!checks_ok) failure = "space camera graphical selftest failed";
        else failure = "renderer resources remained after teardown";
    }
    status = passed ? (options.camera_selftest ? "space_camera_selftest_passed" : "space_camera_render_passed")
                    : "failed";
    if (!write_report()) return 2;
    return passed ? 0 : 2;
}

std::optional<int> SpaceEnvironment::State::finish() {
    completed = true;
    const auto configured = captures.find("configured");
    const auto disabled = captures.find("sky_disabled");
    std::optional<space::Rgb8Image> configured_rgb;
    std::optional<space::Rgb8Image> disabled_rgb;
    if (configured != captures.end()) configured_rgb = rgb_of(configured->second);
    if (disabled != captures.end()) disabled_rgb = rgb_of(disabled->second);
    std::vector<space::SurfaceRegions> regions;
    std::vector<std::optional<space::Rgb8Image>> isolated;
    for (std::size_t index = 0; index < uploaded.size(); ++index) {
        const space::SurfacePlan& surface = plan.surfaces[uploaded[index].surface];
        const assets::Submesh& submesh = surface.model->meshes.front().submeshes.front();
        space::SurfaceRegions region;
        region.mask = space::rasterize(camera, space::render_triangles(submesh));
        const auto quadrants = space::render_triangles_by_uv_quadrant(submesh);
        for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
            region.quadrants[quadrant] = space::rasterize(camera, quadrants[quadrant]);
        }
        if (!options.capture_path.empty()) {
            // Predeclared regions leave the run as plain PGM masks (dilated by
            // the evaluation margin) so an external control can attribute a
            // change to one surface without trusting this process.
            const space::ScreenMask grown = space::dilate(region.mask, space::mask_margin);
            std::string header = "P5\n" + std::to_string(grown.width) + " " + std::to_string(grown.height) + "\n255\n";
            std::vector<std::byte> pgm;
            for (const char character : header) pgm.push_back(static_cast<std::byte>(character));
            for (const std::uint8_t bit : grown.bits) pgm.push_back(bit != 0U ? std::byte{255} : std::byte{0});
            // A failure is recorded and fails the run below, after release.
            static_cast<void>(persist(sibling(options.capture_path, ".mask-surface-" + std::to_string(index) + ".pgm"), pgm));
        }
        regions.push_back(std::move(region));
        std::optional<space::Rgb8Image> alone;
        if (index < isolated_captures.size() && isolated_captures[index]) alone = rgb_of(*isolated_captures[index]);
        isolated.push_back(std::move(alone));
    }
    if (!configured_rgb || !disabled_rgb) {
        pixel_status = "failed";
        pixels.failure = "configured or sky-disabled capture could not be decoded";
    } else {
        pixels = space::evaluate_pixels(regions, *configured_rgb, *disabled_rgb, isolated, occluder_mask);
        pixel_status = pixels.status;
    }
    if (shadow_control(control)) {
        // The receiver is the foreground control region: any sky-caused
        // change there is a shadow, since the unshaded sky emits no light.
        if (pixels.occlusion_status == "verified") lit_foreground = "unchanged";
        else if (pixels.occlusion_status == "failed") lit_foreground = "shadowed_by_sky";
        else lit_foreground = pixels.occlusion_status;
    }
    if (options.fog) evaluate_fog();
    release_all();

    const bool camera_ok = !bridge || (selftest_passed()
        && (!options.camera_selftest || resize_active_at_capture));
    const bool passed = load_status == "loaded" && material_status == "compiled"
        && submission_status == "matched" && pixel_status == "verified"
        && artifacts_failed.empty() && camera_ok
        && teardown_remaining == std::optional<std::size_t>(0)
        && (!options.fog || fog_status == "verified");
    if (!passed && failure.empty()) {
        if (!artifacts_failed.empty()) failure = "requested artifact was not written: " + artifacts_failed.front();
        else if (load_status == "blocked") {
            failure = "space primary sky plan is " + std::string(space::to_string(plan.status))
                + (plan.detail.empty() ? "" : ": " + plan.detail);
        } else if (options.fog && fog_status != "verified" && pixel_status == "verified") failure = fog_failure;
        else if (!camera_ok) failure = "space camera graphical selftest failed";
        else if (lit_foreground == "shadowed_by_sky") {
            failure = std::string("the sky shadowed the lit foreground receiver (sky casting ")
                + (sky_casts ? "enabled by a labelled control)" : "disabled)");
        } else if (submission_status != "matched") failure = "observed submissions disagree with the plan";
        else if (pixel_status != "verified") failure = pixels.failure.empty() ? "pixel evidence " + pixel_status : pixels.failure;
        else failure = "renderer resources remained after teardown";
    }
    status = passed ? (options.fog ? "space_fog_passed" : "space_primary_sky_passed") : "failed";
    if (!write_report()) return 2;
    return passed ? 0 : 2;
}


} // namespace eawr::presentation::godot_backend
