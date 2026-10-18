#pragma once

#include "eawr/campaign/conflict.hpp"
#include "eawr/sim/galactic/snapshot.hpp"
#include "eawr/sim/galactic/native_invasion.hpp"
#include <functional>

namespace eawr::campaign {

// WGM-103..107 / WGM-U09: immutable live behavior/type facts, not XML guesses.
enum class CombatantKind : std::uint8_t { ordinary, transport, company, squadron, space_base, ground_base, fleet };
struct CombatantFacts {
    CombatantKind kind{CombatantKind::ordinary};
    bool deleted{}, combatant{}, stealth{}, stays_in_transport{}, associated_object{true};
    sim::math::Fixed ai_power{};
    std::vector<CombatantFacts> contents;
};
[[nodiscard]] bool triggers_space_combat(const CombatantFacts& object);
[[nodiscard]] bool can_participate_land_combat(const CombatantFacts& object);
[[nodiscard]] bool can_participate_space_combat(const CombatantFacts& object);

// Immutable, qualified object-service inputs. Live containers are flattened by their owner;
// squadron identities remain whole in the force selector (WGT-07).
struct AdmissionForce {
    ConflictForce force;
    sim::galactic::FactionId faction{};
    sim::math::Fixed population{}, power{};
    std::uint32_t categories{};
    bool deleted{}, space_allowed{}, triggers_space{}, ground_capable{};
    bool transport{}, stays_in_transport{}, skips_combat{}, pathfinder{}, superweapon{}, whole_squadron{}, fleet_behavior{};
    friend bool operator==(const AdmissionForce&, const AdmissionForce&) = default;
};
struct OuterFleetFacts {
    bool deleted{}, space_allowed{}, fleet_behavior{}, ground_capable{};
};
struct AttachedFleetPresence {
    sim::galactic::PlayerId owner{};
    OuterFleetFacts facts;
    std::vector<AdmissionForce> direct_members;
};
struct LayerPresence {
    sim::galactic::PlayerId invader{}, defender{};
    std::vector<AdmissionForce> invading, landed; // direct admission entries, inspected list order
    std::vector<AttachedFleetPresence> attached_fleets;
    std::vector<AdmissionForce> invading_forces, orbital_forces; // separate flattened tactical rosters
    sim::galactic::PlayerId starbase_owner{};
    std::uint32_t starbase_level{}, ground_base_level{};
    bool prepared_resolver{}; // WGM-103: PREPARED override precedes either layer's roster checks
};
// Admission uses qualified orbit entries; later phases retain the admitted identities.
// A null snapshot is valid only for an admitted record with no live planet lookup.
struct ConflictPartyRequest {
    const ConflictRecord* admitted{};
    const sim::galactic::GalacticEvent* arrival{};
    const LayerPresence* presence{};
    const sim::galactic::NativeInvasion* invasion{};
    bool current_station_owner{};
};
struct ConflictParties {
    sim::galactic::PlayerId invader{}, defender{}, station_owner{}, station_side{}, planet_owner{};
    // WAR-12: every roster and solver keeps the same two admitted owners.
    [[nodiscard]] bool contains(sim::galactic::PlayerId owner) const noexcept {
        return owner == invader || owner == defender;
    }
};
[[nodiscard]] ConflictParties resolve_conflict_parties(
    const sim::galactic::GalacticSnapshot* snapshot, const ConflictPartyRequest& request);
using ForceQualifier = std::function<core::Result<AdmissionForce>(
    const sim::galactic::FleetMember&, sim::galactic::PlayerId, sim::EntityId)>;
using FleetQualifier = std::function<core::Result<OuterFleetFacts>(const sim::galactic::Fleet&)>;
// WGM-36/41/42/49: movement already gathered matching inbound fleets without a distance
// threshold. Consume their live orbit entries and the separate surface list; qualify live
// combat/transport state through the object-service owner, not guessed XML defaults.
[[nodiscard]] core::Result<LayerPresence> collect_conflict_presence(
    const sim::galactic::GalacticSnapshot& snapshot, const sim::galactic::GalacticEvent& request,
    const ForceQualifier& qualify, const FleetQualifier& qualify_fleet,
    const sim::galactic::NativeInvasion* explicit_invasion = nullptr);
// WGM-41/42: orbital and surface layers deliberately have distinct admission scans.
[[nodiscard]] bool admits_space_conflict(const LayerPresence& presence);
[[nodiscard]] bool admits_land_conflict(const LayerPresence& presence);
[[nodiscard]] bool skips_space_combat(std::span<const AdmissionForce> flattened);
struct RaidRules {
    sim::galactic::FactionId required_faction{}; // zero means unrestricted
    std::uint32_t free_categories{}, limited_categories{}, max_heroes{}, max_limited{};
};
[[nodiscard]] bool raid_capable(std::span<const AdmissionForce> flattened, const RaidRules& rules);

enum class ConflictRoute : std::uint8_t { instant_auto, dialog_auto, vote, pending_tactical, tactical };
struct ArbitrationInputs {
    BattleLayer layer{BattleLayer::space};
    bool multiplayer{}, invader_human{}, defender_human{}, local_involved{}, profile_auto{};
    bool tutorial_disables_auto{}, story_restricts_auto{}, queue_tactical{};
    std::uint8_t custom_option{2}; // WAR-01's command/UI vocabulary
    sim::math::Fixed invader_power{}, defender_power{}, overrun_multiple{};
    bool invader_named_hero{}, defender_named_hero{};
};
struct Arbitration {
    ConflictRoute route{ConflictRoute::tactical};
    bool forced_tactical{}, overrun{};
    friend bool operator==(const Arbitration&, const Arbitration&) = default;
};
// WGM-173..177/185: qualified owner inputs, not inferred from arbitration.
// Missing speech/story/recipient facts leave the alert projection absent.
struct PendingBattleAlertInputs {
    sim::galactic::PlayerId local{};
    bool active_mode{}, queue_tactical{}, multiplayer{}, authorized_issuer{};
    bool always_bypass{}, linked_tactical{}, tutorial_player{}, tutorial_forced{}, story_prevented{};
    bool campaign_conflict_queued{};
    std::uint32_t active_speech_events{};
    std::uint64_t elapsed_wall_ms{}, timeout_ms{}; // WGM-176 owner clock and authored timeout
};
struct PendingBattleAlertView {
    sim::galactic::PlayerId local{};
    bool visible{}, single_button{}, graph_visible{}, begin_admitted{}, resolve_admitted{};
    bool waiting_for_opponent{}, timeout_due{};
    std::uint64_t elapsed_wall_ms{}, timeout_ms{};
};
[[nodiscard]] inline PendingBattleAlertView pending_battle_alert_view(const ConflictRecord& conflict,
    const PendingBattleAlertInputs& inputs, bool choice_ready, std::optional<bool> vote = {}) {
    PendingBattleAlertView view; view.local = inputs.local;
    view.visible = inputs.active_mode && inputs.queue_tactical && (inputs.multiplayer || !inputs.active_speech_events);
    view.single_button = inputs.always_bypass || inputs.linked_tactical || inputs.tutorial_player
        || inputs.tutorial_forced || inputs.story_prevented;
    view.graph_visible = view.visible && (inputs.local == conflict.defender || inputs.local == conflict.invader);
    view.waiting_for_opponent = inputs.multiplayer && vote.has_value();
    view.begin_admitted = view.visible && choice_ready && inputs.authorized_issuer && !view.waiting_for_opponent;
    view.resolve_admitted = view.begin_admitted && !view.single_button && !inputs.campaign_conflict_queued;
    view.elapsed_wall_ms = inputs.elapsed_wall_ms; view.timeout_ms = inputs.timeout_ms;
    // WGM-176: equality waits; yes/zero is a Begin preference, never withdraw.
    view.timeout_due = view.begin_admitted && inputs.multiplayer && inputs.elapsed_wall_ms > inputs.timeout_ms;
    return view;
}

// Presentation sees announcement and choice readiness separately (GFD-18).
// A player-owned pause cannot make an announced conflict ready for a choice.
struct PendingConflictView {
    ConflictRecord conflict;
    Arbitration arbitration;
    bool choice_ready{};
    std::optional<PendingBattleAlertView> alert;
};
[[nodiscard]] core::Result<Arbitration> arbitrate_conflict(const ArbitrationInputs& inputs);

struct ForceSelection {
    std::vector<ConflictForce> initial, reinforcement;
};
// WGT-06..08: ascending type buckets, last-listed first. Population/cap are authored inputs.
[[nodiscard]] core::Result<ForceSelection> select_conflict_forces(std::span<const AdmissionForce> forces,
    sim::math::Fixed cap, std::optional<sim::galactic::TypeId> preferred = {}, bool use_pathfinder = false);

} // namespace eawr::campaign
