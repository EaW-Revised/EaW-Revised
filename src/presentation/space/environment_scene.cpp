#include "eawr/presentation/space/environment_scene.hpp"

#include "eawr/presentation/space/sun_retail.hpp"

#include "eawr/presentation/camera/camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <utility>

namespace eawr::presentation::space {
namespace {

[[nodiscard]] char fold(const char value) noexcept {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
}

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size()
        && std::equal(left.begin(), left.end(), right.begin(), [](const char a, const char b) { return fold(a) == fold(b); });
}

[[nodiscard]] bool finite(const assets::Vec3f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

[[nodiscard]] bool finite(const assets::Vec4f& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
}

[[nodiscard]] float dot(const assets::Vec3f& a, const assets::Vec3f& b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] assets::Vec3f cross(const assets::Vec3f& a, const assets::Vec3f& b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

[[nodiscard]] assets::Vec3f minus(const assets::Vec3f& a, const assets::Vec3f& b) noexcept {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] assets::Vec3f scaled(const assets::Vec3f& a, const float factor) noexcept {
    return {a.x * factor, a.y * factor, a.z * factor};
}

[[nodiscard]] assets::Vec3f plus(const assets::Vec3f& a, const assets::Vec3f& b) noexcept {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] std::optional<assets::Vec3f> unit(const assets::Vec3f& value) noexcept {
    const float length = std::sqrt(dot(value, value));
    if (!(length > 1.0e-6F) || !std::isfinite(length)) return std::nullopt;
    return scaled(value, 1.0F / length);
}

// Little-endian binary32 at `offset`, independent of the host byte order.
[[nodiscard]] float read_f32(const std::vector<std::byte>& bytes, const std::size_t offset) noexcept {
    std::uint32_t raw = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        raw |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (8U * index);
    }
    float value = 0.0F;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

// R-DEC-02/03: the last occurrence long enough to read wins; a longer mini is
// read as its prefix and a shorter one leaves the value unchanged.
[[nodiscard]] std::optional<float> scalar_field(const assets::EnvironmentDescriptor& environment, const std::uint32_t id) {
    std::optional<float> value;
    for (const assets::RawField& field : environment.fields) {
        if (field.id == id && field.bytes.size() >= 4) value = read_f32(field.bytes, 0);
    }
    return value;
}

[[nodiscard]] std::optional<assets::Vec3f> vector_field(const assets::EnvironmentDescriptor& environment,
                                                        const std::uint32_t id) {
    std::optional<assets::Vec3f> value;
    for (const assets::RawField& field : environment.fields) {
        if (field.id == id && field.bytes.size() >= 12) {
            value = assets::Vec3f{read_f32(field.bytes, 0), read_f32(field.bytes, 4), read_f32(field.bytes, 8)};
        }
    }
    return value;
}

using Rigid = std::array<float, 12>;

[[nodiscard]] assets::Vec3f rotate(const Rigid& m, const assets::Vec3f& v) noexcept {
    return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z,
            m[8] * v.x + m[9] * v.y + m[10] * v.z};
}

[[nodiscard]] Rigid compose(const Rigid& parent, const Rigid& child) noexcept {
    Rigid result{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto column = rotate(parent, {child[axis], child[4 + axis], child[8 + axis]});
        result[axis] = column.x;
        result[4 + axis] = column.y;
        result[8 + axis] = column.z;
    }
    const auto translation = transform_point(parent, {child[3], child[7], child[11]});
    result[3] = translation.x;
    result[7] = translation.y;
    result[11] = translation.z;
    return result;
}

// Empty when every bone from `start` to the root is a finite record with no
// billboard ancestor; the bounded walk also catches cycles. The mesh bone's
// own billboard mode is the caller's concern.
[[nodiscard]] std::string chain_problem(const assets::Model& model, const std::int32_t start) {
    if (start < -1) return "bone index " + std::to_string(start) + " is invalid";
    std::int32_t current = start;
    for (std::size_t steps = 0; current >= 0; ++steps) {
        if (steps > model.bones.size()) return "the bone chain does not terminate";
        if (static_cast<std::size_t>(current) >= model.bones.size()) {
            return "bone index " + std::to_string(current) + " is out of range";
        }
        const assets::Bone& bone = model.bones[static_cast<std::size_t>(current)];
        if (current != start && bone.billboard != 0) {
            return "ancestor bone '" + bone.name + "' is a billboard (mode " + std::to_string(bone.billboard) + ")";
        }
        if (!std::all_of(bone.relative_transform.begin(), bone.relative_transform.end(),
                         [](const float value) { return std::isfinite(value); })) {
            return "bone '" + bone.name + "' has a non-finite transform";
        }
        if (!bone.visible) return "bone '" + bone.name + "' on the mesh's chain is stored hidden";
        if (bone.parent < -1) return "bone '" + bone.name + "' has an invalid parent index";
        current = bone.parent;
    }
    return {};
}

[[nodiscard]] Rigid chain_transform(const assets::Model& model, const std::int32_t start) noexcept {
    Rigid result = identity_affine;
    for (std::int32_t current = start; current >= 0; current = model.bones[static_cast<std::size_t>(current)].parent) {
        result = compose(model.bones[static_cast<std::size_t>(current)].relative_transform, result);
    }
    return result;
}

[[nodiscard]] SceneRoute route_of(const std::string_view shader) noexcept {
    if (ieq(shader, "MeshGloss.fx")) return SceneRoute::meshgloss;
    if (ieq(shader, "MeshAdditive.fx")) return SceneRoute::meshadditive;
    if (ieq(shader, "MeshAdditiveVColor.fx")) return SceneRoute::meshadditive_vcolor;
    if (ieq(shader, "Planet.fx")) return SceneRoute::planet;
    if (ieq(shader, "Nebula.fx")) return SceneRoute::nebula;
    // The alpha companion of the planet model stays with that model's owner.
    if (ieq(shader, "MeshAlpha.fx")) return SceneRoute::legacy_mesh;
    return SceneRoute::unsupported;
}

[[nodiscard]] const assets::MaterialParameter* parameter(const assets::Submesh& submesh, const std::string_view name) {
    const assets::MaterialParameter* found = nullptr;
    for (const assets::MaterialParameter& item : submesh.parameters) {
        if (ieq(item.name, name)) {
            if (found != nullptr) return nullptr;
            found = &item;
        }
    }
    return found;
}

[[nodiscard]] std::optional<assets::Vec4f> vector4(const assets::MaterialParameter* item) {
    if (item == nullptr) return std::nullopt;
    if (const auto* four = std::get_if<assets::Vec4f>(&item->value); four != nullptr && finite(*four)) return *four;
    if (const auto* three = std::get_if<assets::Vec3f>(&item->value); three != nullptr && finite(*three)) {
        return assets::Vec4f{three->x, three->y, three->z, 0.0F};
    }
    return std::nullopt;
}

[[nodiscard]] assets::Model single_surface(const assets::Model& source, const assets::Mesh& mesh,
                                           assets::Submesh submesh) {
    assets::Model result;
    result.source = source.source;
    assets::Mesh copy;
    copy.name = mesh.name;
    copy.bounds_min = mesh.bounds_min;
    copy.bounds_max = mesh.bounds_max;
    copy.submeshes.push_back(std::move(submesh));
    result.meshes.push_back(std::move(copy));
    return result;
}

} // namespace

std::optional<EnvironmentLight> environment_light(const assets::EnvironmentDescriptor& environment) {
    const auto colour = vector_field(environment, 0x00);
    const auto specular = vector_field(environment, 0x03);
    const auto ambient = vector_field(environment, 0x04);
    const auto intensity = scalar_field(environment, 0x05);
    const auto heading = scalar_field(environment, 0x08);
    const auto elevation = scalar_field(environment, 0x0b);
    if (!colour || !specular || !ambient || !intensity || !heading || !elevation) return std::nullopt;
    if (!finite(*colour) || !finite(*specular) || !finite(*ambient) || !std::isfinite(*intensity)
        || !std::isfinite(*heading) || !std::isfinite(*elevation)) {
        return std::nullopt;
    }
    const double a = *heading;
    const double e = *elevation;
    EnvironmentLight light;
    light.toward_light = {static_cast<float>(std::sin(a) * std::cos(e)), static_cast<float>(-std::cos(a) * std::cos(e)),
                          static_cast<float>(std::sin(e))};
    light.diffuse = scaled(*colour, *intensity);
    light.specular = scaled(*specular, 2.0F * *intensity);
    light.ambient = *ambient;
    // Finite inputs can still overflow once scaled; reject the record rather than hand on infinity.
    if (!finite(light.diffuse) || !finite(light.specular)) return std::nullopt;
    return light;
}

assets::Vec3f sky_orientation_degrees(const assets::EnvironmentDescriptor& environment, const bool secondary) {
    const auto first = scalar_field(environment, secondary ? 0x1eU : 0x1dU);
    const auto third = scalar_field(environment, secondary ? 0x20U : 0x1fU);
    const float x = first && std::isfinite(*first) ? *first : 0.0F;
    const float z = third && std::isfinite(*third) ? *third : 0.0F;
    return {x, 0.0F, z};
}

Affine object_transform(const assets::Vec3f& position, const assets::Vec3f& orientation_degrees,
                        const float scale) noexcept {
    constexpr double radians = std::numbers::pi / 180.0;
    using M = std::array<std::array<double, 3>, 3>;
    const auto multiply = [](const M& left, const M& right) {
        M result{};
        for (std::size_t row = 0; row < 3; ++row) {
            for (std::size_t column = 0; column < 3; ++column) {
                for (std::size_t k = 0; k < 3; ++k) result[row][column] += left[row][k] * right[k][column];
            }
        }
        return result;
    };
    const auto rz = [](const double angle) {
        return M{{{std::cos(angle), -std::sin(angle), 0.0}, {std::sin(angle), std::cos(angle), 0.0}, {0.0, 0.0, 1.0}}};
    };
    const auto ry = [](const double angle) {
        return M{{{std::cos(angle), 0.0, std::sin(angle)}, {0.0, 1.0, 0.0}, {-std::sin(angle), 0.0, std::cos(angle)}}};
    };
    const auto rx = [](const double angle) {
        return M{{{1.0, 0.0, 0.0}, {0.0, std::cos(angle), -std::sin(angle)}, {0.0, std::sin(angle), std::cos(angle)}}};
    };
    // The quarter turn is exact, so no rounding enters through cos(pi/2).
    const M quarter{{{0.0, -1.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}}};
    const M rotation = multiply(multiply(multiply(rz(orientation_degrees.z * radians), ry(orientation_degrees.y * radians)),
                                         rx(orientation_degrees.x * radians)),
                                quarter);
    Affine result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result[row * 4 + column] = static_cast<float>(rotation[row][column] * scale);
        }
    }
    result[3] = position.x;
    result[7] = position.y;
    result[11] = position.z;
    return result;
}

assets::Vec3f transform_point(const Affine& transform, const assets::Vec3f& point) noexcept {
    const assets::Vec3f rotated = transform_direction(transform, point);
    return {rotated.x + transform[3], rotated.y + transform[7], rotated.z + transform[11]};
}

assets::Vec3f transform_direction(const Affine& transform, const assets::Vec3f& direction) noexcept {
    return {transform[0] * direction.x + transform[1] * direction.y + transform[2] * direction.z,
            transform[4] * direction.x + transform[5] * direction.y + transform[6] * direction.z,
            transform[8] * direction.x + transform[9] * direction.y + transform[10] * direction.z};
}

Affine render_affine(const Affine& source) noexcept {
    // P (x, y, z) = (x, z, -y): render row i reads source row pick[i] with
    // sign[i], and render column j reads source column pick[j] with sign[j].
    constexpr std::array<std::size_t, 3> pick{0, 2, 1};
    constexpr std::array<float, 3> sign{1.0F, 1.0F, -1.0F};
    Affine result{};
    for (std::size_t row = 0; row < 3; ++row) {
        for (std::size_t column = 0; column < 3; ++column) {
            result[row * 4 + column] = sign[row] * sign[column] * source[pick[row] * 4 + pick[column]];
        }
        result[row * 4 + 3] = sign[row] * source[pick[row] * 4 + 3];
    }
    return result;
}

assets::Vec3f render_from_source(const assets::Vec3f& source) noexcept {
    return {source.x, source.z, -source.y};
}

std::string_view to_string(const EnvironmentFamily family) noexcept {
    switch (family) {
    case EnvironmentFamily::none: return "none";
    case EnvironmentFamily::planet: return "planet";
    case EnvironmentFamily::nebula: return "nebula";
    }
    return "unknown";
}

EnvironmentFamily environment_family(const assets::Model& model) noexcept {
    bool nebula = false;
    for (const assets::Mesh& mesh : model.meshes) {
        if (!mesh.visible) continue;
        for (const assets::Submesh& submesh : mesh.submeshes) {
            if (ieq(submesh.shader, "Planet.fx")) return EnvironmentFamily::planet;
            if (ieq(submesh.shader, "Nebula.fx")) nebula = true;
        }
    }
    return nebula ? EnvironmentFamily::nebula : EnvironmentFamily::none;
}

std::string_view to_string(const SceneRoute route) noexcept {
    switch (route) {
    case SceneRoute::meshgloss: return "meshgloss";
    case SceneRoute::meshadditive: return "meshadditive";
    case SceneRoute::meshadditive_vcolor: return "meshadditive_vcolor";
    case SceneRoute::planet: return "planet";
    case SceneRoute::nebula: return "nebula";
    case SceneRoute::legacy_mesh: return "legacy_mesh";
    case SceneRoute::unsupported: return "unsupported";
    }
    return "unknown";
}

std::vector<SceneSurface> scene_surfaces(const assets::Model& model) {
    std::vector<SceneSurface> result;
    for (std::size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model.meshes[mesh_index];
        if (!mesh.visible) continue;
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const assets::Submesh& submesh = mesh.submeshes[submesh_index];
            SceneSurface surface;
            surface.mesh_index = mesh_index;
            surface.submesh_index = submesh_index;
            surface.mesh_name = mesh.name;
            surface.shader = submesh.shader;
            surface.route = route_of(submesh.shader);
            surface.mesh_bone = mesh.bone;
            const auto note = [&](std::string problem) {
                if (surface.problem.empty()) surface.problem = std::move(problem);
            };
            if (surface.route == SceneRoute::unsupported) note("shader " + submesh.shader + " has no environment route");
            if (mesh.bone >= 0 && static_cast<std::size_t>(mesh.bone) < model.bones.size()) {
                surface.billboard = model.bones[static_cast<std::size_t>(mesh.bone)].billboard;
            }
            if (!submesh.skin_bones.empty()) note("the surface is skinned");
            if (const std::string problem = chain_problem(model, mesh.bone); !problem.empty()) note(problem);
            bool geometry = !submesh.vertices.empty() && !submesh.indices.empty() && submesh.indices.size() % 3 == 0;
            for (const std::uint16_t index : submesh.indices) geometry = geometry && index < submesh.vertices.size();
            for (const assets::Vertex& vertex : submesh.vertices) {
                geometry = geometry && finite(vertex.position) && std::isfinite(vertex.texcoord[0].x)
                    && std::isfinite(vertex.texcoord[0].y);
            }
            if (!geometry) note("the surface has no valid triangles, positions or UVs");
            if (surface.billboard == 4 || surface.billboard == 5 || surface.billboard > 7) {
                note("billboard mode " + std::to_string(surface.billboard) + " has no environment rule");
            }
            if (const auto* base = parameter(submesh, "BaseTexture")) {
                if (const auto* name = std::get_if<std::string>(&base->value)) surface.base_texture = *name;
            }
            if ((surface.route == SceneRoute::meshgloss || surface.route == SceneRoute::meshadditive
                 || surface.route == SceneRoute::meshadditive_vcolor) && surface.base_texture.empty()) {
                note("the surface declares no single BaseTexture");
            }
            if (surface.route == SceneRoute::meshgloss) {
                const auto emissive = vector4(parameter(submesh, "Emissive"));
                const auto diffuse = vector4(parameter(submesh, "Diffuse"));
                const auto specular = vector4(parameter(submesh, "Specular"));
                if (!emissive || !diffuse || !specular) {
                    note("a MeshGloss Emissive, Diffuse or Specular value is missing, repeated or not finite");
                } else {
                    MeshGlossMaterial values;
                    values.emissive = *emissive;
                    values.diffuse = *diffuse;
                    values.specular = *specular;
                    if (const auto* shininess = parameter(submesh, "Shininess")) {
                        if (const auto* value = std::get_if<float>(&shininess->value)) values.shininess = *value;
                    }
                    surface.meshgloss = values;
                }
            }
            if (surface.route == SceneRoute::meshadditive || surface.route == SceneRoute::meshadditive_vcolor) {
                MeshAdditiveMaterial values;
                values.color = {1.0F, 1.0F, 1.0F, 1.0F};
                if (surface.route == SceneRoute::meshadditive) {
                    const auto* colour = parameter(submesh, "Color");
                    const auto value = vector4(colour);
                    if (!value) {
                        note("a MeshAdditive Color value is missing, repeated or not finite");
                    } else {
                        values.color = *value;
                        values.color_kind = colour->kind;
                    }
                }
                // An absent scroll rate does not scroll; the capture is at time 0.
                if (const auto rate = vector4(parameter(submesh, "UVScrollRate"))) values.uv_scroll_rate = *rate;
                surface.meshadditive = values;
            }
            assets::Submesh geometry_copy = submesh;
            geometry_copy.skin_bones.clear();
            if (surface.problem.empty()) {
                if (surface.billboard == 0) {
                    const Rigid chain = chain_transform(model, mesh.bone);
                    for (assets::Vertex& vertex : geometry_copy.vertices) {
                        vertex.position = transform_point(chain, vertex.position);
                        vertex.normal = rotate(chain, vertex.normal);
                        vertex.tangent = rotate(chain, vertex.tangent);
                        vertex.binormal = rotate(chain, vertex.binormal);
                        vertex.bone_indices = {};
                        vertex.bone_weights = {};
                    }
                } else {
                    const Rigid parent = chain_transform(model, model.bones[static_cast<std::size_t>(mesh.bone)].parent);
                    surface.bone_origin = {parent[3], parent[7], parent[11]};
                }
            }
            surface.model = single_surface(model, mesh, std::move(geometry_copy));
            result.push_back(std::move(surface));
        }
    }
    return result;
}

float surface_radius(const std::span<const SceneSurface> surfaces) noexcept {
    float radius = 0.0F;
    for (const SceneSurface& surface : surfaces) {
        if (!surface.problem.empty() || surface.billboard != 0) continue;
        for (const assets::Mesh& mesh : surface.model.meshes) {
            for (const assets::Submesh& submesh : mesh.submeshes) {
                for (const assets::Vertex& vertex : submesh.vertices) {
                    radius = std::max(radius, std::sqrt(dot(vertex.position, vertex.position)));
                }
            }
        }
    }
    return radius;
}

Billboard sun_billboard(const assets::Model& sky_model, const SceneSurface& surface, const assets::Vec3f& eye,
                        const assets::Vec3f& target, const assets::Vec3f& up, const assets::Vec3f& toward_light,
                        const float max_distance) {
    Billboard result;
    if (!surface.problem.empty()) {
        result.problem = surface.problem;
        return result;
    }
    if (surface.billboard != 7) {
        result.problem = "not a mode-7 billboard";
        return result;
    }
    SunRetailInput input;
    input.bones = sky_model.bones;
    input.mesh_bone = surface.mesh_bone;
    input.eye = eye;
    input.target = target;
    input.up = up;
    input.toward_light = toward_light;
    const SunRetailPlacement placement = sun_retail_placement(input);
    if (placement.status != SunRetailStatus::placed) {
        result.problem = "sun placement " + std::string(to_string(placement.status))
            + (placement.detail.empty() ? "" : ": " + placement.detail);
        return result;
    }
    if (!(max_distance > 0.0F) || !(placement.distance > 0.0)) {
        result.problem = "sun distance is not positive";
        return result;
    }
    const double pull = std::min(1.0, static_cast<double>(max_distance) / placement.distance);
    result.model = surface.model;
    const assets::Vec3f toward_eye{static_cast<float>(placement.axes[1][0]), static_cast<float>(placement.axes[1][1]),
                                   static_cast<float>(placement.axes[1][2])};
    for (assets::Mesh& mesh : result.model.meshes) {
        for (assets::Submesh& submesh : mesh.submeshes) {
            for (assets::Vertex& vertex : submesh.vertices) {
                const auto placed = sun_retail_vertex(placement, vertex.position);
                if (!placed) {
                    result.problem = "a sun vertex is not finite";
                    result.model = {};
                    return result;
                }
                vertex.position = {static_cast<float>(eye.x + ((*placed)[0] - eye.x) * pull),
                                   static_cast<float>(eye.y + ((*placed)[1] - eye.y) * pull),
                                   static_cast<float>(eye.z + ((*placed)[2] - eye.z) * pull)};
                vertex.normal = toward_eye;
            }
        }
    }
    return result;
}

std::optional<assets::Vec3f> sunlight_glow_offset(const assets::Vec3f& toward_light,
                                                  const assets::Vec3f& toward_eye, const float distance,
                                                  const float face_scale) noexcept {
    if (!finite(toward_light) || !finite(toward_eye) || !std::isfinite(distance) || distance < 0.0F
        || !std::isfinite(face_scale) || !(face_scale > 0.0F)) return std::nullopt;
    const auto light = unit(toward_light);
    const auto face = unit(toward_eye);
    if (!light || !face) return std::nullopt;
    const assets::Vec3f shift = scaled(minus(*light, scaled(*face, dot(*light, *face))), distance * face_scale);
    return finite(shift) ? std::optional<assets::Vec3f>(shift) : std::nullopt;
}

Billboard facing_billboard(const SceneSurface& surface, const Affine& object, const float scale,
                           const assets::Vec3f& eye, const assets::Vec3f& up, const float clearance) {
    Billboard result;
    if (!surface.problem.empty()) {
        result.problem = surface.problem;
        return result;
    }
    if (surface.billboard == 0) {
        result.problem = "not a billboard";
        return result;
    }
    const assets::Vec3f anchor = transform_point(object, surface.bone_origin);
    const assets::Vec3f offset = minus(eye, anchor);
    const float distance = std::sqrt(dot(offset, offset));
    const auto toward_eye = unit(offset);
    const auto right = toward_eye ? unit(cross(up, *toward_eye)) : std::nullopt;
    if (!toward_eye || !right || !std::isfinite(scale) || !(scale > 0.0F) || !std::isfinite(clearance)
        || !(distance > clearance)) {
        result.problem = "the billboard's facing frame is degenerate or the eye is inside its clearance";
        return result;
    }
    const float pull = (distance - clearance) / distance;
    const assets::Vec3f origin = plus(eye, scaled(offset, -pull));
    const float size = scale * pull;
    const assets::Vec3f third = cross(*toward_eye, *right);
    result.model = surface.model;
    for (assets::Mesh& mesh : result.model.meshes) {
        for (assets::Submesh& submesh : mesh.submeshes) {
            for (assets::Vertex& vertex : submesh.vertices) {
                const assets::Vec3f local = vertex.position;
                vertex.position = plus(origin, scaled(plus(plus(scaled(*right, local.x), scaled(*toward_eye, local.y)),
                                                           scaled(third, local.z)),
                                                      size));
                vertex.normal = *toward_eye;
            }
        }
    }
    return result;
}

FixedCamera environment_view_camera(FixedCamera camera) noexcept {
    camera.far_plane = std::max(camera.far_plane, environment_far_plane);
    return camera;
}

FixedCamera environment_laser_camera(FixedCamera drawn, const camera::Constants& tactical) noexcept {
    drawn.near_plane = tactical.near_clip;
    drawn.far_plane = tactical.far_clip;
    return drawn;
}

Affine camera_relative_sky_transform(const Affine& original, const assets::Vec3f& original_eye,
                                     const assets::Vec3f& current_eye) noexcept {
    Affine result = original;
    result[3] += current_eye.x - original_eye.x;
    result[7] += current_eye.y - original_eye.y;
    result[11] += current_eye.z - original_eye.z;
    return result;
}

DefaultCamera default_space_camera(const assets::Map& map, const TacticalDefaults& tactical, const std::uint32_t width,
                                   const std::uint32_t height) {
    DefaultCamera result;
    result.target_kind = "origin";
    if (!map.volumes.empty()) {
        const assets::SourceVolume& volume = map.volumes.front();
        result.centre_source = {(volume.minimum.x + volume.maximum.x) * 0.5F,
                                (volume.minimum.y + volume.maximum.y) * 0.5F, 0.0F};
        result.target_source = result.centre_source;
        result.target_kind = "volume_centre";
    }
    // The FoC skirmish map names team slots, not Player_<n>. Slot 1 is the
    // local player in the Coruscant reference capture.
    std::optional<std::uint32_t> best_spawn;
    std::optional<std::uint32_t> best_base;
    for (const assets::Placement& placement : map.placements) {
        if (placement.type_resolution != assets::TypeResolution::unique || !placement.position
            || !finite(*placement.position)) {
            continue;
        }
        const std::string& name = placement.type_candidates.front().logical_name;
        const bool spawn = ieq(name, "Team_01_Spawn_Point_Marker");
        const bool base = ieq(name, "Team_01_Base_Position_Marker");
        if (!spawn && !base) continue;
        const auto ordinal = placement.key.record_ordinal;
        if (spawn ? (best_spawn && ordinal >= *best_spawn)
                  : (best_spawn || (best_base && ordinal >= *best_base))) continue;
        if (spawn) best_spawn = ordinal;
        else best_base = ordinal;
        result.target_kind = "start_marker";
        result.target_record = placement.key.record_ordinal;
        result.target_type = name;
        result.target_source = *placement.position;
    }

    // Render basis: target (x, z, -y); the eye sits `distance` from it,
    // `pitch` above the horizon, on the side away from the view direction.
    constexpr double radians = std::numbers::pi / 180.0;
    const double yaw = tactical.yaw_degrees * radians;
    const double pitch = tactical.pitch_degrees * radians;
    const double horizontal = tactical.distance * std::cos(pitch);
    const std::array<float, 3> target{result.target_source.x, result.target_source.z, -result.target_source.y};
    result.camera.width = width;
    result.camera.height = height;
    // #515: the XML angle is FoC's (horizontal on 4:3); the renderer takes the vertical one. An
    // angle outside (0, 180) is kept as it is, for the camera checks downstream to refuse.
    const auto vertical = camera::vertical_fov_degrees(tactical.fov_degrees);
    result.camera.vertical_fov_degrees = vertical ? vertical.value() : tactical.fov_degrees;
    result.camera.near_plane = tactical.near_plane;
    result.camera.far_plane = environment_far_plane;
    result.camera.target = target;
    result.camera.eye = {static_cast<float>(target[0] + horizontal * std::sin(yaw)),
                         static_cast<float>(target[1] + tactical.distance * std::sin(pitch)),
                         static_cast<float>(target[2] + horizontal * std::cos(yaw))};
    result.camera.up = {0.0F, 1.0F, 0.0F};
    return result;
}

} // namespace eawr::presentation::space
