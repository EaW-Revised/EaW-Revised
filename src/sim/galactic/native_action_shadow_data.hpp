#pragma once

#include "native_income_projection.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace eawr::sim::galactic {
namespace session_detail { struct NativePopulationSchema; }
struct GalacticSession::NativeActionShadow::Data {
    struct Location {
        Planet planet;
        ControlPlanet presence;
        PlanetProduction production;
        std::shared_ptr<const std::vector<Fleet>> orbit;
        std::shared_ptr<const std::vector<MovementContents>> contents;
    };
    using Force = NativeActionShadow::Force;
    using FleetRequest = NativeActionShadow::FleetRequest;
    using ControlRequest = NativeActionShadow::ControlRequest;
    using CashRequest = NativeActionShadow::CashRequest;
    using ForceRequest = NativeActionShadow::ForceRequest;
    using FinishRequest = NativeActionShadow::FinishRequest;
    using ProductionRequest = NativeActionShadow::ProductionRequest;
    using InvasionRequest = NativeActionShadow::InvasionRequest;
    using JournalRequest = NativeActionShadow::JournalRequest;
    using OrbitRecheckRequest = NativeActionShadow::OrbitRecheckRequest;
    std::map<FleetId, std::optional<Fleet>> fleet_rows;
    std::map<EntityId, std::optional<CensusObject>> actor_rows;
    std::map<EntityId, std::optional<DetachedUnit>> detached_rows;
    std::map<FleetId, bool> fleet_blockers;
    std::map<PlanetId, ControlPlanet> control_rows;
    std::map<PlanetId, PlanetProduction> production_rows;
    std::vector<FleetId> created_blockers;
    bool immediate_batch{}, continuation{}, prelude{}, tail{}, last_published{}, last_admission_ran{};
    NativeMovementListener movement_listener;
    std::map<std::uint64_t, std::optional<NativeInvasion>> invasion_rows;
    std::vector<std::uint64_t> new_invasion_queue;
    std::size_t invasion_records{}, invasion_transports{};
    std::uint64_t next_invasion{1};
    bool invasions_enabled{}, suppress_cash_request{};
    using ObserverPlanet = decltype(ObserverSetup{}.planets)::value_type;
    std::map<PlanetId, ObserverPlanet> observer_rows;
    bool population_stale{};
    std::vector<IntentRecord> action_requests;
    IntentSink intent_sink;
    template<class Request> void record_request(Request request) {
        const auto first = action_requests.empty() ? first_id : action_requests.back().next_id;
        const auto first_request = action_requests.empty() ? first_invasion : action_requests.back().next_invasion;
        action_requests.push_back({Intent{std::move(request)}, first, next_id, first_request, next_invasion});
        // Immediate Lua clones retain their own records. An accepted native
        // publication forwards them exactly once through publish_native_batch.
        if (intent_sink && !immediate_batch) intent_sink(player, action_requests.back());
    }
    std::shared_ptr<const session_detail::NativePopulationSchema> population_schema;
    std::shared_ptr<const IncomeSetup> income_schema;
    std::shared_ptr<const FiscalSetup> fiscal_schema;
    std::shared_ptr<const std::vector<math::Fixed>> base_income;
    std::shared_ptr<const session_detail::NativeIncomeEdges> income_edges;
    std::shared_ptr<const std::vector<ProductionPlayer>> production_projection;
    std::shared_ptr<const std::vector<PopulationTotal>> population_projection;
    std::shared_ptr<const PlanetGraph> graph;
    std::shared_ptr<const MovementSetup> movement;
    std::uint64_t observer_generation{};
    mutable std::shared_ptr<const GalacticSnapshot> publication;
    [[nodiscard]] std::shared_ptr<const GalacticSnapshot> view() const;
    [[nodiscard]] const Fleet* fleet(FleetId) const;
    [[nodiscard]] const CensusObject* actor(EntityId) const;
    [[nodiscard]] const DetachedUnit* detached(EntityId) const;
    [[nodiscard]] const MovementContents* contents(EntityId) const;
    class FleetOwner;
    class ControlOwner;
    const void* owner{};
    std::shared_ptr<const GalacticSnapshot> base;
    std::shared_ptr<const Data> parent;
    std::shared_ptr<const Data> read_parent;
    std::size_t read_depth{}, read_checkpoints{};
    void inherit_reads(const std::shared_ptr<const Data>&);
    mutable std::shared_ptr<const EconomyInputGeneration> last_generation;
    PlayerId player{};
    EntityId first_id{}, next_id{}, range_end{};
    std::uint64_t first_invasion{1}, invasion_range_end{};
    bool independent_player{};
    std::size_t live_objects{}, max_requests{};
    bool can_create{};
    std::map<PlayerId, math::Fixed> accounts;
    std::map<PlanetId, std::shared_ptr<const Location>> locations;
    std::vector<std::shared_ptr<const Force>> forces;
    std::vector<NativeActorChange> actor_changes;
    std::shared_ptr<const VictoryState> victory;
    std::vector<GalacticEvent> events;
    Work work;
};
} // namespace eawr::sim::galactic
