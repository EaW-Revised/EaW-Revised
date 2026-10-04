#include "map_mode_internal.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "legacy/registry.hpp"

namespace eawr::presentation::godot_backend {

namespace {

// The scene builder emits transforms in the TED source basis. The renderer
// consumes snapshot matrices in its render basis, where the documented
// conversion is (x, y, z) -> (x, z, -y). That conversion is a signed
// permutation, so conjugating by it moves raw Q24 values without rounding.
[[nodiscard]] sim::math::Mat3x4 source_to_render(const sim::math::Mat3x4& source) {
    using Fixed = sim::math::Fixed;
    constexpr std::array<std::size_t, 3> axis{0, 2, 1};
    constexpr std::array<std::int64_t, 3> sign{1, 1, -1};
    sim::math::Mat3x4 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result.rows[row][column] = Fixed::from_raw(
                sign[row] * sign[column] * source.rows[axis[row]][axis[column]].raw());
        }
        result.rows[row][3] = Fixed::from_raw(sign[row] * source.rows[axis[row]][3].raw());
    }
    return result;
}

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw())
        / static_cast<double>(sim::math::Fixed::scale));
}

[[nodiscard]] std::array<float, 3> apply(
    const sim::math::Mat3x4& matrix, const std::array<float, 3>& point) {
    std::array<float, 3> result{};
    for (std::size_t row = 0; row < 3; ++row) {
        result[row] = to_float(matrix.rows[row][0]) * point[0] + to_float(matrix.rows[row][1]) * point[1]
            + to_float(matrix.rows[row][2]) * point[2] + to_float(matrix.rows[row][3]);
    }
    return result;
}

struct SurfaceUpload final {
    sim::AssetId renderer_asset{};
    std::optional<std::vector<animation::BonePose>> pose;
    // Source-basis bounds of the posed surface in model space.
    std::array<float, 3> minimum{};
    std::array<float, 3> maximum{};
    // The drawn submesh and its bone, for bounds under another pose.
    const assets::Submesh* submesh{};
    std::int32_t bone{-1};
    // #147: the farthest a vertex of this surface moves in the wind. A tree
    // surface moves BendScale x z_max^2 x |wind| / H^2, H being the placed
    // model's world box height (tree_sway holds all but the 1 / H^2); a grass
    // surface a fixed distance.
    float tree_sway{};
    float grass_sway{};
};

// #147: the model's bone matrices under its reference (bind) pose, source
// basis. Empty for a model without bones or one whose pose does not sample,
// so every mesh keeps its authored frame.
[[nodiscard]] std::vector<lighting::wind::Matrix> reference_bones(const assets::Model& model) {
    std::vector<lighting::wind::Matrix> bones;
    if (model.bones.empty()) return bones;
    auto player = animation::Player::create(model);
    if (!player) return bones;
    auto pose = player.value().sample({});
    if (!pose) return bones;
    for (const animation::BonePose& bone : pose.value().bones) bones.push_back(bone.model_asset);
    return bones;
}

// #147: the wind inputs of one Tree.fx or Grass.fx surface, bound on its
// material: the placed model's bend box in render-basis object space and the
// rows taking the render-basis model VERTEX to source-basis mesh space (a
// rigid mesh's vertices are stored in its bone's frame).
void bind_wind_frame(MaterialDescription& material, const lighting::wind::Box& box, const assets::Mesh& mesh,
    const assets::Submesh& submesh, const std::vector<lighting::wind::Matrix>& bones) {
    const assets::Vec3f& c = box.centre;
    const assets::Vec3f& h = box.half_extent;
    material.bindings.push_back({"eawr_bend_box_min", assets::Vec3f{c.x - h.x, c.z - h.z, -(c.y + h.y)}});
    material.bindings.push_back({"eawr_bend_box_max", assets::Vec3f{c.x + h.x, c.z + h.z, -(c.y - h.y)}});
    std::optional<lighting::wind::Matrix> mesh_from_model;
    if (mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < bones.size() && submesh.skin_bones.empty()) {
        mesh_from_model = lighting::wind::invert_affine(bones[static_cast<std::size_t>(mesh.bone)]);
    }
    const lighting::wind::Matrix m = mesh_from_model.value_or(lighting::wind::Matrix{
        1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F});
    // Source (x, y, z) = render (x, -z, y), so the source-basis row
    // (a, b, c, t) reads the render VERTEX as (a, c, -b, t).
    const auto row = [&m](const std::size_t i) { return assets::Vec4f{m[i], m[8 + i], -m[4 + i], m[12 + i]}; };
    material.bindings.push_back({"eawr_mesh_x", row(0)});
    material.bindings.push_back({"eawr_mesh_y", row(1)});
    material.bindings.push_back({"eawr_mesh_z", row(2)});
}

// Model-space source-basis bounds of a submesh's vertices under a pose.
void posed_bounds(const assets::Submesh& submesh, const std::int32_t bone,
    const std::vector<animation::BonePose>* pose, std::array<float, 3>& minimum, std::array<float, 3>& maximum) {
    minimum = {1e30F, 1e30F, 1e30F};
    maximum = {-1e30F, -1e30F, -1e30F};
    for (const assets::Vertex& vertex : submesh.vertices) {
        std::array<float, 3> point{vertex.position.x, vertex.position.y, vertex.position.z};
        if (pose && bone >= 0 && static_cast<std::size_t>(bone) < pose->size()) {
            // Rigid vertices are stored in their bone's space; palette-skinned ones in bind model space.
            const animation::BonePose& posed = (*pose)[static_cast<std::size_t>(bone)];
            const animation::Matrix& m = submesh.skin_bones.empty() ? posed.model_asset : posed.skin_asset;
            point = {m[0] * point[0] + m[4] * point[1] + m[8] * point[2] + m[12],
                     m[1] * point[0] + m[5] * point[1] + m[9] * point[2] + m[13],
                     m[2] * point[0] + m[6] * point[1] + m[10] * point[2] + m[14]};
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            minimum[axis] = std::min(minimum[axis], point[axis]);
            maximum[axis] = std::max(maximum[axis], point[axis]);
        }
    }
}

// The binding that colours a colorizing surface: the placement's team colour
// replaces any authored Colorization.
void bind_team_colour(MaterialDescription& material, const std::array<std::uint8_t, 3>& colour) {
    std::erase_if(material.bindings, [](const MaterialBinding& binding) { return binding.name == "Colorization"; });
    material.bindings.push_back({"Colorization", scene::colorization_binding(material.program, colour)});
}

} // namespace

bool MapMode::State::compose_placements(const assets::Map& map,
    std::vector<sim::RenderInstance>& instances, sim::AssetId& next_asset,
    sim::EntityId& next_entity) {
    if (!catalog) {
        failure = "populate requires the XML catalog, which did not load";
        return false;
    }
    scene::VfsAssetCache cache(*filesystem);
    scene::BuildInput input;
    input.map = &map;
    input.map_sha256 = map_hash;
    input.catalog = &*catalog;
    input.access = cache.access();
    scene = scene::build(input);
    build_attached_plan(cache);
    // XML CAPTURE_POINT behavior marks the stateful placements. Their ALO
    // meshes are all authored visible, while the matching idle clips vary
    // visibility at the faction emblem bones. Cache that data per model; the
    // placement's capture_state is the Phase 2 owner input.
    std::map<sim::AssetId, std::vector<std::uint8_t>> capture_variants;
    std::set<sim::AssetId> capture_checked;
    for (const scene::Placement& placement : scene->placements) {
        if (!placement.capture_point || placement.asset_id == 0
            || !capture_checked.insert(placement.asset_id).second) continue;
        const assets::Model* model = cache.model(placement.model_path);
        if (model == nullptr) continue;
        std::vector<assets::Animation> idle_clips;
        const std::string stem = placement.model_path.substr(0, placement.model_path.size() - 4);
        for (int index = 0; index <= 9; ++index) {
            const std::string path = stem + "_idle_0" + std::to_string(index) + ".ala";
            if (!filesystem->stat(path)) continue;
            if (auto clip = assets::load_animation(*filesystem, path)) idle_clips.push_back(std::move(clip.value()));
        }
        auto variants = scene::capture_variant_bones(*model, idle_clips);
        if (std::any_of(variants.begin(), variants.end(), [](const std::uint8_t value) { return value != 0U; })) {
            capture_variants.emplace(placement.asset_id, std::move(variants));
        }
    }
    if (fog) {
        for (const auto& placement : scene->placements) {
            if (placement.drawable()) continue;
            for (const auto& issue : placement.issues) {
                fog_uncomposed.push_back("placement " + std::to_string(placement.scene_ordinal)
                    + " " + placement.object_id + ": " + std::string(scene::to_string(issue.cause))
                    + " " + issue.detail);
            }
        }
    }

    // One renderer asset per (scene asset, supported surface, team colour),
    // in scene asset order, then surface order, then colour order, so
    // renderer ids are as stable as scene ids. Only a colorizing surface has
    // more than one colour: no colour (its authored Colorization) and each
    // distinct team colour of the placements that use the asset.
    using Colour = std::optional<std::array<std::uint8_t, 3>>;
    std::map<sim::AssetId, const scene::Placement*> first_use;
    std::map<sim::AssetId, std::set<Colour>> asset_colours;
    for (const scene::Placement& placement : scene->placements) {
        if (placement.asset_id != 0 && (!fog || placement.drawable())) {
            first_use.try_emplace(placement.asset_id, &placement);
            asset_colours[placement.asset_id].insert(placement.team_colour);
        }
        if (!placement.drawable()) continue;
        ++team_colour_by_status[placement.team_colour_status];
        if (placement.team_colour) team_colour_factions.try_emplace(placement.owner_faction, *placement.team_colour);
    }
    const auto surface_colour = [](const scene::Placement& placement, const scene::Surface& surface) {
        return scene::colorizes(surface.shader) ? placement.team_colour : Colour{};
    };
    std::map<std::string, assets::Texture> textures;
    std::map<std::tuple<sim::AssetId, std::size_t, Colour>, SurfaceUpload> uploads;
    // #147: each wind model's source-basis bend box, the tree sway's H.
    std::map<sim::AssetId, lighting::wind::Box> bend_boxes;
    const float wind_speed = wind ? std::abs(wind->speed) : 0.0F;
    for (const scene::SceneAsset& asset : scene->assets) {
        const auto use = first_use.find(asset.asset_id);
        const assets::Model* model = cache.model(asset.logical_path);
        if (use == first_use.end() || model == nullptr) continue;
        const std::vector<scene::Surface>& surfaces = use->second->surfaces;
        std::optional<std::vector<lighting::wind::Matrix>> wind_bones;
        for (std::size_t index = 0; index < surfaces.size(); ++index) {
            const scene::Surface& surface = surfaces[index];
            const scene::LegacySelector* selector = scene::find_legacy_selector(surface.shader);
            if (!surface.supported || selector == nullptr) {
                ++surfaces_unsupported;
                // Never uploaded or drawn, so it cannot show unattenuated
                // colour; listed, not a fog failure (shadow volumes on Naboo).
                if (fog) fog_uncomposed.push_back(asset.logical_path + " surface "
                    + std::to_string(index) + " shader " + surface.shader
                    + ": not drawn (scene surface or legacy selector unsupported)");
                continue;
            }
            const assets::Mesh& source_mesh = model->meshes[surface.mesh_index];
            assets::Model single;
            single.source = model->source;
            single.bones = model->bones;
            assets::Mesh mesh = source_mesh;
            mesh.submeshes = {source_mesh.submeshes[surface.submesh_index]};
            single.meshes.push_back(std::move(mesh));
            const assets::Submesh& submesh = single.meshes.front().submeshes.front();

            assets::Texture texture = placeholder_texture();
            for (const scene::TextureBinding& binding : surface.textures) {
                if (!ieq(binding.parameter, "BaseTexture") || binding.resolved.empty()) continue;
                auto found = textures.find(binding.resolved);
                if (found == textures.end()) {
                    auto decoded = assets::load_texture(*filesystem, binding.resolved);
                    found = textures.emplace(binding.resolved,
                        decoded ? std::move(decoded.value()) : placeholder_texture()).first;
                }
                texture = found->second;
            }
            MaterialDescription authored{
                .schema_version = MaterialDescription::current_schema_version,
                .route = MaterialRoute::legacy_effect,
                .pass = selector->transparent ? RenderPass::transparent : RenderPass::opaque,
                .program = std::string(selector->program),
                .technique = std::string(selector->technique),
                .pass_name = std::string(selector->pass),
                .bindings = {},
            };
            for (const assets::MaterialParameter& parameter : submesh.parameters) {
                authored.bindings.push_back({parameter.name, parameter.value});
            }
            if (authored.program == "MeshAdditiveVColor.fx") {
                authored.bindings.push_back({"eawr_effect_time", 0.0F});
            }
            float tree_sway = 0.0F;
            float grass_sway = 0.0F;
            if (legacy::reads_wind(authored)) {
                if (!wind_bones) wind_bones = reference_bones(*model);
                const auto box = lighting::wind::model_bend_box(*model, *wind_bones);
                if (box) bend_boxes.emplace(asset.asset_id, *box);
                bind_wind_frame(authored, box.value_or(lighting::wind::Box{}), source_mesh, submesh, *wind_bones);
                const float bend_scale = std::abs(legacy::bindings::scalar_or(authored, "BendScale", 1.0F));
                if (selector->program == "Grass.fx") {
                    float reach = 0.0F;
                    for (const assets::Vertex& vertex : submesh.vertices) {
                        reach = std::max(reach, std::abs(1.0F - vertex.texcoord[0].y));
                    }
                    const auto wave = lighting::wind::grass_wave(bend_scale, wind_speed);
                    if (wind_speed > 0.0F) grass_sway = reach * (wave.bend_bias + wave.bend_scale);
                } else {
                    float height = 0.0F;
                    for (const assets::Vertex& vertex : submesh.vertices) {
                        height = std::max(height, std::abs(vertex.position.z));
                    }
                    tree_sway = bend_scale * height * height * wind_speed;
                }
            }
            const bool skinned = !single.bones.empty()
                && (single.meshes.front().bone >= 0 || !submesh.skin_bones.empty());
            std::optional<std::vector<animation::BonePose>> bind_pose;
            if (skinned) {
                // Without an idle clip each rigid mesh draws at its bone's
                // bind-pose attachment.
                auto player = animation::Player::create(single);
                if (player) {
                    if (auto pose = player.value().sample({})) bind_pose = std::move(pose.value().bones);
                }
            }
            // The family's own samplers (NormalTexture, #199) through the
            // scene's resolved references.
            const std::vector<GodotRenderer::BindingTexture> family_textures = family_binding_textures(authored,
                [&](const std::string_view declared) -> std::optional<assets::Texture> {
                    for (const scene::TextureBinding& binding : surface.textures) {
                        if (binding.declared != declared || binding.resolved.empty()) continue;
                        auto found = textures.find(binding.resolved);
                        if (found == textures.end()) {
                            auto decoded = assets::load_texture(*filesystem, binding.resolved);
                            if (!decoded) return std::nullopt;
                            found = textures.emplace(binding.resolved, std::move(decoded.value())).first;
                        }
                        return found->second;
                    }
                    return std::nullopt;
                });
            const std::set<Colour> uncoloured{Colour{}};
            const std::set<Colour>& colours = scene::colorizes(surface.shader)
                ? asset_colours[asset.asset_id] : uncoloured;
            for (const Colour& colour : colours) {
                MaterialDescription material = authored;
                if (colour) bind_team_colour(material, *colour);
                const auto uploaded = renderer->upload(next_asset, single, texture, material, family_textures);
                if (!uploaded) {
                    ++surfaces_failed;
                    if (fog) fog_unsupported.push_back(asset.logical_path + " surface "
                        + std::to_string(index) + ": upload failed: "
                        + core::format_diagnostic(uploaded.error()));
                    if (first_surface_failure.empty()) {
                        first_surface_failure = core::format_diagnostic(uploaded.error());
                    }
                    continue;
                }
                SurfaceUpload upload;
                upload.renderer_asset = next_asset++;
                if (fog) {
                    const auto declared = renderer->declare_fog_consumer(upload.renderer_asset);
                    if (!declared) fog_unsupported.push_back(asset.logical_path + " surface "
                        + std::to_string(index) + " asset " + std::to_string(upload.renderer_asset)
                        + ": " + core::format_diagnostic(declared.error()));
                }
                // Grass declares no shadow volume and its models carry none.
                if (selector->program == "Grass.fx") renderer->set_casts_shadows(upload.renderer_asset, false);
                ++surfaces_uploaded;
                if (colour) ++team_colour_variants;
                if (legacy::reads_wind(material)) {
                    ++(selector->program == "Grass.fx" ? wind_grass_surfaces : wind_tree_surfaces);
                }
                upload.tree_sway = tree_sway;
                upload.grass_sway = grass_sway;
                upload.pose = bind_pose;
                upload.submesh = &model->meshes[surface.mesh_index].submeshes[surface.submesh_index];
                upload.bone = single.meshes.front().bone;
                posed_bounds(submesh, upload.bone, upload.pose ? &*upload.pose : nullptr,
                    upload.minimum, upload.maximum);
                ++renderer_assets;
                if (material.program == "MeshAdditiveVColor.fx") effect_clock_assets.push_back(upload.renderer_asset);
                uploads.emplace(std::make_tuple(asset.asset_id, index, colour), std::move(upload));
            }
        }
    }

    // Idle clips, one player per (model, clip). A placement whose clip does
    // not bind keeps the bind pose and is counted by status.
    struct ClipBinding final {
        std::optional<std::size_t> clip;
        std::string status;
    };
    std::map<std::pair<std::string, std::string>, ClipBinding> clip_players;
    const std::uint32_t final_sample = options.particle_frames - 1U;
    const auto bind_clip = [&](const scene::Placement& placement) -> std::optional<std::size_t> {
        if (placement.idle_animation_status != "corpus_naming_observed" || placement.idle_animation.empty()) {
            ++unit_clip_status["clip_absent"];
            return std::nullopt;
        }
        const auto key = std::make_pair(placement.model_path, placement.idle_animation);
        auto found = clip_players.find(key);
        if (found == clip_players.end()) {
            ClipBinding binding{std::nullopt, "bound"};
            const assets::Model* model = cache.model(placement.model_path);
            auto decoded = assets::load_animation(*filesystem, placement.idle_animation);
            if (model == nullptr || !decoded) {
                binding.status = "clip_decode_failed";
            } else if (auto player = animation::Player::create(*model, &decoded.value()); !player) {
                binding.status = "clip_binding_failed";
            } else if (!particles::map_owner_sample(player.value(), final_sample)) {
                // The owner clock needs an integral frame rate (exact n/30 s).
                binding.status = "clip_rate_unsupported";
            } else {
                binding.clip = unit_clips.size();
                unit_clips.push_back(std::make_shared<const animation::Player>(std::move(player.value())));
            }
            found = clip_players.emplace(key, std::move(binding)).first;
        }
        ++unit_clip_status[found->second.status];
        return found->second.clip;
    };
    // Retail starts every created land object's Idle_Anim_00 the same way as
    // a space object's (#157): at Idle_Anim_00_Rate_Mod from a random frame,
    // looping when Loop_Idle_Anim_00 is set, restarted by an IDLE behaviour
    // named in Behavior or LandBehavior. Land has no DUMMY_STARSHIP exception.
    const auto idle_placement = [&](const scene::Placement& placement, const std::size_t clip) {
        IdlePlacement idle{.clip = clip};
        if (auto object = catalog->resolve(placement.object_id, data::Category::game_object)) {
            const DeclaredIdle declared = declared_idle(scene::idle_tags(object.value(), assets::MapKind::land));
            idle.playback = declared.playback;
            if (declared.rejected_rate) idle_rate_rejected[placement.object_id] = *declared.rejected_rate;
        }
        idle.start_frame = placement_start_frame(placement, unit_clips[clip]->playable_frames());
        IdleObject& entry = idle_objects[placement.object_id];
        entry.clip = placement.idle_animation;
        entry.playback = idle.playback;
        ++entry.placements;
        return idle;
    };
    // Captures show each placement's pose at the held sample plus the idle
    // offset, so the attributable bounds are taken there; the live view has
    // no capture to attribute.
    const std::uint64_t capture_tick = static_cast<std::uint64_t>(final_sample) + idle_offset.value_or(0U);

    terrain_snapshot = std::make_shared<const sim::RenderSnapshot>(0, instances);
    for (const scene::Placement& placement : scene->placements) {
        if (!placement.drawable()) continue;
        const sim::math::Mat3x4 matrix = source_to_render(placement.transform->matrix);
        Bounds bounds{{1e30F, 1e30F, 1e30F}, {-1e30F, -1e30F, -1e30F}};
        bool drawn = false;
        bool animated = false;
        bool any_skinned = false;
        float sway = 0.0F;
        for (std::size_t index = 0; index < placement.surfaces.size(); ++index) {
            const auto upload = uploads.find(std::make_tuple(placement.asset_id, index,
                surface_colour(placement, placement.surfaces[index])));
            if (upload != uploads.end() && upload->second.pose) any_skinned = true;
        }
        const std::optional<std::size_t> clip = any_skinned ? bind_clip(placement) : std::nullopt;
        std::optional<std::size_t> idle;
        std::optional<animation::Pose> capture_pose;
        if (clip) {
            idle = idle_placements.size();
            idle_placements.push_back(idle_placement(placement, *clip));
            const IdlePlacement& bound = idle_placements.back();
            if (auto pose = animation::sample_idle(*unit_clips[*clip], bound.playback, bound.start_frame,
                    capture_tick, particles::map_owner_ticks_per_second)) {
                capture_pose = std::move(pose.value());
            } else {
                ++idle_sample_failures;
            }
        }
        const auto variants = capture_variants.find(placement.asset_id);
        const assets::Model* capture_model = placement.capture_point
            && placement.capture_state == scene::Placement::CaptureState::uncaptured
            && variants != capture_variants.end() ? cache.model(placement.model_path) : nullptr;
        for (std::size_t index = 0; index < placement.surfaces.size(); ++index) {
            if (capture_model != nullptr) {
                const auto& surface = placement.surfaces[index];
                const auto& mesh = capture_model->meshes[surface.mesh_index];
                if (!scene::uncaptured_mesh_visible(*capture_model, mesh, variants->second)) continue;
            }
            const auto upload = uploads.find(std::make_tuple(placement.asset_id, index,
                surface_colour(placement, placement.surfaces[index])));
            if (upload == uploads.end()) continue;
            const sim::EntityId entity = next_entity++;
            instances.push_back({entity, upload->second.renderer_asset, matrix});
            ++unit_instances;
            drawn = true;
            sway = std::max(sway, upload->second.grass_sway);
            if (const auto box = bend_boxes.find(placement.asset_id);
                upload->second.tree_sway > 0.0F && box != bend_boxes.end()) {
                // W-04: the world box height through the placement's absolute matrix.
                const assets::Vec3f& half = box->second.half_extent;
                const sim::math::Mat3x4& m = placement.transform->matrix;
                const float height = 2.0F * (std::abs(to_float(m.rows[2][0]) * half.x)
                    + std::abs(to_float(m.rows[2][1]) * half.y) + std::abs(to_float(m.rows[2][2]) * half.z));
                if (height > 0.0F) sway = std::max(sway, upload->second.tree_sway / (height * height));
            }
            std::array<float, 3> low = upload->second.minimum;
            std::array<float, 3> high = upload->second.maximum;
            if (upload->second.pose) {
                ++skinned_instances;
                const auto posed = renderer->set_skin_pose(
                    entity, upload->second.renderer_asset, *upload->second.pose);
                if (!posed && first_surface_failure.empty()) {
                    first_surface_failure = core::format_diagnostic(posed.error());
                }
                if (idle) {
                    animated_instances.push_back({entity, upload->second.renderer_asset, *idle});
                    animated = true;
                    if (capture_pose) {
                        posed_bounds(*upload->second.submesh, upload->second.bone, &capture_pose->bones, low, high);
                    }
                }
            }
            if (low[0] > high[0]) continue;
            for (int corner = 0; corner < 8; ++corner) {
                const std::array<float, 3> source_corner{
                    (corner & 1) ? high[0] : low[0], (corner & 2) ? high[1] : low[1],
                    (corner & 4) ? high[2] : low[2]};
                // Source-basis model point -> source-basis world point ->
                // render basis, the same conversion the snapshot matrix got.
                const std::array<float, 3> world = apply(placement.transform->matrix, source_corner);
                const std::array<float, 3> render{world[0], world[2], -world[1]};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    bounds.minimum[axis] = std::min(bounds.minimum[axis], render[axis]);
                    bounds.maximum[axis] = std::max(bounds.maximum[axis], render[axis]);
                }
            }
        }
        if (animated) ++units_animated;
        if (sway > 0.0F && bounds.minimum[0] <= bounds.maximum[0]) {
            // The wind is horizontal (source z 0), so only render x and z widen.
            for (const std::size_t axis : {std::size_t{0}, std::size_t{2}}) {
                bounds.minimum[axis] -= sway;
                bounds.maximum[axis] += sway;
            }
            ++wind_widened_placements;
        }
        if (drawn && bounds.minimum[0] <= bounds.maximum[0]) unit_bounds.push_back(bounds);
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
