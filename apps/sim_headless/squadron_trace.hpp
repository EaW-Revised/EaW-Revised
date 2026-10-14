#pragma once

#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/space.hpp"

#include <algorithm>
#include <map>
#include <limits>
#include <ostream>
#include <string_view>

namespace sim_headless {

// Opt-in observation for the existing trace output. Labels describe the
// sampled state; the reason columns distinguish a refusal from ordinary flight.
class SquadronTrace {
public:
    static void header(std::ostream& out) {
        out << "tick,squadron,owner,type,classification,mode,order,target,previous_target,base_target,"
               "formation,complete,reached_done,override,approach,path_routes,idle_cell,members,"
               "chasing,relaxing,movement_locked,scan_due,shots,nearest_enemy,enemy_in_scan_range\n";
    }

    template<class Content>
    void observe(std::ostream& out, const eawr::sim::tactical::TacticalSession& session,
        const Content& content, const std::uint64_t period = 30) {
        namespace tactical = eawr::sim::tactical;
        const auto snapshot = session.snapshot();
        for (const auto& event : snapshot->combat_events()) {
            if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
            for (const auto& squadron : session.squadrons()) {
                if (std::find(squadron.members.begin(), squadron.members.end(), event.shooter) != squadron.members.end()) {
                    ++shots_[squadron.container];
                    break;
                }
            }
        }
        const auto tick = session.completed_tick();
        if (period == 0 || tick % period != 0) return;
        const auto units = session.units();
        const auto find = [&](const eawr::sim::EntityId id) -> const tactical::UnitState* {
            const auto found = std::lower_bound(units.begin(), units.end(), id,
                [](const auto& unit, const auto value) { return unit.entity_id < value; });
            return found != units.end() && found->entity_id == id ? &*found : nullptr;
        };
        for (const auto& squadron : session.squadrons()) {
            const auto state = session.squadron_state(squadron.container);
            const auto* container = find(squadron.container);
            if (!state || !container) continue;
            std::uint64_t chasing = 0, relaxing = 0, locked = 0, scan_due = 0;
            eawr::sim::math::Fixed own_range{};
            for (const auto member : squadron.members) {
                const auto* unit = find(member);
                if (!unit) continue;
                if (const auto flight = session.craft_state(member)) {
                    chasing += flight->chase != 0 ? 1U : 0U;
                    relaxing += flight->relaxation != 0 ? 1U : 0U;
                }
                if (const auto combat = session.combat_state(member)) scan_due += combat->next_scan_frame <= tick ? 1U : 0U;
                if (const auto* profile = content.combat.find(unit->type_id); profile && profile->max_attack_distance)
                    own_range = std::max(own_range, *profile->max_attack_distance);
                const auto abilities = session.ability_state(member);
                const auto* profile = content.abilities.find(unit->type_id);
                if (abilities && profile && tactical::ability_movement_disabled(*profile, *abilities)) ++locked;
            }
            auto allowance = eawr::sim::math::Fixed{};
            if (const auto* profile = content.motion.squadrons.find_squadron(container->type_id)) {
                allowance = state->diversion == tactical::SquadronDiversion::attack_move ? profile->attack_move_response_range
                    : state->mode == tactical::SquadronMode::escort || state->diversion == tactical::SquadronDiversion::guard
                        ? profile->guard_chase_range : profile->idle_chase_range;
            }
            eawr::sim::math::Fixed reach;
            if (!eawr::sim::math::try_add(own_range, allowance, reach)) reach = own_range;
            eawr::sim::EntityId nearest = 0;
            auto nearest_distance = eawr::sim::math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max());
            bool in_range = false;
            for (const auto& enemy : units) {
                if (!tactical::players_hostile(snapshot->players(), container->owner, enemy.owner)) continue;
                eawr::sim::math::Vec3 delta;
                if (!eawr::sim::math::try_subtract(enemy.position.x, container->position.x, delta.x)
                    || !eawr::sim::math::try_subtract(enemy.position.y, container->position.y, delta.y)) continue;
                const auto distance = eawr::sim::math::length(delta);
                if (distance && distance.value() < nearest_distance) { nearest = enemy.entity_id; nearest_distance = distance.value(); }
                in_range = in_range || tactical::within_range(container->position, enemy.position, reach, tactical::RangeMetric::planar);
            }
            const bool live_target = state->target != 0 && find(state->target);
            const auto previous = previous_[squadron.container];
            std::string_view category = "other";
            if (live_target) category = "engaging";
            else if (locked != 0) category = "order-service-gated";
            else if (session.retreat_state().phase != tactical::RetreatPhase::idle
                && session.retreat_state().player == container->owner) category = "retreat-orbit-loop";
            else if ((previous != 0 && !find(previous))
                || (state->formation && state->formation->base_target != 0 && !find(state->formation->base_target))) category = "target-lost";
            else if (state->formation && !state->formation->has_reached_done) category = "formation-never-completed";
            else if (state->target == 0) category = "no-target";
            out << tick << ',' << squadron.container << ',' << container->owner << ',' << container->type_id << ',' << category
                << ',' << static_cast<unsigned>(state->mode) << ',' << tactical::to_string(container->order.kind)
                << ',' << state->target << ',' << previous << ',' << (state->formation ? state->formation->base_target : 0)
                << ',' << state->formation.has_value() << ',' << (state->formation && state->formation->complete)
                << ',' << (state->formation && state->formation->has_reached_done)
                << ',' << (state->formation && state->formation->attack_override) << ',' << state->approach
                << ',' << (state->attack_path ? state->attack_path->stack.size() : 0) << ',' << state->idle_cell.has_value()
                << ',' << squadron.members.size() << ',' << chasing << ',' << relaxing << ',' << locked << ',' << scan_due
                << ',' << shots_[squadron.container] << ',' << nearest << ',' << in_range << '\n';
            previous_[squadron.container] = state->target;
            shots_[squadron.container] = 0;
        }
    }

private:
    std::map<eawr::sim::EntityId, eawr::sim::EntityId> previous_;
    std::map<eawr::sim::EntityId, std::uint64_t> shots_;
};

} // namespace sim_headless
