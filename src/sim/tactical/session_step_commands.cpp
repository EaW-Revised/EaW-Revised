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
        if (effect.destroy_stations || effect.completed) for (auto at = staged.begin(); at != staged.end();) {
            const auto& unit = at->second;
            const bool station = std::binary_search(impl_->victory.starbase_types.begin(), impl_->victory.starbase_types.end(), unit.state.type_id);
            const bool escaped = std::find(pending_retreat_.actors.begin(), pending_retreat_.actors.end(), at->first) != pending_retreat_.actors.end();
            if (unit.state.owner != issuer || !(effect.destroy_stations ? station : !escaped)) { ++at; continue; }
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit.state));
            killed.push_back(unit.state); impl_->adjust_owned(unit.state, false);
            staged.erase(at); ++at;
        }
    }

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::retreat_service() {
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

    mark("staged_map_build", false);
    // Serial, in ascending projectile ID: each hit applies to the staged unit it reached (DG-01 to
    // DG-11). A unit killed by an earlier hit leaves at once; a later projectile that reached it
    // is spent without effect.
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::commands() {
    auto& reinforcement_work = gather_.value().reinforcement_work.value();
    auto& minds = craft_prep_.value().minds.value();
    auto& crafts = staging_.value().crafts.value();
    auto& arrivals = staging_.value().arrivals.value();
    auto& ledgers = staging_.value().ledgers.value();
    auto& pad_requests = staging_.value().pad_requests.value();
    auto& shares = staging_.value().shares.value();
    auto& arrived_squadrons = staging_.value().arrived_squadrons.value();
    auto& earners = staging_.value().earners.value();
    auto& next_id = staging_.value().next_id.value();
    const auto& world = targets_.value().world.value();
    const auto& combat_turns = targets_.value().combat_turns.value();
    const auto& approaches = orders_.value().approaches.value();
    const auto& damage_rules = flights_.value().damage_rules.value();
    auto& replans = tracked_.value().replans.value();
    const auto& frame = tracked_.value().frame.value();
    auto& rebuilt = tracked_.value().rebuilt.value();
    auto& tracking = tracked_.value().tracking.value();
    auto& staged = tracked_.value().staged.value();
    auto& events = impacts_.value().events.value();
    auto& killed = impacts_.value().killed.value();
    const auto& squadron_table = impl_->motion.squadrons;
    auto& pads = impl_->pad_stage.values;
    auto& construction = impl_->construction_stage.values;
    const auto& view = world.value();
    const auto& table = impl_->motion;
    // A plan submits the unit's new prediction: its layer rebuilds from this frame (AV-02,
    // AV-15). `clip`: a waiting group member's plan frame, the end of its prediction (FM-10).
    const auto submit = [&](const EntityId unit_id, const LiveUnit& live, const std::size_t layer,
                            const std::optional<std::uint64_t> clip) -> core::Result<void> {
        rebuilt[layer] = true;
        if (!tracking) return core::Result<void>::success();
        tracking->views[layer].reset();
        tracking->samples.erase(unit_id);
        if (live.motion->kind != MotionKind::none) {
            const auto yaw = yaw_degrees(live.state.rotation);
            if (!yaw) return core::Result<void>::failure(yaw.error());
            auto fresh = sample_windows(*live.motion, frame, tracking->interval, tracking->windows,
                live.state.position, yaw.value(), clip);
            if (!fresh) return core::Result<void>::failure(fresh.error());
            TrackSamples samples;
            samples.current = fresh.value();
            samples.fresh = std::move(fresh).value();
            tracking->samples.emplace(unit_id, std::move(samples));
        }
        return core::Result<void>::success();
    };
    // PC-09: `live` waits for `search`, which has ended: its layer predicts the search's plan
    // from the landing on (sample_ahead). The layer's windows keep their anchor: the landing,
    // not the search's end, is the unit's submission (PC-08), so when the search ends (which
    // depends on the slice size) changes only what a later search reads before the landing.
    const auto publish = [&](const EntityId unit_id, const LiveUnit& live, const Impl::SlicedSearch& search) -> core::Result<void> {
        if (!tracking) return core::Result<void>::success();
        const auto* footprint = tracked_footprint(table, live.state.type_id);
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        if (!layer) return core::Result<void>::success();
        tracking->views[*layer].reset();
        tracking->samples.erase(unit_id);
        const auto yaw = yaw_degrees(live.state.rotation);
        if (!yaw) return core::Result<void>::failure(yaw.error());
        const MotionState* current = live.motion && live.motion->kind != MotionKind::none ? &*live.motion : nullptr;
        const auto sample = [&](const std::uint64_t from) {
            return sample_ahead(current, search.frame, *search.plan, from, tracking->interval, tracking->windows,
                live.state.position, yaw.value());
        };
        const auto start = tracking->current_start[*layer];
        auto from_anchor = sample(start);
        if (!from_anchor) return core::Result<void>::failure(from_anchor.error());
        TrackSamples samples;
        samples.current = std::move(from_anchor).value();
        if (start != frame) {
            auto fresh = sample(frame);
            if (!fresh) return core::Result<void>::failure(fresh.error());
            samples.fresh = std::move(fresh).value();
        }
        tracking->samples.emplace(unit_id, std::move(samples));
        return core::Result<void>::success();
    };
    // The collision world a move in dynamic layer `layer` plans against (AV-01).
    const auto world_for = [&](const std::size_t layer, CollisionWorld& collisions) -> core::Result<void> {
        if (auto ensured = ensure_view(*tracking, table, staged, rebuilt, layer); !ensured) return ensured;
        if (auto statics = ensure_statics(*tracking, table, staged); !statics) return statics;
        collisions.interval = tracking->interval;
        collisions.layers[layer] = &*tracking->views[layer];
        collisions.statics = &*tracking->statics;
        return core::Result<void>::success();
    };
    const auto unit_failure = [&](const EntityId unit_id, const core::Diagnostic& error) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "tick " + std::to_string(tick) + " unit " + std::to_string(unit_id) + ": " + error.message));
    };

    // The tick's path searches (AV-15, FM-07, FM-12; PC-05, PC-07, PC-08): queued in their
    // planning order and planned when something else is about to read or change a layer (a
    // group's slot mapping, a face, stop or other plan, a non-move command) and after the
    // commands. A search reads only its own layer and the static layer (AV-01), so each dynamic
    // layer is a lane: its jobs run in order, each submitting before the next, and the lanes run
    // side by side, the next search of every lane together in the partitioned `plan-searches`
    // phase. That is exactly the serial result. A job is one of three:
    // - a sliced search landing this frame (`landing`): the unit takes its plan (PC-08);
    // - a search, while its layer has spent fewer expansions this tick than the avoidance rules'
    //   search_budget (PC-07): it may spend the rest of the budget, and a search that reaches it
    //   is given up and starts again sliced;
    // - once the budget is spent, a sliced search from the start: it lands search_delay frames
    //   later, and the unit keeps its current plan until then (PC-04), predicted up to that frame.
    // The first failed job in planning order answers.
    struct PlanJob {
        EntityId unit{};
        math::Vec3 destination{};
        std::optional<math::Fixed> max_speed;
        std::size_t layer{};
        CommandKey order{};
        std::uint32_t rank{};
        bool landing{};
    };
    std::vector<PlanJob> pending;
    std::array<std::uint64_t, 4> spent{};
    // PC-08: `job`'s search runs in slices from now on; its unit waits for the landing.
    const auto start_sliced = [&](const PlanJob& job) -> core::Result<void> {
        auto& live = staged.at(job.unit);
        CollisionWorld world;
        if (auto built = world_for(job.layer, world); !built) return built;
        const auto landing = frame + table.avoidance->search_delay;
        auto sliced = impl_->start_sliced(live, job.destination, job.max_speed, frame, landing, world);
        if (!sliced) return core::Result<void>::failure(sliced.error());
        // The wait's maximum speed is hashed only: a unit's own caps nothing it would not cap itself.
        auto max_speed = job.max_speed;
        if (!max_speed) max_speed = sliced.value().inputs.limits.max_speed;
        impl_->searches.insert_or_assign(job.unit, std::move(sliced).value());
        live.formation = FormationWait{landing, job.destination, *max_speed, job.order, job.rank, true};
        return submit(job.unit, live, job.layer, landing);
    };
    // PC-08: the unit takes its sliced search's plan, while it still plans from what the search
    // planned from; otherwise (a change the prediction could not see) it plans now.
    const auto land = [&](const PlanJob& job) -> core::Result<void> {
        auto& live = staged.at(job.unit);
        auto found = impl_->searches.find(job.unit);
        if (found == impl_->searches.end()) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure, "no sliced search to land"));
        }
        const auto inputs = impl_->path_inputs(live, found->second.max_speed);
        if (!inputs) return core::Result<void>::failure(inputs.error());
        if (inputs.value() == found->second.inputs && found->second.search.ended()) {
            auto plan = found->second.search.result();
            if (!plan) return core::Result<void>::failure(plan.error());
            if (found->second.search.exhausted()) {
                Impl::retain_path_failure(live, job.destination, frame, inputs.value().limits.max_speed);
            } else {
                Impl::take_plan(live, std::move(plan).value());
            }
        } else {
            CollisionWorld world;
            if (auto built = world_for(job.layer, world); !built) return built;
            if (auto planned = impl_->plan_path(live, found->second.target, frame, &world, found->second.max_speed, nullptr);
                !planned) {
                return planned;
            }
        }
        if (live.formation && live.formation->failure_frame) {
            live.formation->order = job.order;
            live.formation->rank = job.rank;
        }
        impl_->searches.erase(found);
        return submit(job.unit, live, job.layer, std::nullopt);
    };
    const auto plan_pending = [&]() -> std::optional<std::pair<EntityId, core::Diagnostic>> {
        const auto budget = table.avoidance->search_budget;
        std::array<std::vector<std::size_t>, 4> lanes;
        for (std::size_t index = 0; index < pending.size(); ++index) lanes[pending[index].layer].push_back(index);
        std::array<std::size_t, 4> next{};
        while (true) {
            std::vector<std::size_t> round;
            for (std::size_t layer = 0; layer < lanes.size(); ++layer) {
                if (next[layer] < lanes[layer].size()) round.push_back(lanes[layer][next[layer]++]);
            }
            if (round.empty()) break;
            std::sort(round.begin(), round.end());
            const std::size_t count = round.size();
            // Which jobs search now, each within the rest of its layer's budget.
            std::vector<char> searches(count);
            std::vector<std::size_t> searching;
            for (std::size_t index = 0; index < count; ++index) {
                const auto& job = pending[round[index]];
                searches[index] = !job.landing && spent[job.layer] < budget ? 1 : 0;
                if (searches[index] != 0) searching.push_back(index);
            }
            std::vector<CollisionWorld> worlds(count);
            std::vector<LiveUnit*> units(count);
            std::vector<PathSearchStats> work(count);
            std::vector<char> planned(count);
            std::vector<std::optional<core::Diagnostic>> errors(count);
            for (const auto index : searching) {
                const auto& job = pending[round[index]];
                units[index] = &staged.at(job.unit);
                if (auto built = world_for(job.layer, worlds[index]); !built) errors[index] = built.error();
            }
            const auto search = [&](const std::size_t index) {
                if (errors[index]) return;
                const auto& job = pending[round[index]];
                auto result = impl_->plan_path_within(*units[index], job.destination, frame, &worlds[index], job.max_speed,
                    &work[index], budget - spent[job.layer]);
                if (!result) {
                    errors[index] = result.error();
                    return;
                }
                planned[index] = result.value() ? 1 : 0;
            };
            if (searching.size() == 1) {
                search(searching.front());
            } else if (!searching.empty()) {
                const auto searched = executor.execute_phase("plan-searches", tick_partition_count, [&](const std::size_t partition) {
                    const auto range = partition_range(partition, searching.size());
                    for (auto index = range.begin; index < range.end; ++index) search(searching[index]);
                });
                if (!searched) return std::pair{pending[round[searching.front()]].unit, searched.error()};
            }
            for (std::size_t index = 0; index < count; ++index) {
                const auto& job = pending[round[index]];
                if (errors[index]) return std::pair{job.unit, std::move(*errors[index])};
                if (job.landing) {
                    if (auto landed = land(job); !landed) return std::pair{job.unit, landed.error()};
                    continue;
                }
                if (searches[index] != 0) {
                    spent[job.layer] += work[index].expansions;
                    if (planned[index] != 0) {
                        auto& live = *units[index];
                        if (live.formation && live.formation->failure_frame) {
                            live.formation->order = job.order;
                            live.formation->rank = job.rank;
                        }
                        if (auto submitted = submit(job.unit, *units[index], job.layer, std::nullopt); !submitted) {
                            return std::pair{job.unit, submitted.error()};
                        }
                        continue;
                    }
                }
                if (auto sliced = start_sliced(job); !sliced) return std::pair{job.unit, sliced.error()};
            }
        }
        pending.clear();
        return std::nullopt;
    };

    // FO-07 to FO-10 (#552, #599): squadrons one command sends to the same point take their
    // formation's slots around it, each at its own layer height, and its lanes. Serial, once per
    // command.
    const auto spread_squadrons = [&](const std::vector<EntityId>& squadrons) -> core::Result<void> {
        std::vector<GroupSquadron> members;
        members.reserve(squadrons.size());
        for (const auto id : squadrons) {
            const auto& state = minds.at(id);
            GroupSquadron member;
            member.position = staged.at(id).state.position;
            member.type = state.squadron_type;
            if (const auto* squadron = squadron_table.find_squadron(state.squadron_type);
                squadron != nullptr && !squadron->members.empty()) {
                if (const auto* craft = squadron_table.find_craft(squadron->members.front())) {
                    member.max_speed = craft->max_speed;
                    member.min_speed = craft->min_speed;
                    member.attack_distance = craft->attack_distance;
                }
                // FO-08: the farthest slot plus the craft's soft radius.
                math::Fixed farthest{};
                for (const auto& offset : squadron->offsets) {
                    auto reach = math::length(offset);
                    if (!reach) return core::Result<void>::failure(reach.error());
                    farthest = std::max(farthest, reach.value());
                }
                const auto* footprint = impl_->motion.footprint(squadron->members.front());
                member.radius = math::Fixed::from_raw(farthest.raw() + (footprint != nullptr ? footprint->radius.raw() : 0));
            }
            members.push_back(member);
        }
        // Every squadron of the command flies to the same point; the slots keep its x and y.
        const auto destination = minds.at(squadrons.front()).anchor;
        auto slots = squadron_group_slots(members, destination);
        if (!slots) return core::Result<void>::failure(slots.error());
        for (std::size_t index = 0; index < squadrons.size(); ++index) {
            auto& state = minds.at(squadrons[index]);
            const auto& slot = slots.value()[index];
            state.anchor = {slot.point.x, slot.point.y, state.anchor.z};
            if (slot.lane) {
                // FO-10: the squadron flies its formation's path and its move ends at the line
                // through its slot square to that path (FO-02).
                state.lane = *slot.lane;
                state.lane->formation = squadrons[static_cast<std::size_t>(slot.lane->formation)];
                state.move_origin = {math::Fixed::from_raw(state.anchor.x.raw() - slot.lane->direction.x.raw()),
                    math::Fixed::from_raw(state.anchor.y.raw() - slot.lane->direction.y.raw()), state.anchor.z};
            }
        }
        return core::Result<void>::success();
    };

    // FM-01 to FM-10: the tracked ships of one move command (in command order). One plans as a
    // single move; two or more move as a group: each gets its slot and planning speed, the
    // front ship of each layer plans now and the others wait, their predictions clipped.
    const auto plan_group = [&](const PlayerCommand& command, const std::vector<EntityId>& group) -> core::Result<void> {
        const auto destination = *point_move(command.payload);
        const auto layer_of = [&](const LiveUnit& live) {
            return *dynamic_layer_index(tracked_footprint(table, live.state.type_id)->layer);
        };
        if (group.size() == 1) {
            pending.push_back({group.front(), destination, std::nullopt, layer_of(staged.at(group.front())), command.key, 0,
                false});
            return core::Result<void>::success();
        }
        // The slot mapping reads the layers: the queued searches plan first.
        if (auto failed = plan_pending()) return core::Result<void>::failure(std::move(failed->second));
        std::vector<FormationMember> members;
        members.reserve(group.size());
        CollisionWorld mapping;
        for (const auto unit_id : group) {
            auto& live = staged.at(unit_id);
            const auto* footprint = tracked_footprint(table, live.state.type_id);
            const auto limits = impl_->limits_of(live);
            if (!limits) return core::Result<void>::failure(limits.error());
            if (limits.value().max_speed.raw() <= 0) {
                // FM-09a: no slot for a ship with no speed. Its order is accepted (MV-03) and
                // its old path cleared, so it stays as a stationary obstacle.
                if (auto planned = impl_->plan_path(live, destination, frame, nullptr, math::Fixed{}, nullptr); !planned) {
                    return planned;
                }
                if (auto submitted = submit(unit_id, live, layer_of(live), std::nullopt); !submitted) {
                    return submitted;
                }
                continue;
            }
            const auto yaw = yaw_degrees(live.state.rotation);
            if (!yaw) return core::Result<void>::failure(yaw.error());
            const auto radius = occupation_radius(
                *footprint, limits.value().max_speed, limits.value().rate_of_turn, *table.avoidance);
            if (!radius) return core::Result<void>::failure(radius.error());
            members.push_back(FormationMember{unit_id, live.state.position, yaw.value(), footprint->layer,
                radius.value(), limits.value().max_speed, limits.value().rate_of_turn, footprint->radius,
                footprint->asteroid_damage, live.state.order.through_hazards});
            if (auto built = world_for(layer_of(live), mapping); !built) return built;
        }
        const auto slots = map_group_move(members, destination, mapping, frame, *table.avoidance);
        if (!slots) return core::Result<void>::failure(slots.error());
        for (std::size_t rank = 0; rank < slots.value().size(); ++rank) {
            const auto& slot = slots.value()[rank];
            if (slot.delay == 0) continue;
            auto& live = staged.at(slot.entity);
            live.formation = FormationWait{frame + slot.delay, slot.destination, slot.max_speed, command.key,
                static_cast<std::uint32_t>(rank), false};
            if (auto submitted = submit(slot.entity, live, layer_of(live), live.formation->frame); !submitted) {
                return submitted;
            }
        }
        for (std::size_t rank = 0; rank < slots.value().size(); ++rank) {
            const auto& slot = slots.value()[rank];
            if (slot.delay != 0) continue;
            pending.push_back({slot.entity, slot.destination, slot.max_speed, layer_of(staged.at(slot.entity)), command.key,
                static_cast<std::uint32_t>(rank), false});
        }
        return core::Result<void>::success();
    };

    // A-04: a unit that turns toward its ordered target plans a turn in place as a face order of
    // this tick would, before this tick's commands (which may replace it). A unit a hit
    // destroyed this tick is gone.
    for (const auto& [unit_id, point] : combat_turns) {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) continue;
        auto& live = found->second;
        if (auto planned = impl_->plan_order(live, FacePayload{point}, frame); !planned) {
            return unit_failure(unit_id, planned.error());
        }
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        if (layer && live.motion) {
            if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) {
                return unit_failure(unit_id, submitted.error());
            }
        }
    }

    // OR-05, OR-06: a unit plans towards the approach slot the orders phase mapped, serially in
    // ascending ID. A unit a hit destroyed this tick is gone.
    // Plans `live` towards the slot from this frame and keeps the mapping's prediction frame. A
    // tracked unit's search is a job of its lane (PC-07, PC-08), queued in the same order.
    const auto plan_approach = [&](const EntityId unit_id, LiveUnit& live, const Impl::ApproachPlan& plan) -> core::Result<void> {
        if (live.formation && live.formation->failure_frame) return core::Result<void>::success();
        live.formation.reset();
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        std::optional<std::size_t> layer;
        if (footprint != nullptr) layer = dynamic_layer_index(footprint->layer);
        live.approach = Approach{plan.prediction_frame};
        if (tracking && layer) {
            pending.push_back({unit_id, plan.slot, std::nullopt, layer.value_or(0), CommandKey{}, 0, false});
            return core::Result<void>::success();
        }
        if (auto planned = impl_->plan_path(live, plan.slot, frame, nullptr, std::nullopt, nullptr); !planned) return planned;
        if (layer && live.motion) return submit(unit_id, live, *layer, std::nullopt);
        return core::Result<void>::success();
    };
    for (const auto& [unit_id, plan] : approaches) {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) continue;
        if (auto planned = plan_approach(unit_id, found->second, plan); !planned) {
            return unit_failure(unit_id, planned.error());
        }
    }

    // PC-08: every sliced search runs one slice, side by side in the `plan-searches` phase; a
    // search that lands this frame runs to its end.
    if (!impl_->searches.empty()) {
        std::vector<Impl::SlicedSearch*> running;
        for (auto& entry : impl_->searches) running.push_back(&entry.second);
        const auto slice = table.avoidance->search_slice;
        const auto sliced = executor.execute_phase("plan-searches", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, running.size());
            for (auto index = range.begin; index < range.end; ++index) {
                auto& search = *running[index];
                static_cast<void>(search.search.run(search.frame <= frame ? std::numeric_limits<std::uint64_t>::max() : slice));
            }
        });
        if (!sliced) return core::Result<void>::failure(sliced.error());
        // PC-09 (#613): a search that has ended publishes its plan to its unit's layer at once,
        // as FoC's plan would be there from the frame it was made (PC-02); its unit still takes
        // it at the landing. In ascending unit ID.
        for (auto& [unit_id, search] : impl_->searches) {
            if (search.plan || !search.search.ended()) continue;
            const auto found = staged.find(unit_id);
            if (found == staged.end() || !found->second.formation || !found->second.formation->sliced) continue;
            auto plan = search.search.result();
            if (!plan) continue; // the landing reports it
            search.plan = std::move(plan).value();
            if (auto published = publish(unit_id, found->second, search); !published) {
                return unit_failure(unit_id, published.error());
            }
        }
    }

    // FM-08, FM-10, PC-08: waiting ships whose plan is due (staggered group members, and sliced
    // searches that land) plan first, in the order of their commands and then each group's
    // planning order.
    if (tracking) {
        std::vector<std::tuple<CommandKey, std::uint32_t, EntityId>> due;
        for (const auto& [id, live] : staged) {
            if (live.formation && live.formation->frame <= frame) due.emplace_back(live.formation->order, live.formation->rank, id);
        }
        std::sort(due.begin(), due.end());
        for (const auto& entry : due) {
            auto& live = staged.at(std::get<2>(entry));
            const auto wait = *live.formation;
            live.formation.reset();
            const auto sliced = wait.sliced ? impl_->searches.find(std::get<2>(entry)) : impl_->searches.end();
            const bool landing = sliced != impl_->searches.end() && sliced->second.frame == frame;
            pending.push_back({std::get<2>(entry), wait.destination, wait.max_speed,
                *dynamic_layer_index(tracked_footprint(table, live.state.type_id)->layer), wait.order, wait.rank, landing});
        }
    }

    // AB-24: a ship whose speed multiplier changed plans its move again from this frame, towards
    // the same point, as a move order would (a group member still waiting keeps its wait).
    const auto replan = [&](const EntityId unit_id) -> core::Result<void> {
        const auto found = staged.find(unit_id);
        if (found == staged.end()) return core::Result<void>::success();
        auto& live = found->second;
        if (!live.motion || live.motion->kind != MotionKind::path || live.formation) return core::Result<void>::success();
        const auto target = live.motion->target;
        const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
        const auto layer = footprint != nullptr ? dynamic_layer_index(footprint->layer) : std::nullopt;
        const bool layered = layer.has_value();
        const auto layer_index = layer.value_or(0);
        // A tracked unit's search is a job of its lane (PC-07, PC-08).
        if (tracking && layered) {
            pending.push_back({unit_id, target, std::nullopt, layer_index, CommandKey{}, 0, false});
            return core::Result<void>::success();
        }
        if (auto planned = impl_->plan_path(live, target, frame, nullptr, std::nullopt, nullptr); !planned) return planned;
        if (layered) return submit(unit_id, live, layer_index, std::nullopt);
        return core::Result<void>::success();
    };
    // The replans read the layers and the due waits' new plans: the queued searches plan first.
    if (!replans.empty() && !pending.empty()) {
        if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
    }
    for (const auto unit_id : replans) {
        if (auto planned = replan(unit_id); !planned) return unit_failure(unit_id, planned.error());
    }

    std::deque<PlayerCommand> concentrate_batches;
    std::vector<SpaceBody> concentrate_new_bodies;
    CommandKey concentrate_command_key{};
    const auto ordinary_ability_holders = [&](const EntityId unit_id, const AbilityKind kind) {
        std::vector<LiveUnit*> holders;
        const auto add = [&](const EntityId id) {
            const auto found = staged.find(id);
            if (found == staged.end() || !found->second.abilities) return;
            const auto* profile = impl_->abilities.find(found->second.state.type_id);
            if (profile && ability_slot(*profile, kind)) holders.push_back(&found->second);
        };
        const auto squadron = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
            [unit_id](const Squadron& entry) { return entry.container == unit_id; });
        if (squadron == impl_->squadrons.end()) add(unit_id);
        else for (const auto member : squadron->members) add(member);
        return holders;
    };
    // An ability command on one listed unit (AB-10 to AB-15, AB-40): the unit itself, or for a
    // squadron container its live craft that have the ability. Every holder must be ready and
    // off for an activation to act (AB-15); otherwise nothing changes.
    const auto apply_ability = [&](const EntityId unit_id, const AbilityPayload& payload) -> core::Result<RejectReason> {
        if (payload.ability == AbilityKind::blast) {
            auto& source = staged.at(unit_id);
            const auto* profile = impl_->abilities.find(source.state.type_id);
            const auto index = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
            if (!index || !source.abilities) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto& ability = profile->abilities[*index];
            const auto nested = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& special) {
                return special.kind == SpecialAbilityKind::blast && special.name == ability.gui_activated_ability_name;
            });
            if (nested == profile->special.end() || !ability.charged_blast)
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
            auto& handler = source.abilities->special.slots[static_cast<std::size_t>(nested - profile->special.begin())];
            auto& slot = source.abilities->slots[*index];
            if (payload.action != AbilityAction::activate || nested->initially_enabled == false
                || source.abilities->special.service_cancelled || handler.cancelled
                || !ability_ready(ability, slot, impl_->ability_gate(source, tick), tick))
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            if (!activate_ability(ability, slot, impl_->ability_gate(source, tick), tick).changed)
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            // WHE-35, FC-62: instant initiation starts recharge independently of the handler budget.
            slot.active = false;
            source.abilities->charged_blast = ChargedBlastState{ability.charged_blast->charge_frames, 0, false};
            handler.enabled = true;
            handler.next_service_frame = tick + 1; // commands follow this tick's pre-fire poll
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        if (payload.ability == AbilityKind::corrupt_systems) {
            auto& source = staged.at(unit_id);
            const auto* profile = impl_->abilities.find(source.state.type_id);
            const auto index = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
            if (!index || !source.abilities) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto& ability = profile->abilities[*index];
            const auto nested = std::find_if(profile->special.begin(), profile->special.end(), [&](const auto& special) {
                return special.kind == SpecialAbilityKind::corrupt_systems && special.name == ability.gui_activated_ability_name;
            });
            if (nested == profile->special.end()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            auto& slot = source.abilities->slots[*index];
            if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
            if (payload.action != AbilityAction::activate || !payload.position
                || nested->initially_enabled == false || nested->style != SpecialActivationStyle::user_input
                || !ability_ready(ability, slot, impl_->ability_gate(source, tick), tick))
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto point = *payload.position;
            if (!within_range(source.state.position, point, nested->corrupt_max_range, RangeMetric::planar)
                || (nested->corrupt_min_range.raw() > 0 && within_range(source.state.position, point,
                    nested->corrupt_min_range, RangeMetric::planar)))
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            // WHE-33: query existing enemy collidable trees, then stage eligibility in partitions.
            const auto radius = ability.effective_radius;
            const auto axis = [](const math::Fixed coordinate, const math::Fixed offset) {
                return math::Fixed::from_raw(coordinate.raw() + offset.raw());
            };
            const auto negative = math::Fixed::from_raw(-radius.raw());
            const detail::CullBox box{{axis(point.x, negative), axis(point.y, negative), math::Fixed::from_raw(std::numeric_limits<std::int64_t>::min())},
                {axis(point.x, radius), axis(point.y, radius), math::Fixed::from_raw(std::numeric_limits<std::int64_t>::max())}};
            std::vector<EntityId> candidates;
            for (const auto& player : impl_->snapshot_players) {
                if (!players_hostile(impl_->snapshot_players, source.state.owner, player.player_id)) continue;
                const auto found = targets_->collection->collect(player.player_id, box);
                candidates.insert(candidates.end(), found.begin(), found.end());
            }
            if (!concentrate_new_bodies.empty()) {
                const auto added = SpaceIndex::build(concentrate_new_bodies);
                if (!added) return core::Result<RejectReason>::failure(added.error());
                std::vector<std::uint32_t> positions;
                added.value().box_positions(point, {radius, radius, math::Fixed::from_raw(max_motion_coordinate * math::Fixed::scale)}, positions);
                for (const auto position : positions) candidates.push_back(added.value().bodies()[position].entity_id);
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            std::vector<std::uint8_t> selected(candidates.size());
            std::vector<std::optional<core::Diagnostic>> recipient_errors(candidates.size());
            auto& corrupt_work = gather_->tick_work.value();
            ++corrupt_work.corrupt_queries;
            corrupt_work.corrupt_candidates += candidates.size();
            const auto gathered = executor.execute_phase("corrupt-recipients", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, candidates.size());
                for (auto i = range.begin; i < range.end; ++i) {
                    const auto found = staged.find(candidates[i]);
                    if (found == staged.end() || candidates[i] == unit_id) continue;
                    const auto& recipient = found->second;
                    if (!players_hostile(impl_->snapshot_players, source.state.owner, recipient.state.owner)) continue;
                    const auto* combat = impl_->combat.find(recipient.state.type_id);
                    if (!combat || !combat->living_projectile_collision
                        || (!impl_->motion.find(recipient.state.type_id) && !impl_->motion.squadrons.find_craft(recipient.state.type_id))
                        || arrivals.contains(candidates[i]) || (recipient.durability && recipient.durability->hull.raw() <= 0)) continue;
                    if (!within_range(point, recipient.state.position, radius, RangeMetric::planar)) {
                        // WHE-69: outside centers may still overlap by their transformed collision box.
                        detail::CombatUnit geometry;
                        geometry.profile = combat;
                        const auto transform = unit_transform(recipient);
                        if (!transform) { recipient_errors[i] = transform.error(); continue; }
                        geometry.transform = transform.value();
                        const auto overlap = detail::collision_overlaps_sphere(geometry, point, radius);
                        if (!overlap) { recipient_errors[i] = overlap.error(); continue; }
                        if (!overlap.value()) continue;
                    }
                    selected[i] = 1;
                }
            });
            if (!gathered) return core::Result<RejectReason>::failure(gathered.error());
            for (const auto& error : recipient_errors) if (error) return core::Result<RejectReason>::failure(*error);
            for (std::size_t i = 0; i < candidates.size(); ++i) if (selected[i]) {
                ++corrupt_work.corrupt_recipients;
                staged.at(candidates[i]).corrupt = CorruptSystemsState{source.state.type_id, tick,
                    tick + nested->corrupt_duration_frames, nested->corrupt_drain_per_frame, nested->corrupt_inaccuracy};
            }
            slot.started_tick = tick;
            slot.ready_tick = tick + ability.recharge_frames;
            slot.position = point;
            source.state.order = order_for(StopPayload{}, tick);
            if (source.motion) {
                if (auto stopped = impl_->plan_order(source, StopPayload{}, frame); !stopped)
                    return core::Result<RejectReason>::failure(stopped.error());
                if (const auto* footprint = table.avoidance ? tracked_footprint(table, source.state.type_id) : nullptr) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        if (auto submitted = submit(unit_id, source, *layer, std::nullopt); !submitted)
                            return core::Result<RejectReason>::failure(submitted.error());
                    }
                }
            }
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        if (payload.ability == AbilityKind::replenish_wingmen) {
            LiveUnit* source = &staged.at(unit_id);
            if (const auto group = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
                    [unit_id](const Squadron& entry) { return entry.container == unit_id; }); group != impl_->squadrons.end()) {
                source = nullptr;
                for (const auto member : group->members) {
                    const auto found = staged.find(member);
                    const auto* profile = found != staged.end() ? impl_->abilities.find(found->second.state.type_id) : nullptr;
                    if (profile && ability_slot(*profile, payload.ability)) { source = &found->second; break; }
                }
            }
            const auto* profile = source ? impl_->abilities.find(source->state.type_id) : nullptr;
            const auto slot = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
            if (!source || !source->abilities || !slot) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto& ability = profile->abilities[*slot];
            auto& state = source->abilities->slots[*slot];
            if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
                if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                state.autofire = payload.action == AbilityAction::autofire_on;
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            if (payload.action != AbilityAction::activate || !ability_ready(ability, state, impl_->ability_gate(*source, tick), tick)
                || !squadron_table.find_squadron(ability.replenish_team))
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
                staging_.value().born_spawners, earners, next_id,
                pads, construction, pad_requests, impl_->pad_stage, impl_->construction_stage,
                pending_economy_cues_, &reinforcement_work};
            const auto leader_id = source->state.entity_id;
            const auto first_added = next_id;
            auto filled = impl_->replenish_wingmen(leader_id, ability.replenish_team, staged, stage, tick);
            if (!filled) return core::Result<RejectReason>::failure(filled.error());
            sync_radar_creations(staged, first_added, next_id);
            if (filled.value()) {
                auto& current = staged.at(leader_id).abilities->slots[*slot];
                current.active = false;
                current.started_tick = tick;
                current.ready_tick = tick + ability.recharge_frames;
                for (const auto& group : arrived_squadrons)
                    if (std::find(group.members.begin(), group.members.end(), leader_id) != group.members.end())
                        for (const auto member : group.members) craft_prep_->craft_squadron.value()[member] = group.container;
            }
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        if (payload.ability == AbilityKind::harmonic_bomb || payload.ability == AbilityKind::weaken_enemy
            || payload.ability == AbilityKind::cluster_bomb || payload.ability == AbilityKind::fow_reveal_ping) {
            // AB-15/WFP-02: ordinary spawned abilities act on the container's
            // eligible craft. Admit every holder before committing any spawn.
            const auto holders = ordinary_ability_holders(unit_id, payload.ability);
            if (holders.empty() || !impl_->durability.damage)
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            for (const auto* holder : holders) {
                const auto& profile = *impl_->abilities.find(holder->state.type_id);
                const auto slot = *ability_slot(profile, payload.ability);
                const auto& ability = profile.abilities[slot];
                if (!ability.spawned) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
                    if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                } else if (payload.action == AbilityAction::activate) {
                    if (holder->abilities->slots[slot].active
                        || !ability_ready(ability, holder->abilities->slots[slot], impl_->ability_gate(*holder, tick), tick))
                        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                    if (payload.ability == AbilityKind::cluster_bomb && payload.target != invalid_entity_id && payload.target != unit_id)
                        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                    if ((payload.ability == AbilityKind::fow_reveal_ping || payload.ability == AbilityKind::weaken_enemy)
                        && !payload.position) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                    const auto position = payload.position && *payload.position != math::Vec3{}
                        ? *payload.position : holder->state.position;
                    if (payload.ability == AbilityKind::weaken_enemy && ability.spawned->reach.raw() > 0
                        && !within_range(holder->state.position, position, ability.spawned->reach, RangeMetric::planar))
                        return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                }
            }
            if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
            for (auto* holder : holders) {
                const auto& profile = *impl_->abilities.find(holder->state.type_id);
                const auto slot = *ability_slot(profile, payload.ability);
                const auto& ability = profile.abilities[slot];
                auto& state = holder->abilities->slots[slot];
                if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
                    // FS-29/FSC-46: only an admitted object-script turn observes this toggle.
                    state.autofire = payload.action == AbilityAction::autofire_on;
                    continue;
                }
                auto position = holder->state.position;
                if ((payload.ability == AbilityKind::fow_reveal_ping || payload.ability == AbilityKind::weaken_enemy)
                    && *payload.position != math::Vec3{}) position = *payload.position;
                spawn_instant_ability(*holder, payload.ability, *ability.spawned, position);
                state.started_tick = tick;
                state.ready_tick = tick + ability.recharge_frames;
            }
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        if (payload.ability == AbilityKind::energy_weapon || payload.ability == AbilityKind::tractor_beam
            || payload.ability == AbilityKind::leech_shields) {
            auto& source = staged.at(unit_id);
            const auto* nested = impl_->beam_profile(source, payload.ability);
            if (!source.abilities || !nested) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto& profile = *impl_->abilities.find(source.state.type_id);
            const auto slot = *ability_slot(profile, payload.ability);
            auto& state = source.abilities->slots[slot];
            const auto& ability = profile.abilities[slot];
            if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
                if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                state.autofire = payload.action == AbilityAction::autofire_on;
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            const auto previous = state.target;
            if (payload.action == AbilityAction::deactivate) {
                impl_->release_beam(source, payload.ability, tick);
                if (payload.ability == AbilityKind::tractor_beam) impl_->register_tractor(source, nullptr);
            } else {
                if (!impl_->beam_usable(source, payload.ability, tick))
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                const auto target = staged.find(payload.target);
                if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
                if (!impl_->beam_target_valid(source, target->second, *nested) || !impl_->beam_in_range(source, target->second, *nested))
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                if (state.active && payload.ability == AbilityKind::energy_weapon)
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                // WHE-60: retarget unlocks the old recipient before installing the new source entry.
                if (state.active) {
                    impl_->release_beam(source, payload.ability, tick);
                    state.ready_tick = tick;
                }
                static_cast<void>(impl_->lock_beam(source, payload.target, payload.ability, tick));
                if (payload.ability == AbilityKind::energy_weapon) {
                    state.target_hardpoint = payload.target_hardpoint != no_hardpoint
                            && impl_->target_hardpoint_standing(target->second, payload.target_hardpoint)
                        ? payload.target_hardpoint : impl_->energy_hardpoint(source, target->second);
                }
                if (payload.ability == AbilityKind::tractor_beam) {
                    target->second.formation.reset();
                    impl_->register_tractor(source, &target->second);
                }
            }
            if (payload.ability == AbilityKind::tractor_beam) {
                std::map<EntityId, TypeId> containers;
                for (const auto& account : ledgers) for (const auto& held : account.completed)
                    if (const auto found = staged.find(held.station); found != staged.end())
                        containers.emplace(held.station, found->second.state.type_id);
                std::vector<CombatBonuses> categories(impl_->bonus_categories.size());
                for (const auto id : {previous, state.target}) if (const auto target = staged.find(id); target != staged.end()) {
                    if (auto applied = impl_->apply_bonuses(target->second, impl_->command_bonuses_for(target->second,
                        impl_->command_ledger, ledgers, containers, categories, &staged), Impl::BonusAdjustment::loss); !applied)
                        return core::Result<RejectReason>::failure(applied.error());
                    if (auto planned = replan(id); !planned) return core::Result<RejectReason>::failure(planned.error());
                }
            }
            if (payload.ability == AbilityKind::leech_shields) {
                std::map<EntityId, TypeId> containers;
                for (const auto& account : ledgers) for (const auto& held : account.completed)
                    if (const auto found = staged.find(held.station); found != staged.end())
                        containers.emplace(held.station, found->second.state.type_id);
                std::vector<CombatBonuses> categories(impl_->bonus_categories.size());
                if (auto applied = impl_->apply_bonuses(source, impl_->command_bonuses_for(source,
                    impl_->command_ledger, ledgers, containers, categories, &staged), Impl::BonusAdjustment::loss); !applied)
                    return core::Result<RejectReason>::failure(applied.error());
            }
            if (auto planned = replan(unit_id); !planned) return core::Result<RejectReason>::failure(planned.error());
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        if (payload.ability == AbilityKind::concentrate_fire) {
            auto& source = staged.at(unit_id);
            const auto* profile = impl_->abilities.find(source.state.type_id);
            const auto ordinary = profile ? ability_slot(*profile, payload.ability) : std::nullopt;
            const auto* nested = impl_->concentrate_profile(source);
            if (!source.abilities || !ordinary || nested == nullptr || source.abilities->special.service_cancelled) {
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
            const auto special_index = static_cast<std::size_t>(nested - profile->special.data());
            auto& special = source.abilities->special.slots[special_index];
            auto& state = source.abilities->slots[*ordinary];
            const auto& ability = profile->abilities[*ordinary];
            // AB-40, WAB-36: the autofire flag the attack-state pass reads, only where Supports_Autofire allows it.
            if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
                if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                state.autofire = payload.action == AbilityAction::autofire_on;
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            if (payload.action == AbilityAction::deactivate) {
                static_cast<void>(deactivate_ability(ability, state, tick));
                state.target = invalid_entity_id;
                state.target_hardpoint = no_hardpoint;
                source.abilities->concentrate_recruits.clear();
                special.targets.clear();
                special.context.reset();
                impl_->register_concentrate(source);
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            if (payload.action != AbilityAction::activate || state.active || special.cancelled || source.abilities->special.service_cancelled
                || nested->initially_enabled == false || nested->style != SpecialActivationStyle::user_input
                || !ability_ready(ability, state, impl_->ability_gate(source, tick), tick)) {
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
            const auto target = staged.find(payload.target);
            if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
            if (!players_hostile(impl_->snapshot_players, source.state.owner, target->second.state.owner)) {
                return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
            }
            const auto* target_profile = impl_->combat.find(target->second.state.type_id);
            if (target_profile == nullptr || !special_type_matches(nested->filter, target->second.state.type_id,
                target_profile->category_bits)) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            if (payload.target_hardpoint != no_hardpoint && !impl_->target_hardpoint_standing(target->second, payload.target_hardpoint)) {
                return core::Result<RejectReason>::success(RejectReason::hardpoint_invalid);
            }
            // WHE-24: one target-centred spatial query, then partitioned eligibility and team promotion.
            std::vector<std::uint32_t> candidates;
            std::uint64_t inspected = 0;
            view.index.box_positions(target->second.state.position,
                {ability.effective_radius, ability.effective_radius, ability.effective_radius}, candidates, &inspected);
            std::vector<EntityId> candidate_ids;
            candidate_ids.reserve(candidates.size() + concentrate_new_bodies.size());
            for (const auto position : candidates) candidate_ids.push_back(view.index.bodies()[position].entity_id);
            if (!concentrate_new_bodies.empty()) {
                const auto added_index = SpaceIndex::build(concentrate_new_bodies);
                if (!added_index) return core::Result<RejectReason>::failure(added_index.error());
                added_index.value().box_positions(target->second.state.position,
                    {ability.effective_radius, ability.effective_radius, ability.effective_radius}, candidates, &inspected);
                for (const auto position : candidates) candidate_ids.push_back(added_index.value().bodies()[position].entity_id);
            }
            auto& work = gather_->tick_work.value();
            ++work.concentrate_queries;
            work.concentrate_candidates += inspected;
            std::vector<EntityId> recruited(candidate_ids.size());
            std::vector<std::uint8_t> own_targeting(candidate_ids.size());
            const auto selected = executor.execute_phase("concentrate-recruit", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, candidate_ids.size());
                for (auto i = range.begin; i < range.end; ++i) {
                    const auto id = candidate_ids[i];
                    const auto found = staged.find(id);
                    if (found == staged.end() || found->second.state.owner != source.state.owner
                        || !found->second.combat || !within_range(target->second.state.position, found->second.state.position,
                            ability.effective_radius, RangeMetric::spatial)) continue;
                    own_targeting[i] = 1;
                    if (id == unit_id) continue;
                    const auto team = craft_prep_->craft_squadron.value().find(id);
                    const auto promoted = team == craft_prep_->craft_squadron.value().end() ? id : team->second;
                    const auto parent = staged.find(promoted);
                    if (parent == staged.end() || parent->second.state.owner != source.state.owner || promoted == unit_id
                        || arrivals.contains(promoted)) continue;
                    recruited[i] = promoted;
                }
            });
            if (!selected) return core::Result<RejectReason>::failure(selected.error());
            // WHE-24: an empty same-owner targeting query releases before starting a timer.
            if (std::none_of(own_targeting.begin(), own_targeting.end(), [](const auto match) { return match != 0; })) {
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            std::erase(recruited, invalid_entity_id);
            std::sort(recruited.begin(), recruited.end());
            recruited.erase(std::unique(recruited.begin(), recruited.end()), recruited.end());
            source.abilities->concentrate_recruits = recruited;
            if (!recruited.empty()) {
                concentrate_batches.push_back({concentrate_command_key, std::move(recruited), AttackPayload{payload.target, payload.target_hardpoint}});
                ++work.concentrate_batches;
            }
            // Start the ordinary expiry only after the recruitment transaction succeeded.
            static_cast<void>(activate_ability(ability, state, impl_->ability_gate(source, tick), tick));
            state.target = payload.target;
            state.target_hardpoint = payload.target_hardpoint;
            special.enabled = true;
            special.targets = {payload.target};
            special.next_service_frame = tick + nested->service_interval;
            impl_->register_concentrate(source);
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        if (payload.ability == AbilityKind::barrage && payload.action == AbilityAction::activate) {
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        // WAB-70/71: BUZZ_DROIDS acts on its holder (a squadron's team container, AB-60) at a point.
        if (payload.ability == AbilityKind::buzz_droids) {
            auto& source = staged.at(unit_id);
            const auto* profile = impl_->abilities.find(source.state.type_id);
            const auto index = profile != nullptr ? ability_slot(*profile, AbilityKind::buzz_droids) : std::nullopt;
            if (!index || !source.abilities || !profile->abilities[*index].buzz)
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            const auto& ability = profile->abilities[*index];
            const auto& buzz = *ability.buzz;
            auto& slot = source.abilities->slots[*index];
            if (payload.action == AbilityAction::deactivate) return core::Result<RejectReason>::success(RejectReason::none);
            if (payload.action == AbilityAction::autofire_on || payload.action == AbilityAction::autofire_off) {
                if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                slot.autofire = payload.action == AbilityAction::autofire_on;
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            if (!payload.position || !ability_ready(ability, slot, impl_->ability_gate(source, tick), tick)
                || next_id == invalid_entity_id || impl_->durability.find(buzz.droid_type) == nullptr)
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            // WAB-70: the ordinary attack-range test against the point: planar, inclusive of the
            // authored maximum and exclusive of the minimum; a point has no extent.
            const auto point = *payload.position;
            // WAB-70/UFS-21: keep the admission refusal at or inside the minimum;
            // FS-23/FC-51 establish retries for admitted requests, not this admission.
            if (buzz.min_range.raw() > 0 && within_range(source.state.position, point, buzz.min_range, RangeMetric::planar))
                return core::Result<RejectReason>::success(RejectReason::invalid_position);
            if (const auto mind = minds.find(unit_id); mind != minds.end()) {
                // FS-23/FD-32: the admitted team event retains one special attack and gives
                // its holder the coordinated approach destination. FC-51 retries before scans.
                source.abilities->pending_buzz_position = point;
                source.state.order = order_for(MovePayload{point}, tick);
                auto& team = minds.at(unit_id);
                const auto target = team.target;
                const auto hardpoint = team.target_hardpoint;
                const auto formation = team.formation;
                apply_squadron_order(team, MovePayload{point}, source.state.position, squadron_table);
                team.target = target;
                team.target_hardpoint = hardpoint;
                // FS-23: the special point does not replace the ordinary attack target.
                team.formation->base_target = formation ? formation->base_target : invalid_entity_id;
                team.formation->attack_target = formation ? formation->attack_target : target;
                team.formation->attack_hardpoint = formation ? formation->attack_hardpoint : hardpoint;
                team.approach = false;
                return core::Result<RejectReason>::success(RejectReason::none);
            }
            if (!within_range(source.state.position, point, buzz.max_range, RangeMetric::planar))
                return core::Result<RejectReason>::success(RejectReason::invalid_position);
            // WAB-71: the holder stops, starts its full recharge and creates the droids at the
            // point, unturned, for its owner; the droids act from the arming frame on.
            auto droid = impl_->create_buzz_droids(source, point, tick, next_id);
            const auto droid_id = droid.state.entity_id;
            staged.emplace(droid_id, std::move(droid));
            track_victory_change(nullptr, &staged.at(droid_id).state, false);
            auto& live = staged.at(unit_id); // insertion can move the vector
            if (minds.find(unit_id) != minds.end()) {
                apply_squadron_order(minds.at(unit_id), StopPayload{}, live.state.position, squadron_table);
            } else if (live.motion) {
                if (auto stopped = impl_->plan_order(live, StopPayload{}, frame); !stopped)
                    return core::Result<RejectReason>::failure(stopped.error());
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                if (footprint) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted)
                            return core::Result<RejectReason>::failure(submitted.error());
                    }
                }
            }
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        // AB-61 to AB-65 (#561): ION_CANNON_SHOT acts on the squadron's team container itself.
        if (payload.ability == AbilityKind::ion_cannon_shot) {
            auto& container = staged.at(unit_id);
            auto* team = impl_->ion_slot(container);
            const auto squadron = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
                [&](const Squadron& entry) { return entry.container == unit_id; });
            const auto mind = minds.find(unit_id);
            if (team == nullptr || squadron == impl_->squadrons.end() || mind == minds.end()) {
                return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
            std::vector<AbilitySlot*> craft;
            for (const auto member : squadron->members) {
                const auto found = staged.find(member);
                if (auto* slot = found != staged.end() ? impl_->ion_slot(found->second) : nullptr) craft.push_back(slot);
            }
            const auto& profile = *impl_->abilities.find(container.state.type_id);
            const auto& ability = profile.abilities[*ability_slot(profile, AbilityKind::ion_cannon_shot)];
            switch (payload.action) {
            case AbilityAction::activate: {
                // AB-62: ready, off, with a craft to fire it and none still due; a valid target.
                const bool due = std::any_of(craft.begin(), craft.end(), [](const AbilitySlot* slot) { return slot->active; });
                if (team->active || tick < team->ready_tick || craft.empty() || due
                    || (container.nebula && container.nebula->present)) {
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                }
                const auto target = staged.find(payload.target);
                if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
                if (!impl_->ion_target_valid(container.state.owner, &target->second)) {
                    return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
                }
                const auto* hit = impl_->health_profile(target->second);
                const auto hardpoint = hit != nullptr && payload.target_hardpoint != no_hardpoint
                        && damage_target_valid(*hit, payload.target_hardpoint)
                    ? payload.target_hardpoint
                    : no_hardpoint;
                team->active = true;
                team->started_tick = tick;
                team->target = payload.target;
                team->target_hardpoint = hardpoint;
                for (auto* slot : craft) slot->active = true;
                // AB-63: the squadron attacks the target.
                const CommandPayload attack = AttackPayload{payload.target, hardpoint};
                container.state.order = order_for(attack, tick);
                apply_squadron_order(minds.at(mind->first), attack, container.state.position, squadron_table);
                break;
            }
            case AbilityAction::deactivate:
                if (team->active) impl_->finish_ion_shot(*team, craft, container.state.type_id, tick);
                break;
            case AbilityAction::autofire_on:
            case AbilityAction::autofire_off:
                if (!ability.supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                team->autofire = payload.action == AbilityAction::autofire_on;
                break;
            }
            return core::Result<RejectReason>::success(RejectReason::none);
        }
        const auto holders = ordinary_ability_holders(unit_id, payload.ability);
        const auto holds = [&](LiveUnit& live) -> std::optional<std::size_t> {
            if (!live.abilities) return std::nullopt;
            return ability_slot(*impl_->abilities.find(live.state.type_id), payload.ability);
        };
        if (holders.empty()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        const auto profile_of = [&](const LiveUnit& live) -> const AbilityProfile& {
            const auto& profile = *impl_->abilities.find(live.state.type_id);
            return profile.abilities[*ability_slot(profile, payload.ability)];
        };
        const auto slot_of = [&](LiveUnit& live) -> AbilitySlot& { return live.abilities->slots[*holds(live)]; };
        std::vector<EntityId> changed_speed;
        bool hunt_stopped = false; // also LURE: WAB-05 and WLU-02 stop the behaviour's move
        switch (payload.action) {
        case AbilityAction::activate:
            for (auto* live : holders) {
                const auto& slot = slot_of(*live);
                if (slot.active || !ability_ready(profile_of(*live), slot, impl_->ability_gate(*live, tick), tick)) {
                    return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
                }
            }
            for (auto* live : holders) {
                const auto switched = activate_ability(profile_of(*live), slot_of(*live), impl_->ability_gate(*live, tick), tick);
                if (switched.changed && (payload.ability == AbilityKind::interdict || payload.ability == AbilityKind::missile_shield)) {
                    // WIN-02, WMS-02: deployment halts the current move even without a movement lock.
                    live->formation.reset();
                    live->state.order = order_for(StopPayload{}, tick);
                    if (auto stopped = impl_->plan_order(*live, StopPayload{}, frame); !stopped)
                        return core::Result<RejectReason>::failure(stopped.error());
                }
                if (switched.changed && payload.ability == AbilityKind::sensor_jamming) register_jammer(live->state.entity_id, true);
                if (switched.changed && payload.ability == AbilityKind::defend) impl_->defend_switched_on(*live, tick);
                if (switched.changed && payload.ability == AbilityKind::self_destruct) Impl::self_destruct_switched(*live);
                if (switched.changed && payload.ability == AbilityKind::stealth) {
                    impl_->stealth_switched_on(*live, tick, impl_->stealth_duration_factor(live->state.owner, ledgers));
                    if (crafts.find(live->state.entity_id) != crafts.end()) {
                        crafts.at(live->state.entity_id).chase = invalid_entity_id;
                        crafts.at(live->state.entity_id).chase_until = 0;
                    }
                }
                if (switched.changed && (payload.ability == AbilityKind::hunt || payload.ability == AbilityKind::lure)) {
                    // WAB-04, WLU-02: clear attack targets; HUNT keeps the movement under way.
                    if (live->combat) {
                        live->combat->attack_target = invalid_entity_id;
                        live->combat->attack_hardpoint = no_hardpoint;
                        live->combat->direct = false;
                    }
                    if (live->state.order.target != invalid_entity_id) live->state.order = {};
                    if (crafts.find(live->state.entity_id) != crafts.end()) {
                        crafts.at(live->state.entity_id).chase = invalid_entity_id;
                        crafts.at(live->state.entity_id).chase_until = 0;
                    }
                }
                if (switched.speed) changed_speed.push_back(live->state.entity_id);
            }
            break;
        case AbilityAction::deactivate:
            for (auto* live : holders) {
                auto& slot = slot_of(*live);
                const auto switched = deactivate_ability(profile_of(*live), slot, tick);
                if (switched.changed && payload.ability == AbilityKind::sensor_jamming) register_jammer(live->state.entity_id, false);
                if (switched.changed && payload.ability == AbilityKind::stealth) Impl::clear_stealth(*live); // WST-05
                if (switched.speed) changed_speed.push_back(live->state.entity_id);
                if (switched.changed && payload.ability == AbilityKind::self_destruct) Impl::self_destruct_switched(*live);
                hunt_stopped = hunt_stopped || (switched.changed
                    && (payload.ability == AbilityKind::hunt || payload.ability == AbilityKind::lure));
                if (switched.changed && payload.ability == AbilityKind::hunt) {
                    if (live->combat) {
                        live->combat->attack_target = invalid_entity_id;
                        live->combat->attack_hardpoint = no_hardpoint;
                        live->combat->direct = false;
                    }
                    if (crafts.find(live->state.entity_id) != crafts.end()) {
                        crafts.at(live->state.entity_id).chase = invalid_entity_id;
                        crafts.at(live->state.entity_id).chase_until = 0;
                    }
                }
                if (payload.ability == AbilityKind::barrage) {
                    if (const auto proxy = staged.find(slot.target); proxy != staged.end()
                        && proxy->second.state.barrage_source == live->state.entity_id) {
                        track_victory_change(&proxy->second.state, nullptr, true);
                        staged.erase(proxy);
                    }
                    if (live->combat && live->combat->attack_target == slot.target) {
                        live->combat->attack_target = invalid_entity_id;
                        live->combat->direct = false;
                    }
                    if (live->state.order.target == slot.target) live->state.order = {};
                    slot.target = invalid_entity_id;
                }
            }
            break;
        case AbilityAction::autofire_on:
        case AbilityAction::autofire_off:
            for (auto* live : holders) {
                if (!profile_of(*live).supports_autofire) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
            }
            for (auto* live : holders) slot_of(*live).autofire = payload.action == AbilityAction::autofire_on;
            break;
        }
        const bool behaviour_on = (payload.ability == AbilityKind::hunt || payload.ability == AbilityKind::lure)
            && payload.action == AbilityAction::activate;
        // WLU-02: switching LURE on also stops the squadron's move (only HUNT keeps it, WAB-04).
        if (behaviour_on && payload.ability == AbilityKind::lure) hunt_stopped = true;
        if (behaviour_on) {
            const auto& membership = craft_prep_->craft_squadron.value();
            const auto member = membership.find(unit_id);
            const auto container_id = member != membership.end() ? member->second : unit_id;
            if (const auto mind = minds.find(container_id); mind != minds.end()) {
                minds.at(container_id).target = invalid_entity_id;
                minds.at(container_id).target_hardpoint = attack_hull;
                minds.at(container_id).approach = false;
            }
            auto& container = staged.at(container_id);
            if (container.combat) {
                container.combat->attack_target = invalid_entity_id;
                container.combat->direct = false;
            }
            if (container.state.order.target != invalid_entity_id) container.state.order = {};
        }
        if (hunt_stopped) {
            // WAB-05: disabling the behaviour also stops its current movement.
            const auto& membership = craft_prep_->craft_squadron.value();
            const auto member = membership.find(unit_id);
            const auto container_id = member != membership.end() ? member->second : unit_id;
            auto& live = staged.at(container_id);
            live.state.order = order_for(StopPayload{}, tick);
            if (minds.find(container_id) != minds.end()) {
                apply_squadron_order(minds.at(container_id), StopPayload{}, live.state.position, squadron_table);
            } else {
                if (auto stopped = impl_->plan_order(live, StopPayload{}, frame); !stopped) return core::Result<RejectReason>::failure(stopped.error());
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                if (footprint && live.motion) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        if (auto submitted = submit(container_id, live, *layer, std::nullopt); !submitted) return core::Result<RejectReason>::failure(submitted.error());
                    }
                }
            }
        }
        for (const auto id : changed_speed) {
            if (auto planned = replan(id); !planned) return core::Result<RejectReason>::failure(planned.error());
        }
        // WHE-51: later commands and environmental hits read the newly switched modes.
        for (auto* live : holders) impl_->refresh_damage_modes(*live);
        return core::Result<RejectReason>::success(RejectReason::none);
    };

    const auto apply_area_ability = [&](const EntityId unit_id, const AreaAbilityPayload& payload) -> core::Result<RejectReason> {
        auto& before = staged.at(unit_id);
        const auto* profile = impl_->abilities.find(before.state.type_id);
        const auto index = profile != nullptr ? ability_slot(*profile, payload.ability) : std::nullopt;
        if (!index || !before.abilities || !before.combat || !before.motion) {
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        const auto& ability = profile->abilities[*index];
        const auto& slot = before.abilities->slots[*index];
        if (slot.active) return core::Result<RejectReason>::success(RejectReason::none);
        if (!ability_ready(ability, slot, impl_->ability_gate(before, tick), tick) || next_id == invalid_entity_id
            || impl_->combat.find(ability.barrage_target_type) == nullptr) {
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        const auto enemy = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(), [&](const Player& player) {
            return players_hostile(impl_->snapshot_players, before.state.owner, player.player_id);
        });
        if (enemy == impl_->setup.players.end()) return core::Result<RejectReason>::success(RejectReason::target_not_hostile);
        const auto own = std::lower_bound(impl_->setup.players.begin(), impl_->setup.players.end(), before.state.owner,
            [](const Player& player, const PlayerId id) { return player.player_id < id; });
        const auto player_index = static_cast<std::size_t>(own - impl_->setup.players.begin());
        bool visible = false;
        if (impl_->fog) visible = impl_->fog->revealed(player_index, payload.point);
        else {
            // Point visibility uses the last published revealers; no serial world rebuild per click.
            const auto published = impl_->current_snapshot;
            const auto observers = published->instances();
            std::array<std::uint8_t, tick_partition_count> revealed{};
            const auto checked = executor.execute_phase("barrage-point-visibility", tick_partition_count,
                [&](const std::size_t partition) {
                    const auto range = partition_range(partition, observers.size());
                    for (auto index = range.begin; index < range.end; ++index) {
                        const auto& observer = observers[index];
                        if (observer.team != own->team_id || !observer.reveal_range) continue;
                        const math::Vec3 position{observer.fixed_transform.rows[0][3],
                            observer.fixed_transform.rows[1][3], observer.fixed_transform.rows[2][3]};
                        if (within_range(position, payload.point, *observer.reveal_range, RangeMetric::planar)) {
                            revealed[partition] = 1U;
                            break;
                        }
                    }
                });
            if (!checked) return core::Result<RejectReason>::failure(checked.error());
            visible = std::any_of(revealed.begin(), revealed.end(), [](const std::uint8_t value) { return value != 0; });
        }
        if (!visible) return core::Result<RejectReason>::success(RejectReason::invalid_position);
        auto z = math::add(payload.point.z, ability.target_z_offset);
        if (!z) return core::Result<RejectReason>::success(RejectReason::invalid_position);
        LiveUnit proxy;
        proxy.state = UnitState{next_id, ability.barrage_target_type, enemy->player_id,
            {payload.point.x, payload.point.y, z.value()}, math::identity_quat(), {}};
        proxy.state.barrage_source = unit_id;
        const auto proxy_id = next_id++;
        staged.emplace(proxy_id, std::move(proxy));
        track_victory_change(nullptr, &staged.at(proxy_id).state, false);
        auto& live = staged.at(unit_id); // insertion can move the vector
        auto& active = live.abilities->slots[*index];
        static_cast<void>(activate_ability(ability, active, impl_->ability_gate(live, tick), tick));
        active.target = proxy_id;
        const CommandPayload attack = AttackPayload{proxy_id};
        live.state.order = order_for(attack, tick);
        live.combat->attack_target = proxy_id;
        live.combat->attack_hardpoint = no_hardpoint;
        live.combat->direct = true;
        const auto range = impl_->approach_range(live, false);
        if (!within_range(live.state.position, staged.at(proxy_id).state.position, range, RangeMetric::planar)) {
            auto mapped = impl_->approach_mapping(live, staged.at(proxy_id), range, tick + 1);
            if (!mapped) return core::Result<RejectReason>::failure(mapped.error());
            if (auto planned = plan_approach(unit_id, live, mapped.value()); !planned) {
                return core::Result<RejectReason>::failure(planned.error());
            }
        }
        return core::Result<RejectReason>::success(RejectReason::none);
    };

    const auto apply_manual_target = [&](const EntityId unit_id, const ManualTargetPayload& payload,
                                         const PlayerId issuer, const bool player_clock = true) -> core::Result<RejectReason> {
        const auto target = staged.find(payload.target);
        if (target == staged.end()) return core::Result<RejectReason>::success(RejectReason::target_not_live);
        auto& live = staged.at(unit_id);
        const auto* own_view = view.find(unit_id);
        const auto* target_view = view.find(payload.target);
        if (!live.combat || own_view == nullptr || target_view == nullptr || own_view->profile == nullptr) {
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        const auto selected = std::find_if(own_view->profile->weapons.begin(), own_view->profile->weapons.end(),
            [&](const WeaponProfile& weapon) { return weapon.hardpoint == payload.hardpoint; });
        // WSW-07/40: installed manual special weapons require an enabling effect;
        // isolated gun-contract fixtures without modifier profiles retain their existing admission.
        if (player_clock && selected != own_view->profile->weapons.end() && selected->special
            && std::any_of(impl_->economy.upgrades.begin(), impl_->economy.upgrades.end(),
                [](const UpgradeProfile& upgrade) { return !upgrade.special_unlocks.empty(); })
            && !impl_->special_weapons_enabled(issuer, ledgers, &staged, &special_disabled_by_hardpoint_))
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        const auto clock = manual_clocks_.find(issuer);
        if (player_clock && clock != manual_clocks_.end() && manual_readiness(clock->second, tick).raw() < math::Fixed::scale) {
            return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        }
        // WSW-17/40: the same public frozen predicate backs the HUD, Lua and commands.
        // Copy only these two current staged objects, with the immutable visibility relation.
        // WWP-73: admission and the eventual shot use the same rolled weapon frame.
        const auto own_transform = unit_transform(live);
        if (!own_transform) return core::Result<RejectReason>::failure(own_transform.error());
        const auto enemy_transform = unit_transform(target->second);
        if (!enemy_transform) return core::Result<RejectReason>::failure(enemy_transform.error());
        auto own = impl_->instance_for(live, own_transform.value(), tick, &special_weapons_);
        auto enemy = impl_->instance_for(target->second, enemy_transform.value(), tick, &special_weapons_);
        enemy.visible_to = target_view->visible_to;
        if (const auto arrival = arrivals.find(unit_id); arrival != arrivals.end()) own.arrival = arrival->second.frame;
        auto admitted = manual_special_fire_admission(impl_->snapshot_players, own, enemy, !player_clock, payload.hardpoint);
        if (!admitted) return core::Result<RejectReason>::failure(admitted.error());
        if (!admitted.value()) return core::Result<RejectReason>::success(RejectReason::ability_unavailable);
        const auto weapon = std::find_if(own_view->profile->weapons.begin(), own_view->profile->weapons.end(),
            [&](const WeaponProfile& candidate) { return candidate.hardpoint == payload.hardpoint; });
        const auto index = static_cast<std::size_t>(weapon - own_view->profile->weapons.begin());
        auto& assignment = *live.combat->weapons[index].manual;
        assignment.target = payload.target;
        assignment.requesting_player = issuer;
        assignment.assigned_frame = tick;
        return core::Result<RejectReason>::success(RejectReason::none);
    };

    // Commands phase: serial, in canonical (tick, player, sequence) order.
    commands_.emplace();
    beacons_ = impl_->beacons;
    // HCB-12d: deadline seam at the agreed command tick, independent of worker count.
    std::erase_if(beacons_, [this](const BeaconState& beacon) { return beacon.expires_tick <= tick; });
    // SAE-10: workers test a whole ring against shared immutable staged collision views.
    // The ordinary command below still rechecks after earlier ordered commands.
    auto& searches = commands_->searches;
    for (const auto& [key, request] : impl_->reinforcement_searches) {
        if (key.tick != tick) continue;
        searches.push_back(Commands::Search{key, request, {}, {}, {}});
    }
    std::array<CollisionWorld, 4> search_worlds;
    std::vector<UnitState> prevention_units;
    if (!searches.empty()) {
        for (const auto& part : gather_->prevention_ids) {
            for (const auto id : part) {
                const auto found = staged.find(id);
                if (found != staged.end()) prevention_units.push_back(found->second.state);
            }
        }
        std::array<bool, 4> prepared{};
        for (const auto& search : searches) {
            const auto& payload = std::get<ReinforcePayload>(impl_->pending.at(search.key).payload);
            const auto* footprint = table.footprint(payload.type);
            auto layer = footprint != nullptr ? footprint->layer : SpaceLayer::none;
            if (layer == SpaceLayer::none && table.squadrons.find_squadron(payload.type)) layer = SpaceLayer::corvette;
            const auto selected = dynamic_layer_index(layer);
            if (tracking && selected && !prepared[*selected]) {
                if (auto built = world_for(*selected, search_worlds[*selected]); !built) return built;
                prepared[*selected] = true;
            }
        }
        const auto searched = executor.execute_phase("reinforcement-search", tick_partition_count,
            [&](const std::size_t partition) {
                const auto range = partition_range(partition, searches.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    auto& search = searches[index];
                    const auto& payload = std::get<ReinforcePayload>(impl_->pending.at(search.key).payload);
                    const auto player = search.key.player_id;
                    const auto* account = impl_->economy.player(player);
                    search.result.next_attempt = search.request.attempt;
                    search.result.position = payload.position;
                    // SAE-03/10: wait for pool/population admission before spending a ring.
                    // These are sparse ledger/share reads; the ordered command rechecks them.
                    const auto ledger = std::find_if(ledgers.begin(), ledgers.end(),
                        [player](const PlayerEconomy& entry) { return entry.player == player; });
                    if (account == nullptr || ledger == ledgers.end() || impl_->outcome
                        || impl_->economy.disabled_types.contains(payload.type)
                        || std::find(ledger->pool.begin(), ledger->pool.end(), payload.type) == ledger->pool.end()) continue;
                    std::int64_t owned = 0;
                    for (const auto& [id, share] : shares) {
                        static_cast<void>(id);
                        if (share.owner == player) owned += share.share;
                    }
                    const auto population = population_count(owned);
                    if (population > account->population_cap
                        || impl_->population_of(payload.type) > account->population_cap - population) continue;
                    const auto* footprint = table.footprint(payload.type);
                    auto layer = footprint != nullptr ? footprint->layer : SpaceLayer::none;
                    if (layer == SpaceLayer::none && table.squadrons.find_squadron(payload.type)) layer = SpaceLayer::corvette;
                    const auto selected = dynamic_layer_index(layer);
                    const auto* collisions = selected && prepared[*selected] ? &search_worlds[*selected] : nullptr;
                    auto attempt = search.request.attempt;
                    const DiagnosticTimer search_timer;
                    if (attempt == 0) {
                        // SAE-10: prevention alone selects the starting radius, even in fog.
                        for (const auto& unit : prevention_units) {
                            if (impl_->allied(unit.owner, player)) continue;
                            const auto* prevention = impl_->economy.prevention_of(unit.type_id);
                            const auto dx = math::detail::unsigned_magnitude(unit.position.x.raw() - payload.position.x.raw());
                            const auto dy = math::detail::unsigned_magnitude(unit.position.y.raw() - payload.position.y.raw());
                            auto distance = math::detail::multiply_u64(dx, dx);
                            static_cast<void>(math::detail::add_magnitude(distance, math::detail::multiply_u64(dy, dy)));
                            const auto radius = static_cast<std::uint64_t>(prevention->radius.raw());
                            if (math::detail::compare(distance, math::detail::multiply_u64(radius, radius)) < 0) {
                                attempt = 11; // first angle at radius 1000
                                break;
                            }
                        }
                    }
                    const auto count = attempt == 0 ? 1U : 10U;
                    search.result.next_attempt = attempt == 0 ? 1U : attempt + 10;
                    search.result.position = payload.position;
                    for (std::uint32_t angle = 0; angle < count; ++angle) {
                        const auto candidate = reinforcement_search_candidate(payload.position, attempt + angle,
                            payload.facing_yaw.value_or(account != nullptr ? account->reinforcement_yaw : math::Fixed{}), impl_->economy.bounds);
                        if (!candidate) break;
                        ++search.result.candidates;
                        search.result.position = *candidate;
                        const auto valid = impl_->placement_valid(player, payload.type, *candidate, staged, tick + 1,
                            collisions, &search.work, &prevention_units, payload.facing_yaw);
                        if (!valid) { search.error = valid.error(); break; }
                        if (valid.value()) {
                            search.result.valid = true;
                            search.result.next_attempt = attempt;
                            break;
                        }
                    }
                    search.work.nanoseconds = search_timer.elapsed_nanoseconds();
                    search.result.predictions = search.work.predictions;
                    search.result.nanoseconds = search.work.nanoseconds;
                }
            });
        if (!searched) return searched;
        for (const auto& search : searches) {
            if (search.error) return core::Result<void>::failure(*search.error);
            reinforcement_work.predictions += search.work.predictions;
            reinforcement_work.collision_queries += search.work.collision_queries;
            reinforcement_work.nanoseconds += search.work.nanoseconds;
            for (std::size_t gate = 0; gate < search.work.rejections.size(); ++gate) {
                reinforcement_work.rejections[gate] += search.work.rejections[gate];
            }
        }
    }
    auto& diagnostics = commands_.value().diagnostics.emplace();
    // HCB-59: ready routes re-enter the existing group planner in stable order.
    // These are retained simulation transitions, not new player commands or replay events.
    std::optional<std::deque<PlayerCommand>> waypoint_legs;
    if (!orders_->waypoint_ready.empty()) {
        waypoint_legs.emplace();
        std::map<std::pair<CommandKey, CommandKey>, PlayerCommand> next_legs;
        for (const auto id : orders_->waypoint_ready) {
            const auto found = staged.find(id);
            if (found == staged.end() || !found->second.waypoints) continue;
            auto& route = *found->second.waypoints;
            if (route.active && !route.points.empty()) {
                const auto completed = route.points.front();
                route.points.erase(route.points.begin());
                if (completed.repeat) route.points.push_back(completed);
            }
            if (route.points.empty()) { route.active = false; continue; }
            route.active = true;
            const auto& next = route.points.front();
            auto [leg, inserted] = next_legs.try_emplace(std::pair{route.coordinator.value_or(next.key), next.key},
                PlayerCommand{{tick, found->second.state.owner, next.key.sequence}, {}, next.move});
            static_cast<void>(inserted);
            leg->second.units.push_back(id);
        }
        for (auto& [key, leg] : next_legs) {
            static_cast<void>(key);
            waypoint_legs->push_back(std::move(leg));
        }
    }
    // WAB-36: one authored-order holder pass through ordinary command admission. Immutable
    // candidate preparation is partitioned; command transactions commit in ascending holder ID.
    // The command-stage seam uses the persisted attack target, as the former split passes did.
    // Only an actual switch-on consumes the pass: a rejection or WHE-24 empty-query release
    // leaves the following candidate available. Both the queue and success set die this tick.
    std::deque<PlayerCommand> autofire_commands;
    std::set<EntityId> autofire_switched;
    const auto queue_targeted_autofire = [&]() -> core::Result<void> {
        if (std::none_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(), [](const auto& profile) {
                return std::any_of(profile.special.begin(), profile.special.end(), [](const auto& nested) {
                    return nested.style == SpecialActivationStyle::user_input && attack_target_ability(nested.kind).has_value();
                });
            })) return core::Result<void>::success();
        std::vector<const LiveUnit*> holders;
        for (const auto& [id, unit] : staged) {
            if (unit.abilities && unit.combat && unit.combat->attack_target != invalid_entity_id
                && !arrivals.contains(id)) holders.push_back(&unit);
        }
        if (holders.empty()) return core::Result<void>::success();
        std::vector<std::vector<PlayerCommand>> candidates(holders.size());
        const auto prepared = executor.execute_phase("targeted-autofire", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, holders.size());
            for (auto i = range.begin; i < range.end; ++i) {
                const auto& unit = *holders[i];
                const auto id = unit.state.entity_id;
                const bool departed = std::any_of(impl_->quits.begin(), impl_->quits.end(),
                    [&](const PlayerQuit& quit) { return quit.player == unit.state.owner; });
                const bool takeover = impl_->setup.departure_policy
                    && std::binary_search(impl_->basic_ai.begin(), impl_->basic_ai.end(), unit.state.owner);
                if (departed && !takeover) continue;
                const auto* profile = impl_->abilities.find(unit.state.type_id);
                const auto target = staged.find(unit.combat->attack_target);
                if (profile == nullptr || target == staged.end() || target->first == id) continue;
                const bool human = impl_->abilities.human(unit.state.owner);
                for (const auto& candidate : profile->special) {
                    const auto served = candidate.style == SpecialActivationStyle::user_input
                        ? attack_target_ability(candidate.kind) : std::nullopt;
                    if (!served) continue;
                    const auto slot = ability_slot(*profile, *served);
                    if (!slot) continue;
                    const auto& state = unit.abilities->slots[*slot];
                    if (state.active || (human && !state.autofire)) continue;
                    if (*served != AbilityKind::concentrate_fire) {
                        const auto* beam = impl_->beam_profile(unit, *served);
                        if (beam != &candidate || !impl_->beam_usable(unit, *served, tick)
                            || !impl_->beam_target_valid(unit, target->second, *beam)
                            || !impl_->beam_in_range(unit, target->second, *beam)) continue;
                    } else {
                        if (impl_->concentrate_profile(unit) != &candidate) continue;
                        const auto& special = unit.abilities->special.slots[static_cast<std::size_t>(&candidate - profile->special.data())];
                        const auto* target_profile = impl_->combat.find(target->second.state.type_id);
                        if (special.cancelled || unit.abilities->special.service_cancelled || candidate.initially_enabled == false
                            || !ability_ready(profile->abilities[*slot], state, impl_->ability_gate(unit, tick), tick)
                            || !players_hostile(impl_->snapshot_players, unit.state.owner, target->second.state.owner)
                            || target_profile == nullptr
                            || !special_type_matches(candidate.filter, target->second.state.type_id, target_profile->category_bits)) continue;
                    }
                    // CF and beam candidates of one holder share this key; successful
                    // switch-on consumes the pass by unit ID, never by command key (WAB-36).
                    candidates[i].push_back(PlayerCommand{synthesised_command_key(tick, unit.state.owner, id), {id},
                        AbilityPayload{*served, AbilityAction::activate, target->first}});
                }
            }
        });
        if (!prepared) return prepared;
        for (auto& holder : candidates) for (auto& candidate : holder)
            autofire_commands.push_back(std::move(candidate));
        return core::Result<void>::success();
    };
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
            if (auto queued = queue_targeted_autofire(); !queued) return queued;
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
            const bool allowed = impl_->retreat && pending_retreat_.phase == RetreatPhase::escaping
                && pending_retreat_.player == issuer && !pending_outcome_ && cinematic->frame == 0;
            if (allowed) {
                // Reports never advance time, even when several arrive in one tactical frame.
                pending_retreat_.playback_reported = pending_retreat_.playback_reported || cinematic->playing;
                pending_retreat_.playback_playing = cinematic->playing;
            }
            events.push_back(Event{tick, allowed ? EventKind::order_accepted : EventKind::order_rejected,
                issuer, command.key.sequence, 0, OrderKind::retreat, allowed ? RejectReason::none : RejectReason::ability_unavailable});
            continue;
        }
        if (const auto* retreat = std::get_if<RetreatPayload>(&command.payload)) {
            auto refusal = RetreatRefusal::wrong_mode;
            if (impl_->retreat) {
                auto gate = impl_->retreat_gate(issuer); gate.battle_decided = pending_outcome_.has_value();
                if (retreat->action == RetreatAction::auto_resolve) diagnostics.push_back(detail::diagnostic(
                    diagnostic_codes::command_rejected, "mid-battle solve requires the native interleaved death service (WAR-41/45)", {}, core::Severity::warning));
                else if (impl_->retreat->flight_qualified || retreat->action == RetreatAction::cancel)
                    refusal = request_retreat(impl_->retreat->rules, pending_retreat_, issuer, retreat->action,
                    tick, gate, impl_->retreat_actors());
            }
            events.push_back(Event{tick, refusal == RetreatRefusal::none ? EventKind::order_accepted : EventKind::order_rejected,
                issuer, command.key.sequence, 0, OrderKind::retreat,
                refusal == RetreatRefusal::none ? RejectReason::none : RejectReason::ability_unavailable});
            if (refusal != RetreatRefusal::none) diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                "campaign retreat refused: " + std::to_string(static_cast<unsigned>(refusal)), {}, core::Severity::warning));
            continue;
        }
        if (const auto* special = std::get_if<SpecialFirePayload>(&command.payload)) {
            const auto source_id = command.units.front();
            const auto source = staged.find(source_id);
            const auto target = staged.find(special->target);
            const auto* player = impl_->economy.player(issuer);
            const auto* profile = source != staged.end() ? impl_->combat.find(source->second.state.type_id) : nullptr;
            auto reason = RejectReason::none;
            const bool ai = special->admission != SpecialFireAdmission::player;
            bool dispatched = false, fired = false;
            std::uint32_t index = 0;
            if (ai && (!player || !player->ai)) reason = RejectReason::ability_unavailable;
            else if (source == staged.end()) reason = RejectReason::unit_not_live;
            else if (!profile || !impl_->special_weapons_enabled(issuer, ledgers, &staged, &special_disabled_by_hardpoint_)) reason = RejectReason::ability_unavailable;
            else if (!impl_->allied(issuer, source->second.state.owner)) reason = RejectReason::unit_not_owned;
            else if (profile->planetary_weapon) {
                const auto state = special_weapons_.find(source_id);
                index = profile->planetary_weapon->index;
                if (source->second.state.owner != issuer) reason = RejectReason::unit_not_owned;
                else if (state == special_weapons_.end()) reason = RejectReason::ability_unavailable;
                else {
                    auto& clock = state->second;
                    // WSW-16: the player event admits at .99; the helper still needs full readiness.
                    const auto duration = clock.recharge_until > clock.recharge_started
                        ? clock.recharge_until - clock.recharge_started : 0;
                    const auto elapsed = tick > clock.recharge_started ? tick - clock.recharge_started : 0;
                    const bool outer_ready = tick >= clock.recharge_until || (!ai && duration && elapsed * 100 >= duration * 99);
                    if (!outer_ready) reason = RejectReason::ability_unavailable;
                    else {
                        dispatched = true;
                        if (tick < clock.recharge_until) reason = RejectReason::ability_unavailable;
                        else if (target == staged.end()) reason = RejectReason::target_not_live;
                        else {
                            const auto own_transform = unit_transform(source->second);
                            if (!own_transform) return core::Result<void>::failure(own_transform.error());
                            const auto enemy_transform = unit_transform(target->second);
                            if (!enemy_transform) return core::Result<void>::failure(enemy_transform.error());
                            const auto own = impl_->instance_for(source->second, own_transform.value(), tick, &special_weapons_);
                            const auto enemy = impl_->instance_for(target->second, enemy_transform.value(), tick, &special_weapons_);
                            reason = planetary_special_fire_target_admission(impl_->snapshot_players, own, enemy);
                            if (reason == RejectReason::none && (!view.find(source_id) || !view.find(special->target)))
                                reason = RejectReason::ability_unavailable;
                        }
                        if (reason == RejectReason::none) {
                            // WSW-19/22: wrapper success starts clocks even if shared shot construction later rejects.
                            clock.recharge_started = tick;
                            clock.recharge_until = tick + profile->planetary_weapon->recharge_frames;
                            clock.next_pulse_frame = tick + profile->planetary_weapon->pulse_delay_frames;
                            clock.target = special->target;
                            clock.pulses_left = profile->weapons.front().pulse_count - 1;
                            special_initial_.insert(source_id);
                            SpecialShot shot;
                            shot.source = *view.find(source_id);
                            shot.target = *view.find(special->target);
                            shot.source.position = source->second.state.position;
                            // WWP-73/74: pulse one shares the banked placement of later pulses.
                            const auto source_transform = unit_transform(source->second);
                            if (!source_transform) return core::Result<void>::failure(source_transform.error());
                            shot.source.transform = source_transform.value();
                            shot.target.position = target->second.state.position;
                            const auto target_transform = unit_transform(target->second);
                            if (!target_transform) return core::Result<void>::failure(target_transform.error());
                            shot.target.transform = target_transform.value();
                            shot.target_health = target->second.durability;
                            if (const auto* health = impl_->health_profile(target->second)) shot.target_health_profile = *health;
                            special_shots_.push_back(std::move(shot));
                            if (!clock.pulses_left) clock.target = invalid_entity_id;
                            fired = true;
                        }
                    }
                }
            } else if (!profile->star_base || special->admission == SpecialFireAdmission::ai_taskforce)
                reason = RejectReason::ability_unavailable;
            else {
                const auto weapon = std::find_if(profile->weapons.begin(), profile->weapons.end(),
                    [](const WeaponProfile& entry) { return entry.requires_manual_target; });
                if (weapon == profile->weapons.end() || !source->second.combat) reason = RejectReason::ability_unavailable;
                else {
                    const auto slot = static_cast<std::size_t>(weapon - profile->weapons.begin());
                    if (slot >= source->second.combat->weapons.size()
                        || (ai && source->second.combat->weapons[slot].countdown != 0)) reason = RejectReason::ability_unavailable;
                    else {
                        dispatched = true;
                        auto assigned = apply_manual_target(source_id, {special->target, weapon->hardpoint}, issuer, !ai);
                        if (!assigned) return core::Result<void>::failure(assigned.error());
                        reason = assigned.value();
                        fired = reason == RejectReason::none;
                    }
                }
            }
            // WSW-09/10: player cleanup follows admitted dispatch, even a rejected helper;
            // AI cleanup follows success and keeps its entry-route owner selection.
            if ((!ai && dispatched) || (ai && fired)) {
                const auto owner = special->admission == SpecialFireAdmission::ai_object ? issuer
                    : source->second.state.owner;
                if (auto* account = impl_->ledger_of(ledgers, owner)) {
                    std::vector<CompletedBuild> removed;
                    std::erase_if(account->completed, [&](const CompletedBuild& held) {
                        const auto* upgrade = impl_->economy.upgrade(held.type);
                        if (!upgrade || !upgrade->consume_after_special_fire) return false;
                        removed.push_back(held);
                        return true;
                    });
                    for (const auto& held : removed) impl_->retire_income_modifier(*account, held);
                }
            }
            events.push_back(Event{tick, reason == RejectReason::none ? EventKind::order_accepted : EventKind::order_rejected,
                issuer, command.key.sequence, source_id, OrderKind::special_fire, reason});
            if (fired && profile->planetary_weapon)
                events.push_back(Event{tick, EventKind::special_weapon_fired, source->second.state.owner, index, source_id});
            continue;
        }
        if (const auto* beacon = std::get_if<BeaconPayload>(&command.payload)) {
            const auto rule = std::lower_bound(impl_->setup.beacon_rules.begin(), impl_->setup.beacon_rules.end(), issuer,
                [](const auto& entry, const PlayerId player) { return entry.player < player; });
            const bool allowed = rule != impl_->setup.beacon_rules.end() && rule->player == issuer;
            if (allowed) {
                std::erase_if(beacons_, [issuer](const auto& previous) { return previous.player == issuer; });
                const auto at = std::lower_bound(beacons_.begin(), beacons_.end(), issuer,
                    [](const auto& entry, const PlayerId player) { return entry.player < player; });
                beacons_.insert(at, BeaconState{issuer, rule->type, beacon->position, tick,
                    tick + rule->lifetime_frames, command.key.sequence});
            }
            events.push_back(Event{tick, allowed ? EventKind::order_accepted : EventKind::order_rejected,
                issuer, command.key.sequence, invalid_entity_id, OrderKind::beacon,
                allowed ? RejectReason::none : RejectReason::beacon_unavailable});
            continue;
        }
        // #530: a buy, cancel or reinforce acts on the issuer's economy.
        if (const auto* reveal = std::get_if<RevealAllPayload>(&command.payload)) {
            pending_reveals_.push_back(reveal->player);
            events.push_back(Event{tick, EventKind::order_accepted, issuer, command.key.sequence,
                invalid_entity_id, OrderKind::reveal_all});
            continue;
        }
        if (const auto* repair = std::get_if<RepairHardpointPayload>(&command.payload)) {
            auto reason = RejectReason::none;
            const auto found = staged.find(command.units.front());
            if (found == staged.end()) reason = RejectReason::unit_not_live;
            else if (impl_->ledger_of(ledgers, issuer) == nullptr) reason = RejectReason::no_economy;
            else {
                auto& unit = found->second;
                const auto* profile = unit.durability ? impl_->health_profile(unit) : nullptr;
                const bool station = std::any_of(impl_->economy.menus.begin(), impl_->economy.menus.end(),
                    [&](const StationMenu& menu) { return menu.station == unit.state.type_id; })
                    || std::binary_search(impl_->victory.starbase_types.begin(), impl_->victory.starbase_types.end(), unit.state.type_id);
                if (!station || !profile) reason = RejectReason::not_damageable;
                else if (repair->hardpoint >= profile->hardpoints.size()
                    || !profile->hardpoints[repair->hardpoint].destroyable
                    || unit.durability->hardpoints[repair->hardpoint].raw() <= 0
                    || unit.durability->hardpoints[repair->hardpoint] >= profile->hardpoints[repair->hardpoint].max_health) {
                    reason = RejectReason::hardpoint_invalid;
                } else if (arrivals.contains(unit.state.entity_id)) reason = RejectReason::arriving;
                else {
                    // WSL-40/41: service admission has no ownership gate; each payer registers once.
                    unit.durability->repairing_players.resize(profile->hardpoints.size());
                    auto& payers = unit.durability->repairing_players[repair->hardpoint];
                    if (std::find(payers.begin(), payers.end(), issuer) == payers.end()) payers.push_back(issuer);
                }
            }
            events.push_back(Event{tick, reason == RejectReason::none ? EventKind::order_accepted : EventKind::order_rejected,
                issuer, command.key.sequence, command.units.front(), OrderKind::repair_hardpoint, reason, static_cast<std::uint8_t>(repair->hardpoint)});
            continue;
        }
        if (economy_command(command.payload)) {
            Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
                staging_.value().born_spawners, earners, next_id,
                pads, construction, pad_requests, impl_->pad_stage, impl_->construction_stage, pending_economy_cues_, &reinforcement_work};
            CollisionWorld collisions;
            const CollisionWorld* placement_world = nullptr;
            if (tracking && std::holds_alternative<ReinforcePayload>(command.payload)) {
                const auto type = std::get<ReinforcePayload>(command.payload).type;
                const auto* footprint = table.footprint(type);
                auto layer = footprint != nullptr ? footprint->layer : SpaceLayer::none;
                if (layer == SpaceLayer::none && table.squadrons.find_squadron(type)) layer = SpaceLayer::corvette;
                if (const auto selected = dynamic_layer_index(layer)) {
                    if (auto built = world_for(*selected, collisions); !built) return core::Result<void>::failure(built.error());
                    placement_world = &collisions;
                }
            }
            const auto first_added = next_id;
            std::optional<UnitState> sold_before;
            if (std::holds_alternative<PadSellPayload>(command.payload) && !command.units.empty()) {
                const auto sold = staged.find(command.units.front());
                if (sold != staged.end()) sold_before = sold->second.state;
            }
            if (auto executed = impl_->execute_economy(command, tick, staged, stage, events, placement_world); !executed) {
                return core::Result<void>::failure(executed.error());
            }
            if (first_added != next_id && first_added != invalid_entity_id) {
                // Only this command's newly admitted units, never the live world.
                for (auto id = first_added; id < next_id; ++id) {
                    const auto added = staged.find(id);
                    if (added != staged.end()) {
                        track_victory_change(nullptr, &added->second.state, false);
                        if (added->second.combat) concentrate_new_bodies.push_back({id, added->second.state.owner, added->second.state.position});
                    }
                }
            }
            if (std::holds_alternative<PadSellPayload>(command.payload)
                && events.back().kind == EventKind::order_accepted) {
                // WBP-32/WBF-31: sale uses ordinary elimination evaluation after removal.
                if (sold_before) track_victory_change(&*sold_before, nullptr, true);
                if (tracking) {
                    // WBP-32/AV-15: later move commands must plan without the sold obstacle.
                    tracking->statics.reset();
                    for (auto& layer_view : tracking->views) layer_view.reset();
                    rebuilt.fill(true);
                    tracking->samples.erase(command.units.front());
                }
            }
            if (events.back().kind == EventKind::order_rejected) {
                diagnostics.push_back(detail::diagnostic(diagnostic_codes::command_rejected,
                    command_context(command.key) + ": " + std::string(to_string(events.back().order)) + " rejected ("
                        + std::string(to_string(events.back().reason)) + ")",
                    {}, core::Severity::warning));
            }
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
                // proceeds; collision and special-weapon admission belong to scans only.
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
                    auto applied = apply_area_ability(unit_id, *area);
                    if (!applied) return core::Result<void>::failure(applied.error());
                    reason = applied.value();
                } else if (const auto* manual = std::get_if<ManualTargetPayload>(&command.payload)) {
                    auto applied = apply_manual_target(unit_id, *manual, issuer);
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
                auto& live = unit->second;
                const bool member_move = std::holds_alternative<MovePayload>(command.payload) && [&] {
                    const auto& membership = craft_prep_->craft_squadron.value();
                    const auto member = membership.find(unit_id);
                    return member != membership.end() && member->second != unit_id;
                }();
                const bool queued_wait = [&] {
                    if (waypoint_resume) return false;
                    if (const auto* move = std::get_if<MovePayload>(&command.payload); move && move->append && !member_move) {
                        // HCB-58: admission retains the current leg and bounded chronological queue.
                        if (table.rules.max_waypoints == 0) return true;
                        if (!live.waypoints) { live.waypoints.emplace(); live.waypoints->coordinator = command.key; }
                        auto& route = *live.waypoints;
                        if (route.points.size() >= table.rules.max_waypoints) return true;
                        const MovePayload leg{move->destination, move->through_hazards};
                        if (!route.points.empty() && route.points.back().move == leg) return true;
                        bool closed = false;
                        // The newest entry is an exact duplicate gate, not a loop-close candidate.
                        for (auto prior = route.points.size(); prior > 1; --prior) {
                            const auto index = prior - 2;
                            const auto radius = math::Fixed::from_raw(80 * math::Fixed::scale);
                            if (within_range(route.points[index].move.destination, leg.destination, radius, RangeMetric::planar)
                                && route.points[index].move.destination != leg.destination) {
                                // HCB-58: reject the radius boundary; exact destination equality is handled below.
                                const auto dx = math::subtract(route.points[index].move.destination.x, leg.destination.x).value();
                                const auto dy = math::subtract(route.points[index].move.destination.y, leg.destination.y).value();
                                if (math::length(math::Vec3{dx, dy, {}}).value() >= radius) continue;
                                for (auto repeated = index; repeated < route.points.size(); ++repeated) route.points[repeated].repeat = true;
                                closed = true;
                                break;
                            }
                            if (route.points[index].move.destination == leg.destination) {
                                for (auto repeated = index; repeated < route.points.size(); ++repeated) route.points[repeated].repeat = true;
                                closed = true;
                                break;
                            }
                        }
                        if (closed) return true;
                        route.points.push_back({leg, command.key, false});
                        const auto mind = minds.find(unit_id);
                        const bool moving_team = mind != minds.end() && mind->second.mode != SquadronMode::idle;
                        if (route.active || route.points.size() > 1 || live.formation || moving_team
                            || (live.motion && live.motion->kind != MotionKind::none) || live.approach) return true;
                        route.active = true;
                    } else if (std::holds_alternative<MovePayload>(command.payload)
                        || std::holds_alternative<StopPayload>(command.payload)
                        || std::holds_alternative<AttackPayload>(command.payload)
                        || std::holds_alternative<AttackMovePayload>(command.payload)
                        || std::holds_alternative<GuardPayload>(command.payload)) {
                        live.waypoints.reset();
                    }
                    return false;
                }();
                if (std::holds_alternative<StopPayload>(command.payload)) {
                    // WAB-51: Stop switches Hunt off through the same path as its button,
                    // including clearing chase targets and stopping the whole squadron.
                    const auto& membership = craft_prep_->craft_squadron.value();
                    const auto member = membership.find(unit_id);
                    const auto holder = member == membership.end() ? unit_id : member->second;
                    const auto stopped = apply_ability(holder, AbilityPayload{AbilityKind::hunt, AbilityAction::deactivate});
                    if (!stopped) return core::Result<void>::failure(stopped.error());
                }
                // WSQ-59 (#741): a move given to one craft of a squadron changes nothing on the
                // craft (its order, targets and flight stay); only the cancellations below apply.
                if (!member_move && !queued_wait) live.state.order = order_for(command.payload, tick);
                // WAB-40, AB-65: accepted move, attack and guard orders also cancel the
                // parent shot when addressed to a member. Ability/internal orders stay separate.
                const bool explicitly_addressed = std::find(selected_units.begin(), selected_units.end(), unit_id) != selected_units.end();
                if ((std::holds_alternative<MovePayload>(command.payload)
                    || std::holds_alternative<AttackPayload>(command.payload)
                    || std::holds_alternative<AttackMovePayload>(command.payload)
                    || std::holds_alternative<GuardPayload>(command.payload)) && (waypoint_resume || explicitly_addressed)) {
                    const auto& membership = craft_prep_->craft_squadron.value();
                    const auto member = membership.find(unit_id);
                    const auto parent_id = member == membership.end() ? unit_id : member->second;
                    const auto parent = staged.find(parent_id);
                    // WAB-51: an accepted external order cancels every holder in the team.
                    const auto cancel_hunt = [&](LiveUnit& holder) {
                        if (!holder.abilities) return;
                        const auto* profile = impl_->abilities.find(holder.state.type_id);
                        const auto index = ability_slot(*profile, AbilityKind::hunt);
                        if (index) static_cast<void>(deactivate_ability(profile->abilities[*index], holder.abilities->slots[*index], tick));
                    };
                    cancel_hunt(live);
                    if (parent != staged.end()) cancel_hunt(parent->second);
                    if (const auto mind = minds.find(parent_id); mind != minds.end()) {
                        for (const auto member_id : mind->second.roster) {
                            if (const auto holder = staged.find(member_id); holder != staged.end()) cancel_hunt(holder->second);
                        }
                    }
                    auto* team = parent != staged.end() ? impl_->ion_slot(parent->second) : nullptr;
                    if (team != nullptr && team->active && minds.find(parent_id) != minds.end()) {
                        const auto squadron = std::find_if(impl_->squadrons.begin(), impl_->squadrons.end(),
                            [parent_id](const Squadron& entry) { return entry.container == parent_id; });
                        if (squadron != impl_->squadrons.end()) {
                            std::vector<AbilitySlot*> craft;
                            for (const auto craft_id : squadron->members) {
                                const auto found = staged.find(craft_id);
                                if (auto* slot = found != staged.end() ? impl_->ion_slot(found->second) : nullptr) {
                                    craft.push_back(slot);
                                }
                            }
                            impl_->finish_ion_shot(*team, craft, parent->second.state.type_id, tick);
                        }
                    }
                }
                if (member_move || queued_wait) continue;
                if (!waypoint_resume && (std::holds_alternative<MovePayload>(command.payload)
                    || std::holds_alternative<AttackPayload>(command.payload)
                    || std::holds_alternative<AttackMovePayload>(command.payload)
                    || std::holds_alternative<GuardPayload>(command.payload)) && (live.motion || minds.find(unit_id) != minds.end())) {
                    if (!live.waypoints) live.waypoints.emplace();
                    live.waypoints->coordinator = command.key;
                }
                // FO-01, FO-03 (#424): a squadron takes the order as one unit through its team
                // container; its craft fly it in the next craft phase. Serial, in command order.
                if (const auto mind = minds.find(unit_id); mind != minds.end()) {
                    if (std::holds_alternative<AttackPayload>(command.payload) && !attack_assignment_allowed) continue;
                    auto squadron_payload = command.payload;
                    const auto team_target = [&](const EntityId target) {
                        const auto& membership = craft_prep_.value().craft_squadron.value();
                        const auto member = membership.find(target);
                        return member != membership.end() ? member->second : target;
                    };
                    // WSQ-47/WMV-18: a space team's destination names a team, while the
                    // submitted command and published order retain the requested craft ID.
                    if (auto* attack = std::get_if<AttackPayload>(&squadron_payload)) attack->target = team_target(attack->target);
                    else if (auto* attack_move = std::get_if<AttackMovePayload>(&squadron_payload)) attack_move->target = team_target(attack_move->target);
                    else if (auto* guard = std::get_if<GuardPayload>(&squadron_payload)) guard->target = team_target(guard->target);
                    // UFS-21: an accepted replacement ends the special approach, even when
                    // the replacement is another move to the same point. Face preserves it.
                    if (live.abilities && !std::holds_alternative<FacePayload>(squadron_payload))
                        live.abilities->pending_buzz_position.reset();
                    apply_squadron_order(minds.at(mind->first), squadron_payload, live.state.position, squadron_table);
                    if (mind->second.mode == SquadronMode::move && !std::holds_alternative<FacePayload>(command.payload)) {
                        squadron_group.push_back(unit_id);
                    }
                    continue;
                }
                // An attack order gives the unit and its hardpoints that target (#73); any other
                // order ends a previous attack order.
                if (live.combat) {
                    if (const auto* attack = std::get_if<AttackPayload>(&command.payload)) {
                        if (attack_assignment_allowed) {
                            live.combat->attack_target = attack->target;
                            live.combat->attack_hardpoint = attack->hardpoint;
                            live.combat->direct = true;
                        }
                    } else if (live.combat->direct) {
                        live.combat->attack_target = invalid_entity_id;
                        live.combat->attack_hardpoint = no_hardpoint;
                        live.combat->direct = false;
                    }
                }
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                std::optional<std::size_t> layer;
                if (footprint != nullptr) layer = dynamic_layer_index(footprint->layer);
                const auto failed = [&](const core::Diagnostic& error) {
                    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                        command_context(command.key) + " unit " + std::to_string(unit_id) + ": " + error.message));
                };
                const bool moves = point_move(command.payload).has_value();
                // A move, face or stop submits the unit's new prediction and replaces a group
                // move's wait (FM-10), and so does an order that approaches a unit (#452); other
                // orders keep both. Every order ends the previous approach mapping.
                const bool submits = moves || std::holds_alternative<FacePayload>(command.payload)
                    || std::holds_alternative<StopPayload>(command.payload) || approach != invalid_entity_id;
                if (submits) live.formation.reset();
                live.approach.reset();
                live.sighting.reset();
                if (tracking && layer && moves && live.motion) {
                    group.push_back(unit_id);
                    continue;
                }
                if (layer && (moves || submits) && !pending.empty()) {
                    if (auto queued = plan_pending()) return unit_failure(queued->first, queued->second);
                }
                if (approach != invalid_entity_id) {
                    // OR-02, OR-05: the first mapping. A unit in range of the target holds where
                    // it is; otherwise it plans towards its approach slot.
                    if (!live.motion) continue;
                    const auto& target = staged.at(approach);
                    const bool guard = std::holds_alternative<GuardPayload>(command.payload);
                    // The targeting view's health and combat pointers name the units before
                    // staging moved them; point the target's entry at its staged health and that
                    // health's profile (an upgraded unit keeps its profile in the unit itself).
                    std::optional<detail::CombatUnit> target_view;
                    if (const auto* entry = view.find(approach)) {
                        target_view = *entry;
                        target_view->durability = target.durability ? &*target.durability : nullptr;
                        target_view->durability_profile = impl_->health_profile(target);
                        target_view->combat = target.combat ? &*target.combat : nullptr;
                    }
                    const auto* ordered_attack = std::get_if<AttackPayload>(&command.payload);
                    const auto range = impl_->approach_range(live, guard, target_view ? &*target_view : nullptr);
                    auto reach = core::Result<math::Fixed>::success(range);
                    if (!guard && target_view) {
                        const auto* attack_profile = impl_->combat.find(live.state.type_id);
                        reach = detail::target_attack_distance(&impl_->motion, *target_view, live.state.position,
                            attack_profile != nullptr ? attack_profile->max_attack_distance.value_or(math::Fixed{}) : math::Fixed{},
                            detail::has_aim_hardpoint(*target_view));
                    }
                    if (!reach) return failed(reach.error());
                    const auto point = impl_->approach_point(live, guard, target, target_view ? &*target_view : nullptr,
                        ordered_attack != nullptr ? ordered_attack->hardpoint : attack_hull);
                    if (within_range(live.state.position, point, reach.value(), RangeMetric::planar)) {
                        // A path is dropped; a turn in place under way (A-04's) carries on (OR-05).
                        if (live.motion->kind != MotionKind::path) continue;
                        if (auto held = impl_->plan_order(live, StopPayload{}, tick + 1); !held) return failed(held.error());
                        if (layer) {
                            if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) {
                                return failed(submitted.error());
                            }
                        }
                        continue;
                    }
                    auto mapped = impl_->approach_mapping(live, target, range, tick + 1, point);
                    if (!mapped) return failed(mapped.error());
                    if (auto planned = plan_approach(unit_id, live, mapped.value()); !planned) return failed(planned.error());
                    continue;
                }
                CollisionWorld collisions;
                const CollisionWorld* planning = nullptr;
                if (tracking && layer && moves) {
                    if (auto built = world_for(*layer, collisions); !built) return failed(built.error());
                    planning = &collisions;
                }
                // An attack-move or guard of a point plans as the move it is for a ship (OR-11, OR-16).
                const CommandPayload plan_payload =
                    moves ? CommandPayload{MovePayload{*point_move(command.payload)}} : command.payload;
                auto planned = impl_->plan_order(live, plan_payload, tick + 1, planning);
                if (!planned) {
                    return failed(planned.error());
                }
                if (layer && live.motion && submits) {
                    if (auto submitted = submit(unit_id, live, *layer, std::nullopt); !submitted) return failed(submitted.error());
                }
                continue;
            }
            if (damage_blocked()) continue;
            // With damage rules the shield absorbs scripted damage first (DG-20); without, it is raw.
            // WR-33/WHE-51: privileged Lua damage bypasses arrival immunity, but keeps the take mode.
            // WHE-64: LuaDebugDamage distributes raw shares first, then also damages the leader.
            Hit routed_hit{damage->amount, no_type_index, false, true, true, hull_target};
            const auto routed = redirect_damage(unit_id, routed_hit, [&](const EntityId recipient_id, Hit hit) -> core::Result<void> {
                if (damage_blocked()) return core::Result<void>::success();
                const auto recipient = staged.find(recipient_id);
                if (recipient == staged.end() || !recipient->second.durability) return core::Result<void>::success();
                const auto* recipient_profile = impl_->durability.find(recipient->second.state.type_id);
                if (!recipient_profile) return core::Result<void>::success();
                auto& live = recipient->second;
                const auto hull_before = live.durability->hull;
                const auto shields_before = live.durability->shields;
                hit.take_damage_multiplier = live.take_damage_mode;
                DamageOutcome redirected;
                if (damage_rules) {
                    const auto applied = apply_hit(*recipient_profile, *damage_rules, *live.durability, hit, tick);
                    if (!applied) return core::Result<void>::failure(applied.error());
                    redirected = applied.value().damage;
                    impl_->end_depleted_defend(live, tick, applied.value().storm_shield_branch);
                } else {
                    const auto amount = math::multiply(hit.amount, hit.take_damage_multiplier);
                    if (!amount) return core::Result<void>::failure(amount.error());
                    redirected = apply_damage(*recipient_profile, *live.durability, hull_target, amount.value());
                }
                impl_->track_damage(live, hull_before, shields_before);
                if (redirected.unit_destroyed) {
                    events.push_back(destruction_event(tick, EventKind::unit_destroyed, live.state, 0, command.key.player_id));
                    killed.push_back(live.state);
                    if (const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr) {
                        if (const auto layer = dynamic_layer_index(footprint->layer)) {
                            rebuilt[*layer] = true;
                            if (tracking) tracking->views[*layer].reset();
                        } else if (footprint->layer == SpaceLayer::static_object && tracking) tracking->statics.reset();
                        if (tracking) tracking->samples.erase(recipient_id);
                    }
                    impl_->adjust_owned(live.state, false);
                    staged.erase(recipient);
                }
                return core::Result<void>::success();
            });
            if (!routed) return core::Result<void>::failure(routed.error());
            if (damage_blocked()) continue;
            auto outcome = DamageOutcome{};
            const auto hull_before = unit->second.durability->hull;
            const auto shields_before = unit->second.durability->shields;
            if (damage_rules != nullptr) {
                Hit hit{damage->amount, no_type_index, false, true, true, damage->hardpoint};
                hit.take_damage_multiplier = unit->second.take_damage_mode;
                auto applied = apply_hit(*profile, *damage_rules, *unit->second.durability, hit, tick);
                if (!applied) {
                    return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                        command_context(command.key) + " unit " + std::to_string(unit_id) + ": "
                            + applied.error().message));
                }
                impl_->end_depleted_defend(unit->second, tick, applied.value().storm_shield_branch);
                outcome = applied.value().damage;
            } else {
                const auto amount = math::multiply(damage->amount, unit->second.take_damage_mode);
                if (!amount) return core::Result<void>::failure(amount.error());
                outcome = apply_damage(*profile, *unit->second.durability, damage->hardpoint, amount.value());
            }
            // WCC-54/82: raw scripted damage is tracked even without the optional damage rules.
            impl_->track_damage(unit->second, hull_before, shields_before);
            if (outcome.destroyed_hardpoint) {
                events.push_back(destruction_event(
                    tick, EventKind::hardpoint_destroyed, unit->second.state, *outcome.destroyed_hardpoint));
            }
            if (outcome.unit_destroyed) {
                events.push_back(destruction_event(tick, EventKind::unit_destroyed, unit->second.state, 0, command.key.player_id));
                killed.push_back(unit->second.state);
                // A tracked unit leaving its layer rebuilds it (AV-03): a later order this tick
                // plans without it (AV-15).
                if (const auto* footprint = table.avoidance ? tracked_footprint(table, unit->second.state.type_id) : nullptr) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        rebuilt[*layer] = true;
                        if (tracking) tracking->views[*layer].reset();
                    } else if (footprint->layer == SpaceLayer::static_object && tracking) {
                        tracking->statics.reset();
                    }
                    if (tracking) tracking->samples.erase(unit_id);
                }
                impl_->adjust_owned(unit->second.state, false);
                staged.erase(unit);
            }
        }
        if (!squadron_group.empty()) {
            if (auto spread = spread_squadrons(squadron_group); !spread) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                    command_context(command.key) + ": " + spread.error().message));
            }
        }
        if (!group.empty()) {
            const auto planned = plan_group(command, group);
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

    const bool has_hunt = std::any_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(),
        [](const UnitAbilityProfile& profile) { return ability_slot(profile, AbilityKind::hunt).has_value(); });
    if (has_hunt) {
        // WAB-50..56: immutable inputs, partitioned destination decisions, ordered move commit.
        // The serial pass captures pointers only; all per-hunter service work runs in the workers.
        std::vector<const LiveUnit*> hunt_inputs;
        hunt_inputs.reserve(staged.size());
        for (const auto& [id, live] : staged) { static_cast<void>(id); hunt_inputs.push_back(&live); }
        struct HuntMove { EntityId unit{}; math::Vec3 destination{}; };
        std::vector<std::optional<HuntMove>> hunt_moves(hunt_inputs.size());
        std::vector<std::optional<core::Diagnostic>> hunt_errors(hunt_inputs.size());
        const auto& membership = craft_prep_->craft_squadron.value();
        const auto selected = executor.execute_phase("hunt", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, hunt_inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& hunter = *hunt_inputs[index];
                if (!hunter.abilities) continue;
                const auto* profile = impl_->abilities.find(hunter.state.type_id);
                const auto slot = ability_slot(*profile, AbilityKind::hunt);
                if (!slot || !hunter.abilities->slots[*slot].active) continue;
                const auto& active = hunter.abilities->slots[*slot];
                if ((tick - active.started_tick) % 30 != 0) continue;
                EntityId mover = hunter.state.entity_id;
                const auto member = membership.find(mover);
                const auto container = member != membership.end() ? member->second : mover;
                const auto mind = minds.find(container);
                if (mind != minds.end()) {
                    EntityId leader{};
                    for (const auto id : mind->second.roster) {
                        if (staged.find(id) != staged.end()) { leader = id; break; }
                    }
                    if (mover != leader) continue;
                    // WAB-53 / FM-24: an arrived escort holds a fighter-idle cell;
                    // the order's escort mode alone does not mean it is moving.
                    const bool idle = mind->second.mode == SquadronMode::idle
                        || (mind->second.mode == SquadronMode::escort && mind->second.idle_cell.has_value());
                    if (!idle || mind->second.target != invalid_entity_id) continue;
                    mover = container;
                } else if (!hunter.motion || hunter.motion->kind != MotionKind::none || hunter.formation) continue;
                const auto flight = crafts.find(hunter.state.entity_id);
                if (flight != crafts.end() && (flight->second.chase != invalid_entity_id || flight->second.flipping)) continue;
                if ((hunter.combat && hunter.combat->attack_target != invalid_entity_id)
                    || impl_->ability_gate(hunter, tick).in_nebula || std::any_of(arrived_squadrons.begin(), arrived_squadrons.end(), [&](const Squadron& value) { return value.container == container; })
                    || arrivals.contains(container)) continue;
                const auto player = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(),
                    [&](const Player& value) { return value.player_id == hunter.state.owner; });
                const auto player_index = static_cast<std::size_t>(player - impl_->setup.players.begin());
                const auto fogged = [&](const math::Vec3 point) {
                    return impl_->fog && !impl_->fog->revealed(player_index, point);
                };
                std::vector<HuntEnemy> enemies;
                for (const auto* candidate : hunt_inputs) {
                    if (!players_hostile(impl_->snapshot_players, hunter.state.owner, candidate->state.owner)) continue;
                    const auto* footprint = impl_->motion.footprint(candidate->state.type_id);
                    if (!candidate->motion && crafts.find(candidate->state.entity_id) == crafts.end()
                        && !(footprint && footprint->locomotor)) continue;
                    const auto* enemy_profile = impl_->abilities.find(candidate->state.type_id);
                    enemies.push_back({candidate->state.position, fogged(candidate->state.position), enemy_profile && enemy_profile->force_sensitive});
                    if (enemies.size() == 128) break;
                }
                // WAB-54: a craft's authored range applies even when it has no REVEAL service.
                const auto reveal_range = profile->hunt_reveal_range;
                CombatRandom random(impl_->setup.seed, tick, hunter.state.entity_id, 0xfffe0006U); // coordinator-reserved, WAB-54
                auto destination = hunt_destination(hunter.state.position, impl_->economy.bounds, reveal_range,
                    profile->force_sensitive, enemies, fogged, random);
                if (!destination) hunt_errors[index] = destination.error();
                else hunt_moves[index] = HuntMove{mover, destination.value()};
            }
        });
        if (!selected) return selected;
        for (std::size_t index = 0; index < hunt_moves.size(); ++index) {
            if (hunt_errors[index]) return unit_failure(hunt_inputs[index]->state.entity_id, *hunt_errors[index]);
            if (!hunt_moves[index]) continue;
            const auto& move = *hunt_moves[index];
            auto& live = staged.at(move.unit);
            const CommandPayload payload = AttackMovePayload{move.destination};
            live.state.order = order_for(payload, tick);
            if (minds.find(move.unit) != minds.end()) {
                apply_squadron_order(minds.at(move.unit), payload, live.state.position, squadron_table);
                if (auto spread = spread_squadrons({move.unit}); !spread) return spread;
            } else {
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                std::optional<std::size_t> layer;
                if (footprint) layer = dynamic_layer_index(footprint->layer);
                if (tracking && layer) {
                    pending.push_back({move.unit, move.destination, std::nullopt, *layer, CommandKey{}, 0, false});
                } else {
                    if (auto planned = impl_->plan_order(live, MovePayload{move.destination}, frame); !planned) return planned;
                    if (layer && live.motion) {
                        if (auto submitted = submit(move.unit, live, *layer, std::nullopt); !submitted) return submitted;
                    }
                }
            }
        }
    }

    // WLU-07: an expired LURE switches off like the button (WAB-11): its squadron's move stops.
    {
        const auto& membership = craft_prep_->craft_squadron.value();
        std::vector<EntityId> stops;
        for (const auto id : tracked_.value().lure_ends.value()) {
            const auto member = membership.find(id);
            stops.push_back(member != membership.end() ? member->second : id);
        }
        std::sort(stops.begin(), stops.end());
        stops.erase(std::unique(stops.begin(), stops.end()), stops.end());
        for (const auto container_id : stops) {
            const auto found = staged.find(container_id);
            if (found == staged.end()) continue;
            auto& live = found->second;
            live.state.order = order_for(StopPayload{}, tick);
            if (minds.find(container_id) != minds.end()) {
                apply_squadron_order(minds.at(container_id), StopPayload{}, live.state.position, squadron_table);
            } else if (live.motion) {
                if (auto stopped = impl_->plan_order(live, StopPayload{}, frame); !stopped) return stopped;
                const auto* footprint = table.avoidance ? tracked_footprint(table, live.state.type_id) : nullptr;
                if (footprint) {
                    if (const auto layer = dynamic_layer_index(footprint->layer)) {
                        if (auto submitted = submit(container_id, live, *layer, std::nullopt); !submitted) return submitted;
                    }
                }
            }
        }
    }

    const bool has_lure = std::any_of(impl_->abilities.profiles.begin(), impl_->abilities.profiles.end(),
        [](const UnitAbilityProfile& profile) { return ability_slot(profile, AbilityKind::lure).has_value(); });
    if (has_lure) {
        // WLU-03..WLU-06: each luring craft's pulse reads the start-of-phase world and lists the
        // hostile top-level objects it lures (partitioned, one disjoint slot per craft); the orders
        // are committed serially in ascending craft order, so the last craft to pulse wins (WLU-06).
        std::vector<const LiveUnit*> lure_inputs;
        lure_inputs.reserve(staged.size());
        for (const auto& [id, live] : staged) { static_cast<void>(id); lure_inputs.push_back(&live); }
        std::vector<std::vector<EntityId>> lured(lure_inputs.size());
        const auto& membership = craft_prep_->craft_squadron.value();
        const auto top_level = [&](const EntityId id) {
            const auto member = membership.find(id);
            return member != membership.end() ? member->second : id;
        };
        const auto pulsed = executor.execute_phase("lure", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, lure_inputs.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& lurer = *lure_inputs[index];
                if (!lurer.abilities) continue;
                const auto& profile = *impl_->abilities.find(lurer.state.type_id);
                const auto slot = ability_slot(profile, AbilityKind::lure);
                if (!slot || !lurer.abilities->slots[*slot].active) continue;
                const auto& lure = *profile.abilities[*slot].lure;
                if (!lure_pulses(lure, lurer.abilities->slots[*slot].started_tick, tick)) continue;
                auto& out = lured[index];
                for (const auto* candidate : lure_inputs) {
                    // WLU-04: listed exact types within the radius of the craft (3D, inclusive).
                    if (!std::binary_search(lure.target_types.begin(), lure.target_types.end(), candidate->state.type_id)
                        || !within_range(lurer.state.position, candidate->state.position, lure.radius, RangeMetric::spatial)) continue;
                    // WLU-05: a team member stands for its team; only enemies of the craft's owner.
                    const auto target = staged.find(top_level(candidate->state.entity_id));
                    if (target == staged.end()
                        || !players_hostile(impl_->snapshot_players, lurer.state.owner, target->second.state.owner)) continue;
                    out.push_back(target->first);
                }
                std::sort(out.begin(), out.end());
                out.erase(std::unique(out.begin(), out.end()), out.end());
            }
        });
        if (!pulsed) return pulsed;
        for (std::size_t index = 0; index < lured.size(); ++index) {
            const auto lurer = lure_inputs[index]->state.entity_id;
            const auto lurer_team = top_level(lurer);
            const auto* team_profile = staged.find(lurer_team) != staged.end()
                ? impl_->combat.find(staged.at(lurer_team).state.type_id) : nullptr;
            // WCC-25: an invalid target type is never assigned.
            if (team_profile != nullptr && !team_profile->valid_target) continue;
            for (const auto target_id : lured[index]) {
                auto& target = staged.at(target_id);
                const auto mind = minds.find(target_id);
                // WLU-06: an uninterruptible move (a hyperspace arrival) and a team already
                // attacking this craft are left alone. A lone listed object outside a team is not
                // lured (none in the M2 rosters; WLU-10).
                if (arrivals.contains(target_id) || mind == minds.end()
                    || (target.state.order.kind == OrderKind::attack && target.state.order.target == lurer)) continue;
                const CommandPayload attack = AttackPayload{lurer};
                target.state.order = order_for(attack, tick);
                // WSQ-47: the team's destination names the luring craft's team.
                apply_squadron_order(minds.at(target_id), AttackPayload{lurer_team}, target.state.position, squadron_table);
            }
        }
    }

    if (!pending.empty()) {
        if (auto failed = plan_pending()) return unit_failure(failed->first, failed->second);
    }
    const auto first_pad_added = next_id;
    for (const auto& request : pad_requests) {
        Impl::EconomyStage stage{ledgers, arrivals, shares, crafts, minds, arrived_squadrons,
            staging_.value().born_spawners, earners, next_id,
            pads, construction, pad_requests, impl_->pad_stage, impl_->construction_stage, pending_economy_cues_};
        const auto result = impl_->execute_economy(request, tick, staged, stage, events, nullptr, true);
        if (!result) return core::Result<void>::failure(result.error());
    }
    sync_radar_creations(staged, first_pad_added, next_id);
    // PC-08: a sliced search whose unit is gone or no longer waits for it (a new move, face or
    // stop, FM-10) is dropped with its copied views.
    for (auto iterator = impl_->searches.begin(); iterator != impl_->searches.end();) {
        const auto unit = staged.find(iterator->first);
        const bool waits = unit != staged.end() && unit->second.formation && unit->second.formation->sliced
            && unit->second.formation->frame == iterator->second.frame;
        iterator = waits ? std::next(iterator) : impl_->searches.erase(iterator);
    }

    return core::Result<void>::success();
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
        const auto* policy = rules.player(recipient.player);
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

Order session_detail::Tick::order_for(const CommandPayload& payload, const std::uint64_t tick) {
    Order order;
    order.kind = order_kind(payload);
    order.issued_tick = tick;
    if (const auto* move = std::get_if<MovePayload>(&payload)) {
        order.destination = move->destination;
        order.through_hazards = move->through_hazards;
    } else if (const auto* face = std::get_if<FacePayload>(&payload)) {
        order.destination = face->target;
    } else if (const auto* attack = std::get_if<AttackPayload>(&payload)) {
        order.target = attack->target;
        order.hardpoint = attack->hardpoint;
    } else if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) {
        // OP-01: an order that names a unit keeps no point.
        order.target = attack_move->target;
        if (order.target == invalid_entity_id) order.destination = attack_move->destination;
    } else if (const auto* guard = std::get_if<GuardPayload>(&payload)) {
        order.target = guard->target;
        if (order.target == invalid_entity_id) order.destination = guard->destination;
    }
    return order;
}

std::optional<math::Vec3> session_detail::Tick::point_move(const CommandPayload& payload) {
    if (const auto* move = std::get_if<MovePayload>(&payload)) return move->destination;
    if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload);
        attack_move != nullptr && attack_move->target == invalid_entity_id) {
        return attack_move->destination;
    }
    if (const auto* guard = std::get_if<GuardPayload>(&payload); guard != nullptr && guard->target == invalid_entity_id) {
        return guard->destination;
    }
    return std::nullopt;
}

EntityId session_detail::Tick::approach_target_of(const CommandPayload& payload) {
    if (const auto* attack = std::get_if<AttackPayload>(&payload)) return attack->target;
    if (const auto* attack_move = std::get_if<AttackMovePayload>(&payload)) return attack_move->target;
    if (const auto* guard = std::get_if<GuardPayload>(&payload)) return guard->target;
    return invalid_entity_id;
}

} // namespace eawr::sim::tactical
