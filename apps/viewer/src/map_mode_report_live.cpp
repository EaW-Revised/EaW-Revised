#include "viewer_path.hpp"
#include "map_mode_internal.hpp"
#include "startup_trace.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {

void MapMode::State::write_report_fog(std::ostream& output) const {
    if (fog) {
        const auto selected = fog->source().find(fog->team());
        const auto actual = renderer ? renderer->fog_status() : GodotRenderer::FogStatus{};
        const char* readiness = "disabled";
        switch (actual.readiness) {
        case GodotRenderer::FogReadiness::disabled: readiness = "disabled"; break;
        case GodotRenderer::FogReadiness::awaiting_grid: readiness = "awaiting_grid"; break;
        case GodotRenderer::FogReadiness::ready: readiness = "ready"; break;
        case GodotRenderer::FogReadiness::rejected: readiness = "rejected"; break;
        }
        output << "  \"fog\": {\"source\": [";
        for (std::size_t i = 0; i < fog->sources().size(); ++i) {
            const auto& source = fog->sources()[i];
            output << (i ? ", " : "") << "{\"path\": " << json(ViewerPath::utf8(source.path))
                << ", \"sha256\": " << json(source.sha256)
                << ", \"team\": " << source.team << ", \"revision\": " << source.revision << '}';
        }
        output << "], \"team\": " << fog->team()
            << ", \"source_revision\": " << (selected ? selected->revision() : 0)
            << ", \"grid\": ";
        if (selected) {
            const auto& desc = selected->desc();
            output << "{\"width\": " << desc.width << ", \"height\": " << desc.height
                << ", \"origin_raw\": [" << desc.origin_x_raw << ", " << desc.origin_y_raw
                << "], \"cell_raw\": [" << desc.cell_x_raw << ", " << desc.cell_y_raw << "]}";
        } else output << "null";
        output
            << ", \"bound_revision\": ";
        if (actual.bound_revision) output << *actual.bound_revision;
        else output << "null";
        output << ", \"stream\": " << fog->stream()
            << ", \"snapshot_tick\": " << fog->tick()
            << ", \"mapping\": \"source_xy_from_world_x_negative_z\""
            << ", \"filter\": \"nearest\""
            << ", \"fixed_capture\": " << (fog->fixed_capture() ? "true" : "false")
            << ", \"ignored_input_events\": " << fog_ignored_inputs
            << ", \"override\": " << (fog->override_active() ? "true" : "false")
            << ", \"renderer\": {\"readiness\": " << json(readiness)
            << ", \"submitted_tick\": ";
        if (actual.submitted_tick) output << *actual.submitted_tick;
        else output << "null";
        output << ", \"attached_consumers\": " << actual.attached_consumers
            << ", \"declared_consumers\": " << actual.declared_consumers
            << ", \"external_consumers\": " << actual.external_consumers
            << ", \"live_textures\": " << actual.live_textures
            << ", \"uploads\": " << actual.cache.uploads
            << ", \"upload_bytes\": " << actual.cache.upload_bytes
            << ", \"creates\": " << actual.cache.creates
            << ", \"updates\": " << actual.cache.updates
            << ", \"binds\": " << actual.cache.binds
            << ", \"last_action\": "
            << json(actual.last_action ? std::string(eawr::presentation::fog::to_string(*actual.last_action)) : "")
            << ", \"last_rejection\": "
            << json(actual.last_rejection ? core::format_diagnostic(*actual.last_rejection) : "")
            << ", \"rejected\": " << actual.cache.rejected << "}, \"unsupported\": [";
        for (std::size_t i = 0; i < fog_unsupported.size(); ++i) {
            output << (i ? ", " : "") << json(fog_unsupported[i]);
        }
        output << "], \"uncomposed_placements\": [";
        for (std::size_t i = 0; i < fog_uncomposed.size(); ++i) {
            output << (i ? ", " : "") << json(fog_uncomposed[i]);
        }
        output << "], \"paint_evidence\": {";
        bool first_paint_hash = true;
        for (const auto& [name, hash] : fog_paint_hashes) {
            output << (first_paint_hash ? "" : ", ") << json(name) << ": " << json(hash);
            first_paint_hash = false;
        }
        output << "}, \"paint_uploads\": {";
        bool first_upload = true;
        for (const auto& [name, count] : fog_paint_uploads) {
            output << (first_upload ? "" : ", ") << json(name) << ": " << count;
            first_upload = false;
        }
        output << "}, \"paint_updates\": {";
        bool first_update = true;
        for (const auto& [name, count] : fog_paint_updates) {
            output << (first_update ? "" : ", ") << json(name) << ": " << count;
            first_update = false;
        }
        output << "}, \"map_sha256\": " << json(map_hash)
            << ", \"scene_sha256\": " << json(scene ? scene->scene_sha256 : "")
            << "},\n";
    }
}

void MapMode::State::write_report_camera(std::ostream& output) const {
    if (map_camera) {
        const auto& bridge = *map_camera;
        // An unlocked reported pose must round-trip when its capture is reproduced.
        if (!bridge.controller().capture_locked()) {
            output << std::setprecision(std::numeric_limits<float>::max_digits10);
        }
        const auto& config = bridge.config();
        const auto& bounds = bridge.controller().render_bounds();
        const auto& counters = bridge.adapter().counters();
        output << "  \"map_camera\": {\"schema\": \"eawr-map-camera-loop-v1\""
            << ", \"config_version\": " << config.version
            << ", \"follows_terrain\": " << (bridge.follows_ground() ? "true" : "false")
            << ", \"config_sha256\": " << json(map_camera_config_sha256)
            << ", \"bindings_sha256\": " << json(map_camera_bindings_sha256)
            << ", \"tactical_xml_sha256\": " << json(map_camera_tactical_xml_sha256)
            << ", \"gameconstants_xml_sha256\": " << json(map_camera_gameconstants_xml_sha256)
            << ", \"constant_sources\": [";
        for (std::size_t index = 0; index < map_camera_constant_sources.size(); ++index) {
            const auto& field = map_camera_constant_sources[index];
            output << (index == 0 ? "" : ", ") << "{\"file\": " << json(field.file)
                << ", \"source_sha256\": " << json(field.source_sha256)
                << ", \"definition\": " << json(field.definition)
                << ", \"tag\": " << json(field.tag)
                << ", \"status\": " << json(field.status == camera::FieldStatus::supplied
                    ? "supplied" : "absent") << '}';
        }
        output << "]"
            << eawr::viewer::map_camera_source_members(
                map_camera_constant_overrides, bridge.constants(), bridge.adapter().table())
            << ", \"provenance\": \"project-authored\""
            << ", \"source_id\": " << json(config.bounds.source_id)
            << ", \"map_sha256\": " << json(config.map_sha256)
            << ", \"bounds_source\": [" << config.bounds.min_x << ", "
            << config.bounds.max_x << ", " << config.bounds.min_y << ", "
            << config.bounds.max_y << "]"
            << ", \"bounds_render\": [" << bounds.min_x << ", " << bounds.max_x
            << ", " << bounds.min_z << ", " << bounds.max_z << "]"
            << ", \"initial\": {\"target_source\": [" << config.target_x << ", "
            << config.target_y << "], \"height\": " << config.target_height
            << ", \"zoom\": " << config.zoom << ", \"yaw_degrees\": " << config.yaw_degrees << "}"
            << ", \"final\": {\"target\": [" << camera.target[0] << ", "
            << camera.target[1] << ", " << camera.target[2] << "], \"eye\": ["
            << camera.eye[0] << ", " << camera.eye[1] << ", " << camera.eye[2]
            << "], \"zoom\": " << bridge.controller().state().zoom
            << ", \"pitch_degrees\": " << bridge.controller().state().pitch_degrees
            << ", \"orbit_pitch_offset_degrees\": " << bridge.controller().orbit_pitch_offset()
            << ", \"orbit_pitch_range_degrees\": [" << bridge.orbit_pitch_range().min_degrees
            << ", " << bridge.orbit_pitch_range().max_degrees << "]"
            << ", \"orbit_pitch_per_mouse_unit\": " << bridge.orbit_pitch_per_mouse_unit()
            << ", \"yaw_degrees\": " << bridge.controller().yaw_degrees() << "}"
            << ", \"capture_locked\": " << (bridge.controller().capture_locked() ? "true" : "false")
            << ", \"steps\": " << bridge.steps() << ", \"resets\": " << bridge.resets()
            << ", \"view_resets\": " << bridge.view_resets()
            << ", \"steps_at_freeze\": " << map_camera_steps_at_freeze
            << ", \"settle_frames\": " << map_camera_settle_frames
            << ", \"terminal_hold_test\": " << (map_camera_terminal_hold_test ? "true" : "false")
            << ", \"free_terminal_hold_test\": " << (map_free_terminal_hold_test ? "true" : "false")
            << ", \"free_terminal_release_test\": " << (map_free_terminal_release_test ? "true" : "false")
            << ", \"free_terminal_forward_held_at_step\": "
            << (map_free_terminal_forward_held_at_step ? "true" : "false")
            << ", \"free_terminal_forward_held_at_capture\": "
            << (bridge.adapter().is_held(viewer::camera_input::Action::free_move_forward)
                ? "true" : "false")
            << ", \"input_callbacks\": " << bridge.input_callbacks()
            << ", \"focus_notifications\": " << map_camera_focus_notifications
            << ", \"resize_notifications\": " << map_camera_resize_notifications
            << ", \"resize_active_at_capture\": " << (map_camera_resize_active_at_capture ? "true" : "false")
            << ", \"viewport_generation\": " << counters.viewport_generation
            << ", \"events_delivered\": " << counters.delivered
            << ", \"events_routed\": " << counters.routed
            << ", \"cancellations\": " << counters.cancellations
            << ", \"ignored_ineligible\": " << counters.ignored_ineligible
            << ", \"rejected_event\": " << json(bridge.rejected_event())
            << ", \"active_mode\": " << json(bridge.free_active() ? "free" : "land")
            << ", \"free_camera\": {\"controller\": \"eawr-free-camera-v1\""
            << ", \"settings_provenance\": " << json(bridge.free_settings()
                ? "project-authored-bindings-v2" : "absent")
            << ", \"settings\": ";
        if (bridge.free_settings()) {
            const auto& settings = *bridge.free_settings();
            output << "{\"move_speed\": " << settings.move_speed
                << ", \"vertical_speed\": " << settings.vertical_speed
                << ", \"look_degrees_per_unit\": " << settings.look_degrees_per_unit
                << ", \"pitch_min_degrees\": " << settings.pitch_min_degrees
                << ", \"pitch_max_degrees\": " << settings.pitch_max_degrees << '}';
        } else output << "null";
        output << ", \"entries\": " << bridge.free_entries()
            << ", \"exits\": " << bridge.free_exits()
            << ", \"rejections\": " << bridge.free_rejections()
            << ", \"steps\": " << bridge.free_steps()
            << ", \"rejection\": " << json(bridge.free_rejection())
            << ", \"transitions\": [";
        for (std::size_t index = 0; index < bridge.free_transitions().size(); ++index) {
            output << (index == 0 ? "" : ", ") << json(bridge.free_transitions()[index]);
        }
        output << "]"
            << ", \"pose\": ";
        if (bridge.free_pose()) {
            const auto& pose = *bridge.free_pose();
            output << "{\"eye\": [" << pose.eye[0] << ", " << pose.eye[1] << ", "
                << pose.eye[2] << "], \"yaw_degrees\": " << pose.yaw_degrees
                << ", \"pitch_degrees\": " << pose.pitch_degrees << '}';
        } else output << "null";
        output << "}"
            << ", \"free_selftest\": {\"requested\": "
            << (map_free_selftest ? "true" : "false") << ", \"checks\": [";
        for (std::size_t index = 0; index < map_free_checks.size(); ++index) {
            output << (index == 0 ? "" : ", ") << "{\"name\": "
                << json(map_free_checks[index].first) << ", \"passed\": "
                << (map_free_checks[index].second ? "true" : "false") << '}';
        }
        output << "]}"
            << ", \"selftest\": {\"requested\": " << (map_camera_selftest ? "true" : "false")
            << ", \"checks\": [";
        for (std::size_t index = 0; index < map_camera_checks.size(); ++index) {
            output << (index == 0 ? "" : ", ") << "{\"name\": "
                << json(map_camera_checks[index].first) << ", \"passed\": "
                << (map_camera_checks[index].second ? "true" : "false") << '}';
        }
        output << "]}},\n";
    }
}

} // namespace eawr::presentation::godot_backend
