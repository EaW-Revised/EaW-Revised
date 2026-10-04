#include "eawr/core/load_profile.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/combat_modifiers.hpp"

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


namespace eawr::sim::tactical {

std::optional<math::Vec3> reinforcement_search_candidate(const math::Vec3& requested,
    const std::uint32_t attempt, const math::Fixed yaw,
    const std::optional<std::array<math::Fixed, 4>>& bounds) {
    math::Vec3 point = requested;
    if (attempt != 0) {
        // SAE-10: positive 36-degree steps from the reverse arrival facing, +500 per ring.
        const auto ring = static_cast<std::int64_t>(1 + (attempt - 1) / 10);
        const auto angle = math::add(yaw, math::Fixed::from_integer(180 + 36 * ((attempt - 1) % 10)).value());
        if (!angle) return std::nullopt;
        const auto direction = planar_direction(angle.value());
        if (!direction) return std::nullopt;
        const auto radius = math::Fixed::from_integer(500 * ring);
        if (!radius) return std::nullopt;
        const auto dx = math::multiply(radius.value(), direction.value().x);
        const auto dy = math::multiply(radius.value(), direction.value().y);
        if (!dx || !dy) return std::nullopt;
        const auto x = math::add(requested.x, dx.value());
        const auto y = math::add(requested.y, dy.value());
        if (!x || !y) return std::nullopt;
        point.x = x.value(); point.y = y.value();
    }
    if (bounds) {
        point.x = std::clamp(point.x, (*bounds)[0], (*bounds)[2]);
        point.y = std::clamp(point.y, (*bounds)[1], (*bounds)[3]);
    }
    return point;
}

std::size_t TacticalSession::Impl::ownership_slot(const PlayerId player, const TypeId type) const {
        const OwnershipKey key{player, type};
        const auto found = std::lower_bound(ownership_keys.begin(), ownership_keys.end(), key);
        return found != ownership_keys.end() && *found == key ? static_cast<std::size_t>(found - ownership_keys.begin()) : ownership_keys.size();
    }

void TacticalSession::Impl::adjust_owned(const UnitState& unit, const bool added) {
        const auto slot = ownership_slot(unit.owner, purchase_identity(unit));
        if (slot == ownership_keys.size()) return;
        if (added) ++ownership_counts[slot];
        else if (ownership_counts[slot] != 0) --ownership_counts[slot];
    }

core::Result<void> TacticalSession::Impl::count_owned(const std::vector<LiveUnit>& units, const PartitionExecutor& executor) {
        std::fill(ownership_parts.begin(), ownership_parts.end(), 0);
        const auto counted = executor.execute_phase("production-counts", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, units.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& state = units[index].state;
                const auto slot = ownership_slot(state.owner, purchase_identity(state));
                if (slot != ownership_keys.size()) ++ownership_parts[partition * ownership_keys.size() + slot];
            }
        });
        if (!counted) return counted;
        std::fill(ownership_counts.begin(), ownership_counts.end(), 0);
        for (std::size_t partition = 0; partition < tick_partition_count; ++partition) {
            for (std::size_t slot = 0; slot < ownership_keys.size(); ++slot) ownership_counts[slot] += ownership_parts[partition * ownership_keys.size() + slot];
        }
        return core::Result<void>::success();
    }

CombatBonuses TacticalSession::Impl::bonuses_for(const LiveUnit& unit, const bool committed) const {
        const auto slot = ownership_slot(unit.state.owner, unit.state.type_id);
        return slot != ownership_keys.size() ? (committed ? committed_bonus_profiles[slot] : bonus_profiles[slot]) : CombatBonuses{};
    }

CombatBonuses TacticalSession::Impl::profile_bonuses(const OwnershipKey key, const std::vector<PlayerEconomy>& accounts,
    const std::map<EntityId, TypeId>& containers, const std::span<CombatBonuses> categories, const bool total) const {
        std::fill(categories.begin(), categories.end(), CombatBonuses{});
        for (const auto& account : accounts) {
            if (!allied(account.player, key.first)) continue;
            for (const auto& held : account.completed) {
                const auto container = containers.find(held.station);
                if (container == containers.end() || container->second == key.second) continue;
                const auto* upgrade = economy.upgrade(held.type);
                if (upgrade == nullptr) continue;
                for (const auto& bonus : upgrade->bonuses) {
                    if (!std::binary_search(bonus.applicable.begin(), bonus.applicable.end(), key.second)) continue;
                    auto& largest = categories[static_cast<std::size_t>(std::lower_bound(bonus_categories.begin(), bonus_categories.end(),
                        bonus.stacking_category) - bonus_categories.begin())];
                    for (std::size_t index = 0; index < largest.size(); ++index) {
                        largest[index] = std::max(largest[index], bonus.percentages[index]);
                    }
                }
            }
        }
        if (!total) return {};
        CombatBonuses result{};
        for (const auto& values : categories) {
            for (std::size_t index = 0; index < result.size(); ++index) {
                result[index] = math::Fixed::from_raw(result[index].raw() + values[index].raw());
            }
        }
        return result;
    }

core::Result<void> TacticalSession::Impl::build_bonus_profiles(const std::vector<PlayerEconomy>& accounts,
    const std::map<EntityId, TypeId>& containers, const PartitionExecutor& executor) {
        return executor.execute_phase("upgrade-profiles", tick_partition_count, [&](const std::size_t partition) {
            const auto range = partition_range(partition, ownership_keys.size());
            const auto categories = std::span(bonus_parts).subspan(partition * bonus_categories.size(), bonus_categories.size());
            for (auto slot = range.begin; slot < range.end; ++slot) {
                bonus_profiles[slot] = profile_bonuses(ownership_keys[slot], accounts, containers, categories);
            }
        });
    }

core::Result<TacticalSession::Impl::CommandLedger> TacticalSession::Impl::update_command_ledger(
    const std::vector<LiveUnit>& live, const EntityId first_new, const PartitionExecutor& executor) const {
    using Result = core::Result<CommandLedger>;
    CommandLedger result;
    const auto find = [&](const EntityId id) -> const LiveUnit* {
        const auto found = std::lower_bound(live.begin(), live.end(), id,
            [](const LiveUnit& unit, const EntityId value) { return unit.state.entity_id < value; });
        return found != live.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    const auto contained_type = [](auto&& self, const std::vector<CarriedObject>& objects, const EntityId id) -> TypeId {
        for (const auto& object : objects) {
            if (object.entity_id == id) return object.type_id;
            if (const auto type = self(self, object.members, id); type != 0) return type;
        }
        return 0;
    };
    std::array<std::vector<CommandSource>, tick_partition_count> retained;
    const auto retired = executor.execute_phase("command-source-retire-events", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, command_ledger.sources.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const auto& source = command_ledger.sources[index];
            const auto* host = find(source.host);
            if (host == nullptr) continue;
            const auto type = source.id == source.host ? host->state.type_id
                : contained_type(contained_type, host->state.contained, source.id);
            if (type == economy.command_bonuses[source.profile].type) retained[partition].push_back(source);
        }
    });
    if (!retired) return Result::failure(retired.error());
    for (auto& part : retained) result.sources.insert(result.sources.end(),
        std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    const auto first = static_cast<std::size_t>(std::lower_bound(live.begin(), live.end(), first_new,
        [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; }) - live.begin());
    std::array<std::vector<CommandSource>, tick_partition_count> born;
    const auto registered = executor.execute_phase("command-source-events", tick_partition_count, [&](const std::size_t partition) {
        const auto add = [&](const EntityId id, const EntityId host, const TypeId type) {
            const auto begin = std::lower_bound(economy.command_bonuses.begin(), economy.command_bonuses.end(), type,
                [](const CommandBonusProfile& profile, const TypeId value) { return profile.type < value; });
            for (auto it = begin; it != economy.command_bonuses.end() && it->type == type; ++it)
                born[partition].push_back({id, host, static_cast<std::size_t>(it - economy.command_bonuses.begin()), {}});
        };
        const auto riders = [&](auto&& self, const std::vector<CarriedObject>& objects, const EntityId host) -> void {
            for (const auto& object : objects) {
                add(object.entity_id, host, object.type_id);
                self(self, object.members, host);
            }
        };
        const auto range = partition_range(partition, live.size() - first);
        for (auto offset = range.begin; offset < range.end; ++offset) {
            const auto& unit = live[first + offset];
            add(unit.state.entity_id, unit.state.entity_id, unit.state.type_id);
            riders(riders, unit.state.contained, unit.state.entity_id);
        }
    });
    if (!registered) return Result::failure(registered.error());
    const auto existing = result.sources.size();
    for (auto& part : born) result.sources.insert(result.sources.end(),
        std::make_move_iterator(part.begin()), std::make_move_iterator(part.end()));
    const auto notified = executor.execute_phase("command-recipient-events", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, result.sources.size());
        for (auto index = range.begin; index < range.end; ++index) {
            auto& source = result.sources[index];
            const auto* host = find(source.host);
            const auto& profile = economy.command_bonuses[source.profile];
            std::erase_if(source.targets, [&](const EntityId target) { return find(target) == nullptr; });
            // WHE-53/54: existing applications survive an owner change; qualify only notifications.
            const auto begin = index < existing ? first : std::size_t{0};
            for (auto target_index = begin; target_index < live.size(); ++target_index) {
                const auto& target = live[target_index];
                if (!target.durability || (source.id == target.state.entity_id && !profile.apply_to_self)
                    || !std::binary_search(profile.bonus.applicable.begin(), profile.bonus.applicable.end(), target.state.type_id)) continue;
                const auto player = std::find_if(setup.players.begin(), setup.players.end(),
                    [&](const Player& value) { return value.player_id == target.state.owner; });
                if (profile.specific_faction != 0 ? player == setup.players.end() || player->faction_id != profile.specific_faction
                    : !allied(host->state.owner, target.state.owner)) continue;
                source.targets.push_back(target.state.entity_id);
            }
            std::sort(source.targets.begin(), source.targets.end());
            source.targets.erase(std::unique(source.targets.begin(), source.targets.end()), source.targets.end());
        }
    });
    if (!notified) return Result::failure(notified.error());
    std::sort(result.sources.begin(), result.sources.end(), [](const auto& left, const auto& right) {
        return std::pair{left.id, left.profile} < std::pair{right.id, right.profile};
    });
    // Ordered sparse commit: no target discovery or per-entity work in this reduction.
    for (std::size_t index = 0; index < result.sources.size(); ++index)
        for (const auto target : result.sources[index].targets) result.recipients[target].push_back(index);
    return Result::success(std::move(result));
}

CombatBonuses TacticalSession::Impl::command_bonuses_for(const LiveUnit& unit, const CommandLedger& ledger,
    const std::vector<PlayerEconomy>& accounts, const std::map<EntityId, TypeId>& containers,
    const std::span<CombatBonuses> categories, const UnitStage* effect_world, const bool targeted_effects) const {
    static_cast<void>(profile_bonuses({unit.state.owner, unit.state.type_id}, accounts, containers, categories, false));
    // WHE-55: zero is no contribution; a negative category starts at its first actual value.
    const auto found = ledger.recipients.find(unit.state.entity_id);
    if (found != ledger.recipients.end()) for (const auto source : found->second) {
        const auto& bonus = economy.command_bonuses[ledger.sources[source].profile].bonus;
        const auto category = static_cast<std::size_t>(std::lower_bound(bonus_categories.begin(), bonus_categories.end(),
            bonus.stacking_category) - bonus_categories.begin());
        auto contribution = bonus.percentages;
        if (!unit.durability || !health_profile(unit)->powered) contribution[2] = {};
        if (!unit.durability || durability.find(unit.state.type_id)->max_shields.raw() == 0) contribution[3] = {};
        accumulate_combat_bonus(categories[category], contribution);
    }
    const bool targeted = targeted_effects && accumulate_concentrate_bonus(unit.state.entity_id, categories, effect_world);
    return sum_combat_bonus_categories(categories, found != ledger.recipients.end() || targeted);
}

core::Result<void> TacticalSession::Impl::apply_command_bonuses(std::vector<LiveUnit>& live, const CommandLedger& ledger,
    const std::vector<PlayerEconomy>& accounts, const EntityId first, const PartitionExecutor& executor) const {
    std::map<EntityId, TypeId> containers;
    for (const auto& account : accounts) for (const auto& held : account.completed) {
        const auto found = std::lower_bound(live.begin(), live.end(), held.station,
            [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; });
        if (found != live.end() && found->state.entity_id == held.station) containers.emplace(held.station, found->state.type_id);
    }
    std::vector<CombatBonuses> scratch(tick_partition_count * bonus_categories.size());
    std::array<std::optional<core::Diagnostic>, tick_partition_count> errors{};
    const auto begin = static_cast<std::size_t>(std::lower_bound(live.begin(), live.end(), first,
        [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; }) - live.begin());
    const auto applied = executor.execute_phase("command-bonuses", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, live.size() - begin);
        const auto categories = std::span(scratch).subspan(partition * bonus_categories.size(), bonus_categories.size());
        for (auto offset = range.begin; offset < range.end; ++offset) {
            auto& unit = live[begin + offset];
            bool gained = false, lost = false;
            const auto contains = [&](const CommandLedger& records, const CommandSource& wanted) {
                const auto target = records.recipients.find(unit.state.entity_id);
                if (target == records.recipients.end()) return false;
                return std::any_of(target->second.begin(), target->second.end(), [&](const std::size_t source) {
                    const auto& value = records.sources[source];
                    return value.id == wanted.id && value.profile == wanted.profile;
                });
            };
            if (const auto target = ledger.recipients.find(unit.state.entity_id); target != ledger.recipients.end())
                for (const auto source : target->second) gained = gained || !contains(command_ledger, ledger.sources[source]);
            if (const auto target = command_ledger.recipients.find(unit.state.entity_id); target != command_ledger.recipients.end())
                for (const auto source : target->second) lost = lost || !contains(ledger, command_ledger.sources[source]);
            const auto adjustment = gained ? BonusAdjustment::gain : lost ? BonusAdjustment::loss : BonusAdjustment::legacy;
            const auto changed = apply_bonuses(unit, command_bonuses_for(unit, ledger, accounts, containers, categories), adjustment);
            if (!changed) { errors[partition] = changed.error(); break; }
        }
    });
    if (!applied) return applied;
    for (const auto& error : errors) if (error) return core::Result<void>::failure(*error);
    return core::Result<void>::success();
}

bool TacticalSession::Impl::prune_upgrade_holders(std::vector<PlayerEconomy>& accounts,
    const std::vector<LiveUnit>& live) const {
        bool changed = false;
        for (auto& account : accounts) {
            std::erase_if(account.completed, [&](const CompletedBuild& held) {
                const auto current = std::lower_bound(live.begin(), live.end(), held.station,
                    [](const LiveUnit& unit, const EntityId id) { return unit.state.entity_id < id; });
                if (current != live.end() && current->state.entity_id == held.station) return false;
                const auto* upgrade = economy.upgrade(held.type);
                // U-BP-6: mine-host loss does not establish deletion of a modifier object.
                if (upgrade != nullptr && !upgrade->income_modifiers.empty()) return false;
                changed = true;
                return true;
            });
        }
        return changed;
    }

core::Result<void> TacticalSession::Impl::apply_bonuses(LiveUnit& unit, const CombatBonuses& bonuses, const BonusAdjustment adjustment) const {
        using Void = core::Result<void>;
        if (unit.upgrade_bonuses == bonuses) return Void::success();
        const auto* base = durability.find(unit.state.type_id);
        if (base != nullptr && unit.durability) {
            auto changed = *base;
            const auto old = *health_profile(unit);
            const auto raise = [&](const math::Fixed initial, const std::size_t stat) {
                return math::multiply(initial, math::Fixed::from_raw(math::Fixed::scale + bonuses[stat].raw()));
            };
            const auto hull = raise(base->max_hull, 0);
            const auto shield = raise(base->max_shields, 3);
            const auto energy = raise(base->max_energy, 2);
            if (!hull || !shield || !energy) return Void::failure(!hull ? hull.error() : !shield ? shield.error() : energy.error());
            changed.max_hull = hull.value(); changed.max_shields = shield.value(); changed.max_energy = energy.value();
            const auto adjust = [adjustment](const math::Fixed current, const math::Fixed before, const math::Fixed after) {
                // WHE-18/19 (debug build): raising adds the delta; removing only clamps.
                const auto delta = adjustment == BonusAdjustment::gain ? after.raw() - before.raw()
                    : adjustment == BonusAdjustment::loss ? std::int64_t{0} : std::max<std::int64_t>(0, after.raw() - before.raw());
                return math::Fixed::from_raw(std::clamp(current.raw() + delta,
                    std::int64_t{0}, after.raw()));
            };
            auto state = *unit.durability;
            state.hull = adjust(state.hull, old.max_hull, changed.max_hull);
            state.shields = adjust(state.shields, old.max_shields, changed.max_shields);
            state.energy = adjust(state.energy, old.max_energy, changed.max_energy);
            const auto ratio = math::divide(math::Fixed::from_raw(math::Fixed::scale + bonuses[0].raw()),
                math::Fixed::from_raw(math::Fixed::scale + unit.upgrade_bonuses[0].raw()));
            if (!ratio) return Void::failure(ratio.error());
            for (std::size_t index = 0; index < changed.hardpoints.size(); ++index) {
                const auto maximum = raise(base->hardpoints[index].max_health, 0);
                const auto current = math::multiply(state.hardpoints[index], ratio.value());
                if (!maximum || !current) return Void::failure(!maximum ? maximum.error() : current.error());
                changed.hardpoints[index].max_health = maximum.value();
                state.hardpoints[index] = adjustment == BonusAdjustment::gain
                    || (adjustment == BonusAdjustment::legacy && bonuses[0] >= unit.upgrade_bonuses[0]) ? current.value()
                    : std::min(state.hardpoints[index], maximum.value());
            }
            unit.upgraded_durability = std::move(changed);
            unit.durability = std::move(state);
        }
        if (unit.abilities && unit.upgrade_bonuses[5] != bonuses[5]) unit.abilities->replan_due = true;
        unit.upgrade_bonuses = bonuses;
        return Void::success();
    }

ProductionCounts TacticalSession::Impl::production_counts(const PlayerId buyer, const TypeId type,
    const std::vector<PlayerEconomy>& accounts, const bool committed) const {
        ProductionCounts result;
        for (const auto& player : setup.players) {
            if (!allied(player.player_id, buyer)) continue;
            const auto slot = ownership_slot(player.player_id, type);
            auto current = slot != ownership_keys.size() ? (committed ? committed_ownership_counts[slot] : ownership_counts[slot]) : 0;
            auto has = current;
            std::uint64_t lifetime = 0;
            const auto account = std::find_if(accounts.begin(), accounts.end(),
                [&](const PlayerEconomy& ledger) { return ledger.player == player.player_id; });
            if (account != accounts.end()) {
                has += static_cast<std::uint64_t>(std::count_if(account->completed.begin(), account->completed.end(),
                    [type](const CompletedBuild& entry) { return entry.type == type; }));
                current = has + static_cast<std::uint64_t>(std::count(account->pool.begin(), account->pool.end(), type));
                for (const auto& queue : account->queues) {
                    const auto queued = static_cast<std::uint64_t>(std::count_if(queue.begin(), queue.end(),
                        [type](const QueueEntry& entry) { return entry.type == type; }));
                    current += queued;
                    result.queued_allies += queued;
                    if (player.player_id == buyer) result.queued_player += queued;
                }
                if (const auto built = account->lifetime.find(type); built != account->lifetime.end()) lifetime = built->second;
            }
            result.current_allies += current; result.lifetime_allies += lifetime;
            if (player.player_id == buyer) {
                result.owned_player = has; result.current_player = current; result.lifetime_player = lifetime;
            }
        }
        return result;
    }

std::vector<EconomyView> TacticalSession::Impl::economy_views(
    const std::vector<PlayerEconomy>& staged_ledgers, const std::map<EntityId, PopulationShare>& staged_shares) const {
        std::vector<EconomyView> views;
        views.reserve(staged_ledgers.size());
        for (const auto& ledger : staged_ledgers) {
            std::int64_t total = 0;
            for (const auto& [id, share] : staged_shares) {
                static_cast<void>(id);
                if (share.owner == ledger.player) total += share.share;
            }
            const auto* player = economy.player(ledger.player);
            views.push_back(EconomyView{ledger.player, ledger.credits, population_count(total),
                player != nullptr ? player->population_cap : 0U, ledger.queues, ledger.pool,
                ledger.pool_version, ledger.pool_additions, ledger.pool_addition_frame,
                ledger.completed, ledger.lifetime, ledger.tech_level, ledger.pool_tokens});
        }
        return views;
    }

bool TacticalSession::Impl::allied(const PlayerId left, const PlayerId right) const {
        return teams.at(left) == teams.at(right);
    }

const Player* TacticalSession::Impl::player_of(const PlayerId id) const noexcept {
        const auto found = std::find_if(setup.players.begin(), setup.players.end(),
            [id](const Player& player) { return player.player_id == id; });
        return found != setup.players.end() ? &*found : nullptr;
    }

core::Result<bool> TacticalSession::Impl::replenish_wingmen(const EntityId leader, const TypeId team,
    UnitStage& units, EconomyStage& stage, const std::uint64_t tick) {
    using Result = core::Result<bool>;
    const auto source = units.find(leader);
    const auto* profile = motion.squadrons.find_squadron(team);
    if (source == units.end() || profile == nullptr || profile->members.empty()) return Result::success(false);
    // UnitStage inserts into an ordered vector: births may invalidate the leader iterator.
    const auto source_state = source->second.state;
    const auto contains = [leader](const Squadron& group) {
        return std::find(group.members.begin(), group.members.end(), leader) != group.members.end();
    };
    auto replacement = std::find_if(stage.squadrons.begin(), stage.squadrons.end(), contains);
    const auto committed = std::find_if(squadrons.begin(), squadrons.end(), contains);
    const Squadron* parent = replacement != stage.squadrons.end() ? &*replacement
        : committed != squadrons.end() ? &*committed : nullptr;
    // A parentless craft's internal self-group is a locomotor adapter, not an authored team.
    if (parent && parent->container == leader && parent->members.size() == 1) parent = nullptr;
    Squadron next;
    if (parent) {
        next.container = parent->container;
        for (const auto id : parent->members) if (units.find(id) != units.end()) next.members.push_back(id);
        if (next.members.empty() || next.members.size() >= profile->members.size()) return Result::success(false);
    } else next.members = {leader};
    if (motion.squadrons.find_craft(source_state.type_id) && stage.crafts.find(leader) == stage.crafts.end()) {
        CraftState flight;
        const auto yaw = yaw_degrees(source_state.rotation);
        if (!yaw) return Result::failure(yaw.error());
        flight.yaw = yaw.value();
        stage.crafts[leader] = flight;
    }
    const auto transform = math::to_matrix(source_state.rotation, source_state.position);
    if (!transform) return Result::failure(transform.error());
    const auto append = [&](const std::size_t index) -> core::Result<void> {
        if (stage.next_id == invalid_entity_id) return core::Result<void>::failure(
            detail::diagnostic(diagnostic_codes::resource_limit, "wingman stable ID space exhausted"));
        const auto placed = math::transform_point(transform.value(), profile->offsets[index]);
        if (!placed) return core::Result<void>::failure(placed.error());
        const auto id = stage.next_id;
        const UnitState state{id, profile->members[index], source_state.owner,
            placed.value(), source_state.rotation, {}};
        units.emplace(id, new_unit(state, tick));
        adjust_owned(state, true);
        if (motion.squadrons.find_craft(state.type_id)) {
            CraftState flight;
            flight.yaw = yaw_degrees(state.rotation).value();
            stage.crafts[id] = flight;
        }
        if (const auto share = stage.shares.find(leader); share != stage.shares.end()) stage.shares[id] = share->second;
        next.members.push_back(id);
        stage.next_id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1;
        return core::Result<void>::success();
    };
    if (!parent) {
        if (const auto share = stage.shares.find(leader); share != stage.shares.end())
            share->second.share /= static_cast<std::int64_t>(profile->members.size());
        // WHE-63: retain the leader; initial construction starts at authored index one.
        for (std::size_t index = 1; index < profile->members.size(); ++index)
            if (auto added = append(index); !added) return Result::failure(added.error());
        if (stage.next_id == invalid_entity_id) return Result::failure(
            detail::diagnostic(diagnostic_codes::resource_limit, "wingman team stable ID space exhausted"));
        next.container = stage.next_id;
        const UnitState state{next.container, team, source_state.owner,
            source_state.position, math::identity_quat(), {}};
        units.emplace(next.container, new_unit(state, tick));
        adjust_owned(state, true);
        stage.next_id = next.container == std::numeric_limits<EntityId>::max() ? invalid_entity_id : next.container + 1;
        SquadronState order;
        order.container = next.container; order.squadron_type = team; order.anchor = state.position;
        stage.minds[next.container] = order;
    } else {
        // Existing members are retained; this is an authored type scan, not a missing-index scan.
        for (std::size_t index = 0; index < profile->members.size() && next.members.size() < profile->members.size(); ++index) {
            if (profile->members[index] == source_state.type_id) continue;
            if (auto added = append(index); !added) return Result::failure(added.error());
        }
    }
    if (auto mind = stage.minds.find(next.container); mind != stage.minds.end()) {
        auto& order = stage.minds.at(next.container);
        order.roster = next.members;
        if (order.mode != SquadronMode::idle) { order.approach = true; order.idle_cell.reset(); }
    }
    if (replacement != stage.squadrons.end()) *replacement = std::move(next);
    else stage.squadrons.push_back(std::move(next));
    return Result::success(true);
}

PlayerEconomy* TacticalSession::Impl::ledger_of(std::vector<PlayerEconomy>& staged_ledgers, const PlayerId id) const {
        const auto found = std::find_if(staged_ledgers.begin(), staged_ledgers.end(),
            [id](const PlayerEconomy& ledger) { return ledger.player == id; });
        return found != staged_ledgers.end() ? &*found : nullptr;
    }

const BuildOption* TacticalSession::Impl::build_option(const LiveUnit& station, const PlayerId buyer, const TypeId type) const {
        if (economy.disabled_types.contains(type)) return nullptr;
        if (!allied(station.state.owner, buyer)) return nullptr;
        const auto* owner = player_of(station.state.owner);
        if (owner == nullptr) return nullptr;
        const auto* menu = economy.menu(station.state.type_id, owner->faction_id);
        const auto* option = menu != nullptr ? menu->find(type) : nullptr;
        const auto* upgrade = economy.upgrade(type);
        if (upgrade != nullptr && upgrade->level_up && menu != nullptr
            && economy.disabled_types.contains(menu->next_level)) return nullptr;
        const auto* capture = economy.pads.point(station.state.type_id);
        // WBP-33: capture ownership does not make an ordinary producer a construction pad.
        return menu != nullptr && menu->station_producer && (capture == nullptr || !capture->build_pad)
            && economy.pads.child(station.state.type_id) == nullptr && economy.pads.child(type) == nullptr
            && economy.pads.point(type) == nullptr
            && option != nullptr && option->kind != BuildKind::structure && option->available ? option : nullptr;
    }

std::uint32_t TacticalSession::Impl::population_of(const TypeId type) const noexcept {
        for (const auto& menu : economy.menus) {
            if (const auto* option = menu.find(type)) return option->population;
        }
        return 0;
    }

core::Result<bool> TacticalSession::Impl::placement_valid(const PlayerId player, const TypeId type, const math::Vec3& point,
    const UnitStage& staged, const std::uint64_t tick,
    const CollisionWorld* collisions, TacticalSession::PlacementWork* work,
    const std::vector<UnitState>* prevention_units) const {
        using Valid = core::Result<bool>;
        const auto* issuer = player_of(player);
        if (issuer == nullptr) return Valid::success(false);
        const auto index = static_cast<std::size_t>(issuer - setup.players.data());
        if (fog && !fog->revealed(index, point)) {
            if (work != nullptr) ++work->rejections[0];
            return Valid::success(false);
        }
        const auto planar_within = [&](const math::Vec3& at, const std::int64_t reach) {
            const auto dx = math::detail::unsigned_magnitude(at.x.raw() - point.x.raw());
            const auto dy = math::detail::unsigned_magnitude(at.y.raw() - point.y.raw());
            auto across = math::detail::multiply_u64(dx, dx);
            static_cast<void>(math::detail::add_magnitude(across, math::detail::multiply_u64(dy, dy)));
            const auto limit = static_cast<std::uint64_t>(reach < 0 ? 0 : reach);
            return math::detail::compare(across, math::detail::multiply_u64(limit, limit)) < 0;
        };
        const auto prevents = [&](const UnitState& unit) {
            const auto* prevention = economy.prevention_of(unit.type_id);
            return !allied(unit.owner, player) && prevention != nullptr
                && planar_within(unit.position, prevention->radius.raw());
        };
        bool prevented = false;
        if (prevention_units != nullptr) {
            prevented = std::any_of(prevention_units->begin(), prevention_units->end(), prevents);
        } else {
            for (const auto& [id, unit] : staged) {
                static_cast<void>(id);
                if (prevents(unit.state)) { prevented = true; break; }
            }
        }
        if (prevented) {
            if (work != nullptr) ++work->rejections[1];
            return Valid::success(false);
        }
        // WR-21..24: fog, prevention, bounds, then the type/layer bypasses.
        if (economy.bounds) {
            const auto& box = *economy.bounds;
            if (point.x < box[0] || point.y < box[1] || point.x > box[2] || point.y > box[3]) {
                if (work != nullptr) ++work->rejections[2];
                return Valid::success(false);
            }
        }
        if (type == 0) return Valid::success(true);
        const auto* hero = economy.hero(type);
        const auto deployed = hero != nullptr ? hero->deployed : type;
        // WHE-SQ-02: a solo fighter's internal flight group is not a team placement box.
        const auto* squadron = motion.squadrons.find_craft(deployed) == nullptr
            ? motion.squadrons.find_squadron(deployed) : nullptr;
        const auto* own = motion.footprint(deployed);
        const auto layer = own != nullptr ? own->layer : SpaceLayer::none;
        if ((squadron == nullptr && layer == SpaceLayer::none) || !motion.avoidance) return Valid::success(true);
        const auto selected = dynamic_layer_index(layer == SpaceLayer::none ? SpaceLayer::corvette : layer);
        if (!selected) return Valid::success(true);
        math::Fixed x_extent = own != nullptr ? own->x_extent : math::Fixed{};
        math::Fixed y_extent = own != nullptr ? own->y_extent : math::Fixed{};
        if (squadron != nullptr && layer == SpaceLayer::none) {
            // WR-26: independent rectangular formation extents, in authored axes.
            for (std::size_t member = 0; member < squadron->members.size(); ++member) {
                const auto* craft = motion.footprint(squadron->members[member]);
                const auto& offset = squadron->offsets[member];
                x_extent = std::max(x_extent, math::Fixed::from_raw(std::abs(offset.x.raw()) + (craft ? craft->x_extent.raw() : 0)));
                y_extent = std::max(y_extent, math::Fixed::from_raw(std::abs(offset.y.raw()) + (craft ? craft->y_extent.raw() : 0)));
            }
        }
        // Preview queries build a private prediction view; authoritative commands reuse the
        // tick's staged tracking views. This work runs on demand, never in a per-entity tick phase.
        Tracking preview;
        CollisionWorld private_world;
        if (collisions == nullptr) {
            preview.frame = tick;
            preview.interval = motion.avoidance->tracking_interval;
            // WR-25: only windows intersecting the 115-frame query can affect its verdict.
            // Roll the tracking anchor to the query frame, retaining the layer's window boundaries.
            const auto anchor = rolled_start(tracking_anchor[*selected], tick, preview.interval);
            const auto frames = tick - anchor + arrival_sweep_frames;
            preview.windows = std::min(motion.avoidance->tracking_windows,
                static_cast<std::uint32_t>((frames + preview.interval - 1) / preview.interval));
            preview.current_start.fill(tick);
            preview.current_start[*selected] = anchor;
            for (const auto& [id, arrival] : arrivals) {
                static_cast<void>(arrival);
                preview.suspended.insert(id);
            }
            for (const auto& [id, unit] : staged) {
                const auto* footprint = tracked_footprint(motion, unit.state.type_id);
                if (!unit.motion || arrivals.contains(id) || footprint == nullptr
                    || dynamic_layer_index(footprint->layer) != selected) continue;
                auto yaw = yaw_degrees(unit.state.rotation);
                if (!yaw) return Valid::failure(yaw.error());
                auto samples = sample_windows(*unit.motion, anchor, preview.interval, preview.windows, unit.state.position, yaw.value());
                if (!samples) return Valid::failure(samples.error());
                if (work != nullptr) work->predictions += samples.value().size();
                preview.samples.emplace(id, TrackSamples{std::move(samples).value(), {}});
            }
            if (auto built = ensure_view(preview, motion, staged, {}, *selected); !built) return Valid::failure(built.error());
            if (auto built = ensure_statics(preview, motion, staged); !built) return Valid::failure(built.error());
            private_world.interval = preview.interval;
            private_world.layers[*selected] = &*preview.views[*selected];
            private_world.statics = &*preview.statics;
            collisions = &private_world;
        }
        if (collisions->layers[*selected] == nullptr) return Valid::success(true); // WR-24
        const auto* player_rules = economy.player(player);
        if (player_rules == nullptr) return Valid::success(false);
        auto direction = planar_direction(player_rules->reinforcement_yaw);
        if (!direction) return Valid::failure(direction.error());
        auto dx = math::multiply(direction.value().x, economy.collision_distance);
        auto dy = math::multiply(direction.value().y, economy.collision_distance);
        if (!dx) return Valid::failure(dx.error());
        if (!dy) return Valid::failure(dy.error());
        // WR-25: sweep back along facing, from this frame through frame + 115.
        LinearQuery query;
        query.start = {math::Fixed::from_raw(point.x.raw() - dx.value().raw()), math::Fixed::from_raw(point.y.raw() - dy.value().raw())};
        query.end = {point.x, point.y};
        query.start_frame = math::Fixed::from_raw(static_cast<std::int64_t>(tick) * math::Fixed::scale);
        query.end_frame = math::Fixed::from_raw(static_cast<std::int64_t>(tick + arrival_sweep_frames) * math::Fixed::scale);
        query.facing = {direction.value().x, direction.value().y};
        query.x_extent = x_extent;
        query.y_extent = y_extent;
        std::vector<EntityId> ignored;
        if (squadron != nullptr && layer == SpaceLayer::none) {
            // WR-27: resolved corvette objects and unresolved IDs do not reject a layer-less
            // squadron. Filter IDs before the query so an ignored first hit cannot mask a blocker.
            for (const auto& window : collisions->layers[*selected]->windows) {
                for (const auto& leaf : window) {
                    const auto found = staged.find(leaf.entity);
                    const auto* footprint = found != staged.end() ? motion.footprint(found->second.state.type_id) : nullptr;
                    if (footprint == nullptr || footprint->layer == SpaceLayer::corvette) ignored.push_back(leaf.entity);
                }
            }
            std::sort(ignored.begin(), ignored.end());
            ignored.erase(std::unique(ignored.begin(), ignored.end()), ignored.end());
            query.ignore_group = ignored;
        }
        if (work != nullptr) ++work->collision_queries;
        auto dynamic = find_linear_collision(*collisions->layers[*selected], collisions->interval, query);
        if (!dynamic) return Valid::failure(dynamic.error());
        if (dynamic.value() != 0) {
            if (work != nullptr) ++work->rejections[3];
            return Valid::success(false);
        }
        query.ignore_group = {}; // WR-28: every static collision rejects.
        if (collisions->statics != nullptr) {
            if (work != nullptr) ++work->collision_queries;
            auto statics = find_static_collision(*collisions->statics, query);
            if (!statics) return Valid::failure(statics.error());
            if (statics.value() != 0) {
                if (work != nullptr) ++work->rejections[4];
                return Valid::success(false);
            }
        }
        return Valid::success(true);
    }

core::Result<void> TacticalSession::Impl::block_placement(const UnitState& unit, std::vector<PlacementBox>& blockers) const {
        using Void = core::Result<void>;
        const auto* footprint = economy.footprint_of(unit.type_id);
        if (footprint == nullptr || !footprint->box) return Void::success();
        // The heading of the rotated +X axis in degrees: x' = 1 - 2(y^2 + z^2), y' = 2(xy + wz).
        const auto& q = unit.rotation;
        const auto yy = math::multiply(q.y, q.y);
        const auto zz = math::multiply(q.z, q.z);
        const auto xy = math::multiply(q.x, q.y);
        const auto wz = math::multiply(q.w, q.z);
        if (!yy || !zz || !xy || !wz) return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, "arrival blocker heading"));
        const auto one = math::Fixed::from_raw(math::Fixed::scale);
        const auto heading_x = math::Fixed::from_raw(one.raw() - 2 * (yy.value().raw() + zz.value().raw()));
        const auto heading_y = math::Fixed::from_raw(2 * (xy.value().raw() + wz.value().raw()));
        const auto turns = math::atan2_turn(heading_y, heading_x);
        if (!turns) return Void::failure(turns.error());
        const auto degrees = math::multiply(turns.value(), math::Fixed::from_raw(360 * math::Fixed::scale));
        if (!degrees) return Void::failure(degrees.error());
        auto bounds = blocker_bounds(*footprint->box, unit.position, degrees.value());
        if (!bounds) return Void::failure(bounds.error());
        blockers.push_back(bounds.value());
        return Void::success();
    }

core::Result<void> TacticalSession::Impl::execute_economy(const PlayerCommand& command, const std::uint64_t tick,
    UnitStage& staged, EconomyStage& stage, std::vector<Event>& events,
    const CollisionWorld* collisions, const bool executing_pad) {
        using Void = core::Result<void>;
        const auto issuer = command.key.player_id;
        Event event{
            .tick = tick,
            .kind = EventKind::order_accepted,
            .player = issuer,
            .sequence = command.key.sequence,
            .unit = command.units.empty() ? invalid_entity_id : command.units.front(),
            .order = order_kind(command.payload),
            .reason = RejectReason::none,
            .hardpoint = 0,
        };
        const auto finish = [&](const RejectReason reason) {
            event.reason = reason;
            if (reason != RejectReason::none) event.kind = EventKind::order_rejected;
            events.push_back(event);
            return Void::success();
        };
        auto* ledger = ledger_of(stage.ledgers, issuer);
        const auto* player = economy.player(issuer);
        if (ledger == nullptr || player == nullptr) return finish(RejectReason::no_economy);
        if (std::holds_alternative<PadSellPayload>(command.payload)) {
            const auto sold = staged.find(command.units.front());
            if (sold == staged.end() || (sold->second.durability && sold->second.durability->hull.raw() <= 0)) {
                return finish(RejectReason::unit_not_live);
            }
            const auto& unit = sold->second.state;
            if (unit.owner != issuer) return finish(RejectReason::unit_not_owned);
            const auto* sale = economy.pad_sale(unit.type_id);
            auto parent = std::find_if(stage.pads.begin(), stage.pads.end(),
                [&](const auto& entry) { return entry.second.constructed == unit.entity_id; });
            const auto* parent_state = parent != stage.pads.end() ? &parent->second : nullptr;
            if (!pad_sale_permission(issuer, unit.owner, sale != nullptr, parent_state)) {
                return finish(RejectReason::cannot_produce);
            }
            math::Fixed refund;
            EntityId parent_id{};
            if (parent != stage.pads.end()) {
                parent_id = parent->first;
                const auto live_parent = staged.find(parent_id);
                const auto* owner = player_of(unit.owner);
                const auto* menu = live_parent != staged.end() && owner != nullptr
                    ? economy.menu(live_parent->second.state.type_id, owner->faction_id) : nullptr;
                if (menu != nullptr) {
                    for (const auto& option : menu->options) {
                        const auto* child = economy.pads.child(option.type);
                        if (child == nullptr || child->constructed != unit.type_id) continue;
                        // WBP-31: this is the generic current-mode UC cost, independent
                        // of health and any adjusted pad debit. Stock skirmish uses MP cost.
                        const auto amount = math::multiply(option.price, sale->percentage);
                        if (!amount) return Void::failure(amount.error());
                        if (amount.value().raw() > 0) {
                            const auto whole = amount.value().raw() / math::Fixed::scale
                                + (amount.value().raw() % math::Fixed::scale >= math::Fixed::scale / 2 ? 1 : 0);
                            const auto rounded = math::Fixed::from_integer(whole);
                            if (!rounded) return Void::failure(rounded.error());
                            refund = rounded.value();
                        }
                        break;
                    }
                }
            }
            const auto balance = math::add(ledger->credits, refund);
            if (!balance) return Void::failure(balance.error());
            ledger->credits = balance.value(); // AI positive-credit adjustment remains its interface.
            // WBP-32: detach before removal, without the killed-child route or respawn.
            if (parent != stage.pads.end()) {
                stage.pad_stage.touch(parent_id);
                parent->second.constructed = invalid_entity_id;
            }
            const auto id = unit.entity_id;
            adjust_owned(unit, false);
            staged.erase(sold);
            stage.shares.erase(id);
            stage.arrivals.erase(id);
            std::erase_if(stage.earners, [id](const auto& source) { return source.first == id; });
            events.push_back(Event{tick, EventKind::pad_structure_sold, issuer, parent_id, id});
            return finish(RejectReason::none);
        }
        if (const auto* grant = std::get_if<CreditGrantPayload>(&command.payload)) {
            if (grant->amount.raw() <= 0) return finish(RejectReason::cannot_produce);
            const auto balance = math::add(ledger->credits, grant->amount);
            if (!balance) return Void::failure(balance.error());
            ledger->credits = balance.value();
            return finish(RejectReason::none);
        }
        if (const auto* build = std::get_if<PadBuildPayload>(&command.payload)) {
            const auto pad = staged.find(command.units.front());
            if (pad == staged.end() || (pad->second.durability && pad->second.durability->hull.raw() <= 0)) {
                return finish(RejectReason::unit_not_live);
            }
            const auto* profile = economy.pads.point(pad->second.state.type_id);
            const auto state = stage.pads.find(pad->first);
            const auto* builder = player_of(issuer);
            const auto* menu = builder != nullptr ? economy.menu(pad->second.state.type_id, builder->faction_id) : nullptr;
            const auto* option = menu != nullptr ? menu->find(build->type) : nullptr;
            const auto* child = economy.pads.child(build->type);
            if (profile == nullptr || !profile->build_pad || state == stage.pads.end()
                || state->second.under_construction != invalid_entity_id || state->second.constructed != invalid_entity_id
                || tick < state->second.cooldown_until || !allied(pad->second.state.owner, issuer)
                || option == nullptr || !option->available || child == nullptr
                || economy.disabled_types.contains(build->type)) return finish(RejectReason::cannot_produce);
            // WBP-09/10: request visibility is distinct from menu proximity; no proximity recheck here.
            if (!executing_pad && fog && !fog->revealed(static_cast<std::size_t>(builder - setup.players.data()), pad->second.state.position)) {
                return finish(RejectReason::cannot_produce);
            }
            if (!player->ai && ledger->credits < child->price) return finish(RejectReason::insufficient_credits);
            if (!player->ai && !executing_pad) {
                // WBP-09: schedule the human event; WBP-10 rechecks after command delivery.
                stage.pad_requests.push_back(command);
                return finish(RejectReason::none);
            }
            // WBP-10: debit precedes creation and has no cancellation/refund transaction.
            if (!player->ai) ledger->credits = math::Fixed::from_raw(ledger->credits.raw() - child->price.raw());
            const auto* health = durability.find(child->type);
            const auto seconds = player->ai ? child->ai_seconds : child->seconds;
            if (health == nullptr || seconds == 0 || stage.next_id == invalid_entity_id) return finish(RejectReason::cannot_produce);
            const auto transform = math::to_matrix(pad->second.state.rotation, pad->second.state.position);
            if (!transform) return Void::failure(transform.error());
            const auto position = math::transform_point(transform.value(), profile->attachment);
            if (!position) return Void::failure(position.error());
            const auto id = stage.next_id;
            const auto pad_id = pad->first; // UnitStage insertion can move the pad.
            auto created = new_unit(UnitState{id, child->type, issuer, position.value(), pad->second.state.rotation, {}}, tick);
            created.durability->hull = initial_construction_hull;
            staged.emplace(id, std::move(created));
            stage.construction_stage.touch(id);
            stage.construction.emplace(id, begin_construction(pad_id, issuer, tick, seconds, health->max_hull));
            stage.pad_stage.touch(pad_id);
            state->second.under_construction = id;
            state->second.cooldown_until = 0;
            state->second.cooldown_start = 0;
            stage.next_id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1;
            if (!executing_pad) events.push_back(event);
            events.push_back(Event{tick, EventKind::pad_construction_started, issuer, 0, id});
            return Void::success();
        }
        if (const auto* buy = std::get_if<BuyPayload>(&command.payload)) {
            const auto station = staged.find(command.units.front());
            if (station == staged.end()) return finish(RejectReason::unit_not_live);
            const auto* option = build_option(station->second, issuer, buy->type);
            if (option == nullptr) return finish(RejectReason::cannot_produce);
            if (!production_allowed(*option, true,
                [&](const TypeId type) { return production_counts(issuer, type, stage.ledgers); })) {
                return finish(RejectReason::cannot_produce);
            }
            const auto result = queue_build(*ledger, *player, economy, *option, station->first, tick);
            if (result == RejectReason::none) stage.cues.push_back({issuer, buy->type, tick, BattleEconomyCue::Kind::started});
            return finish(result);
        }
        if (const auto* cancel = std::get_if<CancelPayload>(&command.payload)) {
            const auto& queue = ledger->queues[cancel->queue];
            const auto type = cancel->index < queue.size() ? queue[cancel->index].type : TypeId{};
            const bool removed = cancel_build(*ledger, static_cast<BuildQueue>(cancel->queue), cancel->index, tick);
            if (removed) stage.cues.push_back({issuer, type, tick, BattleEconomyCue::Kind::cancelled});
            return finish(removed ? RejectReason::none : RejectReason::no_queue_entry);
        }
        const auto& reinforce = std::get<ReinforcePayload>(command.payload);
        if (economy.disabled_types.contains(reinforce.type)) {
            return finish(RejectReason::cannot_produce);
        }
        if (outcome) return finish(RejectReason::battle_decided); // WR-19, before creation/pool consumption
        auto pooled = ledger->pool.end();
        if (reinforce.pool_token == 0) {
            pooled = std::find(ledger->pool.begin(), ledger->pool.end(), reinforce.type);
        } else {
            const auto token = std::find(ledger->pool_tokens.begin(), ledger->pool_tokens.end(), reinforce.pool_token);
            if (token != ledger->pool_tokens.end()) {
                const auto index = static_cast<std::size_t>(token - ledger->pool_tokens.begin());
                if (index < ledger->pool.size() && ledger->pool[index] == reinforce.type)
                    pooled = ledger->pool.begin() + static_cast<std::ptrdiff_t>(index);
            }
        }
        if (pooled == ledger->pool.end()) return finish(RejectReason::not_in_pool);
        std::int64_t owned = 0;
        for (const auto& [id, share] : stage.shares) {
            static_cast<void>(id);
            if (share.owner == issuer) owned += share.share;
        }
        const auto population = population_of(reinforce.type);
        const auto count = population_count(owned);
        if (count > player->population_cap || population > player->population_cap - count) {
            stage.cues.push_back({issuer, reinforce.type, tick, BattleEconomyCue::Kind::unit_cap});
            return finish(RejectReason::no_population_room);
        }
        // WR-25: tracking, including a layer rebuilt by an earlier command, starts at the staged frame.
        const auto placement_begin = std::chrono::steady_clock::now();
        auto valid = placement_valid(issuer, reinforce.type, reinforce.position, staged, tick + 1, collisions,
            stage.placement_work);
        if (stage.placement_work != nullptr) {
            stage.placement_work->nanoseconds += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - placement_begin).count());
        }
        if (!valid) return Void::failure(valid.error());
        if (!valid.value()) return finish(RejectReason::invalid_position);
        const auto context = command_context(command.key) + ": ";
        const auto direction = planar_direction(player->reinforcement_yaw);
        const auto rotation = yaw_rotation(player->reinforcement_yaw);
        if (!direction || !rotation) {
            return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, context + "reinforcement facing"));
        }
        // PU-34, PU-35: a new unit at the start of its lane, arriving at `exit`.
        const auto add = [&](const TypeId type, const math::Vec3& exit, const math::Quat& facing,
                             const std::int64_t share) -> core::Result<EntityId> {
            if (stage.next_id == invalid_entity_id) {
                return core::Result<EntityId>::failure(
                    detail::diagnostic(diagnostic_codes::resource_limit, context + "stable ID space exhausted"));
            }
            const auto id = stage.next_id;
            const ArrivalState arrival{0, exit, direction.value()};
            auto start = arrival_position(arrival);
            if (!start) return core::Result<EntityId>::failure(start.error());
            staged.emplace(id, new_unit(UnitState{id, type, issuer, start.value(), facing, {}}, tick));
            adjust_owned(staged.at(id).state, true);
            // WR-32/41: a squadron's craft receive the modifier; its container does not.
            if ((motion.squadrons.find_squadron(type) == nullptr || motion.squadrons.find_craft(type) != nullptr)
                && economy.vulnerability_frames != 0 && economy.vulnerability.raw() != 0) {
                staged.at(id).arrival_vulnerable_until = tick + 1 + economy.vulnerability_frames;
            }
            stage.arrivals.emplace(id, arrival);
            // FL-12: only an authored hangar on the deployed type creates a spawner.
            if (motion.squadrons.find_spawner(type) != nullptr) stage.born_spawners.push_back(id);
            if (share > 0) stage.shares.emplace(id, PopulationShare{issuer, share});
            if (economy.stream(type) != nullptr) stage.earners.emplace_back(id, issuer); // PU-02
            stage.next_id = id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : id + 1;
            return core::Result<EntityId>::success(id);
        };
        // PL-08: the arrival point is on the plane (height 0). A created unit is then raised by its
        // type's Layer_Z_Adjust (LZ-01): a single ship here, each squadron craft below.
        const math::Vec3 point{reinforce.position.x, reinforce.position.y, {}};
        const auto* hero = economy.hero(reinforce.type);
        const auto deployed = hero != nullptr ? hero->deployed : reinforce.type;
        const auto* squadron = motion.squadrons.find_squadron(deployed);
        const auto* solo = motion.squadrons.find_craft(deployed);
        if (squadron == nullptr || solo != nullptr) {
            // PL-08: a single ship is created on the point without a search, so it may overlap.
            const auto* footprint = economy.footprint_of(deployed);
            const math::Vec3 raised{point.x, point.y, footprint != nullptr ? footprint->layer_z : math::Fixed{}};
            auto added = add(deployed, raised, rotation.value(), population_share(population, 1));
            if (!added) return Void::failure(added.error());
            event.unit = added.value();
            if (solo != nullptr && squadron != nullptr) {
                auto flight = launch_state(direction.value(), math::Fixed{});
                if (!flight) return Void::failure(flight.error());
                stage.crafts[added.value()] = flight.value();
                SquadronState order;
                order.container = added.value();
                order.squadron_type = deployed;
                order.roster = {added.value()};
                order.anchor = raised;
                order.next_scan_frame = tick + arrival_frames;
                stage.minds.emplace(added.value(), std::move(order));
                stage.squadrons.push_back(Squadron{added.value(), {added.value()}});
            }
        } else {
            // PL-08: each craft is searched from the point (start angle 0) on the plane and, when
            // nothing within the distance is free, put on the point itself; it is then raised by its
            // own height (LZ-01). Every live unit with a placement box blocks, and so does each craft
            // already placed. The team container follows its craft and holds the point afterwards.
            const auto& facing = direction.value();
            std::vector<PlacementBox> blockers;
            for (const auto& [other_id, other] : staged) {
                static_cast<void>(other_id);
                auto blocked = block_placement(other.state, blockers);
                if (!blocked) return blocked;
            }
            std::vector<EntityId> members;
            math::Fixed layer_z{};
            const auto craft_count = static_cast<std::uint32_t>(squadron->members.size());
            for (std::size_t member = 0; member < squadron->members.size(); ++member) {
                const auto* craft = motion.squadrons.find_craft(squadron->members[member]);
                const auto height = craft != nullptr ? craft->layer_z : math::Fixed{};
                if (member == 0) layer_z = height;
                const auto* footprint = economy.footprint_of(squadron->members[member]);
                math::Vec3 at = point;
                if (footprint != nullptr && footprint->box) {
                    const FreeSpaceSearch search{point, *footprint->box, math::Fixed{}};
                    auto found = find_free_space(search, blockers);
                    if (!found) return Void::failure(found.error());
                    if (found.value()) at = *found.value();
                    auto bounds = blocker_bounds(*footprint->box, at, player->reinforcement_yaw);
                    if (!bounds) return Void::failure(bounds.error());
                    blockers.push_back(bounds.value());
                }
                const auto x = at.x;
                const auto y = at.y;
                auto flight = launch_state(facing, math::Fixed{});
                if (!flight) return Void::failure(flight.error());
                auto turned = craft_rotation(flight.value());
                if (!turned) return Void::failure(turned.error());
                auto added = add(squadron->members[member], math::Vec3{x, y, height}, turned.value(),
                    population_share(population, craft_count));
                if (!added) return Void::failure(added.error());
                stage.crafts[added.value()] = flight.value();
                members.push_back(added.value());
            }
            auto container = add(deployed, math::Vec3{point.x, point.y, layer_z}, math::identity_quat(), 0);
            if (!container) return Void::failure(container.error());
            SquadronState order;
            order.container = container.value();
            order.squadron_type = deployed;
            order.roster = members;
            order.anchor = math::Vec3{point.x, point.y, layer_z};
            order.formation = SquadronFormationState{order.anchor, invalid_entity_id, true, true, false}; // WMV-18
            order.next_scan_frame = tick + arrival_frames;
            stage.minds.emplace(container.value(), std::move(order));
            stage.squadrons.push_back(Squadron{container.value(), std::move(members)});
            event.unit = container.value();
        }
        if (hero != nullptr) {
            auto& carrier = staged.at(event.unit).state;
            adjust_owned(carrier, false);
            carrier.purchase_type = reinforce.type;
            adjust_owned(carrier, true);
            // WHE-49: created riders never enter independent spatial or combat phases.
            for (const auto& profile : hero->riders) {
                if (stage.next_id == invalid_entity_id)
                    return Void::failure(detail::diagnostic(diagnostic_codes::resource_limit, context + "stable rider ID space exhausted"));
                CarriedObject rider;
                rider.entity_id = stage.next_id;
                rider.type_id = profile.type;
                rider.named_hero = profile.named;
                rider.generic_hero = profile.generic;
                if (!contain_object(carrier, rider, false))
                    return Void::failure(detail::diagnostic(diagnostic_codes::worker_failure, context + "invalid rider parent"));
                stage.next_id = stage.next_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : stage.next_id + 1;
            }
        }
        staged.at(event.unit).state.purchase_token = reinforce.pool_token;
        const auto pool_index = static_cast<std::size_t>(pooled - ledger->pool.begin());
        if (pool_index < ledger->pool_tokens.size())
            ledger->pool_tokens.erase(ledger->pool_tokens.begin() + static_cast<std::ptrdiff_t>(pool_index));
        ledger->pool.erase(pooled);
        ++ledger->pool_version;
        return finish(RejectReason::none);
    }

// WBP-25/44..46: admitted modifier objects discover and modify live income streams.
void TacticalSession::Impl::retire_income_modifier(PlayerEconomy& account, const CompletedBuild& held) const {
    const auto* profile = economy.upgrade(held.type);
    if (profile == nullptr || std::none_of(profile->income_modifiers.begin(), profile->income_modifiers.end(),
        [](const IncomeModifier& modifier) { return modifier.reverse; })) return;
    auto residual = held;
    residual.income_modifiers.clear();
    residual.income_modifiers.resize(profile->income_modifiers.size());
    account.income_residuals.push_back(std::move(residual));
}

core::Result<void> TacticalSession::Impl::service_income_modifiers(std::vector<PlayerEconomy>& accounts,
    const std::span<const LiveUnit> live, const std::span<const std::pair<EntityId, PlayerId>> streams,
    const std::uint64_t frame, const PartitionExecutor& executor, const bool initialization_only) {
    using Void = core::Result<void>;
    auto& references = income_service_refs;
    references.clear();
    const auto append = [&](PlayerEconomy& account, CompletedBuild& held, const bool residual) {
        const auto* profile = economy.upgrade(held.type);
        if (profile == nullptr || profile->income_modifiers.empty()) return;
        held.income_modifiers.resize(profile->income_modifiers.size());
        for (std::size_t slot = 0; slot < profile->income_modifiers.size(); ++slot) {
            const auto& modifier = profile->income_modifiers[slot];
            auto& state = held.income_modifiers[slot];
            if (residual && !modifier.reverse) continue;
            const bool due = !state.initialized || (!residual && !modifier.reverse && frame >= state.next_scan_frame);
            // Capacity planning is outside the worker phase; a repeated warmed scan adds no allocation.
            if (due) state.attached.reserve(streams.size());
            references.push_back({account.player, held.object, slot, &modifier, &state, residual});
        }
    };
    // This is the small admitted upgrade-object list, never a world-entity traversal.
    for (auto& account : accounts) {
        for (auto& held : account.completed) append(account, held, false);
        for (auto& held : account.income_residuals) append(account, held, true);
    }
    if (references.empty()) return Void::success();
    std::array<std::optional<core::Diagnostic>, tick_partition_count> errors{};
    const auto source = [&](const EntityId id) -> const LiveUnit* {
        const auto found = std::lower_bound(live.begin(), live.end(), id,
            [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
        return found != live.end() && found->state.entity_id == id ? &*found : nullptr;
    };
    const auto serviced = executor.execute_phase("income-modifiers", tick_partition_count,
        [&](const std::size_t partition) {
            const auto range = partition_range(partition, references.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& reference = references[index];
                auto& state = *reference.state;
                const auto& modifier = *reference.profile;
                if (initialization_only && state.initialized) continue;
                std::erase_if(state.attached, [&](const EntityId id) {
                    const auto* current = source(id);
                    return current == nullptr || economy.stream(current->state.type_id) == nullptr;
                });
                const bool initial = !state.initialized;
                const bool due = initial || (!reference.residual && !modifier.reverse && frame >= state.next_scan_frame);
                if (!due) continue;
                const bool apply = reference.residual ? modifier.reverse : !modifier.reverse;
                if (apply) {
                    for (const auto& [id, owner] : streams) {
                        static_cast<void>(owner); // eligibility reads live ownership, not cached placement identity
                        const auto* current = source(id);
                        if (current == nullptr || current->state.type_id != modifier.target_source) continue;
                        const bool eligible = modifier.all_allies ? allied(reference.owner, current->state.owner)
                                                                  : reference.owner == current->state.owner;
                        if (!eligible) continue;
                        const auto found = std::lower_bound(state.attached.begin(), state.attached.end(), id);
                        if (found == state.attached.end() || *found != id) state.attached.insert(found, id);
                    }
                } else {
                    // WBP-45: reverse activation removes this source's contributions and has no timer.
                    state.attached.clear();
                }
                state.initialized = true;
                if (reference.residual || modifier.reverse) {
                    state.next_scan_frame = 0;
                } else {
                    if (frame > std::numeric_limits<std::uint64_t>::max() - 150) {
                        errors[partition] = detail::diagnostic(diagnostic_codes::resource_limit,
                            "income modifier deadline exceeds the frame range");
                        break;
                    }
                    // WBP-45: randomized first phase, then 150 frames from actual service.
                    std::uint32_t offset = 150;
                    if (initial) {
                        CombatRandom random(setup.seed, frame, reference.object, 0xfffe0005U);
                        for (std::size_t draw = 0; draw <= reference.slot; ++draw) offset = random.uniform(0, 150);
                    }
                    state.next_scan_frame = frame + offset;
                }
            }
        });
    if (!serviced) return serviced;
    for (const auto& error : errors) if (error) return Void::failure(*error);
    return Void::success();
}

void TacticalSession::Impl::reduce_income(const std::vector<PlayerEconomy>& accounts, const EntityId stream,
    const std::span<IncomeCategory> categories) const {
    for (auto& category : categories) category.winners = {};
    const auto reduce = [&](const CompletedBuild& held, const bool residual) {
        const auto* profile = economy.upgrade(held.type);
        if (profile == nullptr) return;
        for (std::size_t slot = 0; slot < profile->income_modifiers.size() && slot < held.income_modifiers.size(); ++slot) {
            const auto& modifier = profile->income_modifiers[slot];
            if (modifier.reverse != residual) continue;
            const auto& state = held.income_modifiers[slot];
            if (std::binary_search(state.attached.begin(), state.attached.end(), stream)) {
                reduce_income_modifier(modifier, categories);
            }
        }
    };
    for (const auto& account : accounts) {
        for (const auto& held : account.completed) reduce(held, false);
        for (const auto& held : account.income_residuals) reduce(held, true);
    }
}

} // namespace eawr::sim::tactical
