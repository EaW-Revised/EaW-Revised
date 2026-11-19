#pragma once

#include "eawr/data/xml.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <array>
#include <charconv>
#include <cctype>
#include <algorithm>
#include <cmath>
#include <cstdint>
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

inline const data::XmlNode* last_child(const data::XmlDocument* document, const std::string_view tag) {
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
    return found;
}

template<std::size_t N, typename T = float>
inline std::array<T, N> read(const data::XmlDocument* document, const std::string_view tag,
                               const std::array<T, N> fallback) {
    const data::XmlNode* found = last_child(document, tag);
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
    return {read<1>(root, "Laser_Beam_Z_Scale_Factor", {8.0F})[0], // xml-fallback: Laser_Beam_Z_Scale_Factor; load: apps/viewer/src/presentation_constants.hpp#load_lasers
            read<1>(root, "Laser_Kite_Z_Scale_Factor", {1.2F})[0]}; // xml-fallback: Laser_Kite_Z_Scale_Factor; load: apps/viewer/src/presentation_constants.hpp#load_lasers
}

struct Flash {
    std::array<float, 3> scale{1.0F, 1.1F, 1.25F};
    double duration{0.1};
};
inline Flash load_flash(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) diagnostic("gameconstants.xml", core::format_diagnostic(document.error()));
    const auto* root = document ? &document.value() : nullptr;
    return {read<3>(root, "Shield_Flash_Scale", {1.0F, 1.1F, 1.25F}), // xml-fallback: Shield_Flash_Scale; load: apps/viewer/src/presentation_constants.hpp#load_flash
            read<1, double>(root, "Shield_Flash_Duration", {0.1})[0]}; // xml-fallback: Shield_Flash_Duration; load: apps/viewer/src/presentation_constants.hpp#load_flash
}

// WU-60: the flash an attack order puts on its target (the rapid multi-flash: three pulses).
struct OrderFlash {
    float level{3.0F};
    double pulse_seconds{0.1};
    static constexpr int pulses = 3; // WU-60: the pulse count is fixed by the game, not authored
};
// WU-61: deliberate presentation safety policy: an uninterrupted burst, then one
// real second of quiet. Refused orders never extend or restart this gate.
class OrderFlashGate {
public:
    [[nodiscard]] bool ready() const noexcept { return remaining_seconds_ <= 0.0; }
    bool try_start(const OrderFlash& flash) {
        if (!ready() || !(flash.pulse_seconds > 0.0) || !std::isfinite(flash.pulse_seconds)) return false;
        remaining_seconds_ = flash.pulse_seconds * OrderFlash::pulses + 1.0;
        return true;
    }
    void advance(const double delta) {
        if (std::isfinite(delta) && delta > 0.0) remaining_seconds_ = std::max(0.0, remaining_seconds_ - delta);
    }
private:
    double remaining_seconds_{};
};
inline OrderFlash load_order_flash(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!document) diagnostic("gameconstants.xml", core::format_diagnostic(document.error()));
    const auto* root = document ? &document.value() : nullptr;
    return {read<1>(root, "GUI_Flash_Level", {3.0F})[0], // xml-fallback: GUI_Flash_Level; load: apps/viewer/src/presentation_constants.hpp#load_order_flash
            read<1, double>(root, "GUI_Rapid_Flash_Duration", {0.1})[0]}; // xml-fallback: GUI_Rapid_Flash_Duration; load: apps/viewer/src/presentation_constants.hpp#load_order_flash
}
// WU-61: one comfort control for the stock hull peak; the original peaks at 4x.
inline constexpr double order_flash_peak_multiplier = 2.0;

// WU-60/61: authored rapid cadence and linear fade, with the owner's brightness limits.
inline std::optional<float> order_flash_multiplier(const OrderFlash& flash, const double elapsed,
        const double footprint_half_area = 0.0) {
    if (!(flash.pulse_seconds > 0.0) || !std::isfinite(flash.pulse_seconds)
        || !std::isfinite(flash.level) || !std::isfinite(elapsed) || elapsed < 0.0) return {};
    const double period = flash.pulse_seconds;
    if (elapsed / period >= OrderFlash::pulses) return {};
    const double phase = std::fmod(elapsed, period) / period;
    const double wave = 1.0 - phase;
    // Cap the peak below the original; reduce it inversely with the hull's world footprint above
    // a 1000 x 1000 source-unit box. This bounds area-weighted added light on large hulls.
    const double area_scale = 250000.0 / std::max(250000.0, footprint_half_area);
    const double amplitude = std::clamp(static_cast<double>(flash.level) / OrderFlash{}.level, 0.0, 1.0)
        * (order_flash_peak_multiplier - 1.0) * area_scale;
    return static_cast<float>(1.0 + amplitude * wave);
}

} // namespace eawr::presentation::godot_backend::presentation_constants
