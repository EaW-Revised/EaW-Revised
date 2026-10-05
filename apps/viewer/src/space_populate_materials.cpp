#include "space_populate.hpp"
#include "eawr/core/load_profile.hpp"
#include "eawr/presentation/space/space.hpp"
#include "frame_timer.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "space_populate_internal.hpp"
#include "shield_shell.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/animation/unit_clips.hpp"
#include "eawr/presentation/ui/pads.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/sim/math/geometry.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

namespace eawr::presentation::godot_backend {

// The land path's lighting policy (MapMode::State::lighting_state) applied to
// the space scene: SH from the environment's three lights and ambient, or the
// frozen P0 hemisphere constants.
[[nodiscard]] GodotRenderer::LightingState space_populate_detail::lighting_state(const SpacePopulation::Options& options,
                                                          const float max_distance) {
    GodotRenderer::LightingState state;
    state.shadows = options.shadows;
    state.shadow_floor = {options.environment.shadow.r, options.environment.shadow.g, options.environment.shadow.b};
    state.shadow_max_distance = max_distance;
    const auto quality = shadow_settings(active_render_profile(), true);
    state.shadow_atlas_size = quality.atlas_size;
    state.shadow_filter = quality.high_filter ? GodotRenderer::ShadowFilter::soft_high
                                            : GodotRenderer::ShadowFilter::soft_medium;
    // Four blended cascades keep the profile's near split depths while the
    // last cascade covers space's longer reach (#231). Godot scales the PCF
    // kernel and depth bias by blur: its RenderingServer default of zero
    // disables both (#667). Use the land path's small effective depth bias
    // so lit hulls clear self-shadow acne without detaching contact shadows.
    state.shadow_layout = GodotRenderer::ShadowLayout::parallel_4_splits;
    state.shadow_split_offsets = quality.split_offsets;
    state.shadow_blend_splits = true;
    state.shadow_blur = 1.0F;
    state.shadow_bias = 0.05F;
    state.shadow_normal_bias = 5.0F;
    lighting::IrradianceMatrices matrices;
    lighting::IrradianceMatrices fill;
    lighting::Vec3 toward{};
    if (options.policy == lighting::Policy::sh) {
        matrices = lighting::source_to_render(lighting::sph_light_all(options.environment));
        fill = lighting::source_to_render(lighting::sph_light_fill(options.environment));
        state.sun_diffuse = lighting::sun_diffuse(options.environment);
        const lighting::Vec3 sun = options.environment.lights[0].direction;
        toward = lighting::source_to_render(lighting::Vec3{-sun.x, -sun.y, -sun.z});
        state.specular = lighting::sun_specular(options.environment);
    } else {
        matrices = lighting::hemisphere_matrices();
        fill = lighting::hemisphere_fill_matrices();
        state.sun_diffuse = lighting::hemisphere_directional;
        const auto& raw = lighting::hemisphere_light_direction;
        toward = {raw[0], raw[1], raw[2]};
        state.specular = {2.0F, 1.88F, 1.72F};
    }
    const float length = std::sqrt(toward.x * toward.x + toward.y * toward.y + toward.z * toward.z);
    state.toward_light = {toward.x / length, toward.y / length, toward.z / length};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        state.sph[channel] = matrices.rgb[channel];
        state.sph_fill[channel] = fill.rgb[channel];
    }
    return state;
}


std::optional<SpacePopulation::SurfaceUpload> SpacePopulation::upload_surface(
    GodotRenderer& renderer, const vfs::Vfs& filesystem, scene::VfsAssetCache& cache,
    std::map<std::string, assets::Texture>& textures, sim::AssetId& next_asset,
    const assets::Model& model, const std::uint32_t mesh_index, const std::uint32_t submesh_index,
    const scene::LegacySelector& selector, const std::string& texture_path,
    const std::optional<std::array<std::uint8_t, 3>>& colour, const std::string& identity) {
        const assets::Mesh& source_mesh = model.meshes[mesh_index];
        assets::Model single;
        single.source = model.source;
        single.bones = model.bones;
        assets::Mesh mesh = source_mesh;
        mesh.submeshes = {source_mesh.submeshes[submesh_index]};
        single.meshes.push_back(std::move(mesh));
        const assets::Submesh& submesh = single.meshes.front().submeshes.front();
        assets::Texture texture = placeholder_texture();
        if (!texture_path.empty()) {
            auto found = textures.find(texture_path);
            if (found == textures.end()) {
                auto decoded = assets::load_texture(filesystem, texture_path);
                found = textures.emplace(texture_path, decoded ? std::move(decoded.value()) : placeholder_texture()).first;
            }
            texture = found->second;
        }
        MaterialDescription material{
            .schema_version = MaterialDescription::current_schema_version,
            .route = MaterialRoute::legacy_effect,
            .pass = selector.transparent ? RenderPass::transparent : RenderPass::opaque,
            .program = std::string(selector.program),
            .technique = std::string(selector.technique),
            .pass_name = std::string(selector.pass),
            .bindings = {},
        };
        for (const assets::MaterialParameter& parameter : submesh.parameters) {
            material.bindings.push_back({parameter.name, parameter.value});
        }
        if (material.program == "MeshAdditiveVColor.fx") {
            material.bindings.push_back({"eawr_effect_time", 0.0F});
        }
        if (colour && scene::colorizes(submesh.shader)) {
            std::erase_if(material.bindings, [](const MaterialBinding& binding) { return ieq(binding.name, "Colorization"); });
            material.bindings.push_back({"Colorization", scene::colorization_binding(material.program, *colour)});
        }
        const std::vector<GodotRenderer::BindingTexture> family_textures = family_binding_textures(material,
            [&](const std::string_view declared) -> std::optional<assets::Texture> {
                const std::string path = probe(cache, "data/art/textures/", declared, texture_suffixes);
                if (path.empty()) return std::nullopt;
                auto found = textures.find(path);
                if (found == textures.end()) {
                    auto decoded = assets::load_texture(filesystem, path);
                    if (!decoded) return std::nullopt;
                    found = textures.emplace(path, std::move(decoded.value())).first;
                }
                return found->second;
            });
        const auto uploaded = options_.reject_upload_for_test && upload_failures_.empty()
            ? core::Result<void>::failure({.code = "EAWR-SPACE-POPULATE-TEST",
                                           .message = "injected supported surface upload failure"})
            : renderer.upload(next_asset, single, texture, material, family_textures);
        if (!uploaded) {
            upload_failures_.push_back(identity + " shader " + submesh.shader + ": "
                                       + core::format_diagnostic(uploaded.error()));
            return std::nullopt;
        }
        SurfaceUpload upload;
        upload.renderer_asset = next_asset++;
        uploaded_.push_back(upload.renderer_asset);
        if (material.program == "MeshAdditiveVColor.fx") effect_clock_assets_.push_back(upload.renderer_asset);
        passes_.emplace(upload.renderer_asset, material.pass);
        ++surfaces_uploaded_;
        if (colour) ++team_colour_variants_;
        const std::int32_t bone = single.meshes.front().bone;
        if (!single.bones.empty() && (bone >= 0 || !submesh.skin_bones.empty())) {
            // Draw the bind pose until this placement's idle clip is sampled.
            if (auto player = animation::Player::create(single)) {
                if (auto pose = player.value().sample({})) upload.pose = std::move(pose.value().bones);
            }
        }
        for (const assets::Vertex& vertex : submesh.vertices) {
            std::array<float, 3> point{vertex.position.x, vertex.position.y, vertex.position.z};
            if (upload.pose && bone >= 0 && static_cast<std::size_t>(bone) < upload.pose->size()) {
                const animation::BonePose& posed = (*upload.pose)[static_cast<std::size_t>(bone)];
                const animation::Matrix& m = submesh.skin_bones.empty() ? posed.model_asset : posed.skin_asset;
                point = {m[0] * point[0] + m[4] * point[1] + m[8] * point[2] + m[12],
                         m[1] * point[0] + m[5] * point[1] + m[9] * point[2] + m[13],
                         m[2] * point[0] + m[6] * point[1] + m[10] * point[2] + m[14]};
            }
            for (std::size_t axis = 0; axis < 3; ++axis) {
                upload.minimum[axis] = std::min(upload.minimum[axis], point[axis]);
                upload.maximum[axis] = std::max(upload.maximum[axis], point[axis]);
            }
        }
        return upload;
    }

std::optional<SpacePopulation::SurfaceUpload> SpacePopulation::upload_shell(
    GodotRenderer& renderer, const vfs::Vfs& filesystem, scene::VfsAssetCache& cache,
    sim::AssetId& next_asset, const assets::Model& model, const std::uint32_t mesh_index,
    const std::uint32_t submesh_index, std::string& status) {
        assets::Model single;
        single.source = model.source;
        single.bones = model.bones;
        assets::Mesh mesh = model.meshes[mesh_index];
        mesh.submeshes = {model.meshes[mesh_index].submeshes[submesh_index]};
        mesh.visible = true;
        single.meshes.push_back(std::move(mesh));
        const assets::Submesh& submesh = single.meshes.front().submeshes.front();
        const MaterialDescription material = meshshield_material(submesh);
        std::optional<assets::Texture> base;
        std::vector<GodotRenderer::BindingTexture> extra;
        for (const std::string_view binding : {std::string_view("BaseTexture"), std::string_view("WaveTexture"),
                                               std::string_view("DistortionTexture")}) {
            const MaterialBinding* found = nullptr;
            for (const MaterialBinding& entry : material.bindings) {
                if (ieq(entry.name, binding)) found = &entry;
            }
            const auto* name = found == nullptr ? nullptr : std::get_if<std::string>(&found->value);
            const std::string path = name == nullptr ? std::string{}
                                                     : probe(cache, "data/art/textures/", *name, texture_suffixes);
            if (path.empty()) {
                status = std::string(binding) + " " + (name == nullptr ? std::string("missing") : *name + " unresolved");
                return std::nullopt;
            }
            auto decoded = assets::load_texture(filesystem, path);
            if (!decoded) {
                status = std::string(binding) + " " + path + ": " + core::format_diagnostic(decoded.error());
                return std::nullopt;
            }
            if (binding == "BaseTexture") base = std::move(decoded.value());
            else extra.push_back({std::string(binding), std::move(decoded.value())});
        }
        const auto uploaded = renderer.upload(next_asset, single, *base, material, extra);
        if (!uploaded) {
            status = "upload_failed: " + core::format_diagnostic(uploaded.error());
            return std::nullopt;
        }
        SurfaceUpload upload;
        upload.renderer_asset = next_asset++;
        uploaded_.push_back(upload.renderer_asset);
        passes_.emplace(upload.renderer_asset, material.pass);
        shield_assets_.push_back(upload.renderer_asset);
        const std::int32_t bone = single.meshes.front().bone;
        if (!single.bones.empty() && (bone >= 0 || !submesh.skin_bones.empty())) {
            if (auto player = animation::Player::create(single)) {
                if (auto pose = player.value().sample({})) upload.pose = std::move(pose.value().bones);
            }
        }
        return upload;  // no bounds: the shell is not part of the pick box
    }

} // namespace eawr::presentation::godot_backend
