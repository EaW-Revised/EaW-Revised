#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/types.hpp"
#include "eawr/sim/tactical/replay.hpp"

#include <optional>
#include <string>
#include <vector>
#include <span>

namespace eawr::sim { class PartitionExecutor; }

namespace eawr::campaign {

// Value records for the boundary in galactic architecture section 3. No health crosses it
// (WGT-24); roster selection, map placement and reconciliation remain their rule owners' work.
enum class BattleLayer { space, land };
enum class ForceRole { deployed, reinforcement, escort, rider, retained, orbital };
struct ConflictForce {
    sim::EntityId id{};
    sim::galactic::TypeId type{};
    sim::galactic::PlayerId owner{};
    sim::EntityId parent{};
    ForceRole role{ForceRole::deployed};
    bool destroy_after_tactical{}, hero{};
    sim::tactical::TypeId tactical_type{}; // zero retains the original type (ADR-017)
    sim::EntityId carrier{}; // spatial carrier for a resolved contained rider
    sim::tactical::TypeId purchase_type{}; // zero retains type; ADR-017 original tactical purchase identity
    friend bool operator==(const ConflictForce&, const ConflictForce&) = default;
};
struct BattleSeat {
    sim::galactic::SeatId seat{};
    sim::tactical::PlayerId player{}; // zero is a read-only observer
    friend bool operator==(const BattleSeat&, const BattleSeat&) = default;
};
struct ReturnedUpgrade {
    sim::tactical::TypeId type{};
    sim::EntityId holder{};
    sim::galactic::PlayerId owner{};
    friend bool operator==(const ReturnedUpgrade&, const ReturnedUpgrade&) = default;
};
struct ConflictRecord {
    sim::EntityId id{};
    BattleLayer layer{BattleLayer::space};
    sim::galactic::PlanetId planet{};
    sim::galactic::PlayerId invader{}, defender{};
    bool raid{}, corruption_mission{}, tutorial{}, ai_tactical_credits{};
    std::vector<ConflictForce> forces; // ascending persistent ID
    std::vector<BattleSeat> seats; // ascending seat ID; effective conflict-scoped assignment
    sim::EntityId station{}, superweapon{};
    sim::galactic::TypeId station_type{};
    std::uint32_t station_level{}, terrain_index{};
    std::string map_identity, story_plot;
    std::vector<std::uint8_t> linked_mission_options; // validated by the story owner; WST-U09 gated
    std::vector<std::pair<sim::EntityId, std::uint32_t>> structure_markers; // WGT-12 remembered map record
    std::vector<ReturnedUpgrade> upgrades; // retained planet upgrades supplied to entry preparation
    sim::tactical::TacticalSetup tactical; // resolved placement and victory inputs belong to the entry builder
    std::uint64_t corruption_request{}; // live host request; zero for ordinary conflict
    friend bool operator==(const ConflictRecord&, const ConflictRecord&) = default;
};

enum class ForceDisposition { survived, destroyed, retreated, destroy_after_tactical };
struct ForceResult {
    sim::EntityId id{};
    ForceDisposition disposition{ForceDisposition::survived};
    sim::galactic::PlayerId final_blow_player{};
    bool hero_defeated{};
    friend bool operator==(const ForceResult&, const ForceResult&) = default;
};
struct ReturnedCredits {
    sim::galactic::PlayerId player{};
    sim::math::Fixed credits{};
    friend bool operator==(const ReturnedCredits&, const ReturnedCredits&) = default;
};

struct BattleCasualty {
    sim::EntityId id{};
    sim::tactical::TypeId tactical_type{};
    sim::galactic::TypeId original_type{}; // zero for battle-only objects
    sim::galactic::PlayerId owner{}, final_blow_player{};
    friend bool operator==(const BattleCasualty&, const BattleCasualty&) = default;
};
struct InvasionOffer {
    sim::galactic::PlanetId planet{};
    sim::galactic::FleetId fleet{};
    sim::galactic::PlayerId player{};
    std::vector<sim::EntityId> transports; // WGP-88: prepared identity, not visibility permission
    friend bool operator==(const InvasionOffer&, const InvasionOffer&) = default;
};
struct BattleResult {
    sim::EntityId conflict{};
    sim::galactic::PlayerId winner{};
    std::optional<sim::galactic::PlayerId> retreat_player;
    std::vector<ForceResult> forces; // ascending persistent ID, including undeployed reserves
    std::uint32_t station_level{};
    bool superweapon_destroyed{};
    sim::galactic::PlayerId superweapon_killer{};
    std::optional<bool> corruption_success; // unknown until that mission's rule owner supplies it
    std::vector<ReturnedCredits> credits;
    std::vector<ReturnedUpgrade> upgrades;
    std::vector<BattleCasualty> casualties;
    std::vector<InvasionOffer> invasion_offers; // prepared survivors; visible admission is a separate observer fact
    std::optional<sim::math::Fixed> bounty; // absent unless its producer supplies sourced accounting
    std::uint64_t tactical_frames{};
    // WGM-82/WGT-U04: deliberately empty until the return-side question is settled.
    std::vector<std::uint8_t> reserved_health;
    friend bool operator==(const BattleResult&, const BattleResult&) = default;
};

// Replaceable deterministic solver seam (WAR-28/51). The model owns its bounded progress and
// checkpoint encoding; GameSession never supplies a wall clock or invents a FoC solver here.
class AutoResolveModel {
public:
    virtual ~AutoResolveModel() = default;
    [[nodiscard]] virtual core::Result<void> begin(const ConflictRecord& conflict) = 0;
    [[nodiscard]] virtual core::Result<std::optional<BattleResult>> step(const sim::PartitionExecutor& executor) = 0;
    [[nodiscard]] virtual std::vector<std::uint8_t> encode_state() const = 0;
    [[nodiscard]] virtual core::Result<void> restore(std::span<const std::uint8_t> state) = 0;
};

// H(session seed, conflict ID), without ambient RNG state.
[[nodiscard]] std::uint64_t child_seed(std::uint64_t session_seed, sim::EntityId conflict);

} // namespace eawr::campaign
