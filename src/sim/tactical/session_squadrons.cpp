#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"

#include "../math/wide.hpp"
#include "../replay_internal.hpp"
#include "combat_internal.hpp"
#include "blast_internal.hpp"
#include "fighters_internal.hpp"
#include "motion_internal.hpp"
#include "orders_internal.hpp"
#include "staging.hpp"
#include "session_impl.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>


namespace eawr::sim::tactical {

namespace session_detail {

// FO-01, FO-03 (#424, docs/behaviour/space-fighters.md): a player order given to a squadron's
// team container. A move flies to the destination at the craft's layer height and drops the
// target and any escort; a stop holds where the squadron is; an attack makes the unit its target
// (FT-01 keeps it) and holds where the squadron is once it is gone. A face order changes nothing.
// FO-05, FO-06 (#452): an attack-move or guard of a point flies there as a move, of a unit
// follows it as an escort; either drops the target and sets the ranges the squadron diverts within.
void apply_squadron_order(SquadronState& state, const CommandPayload& payload, const math::Vec3& position,
    const SquadronTable& table) {
    if (std::holds_alternative<FacePayload>(payload)) return; // FO-03: preserve the current path.
    math::Fixed layer_z{};
    if (const auto* squadron = table.find_squadron(state.squadron_type); squadron != nullptr && !squadron->members.empty()) {
        if (const auto* craft = table.find_craft(squadron->members.front())) layer_z = craft->layer_z;
    }
    const math::Vec3 here{position.x, position.y, layer_z};
    state.lane.reset(); // FO-10: a new order leaves the formation
    // FM-25 (#687, walk 1 WSQ-11, WSQ-39): every order but a face gives the idle cell up; a stop
    // holds its point without one until the squadron next reverts to idle.
    if (!std::holds_alternative<FacePayload>(payload)) state.idle_cell.reset();
    const auto fly_to = [&](const math::Vec3& destination) {
        state.mode = SquadronMode::move;
        state.move_origin = here;
        state.anchor = {destination.x, destination.y, layer_z};
        state.escorted = invalid_entity_id;
        state.target = invalid_entity_id;
        state.target_hardpoint = attack_hull;
    };
    const auto follow = [&](const EntityId unit) {
        state.mode = SquadronMode::escort;
        state.move_origin = {};
        state.anchor = here; // the squadron frame moves it to the unit (FL-07)
        state.escorted = unit;
        state.target = invalid_entity_id;
        state.target_hardpoint = attack_hull;
    };
    if (const auto* move = std::get_if<MovePayload>(&payload)) {
        fly_to(move->destination);
        state.diversion = SquadronDiversion::idle;
    } else if (std::holds_alternative<StopPayload>(payload)) {
        state.mode = SquadronMode::idle;
        state.move_origin = {};
        state.anchor = here;
        state.escorted = invalid_entity_id;
        state.target = invalid_entity_id;
        state.target_hardpoint = attack_hull;
        state.diversion = SquadronDiversion::idle;
    } else if (const auto* attack = std::get_if<AttackPayload>(&payload)) {
        state.mode = SquadronMode::idle;
        state.move_origin = {};
        state.anchor = here;
        state.escorted = invalid_entity_id;
        state.target = attack->target;
        state.target_hardpoint = attack->hardpoint;
        // FA-07 (#497): the attack order plans the squadron's move to the target, so it starts
        // the approach even when the target is the one the squadron already had.
        state.approach = attack->target != invalid_entity_id;
        state.diversion = SquadronDiversion::idle;
    } else if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) {
        if (attack_move->target != invalid_entity_id) {
            follow(attack_move->target);
        } else {
            fly_to(attack_move->destination);
        }
        state.approach = false;
        state.diversion = SquadronDiversion::attack_move;
    } else if (const auto* guard = std::get_if<GuardPayload>(&payload)) {
        if (guard->target != invalid_entity_id) {
            follow(guard->target);
        } else {
            fly_to(guard->destination);
        }
        state.approach = false;
        state.diversion = SquadronDiversion::guard;
    }
    // WMV-17/18: player movement replaces the base destination. A stop's degenerate
    // position destination completes at once; moving destinations retain their own completion.
    const bool reached_done = state.formation && state.formation->has_reached_done;
    state.formation = SquadronFormationState{state.anchor, state.escorted, false, reached_done, false};
    if (const auto* attack = std::get_if<AttackPayload>(&payload)) state.formation->base_target = attack->target;
    if (std::holds_alternative<StopPayload>(payload)) {
        state.formation->complete = true;
        state.formation->has_reached_done = true;
    }
}

} // namespace session_detail

void TacticalSession::Impl::bind_squadrons(const std::vector<LiveUnit>& live) {
        const auto& table = motion.squadrons;
        if (table.empty()) return;
        const auto unit_at = [&live](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(live.begin(), live.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != live.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        std::set<EntityId> grouped;
        for (const auto& squadron : squadrons) {
            grouped.insert(squadron.container);
            grouped.insert(squadron.members.begin(), squadron.members.end());
        }
        // WHE-SQ-02: represent a parentless fighter's flight group with its own ID. The replay
        // setup and visible unit keep one entity; existing squadron members keep their parent.
        for (const auto& unit : live) {
            if (grouped.contains(unit.state.entity_id) || table.find_craft(unit.state.type_id) == nullptr) continue;
            const auto* solo = table.find_squadron(unit.state.type_id);
            if (solo != nullptr && solo->members.size() == 1 && solo->members.front() == unit.state.type_id)
                squadrons.push_back({unit.state.entity_id, {unit.state.entity_id}});
        }
        std::sort(squadrons.begin(), squadrons.end(), [](const Squadron& a, const Squadron& b) {
            return a.container < b.container;
        });
        for (const auto& squadron : squadrons) {
            const auto* container = unit_at(squadron.container);
            if (container == nullptr || table.find_squadron(container->state.type_id) == nullptr) continue;
            SquadronState state;
            state.container = squadron.container;
            state.squadron_type = container->state.type_id;
            state.roster = squadron.members;
            state.anchor = container->state.position;
            state.formation = SquadronFormationState{container->state.position, invalid_entity_id, true, true, false};
            minds.emplace(squadron.container, std::move(state));
            for (const auto member : squadron.members) {
                const auto* craft = unit_at(member);
                if (craft == nullptr || table.find_craft(craft->state.type_id) == nullptr) continue;
                CraftState flight;
                flight.yaw = yaw_degrees(craft->state.rotation).value();
                crafts.emplace(member, flight);
            }
        }
        for (const auto& unit : live) {
            const auto* profile = table.find_spawner(unit.state.type_id);
            if (profile != nullptr && (!profile->entries.empty() || (!setup.free_garrisons.empty() && profile->starbase))) {
                spawners.emplace(unit.state.entity_id, initial_spawner(setup.seed, 1, unit.state.entity_id));
            }
        }
    }

core::Result<void> TacticalSession::Impl::launch(const LiveUnit& spawner, const SpawnerProfile& profile,
    const SpawnDecision& decision, const std::uint64_t frame, EntityId& next, std::vector<LiveUnit>& survivors,
    std::vector<TacticalInstance>& instances, std::vector<Squadron>& squadron_list,
    detail::MapStage<CraftState>& flights, detail::MapStage<SquadronState>& orders,
    const TypeId free_type, const PlayerId free_owner, std::vector<EntityId>* registered) const {
        using Void = core::Result<void>;
        const auto& table = motion.squadrons;
        const auto company_type = free_type != 0 ? free_type : profile.entries[decision.entry].squadron;
        const auto owner = free_type != 0 ? free_owner : spawner.state.owner;
        const auto* squadron = table.find_squadron(company_type);
        const auto& bay = profile.bays[decision.bay];
        const auto context = "tick " + std::to_string(frame) + " spawner " + std::to_string(spawner.state.entity_id) + ": ";
        const auto transform = math::to_matrix(spawner.state.rotation, spawner.state.position);
        if (!transform) return Void::failure(transform.error());
        const auto position = math::transform_point(transform.value(), bay.position);
        auto direction = math::transform_vector(transform.value(), bay.spawn_vector);
        if (!position || !direction) {
            return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, context + "bay point overflows"));
        }
        if (direction.value() == math::Vec3{}) {
            direction = math::transform_vector(transform.value(), {math::Fixed::from_raw(math::Fixed::scale), {}, {}});
        }
        const auto add = [&](const UnitState& state) -> Void {
            const auto placed = math::to_matrix(state.rotation, state.position);
            if (!placed) return Void::failure(placed.error());
            auto unit = new_unit(state, frame);
            instances.push_back(instance_for(unit, placed.value(), frame));
            survivors.push_back(std::move(unit));
            next = state.entity_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : state.entity_id + 1;
            return Void::success();
        };
        if (free_type != 0 && (squadron == nullptr || table.find_craft(company_type) != nullptr)) {
            if (next == invalid_entity_id) return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit,
                context + "stable ID space exhausted"));
            auto at = position.value();
            const auto* footprint = economy.footprint_of(company_type);
            const auto* solo = table.find_craft(company_type);
            const auto height = solo != nullptr ? solo->layer_z : footprint != nullptr ? footprint->layer_z : math::Fixed{};
            const auto raised = math::add(at.z, height);
            if (!raised) return Void::failure(raised.error());
            at.z = raised.value();
            const auto id = next;
            math::Quat rotation = spawner.state.rotation;
            if (solo != nullptr) {
                auto flight = launch_state(direction.value(), solo->max_speed);
                if (!flight) return Void::failure(flight.error());
                auto facing = craft_rotation(flight.value());
                if (!facing) return Void::failure(facing.error());
                rotation = facing.value();
                flights[id] = flight.value();
                squadron_list.push_back({id, {id}});
                SquadronState order;
                order.container = id;
                order.squadron_type = company_type;
                order.roster = {id};
                order.anchor = at;
                order.next_scan_frame = frame + logical_frames_per_second;
                orders.emplace(id, std::move(order));
            }
            if (auto added = add(UnitState{id, company_type, owner, at, rotation, {}}); !added) return added;
            registered->push_back(id);
            return Void::success();
        }
        std::vector<EntityId> members;
        math::Fixed layer_z{};
        for (const auto type : squadron->members) {
            if (next == invalid_entity_id) {
                return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit, context + "stable ID space exhausted"));
            }
            const auto* craft = table.find_craft(type);
            auto flight = launch_state(direction.value(), craft->max_speed);
            if (!flight) return Void::failure(flight.error());
            auto rotation = craft_rotation(flight.value());
            if (!rotation) return Void::failure(rotation.error());
            if (members.empty()) layer_z = craft->layer_z;
            // space-movement LZ-01: a craft is created at the bay raised by its own Layer_Z_Adjust;
            // the team container is not (LZ-02).
            auto at = position.value();
            const auto raised = math::add(at.z, craft->layer_z);
            if (!raised) return Void::failure(raised.error());
            at.z = raised.value();
            const auto id = next;
            if (auto added = add(UnitState{id, type, owner, at, rotation.value(), {}}); !added) {
                return added;
            }
            flights[id] = flight.value();
            members.push_back(id);
            if (registered != nullptr) registered->push_back(id);
        }
        if (next == invalid_entity_id) {
            return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit, context + "stable ID space exhausted"));
        }
        const auto container = next;
        if (auto added = add(UnitState{container, squadron->type_id, owner, position.value(),
                math::identity_quat(), {}}); !added) {
            return added;
        }
        squadron_list.push_back(Squadron{container, members});
        SquadronState order;
        order.container = container;
        order.squadron_type = squadron->type_id;
        order.spawner = free_type != 0 ? invalid_entity_id : spawner.state.entity_id;
        order.entry = decision.entry;
        order.roster = members;
        order.next_scan_frame = frame + logical_frames_per_second; // FT-06: a launched squadron idles first
        if (profile.mobile) {
            order.mode = SquadronMode::escort;
            order.escorted = spawner.state.entity_id;
            order.anchor = {spawner.state.position.x, spawner.state.position.y, layer_z};
        } else {
            const auto& at = position.value();
            const auto& along = direction.value();
            const auto x = math::add(at.x, along.x);
            const auto y = math::add(at.y, along.y);
            if (!x || !y) {
                return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, context + "bay point overflows"));
            }
            order.anchor = {x.value(), y.value(), layer_z};
        }
        order.formation = SquadronFormationState{order.anchor, order.escorted, !profile.mobile, !profile.mobile, false};
        orders.emplace(container, std::move(order));
        return Void::success();
    }

} // namespace eawr::sim::tactical
