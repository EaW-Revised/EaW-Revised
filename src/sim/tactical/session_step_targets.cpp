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
#include "session_tick.hpp"
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

core::Result<void> session_detail::Tick::targets() {
    auto& moving = gather_.value().moving.value();
    const auto& starts = gather_.value().starts.value();
    auto& minds = craft_prep_.value().minds.value();
    auto& squadron_frames = craft_prep_.value().squadron_frames.value();
    const auto& craft_squadron = craft_prep_.value().craft_squadron.value();
    const auto& arrivals = staging_.value().arrivals.value();
    // Targeting phase (#73): after movement and before this tick's commands, so an attack order
    // acts from the next frame like a move. Workers read one immutable world view (the moved
    // units, their health and the last published visibility) and write each unit's combat state
    // and events to its own slot; the serial commit below appends the events in ascending ID.
    targets_.emplace();
    auto& world = targets_.value().world.emplace(impl_->combat_world(moving, starts, tick, executor));
    if (!world) {
        return core::Result<void>::failure(world.error());
    }
    // WSQ-45/47: reuse the immutable membership index prepared for movement. Member scans
    // must not walk every squadron roster to normalize each candidate (ADR-009).
    world.value().craft_containers = &craft_squadron;
    // #530 PU-37: a hidden arriving unit is seen only by its own team. Serial over the arrivals.
    for (const auto& [id, arrival] : arrivals) {
        if (arrival.frame >= arrival_visible_frame) continue;
        auto& units = world.value().units;
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const detail::CombatUnit& unit, const EntityId value) { return unit.id < value; });
        if (found != units.end() && found->id == id) {
            found->visible_to = 0;
            found->in_limbo = true;
        }
    }
    // CO-11: workers compute each unit's box into its own slot; the collection trees then take the
    // boxes serially in ascending ID, as FoC's trees take each object's transform update in turn
    // (the tree's order is its history, so this commit is ordered by nature: O(units) with a short
    // tree walk each). The targeting phase only reads the trees.
    auto& collection = targets_.value().collection.emplace(impl_->collection);
    auto& projectile_collection = targets_.value().projectile_collection.emplace(impl_->projectile_collection);
    if (!impl_->combat.profiles.empty()) {
        std::vector<std::optional<detail::CollectionTrees::Member>> boxes(moving.size());
        std::vector<std::optional<core::Diagnostic>> box_errors(moving.size());
        const auto boxed = executor.execute_phase("collection-boxes", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto member = impl_->collection_member(moving[index]);
                if (!member) {
                    box_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(moving[index].state.entity_id) + ": "
                            + member.error().message);
                    continue;
                }
                boxes[index] = member.value();
            }
        });
        if (!boxed) {
            return core::Result<void>::failure(boxed.error());
        }
        for (auto& error : box_errors) {
            if (error) {
                return core::Result<void>::failure(std::move(*error));
            }
        }
        mark("collection_boxes_commit", true);
        std::vector<detail::CollectionTrees::Member> members;
        std::vector<detail::CollectionTrees::Member> collidables;
        members.reserve(moving.size());
        collidables.reserve(moving.size());
        for (std::size_t index = 0; index < boxes.size(); ++index) {
            const auto& box = boxes[index];
            if (!box) continue;
            members.push_back(*box);
            // DG-30: share partitioned box preparation, never targeting membership/history.
            const auto* profile = world.value().units[index].profile;
            if (impl_->durability.damage && profile != nullptr && profile->living_projectile_collision && profile->collision) {
                collidables.push_back(*box);
            }
        }
        collection.update(members, tick);
        projectile_collection.update(collidables, tick);
        mark("collection_boxes_commit", false);
        world.value().collection = &collection;
        world.value().projectile_collection = &projectile_collection;
    }
    const auto& view = world.value();

    // WSQ-42/43: space targeting belongs to each member, not the team container. Prepare
    // member scans in parallel, then share only sparse changes in service order (ADR-009).
    struct MemberScan {
        std::size_t index{};
        CombatState state;
    };
    std::array<std::vector<MemberScan>, tick_partition_count> scans;
    std::map<EntityId, std::pair<std::size_t, EntityId>> acquired;
    if (!squadron_frames.empty() && !impl_->combat.profiles.empty()) {
        std::vector<std::pair<EntityId, const SquadronFrame*>> scan_teams;
        scan_teams.reserve(squadron_frames.size());
        for (const auto& [container, frame] : squadron_frames) scan_teams.emplace_back(container, &frame);
        const auto scanned = executor.execute_phase("squadron-targets", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, scan_teams.size());
            for (auto team_index = range.begin; team_index < range.end; ++team_index) {
                const auto [container_id, frame_pointer] = scan_teams[team_index];
                const auto& frame = *frame_pointer;
                if (arrivals.contains(container_id)) continue;
                if (frame.profile == nullptr || !frame.scans) continue; // FO-01/05/06
                if (view.frame < frame.state->next_scan_frame) continue; // FT-06: retain launch admission delay
                const auto* container = live_unit(moving, container_id);
                bool player_attack = false;
                if (container != nullptr && container->state.order.kind == OrderKind::attack) {
                    const auto order_target = container->state.order.target;
                    const auto parent = view.container_of(order_target);
                    player_attack = (parent != invalid_entity_id ? parent : order_target) == frame.state->target
                        && frame.state->target != invalid_entity_id;
                }
                auto divert = frame.profile->idle_chase_range;
                if (frame.state->diversion == SquadronDiversion::attack_move) divert = frame.profile->attack_move_response_range;
                else if (frame.state->mode == SquadronMode::escort || frame.state->diversion == SquadronDiversion::guard)
                    divert = frame.profile->guard_chase_range;
                detail::FormationAttackContext formation;
                formation.present = frame.state->formation.has_value();
                formation.leader = view.find(frame.leader);
                auto anchor = frame.hold;
                if (frame.state->formation) {
                    const auto& held = *frame.state->formation;
                    anchor = held.base_position;
                    if (const auto* destination = view.find(held.base_target)) anchor = destination->position;
                    const bool escort = frame.state->mode == SquadronMode::escort;
                    formation.unbounded = !held.complete && frame.state->diversion == SquadronDiversion::attack_move;
                    const auto parent_position = container != nullptr ? container->state.position : anchor;
                    formation.allows_divert = formation.unbounded || ((held.has_reached_done || escort)
                        && (held.base_target == invalid_entity_id || escort)
                        && (held.complete || within_range(parent_position, anchor, divert, RangeMetric::planar)));
                }
                auto members = frame.state->roster;
                std::sort(members.begin(), members.end());
                auto shared_target = frame.state->target;
                for (const auto member_id : members) {
                    const auto own = craft_squadron.find(member_id);
                    const auto* member_unit = view.find(member_id);
                    if (own == craft_squadron.end() || own->second != container_id || member_unit == nullptr) continue;
                    const auto index = static_cast<std::size_t>(member_unit - view.units.data());
                    if (!moving[index].combat) continue;
                    auto unit = view.units[index];
                    std::optional<CombatState> adopted;
                    // WSQ-41/47: a player attack of the container is shared with the members.
                    // Keep this preparation partitioned; autonomous finds use the sparse share below.
                    if (player_attack) {
                        adopted = *unit.combat;
                        adopted->attack_target = frame.state->target;
                        adopted->attack_hardpoint = frame.state->target_hardpoint;
                        adopted->direct = true;
                        unit.combat = &*adopted;
                    }
                    unit.squadron_idle = false;
                    auto state = detail::target_combat(view, unit, divert, frame.state->target, anchor, &formation);
                    const auto& previous = *moving[index].combat;
                    if (state.attack_target != previous.attack_target || state.next_scan_frame != previous.next_scan_frame
                        || state.direct != previous.direct || state.attack_hardpoint != previous.attack_hardpoint) {
                        scans[partition].push_back({index, std::move(state)});
                        const auto selected = scans[partition].back().state.attack_target;
                        if (selected == invalid_entity_id) {
                            if (previous.attack_target != invalid_entity_id && shared_target == previous.attack_target)
                                shared_target = invalid_entity_id;
                        } else {
                            const auto parent = view.container_of(selected);
                            // The first share discards all later speculative scans and clock writes.
                            // Stop computing them too; the ordered commit retains the same behavior.
                            if ((parent != invalid_entity_id ? parent : selected) != shared_target) break;
                        }
                    }
                }
            }
        });
        if (!scanned) return core::Result<void>::failure(scanned.error());
        std::set<EntityId> shared;
        std::vector<MemberScan> scan_commit;
        for (auto& part : scans) for (auto& scan : part) scan_commit.push_back(std::move(scan));
        std::sort(scan_commit.begin(), scan_commit.end(), [](const MemberScan& left, const MemberScan& right) {
            return left.index < right.index;
        });
        // Squad tasks can interleave IDs. Preserve the global ascending-ID sharing/loss order.
        for (auto& scan : scan_commit) {
            const auto container = craft_squadron.at(moving[scan.index].state.entity_id);
            if (shared.contains(container)) continue;
            const auto old_target = moving[scan.index].combat->attack_target;
            moving[scan.index].combat = std::move(scan.state);
            auto& member = *moving[scan.index].combat;
            auto& state = minds.at(container);
            if (member.attack_target == invalid_entity_id) {
                if (old_target != invalid_entity_id && state.target == old_target) {
                    // WSQ-44: loss of the held target is shared too; flight gives up its cell.
                    state.target = invalid_entity_id;
                    state.target_hardpoint = attack_hull;
                    state.approach = false;
                    end_combat(container, state, craft_view(squadron_frames.at(container).leader));
                    hand_target(container, old_target, invalid_entity_id);
                }
                continue;
            }
            const auto parent = view.container_of(member.attack_target);
            const auto selected = parent != invalid_entity_id ? parent : member.attack_target; // WSQ-47
            member.attack_target = selected;
            const auto previous = state.target;
            if (selected == previous) continue;
            state.target = selected;
            state.target_hardpoint = member.direct ? member.attack_hardpoint : attack_hull;
            state.approach = true;
            if (previous != invalid_entity_id) end_combat(container, state, craft_view(squadron_frames.at(container).leader));
            // WSQ-46: the team setter passes its player-direct flag as both direct and pursue.
            // A player pursuit already has a formation destination; an autonomous mismatch
            // notifies the team locomotor, which adopts it and dispatches WSQ-20 to the roster.
            if (!member.direct && squadron_frames.contains(selected)) {
                acquired.emplace(container, std::pair{acquired.size(), selected});
            }
            hand_target(container, previous, selected, state.target_hardpoint, member.direct);
            shared.insert(container);
        }
    }
    std::vector<std::optional<detail::CombatStep>> combat_steps(moving.size());
    std::vector<std::optional<core::Diagnostic>> combat_errors(moving.size());
    struct Pairing {
        std::size_t notification{}, roster_slot{};
        EntityId self{};
        std::vector<EntityId> candidates;
    };
    std::array<std::vector<Pairing>, tick_partition_count> pairings;
    auto& crafts = staging_.value().crafts.value();
    const auto targeted = executor.execute_phase("targeting", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, moving.size());
        for (auto index = range.begin; index < range.end; ++index) {
            // #530 PU-39: an arriving unit neither targets nor fires.
            if (!moving[index].combat || arrivals.contains(moving[index].state.entity_id)) {
                continue;
            }
            // FT-07: a craft holds fire while its squadron (committed above) has no target.
            auto unit = view.units[index];
            const auto squadron = craft_squadron.find(unit.id);
            unit.squadron_idle = squadron != craft_squadron.end() && minds.read(squadron->second).target == invalid_entity_id;
            unit.target_prepared = squadron != craft_squadron.end();
            if (squadron != craft_squadron.end()) {
                const auto notification = acquired.find(squadron->second);
                const auto self = crafts.find(unit.id);
                if (notification != acquired.end() && self != crafts.end() && self->second.chase_until <= tick + 1) {
                    const auto& roster = minds.read(squadron->second).roster;
                    Pairing pairing{notification->second.first,
                        static_cast<std::size_t>(std::find(roster.begin(), roster.end(), unit.id) - roster.begin()), unit.id, {}};
                    // WSQ-20: elapsed timers and target roster order; no cell or follow cone.
                    for (const auto member : minds.read(notification->second.second).roster) {
                        const auto candidate = crafts.find(member);
                        const auto* target = view.find(member);
                        if (candidate != crafts.end() && candidate->second.chase_until <= tick + 1
                            && target != nullptr && view.hostile(unit.owner, target->owner)) pairing.candidates.push_back(member);
                    }
                    if (!pairing.candidates.empty()) pairings[partition].push_back(std::move(pairing));
                }
            }
            auto stepped = detail::step_combat(view, unit);
            if (!stepped) {
                combat_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " unit " + std::to_string(moving[index].state.entity_id) + ": "
                        + stepped.error().message);
                continue;
            }
            combat_steps[index] = std::move(stepped).value();
        }
    });
    if (!targeted) {
        return core::Result<void>::failure(targeted.error());
    }
    std::vector<Pairing> pairing_commit;
    for (auto& part : pairings) for (auto& pairing : part) pairing_commit.push_back(std::move(pairing));
    std::sort(pairing_commit.begin(), pairing_commit.end(), [](const Pairing& left, const Pairing& right) {
        return std::pair{left.notification, left.roster_slot} < std::pair{right.notification, right.roster_slot};
    });
    // Sparse cross-craft writes, not a serial world scan: a prior recipient cannot be taken again.
    for (const auto& pairing : pairing_commit) {
        if (crafts.read(pairing.self).chase_until > tick + 1) continue;
        for (const auto candidate : pairing.candidates) {
            if (crafts.read(candidate).chase_until > tick + 1) continue;
            auto& self = crafts.at(pairing.self);
            auto& chased = crafts.at(candidate);
            self.chase = candidate;
            self.chase_until = tick + 1 + chase_frames;
            chased.chase = invalid_entity_id;
            chased.chase_until = self.chase_until;
            break;
        }
    }
    for (auto& error : combat_errors) {
        if (error) {
            return core::Result<void>::failure(std::move(*error));
        }
    }
    auto& combat_events = targets_.value().combat_events.emplace();
    auto& aim_offsets = targets_.value().aim_offsets.emplace();
    // A-04 to A-07: the turns toward ordered targets, in ascending unit ID; planned below.
    auto& combat_turns = targets_.value().combat_turns.emplace();
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (!combat_steps[index]) {
            continue;
        }
        if (combat_steps[index]->face) {
            combat_turns.emplace_back(moving[index].state.entity_id, *combat_steps[index]->face);
        }
        moving[index].combat = std::move(combat_steps[index]->state);
        // EN-05: what the object weapon fired is paid from the pool it was checked against.
        if (combat_steps[index]->energy_spent.raw() > 0 && moving[index].durability) {
            auto& pool = moving[index].durability->energy;
            pool = math::Fixed::from_raw(pool.raw() - combat_steps[index]->energy_spent.raw());
        }
        // WAD-40: worker outputs record clocks in canonical shooter/weapon order.
        for (const auto& shot : combat_steps[index]->manual_fired) {
            manual_clocks_.insert_or_assign(shot.player, ManualPlayerClock{tick, shot.cooldown_frames});
        }
        for (const auto& feedback : combat_steps[index]->manual_feedback) {
            manual_feedback_.push_back(Event{tick, EventKind::manual_target_timeout, feedback.player, 0,
                moving[index].state.entity_id, OrderKind::manual_target, RejectReason::none,
                static_cast<std::uint8_t>(feedback.hardpoint)});
        }
        auto& produced = combat_steps[index]->events;
        // AB-67 (#561): a craft that fired its ion shot at the target has fired it.
        for (const auto& event : produced) {
            if (event.kind != CombatEventKind::weapon_fired || (event.outcome & fired_ability_shot) == 0U) continue;
            if (auto* slot = impl_->ion_slot(moving[index])) slot->active = false;
        }
        combat_events.insert(combat_events.end(), produced.begin(), produced.end());
        const auto& offsets = combat_steps[index]->aim_offsets;
        aim_offsets.insert(aim_offsets.end(), offsets.begin(), offsets.end());
    }

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::orders() {
    auto& moving = gather_.value().moving.value();
    const auto& world = targets_.value().world.value();
    const auto& view = world.value();
    // Orders phase (#452, docs/behaviour/space-orders.md OR-06): every reevaluation interval after
    // its order, a unit approaching another (attack, attack-move or guard on a unit) checks whether
    // it keeps its movement. Workers read the moved units with this tick's targets and write each
    // unit's new approach to its own slot; the paths plan serially in ascending ID after the
    // attack turns, like every plan with avoidance (AV-15).
    std::vector<std::optional<Impl::ApproachPlan>> approach_plans(moving.size());
    bool approaches_due = false;
    for (const auto& unit : moving) {
        approaches_due = approaches_due || impl_->approach_due(unit, tick);
    }
    if (approaches_due) {
        const auto find_live = [&moving](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(moving.begin(), moving.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != moving.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        std::vector<std::optional<core::Diagnostic>> approach_errors(moving.size());
        const auto approached = executor.execute_phase("orders", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = moving[index];
                if (!impl_->approach_due(unit, tick)) continue;
                const auto target_id = Impl::approach_target(unit);
                const auto* target = find_live(target_id);
                if (target == nullptr) continue;
                // A target fogged to the unit's owner is not followed (research E452-11).
                const auto* target_view = view.find(target_id);
                const auto* unit_view = view.find(unit.state.entity_id);
                if (target_view != nullptr && unit_view != nullptr && target_view->team != unit_view->team
                    && (target_view->visible_to & (std::uint64_t{1} << unit_view->player_index)) == 0U) {
                    continue;
                }
                auto checked = impl_->approach_check(unit, *target, target_view, tick + 1);
                if (!checked) {
                    approach_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, "tick "
                        + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": "
                        + checked.error().message);
                    continue;
                }
                approach_plans[index] = checked.value();
            }
        });
        if (!approached) {
            return core::Result<void>::failure(approached.error());
        }
        for (auto& error : approach_errors) {
            if (error) {
                return core::Result<void>::failure(std::move(*error));
            }
        }
    }
    orders_.emplace();
    auto& approaches = orders_.value().approaches.emplace();
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (approach_plans[index]) approaches.emplace_back(moving[index].state.entity_id, *approach_plans[index]);
    }

    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
