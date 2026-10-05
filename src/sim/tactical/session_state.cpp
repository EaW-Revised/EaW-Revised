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

#include "session_encoding.hpp"

namespace eawr::sim::tactical {

namespace session_detail {

[[nodiscard]] InstanceDurability durability_status(
    const DurabilityProfile& profile, const DurabilityTable& table, const DurabilityState& state) {
    const auto& rules = table.rules;
    InstanceDurability status;
    if (table.damage && profile.max_shields.raw() > 0) {
        status.shields = state.shields;
        status.max_shields = profile.max_shields;
    }
    status.hull = state.hull;
    status.max_hull = profile.max_hull;
    status.max_speed_factor = max_speed_factor(profile, rules, state);
    if (profile.max_speed) {
        // validate_durability bounds the speed and keeps the factor at most 1, so this fits.
        status.max_speed = math::multiply(*profile.max_speed, status.max_speed_factor).value();
    }
    status.engines_online = engines_online(profile, state);
    status.shields_online = shields_online(profile, state);
    status.launch_ready = launch_ready(profile, state);
    status.hardpoints.reserve(profile.hardpoints.size());
    for (std::size_t index = 0; index < profile.hardpoints.size(); ++index) {
        status.hardpoints.push_back(HardpointStatus{
            profile.hardpoints[index].role,
            hardpoint_state(profile, rules, state, index),
            !hardpoint_destroyed(profile, state, index) && !hardpoint_disabled(state, index),
            state.hardpoints[index],
        });
    }
    return status;
}

} // namespace session_detail

void TacticalSession::Impl::publish_staged(const std::vector<LiveUnit>& live, std::vector<Event> events,
    std::vector<CombatEvent> combat_events) {
        std::vector<UnitState> states;
        states.reserve(live.size());
        for (const auto& unit : live) {
            states.push_back(unit.state);
        }
        const auto field = SensorField::build(setup.players, states, sensors).value();
        std::vector<TacticalInstance> instances;
        instances.reserve(live.size());
        for (const auto& unit : live) {
            auto instance = instance_for(unit, math::to_matrix(unit.state.rotation, unit.state.position).value(), completed_tick);
            instance.visible_to = visible_to(field, fog ? &*fog : nullptr, unit.state.owner, unit.state.position);
            instance.reveal_range = field.reveal_range(unit.state.type_id);
            if (const auto craft = crafts.find(unit.state.entity_id); craft != crafts.end()) {
                instance.craft_velocity_per_frame = craft->second.velocity;
            }
            if (const auto squadron = minds.find(unit.state.entity_id); squadron != minds.end()) {
                instance.squadron_in_idle_grid = squadron->second.idle_cell.has_value();
                const auto& flight = squadron->second;
                instance.has_movement_path = flight.mode == SquadronMode::move || flight.approach
                    || (flight.mode == SquadronMode::escort && !flight.idle_cell);
            }
            instances.push_back(std::move(instance));
        }
        current_snapshot = std::make_shared<const TacticalSnapshot>(completed_tick, snapshot_players,
            std::move(instances), std::move(events), std::move(combat_events), projectiles, outcome,
            std::vector<SpinningCraft>{}, std::vector<SquadronTarget>{}, squadrons, economy_views(ledgers, shares), pad_views(), quits,
            losses, productions, manual_clock_views(), economy_cues, ability_spawns);
    }

std::vector<PadView> TacticalSession::Impl::pad_views() const {
        std::vector<PadView> views;
        views.reserve(pads.size());
        for (const auto& [id, state] : pads) {
            PadView view{id, state};
            if (const auto child = construction.find(state.under_construction); child != construction.end()) view.construction = child->second;
            views.push_back(view);
        }
        return views;
    }

std::vector<FogRevealer> TacticalSession::Impl::revealers(const SensorField& field, const std::span<const UnitState> units,
    const std::span<const EntityId> disabled) {
        std::vector<FogRevealer> result;
        for (const auto& unit : units) {
            if (const auto range = field.reveal_range(unit.type_id);
                range && !std::binary_search(disabled.begin(), disabled.end(), unit.entity_id)) {
                result.push_back(FogRevealer{unit.entity_id, unit.owner, unit.position, *range});
            }
        }
        return result;
    }

std::uint64_t TacticalSession::Impl::visible_to(const SensorField& field, const FogCells* cells, const PlayerId owner,
    const math::Vec3& position) const {
        if (cells == nullptr) {
            return field.visible_to(owner, position);
        }
        const auto team = teams.at(owner);
        std::uint64_t mask = 0;
        for (std::size_t index = 0; index < setup.players.size(); ++index) {
            if (setup.players[index].team_id == team || cells->revealed(index, position)) {
                mask |= std::uint64_t{1} << index;
            }
        }
        return mask;
    }

TacticalInstance TacticalSession::Impl::instance_for(const LiveUnit& unit, const math::Mat3x4& transform, const std::uint64_t now) const {
        TacticalInstance instance{unit.state.entity_id, unit.state.type_id, unit.state.owner,
            teams.at(unit.state.owner), transform, 0, {}, {}, {}};
        if (unit.durability) {
            instance.durability = durability_status(*health_profile(unit), durability, *unit.durability);
        }
        instance.abilities = ability_statuses(unit, now);
        instance.has_movement_path = unit.motion && unit.motion->kind == MotionKind::path;
        instance.in_tractor_beam = unit.in_tractor_beam;
        instance.in_asteroid_field = unit.asteroid_contact.has_value();
        instance.in_nebula = unit.nebula && unit.nebula->present;
        instance.in_ion_storm = durability.damage && unit.durability && in_ion_storm(*durability.damage, *unit.durability, now);
        if (instance.in_ion_storm && instance.durability) instance.durability->shields_online = false;
        if (unit.combat) {
            const auto* profile = combat.find(unit.state.type_id);
            for (std::size_t index = 0; index < unit.combat->weapons.size(); ++index) {
                const auto& weapon = unit.combat->weapons[index];
                if (!weapon.manual || profile == nullptr) continue;
                const auto& manual = *weapon.manual;
                instance.manual_weapons.push_back({profile->weapons[index].hardpoint, manual.target,
                    manual.requesting_player, manual.assigned_frame, manual.yaw, manual.pitch});
            }
        }
        if (ion_stunned(unit.ion_stun, now)) {
            instance.ion_stun_frames = static_cast<std::uint32_t>(unit.ion_stun->end_frame - now);
        }
        return instance;
    }

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes() const {
        const auto units = sorted_live();
        std::vector<std::uint8_t> bytes;
        bytes.reserve(120 + setup.players.size() * detail::player_record_size
            + units.size() * (detail::unit_record_size + detail::order_record_size));
        bytes.insert(bytes.end(), state_magic.begin(), state_magic.end());
        sim::detail::append_u32(bytes, tactical_state_encoding_version);
        sim::detail::append_u32(bytes, tactical_rules_version);
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, math::Fixed::fractional_bits);
        sim::detail::append_u32(bytes, tick_numerator);
        sim::detail::append_u32(bytes, tick_denominator);
        sim::detail::append_u64(bytes, completed_tick);
        sim::detail::append_u64(bytes, next_id);
        sim::detail::append_u64(bytes, rng_state);
        bytes.insert(bytes.end(), setup.content_identity.begin(), setup.content_identity.end());
        sim::detail::append_u64(bytes, setup.players.size());
        for (const auto& player : setup.players) {
            detail::append_player(bytes, player);
        }
        sim::detail::append_u64(bytes, units.size());
        for (const auto& unit : units) {
            detail::append_unit_record(bytes, unit.state);
            detail::append_order(bytes, unit.state.order);
            if (unit.state.barrage_source != invalid_entity_id) {
                bytes.insert(bytes.end(), {'B', 'P', 'R', 'X'});
                sim::detail::append_u64(bytes, unit.state.barrage_source);
            }
            // A durable unit appends its health; any other unit encodes exactly as before #72.
            if (unit.durability) {
                append_fixed(bytes, unit.durability->hull);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(unit.durability->hardpoints.size()));
                sim::detail::append_u32(bytes, 0);
                for (const auto health : unit.durability->hardpoints) {
                    append_fixed(bytes, health);
                }
                // With damage rules (#74): the shield and the depletion and last-hit frames; since
                // #361 the energy pool and the next shield and energy recharge frames.
                if (durability.damage) {
                    append_fixed(bytes, unit.durability->shields);
                    for (const auto& frame : {unit.durability->depleted_frame, unit.durability->last_hit_frame}) {
                        sim::detail::append_u32(bytes, frame ? 1U : 0U);
                        sim::detail::append_u64(bytes, frame.value_or(0));
                    }
                    append_fixed(bytes, unit.durability->energy);
                    sim::detail::append_u64(bytes, unit.durability->next_shield_frame);
                    sim::detail::append_u64(bytes, unit.durability->next_energy_frame);
                }
            }
            // A unit with a motion profile (#70) then appends its motion record, and with
            // avoidance rules (#71) its plan's nodes.
            if (unit.motion) {
                append_motion(bytes, *unit.motion);
                if (motion.avoidance) {
                    append_nodes(bytes, *unit.motion);
                }
            }
            // A unit with a combat profile (#73) then appends its combat record.
            if (unit.combat) {
                detail::append_combat(bytes, *unit.combat);
            }
        }
        std::uint64_t manual_weapons = 0;
        for (const auto& unit : units) {
            if (!unit.combat) continue;
            for (const auto& weapon : unit.combat->weapons) manual_weapons += weapon.manual ? 1 : 0;
        }
        // WAD-39/40: MANN exists only with manual weapons/clocks; all old sessions keep their bytes.
        if (manual_weapons != 0 || !manual_clocks.empty()) {
            bytes.insert(bytes.end(), {'M', 'A', 'N', 'N'});
            sim::detail::append_u64(bytes, manual_weapons);
            for (const auto& unit : units) {
                if (!unit.combat) continue;
                for (std::uint32_t index = 0; index < unit.combat->weapons.size(); ++index) {
                    const auto& weapon = unit.combat->weapons[index];
                    if (!weapon.manual) continue;
                    const auto& state = *weapon.manual;
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    sim::detail::append_u32(bytes, index);
                    sim::detail::append_u32(bytes, state.requesting_player);
                    sim::detail::append_u64(bytes, state.target);
                    sim::detail::append_u64(bytes, state.assigned_frame);
                    for (const auto angle : {state.yaw, state.pitch, state.desired_yaw, state.desired_pitch}) append_fixed(bytes, angle);
                }
            }
            sim::detail::append_u64(bytes, manual_clocks.size());
            for (const auto& [player, clock] : manual_clocks) {
                sim::detail::append_u32(bytes, player);
                sim::detail::append_u32(bytes, clock.cooldown_frames);
                sim::detail::append_u64(bytes, clock.last_fired_frame);
            }
        }
        // Squadron membership (#271) and fog cells (#274) follow as tagged blocks, only when
        // present, so a session without them hashes exactly as before.
        if (!squadrons.empty()) {
            bytes.insert(bytes.end(), squadron_tag.begin(), squadron_tag.end());
            sim::detail::append_u64(bytes, squadrons.size());
            for (const auto& squadron : squadrons) {
                sim::detail::append_u64(bytes, squadron.container);
                sim::detail::append_u64(bytes, squadron.members.size());
                for (const auto member : squadron.members) {
                    sim::detail::append_u64(bytes, member);
                }
            }
        }
        if (fog) {
            bytes.insert(bytes.end(), fog_tag.begin(), fog_tag.end());
            fog->append_state(bytes);
        }
        if (motion.avoidance) {
            bytes.insert(bytes.end(), tracking_tag.begin(), tracking_tag.end());
            for (const auto anchor : tracking_anchor) {
                sim::detail::append_u64(bytes, anchor);
            }
        }
        // Rolled units (#351): only those whose roll is not zero, so a session in which no unit
        // ever banked hashes as before.
        std::uint64_t rolled = 0;
        for (const auto& unit : units) {
            rolled += unit.roll.raw() != 0 ? 1 : 0;
        }
        if (rolled != 0) {
            bytes.insert(bytes.end(), roll_tag.begin(), roll_tag.end());
            sim::detail::append_u64(bytes, rolled);
            for (const auto& unit : units) {
                if (unit.roll.raw() != 0) {
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    append_fixed(bytes, unit.roll);
                }
            }
        }
        // Projectiles in flight (#74), with damage rules only.
        if (durability.damage) {
            bytes.insert(bytes.end(), projectile_tag.begin(), projectile_tag.end());
            sim::detail::append_u64(bytes, next_projectile);
            sim::detail::append_u64(bytes, projectiles.size());
            for (const auto& projectile : projectiles) {
                detail::append_projectile(bytes, projectile);
            }
        }
        // Waiting group members (#344), then ships waiting for a sliced search (PC-08), each
        if (!pending_blast_damage.empty()) {
            constexpr std::array<std::uint8_t, 4> blast_tag{'B', 'L', 'S', 'T'};
            bytes.insert(bytes.end(), blast_tag.begin(), blast_tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, pending_blast_damage.size());
            for (const auto& queued : pending_blast_damage) {
                sim::detail::append_u64(bytes, queued.due);
                sim::detail::append_u64(bytes, queued.recipient.id);
                append_fixed(bytes, queued.recipient.amount);
                append_fixed(bytes, queued.recipient.delay);
                sim::detail::append_u32(bytes, queued.recipient.hardpoint);
                detail::append_projectile(bytes, queued.source);
            }
        }
        // Waiting group members (#344), then ships waiting for a sliced search (PC-08), each
        // ascending ID, only while any wait.
        for (const bool sliced : {false, true}) {
            std::vector<std::pair<EntityId, const FormationWait*>> waits;
            for (const auto& unit : units) {
                if (unit.formation && unit.formation->sliced == sliced) waits.emplace_back(unit.state.entity_id, &*unit.formation);
            }
            if (waits.empty()) continue;
            const auto& tag = sliced ? sliced_tag : formation_tag;
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, waits.size());
            for (const auto& [id, wait] : waits) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, wait->frame);
                append_fixed(bytes, wait->destination.x);
                append_fixed(bytes, wait->destination.y);
                append_fixed(bytes, wait->destination.z);
                append_fixed(bytes, wait->max_speed);
                sim::detail::append_u64(bytes, wait->order.tick);
                sim::detail::append_u32(bytes, wait->order.player_id);
                sim::detail::append_u64(bytes, wait->order.sequence);
                sim::detail::append_u32(bytes, wait->rank);
            }
        }
        // Approach mappings (#452), ascending ID, only while any unit has one.
        std::vector<std::pair<EntityId, const Approach*>> approaches;
        for (const auto& unit : units) {
            if (unit.approach) approaches.emplace_back(unit.state.entity_id, &*unit.approach);
        }
        if (!approaches.empty()) {
            bytes.insert(bytes.end(), approach_tag.begin(), approach_tag.end());
            sim::detail::append_u64(bytes, approaches.size());
            for (const auto& [id, approach] : approaches) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, approach->prediction_frame);
            }
        }
        // Squadron flight, orders and hangars (#75), each only when present.
        if (!crafts.empty()) {
            bytes.insert(bytes.end(), craft_tag.begin(), craft_tag.end());
            sim::detail::append_u64(bytes, crafts.size());
            for (const auto& [id, craft] : crafts) {
                append_craft(bytes, id, craft);
            }
        }
        if (!minds.empty()) {
            bytes.insert(bytes.end(), squadron_state_tag.begin(), squadron_state_tag.end());
            sim::detail::append_u64(bytes, minds.size());
            for (const auto& [container, state] : minds) {
                static_cast<void>(container);
                append_squadron_state(bytes, state);
            }
        }
        if (!spawners.empty()) {
            bytes.insert(bytes.end(), hangar_tag.begin(), hangar_tag.end());
            sim::detail::append_u64(bytes, spawners.size());
            for (const auto& [id, state] : spawners) {
                append_spawner(bytes, id, state);
            }
        }
        if (!collection.empty()) {
            bytes.insert(bytes.end(), collection_tag.begin(), collection_tag.end());
            collection.append_state(bytes);
        }
        if (projectile_collection.has_history()) {
            bytes.insert(bytes.end(), projectile_collection_tag.begin(), projectile_collection_tag.end());
            projectile_collection.append_state(bytes);
        }
        // Skirmish purchasing (#530): the ledgers with economy rules, arrivals and population
        // shares while any; a session without an economy hashes as before.
        if (!economy.empty()) {
            bytes.insert(bytes.end(), economy_tag.begin(), economy_tag.end());
            sim::detail::append_u64(bytes, ledgers.size());
            for (const auto& ledger : ledgers) {
                append_ledger(bytes, ledger);
            }
        }
        if (!arrivals.empty()) {
            bytes.insert(bytes.end(), arrival_tag.begin(), arrival_tag.end());
            sim::detail::append_u64(bytes, arrivals.size());
            for (const auto& [id, arrival] : arrivals) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, arrival.frame);
                sim::detail::append_u32(bytes, 0);
                append_vec3(bytes, arrival.exit);
                append_vec3(bytes, arrival.direction);
            }
        }
        if (!pads.empty() || !construction.empty() || !respawns.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'P', 'A', 'D', 'S'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, pads.size());
            for (const auto& [id, state] : pads) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, state.target);
                sim::detail::append_u32(bytes, state.contents_locked ? 1U : 0U);
                sim::detail::append_i64(bytes, state.progress.raw());
                sim::detail::append_u64(bytes, state.under_construction);
                sim::detail::append_u64(bytes, state.constructed);
                sim::detail::append_u64(bytes, state.cooldown_until);
                sim::detail::append_u64(bytes, state.cooldown_start);
            }
            sim::detail::append_u64(bytes, construction.size());
            for (const auto& [id, state] : construction) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, state.parent);
                sim::detail::append_u32(bytes, state.builder);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, state.finish_frame);
                sim::detail::append_i64(bytes, state.increment.raw());
                sim::detail::append_u64(bytes, state.start_frame);
            }
            sim::detail::append_u64(bytes, respawns.size());
            for (const auto& [due, batch] : respawns) {
                sim::detail::append_u64(bytes, due);
                sim::detail::append_u64(bytes, batch.objects.size());
                for (const auto& request : batch.objects) {
                    sim::detail::append_u64(bytes, request.type);
                    sim::detail::append_u32(bytes, request.owner);
                    sim::detail::append_u32(bytes, 0);
                    append_vec3(bytes, request.position);
                    for (const auto component : {request.rotation.x, request.rotation.y, request.rotation.z, request.rotation.w}) {
                        append_fixed(bytes, component);
                    }
                }
            }
        }
        if (!shares.empty()) {
            bytes.insert(bytes.end(), population_tag.begin(), population_tag.end());
            sim::detail::append_u64(bytes, shares.size());
            for (const auto& [id, share] : shares) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, share.owner);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_i64(bytes, share.share);
            }
        }
        // WR-41: AVUL v1, coordinator-reserved. Only active independent defense timers;
        // the initial records and sessions without arrivals retain their existing encoding.
        const auto vulnerable = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.arrival_vulnerable_until.has_value();
        });
        if (vulnerable != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'A', 'V', 'U', 'L'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(vulnerable));
            for (const auto& unit : units) {
                if (!unit.arrival_vulnerable_until) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.arrival_vulnerable_until);
            }
        }
        // WPR-02/22/51/52: UPGD v1, coordinator-reserved. Omitted for empty upgrade state.
        const auto modified = [](const LiveUnit& unit) {
            return unit.upgrade_bonuses != CombatBonuses{} || (unit.durability
                && (std::any_of(unit.durability->disabled.begin(), unit.durability->disabled.end(), [](const bool value) { return value; })
                    || std::any_of(unit.durability->repairing_players.begin(), unit.durability->repairing_players.end(),
                        [](const auto& players) { return !players.empty(); })));
        };
        const auto account_state = [&](const PlayerEconomy& account) {
            const auto* initial = economy.player(account.player);
            return (initial != nullptr && account.tech_level != initial->start_tech) || !account.completed.empty()
                || std::any_of(account.lifetime.begin(), account.lifetime.end(), [&](const auto& built) {
                    return economy.upgrade(built.first) != nullptr && built.second != 0;
                });
        };
        const auto modified_count = std::count_if(units.begin(), units.end(), modified);
        const auto account_count = std::count_if(ledgers.begin(), ledgers.end(), account_state);
        if (modified_count != 0 || account_count != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'U', 'P', 'G', 'D'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1); sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(account_count));
            for (const auto& account : ledgers) {
                if (!account_state(account)) continue;
                sim::detail::append_u32(bytes, account.player); sim::detail::append_u32(bytes, account.tech_level);
                sim::detail::append_u64(bytes, account.lifetime.size());
                for (const auto& [type, count] : account.lifetime) {
                    sim::detail::append_u64(bytes, type); sim::detail::append_u64(bytes, count);
                }
                sim::detail::append_u64(bytes, account.completed.size());
                for (const auto& held : account.completed) {
                    sim::detail::append_u64(bytes, held.type); sim::detail::append_u64(bytes, held.station);
                    sim::detail::append_u64(bytes, held.object);
                }
            }
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(modified_count));
            for (const auto& unit : units) {
                if (!modified(unit)) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                for (const auto percentage : unit.upgrade_bonuses) append_fixed(bytes, percentage);
                sim::detail::append_u64(bytes, unit.durability ? unit.durability->hardpoints.size() : 0);
                if (!unit.durability) continue;
                for (std::size_t slot = 0; slot < unit.durability->hardpoints.size(); ++slot) {
                    sim::detail::append_u32(bytes, hardpoint_disabled(*unit.durability, slot) ? 1U : 0U);
                    const auto* repairing = slot < unit.durability->repairing_players.size()
                        ? &unit.durability->repairing_players[slot] : nullptr;
                    sim::detail::append_u32(bytes, repairing != nullptr ? static_cast<std::uint32_t>(repairing->size()) : 0U);
                    if (repairing != nullptr) for (const auto player : *repairing) sim::detail::append_u32(bytes, player);
                }
            }
        }
        // WBP-44..46: IMOD v1 is omitted when no modifier object has state.
        // Active ownership remains in UPGD; reverse termination effects live only here.
        std::uint64_t modifier_objects = 0;
        for (const auto& account : ledgers) {
            for (const auto& held : account.completed) modifier_objects += !held.income_modifiers.empty() ? 1U : 0U;
            for (const auto& held : account.income_residuals) modifier_objects += !held.income_modifiers.empty() ? 1U : 0U;
        }
        if (modifier_objects != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'I', 'M', 'O', 'D'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1); sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, modifier_objects);
            const auto append = [&](const PlayerId owner, const CompletedBuild& held, const bool residual) {
                if (held.income_modifiers.empty()) return;
                sim::detail::append_u32(bytes, owner); sim::detail::append_u32(bytes, residual ? 1U : 0U);
                sim::detail::append_u64(bytes, held.type); sim::detail::append_u64(bytes, held.station);
                sim::detail::append_u64(bytes, held.object); sim::detail::append_u64(bytes, held.income_modifiers.size());
                for (const auto& state : held.income_modifiers) {
                    sim::detail::append_u32(bytes, state.initialized ? 1U : 0U); sim::detail::append_u32(bytes, 0);
                    sim::detail::append_u64(bytes, state.next_scan_frame); sim::detail::append_u64(bytes, state.attached.size());
                    for (const auto stream : state.attached) sim::detail::append_u64(bytes, stream);
                }
            };
            for (const auto& account : ledgers) {
                for (const auto& held : account.completed) append(account.player, held, false);
                for (const auto& held : account.income_residuals) append(account.player, held, true);
            }
        }
        // The decided battle (#77); an undecided session hashes as before.
        if (outcome) {
            append_outcome(bytes, *outcome);
        }
        // Unit abilities (#76), ascending ID, only when a live unit's type has abilities.
        std::uint64_t able = 0;
        for (const auto& unit : units) able += unit.abilities ? 1 : 0;
        if (able != 0) {
            bytes.insert(bytes.end(), ability_tag.begin(), ability_tag.end());
            sim::detail::append_u64(bytes, able);
            for (const auto& unit : units) {
                if (!unit.abilities) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_abilities(bytes, *unit.abilities);
            }
        }
        // Ion stuns (#561), ascending ID, only while a live unit has one.
        std::uint64_t stunned = 0;
        for (const auto& unit : units) stunned += unit.ion_stun ? 1 : 0;
        if (stunned != 0) {
            bytes.insert(bytes.end(), ion_stun_tag.begin(), ion_stun_tag.end());
            sim::detail::append_u64(bytes, stunned);
            for (const auto& unit : units) {
                if (!unit.ion_stun) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_ion_stun(bytes, *unit.ion_stun);
            }
        }
        const auto hazard_moves = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.state.order.through_hazards;
        });
        if (hazard_moves != 0) {
            bytes.insert(bytes.end(), hazard_move_tag.begin(), hazard_move_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(hazard_moves));
            for (const auto& unit : units) if (unit.state.order.through_hazards) sim::detail::append_u64(bytes, unit.state.entity_id);
        }
        const auto contacts = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.asteroid_contact.has_value();
        });
        if (contacts != 0) {
            bytes.insert(bytes.end(), asteroid_tag.begin(), asteroid_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(contacts));
            for (const auto& unit : units) {
                if (!unit.asteroid_contact) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.asteroid_contact);
            }
        }
        const auto disabled_engines = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.durability && unit.durability->engines_disabled_until;
        });
        if (disabled_engines != 0) {
            bytes.insert(bytes.end(), engine_disable_tag.begin(), engine_disable_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(disabled_engines));
            for (const auto& unit : units) {
                if (!unit.durability || !unit.durability->engines_disabled_until) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.durability->engines_disabled_until);
            }
        }
        const auto nebulas = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) { return unit.nebula.has_value(); });
        if (nebulas != 0) {
            bytes.insert(bytes.end(), nebula_tag.begin(), nebula_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(nebulas));
            for (const auto& unit : units) {
                if (!unit.nebula) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                bytes.push_back(unit.nebula->present ? 1 : 0);
                bytes.push_back(unit.nebula->frame ? 1 : 0);
                sim::detail::append_u64(bytes, unit.nebula->frame.value_or(0));
            }
        }
        const auto storms = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.durability && unit.durability->ion_storm_contact;
        });
        if (storms != 0) {
            bytes.insert(bytes.end(), storm_tag.begin(), storm_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(storms));
            for (const auto& unit : units) {
                if (!unit.durability || !unit.durability->ion_storm_contact) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, *unit.durability->ion_storm_contact);
            }
        }
        if (!command_ledger.sources.empty()) {
            // Coordinator-reserved HBON; station-only sessions keep their exact encoding.
            constexpr std::array<std::uint8_t, 4> tag{'H', 'B', 'O', 'N'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, command_ledger.sources.size());
            for (const auto& source : command_ledger.sources) {
                sim::detail::append_u64(bytes, source.id);
                sim::detail::append_u64(bytes, source.host);
                const auto& profile = economy.command_bonuses[source.profile];
                sim::detail::append_u64(bytes, profile.type);
                sim::detail::append_u32(bytes, profile.slot);
                sim::detail::append_u64(bytes, source.targets.size());
                for (const auto target : source.targets) sim::detail::append_u64(bytes, target);
            }
        }
        // Killed craft spinning away (#447); a session without any hashes as before.
        const auto heroes = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.state.purchase_type != 0 || !unit.state.contained.empty();
        });
        if (heroes != 0) {
            // coordinator-reserved HERO: absent in sessions without converted purchases.
            constexpr std::array<std::uint8_t, 4> tag{'H', 'E', 'R', 'O'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(heroes));
            const auto append_rider = [&](auto&& self, const CarriedObject& rider) -> void {
                sim::detail::append_u64(bytes, rider.entity_id);
                sim::detail::append_u64(bytes, rider.type_id);
                sim::detail::append_u64(bytes, rider.parent);
                const auto flags = (rider.named_hero ? 1U : 0U) | (rider.generic_hero ? 2U : 0U)
                    | (rider.limbo ? 4U : 0U) | (rider.model_visible ? 8U : 0U)
                    | (rider.collidable ? 16U : 0U) | (rider.selected ? 32U : 0U)
                    | (rider.movement_coordinated ? 64U : 0U) | (rider.combat_preserved ? 128U : 0U);
                bytes.push_back(static_cast<std::uint8_t>(flags));
                sim::detail::append_u64(bytes, rider.members.size());
                for (const auto& member : rider.members) self(self, member);
            };
            for (const auto& unit : units) {
                if (unit.state.purchase_type == 0 && unit.state.contained.empty()) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.state.purchase_type);
                sim::detail::append_u64(bytes, unit.state.contained.size());
                for (const auto& rider : unit.state.contained) append_rider(append_rider, rider);
            }
        }
        const auto special_owners = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && !unit.abilities->special.slots.empty();
        });
        if (special_owners != 0) {
            // Coordinator-reserved SPAB, after HERO and before SPIN, in stable owner order.
            constexpr std::array<std::uint8_t, 4> tag{'S', 'P', 'A', 'B'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(special_owners));
            for (const auto& unit : units) {
                if (!unit.abilities || unit.abilities->special.slots.empty()) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_special_abilities(bytes, unit.abilities->special);
            }
        }
        const auto concentrate_owners = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && !unit.abilities->concentrate_recruits.empty();
        });
        if (concentrate_owners != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'C', 'F', 'I', 'R'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(concentrate_owners));
            for (const auto& unit : units) {
                if (!unit.abilities || unit.abilities->concentrate_recruits.empty()) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.abilities->concentrate_recruits.size());
                for (const auto id : unit.abilities->concentrate_recruits) sim::detail::append_u64(bytes, id);
            }
        }
        if (!ability_spawns.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'H', 'A', 'B', 'L'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, ability_spawns.size());
            for (const auto& spawn : ability_spawns) {
                sim::detail::append_u64(bytes, spawn.id);
                sim::detail::append_u64(bytes, spawn.type);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(spawn.kind));
                sim::detail::append_u32(bytes, spawn.owner);
                sim::detail::append_u64(bytes, spawn.source);
                append_vec3(bytes, spawn.position);
                append_fixed(bytes, spawn.rotation.x);
                append_fixed(bytes, spawn.rotation.y);
                append_fixed(bytes, spawn.rotation.z);
                append_fixed(bytes, spawn.rotation.w);
                sim::detail::append_u64(bytes, spawn.due);
                sim::detail::append_u32(bytes, spawn.detonated ? 1 : 0);
                sim::detail::append_u64(bytes, spawn.recipients.size());
                for (const auto& recipient : spawn.recipients) {
                    sim::detail::append_u64(bytes, recipient.target);
                    sim::detail::append_u64(bytes, recipient.expires);
                }
            }
        }
        if (!spins.empty()) {
            bytes.insert(bytes.end(), spin_tag.begin(), spin_tag.end());
            sim::detail::append_u64(bytes, spins.size());
            for (const auto& spin : spins) {
                append_spin(bytes, spin);
            }
        }
        if (!quits.empty()) {
            // coordinator-reserved QUIT: absent status keeps all existing state pins.
            constexpr std::array<std::uint8_t, 4> tag{'Q', 'U', 'I', 'T'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, quits.size());
            for (const auto& quit : quits) {
                sim::detail::append_u32(bytes, quit.player);
                sim::detail::append_u64(bytes, quit.tick);
            }
        }
        // coordinator-reserved PTOK: counters remain authoritative after the last pool entry leaves.
        const auto token_accounts = std::count_if(ledgers.begin(), ledgers.end(), [](const auto& ledger) {
            return ledger.next_pool_token != 1;
        });
        const auto token_units = std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.state.purchase_token != 0;
        });
        if (token_accounts != 0 || token_units != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'P', 'T', 'O', 'K'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(token_accounts));
            for (const auto& ledger : ledgers) {
                if (ledger.next_pool_token == 1) continue;
                sim::detail::append_u32(bytes, ledger.player);
                sim::detail::append_u64(bytes, ledger.next_pool_token);
                sim::detail::append_u64(bytes, ledger.pool_tokens.size());
                for (const auto token : ledger.pool_tokens) sim::detail::append_u64(bytes, token);
            }
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(token_units));
            for (const auto& unit : units) {
                if (unit.state.purchase_token == 0) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.state.purchase_token);
            }
        }
        return bytes;
    }

} // namespace eawr::sim::tactical
