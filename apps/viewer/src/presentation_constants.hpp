#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::godot_backend::presentation_constants {

inline constexpr float logical_frame_seconds =
    1.0F / static_cast<float>(sim::tactical::logical_frames_per_second);

template<typename T = float>
inline std::optional<T> number(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    if (text.starts_with('+')) text.remove_prefix(1);
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) return {};
    return value;
}

// BP-01: integer-valued XML spellings such as 01 and 1.0 select the same renderer.
inline int custom_render(const std::string_view text) {
    double value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)) return 0;
    return value == 1.0 ? 1 : value == 2.0 ? 2 : 0;
}

inline void diagnostic(const std::string_view tag, const std::string_view reason) {
    std::fprintf(stderr, "presentation gameconstants: %.*s: %.*s; using fallback\n",
        static_cast<int>(tag.size()), tag.data(), static_cast<int>(reason.size()), reason.data());
}

template<std::size_t N, typename T = float>
inline std::array<T, N> read(const data::XmlDocument* document, const std::string_view tag,
                               const std::array<T, N> fallback) {
    const data::XmlNode* found = nullptr;
    if (document) for (const auto& child : document->root.children) {
        if (child.name.size() != tag.size()) continue;
        bool matches = true;
        for (std::size_t i = 0; i < tag.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(child.name[i])) !=
                std::tolower(static_cast<unsigned char>(tag[i]))) { matches = false; break; }
        }
        if (matches) found = &child;
    }
    if (!found) { diagnostic(tag, "missing tag"); return fallback; }
    data::tag_trace::used(*found);
    std::array<T, N> result{};
    std::string_view remaining = found->raw_text;
    std::size_t count = 0;
    while (!remaining.empty()) {
        const auto first = remaining.find_first_not_of(" ,\t\r\n");
        if (first == std::string_view::npos) break;
        remaining.remove_prefix(first);
        const auto end = remaining.find_first_of(" ,\t\r\n");
        const auto value = number<T>(remaining.substr(0, end));
        if (count == N || !value || *value < 0.0F) {
            diagnostic(tag, "expected finite nonnegative values"); return fallback;
        }
        result[count++] = *value;
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end);
    }
    if (count != N) { diagnostic(tag, "wrong component count"); return fallback; }
    return result;
}

// BP-04/BP-07: a zero authored Z scale leaves the width unchanged at every depth.
inline float laser_width(const float width, const float z_scale, const float depth) {
    return width * (z_scale * depth + 1.0F);
}

struct Lasers {
    float beam{8.0F};
    float kite{1.2F};
};
inline Lasers load_lasers(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) diagnostic("gameconstants.xml", core::format_diagnostic(document.error()));
    const auto* root = document ? &document.value() : nullptr;
    return {read<1>(root, "Laser_Beam_Z_Scale_Factor", {8.0F})[0],
            read<1>(root, "Laser_Kite_Z_Scale_Factor", {1.2F})[0]};
}

struct Flash {
    std::array<float, 3> scale{1.0F, 1.1F, 1.25F};
    double duration{0.1};
};
inline Flash load_flash(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) diagnostic("gameconstants.xml", core::format_diagnostic(document.error()));
    const auto* root = document ? &document.value() : nullptr;
    return {read<3>(root, "Shield_Flash_Scale", {1.0F, 1.1F, 1.25F}),
            read<1, double>(root, "Shield_Flash_Duration", {0.1})[0]};
}

} // namespace eawr::presentation::godot_backend::presentation_constants
