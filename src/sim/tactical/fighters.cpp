#include "eawr/sim/tactical/fighters.hpp"

#include "eawr/sim/math/math.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/motion.hpp"
#include "../math/wide.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "tactical_internal.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>

#include "fighters_algorithms.hpp"

namespace eawr::sim::tactical {
using namespace fighters_detail;

const CraftProfile* SquadronTable::find_craft(const TypeId type_id) const noexcept { return find_by_type(craft, type_id); }
const SquadronProfile* SquadronTable::find_squadron(const TypeId type_id) const noexcept {
    return find_by_type(squadrons, type_id);
}
const SpawnerProfile* SquadronTable::find_spawner(const TypeId type_id, const PlayerId owner) const noexcept {
    if (const auto found = campaign_spawners.find({type_id, owner}); found != campaign_spawners.end()) return &found->second;
    return find_by_type(spawners, type_id);
}

core::Result<Fixed> formation_member_deceleration(const SquadronProfile& profile,
    const Fixed team_base_max_speed, const Fixed speed_factor) {
    Calc calc;
    Fixed result = profile.formation_deceleration;
    if (profile.formation_type_max_speed.raw() > 0) {
        // WMV-44: only the base speed is constrained by the live roster; modifiers belong
        // to this formation member itself. An explicitly zero type maximum returns raw data.
        const Fixed ratio = calc.div(std::min(team_base_max_speed, profile.formation_type_max_speed),
            profile.formation_type_max_speed);
        result = calc.mul(calc.mul(result, ratio), speed_factor);
    }
    if (!calc.ok()) return core::Result<Fixed>::failure(calc.error("formation member deceleration"));
    return core::Result<Fixed>::success(result);
}

core::Result<void> validate_squadron_table(const SquadronTable& table) {
    if (!increasing(table.craft) || !increasing(table.squadrons) || !increasing(table.spawners)) {
        return invalid("type IDs must strictly increase");
    }
    if (!rate(table.side_error_min, false) || !rate(table.side_error_max, false)) {
        return invalid("a formation side error is out of range");
    }
    for (const auto& craft : table.craft) {
        const auto label = "craft type " + std::to_string(craft.type_id);
        if (!rate(craft.max_speed, true) || !rate(craft.min_speed, false) || !rate(craft.rate_of_turn, true)
            || !rate(craft.lift, false) || !rate(craft.thrust, true) || !rate(craft.roll_rate, false)
            || !distance(craft.strafe_distance)
            || !distance(craft.follow_distance) || !distance(craft.attack_distance)
            || craft.out_of_combat_defense.raw() > Fixed::scale
            || craft.out_of_combat_defense.raw() < -max_out_of_combat_defense * Fixed::scale
            || craft.layer_z.raw() < -max_motion_coordinate * Fixed::scale
            || craft.layer_z.raw() > max_motion_coordinate * Fixed::scale) {
            return invalid(label + ": a rate or distance is out of range");
        }
        if (craft.spin_away && (craft.spin_away->chance.raw() < 0 || craft.spin_away->chance > whole(1)
                                || craft.spin_away->time.raw() < 0 || craft.spin_away->time > whole(max_spin_seconds))) {
            return invalid(label + ": a spin-away chance is in [0, 1] and its time in [0, 60] seconds");
        }
    }
    for (const auto& squadron : table.squadrons) {
        const auto label = "squadron type " + std::to_string(squadron.type_id);
        if (squadron.max_squad_size == 0 || squadron.members.empty() || squadron.offsets.size() != squadron.members.size()) {
            return invalid(label + ": at least one member, one normalized offset each");
        }
        for (const auto member : squadron.members) {
            if (find_by_type(table.craft, member) == nullptr) {
                return invalid(label + ": member type " + std::to_string(member) + " is not a craft of the table");
            }
        }
        if (!std::all_of(squadron.offsets.begin(), squadron.offsets.end(), point) || !distance(squadron.guard_chase_range)
            || !distance(squadron.idle_chase_range) || !distance(squadron.attack_move_response_range)
            || !distance(squadron.formation_tolerance) || !distance(squadron.autonomous_move_extension_vs_attacker)
            || !distance(squadron.formation_deceleration) || !distance(squadron.formation_type_max_speed)) {
            return invalid(label + ": an offset or range is out of range");
        }
    }
    for (const auto& spawner : table.spawners) {
        const auto label = "spawner type " + std::to_string(spawner.type_id);
        // FL-02: dynamic lists; only the draw and stored index width limits their count.
        if (spawner.entries.size() > std::numeric_limits<std::uint32_t>::max()
            || spawner.bays.size() > std::numeric_limits<std::uint32_t>::max()) {
            return invalid(label + ": entry and bay counts must fit in uint32");
        }
        for (const auto& entry : spawner.entries) {
            if (entry.starting < 0 || entry.reserve < unlimited_reserve) {
                return invalid(label + ": negative spawn count");
            }
            if (entry.reserve >= 0 && entry.starting > std::numeric_limits<std::int32_t>::max() - entry.reserve) {
                return invalid(label + ": starting plus reserve exceeds int32");
            }
        }
        for (const auto& bay : spawner.bays) {
            if (!point(bay.position) || !point(bay.spawn_vector)) return invalid(label + ": a bay point is out of range");
        }
        for (const auto& entry : spawner.entries) {
            // WSL-16a: a ship entry is a single ship of the campaign scene, not a squadron profile.
            if (!entry.ship && find_by_type(table.squadrons, entry.squadron) == nullptr) {
                return invalid(label + ": entry squadron type " + std::to_string(entry.squadron) + " is not in the table");
            }
        }
        if (spawner.delay_frames > 30U * 3600U) return invalid(label + ": the delay exceeds an hour");
    }
    if (!table.campaign_spawners.empty()) {
        auto selected = table; selected.campaign_spawners.clear();
        for (const auto& [key, profile] : table.campaign_spawners) {
            if (key.second == 0 || key.first != profile.type_id) return invalid("invalid campaign spawner binding");
            selected.spawners = {profile};
            if (auto valid = validate_squadron_table(selected); !valid) return valid;
        }
    }
    return core::Result<void>::success();
}


} // namespace eawr::sim::tactical
