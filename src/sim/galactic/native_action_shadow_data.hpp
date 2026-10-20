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
    struct Force { NativeProductionRequest request; NativeForcePlacement placement; };
    struct FleetRequest { PlayerId player{}; CommandPayload payload; bool pending{}; NativeFleetReceipt receipt; };
    struct ControlRequest {
        enum class Kind { launch, fleet_land, actor_land } kind{};
        PlayerId player{};
        EntityId object{}, target{};
        bool pending{};
        std::optional<std::uint32_t> live_landed;
        NativeFleetReceipt receipt;
        bool linked{};
    };
    struct CashRequest { PlayerId player{}; math::Fixed delta, multiplier, actual; };
    struct ForceRequest { std::shared_ptr<const Force> force; std::vector<EntityId> actors; };
    struct FinishRequest { EntityId build{}; bool salvage{}; math::Fixed multiplier; };
    struct ProductionRequest { NativeProductionRequest request; NativeProductionReceipt receipt; };
    struct InvasionRequest { bool retire{}; PlayerId player{}; FleetId fleet{}; std::vector<EntityId> transports;
        bool pending{}; NativeInvasionReceipt receipt; std::uint64_t number{}; };
    struct JournalRequest { NativeEffectKind kind{}; std::vector<std::uint8_t> payload; NativeEffectPhase phase{}; };
    struct OrbitRecheckRequest { PlayerId player{}; FleetId fleet{}; };
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
    std::vector<std::variant<FleetRequest, ControlRequest, CashRequest, ForceRequest, ProductionRequest, FinishRequest, InvasionRequest, JournalRequest, OrbitRecheckRequest>> action_requests;
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
    EntityId first_id{}, next_id{};
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
