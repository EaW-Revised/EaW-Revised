#pragma once

#include "eawr/sim/galactic/scoring.hpp"
#include "eawr/campaign/conflict.hpp"
#include "eawr/sim/galactic/hero_return.hpp"
#include "eawr/sim/galactic/session.hpp"
#include "eawr/sim/tactical/session.hpp"

namespace eawr::campaign {
struct TacticalReturnState {
    std::vector<sim::galactic::HeroReturnRecord> heroes;
    std::vector<std::pair<sim::EntityId, std::uint32_t>> structure_markers;
    struct PlanetUpgrade {
        sim::galactic::PlanetId planet{};
        ReturnedUpgrade upgrade;
        friend bool operator==(const PlanetUpgrade&, const PlanetUpgrade&) = default;
    };
    std::vector<PlanetUpgrade> upgrades;
    friend bool operator==(const TacticalReturnState&, const TacticalReturnState&) = default;
};
struct TacticalReturnPlan {
    BattleResult result;
    std::vector<sim::EntityId> destroyed;
    // WGP-98: the final-blow player of each destroyed identity, ascending id.
    std::vector<sim::galactic::BattleKiller> killers;
    std::vector<sim::galactic::FleetId> retreat_fleets;
};
struct MidBattleObject {
    sim::tactical::UnitState unit;
    sim::galactic::TypeId original_type{};
    bool reserve{};
};
using MidBattleReserveGather = std::function<core::Result<std::vector<ConflictForce>>() >;
// WAR-65: live tactical objects and currently unspent reserves, never the galaxy's stale roster.
[[nodiscard]] core::Result<std::vector<MidBattleObject>> collect_midbattle_objects(
    const ConflictRecord&, std::span<const sim::tactical::UnitState>, const MidBattleReserveGather&);
// WGT-37: find both spatial identities and recursively contained rider identities.
[[nodiscard]] bool tactical_force_alive(const ConflictForce&, std::span<const sim::tactical::UnitState>);
[[nodiscard]] core::Result<TacticalReturnPlan> plan_tactical_return(const ConflictRecord&,
    BattleResult, const sim::galactic::GalacticSnapshot&);
// The caller applies its political-control result first. This commits ordinary losses,
// trusted retreat movement and typed ledgers once at that same parent barrier.
[[nodiscard]] core::Result<void> apply_tactical_return(sim::galactic::GalacticSession&,
    const ConflictRecord&, const TacticalReturnPlan&, TacticalReturnState&, const sim::PartitionExecutor&);
[[nodiscard]] std::vector<InvasionOffer> tactical_invasion_offers(const ConflictRecord&,
    const BattleResult&, const sim::galactic::GalacticSnapshot&);
[[nodiscard]] std::vector<std::uint8_t> encode_tactical_return_state(const TacticalReturnState&);
[[nodiscard]] core::Result<TacticalReturnState> decode_tactical_return_state(std::span<const std::uint8_t>);
} // namespace eawr::campaign
