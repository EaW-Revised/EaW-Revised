#pragma once

#include "eawr/assets/assets.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace eawr::presentation {

// A surface keeps the complete immutable model alive, but draws only this
// mesh/submesh. Visibility overrides cover phase-gated and shield geometry.
struct ModelSurface final {
    std::size_t mesh{};
    std::size_t submesh{};
    bool force_visible{};
};

// A short-lived borrowed view; its ModelUpload owner must outlive it.
struct ModelView final {
    struct Mesh final {
        bool visible{};
        std::int32_t bone{};
        std::span<const assets::Submesh> submeshes;
    };

    const assets::Model& original;
    std::span<const assets::Bone> bones;
    std::vector<Mesh> meshes;

    ModelView(const assets::Model& model, const std::optional<ModelSurface> surface = {})
        : original(model), bones(model.bones) {
        if (surface) {
            const auto& mesh = model.meshes.at(surface->mesh);
            meshes.push_back({mesh.visible || surface->force_visible, mesh.bone,
                std::span<const assets::Submesh>(mesh.submeshes).subspan(surface->submesh, 1)});
        } else {
            meshes.reserve(model.meshes.size());
            for (const auto& mesh : model.meshes)
                meshes.push_back({mesh.visible, mesh.bone, mesh.submeshes});
        }
    }
};

struct ModelUpload final {
    std::shared_ptr<const assets::Model> model;
    std::optional<ModelSurface> surface;

    [[nodiscard]] bool valid() const noexcept {
        return model && (!surface || (surface->mesh < model->meshes.size()
            && surface->submesh < model->meshes[surface->mesh].submeshes.size()));
    }
    [[nodiscard]] ModelView view() const { return ModelView(*model, surface); }
};

} // namespace eawr::presentation
