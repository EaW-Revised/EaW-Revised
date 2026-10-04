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
    const auto& combat_events = targets_.value().combat_events.value();
    const auto& aim_offsets = targets_.value().aim_offsets.value();
    const auto& world = targets_.value().world.value();
    const auto& view = world.value();
    const auto& durability = impl_->durability;
    // Projectile phase (#74, DG-30): with damage rules, workers fly every projectile that was in
    // flight at the start of the tick one frame through the same immutable world view and write
    // its outcome to its own slot. The shots of this tick start flying next tick (DG-21).
    flights_.emplace();
    const auto& damage_rules = flights_.value().damage_rules.emplace(durability.damage ? &*durability.damage : nullptr);
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
    auto& launched = flights_.value().launched.emplace();
    auto& next_projectile = flights_.value().next_projectile.emplace(impl_->next_projectile);
    if (damage_rules != nullptr) {
        std::size_t shot_index = 0;
        for (const auto& event : combat_events) {
            if (event.kind != CombatEventKind::weapon_fired) {
                continue;
            }
            const auto offset = shot_index < aim_offsets.size() ? aim_offsets[shot_index] : math::Vec3{};
            ++shot_index;
            const auto* shooter = view.find(event.shooter);
            const auto& weapons = shooter->profile->weapons;
            const auto weapon = std::find_if(weapons.begin(), weapons.end(),
                [&](const WeaponProfile& entry) { return entry.hardpoint == event.weapon; });
            // AB-66 (#561): an ion shot flies the weapon's ability shot.
            if (weapon == weapons.end()) continue;
            const auto& shot = (event.outcome & fired_ability_shot) != 0U ? weapon->ability_shot
                : (event.outcome & fired_barrage_shot) != 0U ? weapon->barrage_shot : weapon->shot;
            if (!shot) continue;
            // DG-05: the shooter's mode flag, on by default and only off without a durability
            // profile's data saying otherwise; no modelled ability changes it (space-abilities AB-25).
            const bool allow_diminishing_firepower =
                shooter->durability_profile == nullptr || shooter->durability_profile->allow_diminishing_firepower;
            const auto* target = view.find(event.target);
            const auto target_radius = target != nullptr ? detail::target_soft_radius(view.motion, *target) : math::Fixed{};
            auto projectile = detail::launch_projectile(
                event, *shot, shooter->owner, allow_diminishing_firepower, next_projectile++, offset, target_radius);
            if (!projectile) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " unit " + std::to_string(event.shooter) + ": "
                        + projectile.error().message));
            }
            const auto source = live_unit(moving, event.shooter);
            if (source != nullptr && source->upgrade_bonuses[1].raw() != 0) {
                // WCC-44: the shooter damage term, followed by target defense at impact.
                const auto boosted = math::multiply(projectile.value().damage,
                    math::Fixed::from_raw(math::Fixed::scale + source->upgrade_bonuses[1].raw()));
                if (!boosted) return core::Result<void>::failure(boosted.error());
                projectile.value().damage = boosted.value();
                projectile.value().source_damage_factor = math::Fixed::from_raw(
                    math::Fixed::scale + source->upgrade_bonuses[1].raw());
            }
            launched.push_back(projectile.value());
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

std::span<const EntityId> session_detail::Tick::redirect_recipients(const EntityId target) const {
    const auto* found = redirect_unit(target);
    const auto* profile = found ? impl_->combat.find(found->state.type_id) : nullptr;
    if (!profile || !profile->redirect_damage_to_teammates) return {};
    const auto& parents = craft_prep_->craft_squadron.value();
    const auto parent = parents.find(target);
    if (parent == parents.end()) return {};
    const auto& minds = craft_prep_->minds.value();
    const auto mind = minds.find(parent->second);
    if (mind == minds.end() || mind->second.roster.size() <= 1) return {};
    return mind->second.roster;
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
    const auto deliver_redirected = [&](const EntityId id, Hit hit, const PlayerId killer) -> core::Result<void> {
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
        const auto target = staged.find(recipient.id);
        if (target == staged.end() || !target->second.durability) return core::Result<void>::success();
        const auto arriving = arrivals.find(recipient.id);
        if (arriving != arrivals.end() && arriving->second.frame < arrival_visible_frame) return core::Result<void>::success();
        const auto* profile = impl_->health_profile(target->second);
        const auto defense = craft_defense.find(recipient.id);
        const auto modifier = math::Fixed::from_raw((defense == craft_defense.end() ? 0 : defense->second.raw())
            + (target->second.arrival_vulnerable_until ? impl_->economy.vulnerability.raw() : 0)
            + impl_->concentrate_defense(target->second, staged, staging_->ledgers.value()).raw());
        auto hit = detail::area_hit(projectile, recipient, modifier);
        if (!hit) return core::Result<void>::failure(hit.error());
        hit.value().take_damage_multiplier = target->second.take_damage_mode;
        // WHE-51: modes are current at delivery; a removed shooter contributes identity.
        const auto source = staged.find(projectile.shooter);
        if (source != staged.end()) hit.value().cause_damage_multiplier = source->second.cause_damage_mode;
        auto redirected = redirect_damage(recipient.id, hit.value(), [&](const EntityId id, Hit share) {
            return deliver_redirected(id, share, projectile.owner);
        });
        if (!redirected) return core::Result<void>::failure(redirected.error());
        if (redirected.value()) return core::Result<void>::success();
        const auto hull = target->second.durability->hull;
        const auto shields = target->second.durability->shields;
        auto taken = apply_hit(*profile, *damage_rules, *target->second.durability, hit.value(), tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(target->second, hull, shields);
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
    // U-04 project policy: retain source metadata, round positive delay up to the next frame,
    // and deliver due entries in (due frame, creation order) before this tick's new impacts.
    std::stable_sort(pending_blast_damage.begin(), pending_blast_damage.end(),
        [](const auto& left, const auto& right) { return left.due < right.due; });
    auto blast_due_end = pending_blast_damage.begin();
    while (blast_due_end != pending_blast_damage.end() && blast_due_end->due <= tick) {
        auto delivered = deliver_area(blast_due_end->source, blast_due_end->recipient);
        if (!delivered) return core::Result<void>::failure(delivered.error());
        ++blast_due_end;
    }
    pending_blast_damage.erase(pending_blast_damage.begin(), blast_due_end);
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
                    const auto frames = static_cast<std::uint64_t>(recipient.delay.raw() / math::Fixed::scale)
                        * logical_frames_per_second + static_cast<std::uint64_t>((recipient.delay.raw() % math::Fixed::scale
                        * logical_frames_per_second + math::Fixed::scale - 1) / math::Fixed::scale);
                    pending_blast_damage.push_back({tick + frames, flights[index]->projectile, recipient});
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
        if (flight->expired) {
            const auto spawn = std::lower_bound(impl_->ability_spawns.begin(), impl_->ability_spawns.end(), flight->projectile.id,
                [](const auto& entry, const std::uint64_t id) { return entry.id < id; });
            if (spawn != impl_->ability_spawns.end() && spawn->id == flight->projectile.id) {
                spawn->detonated = true;
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
                auto delivered = deliver_blast(flight_index);
                if (!delivered) return core::Result<void>::failure(delivered.error());
            }
            continue;
        }
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
        if (projectile.damage.raw() == 0 && projectile.blast.enabled()) {
            auto caused = math::multiply(primary_amount, projectile.source_damage_factor);
            if (!caused) return core::Result<void>::failure(caused.error());
            primary_amount = caused.value();
        }
        Hit hit{primary_amount, projectile.damage_type, true, projectile.shield_damage,
            projectile.hitpoint_damage, hardpoint, projectile.allow_diminishing_firepower,
            projectile.internal_damage_misc, modifier, projectile.energy_damage};
        hit.take_damage_multiplier = target->second.take_damage_mode;
        const auto source = staged.find(projectile.shooter);
        if (source != staged.end()) hit.cause_damage_multiplier = source->second.cause_damage_mode;
        auto redirected = redirect_damage(*flight->hit, hit, [&](const EntityId id, Hit share) {
            return deliver_redirected(id, share, projectile.owner);
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
        impl_->end_depleted_defend(target->second, tick, outcome.value().storm_shield_branch);
        // EN-08: only a positive energy drain disables engines; no locomotor means no effect.
        if (projectile.disable_engines_frames && outcome.value().drained.raw() > 0
            && profile.max_speed && !outcome.value().damage.unit_destroyed) {
            const bool was_online = engines_online(profile, *target->second.durability);
            disable_engines(*target->second.durability, *projectile.disable_engines_frames, tick);
            // EN-09: discard a pending group/search speed cap, retaining its destination.
            const bool waited = target->second.formation.has_value();
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
        if (!service.due || !damage_rules || !target->second.durability) continue;
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
            return deliver_redirected(id, share, source->second.state.owner);
        });
        if (!redirected) return core::Result<void>::failure(redirected.error());
        if (redirected.value()) continue;
        const auto hull_before = target->second.durability->hull;
        const auto shields_before = target->second.durability->shields;
        const auto taken = apply_hit(*impl_->health_profile(target->second), *damage_rules, *target->second.durability, hit, tick);
        if (!taken) return core::Result<void>::failure(taken.error());
        impl_->track_damage(target->second, hull_before, shields_before);
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
    projectiles.insert(projectiles.end(), launched.begin(), launched.end());

    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
