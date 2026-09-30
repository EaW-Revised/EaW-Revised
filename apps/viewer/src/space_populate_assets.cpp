#include "space_populate_internal.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace eawr::presentation::godot_backend {

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; };
        return lower(a) == lower(b);
    });
}

// The scene builder's reference probe (the trimmed authored name as written,
// then by stem with each shipped suffix), for the hardpoint models and
// textures it does not read itself.
[[nodiscard]] std::string probe(scene::VfsAssetCache& cache, const std::string_view root, std::string_view name,
                                const std::span<const std::string_view> suffixes) {
    const auto space = [](const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!name.empty() && space(name.front())) name.remove_prefix(1);
    while (!name.empty() && space(name.back())) name.remove_suffix(1);
    std::string folded;
    for (const char character : name) {
        folded.push_back(character == '\\' ? '/'
            : (character >= 'A' && character <= 'Z' ? static_cast<char>(character + ('a' - 'A')) : character));
    }
    if (folded.empty()) return {};
    const std::string literal = std::string(root) + folded;
    if (cache.exists(literal)) return literal;
    std::string stem = folded;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        const std::string candidate = std::string(root) + stem + std::string(suffix);
        if (cache.exists(candidate)) return candidate;
    }
    return {};
}

[[nodiscard]] assets::Texture placeholder_texture() {
    assets::MipLevel mip;
    mip.width = 1;
    mip.height = 1;
    mip.row_pitch = 4;
    mip.bytes = {std::byte{190}, std::byte{190}, std::byte{190}, std::byte{255}};
    assets::Texture texture;
    texture.width = 1;
    texture.height = 1;
    texture.format = assets::PixelFormat::rgba8;
    texture.mips.push_back(std::move(mip));
    return texture;
}

} // namespace eawr::presentation::godot_backend
