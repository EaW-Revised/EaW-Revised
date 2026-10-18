#include "eawr/core/load_profile.hpp"
#include "eawr/core/tick_profile.hpp"
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
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "session_encoding.hpp"
#include "session_canonical.hpp"
#include "checkpoint_fields_internal.hpp"

namespace eawr::sim::tactical {

core::Result<std::vector<std::uint8_t>> TacticalSession::save_checkpoint_state() const {
    try {
        checkpoint_detail::Writer out;
        out(std::uint32_t{9U}); // Includes retained squadron destination hardpoints.
        auto live = impl_->sorted_live();
        checkpoint_detail::Access::state(out, std::as_const(*impl_));
        out(live);
        out(checkpoint_detail::Access::visibility(*impl_));
        out(static_cast<std::uint64_t>(impl_->searches.size()));
        for (const auto& [id, search] : impl_->searches) {
            out(id, search.search.checkpoint(), search.frame, search.target, search.max_speed, search.inputs, search.plan);
        }
        out(encode_retreat_state(impl_->retreat_state), impl_->midbattle_solve);
        return core::Result<std::vector<std::uint8_t>>::success(std::move(out.bytes));
    } catch (const checkpoint_detail::Invalid& error) {
        return core::Result<std::vector<std::uint8_t>>::failure(detail::diagnostic(diagnostic_codes::resource_limit, error.what()));
    }
}

core::Result<void> TacticalSession::load_checkpoint_state(const std::span<const std::uint8_t> bytes) {
    if (impl_->object_turn_running) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "load_checkpoint_state during an object-turn provider call"));
    }
    try {
        checkpoint_detail::Reader in(bytes);
        std::uint32_t version{};
        in(version);
        if (version != (9U)) throw checkpoint_detail::Invalid("unsupported child state domain");
        const auto cursors = impl_->last_submitted;
        checkpoint_detail::Access::state(in, *impl_);
        if (impl_->last_submitted != cursors) throw checkpoint_detail::Invalid("command cursors disagree with sealed log");
        if (impl_->committed_bonus_profiles.size() != impl_->ownership_keys.size())
            throw checkpoint_detail::Invalid("bonus profile census disagrees with content");
        for (const auto& source : impl_->command_ledger.sources)
            if (source.profile >= impl_->economy.command_bonuses.size()) throw checkpoint_detail::Invalid("invalid command bonus profile");
        for (const auto& [id, sources] : impl_->command_ledger.recipients) {
            static_cast<void>(id);
            for (const auto source : sources) if (source >= impl_->command_ledger.sources.size())
                throw checkpoint_detail::Invalid("invalid command bonus recipient");
        }
        std::vector<LiveUnit> live;
        in(live);
        checkpoint_detail::Access::PublishedVisibility visibility;
        in(visibility);
        if (impl_->completed_tick > max_ticks || live.size() > max_units) throw checkpoint_detail::Invalid("state exceeds tactical limits");
        EntityId previous = 0;
        for (auto& unit : live) {
            if (unit.state.entity_id <= previous || (impl_->next_id != 0 && unit.state.entity_id >= impl_->next_id)
                || !std::binary_search(impl_->setup.players.begin(), impl_->setup.players.end(), unit.state.owner,
                    [](const auto& a, const auto& b) {
                        if constexpr (std::is_same_v<std::decay_t<decltype(a)>, Player>) return a.player_id < b;
                        else return a < b.player_id;
                    })) throw checkpoint_detail::Invalid("invalid live unit identity or owner");
            previous = unit.state.entity_id;
            if (!math::to_matrix(unit.state.rotation, unit.state.position)) throw checkpoint_detail::Invalid("invalid live unit transform");
            if (unit.durability) {
                const auto* profile = impl_->health_profile(unit);
                if (!profile || unit.durability->hardpoints.size() != profile->hardpoints.size())
                    throw checkpoint_detail::Invalid("durability binding disagrees with content");
            }
            if (unit.abilities) {
                const auto* profile = impl_->abilities.find(unit.state.type_id);
                if (!profile || unit.abilities->slots.size() != profile->abilities.size()
                    || unit.abilities->special.slots.size() != profile->special.size())
                    throw checkpoint_detail::Invalid("ability binding disagrees with content");
            }
            if (unit.combat) {
                const auto* profile = impl_->combat.find(unit.state.type_id);
                if (!profile || unit.combat->weapons.size() != profile->weapons.size())
                    throw checkpoint_detail::Invalid("combat binding disagrees with content");
            }
        }
        if (impl_->setup.campaign) {
            auto derived = impl_->update_command_ledger(live, impl_->next_id, InlineExecutor{});
            if (!derived || derived.value().battlefield_defence != impl_->command_ledger.battlefield_defence
                || derived.value().reveal_multipliers != impl_->command_ledger.reveal_multipliers)
                throw checkpoint_detail::Invalid("campaign player modifier ledger disagrees with its live sources");
            for(std::size_t i=0;i<impl_->last_fog_multipliers.size();++i) if(impl_->last_fog_multipliers[i]<math::Fixed{}
                || (i>=impl_->setup.players.size() && impl_->last_fog_multipliers[i]!=math::Fixed{})) throw checkpoint_detail::Invalid("invalid last-fog multiplier cache");
            impl_->fog_modifiers_dirty=impl_->fog_multipliers(impl_->command_ledger)!=impl_->last_fog_multipliers;
        }
        const auto search_count = in.count();
        impl_->searches.clear();
        EntityId previous_search = 0;
        for (std::uint64_t i = 0; i < search_count; ++i) {
            EntityId id{};
            SlicedPathSearch::Checkpoint saved;
            std::uint64_t frame{};
            math::Vec3 target{};
            std::optional<math::Fixed> max_speed;
            Impl::PathInputs inputs;
            std::optional<MotionState> plan;
            in(id, saved, frame, target, max_speed, inputs, plan);
            if (id <= previous_search || saved.entity != id) throw checkpoint_detail::Invalid("invalid sliced search identity");
            previous_search = id;
            auto search = SlicedPathSearch::from_checkpoint(impl_->motion, saved);
            if (!search) throw checkpoint_detail::Invalid(search.error().message);
            impl_->searches.emplace(id, Impl::SlicedSearch{std::move(search).value(), frame, target, max_speed, inputs, plan});
        }
        std::vector<std::uint8_t> retreat_bytes;
        in(retreat_bytes, impl_->midbattle_solve);
        auto retreat_saved = decode_retreat_state(retreat_bytes);
        if (!retreat_saved || (!impl_->retreat && retreat_saved.value().phase != RetreatPhase::idle)
            || (impl_->midbattle_solve && (!impl_->retreat || !impl_->teams.contains(*impl_->midbattle_solve))))
            throw checkpoint_detail::Invalid("invalid campaign retreat state");
        impl_->retreat_state = std::move(retreat_saved).value();
        for (auto& unit : live) impl_->refresh_damage_modes(unit);
        if (in.remaining() != 0) throw checkpoint_detail::Invalid("trailing child-state bytes");
        // The factory submitted the sealed log. Move its past commands into the immutable record.
        impl_->executed.clear();
        for (auto it = impl_->pending.begin(); it != impl_->pending.end() && it->first.tick < impl_->completed_tick;) {
            impl_->executed.push_back(it->second);
            it = impl_->pending.erase(it);
        }
        // #3644: the factory also submitted the object-turn uses of the past ticks.
        impl_->object_turns_executed.clear();
        while (!impl_->object_turn_pending.empty() && impl_->object_turn_pending.begin()->second.tick < impl_->completed_tick) {
            impl_->object_turns_executed.push_back(impl_->object_turn_pending.begin()->second);
            impl_->object_turn_pending.erase(impl_->object_turn_pending.begin());
        }
        impl_->rebuild(live, false);
        impl_->publish_staged(live, {}, {});
        checkpoint_detail::Access::restore_visibility(*impl_, visibility);
        return core::Result<void>::success();
    } catch (const checkpoint_detail::Invalid& error) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_setup, error.what()));
    }
}

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
            profile.hardpoints[index].max_health,
            index < state.repairing_players.size() ? state.repairing_players[index] : std::vector<PlayerId>{},
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
        const auto field = SensorField::build(setup.players, states, sensors, {}, command_ledger.reveal_multipliers).value();
        std::vector<TacticalInstance> instances;
        instances.reserve(live.size());
        waypoint_queues_present = false;
        for (const auto& unit : live) {
            // WWP-73: staged publication preserves the drawn frame without mutating live caches.
            const auto rotation = banked_rotation(unit.state.rotation, unit.roll).value();
            auto instance = instance_for(unit, math::to_matrix(rotation, unit.state.position).value(), completed_tick);
            instance.visible_to = visible_to(field, fog ? &*fog : nullptr, unit.state.owner, unit.state.position,
                unit.state.type_id, unit.state.rotation);
            instance.reveal_range = field.reveal_range(unit.state.type_id, unit.state.owner);
            if (const auto craft = crafts.find(unit.state.entity_id); craft != crafts.end()) {
                instance.craft_velocity_per_frame = craft->second.velocity;
            }
            if (const auto squadron = minds.find(unit.state.entity_id); squadron != minds.end()) {
                instance.squadron_in_idle_grid = squadron->second.idle_cell.has_value();
                if (squadron->second.idle_cell) instance.squadron_idle_anchor = squadron->second.anchor;
                const auto& flight = squadron->second;
                instance.has_movement_path = flight.mode == SquadronMode::move || flight.approach
                    || (flight.mode == SquadronMode::escort && !flight.idle_cell);
            }
            if (unit.waypoints) waypoint_queues_present |= !unit.waypoints->points.empty();
            instances.push_back(std::move(instance));
        }
        current_snapshot = std::make_shared<const TacticalSnapshot>(completed_tick, snapshot_players,
            std::move(instances), std::move(events), std::move(combat_events), projectiles, outcome,
            std::vector<SpinningCraft>{}, std::vector<SquadronTarget>{}, squadrons, economy_views(ledgers, shares), pad_views(), quits,
            losses, productions, manual_clock_views(), economy_cues, ability_spawns, retreat_views(), garrison_timer_views(), beacons);
    }

std::vector<GarrisonTimerView> TacticalSession::Impl::garrison_timer_views() const {
    // WSL-29: bounded player publication copies already committed timers;
    // this changes no service phase, timer admission or canonical state.
    std::vector<GarrisonTimerView> views;
    views.reserve(free_garrisons.size());
    for (const auto& state : free_garrisons)
        views.push_back({state.setup.player, state.setup.delay_frames, state.due_frame});
    return views;
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
    const std::span<const HeldReveal> held) {
        std::vector<FogRevealer> result;
        for (const auto& unit : units) {
            const auto range = field.reveal_range(unit.type_id, unit.owner);
            if (!range) continue;
            math::Vec3 position = unit.position;
            const auto at = std::lower_bound(held.begin(), held.end(), unit.entity_id,
                [](const HeldReveal& entry, const EntityId id) { return entry.id < id; });
            if (at != held.end() && at->id == unit.entity_id) position = at->position;
            result.push_back(FogRevealer{unit.entity_id, unit.owner, position, *range,
                field.profile(unit.type_id)->dense_multiplier});
        }
        return result;
    }

std::uint64_t TacticalSession::Impl::visible_to(const SensorField& field, const FogCells* cells, const PlayerId owner,
    const math::Vec3& position, const TypeId type, const math::Quat rotation) const {
        if (cells == nullptr) {
            return field.visible_to(owner, position);
        }
        const auto team = teams.at(owner);
        // V-21: centre plus corners and edge midpoints of the yaw-oriented hard box.
        std::array<math::Vec3, 9> samples{};
        samples[0] = position;
        std::size_t sample_count = 1;
        const auto* profile = field.profile(type);
        if (profile != nullptr && profile->multisample) {
            const auto yaw = yaw_degrees(rotation);
            const auto level = yaw ? yaw_rotation(yaw.value()) : core::Result<math::Quat>::success(math::identity_quat());
            const auto frame = math::to_matrix(level ? level.value() : math::identity_quat(), position);
            if (frame) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) continue;
                const auto sx = math::add(profile->box_offset.x,
                    math::Fixed::from_raw(x * profile->half_extents.x.raw()));
                const auto sy = math::add(profile->box_offset.y,
                    math::Fixed::from_raw(y * profile->half_extents.y.raw()));
                if (!sx || !sy) continue;
                const auto point = math::transform_point(frame.value(), {sx.value(), sy.value(), math::Fixed{}});
                if (point) samples[sample_count++] = point.value();
            }
        }
        std::uint64_t mask = 0;
        for (std::size_t index = 0; index < setup.players.size(); ++index) {
            if (setup.players[index].team_id == team
                || cells->revealed(index, std::span<const math::Vec3>{samples.data(), sample_count})) {
                mask |= std::uint64_t{1} << index;
            }
        }
        return mask;
    }

TacticalInstance TacticalSession::Impl::instance_for(const LiveUnit& unit, const math::Mat3x4& transform,
    const std::uint64_t now, const std::map<EntityId, SpecialWeaponState>* staged_special) const {
        TacticalInstance instance{unit.state.entity_id, unit.state.type_id, unit.state.owner,
            teams.at(unit.state.owner), transform, 0, {}, {}, {}};
        instance.contained = unit.state.contained;
        if (unit.durability) {
            instance.durability = durability_status(*health_profile(unit), durability, *unit.durability);
        }
        if (unit.waypoints) {
            instance.waypoint_points.reserve(unit.waypoints->points.size());
            for (const auto& point : unit.waypoints->points)
                instance.waypoint_points.push_back({point.move.destination, point.repeat});
        }
        instance.abilities = ability_statuses(unit, now);
        // WU-37: birth marks survive ownership changes and loss of original members.
        // Replenishment does not propagate a mark from craft to team or team to craft.
        instance.garrison_unit = unit.garrison_unit;
        if (const auto team = minds.find(unit.state.entity_id); team != minds.end())
            instance.garrison_unit = team->second.garrison_unit;
        detail::CombatUnit targeting;
        targeting.type_id = unit.state.type_id;
        targeting.profile = combat.find(unit.state.type_id);
        targeting.footprint = combat.footprint(unit.state.type_id);
        instance.category_bits = targeting.profile ? targeting.profile->category_bits : 0;
        instance.target_stealth = targeting.profile && targeting.profile->target_stealth;
        instance.soft_radius = detail::target_soft_radius(&motion, targeting);
        const auto& special_states = staged_special ? *staged_special : special_weapons;
        if (const auto found = special_states.find(unit.state.entity_id); found != special_states.end()) {
            const auto* profile = combat.find(unit.state.type_id);
            if (profile && profile->planetary_weapon) {
                const auto& special = *profile->planetary_weapon;
                const auto& state = found->second;
                auto readiness = math::Fixed::from_raw(math::Fixed::scale);
                if (now < state.recharge_until && state.recharge_until > state.recharge_started) {
                    const auto elapsed = now > state.recharge_started ? now - state.recharge_started : 0;
                    readiness = math::Fixed::from_raw(static_cast<std::int64_t>(
                        elapsed * math::Fixed::scale / (state.recharge_until - state.recharge_started)));
                }
                instance.planetary_weapon = TacticalInstance::PlanetaryWeaponView{
                    special.index, special.target_action, special.valid_targets, state, readiness};
            }
        }
        instance.has_movement_path = unit.motion && unit.motion->kind == MotionKind::path;
        if (unit.motion && motion.find(unit.state.type_id)) instance.locomotor_speed_per_frame = unit.speed;
        instance.in_tractor_beam = unit.in_tractor_beam;
        if (unit.abilities) {
            instance.stealth_on = unit.abilities->stealth_on;
            instance.stealth_invisible = stealth_invisible(*unit.abilities, now, abilities.find(unit.state.type_id));
        }
        instance.in_asteroid_field = unit.asteroid_contact.has_value();
        instance.in_nebula = unit.nebula && unit.nebula->present;
        instance.corrupt = unit.corrupt;
        instance.in_ion_storm = durability.damage && unit.durability && in_ion_storm(*durability.damage, *unit.durability, now);
        if (instance.in_ion_storm && instance.durability) instance.durability->shields_online = false;
        if (unit.combat) {
            const auto* profile = combat.find(unit.state.type_id);
            for (std::size_t index = 0; index < unit.combat->weapons.size(); ++index) {
                const auto& weapon = unit.combat->weapons[index];
                if (weapon.turret) {
                    instance.object_turret = weapon.turret;
                    if (profile && profile->weapons[index].object_turret) {
                        const auto& turret = *profile->weapons[index].object_turret;
                        instance.object_turret_xy_only = turret.xy_only;
                        instance.object_turret_barrel = turret.barrel;
                    }
                }
                if (!weapon.manual || profile == nullptr) continue;
                const auto& manual = *weapon.manual;
                instance.manual_weapons.push_back({profile->weapons[index].hardpoint, manual.target,
                    manual.requesting_player, manual.assigned_frame, manual.yaw, manual.pitch,
                    weapon.countdown, weapon.pulses_left, !unit.durability || weapon_enabled(
                        *health_profile(unit), *unit.durability, profile->weapons[index].hardpoint),
                    manual_admission_profiles.at({unit.state.type_id, profile->weapons[index].hardpoint})});
            }
        }
        if (ion_stunned(unit.ion_stun, now)) {
            instance.ion_stun_frames = static_cast<std::uint32_t>(unit.ion_stun->end_frame - now);
        }
        return instance;
    }

void TacticalSession::Impl::append_canonical_unit(std::vector<std::uint8_t>& bytes, const LiveUnit& unit) const {
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

namespace {
// Walk a map once to retain fixed-range boundaries, without copying any record.
template <typename Map>
auto canonical_map_ranges(const Map& values) {
    std::array<typename Map::const_iterator, tick_partition_count + 1> ranges;
    auto next = values.begin();
    for (std::size_t partition = 0; partition < tick_partition_count; ++partition) {
        ranges[partition] = next;
        const auto range = partition_range(partition, values.size());
        std::advance(next, static_cast<typename Map::difference_type>(range.end - range.begin));
    }
    ranges.back() = next;
    return ranges;
}
} // namespace

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes(const PartitionExecutor& executor) const {
    std::vector<std::pair<EntityId, entt::entity>> order;
    std::vector<LiveUnit> units;
    {
        core::tick_profile::Scope record_profile_scope("serial.tactical.finish.records");
        order.assign(handles.begin(), handles.end());
        units.resize(order.size());
    }
    std::array<std::vector<std::uint8_t>, tick_partition_count> encoded_units;
    std::array<CanonicalCounts, tick_partition_count> sparse_counts;
    CanonicalEncoding encoding;
    auto& craft_parts = encoding.crafts;
    auto& mind_parts = encoding.minds;
    auto& projectile_parts = encoding.projectiles;
    auto& ability_parts = encoding.abilities;
    const auto craft_ranges = canonical_map_ranges(crafts);
    const auto mind_ranges = canonical_map_ranges(minds);
    const auto tree_ranges = canonical_map_ranges(collection.state_trees());
    const auto projectile_tree_ranges = canonical_map_ranges(projectile_collection.state_trees());
    auto& tree_parts = encoding.trees;
    auto& projectile_tree_parts = encoding.projectile_trees;
    // The registry is frozen after the ordered commit. Each fixed range owns its
    // copied records and byte buffer; only pointer metadata is gathered serially.
    const auto prepared = executor.execute_phase("canonical-records", tick_partition_count,
        [&](const std::size_t partition) {
            const auto range = partition_range(partition, units.size());
            std::vector<std::uint8_t> bytes;
            bytes.reserve((range.end - range.begin) * (detail::unit_record_size + detail::order_record_size));
            for (auto index = range.begin; index < range.end; ++index) {
                units[index] = live_at(order[index].first, order[index].second);
                append_canonical_unit(bytes, units[index]);
                sparse_counts[partition].add(units[index]);
                if (units[index].abilities) {
                    sim::detail::append_u64(ability_parts[partition], units[index].state.entity_id);
                    append_abilities(ability_parts[partition], *units[index].abilities);
                }
            }
            encoded_units[partition] = std::move(bytes);
            for (auto it = craft_ranges[partition]; it != craft_ranges[partition + 1]; ++it)
                append_craft(craft_parts[partition], it->first, it->second);
            for (auto it = mind_ranges[partition]; it != mind_ranges[partition + 1]; ++it)
                append_squadron_state(mind_parts[partition], it->second);
            if (durability.damage) {
                const auto projectiles_range = partition_range(partition, projectiles.size());
                for (auto index = projectiles_range.begin; index < projectiles_range.end; ++index)
                    detail::append_projectile(projectile_parts[partition], projectiles[index]);
            }
            const auto encode_trees = [&](const auto& ranges, auto& parts) {
                for (auto it = ranges[partition]; it != ranges[partition + 1]; ++it) {
                    sim::detail::append_u64(parts[partition], it->first);
                    it->second->append_state(parts[partition]);
                }
            };
            if (!collection.empty()) encode_trees(tree_ranges, tree_parts);
            if (projectile_collection.has_history()) encode_trees(projectile_tree_ranges, projectile_tree_parts);
        });
    // This phase only accelerates publication of an already committed tick. A
    // failed read-only worker pass must not leave that tick without its hash and
    // snapshot; discard its partial buffers and use the existing serial encoder.
    if (!prepared) return canonical_bytes();
    for (const auto& part : sparse_counts) encoding.counts.add(part);
    return canonical_bytes(units, encoded_units, &encoding);
}

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes() const {
        const auto units = [&] {
            core::tick_profile::Scope record_profile_scope("serial.tactical.finish.records");
            return sorted_live();
        }();
        return canonical_bytes(units, {});
    }

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes(const std::span<const LiveUnit> units,
    const std::span<const std::vector<std::uint8_t>> encoded_units,
    const CanonicalEncoding* encoding) const {
        core::tick_profile::Scope encoding_profile_scope("serial.tactical.finish.encoding");
        std::vector<std::uint8_t> bytes;
        auto capacity = std::size_t{120} + setup.players.size() * detail::player_record_size;
        if (encoded_units.empty()) capacity += units.size() * (detail::unit_record_size + detail::order_record_size);
        else for (const auto& part : encoded_units) capacity += part.size();
        if (encoding) for (const auto buffers : {&encoding->crafts, &encoding->minds, &encoding->projectiles,
                &encoding->abilities, &encoding->trees, &encoding->projectile_trees})
            for (const auto& part : *buffers) capacity += part.size();
        bytes.reserve(capacity);
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
        if (retreat) {
            bytes.insert(bytes.end(), {'R', 'T', 'R', 'T'});
            const auto encoded = encode_retreat_state(retreat_state);
            sim::detail::append_u64(bytes, encoded.size()); bytes.insert(bytes.end(), encoded.begin(), encoded.end());
            sim::detail::append_u32(bytes, midbattle_solve.value_or(0));
        }
        bytes.insert(bytes.end(), setup.content_identity.begin(), setup.content_identity.end());
        sim::detail::append_u64(bytes, setup.players.size());
        for (const auto& player : setup.players) {
            detail::append_player(bytes, player);
        }
        append_canonical_units_state(bytes, units, encoded_units);
        const auto* counts = encoding ? &encoding->counts : nullptr;
        const auto parts = [encoding](auto member) -> std::span<const std::vector<std::uint8_t>> {
            if (!encoding) return {};
            return encoding->*member;
        };
        append_canonical_garrison_enabled_state(bytes, units, counts);
        append_canonical_named_locks_state(bytes);
        append_canonical_special_weapons_state(bytes);
        append_canonical_garrisons_state(bytes);
        append_canonical_manual_weapons_state(bytes, units, counts);
        append_canonical_turrets_state(bytes, units, counts);
        append_canonical_squadrons_state(bytes);
        append_canonical_fog_state(bytes);
        append_canonical_tracking_state(bytes);
        append_canonical_roll_state(bytes, units, counts);
        append_canonical_projectiles_state(bytes, parts(&CanonicalEncoding::projectiles));
        append_canonical_blast_damage_state(bytes);
        append_canonical_formation_state(bytes, units, counts);
        append_canonical_approach_state(bytes, units, counts);
        append_canonical_sightings_state(bytes, units, counts);
        append_canonical_garrison_marks_state(bytes, units, counts);
        append_canonical_craft_state(bytes, parts(&CanonicalEncoding::crafts));
        append_canonical_squadron_orders_state(bytes, parts(&CanonicalEncoding::minds));
        append_canonical_attack_paths_state(bytes);
        append_canonical_hangars_state(bytes);
        append_canonical_collection_state(bytes, parts(&CanonicalEncoding::trees));
        append_canonical_projectile_collection_state(bytes, parts(&CanonicalEncoding::projectile_trees));
        append_canonical_economy_state(bytes);
        append_canonical_arrivals_state(bytes);
        append_canonical_pads_state(bytes);
        append_canonical_population_state(bytes);
        append_canonical_arrival_defence_state(bytes, units, counts);
        append_canonical_upgrades_state(bytes, units, counts);
        append_canonical_level_up_state(bytes);
        append_canonical_income_state(bytes);
        append_canonical_outcome_state(bytes);
        append_canonical_abilities_state(bytes, units, counts, parts(&CanonicalEncoding::abilities));
        append_canonical_stealth_state(bytes, units, counts);
        append_canonical_stealth_end_state(bytes, units, counts);
        append_canonical_self_destruct_state(bytes, units, counts);
        append_canonical_mc30_state(bytes, units, counts);
        append_canonical_ion_stuns_state(bytes, units, counts);
        append_canonical_hazard_moves_state(bytes, units, counts);
        append_canonical_asteroids_state(bytes, units, counts);
        append_canonical_engine_disable_state(bytes, units, counts);
        append_canonical_nebula_state(bytes, units, counts);
        append_canonical_storms_state(bytes, units, counts);
        append_canonical_damage_tracking_state(bytes, units, counts);
        append_canonical_command_bonuses_state(bytes);
        append_canonical_heroes_state(bytes, units, counts);
        append_canonical_projectile_defence_state(bytes);
        append_canonical_special_abilities_state(bytes, units, counts);
        append_canonical_concentrate_state(bytes, units, counts);
        append_canonical_ability_spawns_state(bytes);
        append_canonical_buzz_requests_state(bytes, units, counts);
        append_canonical_buzz_droids_state(bytes);
        append_canonical_spins_state(bytes);
        append_canonical_departure_state(bytes);
        append_canonical_quits_state(bytes);
        append_canonical_pool_tokens_state(bytes, units, counts);
        append_canonical_queue_identities_state(bytes);
        append_canonical_cinematic_seats_state(bytes);
        append_canonical_campaign_state(bytes);
        append_canonical_corruption_state(bytes, units, counts);
        append_canonical_charged_blast_state(bytes, units, counts);
        append_canonical_waypoints_state(bytes, units, counts);
        append_canonical_beacons_state(bytes);
        append_canonical_order_history_state(bytes, units, counts);
        return bytes;
    }

} // namespace eawr::sim::tactical
