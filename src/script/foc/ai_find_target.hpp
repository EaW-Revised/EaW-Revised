#pragma once

#include "ai_equations.hpp"
#include "eawr/sim/tactical/types.hpp"

namespace eawr::script::foc::ai {
class Engine;
struct Target;

enum class FindTargetManager { current, galactic };
enum class FindTargetService { galactic, land };

// FT-05/08/09/15: copied execution-route data, never an ownership-filtered
// movement path. The provider validates planet/zone source membership.
struct FindTargetPath {
    FindTargetService service{FindTargetService::galactic};
    bool source_valid{true};
    bool empty{};
    bool same_target{};
    Real distance{};
    Real threat{};
    Real prefix_threat{}; // threat before the final hop (Single_Hop_Disconnected)
};

// Frozen for the partitioned Lua service. S4 supplies the campaign provider.
class FindTargetQueries {
public:
    virtual ~FindTargetQueries() = default;
    // FT-07: null means this selected manager is absent, not an empty target list.
    [[nodiscard]] virtual const Engine* manager(FindTargetManager) const = 0;
    // FT-05: galactic lookup resolves direct planet parent, fleet parent's
    // planet, or the object itself when a planet, then returns its goal target.
    // A live object with no lookup result returns null (no source filter).
    [[nodiscard]] virtual const Target* source_target(const Engine&, sim::EntityId object) const = 0;
    // FT-12: nullopt means the candidate has no reachability service (space).
    // An existing service with no route returns a row with empty=true.
    [[nodiscard]] virtual std::optional<FindTargetPath> execution_path(const Engine&,
        sim::tactical::PlayerId, const Target& source, const Target& candidate) const = 0;
};
} // namespace eawr::script::foc::ai
