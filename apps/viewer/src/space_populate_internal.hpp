#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/scene/scene.hpp"

#include <array>
#include <span>
#include <string>
#include <string_view>

// Private asset lookup helpers shared by the space population files.
namespace eawr::presentation::godot_backend {

constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
constexpr std::array<std::string_view, 1> model_suffixes{".alo"};

[[nodiscard]] bool ieq(std::string_view left, std::string_view right);
[[nodiscard]] std::string probe(scene::VfsAssetCache& cache, std::string_view root, std::string_view name,
                                std::span<const std::string_view> suffixes);
[[nodiscard]] assets::Texture placeholder_texture();

} // namespace eawr::presentation::godot_backend
