#include "viewer_host_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {
namespace viewer_host_detail {

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
    write_report_camera(output);
    write_report_scene(output);
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
