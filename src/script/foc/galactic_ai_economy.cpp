#include "galactic_ai_world.hpp"
#include "eawr/sim/galactic/production.hpp"

#include <atomic>
#include <limits>

namespace eawr::script::foc::ai {
namespace {
template<class Rows, class Id, class Key>
const auto* find(const Rows& rows, Id id, Key key) {
    const auto at = std::lower_bound(rows.begin(), rows.end(), id,
        [&](const auto& row, auto value) { return key(row) < value; });
    return at != rows.end() && key(*at) == id ? &*at : nullptr;
}
std::optional<Real> number(const std::vector<Binding>& bindings, std::string_view name) {
    for (const auto& binding : bindings) if (binding.parameter == name)
        if (const auto* value = std::get_if<Real>(&binding.value)) return *value;
    return {};
}
// The fixed token spelling of a per-planet read loop: interned on first use, then reused for every planet.
struct PlanetSlot {
    std::vector<std::string> spelling;
    std::optional<std::uint64_t> id{};
    const std::vector<std::string>* canonical{};
};
bool finite(Real value) { return (value.repr & 0x7ff0000000000000ULL) != 0x7ff0000000000000ULL; }
std::optional<std::int32_t> rounded(Real value) {
    if (!finite(value) || value < real(std::numeric_limits<std::int32_t>::min())
        || real(std::numeric_limits<std::int32_t>::max() - 1) < value) return {};
    return static_cast<std::int32_t>(value + parse_real("0.5").value());
}
std::optional<std::vector<tactical::TypeId>> selected_types(const GalacticPerceptionSetup& setup,
    const std::vector<Binding>& bindings) {
    std::vector<tactical::TypeId> types;
    for (const auto& binding : bindings) if (binding.parameter == "PARAMETER_TYPE") {
        const auto* text = std::get_if<std::string>(&binding.value); if (!text) return {};
        const auto name = upper_case(*text);
        const auto at = std::find_if(setup.types.begin(), setup.types.end(), [&](const auto& row) {
            return upper_case(row.name) == name;
        });
        types.push_back(at == setup.types.end() ? 0 : at->type_id);
    }
    if (types.empty()) return {};
    return types;
}
std::int64_t count(const std::map<tactical::TypeId, std::int64_t>& values, tactical::TypeId type) {
    const auto at = values.find(type); return at == values.end() ? 0 : at->second;
}
bool includes(const std::vector<tactical::TypeId>& types, tactical::TypeId type) {
    return type && std::find(types.begin(), types.end(), type) != types.end();
}
Real fraction(Real a, Real b) { return b == Real{} ? Real{} : a / b; }
core::Result<void> invalid(std::string message) {
    return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, std::move(message)));
}
}

std::shared_ptr<const GalacticEconomyRuntime> fresh_economy_runtime(std::span<const GalacticPlayerFacts> players) {
    auto result = std::make_shared<GalacticEconomyRuntime>();
    result->timeline = 0; result->build_tasks_complete = true;
    for (const auto& player : players) result->budgets.push_back({player.player, {}, {}});
    return result; // WGA-294/295: fresh constructor state, never a load fallback.
}

core::Result<void> GalacticWorld::bind_economy_runtime(std::shared_ptr<const GalacticEconomyRuntime> input) const {
    {
        const auto lock = query_lock();
        if (input == bound().economy_runtime_) return core::Result<void>::success();
    }
    if (input) {
        if (input->timeline && (*input->timeline < 0 || *input->timeline > 5)) return invalid("WGA-295: invalid timeline input");
        std::set<tactical::PlayerId> players;
        std::set<std::tuple<galactic::PlanetId, galactic::ProductionDomain, sim::EntityId>> builds;
        for (const auto& budget : input->budget_rows()) {
            if (!player(budget.player) || !players.insert(budget.player).second || !finite(budget.income))
                return invalid("WGA-293: invalid budget player input");
            for (const auto& [category, values] : budget.categories) {
                (void)category;
                if (!finite(values.income) || !finite(values.resources)) return invalid("WGA-293: invalid category input");
            }
        }
        for (const auto& build : input->build_rows()) {
            if (!build.planet || !build.type || !build.build || !player(build.builder)
                || static_cast<std::size_t>(build.channel) > 1 || !builds.emplace(build.planet, build.channel, build.build).second
                || !finite(build.cost_to_completion) || !finite(build.spendable) || !finite(build.budget_seconds)
                || build.status < GalacticBudgetBuild::Status::unassigned || GalacticBudgetBuild::Status::finished < build.status)
                return invalid("WGA-293: invalid build-task budget input");
        }
    }
    // Runtime publications belong to their upstream saved owners, like bind's
    // completed world input; this derived copy is not a seventh native owner.
    const auto lock = query_lock(); bound().economy_runtime_ = std::move(input);
    return core::Result<void>::success();
}

bool GalacticWorld::refresh_economy_runtime() const {
    if (!economy_supplier_) return true;
    // Inside a partitioned phase, read the copy its owner refreshed before the
    // fan-out (ADR-009): partitions never call the supplier.
    if (partitioned_reads_.load(std::memory_order_acquire) != 0) return partitioned_refresh_ok_;
    // A supplier reads owner caches that are not thread-safe (the engine's
    // build-budget publication); concurrent calls corrupted it at 8 workers
    // (#2971). Other callers take turns: battle-mode child scripts query their
    // galactic parent from their own partitions.
    const auto reads = sequential_reads();
    auto supplied = economy_supplier_();
    if (!supplied || !supplied.value()) return false;
    {
        const auto lock = query_lock();
        if (bound().economy_runtime_ == supplied.value()) return true;
    }
    return static_cast<bool>(bind_economy_runtime(std::move(supplied).value()));
}

GalacticWorld::PartitionedReads GalacticWorld::partitioned_reads() const {
    // Serial, on the owner thread. A nested scope keeps the outer refresh.
    if (partitioned_reads_.load(std::memory_order_acquire) == 0) {
        const Engine::BuildBudgetReads budgets;
        partitioned_refresh_ok_ = refresh_economy_runtime();
        if (!records_ready_) { rebuild_records(); records_ready_ = true; }
        const auto movement = movement_input_revision();
        for (auto& [player, slice] : shards_) {
            static_cast<void>(player); slice.movement_read_base = movement - slice.movement_revision;
        }
    }
    partitioned_reads_.fetch_add(1, std::memory_order_release);
    return PartitionedReads(*this);
}

core::Result<void> GalacticWorld::refresh_economy_queues(const sim::PartitionExecutor& executor, bool queues_changed) {
    const auto& generation = bound().snapshot_->economy_input_generation();
    if (!generation || !bound().economy_generation_ || bound().economy_planets_.size() != bound().planets_.size()
        || bound().economy_players_.size() != setup_.players.size()) return prepare_economy_tokens(executor);
    for (const auto& selected : setup_.players)
        if (!bound().economy_players_.contains(selected.player)) return prepare_economy_tokens(executor);
    // The qualified bind retained physical identity/owner, all production facts
    // except queues, observer inputs, original types, technology and movement.
    // Credits feed account/budget readers directly; they do not enter owned-force
    // or maintenance reductions. Keep those ordered terms and refresh queues only.
    if (queues_changed) {
        // Synthetic extra/missing producer locations take the independent full
        // reducer, whose player queue census visits the complete producer list.
        if (!bound().snapshot_->production() || bound().snapshot_->production()->planets.size() != bound().planets_.size())
            return prepare_economy_tokens(executor);
        for (std::size_t index = 0; index < bound().planets_.size(); ++index)
            if (bound().snapshot_->production()->planets[index].planet != bound().planets_[index].planet.id)
                return prepare_economy_tokens(executor);
        std::vector<std::map<tactical::TypeId, std::int64_t>> planet_queued(bound().planets_.size());
        std::vector<std::map<std::pair<tactical::PlayerId, tactical::TypeId>, std::int64_t>> owner_queued(bound().planets_.size());
        std::vector<tactical::TypeId> heads(bound().planets_.size());
        const auto phase = executor.execute_phase("galactic-ai-economy-queues", sim::tick_partition_count, [&](std::size_t partition) {
            const auto range = sim::partition_range(partition, bound().planets_.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto* production = bound().planets_[index].production;
                if (!production) continue;
                for (const auto& queue : production->queues) for (const auto& entry : queue) {
                    ++planet_queued[index][entry.type]; ++owner_queued[index][{entry.builder, entry.type}];
                }
                if (!production->queues[0].empty()) heads[index] = production->queues[0].front().type;
            }
        });
        if (!phase) return phase;
        std::map<tactical::PlayerId, std::map<tactical::TypeId, std::int64_t>> queued;
        // ADR-009: commit partition counts in planet order; no entity service.
        for (const auto& counts : owner_queued) for (const auto& [key, amount] : counts) {
            if (!bound().economy_players_.contains(key.first)) return prepare_economy_tokens(executor);
            queued[key.first][key.second] += amount;
        }
        for (auto& [owner, row] : bound().economy_players_) row.queued = std::move(queued[owner]);
        for (std::size_t index = 0; index < bound().economy_planets_.size(); ++index) {
            bound().economy_planets_[index].queued = std::move(planet_queued[index]);
            bound().economy_planets_[index].ground_head = heads[index];
        }
        bound().work_.economy_planets = bound().planets_.size();
    }
    bound().economy_generation_ = generation;
    // Ordinary ownership observations and credit-dependent budget queries must
    // still execute at their original synchronized boundary after either delta.
    clear_economy_memos();
    return core::Result<void>::success();
}

core::Result<void> GalacticWorld::prepare_economy_tokens(const sim::PartitionExecutor& executor) {
    const auto& generation = bound().snapshot_->economy_input_generation();
    if (generation && generation == bound().economy_generation_) return core::Result<void>::success();
    std::vector<EconomyPlanet> planets(bound().planets_.size());
    const auto phase = executor.execute_phase("galactic-ai-economy", sim::tick_partition_count, [&](std::size_t partition) {
        const auto range = sim::partition_range(partition, planets.size());
        for (auto i = range.begin; i < range.end; ++i) {
            const auto& source = bound().planets_[i]; auto& out = planets[i]; out.planet = source.planet.id;
            if (bound().snapshot_->observer()) if (const auto* observer = find(bound().snapshot_->observer()->planets, out.planet, [](const auto& row) { return row.planet; }))
                out.owner = observer->target_owner;
            if (!source.production) continue;
            const auto& production = *source.production; out.production_available = true;
            if (!bound().snapshot_->observer()) out.owner = production.target_owner;
            out.ground = production.ground_level; out.space = production.space_level;
            if (setup_.maximum_ground_level && *setup_.maximum_ground_level != Real{})
                out.maximum_ground = real(std::min<std::int64_t>(production.ground_slots, 5)) / *setup_.maximum_ground_level;
            if (setup_.maximum_space_level && *setup_.maximum_space_level != Real{})
                out.maximum_space = real(production.maximum_space_level) / *setup_.maximum_space_level;
            for (const auto& object : production.objects) if (object.kind == galactic::ProductionKind::structure)
                ++out.structures[object.type];
            for (const auto& queue : production.queues) for (const auto& entry : queue) ++out.queued[entry.type];
            if (!production.queues[0].empty()) out.ground_head = production.queues[0].front().type;
        }
    });
    if (!phase) return phase;
    std::map<tactical::PlayerId, EconomyPlayer> players;
    for (const auto& player : setup_.players) {
        auto& out = players[player.player];
        if (player.neutral) out.maintenance = Real{};
        else if (setup_.faction_maintenance.contains(player.player)) out.maintenance = Real{};
    }
    // WGA-288/296: original owned identities, not an orbit-only force total.
    // This roster is already indexed by bind; aggregation runs only on change.
    std::map<sim::EntityId, tactical::TypeId> originals;
    for (const auto& original : bound().snapshot_->commissioned_originals()) originals.emplace(original.id, original.original_type);
    for (const auto& unit : bound().reader_.view()->units) if (unit.type) {
        const auto original = originals.find(unit.id);
        ++players[unit.owner].owned[original == originals.end() ? unit.type : original->second];
    }
    if (bound().snapshot_->production()) for (const auto& location : bound().snapshot_->production()->planets) {
        if (location.starbase_type) ++players[location.starbase_owner].owned[location.starbase_type];
        for (const auto& queue : location.queues) for (const auto& entry : queue) ++players[entry.builder].queued[entry.type];
    }
    for (auto& [owner, out] : players) {
        const auto* account = this->account(owner);
        for (const auto& [id, amount] : out.owned) {
            const auto* type = find(setup_.economy_types, id, [](const auto& row) { return row.type; });
            const auto* force = find(setup_.force_types, id, [](const auto& row) { return row.type; });
            if (!type || !force) { out.force_available = false; continue; }
            const auto append = [&](auto& terms, std::uint64_t categories, Real power) {
                // Repetition preserves double accumulation of single source terms.
                for (std::int64_t i = 0; i < amount; ++i) terms.push_back({owner, categories, to_single(power)});
            };
            if (type->space_unit) append(out.space, force->categories, force->power);
            else if (type->space_structure || force->space_weapon) {
                if (force->space_structure_categories) append(out.space, *force->space_structure_categories, force->power);
                else out.force_available = false;
            }
            if (type->ground_unit) append(out.ground, force->categories, force->power);
            if (!account || account->tech_level > 5) {
                if (std::any_of(force->starting_spawns.begin(), force->starting_spawns.end(), [](const auto& row) { return !row.empty(); })) out.force_available = false;
                continue;
            }
            auto selected = account->tech_level;
            while (selected > 0 && force->starting_spawns[selected].empty()) --selected;
            for (const auto& spawn : force->starting_spawns[selected]) {
                const auto* spawned = find(setup_.force_types, spawn.type, [](const auto& row) { return row.type; });
                const auto first = std::find_if(force->starting_spawns[selected].begin(), force->starting_spawns[selected].end(),
                    [&](const auto& row) { return row.type == spawn.type; });
                if (!spawned || first->count < 0) { out.force_available = false; continue; }
                const auto power = to_single(to_single(spawned->power) * real(first->count));
                append(out.space, spawned->categories, power); append(out.ground, spawned->categories, power);
            }
        }
    }
    // WGA-291: only fleet breakdowns contribute maintenance, including transit.
    std::map<sim::EntityId, const galactic::MovementContents*> contents;
    std::set<sim::EntityId> parented;
    if (bound().snapshot_->movement()) for (const auto& row : bound().snapshot_->movement()->contents) {
        contents.emplace(row.container, &row);
        const auto* container = find(bound().reader_.view()->units, row.container, [](const auto& value) { return value.id; });
        const auto* type = container ? find(bound().snapshot_->movement()->types, container->type, [](const auto& value) { return value.type; }) : nullptr;
        if ((type && type->fleet_container) || find(bound().snapshot_->fleets(), row.container, [](const auto& value) { return value.id; }))
            for (const auto& child : row.children) parented.insert(child.id);
    }
    for (const auto& fleet : bound().snapshot_->fleets()) for (const auto& member : fleet.members) parented.insert(member.id);
    for (const auto& fleet : bound().snapshot_->fleets()) {
      if (parented.contains(fleet.id)) continue;
      std::vector<galactic::FleetMember> breakdown = fleet.members;
      for (const auto& member : fleet.members) {
        const auto* type = bound().snapshot_->movement() ? find(bound().snapshot_->movement()->types, member.type, [](const auto& row) { return row.type; }) : nullptr;
        const auto at = contents.find(member.id);
        if (type && (type->squadron || type->transport) && at != contents.end())
            breakdown.insert(breakdown.end(), at->second->children.begin(), at->second->children.end());
      }
      for (const auto& member : breakdown) {
        auto& out = players[fleet.owner]; if (!out.maintenance) continue;
        const auto* who = player(fleet.owner); if (who && who->neutral) continue;
        const auto factor = setup_.faction_maintenance.find(fleet.owner);
        if (factor == setup_.faction_maintenance.end()) { out.maintenance.reset(); continue; }
        if (factor->second == Real{}) continue;
        const auto original = originals.find(member.id);
        const auto* type = find(setup_.economy_types, original == originals.end() ? member.type : original->second, [](const auto& row) { return row.type; });
        if (!type || !type->maintenance) { out.maintenance.reset(); continue; }
        const auto coefficient = to_single(to_single(*type->maintenance) * to_single(factor->second));
        if (coefficient == Real{} || type->generic_seconds == Real{}) continue;
        const auto rate = to_single(to_single(to_single(real(static_cast<std::int32_t>(type->generic_price))) / to_single(type->generic_seconds)) / real(setup_.logical_fps));
        *out.maintenance += coefficient * rate;
      }
    }
    bound().economy_planets_ = std::move(planets); bound().economy_players_ = std::move(players); bound().economy_generation_ = generation;
    clear_economy_memos(); ++bound().economy_rebuilds_;
    bound().work_.economy_planets = bound().economy_planets_.size(); bound().work_.economy_objects = bound().reader_.view()->units.size();
    bound().work_.economy_rebuilds = 1;
    return core::Result<void>::success();
}

std::optional<Real> GalacticWorld::target_economy_token(const GalacticQueryKey& key, std::string_view token) const {
    const auto* row = find(bound().economy_planets_, key.planet, [](const auto& p) { return p.planet; });
    if (!row || !row->production_available) return {};
    if (token == "HASSTARBASEOFLEVEL" || token == "HASGROUNDBASEOFLEVEL") {
        const auto level = rounded(number(key.bindings, "PARAMETER_LEVEL").value_or(real(1)));
        if (!level || *level < 1) return {};
        return real((token == "HASSTARBASEOFLEVEL" ? row->space : row->ground) >= *level ? 1 : 0);
    }
    const auto types = selected_types(setup_, key.bindings); if (!types) return {};
    const auto complete = number(key.bindings, "PARAMETER_ONLY_CONSIDER_COMPLETE").value_or(Real{}) == real(1);
    std::int64_t total{};
    for (const auto& [type, amount] : row->structures) if (includes(*types, type)) total += amount;
    if (token == "HASSTRUCTURE") return real(total || (!complete && includes(*types, row->ground_head)) ? 1 : 0);
    if (!complete) for (const auto type : *types) total += count(row->queued, type);
    return real(total);
}

GalacticWorld::PlayerEconomyMemo& GalacticWorld::economy_memo(const GalacticQueryKey& key, ReadMode mode, PlayerEconomyMemo& scratch) const {
    // WGA-65/299: aggregate memoization must not suppress the next frame's
    // ordinary ownership observations, including zero-interval cache rows.
    // A restored owner has a cold memo and must publish the same observations.
    auto& mine = shard(key.player);
    if (mode == ReadMode::ordinary && mine.memo_bindings_frame != bound().synchronized_frame_) {
        mine.memo_bindings.clear(); mine.memo_bindings.emplace_back(); mine.memo_binding_ids.clear();
        mine.economy = {}; mine.memo_bindings_frame = bound().synchronized_frame_;
    }
    // Only this player's task writes its aggregate memo. Its remembered input
    // expiries remain player-local, including nested ownership observations.
    auto& memo = mode == ReadMode::ordinary ? mine.economy : scratch;
    if (mode == ReadMode::ordinary && (memo.frame != bound().synchronized_frame_
        || (memo.expiry && *memo.expiry < real(static_cast<std::int64_t>(bound().synchronized_frame_))))) {
        memo.queries.clear(); memo.expiry.reset(); memo.frame = bound().synchronized_frame_;
    }
    return memo;
}

std::optional<Real> GalacticWorld::corruption_total_token(const GalacticQueryKey& key, std::string_view token, ReadMode mode) const {
    // WGA-487: every galaxy planet of any owner with a corruption type, divided by the galaxy planet count for the
    // normalized form (the PlanetsControlled denominator). A missing corruption row stays unavailable. The count is
    // memoized per player and frame like the PlanetsControlled aggregate, so it is one scan per frame, not one per read.
    PlayerEconomyMemo uncached_memo;
    auto& memo = economy_memo(key, mode, uncached_memo);
    std::optional<GalacticInternedKey> canonical;
    if (mode == ReadMode::ordinary) { canonical = memo_key(key); canonical->planet = 0; }
    if (canonical) if (const auto cached = memo.queries.find(*canonical); cached != memo.queries.end()) return cached->second;
    std::optional<Real> value;
    if (const auto* economy = bound().snapshot_->economy_corruption(); economy && economy->planets.size() == bound().planets_.size()) {
        Real corrupted{};
        for (const auto& at : economy->planets) if (at.corruption != galactic::corruption_fixture::Type::none) corrupted += real(1);
        value = token == "PLANETSCORRUPTED" && !bound().planets_.empty()
            ? corrupted / real(static_cast<std::int64_t>(bound().planets_.size())) : corrupted;
    }
    if (canonical) memo.queries.emplace(*canonical, value);
    return value;
}

std::optional<Real> GalacticWorld::player_economy_token(const GalacticQueryKey& key, tactical::PlayerId owner,
    std::string_view token, ReadMode mode) const {
    PlayerEconomyMemo uncached_memo;
    auto& memo = economy_memo(key, mode, uncached_memo);
    const std::vector<Binding> no_bindings;
    // Same read() call, order and arguments as building a fresh key per planet, without the per-planet token copies
    // and re-interning (#4306).
    const auto read_slot = [&](PlanetSlot& slot, const std::vector<Binding>& bindings, galactic::PlanetId planet) {
        if (!slot.id) {
            auto& mine = shard(key.player);
            slot.id = intern_token(mine, slot.spelling);
            slot.canonical = &token_of(mine, *slot.id).tokens;
        }
        return read(GalacticQueryKey{key.player, key.enemy, key.human, planet, *slot.canonical, bindings, *slot.id}, mode);
    };
    PlanetSlot ownership_slot{{"VARIABLE_TARGET", "TARGETPLAYERID"}};
    if (token == "PLANETSCONTROLLED" || token == "PLANETSCONTROLLEDUNNORMALIZED") {
        std::optional<GalacticInternedKey> canonical;
        if (mode == ReadMode::ordinary) { canonical = memo_key(key); canonical->planet = 0; }
        if (mode == ReadMode::ordinary) if (const auto cached = memo.queries.find(*canonical); cached != memo.queries.end()) return cached->second;
        Real controlled{};
        for (const auto& row : bound().planets_) {
            const auto target_owner = read_slot(ownership_slot, no_bindings, row.planet.id); if (!target_owner) return {};
            const auto selected = rounded(*target_owner); if (!selected) return {};
            if (*selected == static_cast<std::int64_t>(owner)) controlled += real(1);
        }
        const auto value = token == "PLANETSCONTROLLED" && !bound().planets_.empty()
            ? controlled / real(static_cast<std::int64_t>(bound().planets_.size())) : controlled;
        if (mode == ReadMode::ordinary) memo.queries.emplace(*canonical, value);
        return value;
    }
    const auto at = bound().economy_players_.find(owner); if (at == bound().economy_players_.end()) return {};
    const auto& player_counts = at->second;
    if (token == "BUDGETFRACTIONTOBUILD") return owner == key.player ? budget_fraction(key, owner) : std::optional{Real{}};
    if (token == "SPACETOTALUNNORMALIZED" || token == "GROUNDTOTALUNNORMALIZED") {
        if (!player_counts.force_available) return {};
        const auto mask_value = number(key.bindings, "PARAMETER_CATEGORY");
        if (mask_value && (!finite(*mask_value) || *mask_value < Real{}
            || !( *mask_value < parse_real("18446744073709551616").value()))) return {};
        const auto mask = mask_value ? static_cast<std::uint64_t>(*mask_value) : ~std::uint64_t{};
        Real total{};
        for (const auto& term : token == "SPACETOTALUNNORMALIZED" ? player_counts.space : player_counts.ground)
            if ((term.categories & mask) != 0) total += term.power;
        return total;
    }
    if (token == "HASUNIT") {
        const auto types = selected_types(setup_, key.bindings); if (!types) return {};
        const auto requested = number(key.bindings, "PARAMETER_COUNT").value_or(real(1));
        if (!finite(requested) || requested < real(std::numeric_limits<std::int32_t>::min())
            || real(std::numeric_limits<std::int32_t>::max()) < requested) return {};
        auto remaining = static_cast<std::int64_t>(static_cast<std::int32_t>(requested));
        const bool complete = Real{} < number(key.bindings, "PARAMETER_ONLY_CONSIDER_COMPLETE").value_or(Real{});
        for (const auto& [type, amount] : player_counts.owned) if (includes(*types, type)) remaining -= amount;
        if (!complete) for (const auto type : *types) remaining -= count(player_counts.queued, type);
        return real(remaining <= 0 ? 1 : 0);
    }
    if (token == "HASTECHTOPRODUCE") {
        const auto types = selected_types(setup_, key.bindings); if (!types) return {};
        const auto* account = this->account(owner); const auto* who = player(owner);
        if (!account || !who || !bound().snapshot_->production()) return {};
        const auto* production = find(bound().snapshot_->production()->players, owner, [](const auto& row) { return row.player; });
        if (!production) return {};
        for (const auto id : *types) {
            if (!id) continue;
            const auto* type = find(setup_.economy_types, id, [](const auto& row) { return row.type; });
            if (!type) return {};
            if (std::find(type->affiliations.begin(), type->affiliations.end(), who->faction) == type->affiliations.end()) continue;
            if (type->initially_locked) {
                if (std::binary_search(production->unlocked.begin(), production->unlocked.end(), id)) return real(1);
            } else if (type->tech_level <= account->tech_level) return real(1);
        }
        return Real{};
    }
    // WGA-299: foreign roots need observer-local target cache aggregation. Do
    // not expose raw foreign production by reusing the selected owner's census.
    if (owner != key.player) return {};
    std::optional<GalacticInternedKey> canonical;
    if (mode == ReadMode::ordinary) { canonical = memo_key(key); canonical->planet = 0; }
    if (mode == ReadMode::ordinary) if (const auto cached = memo.queries.find(*canonical); cached != memo.queries.end()) return cached->second;
    const auto evaluate = [&]() -> std::optional<Real> {
        if (token == "MAINTENANCEUNNORMALIZED") return player_counts.maintenance;
        if (token == "MAINTENANCE" || token == "NETINCOMEUNNORMALIZED" || token == "GROSSINCOMEUNNORMALIZED") {
            Real gross{};
            for (const auto& row : bound().planets_) {
                const auto target_owner = read_slot(ownership_slot, no_bindings, row.planet.id); if (!target_owner) return {};
                if (*target_owner != real(owner)) continue;
                if (!row.income) { return {}; }
                gross += *row.income;
            }
            if (token == "GROSSINCOMEUNNORMALIZED") return gross;
            if (token == "MAINTENANCE" && gross == Real{}) return Real{};
            if (!player_counts.maintenance) return {};
            return token == "MAINTENANCE" ? *player_counts.maintenance / gross : gross - *player_counts.maintenance;
        }
        if (token == "CANADVANCETECH") {
            // Admission is supplied by the same production owner as ordinary
            // buildability; queued next-tech at either channel's head also wins.
            const auto* account = this->account(owner); const auto* who = player(owner);
            if (!account || !who || !bound().snapshot_->production() || !setup_.economy_types_complete) return {};
            if (account->tech_level == std::numeric_limits<std::uint32_t>::max()) return Real{};
            const auto faction = next_technology_types_.find(who->faction);
            if (faction == next_technology_types_.end()) return Real{};
            const auto next = faction->second.find(account->tech_level + 1);
            if (next == faction->second.end()) return Real{};
            const auto& production = *bound().snapshot_->production();
            const auto* builder = find(production.players, owner, [](const auto& row) { return row.player; });
            const auto* balance = bound().snapshot_->player_economy() ? find(bound().snapshot_->player_economy()->players, owner, [](const auto& row) { return row.player; }) : nullptr;
            const auto* population = find(bound().snapshot_->population(), owner, [](const auto& row) { return row.player; });
            if (!builder || !balance) return {};
            const auto reservations = galactic::production_reservations(production, owner);
            // The reduction sources depend only on the income inputs and planets, not on the offer: gather them once
            // per evaluation instead of once per prepared offer and per price/time multiplier (#3003 overload).
            const auto* income_inputs = bound().snapshot_->income() ? &bound().snapshot_->income()->inputs : nullptr;
            std::optional<std::vector<galactic::ProductionReductionSource>> reductions;
            // WGEP-149: the same retained target view exposed by targets(),
            // testing this exact first type's heads before eligibility at each target.
            for (const auto& target : bound().planets_) {
                if (!target.production) continue;
                const auto& location = *target.production;
                for (const auto& queue : location.queues)
                    if (!queue.empty() && queue.front().builder == owner && queue.front().type == next->second) return real(1);
                for (const auto& offer : location.offers) if (offer.type == next->second) {
                    if (income_inputs && !reductions) reductions = galactic::production_reduction_sources(*income_inputs, bound().snapshot_->planets());
                    const auto prepared = galactic::prepared_production_offer(offer, income_inputs,
                        reductions ? std::span<const galactic::ProductionReductionSource>(*reductions)
                                   : std::span<const galactic::ProductionReductionSource>{},
                        location.planet, owner, bound().snapshot_->economy_corruption());
                    if (!prepared) return {};
                    const galactic::ProductionAdmissionInputs inputs{production, location, *builder, *balance,
                        bound().snapshot_->fleets(), target.planet.owner, population, bound().next_id_, bound().reader_.view()->units.size() + bound().snapshot_->routes().size(), &reservations};
                    if (galactic::production_admission(inputs, &prepared.value(), offer.domain, true) == galactic::ProductionAdmission::allowed) return real(1);
                }
            }
            return Real{};
        }
        const auto level = rounded(number(key.bindings, "PARAMETER_LEVEL").value_or(real(1)));
        const auto requested = rounded(number(key.bindings, "PARAMETER_COUNT").value_or(real(1)));
        const auto types = selected_types(setup_, key.bindings);
        std::int64_t owned{}, matches{}, structures{};
        PlanetSlot current_slot{{"VARIABLE_TARGET", token == "MAXEDSTARBASES" ? "STARBASELEVEL" : "GROUNDBASELEVEL"}};
        PlanetSlot maximum_slot{{"VARIABLE_TARGET", token == "MAXEDSTARBASES" ? "MAXSTARBASELEVEL" : "MAXGROUNDBASELEVEL"}};
        PlanetSlot qualifies_slot{{"VARIABLE_TARGET", token.find("STARBASE") != std::string_view::npos ? "HASSTARBASEOFLEVEL" : "HASGROUNDBASEOFLEVEL"}};
        PlanetSlot structure_slot{{"VARIABLE_TARGET", std::string(token)}};
        for (const auto& row : bound().economy_planets_) {
            const auto target_owner = read_slot(ownership_slot, no_bindings, row.planet); if (!target_owner) return {};
            if (*target_owner != real(owner)) continue;
            ++owned;
            if (!row.production_available) return {};
            if (token == "MAXEDSTARBASES" || token == "MAXEDGROUNDBASES") {
                const auto current = read_slot(current_slot, no_bindings, row.planet); if (!current) return {};
                const auto maximum = read_slot(maximum_slot, no_bindings, row.planet); if (!maximum) return {};
                if (*current == *maximum) ++matches;
            } else if (token == "HASSTARBASEOFLEVEL" || token == "NUMSTARBASEOFLEVEL"
                || token == "HASGROUNDBASEOFLEVEL" || token == "NUMGROUNDBASEOFLEVEL") {
                if (!level || *level < 1 || !requested) return {};
                const auto qualifies = read_slot(qualifies_slot, key.bindings, row.planet); if (!qualifies) return {};
                if (*qualifies != Real{}) ++matches;
            } else if (token == "HASSTRUCTURE" || token == "STRUCTURECOUNT") {
                const auto value = read_slot(structure_slot, key.bindings, row.planet); if (!value) return {};
                structures += static_cast<std::int64_t>(*value);
            }
        }
        if (token == "MAXEDSTARBASES" || token == "MAXEDGROUNDBASES") return fraction(real(matches), real(owned));
        if (token == "STRUCTURECOUNT") return types ? std::optional{real(structures)} : std::nullopt;
        if (token == "HASSTRUCTURE" || token == "HASSTARBASEOFLEVEL" || token == "NUMSTARBASEOFLEVEL"
            || token == "HASGROUNDBASEOFLEVEL" || token == "NUMGROUNDBASEOFLEVEL") {
            if (!requested || (token == "HASSTRUCTURE" && !types)) return {};
            const auto total = std::max<std::int64_t>(0, (token == "HASSTRUCTURE" ? structures : matches) - (static_cast<std::int64_t>(*requested) - 1));
            return real(token.starts_with("HAS") ? std::min<std::int64_t>(1, total) : total);
        }
        return {};
    };
    const auto value = evaluate();
    if (mode == ReadMode::ordinary) memo.queries.emplace(*canonical, value);
    return value;
}

std::optional<Real> GalacticWorld::budget_fraction(const GalacticQueryKey& key, tactical::PlayerId owner) const {
    if (!bound().economy_runtime_ || !bound().snapshot_ || bound().economy_runtime_->completed_tick != bound().snapshot_->completed_tick()) return {};
    const auto budget = std::find_if(bound().economy_runtime_->budget_rows().begin(), bound().economy_runtime_->budget_rows().end(),
        [&](const auto& row) { return row.player == owner; });
    const auto types = selected_types(setup_, key.bindings);
    if (budget == bound().economy_runtime_->budget_rows().end() || !types || types->size() != 1 || !types->front()) return {};
    const auto* type = find(setup_.economy_types, types->front(), [](const auto& row) { return row.type; });
    const auto* account = this->account(owner); if (!type || !account || !bound().snapshot_->production()) return {};
    const auto price = to_single(real(static_cast<std::int32_t>(type->generic_price)));
    auto cost = price, seconds = to_single(type->generic_seconds);
    bool active{};
    for (const auto& location : bound().snapshot_->production()->planets) {
        const auto channel = type->built_on_ground ? 0U : 1U;
        const auto& queue = location.queues[channel];
        if (queue.empty() || queue.front().type != type->type || queue.front().builder != owner) continue;
        const auto task = std::find_if(bound().economy_runtime_->build_rows().begin(), bound().economy_runtime_->build_rows().end(),
            [&](const auto& row) { return row.planet == location.planet && row.type == type->type
                && row.builder == owner && static_cast<std::size_t>(row.channel) == channel && row.build == queue.front().build_id; });
        if (task == bound().economy_runtime_->build_rows().end()) {
            if (bound().economy_runtime_->build_tasks_complete) return Real{};
            return {}; // WGA-298: missing owning AI build-task publication.
        }
        if (task->status == GalacticBudgetBuild::Status::finished) continue;
        if (task->status != GalacticBudgetBuild::Status::goal) return Real{};
        const auto& entry = queue.front();
        if (entry.full_seconds == math::Fixed{}) return {}; // progress source unavailable
        const auto progress_remaining = std::clamp(to_single(numeric::from_fixed(entry.last_remaining) / numeric::from_fixed(entry.full_seconds)), Real{}, real(1));
        cost = to_single(std::min(cost, to_single(progress_remaining * price)));
        cost = to_single(std::min(cost, to_single(task->cost_to_completion)) - to_single(task->spendable));
        const auto frame = bound().snapshot_->clock().galactic_frame;
        const auto elapsed = frame > entry.start_frame ? frame - entry.start_frame : 0;
        const auto remaining = elapsed >= entry.duration_frames ? 0 : entry.duration_frames - elapsed;
        const auto countdown = math::Fixed::from_ratio(static_cast<std::int64_t>(remaining), galactic::galactic_frames_per_second);
        if (!countdown) return {};
        seconds = std::min(seconds, to_single(numeric::from_fixed(countdown.value())));
        seconds = std::min(seconds, to_single(task->budget_seconds)); active = true;
    }
    Real income = to_single(budget->income), resources = numeric::from_fixed(account->credits);
    if (const auto category = number(key.bindings, "PARAMETER_CATEGORY")) {
        if (!finite(*category) || *category < Real{} || real(std::numeric_limits<std::uint32_t>::max()) < *category) return {};
        const auto at = budget->categories.find(static_cast<std::uint32_t>(*category));
        income = at == budget->categories.end() ? Real{} : to_single(at->second.income);
        resources = at == budget->categories.end() ? Real{} : to_single(at->second.resources);
    }
    auto support = to_single(seconds * income);
    if (!active) support += to_single(resources);
    if (cost < parse_real("0.0001").value()) return Real{};
    return support > Real{} ? cost / support : parse_real("9.999999843067494e17").value(); // WGA-293
}
} // namespace eawr::script::foc::ai
