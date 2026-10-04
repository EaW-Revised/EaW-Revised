#include "viewer_path.hpp"
#include "map_mode_internal.hpp"
#include "startup_trace.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {

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

void MapMode::State::write_report_population(std::ostream& output) const {
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
    output << "},\n";
}

void MapMode::State::write_report_effects(std::ostream& output, const std::uint64_t particle_allocation, const std::uint64_t particle_live_at_capture) const {
    output << "  \"map_particles\": {\"enabled\": " << (options.map_effects ? "true" : "false")
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
}

} // namespace eawr::presentation::godot_backend
