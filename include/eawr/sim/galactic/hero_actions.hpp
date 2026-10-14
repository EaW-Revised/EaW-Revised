#pragma once

#include "eawr/sim/galactic/corruption_actions.hpp"

#include <functional>
#include <map>

namespace eawr::sim::galactic::hero_actions {
using corruption_fixture::Fixed;
using corruption_fixture::Frame;
using corruption_fixture::Id;
inline constexpr auto one = corruption_fixture::one;

// Fixture-only inputs supplied by validated loaders. No XML reader, session
// sweep, menu, RNG, checkpoint format or lifecycle callback is installed here.
// Curve interpolation/float quantization and companion transport destruction
// are named host seams (WGCH U-05/U-08/U-12, WGCH-95).
using Curve = std::function<Fixed(unsigned)>;
struct Range { Fixed low, high; };
struct Object {
    Id id{}, owner{}, current_type{}, original_type{}, parent{};
    bool present{true}, hero{}, named{}, transport{}, fleet{}, planetary{}, destroyed{};
    bool visible{true}, hidden_details{}, deletion_pending{}, stealth{}, contested{};
    bool major_allowed{true}, minor_allowed{true}, final_blow{};
    Fixed neutralization_cost;
    std::vector<Id> contents;
};
struct Account {
    corruption_actions::Account base;
    unsigned tech{}, maximum_tech{4};
    bool playable{true}, human{true}, leader_kill_victory{};
    Id leader_type{};
    std::set<Id> unlocked, locked;
    Fixed frame_credit_income;
};
struct Protection { Id source{}, category{}; Fixed multiplier{one}; };
struct Structure {
    Id id{};
    Fixed cost, tutorial_cost;
    bool present{true}, deletion_pending{}, removal_accepts{true};
};
struct Siphon {
    corruption_fixture::Siphon fiscal;
    Id source_type{}, company_type{};
    Fixed accumulated;
    bool discovered{}, named{}, major_allowed{true}, minor_allowed{true};
    Fixed neutralization_cost;
};
struct Planet {
    Id id{}, current_allegiance{};
    bool present{true}, planetary{true}, destroyed{}, auto_capture_minor{}, prevent_minor_siphon{};
    bool revealed{true};
    unsigned ground_base{}, land_capacity{}, sabotage_count{}, tech_rating{}, slice_difficulty{};
    corruption_fixture::Planet model;
    std::vector<Id> orbit, landed;
    std::vector<Structure> ground, space;
    std::vector<Protection> protection;
    Siphon siphon;
    std::map<Id, Id> registered_siphon_targets;
    // WGUI-23 insertion appends, including repeated source IDs. Sensor service
    // owns the corresponding recipient/mask/deadline records and removal.
    std::multiset<Id> spy_sources;
    std::set<Id> registered_spy_targets, political_sources;
};
struct Fixture {
    std::vector<Object> objects;
    std::vector<Account> accounts;
    std::vector<Planet> planets;
    // WGCH-23/94: company type and current attacking player, never victim owner.
    std::map<std::pair<Id, Id>, unsigned> neutralizations;
};
enum class Refusal : std::uint8_t {
    none, actor, owner, planet, target, opposing, playable, destroyed,
    superweapon, no_choices, invalid_option, credits, cost, removal,
    captured, prevented, siphon, transport, visibility, contested, remote
};
enum class Effect : std::uint8_t {
    schedule, remove_object, remove_transport, attribution, neutralized,
    end_siphon, debit, arrival, tutorial, presentation, reevaluate_ground,
    reevaluate_space, sabotage_story, sabotage_effect, frame_income,
    unlock, advance_tech, register_target, deregister_target, spy_insert,
    refresh_visibility, hero_land, ability_effect, build_lock, hack_sound,
    corruption_removal, ruins, destroy_landed, clear_ground, clear_political,
    abort_corruption, corruption_cleanup, destroyed_story, terminate_abilities,
    refresh_model, refresh_corruption_visuals, destroy_orbit, clear_starbase, neutral_ownership, hack_particle,
    pending_victory, political_protection, stealth, prepare_menu
};
struct Event {
    Effect effect{};
    Id object{}, player{}, type{};
    Fixed value;
    bool bypass_parent{};
    Id source{};
    friend bool operator==(const Event&, const Event&) = default;
};
struct Outcome {
    Refusal refusal{};
    bool success{}, consumed{};
    std::vector<Event> events;
};
// WGCH-21/93: nonpositive neutralization endpoints use authored default.
[[nodiscard]] core::Result<Fixed> absence(Range, Fixed draw, Fixed default_seconds);

struct NeutralizationAbility {
    bool minor{}, major{};
    Fixed minor_modifier{one}, major_modifier{one}, default_absence;
    Range actor_absence, target_absence;
    Curve base_curve, history_curve, corruption_curve;
};
struct Neutralization {
    Id actor{}, planet{}, target{}, company_type{};
    bool siphon{}, valid{};
    Fixed price, actor_absence, target_absence;
};
struct NeutralizationPreparation {
    Refusal refusal{};
    std::vector<Neutralization> choices;
};
// WGCH-20..24/52/63/91..95: draws are one actor/target pair per enumerated
// resolvable candidate, including disabled/expensive candidates.
[[nodiscard]] core::Result<NeutralizationPreparation> prepare_neutralization(
    const Fixture&, Id actor, Id planet, Id hack_required_type,
    const NeutralizationAbility&, std::span<const std::pair<Fixed, Fixed>> draws);
[[nodiscard]] core::Result<Outcome> neutralize(Fixture&, const Neutralization&);
// WGCH-24/93: uniform choice among valid affordable prepared options.
[[nodiscard]] std::optional<Neutralization> select_neutralization_ai(
    const Fixture&, const NeutralizationPreparation&, unsigned synchronized_index);

struct Sabotage { Id actor{}, planet{}, structure{}; Fixed price, absence; };
enum class SabotageRoute : std::uint8_t { prepared, free_script, direct_ai };
// WGCH-28/97/98: ordinary escalation and tutorial price are separate.
[[nodiscard]] core::Result<Fixed> sabotage_price(Fixed cost, Fixed factor, unsigned prior_count);
[[nodiscard]] core::Result<std::vector<Sabotage>> prepare_sabotage(
    const Fixture&, Id actor, Id planet, Fixed factor, bool tutorial);
[[nodiscard]] core::Result<bool> menu_affordable(Fixed price, Fixed local_credits);
// WGCH-29..31/77/96/99: direct AI posts this-frame income; it does not
// debit/remove the actor. WGCH-70 consumption is a separate call.
[[nodiscard]] core::Result<Outcome> sabotage(Fixture&, const Sabotage&, SabotageRoute);
// WGCH-77: ground then space, skip deletion-pending/unaffordable entries.
[[nodiscard]] core::Result<Outcome> sabotage_ai(Fixture&, Id actor, Id planet,
    Fixed factor, unsigned synchronized_index);
[[nodiscard]] Outcome legacy_sabotage_capture(Fixture&, Id actor, Id planet);

struct Technology {
    Id id{}, faction{};
    unsigned tech{};
    bool initially_locked{}, slicer_unlock{}, space{};
    Fixed cost;
};
struct TheftAbility {
    Curve availability, ground, space, difficulty;
    Fixed affinity_price, affinity_chance, purchase_multiplier{one};
    std::set<Id> affinity_types;
    std::vector<Fixed> absence_min, absence_max;
};
struct Theft { Id actor{}, planet{}, technology{}; Fixed price, chance, absence; bool advance{}; };
struct TheftPreparation { Refusal refusal{}; std::vector<Theft> choices; };
// WGCH-54/55/100/101: initially empty lists refuse before any tech increment.
[[nodiscard]] core::Result<TheftPreparation> prepare_theft(const Fixture&, Id actor,
    Id planet, std::span<const Technology>, const TheftAbility&, Fixed absence_draw);
[[nodiscard]] std::optional<Theft> select_theft_ai(
    const Fixture&, const TheftPreparation&, unsigned synchronized_index);
// WGCH-34..36/64/102: caller supplies synchronized success draw [0,0.999]
// and failure outcome 1..3; false may still consume the actor.
[[nodiscard]] core::Result<Outcome> steal(Fixture&, const Theft&, Fixed success_draw, unsigned failure_draw);

struct SiphonAbility { Range percentage; unsigned cycles{}; };
// WGCH-103..107: immediate transport/company metadata required; register even
// after helper refusal. Strategic fiscal duration is an explicit host input.
[[nodiscard]] core::Result<Outcome> deploy_siphon(Fixture&, Id actor, Id planet,
    const SiphonAbility&, Fixed draw, Frame now, Frame fiscal_frames);
[[nodiscard]] Outcome remove_siphon(Fixture&, Id actor, Id planet);
enum class Activation : std::uint8_t { ground, automatic, other };
struct Spy { Activation activation{}; std::uint32_t mask{}; Fixed duration; bool insertion_accepts{true}; Id unique_source{}; };
// WGCH-56/108: WGUI owns mask validation, source replacement and expiry.
[[nodiscard]] Outcome spy(Fixture&, Id actor, Id planet, const Spy&);
// WGCH-70: successful service result only; generic scheduling does not bypass
// the parent check. Caller decides eligibility/default-time via WGM-79/80.
void consume_success(Fixture&, Id actor, bool causes_despawn, Fixed absence, Outcome&);

enum class HackRoute : std::uint8_t { human_prepared, direct_ai };
struct Hack { Id actor{}, planet{}, required_type{}; Fixed prepared_price; };
// WGCH-109..112: match current type, then human debit, then owner lookup.
[[nodiscard]] core::Result<Outcome> hack(Fixture&, const Hack&, HackRoute,
    std::uint32_t logical_fps, Frame now, Id neutral_player);
// WGCH-57: local humans request a menu; remote non-AI actors do nothing.
[[nodiscard]] core::Result<Outcome> invoke_hack(Fixture&, const Hack&,
    std::uint32_t logical_fps, Frame now, Id neutral_player);
// WGCH-58/67/113..115: contested check and pending victory request, shared
// ruins cleanup; no orbital/starbase/ownership/payment/actor destruction.
[[nodiscard]] Outcome destroy_planet(Fixture&, Id actor, Id planet, bool galactic_mode);
// WGCH-42/59/68: source-keyed boolean and stealth toggles, no sensor algorithm.
[[nodiscard]] Outcome political_protection(Fixture&, Id source, Id planet, bool apply,
    Id unique_source = 0, bool appropriate = true);
[[nodiscard]] Outcome galactic_stealth(Fixture&, Id actor, bool apply);
} // namespace eawr::sim::galactic::hero_actions
