#pragma once

#include <initializer_list>
#include <string>
#include <string_view>

// Dependency-free name rules shared by scene acquisition and the viewer.
namespace eawr::assets {

[[nodiscard]] inline std::string normalized_asset_name(std::string_view name) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!name.empty() && space(name.front())) name.remove_prefix(1);
    while (!name.empty() && space(name.back())) name.remove_suffix(1);
    std::string result;
    result.reserve(name.size());
    for (const char c : name) {
        result.push_back(c == '\\' ? '/' : (c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c));
    }
    return result;
}

// Existing extensionless/TGA/DDS requests retain their literal, TGA, DDS
// sequence. Other source-art suffixes use RGC-122's first dot-token DDS retry.
// The retained legacy branch is a known deviation from that retail rule.
template <typename Exists>
[[nodiscard]] std::string resolve_texture_name(const std::string_view root, const std::string_view name, Exists&& exists) {
    const std::string folded = normalized_asset_name(name);
    if (folded.empty()) return {};
    const std::string literal = std::string(root) + folded;
    if (exists(literal)) return literal;
    const auto slash = folded.find_last_of('/');
    const auto dot = folded.find('.', slash == std::string::npos ? 0 : slash + 1);
    if (dot == std::string::npos || folded.ends_with(".tga") || folded.ends_with(".dds")) {
        std::string stem = folded;
        if (stem.size() > 4 && (folded.ends_with(".tga") || folded.ends_with(".dds"))) stem.resize(stem.size() - 4);
        for (const std::string_view suffix : {".tga", ".dds"}) {
            const std::string candidate = std::string(root) + stem + std::string(suffix);
            if (exists(candidate)) return candidate;
        }
        return {};
    }
    const std::string candidate = std::string(root) + folded.substr(0, dot) + ".dds";
    return exists(candidate) ? candidate : std::string{};
}

// Only an extensionless leaf gains the model suffix; authored extensions stay intact.
[[nodiscard]] inline std::string normalized_model_name(const std::string_view name) {
    std::string result = normalized_asset_name(name);
    if (result.empty()) return {};
    const auto slash = result.find_last_of('/');
    if (result.find('.', slash == std::string::npos ? 0 : slash + 1) == std::string::npos)
        result += ".alo";
    return result;
}

} // namespace eawr::assets
