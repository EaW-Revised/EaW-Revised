#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_actions.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace eawr::script::foc::ai {
namespace {
using Bytes = std::vector<std::uint8_t>;
constexpr std::string_view domain = "eawr-galactic-goals-v4";
constexpr std::uint64_t max_rows = 262144;
template<class T> core::Result<T> fail(std::string message) {
    return core::Result<T>::failure(detail::make_error(authoritative::codes::load_rejected, std::move(message)));
}
// One field inventory drives encoding and decoding. No padding, pointer bytes,
// native floating point, implicit map insertion or unordered iteration.
struct Archive {
    bool reading{};
    Bytes bytes;
    std::span<const std::uint8_t> input;
    std::size_t depth{};
    std::uint64_t nodes{};
    explicit Archive(std::span<const std::uint8_t> source) : reading(true), input(source) {}
    Archive() = default;
    void check(bool ok) const { if (!ok) throw std::runtime_error("invalid galactic goal state encoding"); }
    void word(std::uint64_t& value, std::size_t width) {
        if (reading) {
            check(width <= input.size()); value = 0;
            for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{input[i]} << (i * 8U);
            input = input.subspan(width);
        } else {
            check(bytes.size() + width <= galactic_ai::native_state_max_bytes);
            for (std::size_t i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
        }
    }
    template<class... T> void fields(T&... values) { (item(values), ...); }
    template<class T> requires (std::is_integral_v<T> && !std::is_same_v<T, bool>) void item(T& value) {
        using U = std::make_unsigned_t<T>;
        std::uint64_t bits = static_cast<U>(value); word(bits, sizeof(T));
        if (reading) {
            if constexpr (std::is_signed_v<T>) value = std::bit_cast<T>(static_cast<U>(bits));
            else value = static_cast<T>(bits);
        }
    }
    template<class T> requires std::is_enum_v<T> void item(T& value) {
        auto bits = static_cast<std::underlying_type_t<T>>(value); item(bits);
        if (reading) value = static_cast<T>(bits);
    }
    void item(bool& value) { std::uint8_t bits = value ? 1 : 0; item(bits); check(bits <= 1); if (reading) value = bits != 0; }
    void item(Real& value) { item(value.repr); }
    void item(math::Fixed& value) { auto raw = value.raw(); item(raw); if (reading) value = math::Fixed::from_raw(raw); }
    void item(std::string& value) {
        std::uint64_t length = value.size(); item(length); check(length <= 1048576);
        if (reading) { check(length <= input.size()); value.assign(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(length)); input = input.subspan(static_cast<std::size_t>(length)); }
        else { check(bytes.size() + length <= galactic_ai::native_state_max_bytes); bytes.insert(bytes.end(), value.begin(), value.end()); }
    }
    std::uint64_t count(std::uint64_t value) {
        item(value); check(value <= max_rows && nodes + value <= 1048576); nodes += value;
        if (reading) check(value <= input.size());
        return value;
    }
    template<class T> void item(std::vector<T>& value) {
        const auto size = count(value.size()); if (reading) value.resize(static_cast<std::size_t>(size));
        for (auto& row : value) item(row);
    }
    template<class T> void item(std::optional<T>& value) {
        bool present = value.has_value(); item(present);
        if (reading) { value.reset(); if (present) value.emplace(); }
        if (present) item(*value);
    }
    template<class T, class C> void item(std::set<T, C>& value) {
        const auto size = count(value.size());
        if (!reading) { for (const auto& row : value) { auto copied = row; item(copied); } return; }
        value.clear(); std::optional<T> previous;
        for (std::uint64_t i = 0; i < size; ++i) { T row{}; item(row); check(!previous || C{}(*previous, row)); previous = row; value.insert(std::move(row)); }
    }
    template<class A, class B> void item(std::pair<A, B>& value) { fields(value.first, value.second); }
    template<class K, class V, class C> void item(std::map<K, V, C>& value) {
        const auto size = count(value.size());
        if (!reading) { for (auto& [key, row] : value) { auto copied = key; fields(copied, row); } return; }
        value.clear(); std::optional<K> previous;
        for (std::uint64_t i = 0; i < size; ++i) {
            K key{}; V row{}; fields(key, row); check(!previous || C{}(*previous, key));
            previous = key; check(value.emplace(std::move(key), std::move(row)).second);
        }
    }
    template<class T> void item(galactic_ai::Setting<T>& value) { fields(value.value, value.priority); }
    void item(galactic_ai::TemplatePolicy& value) { fields(value.equations, value.allowances, value.goal_categories, value.goal_types, value.plan_categories, value.plan_types, value.plan_names); }
    void item(math::Vec3& v) { fields(v.x, v.y, v.z); }
    void item(Rect& v) { fields(v.x, v.y, v.width, v.height); }
    void item(Target& v) { fields(v.id, v.object, v.cell_x, v.cell_y, v.region, v.x, v.y, v.name); }
    void item(Contrast& v) { fields(v.category, v.force); }
    void item(PotentialPlan& v) { fields(v.id, v.plan, v.valid, v.reserved, v.funded, v.units, v.freestore, v.sources, v.producers, v.pool_tokens, v.cost, v.taskforce_of_unit, v.team_of_unit, v.target_contrast, v.current_contrast, v.threshold, v.target_contrast_disabled); }
    void item(Goal& v) { fields(v.id, v.function, v.target, v.desire, v.active, v.finished, v.potential, v.plan, v.exclusions_registered); }
    void item(History& v) { item(v.entries); }
    void item(Difficulty& v) { fields(v.space_contrast_multiplier, v.space_goal_cycle_sleep, v.galactic_contrast_multiplier, v.galactic_build_time_multiplier, v.galactic_goal_cycle_sleep, v.credit_multiplier); }
    void item(TeamDef& v) { fields(v.min_count, v.max_count, v.percentage, v.percentage_based, v.types); }
    void item(TaskForceDef& v) { fields(v.name, v.teams, v.escort, v.required, v.minimum_force, v.minimum_size); }
    void item(PlanDef& v) { fields(v.name, v.module, v.goals, v.taskforces, v.ignore_target, v.magic, v.allow_free_store, v.allow_special_weapons, v.allow_engaged, v.per_failure_contrast_adjust, v.min_contrast, v.max_contrast, v.required_categories); }
    void item(GoalType& v) { fields(v.name, v.application, v.mode, v.category, v.reachability, v.time_limit, v.build_time_delay_tolerance, v.tracking_duration, v.per_failure_desire_adjust, v.activation_tracking_duration, v.per_activation_failure_desire_adjust, v.is_like, v.global_exclusions); }
    void item(PlayerType& v) { fields(v.name, v.function_sets, v.space_templates, v.galactic_templates, v.freestore, v.difficulty); }
    void item(Template& v) { fields(v.name, v.priority, v.trigger, v.untrigger, v.budget, v.tactical_allowances,
        v.goals_on, v.goals_off, v.plans_on, v.plans_off, v.goal_types_on, v.goal_types_off,
        v.plan_types_on, v.plan_types_off, v.plan_names_on, v.plan_names_off); }
    void item(Constants& v) { fields(v.region_size, v.fog_cell_size, v.fog_cells_per_threat_cell, v.threat_look_ahead,
        v.threat_range_cap, v.area_threat_scale, v.threat_decay_step, v.low_threat_tolerance,
        v.medium_threat_tolerance, v.high_threat_tolerance, v.health_low_threshold); }
    void item(ThreatEntry& v) { fields(v.weapon, v.power, v.x, v.y, v.radius, v.x0, v.x1, v.y0, v.y1); }
    void item(AiWeapon& v) { fields(v.hardpoint, v.combat_power, v.range, v.targetable, v.destroyable); }
    void item(AiType& v) { fields(v.type_id, v.name, v.category_bits, v.property_bits, v.abilities,
        v.projectile_types, v.hardpoint_projectiles, v.object_projectile, v.point_defence_script,
        v.corrupt_systems_script, v.locomotor, v.star_base, v.space_special_weapon, v.targeting, v.space_evaluator,
        v.tech_level, v.base_level, v.combat_power, v.max_attack_distance, v.max_speed, v.soft_radius,
        v.reveal_range, v.weapons, v.weapon_damage, v.squadron, v.craft, v.squadron_units,
        v.squadron_unit, v.capture_point, v.build_pad, v.initial_state_visible_under_fow,
        v.last_state_visible_under_fow, v.tactical_cost, v.unit_ai, v.previous_base_levels); }
    void item(GoalFunction& v) { fields(v.goal, v.function); }
    void item(Node& v) { fields(v.kind, v.value, v.text, v.lookup); }
    void item(Lookup& v) { fields(v.tokens, v.parameters); }
    void item(Equation& v) { fields(v.name, v.program, v.lookups); }
    void item(BuildTask& v) { fields(v.taskforce, v.block, v.type, v.object, v.source, v.producer, v.completion, v.issued, v.child_floor, v.pool_token, v.pad_build, v.direct_pad, v.prepaid, v.acknowledged, v.generic_cost, v.finished, v.failed, v.staging, v.associated_producer, v.state_entered, v.salvage, v.allocated, v.joined, v.synchronized_staging); }
    void item(Block& v) { fields(v.kind, v.id, v.instance, v.taskforce, v.order, v.attack, v.destination_object, v.destination, v.movers, v.ordered_tick, v.side, v.distance, v.tolerance, v.last_update, v.ready, v.finished, v.result, v.reinforcing, v.entity_floor, v.issued, v.reinforcement_attempt, v.pool_token, v.waiting_on, v.galactic_request, v.galactic_actor, v.held_owner); }
    void item(TaskForce::Purchase& v) { fields(v.type, v.token); }
    void item(TaskForce& v) { fields(v.id, v.plan, v.definition, v.name, v.units, v.types, v.sources, v.producers, v.pooled, v.pool_tokens, v.thread, v.had_units, v.damaged_pending, v.last_target, v.galactic_stage, v.galactic_fleet); }
    void item(Plan& v) { fields(v.id, v.instance, v.definition, v.goal, v.player, v.target, v.target_object, v.target_destroyed_signalled, v.taskforces, v.result, v.removable, v.exited, v.purge, v.start_tick, v.requires_production, v.reserved_pads); }
    void item(TickCost& v) { fields(v.tick, v.freestore, v.plans, v.plan_instances, v.freestore_runs, v.goals_evaluated, v.maintenances, v.plans_attached, v.plans_deferred, v.plans_pumped, v.reinforcement_candidates, v.reinforcement_search_ns); }
    void item(PendingAttach& v) { fields(v.player, v.goal); }
    void item(authoritative::Handle& v) { fields(v.kind, v.id); }
    void item(authoritative::Value& v) {
        check(depth < static_cast<std::size_t>(authoritative::max_value_list_depth)); ++depth;
        auto tag = static_cast<std::uint8_t>(v.data.index()); item(tag); check(tag <= 5);
        if (reading) switch (tag) {
        case 0: v.data = std::monostate{}; break;
        case 1: v.data = false; break;
        case 2: v.data = Real{}; break;
        case 3: v.data = std::string{}; break;
        case 4: v.data = authoritative::Handle{}; break;
        case 5: v.data = authoritative::ValueList{}; break;
        }
        std::visit([&](auto& row) { using T = std::decay_t<decltype(row)>; if constexpr (!std::is_same_v<T, std::monostate>) item(row); }, v.data);
        --depth;
    }
    void item(authoritative::EventKey& v) { fields(v.tick, v.producer, v.entity, v.sequence); }
    void item(authoritative::ScriptEvent& v) { fields(v.key, v.target, v.kind, v.name, v.thread, v.arguments, v.parameter); check(v.kind <= authoritative::ScriptEvent::Kind::start_thread); }
    void item(authoritative::ScriptCommand& v) { fields(v.tick, v.issuer, v.sequence, v.verb, v.arguments); }
};
bool digest(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
}

struct GoalStateCodec {
    static std::string configuration(Engine& engine) {
        if (!engine.native_configuration_.empty()) return engine.native_configuration_;
        Archive ar;
        ar.fields(engine.host_->setup.seed, engine.plans_, engine.data_.goals, engine.data_.players,
            engine.data_.difficulties, engine.data_.function_sets, engine.data_.goal_categories,
            engine.data_.templates, engine.data_.constants);
        for (auto& setup : engine.host_->setup.players) ar.fields(setup.player, setup.faction, setup.neutral, setup.ai, setup.player_type, setup.human);
        // Parsed function code is part of the native configuration too. The
        // required external closure digest covers all transitive XML/Lua/type inputs.
        for (const auto& [name, functions] : engine.data_.function_sets) {
            static_cast<void>(name);
            for (const auto& function : functions) if (const auto* found = engine.data_.equations.find(function.function)) { auto equation = *found; ar.item(equation); }
        }
        engine.native_configuration_ = core::sha256_hex(ar.bytes);
        return engine.native_configuration_;
    }
    static void player(Archive& ar, PlayerAi& v, const AiData& data) {
        std::string type;
        if (!ar.reading) for (const auto& [name, row] : data.players) if (&row == v.type) type = name;
        ar.fields(v.player, type); ar.check(!type.empty() && data.players.contains(type));
        if (ar.reading) v.type = &data.players.at(type);
        std::string selected;
        if (!ar.reading) for (const auto& [name, row] : data.templates) if (&row == v.space_template) selected = name;
        ar.item(selected); ar.check(selected.empty() || data.templates.contains(selected));
        if (ar.reading) v.space_template = selected.empty() ? nullptr : &data.templates.at(selected);
        ar.fields(v.difficulty, v.galactic, v.galactic_policy, v.proposal_counts, v.exclusion_counts);
        const auto functions = ar.count(v.functions.size()); if (ar.reading) v.functions.resize(static_cast<std::size_t>(functions));
        for (auto& f : v.functions) {
            ar.fields(f.goal_name, f.equation_name);
            ar.check(data.goals.contains(f.goal_name) && data.equations.find(f.equation_name));
            if (ar.reading) { f.goal = &data.goals.at(f.goal_name); f.equation = data.equations.find(f.equation_name); }
        }
        ar.fields(v.targets, v.next_goal, v.next_planning, v.next_execution, v.next_learning, v.phase,
            v.sleep_frames, v.per_frame, v.nontrivial, v.next_function, v.next_target, v.maintenance_due,
            v.proposed, v.active, v.proposal_blocks, v.activations, v.goal_outcomes, v.plan_outcomes,
            v.plan_success, v.goal_success, v.target_goal_success, v.target_plan_success, v.unit_survival, v.reserved, v.assigned, v.reserved_pool, v.reserved_pool_tokens, v.reserved_credits,
            v.tasks, v.next_goal_id);
    }
    static void runtime(Archive& ar, Engine& v, std::map<tactical::PlayerId, math::Fixed>& credits) {
        ar.fields(v.initialized_, v.game_seed_, v.frame_, v.event_tick_, v.sequence_, v.order_sequence_,
            v.next_target_id_, v.next_taskforce_id_, v.next_plan_id_, v.next_instance_);
        auto seed = v.sync_.seed(); ar.item(seed); if (ar.reading) v.sync_.set_seed(seed);
        const auto players = ar.count(v.players_.size()); if (ar.reading) v.players_.resize(static_cast<std::size_t>(players));
        for (auto& p : v.players_) player(ar, p, v.data_);
        ar.fields(v.targets_, v.taskforces_, v.running_, v.blocks_, v.plan_of_instance_, v.threats_,
            v.last_hits_, v.globals_, v.pending_attach_, v.events_, v.orders_, credits);
        // Timing/work diagnostics do not drive continuation and must not leak
        // wall-clock nanoseconds into the canonical simulation digest.
        const auto pads = ar.count(v.pad_reservations_.size());
        if (ar.reading) v.pad_reservations_.clear();
        if (!ar.reading) for (auto& [id, row] : v.pad_reservations_) { auto copied = id; ar.fields(copied, row.player, row.goal, row.type, row.consumed, row.issued); }
        else {
            sim::EntityId previous{};
            for (std::uint64_t i = 0; i < pads; ++i) {
                sim::EntityId id{}; Engine::PadReservation row;
                ar.fields(id, row.player, row.goal, row.type, row.consumed, row.issued);
                ar.check(id > previous); previous = id; v.pad_reservations_.emplace(id, row);
            }
        }
    }
    static void publish(Engine& destination, Engine& staged) {
        // Staged pointers name a private copied AiData. Rebind against the
        // live immutable closure before the single ordered owner publication.
        for (auto& p : staged.players_) {
            for (const auto& [name, row] : staged.data_.players) if (&row == p.type) { p.type = &destination.data_.players.at(name); break; }
            for (const auto& [name, row] : staged.data_.templates) if (&row == p.space_template) { p.space_template = &destination.data_.templates.at(name); break; }
            for (auto& f : p.functions) { f.goal = &destination.data_.goals.at(f.goal_name); f.equation = destination.data_.equations.find(f.equation_name); }
            if (auto* old = destination.player_ai(p.player)) p.galactic_funding = old->galactic_funding;
        }
        destination.players_ = std::move(staged.players_); destination.targets_ = std::move(staged.targets_);
        destination.taskforces_ = std::move(staged.taskforces_); destination.running_ = std::move(staged.running_);
        destination.blocks_ = std::move(staged.blocks_); destination.plan_of_instance_ = std::move(staged.plan_of_instance_);
        destination.pad_reservations_ = std::move(staged.pad_reservations_); destination.threats_ = std::move(staged.threats_);
        destination.last_hits_ = std::move(staged.last_hits_); destination.globals_ = std::move(staged.globals_);
        destination.pending_attach_ = std::move(staged.pending_attach_); destination.events_ = std::move(staged.events_);
        destination.orders_ = std::move(staged.orders_); destination.sync_ = staged.sync_;
        destination.game_seed_ = staged.game_seed_; destination.frame_ = staged.frame_; destination.event_tick_ = staged.event_tick_;
        destination.sequence_ = staged.sequence_; destination.order_sequence_ = staged.order_sequence_;
        destination.next_target_id_ = staged.next_target_id_; destination.next_taskforce_id_ = staged.next_taskforce_id_;
        destination.next_plan_id_ = staged.next_plan_id_; destination.next_instance_ = staged.next_instance_;
        destination.initialized_ = staged.initialized_; destination.work_ = {};
        destination.staggered_clock_ = staged.staggered_clock_;
        destination.staggered_seeded_ = staged.staggered_seeded_;
        destination.records_.clear(); destination.costs_.clear();
        destination.producers_.clear(); destination.producers_prepared_ = false; destination.producer_work_ = {};
        destination.mark_galactic_tasks_changed();
        destination.perception_executor_ = nullptr; destination.scripts_ = nullptr;
    }
    static void grid(Archive& ar, ThreatGrid& g) {
        ar.fields(g.left_, g.top_, g.right_, g.bottom_, g.cell_width_, g.cell_height_, g.min_radius_,
            g.x_cells_, g.y_cells_, g.constants_, g.cells_);
        const auto size = ar.count(g.tracked_.size());
        if (!ar.reading) for (auto& [id, row] : g.tracked_) {
            auto copied = id; ar.fields(copied, row.entries, row.next_service, row.last_position, row.has_position);
        } else {
            g.tracked_.clear(); sim::EntityId previous{};
            for (std::uint64_t i = 0; i < size; ++i) {
                sim::EntityId id{}; ThreatGrid::Tracked row;
                ar.fields(id, row.entries, row.next_service, row.last_position, row.has_position);
                ar.check(id > previous); previous = id; g.tracked_.emplace(id, std::move(row));
            }
        }
        ar.check(g.x_cells_ >= 0 && g.y_cells_ >= 0 && std::uint64_t(g.x_cells_) * std::uint64_t(g.y_cells_) == g.cells_.size());
        ar.check(g.x_cells_ == 0 || (Real{} < g.cell_width_ && Real{} < g.cell_height_));
        for (const auto& cell : g.cells_) for (const auto& [id, index] : cell)
            ar.check(g.tracked_.contains(id) && index < g.tracked_.at(id).entries.size());
        if (ar.reading) g.clear_queries();
    }
};

core::Result<void> GoalStateContributor::validate(const Engine& e, const detail::WorldView* completed_view) const {
    // A restore checks references against the rebuilt world's completed view, which the host
    // only holds after validation succeeded; a rebuilt host still has its pre-publication view.
    const detail::WorldView* const view = completed_view ? completed_view : e.host_ ? e.host_->view.get() : nullptr;
    const auto check = [](bool ok) { if (!ok) throw std::runtime_error("galactic goal state has invalid identity/reference"); };
    try {
        check(e.host_ && e.host_->ai_world && digest(content_identity_));
        const bool galactic = e.host_->ai_world->goal_mode() == "GALACTIC";
        check(galactic || e.host_->ai_world->goal_mode() == "SPACE");
        check(e.frame_ >= 0 && e.next_plan_id_ && e.next_target_id_ && e.next_taskforce_id_ && e.next_instance_ >= first_plan_instance);
        check(e.game_seed_ == static_cast<std::uint32_t>(e.host_->setup.seed) || !e.initialized_);
        check(e.initialized_ || (e.players_.empty() && e.running_.empty() && e.targets_.empty() && e.taskforces_.empty() && e.blocks_.empty()));
        if (e.initialized_) check(e.players_.size() == static_cast<std::size_t>(std::count_if(e.host_->setup.players.begin(), e.host_->setup.players.end(),
            [&](const auto& player) { return player.ai && (!galactic || !player.neutral) && !player.player_type.empty()
                && e.data_.players.contains(upper_case(player.player_type)); })));
        std::set<std::uint64_t> plan_ids;
        tactical::PlayerId previous{};
        for (const auto& p : e.players_) {
            check(p.player > previous && p.galactic == galactic && (!galactic || !p.space_template) && p.type && p.next_goal_id && p.per_frame >= 1
                && p.sleep_frames >= 0 && p.nontrivial >= 0 && p.next_function <= p.functions.size() && p.next_target <= p.targets.size());
            previous = p.player;
            const auto setup = e.host_->players.find(p.player); check(setup != e.host_->players.end());
            check(upper_case(setup->second->player_type) == upper_case(p.type->name));
            if (!galactic) {
                const auto selected = p.type->space_templates.empty() ? e.data_.templates.end()
                    : e.data_.templates.find(p.type->space_templates.front());
                check(p.space_template == (selected == e.data_.templates.end() ? nullptr : &selected->second));
            }
            std::vector<std::pair<std::string, std::string>> functions;
            for (const auto& name : p.type->function_sets) if (const auto rows = e.data_.function_sets.find(name); rows != e.data_.function_sets.end())
                for (const auto& f : rows->second) if (const auto goal = e.data_.goals.find(f.goal); goal != e.data_.goals.end() && goal->second.mode == e.host_->ai_world->goal_mode()) functions.emplace_back(f.goal, f.function);
            check(functions.size() == p.functions.size());
            for (std::size_t i = 0; i < functions.size(); ++i) check(functions[i] == std::pair{p.functions[i].goal_name, p.functions[i].equation_name});
            std::set<std::uint64_t> goals, listed_targets;
            for (auto id : p.targets) check(e.targets_.contains(id) && listed_targets.insert(id).second);
            for (const auto* rows : {&p.active, &p.proposed}) for (const auto& g : *rows) {
                check(g.id && g.id < p.next_goal_id && goals.insert(g.id).second && g.function < p.functions.size());
                check(g.target == 0 || e.targets_.contains(g.target));
                if (g.potential) {
                    const auto& potential = *g.potential;
                    check((!galactic || (potential.id && potential.id < e.next_plan_id_ && plan_ids.insert(potential.id).second)) && potential.plan < e.plans_.size());
                    const auto n = potential.units.size();
                    check(potential.freestore.size() == n && potential.sources.size() == n && potential.producers.size() == n
                        && potential.pool_tokens.size() == n && potential.taskforce_of_unit.size() == n && potential.team_of_unit.size() == n);
                    for (auto source : potential.sources) check(source <= 2);
                    if (!galactic) for (const auto type : potential.units) check(e.host_->types.contains(type));
                    const auto& definition = e.plans_[potential.plan];
                    check(std::find(definition.goals.begin(), definition.goals.end(), p.functions[g.function].goal_name) != definition.goals.end());
                    for (std::size_t i = 0; i < n; ++i) check(potential.taskforce_of_unit[i] < definition.taskforces.size()
                        && potential.team_of_unit[i] < definition.taskforces[potential.taskforce_of_unit[i]].teams.size());
                }
                if (g.plan) { const auto found = e.running_.find(g.plan); check((g.finished && found == e.running_.end())
                    || (found != e.running_.end() && found->second.goal == g.id && found->second.player == p.player
                        && found->second.target == g.target && (!g.potential || found->second.definition == g.potential->plan))); }
            }
            for (const auto& [object, goal] : p.reserved) check(object && goals.contains(goal)); // pending loss/cancel can lack a live actor
            for (const auto& [object, tf] : p.assigned) check(object && e.taskforces_.contains(tf) && e.running_.at(e.taskforces_.at(tf).plan).player == p.player);
            for (const auto& [token, goal] : p.reserved_pool_tokens) check(token && goals.contains(goal));
            for (const auto& [goal, amount] : p.reserved_credits) { static_cast<void>(amount); check(goals.contains(goal)); }
            for (const auto& task : p.tasks) {
                check(task.source <= 2 && e.taskforces_.contains(task.taskforce) && (task.block == 0 || e.blocks_.contains(task.block)));
                if (galactic) {
                    const auto* world = dynamic_cast<const GalacticWorld*>(e.host_->ai_world.get()); check(world);
                    check(!task.associated_producer || (task.source == 2 && task.completion && world->containing_planet(task.associated_producer) == task.associated_producer));
                    check(!task.staging || world->containing_planet(task.staging) == task.staging);
                    check(task.state_entered <= static_cast<std::uint64_t>(e.frame_));
                    check(!task.joined || (task.finished && !task.failed && task.object));
                }
            }
            for (const auto& [key, history] : p.plan_outcomes) { static_cast<void>(history); check(key.first < e.plans_.size() && (!key.second || e.targets_.contains(key.second))); }
            for (const auto& [key, value] : p.plan_success) check(key < e.plans_.size() && value.first >= 0 && value.first <= value.second);
            const auto valid_counts = [&](const auto& value) { check(value.first >= 0 && value.first <= value.second); };
            const auto valid_goal = [&](const auto& key) { check(e.data_.goals.contains(key.first) && (!key.second || e.targets_.contains(key.second))); };
            for (const auto* table : {&p.activations, &p.goal_outcomes}) for (const auto& [key, history] : *table) {
                valid_goal(key); static_cast<void>(history);
            }
            for (const auto& [key, counts] : p.goal_success) { check(e.data_.goals.contains(key)); valid_counts(counts); }
            for (const auto& [key, counts] : p.target_goal_success) { valid_goal(key); valid_counts(counts); }
            for (const auto& [key, counts] : p.target_plan_success) {
                check(key.first < e.plans_.size() && (!key.second || e.targets_.contains(key.second))); valid_counts(counts);
            }
            for (const auto& [key, counts] : p.unit_survival) {
                check(!key.empty() && std::any_of(e.host_->types.begin(), e.host_->types.end(),
                    [&](const auto& type) { return type.second && type.second->name == key; })); valid_counts(counts);
            }
        }
        for (const auto& [id, t] : e.targets_) check(id && id == t.id && id < e.next_target_id_);
        for (const auto& [id, p] : e.running_) {
            check(id && id == p.id && id < e.next_plan_id_ && plan_ids.insert(id).second && p.definition < e.plans_.size()
                && p.instance >= first_plan_instance && p.instance < e.next_instance_ && e.player_ai(p.player));
            const auto* owner = e.player_ai(p.player);
            const auto goal = std::find_if(owner->active.begin(), owner->active.end(), [&](const auto& g) { return g.id == p.goal; });
            check(goal != owner->active.end() && goal->plan == id && goal->target == p.target
                && (!goal->potential || goal->potential->plan == p.definition));
            check(e.plan_of_instance_.contains(p.instance) && e.plan_of_instance_.at(p.instance) == id);
            for (const auto tf : p.taskforces) check(e.taskforces_.contains(tf) && e.taskforces_.at(tf).plan == id);
        }
        check(e.plan_of_instance_.size() == e.running_.size());
        for (const auto& [id, tf] : e.taskforces_) {
            check(!tf.galactic_fleet || (galactic && tf.galactic_fleet < e.host_->ai_world->next_entity_id()));
            check(id && id == tf.id && id < e.next_taskforce_id_ && e.running_.contains(tf.plan));
            if (tf.galactic_stage) {
                const auto* world = dynamic_cast<const GalacticWorld*>(e.host_->ai_world.get());
                check(world && view && view->find(tf.galactic_stage)
                    && world->containing_planet(tf.galactic_stage) == tf.galactic_stage);
            }
            const auto& p = e.running_.at(tf.plan);
            check(tf.definition < e.plans_[p.definition].taskforces.size()
                && std::find(p.taskforces.begin(), p.taskforces.end(), id) != p.taskforces.end());
        }
        for (const auto& [id, b] : e.blocks_) {
            check((!b.galactic_request || (galactic && (b.kind == Block::Kind::move || b.kind == Block::Kind::galactic_invade)))
            && (galactic || b.kind <= Block::Kind::reinforce)
            && (!b.galactic_actor || (galactic && b.galactic_actor < e.host_->ai_world->next_entity_id())) && id && id == b.id && b.kind <= Block::Kind::galactic_invade && b.order <= Block::Order::guard
            && e.plan_of_instance_.contains(b.instance) && e.taskforces_.contains(b.taskforce)
            && e.plan_of_instance_.at(b.instance) == e.taskforces_.at(b.taskforce).plan
            && (!b.waiting_on || (b.waiting_on != id && e.blocks_.contains(b.waiting_on) && !e.blocks_.at(b.waiting_on).waiting_on)));
            if (b.kind == Block::Kind::galactic_invade && b.galactic_request) {
                const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(e.host_->galactic_execution.get());
                const auto* invasion = actions ? actions->invasion_status(b.galactic_request) : nullptr;
                check(invasion && invasion->fleet == b.galactic_actor && invasion->invader == e.running_.at(e.plan_of_instance_.at(b.instance)).player);
            }
        }
        const auto has_goal = [&](tactical::PlayerId player, std::uint64_t id) {
            const auto* owner = e.player_ai(player);
            return owner && (std::any_of(owner->active.begin(), owner->active.end(), [&](const auto& g) { return g.id == id; })
                || std::any_of(owner->proposed.begin(), owner->proposed.end(), [&](const auto& g) { return g.id == id; }));
        };
        for (const auto& pending : e.pending_attach_) check(has_goal(pending.player, pending.goal));
        for (const auto& [object, pad] : e.pad_reservations_) check(object && has_goal(pad.player, pad.goal));
    } catch (const std::exception& error) { return fail<void>(error.what()); }
    return core::Result<void>::success();
}

core::Result<Bytes> GoalStateContributor::encode(Engine& engine) const {
    try {
        Archive ar;
        std::string tag(domain), identity(content_identity_), configuration = GoalStateCodec::configuration(engine); std::uint32_t version{10};
        ar.fields(tag, version, identity, configuration);
        auto credits = engine.host_->credit_changes;
        GoalStateCodec::runtime(ar, engine, credits);
        return core::Result<Bytes>::success(std::move(ar.bytes));
    } catch (const std::exception& error) { return fail<Bytes>(error.what()); }
}
core::Result<galactic_ai::NativeState> GoalStateContributor::capture() const {
    if (!engine_) return fail<galactic_ai::NativeState>("goal contributor has no engine");
    if (!engine_->host_ || !engine_->host_->ai_world || engine_->host_->ai_world->goal_mode() != "GALACTIC")
        return fail<galactic_ai::NativeState>("galactic goal contributor requires its galactic owner");
    if (auto valid = validate(*engine_); !valid) return core::Result<galactic_ai::NativeState>::failure(valid.error());
    auto bytes = encode(*engine_); if (!bytes) return core::Result<galactic_ai::NativeState>::failure(bytes.error());
    galactic_ai::NativeState state; state.components[std::string(component)] = std::move(bytes).value();
    return core::Result<galactic_ai::NativeState>::success(std::move(state));
}
core::Result<void> GoalStateContributor::restore(const galactic_ai::NativeState& state) {
    if (!engine_ || state.version != galactic_ai::native_state_version || !state.components.contains(std::string(component))) return fail<void>("goal contributor component/version missing");
    const auto& bytes = state.components.at(std::string(component));
    if (bytes.size() > galactic_ai::native_state_max_bytes) return fail<void>("goal component exceeds byte bound");
    try {
        if (!engine_->host_ || !engine_->host_->ai_world || !engine_->host_->ai_world->available()
            || engine_->host_->ai_world->goal_mode() != "GALACTIC") return fail<void>("restore requires its rebuilt galactic world provider");
        const auto view = engine_->host_->ai_world->view();
        if (!view) return fail<void>("restore requires a copied completed world view");
        Engine staged(engine_->host_, engine_->data_, engine_->plans_);
        Archive ar(bytes); std::string tag, identity, configuration; std::uint32_t version{};
        ar.fields(tag, version, identity, configuration);
        ar.check(tag == domain && version == 10 && identity == content_identity_ && configuration == GoalStateCodec::configuration(staged));
        std::map<tactical::PlayerId, math::Fixed> credits;
        GoalStateCodec::runtime(ar, staged, credits); ar.check(ar.input.empty());
        for (const auto& [player, value] : credits) { static_cast<void>(value); ar.check(staged.host_->players.contains(player)); }
        if (auto valid = validate(staged, view.get()); !valid) return valid;
        // Canonical decoding is enforced field-by-field (especially map order).
        // No live owner or host mutation occurs before the full check succeeds.
        engine_->mark_native_state_changed();
        GoalStateCodec::publish(*engine_, staged);
        engine_->host_->credit_changes = std::move(credits);
        engine_->host_->view = view;
    } catch (const std::exception& error) { return fail<void>(error.what()); }
    return core::Result<void>::success();
}
core::Result<std::string> GoalStateContributor::state_hash() const {
    auto state = capture();
    if (!state) return core::Result<std::string>::failure(state.error());
    return galactic_ai::native_state_hash(state.value());
}

core::Result<Bytes> Engine::save_tactical_native(std::string_view identity,
    authoritative::ScriptScheduler& scripts) const {
    try {
        if (!host_ || !host_->ai_world || host_->ai_world->goal_mode() != "SPACE") return fail<Bytes>("native child requires the tactical provider");
        GoalStateContributor validation({}, std::string(identity));
        if (auto valid = validation.validate(*this); !valid) return core::Result<Bytes>::failure(valid.error());
        Archive ar; std::string tag = "eawr-tactical-ai-v6", content(identity);
        auto& source = const_cast<Engine&>(*this);
        auto configuration = GoalStateCodec::configuration(source);
        auto barrier = scripts.completed_tick(); ar.fields(tag, content, configuration, barrier);
        ar.fields(source.staggered_clock_.paused_frames, source.staggered_clock_.held_since, source.staggered_seeded_);
        auto credits = host_->credit_changes;
        GoalStateCodec::runtime(ar, source, credits);
        GoalStateCodec::grid(ar, source.grid_);
        return core::Result<Bytes>::success(std::move(ar.bytes));
    } catch (const std::exception& error) { return fail<Bytes>(error.what()); }
}
core::Result<void> Engine::restore_tactical_native(std::span<const std::uint8_t> bytes,
    std::string_view identity, authoritative::ScriptScheduler& scripts) {
    try {
        if (bytes.size() > galactic_ai::native_state_max_bytes || !host_ || !host_->ai_world
            || host_->ai_world->goal_mode() != "SPACE" || !host_->view) return fail<void>("native child needs a rebuilt tactical provider");
        Engine staged(host_, data_, plans_); Archive ar(bytes);
        std::string tag, content, configuration; std::uint64_t barrier{};
        ar.fields(tag, content, configuration, barrier);
        ar.check(tag == "eawr-tactical-ai-v6" && content == identity && barrier == scripts.completed_tick()
            && configuration == GoalStateCodec::configuration(staged));
        ar.fields(staged.staggered_clock_.paused_frames, staged.staggered_clock_.held_since, staged.staggered_seeded_);
        ar.check(staged.staggered_clock_.paused_frames <= barrier
            && (!staged.staggered_clock_.held_since || (*staged.staggered_clock_.held_since <= barrier
                && staged.staggered_clock_.paused_frames <= *staged.staggered_clock_.held_since))
            && (staged.staggered() || (!staged.staggered_seeded_ && !staged.staggered_clock_.held_since
                && staged.staggered_clock_.paused_frames == 0)));
        std::map<tactical::PlayerId, math::Fixed> credits;
        GoalStateCodec::runtime(ar, staged, credits); GoalStateCodec::grid(ar, staged.grid_);
        ar.check(ar.input.empty() && (!staged.initialized_ || staged.frame_ == static_cast<std::int64_t>(barrier)));
        GoalStateContributor validation({}, std::string(identity));
        if (auto valid = validation.validate(staged); !valid) return valid;
        const auto instances = scripts.instances();
        for (const auto& [id, plan] : staged.running_) {
            static_cast<void>(id); ar.check(std::binary_search(instances.begin(), instances.end(), plan.instance));
        }
        for (const auto& [player, amount] : credits) { static_cast<void>(amount); ar.check(host_->players.contains(player)); }
        GoalStateCodec::publish(*this, staged);
        mark_native_state_changed();
        grid_ = std::move(staged.grid_);
        host_->credit_changes = std::move(credits);
        scripts_ = &scripts;
        mark_native_state_changed();
        return core::Result<void>::success();
    } catch (const std::exception& error) { return fail<void>(error.what()); }
}
} // namespace eawr::script::foc::ai

namespace eawr::script::foc::tactical_ai_detail {
core::Result<std::string> native_configuration(const AiSetup& setup) {
    try {
        ai::Archive ar;
        auto content = setup.content; auto seed = setup.seed; auto schedule = setup.schedule;
        auto perception = setup.perception; auto special = setup.allow_special_weapons;
        ar.fields(content.types, content.projectile_categories, content.categories, content.properties,
            seed, schedule.mode, schedule.attach_per_tick, special, perception.campaign_game,
            perception.defender, perception.base_level, perception.campaign_defender);
        for (auto player : setup.players) ar.fields(player.player, player.faction, player.neutral,
            player.ai, player.player_type, player.human);
        bool bounds = setup.bounds.has_value(); ar.item(bounds);
        if (bounds) { auto b = *setup.bounds; ar.fields(b.left, b.top, b.right, b.bottom); }
        return core::Result<std::string>::success(core::sha256_hex(ar.bytes));
    } catch (const std::exception& error) { return ai::fail<std::string>(error.what()); }
}
}
