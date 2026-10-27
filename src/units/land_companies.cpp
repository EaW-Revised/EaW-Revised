#include "eawr/units/land_companies.hpp"

#include "eawr/core/text.hpp"

#include <array>
#include <cstddef>

namespace eawr::units {

namespace {

constexpr std::array<LandKindRoute, 5> routes{{
    {LandObjectKind::infantry, false, false, false},
    {LandObjectKind::vehicle, false, true, false},
    {LandObjectKind::hero, false, false, true},
    {LandObjectKind::structure, true, false, false},
    {LandObjectKind::other, false, false, false},
}};

} // namespace

std::string_view to_string(const LandObjectKind kind) noexcept {
    switch (kind) {
    case LandObjectKind::infantry: return "infantry";
    case LandObjectKind::vehicle: return "vehicle";
    case LandObjectKind::hero: return "hero";
    case LandObjectKind::structure: return "structure";
    case LandObjectKind::other: return "other";
    }
    return "other";
}

namespace {
constexpr std::array<std::string_view, land_collision_class_count> collision_class_names{{
    "None", "Landing Transport", "Build Pad", "Low Obstacle", "Medium Obstacle", "High Obstacle",
    "Infantry", "Bike", "Vehicle", "Large Vehicle", "Giant Vehicle",
}};
}

std::string_view to_string(const LandCollisionGroup value) noexcept {
    const auto index = static_cast<std::size_t>(value);
    return index < collision_class_names.size() ? collision_class_names[index] : collision_class_names[0];
}

std::optional<LandCollisionGroup> parse_land_collision_class(const std::string_view name) noexcept {
    for (std::size_t i = 0; i < collision_class_names.size(); ++i)
        if (core::iequals(name, collision_class_names[i])) return static_cast<LandCollisionGroup>(i);
    return std::nullopt;
}

std::span<const LandKindRoute> land_kind_routes() noexcept { return routes; }

const LandKindRoute& land_kind_route(const LandObjectKind kind) noexcept {
    for (const auto& route : routes) {
        if (route.kind == kind) return route;
    }
    return routes.back();
}



} // namespace eawr::units
