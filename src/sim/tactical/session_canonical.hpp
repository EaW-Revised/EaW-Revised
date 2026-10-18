#pragma once

#include "session_types.hpp"

namespace eawr::sim::tactical::session_detail {

// Publication scratch: each worker writes the counter row of its stable unit range.
struct CanonicalCounts {
    std::uint64_t cloaked{};
    std::uint64_t lengthened{};
    std::uint64_t scripted{};
    std::uint64_t mc30_scripts{};
    std::uint64_t special_owners{};
    std::uint64_t concentrate_owners{};
    std::uint64_t buzz_requests{};
    std::uint64_t corrupt_count{};
    std::uint64_t blast_count{};
    std::uint64_t able{};
    std::uint64_t corrupt_casts{};
    std::uint64_t disabled_engines{};
    std::uint64_t stunned{};
    std::uint64_t manual_weapons{};
    std::uint64_t turret_count{};
    std::uint64_t tracked{};
    std::uint64_t vulnerable{};
    std::uint64_t token_units{};
    std::uint64_t modified_count{};
    std::uint64_t hazard_moves{};
    std::uint64_t waypoint_count{};
    std::uint64_t marked_units{};
    std::uint64_t heroes{};
    std::uint64_t history_count{};
    std::uint64_t disabled_garrisons{};
    std::uint64_t rolled{};
    std::uint64_t contacts{};
    std::uint64_t nebulas{};
    std::uint64_t storms{};
    std::array<std::uint64_t, 3> formation{};
    std::uint64_t approaches{};
    std::uint64_t sightings{};

    void add(const LiveUnit& unit) {
        cloaked += (unit.abilities && unit.abilities->stealth_on) ? 1U : 0U;
        lengthened += (unit.abilities && unit.abilities->stealth_on && unit.abilities->stealth_end != 0) ? 1U : 0U;
        scripted += (unit.abilities && unit.abilities->self_destruct_script) ? 1U : 0U;
        mc30_scripts += (unit.abilities && unit.abilities->mc30_script) ? 1U : 0U;
        special_owners += (unit.abilities && !unit.abilities->special.slots.empty()) ? 1U : 0U;
        concentrate_owners += (unit.abilities && !unit.abilities->concentrate_recruits.empty()) ? 1U : 0U;
        buzz_requests += (unit.abilities && unit.abilities->pending_buzz_position) ? 1U : 0U;
        corrupt_count += (unit.corrupt.has_value()) ? 1U : 0U;
        blast_count += (unit.abilities && unit.abilities->charged_blast) ? 1U : 0U;
        able += (unit.abilities.has_value()) ? 1U : 0U;
        disabled_engines += (unit.durability && unit.durability->engines_disabled_until) ? 1U : 0U;
        stunned += (unit.ion_stun.has_value()) ? 1U : 0U;
        tracked += (unit.durability && (unit.durability->window_damage.raw() != 0 || unit.durability->damage_rate.raw() != 0)) ? 1U : 0U;
        vulnerable += (unit.arrival_vulnerable_until.has_value()) ? 1U : 0U;
        token_units += (unit.state.purchase_token != 0) ? 1U : 0U;
        modified_count += (unit.upgrade_bonuses != CombatBonuses{} || (unit.durability && (std::any_of(unit.durability->disabled.begin(), unit.durability->disabled.end(), [](bool v) { return v; }) || std::any_of(unit.durability->repairing_players.begin(), unit.durability->repairing_players.end(), [](const auto& v) { return !v.empty(); })))) ? 1U : 0U;
        hazard_moves += (unit.state.order.through_hazards) ? 1U : 0U;
        waypoint_count += (unit.waypoints.has_value()) ? 1U : 0U;
        marked_units += (unit.garrison_unit) ? 1U : 0U;
        heroes += (unit.state.purchase_type != 0 || !unit.state.contained.empty()) ? 1U : 0U;
        history_count += (unit.position_history.has_value()) ? 1U : 0U;
        disabled_garrisons += !unit.state.garrison_enabled ? 1U : 0U;
        rolled += (unit.roll.raw() != 0) ? 1U : 0U;
        contacts += (unit.asteroid_contact.has_value()) ? 1U : 0U;
        nebulas += (unit.nebula.has_value()) ? 1U : 0U;
        storms += (unit.durability && unit.durability->ion_storm_contact) ? 1U : 0U;
        if (unit.formation) ++formation[unit.formation->failure_frame ? 2U : (unit.formation->sliced ? 1U : 0U)];
        approaches += unit.approach ? 1U : 0U;
        sightings += unit.sighting ? 1U : 0U;
        if (unit.combat) for (const auto& weapon : unit.combat->weapons) {
            manual_weapons += weapon.manual ? 1U : 0U;
            turret_count += weapon.turret ? 1U : 0U;
        }
        if (unit.abilities) for (const auto& slot : unit.abilities->slots)
            corrupt_casts += slot.position ? 1U : 0U;
    }

    void add(const CanonicalCounts& other) {
        cloaked += other.cloaked;
        lengthened += other.lengthened;
        scripted += other.scripted;
        mc30_scripts += other.mc30_scripts;
        special_owners += other.special_owners;
        concentrate_owners += other.concentrate_owners;
        buzz_requests += other.buzz_requests;
        corrupt_count += other.corrupt_count;
        blast_count += other.blast_count;
        able += other.able;
        corrupt_casts += other.corrupt_casts;
        disabled_engines += other.disabled_engines;
        stunned += other.stunned;
        manual_weapons += other.manual_weapons;
        turret_count += other.turret_count;
        tracked += other.tracked;
        vulnerable += other.vulnerable;
        token_units += other.token_units;
        modified_count += other.modified_count;
        hazard_moves += other.hazard_moves;
        waypoint_count += other.waypoint_count;
        marked_units += other.marked_units;
        heroes += other.heroes;
        history_count += other.history_count;
        disabled_garrisons += other.disabled_garrisons;
        rolled += other.rolled;
        contacts += other.contacts;
        nebulas += other.nebulas;
        storms += other.storms;
        for (std::size_t group = 0; group < formation.size(); ++group) formation[group] += other.formation[group];
        approaches += other.approaches;
        sightings += other.sightings;
    }
};

using CanonicalParts = std::array<std::vector<std::uint8_t>, tick_partition_count>;

// Large record payloads share the existing publication barrier. Ordered helpers
// append their headers and payloads directly to the final byte vector.
struct CanonicalEncoding {
    CanonicalCounts counts;
    CanonicalParts crafts;
    CanonicalParts minds;
    CanonicalParts projectiles;
    CanonicalParts abilities;
    CanonicalParts trees;
    CanonicalParts projectile_trees;
};

} // namespace eawr::sim::tactical::session_detail
