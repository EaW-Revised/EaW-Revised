#include "space_environment_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {
namespace space_environment_detail {

[[nodiscard]] std::string report_strings(const std::vector<std::string>& values) {
        std::string text = "[";
        for (std::size_t index = 0; index < values.size(); ++index) text += (index == 0 ? "" : ", ") + json(values[index]);
        return text + "]";
    }

// RFC 8259 has no NaN or infinity; a nonfinite value is written as null.
[[nodiscard]] std::string number(const double value, const int precision) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream output;
    output << std::setprecision(precision) << value;
    return output.str();
}

[[nodiscard]] std::string json(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}

} // namespace space_environment_detail

void SpaceEnvironment::State::write_camera_report(std::ostream& output, const int precision) const {
    const viewer::MapCameraBridge& b = *bridge;
    const viewer::MapCameraSource& source = *options.map_camera;
    const auto& config = b.config();
    const auto& bounds = b.controller().render_bounds();
    const auto& counters = b.adapter().counters();
    const bool locked = b.controller().capture_locked();
    const auto num = [precision](const double value) { return number(value, precision); };
    const auto triple = [&](const std::array<float, 3>& value) {
        return "[" + num(value[0]) + ", " + num(value[1]) + ", " + num(value[2]) + "]";
    };
    output << "  \"space_camera\": {\"schema\": \"eawr-space-map-camera-loop-v1\", \"mode\": "
        << json(tactical::to_string(config.mode)) << ", \"context\": " << json(camera_input::to_string(b.context()))
        << ", \"definition\": " << json(tactical::definition_name(config.mode))
        << ", \"capture_locked\": " << (locked ? "true" : "false")
        << ", \"movement_policy\": \"project policy: bounded tactical controller over the loaded Space_Mode XML "
           "constants and project-authored bindings; not an original-game measurement of speed, zoom or defaults\""
        << ",\n    \"config_version\": " << config.version
        << ", \"config_sha256\": " << json(source.config_sha256)
        << ", \"bindings_sha256\": " << json(source.bindings_sha256)
        << ", \"tactical_xml_sha256\": " << json(source.tactical_xml_sha256)
        << ", \"gameconstants_xml_sha256\": " << json(source.gameconstants_xml_sha256)
        << ",\n    \"constant_sources\": [";
    for (std::size_t index = 0; index < source.provenance.size(); ++index) {
        const auto& field = source.provenance[index];
        output << (index == 0 ? "" : ", ") << "{\"file\": " << json(field.file)
            << ", \"source_sha256\": " << json(field.source_sha256)
            << ", \"definition\": " << json(field.definition) << ", \"tag\": " << json(field.tag)
            << ", \"status\": " << json(field.status == tactical::FieldStatus::supplied ? "supplied" : "absent") << '}';
    }
    output << "]" << viewer::map_camera_source_members(source.overrides, b.constants(), b.adapter().table())
        << viewer::map_overview_source_members(source)
        << ",\n    \"bounds\": {\"authority\": " << json(config.bounds.authority)
        << ", \"source_id\": " << json(config.bounds.source_id)
        << ", \"map_path\": " << json(config.map_path) << ", \"map_sha256\": " << json(config.map_sha256)
        << ", \"source\": [" << num(config.bounds.min_x) << ", " << num(config.bounds.max_x) << ", "
        << num(config.bounds.min_y) << ", " << num(config.bounds.max_y) << "]"
        << ", \"render\": [" << num(bounds.min_x) << ", " << num(bounds.max_x) << ", "
        << num(bounds.min_z) << ", " << num(bounds.max_z) << "]"
        << ", \"cause\": \"explicit map-path/hash-bound project-authored target rectangle; no TED extent, volume or "
           "source-bounds ledger is read\"}"
        << ",\n    \"initial\": {\"target_source\": [" << num(config.target_x) << ", " << num(config.target_y)
        << "], \"height\": " << num(config.target_height) << ", \"zoom\": " << num(config.zoom)
        << ", \"yaw_degrees\": " << num(config.yaw_degrees) << "}"
        << ", \"final\": {\"target\": " << triple(b.frame().target) << ", \"eye\": " << triple(b.frame().eye)
        << ", \"zoom\": " << num(b.controller().state().zoom)
        << ", \"distance\": " << num(b.controller().state().distance)
        << ", \"pitch_degrees\": " << num(b.controller().state().pitch_degrees)
        << ", \"orbit_pitch_offset_degrees\": " << num(b.controller().orbit_pitch_offset())
        << ", \"orbit_pitch_range_degrees\": [" << num(b.orbit_pitch_range().min_degrees)
        << ", " << num(b.orbit_pitch_range().max_degrees) << "]"
        << ", \"yaw_degrees\": " << num(b.controller().yaw_degrees()) << "}"
        << ",\n    \"steps\": " << b.steps() << ", \"resets\": " << b.resets()
        << ", \"view_resets\": " << b.view_resets()
        << ", \"steps_at_freeze\": " << steps_at_freeze << ", \"settle_frames\": " << settle_frames
        << ", \"terminal_baseline_test\": " << (options.camera_terminal_baseline_test ? "true" : "false")
        << ", \"terminal_hold_test\": " << (options.camera_terminal_hold_test ? "true" : "false")
        << ", \"terminal_release_test\": " << (options.camera_terminal_release_test ? "true" : "false")
        << ", \"terminal_step_seconds\": " << number(terminal_step_seconds)
        << ", \"terminal_pan_held_at_step\": " << (terminal_held_at_step ? "true" : "false")
        << ", \"terminal_pan_held_at_capture\": "
        << (b.adapter().is_held(camera_input::Action::pan_right) ? "true" : "false")
        << ",\n    \"input_callbacks\": " << b.input_callbacks()
        << ", \"focus_notifications\": " << focus_notifications
        << ", \"resize_notifications\": " << resize_notifications
        << ", \"resize_active_at_capture\": " << (resize_active_at_capture ? "true" : "false")
        << ", \"viewport_generation\": " << counters.viewport_generation
        << ", \"events_delivered\": " << counters.delivered << ", \"events_routed\": " << counters.routed
        << ", \"cancellations\": " << counters.cancellations
        << ", \"ignored_ineligible\": " << counters.ignored_ineligible
        << ", \"rejected_event\": " << json(b.rejected_event())
        << ",\n    \"trace\": {\"limit\": " << viewer::MapCameraBridge::max_trace_entries
        << ", \"dropped\": " << b.trace_dropped() << ", \"entries\": [";
    for (std::size_t index = 0; index < b.trace().size(); ++index) {
        const viewer::MapCameraTrace& entry = b.trace()[index];
        output << (index == 0 ? "\n      " : ",\n      ") << "{\"step\": " << entry.step
            << ", \"seconds\": " << num(entry.seconds)
            << ", \"consumed\": {\"pan\": [" << num(entry.pan_x) << ", " << num(entry.pan_y) << "]"
            << ", \"drag\": [" << num(entry.drag_x) << ", " << num(entry.drag_y) << "]"
            << ", \"push_scroll\": " << (entry.push_scroll ? "true" : "false")
            << ", \"zoom_detents\": " << num(entry.zoom_detents)
            << ", \"rotate_units\": " << num(entry.rotate_units)
            << ", \"orbit_pitch_units\": " << num(entry.orbit_pitch_units)
            << ", \"translate_units\": [" << num(entry.translate_x) << ", " << num(entry.translate_y) << "]"
            << ", \"resets\": " << entry.resets << ", \"view_resets\": " << entry.view_resets << "}"
            << ", \"target_before\": " << triple(entry.target_before)
            << ", \"target_after\": " << triple(entry.target_after)
            << ", \"zoom_before\": " << num(entry.zoom_before) << ", \"zoom_after\": " << num(entry.zoom_after)
            << ", \"yaw_before\": " << num(entry.yaw_before) << ", \"yaw_after\": " << num(entry.yaw_after)
            << ", \"pitch_before\": " << num(entry.pitch_before)
            << ", \"pitch_after\": " << num(entry.pitch_after) << '}';
    }
    output << (b.trace().empty() ? "" : "\n    ") << "]}"
        << ",\n    \"sky_fidelity\": " << json(locked
            ? "fixed_camera_evidence: space.pixel_evidence_status from masks of the locked --eawr-space-camera frame"
            : "not_evaluated: interactive pose; fixed-camera masks, controls and comparison phases do not apply")
        << ", \"drawn_evidence\": {\"status\": " << json(locked ? "not_applicable" : drawn_status)
        << ", \"control\": \"sky_disabled submission at the frozen pose\""
        << ", \"changed_pixels\": " << drawn_changed_pixels << ", \"pixels\": " << drawn_sampled_pixels
        << ", \"threshold\": " << space::changed_threshold << "}"
        << ",\n    \"selftest\": {\"requested\": " << (options.camera_selftest ? "true" : "false")
        << ", \"checks\": [";
    for (std::size_t index = 0; index < checks.size(); ++index) {
        output << (index == 0 ? "" : ", ") << "{\"name\": " << json(checks[index].first)
            << ", \"passed\": " << (checks[index].second ? "true" : "false") << '}';
    }
    output << "]}},\n";
}

bool SpaceEnvironment::State::write_report() const {
    if (options.report_path.empty()) return true;
    std::error_code error;
    const std::filesystem::path parent = options.report_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) return false;
    std::ofstream output(options.report_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    // A blocked plan never constructs a renderer, so no backend is claimed.
    const BackendInfo backend = renderer
        ? renderer->backend_info() : BackendInfo{"Godot 4.7.2-stable", "renderer_not_created", {}, {}, {}};
    const double frames = static_cast<double>(options.timed_frames);
    const double milliseconds = timed_seconds > 0.0 ? (timed_seconds * 1000.0) / frames : 0.0;

    output << "{\n  \"schema_version\": 1,\n  \"mode\": \"map\",\n"
        << "  \"status\": " << json(status) << ",\n"
        << "  \"failure\": " << json(failure) << ",\n"
        << "  \"backend\": {\"engine\": " << json(backend.engine)
        << ", \"rendering_method\": " << json(backend.rendering_method)
        << ", \"adapter_vendor\": " << json(backend.adapter_vendor)
        << ", \"adapter_name\": " << json(backend.adapter_name)
        << ", \"driver_api\": " << json(backend.driver_api) << "},\n"
        << "  \"render_profile\": " << render_profile_report() << ",\n"
        << "  \"profile\": " << json(options.profile) << ",\n  \"layers\": " << report_strings(options.layers) << ",\n"
        << "  \"map\": {\"logical_path\": " << json(options.map_path)
        << ", \"sha256\": " << json(options.map_sha256)
        << ", \"kind\": \"space\", \"semantic_complete\": " << (options.semantic_complete ? "true" : "false") << "},\n"
        << "  \"terrain\": {\"chunks\": 0, \"uploaded_surfaces\": 0, \"vertices\": 0, \"triangles\": 0, \"status\": "
           "\"not_applicable\", \"cause\": \"a kind-2 map has no terrain view; terrain::build rejects it by contract "
           "and is not called\"},\n";
    if (options.write_hud_report) options.write_hud_report(output);
    output
        << "  \"populate\": {\"requested\": false, \"declared_placements\": " << declared_placements
        << ", \"composed\": " << (options.fog && fog_units ? fog_units->units().size() : 0) << ", \"cause\": "
        << (options.fog ? "\"space fog admission only: placements of the caller-declared --eawr-space-fog-admit XML "
                          "element types; not a classification of placements as environment or gameplay objects\"},\n"
                        : "\"environment-only mode: classifying placements as environment or gameplay "
                          "objects needs catalog/effect evidence this slice does not have\"},\n")
        << (shadow_control(control)
            ? "  \"lighting\": {\"policy\": \"sky_shadow_control\", \"composed\": true, \"cause\": \"control fixture "
              "only: one fixed directional light with shadows aimed through surface 0 onto a lit receiver; not a "
              "source-backed space lighting environment, and the sky adapter stays unshaded\"},\n"
            : "  \"lighting\": {\"policy\": \"off\", \"composed\": false, \"cause\": \"the space hull lighting/shadow "
              "fixture is a separate join; the sky adapter is unshaded\"},\n");

    write_plan_report(output);
    write_surfaces_report(output);
    write_submission_report(output);
    // An unlocked pose is written with round-trip precision so a reproduced
    // capture can be bound to exactly this identity.
    const int precision = interactive() ? std::numeric_limits<float>::max_digits10 : 6;
    const auto triple = [precision](const std::array<float, 3>& value) {
        return "[" + number(value[0], precision) + ", " + number(value[1], precision) + ", "
            + number(value[2], precision) + "]";
    };
    if (options.fog) write_fog_report(output);
    if (bridge) write_camera_report(output, precision);
    output << "  \"capture_identity\": {\"viewport\": {\"width\": " << camera.width
        << ", \"height\": " << camera.height << "}, \"png\": ";
    if (capture_size) output << "{\"width\": " << (*capture_size)[0] << ", \"height\": " << (*capture_size)[1] << '}';
    else output << "null";
    output << ", \"camera\": ";
    // A rejected camera is never used, so it has no capture identity; its
    // text stays in space.camera.input.
    if (camera_status == space::CameraStatus::valid) {
        output << "{\"projection\": \"perspective\", \"position\": " << triple(camera.eye)
            << ", \"target\": " << triple(camera.target) << ", \"up\": " << triple(camera.up)
            << ", \"fov_degrees\": " << number(camera.vertical_fov_degrees, precision)
            << ", \"near\": " << number(camera.near_plane, precision)
            << ", \"far\": " << number(camera.far_plane, precision) << '}';
    } else {
        output << "null";
    }
    output << "},\n"
        << "  \"frame_time\": {\"warmup_frames\": " << options.warmup_frames
        << ", \"timed_frames\": " << options.timed_frames
        << ", \"elapsed_seconds\": " << number(timed_seconds)
        << ", \"milliseconds_per_frame\": " << number(milliseconds) << "},\n"
        << "  \"captures\": {";
    bool first = true;
    for (const auto& [name, hash] : capture_hashes) {
        output << (first ? "" : ", ") << json(name) << ": " << json(hash);
        first = false;
    }
    output << "}\n}\n";
    output.close();
    return static_cast<bool>(output);
}

} // namespace eawr::presentation::godot_backend
