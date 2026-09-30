#include "viewer_host_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// Report strings escape quote, backslash and every U+0000-U+001F byte; a
// camera-binding notice may legitimately carry escaped control characters.
[[nodiscard]] std::string json(const std::string_view value) {
    return eawr::viewer::camera_input::json_string_literal(value);
}

} // namespace

bool ViewerHost::write_report(const std::string_view status) {
    if (options_->report_path.empty()) return true;
    std::error_code error;
    const std::filesystem::path parent = options_->report_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) {
        UtilityFunctions::printerr("EAWR viewer report output directory could not be created");
        return false;
    }
    std::ofstream output(options_->report_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        UtilityFunctions::printerr("EAWR viewer report output could not be opened");
        return false;
    }
    const BackendInfo backend = renderer_ ? renderer_->backend_info() : BackendInfo{
        "Godot 4.7.2-stable", "headless", {}, {}, {}};
    std::vector<RenderPass> observed_passes;
    if (renderer_) {
        for (const GodotRenderer::SubmissionEvidence& draw : renderer_->submission_evidence()) {
            if (observed_passes.empty() || observed_passes.back() != draw.pass) {
                observed_passes.push_back(draw.pass);
            }
        }
    }
    const std::uint64_t capture_tick = fixed_capture_snapshot_
        ? fixed_capture_snapshot_->completed_tick()
        : (snapshots_.empty() ? 0U : snapshots_.front()->completed_tick());
    output << "{\n  \"schema_version\": 1,\n"
        << "  \"status\": " << json(status) << ",\n"
        << "  \"failure\": " << json(status_message_) << ",\n"
        << "  \"backend\": {\"engine\": " << json(backend.engine)
        << ", \"rendering_method\": " << json(backend.rendering_method)
        << ", \"adapter_vendor\": " << json(backend.adapter_vendor)
        << ", \"adapter_name\": " << json(backend.adapter_name)
        << ", \"driver_api\": " << json(backend.driver_api) << "},\n"
        << "  \"render_profile\": " << render_profile_report() << ",\n"
        << "  \"versions\": {\"godot\": \"4.7.2-stable\", \"godot_cpp\": \"10.0.0-stable\", \"material_schema\": 1},\n"
        << "  \"scene_sha256\": " << json(scene_hash_) << ",\n"
        << "  \"replay\": {\"logical_path\": " << json(options_->replay_path.value())
        << ", \"sha256\": " << json(replay_hash_) << "},\n"
        << "  \"snapshot_count\": " << snapshots_.size() << ",\n"
        << "  \"capture_sha256\": " << json(capture_hash_) << ",\n"
        << "  \"capture_identity\": {\"vfs_profile\": " << json(active_content_profile_)
        << ", \"viewport\": {\"width\": "
        << capture_camera_.width << ", \"height\": " << capture_camera_.height
        << "}, \"camera\": {\"projection\": \"perspective\", \"position\": ["
        << capture_camera_.eye[0] << ',' << capture_camera_.eye[1] << ',' << capture_camera_.eye[2]
        << "], \"target\": [" << capture_camera_.target[0] << ',' << capture_camera_.target[1]
        << ',' << capture_camera_.target[2] << "], \"up\": [" << capture_camera_.up[0] << ','
        << capture_camera_.up[1] << ',' << capture_camera_.up[2]
        << "], \"fov_degrees\": " << capture_camera_.vertical_fov_degrees
        << ", \"near\": " << capture_camera_.near_plane << ", \"far\": "
        << capture_camera_.far_plane << "}, \"simulation_tick\": " << capture_tick
        << ", \"capture_frame\": " << frame_ << ", \"animation_time_seconds\": "
        << animation_time_seconds_ << ", \"particle_seed\": 0, \"particle_time_seconds\": 0.0},\n"
        << "  \"model\": {\"logical_path\": " << json(selected_model_path_)
        << ", \"sha256\": " << json(selected_model_hash_) << "},\n"
        << "  \"texture\": {\"logical_path\": " << json(selected_texture_path_)
        << ", \"sha256\": " << json(selected_texture_hash_) << "},\n"
        << "  \"material_program\": " << json(selected_program_) << ",\n"
        << "  \"animation\": {\"logical_path\": " << json(selected_animation_path_)
        << ", \"sha256\": " << json(selected_animation_hash_)
        << ", \"bone_count\": " << (animation_pose_ ? animation_pose_->bones.size() : 0U) << "},\n"
        << "  \"animation_time_seconds\": " << animation_time_seconds_ << ",\n"
        << "  \"skin_palette_bound\": " << (skin_palette_bound_ ? "true" : "false") << ",\n"
        << "  \"animation_capture_verified\": " << (animation_capture_verified_ ? "true" : "false") << ",\n"
        << "  \"renderer_runtime_exercise\": " << (runtime_exercise_ ? "true" : "false") << ",\n"
        << "  \"runtime_capture_verified\": " << (runtime_capture_verified_ ? "true" : "false") << ",\n"
        << "  \"runtime_overlap_verified\": " << (runtime_overlap_verified_ ? "true" : "false") << ",\n"
        << "  \"runtime_post_dependency_verified\": "
        << (runtime_post_dependency_verified_ ? "true" : "false") << ",\n"
        << "  \"runtime_failed_upload_registry_clean\": "
        << (runtime_failed_upload_clean_ ? "true" : "false") << ",\n"
        << "  \"runtime_scene_switch_count\": " << runtime_scene_switches_ << ",\n"
        << "  \"runtime_shutdown_resources_empty\": "
        << (runtime_shutdown_resources_empty_ ? "true" : "false") << ",\n"
        << "  \"tactical_camera_verified\": " << (tactical_camera_verified_ ? "true" : "false")
        << ",\n";
    if (tactical_camera_) {
        const TacticalCameraRun& run = *tactical_camera_;
        output << "  \"tactical_camera\": {\"mode\": "
            << json(tactical_camera::to_string(run.mode)) << ", \"definition\": "
            << json(tactical_camera::definition_name(run.mode)) << ", \"sources\": ["
            << "{\"logical_path\": " << json(run.camera_logical_path) << ", \"sha256\": "
            << json(run.camera_sha256) << "}, {\"logical_path\": "
            << json(run.constants_logical_path) << ", \"sha256\": "
            << json(run.constants_sha256) << "}], \"limits\": {\"distance_min\": "
            << run.constants.distance_min << ", \"distance_max\": "
            << run.constants.distance_max << ", \"distance_default\": "
            << run.constants.distance_default << ", \"pitch_min\": "
            << run.constants.pitch_min << ", \"pitch_max\": " << run.constants.pitch_max
            << ", \"fov_min\": " << run.constants.fov_min << ", \"fov_max\": "
            << run.constants.fov_max << ", \"near_clip\": " << run.constants.near_clip
            << ", \"far_clip\": " << run.constants.far_clip << ", \"use_splines\": "
            << (run.constants.use_splines ? "true" : "false")
            << ", \"tactical_min_scroll_speed\": " << run.constants.tactical_min_scroll_speed
            << ", \"tactical_max_scroll_speed\": " << run.constants.tactical_max_scroll_speed
            << "}, \"solved\": {\"zoom\": " << run.state.zoom << ", \"distance\": "
            << run.state.distance << ", \"pitch_degrees\": " << run.state.pitch_degrees
            << ", \"yaw_degrees\": " << run.state.yaw_degrees << ", \"fov_degrees\": "
            << run.state.fov_degrees << ", \"pan_speed\": " << run.state.pan_speed
            << "}, \"eye\": [" << run.eye[0] << ", " << run.eye[1] << ", " << run.eye[2]
            << "], \"input_mapping\": {\"zoom_step\": " << run.zoom_step
            << ", \"pan_x_per_second\": " << run.pan_x_per_second
            << ", \"push_pan_x_per_second\": " << run.push_pan_x_per_second
            << ", \"edge_scroll_left\": " << run.edge_scroll_left
            << ", \"edge_scroll_right\": " << run.edge_scroll_right
            << ", \"yaw_after_drag\": " << run.yaw_after_drag
            << ", \"pitch_after_drag\": " << run.pitch_after_drag << "}},\n";
    }
    if (camera_interaction_) {
        const CameraInteraction& run = *camera_interaction_;
        const camera_input::AdapterCounters& counters = run.adapter.counters();
        const camera_input::BindingTable* table = run.adapter.table();
        std::ostringstream section;
        section << std::setprecision(9);
        section << "  \"camera_input\": {\"bindings\": {\"logical_path\": "
            << json(run.bindings_path) << ", \"sha256\": " << json(run.bindings_sha256)
            << ", \"schema\": " << json(camera_input::bindings_schema) << ", \"version\": "
            << (table ? table->version : 0) << ", \"provenance\": "
            << json(table ? table->provenance : std::string{}) << ", \"notice\": "
            << json(table ? table->notice : std::string{}) << ", \"binding_count\": "
            << (table ? table->bindings.size() : 0U) << ", \"edge_scroll\": "
            << (table && table->edge_scroll ? "true" : "false") << "}, \"context\": "
            << json(camera_input::to_string(run.adapter.context()))
            << ", \"capture_locked_for_run\": " << (run.capture_locked_for_run ? "true" : "false")
            << ", \"bounds\": null, \"bounds_status\": "
            << json("unavailable: typed #26 camera bounds are not supplied; target is unbounded")
            << ", \"input_callbacks\": " << run.input_callbacks
            << ", \"focus_notifications\": " << run.focus_notifications
            << ", \"resize_notifications\": " << run.resize_notifications
            << ", \"counters\": {\"delivered\": " << counters.delivered << ", \"routed\": "
            << counters.routed << ", \"ignored_ineligible\": " << counters.ignored_ineligible
            << ", \"ignored_unbound\": " << counters.ignored_unbound << ", \"cancellations\": "
            << counters.cancellations << ", \"viewport_generation\": "
            << counters.viewport_generation << "}, \"steps\": " << run.steps
            << ", \"moving_steps\": " << run.moving_steps << ", \"rejected_event\": "
            << json(run.rejected_event) << ", \"pose\": {\"zoom\": " << run.pose.state.zoom
            << ", \"distance\": " << run.pose.state.distance << ", \"pitch_degrees\": "
            << run.pose.state.pitch_degrees << ", \"yaw_degrees\": "
            << run.pose.state.yaw_degrees << ", \"target\": [" << run.pose.target[0] << ", "
            << run.pose.target[1] << ", " << run.pose.target[2] << "]}, \"interactive_eye\": ["
            << run.camera.eye[0] << ", " << run.camera.eye[1] << ", " << run.camera.eye[2]
            << "]";
        // Free flight: project-authored controller, its settings, transitions
        // and the latest free pose. No bound is applied (see bounds_status).
        section << ", \"free_camera\": {\"controller\": \"eawr-free-camera-v1\", "
            << "\"provenance\": " << json(camera_input::project_authored_provenance)
            << ", \"settings\": ";
        if (table && table->free_camera) {
            const tactical_camera::FreeCameraSettings& settings = *table->free_camera;
            section << "{\"move_speed\": " << settings.move_speed << ", \"vertical_speed\": "
                << settings.vertical_speed << ", \"look_degrees_per_unit\": "
                << settings.look_degrees_per_unit << ", \"pitch_min_degrees\": "
                << settings.pitch_min_degrees << ", \"pitch_max_degrees\": "
                << settings.pitch_max_degrees << "}";
        } else {
            section << "null";
        }
        section << ", \"active\": " << (run.free_controller ? "true" : "false")
            << ", \"entries\": " << run.free_entries << ", \"exits\": " << run.free_exits
            << ", \"rejections\": " << run.free_rejections << ", \"last_rejection\": "
            << json(run.free_rejection) << ", \"steps\": " << run.free_steps
            << ", \"moving_steps\": " << run.free_moving_steps << ", \"transitions\": [";
        for (std::size_t index = 0; index < run.free_transitions.size(); ++index) {
            if (index != 0) section << ", ";
            section << "{\"kind\": " << json(run.free_transitions[index].first)
                << ", \"frame\": " << run.free_transitions[index].second << "}";
        }
        section << "], \"pose\": ";
        if (run.free_pose) {
            section << "{\"eye\": [" << run.free_pose->eye[0] << ", " << run.free_pose->eye[1]
                << ", " << run.free_pose->eye[2] << "], \"yaw_degrees\": "
                << run.free_pose->yaw_degrees << ", \"pitch_degrees\": "
                << run.free_pose->pitch_degrees << "}";
        } else {
            section << "null";
        }
        section << "}, \"selftest\": {\"enabled\": " << (run.selftest ? "true" : "false")
            << ", \"checks\": [";
        for (std::size_t index = 0; index < run.checks.size(); ++index) {
            if (index != 0) section << ", ";
            section << "{\"name\": " << json(run.checks[index].first) << ", \"passed\": "
                << (run.checks[index].second ? "true" : "false") << "}";
        }
        section << "]}},\n";
        output << section.str();
    }
    output << "  \"atlas_overlay_verified\": " << (atlas_overlay_verified_ ? "true" : "false") << ",\n";
    if (atlas_overlay_) {
        const AtlasOverlay& overlay = *atlas_overlay_;
        output << "  \"atlas\": {\"mtd\": {\"logical_path\": " << json(overlay.mtd_logical_path)
            << ", \"sha256\": " << json(overlay.mtd_sha256) << "}, \"page\": {\"logical_path\": "
            << json(overlay.page_logical_path) << ", \"sha256\": " << json(overlay.page_sha256)
            << ", \"width\": " << overlay.page_width << ", \"height\": " << overlay.page_height
            << ", \"format\": " << json(overlay.page_format) << ", \"source_origin\": "
            << json(overlay.page_origin) << "}, \"icon\": {\"name\": " << json(overlay.icon_name)
            << ", \"rectangle\": {\"x\": " << overlay.rectangle.x << ", \"y\": "
            << overlay.rectangle.y << ", \"width\": " << overlay.rectangle.width
            << ", \"height\": " << overlay.rectangle.height << "}, \"has_alpha\": "
            << (overlay.icon_has_alpha ? "true" : "false") << ", \"flip_x\": "
            << (overlay.icon_flip_x ? "true" : "false") << ", \"flip_y\": "
            << (overlay.icon_flip_y ? "true" : "false") << "}, \"quad\": {\"x\": "
            << overlay.quad_x << ", \"y\": " << overlay.quad_y << ", \"width\": "
            << overlay.quad_width << ", \"height\": " << overlay.quad_height << ", \"scale\": "
            << overlay.scale << ", \"filter\": \"nearest\"}, \"evidence\": {\"opaque_sampled\": "
            << overlay.opaque_sampled << ", \"opaque_non_background\": "
            << overlay.opaque_non_background << ", \"transparent_sampled\": "
            << overlay.transparent_sampled << ", \"transparent_background\": "
            << overlay.transparent_background << ", \"distinct_opaque_colours\": "
            << overlay.distinct_opaque_colours << ", \"orientation_consistent\": "
            << (overlay.orientation_consistent ? "true" : "false") << ", \"verified\": "
            << (atlas_overlay_verified_ ? "true" : "false") << "}},\n";
    }
    if (model_preview_ && model_preview_->plan.exploratory) {
        const ModelPreview& preview = *model_preview_;
        const auto vector = [](const std::array<float, 3>& value) {
            std::ostringstream text;
            text << '[' << value[0] << ", " << value[1] << ", " << value[2] << ']';
            return text.str();
        };
        const presentation::FixedCamera& camera = preview.fit.camera;
        output << "  \"model_preview\": {\"kind\": " << json(model_preview::to_string(preview.plan.kind))
            << ", \"label\": \"production renderer exploratory preview\""
            << ", \"material_fidelity\": \"unqualified\""
            << ", \"ship_effects\": \"not included: Hull mesh only; no engine glow or exhaust, "
               "lights, hangar, shield, shadow, particles or hardpoints\""
            << ", \"exploratory\": true, \"acceptance\": false"
            << ", \"mesh\": {\"name\": " << json(preview.mesh_name) << ", \"bone\": "
            << preview.mesh_bone << ", \"submesh\": " << preview.submesh_index
            << ", \"vertex_count\": " << preview.rest.vertex_count << ", \"index_count\": "
            << preview.index_count << "}, \"material\": {\"program\": " << json(selected_program_)
            << ", \"technique\": " << json(preview.technique) << ", \"pass\": "
            << json(preview.pass) << ", \"base_texture\": " << json(preview.base_texture)
            << "}, \"model\": {\"logical_path\": " << json(selected_model_path_)
            << ", \"sha256\": " << json(selected_model_hash_) << ", \"expected_sha256\": "
            << json(preview.plan.expected_model_sha256) << "}, \"texture\": {\"logical_path\": "
            << json(selected_texture_path_) << ", \"sha256\": " << json(selected_texture_hash_)
            << ", \"expected_sha256\": " << json(preview.plan.expected_texture_sha256)
            << "}, \"rest_hierarchy\": {\"chain\": [";
        for (std::size_t index = 0; index < preview.rest.chain.size(); ++index) {
            if (index != 0) output << ", ";
            output << json(preview.rest.chain[index]);
        }
        output << "], \"max_placement_delta\": " << preview.rest.max_placement_delta
            << ", \"tolerance\": " << preview.rest.tolerance << ", \"trusted\": true}"
            << ", \"bounds\": {\"asset_min\": " << vector(preview.rest.asset.min)
            << ", \"asset_max\": " << vector(preview.rest.asset.max) << ", \"world_min\": "
            << vector(preview.world.min) << ", \"world_max\": " << vector(preview.world.max)
            << "}, \"camera\": {\"policy\": " << json(model_preview::to_string(preview.plan.camera))
            << ", \"view_direction\": " << vector(preview.fit.view_direction)
            << ", \"fit_margin\": " << model_preview::hull_fit_margin
            << ", \"viewport\": {\"width\": " << camera.width << ", \"height\": "
            << camera.height << "}, \"position\": " << vector(camera.eye) << ", \"target\": "
            << vector(camera.target) << ", \"up\": " << vector(camera.up)
            << ", \"fov_degrees\": " << camera.vertical_fov_degrees << ", \"near\": "
            << camera.near_plane << ", \"far\": " << camera.far_plane
            << ", \"corner_ndc\": {\"min_x\": " << preview.fit.ndc_min_x << ", \"max_x\": "
            << preview.fit.ndc_max_x << ", \"min_y\": " << preview.fit.ndc_min_y
            << ", \"max_y\": " << preview.fit.ndc_max_y << "}, \"corner_depth\": ["
            << preview.fit.depth_min << ", " << preview.fit.depth_max << "]}"
            << ", \"capture\": {\"width\": " << preview.capture_width << ", \"height\": "
            << preview.capture_height << ", \"drawn_pixels\": " << preview.drawn_pixels
            << ", \"drawn_bounds\": [" << preview.drawn_min_x << ", " << preview.drawn_min_y
            << ", " << preview.drawn_max_x << ", " << preview.drawn_max_y
            << "], \"inside_viewport\": " << (preview.capture_verified ? "true" : "false")
            << "}},\n";
    }
    output << "  \"pass_order\": [";
    for (std::size_t index = 0; index < observed_passes.size(); ++index) {
        if (index != 0) output << ", ";
        output << json(eawr::presentation::to_string(observed_passes[index]));
    }
    output << "]\n}\n";
    output.close();
    if (!output) {
        UtilityFunctions::printerr("EAWR viewer report output could not be written completely");
        return false;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
