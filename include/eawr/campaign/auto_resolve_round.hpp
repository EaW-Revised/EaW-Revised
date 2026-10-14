#pragma once

#include "eawr/campaign/auto_resolve_strength.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"

#include <functional>

namespace eawr::campaign {

struct AutoResolveRoundRules {
    sim::math::Fixed display_seconds{}, winner_attrition{}, loser_attrition{};
    sim::math::Fixed retreat_winner_attrition{}, retreat_loser_attrition{};
    sim::math::Fixed allowance{}, transport_losses{}, tactical_multiplier{};
    friend bool operator==(const AutoResolveRoundRules&, const AutoResolveRoundRules&) = default;
};
// Authored values from the validated loader, otherwise the code defaults in WAR's
// "XML tags and GameConstants" section. Malformed authored values are rejected.
[[nodiscard]] core::Result<AutoResolveRoundRules> auto_resolve_round_rules(const data::galactic::GalacticData& data);
// WAR-40 arithmetic is shared with #2026's future interleaved allocator.
[[nodiscard]] core::Result<sim::math::Fixed> auto_resolve_survival_budget(
    sim::math::Fixed remaining, const AutoResolveRoundRules& rules, bool mid_tactical);

struct AutoResolvePlayerTraits {
    bool present{}, human{}, playable{};
    friend bool operator==(const AutoResolvePlayerTraits&, const AutoResolvePlayerTraits&) = default;
};
struct AutoResolveRoundContext {
    std::array<AutoResolvePlayerTraits, 2> players{}; // gathered side order
    bool mid_tactical{}, terrain_known{};
};

// These services read frozen validated data. Bind contrast to FH-30 average_contrast
// (src/script/foc/host.hpp), terrain to WAR-35 (first company member's movement
// class; absent cell = 1), and ground_attack to the company/dummy-ground-base
// predicate in WAR-34. No inferred layer from the conflict's battle layer.
struct AutoResolveAttackServices {
    std::function<core::Result<sim::math::Fixed>(const units::GalacticType&, const AutoResolveCategory&)> contrast;
    std::function<core::Result<sim::math::Fixed>(const units::GalacticType&, std::uint32_t)> terrain;
    std::function<bool(const units::GalacticType&)> ground_attack;
    // WAR-38/41 query the object's live contents, independently of WAR-24's
    // named-hero type flag. WAR-42 protects every superweapon, not only the
    // last selected pressure source in AutoResolveSideStrength.
    std::function<core::Result<bool>(const AutoResolveCombatant&)> contains_named_hero;
    std::function<core::Result<bool>(const AutoResolveCombatant&)> is_superweapon;
    // WAR-48: first company member unless it creates a team, otherwise own type.
    // The combatant also identifies a land planet that has no current base type.
    std::function<core::Result<std::string>(const AutoResolveCombatant&)> learning_type;
    // PostTransportDeathRoster seam (#2024, WAR-U04): after the ordinary lethal
    // path, does this transport still contain a named hero? Killed entries keep
    // their queue positions; this explicit view controls their WAR-41 pick class.
    std::function<core::Result<bool>(const AutoResolveCombatant&)> post_transport_death_contains_hero;
};
// One inclusive-range request per random pick, including a singleton. The caller
// binds its existing simulation RNG stream (AI-53); no per-side seed or slot.
// Stage a copy of the caller's stream and commit it only with a successful result.
using AutoResolvePick = std::function<core::Result<std::size_t>(std::size_t count)>;

struct AutoResolveAllocation {
    sim::EntityId id{}, killer{};
    sim::galactic::PlayerId owner{}, final_blow_player{};
    std::string type, retained_base_type;
    bool killed{}, base_changed{}, base_destroyed{}, escaped_transport{};
    friend bool operator==(const AutoResolveAllocation&, const AutoResolveAllocation&) = default;
};
struct AutoResolveLearning {
    sim::galactic::PlayerId player{};
    sim::EntityId id{};
    std::string type;
    bool survived{};
    friend bool operator==(const AutoResolveLearning&, const AutoResolveLearning&) = default;
};
// OutcomeApplication (#2024) seam: execute killed entries through ordinary GC
// miscellaneous damage (WAR-85..88: 1e18, ordinary immunity/death gates), apply base changes and WAR-43 clears,
// record losses, fix land structures, register victory parameter 5 and feed AI
// learning. This solver never mutates fleets, credits, hero timers or health.
struct AutoResolveRoundOutcome {
    sim::galactic::PlayerId winner{}, retreating_player{};
    std::array<std::vector<sim::math::Fixed>, 2> remaining;
    std::vector<AutoResolveAllocation> allocations; // execution order, deferred weakest last
    std::vector<AutoResolveLearning> learning;
    // WAR-118..120: retained allocator queues in surviving pick order, not history.
    std::array<std::vector<AutoResolveCombatant>, 2> retained_queues;
    bool fix_land_structures{}, superweapon_victory{};
    std::size_t random_picks{};
    friend bool operator==(const AutoResolveRoundOutcome&, const AutoResolveRoundOutcome&) = default;
};

[[nodiscard]] core::Result<std::array<std::vector<sim::math::Fixed>, 2>> auto_resolve_attacks(
    const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    std::span<const units::EnumValue> bits, const AutoResolveRoundContext& context,
    const AutoResolveAttackServices& services);

// Value-only ordered recurrence outside a session tick. Mid-tactical deaths need
// #2026's interleaved projectile draws, so that mode is explicitly rejected here.
[[nodiscard]] core::Result<AutoResolveRoundOutcome> auto_resolve_combat_round(
    const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services,
    const AutoResolvePick& pick, sim::galactic::PlayerId requested_retreat = 0);

enum class AutoResolveRoundProgress { not_done, done, done_with_retreat };
struct AutoResolveRoundState {
    sim::math::Fixed start_seconds{};
    sim::galactic::PlayerId retreating_player{};
    std::optional<AutoResolveRoundOutcome> outcome;
    friend bool operator==(const AutoResolveRoundState&, const AutoResolveRoundState&) = default;
};
struct AutoResolveRoundStep {
    AutoResolveRoundState state;
    AutoResolveRoundProgress progress{AutoResolveRoundProgress::not_done};
};
[[nodiscard]] bool request_auto_resolve_retreat(AutoResolveRoundState& state,
    const AutoResolveStrength& strength, sim::galactic::PlayerId player);
[[nodiscard]] core::Result<AutoResolveRoundStep> step_auto_resolve_round(
    const AutoResolveRoundState& previous, sim::math::Fixed now_seconds, bool force,
    const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services,
    const AutoResolvePick& pick);

// WAR-41: failed validation/allocation cannot consume the caller's stream.
[[nodiscard]] core::Result<AutoResolveRoundStep> step_auto_resolve_round_transaction(
    const AutoResolveRoundState& previous, sim::math::Fixed now_seconds, bool force,
    const AutoResolveStrength& strength, const units::GalacticTypeTable& types,
    std::span<const units::EnumValue> bits, const AutoResolveRoundRules& rules,
    const AutoResolveRoundContext& context, const AutoResolveAttackServices& services,
    script::authoritative::RandomStream& random, sim::galactic::PlayerId overrun_loser = 0);

// WAR-27 resolves zero-space-strength shortcut battles at initiate, before a
// caller can observe an unfought prepared conflict. Ordinary battles remain pending.
[[nodiscard]] core::Result<AutoResolveRoundStep> initiate_auto_resolve_round(
    sim::math::Fixed now_seconds, const AutoResolveStrength& strength,
    const units::GalacticTypeTable& types, std::span<const units::EnumValue> bits,
    const AutoResolveRoundRules& rules, const AutoResolveRoundContext& context,
    const AutoResolveAttackServices& services, script::authoritative::RandomStream& random);

// WAR-51 uses the authored land delay in either layer, divided by three while
// retreat is in progress. Automatic conflict-service scheduling remains its owner.
[[nodiscard]] core::Result<sim::math::Fixed> auto_resolve_service_delay(
    sim::math::Fixed land_delay, bool retreating);

} // namespace eawr::campaign
