#include "eawr/assets/model_facts.hpp"
#include "eawr/core/text.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

namespace eawr::assets {
namespace {

bool kept_mesh(const Mesh& mesh) {
    return mesh.collidable || core::iequals(mesh.name, "COLLISIONBOX");
}

void put_u8(std::vector<std::uint8_t>& out, const std::uint8_t value) { out.push_back(value); }
void put_u16(std::vector<std::uint8_t>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffU));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}
void put_u32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
}
void put_float(std::vector<std::uint8_t>& out, const float value) { put_u32(out, std::bit_cast<std::uint32_t>(value)); }

bool put_name(std::vector<std::uint8_t>& out, const std::string& name) {
    if (name.size() > std::numeric_limits<std::uint16_t>::max()) return false;
    put_u16(out, static_cast<std::uint16_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
    return true;
}

// Reads little-endian fields; every read checks the remaining size, a failed read poisons the reader.
class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return bytes_.size() - at_; }
    std::uint8_t u8() { return take(1) ? bytes_[at_ - 1] : std::uint8_t{}; }
    std::uint16_t u16() {
        if (!take(2)) return 0;
        return static_cast<std::uint16_t>(bytes_[at_ - 2] | (bytes_[at_ - 1] << 8U));
    }
    std::uint32_t u32() {
        if (!take(4)) return 0;
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes_[at_ - 4 + i]) << (i * 8U);
        return value;
    }
    float f32() { return std::bit_cast<float>(u32()); }
    std::string name() {
        const auto size = u16();
        if (!take(size)) return {};
        return std::string(reinterpret_cast<const char*>(bytes_.data() + at_ - size), size);
    }
    // A count whose elements take at least `element_bytes` each: refuses a count the remaining bytes cannot hold.
    std::uint32_t count(const std::size_t element_bytes) {
        const auto value = u32();
        if (ok_ && static_cast<std::uint64_t>(value) * element_bytes > remaining()) ok_ = false;
        return ok_ ? value : 0;
    }
private:
    bool take(const std::size_t size) {
        if (!ok_ || size > remaining()) { ok_ = false; return false; }
        at_ += size;
        return true;
    }
    std::span<const std::uint8_t> bytes_;
    std::size_t at_{};
    bool ok_{true};
};

} // namespace

Model simulation_projection(const Model& full) {
    Model result;
    result.source = full.source;
    result.bones.reserve(full.bones.size());
    for (const auto& bone : full.bones) {
        Bone kept;
        kept.name = bone.name;
        kept.parent = bone.parent;
        kept.visible = bone.visible;
        kept.relative_transform = bone.relative_transform;
        result.bones.push_back(std::move(kept));
    }
    for (const auto& mesh : full.meshes) {
        if (!kept_mesh(mesh)) continue;
        Mesh kept;
        kept.name = mesh.name;
        kept.bounds_min = mesh.bounds_min;
        kept.bounds_max = mesh.bounds_max;
        kept.collidable = mesh.collidable;
        kept.bone = mesh.bone;
        if (!mesh.collidable) {
            result.meshes.push_back(std::move(kept));
            continue; // FD-10 reads only the named box's bounds and placement.
        }
        kept.submeshes.reserve(mesh.submeshes.size());
        for (const auto& submesh : mesh.submeshes) {
            Submesh part;
            part.vertices.resize(submesh.vertices.size());
            for (std::size_t i = 0; i < submesh.vertices.size(); ++i) {
                part.vertices[i].position = submesh.vertices[i].position;
                part.vertices[i].bone_indices[0] = submesh.vertices[i].bone_indices[0];
            }
            part.indices = submesh.indices;
            part.skin_bones = submesh.skin_bones;
            kept.submeshes.push_back(std::move(part));
        }
        result.meshes.push_back(std::move(kept));
    }
    return result;
}

std::optional<std::vector<std::uint8_t>> encode_model_facts(const Model& model, const std::string_view sha256_hex) {
    std::vector<std::uint8_t> out;
    if (!sha256_hex.empty() && sha256_hex.size() != 64) return std::nullopt;
    put_u8(out, sha256_hex.empty() ? 0 : 1);
    for (std::size_t i = 0; i < 32; ++i) {
        std::uint8_t byte = 0;
        if (!sha256_hex.empty()) {
            for (std::size_t half = 0; half < 2; ++half) {
                const char c = sha256_hex[i * 2 + half];
                int digit = -1;
                if (c >= '0' && c <= '9') digit = c - '0';
                else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
                if (digit < 0) return std::nullopt;
                byte = static_cast<std::uint8_t>((byte << 4U) | static_cast<unsigned>(digit));
            }
        }
        out.push_back(byte);
    }
    if (model.bones.size() > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    put_u32(out, static_cast<std::uint32_t>(model.bones.size()));
    for (const auto& bone : model.bones) {
        if (!put_name(out, bone.name)) return std::nullopt;
        put_u32(out, static_cast<std::uint32_t>(bone.parent));
        put_u8(out, bone.visible ? 1 : 0);
        for (const float value : bone.relative_transform) put_float(out, value);
    }
    std::uint32_t meshes = 0;
    for (const auto& mesh : model.meshes) meshes += kept_mesh(mesh) ? 1U : 0U;
    put_u32(out, meshes);
    for (const auto& mesh : model.meshes) {
        if (!kept_mesh(mesh)) continue;
        if (!put_name(out, mesh.name)) return std::nullopt;
        put_u32(out, static_cast<std::uint32_t>(mesh.bone));
        put_u8(out, mesh.collidable ? 1 : 0);
        for (const float value : {mesh.bounds_min.x, mesh.bounds_min.y, mesh.bounds_min.z, mesh.bounds_max.x, mesh.bounds_max.y,
                                  mesh.bounds_max.z}) put_float(out, value);
        put_u32(out, mesh.collidable ? static_cast<std::uint32_t>(mesh.submeshes.size()) : 0U);
        if (!mesh.collidable) continue;
        for (const auto& submesh : mesh.submeshes) {
            put_u32(out, static_cast<std::uint32_t>(submesh.vertices.size()));
            for (const auto& vertex : submesh.vertices) {
                put_float(out, vertex.position.x);
                put_float(out, vertex.position.y);
                put_float(out, vertex.position.z);
                put_u32(out, vertex.bone_indices[0]);
            }
            put_u32(out, static_cast<std::uint32_t>(submesh.indices.size()));
            for (const auto index : submesh.indices) put_u16(out, index);
            put_u32(out, static_cast<std::uint32_t>(submesh.skin_bones.size()));
            for (const auto bone : submesh.skin_bones) put_u32(out, bone);
        }
    }
    return out;
}

std::optional<DecodedModelFacts> decode_model_facts(const std::span<const std::uint8_t> body, Source source) {
    Reader in(body);
    DecodedModelFacts result;
    result.model.source = std::move(source);
    const auto has_sha = in.u8();
    if (has_sha > 1) return std::nullopt;
    constexpr char digits[] = "0123456789abcdef";
    std::string sha;
    for (unsigned i = 0; i < 32; ++i) {
        const auto byte = in.u8();
        sha.push_back(digits[byte >> 4U]);
        sha.push_back(digits[byte & 15U]);
    }
    if (has_sha != 0) result.sha256_hex = std::move(sha);
    const auto bones = in.count(2 + 4 + 1 + 48);
    result.model.bones.reserve(bones);
    for (std::uint32_t i = 0; i < bones && in.ok(); ++i) {
        Bone bone;
        bone.name = in.name();
        bone.parent = static_cast<std::int32_t>(in.u32());
        const auto visible = in.u8();
        if (visible > 1) return std::nullopt;
        bone.visible = visible != 0;
        for (auto& value : bone.relative_transform) value = in.f32();
        result.model.bones.push_back(std::move(bone));
    }
    const auto meshes = in.count(2 + 4 + 1 + 24 + 4);
    for (std::uint32_t m = 0; m < meshes && in.ok(); ++m) {
        Mesh mesh;
        mesh.name = in.name();
        mesh.bone = static_cast<std::int32_t>(in.u32());
        const auto collidable = in.u8();
        if (collidable > 1) return std::nullopt;
        mesh.collidable = collidable != 0;
        mesh.bounds_min = {in.f32(), in.f32(), in.f32()};
        mesh.bounds_max = {in.f32(), in.f32(), in.f32()};
        const auto parts = in.count(12);
        if (!mesh.collidable && parts != 0) return std::nullopt;
        for (std::uint32_t s = 0; s < parts && in.ok(); ++s) {
            Submesh part;
            const auto vertices = in.count(16);
            part.vertices.resize(vertices);
            for (auto& vertex : part.vertices) {
                vertex.position = {in.f32(), in.f32(), in.f32()};
                vertex.bone_indices[0] = in.u32();
            }
            const auto indices = in.count(2);
            part.indices.resize(indices);
            for (auto& index : part.indices) index = in.u16();
            const auto skin = in.count(4);
            part.skin_bones.resize(skin);
            for (auto& bone : part.skin_bones) bone = in.u32();
            mesh.submeshes.push_back(std::move(part));
        }
        result.model.meshes.push_back(std::move(mesh));
    }
    if (!in.ok() || in.remaining() != 0) return std::nullopt;
    return result;
}

} // namespace eawr::assets
