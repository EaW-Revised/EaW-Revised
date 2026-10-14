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

core::Result<void> session_detail::Tick::special_weapons() {
    if (special_weapons_.empty()) return core::Result<void>::success();
    const auto& staged = tracked_->staged.value();
    const auto& view = targets_->world->value();
    const auto& accounts = staging_->ledgers.value();
    struct Service {
        EntityId id{};
        SpecialWeaponState state;
        bool enabled{}, ready{}, removed{};
        std::optional<std::size_t> shot;
    };
    std::vector<Service> services;
    services.reserve(special_weapons_.size());
    // WSW-20/34: prepare only registered sources, never a live-world discovery sweep.
    // Jobs read copied command/source clocks and immutable target poses; each writes its own slot.
    for (const auto& [id, state] : special_weapons_) {
        Service service;
        service.id = id; service.state = state;
        const auto source = staged.find(id);
        const auto* source_view = view.find(id);
        const auto* profile = source != staged.end() ? impl_->combat.find(source->second.state.type_id) : nullptr;
        if (source == staged.end() || !source_view || !profile || !profile->planetary_weapon) service.removed = true;
        else {
            service.enabled = impl_->special_weapons_enabled(source->second.state.owner, accounts, &staged, &special_disabled_by_hardpoint_);
            if (!special_initial_.contains(id) && state.pulses_left && tick >= state.next_pulse_frame) {
                const auto target = staged.find(state.target);
                const auto* target_view = view.find(state.target);
                if (target != staged.end() && target_view) {
                    SpecialShot shot;
                    shot.source = *source_view; shot.target = *target_view;
                    shot.source.position = source->second.state.position;
                    // WWP-73/74: later planetary pulses retain the current rolled source/target pose.
                    const auto source_rotation = banked_rotation(source->second.state.rotation, source->second.roll);
                    if (!source_rotation) return core::Result<void>::failure(source_rotation.error());
                    const auto source_transform = math::to_matrix(source_rotation.value(), shot.source.position);
                    if (!source_transform) return core::Result<void>::failure(source_transform.error());
                    shot.source.transform = source_transform.value();
                    shot.target.position = target->second.state.position;
                    const auto target_rotation = banked_rotation(target->second.state.rotation, target->second.roll);
                    if (!target_rotation) return core::Result<void>::failure(target_rotation.error());
                    const auto target_transform = math::to_matrix(target_rotation.value(), shot.target.position);
                    if (!target_transform) return core::Result<void>::failure(target_transform.error());
                    shot.target.transform = target_transform.value();
                    shot.target_health = target->second.durability;
                    if (const auto* health = impl_->health_profile(target->second)) shot.target_health_profile = *health;
                    shot.pulse = profile->weapons.front().pulse_count - state.pulses_left;
                    service.shot = special_shots_.size();
                    special_shots_.push_back(std::move(shot));
                }
                // WSW-21: absent target launches nothing; retained counter cleanup remains unverified.
            }
        }
        services.push_back(std::move(service));
    }
    std::vector<std::optional<core::Result<detail::CombatStep>>> shots(special_shots_.size());
    const auto prepared = executor.execute_phase("special-weapons", tick_partition_count, [&](const std::size_t partition) {
        const auto shot_range = partition_range(partition, special_shots_.size());
        for (auto index = shot_range.begin; index < shot_range.end; ++index) {
            const auto& input = special_shots_[index];
            auto target = input.target;
            target.durability = input.target_health ? &*input.target_health : nullptr;
            target.durability_profile = input.target_health_profile ? &*input.target_health_profile : nullptr;
            shots[index] = detail::step_planetary_fire(view, input.source, target);
        }
        const auto service_range = partition_range(partition, services.size());
        for (auto index = service_range.begin; index < service_range.end; ++index) {
            auto& service = services[index];
            if (service.removed) continue;
            if (service.shot) {
                // WSW-22: the nonnull/distinct wrapper reports success even when no projectile was constructed.
                const auto& input = special_shots_[*service.shot];
                --service.state.pulses_left;
                service.state.next_pulse_frame += input.source.profile->planetary_weapon->pulse_delay_frames;
                if (!service.state.pulses_left) service.state.target = invalid_entity_id;
            }
            if (service.state.recharge_until > 0 && tick >= service.state.recharge_until && service.enabled) {
                service.ready = true;
                service.state.recharge_until = 0;
            }
        }
    });
    if (!prepared) return prepared;
    auto& combat_events = targets_->combat_events.value();
    auto& projectiles = impacts_->projectiles.value();
    auto& events = impacts_->events.value();
    auto& next_projectile = flights_->next_projectile.value();
    for (std::size_t index = 0; index < shots.size(); ++index) {
        if (!shots[index] || !*shots[index]) return core::Result<void>::failure(shots[index]
            ? shots[index]->error() : detail::diagnostic(diagnostic_codes::worker_failure, "missing special weapon slot"));
        const auto& input = special_shots_[index];
        const auto& output = shots[index]->value();
        if (input.pulse) events.push_back(Event{tick, EventKind::special_weapon_fired, input.source.owner,
            input.source.profile->planetary_weapon->index, input.source.id, OrderKind::none, RejectReason::none, 0, input.pulse});
        combat_events.insert(combat_events.end(), output.events.begin(), output.events.end());
        if (!flights_->damage_rules.value()) continue;
        std::size_t offset = 0;
        for (const auto& event : output.events) {
            if (event.kind != CombatEventKind::weapon_fired) continue;
            const auto& shot = input.source.profile->weapons.front().shot;
            if (!shot) continue;
            const auto target_radius = detail::projectile_target_radius(view, input.source, &input.target);
            if (!target_radius) return core::Result<void>::failure(target_radius.error());
            auto projectile = detail::launch_projectile(event, *shot, input.source.owner, true, next_projectile++,
                output.aim_offsets[offset++], target_radius.value());
            if (!projectile) return core::Result<void>::failure(projectile.error());
            projectiles.push_back(std::move(projectile).value());
        }
    }
    // Ordered commits retain deterministic projectile identities and readiness notification order.
    for (const auto& service : services) {
        if (service.removed) { special_weapons_.erase(service.id); continue; }
        special_weapons_.at(service.id) = service.state;
        if (service.ready) {
            const auto* source = view.find(service.id);
            events.push_back(Event{tick, EventKind::special_weapon_ready, source->owner,
                source->profile->planetary_weapon->index, service.id});
        }
    }
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::ion_team() {
    auto& moving = gather_.value().moving.value();
    auto& minds = craft_prep_.value().minds.value();
    const auto& squadron_table = impl_->motion.squadrons;
    // AB-63 to AB-68 (#561): each squadron's ION_CANNON_SHOT, in container order. Serial because
    // it reads and writes a container and its craft together and orders the squadron, which the
    // tick does serially (FO-01); it visits only squadrons whose team type has the ability.
    const auto lock_ion_shot = [&](LiveUnit& container, AbilitySlot& team, const Squadron& squadron,
                                   SquadronState& mind, const EntityId target, const std::uint32_t hardpoint) {
        team.active = true;
        team.started_tick = tick;
        team.target = target;
        team.target_hardpoint = hardpoint;
        for (const auto member : squadron.members) {
            auto* craft = live_unit(moving, member);
            if (auto* slot = craft != nullptr ? impl_->ion_slot(*craft) : nullptr) slot->active = true;
        }
        // AB-63: the squadron attacks the target.
        const CommandPayload attack = AttackPayload{target};
        container.state.order = order_for(attack, tick);
        apply_squadron_order(mind, attack, container.state.position, squadron_table);
    };
    for (const auto& squadron : impl_->squadrons) {
        auto* container = live_unit(moving, squadron.container);
        auto* team = container != nullptr ? impl_->ion_slot(*container) : nullptr;
        const auto mind = minds.find(squadron.container);
        if (team == nullptr || mind == minds.end()) continue;
        std::vector<AbilitySlot*> craft;
        for (const auto member : squadron.members) {
            auto* live = live_unit(moving, member);
            if (auto* slot = live != nullptr ? impl_->ion_slot(*live) : nullptr) craft.push_back(slot);
        }
        if (team->active) {
            // AB-64, AB-65: it ends once no craft still has its shot due, or its target is gone.
            const bool due = std::any_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; });
            if (!due || (container->nebula && container->nebula->present)
                || !impl_->ion_target_valid(container->state.owner, live_unit(moving, team->target))) {
                impl_->finish_ion_shot(*team, craft, container->state.type_id, tick);
            } else if (mind->second.target != team->target) {
                // AB-63: while it is on, the squadron keeps attacking the target.
                lock_ion_shot(*container, *team, squadron, minds.at(mind->first), team->target, team->target_hardpoint);
            }
            continue;
        }
        // AB-68: on autofire (always for a player the engine plays, AB-40), a ready ion shot locks
        // onto the squadron's attack target.
        const bool autofire = team->autofire || !impl_->abilities.human(container->state.owner);
        const auto target = mind->second.target;
        if (autofire && !(container->nebula && container->nebula->present) && tick >= team->ready_tick && !craft.empty()
            && std::none_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; })
            && impl_->ion_target_valid(container->state.owner, live_unit(moving, target))) {
            lock_ion_shot(*container, *team, squadron, minds.at(mind->first), target, no_hardpoint);
        }
    }

    // Orders phase (#452, docs/behaviour/space-orders.md OR-06): every reevaluation interval after
    // its order, a unit approaching another (attack, attack-move or guard on a unit) checks whether
    // it keeps its movement. Workers read the moved units with this tick's targets and write each
    // unit's new approach to its own slot; the paths plan serially in ascending ID after the
    // attack turns, like every plan with avoidance (AV-15).
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::projectile_inputs() {
    const auto& moving = gather_.value().moving.value();
    // WLD-03..WLD-05: point defence shoots down shots before this tick flies them; the zapped shot's
    // explicit-request expiry below detonates it where it is.
    if (auto zapped = impl_->service_laser_defence(moving, tick, executor, targets_.value().combat_events.value()); !zapped) return zapped;
    const auto& world = targets_.value().world.value();
    const auto& view = world.value();
    const auto& durability = impl_->durability;
    // Projectile phase (#74, DG-30): with damage rules, workers fly every projectile that was in
    // flight at the start of the tick one frame through the same immutable world view and write
    // its outcome to its own slot. The shots of this tick start flying next tick (DG-21).
    flights_.emplace();
    flights_.value().damage_rules.emplace(durability.damage ? &*durability.damage : nullptr);
    const auto& flying = impl_->projectiles;
    auto& flights = flights_.value().flights.emplace(flying.size());
    auto& weaken_recipients = flights_->weaken_recipients;
    weaken_recipients.resize(flying.size());
    std::vector<std::optional<core::Diagnostic>> flight_errors(flying.size());
    auto& projectile_candidates = flights_.value().projectile_candidates.emplace(0);
    auto& projectile_exact_tests = flights_.value().projectile_exact_tests.emplace(0);
    bool detonations_due = false;
    if (!flying.empty()) {
        const auto flown = executor.execute_phase("projectiles", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, flying.size());
            auto& scratch = impl_->projectile_scratch[partition]; // #636: only this partition's
            scratch.candidate_count = 0;
            scratch.exact_count = 0;
            for (auto index = range.begin; index < range.end; ++index) {
                const auto spawn = std::lower_bound(impl_->ability_spawns.begin(), impl_->ability_spawns.end(), flying[index].id,
                    [](const auto& entry, const std::uint64_t id) { return entry.id < id; });
                if (spawn != impl_->ability_spawns.end() && spawn->id == flying[index].id) {
                    detail::ProjectileStep step;
                    step.projectile = flying[index];
                    step.from = flying[index].position;
                    step.expired = tick >= spawn->due;
                    step.projectile.explosion_requested = step.expired;
                    if (spawn->kind == AbilityKind::cluster_bomb || spawn->kind == AbilityKind::fow_reveal_ping) {
                        auto projectile = flying[index];
                        projectile.explosion_requested = projectile.explosion_requested
                            || (spawn->due != 0 && tick >= spawn->due);
                        const auto flown = detail::step_projectile(view, projectile, scratch);
                        if (!flown) { flight_errors[index] = flown.error(); continue; }
                        step = flown.value();
                    }
                    flights[index] = step;
                    const auto* profile = impl_->spawn_profile(spawn->type);
                    if (step.expired && profile && profile->weaken.on_detonation) {
                        const auto radius = profile->weaken.radius;
                        for (const auto& player : view.players) {
                            // WHE-29: different owner, category and combat-modifier recipient.
                            if (player.player_id == spawn->owner) continue;
                            for (const auto id : view.candidates(spawn->position, radius, player.player_id)) {
                                const auto* target = view.find(id);
                                if (!target || !target->profile || !target->profile->collision
                                    || (target->profile->category_bits & profile->weaken.categories) == 0) continue;
                                // U-10 boundary default: sphere against the transformed model box.
                                const auto& matrix = target->transform.rows;
                                const auto delta = math::Vec3{
                                    math::Fixed::from_raw(spawn->position.x.raw() - target->position.x.raw()),
                                    math::Fixed::from_raw(spawn->position.y.raw() - target->position.y.raw()),
                                    math::Fixed::from_raw(spawn->position.z.raw() - target->position.z.raw())};
                                const std::array<math::Fixed, 3> coordinates{delta.x, delta.y, delta.z};
                                std::array<math::Fixed, 3> local{};
                                bool valid = true;
                                for (std::size_t axis = 0; axis < 3; ++axis) for (std::size_t row = 0; row < 3; ++row) {
                                    auto term = math::multiply(matrix[row][axis], coordinates[row]);
                                    if (!term) { flight_errors[index] = term.error(); valid = false; continue; }
                                    auto sum = math::add(local[axis], term.value());
                                    if (!sum) { flight_errors[index] = sum.error(); valid = false; continue; }
                                    local[axis] = sum.value();
                                }
                                if (!valid) continue;
                                const auto& box = *target->profile->collision;
                                const math::Vec3 here{local[0], local[1], local[2]};
                                const math::Vec3 nearest{std::clamp(here.x, box.min.x, box.max.x),
                                    std::clamp(here.y, box.min.y, box.max.y), std::clamp(here.z, box.min.z, box.max.z)};
                                if (within_range(here, nearest, radius, RangeMetric::spatial)) weaken_recipients[index].push_back(id);
                            }
                        }
                    }
                    continue;
                }
                auto stepped = detail::step_projectile(view, flying[index], scratch);
                if (!stepped) {
                    flight_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, "tick "
                        + std::to_string(tick) + " projectile " + std::to_string(flying[index].id) + ": "
                        + stepped.error().message);
                    continue;
                }
                flights[index] = std::move(stepped).value();
            }
        });
        if (!flown) {
            return core::Result<void>::failure(flown.error());
        }
        for (std::size_t index = 0; index < flight_errors.size(); ++index) {
            if (flight_errors[index]) {
                return core::Result<void>::failure(std::move(*flight_errors[index]));
            }
            detonations_due = detonations_due || ((flights[index]->hit || flights[index]->expired)
                && flights[index]->projectile.blast.enabled());
        }
        for (const auto& scratch : impl_->projectile_scratch) {
            projectile_candidates += scratch.candidate_count;
            projectile_exact_tests += scratch.exact_count;
        }
    }
    // This tick's shots become projectiles in event order (ascending shooter, then weapon).
    // WAD: detonation-only recipient work reads copied collidables and writes disjoint slots.
    auto& blasts = flights_.value().blasts.emplace(detonations_due ? flights.size() : 0);
    auto& blasts_without_source = flights_.value().blasts_without_source.emplace(blasts.size());
    std::vector<std::optional<core::Diagnostic>> blast_errors(blasts.size());
    auto& blast_detonations = flights_.value().blast_detonations.emplace(0);
    auto& blast_recipients_examined = flights_.value().blast_recipients_examined.emplace(0);
    const auto prepared_blasts = detonations_due ? executor.execute_phase("blast-recipients", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, flights.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& flight = *flights[index];
            if ((!flight.hit && !flight.expired) || !flight.projectile.blast.enabled()) continue;
            auto prepared = detail::prepare_blast(view, flight.projectile,
                flight.hit ? flight.contact : flight.projectile.position, flight.hit, true,
                detail::direct_blast_mesh(view, flight));
            if (!prepared) blast_errors[index] = prepared.error();
            else blasts[index] = std::move(prepared).value();
            const auto* source = view.find(flight.projectile.shooter);
            if (source != nullptr && source->scatter_radius.raw() != math::Fixed::scale) {
                // WAD-10: an earlier ordered hit can remove the source before this detonation.
                // Prepare that alternative here too; commit never runs a recipient query.
                auto fallback = detail::prepare_blast(view, flight.projectile,
                    flight.hit ? flight.contact : flight.projectile.position, flight.hit, false,
                    detail::direct_blast_mesh(view, flight));
                if (!fallback) blast_errors[index] = fallback.error();
                else blasts_without_source[index] = std::move(fallback).value();
            }
        }
    }) : core::Result<void>::success();
    if (!prepared_blasts) return core::Result<void>::failure(prepared_blasts.error());
    for (std::size_t index = 0; index < blasts.size(); ++index) {
        if (blast_errors[index]) return core::Result<void>::failure(*blast_errors[index]);
        if (!blasts[index]) continue;
        ++blast_detonations;
        blast_recipients_examined += blasts[index]->examined;
        if (blasts_without_source[index]) blast_recipients_examined += blasts_without_source[index]->examined;
    }
    flights_.value().launched.emplace();
    flights_.value().next_projectile.emplace(impl_->next_projectile);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::launch_shots() {
    const auto& moving = gather_->moving.value();
    const auto& combat_events = targets_->combat_events.value();
    const auto& aim_offsets = targets_->aim_offsets.value();
    const auto& view = targets_->world.value().value();
    const auto* damage_rules = flights_->damage_rules.value();
    auto& launched = flights_->launched.value();
    auto& next_projectile = flights_->next_projectile.value();
    launched.clear();
    if (damage_rules != nullptr) {
        struct LaunchInput {
            const CombatEvent* event{};
            const ShotProfile* shot{};
            math::Vec3 offset{};
            std::uint64_t id{};
        };
        std::vector<LaunchInput> inputs;
        std::size_t shot_index{};
        // IDs and event order are committed serially; geometry and launch arithmetic belong
        // to the partitioned phase, just like existing projectile flight (ADR-009).
        for (const auto& event : combat_events) {
            if (event.kind != CombatEventKind::weapon_fired) continue;
            const auto offset = shot_index < aim_offsets.size() ? aim_offsets[shot_index] : math::Vec3{};
            ++shot_index;
            const auto* shooter = view.find(event.shooter);
            const auto& weapons = shooter->profile->weapons;
            const auto weapon = std::find_if(weapons.begin(), weapons.end(),
                [&](const WeaponProfile& entry) { return entry.hardpoint == event.weapon; });
            // AB-66 (#561): an ion shot flies the weapon's ability shot.
            if (weapon == weapons.end()) continue;
            const auto& shot = (event.outcome & fired_ability_shot) != 0U ? weapon->ability_shot
                : (event.outcome & fired_barrage_shot) != 0U ? weapon->barrage_shot
                : (event.outcome & fired_charged_shot) != 0U && weapon->charged_shot ? weapon->charged_shot : weapon->shot;
            if (!shot) continue;
            inputs.push_back({&event, &*shot, offset, next_projectile++});
        }
        std::vector<std::optional<Projectile>> prepared(inputs.size());
        std::vector<std::optional<core::Diagnostic>> errors(inputs.size());
        const auto launches = !inputs.empty() ? executor.execute_phase("projectile-launches", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& input = inputs[index];
                const auto& event = *input.event;
                const auto* shooter = view.find(event.shooter);
                // DG-05: the shooter's mode flag, on by default and only off without a durability
                // profile's data saying otherwise; no modelled ability changes it (space-abilities AB-25).
                const bool allow_diminishing_firepower =
                    shooter->durability_profile == nullptr || shooter->durability_profile->allow_diminishing_firepower;
                const auto* target = view.find(event.target);
                const auto target_radius = detail::projectile_target_radius(view, *shooter, target);
                if (!target_radius) { errors[index] = target_radius.error(); continue; }
                auto projectile = detail::launch_projectile(
                    event, *input.shot, shooter->owner, allow_diminishing_firepower, input.id, input.offset, target_radius.value());
                if (!projectile) {
                    errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                        "tick " + std::to_string(tick) + " unit " + std::to_string(event.shooter) + ": "
                            + projectile.error().message);
                    continue;
                }
                projectile.value().profile_key = ProjectileProfileKey{shooter->type_id, event.weapon,
                    (event.outcome & fired_ability_shot) != 0U ? ProjectileProfileKind::ability
                    : (event.outcome & fired_barrage_shot) != 0U ? ProjectileProfileKind::barrage
                    : (event.outcome & fired_charged_shot) != 0U ? ProjectileProfileKind::charged_blast : ProjectileProfileKind::ordinary};
                if ((event.outcome & fired_charged_shot) != 0U) {
                    // WAD-36: instance direct damage only; selected-type area budget is unchanged.
                    const auto multiplied = math::multiply(projectile.value().damage, shooter->blast_damage_multiplier);
                    if (!multiplied) { errors[index] = multiplied.error(); continue; }
                    projectile.value().damage = multiplied.value();
                }
                const auto source = live_unit(moving, event.shooter);
                if (source != nullptr && source->upgrade_bonuses[1].raw() != 0) {
                    // Retain the separate delayed-area input; immediate delivery uses EUS-16 below.
                    projectile.value().source_damage_factor = math::Fixed::from_raw(
                        math::Fixed::scale + source->upgrade_bonuses[1].raw());
                }
                prepared[index] = projectile.value();
            }
        }) : core::Result<void>::success();
        if (!launches) return launches;
        for (std::size_t index = 0; index < prepared.size(); ++index) {
            if (errors[index]) return core::Result<void>::failure(*errors[index]);
            if (prepared[index]) launched.push_back(*prepared[index]);
        }
    }
    return core::Result<void>::success();
}

const LiveUnit* session_detail::Tick::redirect_unit(const EntityId target) const {
    // Systems releases the journal into its immutable-ID input vector before partition work.
    if (systems_ && systems_->inputs) return live_unit(*systems_->inputs, target);
    const auto found = tracked_->staged->find(target);
    return found != tracked_->staged->end() ? &found->second : nullptr;
}

void session_detail::Tick::notify_retaliation(const LiveUnit& unit, const EntityId attacker,
    const math::Fixed amount) {
    if (attacker == invalid_entity_id || amount.raw() <= 0 || unit.take_damage_mode.raw() <= 0
        || (impl_->health_profile(unit) && impl_->health_profile(unit)->scenario_invulnerable)) return;
    const auto& parents = craft_prep_->craft_squadron.value();
    const auto parent = parents.find(unit.state.entity_id);
    const auto recipient = parent != parents.end() ? parent->second : unit.state.entity_id;
    // The moved vector has transferred into the damage stage before notifications arrive.
    const auto& damage_units = tracked_->staged.value();
    const auto parent_unit = damage_units.find(recipient);
    const auto* receiver = recipient == unit.state.entity_id ? &unit
        : parent_unit != damage_units.end() ? &parent_unit->second : nullptr;
    if (!receiver || !std::binary_search(impl_->combat.unit_ai_types.begin(), impl_->combat.unit_ai_types.end(),
        receiver->state.type_id)) return;
    const auto source = parents.find(attacker);
    retaliation_notifications_.emplace_back(recipient, source != parents.end() ? source->second : attacker);
}

core::Result<void> session_detail::Tick::retaliation() {
    if (retaliation_notifications_.empty()) return core::Result<void>::success();
    auto& staged = tracked_->staged.value();
    const auto& view = targets_->world.value().value();
    const auto& damage_units = staged;
    auto& minds = craft_prep_->minds.value();
    // WTA-29: sparse damage notifications are grouped in delivery order. Workers read the
    // immutable damage journal and own one recipient's staged combat result (ADR-009).
    struct Job {
        EntityId recipient{};
        EntityId firing{};
        std::vector<EntityId> attackers;
        std::optional<CombatState> result;
        std::optional<core::Diagnostic> error;
    };
    std::map<EntityId, std::vector<EntityId>> grouped;
    for (const auto& [recipient, attacker] : retaliation_notifications_) grouped[recipient].push_back(attacker);
    const std::set<std::pair<EntityId, EntityId>> threats(retaliation_notifications_.begin(), retaliation_notifications_.end());
    std::vector<Job> jobs;
    for (auto& [id, attackers] : grouped) jobs.push_back({id, 0, std::move(attackers), {}, {}});
    const auto prepared = executor.execute_phase("unit-ai-retaliation", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, jobs.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& job = jobs[index];
            const auto receiver = damage_units.find(job.recipient);
            if (receiver == damage_units.end()) continue;
            const auto mind = minds.find(job.recipient);
            const LiveUnit* firing = &receiver->second;
            if (!firing->combat && mind != minds.end()) {
                // WTA-29: a container without targeting fires through its first live leader.
                firing = nullptr;
                for (const auto member : mind->second.roster) {
                    const auto live = damage_units.find(member);
                    if (live != damage_units.end() && live->second.combat) { firing = &live->second; break; }
                }
            }
            if (!firing || !firing->combat) continue;
            job.firing = firing->state.entity_id;
            const auto* copied = view.find(job.firing);
            if (!copied || !copied->profile) continue;
            auto unit = *copied;
            unit.durability = firing->durability ? &*firing->durability : nullptr;
            unit.durability_profile = impl_->health_profile(*firing);
            auto state = *firing->combat;
            if (mind != minds.end()) {
                state.attack_target = mind->second.target;
                state.attack_hardpoint = mind->second.target_hardpoint;
                state.direct = receiver->second.state.order.kind == OrderKind::attack;
            }
            detail::FormationAttackContext formation;
            math::Fixed divert{};
            math::Vec3 anchor{};
            const auto kind = receiver->second.state.order.kind;
            // WMV-17/U-07: a completed ship move retains its order/formation. A wait is
            // only the planning delay, so checking motion or FormationWait alone is wrong.
            const bool layered_ship = impl_->motion.find(firing->state.type_id)
                && !impl_->motion.squadrons.find_craft(firing->state.type_id);
            if (mind == minds.end()) {
                formation.present = layered_ship && (receiver->second.formation || kind == OrderKind::move
                    || kind == OrderKind::attack || kind == OrderKind::attack_move || kind == OrderKind::guard);
            } else if (const auto* profile = impl_->motion.squadrons.find_squadron(mind->second.squadron_type)) {
                const auto& team = mind->second;
                formation.present = team.formation.has_value();
                formation.leader = &unit;
                divert = team.diversion == SquadronDiversion::attack_move ? profile->attack_move_response_range
                    : (team.mode == SquadronMode::escort || team.diversion == SquadronDiversion::guard)
                        ? profile->guard_chase_range : profile->idle_chase_range;
                anchor = team.anchor;
                if (team.formation) {
                    const auto& held = *team.formation;
                    anchor = held.base_position;
                    if (const auto destination = damage_units.find(held.base_target); destination != damage_units.end())
                        anchor = destination->second.state.position;
                    const bool escort = team.mode == SquadronMode::escort;
                    formation.unbounded = !held.complete && team.diversion == SquadronDiversion::attack_move;
                    formation.allows_divert = formation.unbounded || ((held.has_reached_done || escort)
                        && (held.base_target == invalid_entity_id || escort));
                    const auto frame = craft_prep_->squadron_frames.value().find(job.recipient);
                    if (frame != craft_prep_->squadron_frames.value().end()) {
                        const auto offset = roster_offset(frame->second, job.firing);
                        const auto x = math::add(anchor.x, offset.x), y = math::add(anchor.y, offset.y);
                        if (!x || !y) { job.error = !x ? x.error() : y.error(); continue; }
                        anchor.x = x.value(); anchor.y = y.value();
                    }
                }
            }
            for (const auto attacker : job.attackers) {
                const auto live_attacker = damage_units.find(attacker);
                const auto* source = view.find(attacker);
                if (live_attacker == damage_units.end() || !source) continue;
                auto target = *source;
                target.durability = live_attacker->second.durability ? &*live_attacker->second.durability : nullptr;
                // Category restrictions and priority use the team company's type, not its
                // leader's type. Members supply aiming/firing only (WTA-29/WCC-25).
                if (!target.profile) continue;
                auto allowance = divert;
                auto gate = formation;
                if (mind != minds.end() && gate.present) {
                    const auto& team = mind->second;
                    const auto& held = *team.formation;
                    const auto* profile = impl_->motion.squadrons.find_squadron(team.squadron_type);
                    // This accepted hit establishes the unit's threat from this attacker.
                    // An escort hit this frame supplies the corresponding escort threat.
                    if (profile)
                        allowance = math::Fixed::from_raw(divert.raw() + profile->autonomous_move_extension_vs_attacker.raw());
                    if (!gate.unbounded && !held.complete
                        && !within_range(receiver->second.state.position, anchor, allowance, RangeMetric::planar))
                        gate.allows_divert = false;
                    if (held.base_target == attacker || team.target == attacker) gate.allows_divert = false;
                    if (team.mode == SquadronMode::escort) {
                        const auto escort = damage_units.find(held.base_target);
                        const bool escort_threat = threats.contains({held.base_target, attacker});
                        if (escort != damage_units.end() && escort->second.motion && escort->second.motion->kind != MotionKind::none
                            && !escort_threat) gate.allows_divert = false;
                    }
                }
                unit.combat = &state;
                auto next = detail::retaliate_combat(view, unit, target, allowance, anchor, &gate);
                if (!next) { job.error = next.error(); break; }
                state = std::move(next).value();
            }
            job.result = std::move(state);
        }
    });
    if (!prepared) return prepared;
    for (auto& job : jobs) {
        if (job.error) return core::Result<void>::failure(*job.error);
        if (!job.result) continue;
        const auto mind = minds.find(job.recipient);
        if (mind == minds.end()) {
            staged.at(job.firing).combat = std::move(job.result);
            continue;
        }
        auto& team = minds.at(job.recipient);
        if (team.target == job.result->attack_target) continue;
        const auto previous = team.target;
        if (previous != invalid_entity_id) end_combat(job.recipient, team, craft_view(job.firing));
        team.target = job.result->attack_target;
        team.target_hardpoint = no_hardpoint;
        team.approach = true;
        if (team.formation) {
            if (team.formation->complete && team.formation->base_target == invalid_entity_id)
                team.formation->base_position = staged.at(job.recipient).state.position;
            team.formation->attack_override = true;
            team.formation->complete = false;
        }
        // EUS-23: target assignment is non-direct. Existing fighter movement owns any
        // later approach; damage notification itself neither turns nor plans movement.
        for (const auto member : team.roster) {
            const auto live = staged.find(member);
            if (live == staged.end() || !live->second.combat) continue;
            auto& combat = *staged.at(member).combat;
            combat.attack_target = team.target;
            combat.attack_hardpoint = no_hardpoint;
            combat.direct = false;
        }
    }
    return core::Result<void>::success();
}

std::vector<EntityId> session_detail::Tick::redirect_recipients(const EntityId target) const {
    const auto* found = redirect_unit(target);
    const auto* profile = found ? impl_->combat.find(found->state.type_id) : nullptr;
    if (!profile || !profile->redirect_damage_to_teammates) return {};
    const auto& parents = craft_prep_->craft_squadron.value();
    const auto parent = parents.find(target);
    if (parent == parents.end()) return {};
    const auto& minds = craft_prep_->minds.value();
    const auto mind = minds.find(parent->second);
    if (mind == minds.end() || mind->second.roster.size() <= 1) return {};
    // WHE-64 counts the parent's current members. The stored roster keeps destroyed escort IDs until the next
    // replenishment, so only members alive this tick share the hit; a leader without a living escort takes its
    // own damage (retail observation, legacy #942).
    std::vector<EntityId> live;
    live.reserve(mind->second.roster.size());
    for (const auto id : mind->second.roster)
        if (id == target || redirect_unit(id) != nullptr) live.push_back(id);
    if (live.size() <= 1) return {};
    return live;
}

core::Result<void> session_detail::Tick::impacts() {
    impl_->impact_bonus_bases.clear();
    impl_->impact_bonus_containers.clear();
    impl_->impact_bonus_containers_ready = false;
    const auto& arrivals = staging_.value().arrivals.value();
    const auto& unloaded = staging_.value().unloaded.value();
    const auto& craft_defense = moved_.value().craft_defense.value();
    auto& combat_events = targets_.value().combat_events.value();
    const auto& damage_rules = flights_.value().damage_rules.value();
    auto& flights = flights_.value().flights.value();
    const auto& blasts = flights_.value().blasts.value();
    auto& blasts_without_source = flights_.value().blasts_without_source.value();
    auto& launched = flights_.value().launched.value();
    auto& staged = tracked_.value().staged.value();
    impacts_.emplace();
    auto& events = impacts_.value().events.emplace(std::move(manual_feedback_));
    // Ordered commit of the environmental workers' signals; crafts address their container.
    for (const auto& signals : impl_->nebula_scratch.events) {
        events.insert(events.end(), signals.begin(), signals.end());
    }
    std::sort(events.begin(), events.end(), [](const Event& left, const Event& right) {
        return std::tie(left.unit, left.sequence, left.kind) < std::tie(right.unit, right.sequence, right.kind);
    });
    events.erase(std::unique(events.begin(), events.end()), events.end());
    // WR-40: arrival completion restores the exit pose in the partitioned movement phase.
    // Visibility is reevaluated below from that pose every tick, including this completion tick.
    for (const auto id : unloaded) {
        if (const auto found = staged.find(id); found != staged.end()) {
            events.push_back(Event{tick, EventKind::reinforcement_unloaded, found->second.state.owner, 0, id});
        }
    }
    // #447: every unit killed this tick as it stood when it died, in destruction order.
    auto& killed = impacts_.value().killed.emplace();
    auto& pending_blast_damage = impacts_.value().pending_blast_damage.emplace(impl_->pending_blast_damage);
    const auto deliver_redirected = [&](const EntityId id, Hit hit, const PlayerId killer, const EntityId attacker) -> core::Result<void> {
        if (damage_blocked()) return core::Result<void>::success();
        const auto recipient = staged.find(id);
        if (recipient == staged.end() || !recipient->second.durability) return core::Result<void>::success();
        const auto arriving = arrivals.find(id);
        if (arriving != arrivals.end() && arriving->second.frame < arrival_visible_frame) return core::Result<void>::success();
        auto& unit = recipient->second;
        const auto defense = craft_defense.find(id);
        hit.defense = math::Fixed::from_raw((defense == craft_defense.end() ? 0 : defense->second.raw())
            + (unit.arrival_vulnerable_until ? impl_->economy.vulnerability.raw() : 0)
            + impl_->concentrate_defense(unit, staged, staging_->ledgers.value()).raw());
        hit.take_damage_multiplier = unit.take_damage_mode;
        const auto hull = unit.durability->hull, shields = unit.durability->shields;
        auto taken = apply_hit(*impl_->health_profile(unit), *damage_rules, *unit.durability, hit, tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(unit, hull, shields);
        if (!taken.value().cancelled) notify_retaliation(unit, attacker, hit.amount);
        impl_->end_depleted_defend(unit, tick, taken.value().storm_shield_branch);
        if (taken.value().damage.destroyed_hardpoint) events.push_back(destruction_event(tick,
            EventKind::hardpoint_destroyed, unit.state, *taken.value().damage.destroyed_hardpoint));
        if (taken.value().damage.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit.state, 0, killer));
            killed.push_back(unit.state); impl_->adjust_owned(unit.state, false); staged.erase(recipient);
        }
        return core::Result<void>::success();
    };
    // WAD-21/25: each prepared share enters ordinary damage in authored order, so state
    // changes (shield-generator loss, last-hit frame) are visible to later deliveries.
    const auto deliver_area = [&](const Projectile& projectile, const detail::BlastRecipient& recipient) -> core::Result<void> {
        if (damage_blocked()) return core::Result<void>::success();
        const auto target = staged.find(recipient.id);
        if (target == staged.end() || !target->second.durability) return core::Result<void>::success();
        const auto arriving = arrivals.find(recipient.id);
        if (arriving != arrivals.end() && arriving->second.frame < arrival_visible_frame) return core::Result<void>::success();
        const auto* profile = impl_->health_profile(target->second);
        const auto defense = craft_defense.find(recipient.id);
        const auto modifier = math::Fixed::from_raw((defense == craft_defense.end() ? 0 : defense->second.raw())
            + (target->second.arrival_vulnerable_until ? impl_->economy.vulnerability.raw() : 0)
            + impl_->concentrate_defense(target->second, staged, staging_->ledgers.value()).raw());
        // WPR-51/WCC-44, EUS-16: immediate delivery resolves the retained shooter now.
        const auto source = staged.find(projectile.shooter);
        const auto factor = math::Fixed::from_raw(math::Fixed::scale
            + (source == staged.end() ? 0 : source->second.upgrade_bonuses[1].raw()));
        auto hit = detail::area_hit(projectile, recipient, modifier, factor);
        if (!hit) return core::Result<void>::failure(hit.error());
        hit.value().take_damage_multiplier = target->second.take_damage_mode;
        // WHE-51: modes are current at delivery; a removed shooter contributes identity.
        if (source != staged.end()) hit.value().cause_damage_multiplier = source->second.cause_damage_mode;
        auto redirected = redirect_damage(recipient.id, hit.value(), [&](const EntityId id, Hit share) {
            return deliver_redirected(id, share, projectile.owner, projectile.shooter);
        });
        if (!redirected) return core::Result<void>::failure(redirected.error());
        if (redirected.value()) return core::Result<void>::success();
        const auto hull = target->second.durability->hull;
        const auto shields = target->second.durability->shields;
        auto taken = apply_hit(*profile, *damage_rules, *target->second.durability, hit.value(), tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(target->second, hull, shields);
        if (!taken.value().cancelled) notify_retaliation(target->second, projectile.shooter, hit.value().amount);
        impl_->end_depleted_defend(target->second, tick, taken.value().storm_shield_branch);
        const auto& damage = taken.value().damage;
        if (damage.destroyed_hardpoint) events.push_back(destruction_event(tick,
            EventKind::hardpoint_destroyed, target->second.state, *damage.destroyed_hardpoint));
        if (damage.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, target->second.state, 0, projectile.owner));
            killed.push_back(target->second.state);
            impl_->adjust_owned(target->second.state, false);
            staged.erase(target);
        }
        return core::Result<void>::success();
    };
    // WAD-26/WFO-14: deliver stored metadata independently of projectile/shooter lifetime.
    // The phase service retains the project ordering in due-frame/creation order.
    std::stable_sort(pending_blast_damage.begin(), pending_blast_damage.end(),
        [](const auto& left, const auto& right) { return left.due < right.due; });
    auto blast_due_end = pending_blast_damage.begin();
    while (blast_due_end != pending_blast_damage.end() && blast_due_end->due <= tick) {
        const auto target = staged.find(blast_due_end->recipient);
        const auto* profile = target == staged.end() ? nullptr : impl_->combat.find(target->second.state.type_id);
        const auto hit = detail::delayed_damage_hit(blast_due_end->amount, blast_due_end->damage_type,
            blast_due_end->internal_damage_misc, blast_due_end->contacted_mesh, profile);
        auto redirected = redirect_damage(blast_due_end->recipient, hit, [&](const EntityId id, const Hit share) {
            return deliver_redirected(id, share, blast_due_end->owner, invalid_entity_id);
        });
        if (!redirected) return core::Result<void>::failure(redirected.error());
        if (!redirected.value()) {
            auto delivered = deliver_redirected(blast_due_end->recipient, hit, blast_due_end->owner, invalid_entity_id);
            if (!delivered) return core::Result<void>::failure(delivered.error());
        }
        ++blast_due_end;
    }
    pending_blast_damage.erase(pending_blast_damage.begin(), blast_due_end);
    const auto queue_damage = [&](const Projectile& source, const detail::BlastRecipient& recipient) {
        if (recipient.amount.raw() == 0) return;
        const auto frames = detail::delayed_damage_frames(recipient.delay);
        std::string contacted_mesh;
        const auto target = staged.find(recipient.id);
        const auto* profile = target == staged.end() ? nullptr : impl_->combat.find(target->second.state.type_id);
        if (profile && recipient.hardpoint < profile->hardpoint_meshes.size())
            contacted_mesh = profile->hardpoint_meshes[recipient.hardpoint];
        pending_blast_damage.push_back({tick + frames, source.owner, source.damage_type,
            source.internal_damage_misc, recipient.id, recipient.amount, std::move(contacted_mesh)});
    };
    const auto deliver_blast = [&](const std::size_t index) -> core::Result<void> {
        if (index >= blasts.size() || !blasts[index]) return core::Result<void>::success();
        const auto player_count = std::max(blasts[index]->players.size(), blasts_without_source[index]
            ? blasts_without_source[index]->players.size() : std::size_t{});
        for (std::size_t player = 0; player < player_count; ++player) {
            // WAD-10: source lookup belongs to each player query, including after this blast
            // kills its own shooter in an earlier immunity-enabled player group.
            const auto& prepared = blasts_without_source[index] && staged.find(flights[index]->projectile.shooter) == staged.end()
                ? *blasts_without_source[index] : *blasts[index];
            if (player >= prepared.players.size()) continue;
            const auto& group = prepared.players[player];
            for (const auto& recipient : group.recipients) {
                if (recipient.delay.raw() <= 0) {
                    auto delivered = deliver_area(flights[index]->projectile, recipient);
                    if (!delivered) return delivered;
                } else {
                    queue_damage(flights[index]->projectile, recipient);
                }
            }
            if (group.capped) return core::Result<void>::success();
        }
        return core::Result<void>::success();
    };
    auto& projectiles = impacts_.value().projectiles.emplace();
    projectiles.reserve(flights.size() + launched.size());
    for (std::size_t flight_index = 0; flight_index < flights.size(); ++flight_index) {
        auto& flight = flights[flight_index];
        const auto moving_spawn = std::lower_bound(impl_->ability_spawns.begin(), impl_->ability_spawns.end(), flight->projectile.id,
            [](const auto& entry, const std::uint64_t id) { return entry.id < id; });
        if (moving_spawn != impl_->ability_spawns.end() && moving_spawn->id == flight->projectile.id)
            moving_spawn->position = flight->projectile.position;
        bool ability_spawn = false;
        if (flight->expired || flight->hit) {
            const auto spawn = std::lower_bound(impl_->ability_spawns.begin(), impl_->ability_spawns.end(), flight->projectile.id,
                [](const auto& entry, const std::uint64_t id) { return entry.id < id; });
            if (spawn != impl_->ability_spawns.end() && spawn->id == flight->projectile.id) {
                ability_spawn = spawn->kind != AbilityKind::cluster_bomb && spawn->kind != AbilityKind::fow_reveal_ping;
                spawn->detonated = true;
                spawn->position = flight->projectile.position;
                const auto* profile = impl_->spawn_profile(spawn->type);
                if (profile && profile->weaken.on_detonation) for (const auto id : flights_->weaken_recipients[flight_index]) {
                    if (const auto recipient = staged.find(id); recipient != staged.end() && recipient->second.combat)
                        spawn->recipients.push_back({id, tick + profile->weaken.duration_frames});
                }
            }
        }
        if (!flight->hit) {
            if (!flight->expired) {
                projectiles.push_back(flight->projectile);
            }
            if (flight->expired) {
                const auto& projectile = flight->projectile;
                // WAD-04/07: a miss expires once at its terminal pose, independently of
                // blast damage and particle availability. Generic removal emits nothing.
                // WHE-62: stationary ability spawns already publish their countdown
                // detonation metadata; do not present that terminal twice.
                if (!ability_spawn) combat_events.push_back(CombatEvent{tick, CombatEventKind::projectile_expired,
                    projectile.shooter, projectile.weapon, projectile.id, no_hardpoint,
                    flight->from, projectile.position, static_cast<std::uint32_t>(flight->expiry_reason),
                    projectile.target});
                auto delivered = deliver_blast(flight_index);
                if (!delivered) return core::Result<void>::failure(delivered.error());
            }
            continue;
        }
        // WCC-40: a pending result spends the projectile without delivering a hit.
        if (damage_blocked()) continue;
        const auto target = staged.find(*flight->hit);
        if (target == staged.end() || !target->second.durability) {
            auto delivered = deliver_blast(flight_index);
            if (!delivered) return core::Result<void>::failure(delivered.error());
            continue;
        }
        // #530 PU-37: a hidden arriving unit takes no damage; the projectile is spent. PU-38: a
        // visible one takes the elevated vulnerability for the rules' duration.
        const auto arriving = arrivals.find(*flight->hit);
        if (arriving != arrivals.end() && arriving->second.frame < arrival_visible_frame) {
            continue;
        }
        const auto arrival_defense = target->second.arrival_vulnerable_until ? impl_->economy.vulnerability : math::Fixed{};
        const auto& projectile = flight->projectile;
        const auto& profile = *impl_->health_profile(target->second);
        // DG-11: with meshes, the hardpoint whose collision mesh the projectile met takes it (#536),
        // or for a shot aimed at a hardpoint of the unit it reached, the hardpoint its aim names (DG-39);
        // with the box alone, the hardpoint the shot aimed at when it reached that unit.
        auto hardpoint = hull_target;
        if (flight->meshed) {
            if (flight->mesh_hardpoint != no_hardpoint && damage_target_valid(profile, flight->mesh_hardpoint)) {
                hardpoint = flight->mesh_hardpoint;
            }
        } else if (*flight->hit == projectile.target && projectile.target_hardpoint != no_hardpoint
            && damage_target_valid(profile, projectile.target_hardpoint)) {
            hardpoint = projectile.target_hardpoint;
        }
        const auto defense = craft_defense.find(*flight->hit);
        auto modifier = defense != craft_defense.end() ? defense->second : math::Fixed{};
        // #530 PU-38: an arriving unit's elevated vulnerability adds to its defense modifier.
        modifier = math::Fixed::from_raw(modifier.raw() + arrival_defense.raw()
            + impl_->concentrate_defense(target->second, staged, staging_->ledgers.value()).raw());
        auto primary_amount = detail::primary_damage(projectile);
        if (projectile.damage_delay.raw() > 0) {
            CombatRandom random(impl_->setup.seed, tick, projectile.id, projectile_damage_delay_slot);
            const auto delay = detail::projectile_delivery_delay(projectile, {}, random);
            if (!delay) return core::Result<void>::failure(delay.error());
            queue_damage(projectile, {*flight->hit, primary_amount, delay.value(), hardpoint});
            combat_events.push_back(CombatEvent{tick, CombatEventKind::projectile_hit, projectile.shooter,
                projectile.weapon, *flight->hit, hardpoint == hull_target ? no_hardpoint : hardpoint,
                flight->from, flight->contact, 0, projectile.target});
            auto delivered = deliver_blast(flight_index);
            if (!delivered) return core::Result<void>::failure(delivered.error());
            continue;
        }
        const auto source = staged.find(projectile.shooter);
        if (source != staged.end() && source->second.upgrade_bonuses[1].raw() != 0) {
            // EUS-16: instance damage stays unmodified in flight, including explicit launches.
            auto caused = math::multiply(primary_amount, math::Fixed::from_raw(math::Fixed::scale
                + source->second.upgrade_bonuses[1].raw()));
            if (!caused) return core::Result<void>::failure(caused.error());
            primary_amount = caused.value();
        }
        Hit hit{primary_amount, projectile.damage_type, true, projectile.shield_damage,
            projectile.hitpoint_damage, hardpoint, projectile.allow_diminishing_firepower,
            projectile.internal_damage_misc, modifier, projectile.energy_damage};
        hit.take_damage_multiplier = target->second.take_damage_mode;
        if (source != staged.end()) hit.cause_damage_multiplier = source->second.cause_damage_mode;
        // WRB-06..WRB-10: after the combat damage modifier and before shields and hull, a Redirect_Blaster handler
        // of the target may deflect the shot: no direct damage, and a fresh projectile of the target's owner.
        if (source != staged.end() && !impl_->abilities.profiles.empty()) {
            const auto handlers = impl_->redirect_blaster_handlers(target->second, source->second, staging_->ledgers.value());
            bool deflected = false;
            CombatRandom random(impl_->setup.seed, tick, projectile.id, redirect_blaster_slot);
            CombatRandom direction_random(impl_->setup.seed, tick, projectile.id, redirect_blaster_direction_slot);
            for (const auto* handler : handlers) {
                const auto* shooter_profile = impl_->combat.find(source->second.state.type_id);
                if (!projectile.profile_key) continue;
                const auto& key = *projectile.profile_key;
                const auto* launch_profile = impl_->combat.find(key.type);
                if (!launch_profile) continue;
                const ShotProfile* shot = nullptr;
                if (key.kind == ProjectileProfileKind::death) {
                    const auto index = key.weapon & ~death_projectile_slot_flag;
                    if (index < launch_profile->death_projectiles.size()) shot = &launch_profile->death_projectiles[index];
                } else {
                    const auto weapon = std::find_if(launch_profile->weapons.begin(), launch_profile->weapons.end(),
                        [&](const WeaponProfile& entry) { return entry.hardpoint == key.weapon; });
                    if (weapon == launch_profile->weapons.end()) continue;
                    const auto& selected = key.kind == ProjectileProfileKind::ability ? weapon->ability_shot
                        : key.kind == ProjectileProfileKind::barrage ? weapon->barrage_shot
                        : key.kind == ProjectileProfileKind::charged_blast && weapon->charged_shot ? weapon->charged_shot : weapon->shot;
                    if (selected) shot = &*selected;
                }
                if (!shot) continue;
                const auto defender_transform = math::to_matrix(target->second.state.rotation, target->second.state.position);
                const auto attacker_transform = math::to_matrix(source->second.state.rotation, source->second.state.position);
                if (!defender_transform || !attacker_transform) return core::Result<void>::failure(
                    !defender_transform ? defender_transform.error() : attacker_transform.error());
                detail::CombatUnit defender;
                defender.id = target->second.state.entity_id; defender.type_id = target->second.state.type_id;
                defender.owner = target->second.state.owner; defender.position = target->second.state.position;
                defender.previous_position = defender.position; defender.transform = defender_transform.value();
                defender.profile = impl_->combat.find(defender.type_id);
                detail::CombatUnit attacker;
                attacker.id = source->second.state.entity_id; attacker.type_id = source->second.state.type_id;
                attacker.owner = source->second.state.owner; attacker.position = source->second.state.position;
                attacker.previous_position = attacker.position; attacker.transform = attacker_transform.value();
                attacker.profile = shooter_profile;
                auto at_contact = projectile;
                at_contact.position = flight->contact;
                const bool defender_moving = target->second.motion && target->second.motion->kind == MotionKind::path;
                auto reaction = detail::redirect_blaster_reaction(at_contact, *shot, *handler, defender, attacker,
                    flights_.value().next_projectile.value(), random, direction_random, defender_moving);
                if (!reaction) return core::Result<void>::failure(reaction.error());
                if (reaction.value().created) {
                    ++flights_.value().next_projectile.value();
                    launched.push_back(std::move(*reaction.value().created));
                }
                if (!reaction.value().blocked) continue;
                deflected = true;
                break;
            }
            if (deflected) {
                combat_events.push_back(CombatEvent{tick, CombatEventKind::projectile_hit, projectile.shooter,
                    projectile.weapon, *flight->hit, hardpoint == hull_target ? no_hardpoint : hardpoint,
                    flight->from, flight->contact, hit_outcome_redirected, projectile.target});
                auto delivered = deliver_blast(flight_index);
                if (!delivered) return core::Result<void>::failure(delivered.error());
                continue;
            }
        }
        auto redirected = redirect_damage(*flight->hit, hit, [&](const EntityId id, Hit share) {
            return deliver_redirected(id, share, projectile.owner, projectile.shooter);
        });
        if (!redirected) return core::Result<void>::failure(redirected.error());
        if (redirected.value()) {
            combat_events.push_back(CombatEvent{tick, CombatEventKind::projectile_hit, projectile.shooter,
                projectile.weapon, *flight->hit, hardpoint == hull_target ? no_hardpoint : hardpoint,
                flight->from, flight->contact, 0, projectile.target});
            auto delivered = deliver_blast(flight_index);
            if (!delivered) return core::Result<void>::failure(delivered.error());
            continue;
        }
        const auto hull_before = target->second.durability->hull;
        const auto shields_before = target->second.durability->shields;
        auto outcome = apply_hit(profile, *damage_rules, *target->second.durability, hit, tick);
        if (!outcome) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                "tick " + std::to_string(tick) + " projectile " + std::to_string(projectile.id) + ": "
                    + outcome.error().message));
        }
        impl_->track_damage(target->second, hull_before, shields_before);
        if (!outcome.value().cancelled) notify_retaliation(target->second, projectile.shooter, hit.amount);
        impl_->end_depleted_defend(target->second, tick, outcome.value().storm_shield_branch);
        // EN-08: only a positive energy drain disables engines; no locomotor means no effect.
        if (projectile.disable_engines_frames && outcome.value().drained.raw() > 0
            && profile.max_speed && !outcome.value().damage.unit_destroyed) {
            const bool was_online = engines_online(profile, *target->second.durability);
            disable_engines(*target->second.durability, *projectile.disable_engines_frames, tick);
            // EN-09: discard a pending group/search speed cap, retaining its destination.
            // FD-64/UFS-33: a failed search has no path to replan; keep its strict cooldown.
            const bool waited = target->second.formation && !target->second.formation->failure_frame;
            if (waited && target->second.motion) {
                target->second.motion->target = target->second.formation->destination;
                target->second.motion->kind = MotionKind::path;
                target->second.formation.reset();
            }
            if ((was_online || waited) && target->second.motion && target->second.motion->kind == MotionKind::path) {
                tracked_.value().replans.value().push_back(target->first);
            }
        }
        // IS-01, IS-02: an ion shot stuns a unit whose type has the ion-stun behaviour.
        if (projectile.ion_stun && profile.ion_stun_effect && !outcome.value().damage.unit_destroyed) {
            impl_->ion_stun_unit(target->second, *projectile.ion_stun, tick);
        }
        // How the hit was taken, for the impact effect only (#80, docs/behaviour/battle-presentation.md).
        std::uint32_t taken = 0;
        if (outcome.value().storm_shield_branch) taken |= hit_outcome_storm_shield;
        if (outcome.value().shield_absorbed) {
            taken |= hit_outcome_shield_absorbed;
        } else if (outcome.value().armor_multiplier <= armor_reduced_limit) {
            taken |= hit_outcome_armor_reduced;
        }
        combat_events.push_back(CombatEvent{tick, CombatEventKind::projectile_hit, projectile.shooter,
            projectile.weapon, *flight->hit, hardpoint == hull_target ? no_hardpoint : hardpoint, flight->from,
            flight->contact, taken, projectile.target});
        const auto& result = outcome.value().damage;
        if (result.destroyed_hardpoint) {
            events.push_back(destruction_event(
                tick, EventKind::hardpoint_destroyed, target->second.state, *result.destroyed_hardpoint));
        }
        if (result.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, target->second.state, 0, projectile.owner));
            killed.push_back(target->second.state);
            impl_->adjust_owned(target->second.state, false);
            staged.erase(target);
        }
        // WAD-03: ordinary direct routing and its single impact event precede secondary damage.
        // Absorption does not suppress the blast; the take-damage owner supplies cancellation.
        if (detail::blast_after_hit(projectile, outcome.value())) {
            auto delivered = deliver_blast(flight_index);
            if (!delivered) return core::Result<void>::failure(delivered.error());
        }
    }
    projectiles.insert(projectiles.end(), launched.begin(), launched.end());
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::late_impacts() {
    const auto& arrivals = staging_->arrivals.value();
    const auto& craft_defense = moved_->craft_defense.value();
    const auto* damage_rules = flights_->damage_rules.value();
    auto& staged = tracked_->staged.value();
    auto& events = impacts_->events.value();
    auto& killed = impacts_->killed.value();
    auto& projectiles = impacts_->projectiles.value();
    auto& next_projectile = flights_->next_projectile.value();
    const auto deliver_redirected = [&](const EntityId id, Hit hit, const PlayerId killer, const EntityId attacker) -> core::Result<void> {
        if (damage_blocked()) return core::Result<void>::success();
        const auto recipient = staged.find(id);
        if (recipient == staged.end() || !recipient->second.durability) return core::Result<void>::success();
        const auto arriving = arrivals.find(id);
        if (arriving != arrivals.end() && arriving->second.frame < arrival_visible_frame) return core::Result<void>::success();
        auto& unit = recipient->second;
        const auto defense = craft_defense.find(id);
        hit.defense = math::Fixed::from_raw((defense == craft_defense.end() ? 0 : defense->second.raw())
            + (unit.arrival_vulnerable_until ? impl_->economy.vulnerability.raw() : 0)
            + impl_->concentrate_defense(unit, staged, staging_->ledgers.value()).raw());
        hit.take_damage_multiplier = unit.take_damage_mode;
        const auto hull = unit.durability->hull, shields = unit.durability->shields;
        auto taken = apply_hit(*impl_->health_profile(unit), *damage_rules, *unit.durability, hit, tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(unit, hull, shields);
        if (!taken.value().cancelled) notify_retaliation(unit, attacker, hit.amount);
        impl_->end_depleted_defend(unit, tick, taken.value().storm_shield_branch);
        if (taken.value().damage.destroyed_hardpoint) events.push_back(destruction_event(tick,
            EventKind::hardpoint_destroyed, unit.state, *taken.value().damage.destroyed_hardpoint));
        if (taken.value().damage.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit.state, 0, killer));
            killed.push_back(unit.state); impl_->adjust_owned(unit.state, false); staged.erase(recipient);
        }
        return core::Result<void>::success();
    };
    // WHE-26: immutable beam checks in workers, then sparse direct-damage delivery in source order.
    if (std::any_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(), [](const auto& profile) {
        return ability_slot(profile, AbilityKind::energy_weapon).has_value();
    })) {
    struct BeamService { EntityId source{}, target{}; bool release{}, due{}; };
    std::vector<const LiveUnit*> beam_inputs;
    for (const auto& entry : staged) beam_inputs.push_back(&entry.second);
    std::vector<BeamService> beam_services(beam_inputs.size());
    const auto prepared_beams = executor.execute_phase("energy-beam-damage", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, beam_inputs.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& source = *beam_inputs[i];
            const auto* nested = impl_->beam_profile(source, AbilityKind::energy_weapon);
            if (!nested || !source.abilities) continue;
            const auto& profile = *impl_->abilities.find(source.state.type_id);
            const auto& state = source.abilities->slots[*ability_slot(profile, AbilityKind::energy_weapon)];
            if (!state.active) continue;
            const auto& special = source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
            auto& service = beam_services[i];
            service.source = source.state.entity_id;
            service.target = state.target;
            const auto target = staged.find(state.target);
            service.release = target == staged.end() || !impl_->beam_target_valid(source, target->second, *nested)
                || !impl_->beam_in_range(source, target->second, *nested)
                || (state.target_hardpoint != no_hardpoint && !impl_->target_hardpoint_standing(target->second, state.target_hardpoint));
            service.due = !service.release && tick >= special.next_service_frame;
        }
    });
    if (!prepared_beams) return prepared_beams;
    for (const auto& service : beam_services) {
        if (service.source == invalid_entity_id) continue;
        const auto source = staged.find(service.source);
        if (source == staged.end()) continue;
        const auto target = staged.find(service.target);
        const auto& source_profile = *impl_->abilities.find(source->second.state.type_id);
        const auto& source_slot = source->second.abilities->slots[*ability_slot(source_profile, AbilityKind::energy_weapon)];
        if (service.release || target == staged.end()
            || (source_slot.target_hardpoint != no_hardpoint
                && !impl_->target_hardpoint_standing(target->second, source_slot.target_hardpoint))) {
            impl_->release_beam(source->second, AbilityKind::energy_weapon, tick);
            continue;
        }
        if (!service.due || !damage_rules || !target->second.durability || damage_blocked()) continue;
        const auto* nested = impl_->beam_profile(source->second, AbilityKind::energy_weapon);
        const auto& profile = *impl_->abilities.find(source->second.state.type_id);
        const auto& state = source->second.abilities->slots[*ability_slot(profile, AbilityKind::energy_weapon)];
        auto& special = source->second.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
        special.next_service_frame = tick + nested->service_interval;
        const auto* combat = impl_->combat.find(target->second.state.type_id);
        const auto hardpoint = state.target_hardpoint == no_hardpoint ? hull_target
            : combat && state.target_hardpoint < combat->aimed_routes.size()
                ? combat->aimed_routes[state.target_hardpoint] : state.target_hardpoint;
        const auto defense = craft_defense.find(service.target);
        const auto modifier = math::Fixed::from_raw(impl_->concentrate_defense(target->second, staged, staging_->ledgers.value()).raw()
            + (defense == craft_defense.end() ? 0 : defense->second.raw())
            + (target->second.arrival_vulnerable_until ? impl_->economy.vulnerability.raw() : 0));
        Hit hit{nested->damage_per_frame, impl_->abilities.beam_damage_type, false, true, true, hardpoint, false, true, modifier};
        hit.cause_damage_multiplier = source->second.cause_damage_mode;
        hit.take_damage_multiplier = target->second.take_damage_mode;
        auto redirected = redirect_damage(service.target, hit, [&](const EntityId id, Hit share) {
            return deliver_redirected(id, share, source->second.state.owner, service.source);
        });
        if (!redirected) return core::Result<void>::failure(redirected.error());
        if (redirected.value()) continue;
        const auto hull_before = target->second.durability->hull;
        const auto shields_before = target->second.durability->shields;
        const auto taken = apply_hit(*impl_->health_profile(target->second), *damage_rules, *target->second.durability, hit, tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(target->second, hull_before, shields_before);
        if (!taken.value().cancelled) notify_retaliation(target->second, service.source, hit.amount);
        impl_->end_depleted_defend(target->second, tick, taken.value().storm_shield_branch);
        if (taken.value().damage.destroyed_hardpoint)
            events.push_back(destruction_event(tick, EventKind::hardpoint_destroyed, target->second.state, *taken.value().damage.destroyed_hardpoint));
        if (taken.value().damage.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, target->second.state, 0, source->second.state.owner));
            killed.push_back(target->second.state);
            impl_->adjust_owned(target->second.state, false);
            staged.erase(target);
            impl_->release_beam(source->second, AbilityKind::energy_weapon, tick);
        }
    }
    }
    // WLS-02: immutable per-owner checks, followed by shield-only drain in source order.
    if (std::any_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(), [](const auto& profile) {
        return ability_slot(profile, AbilityKind::leech_shields).has_value();
    })) {
        struct LeechService { EntityId source{}, target{}; bool release{}, due{}; };
        std::vector<const LiveUnit*> inputs;
        for (const auto& entry : staged) inputs.push_back(&entry.second);
        std::vector<LeechService> services(inputs.size());
        const auto prepared = executor.execute_phase("shield-leech", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, inputs.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& source = *inputs[i];
                const auto* nested = impl_->beam_profile(source, AbilityKind::leech_shields);
                if (!nested || !source.abilities) continue;
                const auto& profile = *impl_->abilities.find(source.state.type_id);
                const auto slot = *ability_slot(profile, AbilityKind::leech_shields);
                const auto& state = source.abilities->slots[slot];
                if (!state.active) continue;
                const auto& special = source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())];
                const auto target = staged.find(state.target);
                services[i] = {source.state.entity_id, state.target,
                    !ability_ready(profile.abilities[slot], state, impl_->ability_gate(source, tick), tick)
                        || target == staged.end() || !impl_->beam_target_valid(source, target->second, *nested, false)
                        || !impl_->beam_in_range(source, target->second, *nested), tick >= special.next_service_frame};
            }
        });
        if (!prepared) return prepared;
        const auto release_leech = [&](LiveUnit& source) -> core::Result<void> {
            impl_->release_beam(source, AbilityKind::leech_shields, tick);
            if (!impl_->impact_bonus_containers_ready) {
                for (const auto& account : staging_->ledgers.value()) for (const auto& held : account.completed)
                    if (const auto found = staged.find(held.station); found != staged.end())
                        impl_->impact_bonus_containers.emplace(held.station, found->second.state.type_id);
                impl_->impact_bonus_containers_ready = true;
            }
            std::vector<CombatBonuses> categories(impl_->bonus_categories.size());
            return impl_->apply_bonuses(source, impl_->command_bonuses_for(source, impl_->command_ledger,
                staging_->ledgers.value(), impl_->impact_bonus_containers, categories, &staged), TacticalSession::Impl::BonusAdjustment::loss);
        };
        for (const auto& service : services) {
            if (service.source == invalid_entity_id) continue;
            auto& source = staged.at(service.source);
            const auto target = staged.find(service.target);
            const auto* nested = impl_->beam_profile(source, AbilityKind::leech_shields);
            if (service.release || target == staged.end() || !impl_->beam_target_valid(source, target->second, *nested, false)) {
                if (auto released = release_leech(source); !released) return released;
                continue;
            }
            if (!service.due || !damage_rules) continue;
            auto& state = *target->second.durability;
            drain_shields(*impl_->health_profile(target->second), *damage_rules, state, nested->damage_per_frame, tick);
            impl_->end_depleted_defend(target->second, tick, in_ion_storm(*damage_rules, state, tick));
            const auto& profile = *impl_->abilities.find(source.state.type_id);
            source.abilities->special.slots[static_cast<std::size_t>(nested - profile.special.data())].next_service_frame = tick + 1;
            if (state.shields.raw() <= 0) {
                if (auto released = release_leech(source); !released) return released;
            }
        }
    }
    // WAB-72..74: buzz droids after the beams. Each droid's victim scan reads the staged
    // world in a worker; delivery and movement commit in ascending droid ID.
    if (!impl_->buzz_droids.empty()) {
        if (auto serviced = service_buzz_droids(staged, events, killed, damage_rules); !serviced) return serviced;
    }
    // WSD-04..07: the countdowns that ran out this tick end their units after the projectiles.
    if (!tracked_.value().self_destructs.value().empty()) {
        if (auto serviced = service_self_destruct(staged, events, killed, projectiles, next_projectile); !serviced) return serviced;
    }

    return core::Result<void>::success();
}

// WSD-04..07: each unit whose SELF_DESTRUCT countdown ran out (listed ascending by the ability phase) creates
// its blast object at its own position and is killed. The blast is an ability-spawned projectile, so its area
// damage (WAD-05, enemies only: WAD-08) lands in the next projectile phase after its shooter has left the
// world (WAD-10 reads the unmodified range then). The kill credits the unit's own owner (WSD-06).
core::Result<void> session_detail::Tick::service_self_destruct(UnitStage& staged, std::vector<Event>& events,
    std::vector<UnitState>& killed, std::vector<Projectile>& projectiles, std::uint64_t& next_projectile) {
    for (const auto id : tracked_.value().self_destructs.value()) {
        const auto found = staged.find(id);
        if (found == staged.end() || !found->second.abilities || damage_blocked()) continue;
        auto& unit = found->second;
        const auto* profile = impl_->abilities.find(unit.state.type_id);
        const auto slot = profile != nullptr ? ability_slot(*profile, AbilityKind::self_destruct) : std::nullopt;
        if (!slot || !profile->abilities[*slot].spawned || !unit.abilities->slots[*slot].active) continue;
        const auto& spawn = *profile->abilities[*slot].spawned;
        Projectile blast;
        blast.id = next_projectile++;
        blast.shooter = id;
        blast.owner = unit.state.owner;
        blast.weapon = object_weapon;
        blast.target_hardpoint = no_hardpoint;
        blast.position = unit.state.position;
        blast.damage = spawn.damage;
        blast.damage_type = spawn.damage_type;
        blast.shield_damage = spawn.shield_damage;
        blast.hitpoint_damage = spawn.hitpoint_damage;
        blast.blast = spawn.blast;
        blast.source_damage_factor = math::Fixed::from_raw(math::Fixed::scale + unit.upgrade_bonuses[1].raw());
        impl_->ability_spawns.push_back({blast.id, spawn.type, AbilityKind::self_destruct, unit.state.owner, id,
            unit.state.position, unit.state.rotation, tick + 1, false, {}});
        projectiles.push_back(blast);
        events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit.state, 0, unit.state.owner));
        killed.push_back(unit.state);
        impl_->adjust_owned(unit.state, false);
        staged.erase(found);
    }
    return core::Result<void>::success();
}

// WAB-72..74: one service per live droid object from its arming frame on. The victim scan of
// each droid runs in a worker over the immutable staged world; the damage and the move commit
// serially in ascending droid ID, so 1/2/4/8 workers give the same frame.
core::Result<void> session_detail::Tick::service_buzz_droids(UnitStage& staged, std::vector<Event>& events,
    std::vector<UnitState>& killed, const DamageRules* damage_rules) {
    std::erase_if(impl_->buzz_droids, [&](const BuzzDroidState& droid) { return staged.find(droid.droid) == staged.end(); });
    struct DroidHit { EntityId target{}; std::uint32_t hardpoint{hull_target}; };
    struct DroidService { bool due{}; std::vector<DroidHit> hits; std::optional<math::Vec3> move; };
    std::vector<const LiveUnit*> candidates;
    for (const auto& entry : staged) candidates.push_back(&entry.second);
    std::vector<DroidService> services(impl_->buzz_droids.size());
    std::vector<std::optional<core::Diagnostic>> failures(tick_partition_count);
    const auto distance_between = [](const math::Vec3& a, const math::Vec3& b) -> core::Result<math::Fixed> {
        auto dx = math::subtract(b.x, a.x);
        auto dy = math::subtract(b.y, a.y);
        auto dz = math::subtract(b.z, a.z);
        if (!dx || !dy || !dz) return core::Result<math::Fixed>::failure((!dx ? dx : !dy ? dy : dz).error());
        return math::length(math::Vec3{dx.value(), dy.value(), dz.value()});
    };
    const auto scanned = executor.execute_phase("buzz-droid-victims", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, impl_->buzz_droids.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& state = impl_->buzz_droids[i];
            const auto* buzz = impl_->buzz_profile(state.source_type);
            if (buzz == nullptr || tick < state.active_from) continue;
            const auto& droid = staged.at(state.droid);
            auto& service = services[i];
            service.due = true;
            // WAB-73: enemy movers near the creation point, of the droid type's valid-target
            // categories, aimed at their standing hardpoint nearest the droid (else the hull).
            bool damaging = false;
            const LiveUnit* nearest = nullptr;
            math::Fixed nearest_distance{};
            for (const auto* candidate : candidates) {
                const auto& unit = *candidate;
                if (unit.state.entity_id == state.droid || !unit.durability || unit.durability->hull.raw() <= 0
                    || !players_hostile(impl_->snapshot_players, droid.state.owner, unit.state.owner)) continue;
                const auto* profile = impl_->combat.find(unit.state.type_id);
                const auto* footprint = impl_->motion.footprint(unit.state.type_id);
                // WAB-73: authored locomotor presence is independent of a usable motion profile.
                const bool locomotor = footprint ? footprint->locomotor
                    : impl_->motion.find(unit.state.type_id) != nullptr
                        || impl_->motion.squadrons.find_craft(unit.state.type_id) != nullptr;
                if (profile == nullptr || (profile->category_bits & buzz->victim_categories) == 0
                    || !locomotor
                    || !within_range(state.start, unit.state.position, buzz->chase_radius, RangeMetric::spatial)) continue;
                auto aim = unit.state.position;
                std::uint32_t hardpoint = hull_target;
                if (!profile->hardpoints.empty()) {
                    auto transform = unit_transform(unit);
                    if (!transform) { failures[partition] = transform.error(); return; }
                    std::optional<math::Fixed> best;
                    for (const auto& point : profile->hardpoints) {
                        if (!impl_->target_hardpoint_standing(unit, point.hardpoint)) continue;
                        auto placed = math::transform_point(transform.value(), point.position);
                        if (!placed) { failures[partition] = placed.error(); return; }
                        auto distance = distance_between(droid.state.position, placed.value());
                        if (!distance) { failures[partition] = distance.error(); return; }
                        if (!best || distance.value().raw() < best->raw()) {
                            best = distance.value();
                            aim = placed.value();
                            hardpoint = point.hardpoint < profile->aimed_routes.size()
                                ? profile->aimed_routes[point.hardpoint] : point.hardpoint;
                        }
                    }
                }
                auto distance = distance_between(droid.state.position, aim);
                if (!distance) { failures[partition] = distance.error(); return; }
                if (distance.value().raw() < buzz->damage_radius.raw()) {
                    damaging = true;
                    service.hits.push_back({unit.state.entity_id, hardpoint});
                } else if (!damaging && (nearest == nullptr || distance.value().raw() < nearest_distance.raw())) {
                    nearest = &unit;
                    nearest_distance = distance.value();
                }
            }
            // WAB-74: with nothing in damage reach, the droids close at full speed on the nearest one.
            if (damaging || nearest == nullptr) continue;
            const auto& from = droid.state.position;
            auto dx = math::subtract(nearest->state.position.x, from.x);
            auto dy = math::subtract(nearest->state.position.y, from.y);
            auto dz = math::subtract(nearest->state.position.z, from.z);
            if (!dx || !dy || !dz) { failures[partition] = (!dx ? dx : !dy ? dy : dz).error(); return; }
            const math::Vec3 delta{dx.value(), dy.value(), dz.value()};
            if (delta == math::Vec3{}) continue;
            auto direction = math::normalize(delta);
            if (!direction) { failures[partition] = direction.error(); return; }
            const auto step = [&](const math::Fixed origin, const math::Fixed axis) -> core::Result<math::Fixed> {
                auto scaled = math::multiply(axis, buzz->speed);
                if (!scaled) return scaled;
                return math::add(origin, scaled.value());
            };
            auto x = step(from.x, direction.value().x);
            auto y = step(from.y, direction.value().y);
            auto z = step(from.z, direction.value().z);
            if (!x || !y || !z) { failures[partition] = (!x ? x : !y ? y : z).error(); return; }
            service.move = math::Vec3{x.value(), y.value(), z.value()};
        }
    });
    if (!scanned) return scanned;
    for (const auto& failure : failures) if (failure) return core::Result<void>::failure(*failure);
    const auto deliver = [&](const EntityId target_id, const math::Fixed amount, const std::uint32_t hardpoint,
                             const std::uint32_t damage_type, const PlayerId killer, const EntityId attacker) -> core::Result<void> {
        const auto target = staged.find(target_id);
        if (target == staged.end() || !target->second.durability || damage_rules == nullptr || damage_blocked())
            return core::Result<void>::success();
        // WAB-72: miscellaneous damage through the ordinary damage interface, as the energy beam.
        Hit hit{amount, damage_type, false, true, true, hardpoint, false, true, {}};
        hit.take_damage_multiplier = target->second.take_damage_mode;
        const auto hull_before = target->second.durability->hull;
        const auto shields_before = target->second.durability->shields;
        const auto taken = apply_hit(*impl_->health_profile(target->second), *damage_rules, *target->second.durability, hit, tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(target->second, hull_before, shields_before);
        if (!taken.value().cancelled) notify_retaliation(target->second, attacker, hit.amount);
        impl_->end_depleted_defend(target->second, tick, taken.value().storm_shield_branch);
        if (taken.value().damage.destroyed_hardpoint)
            events.push_back(destruction_event(tick, EventKind::hardpoint_destroyed, target->second.state,
                *taken.value().damage.destroyed_hardpoint));
        if (taken.value().damage.unit_destroyed) {
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, target->second.state, 0, killer));
            killed.push_back(target->second.state);
            impl_->adjust_owned(target->second.state, false);
            staged.erase(target);
        }
        return core::Result<void>::success();
    };
    for (std::size_t i = 0; i < services.size(); ++i) {
        const auto& service = services[i];
        if (!service.due) continue;
        const auto state = impl_->buzz_droids[i];
        const auto source_droid = staged.find(state.droid);
        if (source_droid == staged.end()) continue; // An earlier hostile swarm may have killed this droid.
        const auto* buzz = impl_->buzz_profile(state.source_type);
        const auto owner = source_droid->second.state.owner;
        // WAB-72: the droids wear themselves down first, then damage every victim in reach.
        if (auto worn = deliver(state.droid, buzz->own_damage, hull_target, buzz->damage_type, owner, invalid_entity_id); !worn) return worn;
        for (const auto& hit : service.hits) {
            if (auto dealt = deliver(hit.target, buzz->enemy_damage, hit.hardpoint, buzz->damage_type, owner, state.droid); !dealt) return dealt;
        }
        if (const auto droid = staged.find(state.droid); droid != staged.end() && service.move) {
            droid->second.state.position = *service.move;
            droid->second.placement_cache.reset();
        }
    }
    std::erase_if(impl_->buzz_droids, [&](const BuzzDroidState& droid) { return staged.find(droid.droid) == staged.end(); });
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
