#include "viewer_path.hpp"
#include "map_mode_internal.hpp"
#include "startup_trace.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {

void MapMode::State::write_report_scene(std::ostream& output) const {
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
}

void MapMode::State::write_report_lighting(std::ostream& output) const {
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
}

} // namespace eawr::presentation::godot_backend
