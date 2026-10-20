#pragma once

#include "galactic_ai_world.hpp"
#include "eawr/sim/world.hpp"

#include <array>
#include <functional>
#include <set>

namespace eawr::script::foc::galactic_ai {

// WGA-27/77: execution owns a separate mode-frame deadline. The qualified
// path producer supplies decoded facts; movement paths cannot stand in for it.
struct ExecutionPathMatrix {
    gc::PlayerId player{};
    std::vector<gc::PlanetId> nodes;
    std::vector<std::uint32_t> costs;
    std::vector<std::size_t> previous;
};
// Derived memo of one player's last build; never saved, hashed or published. When the next build of the same
// player stages the identical node order and packed edge matrix, its rows and matrix are exactly these.
// Pairwise packed distances of one build, valid for as long as the planet facts, trade lanes and distance limit
// below are unchanged. Force never enters it, so fleets moving leave it intact.
struct ExecutionGeometry {
    struct Fact {
        gc::PlanetId planet{};
        sim::math::Vec3 position;
        std::uint64_t layer_z_adjust{};
        bool usable{};
        friend bool operator==(const Fact&, const Fact&) noexcept = default;
    };
    static constexpr std::int32_t no_edge = -1, out_of_range = -2; // else the packed distance, scaled by ten
    std::vector<Fact> facts;
    std::vector<std::array<std::uint64_t, 3>> trades; // from, to, speed bits; ascending
    std::uint64_t maximum_distance{};
    std::vector<std::int32_t> distance; // n * n
};
struct ExecutionBuildMemo {
    std::shared_ptr<const ExecutionGeometry> geometry;
    gc::PlayerId player{};
    std::vector<gc::PlanetId> nodes;
    std::vector<std::uint32_t> edges;
    std::vector<ai::GalacticExecutionRoute> rows; // build order, before the publication sort
    std::shared_ptr<const ExecutionPathMatrix> paths;
};
struct ExecutionRoutes {
    std::uint64_t next_refresh{}, revision{};
    bool available{};
    std::vector<ai::GalacticExecutionRoute> rows;
    std::shared_ptr<const ExecutionPathMatrix> paths;
    std::shared_ptr<const ExecutionBuildMemo> memo;
};
struct ExecutionPlanet {
    gc::PlanetId planet{};
    sim::math::Vec3 position;
    Number layer_z_adjust{}, enemy_space_power{}, nonallied_starbase_power{};
    bool usable{};
};
struct ExecutionRouteInput {
    gc::PlayerId player{};
    std::vector<ExecutionPlanet> planets; // original object-query order, retained at initialization
    std::vector<gc::Route> trade_routes;
    Number maximum_distance{};
};
struct ExecutionRouteBuild {
    std::vector<ai::GalacticExecutionRoute> rows;
    std::shared_ptr<const ExecutionPathMatrix> paths;
    std::shared_ptr<const ExecutionBuildMemo> memo;
    // Logical budget of the all-pairs formulation (n^2 edges, n^3 relaxations), whichever work was skipped.
    std::uint64_t edges_tested{}, relaxations_tested{};
    bool reused{}; // rows and matrix came from the previous build's memo
};
// WGA-126..140: packed execution cost is separate from the movement graph.
// Unqualified numeric overflow refuses this generation (project policy).
// `previous` is the same player's memo from its last build; an identical staged edge matrix reuses it.
[[nodiscard]] core::Result<ExecutionRouteBuild> build_execution_routes(const ExecutionRouteInput&,
    const sim::PartitionExecutor&, const std::shared_ptr<const ExecutionBuildMemo>& previous = {});
using RouteProducer = std::function<core::Result<std::optional<std::vector<ai::GalacticExecutionRoute>>>(
    const sim::PartitionExecutor&)>;
using ExecutionMatrixProducer = std::function<core::Result<std::optional<ExecutionRouteBuild>>(const sim::PartitionExecutor&)>;
struct ExecutionPath { std::vector<gc::PlanetId> planets; Number threat{}; };
// WGA-386/388: read the published generation, never refresh from movement.
[[nodiscard]] core::Result<ExecutionPath> cached_execution_path(const ExecutionRoutes&, gc::PlayerId,
    gc::PlanetId source, gc::PlanetId destination);
[[nodiscard]] core::Result<bool> publish_execution_routes(ExecutionRoutes&, std::uint64_t mode_frame,
    bool execution_due, const sim::PartitionExecutor&, const ExecutionMatrixProducer&);
[[nodiscard]] core::Result<void> initialize_execution_matrix(ExecutionRoutes&, std::uint64_t mode_frame,
    std::uint32_t nonhuman_count, std::uint32_t player_index, const sim::PartitionExecutor&, const ExecutionMatrixProducer&);
[[nodiscard]] core::Result<bool> refresh_execution_routes(ExecutionRoutes&, std::uint64_t mode_frame,
    bool execution_due, const sim::PartitionExecutor&, const RouteProducer&);
[[nodiscard]] ai::GalacticConnectivity execution_connectivity(const ExecutionRoutes&);
[[nodiscard]] core::Result<void> initialize_execution_routes(ExecutionRoutes&, std::uint64_t mode_frame,
    std::uint32_t nonhuman_count, std::uint32_t player_index, const sim::PartitionExecutor&, const RouteProducer&);
[[nodiscard]] core::Result<void> rebuild_execution_routes_after_load(ExecutionRoutes&,
    bool finder_present, const sim::PartitionExecutor&, const RouteProducer&);

// WGA-28/61: semantic object kinds, supplied by validated original-type data.
enum class FreestoreKind { unit, planet, member, fleet, ground_structure, space_structure };
struct FreestoreType {
    gc::TypeId type{};
    FreestoreKind kind{FreestoreKind::unit};
    bool transport{};
};
struct FreestoreObject {
    sim::EntityId id{};
    gc::TypeId type{};
    gc::PlayerId owner{};
    gc::PlanetId planet{};
    FreestoreKind kind{FreestoreKind::unit};
    bool transport{}, transit{}, directly_parented_to_planet{};
    // WGA-30: nullopt is unavailable safety input, never a known-safe value.
    std::optional<bool> safe;
    bool operator==(const FreestoreObject&) const = default;
};
struct FreestoreWork { std::uint64_t objects{}, safety_rows{}; };
struct FreestoreSurface {
    gc::PlanetId planet{};
    std::uint32_t landed{}, maximum{};
    bool accessible{};
};
struct FreestorePool {
    std::vector<FreestoreObject> objects; // ascending persistent identity
    std::map<sim::EntityId, std::uint64_t> reservations; // potential plan identity
    FreestoreWork work;
    bool qualified{}; // WGA-300..315 retained owner, never presence copying
    std::set<gc::PlayerId> qualified_players;
    std::vector<FreestoreSurface> surfaces; // WGA-410: cached live transport counts
    std::vector<sim::EntityId> removed; // ascending: lost, dead or deleting actors (WGA-436 null wrapper)
    // Derived player publications. The flat rows above retain the ordered Lua/object facade.
    std::map<gc::PlayerId, std::shared_ptr<const FreestorePool>> players{};
};
[[nodiscard]] bool freestore_eligible(const FreestoreObject&, gc::PlayerId) noexcept;
enum class FreestoreAssignment { failed, waiting, assigned };
[[nodiscard]] FreestoreAssignment freestore_assignment(const FreestorePool&, sim::EntityId,
    gc::PlayerId, std::uint64_t potential_plan) noexcept;
// Copies world presence, containment and transit. Nested roster members are
// excluded; surface/orbit containers remain whole units. Safety uses live
// control relationships and the authored maximum, not a fixed ten.
[[nodiscard]] core::Result<FreestorePool> copy_freestore(const gc::GalacticSnapshot&,
    std::span<const FreestoreType>, const sim::PartitionExecutor&);
// WGA-30: derived surface safety only; retained membership/flags remain intact.
[[nodiscard]] core::Result<void> prepare_freestore_safety(FreestorePool&, const gc::GalacticSnapshot&,
    std::span<const FreestoreType>, const sim::PartitionExecutor&);

struct FreestoreAttachment { gc::PlayerId player{}; std::uint64_t instance{}; };
class QualifiedFreestore;
inline constexpr auto handle_galactic_freestore = ai::handle_freestore;
// WGA-439: authored planet-type restriction lists read by Can_Land_On_Planet.
struct FreestorePlanetRestriction {
    std::uint64_t categories{};
    std::vector<gc::TypeId> types;
};
struct FreestoreInput {
    // Replace only at the ordered barrier before partitioned Lua service.
    std::shared_ptr<const FreestorePool> pool;
    std::map<std::string, gc::TypeId, std::less<>> types_by_name; // upper-case original names
    // Immutable authored content for Can_Land_On_Planet (WGA-439); never pool state.
    std::map<gc::PlanetId, FreestorePlanetRestriction> planet_restrictions;
    std::map<gc::TypeId, std::uint64_t> type_categories; // original type category masks
};
inline constexpr std::string_view verb_freestore_move = "galactic.freestore_move";
inline constexpr std::string_view freestore_move_reader_seam =
    "R5 freestore movement reader unbound: actor validity/current representation (#2745/#2735); "
    "source-planet AI usability (B usability provider, WGA-127/270); "
    "grandparent containment (R2 current parent reader); cached execution route/safety and live next-orbit occupant (R2/B, WGA-385..389)";
// WGA-436/437/439: pool-fed GameObject callbacks of the stock redistribution helpers (Is_Transport,
// Get_Planet_Location, Can_Land_On_Planet). register_freestore_bindings includes them.
[[nodiscard]] core::Result<void> register_freestore_object_methods(authoritative::ScriptScheduler&,
    std::shared_ptr<const FreestoreInput>, bool unqualified_fixture = false);
// WGA-438: Get_Is_Planet_AI_Usable answers through the native owner's immediate query.
[[nodiscard]] core::Result<void> register_planet_usability_method(authoritative::ScriptScheduler&);
// Unqualified presence is an explicitly opted-in contract fixture only.
[[nodiscard]] core::Result<void> register_freestore_bindings(authoritative::ScriptScheduler&,
    std::shared_ptr<const FreestoreInput>, bool unqualified_fixture = false);
// Fresh attachment definitions precede handle/stamp assignments because stock
// Common_Base_Definitions clears them. Restored Lua uses its restored thread.
[[nodiscard]] core::Result<void> attach_freestore(authoritative::ScriptScheduler&,
    FreestoreAttachment, bool fresh);
[[nodiscard]] core::Result<void> service_freestore(authoritative::ScriptScheduler&,
    FreestoreAttachment, const FreestorePool&, std::uint64_t mode_frame, QualifiedFreestore* = nullptr,
    bool unqualified_fixture = false);

} // namespace eawr::script::foc::galactic_ai
