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

core::Result<void> session_detail::Tick::gather() {
    // Movement phase (#70): before this tick's commands, workers advance every unit that follows
    // a plan to completed tick + 1 in disjoint slots, so an order acts from the next frame (MV-02).
    gather_.emplace();
    gather_.value().tick_work.emplace();
    gather_.value().reinforcement_work.emplace();
    gather_.value().movement_builds.emplace();
    // The map walk gathers only stable handles. Workers gather each unit's component values,
    // initial position and optional craft view together into disjoint ascending-ID slots.
    mark("sorted_live", true);
    const std::vector<std::pair<EntityId, entt::entity>> handles(impl_->handles.begin(), impl_->handles.end());
    auto& moving = gather_.value().moving.emplace(handles.size());
    auto& starts = gather_.value().starts.emplace(handles.size());
    auto& gathered_crafts = gather_.value().gathered_crafts.emplace(handles.size());
    const bool all_units = impl_->victory.condition == VictoryCondition::all_enemy_units_destroyed;
    if (all_units) gather_.value().victory_parts.emplace();
    // Below 256 units, the quiet melee sample's gather took more time on four workers than
    // inline for these small component copies. Keep that small phase as one
    // executor partition; larger gathers use the usual fixed ranges. The cutoff is work
    // scheduling only, independent of worker count and authoritative state (#892).
    const auto gather_partitions = handles.size() < 256 ? std::size_t{1} : tick_partition_count;
    const auto gathered = executor.execute_phase("gather", gather_partitions, [&](const std::size_t partition) {
        const auto range = gather_partitions == 1 ? PartitionRange{0, handles.size()} : partition_range(partition, handles.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto [id, handle] = handles[index];
            moving[index] = impl_->live_at(id, handle);
            // SAE-10: retain only prevention objects in the existing worker gather.
            if (!impl_->reinforcement_searches.empty()
                && impl_->economy.prevention_of(moving[index].state.type_id) != nullptr) {
                gather_.value().prevention_ids[partition].push_back(id);
            }
            // WBF-35: count relevant objects in the existing worker gather, never a
            // serial world scan for each destruction/conversion.
            if (all_units
                && std::binary_search(impl_->victory.relevant_types.begin(), impl_->victory.relevant_types.end(),
                    moving[index].state.type_id)) {
                const auto owner = std::lower_bound(impl_->setup.players.begin(), impl_->setup.players.end(),
                    moving[index].state.owner, [](const Player& player, const PlayerId value) { return player.player_id < value; });
                ++gather_.value().victory_parts.value()[partition][static_cast<std::size_t>(owner - impl_->setup.players.begin())];
            }
            // EN-08: expire the temporary disable in the partitioned gather, before movement.
            if (moving[index].durability) {
                moving[index].engines_recovered = service_disabled_engines(*moving[index].durability, tick);
                // EN-09: a speed recovery releases a pending formation move for replanning.
                if (moving[index].engines_recovered && moving[index].formation && moving[index].motion) {
                    moving[index].motion->target = moving[index].formation->destination;
                    moving[index].motion->kind = MotionKind::path;
                    moving[index].formation.reset();
                }
            }
            starts[index] = {id, moving[index].state.position};
            const auto flight = impl_->crafts.find(id);
            if (flight != impl_->crafts.end()) {
                gathered_crafts[index] = CraftView{id, moving[index].state.position, flight->second,
                    impl_->motion.squadrons.find_craft(moving[index].state.type_id)};
            }
        }
    });
    if (!gathered) return core::Result<void>::failure(gathered.error());
    if (all_units) {
        for (const auto& part : gather_.value().victory_parts.value()) for (std::size_t owner = 0; owner < impl_->setup.players.size(); ++owner) {
            victory_counts_[owner] += part[owner];
        }
    }
    mark("sorted_live", false);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::prepare_craft() {
    auto& moving = gather_.value().moving.value();
    auto& gathered_crafts = gather_.value().gathered_crafts.value();
    const auto& squadron_table = impl_->motion.squadrons;
    // Craft phase inputs (#75): the start-of-tick flight of every squadron craft, and per squadron
    // its leader and order. Built serially like the combat world: an index over the craft and one
    // entry per squadron, which the workers only read.
    mark("craft_views_minds", true);
    craft_prep_.emplace();
    auto& minds = craft_prep_.value().minds.emplace(impl_->minds);
    auto& craft_views = craft_prep_.value().craft_views.emplace();
    craft_views.reserve(impl_->crafts.size());
    for (auto& craft : gathered_crafts) {
        if (!craft) continue;
        // Scratch map insertion is ordered and serial; later craft partitions own one cache each.
        craft->trig_cache = &impl_->craft_trig.try_emplace(craft->id).first->second;
        craft_views.push_back(*craft);
    }
    mark("craft_views_minds", false);
    // FM-24 (#687, walk 1 WSQ-09): a squadron reverting to idle at `desired` keeps the idle cell it
    // holds, else claims one and holds its point; with none free it holds `desired`. Serial, in
    // ascending container ID, since a claim changes what the next one sees.
    const auto idle_reach = math::Fixed::from_raw(idle_cell_size * math::Fixed::scale);
    auto& squadron_frames = craft_prep_.value().squadron_frames.emplace();
    auto& craft_squadron = craft_prep_.value().craft_squadron.emplace();
    for (const auto& squadron : impl_->squadrons) {
        const auto mind = minds.find(squadron.container);
        if (mind == minds.end()) continue;
        auto& state = minds.at(mind->first);
        SquadronFrame frame;
        frame.state = &state;
        frame.live_roster = squadron.members;
        frame.profile = squadron_table.find_squadron(state.squadron_type);
        for (const auto member : squadron.members) {
            if (craft_view(member) == nullptr) continue;
            if (frame.leader == invalid_entity_id) frame.leader = member; // FM-10: the first live craft leads
            craft_squadron.emplace(member, squadron.container);
        }
        const auto* leader = craft_view(frame.leader);
        if (state.formation && state.formation->base_target != invalid_entity_id
            && live_unit(moving, state.formation->base_target) == nullptr) state.formation.reset();
        // WMV-18: completion does not remove a populated position formation. A space
        // team's attack override inherits completed-base history and initializes as done.
        if (state.formation && state.formation->attack_override && state.formation->has_reached_done)
            state.formation->complete = true;
        const math::Fixed layer_z = leader != nullptr ? leader->profile->layer_z : math::Fixed{};
        // FM-25 (#687, walk 1 WSQ-02, WSQ-49): a squadron on a move or with a target gives its idle
        // cell up.
        if (state.idle_cell && (state.mode == SquadronMode::move || state.target != invalid_entity_id)) {
            state.idle_cell.reset();
        }
        // FO-02 (#424): a move ends once the leader passes the line through the destination
        // square to the move (FoC's path-end half plane); the squadron then holds the destination,
        // or the idle cell it claims there (FM-24).
        if (state.mode == SquadronMode::move && leader != nullptr
            && squadron_move_arrived(state.move_origin, state.anchor, leader->position)) {
            state.mode = SquadronMode::idle;
            state.move_origin = {};
            state.lane.reset();
            claim_idle(squadron.container, state, state.anchor);
            if (state.formation) {
                state.formation->complete = true;
                state.formation->has_reached_done = true;
            }
        }
        // FL-07: an escort holds its carrier's position at the craft's layer height. FO-05,
        // FO-06: a guard or attack-move of a unit ends with it, and the squadron holds there.
        if (state.mode == SquadronMode::escort) {
            if (const auto* escorted = live_unit(moving, state.escorted)) {
                const math::Vec3 carrier{escorted->state.position.x, escorted->state.position.y, layer_z};
                // FM-24 (#687, project): FoC's escort is a move to the unit that ends in an idle cell
                // (walk 1, FM-21). The remake's escort claims a cell around the unit once its leader
                // is within a cell's size of it, and gives the cell up when it takes a target or
                // the unit is more than a cell's size from the cell's point.
                if (state.idle_cell && !within_range(idle_cell_point(*state.idle_cell, layer_z), carrier, idle_reach, RangeMetric::planar)) {
                    state.idle_cell.reset();
                }
                if (!state.idle_cell && state.target == invalid_entity_id && leader != nullptr
                    && within_range(leader->position, carrier, idle_reach, RangeMetric::planar)) {
                    claim_idle(squadron.container, state, carrier);
                }
                if (!state.idle_cell) state.anchor = carrier;
            } else {
                state.mode = SquadronMode::idle;
                state.escorted = invalid_entity_id;
                state.diversion = SquadronDiversion::idle;
            }
        }
        frame.hold = state.anchor;
        frame.moving = state.mode == SquadronMode::move;
        // FO-01: a squadron on a move does not scan. FO-05: on an attack-move it scans from its
        // leader all the way. FO-06: on a guard's way it scans once the leader is within
        // `Guard_Chase_Range` of the guarded point.
        frame.scans = !frame.moving;
        if (frame.moving && state.diversion == SquadronDiversion::attack_move) {
            frame.scans = true;
            frame.scan_from_leader = true;
        } else if (frame.moving && state.diversion == SquadronDiversion::guard && leader != nullptr
            && frame.profile != nullptr) {
            frame.scans = within_range(leader->position, state.anchor, frame.profile->guard_chase_range, RangeMetric::planar);
        }
        squadron_frames.emplace(squadron.container, frame);
    }
    // FO-10, FO-11 (#599): the squadrons still on a group move fly their formation's path, each
    // steering toward its lane and keeping level with its row. Serial, per formation: every
    // member reads every other one's place (O(members^2), a few squadrons per formation).
    {
        std::map<EntityId, std::vector<EntityId>> formations;
        for (const auto& [container, frame] : squadron_frames) {
            const auto* leader = craft_view(frame.leader);
            if (frame.moving && frame.state->lane && leader != nullptr && leader->profile != nullptr) {
                formations[frame.state->lane->formation].push_back(container);
            }
        }
        for (const auto& [formation, containers] : formations) {
            std::vector<LaneMember> members;
            members.reserve(containers.size());
            for (const auto container : containers) {
                const auto& frame = squadron_frames.at(container);
                const auto* leader = craft_view(frame.leader);
                const auto* unit = live_unit(moving, container);
                LaneMember member;
                member.position = unit != nullptr ? unit->state.position : leader->position;
                member.lane = *frame.state->lane;
                member.max_speed = leader->profile->max_speed;
                member.min_speed = leader->profile->min_speed;
                members.push_back(member);
            }
            auto flights = formation_lane_flight(members, squadron_table.side_error_min, squadron_table.side_error_max);
            if (!flights) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " formation " + std::to_string(formation) + ": "
                        + flights.error().message));
            }
            for (std::size_t index = 0; index < containers.size(); ++index) {
                squadron_frames.at(containers[index]).lane = flights.value()[index];
            }
        }
    }

    // #457: the squadron behind a target: the target itself when it is a team container, else
    // the squadron of a craft (FO-04). Zero for any other unit.
    const auto squadron_of = [&](const EntityId target) -> EntityId {
        if (target == invalid_entity_id) return invalid_entity_id;
        for (const auto& squadron : impl_->squadrons) {
            if (squadron.container == target
                || std::binary_search(squadron.members.begin(), squadron.members.end(), target)) {
                return squadron.container;
            }
        }
        return invalid_entity_id;
    };
    const auto team_of = [&](const EntityId id) -> std::optional<TeamId> {
        const auto* unit = live_unit(moving, id);
        if (unit == nullptr) return std::nullopt;
        return impl_->teams.at(unit->state.owner);
    };
    const auto hostile_units = [&](const EntityId owner, const EntityId target) {
        const auto* source = live_unit(moving, owner);
        const auto* recipient = live_unit(moving, target);
        return source != nullptr && recipient != nullptr
            && players_hostile(impl_->snapshot_players, source->state.owner, recipient->state.owner);
    };
    // FT-05, WSQ-47: hand a squadron's new target to its craft, or take the old
    // one back.
    // FD-09: combat ends: the squadron leaves its cell and, holding a point, holds where its
    // leader will be this frame at its layer height, or, left without a target, the idle cell it
    // claims there (FM-24, walk 1 WSQ-14).
    // WU-25a: the cell a dogfight around `target` settles on.
    const auto search_cell = [&](const math::Vec3& target) {
        const CombatCell centre = combat_cell_of(target);
        const auto joined = [&](const CombatCell cell) {
            return std::any_of(minds.begin(), minds.end(),
                [&](const auto& entry) { return entry.second.joined && entry.second.cell == cell; });
        };
        CombatCell best_cell = centre;
        std::optional<math::detail::UInt192> best;
        for (std::int32_t y = centre.y - 1; y <= centre.y + 1; ++y) {
            for (std::int32_t x = centre.x - 1; x <= centre.x + 1; ++x) {
                const CombatCell cell{x, y};
                math::detail::UInt192 score = math::detail::from_u64(0);
                if (!joined(cell)) {
                    const auto point = combat_cell_point(cell, math::Fixed{});
                    const auto dx = math::detail::unsigned_magnitude(point.x.raw() - target.x.raw());
                    const auto dy = math::detail::unsigned_magnitude(point.y.raw() - target.y.raw());
                    score = math::detail::multiply_u64(dx, dx);
                    static_cast<void>(math::detail::add_magnitude(score, math::detail::multiply_u64(dy, dy)));
                }
                if (!best || math::detail::compare(score, *best) < 0) {
                    best = score;
                    best_cell = cell;
                }
            }
        }
        return best_cell;
    };
    // The squadrons' targets and dogfights (#457, FD-01 to FD-04, FD-09; foc-battle-world-ui
    // WU-25): serial in ascending container ID, the service order, since a squadron's cell and a
    // retaliation change what the squadrons after it see. O(squadrons) per frame with a short scan
    // of the joined squadrons each.
    for (auto& [container, frame] : squadron_frames) {
        auto& state = minds.at(container);
        const auto* leader = craft_view(frame.leader);
        // FD-09: a dead target gives way to the first enemy squadron joined in the recorded cell,
        // else combat ends.
        if (state.target != invalid_entity_id && !hostile_units(container, state.target)) {
            EntityId next = invalid_entity_id;
            if (state.cell) {
                for (const auto& [other, other_state] : minds) {
                    if (other_state.joined && other_state.cell == state.cell && hostile_units(container, other)) {
                        next = other;
                        break;
                    }
                }
            }
            const auto previous = state.target;
            state.target = next;
            state.target_hardpoint = attack_hull; // #531: the dogfight's own target names no hardpoint
            state.approach = next != invalid_entity_id;
            hand_target(container, previous, next);
            if (next == invalid_entity_id) end_combat(container, state, leader);
        }
        const auto* target = live_unit(moving, state.target);
        if (target == nullptr) {
            state.cell.reset();
            state.joined = false;
            continue;
        }
        frame.attacking = true;
        frame.target_position = target->state.position;
        const auto* footprint = impl_->motion.footprint(target->state.type_id);
        frame.target_radius = footprint != nullptr ? footprint->radius : math::Fixed{};
        // #424 (project, unverified): a squadron's team container counts as craft (FA-06).
        const EntityId target_squadron = squadron_of(state.target);
        frame.target_craft = squadron_table.find_craft(target->state.type_id) != nullptr
            || target_squadron != invalid_entity_id;
        // FA-01: whether the leader is within the strafe reach plus the target's radius (planar).
        bool in_reach = false;
        if (leader != nullptr) {
            in_reach = within_range(leader->position, target->state.position,
                math::Fixed::from_raw(std::max<std::int64_t>(0, leader->profile->strafe_distance.raw() + frame.target_radius.raw())),
                RangeMetric::planar);
        }
        // FA-07: the approach ends for good once the leader is within strafe reach (FA-01).
        if (state.approach && in_reach) state.approach = false;
        frame.approach = state.approach;
        // FD-01: a squadron fights a squadron in the combat grid while its leader is within reach or
        // while it records its target's cell; otherwise it leaves its cell (WU-25).
        const auto theirs = target_squadron != invalid_entity_id ? minds.find(target_squadron) : minds.end();
        const bool sharing = theirs != minds.end() && state.cell && theirs->second.cell && *state.cell == *theirs->second.cell;
        if (theirs == minds.end() || leader == nullptr || (!sharing && !in_reach)) {
            state.cell.reset();
            state.joined = false;
            continue;
        }
        if (!theirs->second.cell) {
            // FD-02: the target records no cell: record the searched one without joining it.
            state.cell = search_cell(target->state.position);
            state.joined = false;
            frame.dogfight = DogfightFlight::find_cell;
            continue;
        }
        // FD-03: join the target's cell.
        const CombatCell cell = *theirs->second.cell;
        state.cell = cell;
        state.joined = true;
        frame.dogfight = DogfightFlight::cell;
        frame.cell = cell;
        // FD-04: once a craft is near the cell point, a target squadron that is not on a move and
        // has no squadron target turns on this squadron.
        auto& other = minds.at(theirs->first);
        bool near = false;
        for (const auto& squadron : impl_->squadrons) {
            if (squadron.container != container) continue;
            for (const auto member : squadron.members) {
                const auto* view = craft_view(member);
                near = near || (view != nullptr && within_combat_cell_reach(view->position, cell));
            }
        }
        if (near && other.mode != SquadronMode::move && squadron_of(other.target) == invalid_entity_id) {
            const auto previous = other.target;
            other.target = container;
            other.target_hardpoint = attack_hull;
            other.approach = true;
            hand_target(target_squadron, previous, container);
        }
    }

    // Chase pairing (#457, FD-05, FD-06). A run-out chase timer clears the chase, and a chased craft
    // that died is dropped (its timer runs on). Then every craft of a squadron fighting in a cell,
    // near the cell point and with its timer run out, looks for a craft to chase among the enemy
    // squadrons joined in its cell, in the cell's order (ascending container ID) and team order.
    // Workers list each such craft's followable candidates from the start-of-frame flight into
    // its own slot; the serial commit, in ascending craft ID (the service order), gives each craft
    // its first candidate whose timer has run out and restarts both timers, so a craft is chased
    // by one other at a time and a chased craft neither chases nor is taken for ten seconds.
    const auto frame_number = tick + 1;
    const auto squadron_frame_of = [&](const EntityId craft) -> const SquadronFrame* {
        const auto squadron = craft_squadron.find(craft);
        return squadron != craft_squadron.end() ? &squadron_frames.at(squadron->second) : nullptr;
    };
    std::vector<std::size_t> scanners;
    for (std::size_t index = 0; index < craft_views.size(); ++index) {
        auto& flight = craft_views[index].state;
        if (flight.chase_until != 0 && frame_number >= flight.chase_until) {
            flight.chase = invalid_entity_id;
            flight.chase_until = 0;
        }
        if (flight.chase != invalid_entity_id
            && (craft_view(flight.chase) == nullptr || !hostile_units(craft_views[index].id, flight.chase))) {
            flight.chase = invalid_entity_id;
        }
        const auto* frame = squadron_frame_of(craft_views[index].id);
        if (frame != nullptr && frame->dogfight == DogfightFlight::cell && flight.chase_until == 0
            && within_combat_cell_reach(craft_views[index].position, frame->cell)) {
            scanners.push_back(index);
        }
    }
    auto& dogfight_cone_tests = craft_prep_.value().dogfight_cone_tests.emplace(0);
    if (!scanners.empty()) {
        // #893: one ordered merge into the shared read-only index, after cell joins
        // and timer expiry. This O(roster) construction replaces a scan per craft.
        std::vector<ChaseCandidate> chase_rows;
        chase_rows.reserve(craft_views.size());
        for (const auto& squadron : impl_->squadrons) {
            const auto mind = minds.find(squadron.container);
            if (mind == minds.end() || !mind->second.joined || !mind->second.cell) continue;
            const auto team = team_of(squadron.container);
            if (!team) continue;
            for (const auto member : mind->second.roster) {
                if (const auto* view = craft_view(member)) chase_rows.push_back({*mind->second.cell, *team, view});
            }
        }
        const ChaseCandidateIndex chase_index(std::move(chase_rows));
        auto& candidates = impl_->chase_candidates;
        if (candidates.size() < scanners.size()) candidates.resize(scanners.size());
        std::vector<std::optional<core::Diagnostic>> chase_errors(scanners.size());
        std::vector<std::uint64_t> cone_tests(scanners.size());
        const auto listed = executor.execute_phase("dogfight-chases", tick_partition_count, [&](const std::size_t partition) {
            auto& scratch = impl_->chase_scratch[partition];
            const auto range = partition_range(partition, scanners.size());
            for (auto index = range.begin; index < range.end; ++index) {
                candidates[index].clear();
                const auto& self = craft_views[scanners[index]];
                const auto& cell = squadron_frame_of(self.id)->cell;
                const auto own = craft_squadron.at(self.id);
                const auto own_team = team_of(own);
                if (!own_team) continue;
                for (const auto* view : chase_index.query(self, cell, *own_team, scratch)) {
                    if (!hostile_units(self.id, view->id)) continue;
                    math::Fixed yaw;
                    math::Fixed pitch;
                    ++cone_tests[index];
                    auto follows = in_follow_cone(self, view->position, yaw, pitch);
                    if (!follows) {
                        chase_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                            "tick " + std::to_string(tick) + ": " + follows.error().message);
                        break;
                    }
                    if (follows.value()) candidates[index].push_back(view->id);
                }
            }
        });
        if (!listed) {
            return core::Result<void>::failure(listed.error());
        }
        for (auto& error : chase_errors) {
            if (error) {
                return core::Result<void>::failure(std::move(*error));
            }
        }
        for (const auto tests : cone_tests) dogfight_cone_tests += tests;
        const auto mutable_view = [&craft_views](const EntityId id) -> CraftView* {
            const auto found = std::lower_bound(craft_views.begin(), craft_views.end(), id,
                [](const CraftView& view, const EntityId value) { return view.id < value; });
            return found != craft_views.end() && found->id == id ? &*found : nullptr;
        };
        for (std::size_t index = 0; index < scanners.size(); ++index) {
            auto& self = craft_views[scanners[index]].state;
            if (self.chase_until != 0) continue; // taken by an earlier craft this frame
            for (const auto candidate : candidates[index]) {
                auto* chased = mutable_view(candidate);
                if (chased->state.chase_until != 0) continue;
                self.chase = candidate;
                self.chase_until = frame_number + chase_frames;
                chased->state.chase = invalid_entity_id;
                chased->state.chase_until = frame_number + chase_frames;
                break;
            }
        }
    }
    // FD-10: the ships and static objects craft steer around, ascending ID, at the start of the frame.
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::movement() {
    auto& tick_work = gather_.value().tick_work.value();
    auto& movement_builds = gather_.value().movement_builds.value();
    auto& moving = gather_.value().moving.value();
    const auto& craft_views = craft_prep_.value().craft_views.value();
    const auto& squadron_frames = craft_prep_.value().squadron_frames.value();
    const auto& craft_squadron = craft_prep_.value().craft_squadron.value();
    std::vector<CraftObstacle> obstacles;
    if (!craft_views.empty()) {
        for (const auto& unit : moving) {
            const auto* footprint = impl_->motion.footprint(unit.state.type_id);
            if (footprint == nullptr || footprint->layer == SpaceLayer::none || footprint->radius.raw() <= 0
                || craft_view(unit.state.entity_id) != nullptr || squadron_frames.contains(unit.state.entity_id)) {
                continue;
            }
            // WHZ-08: fighters avoid ordinary objects and solid asteroids, excluding volumes.
            const auto category = tracking_category(*footprint, false);
            if ((category & (collision_field | collision_storm | collision_nebula)) != 0) continue;
            const auto yaw = yaw_degrees(unit.state.rotation);
            if (!yaw) return core::Result<void>::failure(yaw.error());
            const Prediction still{unit.state.position, yaw.value()};
            const auto leaf = tracking_leaf(unit.state.entity_id, *footprint, still, still);
            if (!leaf) return core::Result<void>::failure(leaf.error());
            obstacles.push_back({unit.state.entity_id, {leaf.value().start.x, leaf.value().start.y, unit.state.position.z},
                footprint->radius});
        }
    }

    movement_.emplace();
    auto& craft_steps = movement_.value().craft_steps.emplace(moving.size());
    std::vector<std::optional<core::Diagnostic>> move_errors(moving.size());
    const auto moved = executor.execute_phase("movement", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, moving.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& unit = moving[index];
            if (unit.arrival_vulnerable_until && tick + 1 >= *unit.arrival_vulnerable_until) {
                unit.arrival_vulnerable_until.reset();
            }
            // #530 PU-35: an arriving unit flies its arrival lane, not its locomotor.
            if (const auto arriving = impl_->arrivals.find(unit.state.entity_id); arriving != impl_->arrivals.end()) {
                auto next = arriving->second;
                next.frame = std::min(next.frame + 1, arrival_frames);
                auto placed = arrival_position(next);
                if (!placed) {
                    move_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": "
                            + placed.error().message);
                    continue;
                }
                unit.state.position = placed.value();
                continue;
            }
            const auto squadron = craft_squadron.find(unit.state.entity_id);
            if (squadron != craft_squadron.end()) {
                // FM-01: a squadron craft flies by the fighter locomotor from the copied inputs.
                const auto& frame = squadron_frames.at(squadron->second);
                CraftFrame inputs;
                inputs.self = craft_view(unit.state.entity_id);
                inputs.leader = craft_view(frame.leader);
                inputs.own_offset = roster_offset(frame, unit.state.entity_id);
                inputs.leader_offset = roster_offset(frame, frame.leader);
                inputs.formation_tolerance = frame.profile != nullptr ? frame.profile->formation_tolerance : math::Fixed{};
                inputs.attacking = frame.attacking;
                inputs.target_position = frame.target_position;
                inputs.target_radius = frame.target_radius;
                inputs.target_craft = frame.target_craft;
                inputs.approach = frame.approach;
                inputs.hold = frame.hold;
                inputs.speed_factor = math::multiply(impl_->ability_factor(unit, AbilityModifier::speed),
                    math::Fixed::from_raw(math::Fixed::scale + unit.upgrade_bonuses[5].raw())).value();
                if (unit.durability && unit.durability->engines_disabled_until) {
                    inputs.speed_factor = math::multiply(inputs.speed_factor, impl_->durability.rules.engines_disabled_speed).value();
                }
                inputs.moving = frame.moving;
                inputs.lane = frame.lane;
                inputs.dogfight = frame.dogfight;
                inputs.cell = frame.cell;
                if (inputs.self->state.chase_until != 0) inputs.chase = craft_view(inputs.self->state.chase);
                inputs.obstacles = obstacles;
                inputs.trig_cache = inputs.self->trig_cache;
                auto stepped = step_craft(inputs);
                if (!stepped) {
                    move_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": "
                            + stepped.error().message);
                    continue;
                }
                unit.state.position = stepped.value().position;
                unit.state.rotation = stepped.value().rotation;
                craft_steps[index] = std::move(stepped).value();
                continue;
            }
            auto advanced = advance(unit, impl_->motion.find(unit.state.type_id), tick + 1);
            if (!advanced) {
                move_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id)
                        + ": " + advanced.error().message);
            }
        }
        // Placement scratch is owned by this movement partition, after all movement branches.
        for (auto index = range.begin; index < range.end; ++index) {
            if (move_errors[index]) continue;
            auto& unit = moving[index];
            const auto transform = level_transform(unit);
            movement_builds[partition] += unit.matrix_builds;
            unit.matrix_builds = 0;
            if (!transform) move_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": "
                    + transform.error().message);
        }
    });
    if (!moved) {
        return core::Result<void>::failure(moved.error());
    }
    for (auto& error : move_errors) {
        if (error) {
            return core::Result<void>::failure(std::move(*error));
        }
    }
    for (const auto builds : movement_builds) tick_work.level_matrix_builds += builds;
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::open_staging() {
    staging_.emplace();
    staging_.value().crafts.emplace(impl_->crafts);
    // #530 PU-35, PU-39: the arrivals advance one frame; frame 150 ends an arrival. Serial over
    // the few arriving units.
    auto& arrivals = staging_.value().arrivals.emplace(impl_->arrivals);
    auto& unloaded = staging_.value().unloaded.emplace();
    for (auto iterator = arrivals.begin(); iterator != arrivals.end();) {
        if (++iterator->second.frame >= arrival_frames) {
            unloaded.push_back(iterator->first); // WR-40: ordered completion notification
            iterator = arrivals.erase(iterator);
        } else {
            ++iterator;
        }
    }
    staging_.value().ledgers.emplace(impl_->ledgers);
    impl_->pad_stage.begin(impl_->pads, impl_->pads.size());
    impl_->construction_stage.begin(impl_->construction, impl_->construction.size() + impl_->pads.size());
    staging_.value().respawns.emplace(impl_->respawns);
    staging_.value().pad_requests.emplace();
    staging_.value().shares.emplace(impl_->shares);
    staging_.value().arrived_squadrons.emplace();
    staging_.value().earners.emplace(impl_->earners);
    staging_.value().next_id.emplace(impl_->next_id);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::commit_moves() {
    const auto& moving = gather_.value().moving.value();
    const auto& squadron_frames = craft_prep_.value().squadron_frames.value();
    const auto& craft_squadron = craft_prep_.value().craft_squadron.value();
    const auto& craft_steps = movement_.value().craft_steps.value();
    auto& crafts = staging_.value().crafts.value();
    // DG-26: the defense each craft's locomotor set this frame, read by this frame's hits.
    moved_.emplace();
    auto& craft_defense = moved_.value().craft_defense.emplace();
    // WU-25 (presentation, not hashed): the squadrons whose leader closes on its target (FA-01).
    auto& closing_squadrons = moved_.value().closing_squadrons.emplace();
    mark("commit_moves", true);
    for (std::size_t index = 0; index < moving.size(); ++index) {
        if (craft_steps[index]) {
            crafts.assign(moving[index].state.entity_id, craft_steps[index]->state);
            if (craft_steps[index]->closing) {
                const auto squadron = craft_squadron.find(moving[index].state.entity_id);
                if (squadron != craft_squadron.end()
                    && squadron_frames.at(squadron->second).leader == moving[index].state.entity_id) {
                    closing_squadrons.insert(squadron->second);
                }
            }
            if (craft_steps[index]->defense.raw() != 0) {
                craft_defense.emplace(moving[index].state.entity_id, craft_steps[index]->defense);
            }
        }
    }

    mark("commit_moves", false);
    return core::Result<void>::success();
}

math::Vec3 session_detail::Tick::roster_offset(const SquadronFrame& frame, const EntityId craft) {
        const auto roster = frame.live_roster;
        const auto slot = static_cast<std::size_t>(std::find(roster.begin(), roster.end(), craft) - roster.begin());
        return frame.profile != nullptr && slot < frame.profile->offsets.size() ? frame.profile->offsets[slot]
                                                                                : math::Vec3{};
}

LiveUnit* session_detail::Tick::live_unit(std::vector<LiveUnit>& units, const EntityId id) {
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != units.end() && found->state.entity_id == id ? &*found : nullptr;
}

const CraftView* session_detail::Tick::craft_view(const EntityId id) {
    const auto& craft_views = craft_prep_.value().craft_views.value();

        const auto found = std::lower_bound(craft_views.begin(), craft_views.end(), id,
            [](const CraftView& view, const EntityId value) { return view.id < value; });
        return found != craft_views.end() && found->id == id ? &*found : nullptr;
}

void session_detail::Tick::claim_idle(const EntityId container, SquadronState& state, const math::Vec3& desired) {
    auto& minds = craft_prep_.value().minds.value();

        if (state.idle_cell) return;
        std::vector<CombatCell> taken;
        for (const auto& [other, mind] : minds) {
            if (other != container && mind.idle_cell) taken.push_back(*mind.idle_cell);
        }
        if (const auto cell = idle_cell_claim(desired, taken)) {
            state.idle_cell = cell;
            state.anchor = idle_cell_point(*cell, desired.z);
        } else {
            state.anchor = desired;
        }
}

void session_detail::Tick::hand_target(const EntityId container, const EntityId previous, const EntityId target,
    const std::uint32_t hardpoint, const bool direct) {
    auto& moving = gather_.value().moving.value();
    auto& minds = craft_prep_.value().minds.value();
    if (const auto found = minds.find(container); found != minds.end() && found->second.formation) {
        auto& formation = minds.at(container).formation.value();
        if (target != invalid_entity_id && target != previous && !direct) {
            // WMV-18: splitting one team copies completion history and resets a completed
            // position base to its current position before adding the autonomous override.
            if (formation.complete && formation.base_target == invalid_entity_id) {
                if (const auto* team = live_unit(moving, container)) formation.base_position = team->state.position;
            }
            formation.attack_override = true;
            formation.complete = false;
        } else if (target == invalid_entity_id && formation.attack_override) {
            formation.attack_override = false;
            formation.complete = formation.has_reached_done;
        }
    }

        for (const auto& squadron : impl_->squadrons) {
            if (squadron.container != container) continue;
            for (const auto member : squadron.members) {
                auto* craft = live_unit(moving, member);
                if (craft == nullptr || !craft->combat) continue;
                if (target != invalid_entity_id) {
                    craft->combat->attack_target = target;
                    craft->combat->attack_hardpoint = hardpoint;
                    // WSQ-47: only a player attack carries the direct/pursuit flags.
                    craft->combat->direct = direct;
                } else if (craft->combat->attack_target == previous) {
                    craft->combat->attack_target = invalid_entity_id;
                    craft->combat->attack_hardpoint = no_hardpoint;
                    craft->combat->direct = false;
                }
            }
        }
}

void session_detail::Tick::end_combat(const EntityId container, SquadronState& state, const CraftView* leader) {
        state.cell.reset();
        state.joined = false;
        if (state.mode == SquadronMode::idle && leader != nullptr) {
            state.anchor = {math::Fixed::from_raw(leader->position.x.raw() + leader->state.velocity.x.raw()),
                math::Fixed::from_raw(leader->position.y.raw() + leader->state.velocity.y.raw()), leader->profile->layer_z};
            if (state.target == invalid_entity_id) claim_idle(container, state, state.anchor);
        }
}

const LiveUnit* session_detail::Tick::live_unit(const std::vector<LiveUnit>& units, const EntityId id) {
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != units.end() && found->state.entity_id == id ? &*found : nullptr;
}

} // namespace eawr::sim::tactical
