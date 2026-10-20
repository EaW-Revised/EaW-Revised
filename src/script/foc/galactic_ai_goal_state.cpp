#include "galactic_ai_goal_state.hpp"
#include "galactic_ai_world.hpp"
#include "galactic_ai_actions.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/core/gc_profile.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <source_location>
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
    bool galactic_tasks{}; // WGA-485: keep the tactical field inventory unchanged.
    Bytes bytes;
    std::span<const std::uint8_t> input;
    std::size_t depth{};
    std::uint64_t nodes{};
    std::size_t encoded_size{};
    std::map<std::tuple<std::uint32_t, std::uint32_t, Bytes>, Bytes, NativeRecordLess>* record_output{};
    bool record_headers_only{};
    template<class Key, class Write> void record(std::uint32_t owner, std::uint32_t kind, Key key, Write write) {
        if (!record_output) { write(); return; }
        if (record_headers_only) return;
        Archive identity; identity.item(key);
        const auto start = bytes.size(); write();
        Bytes payload = identity.bytes; payload.insert(payload.end(), bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.end());
        check(record_output->emplace(std::tuple{owner, kind, std::move(identity.bytes)}, std::move(payload)).second);
        bytes.resize(start);
    }
    template<class K, class V, class C, class Owner> void map_records(std::map<K, V, C>& value, std::uint32_t kind, Owner owner) {
        if (!record_output) { item(value); return; }
        count(value.size());
        if (record_headers_only) return;
        for (auto& [key, row] : value) record(owner(row), kind, key, [&] { auto copied = key; fields(copied, row); });
    }
    void goal_records(std::vector<Goal>& value, std::uint32_t owner, std::uint32_t kind) {
        if (!record_output) { item(value); return; }
        count(value.size());
        if (record_headers_only) return;
        std::uint64_t previous{};
        for (auto& row : value) {
            record(owner, kind, row.id, [&] { const auto size = encoded_size; item(previous); encoded_size = size; item(row); });
            previous = row.id;
        }
    }

    explicit Archive(std::span<const std::uint8_t> source) : reading(true), input(source) {}
    Archive() = default;
    void check(bool ok, const std::source_location& location = std::source_location::current()) const {
        if (!ok) throw std::runtime_error("invalid galactic goal state encoding at line "
            + std::to_string(location.line()));
    }
    void word(std::uint64_t& value, std::size_t width) {
        if (reading) {
            check(width <= input.size()); value = 0;
            for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t{input[i]} << (i * 8U);
            input = input.subspan(width);
        } else {
            check(encoded_size <= galactic_ai::native_state_max_bytes - width); encoded_size += width;
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
        else { check(length <= galactic_ai::native_state_max_bytes - encoded_size); encoded_size += static_cast<std::size_t>(length); bytes.insert(bytes.end(), value.begin(), value.end()); }
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
    void item(PotentialPlan& v) { fields(v.id, v.plan, v.valid, v.reserved, v.funded, v.units, v.freestore, v.sources, v.producers, v.pool_tokens, v.cost, v.taskforce_of_unit, v.team_of_unit, v.target_contrast, v.current_contrast, v.threshold, v.contrast_indices, v.target_contrast_disabled); }
    void item(Goal& v) { fields(v.id, v.function, v.target, v.desire, v.active, v.finished, v.potential, v.plan, v.exclusions_registered); }
    void item(History& v) { item(v.entries); }
    void item(Difficulty& v) { fields(v.space_contrast_multiplier, v.space_goal_cycle_sleep, v.galactic_contrast_multiplier, v.galactic_build_time_multiplier, v.galactic_goal_cycle_sleep, v.credit_multiplier); }
    void item(TeamDef& v) { fields(v.min_count, v.max_count, v.percentage, v.percentage_based, v.types); }
    void item(TaskForceDef& v) { fields(v.name, v.teams, v.escort, v.required, v.minimum_force, v.minimum_size); }
    void item(detail::ContrastEntry& v) { fields(v.category_bits, v.type, v.weight); }
    void item(PlanDef& v) {
        fields(v.name, v.module, v.goals, v.taskforces, v.ignore_target, v.magic, v.allow_free_store, v.allow_special_weapons, v.allow_engaged, v.per_failure_contrast_adjust, v.min_contrast, v.max_contrast, v.required_categories);
        // Plan definitions are immutable configuration inputs, never save-state
        // rows. Preserve tactical configuration bytes; qualify the extra
        // galactic matrix in its configuration identity only.
        if (v.galactic_contrast_qualified) {
            std::string tag{"galactic-plan-contrast-v1"};
            fields(tag, v.galactic_contrast_order, v.galactic_contrast);
        }
    }
    void item(GoalType& v) { fields(v.name, v.application, v.mode, v.category, v.reachability, v.time_limit, v.build_time_delay_tolerance, v.tracking_duration, v.per_failure_desire_adjust, v.activation_tracking_duration, v.per_activation_failure_desire_adjust, v.is_like, v.global_exclusions); }
    void item(PlayerType& v) { fields(v.name, v.goal_set_extension, v.function_sets, v.space_templates, v.galactic_templates, v.freestore, v.difficulty); }
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
    void item(BuildTask& v) {
        fields(v.taskforce, v.block, v.type, v.object, v.source, v.producer, v.completion, v.issued, v.child_floor, v.pool_token, v.pad_build, v.direct_pad, v.prepaid, v.acknowledged, v.generic_cost, v.finished, v.failed, v.staging, v.associated_producer, v.state_entered, v.salvage, v.allocated, v.joined, v.synchronized_staging);
        if (galactic_tasks) fields(v.galactic_assembly, v.galactic_assembly_request, v.galactic_stage_seconds, v.galactic_stage_remaining, v.galactic_stage_tick);
        else check(v.galactic_assembly == BuildTask::GalacticAssembly::none && !v.galactic_assembly_request);
    }
    void item(Block& v) { fields(v.kind, v.id, v.instance, v.taskforce, v.order, v.attack, v.destination_object, v.destination, v.movers, v.ordered_tick, v.side, v.distance, v.tolerance, v.last_update, v.ready, v.finished, v.result, v.reinforcing, v.entity_floor, v.issued, v.reinforcement_attempt, v.pool_token, v.waiting_on, v.galactic_request, v.galactic_actor, v.held_owner); }
    void item(TaskForce::Purchase& v) { fields(v.type, v.token); }
    void item(TaskForce& v) { fields(v.id, v.plan, v.definition, v.name, v.units, v.types, v.sources, v.producers, v.pooled, v.pool_tokens, v.thread, v.had_units, v.damaged_pending, v.last_target, v.galactic_stage, v.galactic_fleet); }
    void item(Plan& v) { fields(v.id, v.instance, v.definition, v.goal, v.player, v.target, v.target_object, v.target_destroyed_signalled, v.taskforces, v.result, v.removable, v.exited, v.purge, v.start_tick, v.requires_production, v.reserved_pads); }
    void item(TickCost& v) { fields(v.tick, v.freestore, v.plans, v.plan_instances, v.freestore_runs, v.goals_evaluated, v.maintenances, v.plans_attached, v.plans_deferred, v.plans_pumped, v.reinforcement_candidates, v.reinforcement_search_ns); }
    void item(PendingAttach& v) { fields(v.player, v.goal); }
    void item(AttackPositioning& v) { fields(v.instance, v.next, v.target); }
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
void admission_configuration(Archive& ar, const AiSetup& setup) {
    // Omitted and explicit-zero admission retain the existing configuration identity.
    auto admission = setup.player_ai_start_ticks;
    std::erase_if(admission, [](const auto& entry) { return entry.second == 0; });
    if (!admission.empty()) {
        std::string admission_domain = "eawr-ai-admission-v1";
        ar.fields(admission_domain, admission);
    }
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
        admission_configuration(ar, engine.host_->setup);
        // Parsed function code is part of the native configuration too. The
        // required external closure digest covers all transitive XML/Lua/type inputs.
        for (const auto& [name, functions] : engine.data_.function_sets) {
            static_cast<void>(name);
            for (const auto& function : functions) if (const auto* found = engine.data_.equations.find(function.function)) { auto equation = *found; ar.item(equation); }
        }
        engine.native_configuration_ = core::sha256_hex(ar.bytes);
        return engine.native_configuration_;
    }
    static void player(Archive& ar, PlayerAi& v, const AiData& data, const AiSetup& setup) {
        std::string type;
        if (!ar.reading) for (const auto& [name, row] : data.players) if (&row == v.type) type = name;
        ar.fields(v.player, type); ar.check(!type.empty() && data.players.contains(type));
        if (ar.reading) v.type = &data.players.at(type);
        std::string selected;
        if (!ar.reading) for (const auto& [name, row] : data.templates) if (&row == v.space_template) selected = name;
        ar.item(selected); ar.check(selected.empty() || data.templates.contains(selected));
        if (ar.reading) v.space_template = selected.empty() ? nullptr : &data.templates.at(selected);
        ar.fields(v.difficulty, v.galactic);
        ar.record(v.player, 33, std::uint64_t{}, [&] { ar.item(v.galactic_policy); });
        ar.fields(v.proposal_counts, v.exclusion_counts);
        const auto functions = ar.count(v.functions.size()); if (ar.reading) v.functions.resize(static_cast<std::size_t>(functions));
        for (auto& f : v.functions) {
            ar.fields(f.goal_name, f.equation_name);
            ar.check(data.goals.contains(f.goal_name) && data.equations.find(f.equation_name));
            if (ar.reading) { f.goal = &data.goals.at(f.goal_name); f.equation = data.equations.find(f.equation_name); }
        }
        ar.fields(v.targets, v.next_goal, v.next_planning, v.next_execution, v.next_learning, v.phase,
            v.sleep_frames, v.per_frame, v.nontrivial, v.next_function, v.next_target, v.maintenance_due);
        ar.goal_records(v.proposed, v.player, 2); ar.goal_records(v.active, v.player, 3); ar.fields(v.proposal_blocks, v.proposal_blockers);
        const auto owner = [&](const auto&) { return v.player; };
        ar.map_records(v.activations, 5, owner); ar.map_records(v.goal_outcomes, 6, owner); ar.map_records(v.plan_outcomes, 7, owner);
        ar.map_records(v.plan_success, 8, owner); ar.map_records(v.goal_success, 9, owner);
        ar.map_records(v.target_goal_success, 10, owner); ar.map_records(v.target_plan_success, 11, owner);
        ar.map_records(v.unit_survival, 12, owner); ar.map_records(v.reserved, 13, owner); ar.map_records(v.assigned, 14, owner);
        ar.map_records(v.reserved_pool, 15, owner); ar.map_records(v.reserved_pool_tokens, 16, owner); ar.map_records(v.reserved_credits, 17, owner);
        ar.record(v.player, 4, std::uint64_t{}, [&] { ar.item(v.tasks); }); ar.item(v.next_goal_id);
        // GFS-A21/A24: both modes retain their per-player streams. Only galactic players use ranges.
        auto seed = v.sync.seed();
        ar.item(seed);
        if (ar.reading) v.sync.set_seed(seed);
        if (v.galactic) {
            ar.fields(v.slot, v.next_target_local, v.next_taskforce_local, v.next_plan_local, v.next_instance_local);
        }
        // Positive admission maps retain the pending first refresh in both the
        // canonical player bytes and the incremental player-header record.
        const auto admission = setup.player_ai_start_ticks.find(v.player);
        if (admission != setup.player_ai_start_ticks.end() && admission->second != 0)
            ar.item(v.admission_pending);
    }
    // Global ascending-ID adapters preserve the existing byte layout without engine-owned rows.
    template<class T> static void rows(Archive& ar, Engine& e, std::map<std::uint64_t, T> PlayerAi::* member,
        const std::function<PlayerAi*(std::uint64_t, const T&)>& owner, std::uint32_t kind = 0,
        const std::function<std::uint32_t(const T&)>& record_owner = {}) {
        if (!ar.reading) {
            const auto ordered = e.ordered_runtime(member);
            (void)ar.count(ordered.size());
            for (const auto& [id, row] : ordered) {
                const auto write = [&] { auto key = id; ar.fields(key, *row); };
                if (kind && ar.record_output) ar.record(record_owner(*row), kind, id, write);
                else write();
            }
        } else {
            std::map<std::uint64_t, T> staged;
            ar.item(staged);
            for (auto& p : e.players_) (p.*member).clear();
            for (auto& [id, row] : staged) {
                auto* selected = owner(id, row); ar.check(selected);
                ar.check((selected->*member).emplace(id, std::move(row)).second);
            }
        }
    }
    static void runtime_header(Archive& ar, Engine& v) {
        ar.fields(v.initialized_, v.game_seed_, v.frame_, v.event_tick_, v.sequence_, v.order_sequence_,
            v.next_target_id_, v.next_taskforce_id_, v.next_plan_id_, v.next_instance_);
        auto seed = v.sync_.seed(); ar.item(seed); if (ar.reading) v.sync_.set_seed(seed);
    }
    static void runtime(Archive& ar, Engine& v, std::map<tactical::PlayerId, math::Fixed>& credits) {
        runtime_header(ar, v);
        const auto players = ar.count(v.players_.size()); if (ar.reading) v.players_.resize(static_cast<std::size_t>(players));
        for (auto& p : v.players_) ar.record(p.player, 1, std::uint64_t{}, [&] { player(ar, p, v.data_, v.host_->setup); });
        const auto shared = [](const auto&) { return std::uint32_t{}; };
        const auto plan_owner = [&](std::uint64_t id) { const auto* row = v.plan(id); return row ? row->player : std::uint32_t{}; };
        ar.map_records(v.targets_, 20, shared);
        // TaskForces precede plans in the stream, so decode those into private staging first.
        std::map<std::uint64_t, TaskForce> forces;
        if (ar.reading) ar.item(forces);
        else rows<TaskForce>(ar, v, &PlayerAi::taskforces, {}, 21, [&](const auto& row) { return plan_owner(row.plan); });
        rows<Plan>(ar, v, &PlayerAi::running, [&](auto, const Plan& row) { return v.player_ai(row.player); },
            22, [](const auto& row) { return row.player; });
        std::map<std::uint64_t, PlayerAi*> plan_owners, instance_owners;
        if (ar.reading) {
            for (auto& owner : v.players_) for (const auto& [id, plan] : owner.running) {
                ar.check(plan_owners.emplace(id, &owner).second);
                ar.check(instance_owners.emplace(plan.instance, &owner).second);
            }
            for (auto& [id, row] : forces) {
                const auto owner = plan_owners.find(row.plan); ar.check(owner != plan_owners.end());
                ar.check(owner->second->taskforces.emplace(id, std::move(row)).second);
            }
        }
        const auto owner_of_instance = [&](std::uint64_t instance) {
            const auto found = instance_owners.find(instance);
            return found == instance_owners.end() ? nullptr : found->second;
        };
        rows<Block>(ar, v, &PlayerAi::blocks, [&](auto, const Block& row) { return owner_of_instance(row.instance); },
            23, [&](const auto& row) { const auto* force = v.taskforce(row.taskforce); return force ? plan_owner(force->plan) : std::uint32_t{}; });
        rows<std::uint64_t>(ar, v, &PlayerAi::plan_of_instance, [&](auto instance, auto) { return owner_of_instance(instance); },
            24, [&](const auto& id) { return plan_owner(id); });
        if (ar.reading) v.rebuild_runtime_index();
        ar.map_records(v.threats_, 25, shared); ar.map_records(v.last_hits_, 26, shared); ar.map_records(v.globals_, 27, shared);
        ar.record(0, 28, std::uint64_t{}, [&] { ar.item(v.pending_attach_); });
        ar.record(0, 29, std::uint64_t{}, [&] { ar.item(v.events_); });
        ar.record(0, 30, std::uint64_t{}, [&] { ar.item(v.orders_); });
        ar.map_records(credits, 31, shared);
        // Timing/work diagnostics do not drive continuation and must not leak
        // wall-clock nanoseconds into the canonical simulation digest.
        const auto pads = ar.count(v.pad_reservations_.size());
        if (ar.reading) v.pad_reservations_.clear();
        if (!ar.reading) for (auto& [id, row] : v.pad_reservations_) ar.record(row.player, 32, id, [&] { auto copied = id; ar.fields(copied, row.player, row.goal, row.type, row.consumed, row.issued); });
        else {
            sim::EntityId previous{};
            for (std::uint64_t i = 0; i < pads; ++i) {
                sim::EntityId id{}; Engine::PadReservation row;
                ar.fields(id, row.player, row.goal, row.type, row.consumed, row.issued);
                ar.check(id > previous); previous = id; v.pad_reservations_.emplace(id, row);
            }
        }
    }
    static Engine::NativeRecordRows records(Engine& engine, bool cold = false) {
        Engine::NativeRecordRows rows;
        if (cold) {
            // Independent oracle: the full canonical field walk, without dirty selections.
            Archive ar; ar.galactic_tasks = true; ar.record_output = &rows;
            auto credits = engine.host_->credit_changes;
            ar.record(0, 0, std::uint64_t{}, [&] { runtime(ar, engine, credits); });
            return rows;
        }
        const auto wanted = [&](const Engine::NativeRecordKey& key) { return cold || engine.native_record_selected(key); };
        const auto kind_wanted = [&](std::uint32_t kind) { return cold || engine.native_record_kind_selected(kind); };
        const auto emit = [&](std::uint32_t owner, std::uint32_t kind, auto key, auto write) {
            if (!engine.native_record_owner_kind_selected(owner, kind)) return;
            ++engine.native_record_inspections_;
            Archive identity; identity.item(key);
            Engine::NativeRecordKey record_key{owner, kind, identity.bytes};
            if (!wanted(record_key)) return;
            Archive value; value.galactic_tasks = true; value.bytes = std::move(identity.bytes); write(value);
            rows.emplace(std::move(record_key), std::move(value.bytes));
            if (!cold) ++engine.native_record_work_.at(kind);
        };
        emit(0, 0, std::uint64_t{}, [&](Archive& ar) {
            runtime_header(ar, engine);
            std::array<std::size_t, 4> counts{};
            for (const auto& owner : engine.players_) {
                counts[0] += owner.taskforces.size(); counts[1] += owner.running.size();
                counts[2] += owner.blocks.size(); counts[3] += owner.plan_of_instance.size();
            }
            for (const auto count : {engine.players_.size(), engine.targets_.size(), counts[0],
                counts[1], counts[2], counts[3], engine.threats_.size(),
                engine.last_hits_.size(), engine.globals_.size(), engine.host_->credit_changes.size(), engine.pad_reservations_.size()})
                ar.count(count);
        });
        std::uint64_t group_kinds{};
        for (const auto& [owner, bits] : engine.native_record_dirty_groups_) { static_cast<void>(owner); group_kinds |= bits; }
        const auto map = [&](auto& values, std::uint32_t kind, auto owner) {
            if (!kind_wanted(kind)) return;
            const auto row = [&](auto at) { emit(owner(at->second), kind, at->first, [&](Archive& ar) { auto copied = at->first; ar.fields(copied, at->second); }); };
            using Key = typename std::remove_reference_t<decltype(values)>::key_type;
            if constexpr (std::is_integral_v<Key>) {
                if (!engine.native_records_all_dirty_ && !(group_kinds & (std::uint64_t{1} << kind))) {
                    // An exact dirty key probes the source map directly; it
                    // never constructs identities for unrelated source rows.
                    std::set<Key> seen;
                    for (const auto& key : engine.native_record_dirty_keys_) {
                        if (std::get<1>(key) != kind) continue;
                        const auto& bytes = std::get<2>(key);
                        if (bytes.size() != sizeof(Key)) continue;
                        std::uint64_t decoded{};
                        for (std::size_t i = 0; i < bytes.size(); ++i) decoded |= std::uint64_t{bytes[i]} << (8U * i);
                        const auto value = static_cast<Key>(decoded);
                        if (!seen.insert(value).second) continue;
                        const auto at = values.find(value); if (at != values.end()) row(at);
                    }
                    return;
                }
            }
            for (auto at = values.begin(); at != values.end(); ++at) row(at);
        };
        // Point-dirty records probe the ID-to-player index once. A group walk visits
        // the same rows as the former global map, then emits into the ordered record set.
        const auto owned_map = [&](auto member, const auto& index, std::uint32_t kind, auto owner) {
            if (!kind_wanted(kind)) return;
            const auto row = [&](std::uint64_t key, auto& value) {
                emit(owner(value), kind, key, [&](Archive& ar) { ar.fields(key, value); });
            };
            if (!engine.native_records_all_dirty_ && !(group_kinds & (std::uint64_t{1} << kind))) {
                std::set<std::uint64_t> seen;
                for (const auto& key : engine.native_record_dirty_keys_) {
                    if (std::get<1>(key) != kind) continue;
                    const auto& bytes = std::get<2>(key);
                    if (bytes.size() != sizeof(std::uint64_t)) continue;
                    std::uint64_t id{};
                    for (std::size_t i = 0; i < bytes.size(); ++i) id |= std::uint64_t{bytes[i]} << (8U * i);
                    if (!seen.insert(id).second) continue;
                    const auto at = index.find(id); if (at == index.end()) continue;
                    const auto* selected = std::as_const(engine).player_ai(at->second);
                    if (!selected) continue;
                    if (auto* value = Engine::runtime_row(const_cast<PlayerAi*>(selected)->*member, id)) row(id, *value);
                }
                return;
            }
            for (auto& selected : engine.players_) for (auto& [id, value] : selected.*member) row(id, value);
        };
        for (auto& p : engine.players_) {
            emit(p.player, 1, std::uint64_t{}, [&](Archive& ar) {
                Engine::NativeRecordRows ignored; ar.record_output = &ignored; ar.record_headers_only = true;
                player(ar, p, engine.data_, engine.host_->setup);
            });
            emit(p.player, 33, std::uint64_t{}, [&](Archive& ar) { ar.item(p.galactic_policy); });
            const auto goals = [&](auto& values, std::uint32_t kind) {
                if (!engine.native_record_owner_kind_selected(p.player, kind)) return;
                std::uint64_t previous{};
                for (auto& goal : values) {
                    emit(p.player, kind, goal.id, [&](Archive& ar) { ar.fields(previous, goal); });
                    previous = goal.id;
                }
            };
            goals(p.proposed, 2); goals(p.active, 3);
            emit(p.player, 4, std::uint64_t{}, [&](Archive& ar) { ar.item(p.tasks); });
            const auto owner = [&](const auto&) { return p.player; };
            map(p.activations, 5, owner); map(p.goal_outcomes, 6, owner); map(p.plan_outcomes, 7, owner);
            map(p.plan_success, 8, owner); map(p.goal_success, 9, owner);
            map(p.target_goal_success, 10, owner); map(p.target_plan_success, 11, owner);
            map(p.unit_survival, 12, owner); map(p.reserved, 13, owner); map(p.assigned, 14, owner);
            map(p.reserved_pool, 15, owner); map(p.reserved_pool_tokens, 16, owner); map(p.reserved_credits, 17, owner);
        }
        const auto shared = [](const auto&) { return std::uint32_t{}; };
        const auto plan_owner = [&](std::uint64_t id) { const auto* row = engine.plan(id); return row ? row->player : std::uint32_t{}; };
        map(engine.targets_, 20, shared);
        owned_map(&PlayerAi::taskforces, engine.runtime_index_.taskforces, 21, [&](const auto& row) { return plan_owner(row.plan); });
        owned_map(&PlayerAi::running, engine.runtime_index_.plans, 22, [](const auto& row) { return row.player; });
        owned_map(&PlayerAi::blocks, engine.runtime_index_.blocks, 23, [&](const auto& row) { const auto* force = engine.taskforce(row.taskforce); return force ? plan_owner(force->plan) : std::uint32_t{}; });
        owned_map(&PlayerAi::plan_of_instance, engine.runtime_index_.instances, 24, [&](const auto& id) { return plan_owner(id); });
        map(engine.threats_, 25, shared); map(engine.last_hits_, 26, shared); map(engine.globals_, 27, shared);
        emit(0, 28, std::uint64_t{}, [&](Archive& ar) { ar.item(engine.pending_attach_); });
        emit(0, 29, std::uint64_t{}, [&](Archive& ar) { ar.item(engine.events_); });
        emit(0, 30, std::uint64_t{}, [&](Archive& ar) { ar.item(engine.orders_); });
        map(engine.host_->credit_changes, 31, shared);
        if (kind_wanted(32)) for (auto& [key, row] : engine.pad_reservations_)
            emit(row.player, 32, key, [&](Archive& ar) { auto copied = key; ar.fields(copied, row.player, row.goal, row.type, row.consumed, row.issued); });
        return rows;
    }
    static void publish(Engine& destination, Engine& staged, bool positioning_restored = false) {
        if (!positioning_restored) {
            // Galactic bytes omit tactical positioning, so restore must retain those
            // rows just as the former engine-wide container did.
            for (const auto& old : destination.players_) if (!old.positioning.empty()) {
                auto* owner = staged.player_ai(old.player);
                if (!owner && !staged.players_.empty()) owner = &staged.players_.front();
                if (!owner) throw std::runtime_error("cannot retain positioning without an AI owner");
                for (const auto& [unit, row] : old.positioning)
                    if (!owner->positioning.emplace(unit, row).second)
                        throw std::runtime_error("duplicate retained positioning identity");
            }
        }
        // Staged pointers name a private copied AiData. Rebind against the
        // live immutable closure before the single ordered owner publication.
        for (auto& p : staged.players_) {
            for (const auto& [name, row] : staged.data_.players) if (&row == p.type) { p.type = &destination.data_.players.at(name); break; }
            for (const auto& [name, row] : staged.data_.templates) if (&row == p.space_template) { p.space_template = &destination.data_.templates.at(name); break; }
            for (auto& f : p.functions) { f.goal = &destination.data_.goals.at(f.goal_name); f.equation = destination.data_.equations.find(f.equation_name); }
            if (auto* old = destination.player_ai(p.player)) p.galactic_funding = old->galactic_funding;
        }
        destination.players_ = std::move(staged.players_); destination.targets_ = std::move(staged.targets_);
        // Decode already built this ID-only index. Move it with its owners so
        // publication does not allocate or repeat the full lookup build.
        destination.runtime_index_ = std::move(staged.runtime_index_);
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
        if (ar.reading) {
            g.clear_queries();
            g.slots_dirty_ = true;
            g.slots_.clear();
        }
        ar.fields(g.left_, g.top_, g.right_, g.bottom_, g.cell_width_, g.cell_height_, g.min_radius_,
            g.x_cells_, g.y_cells_, g.constants_, g.cells_, g.setup_scanned_, g.setup_limit_);
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
        // PE-25: each session player's per-threat-cell last fully-fogged frame (the fog rows are derived).
        std::vector<std::vector<std::int64_t>> stamps;
        if (!ar.reading) for (const auto& watch : g.fog_watch_) stamps.push_back(watch.last_seen);
        ar.item(stamps);
        if (ar.reading) {
            g.fog_watch_.assign(stamps.size(), {});
            for (std::size_t i = 0; i < stamps.size(); ++i) {
                ar.check(stamps[i].size() == g.cells_.size());
                for (const auto frame : stamps[i]) ar.check(frame >= 0);
                g.fog_watch_[i].last_seen = std::move(stamps[i]);
            }
        }
        ar.check(g.x_cells_ >= 0 && g.y_cells_ >= 0 && std::uint64_t(g.x_cells_) * std::uint64_t(g.y_cells_) == g.cells_.size());
        ar.check(g.x_cells_ == 0 || (Real{} < g.cell_width_ && Real{} < g.cell_height_));
        for (const auto& cell : g.cells_) for (const auto& [id, index] : cell)
            ar.check(g.tracked_.contains(id) && index < g.tracked_.at(id).entries.size());
    }
    static void positioning(Archive& ar, Engine& v) {
        // A row's storage shard is derived, never a service-order or decision input.
        // Creation, re-enabling and decode all use the current AI unit owner, else the lowest AI.
        rows<AttackPositioning>(ar, v, &PlayerAi::positioning,
            [&](auto unit, const auto&) { return v.positioning_owner(unit); });
        const auto limit = v.host_->ai_world->next_entity_id();
        // Nonnegative clocks let us subtract below without overflowing frame+150.
        ar.check(v.frame_ >= 0);
        for (const auto& [unit, entry] : v.ordered_runtime(&PlayerAi::positioning)) {
            const auto& row = *entry;
            // Unit loss is cleared at its next positioning service. A departed
            // plan's issuing identity persists until flag-off or unit loss.
            ar.check(unit && unit < limit && row.instance >= first_plan_instance
                && row.instance < v.next_instance_ && row.next >= 0
                && row.next - v.frame_ <= attack_positioning_frames && row.target < limit);
        }
    }
};

void Engine::enable_native_records() {
    if (native_records_enabled_) return;
    native_records_enabled_ = true; native_records_all_dirty_ = true; refresh_native_records();
}
// A player turn stages its invalidations (S-T3b); the commit publishes them in ascending player order.
void Engine::native_records_changed(std::uint32_t owner, std::initializer_list<std::uint32_t> kinds) {
    if (!native_records_enabled_) return;
    // An empty list still touches the owner's group and the header, as a direct call always did.
    if (auto* turn = staged()) {
        if (kinds.size() == 0) turn->invalidations.push_back({owner, touch_only_kind, 0, false});
        for (const auto kind : kinds) turn->invalidations.push_back({owner, kind, 0, false});
        return;
    }
    if (kinds.size() == 0) apply_native_records_changed(owner, touch_only_kind);
    for (const auto kind : kinds) apply_native_records_changed(owner, kind);
    native_record_header_dirty_ = true;
}
void Engine::native_record_changed(std::uint32_t owner, std::uint32_t kind, std::uint64_t key) {
    if (!native_records_enabled_) return;
    if (auto* turn = staged()) { turn->invalidations.push_back({owner, kind, key, true}); return; }
    apply_native_record_changed(owner, kind, key);
}
bool Engine::native_record_selected(const NativeRecordKey& key) const {
    if (native_records_all_dirty_) return true;
    const auto owner = std::get<0>(key), kind = std::get<1>(key);
    if (owner == 0 && kind == 0 && native_record_header_dirty_) return true;
    const auto group = [&](std::uint32_t selected) {
        const auto at = native_record_dirty_groups_.find(selected);
        return at != native_record_dirty_groups_.end() && (at->second & (std::uint64_t{1} << kind));
    };
    return group(owner) || group(native_all_players) || native_record_dirty_keys_.contains(key)
        || native_record_dirty_keys_.contains(std::tuple<std::uint32_t, std::uint32_t, const core::RecordDigest::Bytes&>{native_all_players, kind, std::get<2>(key)});
}
bool Engine::native_record_kind_selected(std::uint32_t kind) const {
    if (native_records_all_dirty_) return true;
    for (const auto& [owner, bits] : native_record_dirty_groups_) {
        static_cast<void>(owner); if (bits & (std::uint64_t{1} << kind)) return true;
    }
    return std::any_of(native_record_dirty_keys_.begin(), native_record_dirty_keys_.end(),
        [&](const auto& key) { return std::get<1>(key) == kind; });
}
bool Engine::native_record_owner_kind_selected(std::uint32_t owner, std::uint32_t kind) const {
    if (native_records_all_dirty_ || (owner == 0 && kind == 0 && native_record_header_dirty_)) return true;
    const auto selected = [&](std::uint32_t player) {
        const auto group = native_record_dirty_groups_.find(player);
        if (group != native_record_dirty_groups_.end() && (group->second & (std::uint64_t{1} << kind))) return true;
        const auto key = native_record_dirty_keys_.lower_bound(NativeRecordKey{player, kind, {}});
        return key != native_record_dirty_keys_.end() && std::get<0>(*key) == player && std::get<1>(*key) == kind;
    };
    return selected(owner) || selected(native_all_players);
}
void Engine::refresh_native_records() noexcept {
    if (!native_records_enabled_ || (!native_records_all_dirty_ && !native_record_header_dirty_
        && native_record_dirty_groups_.empty() && native_record_dirty_keys_.empty())) return;
    gc_profile::Scope profile_scope(gc_profile::Phase::goal_record_maintenance);
    try {
        auto next = GoalStateCodec::records(*this);
        const auto reconcile = [&](NativeRecordRows::iterator at) {
            ++native_record_inspections_;
            if (!next.contains(at->first)) {
                const auto& [owner, kind, key] = at->first;
                native_records_.erase(owner, kind, std::span<const std::uint8_t>(key));
                native_record_rows_.erase(at);
            }
        };
        const auto range = [&](std::uint32_t owner, std::uint32_t kind) {
            auto at = native_record_rows_.lower_bound(NativeRecordPrefix{owner, kind});
            while (at != native_record_rows_.end() && std::get<0>(at->first) == owner && std::get<1>(at->first) == kind)
                reconcile(at++);
        };
        const auto exact = [&](const NativeRecordKey& key) {
            const auto at = native_record_rows_.find(key);
            if (at != native_record_rows_.end()) reconcile(at);
        };
        // Snapshot only the bounded shard list: removing a shard's final row can
        // erase its sum while the selected owner/kind ranges are reconciled.
        std::vector<std::uint32_t> owners;
        const auto all_owners = [&]() -> const std::vector<std::uint32_t>& {
            if (owners.empty()) for (const auto& [owner, sum] : native_records_.shards()) { static_cast<void>(sum); owners.push_back(owner); }
            return owners;
        };
        if (native_records_all_dirty_) {
            for (auto at = native_record_rows_.begin(); at != native_record_rows_.end();) reconcile(at++);
        } else {
            for (const auto& [owner, bits] : native_record_dirty_groups_) for (std::uint32_t kind = 0; kind < native_record_work_.size(); ++kind) {
                if (!(bits & (std::uint64_t{1} << kind))) continue;
                if (owner == native_all_players) for (const auto player : all_owners()) range(player, kind);
                else range(owner, kind);
            }
            for (const auto& key : native_record_dirty_keys_) {
                if (std::get<0>(key) == native_all_players) {
                    for (const auto player : all_owners()) exact({player, std::get<1>(key), std::get<2>(key)});
                } else exact(key);
            }
            if (native_record_header_dirty_) exact({0, 0, core::RecordDigest::Bytes(8)});
        }
        for (auto& [key, bytes] : next) {
            const auto old = native_record_rows_.find(key); if (old != native_record_rows_.end() && old->second == bytes) continue;
            native_records_.set(std::get<0>(key), std::get<1>(key), std::span<const std::uint8_t>(std::get<2>(key)), bytes);
            native_record_rows_.insert_or_assign(key, std::move(bytes));
        }
        native_record_dirty_groups_.clear(); native_record_dirty_keys_.clear();
        native_records_all_dirty_ = false; native_record_header_dirty_ = false; native_record_failure_.reset();
    } catch (const std::exception& error) { native_record_failure_ = detail::make_error(authoritative::codes::invalid_request, error.what()); }
}
core::Result<std::string> GoalStateContributor::record_sync_hash(bool cold) const {
    if (!engine_) return fail<std::string>("goal contributor has no engine");
    if (engine_->native_record_failure_) return core::Result<std::string>::failure(*engine_->native_record_failure_);
    core::RecordDigest rebuilt;
    if (cold) for (const auto& [key, bytes] : GoalStateCodec::records(*engine_, true))
        rebuilt.set(std::get<0>(key), std::get<1>(key), std::span<const std::uint8_t>(std::get<2>(key)), bytes);
    auto configuration = GoalStateCodec::configuration(*engine_);
    return core::Result<std::string>::success(galactic_ai::compose_native_sync_digests({{"records", (cold ? rebuilt : engine_->native_records_).digest(domain)},
        {"content", content_identity_}, {"configuration", configuration}}));
}
core::Result<void> GoalStateContributor::validate(const Engine& e, const detail::WorldView* completed_view) const {
    // A restore checks references against the rebuilt world's completed view, which the host
    // only holds after validation succeeded; a rebuilt host still has its pre-publication view.
    const detail::WorldView* const view = completed_view ? completed_view : e.host_ ? e.host_->view.get() : nullptr;
    const auto check = [](bool ok) { if (!ok) throw std::runtime_error("galactic goal state has invalid identity/reference"); };
    try {
        const auto running = e.ordered_runtime(&PlayerAi::running);
        const auto taskforces = e.ordered_runtime(&PlayerAi::taskforces);
        const auto blocks = e.ordered_runtime(&PlayerAi::blocks);
        std::map<std::uint64_t, std::uint64_t> instances;
        for (const auto& owner : e.players_) for (const auto& [id, row] : owner.plan_of_instance)
            check(instances.emplace(id, row).second);
        std::size_t plan_count{}, force_count{}, block_count{}, positioning_count{};
        for (const auto& owner : e.players_) {
            plan_count += owner.running.size(); force_count += owner.taskforces.size();
            block_count += owner.blocks.size(); positioning_count += owner.positioning.size();
            for (const auto& [id, row] : owner.running) { check(row.player == owner.player); (void)id; }
            for (const auto& [id, row] : owner.taskforces) { check(owner.running.contains(row.plan)); (void)id; }
            for (const auto& [id, row] : owner.blocks) { check(owner.plan_of_instance.contains(row.instance)); (void)id; }
        }

        check(plan_count == running.size() && force_count == taskforces.size() && block_count == blocks.size()
            && positioning_count == e.ordered_runtime(&PlayerAi::positioning).size());
        check(e.host_ && e.host_->ai_world && digest(content_identity_));
        const bool galactic = e.host_->ai_world->goal_mode() == "GALACTIC";
        check(galactic || e.host_->ai_world->goal_mode() == "SPACE");
        const auto* native_actions = galactic ? dynamic_cast<const galactic_ai::NativeActions*>(e.host_->galactic_execution.get()) : nullptr;
        // WGA-484/227: launch can retain a newly allocated fleet, even after a later
        // member refuses. Its identity belongs to the live owner, not perception.
        const auto object_limit = native_actions ? native_actions->next_entity_id() : e.host_->ai_world->next_entity_id();
        check(e.frame_ >= 0 && e.next_plan_id_ && e.next_target_id_ && e.next_taskforce_id_ && e.next_instance_ >= first_plan_instance);
        check(e.game_seed_ == static_cast<std::uint32_t>(e.host_->setup.seed) || !e.initialized_);
        check(e.initialized_ || (e.players_.empty() && running.empty() && e.targets_.empty() && taskforces.empty() && blocks.empty()));
        if (e.initialized_) check(e.players_.size() == static_cast<std::size_t>(std::count_if(e.host_->setup.players.begin(), e.host_->setup.players.end(),
            [&](const auto& player) { return player.ai && (!galactic || !player.neutral) && !player.player_type.empty()
                && e.data_.players.contains(upper_case(player.player_type)); })));
        std::set<std::uint64_t> plan_ids;
        std::set<std::uint64_t> assembly_requests;
        tactical::PlayerId previous{};
        for (const auto& p : e.players_) {
            const auto admission = e.host_->setup.player_ai_start_ticks.find(p.player);
            check(!p.admission_pending || (admission != e.host_->setup.player_ai_start_ticks.end() && admission->second != 0));
            check(p.player > previous && p.galactic == galactic && (!galactic || !p.space_template) && p.type && p.next_goal_id && p.per_frame >= 1
                && p.sleep_frames >= 0 && p.nontrivial >= 0 && p.next_function <= p.functions.size() && p.next_target <= p.targets.size());
            previous = p.player;
            std::uint64_t proposal_holders{};
            for (const auto& [goal, count] : p.proposal_blockers) {
                check(count && std::any_of(p.active.begin(), p.active.end(), [&](const auto& row) { return row.id == goal; }));
                proposal_holders += count;
            }
            check(proposal_holders == p.proposal_blocks);
            if (galactic) {
                // S9-early: the slot is the player's position in the setup's list; ranges only grow.
                check(p.slot < galactic_id_slots && p.slot < e.host_->setup.players.size() && e.host_->setup.players[p.slot].player == p.player
                    && p.next_target_local >= 1 && p.next_target_local <= galactic_id_counter_limit
                    && p.next_taskforce_local >= 1 && p.next_taskforce_local <= galactic_id_counter_limit
                    && p.next_plan_local >= 1 && p.next_plan_local <= galactic_id_counter_limit
                    && p.next_instance_local <= galactic_instance_counter_limit);
            }
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
                    check((!galactic || (e.galactic_plan_issued(potential.id) && galactic_id_slot(potential.id) == p.slot
                        && plan_ids.insert(potential.id).second)) && potential.plan < e.plans_.size());
                    const auto n = potential.units.size();
                    check(potential.freestore.size() == n && potential.sources.size() == n && potential.producers.size() == n
                        && potential.pool_tokens.size() == n && potential.taskforce_of_unit.size() == n && potential.team_of_unit.size() == n);
                    check(galactic || potential.contrast_indices.size() == n);
                    for (auto index : potential.contrast_indices) check(potential.target_contrast.empty() || index < potential.target_contrast.size());
                    for (auto source : potential.sources) check(source <= 2);
                    if (!galactic) for (const auto type : potential.units) check(e.host_->types.contains(type));
                    const auto& definition = e.plans_[potential.plan];
                    check(std::find(definition.goals.begin(), definition.goals.end(), p.functions[g.function].goal_name) != definition.goals.end());
                    for (std::size_t i = 0; i < n; ++i) check(potential.taskforce_of_unit[i] < definition.taskforces.size()
                        && potential.team_of_unit[i] < definition.taskforces[potential.taskforce_of_unit[i]].teams.size());
                }
                if (g.plan) { const auto found = running.find(g.plan); check((g.finished && found == running.end())
                    || (found != running.end() && found->second->goal == g.id && found->second->player == p.player
                        && found->second->target == g.target && (!g.potential || found->second->definition == g.potential->plan))); }
            }
            for (const auto& [object, goal] : p.reserved) check(object && goals.contains(goal)); // pending loss/cancel can lack a live actor
            for (const auto& [object, tf] : p.assigned) check(object && taskforces.contains(tf) && running.at(taskforces.at(tf)->plan)->player == p.player);
            for (const auto& [token, goal] : p.reserved_pool_tokens) check(token && goals.contains(goal));
            for (const auto& [goal, amount] : p.reserved_credits) { static_cast<void>(amount); check(goals.contains(goal)); }
            for (const auto& task : p.tasks) {
                check(task.source <= 2 && taskforces.contains(task.taskforce) && (task.block == 0 || blocks.contains(task.block)));
                if (galactic) {
                    const auto* world = dynamic_cast<const GalacticWorld*>(e.host_->ai_world.get()); check(world);
                    check(!task.associated_producer || (task.source == 2 && task.completion && world->containing_planet(task.associated_producer) == task.associated_producer));
                    check(!task.staging || world->containing_planet(task.staging) == task.staging);
                    check(task.state_entered <= static_cast<std::uint64_t>(e.frame_));
                    using Phase = BuildTask::GalacticAssembly;
                    check(task.galactic_assembly <= Phase::countdown);
                    if (task.galactic_assembly == Phase::none) {
                        check(!task.galactic_assembly_request);
                        check(!task.joined || (task.finished && !task.failed && task.object));
                    } else {
                        check(task.source == 0 && task.staging && task.object && task.block);
                        check(task.finished == (task.galactic_assembly == Phase::complete || task.galactic_assembly == Phase::failed));
                        check(task.failed == (task.galactic_assembly == Phase::failed));
                        const bool waiting = task.galactic_assembly == Phase::waiting || task.galactic_assembly == Phase::countdown;
                        check(!waiting || task.synchronized_staging);
                        check(task.galactic_stage_tick <= static_cast<std::uint64_t>(e.frame_));
                        check(!(task.galactic_stage_seconds < Real{}));
                        check(task.synchronized_staging || (task.galactic_stage_seconds == Real{}
                            && task.galactic_stage_remaining == Real{} && !task.galactic_stage_tick));
                        check((task.galactic_assembly_request != 0) == (task.galactic_assembly == Phase::moving));
                        if (task.galactic_assembly_request) check(assembly_requests.insert(task.galactic_assembly_request).second);
                        check(task.joined || task.failed || waiting); // countdown precedes member insertion
                        if (task.joined && !task.finished) {
                            check(p.assigned.contains(task.object) && p.assigned.at(task.object) == task.taskforce);
                            const auto& members = taskforces.at(task.taskforce)->units;
                            check(std::find(members.begin(), members.end(), task.object) != members.end());
                        }
                        check(blocks.at(task.block)->kind == Block::Kind::produce);
                    }
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
        for (const auto& [id, t] : e.targets_) check(id && id == t.id && (galactic && !e.players_.empty() ? e.galactic_target_issued(id) : id < e.next_target_id_)); // no AI player: engine-wide ids
        for (const auto& [id, entry] : running) {
            const auto& p = *entry;
            const auto* owner = e.player_ai(p.player);
            check(id && id == p.id && plan_ids.insert(id).second && p.definition < e.plans_.size() && owner
                && p.instance >= first_plan_instance
                && (galactic ? e.galactic_plan_issued(id) && galactic_id_slot(id) == owner->slot
                        && e.galactic_instance_issued(p.instance) && galactic_instance_slot(p.instance) == owner->slot
                    : id < e.next_plan_id_ && p.instance < e.next_instance_));
            const auto goal = std::find_if(owner->active.begin(), owner->active.end(), [&](const auto& g) { return g.id == p.goal; });
            check(goal != owner->active.end() && goal->plan == id && goal->target == p.target
                && (!goal->potential || goal->potential->plan == p.definition));
            check(instances.contains(p.instance) && instances.at(p.instance) == id);
            for (const auto tf : p.taskforces) check(taskforces.contains(tf) && taskforces.at(tf)->plan == id);
        }
        check(instances.size() == running.size());
        for (const auto& [id, entry] : taskforces) {
            const auto& tf = *entry;
            check(!tf.galactic_fleet || (galactic && tf.galactic_fleet < object_limit));
            check(id && id == tf.id && running.contains(tf.plan));
            if (galactic) {
                const auto* owner = e.player_ai(running.at(tf.plan)->player);
                check(owner && e.galactic_taskforce_issued(id) && galactic_id_slot(id) == owner->slot);
            } else check(id < e.next_taskforce_id_);
            if (tf.galactic_stage) {
                const auto* world = dynamic_cast<const GalacticWorld*>(e.host_->ai_world.get());
                check(world && view && view->find(tf.galactic_stage)
                    && world->containing_planet(tf.galactic_stage) == tf.galactic_stage);
            }
            const auto& p = *running.at(tf.plan);
            check(tf.definition < e.plans_[p.definition].taskforces.size()
                && std::find(p.taskforces.begin(), p.taskforces.end(), id) != p.taskforces.end());
        }
        for (const auto& [id, entry] : blocks) {
            const auto& b = *entry;
            check((!b.galactic_request || (galactic && (b.kind == Block::Kind::move || b.kind == Block::Kind::galactic_invade)))
            && (galactic || b.kind <= Block::Kind::reinforce)
            && (!b.galactic_actor || (galactic && b.galactic_actor < object_limit)) && id && id == b.id && b.kind <= Block::Kind::galactic_invade && b.order <= Block::Order::guard
            && instances.contains(b.instance) && taskforces.contains(b.taskforce)
            && instances.at(b.instance) == taskforces.at(b.taskforce)->plan
            && (!b.waiting_on || (b.waiting_on != id && blocks.contains(b.waiting_on) && !blocks.at(b.waiting_on)->waiting_on)));
            if (b.kind == Block::Kind::galactic_invade && b.galactic_request) {
                const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(e.host_->galactic_execution.get());
                const auto* invasion = actions ? actions->invasion_status(b.galactic_request) : nullptr;
                check(invasion && invasion->fleet == b.galactic_actor && invasion->invader == running.at(instances.at(b.instance))->player);
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
        Archive ar; ar.galactic_tasks = true;
        std::string tag(domain), identity(content_identity_), configuration = GoalStateCodec::configuration(engine); std::uint32_t version{17};
        ar.fields(tag, version, identity, configuration);
        auto credits = engine.host_->credit_changes;
        GoalStateCodec::runtime(ar, engine, credits);
        return core::Result<Bytes>::success(std::move(ar.bytes));
    } catch (const std::exception& error) { return fail<Bytes>(error.what()); }
}
core::Result<galactic_ai::NativeState> GoalStateContributor::capture() const {
    if (auto verified = verify_record_sync_hash(); !verified) return core::Result<galactic_ai::NativeState>::failure(verified.error());
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
        Archive ar(bytes); ar.galactic_tasks = true;
        std::string tag, identity, configuration; std::uint32_t version{};
        ar.fields(tag, version, identity, configuration);
        ar.check(tag == domain && version == 17 && identity == content_identity_ && configuration == GoalStateCodec::configuration(staged));
        std::map<tactical::PlayerId, math::Fixed> credits;
        GoalStateCodec::runtime(ar, staged, credits); ar.check(ar.input.empty());
        for (const auto& [player, value] : credits) { static_cast<void>(value); ar.check(staged.host_->players.contains(player)); }
        if (auto valid = validate(staged, view.get()); !valid) return valid;
        // Canonical decoding is enforced field-by-field (especially map order).
        // No live owner or host mutation occurs before the full check succeeds.
        const auto native_record_write = engine_->native_record_scope();
        engine_->mark_native_state_changed();
        engine_->native_records_replaced();
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
        Archive ar; std::string tag = "eawr-tactical-ai-v10", content(identity);
        auto& source = const_cast<Engine&>(*this);
        auto configuration = GoalStateCodec::configuration(source);
        auto barrier = scripts.completed_tick(); ar.fields(tag, content, configuration, barrier);
        ar.fields(source.staggered_clock_.paused_frames, source.staggered_clock_.held_since, source.staggered_seeded_);
        auto credits = host_->credit_changes;
        GoalStateCodec::runtime(ar, source, credits);
        GoalStateCodec::grid(ar, source.grid_);
        GoalStateCodec::positioning(ar, source);
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
        ar.check(tag == "eawr-tactical-ai-v10" && content == identity && barrier == scripts.completed_tick()
            && configuration == GoalStateCodec::configuration(staged));
        ar.fields(staged.staggered_clock_.paused_frames, staged.staggered_clock_.held_since, staged.staggered_seeded_);
        ar.check(staged.staggered_clock_.paused_frames <= barrier
            && (!staged.staggered_clock_.held_since || (*staged.staggered_clock_.held_since <= barrier
                && staged.staggered_clock_.paused_frames <= *staged.staggered_clock_.held_since))
            && (staged.staggered() || (!staged.staggered_seeded_ && !staged.staggered_clock_.held_since
                && staged.staggered_clock_.paused_frames == 0)));
        std::map<tactical::PlayerId, math::Fixed> credits;
        GoalStateCodec::runtime(ar, staged, credits); GoalStateCodec::grid(ar, staged.grid_);
        GoalStateCodec::positioning(ar, staged);
        ar.check(ar.input.empty() && (!staged.initialized_ || staged.frame_ == static_cast<std::int64_t>(barrier)));
        // PE-25: the stamps are per session player; a world with another player count cannot own them.
        ar.check(!host_->world || staged.grid_.watched_players() == 0 || staged.grid_.watched_players() == host_->world->players().size());
        GoalStateContributor validation({}, std::string(identity));
        if (auto valid = validation.validate(staged); !valid) return valid;
        const auto instances = scripts.instances();
        for (const auto& [id, row] : staged.ordered_runtime(&PlayerAi::running)) {
            const auto& plan = *row;
            // after_service retains an exited plan until its owner's next execution
            // pass, after the scheduler has already removed the Lua instance.
            static_cast<void>(id);
            const bool present = std::binary_search(instances.begin(), instances.end(), plan.instance);
            ar.check(plan.exited ? !present : present);
        }
        for (const auto& [player, amount] : credits) { static_cast<void>(amount); ar.check(host_->players.contains(player)); }
        GoalStateCodec::publish(*this, staged, true);
        mark_native_state_changed();
        grid_ = std::move(staged.grid_);
        grid_.baseline_fog(*host_); // the completed tick's fog is the next sweep's reference
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
        ai::admission_configuration(ar, setup);
        return core::Result<std::string>::success(core::sha256_hex(ar.bytes));
    } catch (const std::exception& error) { return ai::fail<std::string>(error.what()); }
}
}
