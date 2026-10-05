#include "frame_timer.hpp"
#include "startup_trace.hpp"
#include "battle_content.hpp"
#include "map_mode.hpp"
#include "map_mode_internal.hpp"
#include "eawr/presentation/camera/overview.hpp"
#include "render_profile_viewport.hpp"
#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>

#include <cctype>
#include <cmath>
#include <cstdlib>

#include "map_mode_ready_internal.hpp"

namespace eawr::presentation::godot_backend {

namespace {

// Clean-room Godot spatial adapters authored for this slice. They are the
// modern_spatial route, not a translation of the TERRAIN/SKYDOME effects: the
// legacy route's selector allowlist has no terrain or sky entry and fails
// closed, which is recorded per surface rather than worked around.
// The terrain surface blends every material layer per fragment
// (land_look::terrain_shader, P1-06 #27). Unshaded unless a lighting policy is
// requested; the lit variant is 2 * E(n) * blended diffuse, where E(n) = n4' M
// n4 is the scene irradiance bound as eawr_sph_r/g/b and the doubling is the
// MODULATE2X of the descriptor's live fixed TerrainRenderBump t3_p0 cascade.
// Receiving the directional shadow needs the lit pipeline, so ambient is
// disabled and light() contributes exactly mix(shadow_floor, 1, ATTENUATION)
// per channel.
const std::string terrain_surface_shader = land_look::terrain_shader(false);
const std::string terrain_lit_shader = land_look::terrain_shader(true);

// Fixed variants of the authored terrain shaders. Only the fog declarations,
// world-position varying and one RGB multiply are inserted; render modes,
// lighting, alpha and pass order remain those of the source adapters.
[[nodiscard]] std::string fog_terrain_shader(const bool lit) {
    return land_look::with_map_fog(lit ? terrain_lit_shader : terrain_surface_shader);
}

constexpr std::string_view skydome_shader = R"GODOT(
shader_type spatial;
render_mode unshaded, cull_disabled, depth_draw_never;
uniform sampler2D eawr_diffuse : filter_linear_mipmap, repeat_enable;
void vertex() {
    vec4 clip = PROJECTION_MATRIX * MODELVIEW_MATRIX * vec4(VERTEX, 1.0);
    POSITION = vec4(clip.xy, clip.w * CLIP_SPACE_FAR, clip.w);
}
void fragment() {
    ALBEDO = texture(eawr_diffuse, UV).rgb;
}
)GODOT";


[[nodiscard]] sim::math::Mat3x4 identity_transform() {
    using Fixed = sim::math::Fixed;
    const Fixed zero = Fixed::from_raw(0);
    const Fixed one = Fixed::from_raw(Fixed::scale);
    sim::math::Mat3x4 result{};
    result.rows[0] = {one, zero, zero, zero};
    result.rows[1] = {zero, one, zero, zero};
    result.rows[2] = {zero, zero, one, zero};
    return result;
}
} // namespace

std::string MapMode::State::terrain_material_program() const {
    return fog ? fog_terrain_shader(policy != lighting::Policy::off)
        : std::string(policy == lighting::Policy::off ? terrain_surface_shader : terrain_lit_shader);
}

void MapMode::State::record_terrain_slots(const terrain::Mesh& mesh) {
    State& state = *this;
    // Every declared slot is resolved and decoded for the report; the slots the
    // samples use become the blend's layer array (land_look.hpp).
    for (const terrain::MaterialSlot& slot : mesh.slots) {
        SlotRecord record;
        record.slot = slot.slot;
        record.cells = slot.cells;
        record.effect_program = effect_name(slot.effect);
        record.effect_technique = std::string(terrain::effect_technique(slot.effect));
        record.effect_pass = std::string(terrain::effect_pass(slot.effect));
        if (terrain::declared(slot.primary_texture)) record.declared_primary = *slot.primary_texture;
        if (terrain::declared(slot.secondary_texture)) {
            record.declared_secondary = *slot.secondary_texture;
        }
        if (terrain::declared(slot.primary_texture)) {
            if (const auto resolved = probe_reference(*state.filesystem, "data/art/textures/",
                    *slot.primary_texture, texture_suffixes)) {
                record.resolved_primary = *resolved;
                record.texture_resolved = assets::load_texture(*state.filesystem, *resolved).has_value();
            }
        }
        if (terrain::declared(slot.secondary_texture)) {
            if (const auto resolved = probe_reference(*state.filesystem, "data/art/textures/",
                    *slot.secondary_texture, texture_suffixes)) {
                record.resolved_secondary = *resolved;
            }
        }
        state.slots.push_back(std::move(record));
    }
}

bool MapMode::State::upload_terrain_surfaces(const terrain::Mesh& mesh, const MaterialDescription& terrain_material,
    const assets::Texture& unused_texture, std::vector<sim::RenderInstance>& instances,
    sim::AssetId& next_asset, sim::EntityId& next_entity) {
    State& state = *this;
    for (const terrain::Chunk& chunk : mesh.chunks) {
        for (const terrain::Surface& surface : chunk.surfaces) {
            const assets::Model model = terrain::surface_model(mesh, chunk, surface);
            const auto uploaded = state.renderer->upload(
                next_asset, model, unused_texture, terrain_material);
            if (!uploaded) return state.fail_ready(core::format_diagnostic(uploaded.error()));
            // Retail shadows are stencil volumes of object meshes; the terrain
            // has none, so it receives shadows but never casts them.
            state.renderer->set_casts_shadows(next_asset, false);
            if (state.fog) {
                const auto declared = state.renderer->declare_fog_consumer(next_asset);
                if (!declared) return state.fail_ready("terrain fog consumer " + std::to_string(next_asset)
                    + ": " + core::format_diagnostic(declared.error()));
            }
            instances.push_back({next_entity++, next_asset++, identity_transform()});
            ++state.uploaded_surfaces;
        }
    }
    if (instances.empty()) return state.fail_ready("terrain produced no uploaded surface");

    return true;
}

void MapMode::State::compose_skydome(const assets::Map& map, const assets::ObjectTypeCatalog& catalog,
    const std::string& catalog_failure, const land_look::TextureLookup& lookup_texture,
    sim::AssetId& next_asset, sim::EntityId& next_entity, std::vector<sim::RenderInstance>& instances) {
    State& state = *this;
    // Skydome. The TED environment field names an XML object; resolving it is
    // catalog work, and every failure along the way is an explicit status
    // rather than a silent skip.
    if (map.environments.empty() || !map.environments[state.environment_record].primary_sky) {
        state.skydome_status = "absent";
    } else {
        state.skydome_object = *map.environments[state.environment_record].primary_sky;
        const assets::ObjectTypeRef* type = assets::find_object_type(catalog, state.skydome_object);
        if (type == nullptr) {
            state.skydome_status = catalog_failure.empty()
                ? "object_not_in_catalog" : "catalog_unavailable";
            if (!catalog_failure.empty()) state.failure = catalog_failure;
        } else {
            const std::optional<std::string>& declared =
                map.kind == assets::MapKind::space && type->space_model_name
                    ? type->space_model_name
                    : (type->land_model_name ? type->land_model_name : type->model_name);
            if (!declared) {
                state.skydome_status = "object_declares_no_model";
            } else if (const auto resolved = probe_reference(*state.filesystem,
                           "data/art/models/", *declared, model_suffixes)) {
                state.skydome_model_path = *resolved;
                auto bytes = state.filesystem->open(*resolved);
                auto model = assets::load_model(*state.filesystem, *resolved);
                if (!bytes || !model) {
                    state.skydome_status = "model_failed_to_load";
                } else {
                    state.skydome_model_hash = hash_bytes(bytes.value());
                    const auto& light = state.environment.lights[0].direction;
                    land_look::SkyInputs sky_inputs;
                    sky_inputs.toward_sun = assets::Vec3f{-light.x, -light.y, -light.z};
                    const auto sky = land_look::compose_sky(*state.renderer, model.value(),
                        lookup_texture, sky_inputs, next_asset, next_entity, instances);
                    if (!sky) {
                        state.skydome_status = "upload_rejected";
                        state.failure = core::format_diagnostic(sky.error());
                    } else {
                        state.skydome_casts_shadows = false;
                        state.skydome_drawn = sky.value().dome_surfaces != 0;
                        state.skydome_status = state.skydome_drawn ? "drawn" : "no_dome_surface";
                    }
                }
            } else {
                state.skydome_status = "model_not_in_vfs";
                state.skydome_model_path = *declared;
            }
        }
    }

}

} // namespace eawr::presentation::godot_backend
