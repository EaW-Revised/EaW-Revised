#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/model_upload.hpp"
#include "upload_identity.hpp"

#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

// The loader's captured content owner must outlive every recipe. A generated or
// transformed model keeps its snapshot unless the loader reproduces it exactly.
using ModelLoader = std::function<std::optional<assets::Model>(const assets::Source&)>;

// Digest of every model field admission or the mesh upload can read (bones,
// all meshes including hidden ones that a surface may force visible, all geometry).
[[nodiscard]] inline UploadIdentity model_identity(const assets::Model& model) {
    UploadIdentityWriter out;
    out.size(model.bones.size());
    for (const assets::Bone& bone : model.bones) {
        out.text(bone.name);
        out.scalar(bone.parent);
        out.scalar(bone.visible);
        out.scalar(bone.billboard);
        for (const float value : bone.relative_transform) out.scalar(value);
    }
    out.size(model.meshes.size());
    for (const assets::Mesh& mesh : model.meshes) {
        out.scalar(mesh.visible);
        out.scalar(mesh.bone);
        out.vec(mesh.bounds_min);
        out.vec(mesh.bounds_max);
        out.size(mesh.submeshes.size());
        for (const assets::Submesh& submesh : mesh.submeshes) {
            out.size(submesh.skin_bones.size());
            for (const std::uint32_t bone : submesh.skin_bones) out.scalar(bone);
            out.size(submesh.vertices.size());
            for (const assets::Vertex& vertex : submesh.vertices) {
                out.vec(vertex.position);
                out.vec(vertex.normal);
                out.vec(vertex.tangent);
                out.vec(vertex.binormal);
                for (const assets::Vec2f& uv : vertex.texcoord) out.vec(uv);
                out.vec(vertex.color);
                for (const std::uint32_t index : vertex.bone_indices) out.scalar(index);
                for (const float weight : vertex.bone_weights) out.scalar(weight);
            }
            out.size(submesh.indices.size());
            for (const std::uint16_t index : submesh.indices) out.scalar(index);
        }
    }
    return out.digest();
}

// Cumulative per-renderer counts (never decremented when a recipe is destroyed):
// recipes ever released, and recipes that could never be released (derived or
// generated model, no loader, no source path) and so keep a full snapshot.
struct ModelRecipeStats final {
    std::size_t ever_released{};
    std::size_t ever_fallback{};
};

// Mirrors TextureRecipe: after successful initial admission the geometry of an
// authoritative loader/cache model is released (digest taken from the uploaded
// model, no second decode); later admissions re-read the source and fail rather
// than upload bytes whose digest differs. Derived models never release.
class ModelRecipe final {
public:
    ModelRecipe() = default;
    // `authoritative`: the upload is exactly what `loader` returns for its source
    // (the ModelUpload overload). Derived/transformed models pass false and keep their snapshot.
    ModelRecipe(ModelUpload upload, ModelLoader loader, const bool authoritative)
        : retained_(std::move(upload.model)), surface_(upload.surface),
          loader_(authoritative ? std::move(loader) : ModelLoader{}) {}

    [[nodiscard]] const std::optional<ModelSurface>& surface() const { return surface_; }
    [[nodiscard]] const assets::Model* retained() const { return retained_.get(); }
    [[nodiscard]] const std::shared_ptr<const assets::Model>& snapshot() const { return retained_; }
    [[nodiscard]] bool released() const { return !retained_; }

    // Called after successful GPU admission. No second decode: the digest comes
    // from the model that was uploaded and is checked only when a reload happens.
    // Returns true when geometry was released.
    bool uploaded(ModelRecipeStats& stats) const {
        if (verified_ || !retained_) return false;
        verified_ = true;
        if (!loader_ || retained_->source.logical_path.empty()) { loader_ = {}; ++stats.ever_fallback; return false; }
        source_ = retained_->source;
        identity_ = model_identity(*retained_);
        retained_.reset();
        ++stats.ever_released;
        return true;
    }

    // The whole model (siblings share the caller's batch-local `shared`) or
    // empty when the exact identity can no longer be reproduced (counted by the caller).
    using Batch = std::map<UploadIdentity, std::shared_ptr<const assets::Model>>;
    [[nodiscard]] std::optional<ModelUpload> read(Batch& batch) const {
        if (retained_) return ModelUpload{retained_, surface_};
        if (!loader_) return std::nullopt;
        auto& shared = batch[identity_];
        if (!shared) {
            auto decoded = loader_(source_);
            if (!decoded || model_identity(*decoded) != identity_) { batch.erase(identity_); return std::nullopt; }
            shared = std::make_shared<const assets::Model>(std::move(*decoded));
        }
        ModelUpload upload{shared, surface_};
        if (!upload.valid()) return std::nullopt;
        return upload;
    }

    [[nodiscard]] const assets::Source& source() const { return source_; }
    [[nodiscard]] const UploadIdentity& identity() const { return identity_; }

private:
    // Release state is mutated through the shared const recipe after admission.
    mutable std::shared_ptr<const assets::Model> retained_;
    std::optional<ModelSurface> surface_;
    mutable ModelLoader loader_;
    mutable assets::Source source_;
    mutable UploadIdentity identity_{};
    mutable bool verified_{};
};

} // namespace eawr::presentation::godot_backend::detail
