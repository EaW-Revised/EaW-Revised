#include "eawr/core/load_profile.hpp"
#include "eawr/core/tick_profile.hpp"
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
#include "session_cmd.hpp"
#include "session_services.hpp"
#include "tactical_internal.hpp"

#include "../../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include "diagnostic_timer.hpp"
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <deque>
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

core::Result<void> session_detail::Tick::service_retreat_flight(UnitStage& staged,
    std::vector<Event>& events, std::vector<UnitState>& killed) {
    if (impl_->setup.domain != BattleDomain::space) return core::Result<void>::success();
    // WR-50..52 / FC-R02: service once per active tactical frame, independently of client traffic.
    if (impl_->retreat && pending_retreat_.phase == RetreatPhase::escaping && !pending_outcome_) {
        const auto issuer = pending_retreat_.player;
        const auto frame_sample = pending_retreat_.cinematic_frame;
        const auto seat = std::lower_bound(impl_->setup.cinematic_seats.begin(), impl_->setup.cinematic_seats.end(), issuer,
            [](const auto& row, const PlayerId player) { return row.player < player; });
        const bool enabled = seat == impl_->setup.cinematic_seats.end() || seat->player != issuer || seat->cinematics_enabled;
        const auto effect = tactical::service_retreat_flight(pending_retreat_, enabled, frame_sample);
        if (effect.completed) {
            auto actors = impl_->retreat_actors(&staged);
            for (auto& actor : actors) actor.live = staged.find(actor.id) != staged.end()
                && std::find(pending_retreat_.actors.begin(), pending_retreat_.actors.end(), actor.id) != pending_retreat_.actors.end();
            auto attrition = retreat_attrition(actors, issuer, impl_->retreat->attrition_factor, impl_->retreat->protection, [&]() {
                // The world's stream algorithm: one word per qualified WGT-36 actor.
                pending_rng_ += 0x9e3779b97f4a7c15ULL;
                auto word = pending_rng_;
                word = (word ^ (word >> 30U)) * 0xbf58476d1ce4e5b9ULL;
                word = (word ^ (word >> 27U)) * 0x94d049bb133111ebULL;
                return static_cast<std::uint32_t>((word ^ (word >> 31U)) & 32767U);
            });
            if (!attrition) return core::Result<void>::failure(attrition.error());
            for (const auto id : attrition.value().destroyed) std::erase(pending_retreat_.actors, id);
            const auto winner = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(),
                [&](const auto& row) { return row.commandable() && !impl_->allied(issuer, row.player_id); });
            if (winner == impl_->setup.players.end()) return core::Result<void>::failure(detail::diagnostic(
                diagnostic_codes::invalid_setup, "completed retreat has no enemy contender"));
            pending_outcome_ = BattleOutcome{VictoryCondition::enemies_retreated, winner->player_id,
                winner->team_id, tick, 0, tick};
            pending_victory_event_ = Event{tick, EventKind::victory, winner->player_id};
        }
        // WR-51/53: sparse lifecycle commit over the ending player's live roster.
        // Escaped actors remain persistent survivors. Ordinary death notifications
        // account for stations and the unescaped remainder exactly once.
        // WR-85: craft depart with their escaping squadron. Build this membership
        // only at the terminal lifecycle barrier, after squadron attrition.
        std::set<EntityId> escaped;
        if (effect.completed) {
            escaped.insert(pending_retreat_.actors.begin(), pending_retreat_.actors.end());
            for (const auto& squadron : impl_->squadrons) if (escaped.contains(squadron.container))
                escaped.insert(squadron.members.begin(), squadron.members.end());
        }
        if (effect.destroy_stations || effect.completed) for (auto at = staged.begin(); at != staged.end();) {
            const auto& unit = at->second;
            const bool station = std::binary_search(impl_->victory.starbase_types.begin(), impl_->victory.starbase_types.end(), unit.state.type_id);
            if (unit.state.owner != issuer || !(effect.destroy_stations ? station : !escaped.contains(at->first))) { ++at; continue; }
            // RP-09 / FS-R06: a station destroyed by the retreat creates its authored explosion first, a real
            // projectile at its position (the shooter is the station itself): its blast damages what is near.
            if (effect.destroy_stations) if (const auto* profile = impl_->combat.find(unit.state.type_id); profile && profile->retreat_explosion) {
                auto origin = unit.state.position;
                auto height = math::add(origin.z, profile->ranged_target_z_adjust);
                if (!height) return core::Result<void>::failure(height.error());
                origin.z = height.value();
                auto matrix = math::to_matrix(unit.state.rotation, origin);
                if (!matrix) return core::Result<void>::failure(matrix.error());
                auto aim = math::transform_point(matrix.value(), {math::Fixed::from_raw(math::Fixed::scale), {}, {}});
                if (!aim) return core::Result<void>::failure(aim.error());
                CombatEvent shot;
                shot.tick = tick; shot.shooter = unit.state.entity_id;
                shot.weapon = retreat_explosion_slot_flag;
                shot.target_hardpoint = no_hardpoint; shot.origin = origin; shot.aim = aim.value();
                auto projectile = detail::launch_projectile(shot, *profile->retreat_explosion, unit.state.owner,
                    true, flights_.value().next_projectile.value()++, {}, {});
                if (!projectile) return core::Result<void>::failure(projectile.error());
                impacts_.value().projectiles.value().push_back(std::move(projectile).value());
            }
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit.state));
            killed.push_back(unit.state); impl_->adjust_owned(unit.state, false);
            staged.erase(at); ++at;
        }
    }

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::retreat_service() {
    if (impl_->setup.domain != BattleDomain::space) return core::Result<void>::success();
    if (!impl_->retreat || (pending_retreat_.phase != RetreatPhase::countdown
        && pending_retreat_.phase != RetreatPhase::escaping)) return core::Result<void>::success();
    auto& survivors = surviving_->survivors.value();
    auto& instances = systems_->instances.value();
    UnitStage staged(std::move(survivors));
    const auto survivor_count = staged.size();
    // FS-R02/04, FD-R04: one suffix after object deletion and before income.
    if (pending_retreat_.phase == RetreatPhase::countdown) {
        auto gate = impl_->retreat_gate(pending_retreat_.player); gate.battle_decided = pending_outcome_.has_value();
        const auto actors = tick >= pending_retreat_.deadline ? impl_->retreat_actors(&staged) : std::vector<RetreatActor>{};
        service_retreat(impl_->retreat->rules, pending_retreat_, tick, gate, actors);
        if (pending_retreat_.phase == RetreatPhase::escaping) {
            // FC-R02 / FSC-R03: latch once at countdown expiry. Station death at
            // k181 cannot change the stored 15-second completion deadline.
            bool station{};
            for (const auto& [id, unit] : staged) {
                static_cast<void>(id);
                station = station || (unit.state.owner == pending_retreat_.player
                    && std::binary_search(impl_->victory.starbase_types.begin(), impl_->victory.starbase_types.end(), unit.state.type_id));
            }
            pending_retreat_.cinematic_duration = (station ? 15U : 5U) * logical_frames_per_second;
        }
    }
    if (pending_retreat_.phase == RetreatPhase::escaping) {
        if (auto serviced = service_retreat_flight(staged, impacts_->events.value(), impacts_->killed.value()); !serviced)
            return serviced;
    }
    if (staged.size() != survivor_count)
        std::erase_if(instances, [&](const auto& instance) { return staged.find(instance.entity_id) == staged.end(); });
    survivors = staged.release();
    return core::Result<void>::success();
}

void session_detail::Tick::register_jammer(const EntityId source, const bool active) {
    const auto& static_sources = targets_.value().world.value().value().static_projectile_defences;
    if (std::binary_search(static_sources.begin(), static_sources.end(), source)
        || std::find(created_static_defences_.begin(), created_static_defences_.end(), source) != created_static_defences_.end()) return;
    if (!active) { std::erase(projectile_defence_order_, source); return; }
    if (projectile_defence_order_.empty()) {
        projectile_defence_order_ = static_sources;
        for (const auto id : created_static_defences_)
            if (!std::binary_search(static_sources.begin(), static_sources.end(), id)) projectile_defence_order_.push_back(id);
    }
    if (std::find(projectile_defence_order_.begin(), projectile_defence_order_.end(), source) == projectile_defence_order_.end())
        projectile_defence_order_.push_back(source);
}

void session_detail::Tick::spawn_instant_ability(LiveUnit& source, const AbilityKind kind,
    const SpawnedAbilityProfile& spawn, const math::Vec3& position) {
    const auto count = kind == AbilityKind::cluster_bomb ? spawn.ring_count : 1U;
    for (std::uint32_t bomb = 0; bomb < count; ++bomb) {
        Projectile projectile;
        projectile.id = flights_.value().next_projectile.value()++;
        projectile.shooter = source.state.entity_id;
        projectile.owner = source.state.owner;
        projectile.weapon = object_weapon;
        projectile.target_hardpoint = no_hardpoint;
        projectile.position = position;
        projectile.damage = spawn.damage;
        projectile.damage_type = spawn.damage_type;
        projectile.shield_damage = spawn.shield_damage;
        projectile.hitpoint_damage = spawn.hitpoint_damage;
        projectile.blast = spawn.blast;
        if (kind == AbilityKind::cluster_bomb || kind == AbilityKind::fow_reveal_ping) {
            projectile.speed = spawn.speed;
            projectile.max_travel = spawn.max_travel;
            FlightState flight;
            flight.profile.kind = FlightKind::default_projectile;
            flight.profile.lifetime = spawn.lifetime;
            // WFP-03: the stock missing lifetime ends on the first flight service.
            // Zero is the equivalent threshold for our positive flight ages (WPJ-33).
            if (kind == AbilityKind::fow_reveal_ping
                && (!spawn.lifetime || spawn.lifetime->raw() < 0))
                flight.profile.lifetime = math::Fixed{};
            flight.origin = position;
            projectile.flight = flight;
            if (kind == AbilityKind::cluster_bomb) {
                // WCB-02: fixed authored order, a horizontal 200-unit ring (independent of Effective_Radius).
                const auto angle = math::Fixed::from_raw(static_cast<std::int64_t>(bomb) * math::Fixed::scale / count);
                const auto direction = math::sin_cos_turn(angle);
                projectile.step = {math::multiply(direction.cosine, spawn.speed).value(),
                    math::multiply(direction.sine, spawn.speed).value(), {}};
                projectile.yaw = math::Fixed::from_raw(static_cast<std::int64_t>(bomb) * 360 * math::Fixed::scale / count);
                const auto radius = math::Fixed::from_raw(200 * math::Fixed::scale);
                projectile.flight->aim = {math::add(position.x, math::multiply(direction.cosine, radius).value()).value(),
                    math::add(position.y, math::multiply(direction.sine, radius).value()).value(), position.z};
            }
        }
        // EUS-15: retained delayed-area input only; EUS-16 immediate hits resolve shooter.
        projectile.source_damage_factor = math::Fixed::from_raw(math::Fixed::scale + source.upgrade_bonuses[1].raw());
        impl_->ability_spawns.push_back({projectile.id, spawn.type, kind, source.state.owner, source.state.entity_id,
            position, source.state.rotation, kind == AbilityKind::fow_reveal_ping ? 0
                : tick + std::max<std::uint32_t>(1, spawn.countdown_frames), false, {}});
        impacts_.value().projectiles.value().push_back(projectile);
    }
}

core::Result<void> session_detail::Tick::abilities_tracking() {
    auto& moving = gather_.value().moving.value();
    const auto& arrivals = staging_.value().arrivals.value();
    const auto& approaches = orders_.value().approaches.value();
    // Ability phase (#76, AB-11, AB-16, AB-41, AB-42): workers service each unit's abilities in its
    // own slot: time limits, lost engines, the damage-rate window and the DEFEND stand-in. The
    // serial commit lists, ascending, the moving ships whose speed multiplier changed: their move
    // plans again this tick, before the commands (AB-24).
    // IS-03, IS-05 (#561): the same phase ends each ion stun at its end frame.
    auto& replans = tracked_.value().replans.value();
    auto& self_destructs = tracked_.value().self_destructs.emplace();
    auto& lure_ends = tracked_.value().lure_ends.emplace();
    const bool timed_effect = std::any_of(moving.begin(), moving.end(), [](const LiveUnit& unit) {
        return unit.ion_stun.has_value() || unit.engines_recovered || unit.corrupt.has_value();
    });
    // WCC-82: the damage-rate window closes for every tracked unit (only those ever hold damage).
    const bool rate_closes = rate_window_closes(tick) && std::any_of(moving.begin(), moving.end(), [](const LiveUnit& unit) {
        return unit.durability && (unit.durability->window_damage.raw() != 0 || unit.durability->damage_rate.raw() != 0);
    });
    if (!impl_->abilities.profiles.empty() || timed_effect || rate_closes) {
        std::vector<std::uint8_t> speed_changed(moving.size());
        std::vector<std::uint8_t> jammer_ended(moving.size());
        std::vector<std::uint8_t> detonating(moving.size());
        std::vector<std::uint8_t> lure_ended(moving.size());
        std::vector<std::uint8_t> mc30_fired(moving.size());
        // FC-58/UFS-27 post-movement proximity seam: immutable inputs use all objects'
        // post-movement positions; the original reads positions at each object's turn.
        // Workers write only their own script and slot.
        std::vector<UnitState> script_inputs;
        if (std::any_of(moving.begin(), moving.end(), [&](const LiveUnit& unit) {
                const auto* profile = impl_->abilities.find(unit.state.type_id);
                return profile && profile->mc30_script;
            })) {
            script_inputs.resize(moving.size());
            const auto copied = executor.execute_phase("mc30-inputs", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, moving.size());
                for (auto index = range.begin; index < range.end; ++index) script_inputs[index] = moving[index].state;
            });
            if (!copied) return core::Result<void>::failure(copied.error());
        }
        const auto& craft_squadron = craft_prep_.value().craft_squadron.value();
        const auto& collection = targets_.value().collection.value();
        const auto serviced_abilities = executor.execute_phase("abilities", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, moving.size());
            for (auto index = range.begin; index < range.end; ++index) {
                bool speed = moving[index].engines_recovered;
                if (rate_closes && moving[index].durability) close_rate_window(*moving[index].durability);
                if (moving[index].abilities) {
                    const auto& profile = *impl_->abilities.find(moving[index].state.type_id);
                    const auto jammer = ability_slot(profile, AbilityKind::sensor_jamming);
                    const bool active = jammer && moving[index].abilities->slots[*jammer].active;
                    const auto lure = ability_slot(profile, AbilityKind::lure);
                    const bool luring = lure && moving[index].abilities->slots[*lure].active;
                    speed = impl_->service_abilities(moving[index], tick) || speed;
                    lure_ended[index] = luring && !moving[index].abilities->slots[*lure].active ? 1U : 0U;
                    impl_->detect_stealth(moving[index], moving, tick); // WST-08
                    jammer_ended[index] = active && !moving[index].abilities->slots[*jammer].active ? 1U : 0U;
                    detonating[index] = impl_->service_self_destruct(moving, index, craft_squadron, collection, tick) ? 1U : 0U;
                    mc30_fired[index] = impl_->service_mc30(moving[index], script_inputs, craft_squadron, collection, tick) ? 1U : 0U;
                }
                if (moving[index].ion_stun) Impl::service_ion_stun(moving[index], tick);
                auto& recipient = moving[index];
                if (recipient.corrupt) {
                    // WHE-34: drain before the final decrement; this is a shield setter, not damage.
                    if (recipient.durability) recipient.durability->shields = math::Fixed::from_raw(std::max<std::int64_t>(0,
                        recipient.durability->shields.raw() - recipient.corrupt->drain_per_frame.raw()));
                    if (tick >= recipient.corrupt->expires_tick) {
                        recipient.corrupt.reset();
                        if (recipient.abilities) {
                            const auto& profile = *impl_->abilities.find(recipient.state.type_id);
                            for (std::size_t slot = 0; slot < profile.abilities.size(); ++slot) {
                                auto& state = recipient.abilities->slots[slot];
                                const auto& ability = profile.abilities[slot];
                                // Expiration countdowns are not already-recharged timers.
                                if ((!state.active || state.expires_tick == 0) && state.ready_tick <= tick
                                    && ability.recharge_frames > 0) state.ready_tick = tick + ability.recharge_frames;
                            }
                        }
                    }
                }
                speed_changed[index] = speed ? 1U : 0U;
            }
        });
        if (!serviced_abilities) {
            return core::Result<void>::failure(serviced_abilities.error());
        }
        for (std::size_t index = 0; index < moving.size(); ++index) {
            auto& unit = moving[index];
            // FD-47: ordered projectile IDs in this same admitted turn, after ordinary
            // ability service and before subsequent phase readers. No queued command.
            if (mc30_fired[index]) {
                const auto& profile = *impl_->abilities.find(unit.state.type_id);
                const auto slot = *ability_slot(profile, AbilityKind::cluster_bomb);
                spawn_instant_ability(unit, AbilityKind::cluster_bomb, *profile.abilities[slot].spawned, unit.state.position);
            }
            if (jammer_ended[index]) register_jammer(unit.state.entity_id, false);
            if (detonating[index] != 0U) self_destructs.push_back(unit.state.entity_id);
            if (lure_ended[index] != 0U) lure_ends.push_back(unit.state.entity_id);
            if (speed_changed[index] != 0U && unit.motion && unit.motion->kind == MotionKind::path && !unit.formation) {
                replans.push_back(unit.state.entity_id);
            }
        }
    }
    impl_->concentrate_changed_targets.clear();
    if (auto serviced = impl_->service_concentrate(moving, tick, executor); !serviced) return serviced;
    if (auto serviced = impl_->service_beams(moving, tick, executor); !serviced) return serviced;
    if (auto serviced = impl_->service_weaken(moving, tick, executor); !serviced) return serviced;
    if (auto refreshed = impl_->refresh_concentrate_bonuses(moving, impl_->command_ledger, impl_->ledgers, executor); !refreshed) return refreshed;
    replans.insert(replans.end(), impl_->beam_replans.begin(), impl_->beam_replans.end());
    for (const auto id : impl_->concentrate_changed_targets) {
        const auto unit = std::lower_bound(moving.begin(), moving.end(), id,
            [](const auto& value, const EntityId entity) { return value.state.entity_id < entity; });
        if (unit != moving.end() && unit->state.entity_id == id && unit->motion && unit->motion->kind == MotionKind::path) {
            unit->formation.reset();
            replans.push_back(id);
        }
    }
    // #3644: the object turn follows the ability service and the special-ability handlers and precedes the
    // replan merge, tracking, late impacts and the commands. Without a provider or recorded uses it is a no-op.
    if (auto turned = object_turn_seam(); !turned) return turned;
    std::sort(replans.begin(), replans.end());
    replans.erase(std::unique(replans.begin(), replans.end()), replans.end());
    // Tracking phase (#71, AV-02, AV-03): with avoidance rules and a move due this tick,
    // workers sample each tracked moving unit's prediction at the window boundaries of its
    // layer, from the layer's rolled anchor and from this frame, into disjoint slots. The
    // planning below stays serial in command order (AV-15).
    const auto& table = impl_->motion;
    const auto& frame = tracked_.value().frame.emplace(tick + 1);
    tracked_.value().rebuilt.emplace();
    auto& tracking = tracked_.value().tracking.emplace();
    auto& members_before = tracked_.value().members_before.emplace();
    if (table.avoidance) {
        members_before = layer_members(table, moving);
        // A speed change replans a move (AB-24): a due ability command may cause one.
        // HCB-59/FD-64: retained next legs need the same avoidance world as player moves.
        bool moves_due = !replans.empty() || !orders_->waypoint_ready.empty();
        for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end() && iterator->first.tick == tick;
             ++iterator) {
            const auto& payload = iterator->second.payload;
            moves_due = moves_due || point_move(payload).has_value() || approach_target_of(payload) != invalid_entity_id
                || std::holds_alternative<AbilityPayload>(payload) || std::holds_alternative<AreaAbilityPayload>(payload)
                || std::holds_alternative<ReinforcePayload>(payload);
        }
        for (const auto& unit : moving) {
            moves_due = moves_due || (unit.formation && unit.formation->frame <= frame);
        }
        moves_due = moves_due || !approaches.empty();
        if (moves_due) {
            tracking.emplace();
            tracking->frame = frame;
            tracking->interval = table.avoidance->tracking_interval;
            tracking->windows = table.avoidance->tracking_windows;
            for (const auto& [id, arrival] : arrivals) {
                static_cast<void>(arrival);
                tracking->suspended.insert(id);
            }
            for (std::size_t index = 0; index < 4; ++index) {
                tracking->current_start[index] = rolled_start(impl_->tracking_anchor[index], frame, tracking->interval);
            }
            std::vector<TrackSamples> sampled(moving.size());
            std::vector<std::optional<core::Diagnostic>> sample_errors(moving.size());
            // PC-09: the plans of sliced searches that have ended, by unit, while the unit waits for them.
            std::vector<const Impl::SlicedSearch*> published(moving.size());
            for (std::size_t index = 0; index < moving.size(); ++index) {
                const auto& unit = moving[index];
                if (!unit.formation || !unit.formation->sliced) continue;
                const auto found = impl_->searches.find(unit.state.entity_id);
                if (found != impl_->searches.end() && found->second.plan) published[index] = &found->second;
            }
            const auto tracked = executor.execute_phase("tracking", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, moving.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto& unit = moving[index];
                    const auto* footprint = tracked_footprint(table, unit.state.type_id);
                    const bool moves = unit.motion && unit.motion->kind != MotionKind::none;
                    if (footprint == nullptr || footprint->obstacle || (!moves && published[index] == nullptr)) {
                        continue;
                    }
                    const auto layer = dynamic_layer_index(footprint->layer);
                    if (!layer) continue;
                    const auto fail = [&](const core::Diagnostic& error) {
                        sample_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure, "tick "
                            + std::to_string(tick) + " unit " + std::to_string(unit.state.entity_id) + ": " + error.message);
                    };
                    const auto yaw = yaw_degrees(unit.state.rotation);
                    if (!yaw) {
                        fail(yaw.error());
                        continue;
                    }
                    const auto start = tracking->current_start[*layer];
                    // A waiting group member is predicted up to its plan frame (FM-10); a ship
                    // whose sliced search has ended, along that search's plan after it (PC-09).
                    const auto clip = unit.formation ? std::optional(unit.formation->frame) : std::nullopt;
                    const auto* ahead = published[index];
                    const auto sample = [&](const std::uint64_t from) {
                        if (ahead != nullptr) {
                            return sample_ahead(moves ? &*unit.motion : nullptr, ahead->frame, *ahead->plan, from,
                                tracking->interval, tracking->windows, unit.state.position, yaw.value());
                        }
                        return sample_windows(
                            *unit.motion, from, tracking->interval, tracking->windows, unit.state.position, yaw.value(), clip);
                    };
                    auto current = sample(start);
                    if (!current) {
                        fail(current.error());
                        continue;
                    }
                    sampled[index].current = std::move(current).value();
                    if (start != frame) {
                        auto fresh = sample(frame);
                        if (!fresh) {
                            fail(fresh.error());
                            continue;
                        }
                        sampled[index].fresh = std::move(fresh).value();
                    }
                }
            });
            if (!tracked) {
                return core::Result<void>::failure(tracked.error());
            }
            for (std::size_t index = 0; index < moving.size(); ++index) {
                if (sample_errors[index]) {
                    return core::Result<void>::failure(std::move(*sample_errors[index]));
                }
                if (!sampled[index].current.empty()) {
                    tracking->samples.emplace(moving[index].state.entity_id, std::move(sampled[index]));
                }
            }
        }
    }

    // WHE-63: turn births are already allocated and attached in command order.
    // Join only after the index-aligned movement/tracking inputs have finished.
    const auto first_turn_birth = object_turn_births_.empty() ? staging_->next_id.value() : object_turn_births_.front().state.entity_id;
    for (auto& born : object_turn_births_) {
        track_victory_change(nullptr, &born.state, false);
        moving.push_back(std::move(born));
    }
    object_turn_births_.clear();
    auto& production_census_visits = tracked_.value().production_census_visits.emplace(0);
    const bool buying = std::any_of(impl_->pending.begin(), impl_->pending.end(), [&](const auto& entry) {
        const auto* buy = std::get_if<BuyPayload>(&entry.second.payload);
        if (entry.first.tick != tick || buy == nullptr) return false;
        for (const auto& menu : impl_->economy.menus) {
            const auto* option = menu.find(buy->type);
            if (option != nullptr && (option->requirements.current_player || option->requirements.current_allies
                || !option->requirements.prerequisites.empty())) return true;
        }
        return false;
    });
    if (buying) {
        const auto counted = impl_->count_owned(moving, executor);
        if (!counted) return core::Result<void>::failure(counted.error());
        production_census_visits += moving.size();
    }
    mark("staged_map_build", true);
    tracked_.value().staged.emplace(std::move(moving));
    sync_radar_creations(*tracked_->staged, first_turn_birth, *staging_->next_id);

    mark("staged_map_build", false);
    // Serial, in ascending projectile ID: each hit applies to the staged unit it reached (DG-01 to
    // DG-11). A unit killed by an earlier hit leaves at once; a later projectile that reached it
    // is spent without effect.
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::commands() {
    auto& arrivals = staging_.value().arrivals.value();
    auto& staged = tracked_.value().staged.value();
    auto& events = impacts_.value().events.value();
    PlanLanes lanes{*this};
    auto& concentrate_batches = lanes.concentrate_batches;
    auto& concentrate_new_bodies = lanes.concentrate_new_bodies;
    auto& concentrate_command_key = lanes.concentrate_command_key;
    auto& waypoint_legs = lanes.waypoint_legs;
    auto& autofire_commands = lanes.autofire_commands;
    auto& autofire_switched = lanes.autofire_switched;
    auto& pending = lanes.pending;
    const auto submit = [&](const EntityId id, const LiveUnit& live, const std::size_t layer, const std::optional<std::uint64_t> clip) { return lanes.submit(id, live, layer, clip); };
    const auto world_for = [&](const std::size_t layer, CollisionWorld& world) { return lanes.world_for(layer, world); };
    const auto unit_failure = [&](const EntityId id, const core::Diagnostic& error) { return lanes.unit_failure(id, error); };
    const auto plan_pending = [&]() { return lanes.plan_pending(); };
    const auto plan_approach = [&](const EntityId id, LiveUnit& live, const Impl::ApproachPlan& plan) { return lanes.plan_approach(id, live, plan); };
    const auto replan = [&](const EntityId id) { return lanes.replan(id); };
    if (auto prepared = lanes.prepare_paths(); !prepared) return prepared;

    // Borrow callbacks from the phase-local lane scheduler.
    const CommandHandlers handlers{*this, std::cref(replan), std::cref(submit),
        std::cref(plan_approach), std::cref(world_for), concentrate_batches,
        concentrate_new_bodies, concentrate_command_key};
    const auto apply_ability = [&](const EntityId id, const AbilityPayload& payload) {
        return handlers.apply_ability(id, payload);
    };

    if (auto prepared = lanes.prepare_commands(); !prepared) return prepared;
    auto& diagnostics = commands_->diagnostics.value();
    auto& searches = commands_->searches;
    // WAB-36: one authored-order holder pass through ordinary command admission. Immutable
    // candidate preparation is partitioned; command transactions commit in ascending holder ID.
    // The command-stage seam uses the persisted attack target, as the former split passes did.
    // Only an actual switch-on consumes the pass: a rejection or WHE-24 empty-query release
    // leaves the following candidate available. Both the queue and success set die this tick.

    // Borrow the lane and ability callbacks; their storage stays in this serial phase.
    const auto pending_empty = [&] { return pending.empty(); };
    const CommandOrders command_orders{*this, std::cref(apply_ability), std::cref(submit),
        std::cref(world_for), std::cref(plan_approach), std::cref(plan_pending),
        std::cref(pending_empty), std::cref(unit_failure)};
    bool autofire_queued = false;
    auto& due_end = commands_.value().due_end.emplace(impl_->pending.begin());
    const auto commands_left = [&] {
        return (waypoint_legs && !waypoint_legs->empty()) || !concentrate_batches.empty() || !autofire_commands.empty()
            || (due_end != impl_->pending.end() && due_end->first.tick == tick);
    };
    while (true) {
        if (!commands_left()) {
            if (autofire_queued) break;
            autofire_queued = true;
            if (auto queued = lanes.queue_targeted_autofire(); !queued) return queued;
            if (!commands_left()) break;
        }
        PlayerCommand current;
        const bool waypoint_resume = (waypoint_legs && !waypoint_legs->empty());
        if (waypoint_resume) {
            current = std::move(waypoint_legs->front());
            waypoint_legs->pop_front();
        } else if (!concentrate_batches.empty()) {
            current = std::move(concentrate_batches.front());
            concentrate_batches.pop_front();
        } else if (!autofire_commands.empty()) {
            current = std::move(autofire_commands.front());
            autofire_commands.pop_front();
            if (autofire_switched.contains(current.units.front())) continue;
        } else {
            current = due_end->second;
            ++due_end;
        }
        // WAB-36: a synthesised command and the batch it recruits announce no order events.
        const bool silent = waypoint_resume || synthesised_command(current.key);
        // HCB-59: append once per existing coordinator, including unselected members.
        // This is command admission, outside partitioned per-entity tick services.
        if (!waypoint_resume) {
            if (const auto* move = std::get_if<MovePayload>(&current.payload); move && move->append) {
                const auto& membership = craft_prep_->craft_squadron.value();
                std::vector<EntityId> holders;
                for (const auto id : current.units) {
                    const auto member = membership.find(id);
                    const auto holder = member == membership.end() ? id : member->second;
                    if (std::find(holders.begin(), holders.end(), holder) == holders.end()) holders.push_back(holder);
                }
                current.units = std::move(holders);
            }
        }
        const auto selected_units = current.units;
        if (!waypoint_resume) {
            if (const auto* move = std::get_if<MovePayload>(&current.payload); move && move->append) {
                std::set<CommandKey> coordinators;
                for (const auto id : selected_units) {
                    const auto found = staged.find(id);
                    if (found != staged.end() && found->second.state.owner == current.key.player_id
                        && found->second.waypoints && found->second.waypoints->coordinator)
                        coordinators.insert(*found->second.waypoints->coordinator);
                }
                for (const auto& [id, member] : staged) {
                    if (member.state.owner == current.key.player_id && member.waypoints
                        && member.waypoints->coordinator && coordinators.contains(*member.waypoints->coordinator)
                        && std::find(current.units.begin(), current.units.end(), id) == current.units.end())
                        current.units.push_back(id);
                }
            }
        }
        std::optional<PlayerCommand> resolved;
        const auto search = std::lower_bound(searches.begin(), searches.end(), current.key,
            [](const Commands::Search& entry, const CommandKey& key) { return entry.key < key; });
        if (search != searches.end() && search->key == current.key) {
            // SAE-10: a failed ring advances the live search without issuing a placement.
            if (!search->result.valid) continue;
            resolved = current;
            std::get<ReinforcePayload>(resolved->payload).position = search->result.position;
        }
        const auto& command = resolved ? *resolved : current;
        core::tick_profile::Scope command_scope(
            std::holds_alternative<MovePayload>(command.payload) ? "serial.commands.move"
            : std::holds_alternative<AttackPayload>(command.payload) ? "serial.commands.attack"
            : std::holds_alternative<ReinforcePayload>(command.payload) ? "serial.commands.reinforce"
            : "serial.commands.other");
        concentrate_command_key = command.key;
        // A command other than a move may read or change what the queued searches read.
        if (!std::holds_alternative<MovePayload>(command.payload) && !pending.empty()) {
            if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
        }
        const auto issuer = command.key.player_id;
        const bool departed = std::any_of(impl_->quits.begin(), impl_->quits.end(),
            [&](const PlayerQuit& quit) { return quit.player == issuer; });
        const bool quitting = std::holds_alternative<QuitPayload>(command.payload);
        const bool takeover = impl_->setup.departure_policy && !quitting
            && std::binary_search(impl_->basic_ai.begin(), impl_->basic_ai.end(), issuer);
        if ((departed && !takeover) || (quitting && std::any_of(pending_quits_.begin(), pending_quits_.end(),
                [&](const PlayerQuit& quit) { return quit.player == issuer; }))) {
            if (!silent) events.push_back(Event{tick, EventKind::order_rejected, issuer, command.key.sequence,
                invalid_entity_id, order_kind(command.payload), RejectReason::battle_decided});
            diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                command_context(command.key) + ": player has intentionally quit", {}, core::Severity::warning));
            continue;
        }
        if (quitting) {
            // WBF-48: notify in command order; deactivation/results processing is after end-frame.
            pending_quits_.push_back(PlayerQuit{issuer, tick});
            events.push_back(Event{tick, EventKind::player_quit, issuer, command.key.sequence});
            continue;
        }
        if (const auto* cinematic = std::get_if<RetreatCinematicPayload>(&command.payload)) {
            if (auto applied = lanes.apply_cinematic(command, cinematic, issuer); !applied) return applied;
            continue;
        }
        if (const auto* retreat = std::get_if<RetreatPayload>(&command.payload)) {
            if (auto applied = lanes.apply_retreat(command, retreat, issuer); !applied) return applied;
            continue;
        }
        if (const auto* special = std::get_if<SpecialFirePayload>(&command.payload)) {
            if (auto applied = handlers.special_fire(command, *special, issuer); !applied) return applied;
            continue;
        }
        if (const auto* beacon = std::get_if<BeaconPayload>(&command.payload)) {
            if (auto applied = lanes.apply_beacon(command, beacon, issuer); !applied) return applied;
            continue;
        }
        // #530: a buy, cancel or reinforce acts on the issuer's economy.
        if (const auto* reveal = std::get_if<RevealAllPayload>(&command.payload)) {
            if (auto applied = lanes.apply_reveal(command, reveal, issuer); !applied) return applied;
            continue;
        }
        if (const auto* repair = std::get_if<RepairHardpointPayload>(&command.payload)) {
            if (auto applied = handlers.repair_hardpoint(command, *repair, issuer); !applied) return applied;
            continue;
        }
        if (economy_command(command.payload)) {
            if (auto applied = handlers.economy(command); !applied) return applied;
            continue;
        }
        const auto* damage = std::get_if<DamagePayload>(&command.payload);
        auto command_reason = RejectReason::none;
        bool attack_assignment_allowed = true;
        // The unit an attack, attack-move or guard sends its units towards (#452).
        const auto approach = approach_target_of(command.payload);
        if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
            const auto target = staged.find(attack->target);
            if (target == staged.end()) {
                command_reason = RejectReason::target_not_live;
            } else if (!players_hostile(impl_->snapshot_players, issuer, target->second.state.owner)) {
                command_reason = RejectReason::target_not_hostile;
            } else if (attack->hardpoint != attack_hull
                && !impl_->target_hardpoint_standing(target->second, attack->hardpoint)) {
                command_reason = RejectReason::hardpoint_invalid;
            }
            if (target != staged.end()) {
                // WCC-25: direct assignment promotes a craft to its team, then refuses an
                // invalid type without replacing the held target. The outer order still
                // proceeds; collision and special-weapon admission belong to scans only. A squadron
                // team holds such a target, but its members take it only through that admission
                // (session_step_targets.cpp, #3922).
                const auto& membership = craft_prep_.value().craft_squadron.value();
                const auto member = membership.find(attack->target);
                const auto container = member != membership.end() ? staged.find(member->second) : target;
                const auto* target_profile = impl_->combat.find(
                    container != staged.end() ? container->second.state.type_id : target->second.state.type_id);
                attack_assignment_allowed = target_profile == nullptr || target_profile->valid_target;
            }
        } else if (approach != invalid_entity_id && staged.find(approach) == staged.end()) {
            command_reason = RejectReason::target_not_live;
        }
        std::size_t rejected = 0;
        auto first_reason = RejectReason::none;
        // FM-01: the tracked ships a move command accepts, in command order; two or more move
        // as a group once every unit of the command is accepted.
        std::vector<EntityId> group;
        // FO-07 (#552): the squadrons the command sends flying to a point, in command order.
        std::vector<EntityId> squadron_group;
        for (const auto unit_id : command.units) {
            auto reason = command_reason;
            const auto unit = staged.find(unit_id);
            const DurabilityProfile* profile = nullptr;
            if (reason == RejectReason::none) {
                if (unit == staged.end()) {
                    reason = RejectReason::unit_not_live;
                } else if (damage != nullptr) {
                    // Scripted damage (HD-30) may hit any live unit, whoever owns it.
                    profile = unit->second.durability ? impl_->health_profile(unit->second) : nullptr;
                    if (profile == nullptr) {
                        reason = RejectReason::not_damageable;
                    } else if (!damage_target_valid(*profile, damage->hardpoint)) {
                        reason = RejectReason::hardpoint_invalid;
                    }
                } else if (unit->second.state.owner != issuer) {
                    reason = RejectReason::unit_not_owned;
                } else if (arrivals.contains(unit_id)) {
                    reason = RejectReason::arriving; // #530 PU-39
                } else if (const auto* ability = std::get_if<AbilityPayload>(&command.payload)) {
                    auto applied = apply_ability(unit_id, *ability);
                    if (!applied) {
                        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                            command_context(command.key) + " unit " + std::to_string(unit_id) + ": "
                                + applied.error().message));
                    }
                    reason = applied.value();
                    if (reason == RejectReason::none && synthesised_command(command.key)
                        && ability->action == AbilityAction::activate && unit->second.abilities) {
                        const auto* ability_profile = impl_->abilities.find(unit->second.state.type_id);
                        const auto slot = ability_profile ? ability_slot(*ability_profile, ability->ability) : std::nullopt;
                        if (slot && unit->second.abilities->slots[*slot].active) autofire_switched.insert(unit_id);
                    }
                } else if (const auto* area = std::get_if<AreaAbilityPayload>(&command.payload)) {
                    auto applied = handlers.apply_area_ability(unit_id, *area);
                    if (!applied) return core::Result<void>::failure(applied.error());
                    reason = applied.value();
                } else if (const auto* manual = std::get_if<ManualTargetPayload>(&command.payload)) {
                    auto applied = handlers.apply_manual_target(unit_id, *manual, issuer);
                    if (!applied) return core::Result<void>::failure(applied.error());
                    reason = applied.value();
                } else if (approach != invalid_entity_id && unit_id == approach) {
                    reason = RejectReason::target_is_unit;
                } else if (const auto* attack = std::get_if<AttackPayload>(&command.payload);
                    attack != nullptr && impl_->hidden_from(staged, attack->target, unit->second, tick)) {
                    reason = RejectReason::target_hidden; // WST-11
                }
            }
            Event event{
                .tick = tick,
                .kind = EventKind::order_accepted,
                .player = issuer,
                .sequence = command.key.sequence,
                .unit = unit_id,
                .order = order_kind(command.payload),
                .reason = reason,
                .hardpoint = 0,
            };
            if (reason != RejectReason::none) {
                event.kind = EventKind::order_rejected;
                if (rejected++ == 0) {
                    first_reason = reason;
                }
                if (!silent && std::find(selected_units.begin(), selected_units.end(), unit_id) != selected_units.end()) events.push_back(event);
                continue;
            }
            if (!silent && std::find(selected_units.begin(), selected_units.end(), unit_id) != selected_units.end()) events.push_back(event);
            // An ability command acted above; it never becomes the unit's order (AB-10).
            if (std::holds_alternative<AbilityPayload>(command.payload)
                || std::holds_alternative<AreaAbilityPayload>(command.payload)
                || std::holds_alternative<ManualTargetPayload>(command.payload)) continue;
            if (damage == nullptr) {
                if (auto applied = command_orders.apply(command, selected_units, unit_id, unit->second,
                    approach, waypoint_resume, attack_assignment_allowed, group, squadron_group); !applied) return applied;
                continue;
            }
            if (auto applied = lanes.scripted_damage(command, damage, unit_id, profile); !applied) return applied;
        }
        if (!squadron_group.empty()) {
            if (auto spread = lanes.spread_squadrons(squadron_group); !spread) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    command_context(command.key) + ": " + spread.error().message));
            }
        }
        if (!group.empty()) {
            const auto planned = lanes.plan_group(command, group);
            if (!planned) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    command_context(command.key) + ": " + planned.error().message));
            }
        }
        if (rejected != 0) {
            diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                command_context(command.key) + ": " + std::to_string(rejected) + " of "
                    + std::to_string(command.units.size()) + " unit orders rejected (first: "
                    + std::string(to_string(first_reason)) + ")",
                {}, core::Severity::warning));
        }
    }

    return lanes.finish();
}

void session_detail::Tick::pay_bounty(const TypeId victim, const PlayerId owner, const PlayerId killer) {
    // WGCH-65/164, WGP-189: sparse ordered final-blow accounting. Visit retained
    // modifier sources only; there is no new per-entity periodic service.
    const auto& rules = impl_->economy;
    if (!rules.bounty_collection_enabled || !killer || killer == owner || impl_->allied(owner, killer)) return;
    const auto value = std::lower_bound(rules.bounty_values.begin(), rules.bounty_values.end(), victim,
        [](const auto& row, TypeId id) { return row.first < id; });
    if (value == rules.bounty_values.end() || value->first != victim || value->second <= math::Fixed{}) return;
    const auto player = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(),
        [&](const auto& row) { return row.player_id == owner; });
    if (player == impl_->setup.players.end() || !impl_->teams.contains(killer)) return;
    auto& accounts = staging_->ledgers.value();
    const auto& live = tracked_->staged.value();
    for (auto& recipient : accounts) {
        if (!impl_->allied(recipient.player, killer)) continue;
        std::map<std::int32_t, math::Fixed> categories;
        for (const auto& account : accounts) for (const auto& held : account.completed) {
            const auto* profile = rules.upgrade(held.type);
            if (!profile || profile->bounties.empty()) continue;
            const auto source = live.find(held.station);
            if (source == live.end()) continue;
            for (const auto& bounty : profile->bounties) {
                if (recipient.player != source->second.state.owner
                    && (!bounty.all_allies || !impl_->allied(recipient.player, source->second.state.owner))) continue;
                if (!std::binary_search(bounty.factions.begin(), bounty.factions.end(), player->faction_id)) continue;
                auto& category = categories[bounty.category];
                category = std::max(category, bounty.percentage);
            }
        }
        math::Fixed percentage;
        for (const auto& [category, contribution] : categories) {
            (void)category;
            const auto sum = math::add(percentage, contribution);
            if (!sum) { bounty_error_ = sum.error(); return; }
            percentage = sum.value();
        }
        if (percentage <= math::Fixed{}) continue;
        const auto scaled = math::multiply(value->second, percentage);
        if (!scaled) { bounty_error_ = scaled.error(); return; }
        auto integral = scaled.value().trunc_to_integer();
        if (scaled.value().raw() % math::Fixed::scale >= math::Fixed::scale / 2) ++integral;
        const auto payment = math::Fixed::from_integer(std::max<std::int64_t>(1, integral));
        if (!payment) { bounty_error_ = payment.error(); return; }
        const auto policy = rules.player_at(recipient.player, tick);
        if (!policy) continue;
        auto paid = change_credits(recipient, *policy, payment.value());
        if (!paid) { bounty_error_ = paid.error(); return; }
        const auto tally = math::add(recipient.collected_bounty, payment.value());
        if (!tally) { bounty_error_ = tally.error(); return; }
        recipient.collected_bounty = tally.value();
    }
}

Event session_detail::Tick::destruction_event(const std::uint64_t event_tick, const EventKind kind, const UnitState& unit,
    const std::uint32_t hardpoint, const PlayerId killer) {
    if (const auto* health = impl_->durability.find(unit.type_id)) {
        if (kind == EventKind::hardpoint_destroyed && hardpoint < health->hardpoints.size())
            set_named_hardpoint_gate(unit, health->hardpoints[hardpoint].special_ability_name, false);
        else if (kind == EventKind::unit_destroyed)
            for (const auto& point : health->hardpoints)
                if (!point.special_ability_name.empty()) set_named_hardpoint_gate(unit, point.special_ability_name, false);
        const bool affects_gate = kind == EventKind::hardpoint_destroyed
            ? hardpoint < health->hardpoints.size() && health->hardpoints[hardpoint].allows_special_weapon_use
            : kind == EventKind::unit_destroyed && std::any_of(health->hardpoints.begin(), health->hardpoints.end(),
                [](const HardpointProfile& entry) { return entry.allows_special_weapon_use; });
        if (affects_gate) set_special_hardpoint_gate(unit.owner, false);
    }
    if (kind == EventKind::unit_destroyed) {
        // WHE-41/50: sparse ordered destruction, members before their contained team.
        // These limbo identities were never admitted to the victory or spatial census.
        const auto destroy_rider = [&](auto&& self, const CarriedObject& rider) -> void {
            for (const auto& member : rider.members) self(self, member);
            if (!loss_ids_.insert(rider.entity_id).second) return;
            pay_bounty(rider.type_id, unit.owner, killer);
            loss_killers_.emplace(rider.entity_id, std::pair{pending_losses_.size() + 1U, killer});
            pending_losses_.push_back({unit.owner, rider.type_id, killer, 1, event_tick});
            impacts_.value().events.value().push_back(Event{event_tick, EventKind::unit_destroyed,
                unit.owner, 0, rider.entity_id});
        };
        for (const auto& rider : unit.contained) destroy_rider(destroy_rider, rider);
        track_victory_change(&unit, nullptr, true);
        if (loss_ids_.insert(unit.entity_id).second) {
            pay_bounty(unit.type_id, unit.owner, killer);
            loss_killers_.emplace(unit.entity_id, std::pair{pending_losses_.size() + 1U, killer});
            pending_losses_.push_back({unit.owner, unit.type_id, killer, 1, event_tick});
            if (impl_->setup.attract_pool) {
                // MAT-15: a craft is a team member unless it is its own container (a solo ship).
                const bool team_member = craft_prep_ && craft_prep_->craft_squadron
                    && [&] {
                        const auto container = craft_prep_->craft_squadron->find(unit.entity_id);
                        return container != craft_prep_->craft_squadron->end() && container->second != unit.entity_id;
                    }();
                note_pool_return(unit, impl_->garrison_marked(unit.entity_id), team_member);
            }
        }
    }
    return Event{
        .tick = event_tick,
        .kind = kind,
        .player = unit.owner,
        .sequence = 0,
        .unit = unit.entity_id,
        .order = OrderKind::none,
        .reason = RejectReason::none,
        .hardpoint = static_cast<std::uint8_t>(hardpoint),
    };
}

} // namespace eawr::sim::tactical
