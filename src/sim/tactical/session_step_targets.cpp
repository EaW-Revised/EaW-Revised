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

namespace {

// WMV-13 (debug build, code constant): the planar distance from a lost target's last observed
// position within which a unit gives its order on that target up.
constexpr std::int64_t lost_sight_reach = 40;

} // namespace

core::Result<void> session_detail::Tick::prepare_targets() {
    auto& moving = gather_.value().moving.value();
    const auto& starts = gather_.value().starts.value();
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
        std::vector<std::optional<detail::CollectionTrees::Member>> projectile_boxes(moving.size());
        std::vector<std::optional<core::Diagnostic>> box_errors(moving.size());
        const auto boxed = executor.execute_phase("collection-boxes", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                // A-07/WSQ-45: ordinary space team locomotion uses type attack
                // extents/radius. A movement formation does not activate the
                // specialized formation-team geometry reader.
                auto member = impl_->collection_member(moving[index]);
                if (!member) {
                    box_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(moving[index].state.entity_id) + ": "
                            + member.error().message);
                    continue;
                }
                boxes[index] = member.value();
                const auto* profile = world.value().units[index].profile;
                if (impl_->durability.damage && profile != nullptr && profile->living_projectile_collision && profile->collision) {
                    auto projectile_member = impl_->collection_member(moving[index], true);
                    if (!projectile_member) {
                        box_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                            "tick " + std::to_string(tick) + " unit " + std::to_string(moving[index].state.entity_id) + ": "
                                + projectile_member.error().message);
                        continue;
                    }
                    projectile_boxes[index] = projectile_member.value();
                }
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
            // DG-36b: only projectile candidates enclose attached geometry; the ordinary
            // targeting/blast collection keeps the parent's authored bounds and history.
            if (projectile_boxes[index]) collidables.push_back(*projectile_boxes[index]);
        }
        collection.update(members, tick);
        projectile_collection.update(collidables, tick);
        mark("collection_boxes_commit", false);
        world.value().collection = &collection;
        world.value().projectile_collection = &projectile_collection;
    }
    targets_->combat_events.emplace();
    targets_->aim_offsets.emplace();
    targets_->combat_turns.emplace();
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::targets() {
    auto& moving = gather_->moving.value();
    auto& minds = craft_prep_->minds.value();
    auto& squadron_frames = craft_prep_->squadron_frames.value();
    const auto& craft_squadron = craft_prep_->craft_squadron.value();
    const auto& arrivals = staging_->arrivals.value();
    if (std::any_of(moving.begin(), moving.end(), [](const LiveUnit& unit) {
        return unit.abilities && unit.abilities->pending_buzz_position;
    })) {
        // FS-23/FC-51: a retained team special attack is attempted before ordinary targeting,
        // independently of the team's scan deadline. UFS-21: a failed request whose
        // approach ended is dropped without recharge (an inferred project policy).
        struct BuzzDecision { std::size_t index; bool apply; };
        std::array<std::vector<BuzzDecision>, tick_partition_count> buzz_requests;
        const auto retried = executor.execute_phase("buzz-request-targeting", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& source = moving[index];
                if (!source.abilities || !source.abilities->pending_buzz_position
                    || arrivals.contains(source.state.entity_id)) continue;
                const auto mind = minds.find(source.state.entity_id);
                if (mind == minds.end()) {
                    buzz_requests[partition].push_back({index, false});
                    continue;
                }
                const bool ready = impl_->buzz_request_ready(source, *source.abilities->pending_buzz_position, tick);
                const auto& team = mind->second;
                const auto point = *source.abilities->pending_buzz_position;
                const bool approaching = team.mode == SquadronMode::move && team.formation
                    && !team.formation->complete && team.formation->base_position.x == point.x
                    && team.formation->base_position.y == point.y;
                if (ready || !approaching)
                    buzz_requests[partition].push_back({index, ready});
            }
        });
        if (!retried) return retried;
        auto& next_id = staging_->next_id.value();
        std::vector<LiveUnit> droids;
        // Partition ranges and slots are ascending ID; only successful applications allocate IDs
        // and change shared ownership/registration state (WAB-71/FS-08).
        for (const auto& requests : buzz_requests) for (const auto& decision : requests) {
            auto& source = moving[decision.index];
            if (!decision.apply) {
                source.abilities->pending_buzz_position.reset();
                continue;
            }
            if (next_id == invalid_entity_id) continue;
            const auto point = *source.abilities->pending_buzz_position;
            auto droid = impl_->create_buzz_droids(source, point, tick, next_id);
            track_victory_change(nullptr, &droid.state, false);
            droids.push_back(std::move(droid));
            apply_squadron_order(minds.at(source.state.entity_id), StopPayload{}, source.state.position, impl_->motion.squadrons);
            auto& frame = squadron_frames.at(source.state.entity_id);
            frame.moving = false;
            frame.scans = true;
            frame.approach = false;
            frame.attacking = false;
        }
        moving.insert(moving.end(), std::make_move_iterator(droids.begin()), std::make_move_iterator(droids.end()));
    }
    // WCC-25: rebuild immutable combat inputs after the existing-projectile commit.
    // Preserve prepared geometry and tree history; only health/ability inputs changed.
    auto refreshed = impl_->combat_world(moving, gather_->starts.value(), tick, executor);
    if (!refreshed) return core::Result<void>::failure(refreshed.error());
    auto& world = targets_->world.value();
    const auto preserved = executor.execute_phase("combat-input-refresh", tick_partition_count, [&](const std::size_t partition) {
        auto& units = refreshed.value().units;
        const auto range = partition_range(partition, units.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& unit = units[index];
            if (const auto* previous = world.value().find(unit.id)) {
                unit.formation_footprint = previous->formation_footprint;
                unit.visible_to = previous->visible_to;
                unit.in_limbo = previous->in_limbo;
            }
        }
    });
    if (!preserved) return preserved;
    world = std::move(refreshed);
    world.value().craft_containers = &craft_squadron;
    world.value().collection = &targets_->collection.value();
    world.value().projectile_collection = &targets_->projectile_collection.value();
    const auto& view = world.value();

    // FS-18..20/FD-69: scans have disjoint member outputs. Sharing and tails
    // commit in ordinary launch service rank (last-created member first).
    // Registration changes/restored rank producers remain UFS-14/36.
    struct MemberScan {
        bool present{};
        bool enabled{};
        CombatState before;
        CombatState scanned;
        EntityId destination{};
        detail::FormationAttackContext formation;
        math::Fixed divert{};
        math::Vec3 anchor{};
    };
    std::vector<MemberScan> scans(moving.size());
    std::vector<std::optional<CombatState>> weapon_inputs(moving.size());
    std::vector<bool> weapon_idle(moving.size());
    std::map<EntityId, std::pair<std::size_t, EntityId>> acquired;
    auto scan_view = view;
    scan_view.frame = tick;
    std::map<EntityId, std::vector<CombatState*>> member_states;
    std::map<EntityId, std::vector<std::size_t>> member_indices;
    std::vector<std::size_t> scan_indices;
    for (const auto& [container, frame] : squadron_frames) {
        auto& states = member_states[container];
        for (const auto id : frame.live_roster) {
            if (auto* member = live_unit(moving, id); member != nullptr && member->combat) {
                states.push_back(&*member->combat);
                const auto index = static_cast<std::size_t>(member - moving.data());
                member_indices[container].push_back(index);
                scan_indices.push_back(index);
            }
        }
    }
    // WSQ-42/43: no squadron or no combat table leaves no member scan or phase (as before the
    // ordered commit), so the phase map of a squadron-free session is unchanged.
    std::size_t cursor = squadron_frames.empty() || impl_->combat.profiles.empty() ? 0 : moving.size();
    while (cursor != 0) {
        const auto scanned = executor.execute_phase("squadron-targets", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, scan_indices.size());
            for (auto slot = range.begin; slot < range.end; ++slot) {
                const auto index = scan_indices[slot];
                const auto own = craft_squadron.find(moving[index].state.entity_id);
                if (own == craft_squadron.end() || !moving[index].combat || arrivals.contains(own->second)) continue;
                const auto team = squadron_frames.find(own->second);
                if (team == squadron_frames.end() || team->second.profile == nullptr) continue;
                const auto& frame = team->second;
                const auto& state = minds.read(own->second);
                auto& output = scans[index];
                output.formation = {};
                output.present = true;
                output.before = *moving[index].combat;
                output.formation.present = state.formation.has_value();
                output.formation.leader = view.find(frame.leader);
                output.divert = frame.profile->idle_chase_range;
                if (state.diversion == SquadronDiversion::attack_move)
                    output.divert = frame.profile->attack_move_response_range;
                else if (state.mode == SquadronMode::escort || state.diversion == SquadronDiversion::guard)
                    output.divert = frame.profile->guard_chase_range;
                output.anchor = frame.hold;
                const auto* container = live_unit(moving, own->second);
                if (state.formation) {
                    const auto& held = *state.formation;
                    output.anchor = held.base_position;
                    if (const auto* destination = view.find(held.base_target)) output.anchor = destination->position;
                    const bool escort = state.mode == SquadronMode::escort;
                    output.formation.unbounded = !held.complete && state.diversion == SquadronDiversion::attack_move;
                    const auto position = container != nullptr ? container->state.position : output.anchor;
                    output.formation.allows_divert = output.formation.unbounded || ((held.has_reached_done || escort)
                        && (held.base_target == invalid_entity_id || escort)
                        && (held.complete || within_range(position, output.anchor, output.divert, RangeMetric::planar)));
                }
                // WSQ-41/47: only the team's own attack order shares as the player's: the order's
                // target (or its team container) must be the target the team holds, so an autonomous
                // find after the base target is lost is not taken for it. FT-06: a launched
                // squadron takes no target for its first second, a player's too.
                bool player_attack = false;
                if (view.frame >= state.next_scan_frame && container != nullptr
                    && container->state.order.kind == OrderKind::attack && state.target != invalid_entity_id) {
                    const auto order_target = container->state.order.target;
                    const auto order_parent = view.container_of(order_target);
                    player_attack = (order_parent != invalid_entity_id ? order_parent : order_target) == state.target;
                }
                auto input = output.before;
                if (player_attack) {
                    input.attack_target = state.target;
                    input.attack_hardpoint = state.target_hardpoint;
                    input.direct = true;
                }
                auto unit = view.units[index];
                unit.combat = &input;
                unit.squadron_idle = false;
                output.enabled = frame.scans && view.frame >= state.next_scan_frame;
                const auto target = state.formation ? state.formation->attack_target : invalid_entity_id;
                output.destination = target;
                output.scanned = output.enabled ? detail::target_combat(scan_view, unit, output.divert,
                    target, output.anchor, &output.formation) : input;
            }
        });
        if (!scanned) return core::Result<void>::failure(scanned.error());
        // Only ordered notification/tail commits run here; spatial scans and weapon
        // work stay partitioned. A completed prefix never gets a second object turn.
        while (cursor != 0) {
            const auto index = --cursor;
            const auto& scan = scans[index];
            if (!scan.present) continue;
            const auto container = craft_squadron.at(moving[index].state.entity_id);
            auto& state = minds.at(container);
            auto& member = *moving[index].combat;
            // A share invalidates only unserviced inputs. Refresh that suffix
            // in the partitioned phase; completed members keep one weapon input.
            if (member.attack_target != scan.before.attack_target || member.direct != scan.before.direct
                || member.attack_hardpoint != scan.before.attack_hardpoint
                || scan.destination != (state.formation ? state.formation->attack_target : invalid_entity_id)) {
                ++cursor;
                // Restage only this team's unserviced members. Unrelated teams
                // retain their immutable outputs, so shares do not rescan a world.
                scan_indices.clear();
                for (const auto pending : member_indices.at(container))
                    if (pending < cursor) scan_indices.push_back(pending);
                break;
            }
            const auto old_team_target = state.target;
            auto& roster = member_states.at(container);
            const auto selected_parent = view.container_of(scan.scanned.attack_target);
            const auto selected = selected_parent != invalid_entity_id ? selected_parent : scan.scanned.attack_target;
            const auto other = minds.find(selected);
            const bool same_cell = state.cell && other != minds.end() && other->second.cell
                && state.cell == other->second.cell;
            const auto* parent = view.find(container);
            auto committed = detail::commit_squadron_target(view, view.units[index], scan.scanned,
                member, roster, state, parent != nullptr ? parent->position : scan.anchor,
                scan.divert, scan.anchor, scan.formation, same_cell, scan.enabled);
            if (!committed) return core::Result<void>::failure(committed.error());
            if (committed.value().lost)
                end_combat(container, state, craft_view(squadron_frames.at(container).leader));
            else if (committed.value().shared && old_team_target != invalid_entity_id) {
                // WSQ-64: sharing a live target is not idle reversion. Leave the old
                // combat cell (WSQ-43/49), but keep the held idle destination.
                state.cell.reset();
                state.joined = false;
            }
            if (committed.value().notified && squadron_frames.contains(selected) && !acquired.contains(container))
                acquired.emplace(container, std::pair{acquired.size(), selected});
            const auto destination = state.formation ? state.formation->attack_target : invalid_entity_id;
            weapon_inputs[index] = member;
            weapon_idle[index] = (state.mode == SquadronMode::move && state.diversion == SquadronDiversion::idle)
                || (destination == invalid_entity_id && member.attack_target == invalid_entity_id);
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
            unit.target_prepared = squadron != craft_squadron.end();
            if (squadron != craft_squadron.end()) {
                if (weapon_inputs[index]) {
                    unit.combat = &*weapon_inputs[index];
                    unit.squadron_idle = weapon_idle[index];
                }
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
    auto& combat_events = targets_.value().combat_events.value();
    auto& aim_offsets = targets_.value().aim_offsets.value();
    // A-04 to A-07: the turns toward ordered targets, in ascending unit ID; planned below.
    auto& combat_turns = targets_.value().combat_turns.value();
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (!combat_steps[index]) {
            continue;
        }
        if (combat_steps[index]->face) {
            combat_turns.emplace_back(moving[index].state.entity_id, *combat_steps[index]->face);
        }
        if (weapon_inputs[index]) {
            // Later shares update retained targeting state, but cannot change the
            // weapon input of an already-serviced prefix (FS-42/FD-69).
            const auto& published = *moving[index].combat;
            auto& result = combat_steps[index]->state;
            const auto& input = *weapon_inputs[index];
            if (published.attack_target != input.attack_target || published.attack_hardpoint != input.attack_hardpoint
                || published.direct != input.direct) {
                result.attack_target = published.attack_target;
                result.attack_hardpoint = published.attack_hardpoint;
                result.direct = published.direct;
            }
        }
        moving[index].combat = std::move(combat_steps[index]->state);
        if (moving[index].abilities && moving[index].abilities->charged_blast)
            moving[index].abilities->charged_blast = combat_steps[index]->charged_blast;
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
            // WST-05: any shot of a cloaked unit ends its cloak; the STEALTH countdown runs on.
            if (event.kind == CombatEventKind::weapon_fired && moving[index].abilities) impl_->clear_stealth(moving[index]);
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
    const auto& minds = craft_prep_->minds.value();
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
    const auto find_live = [&moving](const EntityId id) -> const LiveUnit* {
        if (id == invalid_entity_id) return nullptr;
        const auto found = std::lower_bound(moving.begin(), moving.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != moving.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    // WMV-13: the unit's owner sees the target: its own team, else in sight and not hidden by a
    // cloak the unit cannot target (WST-09, T-01's test).
    const auto sees = [](const detail::CombatUnit& unit, const detail::CombatUnit& target) {
        if (target.team == unit.team) return true;
        if (target.stealth_invisible && !(unit.profile != nullptr && unit.profile->target_stealth)) return false;
        return (target.visible_to & (std::uint64_t{1} << unit.player_index)) != 0U;
    };
    if (approaches_due) {
        std::vector<std::optional<core::Diagnostic>> approach_errors(moving.size());
        const auto approached = executor.execute_phase("orders", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = moving[index];
                if (!impl_->approach_due(unit, tick)) continue;
                const auto target_id = Impl::approach_target(unit);
                const auto* target = find_live(target_id);
                if (target == nullptr) continue;
                // A target the unit's owner does not see is not followed (research E452-11); the
                // unit goes to where it last saw it instead (WMV-13, below).
                const auto* target_view = view.find(target_id);
                const auto* unit_view = view.find(unit.state.entity_id);
                if (target_view != nullptr && unit_view != nullptr && !sees(*unit_view, *target_view)) {
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
    // WMV-10, WMV-13 (#719): serial, every frame. A unit whose order names a unit keeps where its
    // owner last saw that unit. Once the owner no longer sees it, the order's point is that last
    // observed position: at the order's reevaluation checks (OR-06), a unit whose movement has
    // ended (no plan left, no group wait, no search running) plans a move there, and within
    // 40 units of it in the plane becomes a move to where it stands and drops the target.
    const auto lost_reach = math::Fixed::from_raw(lost_sight_reach * math::Fixed::scale);
    for (std::size_t index = 0; index < moving.size(); ++index) {
        auto& unit = moving[index];
        const auto target_id = Impl::order_unit(unit);
        const auto* target = find_live(target_id);
        const auto* target_view = target != nullptr ? view.find(target_id) : nullptr;
        const auto* unit_view = target_view != nullptr ? view.find(unit.state.entity_id) : nullptr;
        if (unit_view == nullptr) {
            unit.sighting.reset(); // no order on a unit, or the unit is gone (OR-08)
            continue;
        }
        if (sees(*unit_view, *target_view)) {
            unit.sighting = Sighting{target_id, target->state.position};
            continue;
        }
        if (!unit.sighting || unit.sighting->target != target_id) continue;
        const auto issued = unit.state.order.issued_tick;
        if (tick <= issued || (tick - issued) % impl_->motion.rules.reevaluation_frames != 0) continue;
        if (unit.formation || impl_->searches.contains(unit.state.entity_id)
            || (unit.motion && unit.motion->kind != MotionKind::none)) continue;
        const auto point = unit.sighting->point;
        if (!within_range(unit.state.position, point, lost_reach, RangeMetric::planar)) {
            approach_plans[index] = Impl::ApproachPlan{point, tick + 1};
            continue;
        }
        unit.state.order = order_for(MovePayload{unit.state.position}, tick);
        unit.approach.reset();
        unit.sighting.reset();
        if (unit.combat && unit.combat->attack_target == target_id) {
            unit.combat->attack_target = invalid_entity_id;
            unit.combat->attack_hardpoint = no_hardpoint;
            unit.combat->direct = false;
        }
    }
    orders_.emplace();
    // HCB-59 (FS-37/FC-66): after locomotion and targeting,
    // completion reads the moved snapshot; each worker writes one ready slot.
    // Planning remains in the existing ordered commit, never in this worker phase.
    // Component commit derives this gate during its existing loop: empty coordinator identities cost no readiness phase.
    if (impl_->waypoint_queues_present) {
        std::vector<std::uint8_t> ready(moving.size());
        const auto checked = executor.execute_phase("waypoint-completion", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = moving[index];
                if (!unit.waypoints || unit.waypoints->points.empty() || unit.formation) continue;
                if (const auto mind = minds.find(unit.state.entity_id); mind != minds.end()) {
                    ready[index] = mind->second.mode != SquadronMode::move && mind->second.mode != SquadronMode::escort
                        && (mind->second.mode == SquadronMode::idle
                            || (mind->second.formation && mind->second.formation->complete)) ? 1U : 0U;
                } else {
                    ready[index] = unit.motion && unit.motion->kind == MotionKind::none ? 1U : 0U;
                }
            }
        });
        if (!checked) return core::Result<void>::failure(checked.error());
        // HCB-59: each ship formation or squadron team advances independently.
        // FM-02, FC-66/67, FSC-67: a fast formation never waits for another.
        for (std::size_t index = 0; index < moving.size(); ++index) {
            if (ready[index] != 0U) orders_->waypoint_ready.push_back(moving[index].state.entity_id);
        }
    }
    auto& approaches = orders_.value().approaches.emplace();
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (approach_plans[index]) approaches.emplace_back(moving[index].state.entity_id, *approach_plans[index]);
    }

    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
