#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/render.hpp"

#include <cstddef>
#include <string_view>

namespace eawr::presentation::particles {

// The original mesh order is significant: the first mesh on the proxy bone's
// immediate parent owns the emission surface. All geometry is copied.
struct ProxyMeshBinding final {
    MeshBinding binding;
    std::size_t proxy_bone{};
    std::size_t owner_bone{};
    std::size_t mesh_index{};
};

[[nodiscard]] core::Result<ProxyMeshBinding> bind_proxy_mesh(
    const assets::Model& host, std::string_view proxy_name);
// The same for the proxy at `proxy_index` in host.proxies: a model may name
// several proxies alike (a fighter's twin engines), each on its own bone.
[[nodiscard]] core::Result<ProxyMeshBinding> bind_proxy_mesh(
    const assets::Model& host, std::size_t proxy_index);

// Pose matrices are already in the source Z-up basis. No render conversion or
// skin matrix participates in either particle frame.
[[nodiscard]] core::Result<void> proxy_mesh_frames(
    const animation::Pose& pose, const ProxyMeshBinding& selection,
    EmitterFrame& emitter, MeshFrame& mesh);

} // namespace eawr::presentation::particles
