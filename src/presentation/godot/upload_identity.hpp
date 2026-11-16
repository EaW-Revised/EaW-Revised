#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/presentation/model_upload.hpp"
#include "eawr/presentation/renderer.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

using UploadIdentity = std::array<std::uint8_t, 32>;

// The render identity of one upload: exactly the model, texture and material
// fields GodotRenderer::upload turns into RenderingServer resources. Source
// provenance and notices are left out, so equal content read from another
// layer is still the same asset; any consumed difference is a different one.
// A repeated upload is a shared reference only when the identities match.
//
// `streaming`: the bytes are fed to a SHA-256 hasher in 64 KiB pieces instead of being kept (large spans go in without a
// copy), so the digest equals core::sha256 over the same byte sequence without ever holding it. take() needs the
// default (buffering) mode.
class UploadIdentityWriter final {
public:
    UploadIdentityWriter() = default;
    struct Streaming final {};
    explicit UploadIdentityWriter(Streaming) : sink_(std::in_place) {}

    template <typename T>
    void scalar(const T value) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::array<std::uint8_t, sizeof(T)> raw{};
        std::memcpy(raw.data(), &value, sizeof(T));
        bytes_.insert(bytes_.end(), raw.begin(), raw.end());
        flush_if_full();
    }

    void size(const std::size_t value) { scalar(static_cast<std::uint64_t>(value)); }

    void text(const std::string_view value) {
        size(value.size());
        bytes_.insert(bytes_.end(), value.begin(), value.end());
        flush_if_full();
    }

    void vec(const assets::Vec2f& value) { scalar(value.x); scalar(value.y); }
    void vec(const assets::Vec3f& value) { scalar(value.x); scalar(value.y); scalar(value.z); }
    void vec(const assets::Vec4f& value) {
        scalar(value.x); scalar(value.y); scalar(value.z); scalar(value.w);
    }

    void raw(const std::span<const std::byte> value) {
        size(value.size());
        if (value.empty()) return;
        const auto* begin = reinterpret_cast<const std::uint8_t*>(value.data());
        if (sink_ && value.size() >= flush_bytes) {
            flush();
            sink_->update(std::span<const std::uint8_t>(begin, value.size()));
            return;
        }
        bytes_.insert(bytes_.end(), begin, begin + value.size());
        flush_if_full();
    }

    [[nodiscard]] UploadIdentity digest() {
        if (!sink_) return core::sha256(bytes_);
        flush();
        return sink_->digest();
    }
    [[nodiscard]] std::vector<std::uint8_t> take() && { return std::move(bytes_); }

private:
    static constexpr std::size_t flush_bytes = 64U * 1024U;
    void flush() {
        if (sink_ && !bytes_.empty()) sink_->update(bytes_);
        if (sink_) bytes_.clear();
    }
    void flush_if_full() {
        if (sink_ && bytes_.size() >= flush_bytes) flush();
    }
    std::vector<std::uint8_t> bytes_;
    std::optional<core::Sha256Hasher> sink_;
};

// One per-binding texture of an upload (GodotRenderer::BindingTexture).
struct NamedTexture final {
    std::string_view binding;
    const assets::Texture* texture{};
};

inline void write_texture(UploadIdentityWriter& out, const assets::Texture& texture) {
    out.scalar(texture.format);
    out.scalar(texture.width);
    out.scalar(texture.height);
    out.size(texture.mips.size());
    for (const assets::MipLevel& mip : texture.mips) out.raw(mip.bytes);
}

// `binding_textures` are appended after the material, so an upload without
// any keeps the identity it had before per-binding textures existed.
template <typename Model>
inline void write_upload_identity(
    UploadIdentityWriter& out,
    const Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material,
    const std::span<const NamedTexture> binding_textures) {
    // Mesh upload consumes the bone count (skin binding and skinning checks),
    // visible meshes, their rigid bone and every submesh's skinned geometry,
    // vertex colours included.
    out.size(model.bones.size());
    out.size(model.meshes.size());
    for (const auto& mesh : model.meshes) {
        out.scalar(mesh.visible);
        if (!mesh.visible) continue;
        out.scalar(mesh.bone);
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
                out.vec(vertex.texcoord[0]);
                out.vec(vertex.color);
                for (const std::uint32_t index : vertex.bone_indices) out.scalar(index);
                for (const float weight : vertex.bone_weights) out.scalar(weight);
            }
            out.size(submesh.indices.size());
            for (const std::uint16_t index : submesh.indices) out.scalar(index);
        }
    }

    write_texture(out, texture);

    out.scalar(material.schema_version);
    out.scalar(material.route);
    out.scalar(material.pass);
    out.text(material.program);
    out.text(material.technique);
    out.text(material.pass_name);
    out.size(material.bindings.size());
    for (const MaterialBinding& binding : material.bindings) {
        out.text(binding.name);
        out.size(binding.value.index());
        std::visit([&out](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) {
                out.text(value);
            } else if constexpr (std::is_same_v<T, assets::Vec3f> || std::is_same_v<T, assets::Vec4f>) {
                out.vec(value);
            } else {
                out.scalar(value);
            }
        }, binding.value);
    }
    if (!binding_textures.empty()) {
        out.size(binding_textures.size());
        for (const NamedTexture& item : binding_textures) {
            out.text(item.binding);
            if (item.texture != nullptr) write_texture(out, *item.texture);
        }
    }
}

template <typename Model>
[[nodiscard]] inline std::vector<std::uint8_t> upload_identity_bytes(
    const Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material,
    const std::span<const NamedTexture> binding_textures = {}) {
    UploadIdentityWriter out;
    write_upload_identity(out, model, texture, material, binding_textures);
    return std::move(out).take();
}

template <typename Model>
[[nodiscard]] inline UploadIdentity upload_identity(
    const Model& model,
    const assets::Texture& texture,
    const MaterialDescription& material,
    const std::span<const NamedTexture> binding_textures = {}) {
    UploadIdentityWriter out{UploadIdentityWriter::Streaming{}};
    write_upload_identity(out, model, texture, material, binding_textures);
    return out.digest();
}

// Everything one upload's identity reads, owned: shared immutable snapshots and a copy of the small material. A pool
// worker evaluates it, so the model and texture bytes are never serialised on the uploading thread.
struct UploadIdentityInputs final {
    std::shared_ptr<const assets::Model> model;
    std::optional<ModelSurface> surface;
    std::shared_ptr<const assets::Texture> texture;
    MaterialDescription material;
    std::vector<std::pair<std::string, std::shared_ptr<const assets::Texture>>> bindings;
    // Bytes the job keeps alive beyond the uploading call (the pool's admission budget counts them).
    std::size_t cost{};

    [[nodiscard]] UploadIdentity evaluate() const {
        const ModelView view(*model, surface);
        std::vector<NamedTexture> named;
        named.reserve(bindings.size());
        for (const auto& [name, texture_ptr] : bindings) named.push_back({name, texture_ptr.get()});
        return upload_identity(view, *texture, material, named);
    }
};

} // namespace eawr::presentation::godot_backend::detail
