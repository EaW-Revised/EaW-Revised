#include "map_mode_internal.hpp"
#include "render_profile_viewport.hpp"

#include "family_textures.hpp"
#include "legacy/registry.hpp"

namespace eawr::presentation::godot_backend {
namespace map_mode_detail {

[[nodiscard]] std::string effect_name(const terrain::SurfaceEffect effect) {
    return std::string(terrain::effect_program(effect));
}

} // namespace map_mode_detail

bool MapMode::State::verify_capture(const CaptureResult& capture) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(capture.png_bytes.size()));
    if (!capture.png_bytes.empty()) {
        std::memcpy(encoded.ptrw(), capture.png_bytes.data(), capture.png_bytes.size());
    }
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) {
        failure = "capture PNG could not be decoded";
        return false;
    }
    if (camera_view == "tactical") {
        // A pitched tactical frame has no single terrain footprint: terrain
        // and sky share it. The claim is only that the frame is drawn and not
        // flat; inside_coverage is the fraction of samples that differ from
        // the frame's mean colour.
        double mean_r = 0.0, mean_g = 0.0, mean_b = 0.0;
        std::size_t samples = 0;
        for (int32_t y = 0; y < image->get_height(); y += 4) {
            for (int32_t x = 0; x < image->get_width(); x += 4) {
                const Color pixel = image->get_pixel(x, y);
                mean_r += pixel.r;
                mean_g += pixel.g;
                mean_b += pixel.b;
                ++samples;
            }
        }
        if (samples == 0) {
            failure = "capture geometry left an empty evidence population";
            return false;
        }
        mean_r /= static_cast<double>(samples);
        mean_g /= static_cast<double>(samples);
        mean_b /= static_cast<double>(samples);
        std::size_t varied = 0;
        for (int32_t y = 0; y < image->get_height(); y += 4) {
            for (int32_t x = 0; x < image->get_width(); x += 4) {
                const Color pixel = image->get_pixel(x, y);
                if (std::abs(pixel.r - mean_r) + std::abs(pixel.g - mean_g) + std::abs(pixel.b - mean_b) > 0.04) {
                    ++varied;
                }
            }
        }
        inside_coverage = static_cast<float>(varied) / static_cast<float>(samples);
        outside_coverage = 0.0F;
        if (inside_coverage < 0.05F) {
            failure = "tactical capture is flat: nothing distinguishable was drawn";
            return false;
        }
        return true;
    }
    // The clear colour is whatever the renderer's own environment produces;
    // taking it from a corner keeps the check independent of that choice.
    const Color background = image->get_pixel(0, 0);
    std::size_t inside_total{};
    std::size_t inside_drawn{};
    std::size_t outside_total{};
    std::size_t outside_drawn{};
    // A margin either side of the computed footprint keeps a pixel on the
    // silhouette out of both populations instead of crediting it to one.
    const float inner = 0.85F;
    const float outer = 1.15F;
    for (int32_t y = 0; y < image->get_height(); y += 2) {
        for (int32_t x = 0; x < image->get_width(); x += 2) {
            const Color pixel = image->get_pixel(x, y);
            const float difference = std::abs(pixel.r - background.r)
                + std::abs(pixel.g - background.g) + std::abs(pixel.b - background.b);
            const bool drawn = difference > 0.04F;
            const float u = (static_cast<float>(x) / static_cast<float>(image->get_width())) * 2.0F - 1.0F;
            const float v = (static_cast<float>(y) / static_cast<float>(image->get_height())) * 2.0F - 1.0F;
            const bool within_inner = std::abs(u) <= footprint_half_width * inner
                && std::abs(v) <= footprint_half_height * inner;
            const bool beyond_outer = std::abs(u) > footprint_half_width * outer
                || std::abs(v) > footprint_half_height * outer;
            if (within_inner) {
                ++inside_total;
                if (drawn) ++inside_drawn;
            } else if (beyond_outer) {
                ++outside_total;
                if (drawn) ++outside_drawn;
            }
        }
    }
    if (inside_total == 0 || outside_total == 0) {
        failure = "capture geometry left an empty evidence population";
        return false;
    }
    inside_coverage = static_cast<float>(inside_drawn) / static_cast<float>(inside_total);
    outside_coverage = static_cast<float>(outside_drawn) / static_cast<float>(outside_total);
    if (!fog && inside_coverage < 0.95F) {
        failure = "terrain footprint is not covered by drawn pixels";
        return false;
    }
    if (skydome_drawn) {
        // A skydome is authored to be seen from inside it at ground level, not
        // from a top-down capture that sits above part of the dome, so the
        // claim made here is that the dome drew outside the terrain footprint -
        // not that it filled the frame. Whether it fills a ground-level view is
        // part of the original-screenshot comparison, which is a user task.
        if (outside_coverage < 0.10F) {
            failure = "a resolved skydome drew nothing outside the terrain footprint";
            return false;
        }
    } else if (outside_coverage > 0.05F) {
        failure = "pixels were drawn outside the terrain footprint with no skydome present";
        return false;
    }
    return true;
}

namespace map_mode_detail {

[[nodiscard]] Ref<Image> decode_png(const std::vector<std::byte>& bytes) {
    PackedByteArray encoded;
    encoded.resize(static_cast<int64_t>(bytes.size()));
    if (!bytes.empty()) std::memcpy(encoded.ptrw(), bytes.data(), bytes.size());
    Ref<Image> image;
    image.instantiate();
    if (image->load_png_from_buffer(encoded) != OK || image->is_empty()) return {};
    return image;
}

} // namespace map_mode_detail

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

std::array<float, 2> MapMode::State::project(const std::array<float, 3>& point) const {
    // Look-at perspective camera (the top-down overview looks along -Y with
    // screen-up = -Z; the tactical view is pitched). Pixel y grows downward.
    const auto sub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    const auto unit = [&](const std::array<float, 3>& a) {
        const float length = std::sqrt(dot(a, a));
        return length > 0.0F ? std::array<float, 3>{a[0] / length, a[1] / length, a[2] / length} : a;
    };
    const std::array<float, 3> forward = unit(sub(camera.target, camera.eye));
    const std::array<float, 3> right = unit(cross(forward, camera.up));
    const std::array<float, 3> up = cross(right, forward);
    const std::array<float, 3> offset = sub(point, camera.eye);
    const float half_fov = camera.vertical_fov_degrees * 0.5F * 3.14159265F / 180.0F;
    const float depth = std::max(dot(offset, forward), 1.0e-3F);
    const float half_height = depth * std::tan(half_fov);
    const float aspect = static_cast<float>(camera.width) / static_cast<float>(camera.height);
    const float u = dot(offset, right) / (half_height * aspect);
    const float v = -dot(offset, up) / half_height;
    return {(u + 1.0F) * 0.5F * static_cast<float>(camera.width),
            (v + 1.0F) * 0.5F * static_cast<float>(camera.height)};
}

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
        if (auto object = catalog->resolve(placement.object_id)) {
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

void MapMode::State::pose_units(const std::uint32_t sample) {
    if (idle_placements.empty() || unit_sample == sample) return;
    unit_sample = sample;
    const std::uint64_t tick = static_cast<std::uint64_t>(sample) + idle_offset.value_or(0U);
    std::vector<std::optional<animation::Pose>> poses(idle_placements.size());
    for (std::size_t index = 0; index < idle_placements.size(); ++index) {
        const IdlePlacement& idle = idle_placements[index];
        auto pose = animation::sample_idle(*unit_clips[idle.clip], idle.playback, idle.start_frame, tick,
                                           particles::map_owner_ticks_per_second);
        if (pose) poses[index] = std::move(pose.value());
        else ++idle_sample_failures;
    }
    for (const AnimatedInstance& instance : animated_instances) {
        if (!poses[instance.idle]) continue;
        const auto posed = renderer->set_skin_pose(instance.entity, instance.asset, poses[instance.idle]->bones);
        if (!posed && first_surface_failure.empty()) first_surface_failure = core::format_diagnostic(posed.error());
    }
}

void MapMode::State::advance_wind(const double delta) {
    if (!renderer) return;
    const std::uint32_t offset = idle_offset.value_or(0U);
    if (options.interactive) {
        // Real frame time from the idle offset on, by the scene clock rule (W-01).
        wind_clock_seconds = frame <= 1U
            ? lighting::wind::clock_at(static_cast<double>(offset)
                                       / static_cast<double>(particles::map_owner_ticks_per_second))
            : lighting::wind::advance_clock(wind_clock_seconds, static_cast<float>(delta));
    } else {
        // The idle clips' held sample, so a capture repeats.
        const std::uint32_t sample = std::min(frame, options.particle_frames) - 1U;
        wind_clock_seconds = lighting::wind::clock_at((static_cast<double>(sample) + static_cast<double>(offset))
                                                      / static_cast<double>(particles::map_owner_ticks_per_second));
    }
    // Source (x, y, z) -> render (x, z, -y).
    const assets::Vec3f source = wind ? wind->vector : assets::Vec3f{};
    renderer->set_wind({.wind = {source.x, source.z, -source.y}, .scene_time = wind_clock_seconds});
}

bool MapMode::State::fog_can_reveal(const Bounds& bounds) const {
    if (!fog) return true;
    // The posed unit's source XY footprint is render (X,-Z). Any nonzero
    // intersecting cell makes the unit potentially visible; this conservative
    // test never excuses a unit that might contribute coloured pixels.
    const auto grids = fog->effective();
    const auto* grid = grids.find(fog->team());
    if (!grid) return false;
    const auto& desc = grid->desc();
    constexpr double q24 = 16777216.0;
    const double origin_x = static_cast<double>(desc.origin_x_raw) / q24;
    const double origin_y = static_cast<double>(desc.origin_y_raw) / q24;
    const double cell_x = static_cast<double>(desc.cell_x_raw) / q24;
    const double cell_y = static_cast<double>(desc.cell_y_raw) / q24;
    const double min_x = bounds.minimum[0], max_x = bounds.maximum[0];
    const double min_y = -bounds.maximum[2], max_y = -bounds.minimum[2];
    if (max_x <= origin_x || min_x >= origin_x + cell_x * desc.width
        || max_y <= origin_y || min_y >= origin_y + cell_y * desc.height) return false;
    const auto first = [](const double world, const double origin, const double size,
                          const std::uint32_t dimension) {
        return static_cast<std::uint32_t>(std::clamp(
            std::floor((world - origin) / size), 0.0, static_cast<double>(dimension - 1)));
    };
    const std::uint32_t x0 = first(min_x, origin_x, cell_x, desc.width);
    const std::uint32_t x1 = first(max_x, origin_x, cell_x, desc.width);
    const std::uint32_t y0 = first(min_y, origin_y, cell_y, desc.height);
    const std::uint32_t y1 = first(max_y, origin_y, cell_y, desc.height);
    for (std::uint32_t y = y0; y <= y1; ++y) {
        for (std::uint32_t x = x0; x <= x1; ++x) {
            if (grid->cell(x, y).value_or(0) != 0) return true;
        }
    }
    return false;
}

bool MapMode::State::verify_units(
    const std::vector<std::byte>& populated, const std::vector<std::byte>& terrain_only) {
    const Ref<Image> with_units = decode_png(populated);
    const Ref<Image> without_units = decode_png(terrain_only);
    if (with_units.is_null() || without_units.is_null()
        || with_units->get_width() != without_units->get_width()
        || with_units->get_height() != without_units->get_height()) {
        failure = "populated evidence captures could not be decoded or disagree in size";
        return false;
    }
    const int32_t width = with_units->get_width();
    const int32_t height = with_units->get_height();
    const std::size_t stride = static_cast<std::size_t>(width);
    std::vector<std::uint8_t> changed(stride * static_cast<std::size_t>(height));
    std::vector<std::uint8_t> covered(changed.size());
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const Color a = with_units->get_pixel(x, y);
            const Color b = without_units->get_pixel(x, y);
            const float difference = std::abs(a.r - b.r) + std::abs(a.g - b.g) + std::abs(a.b - b.b);
            changed[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)] =
                difference > 0.04F ? 1U : 0U;
        }
    }
    // A unit's projected bounds come from its posed vertices under the same
    // camera, so a changed pixel inside them is attributable to that unit and
    // a changed pixel outside every unit's bounds is not attributable at all.
    for (const Bounds& bounds : unit_bounds) {
        float left = 1e30F, right = -1e30F, top = 1e30F, bottom = -1e30F;
        for (int corner = 0; corner < 8; ++corner) {
            const std::array<float, 3> point{
                (corner & 1) ? bounds.maximum[0] : bounds.minimum[0],
                (corner & 2) ? bounds.maximum[1] : bounds.minimum[1],
                (corner & 4) ? bounds.maximum[2] : bounds.minimum[2]};
            const std::array<float, 2> pixel = project(point);
            left = std::min(left, pixel[0]);
            right = std::max(right, pixel[0]);
            top = std::min(top, pixel[1]);
            bottom = std::max(bottom, pixel[1]);
        }
        // One pixel of margin absorbs rasterisation at the silhouette.
        const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(left)) - 1);
        const int32_t x1 = std::min(width - 1, static_cast<int32_t>(std::ceil(right)) + 1);
        const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(top)) - 1);
        const int32_t y1 = std::min(height - 1, static_cast<int32_t>(std::ceil(bottom)) + 1);
        if (x0 > x1 || y0 > y1) continue;
        ++units_projected;
        const bool visible = fog_can_reveal(bounds);
        if (visible) ++units_expected_visible;
        else ++units_expected_hidden;
        bool any = false;
        for (int32_t y = y0; y <= y1; ++y) {
            for (int32_t x = x0; x <= x1; ++x) {
                const std::size_t index = static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x);
                covered[index] = 1U;
                if (changed[index] != 0U) any = true;
            }
        }
        if (any && visible) ++units_with_coverage;
    }
    if (particles) {
        for (const ParticlePlacement& placement : particles->placements()) {
            if (!placement.has_bounds) continue;
            float left = 1e30F, right = -1e30F, top = 1e30F, bottom = -1e30F;
            for (int corner = 0; corner < 8; ++corner) {
                const auto pixel = project({
                    (corner & 1) ? placement.bounds_max[0] : placement.bounds_min[0],
                    (corner & 2) ? placement.bounds_max[1] : placement.bounds_min[1],
                    (corner & 4) ? placement.bounds_max[2] : placement.bounds_min[2]});
                left = std::min(left, pixel[0]); right = std::max(right, pixel[0]);
                top = std::min(top, pixel[1]); bottom = std::max(bottom, pixel[1]);
            }
            const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(left)) - 2);
            const int32_t x1 = std::min(width - 1, static_cast<int32_t>(std::ceil(right)) + 2);
            const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(top)) - 2);
            const int32_t y1 = std::min(height - 1, static_cast<int32_t>(std::ceil(bottom)) + 2);
            for (int32_t y = y0; y <= y1; ++y) {
                for (int32_t x = x0; x <= x1; ++x) {
                    covered[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)] = 1U;
                }
            }
        }
    }
    for (std::size_t index = 0; index < changed.size(); ++index) {
        if (covered[index] != 0U) {
            if (changed[index] != 0U) ++changed_inside_pixels;
        } else {
            ++outside_pixels;
            if (changed[index] != 0U) ++changed_outside_pixels;
        }
    }
    if (units_projected == 0 && (!particles || particles->placements().empty())) {
        failure = "no drawn placement projects into the capture";
        return false;
    }
    // At the pitched tactical camera, terrain can hide many projected units.
    // Keep the per-unit majority check for the unobscured overview contract.
    const bool majority_required = camera_view == "overview";
    if (units_projected != 0 && ((!fog && (changed_inside_pixels == 0 || units_with_coverage == 0
            || (majority_required && units_with_coverage * 2 < units_projected)))
        || (fog && ((units_expected_visible != 0
                && (changed_inside_pixels == 0 || units_with_coverage == 0
                    || (majority_required && units_with_coverage * 2 < units_expected_visible)))
            || (units_expected_visible == 0 && changed_inside_pixels != 0))))) {
        failure = "unit instances did not add coverage inside their projected bounds";
        return false;
    }
    if (fog && !fog_unit_submissions_verified) {
        failure = "fog unit submissions or material bindings are missing";
        return false;
    }
    // With shadows enabled, unit silhouettes cast beyond their own bounds.
    if (!shadows && outside_pixels != 0 && changed_outside_pixels * 500 > outside_pixels) {
        failure = "adding units changed pixels outside every unit's projected bounds";
        return false;
    }
    return true;
}


GodotRenderer::LightingState MapMode::State::lighting_state(
    const lighting::Policy which, const float max_distance) const {
    GodotRenderer::LightingState state;
    state.shadows = shadows;
    // The environment's shadow colour (TED 0x17 of a map record), as in
    // space, multiplies stored values per channel like the retail stencil
    // darkening (#150, #225, docs/rendering.md#shadows).
    state.shadow_floor = {environment.shadow.r, environment.shadow.g, environment.shadow.b};
    state.shadow_max_distance = max_distance;
    const auto quality = shadow_settings(active_render_profile(), false);
    state.shadow_atlas_size = quality.atlas_size;
    state.shadow_filter = quality.high_filter ? GodotRenderer::ShadowFilter::soft_high
                                            : GodotRenderer::ShadowFilter::soft_medium;
    // Land shadows (#150): four blended cascades weighted toward the camera,
    // so close tactical zooms get fine texels, and a normal bias that clears
    // self-shadow acne on buildings, rocks and foliage. Godot scales both the
    // PCF kernel and the depth bias by the light's blur, which a
    // RenderingServer light leaves at 0 (#291): blur 1 makes the profile's
    // soft filter apply. The depth bias is a percentage of each cascade's
    // depth range times the filter radius, so it stays small (docs/rendering.md#shadows).
    state.shadow_layout = GodotRenderer::ShadowLayout::parallel_4_splits;
    state.shadow_split_offsets = quality.split_offsets;
    state.shadow_blend_splits = true;
    state.shadow_blur = 1.0F;
    state.shadow_bias = 0.05F;
    state.shadow_normal_bias = 5.0F;
    lighting::IrradianceMatrices matrices;
    lighting::IrradianceMatrices fill;
    lighting::Vec3 toward{};
    if (which == lighting::Policy::sh) {
        matrices = lighting::source_to_render(lighting::sph_light_all(environment));
        fill = lighting::source_to_render(lighting::sph_light_fill(environment));
        state.sun_diffuse = lighting::sun_diffuse(environment);
        const lighting::Vec3 sun = environment.lights[0].direction;
        toward = lighting::source_to_render(lighting::Vec3{-sun.x, -sun.y, -sun.z});
        state.specular = lighting::sun_specular(environment);
    } else {
        // The frozen P0 policy keeps its own key direction and specular.
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

std::vector<std::uint8_t> MapMode::State::unit_mask(const int32_t width, const int32_t height) const {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (const Bounds& bounds : unit_bounds) {
        float left = 1e30F, right = -1e30F, top = 1e30F, bottom = -1e30F;
        for (int corner = 0; corner < 8; ++corner) {
            const std::array<float, 2> pixel = project({
                (corner & 1) ? bounds.maximum[0] : bounds.minimum[0],
                (corner & 2) ? bounds.maximum[1] : bounds.minimum[1],
                (corner & 4) ? bounds.maximum[2] : bounds.minimum[2]});
            left = std::min(left, pixel[0]);
            right = std::max(right, pixel[0]);
            top = std::min(top, pixel[1]);
            bottom = std::max(bottom, pixel[1]);
        }
        const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(left)) - 1);
        const int32_t x1 = std::min(width - 1, static_cast<int32_t>(std::ceil(right)) + 1);
        const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(top)) - 1);
        const int32_t y1 = std::min(height - 1, static_cast<int32_t>(std::ceil(bottom)) + 1);
        for (int32_t y = y0; y <= y1; ++y) {
            for (int32_t x = x0; x <= x1; ++x) {
                mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 1U;
            }
        }
    }
    return mask;
}

namespace {

[[nodiscard]] double luminance(const Color& color) {
    return 0.2126 * color.r + 0.7152 * color.g + 0.0722 * color.b;
}

} // namespace

void MapMode::State::measure_shadows() {
    const auto on_bytes = captures.find(configured_frame());
    const auto off_bytes = captures.find("shadows_off");
    if (on_bytes == captures.end() || off_bytes == captures.end()) return;
    const Ref<Image> on = decode_png(on_bytes->second);
    const Ref<Image> off = decode_png(off_bytes->second);
    if (on.is_null() || off.is_null()) {
        shadow_evidence_status = "capture_undecodable";
        return;
    }
    const int32_t width = on->get_width();
    const int32_t height = on->get_height();
    const std::vector<std::uint8_t> units = unit_mask(width, height);
    std::vector<std::uint8_t> region(units.size());
    std::vector<std::uint8_t> hull(units.size());
    const std::array<float, 3> travel{-configured_lighting.toward_light[0], -configured_lighting.toward_light[1],
                                      -configured_lighting.toward_light[2]};
    const auto fill = [&](std::vector<std::uint8_t>& target, const std::array<std::array<float, 3>, 4>& corners) {
        float left = 1e30F, right = -1e30F, top = 1e30F, bottom = -1e30F;
        for (const auto& corner : corners) {
            const std::array<float, 2> pixel = project(corner);
            left = std::min(left, pixel[0]);
            right = std::max(right, pixel[0]);
            top = std::min(top, pixel[1]);
            bottom = std::max(bottom, pixel[1]);
        }
        const int32_t x0 = std::max(0, static_cast<int32_t>(std::ceil(left)));
        const int32_t x1 = std::min(width - 1, static_cast<int32_t>(std::floor(right)));
        const int32_t y0 = std::max(0, static_cast<int32_t>(std::ceil(top)));
        const int32_t y1 = std::min(height - 1, static_cast<int32_t>(std::floor(bottom)));
        for (int32_t y = y0; y <= y1; ++y) {
            for (int32_t x = x0; x <= x1; ++x) {
                target[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 1U;
            }
        }
    };
    // The shadow a box casts on the ground plane under its base contains the
    // top face translated along the light by height / -travel.y. That
    // translated face, shrunk 20% toward its centre, is the measured region;
    // the hull of base and translated face is excluded from the control.
    if (travel[1] < -0.05F) {
        for (const Bounds& bounds : unit_bounds) {
            const float base = bounds.minimum[1];
            const float height_above = bounds.maximum[1] - base;
            if (height_above <= 0.0F) continue;
            const float reach = height_above / -travel[1];
            const float dx = travel[0] * reach;
            const float dz = travel[2] * reach;
            const float cx = (bounds.minimum[0] + bounds.maximum[0]) * 0.5F + dx;
            const float cz = (bounds.minimum[2] + bounds.maximum[2]) * 0.5F + dz;
            const float hx = (bounds.maximum[0] - bounds.minimum[0]) * 0.5F * 0.8F;
            const float hz = (bounds.maximum[2] - bounds.minimum[2]) * 0.5F * 0.8F;
            fill(region, {{{cx - hx, base, cz - hz}, {cx + hx, base, cz - hz}, {cx - hx, base, cz + hz},
                           {cx + hx, base, cz + hz}}});
            const float lx = std::min(bounds.minimum[0], bounds.minimum[0] + dx);
            const float rx = std::max(bounds.maximum[0], bounds.maximum[0] + dx);
            const float lz = std::min(bounds.minimum[2], bounds.minimum[2] + dz);
            const float rz = std::max(bounds.maximum[2], bounds.maximum[2] + dz);
            fill(hull, {{{lx, base, lz}, {rx, base, lz}, {lx, base, rz}, {rx, base, rz}}});
            ++shadow_regions;
        }
    }
    std::uint64_t control_pixels = 0;
    // A pixel counts as darkened when shadows lower its luminance by more
    // than 0.02 (about five 8-bit steps).
    constexpr double darkened = 0.02;
    for (int32_t y = 0; y < height; ++y) {
        for (int32_t x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x);
            const float u = (static_cast<float>(x) / static_cast<float>(width)) * 2.0F - 1.0F;
            const float v = (static_cast<float>(y) / static_cast<float>(height)) * 2.0F - 1.0F;
            const bool inside_footprint = std::abs(u) <= footprint_half_width * 0.85F
                && std::abs(v) <= footprint_half_height * 0.85F;
            if (units[index] != 0U) continue;
            const double lit_on = luminance(on->get_pixel(x, y));
            const double lit_off = luminance(off->get_pixel(x, y));
            if (region[index] != 0U) {
                ++shadow_region_pixels;
                shadow_region_luminance_on += lit_on;
                shadow_region_luminance_off += lit_off;
            }
            if (hull[index] != 0U) {
                ++shadow_hull_pixels;
                if (lit_on < lit_off - darkened) ++shadow_hull_darkened;
            } else if (inside_footprint) {
                ++control_pixels;
                shadow_control_luminance_on += lit_on;
                shadow_control_luminance_off += lit_off;
                if (lit_on < lit_off - darkened) ++shadow_control_darkened;
            }
        }
    }
    if (shadow_region_pixels != 0) {
        shadow_region_luminance_on /= static_cast<double>(shadow_region_pixels);
        shadow_region_luminance_off /= static_cast<double>(shadow_region_pixels);
    }
    if (control_pixels != 0) {
        shadow_control_luminance_on /= static_cast<double>(control_pixels);
        shadow_control_luminance_off /= static_cast<double>(control_pixels);
    }
    shadow_control_pixels = control_pixels;
    // Too few region pixels (units a pixel or two across) cannot carry the
    // claim either way; that is reported, not counted as a pass or a failure.
    // Two criteria, strongest first. Box-shaped casters fill the translated
    // top face, so its mean luminance must fall to 85% or less. Irregular
    // models (the corpus) cast into only part of it, so the fallback is
    // attribution: the fraction of darkened pixels inside the casters' shadow
    // hulls must be at least 2% and at least five times the fraction of
    // darkened pixels everywhere else in the footprint.
    const double hull_fraction = shadow_hull_pixels == 0 ? 0.0
        : static_cast<double>(shadow_hull_darkened) / static_cast<double>(shadow_hull_pixels);
    const double control_fraction = shadow_control_pixels == 0 ? 0.0
        : static_cast<double>(shadow_control_darkened) / static_cast<double>(shadow_control_pixels);
    if (shadow_region_pixels >= 50 && shadow_region_luminance_off > 0.0
        && shadow_region_luminance_on <= 0.85 * shadow_region_luminance_off) {
        shadow_evidence_status = "verified";
        shadow_criterion = "translated_top_face_luminance";
    } else if (shadow_hull_pixels >= 200 && hull_fraction >= 0.02 && hull_fraction >= 5.0 * control_fraction) {
        shadow_evidence_status = "verified";
        shadow_criterion = "darkening_attributed_to_shadow_hulls";
    } else if ((shadow_region_pixels < 50 && shadow_hull_pixels < 200)
        || (shadow_hull_darkened == 0 && shadow_control_darkened == 0)) {
        // Too small to measure, or no shadow lands in view at all: the
        // terrain never casts (#150), so a close view of bare terrain has none.
        shadow_evidence_status = "not_measurable";
    } else if (shadow_hull_pixels >= 200 && hull_fraction > control_fraction) {
        // Darkening concentrates in the hulls but below the criteria above
        // (for example a low sun that also self-shadows the terrain). Reported
        // as such; only a clear negative fails the run.
        shadow_evidence_status = "inconclusive";
    } else {
        shadow_evidence_status = "failed";
    }
}

void MapMode::State::measure_policies() {
    const auto configured = captures.find(shadows ? "shadows_off" : configured_frame());
    const auto other = captures.find("other_policy");
    if (configured == captures.end() || other == captures.end()) return;
    const std::string configured_name(lighting::to_string(policy));
    const std::string other_name(policy == lighting::Policy::sh ? "hemisphere" : "sh");
    for (const auto& [name, bytes] : {std::pair<std::string, const std::vector<std::byte>*>{configured_name, &configured->second},
                                      std::pair<std::string, const std::vector<std::byte>*>{other_name, &other->second}}) {
        const Ref<Image> image = decode_png(*bytes);
        if (image.is_null()) {
            policy_evidence_status = "capture_undecodable";
            return;
        }
        const int32_t width = image->get_width();
        const int32_t height = image->get_height();
        const std::vector<std::uint8_t> units = unit_mask(width, height);
        PolicyLuminance result;
        std::uint64_t saturated = 0;
        for (int32_t y = 0; y < height; y += 2) {
            for (int32_t x = 0; x < width; x += 2) {
                const float u = (static_cast<float>(x) / static_cast<float>(width)) * 2.0F - 1.0F;
                const float v = (static_cast<float>(y) / static_cast<float>(height)) * 2.0F - 1.0F;
                if (std::abs(u) > footprint_half_width * 0.85F || std::abs(v) > footprint_half_height * 0.85F) continue;
                if (units[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] != 0U) continue;
                const Color pixel = image->get_pixel(x, y);
                result.mean += luminance(pixel);
                if (pixel.r >= 0.995F || pixel.g >= 0.995F || pixel.b >= 0.995F) ++saturated;
                ++result.pixels;
            }
        }
        if (result.pixels != 0) {
            result.mean /= static_cast<double>(result.pixels);
            result.saturated_fraction = static_cast<double>(saturated) / static_cast<double>(result.pixels);
        }
        policy_luminance[name] = result;
    }
    const auto& sh = policy_luminance["sh"];
    const auto& hemisphere = policy_luminance["hemisphere"];
    const auto usable = [](const PolicyLuminance& value) {
        return value.pixels != 0 && value.mean > 0.02 && value.mean < 0.98 && value.saturated_fraction < 0.5;
    };
    // The requested policy must be usable and distinct. A map's authored
    // environment may saturate the unrequested comparison policy.
    const auto& selected = policy == lighting::Policy::sh ? sh : hemisphere;
    policy_evidence_status = usable(selected) && sh.pixels != 0 && hemisphere.pixels != 0
        && std::abs(sh.mean - hemisphere.mean) > 0.01
        ? "verified" : "failed";
}

} // namespace eawr::presentation::godot_backend
