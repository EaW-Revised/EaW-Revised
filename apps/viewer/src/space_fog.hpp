#pragma once

#include "eawr/scene/scene.hpp"
#include "eawr/sim/fog.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Opt-in synthetic space fog admission (P1-07 #28, joining #32). A space map
// run with fog composes only the placements whose catalog XML element type the
// caller names with --eawr-space-fog-admit (for example SpaceUnit). Nothing is
// admitted by default and no type is inferred: the rule is harness policy for
// fog-stub-v1 evidence, not a classification of real space placements, which
// #32 owns and must approve. Engine-independent so it can be contract-tested.
namespace eawr::presentation::godot_backend::space_fog {

inline constexpr std::size_t max_admitted_types = 16;

// Empty when the declared types are usable: 1..16 unique names of ASCII
// letters, digits and underscores. Otherwise the reason they are not.
[[nodiscard]] std::string validate_admission(std::span<const std::string> types);

enum class Decision : std::uint8_t {
    admitted,          // declared type, drawable, every surface a legacy selector
    admitted_blocked,  // declared type that cannot be composed; fails a fog run
    not_admitted,      // catalogued type the caller did not declare
    uncatalogued,      // no catalog winner, so no element type to compare
};
[[nodiscard]] std::string_view to_string(Decision decision) noexcept;

struct PlacementDecision final {
    std::uint64_t scene_ordinal{};
    std::string object_id;
    // The winning definition's XML element name; empty when uncatalogued.
    std::string type_name;
    Decision decision{Decision::uncatalogued};
    // Why an admitted_blocked placement cannot be composed, in surface order
    // after the scene issues. Empty for the other decisions.
    std::vector<std::string> reasons;
};

// One decision per scene placement, in scene order. `type_of` returns the
// catalog winner's XML element name for an object id, or nullopt.
[[nodiscard]] std::vector<PlacementDecision> classify(const scene::Scene& scene,
    const std::function<std::optional<std::string>(std::string_view object_id)>& type_of,
    std::span<const std::string> admitted_types);

// Source-basis XY footprint of a posed unit.
struct SourceBounds final {
    double min_x{};
    double max_x{};
    double min_y{};
    double max_y{};
};

// True when any grid cell the footprint's half-open extent [min, max) covers
// is nonzero, matching the cells' own half-open test: a footprint whose max
// lands exactly on a cell edge does not cover the cell beyond it. A degenerate
// extent (min == max) is the single point min. Outside the grid samples dark,
// so a footprint with no in-grid cell, or a missing grid, cannot reveal
// anything.
[[nodiscard]] bool can_reveal(const sim::fog::FogGrid* grid, const SourceBounds& bounds) noexcept;

} // namespace eawr::presentation::godot_backend::space_fog
