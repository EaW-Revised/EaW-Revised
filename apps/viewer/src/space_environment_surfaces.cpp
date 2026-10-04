#include "space_environment_internal.hpp"
#include "eawr/core/load_profile.hpp"
#include "render_profile_viewport.hpp"
#include "shutdown_trace.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"

namespace eawr::presentation::godot_backend {
namespace {

// MeshAdditiveVColor.fx: the MeshAdditive adapter with the authored vertex
// colour as a further factor (the effect has no Color field; Color is bound
// to (1, 1, 1, 1)). The same stored-value policy as the MeshAdditive route.
[[nodiscard]] const std::string& meshadditive_vcolor_sky_shader() {
    static const std::string source = [] {
        std::string text(meshadditive_sky_shader);
        constexpr std::string_view from = "clamp(Color.rgb * eawr_sky_light_scale.rgb";
        constexpr std::string_view to = "clamp(COLOR.rgb * Color.rgb * eawr_sky_light_scale.rgb";
        const std::size_t at = text.find(from);
        if (at != std::string::npos) text.replace(at, from.size(), to);
        return text;
    }();
    return source;
}


} // namespace

namespace space_environment_detail {

std::optional<assets::Texture> EnvironmentView::texture(const std::string& declared, std::string& logical,
                                                        std::string& failure) {
    const auto cached = textures_.find(declared);
    if (cached != textures_.end()) {
        logical = cached->second.first;
        if (!cached->second.second) failure = texture_failures_[declared];
        return cached->second.second;
    }
    std::optional<assets::Texture> result;
    const auto path = probe_reference(*filesystem_, "data/art/textures/", declared, texture_suffixes);
    if (!path) {
        failure = "texture " + declared + " is not in the VFS";
    } else {
        logical = *path;
        auto loaded = assets::load_texture(*filesystem_, *path);
        if (!loaded) {
            failure = core::format_diagnostic(loaded.error());
        } else if (auto normalized = space::normalize_texture(loaded.value()); !normalized) {
            failure = core::format_diagnostic(normalized.error());
        } else {
            result = std::move(normalized.value());
        }
    }
    textures_.emplace(declared, std::make_pair(logical, result));
    if (!result) texture_failures_[declared] = failure;
    return result;
}

void EnvironmentView::upload(Item item, const space::SceneSurface& surface, const assets::Model& geometry,
                             const space::Affine& transform, const assets::Model* planned_from,
                             const bool sunlight_glow, const std::optional<std::size_t> idle) {
    const auto skip = [&](std::string cause) {
        item.cause = std::move(cause);
        items_.push_back(std::move(item));
    };
    MaterialDescription material{
        .schema_version = MaterialDescription::current_schema_version,
        .route = MaterialRoute::modern_spatial,
        .pass = RenderPass::opaque,
        .program = {},
        .technique = {},
        .pass_name = {},
        .bindings = {},
    };
    std::vector<std::pair<std::string, std::string>> textures;
    switch (surface.route) {
    case space::SceneRoute::meshgloss:
        material.program = std::string(meshgloss_sky_shader);
        material.bindings = meshgloss_bindings(*surface.meshgloss, space::unlit_sky_policy());
        textures.emplace_back("BaseTexture", surface.base_texture);
        break;
    case space::SceneRoute::meshadditive:
    case space::SceneRoute::meshadditive_vcolor:
        material.pass = RenderPass::transparent;
        material.program = surface.route == space::SceneRoute::meshadditive
            ? std::string(meshadditive_sky_shader) : meshadditive_vcolor_sky_shader();
        material.bindings = meshadditive_bindings(*surface.meshadditive, space::meshadditive_default_inputs());
        textures.emplace_back("BaseTexture", surface.base_texture);
        break;
    case space::SceneRoute::planet:
    case space::SceneRoute::nebula: {
        space::EnvironmentEffectInputs inputs;
        inputs.id = light_ ? "environment-0-light-0" : "no-light";
        inputs.time_seconds = 0.0F;
        inputs.light_scale = {1.0F, 1.0F, 1.0F, 1.0F};
        if (light_) {
            inputs.light_direction = space::render_from_source(light_->toward_light);
            inputs.ambient_light = light_->ambient;
            inputs.diffuse_light = light_->diffuse;
            inputs.specular_light = light_->specular;
        }
        const space::EnvironmentEffectPlan plan = space::plan_environment_effect(
            *planned_from, surface.mesh_index, surface.submesh_index, "t0", inputs);
        if (plan.status != space::EnvironmentEffectStatus::ready) {
            return skip("plan " + std::string(space::to_string(plan.status)) + ": " + plan.detail);
        }
        material = plan.material;
        textures = plan.textures;
        break;
    }
    case space::SceneRoute::legacy_mesh: {
        const auto* selector = scene::find_legacy_selector(surface.shader);
        if (!selector) return skip("companion mesh has no scene selector");
        material.route = MaterialRoute::legacy_effect;
        material.pass = selector->transparent ? RenderPass::transparent : RenderPass::opaque;
        material.program = std::string(selector->program);
        material.technique = std::string(selector->technique);
        material.pass_name = std::string(selector->pass);
        for (const auto& parameter : planned_from->meshes[surface.mesh_index].submeshes[surface.submesh_index].parameters) {
            material.bindings.push_back({parameter.name, parameter.value});
        }
        textures.emplace_back("BaseTexture", surface.base_texture);
        break;
    }
    case space::SceneRoute::unsupported:
        return skip(surface.problem);
    }
    if (textures.empty() || textures.front().first != "BaseTexture") return skip("the surface binds no BaseTexture");
    std::optional<assets::Texture> primary;
    std::vector<GodotRenderer::BindingTexture> extra;
    for (const auto& [binding, declared] : textures) {
        std::string logical;
        std::string failure;
        auto loaded = texture(declared, logical, failure);
        if (!loaded) return skip(binding + ": " + failure);
        if (binding == "BaseTexture") {
            item.texture = logical;
            primary = std::move(loaded);
        } else {
            extra.push_back({binding, std::move(*loaded)});
        }
    }
    const auto matrix = instance_matrix(transform);
    if (!matrix) return skip("the object transform is not finite in Q24");
    const sim::AssetId asset = next_asset_++;
    if (auto uploaded = renderer_->upload(asset, geometry, *primary, material, extra); !uploaded) {
        return skip(core::format_diagnostic(uploaded.error()));
    }
    // FW-19: backdrop layers precede fading foreground geometry. Godot also
    // routes the opaque sky into its transparent pass because depth_draw_never
    // disables depth writes. Order that sky before the additive nebula layers,
    // or its later opaque colour replaces the clouds already drawn.
    const std::int32_t priority = material.pass == RenderPass::transparent ? -127 : -128;
    if (auto ordered = renderer_->set_material_priority(asset, priority); !ordered) {
        static_cast<void>(renderer_->release(asset));
        return skip(core::format_diagnostic(ordered.error()));
    }
    if (sunlight_glow && light_) {
        const assets::Vec3f toward = space::render_from_source(light_->toward_light);
        renderer_->set_billboard_light(asset, {toward.x, toward.y, toward.z});
    }
    // An environment surface never casts: the sky encloses the scene, and the
    // backdrop objects sit far outside any shadow the scene would use.
    renderer_->set_casts_shadows(asset, false);
    assets_.push_back(asset);
    if (surface.route == space::SceneRoute::planet || surface.route == space::SceneRoute::nebula) {
        effect_clock_assets_.push_back(asset);
    }
    instances_.push_back({static_cast<sim::EntityId>(asset), asset, *matrix});
    if (idle && !geometry.bones.empty() && std::any_of(geometry.meshes.begin(), geometry.meshes.end(),
            [](const assets::Mesh& mesh) {
                return mesh.bone >= 0 || std::any_of(mesh.submeshes.begin(), mesh.submeshes.end(),
                    [](const assets::Submesh& submesh) { return !submesh.skin_bones.empty(); });
            })) {
        environment_idle_[*idle].instances.emplace_back(static_cast<sim::EntityId>(asset), asset);
    }
    if (item.role == "primary_sky" || item.role == "secondary_sky" || item.role == "sun") {
        sky_instances_.emplace_back(instances_.size() - 1, transform);
    }
    item.asset = asset;
    item.pass = material.pass;
    item.status = "drawn";
    items_.push_back(std::move(item));
}

void EnvironmentView::compose_object(const Placed& placed, const std::vector<space::SceneSurface>& surfaces) {
    const bool sky = placed.role == "primary_sky" || placed.role == "secondary_sky";
    for (const space::SceneSurface& surface : surfaces) {
        Item item;
        item.role = placed.role;
        item.object = placed.object;
        item.record = placed.record;
        item.model = placed.model_path;
        item.mesh = surface.mesh_name;
        item.shader = surface.shader;
        item.route = std::string(space::to_string(surface.route));
        item.billboard = surface.billboard;
        if (!surface.problem.empty()) {
            item.cause = surface.problem;
            items_.push_back(std::move(item));
            continue;
        }
        if (surface.billboard == 0) {
            if (placed.idle) {
                // Retain the rigid mesh's bone and local vertices for the existing
                // renderer palette; the scene surface's bind chain is already baked.
                assets::Model animated;
                animated.source = placed.model->source;
                animated.bones = placed.model->bones;
                assets::Mesh mesh = placed.model->meshes[surface.mesh_index];
                mesh.submeshes = {mesh.submeshes[surface.submesh_index]};
                animated.meshes.push_back(std::move(mesh));
                upload(std::move(item), surface, animated, placed.transform, placed.model, false, placed.idle);
            } else {
                upload(std::move(item), surface, surface.model, placed.transform, placed.model);
            }
            continue;
        }
        if (!(surface.billboard == 7 && sky)) {
            // Preserve the authored rigid bone and its local vertices. The
            // shared renderer updates its camera-facing palette every frame.
            assets::Model billboard_model;
            billboard_model.source = placed.model->source;
            billboard_model.bones = placed.model->bones;
            assets::Mesh mesh = placed.model->meshes[surface.mesh_index];
            mesh.submeshes = {mesh.submeshes[surface.submesh_index]};
            billboard_model.meshes.push_back(std::move(mesh));
            if (surface.billboard == 6) item.role = placed.role + "_glow";
            if (surface.billboard == 6 && !light_) {
                item.cause = "sunlight glow needs environment light 0";
                items_.push_back(std::move(item));
                continue;
            }
            upload(std::move(item), surface, billboard_model, placed.transform, placed.model,
                   surface.billboard == 6, placed.idle);
            continue;
        }
        space::Billboard billboard;
        if (surface.billboard == 7 && sky) {
            item.role = "sun";
            if (!light_) {
                item.cause = "environment light 0 does not decode, so the sun has no direction";
                items_.push_back(std::move(item));
                continue;
            }
            billboard = space::sun_billboard(*placed.model, surface, eye_source_, target_source_, up_source_,
                                             light_->toward_light, space::environment_sky_radius * 0.9F);
        } else {
            item.cause = "billboard mode " + std::to_string(surface.billboard) + " has no rule on a "
                + placed.role + " object";
            items_.push_back(std::move(item));
            continue;
        }
        if (!billboard.problem.empty()) {
            item.cause = billboard.problem;
            items_.push_back(std::move(item));
            continue;
        }
        upload(std::move(item), surface, billboard.model, space::identity_affine, placed.model);
    }
}


} // namespace space_environment_detail
} // namespace eawr::presentation::godot_backend
