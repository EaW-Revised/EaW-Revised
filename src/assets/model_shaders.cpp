#include "eawr/assets/model_shaders.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace eawr::assets {
namespace {

// Chunk types of the model container (src/assets/model.cpp reads the same ones in full).
constexpr std::uint32_t mesh_chunk = 0x400;
constexpr std::uint32_t mesh_info_chunk = 0x402;
constexpr std::uint32_t submesh_material_chunk = 0x10100;
constexpr std::uint32_t shader_name_chunk = 0x10101;
// The stored-hidden word of the mesh info payload: submesh count, two vec3 bounds and one reserved word come first.
constexpr std::uint64_t hidden_word_offset = 4U + 24U + 4U;
constexpr std::uint64_t header_bytes = 8;
constexpr std::size_t name_cap = 256;

struct Header final {
    std::uint32_t type{};
    bool group{};
    std::uint32_t size{};
};

bool read_u32(const PositionalReader& read, const std::uint64_t at, std::uint32_t& value) {
    std::array<std::byte, 4> raw{};
    if (!read(at, raw)) return false;
    value = static_cast<std::uint32_t>(raw[0]) | static_cast<std::uint32_t>(raw[1]) << 8U
        | static_cast<std::uint32_t>(raw[2]) << 16U | static_cast<std::uint32_t>(raw[3]) << 24U;
    return true;
}

bool read_header(const PositionalReader& read, const std::uint64_t at, Header& header) {
    std::uint32_t type{}, word{};
    if (!read_u32(read, at, type) || !read_u32(read, at + 4U, word)) return false;
    header = {type, (word & 0x80000000U) != 0U, word & 0x7fffffffU};
    return true;
}

} // namespace

ModelShaderScan scan_model_shaders(const std::uint64_t size, const PositionalReader& read, const std::size_t max_meshes) {
    ModelShaderScan result;
    std::size_t meshes = 0;
    std::uint64_t position = 0;
    while (position + header_bytes <= size) {
        Header top;
        if (!read_header(read, position, top)) { result.complete = false; return result; }
        const std::uint64_t body = position + header_bytes;
        const std::uint64_t end = body + top.size;
        if (end > size) { result.complete = false; return result; }
        if (top.type == mesh_chunk && top.group) {
            if (++meshes > max_meshes) { result.complete = false; return result; }
            bool visible = true;
            std::vector<std::string> shaders;
            std::uint64_t child = body;
            while (child + header_bytes <= end) {
                Header header;
                if (!read_header(read, child, header)) { result.complete = false; return result; }
                const std::uint64_t child_body = child + header_bytes;
                const std::uint64_t child_end = child_body + header.size;
                if (child_end > end) { result.complete = false; return result; }
                if (header.type == mesh_info_chunk && header.size >= hidden_word_offset + 4U) {
                    std::uint32_t hidden{};
                    if (!read_u32(read, child_body + hidden_word_offset, hidden)) { result.complete = false; return result; }
                    visible = hidden == 0U;
                } else if (header.type == submesh_material_chunk && header.group && header.size >= header_bytes) {
                    Header name;
                    if (!read_header(read, child_body, name)) { result.complete = false; return result; }
                    if (name.type == shader_name_chunk && !name.group
                        && child_body + header_bytes + name.size <= child_end) {
                        std::array<std::byte, name_cap> raw{};
                        const std::size_t count = std::min<std::size_t>(name.size, name_cap);
                        if (count != 0U && !read(child_body + header_bytes, std::span(raw.data(), count))) {
                            result.complete = false;
                            return result;
                        }
                        const auto* text = reinterpret_cast<const char*>(raw.data());
                        shaders.emplace_back(text, static_cast<std::size_t>(std::find(text, text + count, '\0') - text));
                    }
                }
                child = child_end;
            }
            for (std::string& shader : shaders) result.meshes.push_back({std::move(shader), visible});
        }
        position = end;
    }
    return result;
}

ModelShaderScan scan_model_shaders(const std::span<const std::byte> bytes) {
    return scan_model_shaders(bytes.size(), [bytes](const std::uint64_t offset, const std::span<std::byte> out) {
        if (offset > bytes.size() || out.size() > bytes.size() - offset) return false;
        std::memcpy(out.data(), bytes.data() + offset, out.size());
        return true;
    });
}

} // namespace eawr::assets
