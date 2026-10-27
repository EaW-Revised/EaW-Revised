#pragma once

#include "eawr/sim/galactic/types.hpp"

#include <cstdint>
#include <cstddef>
#include <string_view>

// Galactic events: what one completed tick did, in the order its serial commits did it. They feed
// presentation, and later the Lua inbox and the AI; nothing reads them back into the galaxy.
// Galactic event kinds are their own number space, separate from the tactical EventKind.
namespace eawr::sim::galactic {

// WGP-53/54: qualified feedback at its committed gameplay owner. Runtime
// delivery only: these values are neither replay opcodes nor checkpoint state.
struct GalacticFeedbackNotice {
    enum class Kind { capture_reward, planet_corrupted, corruption_removed, enemy_approaching };
    Kind kind{};
    PlayerId player{};
    PlanetId planet{};
    std::size_t preceding_events{}; // committed tick events before this notice
};

enum class GalacticEventKind : std::uint8_t {
    ability_prepared = 44, // request identity only; observer facts are published separately
    ability_settled = 45,
    ability_cancelled = 46,
    gambling_income_sampled = 36, // source, value: signed Q24 contribution
    black_market_purchased = 42, // subject current buyer, value independent market item
    formation_land = 43, // WGA-413: actor/planet notification, even on failed link
    command_executed = 1,
    command_rejected = 2,
    planet_owner_changed = 3,
    fleet_orbit_changed = 4,
    fiscal_cycle_completed = 5, // WGM-03, WGEP-01; value: the new cycle number
    galaxy_paused = 6,          // WGM-05: the first pause only
    galaxy_resumed = 7,         // WGM-06: the decrement to zero only
    speed_changed = 8,          // pacing only; value: the Q24 raw factor, subject: 0 strategic, 1 tactical
    save_requested = 9, // coordinator-reserved; value: signed requested slot as raw bits
    save_rejected = 10, // coordinator-reserved
    load_fixup_completed = 11, // coordinator-reserved; subject: pending battle, value: autosave
    fleet_created = 12, // coordinator-reserved; subject fleet, value planet
    fleet_destroyed = 13, // coordinator-reserved
    member_attached = 14, // coordinator-reserved; subject member, value fleet
    member_detached = 15, // coordinator-reserved; subject member, value former fleet
    fleet_slot_changed = 16, // coordinator-reserved; subject fleet, value slot
    movement_begin = 17, // coordinator-reserved; subject fleet, value final planet
    movement_finished = 18, // coordinator-reserved; value arrival planet
    movement_canceled = 19, // coordinator-reserved
    movement_bounced = 20, // coordinator-reserved; value inaccessible planet
    movement_conflict = 21, // coordinator-reserved; potential conflict seam, not admission
    production_started = 22, // subject planet, value type, sequence build identity
    production_completed = 23,
    production_cancelled = 24,
    land_conflict_requested = 25, // validated defended landing; no surface mutation (WGM-39/42)
    combat_imminent = 26, // WGM-43, announcement stage
    planet_combat_begin = 27,
    conflict_pending = 28,
    child_battle_selected = 29,
    auto_resolve_won = 30, // WAR-59; subject planet, value winning player
    auto_resolve_lost = 31, // WAR-59; subject planet, value losing player
    victory_pending = 39, // subject winning player, value condition
    victory_matured = 40, // presentation boundary
    victory_rejected = 41,
    auto_resolve_completed = 32, // subject conflict, value winner
    planet_discovered = 33, // subject planet, value observer player
    intelligence_refreshed = 34, // subject planet, value observer player
    discovery_feedback = 35, // one per nonzero service pass with new discovery
};

enum class GalacticRejectReason : std::uint8_t {
    none = 0,
    unknown_subject = 1, // the command names no object of the kind it acts on
    unknown_value = 2,   // the player or planet it names does not exist
    not_authorized = 3,  // the seat's player does not own the object (ADR-020)
    inactive_seat = 4,   // the issuing seat has left
    not_paused = 5,      // a resume with no pause to undo (WGM-06)
    paused = 6, // coordinator-reserved; WGM-07 management admission
    invalid_membership = 7, // coordinator-reserved
    incompatible_location = 8, // coordinator-reserved
    movement_ability_required = 9, // coordinator-reserved
    resource_limit = 10, // coordinator-reserved
    invalid_path = 11, // coordinator-reserved
    cannot_depart = 12, // coordinator-reserved
    movement_blocked = 13, // coordinator-reserved
    victory_unavailable = 20, // coordinator-reserved
    production_admission = 14, // value: ProductionAdmission
    insufficient_credits = 15, // the mover cannot pay the bribe admission cost of the path (WGM-84)
    observer_input = 18, // invalid source, clock or supplied observer facts
};

// One event. `producer` is the seat whose command produced it; `subject` the object it is
// about; `sequence` the producing command's sequence; `value` the new owner or orbit planet.
struct GalacticEvent {
    std::uint64_t tick{};
    GalacticEventKind kind{GalacticEventKind::command_executed};
    SeatId producer{};
    EntityId subject{};
    std::uint64_t sequence{};
    std::uint64_t value{};
    GalacticRejectReason reason{GalacticRejectReason::none};
    friend constexpr bool operator==(const GalacticEvent&, const GalacticEvent&) noexcept = default;
};

[[nodiscard]] std::string_view to_string(GalacticEventKind kind) noexcept;
[[nodiscard]] std::string_view to_string(GalacticRejectReason reason) noexcept;

} // namespace eawr::sim::galactic
