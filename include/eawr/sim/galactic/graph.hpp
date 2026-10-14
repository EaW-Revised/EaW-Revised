#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <optional>
#include <span>
#include <vector>

namespace eawr::sim::galactic {

struct GraphEdge {
    PlanetId to{};
    RouteId route{}; // zero for an ordinary proximity connection
    math::Fixed cost;
    friend bool operator==(const GraphEdge&, const GraphEdge&) = default;
};
struct GraphNode {
    PlanetId id{};
    math::Vec3 position;
    std::vector<GraphEdge> edges; // ascending destination ID
    friend bool operator==(const GraphNode&, const GraphNode&) = default;
};
struct GraphWork {
    std::uint64_t pairs_tested{};
    std::uint64_t directed_edges{};
    friend bool operator==(const GraphWork&, const GraphWork&) = default;
};

// Immutable, derived from the authoritative planet/route rows at the setup boundary (WGM-16/17).
// A bounded quadratic build never runs in the tick. No data-loading or presentation dependencies.
class PlanetGraph final {
public:
    [[nodiscard]] static core::Result<PlanetGraph> build(std::span<const Planet> planets,
        std::span<const Route> routes, math::Fixed maximum_distance);
    [[nodiscard]] std::span<const GraphNode> nodes() const noexcept { return nodes_; }
    [[nodiscard]] const GraphNode* node(PlanetId id) const noexcept;
    [[nodiscard]] std::optional<math::Fixed> largest_route_factor() const noexcept { return largest_factor_; }
    [[nodiscard]] GraphWork work() const noexcept { return work_; }
private:
    std::vector<GraphNode> nodes_;
    std::optional<math::Fixed> largest_factor_{};
    GraphWork work_;
};

enum class PathRelation { allied, neutral, hostile };

// Observer-specific, copied inputs supplied by the intelligence/control/AI owners. Missing AI
// classification is represented separately from failed enemy-force perception (WGM-19/86).
struct PathPlanet {
    PlanetId id{};
    bool revealed{true};
    bool owner_visible{true};
    PathRelation relation{PathRelation::allied};
    bool corrupted{};
    bool special_space_map{};
    bool ai_usable{true};
    bool ai_classification_succeeded{true}; // failed classification accepts allegiance, WGM-19
    std::optional<PathRelation> ai_relation{};
    std::optional<PathRelation> starbase_relation{};
    bool fleet_presence_visible{};
    bool nonallied_fleet{};
    std::optional<bool> ai_enemy_space_force{};
};
struct PathOptions {
    bool ai_player{};
    bool multiplayer{};
    bool benefits_from_corruption{};
    bool allow_neutral{};
    bool allow_hostile{};
    bool allow_defended{};
    bool allow_bad_final{};
    bool allow_bad_if_no_force{true};
    bool ai_restrictions{true};
};
struct PathWork {
    std::uint64_t nodes_expanded{};
    std::uint64_t edges_tested{};
    friend bool operator==(const PathWork&, const PathWork&) = default;
};
struct PlanetPath {
    std::vector<PlanetId> planets; // empty if unreachable
    math::Fixed cost;
    PathWork work;
};

// WGM-18..22/86: pure search over a complete observer table (exactly one row per graph node).
// Total priority is (accumulated Q24 cost, ascending planet ID). Equal costs never replace a
// predecessor. This explicit remake ordering retains the WGM-91 enumeration-parity boundary.
[[nodiscard]] core::Result<PlanetPath> find_planet_path(const PlanetGraph& graph,
    PlanetId source, PlanetId destination, std::span<const PathPlanet> access, const PathOptions& options = {});

} // namespace eawr::sim::galactic
