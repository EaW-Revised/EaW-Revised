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

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes(const PartitionExecutor& executor) const {
    std::vector<std::pair<EntityId, entt::entity>> order;
    std::vector<LiveUnit> units;
    {
        core::tick_profile::Scope record_profile_scope("serial.tactical.finish.records");
        order.assign(handles.begin(), handles.end());
        units.resize(order.size());
    }
    std::array<std::vector<std::uint8_t>, tick_partition_count> encoded_units;
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
            }
            encoded_units[partition] = std::move(bytes);
        });
    // This phase only accelerates publication of an already committed tick. A
    // failed read-only worker pass must not leave that tick without its hash and
    // snapshot; discard its partial buffers and use the existing serial encoder.
    if (!prepared) return canonical_bytes();
    return canonical_bytes(units, encoded_units);
}

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes() const {
        const auto units = [&] {
            core::tick_profile::Scope record_profile_scope("serial.tactical.finish.records");
            return sorted_live();
        }();
        return canonical_bytes(units, {});
    }

std::vector<std::uint8_t> TacticalSession::Impl::canonical_bytes(const std::span<const LiveUnit> units,
    const std::span<const std::vector<std::uint8_t>> encoded_units) const {
        core::tick_profile::Scope encoding_profile_scope("serial.tactical.finish.encoding");
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
        sim::detail::append_u64(bytes, units.size());
        if (encoded_units.empty()) {
            for (const auto& unit : units) append_canonical_unit(bytes, unit);
        } else {
            // Fixed contiguous ranges concatenate in exactly the serial entity order.
            for (const auto& part : encoded_units) bytes.insert(bytes.end(), part.begin(), part.end());
        }
        std::uint64_t disabled_garrisons = 0;
        for (const auto& unit : units) disabled_garrisons += unit.state.garrison_enabled ? 0U : 1U;
        // FL-13: sparse GSPN keeps every all-enabled session's canonical bytes unchanged.
        if (disabled_garrisons != 0) {
            bytes.insert(bytes.end(), {'G', 'S', 'P', 'N'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, disabled_garrisons);
            for (const auto& unit : units) {
                if (!unit.state.garrison_enabled) sim::detail::append_u64(bytes, unit.state.entity_id);
            }
        }
        std::uint64_t manual_weapons = 0;
        if (!named_ability_locks.empty()) {
            bytes.insert(bytes.end(), {'N', 'L', 'C', 'K'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, named_ability_locks.size());
            for (const auto& [key, locked] : named_ability_locks) {
                const auto& [owner, type, name] = key;
                sim::detail::append_u32(bytes, owner);
                sim::detail::append_u64(bytes, type);
                sim::detail::append_u64(bytes, name.size());
                bytes.insert(bytes.end(), name.begin(), name.end());
                sim::detail::append_u32(bytes, locked ? 1U : 0U);
            }
        }
        // WSW-14/19: coordinator-reserved optional block; ordinary sessions retain their bytes.
        if (!special_weapons.empty() || !special_disabled_by_hardpoint.empty()) {
            bytes.insert(bytes.end(), {'S', 'W', 'P', 'N'});
            sim::detail::append_u32(bytes, special_disabled_by_hardpoint.empty() ? 1U : 2U);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, special_weapons.size());
            for (const auto& [id, state] : special_weapons) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, state.recharge_started);
                sim::detail::append_u64(bytes, state.recharge_until);
                sim::detail::append_u64(bytes, state.next_pulse_frame);
                sim::detail::append_u64(bytes, state.target);
                sim::detail::append_u32(bytes, state.pulses_left);
                sim::detail::append_u32(bytes, state.instances);
            }
            if (!special_disabled_by_hardpoint.empty()) {
                sim::detail::append_u64(bytes, special_disabled_by_hardpoint.size());
                for (const auto player : special_disabled_by_hardpoint) sim::detail::append_u32(bytes, player);
            }
        }
        if (!free_garrisons.empty()) {
            bytes.insert(bytes.end(), {'G', 'A', 'R', 'R'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, free_garrisons.size());
            for (const auto& state : free_garrisons) {
                sim::detail::append_u32(bytes, state.setup.player);
                sim::detail::append_u32(bytes, state.setup.delay_frames);
                sim::detail::append_u64(bytes, state.setup.templates.size());
                for (const auto type : state.setup.templates) sim::detail::append_u64(bytes, type);
                sim::detail::append_u64(bytes, state.registered.size());
                for (const auto id : state.registered) sim::detail::append_u64(bytes, id);
                sim::detail::append_u32(bytes, state.due_frame ? 1U : 0U);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, state.due_frame.value_or(0));
                sim::detail::append_u64(bytes, state.pending.size());
                for (const auto type : state.pending) sim::detail::append_u64(bytes, type);
            }
        }
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
        std::uint64_t turret_count = 0;
        for (const auto& unit : units) {
            if (!unit.combat) continue;
            for (const auto& weapon : unit.combat->weapons) turret_count += weapon.turret ? 1 : 0;
        }
        if (turret_count != 0) {
            bytes.insert(bytes.end(), {'O', 'T', 'U', 'R'});
            sim::detail::append_u64(bytes, turret_count);
            for (const auto& unit : units) {
                if (!unit.combat) continue;
                for (std::uint32_t index = 0; index < unit.combat->weapons.size(); ++index) {
                    const auto& turret = unit.combat->weapons[index].turret;
                    if (!turret) continue;
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    sim::detail::append_u32(bytes, index);
                    sim::detail::append_u64(bytes, turret->target);
                    for (const auto angle : {turret->yaw, turret->pitch, turret->desired_yaw, turret->desired_pitch})
                        append_fixed(bytes, angle);
                    bytes.push_back(turret->ready ? 1 : 0);
                    bytes.push_back(turret->rest_started_frame ? 1 : 0);
                    sim::detail::append_u64(bytes, turret->rest_started_frame.value_or(0));
                    bytes.push_back(turret->returning_to_rest ? 1 : 0);
                }
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
            if (setup.campaign) {
                bytes.insert(bytes.end(),{'G','C','F','W'});
                sim::detail::append_u64(bytes,setup.players.size());
                for(std::size_t i=0;i<setup.players.size();++i) sim::detail::append_i64(bytes,last_fog_multipliers[i].raw());
            }
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
        // Waiting group members (#344), sliced searches (PC-08), and failed searches (FC-68), each
        if (!pending_blast_damage.empty()) {
            constexpr std::array<std::uint8_t, 4> blast_tag{'D', 'D', 'M', 'G'};
            bytes.insert(bytes.end(), blast_tag.begin(), blast_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, pending_blast_damage.size());
            std::vector<std::size_t> order(pending_blast_damage.size());
            for (std::size_t index = 0; index < order.size(); ++index) order[index] = index;
            std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
                return pending_blast_damage[left].due < pending_blast_damage[right].due;
            });
            for (const auto index : order) {
                const auto& queued = pending_blast_damage[index];
                sim::detail::append_u64(bytes, queued.due);
                sim::detail::append_u64(bytes, queued.recipient);
                append_fixed(bytes, queued.amount);
                sim::detail::append_u32(bytes, queued.owner);
                sim::detail::append_u32(bytes, queued.damage_type);
                sim::detail::append_u32(bytes, queued.internal_damage_misc ? 1U : 0U);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(queued.contacted_mesh.size()));
                bytes.insert(bytes.end(), queued.contacted_mesh.begin(), queued.contacted_mesh.end());
            }
        }
        // Waiting group members (#344), then ships waiting for a sliced search (PC-08), each
        // ascending ID, only while any wait.
        for (const unsigned group : {0U, 1U, 2U}) {
            std::vector<std::pair<EntityId, const FormationWait*>> waits;
            for (const auto& unit : units) {
                if (!unit.formation) continue;
                const auto wait_group = unit.formation->failure_frame ? 2U : (unit.formation->sliced ? 1U : 0U);
                if (wait_group == group) waits.emplace_back(unit.state.entity_id, &*unit.formation);
            }
            if (waits.empty()) continue;
            const auto& tag = group == 2U ? path_retry_tag : (group == 1U ? sliced_tag : formation_tag);
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            if (group == 2U) sim::detail::append_u32(bytes, path_retry_block_version);
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
                if (group == 2U) sim::detail::append_u64(bytes, *wait->failure_frame);
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
        // WMV-13 (#719): last observed order targets, ascending ID, only while any unit keeps one.
        std::vector<std::pair<EntityId, const Sighting*>> sightings;
        for (const auto& unit : units) {
            if (unit.sighting) sightings.emplace_back(unit.state.entity_id, &*unit.sighting);
        }
        if (!sightings.empty()) {
            bytes.insert(bytes.end(), sighting_tag.begin(), sighting_tag.end());
            sim::detail::append_u64(bytes, sightings.size());
            for (const auto& [id, sighting] : sightings) {
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, sighting->target);
                append_fixed(bytes, sighting->point.x);
                append_fixed(bytes, sighting->point.y);
                append_fixed(bytes, sighting->point.z);
            }
        }
        // WU-37, GRSN v1: independent object and team birth marks, in stable ID order.
        const auto marked_units = std::count_if(units.begin(), units.end(),
            [](const auto& unit) { return unit.garrison_unit; });
        const auto marked_teams = std::count_if(minds.begin(), minds.end(),
            [](const auto& entry) { return entry.second.garrison_unit; });
        if (marked_units != 0 || marked_teams != 0) {
            bytes.insert(bytes.end(), garrison_tag.begin(), garrison_tag.end());
            sim::detail::append_u32(bytes, garrison_block_version);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(marked_units));
            for (const auto& unit : units) if (unit.garrison_unit)
                sim::detail::append_u64(bytes, unit.state.entity_id);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(marked_teams));
            for (const auto& [id, mind] : minds) if (mind.garrison_unit)
                sim::detail::append_u64(bytes, id);
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
        // SQAP v2 retains route facing/mapping and member annotations.
        const auto attack_paths = std::count_if(minds.begin(), minds.end(),
            [](const auto& entry) { return entry.second.attack_path.has_value(); });
        if (attack_paths != 0) {
            bytes.insert(bytes.end(), squadron_attack_path_tag.begin(), squadron_attack_path_tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(attack_paths));
            for (const auto& [id, state] : minds) {
                if (!state.attack_path) continue;
                const auto& path = *state.attack_path;
                sim::detail::append_u64(bytes, id);
                sim::detail::append_u64(bytes, path.target);
                append_vec3(bytes, path.cached_position);
                sim::detail::append_u64(bytes, path.next_refresh_frame);
                sim::detail::append_u64(bytes, path.collision_identity);
                sim::detail::append_u64(bytes, path.stack_changed_frame);
                sim::detail::append_u64(bytes, path.escorted_destination);
                sim::detail::append_u32(bytes, path.initialization_pending ? 1U : 0U);
                sim::detail::append_u32(bytes, path.failure_frame ? 1U : 0U);
                if (path.failure_frame) sim::detail::append_u64(bytes, *path.failure_frame);
                sim::detail::append_u64(bytes, path.stack.size());
                for (const auto& route : path.stack) {
                    sim::detail::append_u64(bytes, route.identity);
                    append_fixed(bytes, route.aside);
                    append_vec3(bytes, route.start_facing);
                    append_fixed(bytes, route.ahead);
                    sim::detail::append_u64(bytes, route.nodes.size());
                    for (const auto& node : route.nodes) append_vec3(bytes, node);
                    sim::detail::append_u64(bytes, route.node_speeds.size());
                    for (const auto speed : route.node_speeds) append_fixed(bytes, speed);
                    sim::detail::append_u64(bytes, route.trackers.size());
                    for (const auto& [member, tracker] : route.trackers) {
                        sim::detail::append_u64(bytes, member);
                        sim::detail::append_u32(bytes, tracker.segment);
                        sim::detail::append_u32(bytes, tracker.started ? 1U : 0U);
                        append_fixed(bytes, tracker.turn_scale);
                        append_fixed(bytes, tracker.speed_scale);
                    }
                }
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
        // WSL-31: LSVC v1, coordinator-reserved; only pending held level-up deadlines.
        std::uint64_t level_up_services = 0;
        for (const auto& account : ledgers) for (const auto& held : account.completed)
            level_up_services += held.level_up_service_frame.has_value() ? 1U : 0U;
        if (level_up_services != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'L', 'S', 'V', 'C'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1); sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, level_up_services);
            for (const auto& account : ledgers) for (const auto& held : account.completed) {
                if (!held.level_up_service_frame) continue;
                sim::detail::append_u32(bytes, account.player); sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, held.object);
                sim::detail::append_u64(bytes, *held.level_up_service_frame);
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
        // Cloaks (#1887, WST-03), ascending ID, only while one is on: the last detection frame.
        const auto cloaked = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->stealth_on;
        });
        if (cloaked != 0) {
            bytes.insert(bytes.end(), stealth_tag.begin(), stealth_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(cloaked));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->stealth_on) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.abilities->stealth_detected);
            }
        }
        // Cloaks with their own end (#1906, WST-03), ascending ID, only while one is on.
        const auto lengthened = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->stealth_on && unit.abilities->stealth_end != 0;
        });
        if (lengthened != 0) {
            bytes.insert(bytes.end(), stealth_end_tag.begin(), stealth_end_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(lengthened));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->stealth_on || unit.abilities->stealth_end == 0) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.abilities->stealth_end);
            }
        }
        // Krayt object scripts (WSD-09), ascending ID, once a live unit's script has run.
        const auto scripted = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->self_destruct_script;
        });
        if (scripted != 0) {
            bytes.insert(bytes.end(), self_destruct_script_tag.begin(), self_destruct_script_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(scripted));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->self_destruct_script) continue;
                const auto& script = *unit.abilities->self_destruct_script;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_i64(bytes, script.last_service);
                sim::detail::append_u32(bytes, script.nearby);
                sim::detail::append_i64(bytes, script.threat.raw());
                sim::detail::append_u32(bytes, script.armed ? 1U : 0U);
            }
        }
        // FC-57/58: only initialized MC30 scripts add this optional state block.
        const auto mc30_scripts = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->mc30_script;
        });
        if (mc30_scripts != 0) {
            bytes.insert(bytes.end(), mc30_script_tag.begin(), mc30_script_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(mc30_scripts));
            for (const auto& unit : units) {
                if (!unit.abilities || !unit.abilities->mc30_script) continue;
                const auto& script = *unit.abilities->mc30_script;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_i64(bytes, script.last_service);
                sim::detail::append_u32(bytes, script.nearby);
                sim::detail::append_i64(bytes, script.threat.raw());
                sim::detail::append_u32(bytes, script.human ? 1U : 0U);
                sim::detail::append_u32(bytes, script.armed ? 1U : 0U);
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
        // Damage tracking (WCC-82), ascending ID, only while a live unit's window or rate is not zero.
        const auto tracking = [](const LiveUnit& unit) {
            return unit.durability && (unit.durability->window_damage.raw() != 0 || unit.durability->damage_rate.raw() != 0);
        };
        if (const auto tracked = std::count_if(units.begin(), units.end(), tracking); tracked != 0) {
            bytes.insert(bytes.end(), damage_tracking_tag.begin(), damage_tracking_tag.end());
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(tracked));
            for (const auto& unit : units) {
                if (!tracking(unit)) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_i64(bytes, unit.durability->window_damage.raw());
                sim::detail::append_i64(bytes, unit.durability->damage_rate.raw());
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
        if (!projectile_defence_order.empty()) {
            // Coordinator-reserved PDEF: source IDs in registration order, never sorted.
            constexpr std::array<std::uint8_t, 4> tag{'P', 'D', 'E', 'F'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, projectile_defence_order.size());
            for (const auto id : projectile_defence_order) sim::detail::append_u64(bytes, id);
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
        const auto buzz_requests = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) {
            return unit.abilities && unit.abilities->pending_buzz_position;
        });
        if (buzz_requests != 0) {
            constexpr std::array<std::uint8_t, 4> tag{'B', 'Z', 'Q', 'R'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(buzz_requests));
            for (const auto& unit : units) if (unit.abilities && unit.abilities->pending_buzz_position) {
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_vec3(bytes, *unit.abilities->pending_buzz_position);
            }
        }
        if (!buzz_droids.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'B', 'U', 'Z', 'Z'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u64(bytes, buzz_droids.size());
            for (const auto& droid : buzz_droids) {
                sim::detail::append_u64(bytes, droid.droid);
                sim::detail::append_u64(bytes, droid.source_type);
                append_vec3(bytes, droid.start);
                sim::detail::append_u64(bytes, droid.active_from);
            }
        }
        if (!spins.empty()) {
            bytes.insert(bytes.end(), spin_tag.begin(), spin_tag.end());
            sim::detail::append_u64(bytes, spins.size());
            for (const auto& spin : spins) {
                append_spin(bytes, spin);
            }
        }
        if (setup.departure_policy) {
            constexpr std::array<std::uint8_t, 4> tag{'M', 'P', 'D', 'P'}; // coordinator-reserved
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(setup.departure_policy->humans.size()));
            for (const auto id : setup.departure_policy->humans) sim::detail::append_u32(bytes, id);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(departure_controls.size()));
            for (const auto& control : departure_controls) {
                sim::detail::append_u32(bytes, control.player);
                sim::detail::append_u32(bytes, control.controller);
                sim::detail::append_u64(bytes, control.tick);
                sim::detail::append_u32(bytes, control.basic_ai ? 1U : 0U);
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
        if (setup.queue_identities && !economy.empty()) {
            // coordinator-reserved QIDS: retain the counter even after every queued entry leaves.
            constexpr std::array<std::uint8_t, 4> tag{'Q', 'I', 'D', 'S'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, ledgers.size());
            for (const auto& ledger : ledgers) {
                sim::detail::append_u32(bytes, ledger.player);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, ledger.next_queue_entry_id);
                for (const auto& queue : ledger.queues) {
                    sim::detail::append_u64(bytes, queue.size());
                    for (const auto& entry : queue) sim::detail::append_u64(bytes, entry.entry_id);
                }
            }
        }
        if (!setup.cinematic_seats.empty()) {
            constexpr std::array<std::uint8_t, 4> tag{'C', 'I', 'N', 'S'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(setup.cinematic_seats.size()));
            for (const auto& seat : setup.cinematic_seats) {
                sim::detail::append_u32(bytes, seat.player);
                sim::detail::append_u32(bytes, seat.cinematics_enabled ? 1U : 0U);
            }
        }
        if (setup.campaign) {
            bytes.insert(bytes.end(), {'B','O','U','N','T','Y',1});
            for (const auto& ledger : ledgers) sim::detail::append_i64(bytes, ledger.collected_bounty.raw());
            constexpr std::array<std::uint8_t, 4> tag{'C', 'A', 'M', 'C'};
            bytes.insert(bytes.end(), tag.begin(), tag.end());
            sim::detail::append_u32(bytes, 1);
            bytes.insert(bytes.end(), campaign_binding.begin(), campaign_binding.end());
            sim::detail::append_u64(bytes, ledgers.size());
            for (const auto& ledger : ledgers) {
                sim::detail::append_u32(bytes, ledger.player);
                sim::detail::append_i64(bytes, ledger.campaign_extra_releases.raw());
            }
        }
        const auto corrupt_count = std::count_if(units.begin(), units.end(), [](const LiveUnit& unit) { return unit.corrupt.has_value(); });
        std::uint64_t corrupt_casts = 0;
        for (const auto& unit : units) if (unit.abilities)
            for (const auto& slot : unit.abilities->slots) corrupt_casts += slot.position ? 1U : 0U;
        if (corrupt_count != 0 || corrupt_casts != 0) {
            bytes.insert(bytes.end(), corrupt_tag.begin(), corrupt_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(corrupt_count));
            for (const auto& unit : units) if (unit.corrupt) {
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u64(bytes, unit.corrupt->source_type);
                sim::detail::append_u64(bytes, unit.corrupt->started_tick);
                sim::detail::append_u64(bytes, unit.corrupt->expires_tick);
                append_fixed(bytes, unit.corrupt->drain_per_frame);
                append_fixed(bytes, unit.corrupt->inaccuracy);
            }
            sim::detail::append_u64(bytes, corrupt_casts);
            for (const auto& unit : units) if (unit.abilities)
                for (std::size_t slot = 0; slot < unit.abilities->slots.size(); ++slot) if (unit.abilities->slots[slot].position) {
                    sim::detail::append_u64(bytes, unit.state.entity_id);
                    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(slot));
                    append_vec3(bytes, *unit.abilities->slots[slot].position);
                }
        }
        const auto blast_count = std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.abilities && unit.abilities->charged_blast;
        });
        if (blast_count != 0) {
            bytes.insert(bytes.end(), charged_blast_tag.begin(), charged_blast_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(blast_count));
            for (const auto& unit : units) if (unit.abilities && unit.abilities->charged_blast) {
                const auto& blast = *unit.abilities->charged_blast;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u32(bytes, blast.charge_remaining);
                sim::detail::append_u32(bytes, blast.shots_remaining);
                sim::detail::append_u32(bytes, blast.charged_presentation ? 1U : 0U);
            }
        }
        const auto waypoint_count = std::count_if(units.begin(), units.end(), [](const auto& unit) {
            return unit.waypoints.has_value();
        });
        if (waypoint_count != 0) {
            constexpr std::array<std::uint8_t, 4> waypoint_tag{'W', 'Y', 'P', 'T'};
            bytes.insert(bytes.end(), waypoint_tag.begin(), waypoint_tag.end());
            sim::detail::append_u32(bytes, 2);
            sim::detail::append_u32(bytes, motion.rules.max_waypoints);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(waypoint_count));
            for (const auto& unit : units) {
                if (!unit.waypoints) continue;
                sim::detail::append_u64(bytes, unit.state.entity_id);
                sim::detail::append_u32(bytes, unit.waypoints->coordinator ? 1U : 0U);
                if (unit.waypoints->coordinator) {
                    const auto& key = *unit.waypoints->coordinator;
                    sim::detail::append_u32(bytes, key.player_id);
                    sim::detail::append_u64(bytes, key.tick);
                    sim::detail::append_u64(bytes, key.sequence);
                }
                sim::detail::append_u32(bytes, unit.waypoints->active ? 1U : 0U);
                sim::detail::append_u32(bytes, static_cast<std::uint32_t>(unit.waypoints->points.size()));
                for (const auto& point : unit.waypoints->points) {
                    append_vec3(bytes, point.move.destination);
                    sim::detail::append_u32(bytes, (point.move.through_hazards ? 1U : 0U) | (point.repeat ? 2U : 0U));
                    sim::detail::append_u32(bytes, point.key.player_id);
                    sim::detail::append_u64(bytes, point.key.tick);
                    sim::detail::append_u64(bytes, point.key.sequence);
                }
            }
        }
        if (!beacons.empty()) {
            constexpr std::array<std::uint8_t, 4> beacon_tag{'B', 'E', 'C', 'N'};
            bytes.insert(bytes.end(), beacon_tag.begin(), beacon_tag.end());
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(beacons.size()));
            for (const auto& beacon : beacons) {
                sim::detail::append_u32(bytes, beacon.player);
                sim::detail::append_u32(bytes, 0);
                sim::detail::append_u64(bytes, beacon.type);
                append_vec3(bytes, beacon.position);
                sim::detail::append_u64(bytes, beacon.placed_tick);
                sim::detail::append_u64(bytes, beacon.expires_tick);
                sim::detail::append_u64(bytes, beacon.sequence);
            }
        }
        const auto history_count = std::count_if(units.begin(), units.end(),
            [](const LiveUnit& unit) { return unit.position_history.has_value(); });
        if (history_count != 0) {
            bytes.insert(bytes.end(), {'P', 'H', 'S', 'T'});
            sim::detail::append_u32(bytes, 1);
            sim::detail::append_u32(bytes, 0);
            sim::detail::append_u64(bytes, static_cast<std::uint64_t>(history_count));
            for (const auto& unit : units) if (unit.position_history) {
                sim::detail::append_u64(bytes, unit.state.entity_id);
                append_vec3(bytes, unit.position_history->previous);
                append_vec3(bytes, unit.position_history->before_previous);
            }
        }
        return bytes;
    }

} // namespace eawr::sim::tactical
