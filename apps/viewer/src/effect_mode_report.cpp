#include "effect_mode_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {

[[nodiscard]] std::string effect_mode_detail::json(const std::string_view value) {
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
            if (character < 0x20U || character >= 0x7fU) {
                output << "\\u00" << hex[character >> 4U] << hex[character & 15U];
            } else {
                output << static_cast<char>(character);
            }
        }
    }
    output << '"';
    return output.str();
}
namespace {



[[nodiscard]] std::uint64_t drawn_primitives(const particles::EffectFrameStats& stats) {
    std::uint64_t quads{};
    for (const particles::EmitterFrameStats& emitter : stats.emitters) {
        if (emitter.drawn) quads += emitter.quads + emitter.triangles;
    }
    return quads;
}

} // namespace

int EffectMode::State::finish() {
    completed = true;
    evidence.label = "final";
    evidence.frame = records.back().frame;
    auto capture = renderer->capture(camera);
    if (!capture) {
        failure = core::format_diagnostic(capture.error());
        static_cast<void>(write_report());
        return 2;
    }
    evidence.sha256 = hash_bytes(capture.value().png_bytes);
    evidence.verified = verify_capture(capture.value(), records.back(), evidence, empty_capture_allowed("final"));
    if (!evidence.verified && failure.empty()) failure = evidence.failure;
    if (!options.capture_path.empty()) evidence.written = write_png(options.capture_path, capture.value());

    // Release the instance, unless the schedule already did, and prove its
    // RenderingServer resources went with it. A released handle is never
    // released twice.
    bool released = true;
    if (visibility_life) {
        live_rids_before_release = backend->live_rids();
        auto all = visibility_life->release_all();
        released = static_cast<bool>(all);
        visibility_released_at_end = all ? all.value() : 0U;
        lifecycle.live = false;
        released_by = visibility_released_at_end != 0 ? "end_of_run" : "visibility";
    } else if (lifecycle.live) {
        live_rids_before_release = backend->live_rids();
        released = static_cast<bool>(registry->release(handle));
        lifecycle.live = false;
        released_by = detach_frame ? "end_of_run" : "owner";
    } else {
        released_by = lifecycle.released_by;
    }
    live_rids_after_release = backend->live_rids();
    registry_resources_after_release = registry->live_backend_resources();
    const bool clean = released && live_rids_after_release == 0 && registry_resources_after_release == 0;
    if (!clean && failure.empty()) failure = "releasing the effect left live RenderingServer resources";

    bool scheduled = true;
    if (detach_frame) {
        const bool rendered = std::any_of(records.begin(), records.end(),
            [](const FrameRecord& record) { return drawn_primitives(record.stats) != 0; });
        const bool captures = std::all_of(detach_captures.begin(), detach_captures.end(),
            [](const CaptureEvidence& entry) { return entry.verified; });
        if (!lifecycle.result) {
            scheduled = false;
            if (failure.empty()) failure = "the scheduled detach did not take effect";
        } else if (lifecycle.result != replay_result) {
            scheduled = false;
            if (failure.empty()) failure = "the graphical detach result differs from the replay validation";
        } else if (!rendered) {
            scheduled = false;
            if (failure.empty()) failure = "the effect never drew a particle; an empty capture is not a detach outcome";
        } else if (!captures) {
            scheduled = false;
            if (failure.empty()) {
                for (const CaptureEvidence& entry : detach_captures) {
                    if (!entry.verified) {
                        failure = entry.label + " capture: " + entry.failure;
                        break;
                    }
                }
            }
        }
    }
    if (visibility) {
        const bool rendered = std::any_of(records.begin(), records.end(),
            [](const FrameRecord& record) { return drawn_primitives(record.stats) != 0; });
        const bool captures = std::all_of(detach_captures.begin(), detach_captures.end(),
            [](const CaptureEvidence& entry) { return entry.verified; });
        if (visibility_frames != replay_visibility) {
            scheduled = false;
            if (failure.empty()) failure = "the graphical visibility lifecycle differs from the replay validation";
        } else if (!rendered) {
            scheduled = false;
            if (failure.empty()) failure = "the effect never drew a particle; an empty capture is not a visibility outcome";
        } else if (!captures) {
            scheduled = false;
            if (failure.empty()) {
                for (const CaptureEvidence& entry : detach_captures) {
                    if (!entry.verified) {
                        failure = entry.label + " capture: " + entry.failure;
                        break;
                    }
                }
            }
        }
    }
    status = evidence.verified && clean && scheduled ? "effect_render_passed" : "failed";
    if (!write_report()) return 2;
    return status == "effect_render_passed" ? 0 : 2;
}

bool EffectMode::State::write_report() const {
    if (options.report_path.empty()) return true;
    std::error_code error;
    const std::filesystem::path parent = options.report_path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, error);
    if (error) return false;
    std::ofstream output(options.report_path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    const BackendInfo backend_info = renderer
        ? renderer->backend_info() : BackendInfo{"Godot 4.7.2-stable", "headless", {}, {}, {}};
    const double elapsed = records.size() > 1
        ? std::chrono::duration<double>(last_frame - first_frame).count() : 0.0;
    const double per_frame = records.size() > 1 ? elapsed * 1000.0 / static_cast<double>(records.size() - 1) : 0.0;

    std::set<std::string> adapters;
    std::set<std::string> materials;
    std::set<std::string> selectors;
    std::set<std::string> families;
    for (const particles::EmitterRenderPlan& plan : plans) {
        if (plan.family != particles::RenderFamily::unsupported) families.insert(std::string(to_string(plan.family)));
        if (!plan.drawable) continue;
        adapters.insert(std::string(GodotParticleBackend::adapter_name(plan.family)));
        materials.insert(std::string(GodotParticleBackend::material_adapter_name(plan.blend)));
        selectors.insert(std::string(plan.program) + " " + std::string(plan.technique));
    }
    const auto list = [&](const std::set<std::string>& values) {
        std::string text = "[";
        bool first = true;
        for (const std::string& value : values) {
            text += (first ? "" : ", ") + json(value);
            first = false;
        }
        return text + "]";
    };
    bool rendered_before_detach = false;
    for (const FrameRecord& record : records) {
        if (detach_frame && record.frame - 1 < *detach_frame && drawn_primitives(record.stats) != 0) {
            rendered_before_detach = true;
        }
    }
    bool probe_matches = probe_hashes.size() == records.size();
    for (std::size_t index = 0; probe_matches && index < records.size(); ++index) {
        probe_matches = probe_hashes[index] == records[index].stats.hash;
    }

    output << "{\n  \"schema_version\": 1,\n  \"mode\": \"effect\",\n"
        << "  \"status\": " << json(status) << ",\n"
        << "  \"failure\": " << json(failure) << ",\n"
        << "  \"backend\": {\"engine\": " << json(backend_info.engine)
        << ", \"rendering_method\": " << json(backend_info.rendering_method)
        << ", \"adapter_vendor\": " << json(backend_info.adapter_vendor)
        << ", \"adapter_name\": " << json(backend_info.adapter_name)
        << ", \"driver_api\": " << json(backend_info.driver_api) << "},\n"
        << "  \"render_profile\": " << render_profile_report() << ",\n"
        << "  \"profile\": " << json(profile) << ",\n  \"layers\": [";
    for (std::size_t index = 0; index < layers.size(); ++index) output << (index ? ", " : "") << json(layers[index]);
    output << "],\n  \"effect\": {\"logical_path\": " << json(options.effect_path)
        << ", \"sha256\": " << json(effect_hash)
        << ", \"version\": " << json(system.version == particles::AloParticleVersion::legacy_v1 ? "legacy_v1" : "plugin_v2")
        << ", \"emitters\": " << system.emitters.size()
        << ", \"families\": " << list(families) << "},\n"
        << "  \"run\": {\"seed\": " << options.seed << ", \"frames\": " << options.frames
        << ", \"delta_seconds\": " << options.delta_seconds << ", \"capacity\": " << options.capacity
        << ", \"detach_frame\": " << (detach_frame ? std::to_string(*detach_frame) : std::string("null"))
        << ", \"clock\": \"presentation\", \"simulation_snapshot_submitted\": false},\n"
        << "  \"adapters_used\": " << list(adapters) << ",\n"
        << "  \"material_adapters_used\": " << list(materials) << ",\n"
        << "  \"material_selectors\": " << list(selectors) << ",\n"
        << "  \"emitters\": [";
    write_probe_emitters(output);
    write_probe_frames(output);
    output << (records.empty() ? "" : "\n  ") << "],\n"
        << "  \"determinism\": {\"final_stream_hash\": "
        << json(records.empty() ? std::string{} : particles::hex64(records.back().stats.hash))
        << ", \"dry_run_frames\": " << probe_hashes.size()
        << ", \"dry_run_matches\": " << (probe_matches ? "true" : "false") << "},\n"
        << "  \"attachment\": {\"requested\": " << (options.attach.empty() ? "false" : "true")
        << ", \"model_logical_path\": " << json(attach_model)
        << ", \"model_sha256\": " << json(attach_model_hash)
        << ", \"bone\": " << json(attach_bone)
        << ", \"animation_logical_path\": " << json(options.animation_path)
        << ", \"animation_sha256\": " << json(animation_hash)
        << ", \"space\": \"model\", \"origins\": [";
    for (std::size_t index = 0; index < attachment_origins.size(); ++index) {
        const particles::Vec3 origin = attachment_origins[index];
        output << (index ? ", " : "") << "[" << origin.x << ", " << origin.y << ", " << origin.z << "]";
    }
    output << "]},\n"
        << "  \"proxy_mesh\": {\"requested\": " << (options.proxy_name.empty() ? "false" : "true")
        << ", \"host_logical_path\": " << json(options.proxy_host)
        << ", \"name\": " << json(options.proxy_name)
        << ", \"owner_mesh_index\": " << (proxy_mesh ? static_cast<long long>(proxy_mesh->mesh_index) : -1)
        << ", \"proxy_origins\": [";
    for (std::size_t index = 0; index < proxy_origins.size(); ++index) {
        const auto origin = proxy_origins[index];
        output << (index ? ", " : "") << "[" << origin.x << ", " << origin.y << ", " << origin.z << "]";
    }
    output << "]"
        << ", \"mesh_origins\": [";
    for (std::size_t index = 0; index < mesh_origins.size(); ++index) {
        const auto origin = mesh_origins[index];
        output << (index ? ", " : "") << "[" << origin.x << ", " << origin.y << ", " << origin.z << "]";
    }
    output << "]},\n"
        << "  \"lifecycle\": {\"live_rids_before_release\": " << live_rids_before_release
        << ", \"live_rids_after_release\": " << live_rids_after_release
        << ", \"registry_resources_after_release\": " << registry_resources_after_release
        << ", \"released_by\": " << json(released_by) << "},\n"
        << "  \"detach\": {\"requested\": " << (detach_frame ? "true" : "false")
        << ", \"frame\": " << (detach_frame ? std::to_string(*detach_frame) : std::string("null"))
        << ", \"leave_particles\": " << (system.leave_particles ? "true" : "false")
        << ", \"result\": " << json(lifecycle.result ? to_string(*lifecycle.result) : std::string_view{})
        << ", \"replay_result\": " << json(replay_result ? to_string(*replay_result) : std::string_view{})
        << ", \"release_frame\": "
        << (lifecycle.release_index ? std::to_string(*lifecycle.release_index) : std::string("null"))
        << ", \"released_by\": " << json(lifecycle.released_by)
        << ", \"rendered_before_detach\": " << (rendered_before_detach ? "true" : "false")
        << ", \"final_capture_empty\": " << (evidence.check == "empty_after_detach" ? "true" : "false")
        << ", \"captures\": [";
    // Intermediate captures belong to whichever lifecycle scheduled them.
    const auto write_captures = [&](const bool owned) {
        const std::size_t count = owned ? detach_captures.size() : 0U;
        for (std::size_t index = 0; index < count; ++index) {
            const CaptureEvidence& entry = detach_captures[index];
            output << (index == 0 ? "\n    " : ",\n    ")
                << "{\"label\": " << json(entry.label) << ", \"frame\": " << entry.frame
                << ", \"index\": " << entry.frame - 1
                << ", \"check\": " << json(entry.check) << ", \"capture_sha256\": " << json(entry.sha256)
                << ", \"projected_bounds_pixels\": [" << entry.projected[0] << ", " << entry.projected[1] << ", "
                << entry.projected[2] << ", " << entry.projected[3] << "]"
                << ", \"inside_coverage\": " << entry.inside_coverage
                << ", \"inside_drawn_samples\": " << entry.inside_drawn
                << ", \"outside_coverage\": " << entry.outside_coverage
                << ", \"drawn_samples\": " << entry.drawn_samples
                << ", \"written\": " << (entry.written ? "true" : "false")
                << ", \"verified\": " << (entry.verified ? "true" : "false")
                << ", \"failure\": " << json(entry.failure) << "}";
        }
        output << (count == 0 ? "" : "\n  ") << "]";
    };
    write_captures(!visibility);
    output << "},\n"
        << "  \"visibility\": {\"requested\": " << (visibility ? "true" : "false")
        << ", \"policy\": " << json(visibility ? to_string(*visibility) : std::string_view{})
        << ", \"bone\": " << json(visibility_bone_name)
        << ", \"bone_index\": " << (visibility_bone ? std::to_string(*visibility_bone) : std::string("null"))
        << ", \"leave_particles\": " << (system.leave_particles ? "true" : "false")
        << ", \"generations\": " << (visibility_life ? visibility_life->generations() : 0U)
        << ", \"detaches\": " << (visibility_life ? visibility_life->detaches() : 0U)
        << ", \"drains_released\": " << (visibility_life ? visibility_life->drains_released() : 0U)
        << ", \"drains_cut_short\": " << (visibility_life ? visibility_life->drains_cut_short() : 0U)
        << ", \"released_at_end\": " << visibility_released_at_end
        << ", \"peak_live_rids\": " << peak_live_rids
        << ", \"replay_matches\": " << (visibility && visibility_frames == replay_visibility ? "true" : "false")
        << ", \"frames\": [";
    for (std::size_t index = 0; index < visibility_frames.size(); ++index) {
        const VisibilityFrame& entry = visibility_frames[index];
        output << (index == 0 ? "\n    " : ",\n    ")
            << "{\"index\": " << index
            << ", \"visible\": " << (entry.visible ? "true" : "false")
            << ", \"spawned\": " << (entry.spawned ? "true" : "false")
            << ", \"detached\": " << json(entry.detached)
            << ", \"drains_released\": " << entry.drains_released
            << ", \"drains_cut_short\": " << entry.drains_cut_short
            << ", \"live_instances\": " << entry.live_instances
            << ", \"active\": " << (entry.active ? "true" : "false") << "}";
    }
    output << (visibility_frames.empty() ? "" : "\n  ") << "], \"captures\": [";
    write_captures(visibility.has_value());
    output << "},\n"
        << "  \"capture_identity\": {\"viewport\": {\"width\": " << camera.width
        << ", \"height\": " << camera.height << "}, \"png\": "
        << (evidence.width == 0 ? std::string("null") : "{\"width\": " + std::to_string(evidence.width)
            + ", \"height\": " + std::to_string(evidence.height) + "}")
        << ", \"camera\": {\"projection\": \"perspective\""
        << ", \"position\": [" << camera.eye[0] << ", " << camera.eye[1] << ", " << camera.eye[2]
        << "], \"target\": [" << camera.target[0] << ", " << camera.target[1] << ", " << camera.target[2]
        << "], \"up\": [" << camera.up[0] << ", " << camera.up[1] << ", " << camera.up[2]
        << "], \"fov_degrees\": " << camera.vertical_fov_degrees
        << ", \"near\": " << camera.near_plane << ", \"far\": " << camera.far_plane << "}},\n"
        << "  \"frame_time\": {\"frames\": " << records.size()
        << ", \"elapsed_seconds\": " << elapsed
        << ", \"milliseconds_per_frame\": " << per_frame << "},\n"
        << "  \"evidence\": {\"check\": " << json(evidence.check)
        << ", \"capture_sha256\": " << json(evidence.sha256)
        << ", \"projected_bounds_pixels\": [" << evidence.projected[0] << ", " << evidence.projected[1] << ", "
        << evidence.projected[2] << ", " << evidence.projected[3] << "]"
        << ", \"inside_coverage\": " << evidence.inside_coverage
        << ", \"inside_drawn_samples\": " << evidence.inside_drawn
        << ", \"outside_coverage\": " << evidence.outside_coverage
        << ", \"drawn_samples\": " << evidence.drawn_samples
        << ", \"verified\": " << (evidence.verified ? "true" : "false") << "}\n}\n";
    output.close();
    return static_cast<bool>(output);
}

} // namespace eawr::presentation::godot_backend
