#pragma once

#include "eawr/presentation/particles/render.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace eawr::presentation::godot_backend::detail {

// The uncompressed RenderingServer particle surface: float3 positions, RGBA8 colour
// followed by float2 UV. Offsets/strides come from the pinned engine's format queries.
inline void pack_particle_vertex(const particles::ParticleVertex& vertex,
    std::uint8_t* position, std::uint8_t* colour, std::uint8_t* uv) {
    const std::array<float, 3> converted{vertex.position.x, vertex.position.z, -vertex.position.y};
    std::memcpy(position, converted.data(), sizeof(converted));
    const std::array<float, 4> rgba{vertex.color.x, vertex.color.y, vertex.color.z, vertex.color.w};
    for (std::size_t channel = 0; channel < rgba.size(); ++channel) {
        // Match the public array upload's double-precision scaling and truncation.
        colour[channel] = static_cast<std::uint8_t>(std::clamp(static_cast<double>(rgba[channel]) * 255.0, 0.0, 255.0));
    }
    const std::array<float, 2> coordinates{vertex.u, vertex.v};
    std::memcpy(uv, coordinates.data(), sizeof(coordinates));
}

struct ParticleSurfaceShape final {
    std::size_t vertices{};
    std::size_t indices{};

    [[nodiscard]] bool matches(const particles::VertexStream& stream) const noexcept {
        return vertices != 0 && vertices == stream.vertices.size() && indices == stream.indices.size();
    }
    void assign(const particles::VertexStream& stream) noexcept {
        vertices = stream.vertices.size();
        indices = stream.indices.size();
    }
};

} // namespace eawr::presentation::godot_backend::detail
