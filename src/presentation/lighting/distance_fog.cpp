#include "eawr/presentation/lighting/distance_fog.hpp"

#include "eawr/core/text.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <string>

namespace eawr::presentation::lighting {
namespace {

constexpr float start_cap = 10000.0F;

[[nodiscard]] bool read_float(const std::span<const std::byte> bytes, const std::size_t offset, float& out) noexcept {
    if (bytes.size() < offset + 4U) return false; // a longer mini is read as its prefix (R-DEC-03)
    std::uint32_t raw{};
    std::memcpy(&raw, bytes.data() + offset, 4U);
    out = std::bit_cast<float>(raw);
    return std::isfinite(out);
}

// Effects whose vertex stage writes the fog factor from FOG_VALS (measured on the installed effect files' vertex stages;
// the other effects write a constant fog output).
constexpr std::array<std::string_view, 23> fogged_programs{
    "batchmeshalpha", "batchmeshgloss", "meshalpha", "meshalphagloss", "meshalphascroll", "meshbumpcolorize",
    "meshbumpreflectcolorize", "meshcollision", "meshgloss", "meshglosscolorize", "meshoccludedunit", "planet",
    "grass", "tree", "terrainmeshbump", "terrainmeshgloss", "rskinalpha", "rskinalphagloss", "rskinbumpcolorize",
    "rskinbumpreflectcolorize", "rskingloss", "rskinglosscolorize", "rskinoccludedunit"};

} // namespace

FogVals fog_vals(const DistanceFog& fog) noexcept {
    if (!fog.enabled || !std::isfinite(fog.start_distance) || !std::isfinite(fog.end_distance) || !(fog.start_distance < fog.end_distance)) return {};
    const float slope = 1.0F / (fog.start_distance - fog.end_distance);
    return {slope, -(fog.end_distance * slope)};
}

FogGlobals fog_globals(const std::optional<DistanceFog>& fog) noexcept {
    if (!fog) return {};
    const FogVals vals = fog_vals(*fog);
    if (!fog->enabled || vals == FogVals{}) return {};
    return {vals, fog->color};
}

float fog_factor(const FogVals& vals, const float distance) noexcept {
    return std::clamp(vals.x * distance + vals.y, 0.0F, 1.0F);
}

std::array<float, 3> fogged_color(const std::array<float, 3>& color, const DistanceFog& fog, const float distance) noexcept {
    const float factor = fog_factor(fog_vals(fog), distance);
    std::array<float, 3> result{};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        result[channel] = factor * color[channel] + (1.0F - factor) * fog.color[channel];
    }
    return result;
}

std::optional<DistanceFog> distance_fog(const std::span<const RawField> fields) {
    DistanceFog fog;
    float start = fog.start_distance;
    float end = fog.end_distance;
    for (const RawField& field : fields) {
        float value{};
        switch (field.id) {
        case fog_color_field_id: {
            std::array<float, 3> color{};
            for (std::size_t channel = 0; channel < 3; ++channel) {
                if (!read_float(field.bytes, channel * 4U, color[channel])) return std::nullopt;
            }
            fog.color = color;
            break;
        }
        case fog_enabled_field_id:
            if (!field.bytes.empty()) fog.enabled = std::to_integer<unsigned>(field.bytes.front()) != 0U;
            break;
        case fog_start_field_id:
            if (read_float(field.bytes, 0U, value)) start = value;
            break;
        case fog_end_field_id:
            if (read_float(field.bytes, 0U, value)) end = value;
            break;
        default: break;
        }
    }
    fog.start_distance = std::min(start, start_cap);
    fog.end_distance = std::max(fog.start_distance, end);
    return fog;
}

bool program_fades_to_fog(const std::string_view program) noexcept {
    std::string name(program);
    if (const auto slash = name.find_last_of("/\\"); slash != std::string::npos) name.erase(0, slash + 1);
    if (const auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    for (char& c : name) c = static_cast<char>(core::ascii_lower(static_cast<unsigned char>(c)));
    return std::find(fogged_programs.begin(), fogged_programs.end(), std::string_view(name)) != fogged_programs.end();
}

} // namespace eawr::presentation::lighting
