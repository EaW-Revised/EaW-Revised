#pragma once
#include "eawr/scene/scene.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

// Private helpers shared by the scene implementation files.
namespace eawr::scene {

constexpr std::array<std::string_view, 2> texture_suffixes{".tga", ".dds"};
constexpr std::array<std::string_view, 1> model_suffixes{".alo"};

[[nodiscard]] char fold(char value) noexcept;
[[nodiscard]] bool ieq(std::string_view left, std::string_view right) noexcept;
[[nodiscard]] std::string canonical(std::string_view name);
[[nodiscard]] std::string probe(const AssetAccess& access, std::string_view root, std::string_view name,
                                std::span<const std::string_view> suffixes);
[[nodiscard]] core::Diagnostic diagnostic(std::string_view code, std::string message);
[[nodiscard]] std::string hex32(std::uint32_t value);
[[nodiscard]] std::string trimmed(std::string_view value);
[[nodiscard]] ModelFacts model_facts(const AssetAccess& access, const std::string& path);

} // namespace eawr::scene
