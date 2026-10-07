#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"

#include "eawr/sim/tactical/formation.hpp"
#include "eawr/sim/tactical/pathfind.hpp"
#include "eawr/sim/math/trig.hpp"

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

core::Result<void> session_detail::Tick::nebulas() {
    auto& units = gather_.value().moving.value();
    auto& scratch = impl_->nebula_scratch;
    for (auto& signals : scratch.events) signals.clear();
    const auto& content = impl_->motion;
    const bool has_volumes = std::any_of(content.footprints.begin(), content.footprints.end(),
        [](const Footprint& footprint) { return (footprint.nebula || footprint.ion_storm) && footprint.obstacle && footprint.layer == SpaceLayer::static_object; });
    if (!has_volumes && content.nebula_service_types.empty()) return core::Result<void>::success();
    scratch.slots.assign(units.size(), std::nullopt);
    scratch.errors.assign(units.size(), std::nullopt);
    scratch.events.resize(units.size());
    scratch.members.resize(units.size());
    scratch.previous.resize(units.size());
    scratch.volumes.clear();
    scratch.bodies.clear();
    const auto& arrivals = staging_.value().arrivals.value();
    const auto prepared = executor.execute_phase("nebula-inputs", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, units.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& unit = units[index];
            const auto* footprint = content.footprint(unit.state.type_id);
            if (!content.avoidance || footprint == nullptr || !footprint->obstacle || (!footprint->nebula && !footprint->ion_storm)
                || footprint->layer != SpaceLayer::static_object || arrivals.contains(unit.state.entity_id)
                || (unit.durability && unit.durability->hull.raw() <= 0)) continue;
            const auto yaw = yaw_degrees(unit.state.rotation);
            if (!yaw) { scratch.errors[index] = yaw.error(); continue; }
            const Prediction still{unit.state.position, yaw.value()};
            const auto leaf = tracking_leaf(unit.state.entity_id, *footprint, still, still);
            const auto turn = math::divide(yaw.value(), math::Fixed::from_raw(360 * math::Fixed::scale));
            if (!leaf) { scratch.errors[index] = leaf.error(); continue; }
            if (!turn) { scratch.errors[index] = turn.error(); continue; }
            const auto trig = math::sin_cos_turn(turn.value());
            scratch.slots[index] = NebulaInput{{unit.state.entity_id, unit.state.owner,
                {unit.state.position.x, unit.state.position.y, math::Fixed{}}},
                {leaf.value().start.x, leaf.value().start.y, math::Fixed{}}, footprint->radius,
                footprint->x_extent, footprint->y_extent, {trig.cosine, trig.sine}, footprint->nebula, footprint->ion_storm};
        }
    });
    if (!prepared) return prepared;
    math::Fixed reach{};
    // Ordered commit of copied volume geometry; all object computations ran in workers.
    for (std::size_t index = 0; index < scratch.slots.size(); ++index) {
        if (scratch.errors[index]) return core::Result<void>::failure(*scratch.errors[index]);
        if (!scratch.slots[index]) continue;
        const auto& volume = *scratch.slots[index];
        scratch.volumes.push_back(volume);
        scratch.bodies.push_back(volume.body);
        const auto abs = [](const math::Fixed value) { return value.raw() < 0 ? -value.raw() : value.raw(); };
        const auto broad = abs(math::Fixed::from_raw(volume.soft_center.x.raw() - volume.body.position.x.raw()))
            + abs(math::Fixed::from_raw(volume.soft_center.y.raw() - volume.body.position.y.raw()))
            + volume.radius.raw() + volume.x_extent.raw() + volume.y_extent.raw();
        reach = std::max(reach, math::Fixed::from_raw(broad));
    }
    const auto indexed = scratch.index.rebuild_sorted(scratch.bodies);
    if (!indexed) return indexed;
    scratch.reach = reach;
    const auto signal_unit = [&](const LiveUnit& unit) {
        const auto found = craft_prep_.value().craft_squadron.value().find(unit.state.entity_id);
        return found == craft_prep_.value().craft_squadron.value().end() ? unit.state.entity_id : found->second;
    };
    const auto signals = [&](LiveUnit& unit, const std::size_t index, const bool enter) {
        if (!unit.abilities) return;
        const auto& profile = *impl_->abilities.find(unit.state.type_id);
        for (std::size_t slot = 0; slot < profile.abilities.size(); ++slot) {
            const auto& ability = profile.abilities[slot];
            auto& state = unit.abilities->slots[slot];
            if (enter) {
                if (!state.active) continue;
                // WHZ-26/WAB-65: the team adapter owns pending-shot accounting and recharge.
                if (ability.kind != AbilityKind::ion_cannon_shot) {
                    const auto changed = deactivate_ability(ability, state, tick);
                    unit.abilities->replan_due = unit.abilities->replan_due || changed.speed;
                }
            } else {
                auto gate = impl_->ability_gate(unit, tick);
                gate.in_nebula = false;
                // WHZ-24: normal enablement of the two primary slots, never activation.
                if (!ability_ready(ability, state, gate, tick)) continue;
            }
            scratch.events[index].push_back(Event{tick, enter ? EventKind::ability_cancelled : EventKind::ability_ready,
                unit.state.owner, static_cast<std::uint64_t>(ability.kind), signal_unit(unit)});
        }
    };
    const auto serviced = executor.execute_phase("nebula-service", tick_partition_count, [&](const std::size_t partition) {
        auto& candidates = scratch.candidates[partition];
        const auto range = partition_range(partition, units.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& unit = units[index];
            scratch.previous[index] = unit.nebula && unit.nebula->present ? 1 : 0;
            const bool had_contact = unit.nebula && unit.nebula->frame.has_value();
            const std::uint64_t contact_frame = had_contact ? unit.nebula->frame.value() : 0;
            const bool behavior = std::binary_search(content.nebula_service_types.begin(), content.nebula_service_types.end(), unit.state.type_id);
            // WHZ-21: strict contact-age window; geometry is skipped while the cache holds.
            if (behavior && had_contact && tick >= contact_frame && (tick - contact_frame) * math::Fixed::scale
                < static_cast<std::uint64_t>(content.nebula_disable_seconds.raw()) * logical_frames_per_second) {
                unit.nebula->present = true;
                scratch.members[index] = 1;
                continue;
            }
            bool inside = false;
            if (!behavior || !arrivals.contains(unit.state.entity_id)) {
                const math::Vec3 center{unit.state.position.x, unit.state.position.y, math::Fixed{}};
                scratch.index.box_positions(center, {reach, reach, math::Fixed{}}, candidates);
                std::sort(candidates.begin(), candidates.end());
                for (const auto candidate : candidates) {
                    const auto& volume = scratch.volumes[candidate];
                    if (!volume.nebula) continue;
                    if (behavior && volume.body.entity_id == unit.state.entity_id) continue;
                    if (behavior) {
                        inside = within_range(center, volume.soft_center, volume.radius, RangeMetric::planar);
                    } else {
                        // WHZ-25/EHZ-19: hard oriented box at the object position, not soft radius/offset.
                        const auto dx = math::Fixed::from_raw(center.x.raw() - volume.body.position.x.raw());
                        const auto dy = math::Fixed::from_raw(center.y.raw() - volume.body.position.y.raw());
                        const auto xc = math::multiply(dx, volume.forward.x);
                        const auto ys = math::multiply(dy, volume.forward.y);
                        const auto xs = math::multiply(dx, volume.forward.y);
                        const auto yc = math::multiply(dy, volume.forward.x);
                        if (!xc || !ys || !xs || !yc) {
                            scratch.errors[index] = !xc ? xc.error() : !ys ? ys.error() : !xs ? xs.error() : yc.error();
                            break;
                        }
                        const auto x = xc.value().raw() + ys.value().raw();
                        const auto y = yc.value().raw() - xs.value().raw();
                        inside = x >= -volume.x_extent.raw() && x <= volume.x_extent.raw()
                            && y >= -volume.y_extent.raw() && y <= volume.y_extent.raw();
                    }
                    if (inside) break;
                }
            }
            const bool present = inside && (!behavior || content.nebula_disable_seconds.raw() > 0);
            unit.nebula = inside ? std::optional(NebulaContact{behavior ? std::optional(tick) : std::nullopt, present}) : std::nullopt;
            scratch.members[index] = present ? 1 : 0;
            if (behavior && inside && !had_contact) signals(unit, index, true);
            if (behavior && !inside && had_contact) signals(unit, index, false);
        }
    });
    if (!serviced) return serviced;
    for (const auto& error : scratch.errors) if (error) return core::Result<void>::failure(*error);
    // WHZ-25: a second immutable member view supplies the team union; workers only write containers.
    const auto teams = executor.execute_phase("nebula-teams", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, impl_->squadrons.size());
        for (auto slot = range.begin; slot < range.end; ++slot) {
            const auto& squadron = impl_->squadrons[slot];
            const auto found = std::lower_bound(units.begin(), units.end(), squadron.container,
                [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; });
            if (found == units.end() || found->state.entity_id != squadron.container) continue;
            const auto index = static_cast<std::size_t>(found - units.begin());
            const bool before = scratch.previous[index] != 0;
            bool inside = false;
            for (const auto id : squadron.members) {
                const auto member = std::lower_bound(units.begin(), units.end(), id,
                    [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
                if (member != units.end() && member->state.entity_id == id && scratch.members[static_cast<std::size_t>(member - units.begin())] != 0) { inside = true; break; }
            }
            found->nebula = inside ? std::optional(NebulaContact{std::nullopt, true}) : std::nullopt;
            if (inside && !before) signals(*found, index, true);
        }
    });
    return teams;
}

core::Result<void> session_detail::Tick::systems() {
    const auto& arrivals = staging_.value().arrivals.value();
    const auto& world = targets_.value().world.value();
    const auto& damage_rules = flights_.value().damage_rules.value();
    auto& staged = tracked_.value().staged.value();
    const auto& craft_states = staging_.value().crafts.value();
    const auto& squadron_states = craft_prep_.value().minds.value();
    auto& pads = impl_->pad_stage.values;
    auto& construction = impl_->construction_stage.values;
    const auto& durability = impl_->durability;
    // Systems phase: workers read the copied ascending-ID units and fill disjoint slots. Each
    // durable unit's hull and hardpoints are serviced (HS-01) before its instance is built.
    mark("systems_setup", true);
    systems_.emplace();
    auto& inputs = systems_.value().inputs.emplace(staged.release());
    if (auto serviced = impl_->service_beams(inputs, tick, executor); !serviced) return serviced;
    if (auto serviced = impl_->service_weaken(inputs, tick, executor); !serviced) return serviced;
    if (inputs.size() != impl_->concentrate_service_census
        || (!inputs.empty() && inputs.back().state.entity_id != impl_->concentrate_service_last_id)) {
        if (auto serviced = impl_->service_concentrate(inputs, tick, executor); !serviced) return serviced;
    }
    if (auto refreshed = impl_->refresh_concentrate_bonuses(inputs, impl_->command_ledger, staging_->ledgers.value(), executor); !refreshed) return refreshed;
    auto& asteroid_scratch = impl_->asteroid_scratch;
    auto& asteroid_impacts = asteroid_scratch.impacts;
    auto& asteroid_queries = asteroid_scratch.queries;
    auto& asteroid_candidates = asteroid_scratch.examined;
    asteroid_impacts.resize(inputs.size());
    asteroid_scratch.deliveries.resize(inputs.size());
    for (auto& output : asteroid_scratch.deliveries) output.clear();
    asteroid_queries.assign(inputs.size(), 0);
    asteroid_candidates.assign(inputs.size(), 0);
    for (auto& output : asteroid_impacts) output.clear();
    auto& field_slots = asteroid_scratch.field_slots;
    auto& fields = asteroid_scratch.fields;
    auto& field_index = asteroid_scratch.index;
    fields.clear();
    math::Fixed field_reach{};
    // WHZ-11: disabled scalars skip geometry; static tracking is available only with avoidance.
    if (damage_rules != nullptr && damage_rules->asteroid_damage.raw() != 0
        && damage_rules->asteroid_rate.raw() != 0 && impl_->motion.avoidance) {
        field_slots.assign(inputs.size(), std::nullopt);
        auto& field_errors = asteroid_scratch.errors;
        field_errors.assign(inputs.size(), std::nullopt);
        const auto prepared = executor.execute_phase("asteroid-inputs", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, inputs.size());
            for (auto slot = range.begin; slot < range.end; ++slot) {
                const auto& unit = inputs[slot];
                const auto* footprint = impl_->motion.footprint(unit.state.type_id);
                if (footprint == nullptr || !footprint->obstacle || !footprint->asteroid_field
                    || footprint->layer != SpaceLayer::static_object || arrivals.contains(unit.state.entity_id)
                    || (unit.durability && unit.durability->hull.raw() <= 0)) continue;
                const auto yaw = yaw_degrees(unit.state.rotation);
                if (!yaw) { field_errors[slot] = yaw.error(); continue; }
                const Prediction still{unit.state.position, yaw.value()};
                const auto leaf = tracking_leaf(unit.state.entity_id, *footprint, still, still);
                if (!leaf) { field_errors[slot] = leaf.error(); continue; }
                field_slots[slot] = AsteroidFieldInput{{unit.state.entity_id, unit.state.owner,
                    {leaf.value().start.x, leaf.value().start.y, math::Fixed{}}}, footprint->radius};
            }
        });
        if (!prepared) return core::Result<void>::failure(prepared.error());
        // Ordered commit of partitioned geometry, not a new serial per-entity service.
        auto& bodies = asteroid_scratch.bodies;
        bodies.clear();
        for (std::size_t slot = 0; slot < field_slots.size(); ++slot) {
            if (field_errors[slot]) return core::Result<void>::failure(*field_errors[slot]);
            if (!field_slots[slot]) continue;
            fields.push_back(*field_slots[slot]);
            bodies.push_back(field_slots[slot]->body);
            field_reach = std::max(field_reach, field_slots[slot]->radius);
        }
        const auto indexed = field_index.rebuild_sorted(bodies);
        if (!indexed) return core::Result<void>::failure(indexed.error());
    }
    for (auto& candidates : asteroid_scratch.candidates) candidates.reserve(fields.size());
    for (auto& impacts : asteroid_impacts) impacts.reserve(fields.size());
    auto& capture_jobs = impl_->capture_jobs;
    auto& capture_inputs = impl_->capture_inputs;
    capture_jobs.clear();
    if ((tick + 1) % 4 == 0) {
        // Pad-only scheduling; occupied points do not prepare or query the world.
        capture_jobs.reserve(pads.size());
        for (const auto& [id, state] : pads) {
            if (state.under_construction != invalid_entity_id || state.constructed != invalid_entity_id) continue;
            const auto found = std::lower_bound(inputs.begin(), inputs.end(), id,
                [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
            if (found == inputs.end() || found->state.entity_id != id
                || (found->durability && found->durability->hull.raw() <= 0)) continue;
            const auto* profile = impl_->economy.pads.point(found->state.type_id);
            if (profile != nullptr) capture_jobs.push_back({id, static_cast<std::size_t>(found - inputs.begin()),
                profile, state, found->state.owner, found->state.owner, 0});
        }
    }
    auto& capture_index_bodies = systems_.value().capture_index_bodies.emplace(0);
    auto& capture_prepare_bodies = systems_.value().capture_prepare_bodies.emplace(0);
    if (!capture_jobs.empty()) {
        capture_inputs.resize(inputs.size());
        const auto& shared_index = world.value().index;
        // The combat index has this frame's moved positions. Commands can create or remove
        // objects, so partitions index only new/changed positions and query live copied inputs.
        for (std::size_t partition = 0; partition < tick_partition_count; ++partition) {
            auto& scratch = impl_->capture_scratch[partition];
            scratch.bodies.clear();
            scratch.error.reset();
            const auto range = partition_range(partition, inputs.size());
            scratch.bodies.reserve(range.end - range.begin);
            const auto jobs = partition_range(partition, capture_jobs.size());
            if (jobs.begin != jobs.end) {
                scratch.positions.reserve(inputs.size());
                scratch.candidates.reserve(inputs.size());
            }
        }
        const auto prepare_capture = [&](const std::size_t partition) {
            auto& scratch = impl_->capture_scratch[partition];
            const auto range = partition_range(partition, inputs.size());
            for (auto slot = range.begin; slot < range.end; ++slot) {
                const auto& unit = inputs[slot];
                const SpaceBody body{unit.state.entity_id, unit.state.owner, unit.state.position};
                capture_inputs[slot] = {body, unit.state.type_id,
                    !arrivals.contains(body.entity_id) && (!unit.durability || unit.durability->hull.raw() > 0)};
                const auto* indexed = shared_index.find(body.entity_id);
                if (indexed == nullptr || indexed->position != body.position) scratch.bodies.push_back(body);
            }
            const auto rebuilt = scratch.index.rebuild_sorted(scratch.bodies);
            if (!rebuilt) scratch.error = rebuilt.error();
        };
        // The executor owns a std::function; a one-reference wrapper fits its inline
        // storage, including on service ticks after all buffers have warmed.
        const auto prepared = executor.execute_phase("capture-prepare", tick_partition_count,
            [&prepare_capture](const std::size_t partition) { prepare_capture(partition); });
        if (!prepared) return core::Result<void>::failure(prepared.error());
        capture_prepare_bodies = inputs.size();
        for (const auto& scratch : impl_->capture_scratch) {
            if (scratch.error) return core::Result<void>::failure(*scratch.error);
            capture_index_bodies += scratch.bodies.size();
        }
        const auto service_capture_jobs = [&](const std::size_t partition) {
            auto& scratch = impl_->capture_scratch[partition];
            const auto range = partition_range(partition, capture_jobs.size());
            for (auto slot = range.begin; slot < range.end; ++slot) {
                auto& job = capture_jobs[slot];
                const auto position = inputs[job.slot].state.position;
                const auto radius = job.profile->radius;
                scratch.candidates.clear();
                const auto query = [&](const SpaceIndex& index) {
                    if (index.size() == 0) return;
                    index.box_positions(position, {radius, radius, radius}, scratch.positions, &job.inspected);
                    for (const auto body_slot : scratch.positions) {
                        const auto id = index.bodies()[body_slot].entity_id;
                        const auto found = std::lower_bound(capture_inputs.begin(), capture_inputs.end(), id,
                            [](const CaptureCandidate& entry, const EntityId key) { return entry.body.entity_id < key; });
                        if (found != capture_inputs.end() && found->body.entity_id == id
                            && within_range(position, found->body.position, radius, RangeMetric::spatial)) {
                            scratch.candidates.push_back(*found);
                        }
                    }
                };
                query(shared_index);
                for (const auto& prepared : impl_->capture_scratch) query(prepared.index);
                std::sort(scratch.candidates.begin(), scratch.candidates.end(), [](const auto& a, const auto& b) {
                    return a.body.entity_id < b.body.entity_id;
                });
                const auto unique = std::unique(scratch.candidates.begin(), scratch.candidates.end(), [](const auto& a, const auto& b) {
                    return a.body.entity_id == b.body.entity_id;
                });
                scratch.candidates.erase(unique, scratch.candidates.end());
                job.owner = service_capture(*job.profile, job.next, job.before, impl_->economy.pads.neutral,
                    impl_->setup.players, scratch.candidates, impl_->economy.pads, job.id, position);
            }
        };
        const auto captured = executor.execute_phase("capture-service", tick_partition_count,
            [&service_capture_jobs](const std::size_t partition) { service_capture_jobs(partition); });
        if (!captured) return core::Result<void>::failure(captured.error());
    }
    auto& instances = systems_.value().instances.emplace(inputs.size());
    auto& serviced = systems_.value().serviced.emplace(inputs.size());
    std::vector<std::optional<core::Diagnostic>> errors(inputs.size());
    std::vector<std::uint8_t> proxy_live;
    const bool barrage_bound = std::any_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(),
        [](const UnitAbilityProfile& profile) { return ability_slot(profile, AbilityKind::barrage).has_value(); });
    if (barrage_bound) {
        proxy_live.resize(inputs.size(), 1U);
        // Read the staged ability/health state before workers mutate their own inputs.
        const auto proxies = executor.execute_phase("barrage-proxies", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& unit = inputs[index];
                if (unit.state.barrage_source == invalid_entity_id) continue;
                const auto source = std::lower_bound(inputs.begin(), inputs.end(), unit.state.barrage_source,
                    [](const LiveUnit& candidate, const EntityId id) { return candidate.state.entity_id < id; });
                const auto* profile = source != inputs.end() && source->state.entity_id == unit.state.barrage_source
                    ? impl_->abilities.find(source->state.type_id) : nullptr;
                const auto slot = profile != nullptr ? ability_slot(*profile, AbilityKind::barrage) : std::nullopt;
                proxy_live[index] = source != inputs.end() && slot && source->abilities
                    && (!source->durability || source->durability->hull.raw() > 0)
                    && source->abilities->slots[*slot].active
                    && source->abilities->slots[*slot].target == unit.state.entity_id ? 1U : 0U;
            }
        });
        if (!proxies) return core::Result<void>::failure(proxies.error());
    }
    // WNO-23/42: transfer work is bounded by capture jobs and held bonus objects.
    // Each transferred unit changes owner inside the existing partitioned service.
    const bool transfers = std::any_of(capture_jobs.begin(), capture_jobs.end(),
        [](const CaptureJob& job) { return job.before != job.owner; });
    std::map<EntityId, TypeId> transfer_containers;
    std::vector<CombatBonuses> transfer_categories;
    if (transfers) {
        transfer_categories.resize(tick_partition_count * impl_->bonus_categories.size());
        for (const auto& account : staging_->ledgers.value()) for (const auto& held : account.completed) {
            const auto host = std::lower_bound(inputs.begin(), inputs.end(), held.station,
                [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; });
            if (host != inputs.end() && host->state.entity_id == held.station)
                transfer_containers.emplace(held.station, host->state.type_id);
        }
    }
    mark("systems_setup", false);
    // WSL-38/41: gather only active repair work inside the existing partitioned entity phase.
    std::array<std::vector<std::size_t>, tick_partition_count> repair_units;
    const auto executed = executor.execute_phase("unit-systems", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, inputs.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& unit = inputs[index];
            if (unit.state.barrage_source != invalid_entity_id && proxy_live[index] == 0U) {
                serviced[index].unit_destroyed = true;
                continue;
            }
            if (unit.abilities) {
                const auto& profile = *impl_->abilities.find(unit.state.type_id);
                if (const auto slot = ability_slot(profile, AbilityKind::barrage)) {
                    auto& active = unit.abilities->slots[*slot];
                    const auto target = std::lower_bound(inputs.begin(), inputs.end(), active.target,
                        [](const LiveUnit& candidate, const EntityId id) { return candidate.state.entity_id < id; });
                    if (!active.active || target == inputs.end() || target->state.entity_id != active.target) {
                        static_cast<void>(deactivate_ability(profile.abilities[*slot], active, tick));
                        if (unit.combat && unit.combat->attack_target == active.target) {
                            unit.combat->attack_target = invalid_entity_id;
                            unit.combat->direct = false;
                        }
                        if (unit.state.order.target == active.target) unit.state.order = {};
                        active.target = invalid_entity_id;
                    }
                }
            }
            const auto context = [&] {
                return "tick " + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": ";
            };
            const auto job = std::lower_bound(capture_jobs.begin(), capture_jobs.end(), unit.state.entity_id,
                [](const CaptureJob& entry, const EntityId id) { return entry.id < id; });
            if (job != capture_jobs.end() && job->id == unit.state.entity_id && job->before != job->owner) {
                // Resolve the new owner's contributions without changing the live identity.
                const auto categories = std::span(transfer_categories).subspan(
                    partition * impl_->bonus_categories.size(), impl_->bonus_categories.size());
                const auto bonuses = impl_->command_bonuses_for(unit, impl_->command_ledger,
                    staging_->ledgers.value(), transfer_containers, categories, nullptr, true, job->owner);
                const auto changed = impl_->transfer_owner(unit, job->owner, bonuses);
                if (!changed) {
                    errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, context() + changed.error().message);
                    continue;
                }
            }
            if (unit.durability) {
                const auto& profile = *impl_->health_profile(unit);
                if (const auto building = construction.find(unit.state.entity_id);
                    building != construction.end() && tick > building->second.start_frame && unit.durability->hull.raw() > 0) {
                    unit.durability->hull = service_construction(unit.durability->hull, profile.max_hull, building->second);
                }
                auto outcome = service_durability(profile, durability.rules, *unit.durability);
                if (!outcome) {
                    errors[index] = detail::diagnostic(
                        diagnostic_codes::worker_failure, context() + outcome.error().message);
                    continue;
                }
                serviced[index] = std::move(outcome).value();
                // With damage rules (#74): a lost last shield generator drops the shield (DG-17); the
                // pool (EN-02), then the shield (DG-13), recharge on the unit's own recharge frames.
                if (damage_rules != nullptr) {
                    auto& state = *unit.durability;
                    shield_generators_lost(profile, *damage_rules, state, tick);
                    auto recharged = core::Result<void>::success();
                    // AB-22, AB-23: an active ability scales the recharge intervals and amounts.
                    const auto factor = [&](const AbilityModifier modifier) { return impl_->ability_factor(unit, modifier); };
                    if (has_energy_pool(profile, *damage_rules) && tick >= state.next_energy_frame) {
                        state.next_energy_frame =
                            tick + scaled_interval(damage_rules->energy_recharge_frames, factor(AbilityModifier::energy_regen_interval));
                        recharged = recharge_energy(profile, *damage_rules, state, factor(AbilityModifier::energy_regen));
                    }
                    if (recharged && tick >= state.next_shield_frame) {
                        state.next_shield_frame =
                            tick + scaled_interval(damage_rules->shield_recharge_frames, factor(AbilityModifier::shield_regen_interval));
                        recharged = recharge_shields(profile, *damage_rules, state, tick, factor(AbilityModifier::shield_regen));
                        if (recharged && profile.max_shields.raw() > 0 && unit.combat) {
                            // WHZ-30/31: recharge first, then refresh contact every shield service.
                            // Neither membership nor a successful refresh cancels DEFEND.
                            auto& nebula = impl_->nebula_scratch;
                            auto& candidates = nebula.candidates[partition];
                            const math::Vec3 center{unit.state.position.x, unit.state.position.y, math::Fixed{}};
                            nebula.index.box_positions(center, {nebula.reach, nebula.reach, math::Fixed{}}, candidates);
                            bool contact = false;
                            for (const auto candidate : candidates) {
                                const auto& volume = nebula.volumes[candidate];
                                if (volume.ion_storm && volume.body.entity_id != unit.state.entity_id
                                    && within_range(center, volume.soft_center, volume.radius, RangeMetric::planar)) { contact = true; break; }
                            }
                            state.ion_storm_contact = contact ? std::optional(tick) : std::nullopt;
                        }
                    }
                    if (recharged) impl_->end_depleted_defend(unit, tick);
                    if (!recharged) {
                        errors[index] = detail::diagnostic(
                            diagnostic_codes::worker_failure, context() + recharged.error().message);
                        continue;
                    }
                }
            }
            const auto* footprint = impl_->motion.footprint(unit.state.type_id);
            // WHZ-10/11/13: failed service gates retain prior contact; moving means translation.
            const bool layer = footprint != nullptr && (footprint->layer == SpaceLayer::frigate
                || footprint->layer == SpaceLayer::capital || footprint->layer == SpaceLayer::super_capital);
            const bool locomotor_gate = footprint != nullptr && (!footprint->locomotor
                || (unit.speed.raw() > 0 && unit.motion && unit.motion->kind != MotionKind::turn
                    && !arrivals.contains(unit.state.entity_id)));
            if (!damage_blocked() && damage_rules != nullptr && damage_rules->asteroid_damage.raw() != 0
                && damage_rules->asteroid_rate.raw() != 0 && footprint != nullptr && footprint->asteroid_damage
                && layer && locomotor_gate && unit.combat && unit.durability && unit.durability->hull.raw() > 0) {
                asteroid_queries[index] = 1;
                bool contact = false;
                CombatRandom random(impl_->setup.seed, tick, unit.state.entity_id, asteroid_service_slot);
                // WHZ-12: predict only this unit's health to preserve successive hardpoint
                // selection and random draws. Actual damage belongs to the ordered commit.
                std::optional<LiveUnit> predicted;
                // Center XY at this logical frame; neither swept motion nor hull/height contact.
                auto& candidates = asteroid_scratch.candidates[partition];
                // Flat index coordinates keep height out of the broad phase as well as exact contact.
                const math::Vec3 center{unit.state.position.x, unit.state.position.y, math::Fixed{}};
                field_index.box_positions(center, {field_reach, field_reach, math::Fixed{}}, candidates);
                std::sort(candidates.begin(), candidates.end()); // sorted body positions are sorted IDs
                for (const auto position : candidates) {
                    const auto& field = fields[position];
                    const auto id = field.body.entity_id;
                    if (id == unit.state.entity_id
                        || !within_range(center, field.body.position, field_reach, RangeMetric::planar)) continue;
                    ++asteroid_candidates[index];
                    if (!within_range(center, field.body.position, field.radius, RangeMetric::planar)) continue;
                    contact = true;
                    // WHZ-12: each overlapping field consumes its own inclusive probability draw.
                    const auto draw = random.uniform(0, static_cast<std::uint32_t>(math::Fixed::scale));
                    if (static_cast<std::int64_t>(draw) > damage_rules->asteroid_rate.raw()) continue;
                    if (!predicted) predicted = unit;
                    const auto& profile = *impl_->health_profile(unit);
                    const auto selected = random_destroyable_hardpoint(profile, *predicted->durability, random);
                    const auto* combat_profile = impl_->combat.find(unit.state.type_id);
                    const auto selected_route = combat_profile != nullptr && selected != hull_target
                        ? damage_mesh_route(combat_profile->hardpoint_meshes, selected) : hull_target;
                    const auto route = damage_target_valid(profile, selected_route) ? selected_route : hull_target;
                    Hit hit;
                    hit.amount = damage_rules->asteroid_damage;
                    hit.damage_type = damage_rules->asteroid_damage_type;
                    hit.hardpoint = route;
                    hit.kind = HitKind::asteroid;
                    hit.source = id;
                    hit.internal_damage_misc = false;
                    const auto arrival_defense = unit.arrival_vulnerable_until ? impl_->economy.vulnerability : math::Fixed{};
                    hit.defense = math::Fixed::from_raw(unit.upgrade_bonuses[4].raw() + arrival_defense.raw());
                    hit.take_damage_multiplier = predicted->take_damage_mode;
                    asteroid_scratch.deliveries[index].push_back(hit);
                    if (!redirect_recipients(unit.state.entity_id).empty()) continue;
                    auto outcome = apply_hit(profile, *damage_rules, *predicted->durability, hit, tick);
                    if (!outcome) { errors[index] = outcome.error(); break; }
                    impl_->end_depleted_defend(*predicted, tick, outcome.value().storm_shield_branch);
                    if (outcome.value().damage.unit_destroyed) break;
                }
                unit.asteroid_contact = contact ? std::optional(tick) : std::nullopt;
                if (errors[index]) continue;
            }
            const auto transform = unit_transform(unit);
            if (!transform) {
                errors[index] = detail::diagnostic(
                    diagnostic_codes::worker_failure, context() + transform.error().message);
                continue;
            }
            if (unit.durability && !serviced[index].unit_destroyed && !arrivals.contains(unit.state.entity_id)
                && std::any_of(unit.durability->repairing_players.begin(), unit.durability->repairing_players.end(),
                    [](const auto& payers) { return !payers.empty(); })) repair_units[partition].push_back(index);
            instances[index] = impl_->instance_for(unit, transform.value(), tick + 1);
            // WSU-34: publish copied render inputs in the existing disjoint worker slots.
            if (const auto craft = craft_states.find(unit.state.entity_id); craft != craft_states.end()) {
                instances[index].craft_velocity_per_frame = craft->second.velocity;
            }
            if (const auto squadron = squadron_states.find(unit.state.entity_id); squadron != squadron_states.end()) {
                instances[index].squadron_in_idle_grid = squadron->second.idle_cell.has_value();
                if (squadron->second.idle_cell) instances[index].squadron_idle_anchor = squadron->second.anchor;
                // SND-46: copied path state in this existing disjoint snapshot slot; no new sim pass.
                const auto& flight = squadron->second;
                instances[index].has_movement_path = flight.mode == SquadronMode::move || flight.approach
                    || (flight.mode == SquadronMode::escort && !flight.idle_cell);
            }
        }
    });
    if (!executed) {
        return core::Result<void>::failure(executed.error());
    }
    for (auto& error : errors) {
        if (error) {
            return core::Result<void>::failure(std::move(*error));
        }
    }

    auto& events = impacts_.value().events.value();
    // VT-02/WCC-40/WHE-64: commit all staged environmental hits in source/field/member order.
    // Register each destruction before the next delivery, including another routed share.
    const auto deliver = [&](const EntityId id, Hit hit) -> core::Result<void> {
        if (damage_blocked()) return core::Result<void>::success();
        const auto found = std::lower_bound(inputs.begin(), inputs.end(), id,
            [](const LiveUnit& unit, const EntityId sought) { return unit.state.entity_id < sought; });
        if (found == inputs.end() || found->state.entity_id != id || !found->durability) return core::Result<void>::success();
        const auto index = static_cast<std::size_t>(found - inputs.begin());
        if (serviced[index].unit_destroyed) return core::Result<void>::success();
        auto& unit = *found;
        const auto* profile = impl_->health_profile(unit);
        if (!profile) return core::Result<void>::success();
        const auto hull = unit.durability->hull;
        const auto shields = unit.durability->shields;
        hit.take_damage_multiplier = unit.take_damage_mode;
        hit.defense = math::Fixed::from_raw(unit.upgrade_bonuses[4].raw()
            + (unit.arrival_vulnerable_until ? impl_->economy.vulnerability.raw() : 0));
        const auto applied = apply_hit(*profile, *damage_rules, *unit.durability, hit, tick);
        if (!applied) return core::Result<void>::failure(applied.error());
        impl_->track_damage(unit, hull, shields);
        impl_->end_depleted_defend(unit, tick, applied.value().storm_shield_branch);
        asteroid_impacts[index].push_back({id, hit, applied.value()});
        if (applied.value().damage.destroyed_hardpoint)
            events.push_back(destruction_event(tick, EventKind::hardpoint_destroyed,
                unit.state, *applied.value().damage.destroyed_hardpoint));
        if (applied.value().damage.unit_destroyed) {
            serviced[index].unit_destroyed = true;
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit.state));
        }
        const auto updated = impl_->instance_for(unit, instances[index].fixed_transform, tick + 1);
        instances[index].durability = updated.durability;
        instances[index].abilities = updated.abilities;
        return core::Result<void>::success();
    };
    for (std::size_t source = 0; source < inputs.size(); ++source) {
        for (const auto hardpoint : serviced[source].destroyed_hardpoints)
            events.push_back(destruction_event(tick, EventKind::hardpoint_destroyed, inputs[source].state, hardpoint));
        if (serviced[source].unit_destroyed && !loss_ids_.contains(inputs[source].state.entity_id))
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, inputs[source].state));
        if (serviced[source].unit_destroyed) continue;
        for (const auto& incoming : asteroid_scratch.deliveries[source]) {
            if (damage_blocked() || serviced[source].unit_destroyed) break;
            const auto routed = redirect_damage(inputs[source].state.entity_id, incoming, deliver);
            if (!routed) return core::Result<void>::failure(routed.error());
            if (!routed.value()) {
                if (auto delivered = deliver(inputs[source].state.entity_id, incoming); !delivered) return delivered;
            }
        }
    }

    struct RepairJob {
        std::size_t unit{};
        std::vector<std::vector<PlayerId>> paid;
    };
    std::vector<RepairJob> repairs;
    auto& accounts = staging_->ledgers.value();
    std::vector<RepairBudget> budgets;
    if (std::any_of(repair_units.begin(), repair_units.end(), [](const auto& part) { return !part.empty(); }))
        for (const auto& account : accounts) budgets.push_back({account.player, account.credits});
    // WSL-41: reserve shared credits in unit/index/registration order. This sparse commit
    // visits staged repairs only; health/hull arithmetic remains in disjoint worker outputs.
    for (const auto& partition : repair_units) for (const auto index : partition) {
        auto& unit = inputs[index];
        if (serviced[index].unit_destroyed) continue; // deletion wins over active repair
        const auto& profile = *impl_->health_profile(unit);
        const auto& state = *unit.durability;
        RepairJob job{index, reserve_hardpoint_repairs(profile, state, budgets)};
        repairs.push_back(std::move(job));
    }
    for (const auto& budget : budgets) {
        if (auto* account = impl_->ledger_of(accounts, budget.player)) {
            // WPR-12: reserved repair spending is a debit, without positive AI credit scaling.
            const auto debit = math::Fixed::from_raw(budget.credits.raw() - account->credits.raw());
            const auto changed = change_credits(*account, *impl_->economy.player(budget.player), debit);
            if (!changed) return core::Result<void>::failure(changed.error());
        }
    }
    if (!repairs.empty()) {
        const auto repaired = executor.execute_phase("hardpoint-repair", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, repairs.size());
            for (auto job_index = range.begin; job_index < range.end; ++job_index) {
                auto& job = repairs[job_index];
                auto& unit = inputs[job.unit];
                const auto& profile = *impl_->health_profile(unit);
                auto& state = *unit.durability;
                for (std::size_t slot = 0; slot < job.paid.size(); ++slot) {
                    state.repairing_players[slot] = std::move(job.paid[slot]);
                    for (const auto payer : state.repairing_players[slot]) {
                        static_cast<void>(payer);
                        const auto result = repair_frame(profile, state, slot, profile.hardpoints[slot].repair_cost_per_frame);
                        if (!result) { errors[job.unit] = result.error(); break; }
                        if (result.value().stopped) { state.repairing_players[slot].clear(); break; }
                    }
                }
                const auto updated = impl_->instance_for(unit, instances[job.unit].fixed_transform, tick + 1);
                instances[job.unit].durability = updated.durability;
            }
        });
        if (!repaired) return repaired;
        for (const auto& job : repairs) if (errors[job.unit]) return core::Result<void>::failure(*errors[job.unit]);
    }
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::commit_survivors() {
    auto& tick_work = gather_.value().tick_work.value();
    auto& events = impacts_.value().events.value();
    auto& killed = impacts_.value().killed.value();
    auto& inputs = systems_.value().inputs.value();
    auto& instances = systems_.value().instances.value();
    const auto& serviced = systems_.value().serviced.value();
    auto& pads = impl_->pad_stage.values;
    auto& capture_jobs = impl_->capture_jobs;
    // Serial, in ascending ID: destruction hooks already ran at delivery; dead units leave (HD-20).
    mark("survivors", true);
    surviving_.emplace();
    auto& survivors = surviving_.value().survivors.emplace();
    std::vector<TacticalInstance> surviving_instances;
    survivors.reserve(inputs.size());
    surviving_instances.reserve(inputs.size());
    auto& capture_candidates = surviving_.value().capture_candidates.emplace(0);
    for (const auto& job : capture_jobs) {
        capture_candidates += job.inspected;
        if (pads.at(job.id) != job.next) {
            impl_->pad_stage.touch(job.id);
            pads.at(job.id) = job.next;
        }
    }
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        const auto job = std::lower_bound(capture_jobs.begin(), capture_jobs.end(), inputs[index].state.entity_id,
            [](const CaptureJob& entry, const EntityId id) { return entry.id < id; });
        if (job != capture_jobs.end() && job->id == inputs[index].state.entity_id && job->owner != job->before) {
            auto before = inputs[index].state;
            before.owner = job->before;
            track_victory_change(&before, &inputs[index].state, true);
            // WNO-23: sequence carries the previous owner for selection/control-group removal.
            events.push_back(Event{tick, EventKind::pad_captured, job->owner, job->before, job->id});
        }
        tick_work.level_matrix_builds += inputs[index].matrix_builds;
        tick_work.banked_matrix_builds += inputs[index].banked_matrix_builds;
        if (serviced[index].unit_destroyed) {
            killed.push_back(inputs[index].state);
            continue;
        }
        survivors.push_back(std::move(inputs[index]));
        surviving_instances.push_back(std::move(instances[index]));
    }
    instances = std::move(surviving_instances);

    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
