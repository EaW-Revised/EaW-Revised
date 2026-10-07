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

core::Result<void> session_detail::Tick::pad_lifecycle() {
    auto& tick_work = gather_.value().tick_work.value();
    auto& respawns = staging_.value().respawns.value();
    auto& earners = staging_.value().earners.value();
    auto& next_id = staging_.value().next_id.value();
    auto& events = impacts_.value().events.value();
    auto& killed = impacts_.value().killed.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    auto& pads = impl_->pad_stage.values;
    auto& construction = impl_->construction_stage.values;
    const auto& durability = impl_->durability;
    // WBP-16/18/19: ordered lifecycle commit of the partitioned object services.
    // Damage has resolved first; lethal damage wins over a same-frame deadline (U-BP-1).
    const auto pad_survivor = [&](const EntityId id) -> LiveUnit* {
        const auto found = std::lower_bound(survivors.begin(), survivors.end(), id,
            [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
        return found != survivors.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    std::vector<EntityId> replaced_children;
    std::optional<core::Diagnostic> transfer_error;
    // WBP-27/28: ordered child-death notification, distinct from bare detach.
    const auto child_died = [&](const EntityId parent_id, PadState& state) -> bool {
        auto* parent = pad_survivor(parent_id);
        if (parent == nullptr) return false;
        const auto* profile = impl_->economy.pads.point(parent->state.type_id);
        if (profile == nullptr) return false;
        impl_->pad_stage.touch(parent_id);
        state.under_construction = invalid_entity_id;
        state.constructed = invalid_entity_id;
        if (profile->destroy_when_child_dies) {
            killed.push_back(parent->state);
            events.push_back(destruction_event(tick, EventKind::unit_destroyed, parent->state));
            replaced_children.push_back(parent_id);
        } else {
            const auto before = parent->state;
            // WNO-23/42: surviving-pad reclamation is also a real ownership transfer.
            // This notification visits only the child-death event and held bonus objects.
            std::map<EntityId, TypeId> containers;
            for (const auto& account : staging_->ledgers.value()) for (const auto& held : account.completed) {
                if (const auto* host = pad_survivor(held.station)) containers.emplace(held.station, host->state.type_id);
            }
            std::vector<CombatBonuses> categories(impl_->bonus_categories.size());
            const auto bonuses = impl_->command_bonuses_for(*parent, impl_->command_ledger,
                staging_->ledgers.value(), containers, categories, nullptr, true, impl_->economy.pads.neutral);
            if (const auto changed = impl_->transfer_owner(*parent, impl_->economy.pads.neutral, bonuses); !changed) {
                transfer_error = changed.error();
                return false;
            }
            track_victory_change(&before, &parent->state, true);
            if (before.owner != parent->state.owner)
                events.push_back(Event{tick, EventKind::pad_captured, parent->state.owner, before.owner, parent_id});
            state.target = impl_->economy.pads.neutral;
            state.progress = {};
            state.cooldown_start = profile->rebuild_frames != 0 ? tick : 0;
            state.cooldown_until = profile->rebuild_frames != 0 ? tick + profile->rebuild_frames : 0;
            const auto instance = std::lower_bound(instances.begin(), instances.end(), parent_id,
                [](const TacticalInstance& entry, const EntityId id) { return entry.entity_id < id; });
            if (instance != instances.end() && instance->entity_id == parent_id)
                *instance = impl_->instance_for(*parent, instance->fixed_transform, tick + 1);
        }
        return profile->destroy_when_child_dies;
    };
    for (auto iterator = construction.begin(); iterator != construction.end();) {
        const auto id = iterator->first;
        const auto state = iterator->second;
        auto* child = pad_survivor(id);
        auto* parent = pad_survivor(state.parent);
        const auto pad = pads.find(state.parent);
        if (child == nullptr || parent == nullptr || pad == pads.end()) {
            if (child == nullptr && parent != nullptr && pad != pads.end()) child_died(state.parent, pad->second);
            impl_->construction_stage.touch(id);
            if (pad != pads.end()) impl_->pad_stage.touch(state.parent);
            if (pad != pads.end()) pad->second.under_construction = invalid_entity_id;
            if (child != nullptr) replaced_children.push_back(id);
            iterator = construction.erase(iterator);
            continue;
        }
        if (tick < state.finish_frame) { ++iterator; continue; }
        impl_->construction_stage.touch(id);
        impl_->pad_stage.touch(state.parent);
        const auto* profile = impl_->economy.pads.child(child->state.type_id);
        const auto* final_health = profile != nullptr ? durability.find(profile->constructed) : nullptr;
        if (profile != nullptr && final_health != nullptr && next_id != invalid_entity_id
            && pad->second.under_construction == id) {
            const auto final_id = next_id;
            auto position = child->state.position;
            // WBP-18: the completed child is raised 0.1 code units above the attachment.
            const auto raised = math::add(position.z, math::Fixed::from_raw(math::Fixed::scale / 10));
            if (!raised) return core::Result<void>::failure(raised.error());
            position.z = raised.value();
            auto final = impl_->new_unit(UnitState{final_id, profile->constructed, state.builder,
                position, parent->state.rotation, {}}, tick);
            const auto fraction = math::divide(child->durability->hull, durability.find(child->state.type_id)->max_hull);
            if (!fraction) return core::Result<void>::failure(fraction.error());
            const auto hull = math::multiply(final_health->max_hull, fraction.value());
            if (!hull) return core::Result<void>::failure(hull.error());
            pad->second.constructed = final_id;
            events.push_back(Event{tick, EventKind::pad_construction_completed, state.builder, 0, final_id});
            pending_productions_.push_back({state.builder, profile->constructed, tick});
            final.durability->hull = hull.value();
            const auto transform = unit_transform(final);
            if (!transform) return core::Result<void>::failure(transform.error());
            instances.push_back(impl_->instance_for(final, transform.value(), tick + 1));
            track_victory_change(nullptr, &final.state, false);
            survivors.push_back(std::move(final));
            next_id = final_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : final_id + 1;
            // WBP-22/WPR-16: admission precedes this frame's income pass.
            if (impl_->economy.stream(profile->constructed) != nullptr) earners.emplace_back(final_id, state.builder);
        }
        pad->second.under_construction = invalid_entity_id;
        replaced_children.push_back(id);
        iterator = construction.erase(iterator);
    }
    // WBP-27: freeze a sorted prefix for prior UC replacements. Final-child deaths
    // append removals but return their parent result directly, preserving O(P log D)
    // membership work without searching a growing removal vector for every pad.
    std::sort(replaced_children.begin(), replaced_children.end());
    const auto prior_removals = replaced_children.size();
    const auto previously_removed = [&](const EntityId id) {
        ++tick_work.pad_death_membership_work;
        return std::binary_search(replaced_children.begin(), replaced_children.begin() + prior_removals, id,
            [&](const EntityId left, const EntityId right) {
                ++tick_work.pad_death_membership_work;
                return left < right;
            });
    };
    // Commit the matching parent links; removed parents never retain their old child.
    for (auto iterator = pads.begin(); iterator != pads.end();) {
        if (pad_survivor(iterator->first) == nullptr
            || previously_removed(iterator->first)) {
            impl_->pad_stage.touch(iterator->first);
            if (iterator->second.constructed != invalid_entity_id) replaced_children.push_back(iterator->second.constructed);
            iterator = pads.erase(iterator);
        } else {
            if (iterator->second.constructed != invalid_entity_id && pad_survivor(iterator->second.constructed) == nullptr) {
                if (child_died(iterator->first, iterator->second)) {
                    iterator = pads.erase(iterator);
                    continue;
                }
            }
            ++iterator;
        }
    }
    // WHZ-52/WBP-29/49: only actual deaths schedule; UC replacement and detach do not.
    // This serial queue visits death/creation events, never the whole live world.
    if (transfer_error) return core::Result<void>::failure(*transfer_error);
    for (const auto& dead : killed) {
        const auto replacement = respawn_after_death(dead, impl_->economy.pads);
        if (!replacement) continue;
        const auto due = tick + impl_->economy.pads.replacement(dead.type_id)->frames;
        respawns[due].objects.push_back(*replacement);
    }
    if (!replaced_children.empty()) {
        std::sort(replaced_children.begin(), replaced_children.end());
        std::erase_if(survivors, [&](const LiveUnit& unit) {
            const bool removed = std::binary_search(replaced_children.begin(), replaced_children.end(), unit.state.entity_id);
            // The sparse journal suppresses a duplicate notification for a dead parent.
            if (removed) track_victory_change(&unit.state, nullptr, false);
            return removed;
        });
        std::erase_if(instances, [&](const TacticalInstance& instance) {
            return std::binary_search(replaced_children.begin(), replaced_children.end(), instance.entity_id);
        });
    }

    mark("survivors", false);
    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::station_upgrades() {
    auto& minds = craft_prep_.value().minds.value();
    auto& ledgers = staging_.value().ledgers.value();
    auto& earners = staging_.value().earners.value();
    auto& next_id = staging_.value().next_id.value();
    const auto& frame = tracked_.value().frame.value();
    auto& events = impacts_.value().events.value();
    auto& projectiles = impacts_.value().projectiles.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    auto& spawners = fighters_.value().spawners.value();
    const auto& squadron_table = impl_->motion.squadrons;
    const auto increase_tech = [&](const PlayerId owner) {
        for (auto& account : ledgers) {
            if (!impl_->allied(owner, account.player)) continue;
            const auto maximum = impl_->economy.player(account.player)->max_tech;
            if (account.tech_level < maximum) ++account.tech_level;
        }
    };
    // WSL-31/WFO-31: service held objects before the late queues can create more.
    // The initial WSL-31 deadline is creation + an inclusive 0..2 draw. Traversal
    // has already ended at creation, so the first eligible service is 1 or 2 frames later.
    // WPR-52: ordered lifecycle commit over held upgrades, not a new live-world sweep.
    for (auto& account : ledgers) {
        for (std::size_t index = 0; index < account.completed.size();) {
            const auto held = account.completed[index];
            const auto* upgrade = impl_->economy.upgrade(held.type);
            if (upgrade == nullptr || !upgrade->level_up) { ++index; continue; }
            if (!held.level_up_service_frame || tick < *held.level_up_service_frame) { ++index; continue; }
            const auto* previous = survivor(held.station);
            if (previous == nullptr) { account.completed.erase(account.completed.begin() + static_cast<std::ptrdiff_t>(index)); continue; }
            const auto old = *previous;
            const auto* menu = impl_->economy.menu(old.state.type_id, impl_->player_of(old.state.owner)->faction_id);
            if (menu == nullptr || menu->next_level == 0
                || impl_->economy.disabled_types.contains(held.type)
                || impl_->economy.disabled_types.contains(menu->next_level)) {
                // WSL-31/32: the first eligible service attempts once, even without a next type.
                account.completed.erase(account.completed.begin() + static_cast<std::ptrdiff_t>(index));
                continue;
            }
            if (next_id == invalid_entity_id || next_id == std::numeric_limits<EntityId>::max()) {
                return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit, "station replacement ID space exhausted"));
            }
            // 1. New type, same owner, position and facing; it receives a fresh stable ID.
            const auto replacement_id = next_id++;
            auto replacement = impl_->new_unit(UnitState{replacement_id, menu->next_level, old.state.owner,
                old.state.position, old.state.rotation, {}}, tick);
            replacement.state.garrison_enabled = old.state.garrison_enabled; // FL-13: upgrades preserve the object flag
            // 2. Carry hardpoints by list index; disabled/destroyed become disabled at 0.1.
            if (replacement.durability && old.durability) {
                carry_station_hardpoints(*impl_->health_profile(old), *old.durability, *replacement.durability);
            }
            // WPR-53 (project): references follow the logical station; invalid slots fall back to hull.
            const auto* replacement_health = impl_->health_profile(replacement);
            const auto transfer = [&](EntityId& target, std::uint32_t& hardpoint) {
                if (target != held.station) return;
                target = replacement_id;
                if (hardpoint != no_hardpoint && (replacement_health == nullptr
                    || !damage_target_valid(*replacement_health, hardpoint))) hardpoint = no_hardpoint;
            };
            std::vector<SquadronState*> squadron_refs;
            for (const auto& [id, mind] : minds) {
                if (mind.target == held.station || mind.escorted == held.station) squadron_refs.push_back(&minds.at(id));
            }
            const auto transferred = executor.execute_phase("station-references", tick_partition_count,
                [&](const std::size_t partition) {
                    const auto units = partition_range(partition, survivors.size());
                    for (auto slot = units.begin; slot < units.end; ++slot) {
                        auto& unit = survivors[slot];
                        transfer(unit.state.order.target, unit.state.order.hardpoint);
                        if (unit.combat) {
                            transfer(unit.combat->attack_target, unit.combat->attack_hardpoint);
                            for (auto& weapon : unit.combat->weapons) {
                                if (weapon.opportunity.target == held.station) weapon.opportunity.target = replacement_id;
                            }
                        }
                        if (unit.abilities) for (auto& ability : unit.abilities->slots) transfer(ability.target, ability.target_hardpoint);
                    }
                    const auto squads = partition_range(partition, squadron_refs.size());
                    for (auto slot = squads.begin; slot < squads.end; ++slot) {
                        auto& mind = *squadron_refs[slot];
                        transfer(mind.target, mind.target_hardpoint);
                        if (mind.escorted == held.station) mind.escorted = replacement_id;
                    }
                    const auto shots = partition_range(partition, projectiles.size());
                    for (auto slot = shots.begin; slot < shots.end; ++slot) {
                        auto& shot = projectiles[slot];
                        transfer(shot.target, shot.target_hardpoint);
                        if (shot.shooter == held.station) shot.shooter = replacement_id;
                    }
                });
            if (!transferred) return core::Result<void>::failure(transferred.error());
            // 3/4. Move held objects and every allied queue entry, then raise allied tech.
            account.completed.erase(account.completed.begin() + static_cast<std::ptrdiff_t>(index));
            for (auto& ledger : ledgers) {
                for (auto& object : ledger.completed) if (object.station == held.station) object.station = replacement_id;
                if (impl_->allied(old.state.owner, ledger.player)) for (auto& queue : ledger.queues) {
                    for (auto& entry : queue) if (entry.station == held.station) entry.station = replacement_id;
                }
            }
            increase_tech(account.player);
            // 5. Removal without a destruction event: no kill, explosion or victory test.
            track_victory_change(&old.state, &replacement.state, false);
            std::erase_if(survivors, [&](const LiveUnit& unit) { return unit.state.entity_id == held.station; });
            std::erase_if(instances, [&](const TacticalInstance& unit) { return unit.entity_id == held.station; });
            survivors.push_back(std::move(replacement));
            instances.push_back(impl_->instance_for(survivors.back(),
                math::to_matrix(old.state.rotation, old.state.position).value(), tick + 1));
            std::erase_if(earners, [&](const auto& earner) {
                return earner.first == held.station && impl_->economy.stream(menu->next_level) == nullptr;
            });
            for (auto& earner : earners) if (earner.first == held.station) earner.first = replacement_id;
            // WPR-56 (project), FL-02/08: reconcile by squadron type, never by an old entry index.
            const auto* new_hangar = squadron_table.find_spawner(menu->next_level);
            const auto* old_hangar = squadron_table.find_spawner(old.state.type_id);
            // WFO-12: a station born during object service waits for a subsequent traversal.
            auto next_hangar = initial_spawner(impl_->setup.seed, frame + 1, replacement_id);
            if (new_hangar != nullptr) {
                next_hangar.ready = true;
                next_hangar.next_spawn_frame = frame;
                for (const auto& entry : new_hangar->entries) {
                    auto remaining = entry.reserve < 0 ? unlimited_reserve : entry.reserve + entry.starting;
                    const auto prior = spawners.find(held.station);
                    if (remaining != unlimited_reserve && old_hangar != nullptr && prior != spawners.end() && prior->second.ready) {
                        const auto match = std::find_if(old_hangar->entries.begin(), old_hangar->entries.end(),
                            [&](const SpawnEntryProfile& previous_entry) { return previous_entry.squadron == entry.squadron; });
                        if (match != old_hangar->entries.end() && match->reserve != unlimited_reserve) {
                            const auto slot = static_cast<std::size_t>(match - old_hangar->entries.begin());
                            if (slot < prior->second.entries.size()) remaining = std::max(0,
                                remaining - (match->reserve + match->starting - prior->second.entries[slot].remaining));
                        }
                    }
                    next_hangar.entries.push_back({0, remaining});
                }
            }
            for (const auto& [id, previous_mind] : minds) {
                if (previous_mind.spawner != held.station) continue;
                auto& mind = minds.at(id);
                const auto match = new_hangar != nullptr ? std::find_if(new_hangar->entries.begin(), new_hangar->entries.end(),
                    [&](const SpawnEntryProfile& entry) { return entry.squadron == mind.squadron_type; }) : std::vector<SpawnEntryProfile>::const_iterator{};
                if (new_hangar == nullptr || match == new_hangar->entries.end()) {
                    mind.spawner = invalid_entity_id; mind.entry = 0;
                } else {
                    mind.spawner = replacement_id;
                    mind.entry = static_cast<std::uint32_t>(match - new_hangar->entries.begin());
                    ++next_hangar.entries[mind.entry].alive;
                }
            }
            if (const auto prior = spawners.find(held.station); prior != spawners.end()) spawners.erase(prior);
            if (new_hangar != nullptr) spawners.emplace(replacement_id, std::move(next_hangar));
            // 6. The presentation event moves selection and provides the faction sound trigger.
            events.push_back(Event{tick, EventKind::station_replaced, old.state.owner, replacement_id, held.station});
        }
    }

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::economy() {
    auto& arrivals = staging_.value().arrivals.value();
    auto& ledgers = staging_.value().ledgers.value();
    auto& shares = staging_.value().shares.value();
    auto& earners = staging_.value().earners.value();
    auto& next_id = staging_.value().next_id.value();
    auto& production_census_visits = tracked_.value().production_census_visits.value();
    auto& survivors = surviving_.value().survivors.value();
    // #530 economy service (PU-02 to PU-05, PU-16, PU-18): serial per economy player, after the
    // frame's commands and destructions. It visits the few income stations, queue entries and
    // population shares, never every unit.
    std::erase_if(earners, [&](const auto& entry) {
        const auto* source = survivor(entry.first);
        return source == nullptr || impl_->economy.stream(source->state.type_id) == nullptr;
    });
    std::erase_if(shares, [&](const auto& entry) { return survivor(entry.first) == nullptr; });
    std::erase_if(arrivals, [&](const auto& entry) { return survivor(entry.first) == nullptr; });
    // WPR-33/55: queues reuse flat partition scratch; buys shared one census with ordered deltas.
    const bool queued_production = std::any_of(ledgers.begin(), ledgers.end(), [&](const PlayerEconomy& ledger) {
        return std::any_of(ledger.queues.begin(), ledger.queues.end(), [&](const auto& queue) {
            return std::any_of(queue.begin(), queue.end(), [&](const QueueEntry& entry) {
                const auto* station = survivor(entry.station);
                const auto* option = station != nullptr ? impl_->build_option(*station, ledger.player, entry.type) : nullptr;
                return option != nullptr && (option->requirements.current_player || option->requirements.current_allies
                    || !option->requirements.prerequisites.empty());
            });
        });
    });
    if (queued_production) {
        const auto counted = impl_->count_owned(survivors, executor);
        if (!counted) return core::Result<void>::failure(counted.error());
        production_census_visits += survivors.size();
    }
    static_cast<void>(impl_->prune_upgrade_holders(ledgers, survivors));
    const auto increase_tech = [&](const PlayerId owner, const bool same_faction) {
        for (auto& account : ledgers) {
            if (!impl_->allied(owner, account.player)) continue;
            if (same_faction && impl_->player_of(owner)->faction_id != impl_->player_of(account.player)->faction_id) continue;
            const auto maximum = impl_->economy.player(account.player)->max_tech;
            if (account.tech_level < maximum) ++account.tech_level;
        }
    };
    bool upgrade_ids_exhausted = false;
    const auto modifier_service = impl_->service_income_modifiers(ledgers, survivors, earners, tick, executor);
    if (!modifier_service) return modifier_service;
    std::array<std::optional<core::Diagnostic>, tick_partition_count> income_errors{};
    // WBP-22/23/26, PU-02..04: workers read live ownership and standing bonuses once
    // per source. Retained disjoint outputs keep this phase allocation-free after warm-up.
    auto& payments = impl_->income_payments;
    payments.resize(earners.size());
    const auto source_payments = [&](const std::size_t partition) {
        const auto range = partition_range(partition, earners.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto* source = survivor(earners[index].first);
            const auto* stream = impl_->economy.stream(source->state.type_id);
            auto amount = stream->per_frame.raw();
            const auto* hull = impl_->health_profile(*source);
            for (const auto& bonus : stream->bonuses) {
                const bool standing = bonus.hardpoint == always_on
                    || (source->durability && hull != nullptr && bonus.hardpoint < hull->hardpoints.size()
                        && !hardpoint_destroyed(*hull, *source->durability, bonus.hardpoint)
                        && !hardpoint_disabled(*source->durability, bonus.hardpoint));
                if (standing) amount += bonus.per_frame.raw();
            }
            if (!impl_->income_category_parts.empty()) {
                const auto count = impl_->income_category_parts.size() / tick_partition_count;
                auto categories = std::span(impl_->income_category_parts).subspan(partition * count, count);
                impl_->reduce_income(ledgers, source->state.entity_id, categories);
                const bool modified = std::any_of(categories.begin(), categories.end(), [](const IncomeCategory& category) {
                    return std::any_of(category.winners.begin(), category.winners.end(), [](const auto& winner) { return winner.has_value(); });
                });
                if (modified) {
                    const auto bonus = math::Fixed::from_raw(amount - stream->per_frame.raw());
                    const auto rate = modified_income_per_frame(stream->base_value, stream->interval_seconds, categories);
                    if (!rate) { income_errors[partition] = rate.error(); break; }
                    const auto total = math::add(rate.value(), bonus);
                    if (!total) { income_errors[partition] = total.error(); break; }
                    amount = total.value().raw();
                }
            }
            if (stream->split_with_allies && !stream->full_amount_to_everyone) {
                std::int64_t recipients = 0;
                for (const auto& ledger : ledgers) recipients += impl_->allied(source->state.owner, ledger.player) ? 1 : 0;
                if (recipients != 0) amount /= recipients;
            }
            payments[index] = {source->state.owner, amount, stream->split_with_allies};
        }
    };
    if (!earners.empty()) {
        const auto paid = executor.execute_phase("income-sources", tick_partition_count,
            [&source_payments](const std::size_t partition) { source_payments(partition); });
        if (!paid) return core::Result<void>::failure(paid.error());
    }
    for (const auto& error : income_errors) if (error) return core::Result<void>::failure(*error);
    // Ordered ledger commit over the few players and admitted sources, not world entities.
    for (std::size_t index = 0; index < earners.size(); ++index) earners[index].second = payments[index].owner;
    for (auto& ledger : ledgers) {
        const auto* player = impl_->economy.player(ledger.player);
        for (const auto& payment : payments) {
            if (payment.owner != ledger.player && (!payment.split || !impl_->allied(payment.owner, ledger.player))) continue;
            const auto balance = change_credits(ledger, *player, math::Fixed::from_raw(payment.amount));
            if (!balance) return core::Result<void>::failure(balance.error());
        }
        // PU-18: an entry whose station is gone or no longer offers its type is dropped.
        const auto produced = service_production(ledger, *player, tick,
            [&](const QueueEntry& entry) {
                const auto* station = survivor(entry.station);
                const auto* option = station != nullptr ? impl_->build_option(*station, ledger.player, entry.type) : nullptr;
                const bool allowed = option != nullptr && production_allowed(*option, false,
                    [&](const TypeId type) { return impl_->production_counts(ledger.player, type, ledgers); });
                // WPR-20/31: validity removal goes through the same local cancel feedback.
                if (!allowed) pending_economy_cues_.push_back({ledger.player, entry.type, tick, BattleEconomyCue::Kind::cancelled});
                return allowed;
            },
            [&](const QueueEntry& entry) {
                const auto* station = survivor(entry.station);
                const auto* option = station != nullptr ? impl_->build_option(*station, ledger.player, entry.type) : nullptr;
                return option != nullptr ? option->kind : BuildKind::unit;
            },
            [&](const QueueEntry& entry) {
                const auto* upgrade = impl_->economy.upgrade(entry.type);
                pending_productions_.push_back({ledger.player, entry.type, tick});
                if (upgrade == nullptr) return;
                if (!ledger.completed.empty()) {
                    if (next_id == invalid_entity_id || next_id == std::numeric_limits<EntityId>::max()) {
                        upgrade_ids_exhausted = true;
                        return;
                    }
                    auto& held = ledger.completed.back();
                    held.object = next_id++;
                    if (upgrade->level_up) {
                        // WSL-31: code interval 2; initial deadline includes both endpoints.
                        CombatRandom phase(impl_->setup.seed, tick, held.object, station_upgrade_phase_slot);
                        held.level_up_service_frame = tick + phase.uniform(0, 2);
                    }
                }
                // WPR-22 steps 4 and 5, before the next queue entry becomes front.
                if (upgrade->removes_previous != 0) for (auto& account : ledgers) {
                    if (impl_->allied(ledger.player, account.player)) {
                        std::erase_if(account.completed, [&](const CompletedBuild& held) {
                            if (held.type != upgrade->removes_previous) return false;
                            impl_->retire_income_modifier(account, held);
                            return true;
                        });
                    }
                }
                if (upgrade->increments_tech) increase_tech(ledger.player, true);
            });
        if (!produced) return produced;
    }

    if (upgrade_ids_exhausted) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "upgrade object ID space exhausted"));
    }
    // WFO-31/WBP-45: queue-created objects activate after current income, before
    // ordinary services in the next traversal. Activation never repays earlier frames.
    const auto activated_modifiers = impl_->service_income_modifiers(ledgers, survivors, earners, tick, executor, true);
    if (!activated_modifiers) return activated_modifiers;

    return core::Result<void>::success();
}

core::Result<void> session_detail::Tick::bonuses() {
    const auto& ledgers = staging_.value().ledgers.value();
    const auto& next_id = staging_.value().next_id.value();
    auto& instances = systems_.value().instances.value();
    auto& survivors = surviving_.value().survivors.value();
    // WPR-51: existing and newly created units, on disjoint partition slots.
    bonuses_.emplace();
    if (survivors.size() != impl_->concentrate_service_census
        || (!survivors.empty() && survivors.back().state.entity_id != impl_->concentrate_service_last_id)) {
        if (auto serviced = impl_->service_concentrate(survivors, tick, executor); !serviced) return serviced;
    }
    const auto& sources_changed = bonuses_.value().sources_changed.emplace(std::any_of(ledgers.begin(), ledgers.end(), [&](const PlayerEconomy& account) {
        const auto old = std::find_if(impl_->ledgers.begin(), impl_->ledgers.end(),
            [&](const PlayerEconomy& previous) { return previous.player == account.player; });
        return old == impl_->ledgers.end() || old->completed.size() != account.completed.size()
            || !std::equal(old->completed.begin(), old->completed.end(), account.completed.begin(),
                [](const CompletedBuild& before, const CompletedBuild& after) {
                    return before.type == after.type && before.station == after.station && before.object == after.object;
                });
    }));
    auto& bonus_profile_evaluations = bonuses_.value().bonus_profile_evaluations.emplace(0);
    const bool command_events = !impl_->economy.command_bonuses.empty()
        && (next_id != impl_->next_id || survivors.size() != impl_->handles.size());
    if (command_events) {
        auto ledger = impl_->update_command_ledger(survivors, impl_->next_id, executor);
        if (!ledger) return core::Result<void>::failure(ledger.error());
        bonuses_.value().command_ledger.emplace(std::move(ledger).value());
    }
    const bool command_sources_changed = command_events && (bonuses_.value().command_ledger->sources.size() != impl_->command_ledger.sources.size()
        || !std::equal(bonuses_.value().command_ledger->sources.begin(), bonuses_.value().command_ledger->sources.end(),
            impl_->command_ledger.sources.begin(), [](const auto& before, const auto& after) {
                return before.id == after.id && before.profile == after.profile;
            }));
    if (!impl_->bonus_categories.empty() && (sources_changed || next_id != impl_->next_id || command_events)) {
        // Rebuild once per source change, by player/type; births only read the cached result.
        if (sources_changed) {
            std::map<EntityId, TypeId> containers;
            for (const auto& account : ledgers) for (const auto& object : account.completed) {
                if (const auto* station = survivor(object.station)) containers.emplace(object.station, station->state.type_id);
            }
            const auto profiles = impl_->build_bonus_profiles(ledgers, containers, executor);
            if (!profiles) return core::Result<void>::failure(profiles.error());
            bonus_profile_evaluations = impl_->ownership_keys.size();
        }
        std::array<std::optional<core::Diagnostic>, tick_partition_count> bonus_errors{};
        const auto first_bonus_unit = sources_changed || command_sources_changed ? std::size_t{0} : static_cast<std::size_t>(
            std::lower_bound(survivors.begin(), survivors.end(), impl_->next_id,
                [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; }) - survivors.begin());
        const bool shared_modifiers = !impl_->economy.command_bonuses.empty() || !impl_->concentrate_effects.empty();
        if (shared_modifiers) {
            const auto& ledger = bonuses_.value().command_ledger ? *bonuses_.value().command_ledger : impl_->command_ledger;
            const auto changed = impl_->apply_command_bonuses(survivors, ledger, ledgers,
                sources_changed || command_sources_changed ? 0 : impl_->next_id, executor);
            if (!changed) return changed;
        }
        const auto applied = executor.execute_phase("upgrade-bonuses", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, survivors.size() - first_bonus_unit);
            for (auto offset = range.begin; offset < range.end; ++offset) {
                const auto index = first_bonus_unit + offset;
                auto& unit = survivors[index];
                if (!shared_modifiers) {
                    const auto bonuses = impl_->bonuses_for(unit, !sources_changed);
                    if (unit.upgrade_bonuses == bonuses) continue;
                    const auto changed = impl_->apply_bonuses(unit, bonuses);
                    if (!changed) { bonus_errors[partition] = changed.error(); break; }
                }
                instances[index] = impl_->instance_for(unit, math::to_matrix(unit.state.rotation, unit.state.position).value(), tick + 1);
            }
        });
        if (!applied) return core::Result<void>::failure(applied.error());
        for (const auto& error : bonus_errors) if (error) return core::Result<void>::failure(*error);
    }

    const auto& current_command_ledger = bonuses_->command_ledger ? *bonuses_->command_ledger : impl_->command_ledger;
    if (auto refreshed = impl_->refresh_concentrate_bonuses(survivors, current_command_ledger, ledgers, executor); !refreshed) return refreshed;
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical
