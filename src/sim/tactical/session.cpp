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

core::Result<void> TacticalSession::submit(const PlayerCommand& command) {
    const auto context = command_context(command.key);
    const auto shape = detail::validate_command_shape(command, impl_->setup.players, context, {});
    if (!shape) {
        return shape;
    }
    if (const auto* cancel = std::get_if<CancelPayload>(&command.payload);
        cancel != nullptr && cancel->entry_id != 0 && !impl_->setup.queue_identities) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::invalid_command,
            context + ": cancel entry requires QIDS extension"));
    }
    if (command.key.tick >= max_ticks) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            context + ": tick exceeds the tactical tick limit"));
    }
    if (command.key.tick < impl_->completed_tick) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::late_command,
            context + ": tick " + std::to_string(command.key.tick)
                + " has already executed; the next tick to execute is "
                + std::to_string(impl_->completed_tick)));
    }
    const std::pair key{command.key.tick, command.key.sequence};
    const auto previous = impl_->last_submitted.find(command.key.player_id);
    if (previous != impl_->last_submitted.end() && !(previous->second < key)) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::order,
            context + ": does not follow the issuer's previous command (tick "
                + std::to_string(previous->second.first) + ", sequence "
                + std::to_string(previous->second.second) + ")"));
    }
    if (impl_->executed.size() + impl_->pending.size() >= max_commands) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::resource_limit,
            context + ": session command count limit reached"));
    }
    impl_->pending.emplace(command.key, command);
    impl_->last_submitted.insert_or_assign(command.key.player_id, key);
    return core::Result<void>::success();
}

void TacticalSession::set_state_hasher(std::shared_ptr<StateHasher> hasher) noexcept { impl_->hasher = std::move(hasher); }

core::Result<void> TacticalSession::submit_reinforcement_search(const PlayerCommand& command,
    const ReinforcementSearch search) {
    if (!std::holds_alternative<ReinforcePayload>(command.payload)
        || (search.attempt != 0 && (search.attempt - 1) % 10 != 0)) {
        return core::Result<void>::failure(detail::diagnostic(diagnostic_codes::command_rejected,
            "SAE-10 search expects an ordinary reinforcement and a complete ring"));
    }
    if (auto submitted = submit(command); !submitted) return submitted;
    impl_->reinforcement_searches.emplace(command.key, search);
    return core::Result<void>::success();
}

std::optional<TacticalSession::ReinforcementSearchResult> TacticalSession::reinforcement_search_result(
    const PlayerId player, const std::uint64_t token) const {
    const auto found = impl_->reinforcement_search_results.find({player, token});
    return found != impl_->reinforcement_search_results.end() ? std::optional(found->second) : std::nullopt;
}

void TacticalSession::set_commit_observer(std::function<void(std::string_view, bool)> observer) {
    impl_->commit_observer = std::move(observer);
}
TacticalSession::TickWork TacticalSession::tick_work() const noexcept { return impl_->tick_work; }

std::uint64_t TacticalSession::completed_tick() const noexcept { return impl_->completed_tick; }
EntityId TacticalSession::next_entity_id() const noexcept { return impl_->next_id; }
std::uint64_t TacticalSession::rng_state() const noexcept { return impl_->rng_state; }
std::span<const Player> TacticalSession::players() const noexcept { return impl_->setup.players; }
std::span<const SensorProfile> TacticalSession::sensors() const noexcept { return impl_->sensors; }
const DurabilityTable& TacticalSession::durability() const noexcept { return impl_->durability; }
const AbilityTable& TacticalSession::abilities() const noexcept { return impl_->abilities; }
const MotionTable& TacticalSession::motion() const noexcept { return impl_->motion; }
const CombatTable& TacticalSession::combat() const noexcept { return impl_->combat; }
const VictoryRules& TacticalSession::victory() const noexcept { return impl_->victory; }
const EconomyRules& TacticalSession::economy() const noexcept { return impl_->economy; }
std::span<const PlayerEconomy> TacticalSession::ledgers() const noexcept { return impl_->ledgers; }
ProductionCounts TacticalSession::production_counts(const PlayerId player, const TypeId type) const {
    if (impl_->player_of(player) == nullptr) return {};
    return impl_->production_counts(player, type, impl_->ledgers, true);
}
const BuildOption* TacticalSession::build_option(const PlayerId player, const EntityId producer, const TypeId type) const {
    if (impl_->player_of(player) == nullptr || impl_->economy.disabled_types.contains(type)) return nullptr;
    const auto found = impl_->handles.find(producer);
    if (found == impl_->handles.end()) return nullptr;
    const auto& identity = impl_->registry.get<Identity>(found->second);
    LiveUnit unit;
    unit.state.entity_id = producer;
    unit.state.type_id = identity.type_id;
    unit.state.owner = identity.owner;
    if (const auto pad = impl_->pads.find(producer); pad != impl_->pads.end()) {
        const auto* builder = impl_->player_of(player);
        const auto* menu = builder != nullptr ? impl_->economy.menu(identity.type_id, builder->faction_id) : nullptr;
        const auto* option = menu != nullptr ? menu->find(type) : nullptr;
        return option != nullptr && option->available && impl_->economy.pads.child(type) != nullptr
            && impl_->allied(player, identity.owner) ? option : nullptr;
    }
    return impl_->build_option(unit, player, type);
}
bool TacticalSession::build_allowed(const PlayerId player, const EntityId producer, const TypeId type) const {
    const auto* option = build_option(player, producer, type);
    if (option == nullptr) return false;
    if (const auto pad = impl_->pads.find(producer); pad != impl_->pads.end()) {
        return pad->second.under_construction == 0
            && pad->second.constructed == 0 && impl_->completed_tick >= pad->second.cooldown_until;
    }
    return production_allowed(*option, true,
        [&](const TypeId counted) { return production_counts(player, counted); });
}
const std::map<EntityId, PadState>& TacticalSession::pads() const noexcept { return impl_->pads; }
const std::map<EntityId, ConstructionState>& TacticalSession::construction() const noexcept { return impl_->construction; }
bool TacticalSession::pad_sale_allowed(const PlayerId player, const EntityId child, const bool single_step) const {
    const auto found = impl_->handles.find(child);
    if (found == impl_->handles.end() || impl_->economy.player(player) == nullptr) return false;
    const auto& identity = impl_->registry.get<Identity>(found->second);
    const auto parent = std::find_if(impl_->pads.begin(), impl_->pads.end(),
        [child](const auto& entry) { return entry.second.constructed == child; });
    return pad_sale_permission(player, identity.owner, impl_->economy.pad_sale(identity.type_id) != nullptr,
        parent != impl_->pads.end() ? &parent->second : nullptr, single_step);
}
core::Result<bool> TacticalSession::pad_build_allowed(const PlayerId player, const EntityId pad) const {
    const auto live = impl_->sorted_live();
    const auto found = std::lower_bound(live.begin(), live.end(), pad,
        [](const LiveUnit& unit, const EntityId key) { return unit.state.entity_id < key; });
    const auto state = impl_->pads.find(pad);
    const auto* builder = impl_->player_of(player);
    if (found == live.end() || found->state.entity_id != pad || state == impl_->pads.end() || builder == nullptr) {
        return core::Result<bool>::success(false);
    }
    const auto* profile = impl_->economy.pads.point(found->state.type_id);
    const auto* menu = impl_->economy.menu(found->state.type_id, builder->faction_id);
    if (profile == nullptr || !profile->build_pad || menu == nullptr || menu->options.empty()
        || !impl_->allied(player, found->state.owner) || impl_->completed_tick < state->second.cooldown_until
        || (impl_->fog && !impl_->fog->revealed(static_cast<std::size_t>(builder - impl_->setup.players.data()), found->state.position))) {
        return core::Result<bool>::success(false);
    }
    std::vector<CaptureCandidate> candidates;
    for (const auto& unit : live) candidates.push_back({{unit.state.entity_id, unit.state.owner, unit.state.position},
        unit.state.type_id, !impl_->arrivals.contains(unit.state.entity_id)});
    return core::Result<bool>::success(pad_construction_allowed(*profile, state->second, player,
        impl_->setup.players, candidates, impl_->economy.pads, pad, found->state.position));
}
const std::map<EntityId, ArrivalState>& TacticalSession::arrivals() const noexcept { return impl_->arrivals; }
core::Result<bool> TacticalSession::reinforcement_point(const PlayerId player, const TypeId type, const math::Vec3& point,
    PlacementWork* work, const std::optional<math::Fixed> facing_yaw) const {
    if (impl_->outcome || impl_->economy.disabled_types.contains(type)) {
        return core::Result<bool>::success(false);
    }
    UnitStage staged(impl_->sorted_live());
    return impl_->placement_valid(player, type, point, staged, impl_->completed_tick, nullptr, work, nullptr, facing_yaw);
}
const std::optional<BattleOutcome>& TacticalSession::outcome() const noexcept { return impl_->outcome; }
std::vector<UnitState> TacticalSession::units() const { return impl_->sorted_units(); }
std::span<const Squadron> TacticalSession::squadrons() const noexcept { return impl_->squadrons; }
const FogCells* TacticalSession::fog_cells() const noexcept { return impl_->fog ? &*impl_->fog : nullptr; }
std::span<const Projectile> TacticalSession::projectiles() const noexcept { return impl_->projectiles; }

bool TacticalSession::request_projectile_explosion(const std::uint64_t id) noexcept {
    const auto found = std::lower_bound(impl_->projectiles.begin(), impl_->projectiles.end(), id,
        [](const Projectile& projectile, const std::uint64_t value) { return projectile.id < value; });
    if (found == impl_->projectiles.end() || found->id != id) return false;
    found->explosion_requested = true;
    return true;
}

std::optional<DurabilityState> TacticalSession::durability_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* health = impl_->registry.try_get<Health>(found->second);
    return health != nullptr ? std::optional(health->value) : std::nullopt;
}

std::optional<AbilityState> TacticalSession::ability_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* able = impl_->registry.try_get<Abilities>(found->second);
    return able != nullptr ? std::optional(able->value) : std::nullopt;
}

TacticalSession::SlicedSearchUse TacticalSession::sliced_searches() const noexcept {
    SlicedSearchUse use;
    for (const auto& entry : impl_->searches) {
        ++use.searches;
        use.bytes += entry.second.search.held_bytes();
    }
    return use;
}

std::optional<MotionState> TacticalSession::motion_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* moving = impl_->registry.try_get<Motion>(found->second);
    return moving != nullptr ? std::optional(moving->value) : std::nullopt;
}

std::optional<math::Fixed> TacticalSession::roll_degrees(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* moving = impl_->registry.try_get<Motion>(found->second);
    return moving != nullptr ? std::optional(moving->roll) : std::nullopt;
}

std::optional<CraftState> TacticalSession::craft_state(const EntityId craft) const {
    const auto found = impl_->crafts.find(craft);
    return found != impl_->crafts.end() ? std::optional(found->second) : std::nullopt;
}

std::optional<SquadronState> TacticalSession::squadron_state(const EntityId container) const {
    const auto found = impl_->minds.find(container);
    return found != impl_->minds.end() ? std::optional(found->second) : std::nullopt;
}

std::optional<CombatState> TacticalSession::combat_state(const EntityId unit) const {
    const auto found = impl_->handles.find(unit);
    if (found == impl_->handles.end()) {
        return std::nullopt;
    }
    const auto* fighting = impl_->registry.try_get<Combat>(found->second);
    return fighting != nullptr ? std::optional(fighting->value) : std::nullopt;
}

std::size_t TacticalSession::pending_command_count() const noexcept { return impl_->pending.size(); }
std::vector<std::uint8_t> TacticalSession::canonical_state_bytes() const { return impl_->canonical_bytes(); }

std::string TacticalSession::state_sha256() const {
    return sim::sha256_hex(impl_->canonical_bytes());
}

std::shared_ptr<const TacticalSnapshot> TacticalSession::snapshot() const noexcept {
    return impl_->current_snapshot;
}

TacticalReplay TacticalSession::record() const {
    return TacticalReplay{impl_->setup, impl_->completed_tick, impl_->executed};
}

TacticalReplay TacticalSession::record_through_next_tick() const {
    TacticalReplay replay{impl_->setup, impl_->completed_tick + 1, impl_->executed};
    // The next step executes the commands keyed to the completed tick, in the queue's canonical order.
    for (const auto& [key, command] : impl_->pending) {
        if (key.tick > impl_->completed_tick) break;
        replay.commands.push_back(command);
    }
    return replay;
}

void TacticalSession::scramble_storage_for_testing() {
    const auto units = impl_->sorted_live();
    impl_->rebuild(units, true);
}

} // namespace eawr::sim::tactical
