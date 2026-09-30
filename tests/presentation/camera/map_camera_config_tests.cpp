#include "map_camera_test_support.hpp"

namespace eawr_map_camera_test {

constexpr std::string_view land_overrides =
    "  <constant_overrides schema=\"eawr-map-camera-overrides\" version=\"1\""
    " source_id=\"synthetic-land-overrides-v1\" authority=\"project-authored\">\n"
    "    <override tag=\"Distance_Per_Mouse_Unit\" value=\"160\"/>\n"
    "    <override tag=\"Tactical_Max_Scroll_Speed\" value=\"3000\"/>\n"
    "  </constant_overrides>";

void test_committed_configs_carry_no_overrides() {
    // Land configs still resolve directly to the XML layer.
    for (const auto& [name, mode, path, sha] : {
             std::tuple{"map-camera.xml", camera::Mode::land, land_map, land_sha},
             std::tuple{"map-camera-free.xml", camera::Mode::land, land_map, land_sha}}) {
        auto config = eawr::viewer::parse_map_camera_config(read(name), path, sha, mode);
        expect(config.has_value() && config.value().constant_overrides.empty()
               && config.value().overrides_source_id.empty(), "committed config has no override block");
        if (!config) continue;
        const auto xml = load(mode);
        auto resolved = resolve(config.value(), mode);
        expect(resolved.has_value() && xml.has_value() && resolved.value().overrides.empty()
               && resolved.value().provenance.size() == xml->provenance.size()
               && resolved.value().constants.distance_per_mouse_unit == xml->constants.distance_per_mouse_unit
               && resolved.value().constants.tactical_max_scroll_speed == xml->constants.tactical_max_scroll_speed,
               "no override block resolves to the XML layer unchanged");
    }
}

void test_config_override_parsing_rejects() {
    const std::string land_xml = read("map-camera.xml");
    std::string space_xml = read("space-map-camera.xml");
    const auto override_start = space_xml.find("  <constant_overrides ");
    const auto override_end = space_xml.find("  </constant_overrides>", override_start);
    expect(override_start != std::string::npos && override_end != std::string::npos,
           "synthetic space config carries its committed override block");
    if (override_start == std::string::npos || override_end == std::string::npos) return;
    const auto next_line = space_xml.find('\n', override_end);
    if (next_line == std::string::npos) return;
    space_xml.erase(override_start, next_line + 1 - override_start);
    const auto rejects = [](const std::optional<MapCameraConfig>& config, const std::string_view name) {
        expect(!config.has_value(), name);
    };
    const auto parsed = land_config(with_overrides(land_xml, land_overrides));
    expect(parsed.has_value() && parsed->constant_overrides.size() == 2
           && parsed->constant_overrides[0].tag == "Distance_Per_Mouse_Unit"
           && parsed->constant_overrides[0].value == "160"
           && parsed->overrides_source_id == "synthetic-land-overrides-v1"
           && parsed->overrides_authority == "project-authored", "land override block parses in order");
    const auto free_parsed = land_config(with_overrides(read("map-camera-free.xml"), land_overrides));
    expect(free_parsed.has_value() && free_parsed->version == 2U && free_parsed->constant_overrides.size() == 2,
           "override block is accepted in land config version 2");

    const std::string good(land_overrides);
    rejects(land_config(with_overrides(land_xml, replaced(good, "eawr-map-camera-overrides", "eawr-overrides"))),
            "wrong override schema rejects");
    rejects(land_config(with_overrides(land_xml, replaced(good, "version=\"1\"", "version=\"2\""))),
            "unknown override version rejects");
    rejects(land_config(with_overrides(land_xml, replaced(good, " authority=", " extra=\"x\" authority="))),
            "extra override block attribute rejects");
    rejects(land_config(with_overrides(land_xml, replaced(good, " source_id=\"synthetic-land-overrides-v1\"", ""))),
            "missing source_id attribute rejects");
    rejects(land_config(with_overrides(land_xml, replaced(good, "<override tag", "<replace tag"))),
            "non-override child rejects");
    rejects(land_config(with_overrides(land_xml, replaced(good, "value=\"160\"/>", "value=\"160\" unit=\"m\"/>"))),
            "extra override attribute rejects");
    rejects(land_config(with_overrides(land_xml, replaced(good, "value=\"160\"/>", "value=\"160\">x</override>"))),
            "override with content rejects");
    rejects(land_config(with_overrides(land_xml,
        "  <constant_overrides schema=\"eawr-map-camera-overrides\" version=\"1\""
        " source_id=\"empty\" authority=\"project-authored\"/>")), "empty override block rejects");
    rejects(land_config(with_overrides(land_xml, good + "\n  <bounds/>")), "element after override block rejects");
    rejects(land_config(with_overrides(land_xml, good + "\n" + good)), "second override block rejects");
    rejects(land_config(replaced(land_xml, "  <bindings path", good + "\n  <bindings path")),
            "override block before bindings rejects");

    // Semantic checks happen when the layer is resolved, never silently.
    const auto resolve_rejects = [](const std::string& xml, const std::string_view needle,
                                    const std::string_view name) {
        const auto config = land_config(xml);
        expect(config.has_value(), "semantic-case config parses");
        if (!config) return;
        auto resolved = resolve(*config, camera::Mode::land);
        expect(!resolved && resolved.error().message.find(needle) != std::string::npos, name);
    };
    resolve_rejects(with_overrides(land_xml, replaced(good, "authority=\"project-authored\"",
        "authority=\"original\"")), "project-authored", "original override authority fails closed");
    resolve_rejects(with_overrides(land_xml, replaced(good, "Tactical_Max_Scroll_Speed", "Camera_Height")),
                    "not a scalar camera constant", "unknown override tag fails closed");
    resolve_rejects(with_overrides(land_xml, replaced(good, "Tactical_Max_Scroll_Speed", "Pitch_Spline")),
                    "not overridable", "spline override fails closed");
    resolve_rejects(with_overrides(land_xml, replaced(good, "Tactical_Max_Scroll_Speed", "Distance_Per_Mouse_Unit")),
                    "more than once", "duplicate override fails closed");
    resolve_rejects(with_overrides(land_xml, replaced(good, "value=\"3000\"", "value=\"fast\"")),
                    "finite decimal", "malformed override value fails closed");
    resolve_rejects(with_overrides(land_xml, replaced(good, "value=\"3000\"", "value=\"100\"")),
                    "Tactical_Max_Scroll_Speed must not be below", "invalidating override fails closed");

    // Space uses the same block over its own Space_Mode layer.
    const auto space = space_config(with_overrides(space_xml,
        "  <constant_overrides schema=\"eawr-map-camera-overrides\" version=\"1\""
        " source_id=\"synthetic-space-overrides-v1\" authority=\"project-authored\">"
        "<override tag=\"Distance_Max\" value=\"1600\"/></constant_overrides>"));
    expect(space.has_value(), "space override block parses");
    if (space) {
        auto resolved = resolve(*space, camera::Mode::space);
        expect(resolved.has_value() && resolved.value().constants.distance_max == 1600.0F
               && resolved.value().overrides.size() == 1
               && resolved.value().overrides[0].base.definition == "Space_Mode"
               && resolved.value().overrides[0].base_value == 1200.0F,
               "space override replaces the Space_Mode field and names it");
    }

    const auto overview_block = [](const std::string_view value) {
        return "  <constant_overrides schema=\"eawr-map-camera-overrides\" version=\"1\""
               " source_id=\"synthetic-overview\" authority=\"project-authored\">"
               "<override tag=\"Tactical_Overview_Clicks\" value=\""
            + std::string(value) + "\"/></constant_overrides>";
    };
    const auto overview = space_config(with_overrides(space_xml, overview_block("5")));
    expect(overview && overview->overview_clicks == 5U && overview->constant_overrides.empty(),
           "space overview-only override parses independently of tactical constants");
    for (const std::string_view value : {"0", "2.5", "1000001", "bad"}) {
        expect(!space_config(with_overrides(space_xml, overview_block(value))),
               "invalid space overview click count is refused");
    }
    expect(!land_config(with_overrides(land_xml, overview_block("5"))),
           "land overview count cannot be overridden by the space policy");
    const std::string duplicate = replaced(overview_block("5"), "</constant_overrides>",
        "<override tag=\"Tactical_Overview_Clicks\" value=\"5\"/></constant_overrides>");
    expect(!space_config(with_overrides(space_xml, duplicate)), "duplicate overview count is refused");
}

void test_overrides_drive_the_bridge_through_lifecycle() {
    Land base;
    Land over;
    activate_land(base, "");
    activate_land(over, land_overrides);
    if (!base.ok || !over.ok) return;
    expect(base.bridge.frame() == over.bridge.frame(),
           "overriding rates does not move the authored initial pose");
    // Distance_Per_Mouse_Unit 80 over a 400-unit range: 0.2 per detent; the
    // override doubles it to 0.4.
    expect(handled(base.bridge, wheel(true)) && base.bridge.step(0.0F).has_value(), "base wheel step");
    expect(handled(over.bridge, wheel(true)) && over.bridge.step(0.0F).has_value(), "override wheel step");
    close(base.bridge.controller().state().zoom, 0.7F, "XML Distance_Per_Mouse_Unit zoom step");
    close(over.bridge.controller().state().zoom, 0.9F, "overridden Distance_Per_Mouse_Unit zoom step");

    // Focus loss and resize cancel input but never touch the constant layer.
    auto& bridge = over.bridge;
    expect(handled(bridge, wheel(false)), "pending wheel before focus loss");
    bridge.set_focus(false);
    bridge.set_focus(true);
    expect(bridge.step(0.0F).has_value(), "step after focus regain");
    close(bridge.controller().state().zoom, 0.9F, "focus loss dropped the pending override-rate wheel");
    expect(bridge.set_viewport(1000.0F, 600.0F).has_value(), "resize with overrides");
    expect(bridge.frame().width == 1000U && bridge.frame().height == 600U, "resize reaches the frame");
    expect(handled(bridge, wheel(false)) && bridge.step(0.0F).has_value(), "wheel after resize");
    close(bridge.controller().state().zoom, 0.5F, "override rate survives focus loss and resize");
}

void test_fixed_capture_outranks_overrides() {
    const camera::TacticalFrame fixed{1280, 720, 60.0F, 1.0F, 5000.0F, {320.0F, 900.0F, -240.0F},
                                      {320.0F, 0.0F, -240.0F}, {0.0F, 0.0F, -1.0F}};
    Land base;
    Land over;
    activate_land(base, "", fixed);
    activate_land(over, land_overrides, fixed);
    if (!base.ok || !over.ok) return;
    for (Land* land : {&base, &over}) {
        auto& bridge = land->bridge;
        expect(bridge.frame() == fixed, "fixed capture frame is reported exactly");
        expect(handled(bridge, wheel(true)) && handled(bridge, key("D", true)), "hostile input handled");
        bridge.set_focus(false);
        bridge.set_focus(true);
        expect(bridge.step(1.0F).has_value() && bridge.set_viewport(640.0F, 480.0F).has_value(),
               "locked step and resize");
        expect(bridge.frame() == fixed && bridge.steps() == 0U, "fixed capture ignores input and resize");
    }
    expect(base.bridge.frame() == over.bridge.frame(), "overrides cannot alter a fixed capture frame");
}

void test_report_members() {
    Land over;
    activate_land(over, land_overrides);
    if (!over.ok || !over.constants) return;
    const std::string json = eawr::viewer::map_camera_source_members(
        over.constants->overrides, over.bridge.constants(), over.bridge.adapter().table());
    const auto has = [&](const std::string_view needle, const std::string_view name) {
        expect(json.find(needle) != std::string::npos, name);
    };
    expect(json.rfind(", ", 0) == 0, "report members start with a separator");
    has("\"constant_precedence\": [\"effective-vfs-xml\", \"project-authored-map-override\", "
        "\"fixed-capture-frame\"]", "precedence is reported");
    has("{\"tag\": \"Distance_Per_Mouse_Unit\", \"value\": 160, \"replaced_value\": 80, "
        "\"file\": \"map-camera.xml\", \"source_sha256\": \"" + std::string(config_sha) + "\", "
        "\"source_id\": \"synthetic-land-overrides-v1\", \"authority\": \"project-authored\", "
        "\"effect\": \"consumed\", \"effect_reason\": \"read by the bounded tactical controller\", "
        "\"replaced\": {\"file\": \"data/xml/tacticalcameras.xml\", \"source_sha256\": \"tactical-sha\", "
        "\"definition\": \"Land_Mode\", \"status\": \"supplied\"}}", "override row carries both provenances");
    has("\"replaced\": {\"file\": \"data/xml/gameconstants.xml\"", "global override names gameconstants.xml");
    has("\"control_provenance\": \"project-authored\"", "controls are project-authored");
    has("\"original_binding_source\": \"unresolved: no camera binding table in the inventoried original XML corpus\"",
        "original binding source is reported unresolved");
    has("\"edge_scroll_rate_tags\": [\"Tactical_Edge_Scroll_Region\", \"Tactical_Offscreen_Scroll_Region\", "
        "\"Tactical_Min_Scroll_Speed\", \"Tactical_Max_Scroll_Speed\"]", "edge scroll rate tags");
    has("{\"action\": \"pan_left\", \"rate_tags\": [\"Tactical_Min_Scroll_Speed\", \"Tactical_Max_Scroll_Speed\"]}",
        "pan rate tags");
    has("{\"action\": \"zoom\", \"rate_tags\": [\"Distance_Per_Mouse_Unit\", \"Distance_Min\", "
        "\"Distance_Max\"]}", "zoom rate tags include the distance span");
    has("{\"action\": \"rotate\", \"rate_tags\": [\"Yaw_Per_Mouse_Unit\"]}", "rotate rate tag");
    has("{\"action\": \"rotate_grab\", \"rate_tags\": []}", "grab has no XML rate");
    has("{\"action\": \"reset_view\", \"rate_tags\": []}", "reset has no XML rate");
    expect(json.find("\"action\": \"pan_left\"") == json.rfind("\"action\": \"pan_left\""),
           "each bound action is listed once");

    const std::string empty = eawr::viewer::map_camera_source_members({}, camera::Constants{}, nullptr);
    expect(empty.find("\"constant_overrides\": []") != std::string::npos
           && empty.find("\"control_provenance\": \"none\"") != std::string::npos
           && empty.find("\"actions\": []") != std::string::npos
           && empty.find("\"edge_scroll_rate_tags\": []") != std::string::npos,
           "no overrides and no table report empty ledgers");
}

// Resolve refuses what the controller would refuse for this config's bounds
// and pose, before any bridge exists, and names the controller's own code.
void test_resolve_dry_runs_the_controller() {
    const std::string land_xml = read("map-camera.xml");
    const auto block = [](const std::string_view overrides) {
        return "  <constant_overrides schema=\"eawr-map-camera-overrides\" version=\"1\""
               " source_id=\"synthetic-land-overrides-v1\" authority=\"project-authored\">"
            + std::string(overrides) + "</constant_overrides>";
    };
    struct Case final {
        std::string_view name;
        std::string_view overrides;
        std::string_view controller_message;
    };
    for (const Case& item : {
             Case{"FOV outside (0, 180)", "<override tag=\"Fov_Max\" value=\"200\"/>"
                  "<override tag=\"Fov_Default\" value=\"190\"/>", "camera frame is not finite and valid"},
             Case{"initial yaw below Yaw_Min", "<override tag=\"Yaw_Min\" value=\"10\"/>",
                  "initial yaw is outside the supplied camera range"}}) {
        const auto config = land_config(with_overrides(land_xml, block(item.overrides)));
        expect(config.has_value(), "controller-refusal config parses");
        if (!config) continue;
        const auto xml = load(camera::Mode::land);
        if (!xml) continue;
        // The constants layer alone accepts these; only the dry run refuses them.
        auto layered = camera::apply_map_overrides(*xml,
            {"map-camera.xml", config_sha, config->overrides_source_id, config->overrides_authority},
            config->constant_overrides);
        expect(layered.has_value(), std::string(item.name) + ": constants layer accepts");
        if (!layered) continue;
        auto direct = camera::BoundedTacticalController::create(layered.value().constants, config->bounds,
            {config->target_x, config->target_height, -config->target_y}, config->zoom,
            config->yaw_degrees, 1280, 720);
        expect(!direct && direct.error().code == "EAWR-CAMERA-0402"
               && direct.error().message == item.controller_message,
               std::string(item.name) + ": create at the real viewport refuses");
        auto resolved = resolve(*config, camera::Mode::land);
        expect(!resolved && resolved.error().code == camera::invalid_override
               && resolved.error().message == "map camera override: the tactical controller refuses the "
                  "overridden constants for this config's bounds and initial pose (EAWR-CAMERA-0402: "
                  + std::string(item.controller_message) + ")",
               std::string(item.name) + ": resolve fails closed with the exact diagnostic");

        // No state mutation: an active bridge is untouched by the rejected
        // resolve, and activating it with the refused constants anyway fails
        // without changing it.
        Land active;
        activate_land(active, "");
        if (!active.ok) continue;
        const auto frame = active.bridge.frame();
        auto forced = active.bridge.activate(*config, layered.value().constants,
            read("map-camera-bindings.json"), 1280, 720, std::nullopt);
        expect(!forced && active.bridge.frame() == frame && active.bridge.steps() == 0U
               && active.bridge.config().constant_overrides.empty()
               && active.bridge.constants().yaw_min == -1000.0F
               && active.bridge.constants().fov_max == 50.0F,
               std::string(item.name) + ": a refused activation leaves the bridge unchanged");
    }
}

// Every applied override says whether this run's map camera reads it.
void test_override_effect_markers() {
    const auto config = land_config(with_overrides(read("map-camera.xml"),
        "  <constant_overrides schema=\"eawr-map-camera-overrides\" version=\"1\""
        " source_id=\"synthetic-land-overrides-v1\" authority=\"project-authored\">"
        "<override tag=\"Distance_Default\" value=\"250\"/>"
        "<override tag=\"Pitch_Min\" value=\"25\"/>"
        "<override tag=\"Scroll_Acceleration_Factor\" value=\"0.5\"/>"
        "<override tag=\"Push_Scroll_Speed_Modifier\" value=\"3\"/>"
        "<override tag=\"Tactical_Edge_Scroll_Region\" value=\"5\"/>"
        "<override tag=\"Yaw_Per_Mouse_Unit\" value=\"2\"/>"
        "</constant_overrides>"));
    expect(config.has_value(), "effect-marker config parses");
    if (!config) return;
    auto resolved = resolve(*config, camera::Mode::land);
    expect(resolved.has_value(), "inert and unbound overrides are accepted, not refused");
    if (!resolved) return;
    MapCameraBridge bridge{true, input::Context::land};
    auto active = bridge.activate(*config, resolved.value().constants,
        read("map-camera-bindings.json"), 1280, 720, std::nullopt);
    expect(active.has_value(), "effect-marker bridge activates");
    if (!active) return;
    const std::string json = eawr::viewer::map_camera_source_members(
        resolved.value().overrides, bridge.constants(), bridge.adapter().table());
    const auto marked = [&](const std::string& source, const std::string_view tag,
                            const std::string_view effect, const std::string_view reason) {
        const auto at = source.find("{\"tag\": \"" + std::string(tag) + "\"");
        const auto end = source.find("\"replaced\"", at);
        const std::string expected = "\"effect\": \"" + std::string(effect) + "\", \"effect_reason\": \""
            + std::string(reason) + "\"";
        expect(at != std::string::npos && end != std::string::npos
               && source.substr(at, end - at).find(expected) != std::string::npos,
               std::string(tag) + " is marked " + std::string(effect));
    };
    marked(json, "Distance_Default", "inert", "the map config's initial zoom sets the starting distance");
    marked(json, "Pitch_Min", "inert", "Use_Splines is set, so Pitch_Spline supersedes this pitch field");
    marked(json, "Scroll_Acceleration_Factor", "consumed",
           "read by the bounded tactical controller");
    // The land table binds no push_scroll action; it does enable edge scrolling.
    constexpr std::string_view unbound_reason =
        "only an input the active binding table does not bind reads this rate";
    constexpr std::string_view consumed_reason = "read by the bounded tactical controller";
    marked(json, "Push_Scroll_Speed_Modifier", "unbound", unbound_reason);
    marked(json, "Tactical_Edge_Scroll_Region", "consumed", consumed_reason);
    marked(json, "Yaw_Per_Mouse_Unit", "consumed", consumed_reason);
    const std::string no_table = eawr::viewer::map_camera_source_members(
        resolved.value().overrides, bridge.constants(), nullptr);
    marked(no_table, "Tactical_Edge_Scroll_Region", "unbound", unbound_reason);
    marked(no_table, "Yaw_Per_Mouse_Unit", "unbound", unbound_reason);
    marked(no_table, "Distance_Default", "inert", "the map config's initial zoom sets the starting distance");

    // Without splines the pitch fields are read, so they are inert only under them.
    for (const std::string_view tag : {"Pitch_Min", "Pitch_Max", "Pitch_Default", "Pitch_Per_Zoom_Unit",
                                       "Pitch_When_Zoomed_In", "Pitch_Zoom_Begin_Fraction"}) {
        expect(!eawr::viewer::map_override_inert_reason(tag, true).empty()
               && eawr::viewer::map_override_inert_reason(tag, false).empty(),
               std::string(tag) + " is inert only under Use_Splines");
    }
    for (const std::string_view tag : {"Fov_Per_Mouse_Unit", "Yaw_Default"}) {
        expect(!eawr::viewer::map_override_inert_reason(tag, false).empty(),
               std::string(tag) + " is inert on the map camera path");
    }
    for (const std::string_view tag : {"Distance_Min", "Distance_Max", "Distance_Per_Mouse_Unit",
                                       "Distance_Smooth_Time", "Scroll_Deceleration_Factor",
                                       "Yaw_Min", "Yaw_Max", "Yaw_Per_Mouse_Unit", "Pitch_Per_Mouse_Unit",
                                       "Fov_Min", "Fov_Max",
                                       "Fov_Default", "Near_Clip", "Far_Clip", "Tactical_Min_Scroll_Speed",
                                       "Tactical_Max_Scroll_Speed", "Tactical_Edge_Scroll_Region",
                                       "Tactical_Offscreen_Scroll_Region", "Push_Scroll_Speed_Modifier"}) {
        expect(eawr::viewer::map_override_inert_reason(tag, true).empty(),
               std::string(tag) + " is read by the map camera");
    }
}

void test_committed_m1_configs_parse() {
    // Project-authored configs for the M1 reference maps (the identity strings
    // are the maps' recorded SHA-256 values, not retail bytes).
    const auto naboo = eawr::viewer::parse_map_camera_config(read("naboo-map-camera.xml"),
        "data/art/maps/_mp_land_naboo.ted",
        "c2d8eadb44e17eb259fa169ef58733e42ca49119398fe4569c37fdb8d9662d70", camera::Mode::land);
    expect(naboo.has_value() && naboo.value().constant_overrides.empty()
           && naboo.value().bindings_path == "map-camera-bindings.json", "Naboo land camera config parses");
    if (naboo) {
        close(naboo.value().bounds.min_x, 400.0F, "FoC Naboo camera minimum X follows source volume 0");
        close(naboo.value().bounds.max_x, 4580.0F, "FoC Naboo camera maximum X follows source volume 0");
        close(naboo.value().bounds.min_y, 400.0F, "FoC Naboo camera minimum Y follows source volume 0");
        close(naboo.value().bounds.max_y, 4580.0F, "FoC Naboo camera maximum Y follows source volume 0");
        expect(naboo.value().bounds.source_id == "naboo-source-volume-0",
               "FoC Naboo camera source id names volume 0");
        close(naboo.value().target_x, 2480.0F, "FoC Naboo initial target remains centered in its bounds");
        close(naboo.value().target_y, 2480.0F, "FoC Naboo initial target remains centered in its bounds");
    }
    const auto coruscant = eawr::viewer::parse_map_camera_config(read("coruscant-space-map-camera.xml"),
        "data/art/maps/_mp_space_coruscant.ted",
        "91a1fd50ae8ac521e43773f60a3743d64eceb3a00736d80f0ae2234d95108286", camera::Mode::space);
    // #390: the close-zoom override block (checked in test_space_close_zoom_and_depth_floor).
    expect(coruscant.has_value() && coruscant.value().constant_overrides.size() == 3
           && coruscant.value().overview_clicks == 5U
           && coruscant.value().bindings_path == "space-map-camera-bindings.json",
           "Coruscant space camera config parses");
    if (coruscant) {
        close(coruscant.value().bounds.min_x, -6100.0F, "FoC Coruscant camera minimum X follows source volume 1");
        close(coruscant.value().bounds.max_x, 6100.0F, "FoC Coruscant camera maximum X follows source volume 1");
        close(coruscant.value().bounds.min_y, -6100.0F, "FoC Coruscant camera minimum Y follows source volume 1");
        close(coruscant.value().bounds.max_y, 6100.0F, "FoC Coruscant camera maximum Y follows source volume 1");
        expect(coruscant.value().bounds.source_id == "coruscant-source-volume-1",
               "FoC Coruscant camera source id names inner volume 1");
        close(coruscant.value().target_x, 4118.37F, "Coruscant local spawn X", 0.01F);
        close(coruscant.value().target_y, -4976.0F, "Coruscant local spawn Y");
        close(coruscant.value().yaw_degrees, 0.0F, "Coruscant Space_Mode yaw default");
        // Project deviations (owner): distance 1200 (#337/#348), a bit further out than FoC's
        // Distance_Default 1000, of the project range 100..1900 (#390; FoC's is 200..1900).
        close(coruscant.value().zoom, 1100.0F / 1800.0F, "Coruscant opens at distance 1200", 1e-6F);
    }
}

} // namespace eawr_map_camera_test
