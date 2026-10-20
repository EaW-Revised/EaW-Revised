#include "route_lines.hpp"
#include "eawr/core/text.hpp"
#include "eawr/data/retail_values.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>

namespace eawr::viewer::galactic {
namespace {
std::string upper(std::string_view text) {
    return core::to_upper(text);
}
std::string trim(std::string_view text) {
    return core::trim(text, core::xml_whitespace);
}
std::string attribute(const data::XmlNode& node, std::string_view name) {
    for (const auto& item : node.attributes) if (upper(item.name) == upper(name)) return trim(item.value);
    return {};
}
float number(sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}
std::optional<LineColour> colour(std::string raw, LineColour previous) {
    std::replace(raw.begin(), raw.end(), ',', ' ');
    std::istringstream input(raw);
    std::array<float*, 4> components{&previous.r, &previous.g, &previous.b, &previous.a};
    std::string token;
    // WGP-10 / U-04 research: comma/whitespace tokens, integer prefix -> unsigned byte;
    // omitted components preserve their previous values, extra tokens are ignored.
    for (float* component : components) {
        if (!(input >> token)) break;
        std::string_view text(token);
        if (text.starts_with('+')) text.remove_prefix(1);
        std::int32_t integer{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
        if (parsed.ec == std::errc::result_out_of_range) return std::nullopt;
        *component = static_cast<float>(static_cast<std::uint8_t>(integer)) / 255.0F;
    }
    return previous;
}
LineColour blend(LineColour a, LineColour b, float weight) {
    return {a.r * (1 - weight) + b.r * weight, a.g * (1 - weight) + b.g * weight,
            a.b * (1 - weight) + b.b * weight, a.a * (1 - weight) + b.a * weight};
}
RouteType slot(std::string_view name) {
    const std::string folded = upper(name);
    for (RouteType type : {RouteType::enemy, RouteType::ally, RouteType::good, RouteType::evil, RouteType::corrupt})
        if (folded == upper(to_string(type))) return type;
    return RouteType::neutral;
}
}

std::string_view to_string(RouteType type) noexcept {
    switch (type) {
    case RouteType::neutral: return "neutral";
    case RouteType::enemy: return "enemy";
    case RouteType::ally: return "ally";
    case RouteType::good: return "good";
    case RouteType::evil: return "evil";
    case RouteType::corrupt: return "corrupt";
    }
    return "neutral";
}

RouteStyles parse_route_styles(const data::XmlNode& root) {
    RouteStyles result;
    for (const auto& node : root.children) {
        if (upper(node.name) != "TRADEROUTELINE") continue;
        const std::string name = upper(attribute(node, "Name"));
        if (name.empty()) { result.diagnostics.push_back("unnamed trade route style"); continue; }
        auto& slots = result.named[name];
        for (const auto& settings : node.children) {
            if (upper(settings.name) != "SETTINGS_FOR_FACTION") continue;
            auto& style = slots[static_cast<std::size_t>(slot(attribute(settings, "Name")))];
            for (const auto& value : settings.children) {
                const std::string raw = trim(value.raw_text), key = upper(value.name);
                if (raw.empty()) continue;
                if (upper(raw) == "TBD") { result.diagnostics.push_back(name + ": ignored TBD " + value.name); continue; }
                if (key == "COLOR_ZOOMED_OUT" || key == "COLOR_ZOOMED_IN") {
                    const auto parsed = colour(raw, key == "COLOR_ZOOMED_OUT" ? style.out : style.in);
                    if (parsed) (key == "COLOR_ZOOMED_OUT" ? style.out : style.in) = *parsed;
                    else result.diagnostics.push_back(name + ": invalid " + value.name);
                } else if (key == "WIDTH") {
                    const auto parsed = data::retail::leading_number(raw);
                    if (parsed) style.width = number(parsed->value);
                    else result.diagnostics.push_back(name + ": invalid Width");
                } else if (key == "RENDER_MODE") style.render_mode = upper(raw);
                else if (key == "TEXTURE_NAME") style.texture = upper(raw);
                else if (key == "SPARK_TEXTURE_NAME") style.spark_texture = upper(raw);
                else result.diagnostics.push_back(name + ": unknown " + value.name);
            }
        }
    }
    return result;
}

core::Result<RouteStyles> load_route_styles(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, "data/xml/traderoutelines.xml");
    if (!document) return core::Result<RouteStyles>::failure(document.error());
    if (upper(document.value().root.name) != "TRADEROUTELINES") {
        core::Diagnostic error;
        error.code = "EAWR-GALACTIC-LINES-0001";
        error.message = "route style document must have a TradeRouteLines root";
        return core::Result<RouteStyles>::failure(std::move(error));
    }
    RouteStyles result = parse_route_styles(document.value().root);
    auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!constants) return core::Result<RouteStyles>::failure(constants.error());
    for (const auto& value : constants.value().root.children) {
        const std::string key = upper(value.name), raw = trim(value.raw_text);
        if (key == "GOOD_SIDE_NAME") result.good_faction = raw;
        else if (key == "EVIL_SIDE_NAME") result.evil_faction = raw;
        else if (key == "FLEET_MOVEMENT_LINE_TEXTURE_NAME") result.fleet_texture = raw;
        else if (key == "FLEET_HYPERSPACE_BAND_TEXTURE_NAME") result.band_texture = raw;
    }
    return core::Result<RouteStyles>::success(std::move(result));
}

RouteType route_type(const RouteOwners& owners, bool red_green, std::string_view good, std::string_view evil) {
    if (!owners.a || !owners.b || owners.a_neutral || owners.b_neutral || owners.enemies ||
        owners.a_faction != owners.b_faction) return RouteType::neutral;
    if (red_green) return owners.allied_to_local ? RouteType::ally : RouteType::enemy;
    if (upper(owners.a_faction) == upper(good)) return RouteType::good;
    if (upper(owners.a_faction) == upper(evil)) return RouteType::evil;
    return RouteType::corrupt;
}

void RouteLine::recreate(RouteType type) {
    type_ = type;
    colour_ = blend(style().in, style().out, zoom_); // WGP-12: swapped initial weighting.
    current_ = true;
    sparks_ = type != RouteType::neutral;
}
bool RouteLine::set_type(RouteType type) {
    if (type == type_) return false;
    recreate(type);
    return true;
}
bool RouteLine::set_zoom(float zoom) {
    const bool changed = zoom != zoom_;
    zoom_ = zoom;
    if (!changed || !current_) return false;
    colour_ = blend(style().out, style().in, zoom); // WGP-13: normal zoom weighting.
    sparks_ = type_ != RouteType::neutral && zoom != 1.0F;
    return true;
}

float spark_position(float signed_position, std::uint64_t services) noexcept {
    const double step = static_cast<double>(services % static_cast<std::uint64_t>(route_spark_frames)) / route_spark_frames;
    const double position = std::abs(signed_position) + (signed_position < 0 ? -step : step);
    return static_cast<float>(position - std::floor(position));
}

std::optional<FleetLine> fleet_line(Vec3 position, bool has_destination_planet,
    std::optional<Vec3> immediate_destination, Vec3 desired, float destination_extent,
    bool locally_visible, Rgb owner_colour) {
    if (!has_destination_planet || !locally_visible) return std::nullopt;
    const Vec3 target = immediate_destination.value_or(desired);
    const Vec3 delta{target.x - position.x, target.y - position.y, target.z - position.z};
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    if (!(length > 0.0F)) return std::nullopt; // Project safety at the degenerate adapter boundary.
    const float fraction = destination_extent / length;
    return FleetLine{position, {target.x - delta.x * fraction, target.y - delta.y * fraction,
                               target.z - delta.z * fraction}, owner_colour, length};
}

std::array<float, 12> fleet_model_transform(Vec3 position, Vec3 facing, float scale) noexcept {
    const float yaw = facing.x == 0.0F && facing.y == 0.0F ? 0.0F : std::atan2(facing.y, facing.x);
    const float c = std::cos(yaw) * scale, s = std::sin(yaw) * scale;
    // Rz(yaw) * Rz(-90): the model's +Y goes along the facing, its +X to the facing's right.
    return {s, c, 0.0F, position.x, -c, s, 0.0F, position.y, 0.0F, 0.0F, scale, position.z};
}

} // namespace eawr::viewer::galactic
