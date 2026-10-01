#include "viewer_path.hpp"
#include "map_mode_internal.hpp"
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

namespace {

[[nodiscard]] std::string_view effect_status(const particles::MapEffectStatus status) {
    constexpr std::array<std::string_view, 4> names{"admitted", "hidden", "unresolved", "unsupported"};
    return names.at(static_cast<std::size_t>(status));
}

[[nodiscard]] std::string_view effect_cause(const particles::MapEffectCause cause) {
    constexpr std::array<std::string_view, 24> names{
        "none", "missing_placement", "missing_model", "model_mismatch", "missing_transform",
        "missing_reference", "reference_mismatch", "unresolved_reference", "unknown_effect_kind",
        "non_particle_reference", "unknown_visibility", "hidden_proxy", "hidden_bone",
        "malformed_variant_tag", "missing_alt_selection", "missing_lod_selection",
        "alt_mismatch", "lod_mismatch", "missing_bone", "missing_frame", "zero_capacity",
        "capacity_exhausted", "frame_overflow", "hardpoint_state"};
    return names.at(static_cast<std::size_t>(cause));
}

} // namespace

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
void MapMode::State::write_attached_owner(std::ostream& output, const std::size_t index,
    const particles::MapEffectRecord& record) const {
    const particles::MapAttachmentOwner* owner = particles ? particles->owner_for(index) : nullptr;
    const auto clip = attached_clips.find(record.scene_ordinal);
    const scene::Placement* placement = nullptr;
    if (scene) {
        const auto found = std::find_if(scene->placements.begin(), scene->placements.end(),
            [&](const scene::Placement& entry) { return entry.scene_ordinal == record.scene_ordinal; });
        if (found != scene->placements.end()) placement = &*found;
    }
    output << ", \"owner\": {\"kind\": " << json(owner ? "idle_clip"
            : record.status == particles::MapEffectStatus::admitted ? "static" : "none")
        << ", \"idle_animation\": " << json(placement ? placement->idle_animation : "")
        << ", \"idle_animation_status\": " << json(placement ? placement->idle_animation_status : "")
        << ", \"idle_animation_provenance\": ";
    if (placement) {
        const scene::Provenance& provenance = placement->idle_animation_provenance;
        output << "{\"tag\": " << json(provenance.tag)
            << ", \"source_object_id\": " << json(provenance.source_object_id)
            << ", \"logical_path\": " << json(provenance.logical_path)
            << ", \"line\": " << provenance.line << "}";
    } else {
        output << "null";
    }
    output << ", \"clip_status\": " << json(clip != attached_clips.end() ? clip->second.status : "not_applicable")
        << ", \"clip_detail\": " << json(clip != attached_clips.end() ? clip->second.detail : "")
        << ", \"mesh_pose\": \"bind\"";
    if (particles) {
        if (const auto edges = particles->watch_edges(index)) {
            output << ", \"unadmitted_visible_edges\": " << *edges;
        }
    }
    if (owner) {
        const particles::AttachmentLifecycle& life = owner->lifecycle();
        output << ", \"policy\": " << json(particles::to_string(life.policy()))
            << ", \"max_draining\": " << particles::map_owner_max_draining
            << ", \"bone\": " << owner->bone()
            << ", \"samples\": " << owner->samples()
            << ", \"generations\": " << life.generations()
            << ", \"detaches\": " << life.detaches()
            << ", \"drains_released\": " << life.drains_released()
            << ", \"drains_cut_short\": " << life.drains_cut_short()
            << ", \"peak_live_instances\": " << owner->peak_live_instances()
            << ", \"released\": " << (owner->released() ? "true" : "false")
            << ", \"events\": [";
        const auto events = owner->events();
        for (std::size_t event = 0; event < events.size(); ++event) {
            const particles::MapOwnerEvent& entry = events[event];
            output << (event ? ", " : "") << "{\"sample\": " << entry.sample
                << ", \"visible\": " << (entry.visible ? "true" : "false")
                << ", \"spawned_generation\": ";
            if (entry.spawned_generation) output << *entry.spawned_generation; else output << "null";
            output << ", \"detached\": ";
            if (entry.detached) output << json(particles::to_string(*entry.detached)); else output << "null";
            output << ", \"drains_released\": " << entry.drains_released
                << ", \"drains_cut_short\": " << entry.drains_cut_short
                << ", \"live_instances\": " << entry.live_instances << "}";
        }
        output << "]";
    }
    output << "}";
}

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
    if (hud) output << "  \"hud\": " << hud->report_json() << ",\n";
    output << "  \"perf_overlay\": " << perf_report_json() << ",\n";
    const lighting::bloom::SceneBloom bloom_values = scene_bloom.value_or(lighting::bloom::SceneBloom{});
    output << "  \"scene_bloom\": {\"status\": "
        << json(!scene_bloom ? (bloom_skipped_unlit ? "lighting_off" : "off") : !renderer ? "renderer_not_created"
                : scene_bloom_applied ? "on" : "unsupported_backend")
        << ", \"source\": " << json(environment_records > 0 ? "environment " + std::to_string(environment_record) : std::string("loader defaults"))
        << ", \"strength\": " << bloom_values.strength << ", \"cutoff\": " << bloom_values.cutoff
        << ", \"size\": " << bloom_values.size << ", \"evidence_frame\": " << json(configured_frame())
        << ", \"rule\": \"docs/rendering.md#bloom\"},\n"
        << "  \"terrain\": {\"chunks\": " << chunk_count
        << ", \"uploaded_surfaces\": " << uploaded_surfaces
        << ", \"vertices\": " << vertex_count
        << ", \"triangles\": " << triangle_count
        << ", \"blend\": {\"adapter\": " << json(land_look::terrain_adapter_id)
        << ", \"layers\": " << terrain_blend.layers
        << ", \"layer_edge\": " << terrain_blend.layer_edge
        << ", \"unresolved_layers\": " << terrain_blend.unresolved_layers << "}},\n"
        << "  \"material_slots\": [";
    for (std::size_t index = 0; index < slots.size(); ++index) {
        const SlotRecord& slot = slots[index];
        output << (index == 0 ? "\n    " : ",\n    ")
            << "{\"slot\": " << static_cast<unsigned>(slot.slot)
            << ", \"cells\": " << slot.cells
            << ", \"effect_program\": " << json(slot.effect_program)
            << ", \"effect_technique\": " << json(slot.effect_technique)
            << ", \"effect_pass\": " << json(slot.effect_pass)
            << ", \"declared_primary\": " << json(slot.declared_primary)
            << ", \"resolved_primary\": " << json(slot.resolved_primary)
            << ", \"declared_secondary\": " << json(slot.declared_secondary)
            << ", \"resolved_secondary\": " << json(slot.resolved_secondary)
            << ", \"texture_resolved\": " << (slot.texture_resolved ? "true" : "false")
            // The legacy route's selector allowlist has no TERRAIN entry, so
            // the recorded selector is what the surface would bind once that
            // route carries one; today it fails closed and the surface is drawn
            // through the clean-room modern_spatial adapter instead.
            << ", \"legacy_route_supported\": false}";
    }
    output << (slots.empty() ? "" : "\n  ") << "],\n"
        << "  \"skydome\": {\"object_id\": " << json(skydome_object)
        << ", \"model_logical_path\": " << json(skydome_model_path)
        << ", \"model_sha256\": " << json(skydome_model_hash)
        << ", \"status\": " << json(skydome_status)
        << ", \"drawn\": " << (skydome_drawn ? "true" : "false")
        << ", \"effect_program\": \"Skydome.fx\", \"effect_technique\": \"sph_t2\""
        << ", \"effect_pass\": \"sph_t2_p0\", \"legacy_route_supported\": false},\n"
        << "  \"water\": {\"records\": " << water_records
        << ", \"status\": " << json(water_status)
        << ", \"cause\": " << json(water_cause)
        << ", \"plane_family\": " << (water.plane ? water.plane->family : 0U)
        << ", \"plane_height\": " << (water.plane ? water.plane->height : 0.0F)
        << ", \"plane_drawn\": " << (water.plane_drawn ? "true" : "false")
        << ", \"rivers_drawn\": " << water.rivers_drawn
        << ", \"water_decoration_tracks_drawn\": " << water.water_decoration_tracks_drawn
        << ", \"terrain_tracks_drawn\": " << water.terrain_tracks_drawn
        << ", \"rivers_skipped\": " << water.rivers_skipped << "},\n"
        << "  \"nebula\": {\"status\": " << json(nebula_status) << "},\n"
        << "  \"environment_records\": " << environment_records << ",\n"
        << "  \"adapters\": {\"route\": \"modern_spatial\", \"terrain\": \"eawr-terrain-surface-v1\","
           " \"skydome\": \"eawr-skydome-v1\"},\n";
    {
        const auto write_vector = [&](const std::array<float, 3>& value) {
            output << '[' << value[0] << ", " << value[1] << ", " << value[2] << ']';
        };
        output << "  \"lighting\": {\"policy\": " << json(lighting::to_string(policy))
            << ", \"environment\": {\"requested\": " << json(environment_choice)
            << ", \"record\": " << environment_record << ", \"record_name\": " << json(environment_record_name)
            << ", \"source\": " << json(lighting::to_string(environment.source)) << ", \"lights\": [";
        for (std::size_t index = 0; index < 3; ++index) {
            const auto& light = environment.lights[index];
            output << (index == 0 ? "" : ", ") << "{\"color\": ";
            write_vector({light.color.r, light.color.g, light.color.b});
            output << ", \"intensity\": " << light.color.a << ", \"direction_source_basis\": ";
            write_vector({light.direction.x, light.direction.y, light.direction.z});
            output << '}';
        }
        output << "], \"ambient\": ";
        write_vector({environment.ambient.r, environment.ambient.g, environment.ambient.b});
        output << ", \"shadow_color\": ";
        write_vector({environment.shadow.r, environment.shadow.g, environment.shadow.b});
        output << "}, \"sh_light_all_coefficients\": [";
        const auto coefficients = lighting::project_lights(
            std::span<const lighting::DirectionalLight>(environment.lights.data(), 3));
        for (std::size_t channel = 0; channel < 3; ++channel) {
            output << (channel == 0 ? "[" : ", [");
            for (std::size_t index = 0; index < 9; ++index) {
                output << (index == 0 ? "" : ", ") << coefficients.rgb[channel][index];
            }
            output << ']';
        }
        output << "], \"irradiance_matrices_render_basis\": [";
        for (std::size_t channel = 0; channel < 3; ++channel) {
            output << (channel == 0 ? "[" : ", [");
            for (std::size_t index = 0; index < 16; ++index) {
                output << (index == 0 ? "" : ", ") << configured_lighting.sph[channel][index];
            }
            output << ']';
        }
        output << "], \"toward_light_render_basis\": ";
        write_vector(configured_lighting.toward_light);
        output << ", \"terrain_adapter\": "
            << json(policy == lighting::Policy::off ? "eawr-terrain-surface-v1" : "eawr-terrain-lit-v1")
            << ",\n    \"shadows\": {\"requested\": " << (shadows ? "true" : "false")
            << ", \"backend\": \"Godot RenderingServer directional light\", \"mode\": "
            << json(to_string(configured_lighting.shadow_layout));
        if (configured_lighting.shadow_layout != GodotRenderer::ShadowLayout::orthogonal) {
            output << ", \"split_offsets\": ";
            write_vector(configured_lighting.shadow_split_offsets);
            output << ", \"blend_splits\": " << (configured_lighting.shadow_blend_splits ? "true" : "false");
        }
        output << ", \"atlas_size\": " << configured_lighting.shadow_atlas_size
            << ", \"filter\": " << json(to_string(configured_lighting.shadow_filter))
            << ", \"stabilization\": \"godot_sphere_texel_snap\""
            << ", \"max_distance\": " << configured_lighting.shadow_max_distance
            << ", \"bias\": " << configured_lighting.shadow_bias.value_or(0.0F)
            << ", \"normal_bias\": " << configured_lighting.shadow_normal_bias.value_or(0.0F)
            << ", \"blur\": " << configured_lighting.shadow_blur.value_or(0.0F)
            << ", \"shadow_floor\": ";
        write_vector(configured_lighting.shadow_floor);
        output << ", \"terrain_casts_shadows\": false"
            << ", \"skydome_casts_shadows\": " << (skydome_drawn ? (skydome_casts_shadows ? "true" : "false") : "null")
            << ", \"shadow_receiving_legacy_materials\": "
            << (renderer ? renderer->shadow_receiving_materials() : 0U)
            << ", \"shadow_variant_failures\": " << (renderer ? renderer->shadow_variant_failures() : 0U)
            << ", \"evidence\": {\"status\": " << json(shadow_evidence_status)
            << ", \"regions\": " << shadow_regions << ", \"region_pixels\": " << shadow_region_pixels
            << ", \"region_luminance_on\": " << shadow_region_luminance_on
            << ", \"region_luminance_off\": " << shadow_region_luminance_off
            << ", \"control_luminance_on\": " << shadow_control_luminance_on
            << ", \"control_luminance_off\": " << shadow_control_luminance_off
            << ", \"criterion\": " << json(shadow_criterion)
            << ", \"hull_pixels\": " << shadow_hull_pixels << ", \"hull_darkened\": " << shadow_hull_darkened
            << ", \"control_pixels\": " << shadow_control_pixels
            << ", \"control_darkened\": " << shadow_control_darkened << "}},\n"
            << "    \"policy_luminance\": {\"status\": " << json(policy_evidence_status);
        for (const auto& [name, value] : policy_luminance) {
            output << ", " << json(name) << ": {\"mean\": " << value.mean
                << ", \"saturated_fraction\": " << value.saturated_fraction << ", \"pixels\": " << value.pixels << '}';
        }
        output << "}, \"captures\": {";
        bool first = true;
        for (const auto& [name, hash] : capture_hashes) {
            output << (first ? "" : ", ") << json(name) << ": " << json(hash);
            first = false;
        }
        output << "}},\n";
    }
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
    output << "  \"populate\": {\"requested\": " << (populate ? "true" : "false");
    if (scene) {
        output << ", \"scene_contract_version\": " << scene->contract_version
            << ", \"scene_sha256\": " << json(scene->scene_sha256)
            << ", \"placements\": " << scene->placements.size()
            << ", \"resolved\": " << scene->resolved_count()
            << ", \"drawable\": " << scene->drawable_count()
            << ", \"drawn\": " << unit_bounds.size()
            << ", \"unresolved\": " << (scene->placements.size() - scene->resolved_count())
            << ", \"scene_assets\": " << scene->assets.size()
            << ", \"unresolved_by_cause\": {";
        bool first = true;
        for (const scene::Cause cause : scene::all_causes()) {
            output << (first ? "" : ", ") << json(scene::to_string(cause)) << ": " << scene->count(cause);
            first = false;
        }
        output << "}, \"unresolved_detail_count_basis\": "
            << json("placements per cause and detail; a placement can occur in multiple detail groups")
            << ", \"unresolved_details\": {";
        const auto groups = scene::issue_groups(*scene);
        bool first_cause = true;
        for (const scene::Cause cause : scene::all_causes()) {
            if (scene->count(cause) == 0) continue;
            output << (first_cause ? "" : ", ") << json(scene::to_string(cause)) << ": [";
            bool first_group = true;
            for (const scene::IssueGroup& group : groups) {
                if (group.cause != cause) continue;
                output << (first_group ? "" : ", ") << "{\"object_type\": " << json(group.object_id)
                    << ", \"model_path\": " << json(group.model_path)
                    << (cause == scene::Cause::shader_unsupported ? ", \"shader\": " : ", \"detail\": ")
                    << json(group.detail) << ", \"count\": " << group.count << '}';
                first_group = false;
            }
            output << ']';
            first_cause = false;
        }
        std::map<std::string, std::uint64_t> shaders;
        for (const scene::Placement& placement : scene->placements) {
            if (!placement.drawable()) continue;
            for (const scene::Surface& surface : placement.surfaces) {
                if (!surface.supported) ++shaders[surface.shader];
            }
        }
        output << "}, \"unsupported_surfaces_on_drawn_placements\": {";
        first = true;
        for (const auto& [shader, count] : shaders) {
            output << (first ? "" : ", ") << json(shader) << ": " << count;
            first = false;
        }
        output << "}, \"renderer_assets\": " << renderer_assets
            << ", \"surfaces_uploaded\": " << surfaces_uploaded
            << ", \"surfaces_unsupported\": " << surfaces_unsupported
            << ", \"surfaces_failed\": " << surfaces_failed
            << ", \"surface_failure\": " << json(first_surface_failure)
            << ", \"instances\": " << unit_instances
            << ", \"skinned_instances\": " << skinned_instances;
        // #32: drawn placements by team colour status, the faction colours in
        // use, and the unit idle clips.
        output << ", \"team_colour\": {\"source\": \"TED placement mini 2 owner -> faction in catalog load order -> <Color>\""
            << ", \"colour_variants\": " << team_colour_variants << ", \"drawn_by_status\": {";
        first = true;
        for (const auto& [colour_status, count] : team_colour_by_status) {
            output << (first ? "" : ", ") << json(colour_status) << ": " << count;
            first = false;
        }
        output << "}, \"faction_colours\": {";
        first = true;
        for (const auto& [faction, colour] : team_colour_factions) {
            output << (first ? "" : ", ") << json(faction) << ": [" << static_cast<unsigned>(colour[0]) << ", "
                << static_cast<unsigned>(colour[1]) << ", " << static_cast<unsigned>(colour[2]) << "]";
            first = false;
        }
        output << "}}, \"unit_animation\": {\"clips\": " << unit_clips.size()
            << ", \"placements_animated\": " << units_animated
            << ", \"instances_animated\": " << animated_instances.size()
            << ", \"sample_at_capture\": " << (unit_sample ? std::to_string(*unit_sample) : std::string("null"))
            << ", \"clock\": " << json(options.interactive ? "live" : "held")
            << ", \"idle_offset\": " << idle_offset.value_or(0U)
            << ", \"idle_sample_failures\": " << idle_sample_failures
            << ", \"idle_rule\": " << json(idle_rule)
            << ", \"skinned_placements_by_clip_status\": {";
        first = true;
        for (const auto& [clip_status, count] : unit_clip_status) {
            output << (first ? "" : ", ") << json(clip_status) << ": " << count;
            first = false;
        }
        // #157: each animated object type, its clip and the playback its XML declares.
        output << "}, \"objects\": [";
        first = true;
        for (const auto& [object, idle] : idle_objects) {
            output << (first ? "" : ", ") << "{\"object\": " << json(object) << ", \"clip\": " << json(idle.clip)
                << ", \"placements\": " << idle.placements
                << ", \"loop\": " << (idle.playback.loop ? "true" : "false")
                << ", \"restarts\": " << (idle.playback.restarts ? "true" : "false")
                << ", \"random_start\": " << (idle.playback.random_start ? "true" : "false")
                << ", \"rate_mod\": [" << idle.playback.rate.numerator << ", " << idle.playback.rate.denominator << ']';
            if (const auto rejected = idle_rate_rejected.find(object); rejected != idle_rate_rejected.end()) {
                output << ", \"rate_mod_rejected\": " << json(rejected->second);
            }
            output << '}';
            first = false;
        }
        // #147: the scene wind and clock that bend Tree.fx and Grass.fx surfaces.
        const assets::Vec3f wind_vector = wind ? wind->vector : assets::Vec3f{};
        output << "]}, \"wind\": {\"source\": " << json(wind ? "environment 0" : "none")
            << ", \"heading_degrees\": " << (wind ? wind->heading_degrees : 0.0F)
            << ", \"speed\": " << (wind ? wind->speed : 0.0F)
            << ", \"vector\": [" << wind_vector.x << ", " << wind_vector.y << ", " << wind_vector.z << ']'
            << ", \"clock\": " << json(options.interactive ? "live" : "held")
            << ", \"scene_time_seconds\": " << wind_clock_seconds
            << ", \"tree_surfaces\": " << wind_tree_surfaces
            << ", \"grass_surfaces\": " << wind_grass_surfaces
            << ", \"sway_widened_placements\": " << wind_widened_placements
            << ", \"rule\": \"docs/behaviour/vegetation-effects.md W-01..W-09\"}"
            << ", \"transform_basis\":\"scene: TED source Q24; snapshot: exact signed-permutation conjugation (x, y, z) -> (x, z, -y)\""
            << ", \"evidence\": {\"terrain_only_capture_sha256\": " << json(terrain_only_capture_hash)
            << ", \"units_projected\": " << units_projected
            << ", \"units_with_coverage\": " << units_with_coverage;
        if (fog) {
            output << ", \"units_expected_hidden\": " << units_expected_hidden
                << ", \"units_expected_visible\": " << units_expected_visible
                << ", \"unit_submissions_and_fog_materials_verified\": "
                << (fog_unit_submissions_verified ? "true" : "false");
        }
        output << ", \"changed_pixels_inside_unit_bounds\": " << changed_inside_pixels
            << ", \"changed_pixels_outside_unit_bounds\": " << changed_outside_pixels
            << ", \"pixels_outside_unit_bounds\": " << outside_pixels
            << ", \"verified\": " << (unit_evidence_verified ? "true" : "false") << "}";
    }
    output << "},\n"
        << "  \"map_particles\": {\"enabled\": " << (options.map_effects ? "true" : "false")
        << ", \"presentation_only\": true, \"seed\": " << options.particle_seed
        << ", \"total_capacity\": " << options.particle_capacity
        << ", \"allocated_capacity\": " << particle_allocation
        << ", \"particles_at_capture\": " << particle_live_at_capture
        << ", \"target_frames\": " << options.particle_frames
        << ", \"advanced_frames\": " << (particles ? particles->frames() : 0)
        << ", \"clock\": " << json(options.interactive ? "live" : "held")
        << ", \"declared_particle_models\": "
        << (scene ? scene->count(scene::Cause::model_particle_system) : 0)
        << ", \"omitted_by_request\": "
        << (!options.map_effects && scene ? scene->count(scene::Cause::model_particle_system) : 0)
        << ", \"effects_off_capture_sha256\": " << json(capture_hashes.contains("effects_off")
            ? capture_hashes.at("effects_off") : "")
        << ", \"changed_pixels_outside_bounds\": " << particle_changed_outside
        << ", \"evidence_verified\": " << (particle_evidence_verified ? "true" : "false")
        << ", \"empty_placements_at_capture\": " << particle_placements_empty
        << ", \"outside_view_placements_at_capture\": " << particle_placements_outside_view
        << ", \"subpixel_heat_placements_at_capture\": " << particle_placements_subpixel_heat
        << ", \"live_rids_at_capture\": " << particle_rids_at_capture
        << ", \"live_resources_at_capture\": " << particle_resources_at_capture
        << ", \"live_rids_after_release\": " << particle_rids_after_release
        << ", \"live_resources_after_release\": " << particle_resources_after_release
        << ", \"fog_consumers_at_capture\": " << particle_fog_consumers_at_capture
        << ", \"fog_consumers_after_release\": " << particle_fog_consumers_after_release
        << ", \"fog_bound_at_capture\": " << (particle_fog_bound_at_capture ? "true" : "false")
        << ", \"placements\": [";
    if (particles) {
        for (std::size_t index = 0; index < particles->placements().size(); ++index) {
            const ParticlePlacement& placement = particles->placements()[index];
            output << (index ? ", " : "") << "{\"identity\": " << json(placement.identity)
                << ", \"object_id\": " << json(placement.object_id)
                << ", \"record_ordinal\": " << placement.record_ordinal
                << ", \"logical_path\": " << json(placement.logical_path)
                << ", \"classification\": " << json(placement.attached
                    ? "admitted_attached_effect" : placement.confirmed_particle_model
                    ? "confirmed_particle_system" : "unresolved_model_kind")
                << ", \"source_sha256\": " << json(placement.sha256)
                << ", \"seed\": " << placement.seed
                << ", \"capacity\": " << placement.capacity
                << ", \"scale_raw\": " << placement.scale_raw
                << ", \"transform_raw\": [";
            for (std::size_t element = 0; element < placement.transform_raw.size(); ++element) {
                output << (element ? ", " : "") << placement.transform_raw[element];
            }
            output << "], \"status\": " << json(placement.status)
                << ", \"bounds_min\": [" << placement.bounds_min[0] << ", "
                << placement.bounds_min[1] << ", " << placement.bounds_min[2] << "]"
                << ", \"bounds_max\": [" << placement.bounds_max[0] << ", "
                << placement.bounds_max[1] << ", " << placement.bounds_max[2] << "]"
                << ", \"causes\": [";
            for (std::size_t cause = 0; cause < placement.causes.size(); ++cause) {
                output << (cause ? ", " : "") << json(placement.causes[cause]);
            }
            output << "], \"stream_hash\": " << json(particles::hex64(placement.stats.hash))
                << ", \"particles\": " << placement.stats.particles
                << ", \"changed_pixels\": " << placement.changed_pixels
                << ", \"emitters\": [";
            for (std::size_t emitter_index = 0; emitter_index < placement.emitters.size(); ++emitter_index) {
                const auto& emitter = placement.emitters[emitter_index];
                output << (emitter_index ? ", " : "") << "{\"index\": " << emitter.index
                    << ", \"blend\": " << json(emitter.blend)
                    << ", \"resource\": " << emitter.resource
                    << ", \"fog_handle\": " << emitter.fog_handle
                    << ", \"quads\": " << emitter.quads
                    << ", \"maximum_alpha\": " << emitter.maximum_alpha
                    << ", \"fog_bound\": " << (emitter.fog_bound ? "true" : "false")
                    << ", \"attenuation_bytes\": [" << static_cast<unsigned>(emitter.minimum_attenuation)
                    << ", " << static_cast<unsigned>(emitter.maximum_attenuation) << "]";
                if (fog) {
                    output << ", \"fog_source_sha256\": " << json(emitter.fog_source_sha256)
                        << ", \"fog_team\": " << emitter.fog_team
                        << ", \"fog_tick\": " << emitter.fog_tick
                        << ", \"fog_revision\": " << emitter.fog_revision;
                }
                output << "}";
            }
            output << "]}";
        }
    }
    output << "]},\n"
        << "  \"map_attached_effects\": {\"selected_alt\": ";
    if (options.effect_alt) output << *options.effect_alt; else output << "null";
    output << ", \"selected_lod\": ";
    if (options.effect_lod) output << *options.effect_lod; else output << "null";
    output << ", \"requested_capacity_per_proxy\": " << options.attached_capacity
        << ", \"aggregate_capacity\": " << attached_plan.aggregate_capacity
        << ", \"allocated_capacity\": " << attached_plan.allocated_capacity;
    if (effect_animation_idle()) {
        // Present only under --eawr-map-effect-animation idle, so the default
        // report is byte-for-byte what it was.
        output << ", \"effect_animation\": \"idle\""
            << ", \"presentation_clock\": \"sample n at n/30 s, loop, exact integer frame position\""
            << ", \"attached_budget\": " << attached_budget
            << ", \"drain_headroom\": " << attached_drain_headroom
            << ", \"live_capacity_limit\": " << attached_owners.live_capacity_limit
            << ", \"policy\": \"respawn\", \"max_draining\": " << particles::map_owner_max_draining
            << ", \"mesh_pose\": \"bind\""
            << ", \"animation_failure\": " << json(attached_animation_failure)
            << ", \"owners_empty_at_capture\": " << particle_owners_empty;
    }
    output << ", \"records\": [";
    for (std::size_t index = 0; index < attached_plan.records.size(); ++index) {
        const auto& record = attached_plan.records[index];
        const ParticlePlacement* runtime = nullptr;
        if (particles) {
            for (const auto& placement : particles->placements()) {
                if (placement.attached && placement.effect_plan_index == index) {
                    runtime = &placement;
                    break;
                }
            }
        }
        output << (index ? ", " : "") << "{\"scene_ordinal\": " << record.scene_ordinal
            << ", \"map_logical_path\": " << json(record.map_logical_path)
            << ", \"record_ordinal\": " << record.record_ordinal
            << ", \"proxy_ordinal\": " << record.proxy_ordinal
            << ", \"proxy_name\": " << json(record.proxy_name)
            << ", \"model_logical_path\": " << json(record.model_logical_path)
            << ", \"model_source_id\": " << json(record.model_source.source_id)
            << ", \"model_layer_id\": " << json(record.model_source.layer_id)
            << ", \"model_provenance\": {\"tag\": " << json(record.model_provenance.tag)
            << ", \"source_object_id\": " << json(record.model_provenance.source_object_id)
            << ", \"logical_path\": " << json(record.model_provenance.logical_path)
            << ", \"line\": " << record.model_provenance.line << "}"
            << ", \"effect_logical_path\": " << json(record.effect_logical_path)
            << ", \"alternate_suffix_removed\": "
            << (record.alternate_suffix_removed ? "true" : "false")
            << ", \"status\": " << json(effect_status(record.status))
            << ", \"cause\": " << json(effect_cause(record.cause))
            << ", \"detail\": " << json(record.detail)
            << ", \"seed\": " << record.seed
            << ", \"capacity\": " << record.capacity
            << ", \"emitter_origin\": ";
        if (record.emitter_frame) {
            const auto emitter_frame = particles::source_emitter_frame(*record.emitter_frame);
            output << "[" << emitter_frame.origin.x << ", " << emitter_frame.origin.y << ", " << emitter_frame.origin.z << "]";
        } else output << "null";
        output << ", \"runtime_status\": " << json(runtime ? runtime->status :
            (!options.map_effects && record.status == particles::MapEffectStatus::admitted
                ? "omitted_by_request" : "not_spawned"))
            << ", \"runtime_causes\": [";
        if (runtime) {
            for (std::size_t cause = 0; cause < runtime->causes.size(); ++cause)
                output << (cause ? ", " : "") << json(runtime->causes[cause]);
        }
        output << "]"
            << ", \"stream_hash\": " << json(runtime ? particles::hex64(runtime->stats.hash) : "")
            << ", \"changed_pixels\": " << (runtime ? runtime->changed_pixels : 0);
        if (effect_animation_idle()) write_attached_owner(output, index, record);
        output << "}";
    }
    output << "]},\n";
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
