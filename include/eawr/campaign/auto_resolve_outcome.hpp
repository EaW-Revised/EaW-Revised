#pragma once

#include "eawr/campaign/auto_resolve_round.hpp"
#include "eawr/sim/galactic/session.hpp"

#include <optional>
#include <span>

namespace eawr::campaign {

struct AutoResolveDeath {
    sim::EntityId id{}, killer{};
    sim::galactic::PlayerId owner{}, final_blow_player{};
    friend bool operator==(const AutoResolveDeath&, const AutoResolveDeath&) = default;
};
struct AutoResolveRecordEntry {
    std::string original_type;
    sim::galactic::PlayerId owner{};
    friend bool operator==(const AutoResolveRecordEntry&, const AutoResolveRecordEntry&) = default;
};
struct AutoResolveOutcomeApplication {
    sim::galactic::PlanetId planet{};
    sim::galactic::PlayerId invader{}, defender{};
    bool land{}, fix_land_structures{}, base_changed{}, pending_superweapon_victory{};
    BattleResult result;
    std::vector<sim::EntityId> destroyed; // ascending confirmed post-delivery persistent casualties
    std::vector<AutoResolveDeath> deaths; // ordinary damage requests, not proof of successful death
    std::vector<sim::EntityId> land_invaders; // surviving transports in invasion-list order
    std::vector<sim::galactic::FleetId> retreat_fleets; // reverse source-orbit order, WAR-49/58
    std::string planet_type;
    std::vector<AutoResolveRecordEntry> record;
    std::vector<AutoResolveLearning> learning;
    // WAR-43a: the planet's space special structures cleared with the loser's destroyed base. They die as scoring kills
    // (the results screen lists them) but are not battle history (`record`), so the advisor counts stay WAR-64's.
    std::vector<AutoResolveRecordEntry> structure_losses;
    friend bool operator==(const AutoResolveOutcomeApplication&, const AutoResolveOutcomeApplication&) = default;
};
struct AutoResolveOutcomeInputs {
    std::string planet_type;
    // WAR-85..88 / WAR-U04: required post-delivery casualties for every damage
    // request, including the selected identity only if ordinary death succeeded.
    // Empty means immunity/restoration kept the selected object alive.
    std::function<core::Result<std::vector<sim::EntityId>>(const AutoResolveAllocation&)> lethal_closure;
    // WAR-88: attrition container history reads contents after delivery. Space
    // transport-loss history instead uses its pre-delivery contents (WAR-38).
    std::function<core::Result<std::vector<AutoResolveRecordEntry>>(const AutoResolveAllocation&)> post_delivery_record;
    // WAR-44: validated base-type-to-level adapter, including the empty type -> zero.
    std::function<core::Result<std::uint32_t>(std::string_view)> base_level;
    std::vector<sim::EntityId> raid_destination_entries;
};
[[nodiscard]] core::Result<AutoResolveOutcomeApplication> plan_auto_resolve_outcome(
    const ConflictRecord& conflict, const AutoResolveRoster& roster,
    const AutoResolveRoundOutcome& outcome, const AutoResolveOutcomeInputs& inputs);

// WAR-43: a space auto-resolve that destroys the LOSING planet's base also clears that planet's space special structures.
// The winner's own budget downgrade to zero (WAR-44) is not this case. `presence` is the planet's control row before the
// return; real battle returns never use this (WGM-57: only structures that died in the fight leave).
[[nodiscard]] bool auto_resolve_clears_space_structures(const AutoResolveOutcomeApplication& application,
    const sim::galactic::ControlPlanet* presence) noexcept;
// WAR-43a: the results-screen losses of that clearing: every space special structure among `objects` (the planet's
// production objects, in their order), by original type name, when WAR-43 clears them. When it clears nothing the objects
// are never read, so an unresolvable structure type cannot fail an outcome that clears none; when it does clear them, a
// structure whose type has no name fails (`type_name` returns nothing).
[[nodiscard]] core::Result<std::vector<AutoResolveRecordEntry>> auto_resolve_structure_losses(
    const AutoResolveOutcomeApplication& application, const sim::galactic::ControlPlanet* presence,
    std::span<const sim::galactic::ProductionObject> objects,
    const std::function<std::optional<std::string>(sim::galactic::TypeId)>& type_name);

struct AutoResolveOutcomeServices {
    // WAR-U04LethalEffects: ordinary GC death path owns bounty, hero scheduling,
    // elimination and sound. Absence retains the documented least-visible seam.
    std::function<core::Result<void>(sim::galactic::GalacticSession&, std::span<const AutoResolveDeath>)> war_u04_lethal_effects;
    // ControlOutcomeApplication: bind to the political-control return API. The
    // solver never assigns ownership. Required for land/base changes (WAR-62).
    std::function<core::Result<void>(sim::galactic::GalacticSession&, const AutoResolveOutcomeApplication&)> control;
    // WGM-61/62/63 owns destinations and normal movement; no teleport or guessed tie.
    std::function<core::Result<void>(sim::galactic::GalacticSession&, const AutoResolveOutcomeApplication&)> retreat;
    // WAR-60 branch (b): pays the final-blow players of the ordinary deaths (a.deaths and the lethal closure
    // of each) as credits, before any ownership return. Branch (a), the explicit bounty-collection switch,
    // allied payout and sub-one rounding stay documented seams (legacy #4911).
    std::function<core::Result<void>(sim::galactic::GalacticSession&, const AutoResolveOutcomeApplication&)> bounty;
    // WAR-47/48/55/59: structures, victory parameter 5, learning, cleanup and story consumers.
    std::function<core::Result<void>(sim::galactic::GalacticSession&, const AutoResolveOutcomeApplication&)> effects;
};
[[nodiscard]] core::Result<void> apply_auto_resolve_outcome(sim::galactic::GalacticSession& galaxy,
    const AutoResolveOutcomeApplication& application, const AutoResolveOutcomeServices& services);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_auto_resolve_outcome(const AutoResolveOutcomeApplication& application);
[[nodiscard]] core::Result<AutoResolveOutcomeApplication> decode_auto_resolve_outcome(std::span<const std::uint8_t> bytes);

} // namespace eawr::campaign
