#pragma once

#include "eawr/assets/assets.hpp"

#include <cstdint>

namespace eawr::assets {

// A metadata-only copy of a Model (bones, mesh names/bones, proxies; no submeshes) carries this notice.
// It must never reach a renderer upload: it still names the source path, so a reload would find no geometry.
inline constexpr std::uint32_t geometry_stripped_chunk = 0x45544D47U; // 'GMTE'

[[nodiscard]] inline bool geometry_stripped(const Model& model) noexcept {
    for (const Notice& notice : model.notices) {
        if (notice.chunk_type == geometry_stripped_chunk) return true;
    }
    return false;
}

// What the emitter planners read of a Model: source, bones, mesh names/bones and proxies. Submeshes
// (vertices, indices, materials), lights and dazzles are not copied; no heavy buffer is copied first.
[[nodiscard]] inline Model geometry_stripped_copy(const Model& full) {
    Model stripped;
    stripped.source = full.source;
    stripped.bones = full.bones;
    stripped.proxies = full.proxies;
    stripped.meshes.reserve(full.meshes.size());
    for (const Mesh& mesh : full.meshes) {
        Mesh& kept = stripped.meshes.emplace_back();
        kept.name = mesh.name;
        kept.bounds_min = mesh.bounds_min;
        kept.bounds_max = mesh.bounds_max;
        kept.visible = mesh.visible;
        kept.collidable = mesh.collidable;
        kept.bone = mesh.bone;
    }
    stripped.notices.push_back({geometry_stripped_chunk, 0, 0, "emitter metadata: geometry stripped"});
    return stripped;
}

} // namespace eawr::assets
