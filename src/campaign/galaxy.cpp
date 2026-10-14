#include "eawr/campaign/galaxy.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/sim/galactic/graph.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace eawr::campaign {
namespace {
std::string key(std::string text) {
    for (auto& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return text;
}
core::Result<GalaxyModel> invalid(const data::galactic::Campaign& campaign, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(sim::galactic::diagnostic_codes::invalid_setup);
    diagnostic.severity = core::Severity::error;
    diagnostic.logical_path = campaign.file;
    diagnostic.message = campaign.name + ": " + std::move(message);
    return core::Result<GalaxyModel>::failure(std::move(diagnostic));
}
} // namespace
core::Result<GalaxyModel> build_campaign_galaxy(const data::galactic::GalacticData& data,
    const units::GalacticTypeTable& types, const data::galactic::Campaign& campaign,
    const sim::galactic::PlayerId initial_owner, const sim::EntityId first_free) {
    const auto count = campaign.planets.size() + campaign.trade_routes.size();
    if (initial_owner == 0 || first_free == 0 || count >= std::numeric_limits<sim::EntityId>::max() - first_free)
        return invalid(campaign, "invalid initial owner or allocator frontier");
    const auto* maximum = data.constant("MaximumFleetMovementDistance");
    if (!maximum || !maximum->number || maximum->number->raw() < 0)
        return invalid(campaign, "missing or invalid MaximumFleetMovementDistance");
    GalaxyModel result;
    result.next_id = first_free;
    result.maximum_movement_distance = *maximum->number;
    std::map<std::string, sim::galactic::PlanetId> ids;
    for (const auto& name : campaign.planets) {
        const auto* planet = data.planet(name);
        if (data.galaxy_art_object(name) || (planet && !planet->has_planetary_behavior()))
            return invalid(campaign, "non-conquerable galaxy art object listed as a planet: " + name);
        if (!planet || !planet->galactic_position) return invalid(campaign, "missing Galactic_Position for " + name);
        const auto id = result.next_id++;
        if (!ids.emplace(key(name), id).second) return invalid(campaign, "duplicate campaign planet " + name);
        const auto& p = *planet->galactic_position;
        result.planets.push_back({id, {p.x, p.y, p.z}, initial_owner});
        result.names.push_back({id, planet->name});
    }
    for (const auto& name : campaign.trade_routes) {
        const auto* route = data.trade_route(name);
        if (!route || !route->hs_speed_factor || route->hs_speed_factor->raw() <= 0)
            return invalid(campaign, "missing or invalid HS_Speed_Factor for " + name);
        const auto a = ids.find(key(route->point_a)); const auto b = ids.find(key(route->point_b));
        if (a == ids.end() || b == ids.end()) return invalid(campaign, "route endpoint outside campaign: " + name);
        const auto id = result.next_id++;
        result.routes.push_back({id, a->second, b->second, *route->hs_speed_factor});
        result.names.push_back({id, route->name});
    }
    auto graph = sim::galactic::PlanetGraph::build(result.planets, result.routes, result.maximum_movement_distance);
    if (!graph) return invalid(campaign, graph.error().message);
    // ADR-017: GC original-type identity participates without changing the tactical type table.
    const auto data_identity = data::galactic::content_identity(data);
    const auto type_identity = units::galactic_content_identity(types);
    const std::string domain = "eawr-campaign-galaxy-content-v1";
    std::vector<std::uint8_t> bytes(domain.begin(), domain.end());
    bytes.insert(bytes.end(), data_identity.begin(), data_identity.end());
    bytes.insert(bytes.end(), type_identity.begin(), type_identity.end());
    result.content_identity = core::sha256(bytes);
    return core::Result<GalaxyModel>::success(std::move(result));
}
} // namespace eawr::campaign
