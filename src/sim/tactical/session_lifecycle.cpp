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


namespace eawr::sim::tactical {

namespace session_detail {

[[nodiscard]] std::string command_context(const CommandKey& key) {
    return "command (tick " + std::to_string(key.tick) + ", player " + std::to_string(key.player_id)
        + ", sequence " + std::to_string(key.sequence) + ")";
}

} // namespace session_detail

TacticalSession::Impl::Impl(const TacticalSetup& source, const std::span<const SensorProfile> sensor_table, const DurabilityTable& table,
    const MotionTable& motion_table, const std::optional<FogRules>& fog_rules, const CombatTable& combat_table,
    const VictoryRules& victory_rules, const AbilityTable& ability_table, const EconomyRules& economy_rules)
    : setup(source), sensors(sensor_table.begin(), sensor_table.end()), durability(table), motion(motion_table),
      combat(combat_table), victory(victory_rules), abilities(ability_table), economy(economy_rules),
      squadrons(source.squadrons), rng_state(source.seed) {
        // WPR-55: content-indexed counters and bonus profiles; storage is reused by every phase.
        std::vector<TypeId> types;
        // SAE-02: perceptions also count starting structures and completed pad children.
        for (const auto& unit : source.units) types.push_back(unit.type_id);
        for (const auto& child : economy.pads.construction) {
            types.push_back(child.type);
            types.push_back(child.constructed);
        }
        for (const auto& menu : economy.menus) {
            types.push_back(menu.station);
            for (const auto& option : menu.options) {
                types.push_back(option.type);
                types.insert(types.end(), option.requirements.prerequisites.begin(), option.requirements.prerequisites.end());
            }
        }
        for (const auto& upgrade : economy.upgrades) for (const auto& bonus : upgrade.bonuses) {
            types.insert(types.end(), bonus.applicable.begin(), bonus.applicable.end());
            bonus_categories.push_back(bonus.stacking_category);
        }
        for (const auto& source_profile : economy.command_bonuses) {
            types.push_back(source_profile.type);
            types.insert(types.end(), source_profile.bonus.applicable.begin(), source_profile.bonus.applicable.end());
            bonus_categories.push_back(source_profile.bonus.stacking_category);
        }
        for (const auto& profile : abilities.profiles) for (const auto& special : profile.special) {
            if (special.kind == SpecialAbilityKind::concentrate_fire || special.kind == SpecialAbilityKind::tractor_beam)
                bonus_categories.push_back(special.concentrate_stacking_category);
        }
        std::sort(types.begin(), types.end());
        for (const auto& profile : abilities.profiles) for (const auto& ability : profile.abilities)
            if (ability.spawned && ability.spawned->weaken.on_detonation) bonus_categories.push_back(0);
        types.erase(std::unique(types.begin(), types.end()), types.end());
        std::sort(bonus_categories.begin(), bonus_categories.end());
        bonus_categories.erase(std::unique(bonus_categories.begin(), bonus_categories.end()), bonus_categories.end());
        for (const auto& player : source.players) for (const auto type : types) ownership_keys.emplace_back(player.player_id, type);
        std::sort(ownership_keys.begin(), ownership_keys.end());
        ownership_counts.resize(ownership_keys.size());
        committed_ownership_counts.resize(ownership_keys.size());
        ownership_parts.resize(tick_partition_count * ownership_keys.size());
        bonus_profiles.resize(ownership_keys.size());
        committed_bonus_profiles.resize(ownership_keys.size());
        bonus_parts.resize(tick_partition_count * bonus_categories.size());
        income_category_parts = initial_income_categories(economy, tick_partition_count);
        for (const auto& player : economy.players) {
            PlayerEconomy ledger;
            ledger.player = player.player;
            ledger.credits = player.credits;
            ledger.tech_level = player.start_tech;
            ledgers.push_back(std::move(ledger));
        }
        for (const auto& player : source.players) {
            teams.emplace(player.player_id, player.team_id);
            const bool neutral = (!economy.pads.capture.empty() && player.player_id == economy.pads.neutral)
                || std::binary_search(combat.pad_neutral_factions.begin(), combat.pad_neutral_factions.end(), player.faction_id);
            snapshot_players.push_back(SnapshotPlayer{player.player_id, player.team_id, neutral});
        }
        for (const auto& unit : source.units) {
            add_starbase(unit);
            if (economy.pads.point(unit.type_id) != nullptr) pads.emplace(unit.entity_id, PadState{unit.owner});
            // #530 PU-02: the stations whose stream pays, ascending ID.
            if (!economy.empty() && economy.stream(unit.type_id) != nullptr) earners.emplace_back(unit.entity_id, unit.owner);
        }
        if (source.units.empty()) {
            next_id = 1;
        } else if (source.units.back().entity_id == std::numeric_limits<EntityId>::max()) {
            next_id = invalid_entity_id;
        } else {
            next_id = source.units.back().entity_id + 1;
        }
        std::vector<LiveUnit> live;
        live.reserve(source.units.size());
        for (const auto& unit : source.units) {
            live.push_back(new_unit(unit, 0));
        }
        rebuild(live, false);
        // Partial-table sessions retain setup bindings; only bound hangars can consume them.
        const bool creation_tables = !motion.squadrons.spawners.empty();
        for (const auto& binding : source.free_garrisons) {
            for (const auto type : binding.templates) {
                if (creation_tables && motion.squadrons.find_squadron(type) == nullptr && motion.find(type) == nullptr
                    && durability.find(type) == nullptr) {
                    initialization_error = detail::diagnostic(diagnostic_codes::invalid_setup,
                        "free garrison template has no bound creation profile");
                }
            }
            free_garrisons.push_back({binding, binding.registered, std::nullopt, {}});
        }
        bind_squadrons(live);
        if (!economy.command_bonuses.empty()) {
            auto ledger = update_command_ledger(live, 0, InlineExecutor{});
            if (!ledger) initialization_error = ledger.error();
            else {
                command_ledger = std::move(ledger).value();
                const auto applied = apply_command_bonuses(live, command_ledger, ledgers, 0, InlineExecutor{});
                if (!applied) initialization_error = applied.error();
            }
            rebuild(live, false);
        }
        // validate_setup and validate_sensors have already proved that the sensor field
        // builds and that every rotation converts.
        if (fog_rules) {
            core::load_profile::Scope fog_scope(core::load_profile::Phase::fog);
            // Tick zero marks every revealer's circle; there is no service before the first
            // step. The inline executor runs the same partitions a step does.
            const auto field = SensorField::build(source.players, source.units, sensors).value();
            fog.emplace(*fog_rules, source.players);
            static_cast<void>(fog->advance(0, false, revealers(field, source.units), InlineExecutor{}));
        }
        publish_staged(live, {}, {});
    }

void TacticalSession::Impl::add_starbase(const UnitState& unit) {
        if (victory.condition == VictoryCondition::none
            || !std::binary_search(victory.starbase_types.begin(), victory.starbase_types.end(), unit.type_id)
            || !std::binary_search(victory.contenders.begin(), victory.contenders.end(), unit.owner)) {
            return;
        }
        const StarbaseEntry entry{unit.entity_id, unit.owner};
        starbases.insert(std::upper_bound(starbases.begin(), starbases.end(), entry,
                             [](const StarbaseEntry& left, const StarbaseEntry& right) { return left.unit < right.unit; }),
            entry);
    }

std::optional<DurabilityState> TacticalSession::Impl::entering_durability(
    const DurabilityProfile* profile, const EntityId unit, const std::uint64_t frame) const {
        if (profile == nullptr) return std::nullopt;
        auto state = full_durability(*profile);
        if (durability.damage) {
            const auto& rules = *durability.damage;
            CombatRandom shield(setup.seed, frame, unit, shield_phase_slot);
            state.next_shield_frame = frame + shield.uniform(0, rules.shield_recharge_frames - 1U);
            if (has_energy_pool(*profile, rules)) {
                CombatRandom energy(setup.seed, frame, unit, energy_phase_slot);
                state.next_energy_frame = frame + energy.uniform(0, rules.energy_recharge_frames - 1U);
            }
        }
        return state;
    }

LiveUnit TacticalSession::Impl::new_unit(const UnitState& unit, const std::uint64_t frame) const {
        const auto* profile = durability.find(unit.type_id);
        const auto* fighting = combat.find(unit.type_id);
        return LiveUnit{unit, entering_durability(profile, unit.entity_id, frame),
            motion.find(unit.type_id) != nullptr ? std::optional(MotionState{}) : std::nullopt, math::Fixed{},
            math::Fixed{},
            fighting != nullptr ? std::optional(initial_combat(*fighting, setup.seed, frame, unit.entity_id))
                                : std::nullopt,
            std::nullopt, entering_abilities(unit.type_id, unit.owner)};
    }

const DurabilityProfile* TacticalSession::Impl::health_profile(const LiveUnit& unit) const noexcept {
        return unit.upgraded_durability ? &*unit.upgraded_durability : durability.find(unit.state.type_id);
    }

TacticalSession::TacticalSession(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
TacticalSession::TacticalSession(TacticalSession&&) noexcept = default;
TacticalSession& TacticalSession::operator=(TacticalSession&&) noexcept = default;
TacticalSession::~TacticalSession() = default;

core::Result<TacticalSession> TacticalSession::create(const TacticalSetup& setup,
    const std::span<const SensorProfile> sensors, const DurabilityTable& durability, const MotionTable& motion,
    const std::optional<FogRules>& fog, const CombatTable& combat, const VictoryRules& victory,
    const AbilityTable& abilities, const EconomyRules& economy) {
    auto valid = validate_setup(setup);
    if (valid) {
        valid = validate_sensors(sensors);
    }
    if (valid) {
        valid = validate_durability(durability);
    }
    if (valid && !motion.profiles.empty()) {
        valid = validate_motion(motion);
    } else if (valid) {
        valid = validate_squadron_table(motion.squadrons);
    }
    if (valid && fog) {
        valid = validate_fog_rules(*fog);
    }
    if (valid) {
        valid = validate_combat(combat);
    }
    if (valid) {
        valid = validate_victory(victory, setup.players);
    }
    if (valid) {
        valid = validate_abilities(abilities);
    }
    if (valid && (!economy.empty() || !economy.command_bonuses.empty())) {
        valid = validate_economy(economy, setup.players);
    }
    if (!valid) {
        return core::Result<TacticalSession>::failure(valid.error());
    }
    auto impl = std::make_unique<Impl>(setup, sensors, durability, motion, fog, combat, victory, abilities, economy);
    if (impl->initialization_error) return core::Result<TacticalSession>::failure(*impl->initialization_error);
    return core::Result<TacticalSession>::success(TacticalSession(std::move(impl)));
}

core::Result<TacticalSession> TacticalSession::from_replay(const TacticalReplay& replay,
    const std::span<const SensorProfile> sensors, const DurabilityTable& durability, const MotionTable& motion,
    const std::optional<FogRules>& fog, const CombatTable& combat, const VictoryRules& victory,
    const AbilityTable& abilities, const EconomyRules& economy) {
    auto valid = validate_replay(replay);
    if (valid && replay.setup.match_policy.value_or(SkirmishMatchPolicy{}) != economy.match_policy) {
        return core::Result<TacticalSession>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "replay match policy does not match the bound skirmish economy policy"));
    }
    if (valid) {
        valid = validate_sensors(sensors);
    }
    if (valid) {
        valid = validate_durability(durability);
    }
    if (valid && !motion.profiles.empty()) {
        valid = validate_motion(motion);
    } else if (valid) {
        valid = validate_squadron_table(motion.squadrons);
    }
    if (valid && fog) {
        valid = validate_fog_rules(*fog);
    }
    if (valid) {
        valid = validate_combat(combat);
    }
    if (valid) {
        valid = validate_victory(victory, replay.setup.players);
    }
    if (valid) {
        valid = validate_abilities(abilities);
    }
    if (valid && (!economy.empty() || !economy.command_bonuses.empty())) {
        valid = validate_economy(economy, replay.setup.players);
    }
    if (!valid) {
        return core::Result<TacticalSession>::failure(valid.error());
    }
    auto session = TacticalSession(
        std::make_unique<Impl>(replay.setup, sensors, durability, motion, fog, combat, victory, abilities,
            economy));
    if (session.impl_->initialization_error) return core::Result<TacticalSession>::failure(*session.impl_->initialization_error);
    for (const auto& command : replay.commands) {
        const auto submitted = session.submit(command);
        if (!submitted) {
            return core::Result<TacticalSession>::failure(submitted.error());
        }
    }
    return core::Result<TacticalSession>::success(std::move(session));
}



core::Result<EntityId> TacticalSession::stage_spawn(const UnitState& unit) {
    if (!impl_->teams.contains(unit.owner)) {
        return core::Result<EntityId>::failure(detail::diagnostic(diagnostic_codes::invalid_setup,
            "staged unit owner " + std::to_string(unit.owner) + " is not a declared player"));
    }
    if (impl_->next_id == invalid_entity_id) {
        return core::Result<EntityId>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            "stable ID space exhausted"));
    }
    const auto transform = math::to_matrix(unit.rotation, unit.position);
    if (!transform) {
        return core::Result<EntityId>::failure(transform.error());
    }
    auto state = unit;
    state.entity_id = impl_->next_id;
    state.order = Order{};
    auto live = impl_->sorted_live();
    live.push_back(impl_->new_unit(state, impl_->completed_tick));
    std::optional<Impl::CommandLedger> command_ledger;
    if (!impl_->economy.command_bonuses.empty()) {
        auto changed = impl_->update_command_ledger(live, state.entity_id, InlineExecutor{});
        if (!changed) return core::Result<EntityId>::failure(changed.error());
        command_ledger.emplace(std::move(changed).value());
        const auto applied = impl_->apply_command_bonuses(live, *command_ledger, impl_->ledgers, 0, InlineExecutor{});
        if (!applied) return core::Result<EntityId>::failure(applied.error());
    }
    if (!impl_->economy.upgrades.empty() && !command_ledger) {
        const auto applied = impl_->apply_bonuses(live.back(), impl_->bonuses_for(live.back(), true));
        if (!applied) return core::Result<EntityId>::failure(applied.error());
    }
    impl_->rebuild(live, false);
    if (command_ledger) impl_->command_ledger = std::move(*command_ledger);
    if (impl_->motion.squadrons.find_spawner(state.type_id) != nullptr) {
        impl_->spawners.emplace(state.entity_id,
            initial_spawner(impl_->setup.seed, impl_->completed_tick + 1, state.entity_id));
    }
    impl_->add_starbase(state);
    if (impl_->economy.stream(state.type_id) != nullptr) impl_->earners.emplace_back(state.entity_id, state.owner);
    if (impl_->economy.pads.point(state.type_id) != nullptr) {
        impl_->pads.emplace(state.entity_id, PadState{state.owner});
        impl_->pad_stage.initialized = false;
    }
    impl_->next_id = state.entity_id == std::numeric_limits<EntityId>::max() ? invalid_entity_id : state.entity_id + 1;
    const auto& published = *impl_->current_snapshot;
    impl_->publish_staged(live, {published.events().begin(), published.events().end()},
        {published.combat_events().begin(), published.combat_events().end()});
    return core::Result<EntityId>::success(state.entity_id);
}

core::Result<void> TacticalSession::stage_remove(const EntityId unit) {
    auto live = impl_->sorted_live();
    const auto found = std::find_if(
        live.begin(), live.end(), [&](const LiveUnit& entry) { return entry.state.entity_id == unit; });
    if (found == live.end()) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            "staged removal of unit " + std::to_string(unit) + ", which is not live"));
    }
    for (const auto& squadron : impl_->squadrons) {
        if (squadron.container == unit
            || std::find(squadron.members.begin(), squadron.members.end(), unit) != squadron.members.end()) {
            return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
                "staged removal of unit " + std::to_string(unit) + ", which belongs to a squadron"));
        }
    }
    live.erase(found);
    auto ledgers = impl_->ledgers;
    const bool sources_changed = impl_->prune_upgrade_holders(ledgers, live);
    std::optional<Impl::CommandLedger> command_ledger;
    if (!impl_->economy.command_bonuses.empty()) {
        auto changed = impl_->update_command_ledger(live, impl_->next_id, InlineExecutor{});
        if (!changed) return core::Result<void>::failure(changed.error());
        command_ledger.emplace(std::move(changed).value());
        const auto applied = impl_->apply_command_bonuses(live, *command_ledger, ledgers, 0, InlineExecutor{});
        if (!applied) return applied;
    }
    if (sources_changed && !impl_->bonus_categories.empty()) {
        std::map<EntityId, TypeId> containers;
        for (const auto& account : ledgers) for (const auto& held : account.completed) {
            const auto station = std::lower_bound(live.begin(), live.end(), held.station,
                [](const LiveUnit& entry, const EntityId id) { return entry.state.entity_id < id; });
            if (station != live.end() && station->state.entity_id == held.station) containers.emplace(held.station, station->state.type_id);
        }
        const auto profiles = impl_->build_bonus_profiles(ledgers, containers, InlineExecutor{});
        if (!profiles) return profiles;
        // Staging is outside the tick: publish the reconciled profiles and unit maxima atomically.
        for (auto& entry : live) {
            if (command_ledger) break;
            const auto applied = impl_->apply_bonuses(entry, impl_->bonuses_for(entry));
            if (!applied) return applied;
        }
    }
    impl_->rebuild(live, false);
    impl_->ledgers = std::move(ledgers);
    if (command_ledger) impl_->command_ledger = std::move(*command_ledger);
    if (sources_changed && !impl_->bonus_categories.empty()) impl_->committed_bonus_profiles.swap(impl_->bonus_profiles);
    impl_->spawners.erase(unit);
    impl_->pads.erase(unit);
    impl_->construction.erase(unit);
    impl_->pad_stage.initialized = false;
    impl_->construction_stage.initialized = false;
    for (auto& [id, state] : impl_->pads) {
        static_cast<void>(id);
        if (state.under_construction == unit) state.under_construction = invalid_entity_id;
        if (state.constructed == unit) state.constructed = invalid_entity_id;
    }
    // A staged removal is the recorder's deletion, not a destruction: it decides nothing (VT-12).
    std::erase_if(impl_->starbases, [&](const StarbaseEntry& entry) { return entry.unit == unit; });
    const auto& published = *impl_->current_snapshot;
    impl_->publish_staged(live, {published.events().begin(), published.events().end()},
        {published.combat_events().begin(), published.combat_events().end()});
    return core::Result<void>::success();
}



} // namespace eawr::sim::tactical
