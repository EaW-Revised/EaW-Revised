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

void session_detail::Tick::set_special_hardpoint_gate(const PlayerId owner, const bool enabled) {
    // WSW-48: one sparse lifecycle notification writes each allied player's flag.
    // The latest notification wins; there is no reduction over other stations.
    for (const auto& player : impl_->setup.players) if (impl_->allied(owner, player.player_id)) {
        if (enabled) special_disabled_by_hardpoint_.erase(player.player_id);
        else special_disabled_by_hardpoint_.insert(player.player_id);
    }
}

void session_detail::Tick::set_named_hardpoint_gate(const UnitState& unit, const std::string_view name,
    const bool enabled) {
    if (const auto* profile = impl_->abilities.find(unit.type_id))
        radar_dirty_ = set_named_ability_lock(named_ability_locks_, unit.owner, *profile, name, !enabled)
            || radar_dirty_;
}

void session_detail::Tick::publish_special_hardpoints(const LiveUnit& unit) {
    const auto* profile = impl_->health_profile(unit);
    if (!profile || !unit.durability) return;
    for (std::size_t slot = 0; slot < profile->hardpoints.size(); ++slot) {
        if (!profile->hardpoints[slot].special_ability_name.empty())
            set_named_hardpoint_gate(unit.state, profile->hardpoints[slot].special_ability_name,
                !hardpoint_destroyed(*profile, *unit.durability, slot) && !hardpoint_disabled(*unit.durability, slot));
        if (profile->hardpoints[slot].allows_special_weapon_use)
            set_special_hardpoint_gate(unit.state.owner,
                !hardpoint_destroyed(*profile, *unit.durability, slot) && !hardpoint_disabled(*unit.durability, slot));
    }
}

void session_detail::Tick::sync_radar_creations(const UnitStage& staged, const EntityId first, const EntityId next) {
    // FC-75: visit only IDs allocated by this creation event, without writing other lifecycle gates.
    if (first == invalid_entity_id || first == next) return;
    for (auto id = first; id != next; ++id) {
        const auto added = staged.find(id);
        if (added != staged.end()) {
            auto slots = impl_->radar_slots(added->second.state.type_id);
            if (!slots.empty()) {
                if (!radar_roster_) radar_roster_ = impl_->radar_roster;
                radar_roster_->insert_or_assign(id, std::move(slots));
                radar_dirty_ = true;
            }
        }
        if (id == std::numeric_limits<EntityId>::max()) break;
    }
}

void session_detail::Tick::track_victory_change(const UnitState* before, const UnitState* after, const bool evaluate) {
    const auto& roster = radar_roster_ ? *radar_roster_ : impl_->radar_roster;
    auto slots = after ? impl_->radar_slots(after->type_id) : std::vector<std::size_t>{};
    if ((before && roster.contains(before->entity_id)) || !slots.empty()) {
        if (!radar_roster_) radar_roster_ = impl_->radar_roster;
        if (before) radar_roster_->erase(before->entity_id);
        if (after && !slots.empty()) radar_roster_->insert_or_assign(after->entity_id, std::move(slots));
        radar_dirty_ = true;
    }
    if (!before && after) if (const auto* health = impl_->durability.find(after->type_id))
        for (const auto& point : health->hardpoints)
            if (!point.special_ability_name.empty()) set_named_hardpoint_gate(*after, point.special_ability_name, true);
    // WSW-48: existing sparse creation notification; no additional entity scan.
    if (!before && after) if (const auto* health = impl_->durability.find(after->type_id))
        for (const auto& hardpoint : health->hardpoints) if (hardpoint.allows_special_weapon_use)
            set_special_hardpoint_gate(after->owner, true);
    // WPJ-17: sparse ordered creation notifications register static sources even
    // when victory is disabled, before a later same-frame jammer activation.
    if (!before && after) {
        const auto* combat = impl_->combat.find(after->type_id);
        const auto* ability = impl_->abilities.find(after->type_id);
        if (combat && (combat->passive_missile_shield_radius.raw() > 0
            || (ability && ability_slot(*ability, AbilityKind::missile_shield)))) {
            if (std::find(created_static_defences_.begin(), created_static_defences_.end(), after->entity_id) == created_static_defences_.end())
                created_static_defences_.push_back(after->entity_id);
            if (!projectile_defence_order_.empty()
                && std::find(projectile_defence_order_.begin(), projectile_defence_order_.end(), after->entity_id) == projectile_defence_order_.end())
                projectile_defence_order_.push_back(after->entity_id);
        }
    }
    const auto& rules = impl_->victory;
    if (rules.condition == VictoryCondition::none) return;
    // WGT-29/WGM-51: campaign installs these alongside all-enemy-units, without a lobby choice.
    if (!pending_outcome_ && evaluate && before && !after && impl_->setup.campaign) {
        const auto leader = [&](auto&& self, const CarriedObject& object) -> bool {
            if (std::binary_search(rules.campaign_leaders.begin(), rules.campaign_leaders.end(), object.entity_id)) return true;
            return std::any_of(object.members.begin(), object.members.end(), [&](const auto& member) { return self(self, member); });
        };
        const bool lost_leader = std::binary_search(rules.campaign_leaders.begin(), rules.campaign_leaders.end(), before->entity_id)
            || std::any_of(before->contained.begin(), before->contained.end(), [&](const auto& object) { return leader(leader, object); });
        if (lost_leader || before->entity_id == rules.campaign_station) {
            const auto winner = std::find_if(rules.contenders.begin(), rules.contenders.end(), [&](const auto player) {
                return impl_->teams.at(player) != impl_->teams.at(before->owner);
            });
            if (winner != rules.contenders.end()) {
                const auto condition = lost_leader ? VictoryCondition::enemy_leader_destroyed : VictoryCondition::campaign_station_destroyed;
                pending_outcome_ = BattleOutcome{condition, *winner, impl_->teams.at(*winner), tick, before->entity_id,
                    tick + victory_countdown_frames};
                pending_victory_event_ = Event{tick, EventKind::victory, *winner, 0, before->entity_id};
            }
        }
    }
    const auto& types = rules.condition == VictoryCondition::all_enemy_units_destroyed ? rules.relevant_types : rules.starbase_types;
    const auto relevant = [&](const UnitState* unit) {
        return unit != nullptr && std::binary_search(types.begin(), types.end(), unit->type_id);
    };
    if (!relevant(before) && !relevant(after)) return;
    if (before != nullptr && after == nullptr && !victory_removed_.insert(before->entity_id).second) return;
    // VT-02, WCC-40: publish pending victory in the ordered destruction hook,
    // before any later damage delivery. Only relevant lifecycle hooks visit player/base rows.
    const bool all_units = rules.condition == VictoryCondition::all_enemy_units_destroyed;
    const auto count_of = [&](const PlayerId owner) -> std::uint64_t& {
        const auto player = std::lower_bound(impl_->setup.players.begin(), impl_->setup.players.end(), owner,
            [](const Player& entry, const PlayerId value) { return entry.player_id < value; });
        return victory_counts_[static_cast<std::size_t>(player - impl_->setup.players.begin())];
    };
    bool lost_base = false;
    if (relevant(before)) {
        if (all_units) {
            auto& count = count_of(before->owner);
            if (count == 0) {
                victory_error_ = detail::diagnostic(diagnostic_codes::worker_failure,
                    "victory relevance count underflow for unit " + std::to_string(before->entity_id));
                return;
            }
            --count;
        } else {
            const auto base = std::find_if(pending_starbases_.begin(), pending_starbases_.end(),
                [&](const StarbaseEntry& entry) { return entry.unit == before->entity_id; });
            lost_base = base != pending_starbases_.end();
            if (lost_base) pending_starbases_.erase(base);
        }
    }
    if (relevant(after)) {
        if (all_units) ++count_of(after->owner);
        else if (std::binary_search(rules.contenders.begin(), rules.contenders.end(), after->owner)) {
            const auto position = std::lower_bound(pending_starbases_.begin(), pending_starbases_.end(), after->entity_id,
                [](const StarbaseEntry& entry, const EntityId id) { return entry.unit < id; });
            pending_starbases_.insert(position, StarbaseEntry{after->entity_id, after->owner});
        }
    }
    if (pending_outcome_ || !evaluate || !before || (!all_units && !lost_base)) return;
    std::optional<PlayerId> winner;
    if (all_units) {
        std::array<OwnerUnitCount, max_players> counts{};
        for (std::size_t index = 0; index < impl_->setup.players.size(); ++index) {
            counts[index] = {impl_->setup.players[index].player_id, victory_counts_[index]};
        }
        winner = all_units_destroyed_winner(rules, impl_->setup.players, before->owner,
            std::span<const OwnerUnitCount>{counts.data(), impl_->setup.players.size()});
    } else {
        winner = starbase_destroyed_winner(rules, impl_->setup.players, before->owner, pending_starbases_);
    }
    if (winner) {
        pending_outcome_ = BattleOutcome{rules.condition, *winner, impl_->teams.at(*winner), tick,
            before->entity_id, tick + rules.countdown_frames};
        pending_victory_event_ = Event{tick, EventKind::victory, *winner, 0, before->entity_id};
    }
}

core::Result<void> session_detail::Tick::finalize() {
    const auto& arrivals = staging_.value().arrivals.value();
    const auto& combat_events = targets_.value().combat_events.value();
    const auto& frame = tracked_.value().frame.value();
    const auto& rebuilt = tracked_.value().rebuilt.value();
    const auto& members_before = tracked_.value().members_before.value();
    auto& events = impacts_.value().events.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    const auto& spins = fighters_.value().spins.value();
    const auto& world = targets_.value().world.value();
    const auto& view = world.value();
    const auto& table = impl_->motion;
    finalized_.emplace();
    auto& basic_ai = finalized_->basic_ai.emplace(impl_->basic_ai);
    auto& controls = finalized_->departure_controls.emplace(impl_->departure_controls);
    if (impl_->setup.departure_policy && !pending_quits_.empty()) {
        // MP-37: deactivate controllers at the frame tail; transfer objects without
        // killing them. Human priority precedes AI priority in ascending player lookup.
        const auto& humans = impl_->setup.departure_policy->humans;
        const auto active = [&](PlayerId player) {
            const auto left = [player](const PlayerQuit& quit) { return quit.player == player; };
            return std::none_of(impl_->quits.begin(), impl_->quits.end(), left)
                && std::none_of(pending_quits_.begin(), pending_quits_.end(), left);
        };
        std::map<PlayerId, PlayerId> recipients;
        for (const auto& quit : pending_quits_) {
            PlayerId recipient{};
            for (const bool human : {true, false}) {
                for (const auto& player : impl_->setup.players)
                    if (player.commandable() && active(player.player_id)
                        && impl_->allied(quit.player, player.player_id)
                        && std::binary_search(humans.begin(), humans.end(), player.player_id) == human) {
                        recipient = player.player_id;
                        break;
                    }
                if (recipient != 0) break;
            }
            controls.push_back({quit.player, recipient ? recipient : quit.player, quit.tick, recipient == 0});
            if (recipient) recipients.emplace(quit.player, recipient);
            else if (!std::binary_search(basic_ai.begin(), basic_ai.end(), quit.player))
                basic_ai.insert(std::lower_bound(basic_ai.begin(), basic_ai.end(), quit.player), quit.player);
        }
        if (!recipients.empty()) {
            std::vector<std::optional<UnitState>> before(survivors.size());
            std::vector<std::optional<core::Diagnostic>> errors(tick_partition_count);
            std::vector<CombatBonuses> categories(tick_partition_count * impl_->bonus_categories.size());
            std::map<EntityId, TypeId> containers;
            for (const auto& account : staging_->ledgers.value()) for (const auto& held : account.completed) {
                const auto host = std::lower_bound(survivors.begin(), survivors.end(), held.station,
                    [](const LiveUnit& unit, EntityId id) { return unit.state.entity_id < id; });
                if (host != survivors.end() && host->state.entity_id == held.station)
                    containers.emplace(held.station, host->state.type_id);
            }
            const auto transferred = executor.execute_phase("departure-transfer", tick_partition_count,
                [&](std::size_t partition) {
                    const auto range = partition_range(partition, survivors.size());
                    for (auto index = range.begin; index < range.end; ++index) {
                        auto& unit = survivors[index];
                        const auto recipient = recipients.find(unit.state.owner);
                        if (recipient == recipients.end()) continue;
                        before[index] = unit.state;
                        const auto scratch = std::span(categories).subspan(
                            partition * impl_->bonus_categories.size(), impl_->bonus_categories.size());
                        const auto bonuses = impl_->command_bonuses_for(unit, impl_->command_ledger,
                            staging_->ledgers.value(), containers, scratch, nullptr, true, recipient->second);
                        auto changed = impl_->transfer_owner(unit, recipient->second, bonuses);
                        if (!changed) { errors[partition] = changed.error(); break; }
                        instances[index].owner = recipient->second;
                    }
                });
            if (!transferred) return transferred;
            for (const auto& error : errors) if (error) return core::Result<void>::failure(*error);
            // Sparse ordered notifications reconcile victory accounting after worker writes.
            for (std::size_t index = 0; index < before.size(); ++index)
                if (before[index]) {
                    impl_->adjust_owned(*before[index], false);
                    impl_->adjust_owned(survivors[index].state, true);
                    if (auto share = staging_->shares.value().find(survivors[index].state.entity_id);
                        share != staging_->shares.value().end()) share->second.owner = survivors[index].state.owner;
                    track_victory_change(&*before[index], &survivors[index].state, false);
                }
        }
    }
    if (radar_dirty_) {
        auto radar = impl_->publish_named_abilities(survivors, named_ability_locks_,
            radar_roster_ ? *radar_roster_ : impl_->radar_roster, executor);
        if (!radar) return core::Result<void>::failure(radar.error());
        finalized_->players = std::move(radar).value();
    } else finalized_->players = impl_->snapshot_players;
    if (!projectile_defence_order_.empty()) {
        std::vector<std::uint8_t> registered(survivors.size()), jammer_only(survivors.size());
        const auto prepared = executor.execute_phase("projectile-defence-lifecycle", tick_partition_count,
            [&](const std::size_t partition) {
            const auto range = partition_range(partition, survivors.size());
            for (auto slot = range.begin; slot < range.end; ++slot) {
                const auto& unit = survivors[slot];
                const auto* combat = impl_->combat.find(unit.state.type_id);
                if (!combat || (unit.durability && unit.durability->hull.raw() <= 0)) continue;
                const auto* ability = impl_->abilities.find(unit.state.type_id);
                const bool static_source = combat->passive_missile_shield_radius.raw() > 0
                    || (ability && ability_slot(*ability, AbilityKind::missile_shield));
                const auto jammer = ability ? ability_slot(*ability, AbilityKind::sensor_jamming) : std::nullopt;
                const bool active_jammer = jammer && unit.abilities && unit.abilities->slots[*jammer].active;
                registered[slot] = static_source || active_jammer ? 1U : 0U;
                jammer_only[slot] = active_jammer && !static_source ? 1U : 0U;
            }
        });
        if (!prepared) return prepared;
        if (std::none_of(jammer_only.begin(), jammer_only.end(), [](const auto value) { return value != 0; })) {
            projectile_defence_order_.clear(); // static creation order needs no additional state
        } else {
            std::vector<EntityId> retained;
            for (std::size_t slot = 0; slot < survivors.size(); ++slot)
                if (registered[slot]) retained.push_back(survivors[slot].state.entity_id);
            std::erase_if(projectile_defence_order_, [&](const EntityId id) {
                return !std::binary_search(retained.begin(), retained.end(), id);
            });
            // New static sources append after already registered jammers; their IDs
            // follow creation order. Activation notifications already retained command order.
            for (const auto id : retained)
                if (std::find(projectile_defence_order_.begin(), projectile_defence_order_.end(), id) == projectile_defence_order_.end())
                    projectile_defence_order_.push_back(id);
        }
    }
    std::vector<UnitState> positions;
    positions.reserve(survivors.size());
    for (const auto& unit : survivors) {
        positions.push_back(unit.state);
    }

    // Visibility phase: after every per-unit system, workers read one sensor field built
    // from the tick's final positions and fill the same disjoint slots.
    // WR-35/39: only the few active arrivals supply disabled IDs; the field's existing
    // observer build filters them. Model visibility at 35 never enables a sensor.
    // WR-43: the disabled service never releases what the creation frame marked at the drop point,
    // so the fog grid keeps that circle (the arrivals map is ascending by ID).
    std::vector<EntityId> disabled_revealers;
    std::vector<Impl::HeldReveal> held_reveals;
    for (const auto& [id, arrival] : arrivals) {
        if (arrival.frame >= arrival_reveal_frame) continue;
        disabled_revealers.push_back(id);
        held_reveals.push_back({id, arrival.exit});
    }
    auto field = SensorField::build(impl_->setup.players, positions, impl_->sensors, disabled_revealers,
        bonuses_->command_ledger ? bonuses_->command_ledger->reveal_multipliers : impl_->command_ledger.reveal_multipliers);
    if (!field) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "tick " + std::to_string(tick) + ": " + field.error().message));
    }
    const auto& sensing = field.value();
    // Fog cells (#274): the grids due this tick are serviced, then revealers release and mark
    // their circles, from the same final positions.
    // A transactional view of the session's shared rows: pointer metadata only, never grids.
    // Keeping the old rows until visibility succeeds also preserves failed-tick atomicity.
    auto& fog = finalized_.value().fog.emplace();
    if (impl_->fog) fog.emplace(*impl_->fog);
    if (fog) {
        for (const auto player : pending_reveals_) {
            if (auto revealed = fog->reveal_all(player, executor); !revealed) return revealed;
        }
        // V-19 (#495): each shot of a revealing unit shows its shooter to the owner of the unit it
        // fired at, from where it fired. One entry per shot in event order, read serially: the
        // shots are few and their order is the commit order.
        std::vector<FogFlash> flashes;
        for (const auto& event : combat_events) {
            if (event.kind != CombatEventKind::weapon_fired) {
                continue;
            }
            const auto* shooter = view.find(event.shooter);
            const auto* target = view.find(event.target);
            if (shooter == nullptr || target == nullptr || !sensing.reveal_range(shooter->type_id)) {
                continue;
            }
            const auto* profile = sensing.profile(shooter->type_id);
            const auto x = math::add(shooter->position.x, profile->flash_offset.x);
            const auto y = math::add(shooter->position.y, profile->flash_offset.y);
            if (!x || !y) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                "fire reveal centre exceeds the coordinate range"));
            flashes.push_back(FogFlash{target->owner, {x.value(), y.value(), shooter->position.z}, profile->flash_radius});
        }
        auto revealers = Impl::revealers(sensing, positions, held_reveals);
        if (!impl_->ability_spawns.empty()) {
            std::array<std::vector<FogRevealer>, tick_partition_count> projectile_revealers;
            const auto gathered = executor.execute_phase("projectile-reveal-sources", tick_partition_count, [&](const std::size_t partition) {
                const auto range = partition_range(partition, impl_->ability_spawns.size());
                for (auto index = range.begin; index < range.end; ++index) {
                    const auto& spawn = impl_->ability_spawns[index];
                    if (spawn.detonated || spawn.kind != AbilityKind::fow_reveal_ping) continue;
                    const auto* profile = impl_->spawn_profile(spawn.type);
                    if (!profile || !profile->reveal_range) continue;
                    // WFP-01: distinct source identity, owner team only, through the ordinary fog service.
                    projectile_revealers[partition].push_back({spawn.id | (std::uint64_t{1} << 63), spawn.owner,
                        spawn.position, *profile->reveal_range, profile->dense_reveal_multiplier});
                }
            });
            if (!gathered) return gathered;
            for (const auto& part : projectile_revealers) revealers.insert(revealers.end(), part.begin(), part.end());
            std::sort(revealers.begin(), revealers.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        }
        std::array<PlayerId,max_players> forced{}; std::size_t forced_count{};
        if (impl_->setup.campaign && (impl_->fog_modifiers_dirty || bonuses_->command_ledger)) {
            const auto current=impl_->fog_multipliers(bonuses_->command_ledger ? *bonuses_->command_ledger : impl_->command_ledger);
            for(std::size_t i=0;i<impl_->setup.players.size();++i) if(current[i]!=impl_->last_fog_multipliers[i]) forced[forced_count++]=impl_->setup.players[i].player_id;
            finalized_->fog_multipliers=current;
        }
        const auto advanced = fog->advance(tick, true, revealers, executor, flashes,
            std::span<const PlayerId>(forced.data(),forced_count));
        if (!advanced) {
            return core::Result<void>::failure(advanced.error());
        }
    }
    const FogCells* cells = fog ? &*fog : nullptr;
    // Spinning craft (#447) are seen like their owner's units at their spin positions.
    auto& spinning = finalized_.value().spinning.emplace(spins.size());
    std::vector<std::optional<core::Diagnostic>> spin_pose_errors(spins.size());
    const auto sensed = executor.execute_phase("visibility", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, positions.size());
        for (auto index = range.begin; index < range.end; ++index) {
            // FS-09/FD-06: completed removal detaches the held ship target at
            // frame end, including deaths after targeting. The next admitted
            // turn therefore starts without it; keep every scan/weapon clock.
            // Read the immutable final identities and write only this survivor.
            auto& combat = survivors[index].combat;
            if (combat && combat->attack_target != invalid_entity_id) {
                const auto target = std::lower_bound(positions.cbegin(), positions.cend(), combat->attack_target,
                    [](const UnitState& unit, const EntityId id) { return unit.entity_id < id; });
                if (target == positions.cend() || target->entity_id != combat->attack_target) {
                    combat->attack_target = invalid_entity_id;
                    combat->attack_hardpoint = no_hardpoint;
                    combat->direct = false;
                }
            }
            instances[index].visible_to = impl_->visible_to(sensing, cells, positions[index].owner, positions[index].position,
                positions[index].type_id, positions[index].rotation);
            instances[index].reveal_range = std::binary_search(disabled_revealers.begin(), disabled_revealers.end(), positions[index].entity_id)
                ? std::nullopt : sensing.reveal_range(positions[index].type_id, positions[index].owner);
        }
        const auto spun = partition_range(partition, spins.size());
        for (auto index = spun.begin; index < spun.end; ++index) {
            const auto& spin = spins[index];
            auto rotation = spin_rotation(spin);
            if (!rotation) {
                spin_pose_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " spinning craft " + std::to_string(spin.unit) + ": "
                        + rotation.error().message);
                continue;
            }
            auto transform = math::to_matrix(rotation.value(), spin.position);
            if (!transform) {
                spin_pose_errors[index] = detail::diagnostic(diagnostic_codes::worker_failure,
                    "tick " + std::to_string(tick) + " spinning craft " + std::to_string(spin.unit) + ": "
                        + transform.error().message);
                continue;
            }
            spinning[index] = SpinningCraft{spin.unit, spin.type, spin.owner, transform.value(), spin.roll, spin.pitch,
                spin.yaw, impl_->visible_to(sensing, cells, spin.owner, spin.position, spin.type, rotation.value())};
        }
    });
    if (!sensed) {
        return core::Result<void>::failure(sensed.error());
    }
    for (auto& error : spin_pose_errors) {
        if (error) {
            return core::Result<void>::failure(std::move(*error));
        }
    }
    // #530 PU-36, PU-37: an arriving unit's instance carries its arrival frame; while hidden only
    // its own team sees it. Serial over the arrivals.
    for (const auto& [id, arrival] : arrivals) {
        const auto found = std::lower_bound(positions.begin(), positions.end(), id,
            [](const UnitState& unit, const EntityId value) { return unit.entity_id < value; });
        if (found == positions.end() || found->entity_id != id) continue;
        auto& instance = instances[static_cast<std::size_t>(found - positions.begin())];
        instance.arrival = arrival.frame;
        instance.arrival_exit = arrival.exit;
        if (arrival.frame == arrival_visible_frame) {
            // WR-37: retain the authored cue's pose/visibility without changing simulation events.
            pending_economy_cues_.push_back({found->owner, found->type_id, tick,
                BattleEconomyCue::Kind::arrival, found->position, instance.visible_to});
        }
        if (arrival.frame < arrival_visible_frame) {
            const auto team = impl_->teams.at(found->owner);
            std::uint64_t mask = 0;
            for (std::size_t index = 0; index < impl_->setup.players.size(); ++index) {
                if (impl_->setup.players[index].team_id == team) mask |= std::uint64_t{1} << index;
            }
            instance.visible_to = mask;
        }
    }

    // A layer that a submission or a member change rebuilt this tick rolls from this frame.
    auto& anchors = finalized_.value().anchors.emplace(impl_->tracking_anchor);
    if (table.avoidance) {
        const auto members_after = layer_members(table, survivors);
        const auto in_layer = [](const std::vector<std::pair<std::size_t, EntityId>>& members, const std::size_t index) {
            std::vector<EntityId> ids;
            for (const auto& [layer, id] : members) {
                if (layer == index) ids.push_back(id);
            }
            return ids;
        };
        for (std::size_t index = 0; index < anchors.size(); ++index) {
            if (rebuilt[index] || in_layer(members_before, index) != in_layer(members_after, index)) anchors[index] = frame;
        }
    }

    if (victory_error_) return core::Result<void>::failure(*victory_error_);
    if (bounty_error_) return core::Result<void>::failure(*bounty_error_);
    finalized_.value().starbases.emplace(pending_starbases_);
    auto& outcome = finalized_.value().outcome.emplace(pending_outcome_);
    if (pending_victory_event_) events.push_back(*pending_victory_event_);

    // WBF-45: retain sparse notifications in delivery order for mounted scoring Lua.
    if (!pending_losses_.empty()) {
        auto& losses = finalized_.value().losses.emplace(*impl_->losses);
        losses.insert(losses.end(), pending_losses_.begin(), pending_losses_.end());
    }
    if (!pending_productions_.empty()) {
        auto& productions = finalized_.value().productions.emplace(*impl_->productions);
        productions.insert(productions.end(), pending_productions_.begin(), pending_productions_.end());
    }
    if (!pending_economy_cues_.empty()) {
        auto& cues = finalized_.value().economy_cues.emplace(*impl_->economy_cues);
        cues.insert(cues.end(), pending_economy_cues_.begin(), pending_economy_cues_.end());
    }
    auto& quits = finalized_.value().quits.emplace(impl_->quits);
    for (const auto& quit : pending_quits_) {
        const auto position = std::lower_bound(quits.begin(), quits.end(), quit.player,
            [](const PlayerQuit& entry, const PlayerId id) { return entry.player < id; });
        quits.insert(position, quit);
        // WBF-43: preserve a matured winner (and the first immediate quit result),
        // otherwise intentional local departure selects an enemy without a countdown.
        if (outcome && (outcome->end_tick <= tick || outcome->condition == VictoryCondition::intentional_quit)) continue;
        const auto active = [&](const PlayerId id) {
            return std::none_of(quits.begin(), quits.end(), [id](const PlayerQuit& entry) { return entry.player == id; });
        };
        const auto remaining = std::count_if(impl_->setup.players.begin(), impl_->setup.players.end(),
            [&](const Player& player) {
                // MP-38, LAN MVP owner policy: AI slots do not keep a one-human
                // network match open. Legacy local games retain controller counting.
                return player.commandable() && active(player.player_id)
                    && (!impl_->setup.departure_policy || std::binary_search(
                        impl_->setup.departure_policy->humans.begin(), impl_->setup.departure_policy->humans.end(), player.player_id));
            });
        const bool local_departure = !impl_->setup.departure_policy
            && std::binary_search(impl_->victory.humans.begin(), impl_->victory.humans.end(), quit.player);
        if (!local_departure && remaining >= 2) continue;
        const auto* leaving = impl_->player_of(quit.player);
        const auto enemy = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(), [&](const Player& player) {
            return player.commandable() && active(player.player_id) && player.team_id != leaving->team_id
                && (!impl_->setup.departure_policy || std::binary_search(
                    impl_->setup.departure_policy->humans.begin(), impl_->setup.departure_policy->humans.end(), player.player_id));
        });
        // WBF-43's unresolved fallback; ordinary two-player local skirmish finds its enemy above.
        auto winner = enemy != impl_->setup.players.end() ? enemy->player_id : quit.player;
        if (impl_->setup.departure_policy && enemy == impl_->setup.players.end() && remaining == 1) {
            const auto survivor = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(), [&](const Player& player) {
                return player.commandable() && active(player.player_id) && std::binary_search(
                    impl_->setup.departure_policy->humans.begin(), impl_->setup.departure_policy->humans.end(), player.player_id);
            });
            winner = survivor->player_id;
        }
        outcome = BattleOutcome{VictoryCondition::intentional_quit, winner, impl_->teams.at(winner), tick,
            invalid_entity_id, tick + 1U};
    }
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::commit() {
    const auto& tick_work = gather_.value().tick_work.value();
    auto& minds = craft_prep_.value().minds.value();
    auto& crafts = staging_.value().crafts.value();
    auto& arrivals = staging_.value().arrivals.value();
    auto& ledgers = staging_.value().ledgers.value();
    auto& respawns = staging_.value().respawns.value();
    auto& shares = staging_.value().shares.value();
    auto& earners = staging_.value().earners.value();
    const auto& next_id = staging_.value().next_id.value();
    auto& collection = targets_.value().collection.value();
    const auto& next_projectile = flights_.value().next_projectile.value();
    auto& pending_blast_damage = impacts_.value().pending_blast_damage.value();
    auto& projectiles = impacts_.value().projectiles.value();
    const auto& due_end = commands_.value().due_end.value();
    const auto& initial_emplacements = metrics_.value().initial_emplacements.value();
    auto& survivors = surviving_.value().survivors.value();
    auto& spins = fighters_.value().spins.value();
    auto& squadrons = fighters_.value().squadrons.value();
    auto& spawners = fighters_.value().spawners.value();
    auto& fog = finalized_.value().fog.value();
    const auto& anchors = finalized_.value().anchors.value();
    auto& starbases = finalized_.value().starbases.value();
    const auto& outcome = finalized_.value().outcome.value();
    auto& quits = finalized_.value().quits.value();
    const auto& sources_changed = bonuses_.value().sources_changed.value();
    // Commit in stable-ID order. Sparse map journals restore their entries on earlier failures.
    committed_.emplace();
    auto& registry_component_writes = committed_.value().registry_component_writes.emplace();
    committed_.value().staged_map_copies.emplace(crafts.copied_elements() + minds.copied_elements() + spawners.copied_elements());
    mark("rebuild_registry", true);
    impl_->commit_units(survivors, registry_component_writes);
    committed_.value().registry_emplacements.emplace(impl_->registry_emplacement_count - initial_emplacements);
    mark("rebuild_registry", false);
    impl_->starbases = std::move(starbases);
    impl_->outcome = outcome;
    impl_->retreat_state = std::move(pending_retreat_);
    impl_->midbattle_solve = pending_midbattle_solve_;
    impl_->rng_state = pending_rng_;
    impl_->quits = std::move(quits);
    impl_->beacons = std::move(beacons_);
    impl_->basic_ai = std::move(finalized_->basic_ai.value());
    impl_->departure_controls = std::move(finalized_->departure_controls.value());
    if (finalized_.value().losses) impl_->losses = std::make_shared<const std::vector<BattleLoss>>(std::move(*finalized_.value().losses));
    if (finalized_.value().productions) impl_->productions = std::make_shared<const std::vector<BattleProduction>>(std::move(*finalized_.value().productions));
    if (finalized_.value().economy_cues) impl_->economy_cues = std::make_shared<const std::vector<BattleEconomyCue>>(std::move(*finalized_.value().economy_cues));
    impl_->tracking_anchor = anchors;
    impl_->squadrons = std::move(squadrons);
    impl_->free_garrisons = std::move(fighters_.value().free_garrisons.value());
    impl_->tick_work = tick_work;
    crafts.commit();
    minds.commit();
    spawners.commit();
    respawns.commit();
    impl_->spins = std::move(spins);
    impl_->collection = std::move(collection);
    impl_->projectile_collection = std::move(targets_.value().projectile_collection.value());
    impl_->next_id = next_id;
    impl_->manual_clocks = std::move(manual_clocks_);
    impl_->special_weapons = std::move(special_weapons_);
    impl_->special_disabled_by_hardpoint = std::move(special_disabled_by_hardpoint_);
    impl_->named_ability_locks = std::move(named_ability_locks_);
    if (radar_roster_) impl_->radar_roster = std::move(*radar_roster_);
    impl_->snapshot_players = std::move(finalized_->players.value());
    impl_->projectile_defence_order = std::move(projectile_defence_order_);
    impl_->ledgers = std::move(ledgers);
    if (bonuses_.value().command_ledger) impl_->command_ledger = std::move(*bonuses_.value().command_ledger);
    if (sources_changed && !impl_->bonus_categories.empty()) impl_->committed_bonus_profiles.swap(impl_->bonus_profiles);
    impl_->pad_stage.commit(impl_->pads);
    impl_->construction_stage.commit(impl_->construction);
    impl_->arrivals = std::move(arrivals);
    impl_->shares = std::move(shares);
    impl_->earners = std::move(earners);
    impl_->fog = std::move(fog);
    if (finalized_->fog_multipliers) { impl_->last_fog_multipliers=*finalized_->fog_multipliers; impl_->fog_modifiers_dirty=false; }
    impl_->projectiles = std::move(projectiles);
    impl_->pending_blast_damage = std::move(pending_blast_damage);
    impl_->next_projectile = next_projectile;
    for (auto iterator = impl_->pending.begin(); iterator != due_end; ++iterator) {
        const auto& searches = commands_->searches;
        const auto search = std::lower_bound(searches.begin(), searches.end(), iterator->first,
            [](const Commands::Search& entry, const CommandKey& key) { return entry.key < key; });
        if (search != searches.end() && search->key == iterator->first) {
            std::get<ReinforcePayload>(iterator->second.payload).position = search->result.position;
            impl_->reinforcement_search_results.insert_or_assign(
                std::pair{iterator->first.player_id, search->request.token}, search->result);
            impl_->reinforcement_searches.erase(iterator->first);
            // SAE-10: only a successful search becomes an ordinary recorded command.
            if (!search->result.valid) continue;
        }
        impl_->executed.push_back(std::move(iterator->second));
    }
    impl_->pending.erase(impl_->pending.begin(), due_end);
    ++impl_->completed_tick;
    return core::Result<void>::success();
}

core::Result<TacticalTick> session_detail::Tick::finish() {
    const auto& reinforcement_work = gather_.value().reinforcement_work.value();
    const auto& dogfight_cone_tests = craft_prep_.value().dogfight_cone_tests.value();
    const auto& closing_squadrons = moved_.value().closing_squadrons.value();
    auto& combat_events = targets_.value().combat_events.value();
    const auto& projectile_candidates = flights_.value().projectile_candidates.value();
    const auto& projectile_exact_tests = flights_.value().projectile_exact_tests.value();
    const auto& blast_detonations = flights_.value().blast_detonations.value();
    const auto& blast_recipients_examined = flights_.value().blast_recipients_examined.value();
    const auto& production_census_visits = tracked_.value().production_census_visits.value();
    auto& events = impacts_.value().events.value();
    auto& diagnostics = commands_.value().diagnostics.value();
    const auto& capture_index_bodies = systems_.value().capture_index_bodies.value();
    const auto& capture_prepare_bodies = systems_.value().capture_prepare_bodies.value();
    auto& instances = systems_.value().instances.value();
    const auto& capture_candidates = surviving_.value().capture_candidates.value();
    const auto& bonus_profile_evaluations = bonuses_.value().bonus_profile_evaluations.value();
    auto& spinning = finalized_.value().spinning.value();
    const auto& registry_component_writes = committed_.value().registry_component_writes.value();
    const auto& staged_map_copies = committed_.value().staged_map_copies.value();
    const auto& registry_emplacements = committed_.value().registry_emplacements.value();
    std::vector<AsteroidImpact> asteroid_impacts;
    std::uint64_t asteroid_queries = 0;
    std::uint64_t asteroid_candidates = 0;
    for (std::size_t slot = 0; slot < impl_->asteroid_scratch.impacts.size(); ++slot) {
        auto& impacts = impl_->asteroid_scratch.impacts[slot];
        asteroid_impacts.insert(asteroid_impacts.end(), std::make_move_iterator(impacts.begin()), std::make_move_iterator(impacts.end()));
        asteroid_queries += impl_->asteroid_scratch.queries[slot];
        asteroid_candidates += impl_->asteroid_scratch.examined[slot];
    }
    std::string hash;
    StateHash state_hash;
    if (impl_->hasher) {
        state_hash = impl_->hasher->hash(impl_->canonical_bytes());
    } else {
        hash = session_.state_sha256();
        state_hash = StateHash(hash);
    }
    // #424: the squadrons' targets for the world UI (presentation only, not hashed).
    std::vector<SquadronTarget> squadron_targets;
    for (const auto& [container, mind] : impl_->minds) {
        if (mind.target != invalid_entity_id) {
            squadron_targets.push_back({container, mind.target, closing_squadrons.contains(container), mind.cell.has_value(), mind.joined,
                mind.cell ? mind.cell->x : 0, mind.cell ? mind.cell->y : 0});
        }
    }
    impl_->current_snapshot = std::make_shared<const TacticalSnapshot>(impl_->completed_tick, impl_->snapshot_players,
        std::move(instances), std::move(events), std::move(combat_events), impl_->projectiles, impl_->outcome,
        std::move(spinning), std::move(squadron_targets), impl_->squadrons,
        impl_->economy_views(impl_->ledgers, impl_->shares), impl_->pad_views(), impl_->quits, impl_->losses, impl_->productions,
        impl_->manual_clock_views(), impl_->economy_cues, impl_->ability_spawns, session_.retreat_views(), impl_->garrison_timer_views(), impl_->beacons);
    std::vector<std::pair<EntityId, PlayerId>> deaths;
    if (impl_->setup.first_free_entity_id) {
        deaths.reserve(loss_killers_.size());
        for (const auto& [id, entry] : loss_killers_) deaths.emplace_back(id, entry.second);
    }
    return core::Result<TacticalTick>::success(TacticalTick{
        impl_->completed_tick,
        std::move(hash),
        impl_->current_snapshot,
        std::move(diagnostics),
        projectile_candidates,
        projectile_exact_tests,
        dogfight_cone_tests,
        std::move(state_hash),
        registry_emplacements,
        registry_component_writes,
        staged_map_copies,
        production_census_visits,
        bonus_profile_evaluations,
        capture_candidates,
        capture_index_bodies,
        capture_prepare_bodies,
        0,
        blast_detonations,
        blast_recipients_examined,
        reinforcement_work.collision_queries,
        reinforcement_work.nanoseconds,
        reinforcement_work.rejections,
        std::move(asteroid_impacts),
        asteroid_queries,
        asteroid_candidates,
        std::move(deaths),
    });
}

} // namespace eawr::sim::tactical
