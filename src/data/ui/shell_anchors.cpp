#include "eawr/data/ui/shell_anchors.hpp"

#include "ui_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

namespace eawr::data::ui {
namespace {

core::Diagnostic report(const assets::Source& source, const std::string_view code, std::string message) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::warning,
        .message = std::move(message),
        .logical_path = source.logical_path,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = source.source_id.empty() ? std::nullopt : std::optional<std::string>(source.source_id),
    };
}

ShellTransform identity() {
    return {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
}
ShellTransform multiply(const ShellTransform& a, const ShellTransform& b) {
    ShellTransform out{};
    for (std::size_t c = 0; c < 4; ++c)
        for (std::size_t r = 0; r < 4; ++r)
            for (std::size_t k = 0; k < 4; ++k) out[c*4+r] += a[k*4+r]*b[c*4+k];
    return out;
}
assets::Vec3f transform(const ShellTransform& m, const assets::Vec3f p) {
    return {m[0]*p.x+m[4]*p.y+m[8]*p.z+m[12],
            m[1]*p.x+m[5]*p.y+m[9]*p.z+m[13],
            m[2]*p.x+m[6]*p.y+m[10]*p.z+m[14]};
}

std::string texture_parameter(const assets::Submesh& submesh) {
    for (const assets::MaterialParameter& parameter : submesh.parameters) {
        if (parameter.kind != assets::ParameterKind::texture || !detail::iequals(parameter.name, "BaseTexture")) continue;
        if (const auto* name = std::get_if<std::string>(&parameter.value)) return *name;
    }
    return {};
}

} // namespace

const ShellAnchor* ShellAnchors::find(const std::string_view name) const noexcept {
    const auto match = std::find_if(anchors_.begin(), anchors_.end(),
        [&](const ShellAnchor& anchor) { return detail::iequals(anchor.name, name); });
    return match == anchors_.end() ? nullptr : &*match;
}

std::vector<const ShellAnchor*> ShellAnchors::for_variant(const std::uint32_t variant) const {
    std::vector<const ShellAnchor*> result;
    for (const ShellAnchor& anchor : anchors_) {
        if (anchor.alt.shown_for(variant)) result.push_back(&anchor);
    }
    return result;
}

ShellAnchorLoad shell_anchors(const assets::Model& model) {
    std::vector<ShellTransform> transforms;
    transforms.reserve(model.bones.size());
    for (std::size_t i = 0; i < model.bones.size(); ++i) {
        auto combined = identity();
        // Loaded models have validated parent indices and no cycles.
        for (std::int32_t index = static_cast<std::int32_t>(i); index >= 0;) {
            const auto& bone = model.bones[static_cast<std::size_t>(index)];
            auto local = identity();
            for (std::size_t r = 0; r < 3; ++r)
                for (std::size_t c = 0; c < 4; ++c) local[c*4+r] = bone.relative_transform[r*4+c];
            combined = multiply(local, combined);
            index = bone.parent;
        }
        transforms.push_back(combined);
    }
    return shell_anchors(model, transforms);
}

ShellAnchorLoad shell_anchors(const assets::Model& model, const std::span<const ShellTransform> model_transforms) {
    ShellAnchorLoad result;
    result.shell.set_model_path(model.source.logical_path);
    for (const assets::Mesh& mesh : model.meshes) {
        ShellTransform matrix = identity();
        std::string bone_name;
        std::optional<assets::Vec2f> origin;
        if (mesh.bone < 0 || static_cast<std::size_t>(mesh.bone) >= model_transforms.size()) {
            result.diagnostics.push_back(report(model.source, diagnostic_codes::shell_mesh_unbound,
                "shell mesh " + mesh.name + " has no bone; its vertices are used as placed"));
        } else {
            matrix = model_transforms[static_cast<std::size_t>(mesh.bone)];
            bone_name = model.bones[static_cast<std::size_t>(mesh.bone)].name;
            origin = assets::Vec2f{matrix[12], matrix[13]};
        }
        std::vector<ShellTriangle> triangles;
        float x_min = std::numeric_limits<float>::max();
        float y_min = x_min;
        float z_min = x_min;
        float x_max = std::numeric_limits<float>::lowest();
        float y_max = x_max;
        float z_max = x_max;
        std::size_t vertices = 0;
        for (const assets::Submesh& submesh : mesh.submeshes) {
            for (std::size_t i = 0; i + 2 < submesh.indices.size(); i += 3) {
                ShellTriangle triangle;
                triangle.base_texture = texture_parameter(submesh);
                for (std::size_t j = 0; j < 3; ++j) {
                    const auto& vertex = submesh.vertices[submesh.indices[i+j]];
                    triangle.vertices[j] = {transform(matrix, vertex.position), vertex.texcoord[0]};
                }
                triangles.push_back(std::move(triangle));
            }
            for (const assets::Vertex& vertex : submesh.vertices) {
                const auto position = transform(matrix, vertex.position);
                x_min = std::min(x_min, position.x);
                y_min = std::min(y_min, position.y);
                z_min = std::min(z_min, position.z);
                x_max = std::max(x_max, position.x);
                y_max = std::max(y_max, position.y);
                z_max = std::max(z_max, position.z);
                ++vertices;
            }
        }
        if (vertices == 0) {
            result.diagnostics.push_back(report(model.source, diagnostic_codes::shell_mesh_empty,
                "shell mesh \"" + mesh.name + "\" has no vertices and no anchor"));
            continue;
        }
        if (result.shell.find(mesh.name) != nullptr) {
            result.diagnostics.push_back(report(model.source, diagnostic_codes::shell_mesh_duplicate,
                "shell mesh \"" + mesh.name + "\" appears again; lookups return the first"));
        }

        ShellAnchor anchor;
        anchor.name = mesh.name;
        anchor.alt = split_alt(mesh.name);
        anchor.rect = ReferenceRect{x_min, y_min, x_max - x_min, y_max - y_min};
        anchor.z_min = z_min;
        anchor.z_max = z_max;
        anchor.visible = mesh.visible;
        anchor.bone = std::move(bone_name);
        anchor.origin = origin;
        anchor.triangles = std::move(triangles);
        if (!mesh.submeshes.empty()) {
            anchor.shader = mesh.submeshes.front().shader;
            anchor.base_texture = texture_parameter(mesh.submeshes.front());
        }
        result.shell.add(std::move(anchor));
    }
    return result;
}

core::Result<ShellAnchorLoad> load_shell_anchors(const vfs::Vfs& filesystem, const std::string_view model_name) {
    auto model = assets::load_model(filesystem, "data/art/models/" + std::string(model_name));
    if (!model) return core::Result<ShellAnchorLoad>::failure(model.error());
    return core::Result<ShellAnchorLoad>::success(shell_anchors(model.value()));
}

std::vector<BoundAnchor> bind_components(const ShellAnchors& shell, const CommandBarCatalog& catalog) {
    std::vector<BoundAnchor> result;
    result.reserve(shell.anchors().size());
    for (const ShellAnchor& anchor : shell.anchors()) result.push_back({&anchor, catalog.find(anchor.name)});
    return result;
}

} // namespace eawr::data::ui
