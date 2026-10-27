#pragma once

#include "eawr/assets/assets.hpp"
#include <ostream>
#include <set>

namespace eawr::assets {

// Vector backing storage only: excludes strings, objects, allocator overhead
// and GPU allocations. Collision is a subset of vertex + index, not additive.
struct PayloadCapacity final {
    std::uint64_t vertex_bytes{}, index_bytes{}, material_bytes{}, bone_bytes{};
    std::uint64_t collision_bytes{}, structure_bytes{}, texture_payload_bytes{};
    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return vertex_bytes + index_bytes + material_bytes + bone_bytes
            + structure_bytes + texture_payload_bytes;
    }
    PayloadCapacity& operator+=(const PayloadCapacity& other) noexcept {
        vertex_bytes += other.vertex_bytes;
        index_bytes += other.index_bytes;
        material_bytes += other.material_bytes;
        bone_bytes += other.bone_bytes;
        collision_bytes += other.collision_bytes;
        structure_bytes += other.structure_bytes;
        texture_payload_bytes += other.texture_payload_bytes;
        return *this;
    }
    friend bool operator==(const PayloadCapacity&, const PayloadCapacity&) = default;
};

template<class T>
[[nodiscard]] inline std::uint64_t vector_capacity_bytes(const std::vector<T>& values) noexcept {
    return static_cast<std::uint64_t>(values.capacity()) * sizeof(T);
}

[[nodiscard]] inline PayloadCapacity payload_capacity(const Model& model) noexcept {
    PayloadCapacity result;
    result.bone_bytes = vector_capacity_bytes(model.bones);
    result.structure_bytes = vector_capacity_bytes(model.meshes) + vector_capacity_bytes(model.lights)
        + vector_capacity_bytes(model.proxies) + vector_capacity_bytes(model.dazzles)
        + vector_capacity_bytes(model.notices);
    for (const auto& mesh : model.meshes) {
        result.structure_bytes += vector_capacity_bytes(mesh.submeshes);
        for (const auto& surface : mesh.submeshes) {
            const auto vertices = vector_capacity_bytes(surface.vertices);
            const auto indices = vector_capacity_bytes(surface.indices);
            result.vertex_bytes += vertices;
            result.index_bytes += indices;
            result.material_bytes += vector_capacity_bytes(surface.parameters);
            result.bone_bytes += vector_capacity_bytes(surface.skin_bones);
            if (mesh.collidable) result.collision_bytes += vertices + indices;
        }
    }
    return result;
}

[[nodiscard]] inline PayloadCapacity payload_capacity(const Texture& texture) noexcept {
    PayloadCapacity result;
    result.structure_bytes = vector_capacity_bytes(texture.mips) + vector_capacity_bytes(texture.notices);
    for (const auto& mip : texture.mips) result.texture_payload_bytes += vector_capacity_bytes(mip.bytes);
    return result;
}

inline void write_payload_capacity(std::ostream& output, const PayloadCapacity& value) {
    output << "{\"vertex_bytes\":" << value.vertex_bytes << ",\"index_bytes\":" << value.index_bytes
        << ",\"material_bytes\":" << value.material_bytes << ",\"bone_bytes\":" << value.bone_bytes
        << ",\"collision_subset_bytes\":" << value.collision_bytes
        << ",\"structure_bytes\":" << value.structure_bytes
        << ",\"texture_payload_bytes\":" << value.texture_payload_bytes
        << ",\"total_bytes\":" << value.total_bytes() << '}';
}

// Explicit diagnostic probes may allocate and walk metadata; never call this
// from upload, submit or a per-frame loop. Pointer identity matches ownership.
struct RetainedPayload final {
    std::uint64_t recipes{}, models{}, textures{};
    PayloadCapacity model_capacity, texture_capacity;
    std::uint64_t proxy_payload_bytes{};
};
class PayloadProbe final {
public:
    void model(const Model* value) {
        if (value && models_.insert(value).second) {
            ++result.models;
            result.model_capacity += payload_capacity(*value);
        }
    }
    void texture(const Texture* value) {
        if (value && textures_.insert(value).second) {
            ++result.textures;
            result.texture_capacity += payload_capacity(*value);
        }
    }
    RetainedPayload result;
private:
    std::set<const Model*> models_;
    std::set<const Texture*> textures_;
};

inline void write_retained_payload(std::ostream& output, const RetainedPayload& value) {
    output << "{\"recipes\":" << value.recipes << ",\"models\":" << value.models
        << ",\"textures\":" << value.textures << ",\"models_overlap_vfs\":true,\"model_capacity\":";
    write_payload_capacity(output, value.model_capacity);
    output << ",\"texture_capacity\":";
    write_payload_capacity(output, value.texture_capacity);
    output << ",\"proxy_payload_bytes\":" << value.proxy_payload_bytes << '}';
}
} // namespace eawr::assets
