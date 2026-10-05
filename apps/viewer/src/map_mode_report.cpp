#include "viewer_path.hpp"
#include "map_mode_internal.hpp"
#include "startup_trace.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {
namespace map_mode_detail {

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

} // namespace map_mode_detail

std::optional<int> MapMode::State::finish() {
    completed = true;
    release_particles();
    const bool lit = policy != lighting::Policy::off;
    if (particles && !particles->placements().empty()) {
        const auto effects_off = captures.find("effects_off");
        const auto configured = captures.find(configured_frame());
        if (effects_off != captures.end() && configured != captures.end()) {
            particle_evidence_verified = verify_particles(configured->second, effects_off->second);
        } else if (std::any_of(particles->placements().begin(), particles->placements().end(),
                [](const ParticlePlacement& p) { return p.status == "drawn"; })) {
            failure = "map particle effects-off comparison capture is missing";
        }
    }
    const auto unshadowed = captures.find(shadows ? "shadows_off" : configured_frame());
    if (populate) {
        const auto terrain_only = captures.find("terrain_only");
        if (unshadowed == captures.end() || terrain_only == captures.end()) {
            failure = "populated comparison captures are missing";
            static_cast<void>(write_report());
            return 2;
        }
        terrain_only_capture_hash = capture_hashes["terrain_only"];
        unit_evidence_verified = verify_units(unshadowed->second, terrain_only->second);
    }
    if (fog) {
        if (shadows) shadow_evidence_status = "not_attributable_with_fog";
        if (lit) policy_evidence_status = "not_attributable_with_fog";
    } else {
        if (shadows) measure_shadows();
        if (lit) measure_policies();
    }
    bool passed = evidence_verified;
    if (map_camera_selftest) {
        passed = passed && map_camera_checks.size() == map_camera_selftest_check_count
            && std::all_of(map_camera_checks.begin(), map_camera_checks.end(),
                [](const auto& entry) { return entry.second; });
        if (!passed && failure.empty()) failure = "map camera graphical selftest failed";
    }
    if (map_free_selftest) {
        const bool free_checks = !map_free_checks.empty()
            && std::all_of(map_free_checks.begin(), map_free_checks.end(),
                [](const auto& entry) { return entry.second; });
        passed = passed && free_checks;
        if (!free_checks && failure.empty()) failure = "map free camera graphical selftest failed";
    }
    if (populate) passed = passed && unit_evidence_verified;
    if (particles && particles->frames() != 0) passed = passed && particle_evidence_verified
        && !particles->failed() && particle_rids_after_release == 0
        && particle_resources_after_release == 0;
    if (!fog && shadows && shadow_evidence_status == "failed") {
        passed = false;
        if (failure.empty()) failure = "shadowed regions are not darker than with shadows off";
    }
    if (!fog && shadows && populate && shadow_evidence_status == "not_requested") passed = false;
    if (!fog && lit && policy_evidence_status != "verified") {
        passed = false;
        if (failure.empty()) failure = "sh and hemisphere terrain shading is degenerate or identical";
    }
    status = passed ? (map_camera_selftest ? "map_camera_selftest_passed" : "map_render_passed") : "failed";
    if (!write_report()) return 2;
    return passed ? 0 : 2;
}

// The per-record owner block of --eawr-map-effect-animation idle: which owner
// drives the record, the clip's provenance and status, and the lifecycle
// counters and events. The mesh still draws the bind pose.
bool MapMode::State::write_report() const {
    if (options.report_path.empty()) return true;
    std::error_code error;
    const std::filesystem::path parent = options.report_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) return false;
    std::ofstream output(options.report_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const BackendInfo backend = renderer
        ? renderer->backend_info() : BackendInfo{"Godot 4.7.2-stable", "headless", {}, {}, {}};
    const double frames = static_cast<double>(options.timed_frames);
    const double milliseconds = timed_seconds > 0.0 ? (timed_seconds * 1000.0) / frames : 0.0;
    std::uint64_t particle_allocation{};
    std::uint64_t particle_live_at_capture{};
    if (particles) {
        for (const ParticlePlacement& placement : particles->placements()) {
            particle_allocation += placement.capacity;
            particle_live_at_capture += placement.stats.particles;
        }
    }
    output << "{\n  \"schema_version\": 1,\n"
        << "  \"mode\": \"map\",\n"
        << "  \"status\": " << json(status) << ",\n"
        << "  \"failure\": " << json(failure) << ",\n"
        << "  \"backend\": {\"engine\": " << json(backend.engine)
        << ", \"rendering_method\": " << json(backend.rendering_method)
        << ", \"adapter_vendor\": " << json(backend.adapter_vendor)
        << ", \"adapter_name\": " << json(backend.adapter_name)
        << ", \"driver_api\": " << json(backend.driver_api) << "},\n"
        << "  \"render_profile\": " << render_profile_report() << ",\n"
        << "  \"profile\": " << json(profile) << ",\n  \"layers\": [";
    for (std::size_t index = 0; index < layers.size(); ++index) {
        if (index != 0) output << ", ";
        output << json(layers[index]);
    }
    output << "],\n  \"map\": {\"logical_path\": " << json(options.map_path)
        << ", \"sha256\": " << json(map_hash)
        << ", \"kind\": " << json(map_kind)
        << ", \"semantic_complete\": " << (semantic_complete ? "true" : "false") << "},\n";
    startup_trace.write(output);
    if (hud) output << "  \"hud\": " << hud->report_json() << ",\n";
    output << "  \"perf_overlay\": " << perf_report_json() << ",\n";
    write_report_scene(output);
    write_report_lighting(output);
    write_report_fog(output);
    write_report_population(output);
    write_report_effects(output, particle_allocation, particle_live_at_capture);
    write_report_camera(output);
    output << "  \"capture_identity\": {\"viewport\": {\"width\": " << camera.width
        << ", \"height\": " << camera.height << "}, \"png\": ";
    if (capture_size) output << "{\"width\": " << (*capture_size)[0] << ", \"height\": " << (*capture_size)[1] << '}';
    else output << "null";
    output << ", \"camera\": {\"projection\": \"perspective\""
        << ", \"position\": [" << camera.eye[0] << ", " << camera.eye[1] << ", " << camera.eye[2]
        << "], \"target\": [" << camera.target[0] << ", " << camera.target[1] << ", "
        << camera.target[2] << "], \"up\": [" << camera.up[0] << ", " << camera.up[1] << ", "
        << camera.up[2] << "], \"fov_degrees\": " << camera.vertical_fov_degrees
        << ", \"near\": " << camera.near_plane << ", \"far\": " << camera.far_plane << "}"
        << ", \"view\": " << json(camera_view) << ", \"view_cause\": " << json(camera_view_cause);
    if (tactical) {
        output << ", \"tactical\": {\"zoom\": " << tactical->zoom
            << ", \"yaw_degrees\": " << tactical->yaw_degrees
            << ", \"distance\": " << tactical->distance
            << ", \"pitch_degrees\": " << tactical->pitch_degrees
            << ", \"target_source\": [" << tactical->target_source[0] << ", " << tactical->target_source[1]
            << ", " << tactical->target_source[2] << "], \"target_origin\": " << json(tactical->target_origin)
            << ", \"tactical_xml_sha256\": " << json(tactical->tactical_xml_sha256) << '}';
    }
    output << "},\n"
        << "  \"frame_time\": {\"warmup_frames\": " << options.warmup_frames
        << ", \"timed_frames\": " << options.timed_frames
        << ", \"elapsed_seconds\": " << timed_seconds
        << ", \"milliseconds_per_frame\": " << milliseconds
        << ", \"vsync\": " << (benchmark ? "\"disabled\"" : "\"project_default\"") << "},\n"
        << "  \"evidence\": {\"capture_sha256\": " << json(capture_hash)
        << ", \"footprint_half_width\": " << footprint_half_width
        << ", \"footprint_half_height\": " << footprint_half_height
        << ", \"inside_footprint_coverage\": " << inside_coverage
        << ", \"outside_footprint_coverage\": " << outside_coverage
        << ", \"unlocked_changed_pixel_coverage\": " << unlocked_changed_pixel_coverage
        << ", \"unlocked_terminal_changed_pixels\": " << map_camera_terminal_changed_pixels
        << ", \"verified\": " << (evidence_verified ? "true" : "false") << "}\n}\n";
    output.close();
    return static_cast<bool>(output);
}

} // namespace eawr::presentation::godot_backend
