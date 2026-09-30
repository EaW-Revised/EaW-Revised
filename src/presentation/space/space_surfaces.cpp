#include "eawr/presentation/space/space.hpp"

#include "space_internal.hpp"

#include "eawr/presentation/terrain/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace eawr::presentation::space {
namespace {

[[nodiscard]] core::Diagnostic error(
    const std::string_view code, std::string message, const std::string& logical_path) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = logical_path.empty() ? std::nullopt : std::optional<std::string>(logical_path),
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("presentation.space"),
    };
}

[[nodiscard]] const Qualification* find_row(
    const std::span<const Qualification> rows, const std::string_view shader) {
    for (const Qualification& row : rows) {
        if (ieq(row.shader, shader)) return &row;
    }
    return nullptr;
}

[[nodiscard]] const MaterialRouteRow* find_route(
    const std::span<const MaterialRouteRow> routes, const std::string_view shader) {
    for (const MaterialRouteRow& route : routes) {
        if (ieq(route.shader, shader)) return &route;
    }
    return nullptr;
}

// The four authored MeshGloss fields besides BaseTexture, in the order of
// meshgloss_fields() after its BaseTexture row.
struct MeshGlossField final {
    std::string_view name;
    assets::ParameterKind kind;
};
constexpr std::array<MeshGlossField, 4> meshgloss_value_fields{{
    {"Emissive", assets::ParameterKind::vector4},
    {"Diffuse", assets::ParameterKind::vector4},
    {"Specular", assets::ParameterKind::vector4},
    {"Shininess", assets::ParameterKind::scalar},
}};

// The two authored MeshAdditive fields besides BaseTexture: Color (vector3 or
// vector4) and UVScrollRate (vector4 only).
constexpr std::array<std::string_view, 2> meshadditive_value_fields{"Color", "UVScrollRate"};

// One submesh's MeshGloss value fields: occurrences, well-typed and finite,
// value.
struct MeshGlossScan final {
    std::array<std::size_t, meshgloss_value_fields.size()> field_count{};
    std::array<bool, meshgloss_value_fields.size()> field_typed{true, true, true, true};
    std::array<bool, meshgloss_value_fields.size()> field_finite{true, true, true, true};
    MeshGlossMaterial values;
};

// One submesh's MeshAdditive fields, in meshadditive_fields() order after
// BaseTexture.
struct MeshAdditiveScan final {
    std::array<std::size_t, meshadditive_value_fields.size()> additive_count{};
    std::array<bool, meshadditive_value_fields.size()> additive_typed{true, true};
    std::array<bool, meshadditive_value_fields.size()> additive_finite{true, true};
    MeshAdditiveMaterial additive;
};

struct ParameterScan final {
    std::size_t base_textures = 0;
    bool base_texture_typed = true;
    bool unreviewed = false;
    MeshGlossScan gloss;
    MeshAdditiveScan additive;
};

[[nodiscard]] SurfacePlan surface_identity(const assets::Mesh& mesh, const std::size_t mesh_index,
                                           const std::size_t submesh_index) {
    const assets::Submesh& submesh = mesh.submeshes[submesh_index];
    SurfacePlan surface;
    surface.mesh_index = mesh_index;
    surface.submesh_index = submesh_index;
    surface.mesh_name = mesh.name;
    surface.original_shader = submesh.shader;
    surface.vertices = submesh.vertices.size();
    surface.triangles = submesh.indices.size() / 3;
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        surface.parameters.push_back(parameter.name + ":" + std::string(kind_name(parameter.kind)));
    }
    return surface;
}

[[nodiscard]] bool geometry_valid(const assets::Submesh& submesh) {
    bool geometry_ok = !submesh.vertices.empty() && !submesh.indices.empty()
        && submesh.indices.size() % 3 == 0;
    for (const std::uint16_t index : submesh.indices) {
        if (index >= submesh.vertices.size()) geometry_ok = false;
    }
    for (const assets::Vertex& vertex : submesh.vertices) {
        if (!finite(vertex.position) || !std::isfinite(vertex.texcoord[0].x)
            || !std::isfinite(vertex.texcoord[0].y)) {
            geometry_ok = false;
        }
    }
    return geometry_ok;
}

// The bone chain must be supported and compose to a finite proper rigid
// transform under which every baked vertex stays finite.
void check_hierarchy(SurfacePlan& surface, const assets::Model& model, const assets::Mesh& mesh,
                     const assets::Submesh& submesh, const bool meshgloss) {
    if (const std::string problem = hierarchy_problem(model, mesh.bone); !problem.empty()) {
        add_cause(surface, SurfaceStatus::hierarchy_unsupported);
        surface.detail = problem;
        return;
    }
    const Rigid transform = model_rigid(model, mesh.bone);
    if (!proper_rigid(transform)) {
        add_cause(surface, SurfaceStatus::hierarchy_unsupported);
        surface.detail = "the composed bone chain is not a finite proper rigid transform";
        return;
    }
    for (const assets::Vertex& vertex : submesh.vertices) {
        if (!finite(point(transform, vertex.position)) || !finite(direction(transform, vertex.normal))
            || !finite(direction(transform, vertex.tangent))
            || !finite(direction(transform, vertex.binormal))) {
            add_cause(surface, SurfaceStatus::geometry_invalid);
            break;
        }
        // MeshGloss lights per vertex from the normalised
        // normal; a zero normal has no defined direction.
        const assets::Vec3f normal = direction(transform, vertex.normal);
        if (meshgloss && !(dot3(normal, normal) > 1.0e-12F)) {
            add_cause(surface, SurfaceStatus::geometry_invalid);
            if (surface.detail.empty()) {
                surface.detail = "a MeshGloss vertex normal is zero, so its lighting direction is undefined";
            }
            break;
        }
    }
}

// Records one MeshAdditive value field; false when the parameter is not one.
[[nodiscard]] bool scan_meshadditive_field(MeshAdditiveScan& scan, const assets::MaterialParameter& parameter) {
    const auto field = std::find_if(meshadditive_value_fields.begin(), meshadditive_value_fields.end(),
        [&](const std::string_view name) { return ieq(name, parameter.name); });
    if (field == meshadditive_value_fields.end()) return false;
    const auto slot = static_cast<std::size_t>(field - meshadditive_value_fields.begin());
    ++scan.additive_count[slot];
    const auto* four = std::get_if<assets::Vec4f>(&parameter.value);
    const auto* three = std::get_if<assets::Vec3f>(&parameter.value);
    if (parameter.kind == assets::ParameterKind::vector4 && four != nullptr) {
        if (!finite4(*four)) scan.additive_finite[slot] = false;
        if (slot == 0) {
            scan.additive.color = *four;
            scan.additive.color_kind = assets::ParameterKind::vector4;
        } else {
            scan.additive.uv_scroll_rate = *four;
        }
    } else if (slot == 0 && parameter.kind == assets::ParameterKind::vector3 && three != nullptr) {
        // Color is declared float3, so a float3 chunk is accepted as authored.
        if (!finite(*three)) scan.additive_finite[slot] = false;
        scan.additive.color = {three->x, three->y, three->z, 0.0F};
        scan.additive.color_kind = assets::ParameterKind::vector3;
    } else {
        scan.additive_typed[slot] = false;
    }
    return true;
}

// Records one MeshGloss value field; false when the parameter is not one.
[[nodiscard]] bool scan_meshgloss_field(
    MeshGlossScan& scan, SurfacePlan& surface, const assets::MaterialParameter& parameter) {
    const auto field = std::find_if(meshgloss_value_fields.begin(), meshgloss_value_fields.end(),
        [&](const MeshGlossField& item) { return ieq(item.name, parameter.name); });
    if (field == meshgloss_value_fields.end()) return false;
    const auto slot = static_cast<std::size_t>(field - meshgloss_value_fields.begin());
    ++scan.field_count[slot];
    if (field->kind == assets::ParameterKind::vector4) {
        const auto* value = std::get_if<assets::Vec4f>(&parameter.value);
        if (parameter.kind != field->kind || value == nullptr) {
            scan.field_typed[slot] = false;
        } else {
            if (!finite4(*value)) scan.field_finite[slot] = false;
            (slot == 0 ? scan.values.emissive : slot == 1 ? scan.values.diffuse : scan.values.specular) = *value;
        }
    } else {
        const auto* value = std::get_if<float>(&parameter.value);
        if (parameter.kind != field->kind || value == nullptr) {
            scan.field_typed[slot] = false;
        } else {
            if (!std::isfinite(*value)) scan.field_finite[slot] = false;
            scan.values.shininess = *value;
        }
        // Reviewed: recorded, not consumed by the selected path.
        surface.unconsumed_parameters.push_back(parameter.name);
    }
    return true;
}

[[nodiscard]] ParameterScan scan_parameters(SurfacePlan& surface, const assets::Submesh& submesh,
                                            const Qualification* row, const bool meshgloss, const bool meshadditive) {
    ParameterScan scan;
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        if (meshadditive && scan_meshadditive_field(scan.additive, parameter)) continue;
        if (meshgloss && scan_meshgloss_field(scan.gloss, surface, parameter)) continue;
        if (ieq(parameter.name, "BaseTexture")) {
            ++scan.base_textures;
            const auto* name = std::get_if<std::string>(&parameter.value);
            if (parameter.kind != assets::ParameterKind::texture || name == nullptr
                || !declared(std::optional<std::string>(*name))) {
                scan.base_texture_typed = false;
            } else if (surface.base_texture.empty()) {
                surface.base_texture = *name;
            }
            continue;
        }
        surface.unconsumed_parameters.push_back(parameter.name);
        if (parameter.kind == assets::ParameterKind::texture) {
            add_cause(surface, SurfaceStatus::multitexture_required);
            continue;
        }
        const bool reviewed = row != nullptr
            && std::any_of(row->reviewed_unconsumed_parameters.begin(),
                row->reviewed_unconsumed_parameters.end(),
                [&](const std::string& name) { return ieq(name, parameter.name); });
        if (!reviewed) scan.unreviewed = true;
    }
    return scan;
}

void note_meshgloss_field(SurfacePlan& surface, bool& complete, const SurfaceStatus cause,
                          const MeshGlossField& field, const std::string& why) {
    complete = false;
    add_cause(surface, cause);
    if (surface.detail.empty()) surface.detail = "MeshGloss field " + std::string(field.name) + " " + why;
}

void settle_meshgloss(SurfacePlan& surface, const MeshGlossScan& scan) {
    surface.field_dispositions.assign(meshgloss_fields().begin(), meshgloss_fields().end());
    bool complete = true;
    for (std::size_t slot = 0; slot < meshgloss_value_fields.size(); ++slot) {
        const MeshGlossField& field = meshgloss_value_fields[slot];
        const std::string kind(kind_name(field.kind));
        if (scan.field_count[slot] == 0) {
            note_meshgloss_field(surface, complete, SurfaceStatus::material_parameter_missing, field,
                                 "is not authored; no default is substituted");
        } else if (scan.field_count[slot] > 1) {
            note_meshgloss_field(surface, complete, SurfaceStatus::material_parameter_invalid, field,
                                 "is authored more than once");
        } else if (!scan.field_typed[slot]) {
            note_meshgloss_field(surface, complete, SurfaceStatus::material_parameter_invalid, field,
                                 "is not a " + kind);
        } else if (!scan.field_finite[slot]) {
            note_meshgloss_field(surface, complete, SurfaceStatus::material_value_nonfinite, field,
                                 "has a non-finite component");
        }
    }
    if (complete) surface.meshgloss = scan.values;
}

void note_meshadditive_field(SurfacePlan& surface, bool& complete, const SurfaceStatus cause,
                             const std::string& name, const std::string& why) {
    complete = false;
    add_cause(surface, cause);
    if (surface.detail.empty()) surface.detail = "MeshAdditive field " + name + " " + why;
}

void settle_meshadditive(SurfacePlan& surface, const MeshAdditiveScan& scan) {
    surface.field_dispositions.assign(meshadditive_fields().begin(), meshadditive_fields().end());
    bool complete = true;
    for (std::size_t slot = 0; slot < meshadditive_value_fields.size(); ++slot) {
        const std::string name(meshadditive_value_fields[slot]);
        if (scan.additive_count[slot] == 0) {
            note_meshadditive_field(surface, complete, SurfaceStatus::material_parameter_missing, name,
                                    "is not authored; the effect default is not substituted");
        } else if (scan.additive_count[slot] > 1) {
            note_meshadditive_field(surface, complete, SurfaceStatus::material_parameter_invalid, name,
                                    "is authored more than once");
        } else if (!scan.additive_typed[slot]) {
            note_meshadditive_field(surface, complete, SurfaceStatus::material_parameter_invalid, name,
                                    slot == 0 ? "is not a vector3 or vector4" : "is not a vector4");
        } else if (!scan.additive_finite[slot]) {
            note_meshadditive_field(surface, complete, SurfaceStatus::material_value_nonfinite, name,
                                    "has a non-finite component");
        }
    }
    if (complete) surface.meshadditive = scan.additive;
}

[[nodiscard]] SurfacePlan plan_surface(
    const assets::Model& model, const std::size_t mesh_index, const std::size_t submesh_index,
    const std::span<const Qualification> rows, const std::span<const MaterialRouteRow> routes) {
    const assets::Mesh& mesh = model.meshes[mesh_index];
    const assets::Submesh& submesh = mesh.submeshes[submesh_index];
    SurfacePlan surface = surface_identity(mesh, mesh_index, submesh_index);
    if (!mesh.visible) {
        add_cause(surface, SurfaceStatus::mesh_invisible);
        surface.detail = "mesh is hidden in the model; it is skipped and never counted as drawable";
        settle(surface);
        return surface;
    }

    if (!geometry_valid(submesh)) add_cause(surface, SurfaceStatus::geometry_invalid);
    const Qualification* row = find_row(rows, submesh.shader);
    const MaterialRouteRow* route = row == nullptr ? find_route(routes, submesh.shader) : nullptr;
    if (row != nullptr) surface.material = SkyMaterial::opaque_diffuse;
    else if (route != nullptr) surface.material = route->material;
    const bool meshgloss = route != nullptr && route->material == SkyMaterial::meshgloss;
    const bool meshadditive = route != nullptr && route->material == SkyMaterial::meshadditive;
    if (!submesh.skin_bones.empty()) add_cause(surface, SurfaceStatus::skinning_unsupported);
    check_hierarchy(surface, model, mesh, submesh, meshgloss);
    if (const std::string hidden = hidden_bone(model, mesh.bone); !hidden.empty()) {
        add_cause(surface, SurfaceStatus::bone_visibility_unsupported);
        if (surface.detail.empty()) surface.detail = hidden;
    }

    const ParameterScan scan = scan_parameters(surface, submesh, row, meshgloss, meshadditive);
    if (scan.base_textures == 0) add_cause(surface, SurfaceStatus::base_texture_missing);
    else if (scan.base_textures > 1) add_cause(surface, SurfaceStatus::base_texture_duplicate);
    else if (!scan.base_texture_typed) add_cause(surface, SurfaceStatus::base_texture_wrong_type);
    if (meshgloss) settle_meshgloss(surface, scan.gloss);
    if (meshadditive) settle_meshadditive(surface, scan.additive);
    if (scan.unreviewed) add_cause(surface, SurfaceStatus::unconsumed_parameter);
    if (row == nullptr && route == nullptr) add_cause(surface, SurfaceStatus::shader_not_qualified);
    settle(surface);
    return surface;
}

} // namespace

std::vector<SurfacePlan> plan_surfaces(
    const assets::Model& model, const std::span<const Qualification> rows) {
    return plan_surfaces(model, rows, {});
}

std::vector<SurfacePlan> plan_surfaces(const assets::Model& model, const std::span<const Qualification> rows,
                                       const std::span<const MaterialRouteRow> routes) {
    std::vector<SurfacePlan> result;
    for (std::size_t mesh_index = 0; mesh_index < model.meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model.meshes[mesh_index];
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            result.push_back(plan_surface(model, mesh_index, submesh_index, rows, routes));
        }
    }
    return result;
}

core::Result<assets::Texture> normalize_texture(const assets::Texture& texture) {
    const std::string& path = texture.source.logical_path;
    const auto failure = [&](std::string message) {
        return core::Result<assets::Texture>::failure(
            error(diagnostic_codes::texture_unsupported, std::move(message), path));
    };
    if (texture.width == 0 || texture.height == 0 || texture.mips.empty()) {
        return failure("texture has no pixels");
    }
    if (texture.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
        || texture.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        return failure("texture dimensions exceed the renderer's signed 32-bit limit");
    }
    if (texture.source_origin != assets::ImageOrigin::top_left
        && texture.source_origin != assets::ImageOrigin::bottom_left) {
        return failure("texture has an invalid source origin");
    }
    std::size_t source_bytes = 0;
    std::size_t block_bytes = 0;
    switch (texture.format) {
    case assets::PixelFormat::rgba8:
    case assets::PixelFormat::bgra8: source_bytes = 4; break;
    case assets::PixelFormat::bgr8: source_bytes = 3; break;
    case assets::PixelFormat::l8: source_bytes = 1; break;
    case assets::PixelFormat::a8:
        return failure("an alpha-only texture carries no diffuse colour for the opaque diffuse contract");
    case assets::PixelFormat::bc1:
    case assets::PixelFormat::bc4: block_bytes = 8; break;
    case assets::PixelFormat::bc2:
    case assets::PixelFormat::bc3:
    case assets::PixelFormat::bc5:
    case assets::PixelFormat::bc7: block_bytes = 16; break;
    default: return failure("texture has an unsupported pixel format");
    }
    if (block_bytes != 0 && texture.source_origin != assets::ImageOrigin::top_left) {
        return failure("a bottom-left block-compressed texture is not reoriented by this slice");
    }

    // Check the entire chain before copying BC bytes or allocating RGBA output.
    // Uncompressed rows may be padded; BC rows and payloads must be tight because
    // the renderer concatenates compressed mip bytes without using row_pitch.
    std::uint32_t expected_width = texture.width;
    std::uint32_t expected_height = texture.height;
    for (std::size_t level = 0; level < texture.mips.size(); ++level) {
        const assets::MipLevel& mip = texture.mips[level];
        if (mip.width > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())
            || mip.height > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
            return failure("mip dimensions exceed the renderer's signed 32-bit limit");
        }
        if (level != 0 && texture.mips[level - 1].width == 1
            && texture.mips[level - 1].height == 1) {
            return failure("mip chain continues beyond 1x1");
        }
        if (mip.width != expected_width || mip.height != expected_height) {
            return failure("mip level dimensions disagree with the texture or mip chain");
        }
        const std::size_t columns = block_bytes == 0 ? mip.width : 1U + (mip.width - 1U) / 4U;
        const std::size_t rows = block_bytes == 0 ? mip.height : 1U + (mip.height - 1U) / 4U;
        const std::size_t unit_bytes = block_bytes == 0 ? source_bytes : block_bytes;
        if (columns > std::numeric_limits<std::size_t>::max() / unit_bytes) {
            return failure("mip row size overflows");
        }
        const std::size_t row_bytes = columns * unit_bytes;
        if (row_bytes > std::numeric_limits<std::uint32_t>::max()
            || (block_bytes != 0 ? mip.row_pitch != row_bytes : mip.row_pitch < row_bytes)) {
            return failure("mip row pitch disagrees with its dimensions");
        }
        const std::size_t pitch = mip.row_pitch;
        if (block_bytes != 0) {
            if (rows > std::numeric_limits<std::size_t>::max() / pitch) {
                return failure("mip byte length overflows");
            }
            if (mip.bytes.size() != rows * pitch) {
                return failure("mip level bytes disagree with its dimensions");
            }
        } else {
            if (rows - 1U > (std::numeric_limits<std::size_t>::max() - row_bytes) / pitch) {
                return failure("mip byte length overflows");
            }
            if (mip.bytes.size() < (rows - 1U) * pitch + row_bytes) {
                return failure("mip level bytes disagree with its dimensions");
            }
            const std::size_t width = mip.width;
            const std::size_t height = mip.height;
            if (width > std::numeric_limits<std::uint32_t>::max() / 4U
                || height > std::vector<std::byte>().max_size() / (width * 4U)) {
                return failure("normalized mip size overflows");
            }
        }
        expected_width = std::max(1U, expected_width / 2U);
        expected_height = std::max(1U, expected_height / 2U);
    }
    if (block_bytes != 0) return core::Result<assets::Texture>::success(texture);

    assets::Texture result;
    result.source = texture.source;
    result.width = texture.width;
    result.height = texture.height;
    result.format = assets::PixelFormat::rgba8;
    result.source_origin = assets::ImageOrigin::top_left;
    result.has_alpha = texture.has_alpha;
    result.notices = texture.notices;
    const bool flip = texture.source_origin == assets::ImageOrigin::bottom_left;
    for (const assets::MipLevel& mip : texture.mips) {
        const std::size_t width = mip.width;
        const std::size_t height = mip.height;
        assets::MipLevel output;
        output.width = mip.width;
        output.height = mip.height;
        output.row_pitch = mip.width * 4U;
        output.bytes.resize(width * height * 4U);
        for (std::size_t row = 0; row < height; ++row) {
            const std::size_t source_row = flip ? height - 1 - row : row;
            const std::byte* in = mip.bytes.data() + source_row * mip.row_pitch;
            std::byte* out = output.bytes.data() + row * width * 4U;
            for (std::size_t column = 0; column < width; ++column) {
                const std::byte* pixel = in + column * source_bytes;
                std::byte* target = out + column * 4U;
                switch (texture.format) {
                case assets::PixelFormat::rgba8:
                    target[0] = pixel[0]; target[1] = pixel[1]; target[2] = pixel[2]; target[3] = pixel[3];
                    break;
                case assets::PixelFormat::bgra8:
                    target[0] = pixel[2]; target[1] = pixel[1]; target[2] = pixel[0]; target[3] = pixel[3];
                    break;
                case assets::PixelFormat::bgr8:
                    target[0] = pixel[2]; target[1] = pixel[1]; target[2] = pixel[0]; target[3] = std::byte{0xff};
                    break;
                default:
                    target[0] = pixel[0]; target[1] = pixel[0]; target[2] = pixel[0]; target[3] = std::byte{0xff};
                    break;
                }
            }
        }
        result.mips.push_back(std::move(output));
    }
    return core::Result<assets::Texture>::success(std::move(result));
}

} // namespace eawr::presentation::space
