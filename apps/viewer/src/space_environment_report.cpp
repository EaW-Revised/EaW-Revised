#include "space_environment_internal.hpp"
#include "render_profile_viewport.hpp"

namespace eawr::presentation::godot_backend {
namespace space_environment_detail {

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
    const auto list = [&](const std::vector<std::string>& values) {
        std::string text = "[";
        for (std::size_t index = 0; index < values.size(); ++index) text += (index == 0 ? "" : ", ") + json(values[index]);
        return text + "]";
    };
    output << "{\n  \"schema_version\": 1,\n  \"mode\": \"map\",\n"
        << "  \"status\": " << json(status) << ",\n"
        << "  \"failure\": " << json(failure) << ",\n"
        << "  \"backend\": {\"engine\": " << json(backend.engine)
        << ", \"rendering_method\": " << json(backend.rendering_method)
        << ", \"adapter_vendor\": " << json(backend.adapter_vendor)
        << ", \"adapter_name\": " << json(backend.adapter_name)
        << ", \"driver_api\": " << json(backend.driver_api) << "},\n"
        << "  \"render_profile\": " << render_profile_report() << ",\n"
        << "  \"profile\": " << json(options.profile) << ",\n  \"layers\": " << list(options.layers) << ",\n"
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
            << ", \"causes\": " << list(causes) << ", \"detail\": " << json(surface.detail)
            << ", \"parameters\": " << list(surface.parameters)
            << ", \"unconsumed_parameters\": " << list(surface.unconsumed_parameters)
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
    output << "], \"missing\": " << list(missing_submissions) << "},\n"
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
    output << ", \"written\": " << list(artifacts_written) << ", \"failed\": " << list(artifacts_failed) << "}},\n";
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
    first = true;
    for (const auto& [name, hash] : capture_hashes) {
        output << (first ? "" : ", ") << json(name) << ": " << json(hash);
        first = false;
    }
    output << "}\n}\n";
    output.close();
    return static_cast<bool>(output);
}

} // namespace eawr::presentation::godot_backend
