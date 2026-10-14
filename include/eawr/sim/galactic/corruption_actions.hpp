#pragma once

#include "eawr/sim/galactic/corruption_model.hpp"

namespace eawr::sim::galactic::corruption_actions {
using namespace corruption_fixture;

// Fixture boundary: authored IDs/values come from validated data; these do not
// add a technology gate (none is established by WGCH-01..10/48/49).
enum class MissionMode : std::uint8_t { none, land, space };
struct Requirements {
    std::vector<Id> unit_types, planet_types;
    unsigned unit_count{}, mission_count{};
    Type mission_type{};
};
struct Choice {
    Type type{};
    Id briefing{}, map{}, script{};
    Fixed credits;
    Requirements requirements;
    MissionMode mode{};
};
struct Actor {
    Id id{}, owner{};
    bool present{true};
    // WGCH-02: the caller flattens the fleet and performs depth-first contained
    // ability lookup. Appropriate IDs retain that traversal order.
    std::optional<unsigned> flattened_fleet_count;
    std::vector<Id> appropriate_abilities;
};
struct Account {
    Player player;
    bool ai{}, local{};
    std::set<Id> allies;
    std::array<unsigned, 9> uses{};
};
struct Location {
    Id id{}, type{}, owner{};
    bool present{true}, neutral_owner{}, destroyed{}, retains_influence{};
    corruption_fixture::Planet model;
    std::array<Choice, 3> slots;
};
struct Unit {
    Id id{}, owner{}, original_type{}, container{}, planet{};
    bool present{true}, consumable{}, named{};
    // WGCH-07/62: caller supplies synchronized weight in [0,4]. U-03 equal
    // weights have no established stable-ID policy: tie_rank is an explicit
    // unverified seam supplied by the host, unique within an equal-weight group.
    unsigned weight{}, tie_rank{};
};
struct Fixture {
    std::vector<Actor> actors;
    std::vector<Account> accounts;
    std::vector<Location> locations;
    std::vector<Unit> units;
};
enum class Refusal : std::uint8_t {
    none, tutorial, actor, owner, planet, fleet, ability, allied_owner,
    neutral_owner, destroyed, transitioning, corrupted, residual_influence,
    no_choices, credits, units, mission_usage, required_planet, no_ai_choice,
    mission_queue, transition, mission_state
};
struct Prepared { Id actor{}, planet{}; Choice choice; };
struct Preparation {
    Refusal refusal{};
    Id ability{};
    std::vector<Prepared> choices;
};
// WGCH-01..04/48/49: route names in authored order; local preparation never
// charges or hides unaffordable choices. Missing galaxy planet types are omitted.
[[nodiscard]] Preparation prepare(const Fixture&, Id actor, Id planet,
    bool tutorial_allowed, std::span<const Id> activated_abilities);
[[nodiscard]] Refusal prerequisites(const Fixture&, const Account&, const Choice&);
// WGCH-50: code constants, distinct from data-driven fiscal factors.
[[nodiscard]] Fixed ai_score(Type, unsigned uses);
// WGCH-03: caller supplies synchronized unit draw in [0,1); no RNG owned here.
[[nodiscard]] core::Result<std::optional<Prepared>> select_ai(
    const Fixture&, const Preparation&, Fixed unit_draw);

enum class Effect : std::uint8_t {
    debit, schedule_prerequisite_default, destroy_prerequisite,
    queue_mission, begin_transition, increment_usage, hero_arrival,
    local_success, schedule_actor_default, remove_actor, mission_success, mission_failure
};
struct Event {
    Effect effect{};
    Id object{}, player{};
    friend bool operator==(const Event&, const Event&) = default;
};
enum class MissionState : std::uint8_t { queued, succeeded, failed, cancelled };
struct Mission {
    Id actor{}, planet{}, player{}, map{}, script{};
    Type type{};
    MissionMode mode{};
    MissionState state{MissionState::queued};
    bool local_success_permitted{true};
};
struct Execution {
    Refusal refusal{};
    std::vector<Event> events;
    std::optional<Mission> mission;
    [[nodiscard]] bool accepted() const { return refusal == Refusal::none; }
};
struct ExecutionInput {
    bool multiplayer{}, fast_corruption{}, mission_queue_accepts{true};
    bool local_success_permitted{true};
    std::uint32_t logical_fps{30};
    Frame now{};
};
// WGCH-05..09/72: current owner, debit before stale unit recheck. Refusals
// preserve earlier mutations. Arithmetic/input diagnostics are separate.
[[nodiscard]] core::Result<Execution> execute(Fixture&, const Prepared&, const ExecutionInput&);
// WGCH-10/WGM-81: success begins the stored 5 s request; failure does not
// reconcile conquest/survivors. Cancel is an explicit host seam, not a claimed
// retail interruption policy. Already settled records refuse repeat callbacks.
[[nodiscard]] core::Result<Execution> finish_mission(Fixture&, Mission&, MissionState,
    std::uint32_t logical_fps, Frame now);
} // namespace eawr::sim::galactic::corruption_actions
