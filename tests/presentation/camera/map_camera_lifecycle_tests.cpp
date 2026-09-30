#include "map_camera_test_support.hpp"

namespace eawr_map_camera_test {

void test_space_activation_and_provenance() {
    const auto loaded = load(camera::Mode::space);
    expect(loaded.has_value(), "space constants load through load_constants(Mode::space)");
    if (!loaded) return;
    bool space_definition = false;
    bool land_definition = false;
    for (const auto& field : loaded->provenance) {
        if (field.definition == "Space_Mode") space_definition = true;
        if (field.definition == "Land_Mode") land_definition = true;
    }
    expect(space_definition && !land_definition, "space provenance names only Space_Mode");
    close(loaded->constants.distance_min, 400.0F, "space distance range comes from Space_Mode");
    expect(!loaded->constants.use_splines, "space fixture is linear");

    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    const auto& bridge = space.bridge;
    expect(bridge.active() && bridge.context() == input::Context::space
        && bridge.adapter().context() == input::Context::space, "space bridge context");
    expect(bridge.config().mode == camera::Mode::space && bridge.config().version == 1U,
           "space config mode and version");
    expect(bridge.config().bounds.authority == "project-authored"
        && bridge.config().bounds.source_id == "synthetic-space-target-rectangle-v1"
        && bridge.config().bounds.logical_path == space_map, "bounds authority and identity");
    expect(bridge.controller().render_bounds() == camera::RenderTargetBounds{-400.0F, 400.0F, -800.0F, 0.0F},
           "source Y is inverted exactly once into render Z");
    close(bridge.frame().target[0], 160.0F, "initial target x");
    close(bridge.frame().target[1], 60.0F, "initial target height");
    close(bridge.frame().target[2], -400.0F, "initial target render z");
    close(bridge.controller().state().zoom, 0.5F, "initial zoom");
    close(bridge.controller().state().distance, 800.0F, "initial distance is the linear midpoint");
    close(bridge.controller().state().pitch_degrees, 45.0F, "space pitch from Space_Mode");
    expect(!bridge.free_settings() && !bridge.free_active(), "space has no free flight");
}

void test_incompatible_configs_and_bindings_reject() {
    const std::string space_xml = read("space-map-camera.xml");
    const std::string land_xml = read("map-camera.xml");
    const auto space_constants = load(camera::Mode::space);
    const auto land_constants = load(camera::Mode::land);

    // Mode-specific schemas.
    expect(!eawr::viewer::parse_map_camera_config(space_xml, space_map, space_sha, camera::Mode::land),
           "space config does not parse as land");
    expect(!eawr::viewer::parse_map_camera_config(land_xml, land_map, land_sha, camera::Mode::space),
           "land config does not parse as space");
    expect(!eawr::viewer::parse_map_camera_config(space_xml, space_map, space_sha, camera::Mode::unlocked),
           "unlocked has no map camera config");
    expect(!space_config(replaced(space_xml, "version=\"1\"", "version=\"2\"")),
           "space config has no version 2");
    // Map identity and bounds validate before activation.
    expect(!eawr::viewer::parse_map_camera_config(space_xml, space_map,
        "45377e070c521b37edd679fd1a092f9b77028914531eff1bcbe330dc1ff87202", camera::Mode::space),
           "wrong map hash rejects");
    expect(!eawr::viewer::parse_map_camera_config(space_xml, "data/art/maps/other.ted", space_sha,
        camera::Mode::space), "wrong map path rejects");
    expect(!space_config(replaced(space_xml, "max_x=\"400\"", "max_x=\"inf\"")), "infinite bound rejects");
    expect(!space_config(replaced(space_xml, "min_y=\"0\"", "min_y=\"nan\"")), "NaN bound rejects");
    expect(!space_config(replaced(space_xml, "authority=\"project-authored\"", "authority=\"ted-volume\"")),
           "non-project bounds authority rejects");
    expect(!space_config(replaced(space_xml, "zoom=\"0.5\"", "zoom=\"1.5\"")), "zoom outside [0,1] rejects");
    // Missing Space_Mode cannot fall back to Land_Mode.
    const std::string land_only = std::string(
        tactical_xml.substr(0, tactical_xml.find("  <TacticalCamera Name=\"Space_Mode\">")))
        + "</TacticalCameras>\n";
    expect(!load(camera::Mode::space, land_only), "space constants never fall back to Land_Mode");

    const auto config = space_config(space_xml);
    if (!config || !space_constants || !land_constants) return;
    const auto rejects = [&](const std::string& bindings, const std::string_view needle, const std::string_view name) {
        MapCameraBridge bridge(true, input::Context::space);
        auto result = bridge.activate(*config, space_constants->constants, bindings, 1280, 720, std::nullopt);
        expect(!result && result.error().message.find(needle) != std::string::npos, name);
        expect(!bridge.active() && !bridge.adapter().has_table(), "rejected activation leaves bridge inactive");
    };
    rejects(read("map-camera-bindings.json"), "incompatible land bindings", "land bindings reject in space");
    rejects(read("camera-bindings.json"), "unsupported free-camera bindings", "free bindings reject in space");
    rejects(read("map-camera-free-bindings.json"), "unsupported free-camera bindings",
            "land/free v2 bindings reject in space");
    {
        MapCameraBridge land(true, input::Context::land);
        expect(!land.activate(*config, space_constants->constants, read("space-map-camera-bindings.json"),
            1280, 720, std::nullopt), "a land bridge refuses a space config");
        auto land_config = eawr::viewer::parse_map_camera_config(land_xml, land_map, land_sha);
        expect(land_config.has_value(), "land config parses with the default mode");
        if (land_config) {
            MapCameraBridge space(true, input::Context::space);
            expect(!space.activate(land_config.value(), land_constants->constants,
                read("map-camera-bindings.json"), 1280, 720, std::nullopt), "a space bridge refuses a land config");
            auto rejected = land.activate(land_config.value(), land_constants->constants,
                read("space-map-camera-bindings.json"), 1280, 720, std::nullopt);
            expect(!rejected && rejected.error().message.find("space bindings") != std::string::npos,
                   "land keeps rejecting space bindings");
            expect(land.activate(land_config.value(), land_constants->constants,
                read("map-camera-bindings.json"), 1280, 720, std::nullopt).has_value(),
                   "land activation is preserved");
            expect(land.context() == input::Context::land && land.adapter().context() == input::Context::land,
                   "land context is preserved");
        }
    }
    // Inverted bounds parse but cannot create a controller; nothing activates.
    const auto inverted = space_config(replaced(space_xml, "min_x=\"-400\" max_x=\"400\"",
                                                "min_x=\"400\" max_x=\"-400\""));
    expect(inverted.has_value(), "inverted bounds are a controller rejection, not a parse error");
    if (inverted) {
        MapCameraBridge bridge(true, input::Context::space);
        expect(!bridge.activate(*inverted, space_constants->constants, read("space-map-camera-bindings.json"),
            1280, 720, std::nullopt), "inverted bounds reject before activation");
        expect(!bridge.active() && !bridge.adapter().has_table(), "inverted bounds leave the bridge inactive");
        expect(!bridge.step(0.1F), "an inactive bridge refuses to step");
        expect(!bridge.set_viewport(800.0F, 600.0F), "an inactive bridge refuses a viewport");
    }
}

void test_lifecycle_clears_stale_input() {
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& bridge = space.bridge;
    const auto moved = [&](const float seconds = 0.05F) {
        const auto before = bridge.frame();
        expect(bridge.step(seconds).has_value(), "lifecycle step succeeds");
        return !(bridge.frame() == before);
    };

    // Focus loss; regain needs a fresh press.
    expect(handled(bridge, key("D", true)) && moved(), "held pan moves before focus loss");
    bridge.set_focus(false);
    expect(!moved(), "focus loss clears held pan");
    bridge.set_focus(true);
    expect(!moved(), "focus regain does not resurrect held pan");
    expect(handled(bridge, key("D", false)) && handled(bridge, key("D", true)) && moved(),
           "fresh press after focus regain moves");

    // Pending wheel is cleared by focus loss too.
    expect(handled(bridge, wheel(true)), "pending wheel routes");
    bridge.set_focus(false);
    bridge.set_focus(true);
    const float zoom = bridge.controller().state().zoom;
    expect(bridge.step(0.0F).has_value() && bridge.controller().state().zoom == zoom,
           "focus loss clears pending wheel");

    // Resize and minimize.
    expect(handled(bridge, key("D", false)) && handled(bridge, key("D", true)) && moved(), "pan before resize");
    const auto generation = bridge.adapter().counters().viewport_generation;
    expect(bridge.set_viewport(1000.0F, 600.0F).has_value(), "resize accepted");
    expect(bridge.adapter().counters().viewport_generation > generation, "resize publishes a generation");
    expect(bridge.frame().width == 1000U && bridge.frame().height == 600U, "resize reaches the frame");
    expect(!moved(), "resize clears held pan");
    expect(handled(bridge, key("D", false)) && handled(bridge, key("D", true)) && moved(), "pan before minimize");
    expect(bridge.set_viewport(0.0F, 0.0F).has_value(), "minimize accepted");
    expect(!moved(), "minimize suspends and clears held pan");
    expect(bridge.set_viewport(1280.0F, 720.0F).has_value(), "restore accepted");
    expect(!moved(), "restore does not resurrect held pan");
    expect(handled(bridge, key("D", false)), "release");

    // Pointer exit clears edge scrolling.
    expect(handled(bridge, motion(0.0F, 360.0F, 0.0F)) && moved(), "left edge scroll moves");
    bridge.pointer_left();
    expect(!moved(), "pointer exit clears edge scroll");
    expect(handled(bridge, motion(640.0F, 360.0F, 0.0F)) && !moved(), "centre pointer does not scroll");

    // Capture lock: the fixed frame is returned and never changes.
    Space locked;
    camera::TacticalFrame fixed{1280, 720, 60.0F, 1.0F, 5000.0F, {0.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F},
                                {0.0F, 1.0F, 0.0F}};
    activate(locked, read("space-map-camera.xml"), fixed);
    if (!locked.ok) return;
    auto& lock = locked.bridge;
    expect(lock.controller().capture_locked() && lock.adapter().capture_locked(), "capture lock engaged");
    expect(lock.frame() == fixed, "locked bridge reports the fixed frame");
    expect(handled(lock, key("D", true)) && handled(lock, wheel(true)) && handled(lock, middle(true))
        && handled(lock, motion(0.0F, 360.0F, 30.0F)) && handled(lock, key("Home", true)), "hostile input handled");
    expect(lock.step(1.0F).has_value() && lock.set_viewport(640.0F, 480.0F).has_value(), "locked step/resize");
    expect(lock.frame() == fixed && lock.steps() == 0U && lock.resets() == 0U && lock.trace().empty(),
           "capture lock ignores input, steps and resize");
    expect(lock.adapter().counters().ignored_ineligible >= 5U && lock.adapter().counters().routed == 0U,
           "capture lock counts every event as ineligible");
}

void test_failures_do_not_partially_mutate() {
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& bridge = space.bridge;
    expect(handled(bridge, wheel(true)), "pending wheel before invalid step");
    const auto frame = bridge.frame();
    const auto steps = bridge.steps();
    expect(!bridge.step(std::numeric_limits<float>::quiet_NaN()), "NaN step rejects");
    expect(!bridge.step(-1.0F), "negative step rejects");
    expect(bridge.frame() == frame && bridge.steps() == steps, "rejected steps change nothing");
    expect(bridge.step(0.0F).has_value() && bridge.controller().state().zoom > 0.5F,
           "pending wheel survived the rejected steps");

    const auto generation = bridge.adapter().counters().viewport_generation;
    const auto sized = bridge.frame();
    expect(!bridge.set_viewport(1.5F, 720.0F), "fractional viewport rejects");
    expect(!bridge.set_viewport(std::numeric_limits<float>::infinity(), 720.0F), "infinite viewport rejects");
    expect(bridge.frame() == sized && bridge.adapter().counters().viewport_generation == generation,
           "rejected viewport changes nothing");

    auto bad = motion(1.0F, 1.0F, std::numeric_limits<float>::quiet_NaN());
    const auto counters = bridge.adapter().counters();
    expect(!bridge.handle(bad), "NaN motion rejects");
    expect(bridge.adapter().counters() == counters && !bridge.rejected_event().empty(),
           "rejected event leaves adapter counters unchanged and is recorded");

    // A failed re-activation keeps the active bridge exactly as it was.
    expect(handled(bridge, key("D", true)), "held before failed re-activation");
    const auto before = bridge.frame();
    const auto held = bridge.adapter().is_held(input::Action::pan_right);
    const auto config = space_config(read("space-map-camera.xml"));
    const auto constants = load(camera::Mode::space);
    if (config && constants) {
        expect(!bridge.activate(*config, constants->constants, read("map-camera-bindings.json"), 1280, 720,
            std::nullopt), "incompatible re-activation rejects");
        expect(!bridge.activate(*config, constants->constants, "{not json", 1280, 720, std::nullopt),
               "malformed re-activation rejects");
    }
    expect(bridge.frame() == before && bridge.adapter().is_held(input::Action::pan_right) == held
        && bridge.adapter().context() == input::Context::space, "failed re-activation mutates nothing");
}

void test_bounded_trace() {
    Space space;
    activate(space, read("space-map-camera.xml"));
    if (!space.ok) return;
    auto& bridge = space.bridge;
    expect(bridge.step(0.1F).has_value() && bridge.trace().empty(), "idle steps are not traced");
    expect(handled(bridge, key("D", true)) && bridge.step(0.05F).has_value(), "traced pan");
    expect(bridge.trace().size() == 1U, "one consumed step is traced");
    if (bridge.trace().size() == 1U) {
        const auto& entry = bridge.trace().front();
        close(entry.seconds, 0.05F, "trace step duration");
        close(entry.pan_x, 1.0F, "trace consumed pan");
        close(entry.target_before[0], 160.0F, "trace initial target");
        close(entry.target_after[0] - entry.target_before[0], pan_speed(0.5F) * 0.05F, "trace displacement");
        close(entry.zoom_before, 0.5F, "trace zoom before");
        close(entry.zoom_after, 0.5F, "trace zoom after");
        close(entry.yaw_after, 0.0F, "trace yaw after");
        expect(entry.step == bridge.steps(), "trace step index");
    }
    expect(handled(bridge, key("D", false)), "release");
    for (std::size_t index = 0; index < MapCameraBridge::max_trace_entries + 5U; ++index) {
        expect(handled(bridge, wheel(true)) && bridge.step(0.0F).has_value(), "wheel step");
    }
    expect(bridge.trace().size() == MapCameraBridge::max_trace_entries && bridge.trace_dropped() == 6U,
           "trace is bounded and counts dropped entries");
}

} // namespace eawr_map_camera_test
