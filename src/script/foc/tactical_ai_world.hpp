#pragma once

// Tactical implementation of the AI world boundary (WGA-01). Borrowed session
// and snapshot inputs are rebound by the manager before every script barrier.
#include "ai_engine.hpp"

#include <array>

namespace eawr::script::foc::detail {
inline std::shared_ptr<const WorldView> build_view(const tactical::TacticalSession& world, const tactical::TacticalSnapshot& snapshot,
    const sim::PartitionExecutor* executor = nullptr) {
    auto view = std::make_shared<WorldView>();
    view->tick = world.completed_tick();
    const auto snapshot_players = snapshot.players();
    for (std::size_t index = 0; index < snapshot_players.size(); ++index) {
        view->players.push_back(ViewPlayer{snapshot_players[index].player_id, snapshot_players[index].team_id, index});
    }
    // Published instances and live containers already have ascending identities.
    // Prepare only the member join and formation values; no per-tick node maps.
    const auto instances = snapshot.instances();
    const auto squadrons = world.squadrons();
    using Member = std::pair<sim::EntityId, std::size_t>;
    std::array<std::vector<Member>, sim::tick_partition_count> member_rows;
    std::vector<std::optional<std::pair<sim::EntityId, bool>>> formations(squadrons.size());
    // The current publication supplies identities even if the requested snapshot
    // omits an instance. Read each exact live state in its own worker output row.
    const auto completed = world.snapshot();
    const auto units = completed->instances();
    const sim::InlineExecutor inline_executor;
    // Fixed work threshold, independent of wall time or worker count.
    constexpr std::size_t parallel_view_rows = 256;
    const auto& preparation_executor = executor && units.size() >= parallel_view_rows
        ? *executor : static_cast<const sim::PartitionExecutor&>(inline_executor);
    if (!squadrons.empty()) {
        const auto indexed = preparation_executor.execute_phase("ai-world-view",
            sim::tick_partition_count, [&](const std::size_t partition) {
            auto& members = member_rows[partition];
            const auto range = sim::partition_range(partition, squadrons.size());
            for (auto index = range.begin; index < range.end; ++index) {
                const auto& squadron = squadrons[index];
                if (const auto state = world.squadron_state(squadron.container))
                    formations[index] = std::pair{state->target, state->mode == tactical::SquadronMode::move};
                for (const auto member : squadron.members) members.emplace_back(member, index);
            }
            // The original emplace retained the first container for a repeated member.
            std::stable_sort(members.begin(), members.end(),
                [](const Member& left, const Member& right) { return left.first < right.first; });
        });
        if (!indexed) throw std::runtime_error("AI world view preparation failed: " + indexed.error().message);
    }
    // Ordered balanced merge of the already sorted partition runs. Equal keys keep
    // container/input order, exactly as the preceding first-emplace map did.
    std::array<std::size_t, sim::tick_partition_count + 1> offsets{};
    for (std::size_t index = 0; index < member_rows.size(); ++index)
        offsets[index + 1] = offsets[index] + member_rows[index].size();
    std::vector<Member> craft;
    craft.reserve(offsets.back());
    for (const auto& row : member_rows) craft.insert(craft.end(), row.begin(), row.end());
    for (std::size_t width = 1; !craft.empty() && width < member_rows.size(); width *= 2) {
        for (std::size_t first = 0; first < member_rows.size(); first += width * 2) {
            const auto middle = std::min(first + width, member_rows.size());
            const auto last = std::min(first + width * 2, member_rows.size());
            std::inplace_merge(craft.begin() + static_cast<std::ptrdiff_t>(offsets[first]),
                craft.begin() + static_cast<std::ptrdiff_t>(offsets[middle]),
                craft.begin() + static_cast<std::ptrdiff_t>(offsets[last]),
                [](const Member& left, const Member& right) { return left.first < right.first; });
        }
    }
    const auto fraction = [](math::Fixed part, math::Fixed whole) {
        if (whole.raw() <= 0) return LuaNumber(0);
        return numeric::from_fixed(part) / numeric::from_fixed(whole);
    };
    view->units.resize(units.size());
    view->capture_candidates.resize(units.size());
    const auto prepared = preparation_executor.execute_phase("ai-world-view",
        sim::tick_partition_count, [&](const std::size_t partition) {
      const auto range = sim::partition_range(partition, units.size());
      for (auto index = range.begin; index < range.end; ++index) {
        const auto current_unit = world.unit_state(units[index].entity_id);
        if (!current_unit) throw std::runtime_error("AI world view identity is not live");
        const tactical::UnitState& unit = *current_unit;
        ViewUnit entry;
        entry.id = unit.entity_id;
        entry.type = unit.type_id;
        entry.purchase_type = tactical::purchase_identity(unit);
        entry.purchase_token = unit.purchase_token;
        entry.owner = unit.owner;
        entry.position = unit.position;
        entry.rotation = unit.rotation;
        entry.order = unit.order.kind;
        entry.order_target = unit.order.target;
        const auto squadron = std::lower_bound(squadrons.begin(), squadrons.end(), unit.entity_id,
            [](const tactical::Squadron& row, sim::EntityId id) { return row.container < id; });
        if (squadron != squadrons.end() && squadron->container == unit.entity_id) {
            entry.container = true;
            entry.members = squadron->members;
        }
        const auto member = std::lower_bound(craft.begin(), craft.end(), unit.entity_id,
            [](const Member& row, sim::EntityId id) { return row.first < id; });
        if (member != craft.end() && member->first == unit.entity_id) {
            entry.craft = true;
            entry.squadron = squadrons[member->second].container;
        }
        const auto instance = std::lower_bound(instances.begin(), instances.end(), unit.entity_id,
            [](const tactical::TacticalInstance& row, sim::EntityId id) { return row.entity_id < id; });
        if (instance != instances.end() && instance->entity_id == unit.entity_id) {
            entry.team = instance->team;
            entry.visible_to = instance->visible_to;
            entry.abilities = instance->abilities;
            entry.planetary_weapon = instance->planetary_weapon;
            if (const auto& durability = instance->durability) {
                entry.hull = durability->hull;
                entry.max_hull = durability->max_hull;
                entry.health = fraction(durability->hull, durability->max_hull);
                entry.max_shields = durability->max_shields;
                if (durability->shields && durability->max_shields) {
                    entry.shield = fraction(*durability->shields, *durability->max_shields);
                }
                entry.engines_online = durability->engines_online;
                for (const auto& hardpoint : durability->hardpoints) {
                    entry.hardpoint_destroyed.push_back(hardpoint.state == tactical::HardpointState::destroyed);
                }
                const auto* profile = world.durability().find(unit.type_id);
                if (profile != nullptr && profile->damage_tracking) {
                    if (const auto state = world.durability_state(unit.entity_id)) {
                        entry.damage_rate = state->damage_rate;
                        entry.time_till_dead = tactical::time_to_death(*profile, *state);
                    }
                }
            }
        }
        if (const auto combat = world.combat_state(unit.entity_id)) entry.attack_target = combat->attack_target;
        if (const auto motion = world.motion_state(unit.entity_id)) entry.moving = motion->kind != tactical::MotionKind::none;
        // FH-23: a squadron (and its craft) reports the formation target before a weapon
        // target. Its container travels through the formation, without a ship motion state.
        const auto formation_owner = entry.craft ? squadrons.begin() + static_cast<std::ptrdiff_t>(member->second) : squadron;
        if (formation_owner != squadrons.end() && formation_owner->container == (entry.craft ? entry.squadron : entry.id)) {
            const auto& formation = formations[static_cast<std::size_t>(formation_owner - squadrons.begin())];
            if (formation) {
                entry.formation_target = formation->first;
                entry.formation_moving = formation->second;
            }
        }
        view->units[index] = std::move(entry);
        view->capture_candidates[index] = {{unit.entity_id, unit.owner, unit.position}, unit.type_id,
            !world.arrivals().contains(unit.entity_id)};
      }
    });
    if (!prepared) throw std::runtime_error("AI world view preparation failed: " + prepared.error().message);
    return view;
}

} // namespace eawr::script::foc::detail

namespace eawr::script::foc::ai {

class TacticalWorld final : public World {
public:
    explicit TacticalWorld(const Host& host) : host_(host) {}
    struct Evaluator;
    std::shared_ptr<const WorldView> view() const override { return host_.view; }
    bool available() const noexcept override { return host_.world != nullptr; }
    std::uint64_t completed_tick() const noexcept override { return host_.view ? host_.view->tick : 0; }
    std::optional<Real> perception(const Engine&, const Equation&, const Context&, AiRandom&) const override;
    Context goal_context(const PlayerAi&, const Target*) const override;
    bool target_matches(tactical::PlayerId, const std::set<std::string>&, const Target*) const override;
    std::vector<Target> targets(const Engine&, const PlayerAi&) const override;
    std::vector<Target> new_targets(const Engine& engine) const override {
        std::vector<Target> result;
        for (const auto& unit : host_.view->units) {
            const auto* type = host_.type(unit.type);
            if (unit.planetary_weapon || type == nullptr || !type->space_evaluator || engine.target_of_object(unit.id) != nullptr) continue;
            Target target; target.object = unit.id; target.name = "OBJECT_" + std::to_string(unit.id);
            result.push_back(std::move(target));
        }
        return result;
    }
    bool target_alive(const Target& target) const override {
        return target.object == 0 || host_.view->find(target.object) != nullptr;
    }
    std::string_view goal_mode() const noexcept override { return "SPACE"; }
    std::span<const tactical::Player> players() const noexcept override {
        return host_.world ? host_.world->players() : std::span<const tactical::Player>{};
    }
    const tactical::EconomyRules& economy() const noexcept override {
        static const tactical::EconomyRules empty;
        return host_.world ? host_.world->economy() : empty;
    }
    const tactical::EconomyView* account(tactical::PlayerId id) const override {
        if (host_.snapshot) for (const auto& entry : host_.snapshot->economy()) if (entry.player == id) return &entry;
        return nullptr;
    }
    const std::map<sim::EntityId, tactical::PadState>& pads() const noexcept override {
        static const std::map<sim::EntityId, tactical::PadState> empty;
        return host_.world ? host_.world->pads() : empty;
    }
    bool arriving(sim::EntityId id) const override { return host_.world && host_.world->arrivals().contains(id); }
    bool build_allowed(tactical::PlayerId player, sim::EntityId producer, tactical::TypeId type) const override {
        return host_.world && host_.world->build_allowed(player, producer, type);
    }
    const tactical::BuildOption* build_option(tactical::PlayerId player, sim::EntityId producer, tactical::TypeId type) const override {
        return host_.world ? host_.world->build_option(player, producer, type) : nullptr;
    }
    tactical::ProductionCounts production_counts(tactical::PlayerId player, tactical::TypeId type) const override {
        return host_.world ? host_.world->production_counts(player, type) : tactical::ProductionCounts{};
    }
    bool construction_allowed(tactical::PlayerId player, sim::EntityId id) const override {
        const auto state = pads().find(id);
        const auto* unit = host_.view ? host_.view->find(id) : nullptr;
        const auto* profile = unit ? economy().pads.point(unit->type) : nullptr;
        return profile && state != pads().end() && tactical::pad_construction_allowed(*profile, state->second,
            player, players(), host_.view->capture_candidates, economy().pads, id, unit->position);
    }
    bool constructing_for(sim::EntityId id, tactical::PlayerId player) const override {
        if (!host_.world) return false;
        const auto found = host_.world->construction().find(id);
        return found != host_.world->construction().end() && found->second.builder == player;
    }
    sim::EntityId next_entity_id() const noexcept override { return host_.world ? host_.world->next_entity_id() : 0; }
    std::optional<ReinforcementResult> reinforcement_search_result(tactical::PlayerId player, std::uint64_t token) const override {
        if (host_.world) if (const auto result = host_.world->reinforcement_search_result(player, token))
            return ReinforcementResult{result->next_attempt, result->candidates, result->nanoseconds};
        return std::nullopt;
    }
    std::span<const tactical::Event> events() const noexcept override {
        return host_.snapshot ? host_.snapshot->events() : std::span<const tactical::Event>{};
    }
    std::span<const tactical::CombatEvent> combat_events() const noexcept override {
        return host_.snapshot ? host_.snapshot->combat_events() : std::span<const tactical::CombatEvent>{};
    }
private:
    static std::int64_t ceiling(Real value) {
        const auto whole = truncate(value);
        return real(whole) < value ? whole + 1 : whole;
    }
    static Real clamp_to(Real value, Real low, Real high) {
        return value < low ? low : (high < value ? high : value);
    }
    const Host& host_;
};

inline Context TacticalWorld::goal_context(const PlayerAi& player, const Target* target) const {
    Context context;
    context.player = player.player;
    context.target = target;
    for (const AiPlayer& other : host_.setup.players) {
        if (!context.enemy && !other.neutral && !host_.allied(other.player, player.player)) context.enemy = other.player;
        if (!context.human && !other.ai && !other.neutral) context.human = other.player;
    }
    return context;
}

inline bool TacticalWorld::target_matches(tactical::PlayerId player, const std::set<std::string>& flags, const Target* target) const {
    if (target == nullptr) return false;
    if (target->object == 0) return flags.contains("TACTICAL_LOCATION");
    const ViewUnit* unit = host_.view->find(target->object);
    if (unit == nullptr) return false;
    const AiType* type = host_.type(unit->type);
    // WNO-13: either authored persistent-state flag bypasses this forced object-fog gate.
    if (!(type != nullptr && (type->initial_state_visible_under_fow || type->last_state_visible_under_fow)) &&
        detail::object_fogged(*host_.view, *unit, player)) return false;
    const bool allied = unit->owner != 0 && host_.players.contains(unit->owner) && !host_.neutral(unit->owner) &&
        host_.allied(unit->owner, player);
    // GS-11: capture ownership alone does not make a target a build pad.
    if (type != nullptr && type->build_pad)
        return flags.contains(allied ? "FRIENDLY_BUILD_PAD" : "ENEMY_BUILD_PAD");
    const bool structure = type != nullptr && (type->star_base || (type->capture_point && !type->locomotor));
    const std::string flag = std::string(allied ? "FRIENDLY_" : "ENEMY_") + (structure ? "STRUCTURE" : "UNIT");
    return flags.contains(flag);
}

inline std::vector<Target> TacticalWorld::targets(const Engine& engine, const PlayerAi& player) const {
    static_cast<void>(player);
    std::vector<Target> result;
    for (const ViewUnit& unit : host_.view->units) {
        const AiType* type = host_.type(unit.type);
        if (unit.planetary_weapon || type == nullptr || !type->space_evaluator) continue;
        Target target;
        target.object = unit.id;
        target.name = "OBJECT_" + std::to_string(unit.id);
        result.push_back(std::move(target));
    }
    if (!host_.setup.bounds) return result;
    const AiBounds& bounds = *host_.setup.bounds;
    const Real size = engine.data().constants.region_size;
    const std::int64_t columns = ceiling(to_single((bounds.right - bounds.left) / size));
    const std::int64_t rows = ceiling(to_single((bounds.top - bounds.bottom) / size));
    for (std::int64_t column = 0; column < columns; ++column) {
        for (std::int64_t row = 0; row < rows; ++row) {
            const std::string name = "CELL_" + std::to_string(column) + "_" + std::to_string(row);
            Target target;
            target.cell_x = static_cast<std::int32_t>(column);
            target.cell_y = static_cast<std::int32_t>(row);
            target.region.x = to_single(bounds.left + real(column) * size);
            target.region.y = to_single(bounds.bottom + real(row) * size);
            target.region.width = clamp_to(size, Real{}, to_single(bounds.right - target.region.x));
            target.region.height = clamp_to(size, Real{}, to_single(bounds.top - target.region.y));
            target.x = to_single(target.region.x + size / real(2));
            target.y = to_single(target.region.y + size / real(2));
            target.name = name;
            result.push_back(std::move(target));
        }
    }
    return result;
}

} // namespace eawr::script::foc::ai
