#pragma once

#include "eawr/campaign/auto_resolve_round.hpp"
#include "eawr/sim/galactic/session.hpp"

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
