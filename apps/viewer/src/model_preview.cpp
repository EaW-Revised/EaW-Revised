#include "model_preview.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <utility>
#include <variant>

namespace eawr::viewer::model_preview {
namespace {

using Vec3 = std::array<float, 3>;

// Affine rest transform in row form: point' = rotation * point + translation.
struct Affine final {
    std::array<std::array<float, 3>, 3> rotation{{{1.0F, 0.0F, 0.0F},
                                                  {0.0F, 1.0F, 0.0F},
                                                  {0.0F, 0.0F, 1.0F}}};
    Vec3 translation{};
};

// ALO stores three float4 source columns for vector-left multiplication; the
// output component r is the dot product with column r, translation in its
// fourth element (see animation.cpp from_asset_bone).
[[nodiscard]] Affine from_asset_bone(const std::array<float, 12>& source) noexcept {
    Affine result;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result.rotation[row][column] = source[row * 4 + column];
        }
        result.translation[row] = source[row * 4 + 3];
    }
    return result;
}

[[nodiscard]] Affine compose(const Affine& parent, const Affine& local) noexcept {
    Affine result;
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            float value{};
            for (std::size_t index = 0; index < 3; ++index) {
                value += parent.rotation[row][index] * local.rotation[index][column];
            }
            result.rotation[row][column] = value;
        }
        float translated = parent.translation[row];
        for (std::size_t index = 0; index < 3; ++index) {
            translated += parent.rotation[row][index] * local.translation[index];
        }
        result.translation[row] = translated;
    }
    return result;
}

[[nodiscard]] Vec3 transform_point(const Affine& transform, const Vec3& point) noexcept {
    Vec3 result{};
    for (std::size_t row = 0; row < 3; ++row) {
        result[row] = transform.rotation[row][0] * point[0] + transform.rotation[row][1] * point[1]
            + transform.rotation[row][2] * point[2] + transform.translation[row];
    }
    return result;
}

[[nodiscard]] bool finite(const float value) noexcept { return std::isfinite(value); }

[[nodiscard]] bool finite(const Vec3& value) noexcept {
    return finite(value[0]) && finite(value[1]) && finite(value[2]);
}

[[nodiscard]] float length(const Vec3& value) noexcept {
    return std::sqrt(value[0] * value[0] + value[1] * value[1] + value[2] * value[2]);
}

[[nodiscard]] Vec3 subtract(const Vec3& left, const Vec3& right) noexcept {
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

[[nodiscard]] float dot(const Vec3& left, const Vec3& right) noexcept {
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

[[nodiscard]] Vec3 cross(const Vec3& left, const Vec3& right) noexcept {
    return {left[1] * right[2] - left[2] * right[1], left[2] * right[0] - left[0] * right[2],
        left[0] * right[1] - left[1] * right[0]};
}

[[nodiscard]] Vec3 scaled(const Vec3& value, const float scale) noexcept {
    return {value[0] * scale, value[1] * scale, value[2] * scale};
}

[[nodiscard]] std::array<Vec3, 8> corners(const Bounds& bounds) noexcept {
    std::array<Vec3, 8> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = {(index & 1U) != 0U ? bounds.max[0] : bounds.min[0],
            (index & 2U) != 0U ? bounds.max[1] : bounds.min[1],
            (index & 4U) != 0U ? bounds.max[2] : bounds.min[2]};
    }
    return result;
}

[[nodiscard]] bool valid_bounds(const Bounds& bounds) noexcept {
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!finite(bounds.min[axis]) || !finite(bounds.max[axis])
            || bounds.max[axis] < bounds.min[axis]) {
            return false;
        }
    }
    return true;
}

constexpr float pi = 3.14159265358979323846F;

} // namespace

std::string_view to_string(const Kind kind) noexcept {
    switch (kind) {
    case Kind::frozen_hangar: return "frozen_hangar";
    case Kind::explicit_hangar: return "explicit_hangar";
    case Kind::exploratory_hull: return "exploratory_hull";
    case Kind::unpinned_model: return "unpinned_model";
    }
    return "unknown";
}

std::string_view to_string(const CameraPolicy policy) noexcept {
    switch (policy) {
    case CameraPolicy::frozen_fixed: return "frozen_fixed";
    case CameraPolicy::hull_bounds_fit: return "hull_bounds_fit";
    case CameraPolicy::legacy_mesh_bounds: return "legacy_mesh_bounds";
    }
    return "unknown";
}

bool iequals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](const char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
        };
        if (fold(left[index]) != fold(right[index])) return false;
    }
    return true;
}

std::optional<LegacySelection> legacy_selection(const std::string_view program) {
    if (iequals(program, "MeshGloss.fx")) return LegacySelection{"sph_t0", "sph_t0_p0"};
    if (iequals(program, "MeshBumpColorize.fx")) return LegacySelection{"t0", "t0_p0"};
    if (iequals(program, "RSkinBumpColorize.fx")) return LegacySelection{"sph_t0", "sph_t0_p0"};
    if (iequals(program, "RSkinGloss.fx")) return LegacySelection{"sph_t1", "sph_t1_p0"};
    if (iequals(program, "RSkinGlossColorize.fx")) return LegacySelection{"sph_t0", "sph_t0_p0"};
    return std::nullopt;
}

std::optional<std::string> base_texture_name(const assets::Submesh& submesh) {
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        if (!iequals(parameter.name, "BaseTexture")) continue;
        if (const auto* value = std::get_if<std::string>(&parameter.value)) return *value;
    }
    return std::nullopt;
}

std::string texture_logical_path(const std::string_view name) {
    std::string result{"data/art/textures/"};
    result.append(name);
    return result;
}

Checked<Plan> plan_for(const Request& request) {
    Plan plan;
    if (!iequals(request.model_path, pinned_model_path)) {
        plan.kind = Kind::unpinned_model;
        plan.required_mesh = std::string(request.mesh_name);
        plan.texture_path = std::string(request.texture_override);
        plan.camera = CameraPolicy::legacy_mesh_bounds;
        return Checked<Plan>::ok(std::move(plan));
    }
    plan.expected_model_sha256 = std::string(pinned_model_sha256);
    if (request.mesh_name.empty() || request.mesh_name == hangar_mesh) {
        plan.kind = request.mesh_name.empty() ? Kind::frozen_hangar : Kind::explicit_hangar;
        plan.required_mesh = std::string(hangar_mesh);
        plan.required_program = std::string(hangar_program);
        plan.texture_path = request.texture_override.empty()
            ? std::string(hangar_texture_path) : std::string(request.texture_override);
        plan.expected_texture_sha256 = std::string(hangar_texture_sha256);
        plan.camera = CameraPolicy::frozen_fixed;
        return Checked<Plan>::ok(std::move(plan));
    }
    if (request.mesh_name == hull_mesh) {
        if (!request.texture_override.empty()) {
            return Checked<Plan>::fail("exploratory Hull preview selects its own BaseTexture; "
                                       "--eawr-texture is not accepted with --eawr-mesh Hull");
        }
        if (request.animation_requested) {
            return Checked<Plan>::fail("exploratory Hull preview frames the rest pose; "
                                       "--eawr-animation is not accepted with --eawr-mesh Hull");
        }
        plan.kind = Kind::exploratory_hull;
        plan.required_mesh = std::string(hull_mesh);
        plan.required_program = std::string(hull_program);
        plan.required_base_texture = std::string(hull_base_texture);
        plan.expected_texture_sha256 = std::string(hull_texture_sha256);
        plan.camera = CameraPolicy::hull_bounds_fit;
        plan.exploratory = true;
        return Checked<Plan>::ok(std::move(plan));
    }
    return Checked<Plan>::fail("pinned MC-50 model draws only --eawr-mesh Hangar (frozen) or "
                               "Hull (exploratory preview); requested mesh: "
        + std::string(request.mesh_name));
}

Checked<Selection> select_submesh(const assets::Model& model, const Plan& plan) {
    bool mesh_found{};
    for (std::size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model.meshes[mesh_index];
        if (!plan.required_mesh.empty() && mesh.name != plan.required_mesh) continue;
        mesh_found = true;
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const assets::Submesh& submesh = mesh.submeshes[submesh_index];
            auto material = legacy_selection(submesh.shader);
            if (!material) continue;
            if (!plan.required_program.empty() && !iequals(submesh.shader, plan.required_program)) {
                continue;
            }
            if (plan.exploratory) {
                if (!mesh.visible) {
                    return Checked<Selection>::fail("exploratory Hull preview: mesh " + mesh.name
                        + " is hidden, so the renderer would draw nothing");
                }
                const auto base = base_texture_name(submesh);
                if (!base) {
                    return Checked<Selection>::fail(
                        "exploratory Hull preview: selected material has no BaseTexture");
                }
                if (!plan.required_base_texture.empty()
                    && !iequals(*base, plan.required_base_texture)) {
                    return Checked<Selection>::fail("exploratory Hull preview: BaseTexture is "
                        + *base + ", expected " + plan.required_base_texture);
                }
            }
            return Checked<Selection>::ok({mesh_index, submesh_index, std::move(*material)});
        }
    }
    if (!plan.exploratory) {
        return Checked<Selection>::fail("selected model/mesh has no implemented drawable material");
    }
    if (!mesh_found) {
        return Checked<Selection>::fail(
            "exploratory Hull preview: requested mesh " + plan.required_mesh + " is missing");
    }
    return Checked<Selection>::fail("exploratory Hull preview: mesh " + plan.required_mesh
        + " has no " + plan.required_program + " submesh");
}

Checked<std::string> texture_path_for(const Plan& plan, const assets::Submesh& submesh) {
    if (!plan.texture_path.empty()) return Checked<std::string>::ok(plan.texture_path);
    const auto name = base_texture_name(submesh);
    if (!name) return Checked<std::string>::fail("selected material has no BaseTexture path");
    return Checked<std::string>::ok(texture_logical_path(*name));
}

bool hash_matches(const std::string_view actual, const std::string_view expected) noexcept {
    return expected.empty() || actual == expected;
}

Checked<RestPlacement> rest_placement(
    const assets::Model& model, const std::size_t mesh_index, const std::size_t submesh_index) {
    if (mesh_index >= model.meshes.size()
        || submesh_index >= model.meshes[mesh_index].submeshes.size()) {
        return Checked<RestPlacement>::fail("preview mesh selection is out of range");
    }
    const assets::Mesh& mesh = model.meshes[mesh_index];
    const assets::Submesh& submesh = mesh.submeshes[submesh_index];

    // The hierarchy must be parent-first and acyclic with finite transforms,
    // exactly as the animation player requires before it binds a rest pose.
    std::vector<Affine> model_space(model.bones.size());
    for (std::size_t index = 0; index < model.bones.size(); ++index) {
        const assets::Bone& bone = model.bones[index];
        if (bone.parent < -1 || bone.parent >= static_cast<std::int32_t>(index)) {
            return Checked<RestPlacement>::fail(
                "model bone hierarchy is not parent-first and acyclic");
        }
        if (!std::all_of(bone.relative_transform.begin(), bone.relative_transform.end(),
                [](const float value) { return finite(value); })) {
            return Checked<RestPlacement>::fail("model bone rest transform is non-finite: "
                + bone.name);
        }
        const Affine local = from_asset_bone(bone.relative_transform);
        model_space[index] = bone.parent < 0
            ? local : compose(model_space[static_cast<std::size_t>(bone.parent)], local);
    }

    RestPlacement result;
    Affine hierarchical;
    if (mesh.bone >= 0) {
        if (static_cast<std::size_t>(mesh.bone) >= model.bones.size()) {
            return Checked<RestPlacement>::fail("preview mesh references a missing bone");
        }
        hierarchical = model_space[static_cast<std::size_t>(mesh.bone)];
        for (std::int32_t bone = mesh.bone; bone >= 0;
             bone = model.bones[static_cast<std::size_t>(bone)].parent) {
            result.chain.push_back(model.bones[static_cast<std::size_t>(bone)].name);
        }
        std::reverse(result.chain.begin(), result.chain.end());
    }
    // A palette-skinned submesh stores bind-space vertices; only a rigid
    // (bone-attached) submesh is placed by its bone's rest transform.
    const bool rigid = mesh.bone >= 0 && submesh.skin_bones.empty();

    if (submesh.vertices.empty()) {
        return Checked<RestPlacement>::fail("preview submesh has no vertices");
    }
    constexpr float huge = std::numeric_limits<float>::max();
    result.asset = Bounds{{huge, huge, huge}, {-huge, -huge, -huge}};
    for (const assets::Vertex& vertex : submesh.vertices) {
        const Vec3 position{vertex.position.x, vertex.position.y, vertex.position.z};
        if (!finite(position)) {
            return Checked<RestPlacement>::fail("preview submesh has a non-finite vertex");
        }
        for (std::size_t axis = 0; axis < 3; ++axis) {
            result.asset.min[axis] = std::min(result.asset.min[axis], position[axis]);
            result.asset.max[axis] = std::max(result.asset.max[axis], position[axis]);
        }
    }
    result.vertex_count = submesh.vertices.size();
    const float extent = std::max({result.asset.max[0] - result.asset.min[0],
        result.asset.max[1] - result.asset.min[1], result.asset.max[2] - result.asset.min[2]});
    if (!(extent > 1.0e-4F)) {
        return Checked<RestPlacement>::fail("preview submesh bounds are degenerate");
    }
    result.tolerance = std::max(extent * 1.0e-3F, 1.0e-4F);

    const Bounds header{{mesh.bounds_min.x, mesh.bounds_min.y, mesh.bounds_min.z},
        {mesh.bounds_max.x, mesh.bounds_max.y, mesh.bounds_max.z}};
    if (!valid_bounds(header)) {
        return Checked<RestPlacement>::fail("preview mesh header bounds are invalid");
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (result.asset.min[axis] < header.min[axis] - result.tolerance
            || result.asset.max[axis] > header.max[axis] + result.tolerance) {
            return Checked<RestPlacement>::fail(
                "preview vertices fall outside the mesh header bounds");
        }
    }

    // Rigid vertices are stored in their bone's space (the header bounds too),
    // and the renderer draws them at their bone's rest transform, so the
    // framed bounds are the hierarchically placed ones.
    if (rigid) {
        Bounds placed{{huge, huge, huge}, {-huge, -huge, -huge}};
        for (const assets::Vertex& vertex : submesh.vertices) {
            const Vec3 position{vertex.position.x, vertex.position.y, vertex.position.z};
            const Vec3 moved = transform_point(hierarchical, position);
            const float delta = length(subtract(moved, position));
            if (!finite(delta) || !finite(moved)) {
                return Checked<RestPlacement>::fail("preview rest placement is non-finite");
            }
            result.max_placement_delta = std::max(result.max_placement_delta, delta);
            for (std::size_t axis = 0; axis < 3; ++axis) {
                placed.min[axis] = std::min(placed.min[axis], moved[axis]);
                placed.max[axis] = std::max(placed.max[axis], moved[axis]);
            }
        }
        result.asset = placed;
    }
    return Checked<RestPlacement>::ok(std::move(result));
}

Bounds world_bounds(const Bounds& asset, const std::array<float, 16>& instance) {
    constexpr float huge = std::numeric_limits<float>::max();
    Bounds result{{huge, huge, huge}, {-huge, -huge, -huge}};
    for (const Vec3& corner : corners(asset)) {
        const Vec3 render{corner[0], corner[2], -corner[1]};
        for (std::size_t row = 0; row < 3; ++row) {
            const float value = instance[row] * render[0] + instance[4 + row] * render[1]
                + instance[8 + row] * render[2] + instance[12 + row];
            result.min[row] = std::min(result.min[row], value);
            result.max[row] = std::max(result.max[row], value);
        }
    }
    return result;
}

Projection project_corners(const Bounds& world, const presentation::FixedCamera& camera) {
    Projection result;
    const Vec3 forward_raw = subtract(camera.target, camera.eye);
    const float forward_length = length(forward_raw);
    if (!(forward_length > 0.0F) || camera.width == 0 || camera.height == 0) return result;
    const Vec3 forward = scaled(forward_raw, 1.0F / forward_length);
    const Vec3 right_raw = cross(forward, camera.up);
    const float right_length = length(right_raw);
    if (!(right_length > 1.0e-6F)) return result;
    const Vec3 right = scaled(right_raw, 1.0F / right_length);
    const Vec3 up = cross(right, forward);
    const float tan_half_v = std::tan(camera.vertical_fov_degrees * pi / 360.0F);
    const float aspect = static_cast<float>(camera.width) / static_cast<float>(camera.height);
    const float tan_half_h = tan_half_v * aspect;
    constexpr float huge = std::numeric_limits<float>::max();
    result.ndc_min_x = result.ndc_min_y = result.depth_min = huge;
    result.ndc_max_x = result.ndc_max_y = result.depth_max = -huge;
    for (const Vec3& corner : corners(world)) {
        const Vec3 relative = subtract(corner, camera.eye);
        const float depth = dot(relative, forward);
        if (!(depth > 0.0F)) return result;
        const float x = dot(relative, right) / (depth * tan_half_h);
        const float y = dot(relative, up) / (depth * tan_half_v);
        result.ndc_min_x = std::min(result.ndc_min_x, x);
        result.ndc_max_x = std::max(result.ndc_max_x, x);
        result.ndc_min_y = std::min(result.ndc_min_y, y);
        result.ndc_max_y = std::max(result.ndc_max_y, y);
        result.depth_min = std::min(result.depth_min, depth);
        result.depth_max = std::max(result.depth_max, depth);
    }
    result.finite = finite(result.ndc_min_x) && finite(result.ndc_max_x)
        && finite(result.ndc_min_y) && finite(result.ndc_max_y)
        && finite(result.depth_min) && finite(result.depth_max);
    return result;
}

Checked<Fit> fit_camera(const Bounds& world, const presentation::FixedCamera& base,
    const std::array<float, 3>& view_direction, const float margin) {
    if (!valid_bounds(world)) return Checked<Fit>::fail("preview world bounds are invalid");
    if (base.width == 0 || base.height == 0 || !(base.vertical_fov_degrees > 0.0F)
        || !(base.vertical_fov_degrees < 180.0F) || !(margin >= 1.0F) || !finite(margin)) {
        return Checked<Fit>::fail("preview camera base is invalid");
    }
    const float direction_length = length(view_direction);
    if (!finite(view_direction) || !(direction_length > 0.0F)) {
        return Checked<Fit>::fail("preview view direction is invalid");
    }
    const Vec3 direction = scaled(view_direction, 1.0F / direction_length);
    if (!(length(cross(direction, base.up)) > 1.0e-3F)) {
        return Checked<Fit>::fail("preview view direction is parallel to the camera up vector");
    }
    const Vec3 center{(world.min[0] + world.max[0]) * 0.5F, (world.min[1] + world.max[1]) * 0.5F,
        (world.min[2] + world.max[2]) * 0.5F};
    const float radius = 0.5F * length(subtract(world.max, world.min));
    if (!(radius > 0.0F)) return Checked<Fit>::fail("preview world bounds are degenerate");
    const float tan_v = std::tan(base.vertical_fov_degrees * pi / 360.0F);
    const float tan_h = tan_v
        * (static_cast<float>(base.width) / static_cast<float>(base.height));

    // Same camera basis project_corners() uses: the camera looks back along
    // `direction` at the centre.
    const Vec3 forward = scaled(direction, -1.0F);
    const Vec3 right_raw = cross(forward, base.up);
    const Vec3 right = scaled(right_raw, 1.0F / length(right_raw));
    const Vec3 up = cross(right, forward);
    // Exact fit: a corner at lateral offset x and offset `along` towards the
    // eye sits at depth (distance - along), so it stays within 1/margin of the
    // half-extent once distance >= along + |x| * margin / tan(half fov).
    float distance{};
    float nearest_along = -std::numeric_limits<float>::max();
    for (const Vec3& corner : corners(world)) {
        const Vec3 relative = subtract(corner, center);
        const float along = dot(relative, direction);
        nearest_along = std::max(nearest_along, along);
        distance = std::max({distance, along + std::abs(dot(relative, right)) * margin / tan_h,
            along + std::abs(dot(relative, up)) * margin / tan_v});
    }
    // Keep the eye outside the bounding sphere, so every corner has positive
    // depth even when one lies on the view axis.
    distance = std::max(distance, radius * 1.05F);

    Fit fit;
    fit.view_direction = direction;
    fit.camera = base;
    fit.camera.target = center;
    fit.camera.eye = {center[0] + direction[0] * distance, center[1] + direction[1] * distance,
        center[2] + direction[2] * distance};
    // Corner depths lie in [distance - nearest_along, distance + radius].
    fit.camera.near_plane = std::max(0.01F, (distance - nearest_along) * 0.5F);
    fit.camera.far_plane = (distance + radius) * 1.5F;
    if (!finite(fit.camera.eye) || !finite(fit.camera.near_plane) || !finite(fit.camera.far_plane)
        || !(fit.camera.far_plane > fit.camera.near_plane)) {
        return Checked<Fit>::fail("preview camera fit is non-finite");
    }

    const Projection projection = project_corners(world, fit.camera);
    if (!projection.finite) {
        return Checked<Fit>::fail("preview bounds are behind or at the camera");
    }
    fit.ndc_min_x = projection.ndc_min_x;
    fit.ndc_max_x = projection.ndc_max_x;
    fit.ndc_min_y = projection.ndc_min_y;
    fit.ndc_max_y = projection.ndc_max_y;
    fit.depth_min = projection.depth_min;
    fit.depth_max = projection.depth_max;
    if (fit.ndc_min_x < -1.0F || fit.ndc_max_x > 1.0F || fit.ndc_min_y < -1.0F
        || fit.ndc_max_y > 1.0F) {
        return Checked<Fit>::fail("preview bounds do not fit the viewport");
    }
    if (!(fit.depth_min > fit.camera.near_plane) || !(fit.depth_max < fit.camera.far_plane)) {
        return Checked<Fit>::fail("preview bounds do not fit the clip planes");
    }
    return Checked<Fit>::ok(fit);
}

MeshLevels mesh_levels(std::string_view name) {
    // The reference strips each tag and its digits before looking for the
    // next one, so "_ALT1_LOD2" and "_LOD2_ALT1" both parse.
    std::string remaining(name);
    MeshLevels result;
    const std::array<std::pair<std::string_view, int*>, 2> tags{{{"_ALT", &result.alt}, {"_LOD", &result.lod}}};
    for (const auto& [tag, value] : tags) {
        const std::size_t position = remaining.find(tag);
        if (position == std::string::npos) continue;
        std::size_t end = position + tag.size();
        long long parsed = 0;
        while (end < remaining.size() && remaining[end] >= '0' && remaining[end] <= '9') {
            parsed = std::min<long long>(parsed * 10 + (remaining[end] - '0'), std::numeric_limits<int>::max());
            ++end;
        }
        if (end > position + tag.size()) *value = static_cast<int>(parsed);
        remaining.erase(position, end - position);
    }
    return result;
}

UnitLevels unit_levels(const assets::Model& model, const std::optional<int> lod) {
    UnitLevels result;
    for (const assets::Mesh& mesh : model.meshes) {
        const MeshLevels levels = mesh_levels(mesh.name);
        result.max_alt = std::max(result.max_alt, levels.alt);
        result.max_lod = std::max(result.max_lod, levels.lod);
    }
    result.alt = 0;
    result.lod = std::clamp(lod.value_or(result.max_lod), 0, std::max(result.max_lod, 0));
    return result;
}

std::vector<UnitSurface> unit_surfaces(const assets::Model& model, const UnitLevels& levels) {
    std::vector<UnitSurface> result;
    for (std::size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model.meshes[mesh_index];
        if (!mesh.visible) continue;
        const MeshLevels tags = mesh_levels(mesh.name);
        if ((tags.alt != -1 && tags.alt != levels.alt) || (tags.lod != -1 && tags.lod != levels.lod)) continue;
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            result.push_back({mesh_index, submesh_index});
        }
    }
    return result;
}

bool surface_visible(const assets::Model& model, const UnitSurface& surface,
    const std::span<const presentation::animation::BonePose> pose) {
    if (surface.mesh >= model.meshes.size()) return false;
    const std::int32_t bone = model.meshes[surface.mesh].bone;
    if (bone < 0) return true;
    return static_cast<std::size_t>(bone) < pose.size() && pose[static_cast<std::size_t>(bone)].visible;
}

Checked<Bounds> posed_bounds(const assets::Model& model, const std::span<const UnitSurface> surfaces,
    const std::span<const presentation::animation::BonePose> pose) {
    using presentation::animation::Matrix;
    const auto apply = [](const Matrix& m, const assets::Vec3f& p) {
        return Vec3{m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
                    m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
    };
    if (pose.size() != model.bones.size()) return Checked<Bounds>::fail("unit pose does not match the model bones");
    constexpr float huge = std::numeric_limits<float>::max();
    Bounds result{{huge, huge, huge}, {-huge, -huge, -huge}};
    bool any = false;
    for (const UnitSurface& surface : surfaces) {
        if (surface.mesh >= model.meshes.size() || surface.submesh >= model.meshes[surface.mesh].submeshes.size()) {
            return Checked<Bounds>::fail("unit surface selection is out of range");
        }
        if (!surface_visible(model, surface, pose)) continue;
        const assets::Mesh& mesh = model.meshes[surface.mesh];
        const assets::Submesh& submesh = mesh.submeshes[surface.submesh];
        if (mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) >= pose.size()) {
            return Checked<Bounds>::fail("unit mesh " + mesh.name + " references a missing bone");
        }
        for (const assets::Vertex& vertex : submesh.vertices) {
            Vec3 point{vertex.position.x, vertex.position.y, vertex.position.z};
            if (!submesh.skin_bones.empty()) {
                point = {};
                for (std::size_t influence = 0; influence < 4; ++influence) {
                    const float weight = vertex.bone_weights[influence];
                    if (weight == 0.0F) continue;
                    const std::uint32_t local = vertex.bone_indices[influence];
                    if (local >= submesh.skin_bones.size() || submesh.skin_bones[local] >= pose.size()) {
                        return Checked<Bounds>::fail("unit mesh " + mesh.name + " names a palette bone out of range");
                    }
                    const Vec3 moved = apply(pose[submesh.skin_bones[local]].skin_asset, vertex.position);
                    for (std::size_t axis = 0; axis < 3; ++axis) point[axis] += weight * moved[axis];
                }
            } else if (mesh.bone >= 0) {
                point = apply(pose[static_cast<std::size_t>(mesh.bone)].model_asset, vertex.position);
            }
            if (!finite(point)) return Checked<Bounds>::fail("unit mesh " + mesh.name + " places a non-finite vertex");
            for (std::size_t axis = 0; axis < 3; ++axis) {
                result.min[axis] = std::min(result.min[axis], point[axis]);
                result.max[axis] = std::max(result.max[axis], point[axis]);
            }
            any = true;
        }
    }
    if (!any) return Checked<Bounds>::fail("no visible unit vertex to frame");
    return Checked<Bounds>::ok(result);
}

Checked<std::vector<float>> parse_floats(const std::string_view text) {
    std::vector<float> result;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t comma = std::min(text.find(',', start), text.size());
        const std::string token(text.substr(start, comma - start));
        const bool allowed = !token.empty() && std::all_of(token.begin(), token.end(), [](const char c) {
            return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E';
        });
        float value{};
        const auto parsed = allowed ? std::from_chars(token.data(), token.data() + token.size(), value)
                                    : std::from_chars_result{token.data(), std::errc::invalid_argument};
        if (!allowed || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || !finite(value)) {
            return Checked<std::vector<float>>::fail("expected comma-separated finite decimals, got: " + std::string(text));
        }
        result.push_back(value);
        start = comma + 1;
    }
    return Checked<std::vector<float>>::ok(std::move(result));
}

StripLayout strip_layout(const std::size_t frames, const std::uint32_t width, const std::uint32_t height) {
    if (frames <= 1) return {1U, 1U, width, height};
    const auto columns = static_cast<std::uint32_t>(std::min<std::size_t>(frames, 3U));
    const auto rows = static_cast<std::uint32_t>((frames + 2U) / 3U);
    return {columns, rows, std::max(width / 2U, 1U), std::max(height / 2U, 1U)};
}

} // namespace eawr::viewer::model_preview
