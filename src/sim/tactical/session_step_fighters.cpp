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

core::Result<void> session_detail::Tick::fighters() {
    auto& minds = craft_prep_.value().minds.value();
    auto& crafts = staging_.value().crafts.value();
    const auto& arrived_squadrons = staging_.value().arrived_squadrons.value();
    const auto& frame = tracked_.value().frame.value();
    auto& events = impacts_.value().events.value();
    const auto& killed = impacts_.value().killed.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    const auto& squadron_table = impl_->motion.squadrons;
    // Spin phase (#447, space-fighter-deaths SP-04 to SP-08): workers service each spinning
    // craft's copy in its own slot; the serial commit in ascending ID drops those that ended and
    // publishes their spin_away_ended. The craft killed this tick then draw whether they spin away
    // (SP-02, SP-03), serially in destruction order: one keyed draw per death, so this walks the
    // tick's deaths, never every unit, and their order is the events' order.
    fighters_.emplace();
    auto& spins = fighters_.value().spins.emplace(impl_->spins.size());
    if (!spins.empty()) {
        std::vector<std::uint8_t> ended(spins.size());
        std::vector<std::optional<core::Diagnostic>> spin_errors(spins.size());
        const auto spun = executor.execute_phase("spins", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, spins.size());
            for (auto index = range.begin; index < range.end; ++index) {
                spins[index] = impl_->spins[index];
                const auto* craft = squadron_table.find_craft(spins[index].type);
                if (craft == nullptr || !craft->spin_away) {
                    ended[index] = 1;
                    continue;
                }
                const auto stepped = step_spin(spins[index], craft->rate_of_turn, *craft->spin_away,
                    impl_->setup.seed, frame);
                if (!stepped) {
                    spin_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + ": " + stepped.error().message);
                    continue;
                }
                ended[index] = stepped.value() ? 1 : 0;
            }
        });
        if (!spun) {
            return core::Result<void>::failure(spun.error());
        }
        std::vector<DeathSpin> running;
        for (std::size_t index = 0; index < spins.size(); ++index) {
            if (spin_errors[index]) {
                return core::Result<void>::failure(std::move(*spin_errors[index]));
            }
            if (ended[index] != 0) {
                events.push_back(Event{tick, EventKind::spin_away_ended, spins[index].owner, 0, spins[index].unit});
                continue;
            }
            running.push_back(std::move(spins[index]));
        }
        spins = std::move(running);
    }
    for (const auto& state : killed) {
        const auto* craft = squadron_table.find_craft(state.type_id);
        if (craft == nullptr || !craft->spin_away
            || !spins_away(*craft->spin_away, impl_->setup.seed, frame, state.entity_id)) {
            continue;
        }
        // SP-03: a craft without a flight state spins from rest.
        const auto flight = crafts.find(state.entity_id);
        spins.push_back(start_spin(state.entity_id, state.type_id, state.owner, state.position,
            flight != crafts.end() ? flight->second : CraftState{}));
        events.push_back(Event{tick, EventKind::spin_away_started, state.owner, 0, state.entity_id});
    }
    std::sort(spins.begin(), spins.end(), [](const DeathSpin& left, const DeathSpin& right) { return left.unit < right.unit; });

    // Squadron phase (#271, V-03): workers read the survivors and fill one slot per squadron
    // with its live craft and their bounding-box centre. The serial commit then moves each
    // container there, or removes a container whose last craft has died.
    std::vector<const Squadron*> squadron_inputs;
    squadron_inputs.reserve(impl_->squadrons.size() + arrived_squadrons.size());
    std::set<EntityId> replaced;
    std::set<EntityId> regrouped;
    for (const auto& squadron : arrived_squadrons) replaced.insert(squadron.container);
    for (const auto& squadron : arrived_squadrons) for (const auto member : squadron.members) regrouped.insert(member);
    for (const auto& squadron : impl_->squadrons)
        if (!replaced.contains(squadron.container) && !(squadron.members.size() == 1
            && squadron.members.front() == squadron.container && regrouped.contains(squadron.container)))
            squadron_inputs.push_back(&squadron);
    for (const auto& squadron : arrived_squadrons) squadron_inputs.push_back(&squadron);
    if (!replaced.empty()) std::sort(squadron_inputs.begin(), squadron_inputs.end(),
        [](const auto* left, const auto* right) { return left->container < right->container; });
    auto& squadrons = fighters_.value().squadrons.emplace();
    if (!squadron_inputs.empty()) {
        const auto find = [&survivors](const EntityId id) -> const LiveUnit* {
            const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
                [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
            return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
        };
        std::vector<SquadronOutcome> outcomes(squadron_inputs.size());
        std::vector<std::pair<std::size_t, PlayerId>> final_killers(squadron_inputs.size());
        const auto grouped = executor.execute_phase("squadrons", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, squadron_inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& outcome = outcomes[index];
                const auto* container = find(squadron_inputs[index]->container);
                outcome.container_live = container != nullptr;
                math::Vec3 low{};
                math::Vec3 high{};
                for (const auto member : squadron_inputs[index]->members) {
                    const auto* craft = find(member);
                    if (craft == nullptr) {
                        if (const auto death = loss_killers_.find(member); death != loss_killers_.end()
                            && death->second.first > final_killers[index].first) final_killers[index] = death->second;
                        continue;
                    }
                    const auto& at = craft->state.position;
                    if (outcome.members.empty()) {
                        low = at;
                        high = at;
                    } else {
                        low = {std::min(low.x, at.x), std::min(low.y, at.y), std::min(low.z, at.z)};
                        high = {std::max(high.x, at.x), std::max(high.y, at.y), std::max(high.z, at.z)};
                    }
                    outcome.members.push_back(member);
                }
                if (container == nullptr || outcome.members.empty()) {
                    continue;
                }
                outcome.centre = {math::Fixed::from_raw(midpoint(low.x.raw(), high.x.raw())),
                    math::Fixed::from_raw(midpoint(low.y.raw(), high.y.raw())),
                    math::Fixed::from_raw(midpoint(low.z.raw(), high.z.raw()))};
                // FM-26 (#687, walk 1 WSQ-50): a squadron holding an idle cell has its container
                // on the cell's point.
                const bool solo = squadron_inputs[index]->members.size() == 1
                    && squadron_inputs[index]->members.front() == squadron_inputs[index]->container;
                if (const auto mind = minds.find(squadron_inputs[index]->container);
                    !solo && mind != minds.end() && mind->second.idle_cell) {
                    outcome.centre = mind->second.anchor;
                }
                outcome.transform = container->placement_cache->matrix;
                outcome.transform.rows[0][3] = outcome.centre.x;
                outcome.transform.rows[1][3] = outcome.centre.y;
                outcome.transform.rows[2][3] = outcome.centre.z;
            }
        });
        if (!grouped) {
            return core::Result<void>::failure(grouped.error());
        }
        mark("squadron_commit", true);
        std::vector<EntityId> emptied;
        std::vector<Squadron> kept;
        for (std::size_t index = 0; index < squadron_inputs.size(); ++index) {
            auto& outcome = outcomes[index];
            if (outcome.error) {
                return core::Result<void>::failure(std::move(*outcome.error));
            }
            if (!outcome.container_live) {
                continue; // the container itself died: its craft fly on without a sensor
            }
            const auto container = squadron_inputs[index]->container;
            if (outcome.members.empty()) {
                // WBF-31: the relevant parent container's last member also reevaluates elimination.
                track_victory_change(&find(container)->state, nullptr, true);
                if (loss_ids_.insert(container).second) {
                    const auto& state = find(container)->state;
                    pending_losses_.push_back({state.owner, state.type_id, final_killers[index].second, 1, tick});
                }
                emptied.push_back(container);
                continue;
            }
            const auto slot = static_cast<std::size_t>(find(container) - survivors.data());
            survivors[slot].state.position = outcome.centre;
            instances[slot].fixed_transform = outcome.transform;
            kept.push_back(Squadron{container, std::move(outcome.members)});
        }
        // Containers without craft leave the session. They have no hull, so no event is
        // published; presentation sees them leave the snapshot.
        if (!emptied.empty()) {
            std::vector<LiveUnit> remaining;
            std::vector<TacticalInstance> remaining_instances;
            for (std::size_t index = 0; index < survivors.size(); ++index) {
                if (std::binary_search(emptied.begin(), emptied.end(), survivors[index].state.entity_id)) {
                    continue;
                }
                remaining.push_back(std::move(survivors[index]));
                remaining_instances.push_back(std::move(instances[index]));
            }
            survivors = std::move(remaining);
            instances = std::move(remaining_instances);
        }
        squadrons = std::move(kept);
        mark("squadron_commit", false);
    }

    // #75: a squadron that left releases its spawner entry (FL-08); dead craft, squadrons and
    // spawners drop their state. Serial, in ascending ID.
    mark("minds_cleanup", true);
    auto& spawners = fighters_.value().spawners.emplace(impl_->spawners);
    // FL-12: commit only this tick's carrier births through the starting-unit initializer.
    for (const auto id : staging_.value().born_spawners) {
        if (survivor(id) != nullptr) spawners.emplace(id, initial_spawner(impl_->setup.seed, frame, id));
    }
    for (auto iterator = minds.begin(); iterator != minds.end();) {
        const auto container = iterator->first;
        const auto found = std::lower_bound(squadrons.begin(), squadrons.end(), container,
            [](const Squadron& squadron, EntityId id) { return squadron.container < id; });
        const bool kept = found != squadrons.end() && found->container == container;
        if (kept) {
            ++iterator;
            continue;
        }
        const auto& state = iterator->second;
        const auto hangar = spawners.find(state.spawner);
        const auto* spawner = survivor(state.spawner);
        if (hangar != spawners.end() && spawner != nullptr) {
            squadron_lost(*squadron_table.find_spawner(spawner->state.type_id), spawners.at(hangar->first), state.entry, frame);
        }
        iterator = minds.erase(iterator);
    }
    crafts.erase_if([&](const auto& entry) {
        if (survivor(entry.first) != nullptr) return false;
        impl_->craft_trig.erase(entry.first);
        return true;
    });
    spawners.erase_if([&](const auto& entry) { return survivor(entry.first) == nullptr; });

    mark("minds_cleanup", false);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::hangars() {
    auto& minds = craft_prep_.value().minds.value();
    auto& crafts = staging_.value().crafts.value();
    auto& respawns = staging_.value().respawns.value();
    auto& earners = staging_.value().earners.value();
    auto& next_id = staging_.value().next_id.value();
    const auto& frame = tracked_.value().frame.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    auto& squadrons = fighters_.value().squadrons.value();
    auto& spawners = fighters_.value().spawners.value();
    const auto& squadron_table = impl_->motion.squadrons;
    auto& pads = impl_->pad_stage.values;
    const auto& arrivals = staging_.value().arrivals.value();
    // Hangar phase (#75, FL-01 to FL-06): workers service each spawner's hangar copy in its own
    // slot; the serial commit launches the squadrons in ascending spawner ID with the next stable
    // IDs, craft first and their team container last (FL-06).
    if (!spawners.empty()) {
        std::vector<std::pair<EntityId, SpawnerState>> hangars(spawners.begin(), spawners.end());
        std::vector<std::optional<SpawnDecision>> decisions(hangars.size());
        const auto serviced_hangars = executor.execute_phase("hangars", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, hangars.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = *survivor(hangars[index].first);
                const auto& profile = *squadron_table.find_spawner(unit.state.type_id);
                std::vector<bool> intact;
                const auto* hull = impl_->health_profile(unit);
                for (const auto& bay : profile.bays) {
                    intact.push_back(!unit.durability || hull == nullptr || bay.hardpoint >= hull->hardpoints.size()
                        || (!hardpoint_destroyed(*hull, *unit.durability, bay.hardpoint)
                            && !hardpoint_disabled(*unit.durability, bay.hardpoint)));
                }
                decisions[index] = service_spawner(profile, hangars[index].second, impl_->setup.seed, frame,
                    hangars[index].first, intact, arrivals.contains(hangars[index].first));
            }
        });
        if (!serviced_hangars) {
            return core::Result<void>::failure(serviced_hangars.error());
        }
        for (std::size_t index = 0; index < hangars.size(); ++index) {
            spawners.assign(hangars[index].first, std::move(hangars[index].second));
            if (!decisions[index]) continue;
            const auto spawner = *survivor(hangars[index].first); // copied: launches grow `survivors`
            const auto& profile = *squadron_table.find_spawner(spawner.state.type_id);
            const auto first_added = survivors.size();
            const auto flown_out = impl_->launch(spawner, profile, *decisions[index], frame, next_id, survivors, instances,
                squadrons, crafts, minds);
            if (!flown_out) {
                return core::Result<void>::failure(flown_out.error());
            }
            for (auto added = first_added; added < survivors.size(); ++added) {
                track_victory_change(nullptr, &survivors[added].state, false);
            }
        }
    }

    while (!respawns.empty() && respawns.begin()->first <= tick) {
        for (const auto& request : respawns.begin()->second.objects) {
            if (next_id == invalid_entity_id) return core::Result<void>::failure(
                detail::diagnostic(diagnostic_codes::resource_limit, "respawn exhausted stable IDs"));
            const auto id = next_id;
            auto unit = impl_->new_unit(UnitState{id, request.type, request.owner, request.position, request.rotation, {}}, tick);
            const auto transform = unit_transform(unit);
            if (!transform) return core::Result<void>::failure(transform.error());
            instances.push_back(impl_->instance_for(unit, transform.value(), tick + 1));
            track_victory_change(nullptr, &unit.state, false);
            survivors.push_back(std::move(unit));
            if (impl_->economy.pads.point(request.type) != nullptr) {
                impl_->pad_stage.touch(id);
                pads.emplace(id, PadState{request.owner});
            }
            if (impl_->economy.stream(request.type) != nullptr) earners.emplace_back(id, request.owner);
            next_id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1;
        }
        respawns.erase(respawns.begin());
    }

    return core::Result<void>::success();
}

const LiveUnit* session_detail::Tick::survivor(const EntityId id) {
    const auto& survivors = surviving_.value().survivors.value();

        const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
}

std::int64_t session_detail::Tick::midpoint(const std::int64_t low, const std::int64_t high) noexcept {
    const auto half = (static_cast<std::uint64_t>(high) - static_cast<std::uint64_t>(low)) / 2U;
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(low) + half);
}

} // namespace eawr::sim::tactical
