#include "eawr/presentation/particles/proxy_binding.hpp"

#include <string>
#include <utility>

namespace eawr::presentation::particles {
namespace {

template<class T>
core::Result<T> fail(const assets::Model& host, std::string message) {
    return core::Result<T>::failure({std::string(diagnostic_codes::mesh_binding),
        core::Severity::error, std::move(message), host.source.logical_path, {}, {}, {}});
}

Basis3 basis(const animation::Matrix& matrix) {
    return {{matrix[0], matrix[1], matrix[2]},
            {matrix[4], matrix[5], matrix[6]},
            {matrix[8], matrix[9], matrix[10]}};
}

Vec3 origin(const animation::Matrix& matrix) {
    return {matrix[12], matrix[13], matrix[14]};
}

} // namespace

core::Result<ProxyMeshBinding> bind_proxy_mesh(
    const assets::Model& host, const std::string_view proxy_name) {
    if (proxy_name.empty()) return fail<ProxyMeshBinding>(host, "proxy name is empty");
    std::size_t selected = host.proxies.size();
    for (std::size_t index = 0; index < host.proxies.size(); ++index) {
        if (host.proxies[index].name != proxy_name) continue;
        if (selected != host.proxies.size()) return fail<ProxyMeshBinding>(host,
            "named proxy is ambiguous: " + std::string(proxy_name));
        selected = index;
    }
    if (selected == host.proxies.size()) return fail<ProxyMeshBinding>(host,
        "named proxy is absent: " + std::string(proxy_name));
    return bind_proxy_mesh(host, selected);
}

core::Result<ProxyMeshBinding> bind_proxy_mesh(
    const assets::Model& host, const std::size_t proxy_index) {
    if (proxy_index >= host.proxies.size())
        return fail<ProxyMeshBinding>(host, "proxy index is out of range");
    const assets::Proxy* selected = &host.proxies[proxy_index];
    if (selected->bone >= host.bones.size())
        return fail<ProxyMeshBinding>(host, "proxy bone index is out of range");
    const auto parent = host.bones[selected->bone].parent;
    if (parent < 0 || static_cast<std::size_t>(parent) >= host.bones.size())
        return fail<ProxyMeshBinding>(host, "proxy bone has no valid immediate parent");
    const auto owner = static_cast<std::size_t>(parent);
    for (std::size_t index = 0; index < host.meshes.size(); ++index) {
        const auto& source = host.meshes[index];
        if (source.bone != parent) continue;
        ProxyMeshBinding result;
        result.proxy_bone = selected->bone;
        result.owner_bone = owner;
        result.mesh_index = index;
        result.binding.geometry.submeshes.reserve(source.submeshes.size());
        for (const auto& input : source.submeshes) {
            MeshSubmesh submesh;
            submesh.vertices.reserve(input.vertices.size());
            for (const auto& vertex : input.vertices) {
                submesh.vertices.push_back({
                    {vertex.position.x, vertex.position.y, vertex.position.z},
                    {vertex.normal.x, vertex.normal.y, vertex.normal.z}});
            }
            submesh.triangle_indices.assign(input.indices.begin(), input.indices.end());
            result.binding.geometry.submeshes.push_back(std::move(submesh));
        }
        return core::Result<ProxyMeshBinding>::success(std::move(result));
    }
    return fail<ProxyMeshBinding>(host, "proxy immediate parent has no owning mesh");
}

core::Result<void> proxy_mesh_frames(
    const animation::Pose& pose, const ProxyMeshBinding& selection,
    EmitterFrame& emitter, MeshFrame& mesh) {
    if (selection.proxy_bone >= pose.bones.size() || selection.owner_bone >= pose.bones.size())
        return core::Result<void>::failure({std::string(diagnostic_codes::mesh_binding),
            core::Severity::error, "proxy or mesh owner is absent from pose", {}, {}, {}, {}});
    const auto& proxy = pose.bones[selection.proxy_bone].model_asset;
    const auto& owner = pose.bones[selection.owner_bone].model_asset;
    emitter = {origin(proxy), basis(proxy)};
    mesh = {origin(owner), basis(owner)};
    return core::Result<void>::success();
}

} // namespace eawr::presentation::particles
