#include "space_environment_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {

void SpaceEnvironment::State::write_plan_report(std::ostream& output) const {
    const space::SkyPlan& p = plan;
    output << "  \"space\": {\"slice\": " << json(space::slice_id)
        << ", \"adapter_id\": " << json(space::adapter_id)
        << ", \"route\": \"modern_spatial\", \"control\": " << json(control_label())
        << ", \"control_argument\": " << json(options.control)
        << ",\n    \"load_status\": " << json(load_status)
        << ", \"material_status\": " << json(material_status)
        << ", \"submission_status\": " << json(submission_status)
        << ", \"pixel_evidence_status\": " << json(pixel_status)
        << ", \"environment_complete\": false"
        << ", \"original_comparison_status\": \"not_performed\""
        << ",\n    \"plan_status\": " << json(plan_built ? space::to_string(p.status) : "not_built")
        << ", \"plan_detail\": " << json(p.detail)
        << ", \"catalog_failure\": " << json(catalog_failure)
        << ",\n    \"environment\": {\"selection_policy\": \"environment 0 only (slice policy, not an original rule)\""
        << ", \"index\": " << p.environment.environment_index
        << ", \"count\": " << p.environment.environment_count
        << ", \"name\": " << json(p.environment.environment_name.value_or(""))
        << ", \"primary_sky\": " << json(p.environment.primary_sky)
        << ", \"status\": " << json(space::to_string(p.environment.status)) << ", \"not_rendered\": [";
    for (std::size_t index = 0; index < p.environment.not_rendered.size(); ++index) {
        const space::NotRendered& item = p.environment.not_rendered[index];
        output << (index == 0 ? "" : ", ") << "{\"component\": " << json(item.component)
            << ", \"declared\": " << json(item.declared) << ", \"status\": " << json(item.status)
            << ", \"cause\": " << json(item.cause) << '}';
    }
    output << "]},\n    \"sky_object\": {\"status\": " << json(space::to_string(p.model.status))
        << ", \"source_logical_path\": " << json(p.model.object_source_path)
        << ", \"source_id\": " << json(p.model.object_source_id)
        << ", \"layer_id\": " << json(p.model.object_layer_id)
        << ", \"line\": " << p.model.object_line
        << ", \"declared_model\": " << json(p.model.declared_name)
        << ", \"declared_tag\": " << json(p.model.declared_tag)
        << ", \"selection_rule\": " << json(space::model_selection_rule) << "},\n"
        << "    \"model\": {\"logical_path\": " << json(p.model_logical_path)
        << ", \"sha256\": " << json(p.model_sha256)
        << ", \"source_id\": " << json(p.model_source_id)
        << ", \"layer_id\": " << json(p.model_layer_id)
        << ", \"bones\": " << p.model_bones << ", \"meshes\": " << p.model_meshes << "},\n"
        << "    \"visibility_binding\": " << json(p.visibility ? p.visibility->provider : "unbound")
        << ",\n    \"qualification_rows\": [";
    const auto rows = space::qualifications();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        output << (index == 0 ? "" : ", ") << "{\"shader\": " << json(rows[index].shader)
            << ", \"provenance\": " << json(rows[index].provenance) << '}';
    }
    output << "],\n    \"material_routes\": [";
    const auto routes = additive.enabled ? space::meshadditive_material_routes() : space::material_routes();
    for (std::size_t index = 0; index < routes.size(); ++index) {
        output << (index == 0 ? "" : ", ") << "{\"shader\": " << json(routes[index].shader)
            << ", \"material\": " << json(space::to_string(routes[index].material))
            << ", \"route_id\": " << json(routes[index].material == space::SkyMaterial::meshadditive
                ? space::meshadditive_route_id : space::meshgloss_route_id)
            << ", \"provenance\": " << json(routes[index].provenance) << '}';
    }
}

void SpaceEnvironment::State::write_surfaces_report(std::ostream& output) const {
    const space::SkyPlan& p = plan;
    const auto vec3 = [](const assets::Vec3f& value) {
        return "[" + number(value.x, 9) + ", " + number(value.y, 9) + ", " + number(value.z, 9) + "]";
    };
    const auto vec4 = [](const assets::Vec4f& value) {
        return "[" + number(value.x, 9) + ", " + number(value.y, 9) + ", " + number(value.z, 9) + ", "
            + number(value.w, 9) + "]";
    };
    output << "],\n    \"sky_light_policy\": {\"id\": " << json(sky_light.id) << ", \"sph\": [";
    for (std::size_t channel = 0; channel < 3; ++channel) {
        output << (channel == 0 ? "[" : ", [");
        for (std::size_t element = 0; element < 16; ++element) {
            output << (element == 0 ? "" : ", ") << number(sky_light.sph[channel][element], 9);
        }
        output << ']';
    }
    output << "], \"sph_layout\": \"per channel r, g, b: column-major 4x4, element [column * 4 + row]\""
        << ", \"light_direction\": " << vec3(sky_light.light_direction)
        << ", \"light_specular\": " << vec3(sky_light.light_specular)
        << ", \"light_scale\": " << vec4(sky_light.light_scale)
        << ", \"eye\": \"live camera position (CAMERA_POSITION_WORLD)\""
        << ", \"basis\": \"render (Godot) world\", \"cause\": " << json(sky_light.cause) << "}";
    if (additive.enabled) {
        output << ",\n    \"meshadditive\": {\"control\": " << json(meshadditive_control)
            << ", \"route_id\": " << json(space::meshadditive_route_id)
            << ", \"inputs\": {\"id\": " << json(additive.inputs.id)
            << ", \"time_seconds\": " << number(additive.inputs.time, 9)
            << ", \"light_scale\": " << vec4(additive.inputs.light_scale)
            << ", \"cause\": " << json(additive.inputs.cause) << '}'
            << ", \"render_policy\": {\"id\": " << json(space::meshadditive_render_policy_id) << ", \"states\": [";
        const auto states = space::meshadditive_render_policy();
        for (std::size_t index = 0; index < states.size(); ++index) {
            output << (index == 0 ? "" : ", ") << "{\"state\": " << json(states[index].state)
                << ", \"value\": " << json(states[index].value) << ", \"rule\": " << json(states[index].rule) << '}';
        }
        output << "]}, \"gates\": {\"G-01\": \"open: every billboard bone, mode 7 included, is hierarchy_unsupported\""
            << ", \"G-02\": \"open: synthetic scene only; no sky-object world transform or draw-order claim\""
            << ", \"G-03\": \"open: the route has no sun-direction input\""
            << ", \"G-04\": \"open: TIME is a declared input, not a clock\""
            << ", \"G-05\": \"open: LIGHT_SCALE is a declared input, not a recovered value\"}}";
    }
    output << ",\n    \"surfaces\": [";
    for (std::size_t index = 0; index < p.surfaces.size(); ++index) {
        const space::SurfacePlan& surface = p.surfaces[index];
        const auto upload = std::find_if(uploaded.begin(), uploaded.end(),
            [&](const Uploaded& item) { return item.surface == index; });
        std::vector<std::string> causes;
        for (const space::SurfaceStatus cause : surface.causes) causes.emplace_back(space::to_string(cause));
        const space::TextureIdentity& texture = surface.texture_identity;
        const bool gloss = surface.material == space::SkyMaterial::meshgloss;
        const bool additive_surface = surface.material == space::SkyMaterial::meshadditive;
        output << (index == 0 ? "\n      " : ",\n      ")
            << "{\"mesh_index\": " << surface.mesh_index << ", \"submesh_index\": " << surface.submesh_index
            << ", \"mesh_name\": " << json(surface.mesh_name)
            << ", \"original_shader\": " << json(surface.original_shader)
            << ", \"status\": " << json(space::to_string(surface.status))
            << ", \"causes\": " << report_strings(causes) << ", \"detail\": " << json(surface.detail)
            << ", \"parameters\": " << report_strings(surface.parameters)
            << ", \"unconsumed_parameters\": " << report_strings(surface.unconsumed_parameters)
            << ", \"vertices\": " << surface.vertices << ", \"triangles\": " << surface.triangles
            << ",\n        \"base_texture\": " << json(surface.base_texture)
            << ", \"texture\": {\"logical_path\": " << json(texture.logical_path)
            << ", \"sha256\": " << json(texture.sha256) << ", \"source_id\": " << json(texture.source_id)
            << ", \"layer_id\": " << json(texture.layer_id) << ", \"format\": " << json(texture.format)
            << ", \"source_origin\": " << json(texture.source_origin)
            << ", \"has_alpha\": " << (texture.has_alpha ? "true" : "false")
            << ", \"width\": " << texture.width << ", \"height\": " << texture.height
            << ", \"mips\": " << texture.mip_count
            << ", \"alpha_policy\": " << json(gloss
                ? "sampled: texture alpha weights the interpolated vertex specular; output alpha is the light-scale "
                  "alpha (1, opaque)"
                : (additive_surface
                    ? "sampled: texture alpha never scales the ONE/ONE rgb contribution; the adapter keeps fragment "
                      "alpha 1 and destination alpha is not captured (space-sky-meshadditive-state-v1)"
                    : "retained in the upload, not sampled: the opaque diffuse contract writes ALBEDO rgb only")) << '}'
            << ",\n        \"material\": {\"route\": " << json(surface.material ? space::to_string(*surface.material) : "")
            << ", \"values\": ";
        if (surface.meshgloss) {
            output << "{\"Emissive\": " << vec4(surface.meshgloss->emissive)
                << ", \"Diffuse\": " << vec4(surface.meshgloss->diffuse)
                << ", \"Specular\": " << vec4(surface.meshgloss->specular)
                << ", \"Shininess\": " << number(surface.meshgloss->shininess, 9) << '}';
        } else if (surface.meshadditive) {
            output << "{\"Color\": " << vec4(surface.meshadditive->color)
                << ", \"Color_kind\": " << json(surface.meshadditive->color_kind == assets::ParameterKind::vector3
                    ? "vector3" : "vector4")
                << ", \"UVScrollRate\": " << vec4(surface.meshadditive->uv_scroll_rate) << '}';
        } else {
            output << "null";
        }
        output << ", \"fields\": [";
        for (std::size_t field = 0; field < surface.field_dispositions.size(); ++field) {
            const space::FieldDisposition& item = surface.field_dispositions[field];
            output << (field == 0 ? "" : ", ") << "{\"name\": " << json(item.name) << ", \"kind\": " << json(item.kind)
                << ", \"disposition\": " << json(item.disposition) << ", \"rule\": " << json(item.rule) << '}';
        }
        output << "], \"light_policy\": " << json(gloss ? sky_light.id : (additive_surface ? additive.inputs.id : ""))
            << '}'
            << ",\n        \"adapter_id\": " << json(surface.status == space::SurfaceStatus::accepted
                ? (gloss ? space::meshgloss_route_id : (additive_surface ? space::meshadditive_route_id : space::adapter_id))
                : "")
            << ", \"route\": " << json(surface.status == space::SurfaceStatus::accepted ? "modern_spatial" : "")
            << ", \"technique\": \"\", \"pass_name\": \"\", \"render_pass\": "
            << json(surface.status == space::SurfaceStatus::accepted ? (additive_surface ? "transparent" : "opaque") : "")
            << ", \"compiler\": " << json(index < compiler.size() ? compiler[index] : "not_attempted")
            << ", \"upload_failure\": " << json(index < upload_failure.size() ? upload_failure[index] : "")
            << ", \"renderer_asset\": " << (upload != uploaded.end() ? upload->asset : 0)
            << ", \"entity\": " << (upload != uploaded.end() ? upload->entity : 0)
            << ", \"submitted\": " << (upload != uploaded.end() && upload->submitted ? "true" : "false")
            << ", \"casts_shadows\": " << (upload != uploaded.end() ? (sky_casts ? "true" : "false") : "null");
        if (upload != uploaded.end()) {
            const std::size_t slot = static_cast<std::size_t>(upload - uploaded.begin());
            if (slot < pixels.surfaces.size()) {
                const space::SurfacePixels& evidence = pixels.surfaces[slot];
                output << ",\n        \"pixels\": {\"status\": " << json(space::to_string(evidence.status))
                    << ", \"mask_pixels\": " << evidence.mask_pixels
                    << ", \"interior_pixels\": " << evidence.interior_pixels
                    << ", \"changed_interior\": " << evidence.changed_interior
                    << ", \"isolated_changed_inside\": " << evidence.isolated_changed_inside
                    << ", \"isolated_changed_outside\": " << evidence.isolated_changed_outside
                    << ", \"isolated_pixels_outside\": " << evidence.isolated_pixels_outside
                    << ", \"uv_quadrants\": [";
                for (std::size_t quadrant = 0; quadrant < 4; ++quadrant) {
                    const auto& mean = evidence.quadrant_mean_rgb[quadrant];
                    output << (quadrant == 0 ? "" : ", ") << "{\"pixels\": " << evidence.quadrant_pixels[quadrant]
                        << ", \"mean_rgb\": [" << number(mean[0]) << ", " << number(mean[1]) << ", "
                        << number(mean[2]) << "]}";
                }
                output << "]}";
            }
        }
        output << '}';
    }
    output << (p.surfaces.empty() ? "" : "\n    ") << "],\n    \"submissions\": {\"expected\": [";
}

void SpaceEnvironment::State::write_submission_report(std::ostream& output) const {
    bool first = true;
    for (const Uploaded& item : uploaded) {
        output << (first ? "" : ", ") << "{\"entity\": " << item.entity << ", \"asset\": " << item.asset
            << ", \"pass\": \"opaque\", \"role\": \"sky\"}";
        first = false;
    }
    if (occluder_uploaded || occluder_mask) {
        output << (first ? "" : ", ") << "{\"entity\": " << occluder_entity << ", \"asset\": " << occluder_asset
            << ", \"pass\": \"opaque\", \"role\": " << json(foreground_role) << '}';
    }
    output << "], \"observed\": [";
    for (std::size_t index = 0; index < observed.size(); ++index) {
        output << (index == 0 ? "" : ", ") << "{\"entity\": " << observed[index].entity_id
            << ", \"asset\": " << observed[index].asset_id
            << ", \"pass\": " << json(to_string(observed[index].pass)) << '}';
    }
    output << "], \"missing\": " << report_strings(missing_submissions) << "},\n"
        << "    \"camera\": {\"status\": " << json(space::to_string(camera_status))
        << ", \"input\": " << json(options.camera)
        << ", \"basis\": \"render (Godot): source (x, y, z) -> (x, z, -y)\", \"source\": " << json(camera_source) << "},\n"
        << "    \"evidence\": {\"status\": " << json(pixel_status) << ", \"failure\": " << json(pixels.failure)
        << ", \"control_capture\": \"sky_disabled (explicit empty-sky submission, not a corner colour)\""
        << ", \"changed_threshold\": " << space::changed_threshold << ", \"mask_margin\": " << space::mask_margin
        << ", \"outside_pixels\": " << pixels.outside_pixels << ", \"changed_outside\": " << pixels.changed_outside
        << ", \"occlusion\": {\"status\": " << json(pixels.occlusion_status)
        << ", \"occluder_pixels\": " << pixels.occluder_pixels
        << ", \"occluder_changed_by_sky\": " << pixels.occluder_changed << "}},\n"
        << "    \"shadows\": {\"sky_cast_flag\": " << (sky_casts ? "true" : "false")
        << ", \"set_before_instances\": true"
        << ", \"sky_material\": " << json(caster_control(control) ? "depth_writing_caster_control" : "shipped_adapter")
        << ", \"lit_foreground_control\": " << json(lit_foreground)
        << ", \"receiver_pixels\": " << (shadow_control(control) ? pixels.occluder_pixels : 0)
        << ", \"receiver_changed_by_sky\": " << (shadow_control(control) ? pixels.occluder_changed : 0)
        << ", \"cause\": " << json(shadow_control(control)
            ? "sky exclusion measured on a controlled lit receiver that surface 0 would shadow if it cast; the "
              "sky-shadow-caster-cast positive control (depth-writing caster variant, flag on) must darken it. Not "
              "capital-hull self-shadow fidelity and not an original comparison"
            : "not requested in this run; --eawr-space-control sky-shadow[-cast] / sky-shadow-caster[-cast] execute the lit "
              "foreground controls")
        << "},\n";
    const auto sizes = [&](const std::vector<std::size_t>& values) {
        std::string text = "[";
        for (std::size_t index = 0; index < values.size(); ++index) {
            text += (index == 0 ? "" : ", ") + std::to_string(values[index]);
        }
        return text + "]";
    };
    output << "    \"lifecycle\": {\"mode\": " << json(control == "reload-cycle" ? "live" : "not_requested")
        << ", \"reload_cycles\": " << lifecycle_released.size()
        << ", \"instances_before_release\": " << sizes(lifecycle_instances_before)
        << ", \"resources_before_release\": " << sizes(lifecycle_resources_before)
        << ", \"instances_after_release\": " << sizes(lifecycle_instances_after)
        << ", \"resources_after_release\": " << sizes(lifecycle_released)
        << ", \"resources_after_reupload\": " << sizes(lifecycle_reuploaded)
        << ", \"partial_failure\": " << (partial_failure ? "true" : "false")
        << ", \"resources_after_partial_failure\": " << partial_failure_remaining
        << ", \"resources_after_teardown\": ";
    if (teardown_remaining) output << *teardown_remaining; else output << "null";
    output << ", \"instances_after_teardown\": ";
    if (teardown_instances) output << *teardown_instances; else output << "null";
    output << "},\n    \"artifacts\": {\"status\": ";
    if (options.capture_path.empty()) output << "\"not_requested\"";
    else if (!artifacts_failed.empty()) output << "\"failed\"";
    else if (artifacts_written.empty()) output << "\"not_attempted\"";
    else output << "\"written\"";
    output << ", \"written\": " << report_strings(artifacts_written) << ", \"failed\": " << report_strings(artifacts_failed) << "}},\n";
}

} // namespace eawr::presentation::godot_backend
