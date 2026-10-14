#include "eawr/sim/galactic/graph.hpp"

#include "encoding.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>
#include <utility>

namespace eawr::sim::galactic {
namespace {
template <class T> core::Result<T> invalid(std::string message) {
    return core::Result<T>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, std::move(message)));
}
core::Result<math::Fixed> distance(const math::Vec3& a, const math::Vec3& b) {
    const auto x = math::subtract(a.x, b.x);
    const auto y = math::subtract(a.y, b.y);
    const auto z = math::subtract(a.z, b.z);
    if (!x || !y || !z) return invalid<math::Fixed>("graph position difference overflows");
    return math::length(math::Vec3{x.value(), y.value(), z.value()});
}
bool defended(const PathPlanet& planet, const PathOptions& options) {
    // WGM-86: a starbase supersedes attached fleets; failed AI perception is conservative.
    if (planet.starbase_relation) return *planet.starbase_relation != PathRelation::allied;
    if (options.ai_player) return planet.ai_enemy_space_force.value_or(true);
    return planet.fleet_presence_visible && planet.nonallied_fleet;
}
bool bypass(const PathPlanet& planet, const PathOptions& options) {
    return options.benefits_from_corruption && planet.corrupted;
}
bool passes_allegiance(const PathPlanet& planet, const PathOptions& options) {
    // WGM-19: actual allies and corruption pass before observer/AI classification.
    if (planet.relation == PathRelation::allied || bypass(planet, options)) return true;
    if (options.ai_player && !planet.ai_classification_succeeded) return true;
    const auto relation = options.ai_player ? planet.ai_relation.value_or(PathRelation::neutral)
        : (planet.owner_visible ? planet.relation : PathRelation::neutral);
    return relation == PathRelation::neutral ? options.allow_neutral : options.allow_hostile;
}
bool expandable(const PathPlanet& planet, const PathOptions& options) {
    // WGM-20/22/86: current-node validation also applies to the source. No reveal/map gate here.
    if (options.ai_player && options.ai_restrictions && !planet.ai_usable) return false;
    if (passes_allegiance(planet, options))
        return options.allow_defended || bypass(planet, options) || !defended(planet, options);
    return options.allow_bad_if_no_force && (!defended(planet, options) || bypass(planet, options));
}
std::optional<math::Fixed> admission(const PathPlanet& from, const PathPlanet& to,
    const PlanetId destination, const PathOptions& options) {
    // WGM-18: reveal and AI endpoint usability precede the final-endpoint bypass.
    if (!options.ai_player && !options.multiplayer && !to.revealed) return std::nullopt;
    if (options.ai_player && options.ai_restrictions && (!from.ai_usable || !to.ai_usable)) return std::nullopt;
    if (to.id == destination && options.allow_bad_final) return math::Fixed{};
    if (to.special_space_map) return std::nullopt; // WGM-20
    const bool allegiance = passes_allegiance(to, options);
    const bool has_defense = defended(to, options);
    if (!allegiance && !options.allow_bad_if_no_force) return std::nullopt;
    if (!has_defense || (allegiance && options.allow_defended)) return math::Fixed{};
    // WGM-21: pay the preference penalty only where corruption is needed for admission.
    if (bypass(to, options))
        return math::Fixed::from_raw(100 * math::Fixed::scale);
    return std::nullopt;
}
} // namespace

core::Result<PlanetGraph> PlanetGraph::build(const std::span<const Planet> planets,
    const std::span<const Route> routes, const math::Fixed maximum_distance) {
    // A hostile replay must not make a quadratic allocation over the million-object setup cap.
    constexpr std::size_t max_graph_nodes = 4096;
    if (planets.size() > max_graph_nodes || routes.size() > 500'000 || maximum_distance.raw() < 0)
        return invalid<PlanetGraph>("graph needs at most 4096 planets and a nonnegative movement distance");
    PlanetGraph graph;
    std::set<EntityId> ids;
    for (const auto& planet : planets) graph.nodes_.push_back({planet.id, planet.position, {}});
    std::sort(graph.nodes_.begin(), graph.nodes_.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    for (std::size_t i = 0; i < graph.nodes_.size(); ++i) {
        if (graph.nodes_[i].id == 0 || (i != 0 && graph.nodes_[i - 1].id == graph.nodes_[i].id))
            return invalid<PlanetGraph>("graph planet IDs must be nonzero and distinct");
        ids.insert(graph.nodes_[i].id);
    }
    std::map<std::pair<PlanetId, PlanetId>, const Route*> lanes;
    for (const auto& route : routes) {
        if (route.id == 0 || !ids.insert(route.id).second || !graph.node(route.from) || !graph.node(route.to)
            || route.from == route.to || route.hs_speed_factor.raw() <= 0)
            return invalid<PlanetGraph>("graph route needs distinct endpoints and a positive speed factor");
        if (lanes.empty()) graph.largest_factor_ = route.hs_speed_factor;
        const auto endpoints = std::minmax(route.from, route.to);
        if (!lanes.emplace(std::pair{endpoints.first, endpoints.second}, &route).second)
            return invalid<PlanetGraph>("graph duplicate route endpoints are unverified");
        graph.largest_factor_ = std::max(*graph.largest_factor_, route.hs_speed_factor);
    }
    // Setup-only serial pair enumeration, outside the tick; ascending IDs and edges (WGM-16).
    for (std::size_t i = 0; i < graph.nodes_.size(); ++i) {
        for (std::size_t j = i + 1; j < graph.nodes_.size(); ++j) {
            ++graph.work_.pairs_tested;
            auto length = distance(graph.nodes_[i].position, graph.nodes_[j].position);
            if (!length) return core::Result<PlanetGraph>::failure(length.error());
            const auto lane = lanes.find({graph.nodes_[i].id, graph.nodes_[j].id});
            if (lane == lanes.end() && length.value() > maximum_distance) continue;
            auto cost = lane == lanes.end() ? length : math::divide(length.value(), lane->second->hs_speed_factor);
            if (!cost) return core::Result<PlanetGraph>::failure(cost.error());
            const auto route = lane == lanes.end() ? RouteId{} : lane->second->id;
            if (graph.work_.directed_edges >= 1'000'000)
                return invalid<PlanetGraph>("graph exceeds one million directed edges");
            graph.nodes_[i].edges.push_back({graph.nodes_[j].id, route, cost.value()});
            graph.nodes_[j].edges.push_back({graph.nodes_[i].id, route, cost.value()});
            graph.work_.directed_edges += 2;
        }
    }
    return core::Result<PlanetGraph>::success(std::move(graph));
}
const GraphNode* PlanetGraph::node(const PlanetId id) const noexcept {
    const auto found = std::lower_bound(nodes_.begin(), nodes_.end(), id, [](const auto& row, const auto value) { return row.id < value; });
    return found != nodes_.end() && found->id == id ? &*found : nullptr;
}

core::Result<PlanetPath> find_planet_path(const PlanetGraph& graph, const PlanetId source,
    const PlanetId destination, const std::span<const PathPlanet> access, const PathOptions& options) {
    if (!graph.node(source) || !graph.node(destination)) return invalid<PlanetPath>("path endpoints must be graph planets");
    if (source == destination) return core::Result<PlanetPath>::success({{source}, {}, {}}); // WGM-22
    std::map<PlanetId, const PathPlanet*> observations;
    for (const auto& row : access) {
        if (!graph.node(row.id) || !observations.emplace(row.id, &row).second)
            return invalid<PlanetPath>("path observation IDs must be distinct graph planets");
    }
    if (observations.size() != graph.nodes().size()) return invalid<PlanetPath>("path needs a complete observer table");
    struct Label { math::Fixed cost; PlanetId previous{}; };
    std::map<PlanetId, Label> labels;
    using Entry = std::pair<math::Fixed, PlanetId>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open;
    labels.emplace(source, Label{}); open.emplace(math::Fixed{}, source);
    PlanetPath result;
    while (!open.empty()) {
        const auto [cost, current] = open.top(); open.pop();
        if (labels.at(current).cost != cost) continue;
        if (current == destination) {
            result.cost = cost;
            for (auto id = destination; id != 0; id = labels.at(id).previous) result.planets.push_back(id);
            std::reverse(result.planets.begin(), result.planets.end());
            break;
        }
        ++result.work.nodes_expanded;
        if (!expandable(*observations.at(current), options)) continue;
        for (const auto& edge : graph.node(current)->edges) {
            ++result.work.edges_tested;
            auto penalty = admission(*observations.at(current), *observations.at(edge.to), destination, options);
            if (!penalty) continue;
            auto next = math::add(cost, edge.cost);
            if (next) next = math::add(next.value(), *penalty);
            if (!next) return invalid<PlanetPath>("path cost overflows");
            const auto existing = labels.find(edge.to);
            if (existing != labels.end() && existing->second.cost <= next.value()) continue;
            labels.insert_or_assign(edge.to, Label{next.value(), current});
            open.emplace(next.value(), edge.to);
        }
    }
    return core::Result<PlanetPath>::success(std::move(result));
}
} // namespace eawr::sim::galactic
