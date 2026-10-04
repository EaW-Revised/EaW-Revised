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

void session_detail::Tick::track_victory_change(const UnitState* before, const UnitState* after, const bool evaluate) {
    const auto& rules = impl_->victory;
    if (rules.condition == VictoryCondition::none) return;
    const auto& types = rules.condition == VictoryCondition::all_enemy_units_destroyed ? rules.relevant_types : rules.starbase_types;
    const auto relevant = [&](const UnitState* unit) {
        return unit != nullptr && std::binary_search(types.begin(), types.end(), unit->type_id);
    };
    if (!relevant(before) && !relevant(after)) return;
    if (before != nullptr && after == nullptr && !victory_removed_.insert(before->entity_id).second) return;
    const auto object = [](const UnitState* unit) -> std::optional<VictoryObject> {
        return unit != nullptr ? std::optional(VictoryObject{unit->entity_id, unit->type_id, unit->owner}) : std::nullopt;
    };
    victory_changes_.push_back(VictoryChange{object(before), object(after), evaluate});
}

core::Result<void> session_detail::Tick::finalize() {
    const auto& arrivals = staging_.value().arrivals.value();
    const auto& combat_events = targets_.value().combat_events.value();
    const auto& frame = tracked_.value().frame.value();
    const auto& rebuilt = tracked_.value().rebuilt.value();
    const auto& members_before = tracked_.value().members_before.value();
    auto& events = impacts_.value().events.value();
    auto& instances = systems_.value().instances.value();
    const auto& survivors = surviving_.value().survivors.value();
    const auto& spins = fighters_.value().spins.value();
    const auto& world = targets_.value().world.value();
    const auto& view = world.value();
    const auto& table = impl_->motion;
    std::vector<UnitState> positions;
    positions.reserve(survivors.size());
    for (const auto& unit : survivors) {
        positions.push_back(unit.state);
    }

    // Visibility phase: after every per-unit system, workers read one sensor field built
    // from the tick's final positions and fill the same disjoint slots.
    // WR-35/39: only the few active arrivals supply disabled IDs; the field's existing
    // observer build filters them. Model visibility at 35 never enables a sensor.
    std::vector<EntityId> disabled_revealers;
    for (const auto& [id, arrival] : arrivals) {
        if (arrival.frame < arrival_reveal_frame) disabled_revealers.push_back(id);
    }
    auto field = SensorField::build(impl_->setup.players, positions, impl_->sensors, disabled_revealers);
    if (!field) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
            "tick " + std::to_string(tick) + ": " + field.error().message));
    }
    const auto& sensing = field.value();
    // Fog cells (#274): the grids due this tick are serviced, then revealers release and mark
    // their circles, from the same final positions.
    // A transactional view of the session's shared rows: pointer metadata only, never grids.
    // Keeping the old rows until visibility succeeds also preserves failed-tick atomicity.
    finalized_.emplace();
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
            flashes.push_back(FogFlash{target->owner, shooter->position});
        }
        const auto advanced = fog->advance(tick, true, Impl::revealers(sensing, positions, disabled_revealers), executor, flashes);
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
            instances[index].visible_to = impl_->visible_to(sensing, cells, positions[index].owner, positions[index].position);
            instances[index].reveal_range = std::binary_search(disabled_revealers.begin(), disabled_revealers.end(), positions[index].entity_id)
                ? std::nullopt : sensing.reveal_range(positions[index].type_id);
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
                spin.yaw, impl_->visible_to(sensing, cells, spin.owner, spin.position)};
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

    // WBF-31/32/35/37: consume sparse lifecycle notifications in their ordered commit
    // order. All-unit counts came from worker gather; each hook visits only player rows.
    // Later losses of this tick still stand at an earlier hook, preserving the first winner.
    auto& starbases = finalized_.value().starbases.emplace(impl_->starbases);
    auto& outcome = finalized_.value().outcome.emplace(impl_->outcome);
    if (impl_->victory.condition != VictoryCondition::none) {
        std::optional<Event> decided;
        const auto& rules = impl_->victory;
        const bool all_units = rules.condition == VictoryCondition::all_enemy_units_destroyed;
        std::array<OwnerUnitCount, max_players> counts{};
        for (std::size_t index = 0; index < impl_->setup.players.size(); ++index) {
            counts[index] = {impl_->setup.players[index].player_id, victory_counts_[index]};
        }
        const auto& types = all_units ? rules.relevant_types : rules.starbase_types;
        const auto relevant = [&](const std::optional<VictoryObject>& object) {
            return object && std::binary_search(types.begin(), types.end(), object->type);
        };
        const auto count_of = [&](const PlayerId owner) -> std::uint64_t& {
            const auto player = std::lower_bound(impl_->setup.players.begin(), impl_->setup.players.end(), owner,
                [](const Player& entry, const PlayerId value) { return entry.player_id < value; });
            return counts[static_cast<std::size_t>(player - impl_->setup.players.begin())].units;
        };
        for (const auto& change : victory_changes_) {
            bool lost_base = false;
            if (relevant(change.before)) {
                if (all_units) {
                    auto& count = count_of(change.before->owner);
                    if (count == 0) return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::worker_failure,
                        "victory relevance count underflow for unit " + std::to_string(change.before->unit)));
                    --count;
                } else {
                    const auto base = std::find_if(starbases.begin(), starbases.end(),
                        [&](const StarbaseEntry& entry) { return entry.unit == change.before->unit; });
                    lost_base = base != starbases.end();
                    if (lost_base) starbases.erase(base);
                }
            }
            if (relevant(change.after)) {
                if (all_units) ++count_of(change.after->owner);
                else if (std::binary_search(rules.contenders.begin(), rules.contenders.end(), change.after->owner)) {
                    starbases.push_back(StarbaseEntry{change.after->unit, change.after->owner});
                }
            }
            if (outcome || !change.evaluate || !change.before || (!all_units && !lost_base)) continue;
            const auto winner = all_units
                ? all_units_destroyed_winner(rules, impl_->setup.players, change.before->owner,
                    std::span<const OwnerUnitCount>{counts.data(), impl_->setup.players.size()})
                : starbase_destroyed_winner(rules, impl_->setup.players, change.before->owner, starbases);
            if (winner) {
                outcome = BattleOutcome{rules.condition, *winner, impl_->teams.at(*winner), tick, change.before->unit,
                    tick + rules.countdown_frames};
                decided = Event{tick, EventKind::victory, *winner, 0, change.before->unit};
            }
        }
        if (decided) {
            events.push_back(*decided);
        }
        std::sort(starbases.begin(), starbases.end(), [](const StarbaseEntry& left, const StarbaseEntry& right) { return left.unit < right.unit; });
    }

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
            [&](const Player& player) { return player.commandable() && active(player.player_id); });
        const bool local_departure = std::binary_search(impl_->victory.humans.begin(), impl_->victory.humans.end(), quit.player);
        if (!local_departure && remaining >= 2) continue;
        const auto* leaving = impl_->player_of(quit.player);
        const auto enemy = std::find_if(impl_->setup.players.begin(), impl_->setup.players.end(), [&](const Player& player) {
            return player.commandable() && active(player.player_id) && player.team_id != leaving->team_id;
        });
        // WBF-43's unresolved fallback; ordinary two-player local skirmish finds its enemy above.
        const auto winner = enemy != impl_->setup.players.end() ? enemy->player_id : quit.player;
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
    impl_->quits = std::move(quits);
    if (finalized_.value().losses) impl_->losses = std::make_shared<const std::vector<BattleLoss>>(std::move(*finalized_.value().losses));
    if (finalized_.value().productions) impl_->productions = std::make_shared<const std::vector<BattleProduction>>(std::move(*finalized_.value().productions));
    if (finalized_.value().economy_cues) impl_->economy_cues = std::make_shared<const std::vector<BattleEconomyCue>>(std::move(*finalized_.value().economy_cues));
    impl_->tracking_anchor = anchors;
    impl_->squadrons = std::move(squadrons);
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
    impl_->ledgers = std::move(ledgers);
    if (bonuses_.value().command_ledger) impl_->command_ledger = std::move(*bonuses_.value().command_ledger);
    if (sources_changed && !impl_->bonus_categories.empty()) impl_->committed_bonus_profiles.swap(impl_->bonus_profiles);
    impl_->pad_stage.commit(impl_->pads);
    impl_->construction_stage.commit(impl_->construction);
    impl_->arrivals = std::move(arrivals);
    impl_->shares = std::move(shares);
    impl_->earners = std::move(earners);
    impl_->fog = std::move(fog);
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
        impl_->manual_clock_views(), impl_->economy_cues, impl_->ability_spawns);
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
    });
}

} // namespace eawr::sim::tactical
