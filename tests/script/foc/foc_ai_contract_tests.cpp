// Contracts of the hosted FoC goal-system pieces (#449, docs/behaviour/foc-tactical-ai.md): the
// perceptual equations (PE-01 to PE-05), the AI random (PE-11), the target name hash (PE-12),
// the threat grid (PG-01 to PG-07), the TaskForce definitions (PL-10 to PL-12) and the
// scheduler reads the engine uses (thread slots, instance removal, command sequences). The
// M2 battle with game data (tests/skirmish/foc_plan_tests.cpp) covers the plan bindings.

#include "ai_engine.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "mode_time.hpp"
#include "galactic_ai_execution.hpp"
#include "galactic_ai_goal_state.hpp"
#include "eawr/core/ascii.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/vfs/vfs.hpp"
#include "eawr/skirmish/ai.hpp"
#include <algorithm>
#include <filesystem>
#include <cstdlib>

#include "eawr/sim/world.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/platform/battle_scoring.hpp"
#include <sstream>
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/combat.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/vfs/vfs.hpp"

#include <iostream>
#include <fstream>
#include <chrono>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <stdexcept>
#include <bit>
#include <cfenv>
#include <clocale>
#include <limits>
#include <tuple>
#include <cstdlib>
#include <filesystem>

namespace eawr::script::foc::ai {
// Fixture setup only; execution and admission use the production paths.
inline std::shared_ptr<Host> make_tactical_host() {
    auto host = std::make_shared<Host>();
    host->ai_world = std::make_shared<TacticalWorld>(*host);
    return host;
}

struct EngineContractAccess {
    static PlayerAi& add_player(Engine& engine, tactical::PlayerId id) {
        PlayerAi row; row.player = id; engine.players_.push_back(std::move(row));

        return engine.players_.back();
    }
    static PlayerAi& fixture_owner(Engine& engine, tactical::PlayerId id = 2) {
        if (auto* owner = engine.player_ai(id)) return *owner;
        return add_player(engine, id);
    }
    static const PlayerAi& fixture_owner(const Engine& engine, tactical::PlayerId id = 2) {
        const auto* owner = engine.player_ai(id);
        if (!owner) throw std::runtime_error("synthetic owner missing");
        return *owner;
    }

    static ThreatGrid& grid(Engine& engine) { return engine.grid_; }
    static void prepare_inputs(Engine& engine, const sim::PartitionExecutor& executor) {
        engine.prepare_producers(&executor); engine.prepare_pad_inputs(&executor);
    }
    static void prepare_pads(Engine& engine, const sim::PartitionExecutor& executor) { engine.prepare_pad_inputs(&executor); }
    static void request_open(Engine& engine, tactical::PlayerId player) { engine.pad_open_requested_.insert(player); }
    static bool has_prepared_open(const Engine& engine, tactical::PlayerId player) {
        if (!engine.pad_inputs_) return false;
        for (const auto& row : *engine.pad_inputs_) for (const auto& buyer : row.buyers)
            if (buyer.player == player && buyer.can_build && buyer.open.has_value()) return true;
        return false;
    }
    static void reset_pads(Engine& engine) { engine.pad_inputs_.reset(); }
    static bool has_prepared_pads(const Engine& engine) { return engine.pad_inputs_.has_value(); }
    static const auto& producers(const Engine& engine, tactical::PlayerId player, tactical::TypeId type) {
        return engine.producers_.at({player, type});
    }
    static void reserve_pad(Engine& engine, sim::EntityId pad, tactical::PlayerId player, tactical::TypeId type) {
        engine.pad_reservations_[pad] = {player, 1, type, false, 0};
    }
    static void release_pad(Engine& engine, sim::EntityId pad) { engine.pad_reservations_.erase(pad); }
    static bool has_prepared_station_levels(const Engine& engine) { return engine.station_levels_.has_value(); }
    static std::optional<Real> prepared_station_level(Engine& engine, tactical::PlayerId player,
        const sim::PartitionExecutor& executor) {
        engine.host_->view = detail::build_view(*engine.host_->world, *engine.host_->snapshot, &executor);
        engine.prepare_station_levels(&executor);
        const auto result = engine.evaluate("Level", player, nullptr);
        engine.station_levels_.reset();
        return result;
    }
    static PlayerAi& collect_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 2;
        player.assigned[14] = 99;
        player.reserved[15] = 7;
        player.reserved[16] = 1;
        engine.players_.push_back(std::move(player));
        Plan plan;
        plan.id = plan.instance = plan.goal = 1;
        plan.player = 2;
        plan.taskforces = {1};
        fixture_owner(engine).running.emplace(1, plan);
        fixture_owner(engine).plan_of_instance[1] = 1;
        TaskForce force;
        force.id = force.plan = 1;
        fixture_owner(engine).taskforces.emplace(1, force);

        engine.rebuild_runtime_index();
        return engine.players_.back();
    }
    static const PlayerAi* player(const Engine& engine, tactical::PlayerId id) { return engine.player_ai(id); }
    static void initialize_goals(Engine& engine, PlayerAi& player) { engine.initialize_goals(player); }
    static void build_targets(Engine& engine, PlayerAi& player) { engine.build_targets(player); }
    static void update_targets(Engine& engine) { engine.update_targets(); }
    static bool matches(const Engine& engine, tactical::PlayerId player, const std::set<std::string>& flags,
        const Target& target) { return engine.target_matches(player, flags, &target); }
    static void target_fixture(Engine& engine, std::uint64_t id, sim::EntityId object) {
        Target target;
        target.id = id;
        target.object = object;
        engine.targets_.emplace(id, target);
    }
    static void order_fixture(Engine& engine, std::vector<sim::EntityId> members) {
        collect_fixture(engine);
        fixture_owner(engine).taskforces.at(1).units = std::move(members);
        target_fixture(engine, 1, 11);
        target_fixture(engine, 2, 0);
        target_fixture(engine, 3, 999);
    }
    static void proposal_fixture(Engine& engine) {
        order_fixture(engine, {10, 11, 12});
        Plan plan; plan.id = plan.instance = plan.goal = 2; plan.player = 2; plan.taskforces = {2};
        fixture_owner(engine).running.emplace(2, plan); fixture_owner(engine).plan_of_instance[2] = 2;
        TaskForce force; force.id = force.plan = 2; force.units = {10};
        fixture_owner(engine).taskforces.emplace(2, force);

        engine.rebuild_runtime_index();
    }
    static void add_find_target(Engine& engine, std::uint64_t id, sim::EntityId object) {
        target_fixture(engine, id, object); engine.players_.back().targets.push_back(id);
    }
    static void find_target_fixture(Engine& engine) {
        order_fixture(engine, {10});
        engine.players_.back().targets = {1, 2, 3};
    }
    static void propose(Engine& engine, PlayerAi& player) { engine.propose(player); }
    static std::uint32_t evaluated(const Engine& engine) { return engine.work_.goals_evaluated; }
    static void goals(Engine& engine, PlayerAi& player) { engine.service_goals(player); }
    static std::uint32_t maintenances(const Engine& engine) { return engine.work_.maintenances; }
    static std::int64_t admission_budget(const Engine& engine) { return engine.proposal_budget(100000); }
    static void maintain(Engine& engine, PlayerAi& player) { engine.maintain(player); }
    static PlayerAi& active_desire_fixture(Engine& engine, const GoalType& goal,
        const GoalType& competitor, bool removable, bool proposals) {
        PlayerAi player; player.player = 2;
        player.functions = {{&goal, "RETAINED", engine.data_.equations.find("Desire"), "Desire"},
            {&competitor, "COMPETITOR", engine.data_.equations.find("Positive"), "Positive"}};
        Goal active; active.id = active.plan = active.target = 1;
        active.active = true; active.desire = real(123);
        player.active.push_back(active);
        if (proposals) {
            Goal candidate; candidate.id = 2; candidate.function = 1; candidate.desire = real(10);
            player.proposed.push_back(candidate);
        }
        engine.players_.push_back(std::move(player));
        Plan plan; plan.id = plan.instance = plan.goal = 1; plan.player = 2;
        plan.removable = removable;
        fixture_owner(engine).running.emplace(1, plan); fixture_owner(engine).plan_of_instance[1] = 1;

        engine.rebuild_runtime_index();
        return engine.players_.back();
    }
    static void contrast_fixture(Engine& engine, PlayerAi& player) {
        auto& goal = player.active.front();
        PotentialPlan selected; selected.valid = selected.reserved = selected.funded = true;
        selected.units = {1}; selected.freestore = {10}; selected.sources = {0};
        selected.producers = {0}; selected.pool_tokens = {0};
        selected.taskforce_of_unit = selected.team_of_unit = selected.contrast_indices = {0};
        goal.potential = selected;
        fixture_owner(engine).running.at(1).taskforces = {1};
        TaskForce force; force.id = force.plan = 1; force.units = {10};
        fixture_owner(engine).taskforces.emplace(1, force);
        player.assigned[10] = 1; player.reserved[10] = goal.id;

        engine.rebuild_runtime_index();
    }
    static bool contrast(Engine& engine, PlayerAi& player) {
        return engine.test_target_contrast(player, player.active.front());
    }
    static void retain_pad(Engine& engine, std::uint64_t pad) { fixture_owner(engine).running.at(1).reserved_pads = {pad}; }
    static void release_retained_pad(Engine& engine) { fixture_owner(engine).running.at(1).reserved_pads.clear(); }
    static bool completed_pad_contrast(Engine& engine, PlayerAi& player) {
        Goal goal; goal.id = goal.plan = 1; goal.potential.emplace();
        auto& selection = *goal.potential; selection.valid = selection.reserved = selection.funded = true;
        selection.units = {30}; selection.freestore = {0}; selection.sources = {2};
        selection.producers = {1}; selection.pool_tokens = {0}; selection.contrast_indices = {0};
        return engine.test_target_contrast(player, goal);
    }
    static bool selection(Engine& engine, PlayerAi& player, PotentialPlan& potential, std::uint32_t seed,
        std::uint64_t goal_id = 1) {
        player.sync.set_seed(seed);
        Goal goal;
        goal.id = goal_id;
        return engine.select_units(player, goal, potential);
    }
    static const PlayerAi& checked_player(const Engine& engine, tactical::PlayerId id) {
        const auto* player = engine.player_ai(id);
        if (!player) throw std::runtime_error("synthetic AI player missing");
        return *player;
    }
    static PlayerAi& checked_player(Engine& engine, tactical::PlayerId id) {
        auto* player = engine.player_ai(id);
        if (!player) throw std::runtime_error("synthetic AI player missing");
        return *player;
    }
    static std::uint32_t selection_seed(const Engine& engine, tactical::PlayerId player) { return checked_player(engine, player).sync.seed(); }
    static void set_selection_seed(Engine& engine, tactical::PlayerId player, std::uint32_t seed) { checked_player(engine, player).sync.set_seed(seed); }
    static std::uint32_t engine_seed(const Engine& engine) { return engine.sync_.seed(); }
    static std::uint32_t draw(Engine& engine, tactical::PlayerId player) { return engine.stream(checked_player(engine, player)).next(); }
    static void retain_plan_without_instance(Engine& engine, tactical::PlayerId id, bool exited) {
        auto& player = checked_player(engine, id);
        Goal goal; goal.id = 1; goal.plan = 1;
        player.active.clear(); player.active.push_back(goal); player.next_goal_id = 2;
        Plan plan; plan.id = plan.goal = 1; plan.player = id;
        plan.instance = first_plan_instance; plan.exited = exited;
        player.running[1] = plan; player.plan_of_instance[plan.instance] = 1;
        engine.next_plan_id_ = 2; engine.next_instance_ = first_plan_instance + 1;
        player.positioning[10] = {plan.instance, engine.frame_ + 75, 11};

        engine.rebuild_runtime_index();
    }
    static void ownership_fixture(Engine& engine, authoritative::ScriptScheduler& scripts) {
        add_player(engine, 7); add_player(engine, 9);
        engine.scripts_ = &scripts; engine.event_tick_ = 1;
        for (const std::uint64_t id : {3U, 1U, 2U}) {
            auto& owner = checked_player(engine, id == 2 ? 7 : 9);
            Plan plan; plan.id = id; plan.instance = 100 + id; plan.player = owner.player;
            plan.taskforces = {id}; owner.running.emplace(id, plan);
            owner.plan_of_instance.emplace(plan.instance, id);
            TaskForce force; force.id = force.plan = id; force.name = "MainForce";
            force.units = {999}; owner.taskforces.emplace(id, std::move(force));
        }
        auto& owner = checked_player(engine, 9);
        Block active; active.id = 1101; active.instance = 101; active.taskforce = 1;
        owner.blocks.emplace(active.id, active);
        Block alias = active; alias.id = 1100; alias.waiting_on = active.id;
        owner.blocks.emplace(alias.id, alias);
        engine.rebuild_runtime_index();
    }
    static void foreign_task(Engine& engine) {
        BuildTask task; task.taskforce = 2; task.source = 1; task.type = 1; task.pool_token = 77;
        auto& owner = checked_player(engine, 9); owner.tasks.push_back(task); engine.service_execution(owner);
    }
    static void restore_foreign_units(Engine& engine) { checked_player(engine, 7).taskforces.at(2).units = {999}; }
    // ---- S-T3b: player effects. The reference engine writes directly; the other runs the same turns staged. ----
    template<class Body> static void turn(Engine& engine, tactical::PlayerId id, Body&& body) {
        auto& player = checked_player(engine, id);
        const Engine::TurnScope scope(engine, player);
        body(player);
    }
    static core::Result<void> commit(Engine& engine, authoritative::ScriptScheduler& scripts) { return engine.commit_effects(scripts); }
    static bool staging(const Engine& engine) { return engine.staged() != nullptr; }
    static const EffectCounts& totals(const Engine& engine) { return engine.effect_totals_; }
    static const PlayerEffects& effects_of(const Engine& engine, tactical::PlayerId id) { return checked_player(engine, id).effects; }
    static void emit(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::uint64_t force_id, std::string_view event) {
        auto& player = checked_player(engine, owner);
        engine.emit_call(player.running.at(plan_id), player.taskforces.at(force_id), event, {});
    }
    static void signal(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::uint64_t force_id, std::string_view event) {
        auto& player = checked_player(engine, owner);
        player.taskforces.at(force_id).thread = 0;
        engine.emit_thread_event(player.running.at(plan_id), player.taskforces.at(force_id), event, {});
    }
    static void move(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, const ViewUnit& unit) {
        engine.issue_move(checked_player(engine, owner).running.at(plan_id), unit, math::Vec3{});
    }
    static void note(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::string text) {
        engine.record(&checked_player(engine, owner).running.at(plan_id), owner, "effects", std::move(text));
    }
    static void wallet(Engine& engine, tactical::PlayerId player, Real amount) { engine.change_wallet(player, amount); }
    static std::int64_t stacked(const Engine& engine, tactical::PlayerId player) { return engine.stacked_credits(player).raw(); }
    static std::int64_t host_stack(const Engine& engine, tactical::PlayerId player) { return engine.host_->uncommitted_credits(player); }
    static void claim_pad(Engine& engine, sim::EntityId pad, tactical::PlayerId player, std::uint64_t goal) {
        engine.set_pad_claim(pad, PadReservation{player, goal, 30});
    }
    static std::optional<PadReservation> claim(const Engine& engine, sim::EntityId pad) {
        const auto* held = engine.pad_claim(pad);
        return held ? std::optional<PadReservation>(*held) : std::nullopt;
    }
    static void free_pads(Engine& engine, tactical::PlayerId player, std::uint64_t goal) { engine.release_pad_claims(player, goal); }
    static void free_pad(Engine& engine, sim::EntityId pad) { engine.release_pad_claim(pad); }
    static void wait_for_attach(Engine& engine, PlayerAi& player, std::uint64_t goal_id) {
        Goal goal; goal.id = goal_id; engine.queue_attach(player, goal);
    }
    static void reindex(Engine& engine) { engine.rebuild_runtime_index(); }
    static void backlog(Engine& engine, tactical::PlayerId player, std::uint64_t goal) { engine.pending_attach_.push_back({player, goal}); }
    static void finish(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(checked_player(engine, owner), plan_id, scripts, true);
    }
    static void execute(Engine& engine, tactical::PlayerId owner) { engine.service_execution(checked_player(engine, owner)); }
    static void pool_task(Engine& engine, tactical::PlayerId owner, std::uint64_t force, std::uint64_t token) {
        BuildTask task; task.taskforce = force; task.source = 1; task.type = 1; task.pool_token = token;
        checked_player(engine, owner).tasks.push_back(task);
    }
    // A goal the player's active list holds; an attachable one carries a potential plan of the first PlanDef.
    static void active_goal(Engine& engine, tactical::PlayerId id, std::uint64_t goal_id, bool attachable) {
        Goal goal; goal.id = goal_id;
        if (attachable) goal.potential = PotentialPlan{};
        checked_player(engine, id).active.push_back(std::move(goal));
    }
    // Inside a turn: a scheduler event (submitted at the commit) of the given tick, and an engine order.
    static void stage_event(Engine& engine, std::uint64_t tick) {
        authoritative::ScriptEvent event;
        event.key = {tick, producer_foc_engine, 1, 0};
        event.target = 1;
        event.kind = authoritative::ScriptEvent::Kind::pump;
        engine.staged()->scheduler.push_back(PlayerEffects::SchedulerEvent{std::move(event), true});
    }
    static void stage_order(Engine& engine) { engine.queue_order(authoritative::ScriptCommand{}); }
    static void bare_players(Engine& engine, authoritative::ScriptScheduler& scripts) {
        for (const tactical::PlayerId id : {7U, 9U}) {
            add_player(engine, id).functions.push_back(GoalFunctionEntry{nullptr, "ATTACH_GOAL", nullptr, "Attach"});
        }
        engine.scripts_ = &scripts; engine.event_tick_ = 1;
    }
    static core::Result<void> drain(Engine& engine, authoritative::ScriptScheduler& scripts) {
        std::uint64_t sequence = 0;
        return engine.drain_attaches(scripts, sequence);
    }
    static std::string goal_plans(const Engine& engine) {
        std::ostringstream out;
        for (const auto& player : engine.players_) for (const auto& goal : player.active)
            out << 'G' << player.player << ',' << goal.id << ':' << goal.plan << ';';
        return out.str();
    }
    static bool attach_throws(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        Goal goal; std::uint64_t sequence = 0;
        try { static_cast<void>(engine.attach_plan(player, goal, scripts, sequence)); } catch (const std::logic_error&) { return true; }
        return false;
    }
    static std::size_t waiting_events(const Engine& engine) { return engine.events_.size(); }
    static std::size_t waiting_orders(const Engine& engine) { return engine.orders_.size(); }
    static std::size_t waiting_attaches(const Engine& engine) { return engine.pending_attach_.size(); }
    static std::size_t forced_units(const Engine& engine, std::uint64_t force) {
        const auto* row = engine.taskforce(force); return row ? row->pooled.size() : 0;
    }
    static std::vector<std::uint64_t> pooled_tokens(const Engine& engine, std::uint64_t force) {
        std::vector<std::uint64_t> tokens;
        if (const auto* row = engine.taskforce(force)) for (const auto& purchase : row->pooled) tokens.push_back(purchase.token);
        return tokens;
    }
    static void arm_native_records(Engine& engine, bool on) { engine.native_records_enabled_ = on; }
    static void hold_native_scope(std::optional<Engine::NativeRecordWrite>& slot, Engine& engine) { slot.emplace(engine); }
    static std::size_t committed_claims(const Engine& engine) { return engine.pad_reservations_.size(); }
    static std::size_t committed_orders(const Engine& engine) { return engine.orders_.size(); }
    static std::size_t staged_effects(const Engine& engine) {
        std::size_t staged = 0;
        for (const auto& player : engine.players_) staged += player.effects.empty() ? 0 : 1;
        return staged;
    }
    static std::size_t committed_wallets(const Engine& engine) { return engine.host_->credit_changes.size(); }
    static void native_changes(Engine& engine, std::uint32_t owner) {
        const auto write = engine.native_record_scope();
        engine.native_records_changed(owner, {1, 13});
        engine.native_record_changed(owner, 2, 5 + owner);
        engine.mark_native_state_changed();
        engine.mark_native_state_changed();
    }
    static void native_shared_change(Engine& engine) {
        engine.native_records_changed(Engine::native_all_players, {32});
        engine.native_record_changed(0, 20, 9);
        engine.mark_native_state_changed();
    }
    static std::size_t native_depth(const Engine& engine) { return engine.native_record_depth_; }
    static std::string native_snapshot(const Engine& engine) {
        std::ostringstream out;
        for (const auto& [owner, bits] : engine.native_record_dirty_groups_) out << 'G' << owner << ':' << bits << ';';
        for (const auto& key : engine.native_record_dirty_keys_) out << 'K' << std::get<0>(key) << ',' << std::get<1>(key) << ',' << std::get<2>(key).size() << ';';
        out << "R" << engine.native_revision_ << ',' << engine.native_record_header_dirty_ << ',' << engine.native_record_depth_;
        return out.str();
    }
    static std::string snapshot(Engine& engine, authoritative::ScriptScheduler& scripts) {
        std::ostringstream out;
        for (const auto& event : engine.events_) {
            out << 'E' << event.key.tick << ',' << event.key.producer << ',' << event.key.entity << ',' << event.key.sequence << ','
                << static_cast<int>(event.kind) << ',' << event.name << ',' << event.target << ',' << event.thread << ','
                << event.arguments.size() << ';';
        }
        for (const auto& order : engine.orders_)
            out << 'O' << order.tick << ',' << order.issuer << ',' << order.sequence << ',' << order.verb << ',' << order.arguments.size() << ';';
        for (const auto& [player, change] : engine.host_->credit_changes) out << 'C' << player << ',' << change.raw() << ';';
        for (const auto& [pad, held] : engine.pad_reservations_)
            out << 'P' << pad << ',' << held.player << ',' << held.goal << ',' << held.type << ',' << held.consumed << ',' << held.issued << ';';
        for (const auto& wait : engine.pending_attach_) out << 'A' << wait.player << ',' << wait.goal << ';';
        for (const auto& line : engine.records_)
            out << 'R' << line.tick << ',' << line.player << ',' << line.plan << ',' << line.goal << ',' << line.target << ',' << line.event << ',' << line.detail << ';';
        if (const auto& journal = engine.host_->setup.journal) {
            for (const auto& row : journal->plans)
                out << 'J' << row.tick << ',' << row.player << ',' << row.plan << ',' << row.goal << ',' << row.target << ',' << row.event << ',' << row.detail << ';';
            for (const auto& row : journal->services)
                out << 'V' << row.frame << ',' << row.player << ',' << row.goals << row.planning << row.execution << ';';
        }
        out << 'S' << engine.sequence_ << ',' << engine.order_sequence_ << ',' << engine.native_revision_ << ','
            << engine.work_.goals_evaluated << ',' << engine.work_.maintenances << ',' << engine.work_.plans_pumped << ','
            << engine.producer_work_.candidates << ',' << engine.records_dropped_ << ';';
        for (const auto& [id, owner] : engine.runtime_index_.plans) out << "Ip" << id << ',' << owner << ';';
        for (const auto& [id, owner] : engine.runtime_index_.taskforces) out << "It" << id << ',' << owner << ';';
        for (const auto& [id, owner] : engine.runtime_index_.blocks) out << "Ib" << id << ',' << owner << ';';
        for (const auto& [id, owner] : engine.runtime_index_.instances) out << "Ii" << id << ',' << owner << ';';
        for (const auto instance : scripts.instances()) out << "Si" << instance << ';';
        return out.str();
    }
    static void orphan_blocks_fixture(Engine& engine) {
        auto& owner = fixture_owner(engine);
        for (const auto kind : {Block::Kind::produce, Block::Kind::reinforce, Block::Kind::ambush, Block::Kind::move}) {
            Block row; row.id = static_cast<std::uint64_t>(kind) + 1; row.taskforce = 999; row.kind = kind;
            row.ready = true; owner.blocks.emplace(row.id, row);
        }
        engine.rebuild_runtime_index();
    }
    static void interleaved_native_fixture(Engine& engine) {
        for (const std::uint64_t id : {3U, 1U, 2U}) {
            auto& owner = checked_player(engine, id == 2 ? 7 : 9);
            Goal goal; goal.id = id == 3 ? 2 : 1; goal.plan = id; owner.active.push_back(goal);
            owner.next_goal_id = std::max(owner.next_goal_id, goal.id + 1);
            Plan plan; plan.id = id; plan.goal = goal.id; plan.instance = first_plan_instance + id - 1;
            plan.player = owner.player; plan.exited = true; plan.taskforces = {id}; owner.running.emplace(id, plan);
            owner.plan_of_instance.emplace(plan.instance, id);
            TaskForce force; force.id = force.plan = id; force.name = "MainForce"; owner.taskforces.emplace(id, force);
            Block row; row.id = block_id(plan.instance, 17); row.instance = plan.instance; row.taskforce = id;
            owner.blocks.emplace(row.id, row);
        }
        engine.next_plan_id_ = engine.next_taskforce_id_ = 4;
        engine.next_instance_ = first_plan_instance + 3;
        checked_player(engine, 9).positioning[10] = {first_plan_instance, engine.frame_ + 1, 0};
        checked_player(engine, 7).positioning[12] = {first_plan_instance + 1, engine.frame_ + 1, 0};
        engine.rebuild_runtime_index();
    }
    static void stage_movement_queries(Engine& engine) {
        auto& first = checked_player(engine, 9).blocks.at(1100);
        auto& second = checked_player(engine, 9).blocks.at(1101);
        first.galactic_request = 200; second.galactic_request = 100;
        Block row; row.id = 1099; row.galactic_request = 300;
        checked_player(engine, 7).blocks.emplace(row.id, row);
        engine.rebuild_runtime_index();
    }
    static void clear_movement_queries(Engine& engine) {
        checked_player(engine, 9).blocks.at(1100).galactic_request = 0;
        checked_player(engine, 9).blocks.at(1101).galactic_request = 0;
        checked_player(engine, 7).blocks.erase(1099);
        engine.rebuild_runtime_index();
    }
    static void depart_positioning_plan(Engine& engine, tactical::PlayerId id) {
        auto& owner = checked_player(engine, id);
        owner.running.clear(); owner.plan_of_instance.clear(); owner.active.clear();

        engine.rebuild_runtime_index();
    }
    static core::Result<void> attach(Engine& engine, PlayerAi& player, Goal& goal,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.event_tick_ = 1;
        engine.sequence_ = sequence;
        engine.scripts_ = &scripts;
        return engine.attach_plan(player, goal, scripts, sequence);
    }
    static const std::vector<authoritative::ScriptEvent>& attachment_events(const Engine& engine) {
        return engine.events_;
    }
    static std::vector<authoritative::ScriptEvent> take_attachment_events(Engine& engine) {
        auto events = std::move(engine.events_); engine.events_.clear(); return events;
    }
    static void defer_event(Engine& engine, authoritative::ScriptEvent event) {
        engine.events_.push_back(std::move(event));
    }
    static std::vector<sim::EntityId> eligible(Engine& engine, const PlayerAi& player,
        const Goal& goal, const PlanDef& plan) {
        std::vector<sim::EntityId> units;
        for (const ViewUnit* unit : engine.freestore_list(player, goal, plan)) units.push_back(unit->id);
        return units;
    }
    static std::vector<authoritative::ScriptEvent> hazard_events(Engine& engine,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.scripts_ = &scripts;
        engine.sequence_ = sequence;
        engine.event_tick_ = 1;
        Plan plan;
        plan.id = plan.instance = 1;
        plan.player = 1;
        plan.taskforces = {1};
        fixture_owner(engine, 1).running.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        force.name = "MainForce";
        force.units = {7};
        fixture_owner(engine, 1).taskforces.emplace(1, force);
        engine.service_taskforce_events();

        engine.rebuild_runtime_index();
        return engine.events_;
    }
    static void event_force(Engine& engine, authoritative::ScriptScheduler& scripts, std::vector<sim::EntityId> members) {
        engine.scripts_ = &scripts;
        engine.sequence_ = 0;
        engine.event_tick_ = 1;
        Plan plan;
        plan.id = plan.instance = 1;
        plan.player = 1;
        plan.taskforces = {1};
        fixture_owner(engine, 1).running.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        force.name = "MainForce";
        force.units = std::move(members);
        fixture_owner(engine, 1).taskforces.emplace(1, force);

        engine.rebuild_runtime_index();
    }
    static std::vector<authoritative::ScriptEvent> run_events(Engine& engine) {
        engine.events_.clear();
        engine.service_taskforce_events();
        return engine.events_;
    }
    static void seed(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        engine.scripts_ = &scripts;
        Plan plan;
        plan.id = plan.instance = plan.goal = 1;
        plan.player = player.player;
        plan.taskforces = {1};
        plan.reserved_pads = {1};
        player.running.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        player.taskforces.emplace(1, force);
        Block block;
        block.id = block.taskforce = 1;
        block.kind = Block::Kind::produce;
        player.blocks.emplace(1, block);
        BuildTask task;
        task.taskforce = task.block = 1;
        task.type = 30;
        task.source = 2;
        task.producer = 1;
        player.tasks.push_back(task);
        player.reserved_credits[1] = real(100);
        engine.pad_reservations_[1] = {player.player, 1, 30};

        engine.rebuild_runtime_index();
    }
    static void service(Engine& engine, PlayerAi& player, std::int64_t frame) {
        engine.frame_ = frame;
        if (engine.host_->ai_world) engine.observe_purchases(player, *engine.host_->ai_world);
        engine.service_execution(player);
    }
    static PlayerAi& rejected_station_fixture(Engine& engine) {
        auto& player = collect_fixture(engine);
        Block block; block.id = block.taskforce = 1; block.kind = Block::Kind::produce;
        fixture_owner(engine).blocks.emplace(1, block);
        BuildTask task; task.taskforce = task.block = 1; task.type = 50; task.source = 2;
        task.issued = 1; task.finished = task.failed = true;
        player.tasks.push_back(task);

        engine.rebuild_runtime_index();
        return player;
    }
    static void direct_pad_service(Engine& engine, PlayerAi& player) { engine.service_execution(player, true); }
    static void reserve(Engine& engine, PlayerAi& player, Goal& goal) { engine.reserve(player, goal); }
    static Goal& pool_goal(Engine& engine, PlayerAi& player, std::vector<std::uint64_t> tokens, bool active, bool non_pool = false) {
        Goal goal; goal.id = player.next_goal_id++; goal.active = active; goal.potential.emplace();
        auto& selection = *goal.potential; selection.valid = true;
        selection.units.assign(tokens.size(), 1); selection.sources.assign(tokens.size(), 1);
        selection.freestore.assign(tokens.size(), 0); selection.producers.assign(tokens.size(), 0);
        selection.taskforce_of_unit.assign(tokens.size(), 0); selection.team_of_unit.assign(tokens.size(), 0);
        selection.contrast_indices.assign(tokens.size(), 0); selection.pool_tokens = std::move(tokens);
        if (non_pool) {
            // Ignored token metadata on new production must not release a pool
            // reservation of another type, even when the numeric token matches.
            selection.units.insert(selection.units.begin(), 2); selection.sources.insert(selection.sources.begin(), 2);
            selection.freestore.insert(selection.freestore.begin(), 0); selection.producers.insert(selection.producers.begin(), 0);
            selection.taskforce_of_unit.insert(selection.taskforce_of_unit.begin(), 0);
            selection.team_of_unit.insert(selection.team_of_unit.begin(), 0);
            selection.contrast_indices.insert(selection.contrast_indices.begin(), 0);
            selection.pool_tokens.insert(selection.pool_tokens.begin(), 78);
        }
        auto& retained = (active ? player.active : player.proposed).emplace_back(std::move(goal));
        engine.reserve(player, retained);
        return retained;
    }
    static void retire_pool_goal(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts, bool abandoned) {
        engine.finish_plan(player, player.active.front().plan, scripts, abandoned);
        player.active.clear();
    }
    static void await_pool_arrival(Engine& engine, PlayerAi& player, std::uint64_t plan_id) {
        const auto& plan = player.running.at(plan_id);
        Block block; block.kind = Block::Kind::reinforce; block.instance = plan.instance;
        block.id = block_id(plan.instance, 19); block.taskforce = plan.taskforces.front();
        block.reinforcing = 1; block.pool_token = 77;
        player.blocks.emplace(block.id, block);
        engine.rebuild_runtime_index();
    }
    static void bind_scripts(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.scripts_ = &scripts; engine.sequence_ = sequence; engine.event_tick_ = scripts.completed_tick() + 1;
    }
    static void pause_planning(Engine& engine) {
        engine.initialized_ = true;
        // This fixture installs its own already-seeded deadlines. Production
        // startup intentionally leaves them unseeded until its first enabled turn.
        engine.staggered_seeded_ = true;
        for (auto& player : engine.players_) {
            player.next_goal = player.next_planning = player.next_execution = player.next_learning = 1000000;
        }
    }
    static void release(Engine& engine, PlayerAi& player, Goal& goal, bool refund = true) {
        engine.release(player, goal, refund);
    }
    static void perception_executor(Engine& engine, const sim::PartitionExecutor* executor) {
        engine.perception_executor_ = executor;
    }
    static std::vector<authoritative::ScriptCommand> take_orders(Engine& engine) {
        auto orders = std::move(engine.orders_); engine.orders_.clear(); return orders;
    }
    static void return_orders(Engine& engine, authoritative::ServiceReport& report) {
        engine.orders_ = std::move(report.commands);
    }
    static void purchase(Engine& engine, PlayerAi& player, const Goal& goal,
        tactical::TypeId type, sim::EntityId producer) {
        Plan plan; plan.id = plan.instance = plan.goal = goal.id; plan.player = player.player;
        if (engine.host_->world->pads().contains(producer)) plan.reserved_pads = {producer};
        plan.taskforces = {goal.id}; player.running[goal.id] = plan;
        TaskForce force; force.id = force.plan = goal.id; player.taskforces[goal.id] = force;
        Block block; block.id = block.taskforce = goal.id; block.kind = Block::Kind::produce;
        player.blocks[goal.id] = block;
        BuildTask task; task.taskforce = task.block = goal.id; task.type = type;
        task.producer = producer; task.source = 2; player.tasks.push_back(task);

        engine.rebuild_runtime_index();
    }
    static void abandon_goal(Engine& engine, PlayerAi& player, std::uint64_t id, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(player, id, scripts, true);
    }
    static PlayerAi& direct_pad_fixture(Engine& engine, tactical::TypeId type, sim::EntityId pad) {
        auto& owner = fixture_owner(engine);
        Goal goal; goal.id = 1; purchase(engine, owner, goal, type, pad);
        owner.tasks.clear();
        owner.plan_of_instance[1] = 1; owner.running.at(1).reserved_pads = {pad};
        auto& force = owner.taskforces.at(1); force.types = {type}; force.sources = {2}; force.producers = {pad};
        engine.frame_ = static_cast<std::int64_t>(engine.host_->world->completed_tick());

        engine.rebuild_runtime_index();
        return owner;
    }
    static void seed_purchases(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        seed(engine, player, scripts);
        player.running.at(1).reserved_pads.clear();
        player.tasks.front().type = 50;
        auto later = player.tasks.front();
        later.type = 51;
        player.tasks.push_back(later);
    }
    static std::size_t orders(const Engine& engine) { return engine.orders_.size(); }
    static void seed_reinforcements(Engine& engine, PlayerAi& player, const std::uint64_t count, const bool large_block = false) {
        for (std::uint64_t id = 1; id <= count; ++id) {
            Plan plan;
            plan.id = plan.instance = id;
            plan.player = player.player;
            plan.taskforces = {id};
            player.running.emplace(id, plan);
            TaskForce force;
            force.id = force.plan = id;
            force.pooled = {{1, id * 2 - 1}, {1, id * 2}};
            player.taskforces.emplace(id, force);
            Block block;
            block.id = large_block ? block_id(first_plan_instance + id, 17) : id;
            block.taskforce = id;
            block.kind = Block::Kind::reinforce;
            player.blocks.emplace(block.id, block);
            player.reserved_pool[1] += 2;
            player.reserved_pool_tokens[id * 2 - 1] = id;
            player.reserved_pool_tokens[id * 2] = id;
        }

        engine.rebuild_runtime_index();
    }
    static const std::vector<authoritative::ScriptCommand>& reinforce_service(
        Engine& engine, PlayerAi& player, const std::int64_t frame) {
        engine.frame_ = frame;
        engine.work_ = {};
        engine.orders_.clear();
        engine.service_reinforcements(player);
        return engine.orders_;
    }
    static std::uint32_t reinforcement_candidates(const Engine& engine) {
        return engine.work_.reinforcement_candidates;
    }
    static PlayerAi& duplicate_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 1;
        seed_reinforcements(engine, player, 1);
        player.blocks.clear();
        player.plan_of_instance[1] = 1;
        engine.players_.push_back(std::move(player));

        engine.rebuild_runtime_index();
        return engine.players_.back();
    }
    static PlayerAi& concurrent_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 1;
        seed_reinforcements(engine, player, 2);
        engine.players_.push_back(std::move(player));

        engine.rebuild_runtime_index();
        return engine.players_.back();
    }
    static bool in_freestore(const Engine& engine, const ViewUnit& unit) {
        return engine.in_freestore(unit, 1);
    }
    static void abandon(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(player, 1, scripts, true);
    }
    static bool test_valid(Engine& engine, PlayerAi& player, Goal& goal) {
        engine.frame_ = 1;
        return engine.test_valid(player, goal);
    }
    static void reinforce_at(Engine& engine, const math::Vec3& position) {
        fixture_owner(engine, 1).blocks.at(block_id(first_plan_instance + 1, 17)).destination = position;
    }
    static void movement_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence) {
        engine.scripts_ = &scripts;
        engine.sequence_ = sequence;
        engine.event_tick_ = 1;
        Plan plan;
        plan.id = plan.instance = 1;
        plan.player = 1;
        plan.taskforces = {1};
        fixture_owner(engine, 1).running.emplace(1, plan);
        TaskForce force;
        force.id = force.plan = 1;
        force.name = "MainForce";
        force.units = {10};
        fixture_owner(engine, 1).taskforces.emplace(1, force);
        for (const auto kind : {Block::Kind::move, Block::Kind::ambush}) {
            Block block;
            block.id = kind == Block::Kind::move ? 1 : 2;
            block.taskforce = 1;
            block.kind = kind;
            block.destination_object = kind == Block::Kind::ambush ? 20 : 0;
            block.movers = {10};
            block.ordered_tick[10] = 0;
            fixture_owner(engine, 1).blocks.emplace(block.id, std::move(block));
        }

        engine.rebuild_runtime_index();
    }
    static void movement_service(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_blocks();
    }
    static std::uint64_t event_sequence(const Engine& engine) { return engine.sequence_; }
    static bool movement_finished(const Engine& engine, std::uint64_t id) {
        return fixture_owner(engine, 1).blocks.at(id).finished;
    }
    static void guard_fixture(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        movement_fixture(engine, scripts, sequence);
        auto& guard = fixture_owner(engine, 1).blocks.at(1);
        guard.order = Block::Order::guard;
        guard.destination_object = 20;
        auto& move = fixture_owner(engine, 1).blocks.at(2);
        move.kind = Block::Kind::move;
        move.order = Block::Order::move;
    }
    static void empty_guard_force(Engine& engine) { fixture_owner(engine, 1).taskforces.at(1).units.clear(); }
    // EX-31 (#3087): one attack block of player 1's force {10} on `target`, ordered at frame 0.
    static const Block& attack_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
        std::uint64_t& sequence, sim::EntityId target) {
        movement_fixture(engine, scripts, sequence);
        fixture_owner(engine, 1).blocks.clear();
        engine.frame_ = 0;
        Block block;
        block.id = 1;
        block.kind = Block::Kind::move;
        block.taskforce = 1;
        block.order = Block::Order::attack;
        block.attack = true;
        block.destination_object = target;
        engine.order_block(block, fixture_owner(engine, 1).taskforces.at(1), fixture_owner(engine, 1).running.at(1));

        engine.rebuild_runtime_index();
        return fixture_owner(engine, 1).blocks.emplace(1, std::move(block)).first->second;
    }
    static std::size_t order_count(const Engine& engine) { return engine.orders_.size(); }
    static void point_defence(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_point_defence();
    }
    static void corrupt_systems(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_corrupt_systems();
    }
    static std::size_t point_defence_orders(const Engine& engine) { return engine.orders_.size(); }
    static const authoritative::ScriptCommand* last_order(const Engine& engine) {
        return engine.orders_.empty() ? nullptr : &engine.orders_.back();
    }
    // WTA-23 to WTA-28: plan instance first_plan_instance of player 2 runs TaskForce 1 with unit 30;
    // a perception grid of 20 x 20 cells of 400 over -4000..4000.
    static void positioning_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 2;
        engine.players_.push_back(std::move(player));
        PlanDef definition;
        definition.name = "DestroyUnit";
        engine.plans_.push_back(std::move(definition));
        Plan plan;
        plan.id = 1;
        plan.instance = first_plan_instance;
        plan.player = 2;
        plan.taskforces = {1};
        fixture_owner(engine).running.emplace(1, plan);
        fixture_owner(engine).plan_of_instance.emplace(first_plan_instance, 1);
        TaskForce force;
        force.id = force.plan = 1;
        force.name = "MainForce";
        force.units = {30};
        fixture_owner(engine).taskforces.emplace(1, force);
        const AiBounds bounds{real(-4000), real(4000), real(4000), real(-4000)};
        engine.grid_.partition(bounds, 20, 20, Constants{});
        engine.grid_.service(*engine.host_->view, *engine.host_, 0);

        engine.rebuild_runtime_index();
    }
    // The plan script's Enable_Attack_Positioning request, as after_service takes it.
    static void enable_positioning(Engine& engine, std::int64_t frame, bool on) {
        engine.frame_ = frame;
        authoritative::ServiceReport report;
        authoritative::ScriptCommand command;
        command.issuer = first_plan_instance;
        command.verb = std::string(verb_ai);
        using authoritative::Value;
        command.arguments = {Value::text("attack_positioning"), Value{authoritative::Handle{handle_taskforce, 1}}, Value{on}};
        report.commands.push_back(std::move(command));
        static_cast<void>(engine.after_service(report));
    }
    static void positioning_service(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_attack_positioning();
    }
    static void positioning_point_order(Engine& engine, const math::Vec3& point) {
        Block block;
        block.order = Block::Order::attack_move;
        block.destination = point;
        engine.order_block(block, fixture_owner(engine).taskforces.at(1), fixture_owner(engine).running.at(1));
    }
    static void positioning_script_point_order(Engine& engine, const math::Vec3& point) {
        authoritative::ServiceReport report;
        authoritative::ScriptCommand command;
        command.issuer = first_plan_instance;
        command.verb = std::string(verb_attack_move);
        using authoritative::Value;
        command.arguments = {Value::number(real(2)), Value{authoritative::Handle{handle_game_object, 30}},
            Value::number(numeric::from_fixed(point.x)), Value::number(numeric::from_fixed(point.y)),
            Value::number(numeric::from_fixed(point.z))};
        report.commands.push_back(std::move(command));
        static_cast<void>(engine.after_service(report));
    }
    static bool positioning_on(const Engine& engine, sim::EntityId unit) { return fixture_owner(engine).positioning.contains(unit); }
    static void threat(Engine& engine, sim::EntityId victim, sim::EntityId attacker, std::int64_t amount) {
        engine.threats_[victim][attacker] = Real(static_cast<int>(amount));
    }
};
} // namespace eawr::script::foc::ai

namespace {

namespace ai = eawr::script::foc::ai;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
namespace skirmish = eawr::skirmish;
namespace sim = eawr::sim;
namespace tactical = eawr::sim::tactical;
using ai::Real;

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Fixed final : ai::LookupResolver {
    std::optional<Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>& bindings) override {
        std::string path;
        for (const std::string& token : lookup.tokens) path += (path.empty() ? "" : ".") + token;
        last_bindings = bindings;
        if (path == "VARIABLE_TARGET.HEALTH") return ai::real(1) / ai::real(2);
        if (path == "GAME.AGE") return ai::real(30);
        return std::nullopt;
    }
    std::vector<ai::Binding> last_bindings;
};

std::optional<Real> evaluate(std::string_view body) {
    const ai::ConverterFunction converters = [](std::string_view converter, std::string_view value) -> std::optional<Real> {
        if (converter == "GameObjectCategoryType" && ai::trimmed(value) == "Fighter | Bomber") return ai::real(3);
        return std::nullopt;
    };
    auto equation = ai::parse_equation("Test", body, converters);
    if (!equation) return std::nullopt;
    Fixed resolver;
    ai::AiRandom random;
    random.set_seed(7);
    return ai::run(equation.value(), resolver, random);
}

void equations() {
    {
        auto host = std::make_shared<foc::detail::Host>();
        host->setup.xml["data/xml/enum/aigoalcategorytype.xml"] =
            "<AIGoalCategoryType><Offensive>0</Offensive><Defensive>1</Defensive>"
            "<Information>2</Information><Infrastructure>3</Infrastructure>"
            "<MajorItem>11</MajorItem></AIGoalCategoryType>";
        const auto categories = foc::tactical_ai_detail::converters(host);
        for (const auto& [name, value] : std::map<std::string, int>{{"Offensive", 0}, {"Defensive", 1},
                {"Information", 2}, {"Infrastructure", 3}, {"MajorItem", 11}})
            expect(categories("AIGoalCategoryType", name) == std::optional<ai::Real>{ai::real(value)},
                "PE-01: goal categories use the authored enum ordinals");
        expect(!categories("AIGoalCategoryType", "Unknown"), "PE-01: an unknown goal category stays invalid");
        host->setup.xml["data/xml/enum/aigoalcategorytype.xml"] =
            "<EnumDefinition><Offensive>23</Offensive><ModCategory>37</ModCategory></EnumDefinition>";
        const auto mod = foc::tactical_ai_detail::converters(host);
        expect(mod("aigoalcategorytype", " offensive ") == std::optional<ai::Real>{ai::real(23)}
            && mod("AIGoalCategoryType", "ModCategory") == std::optional<ai::Real>{ai::real(37)},
            "PE-01: enum overrides and additional mod names are data driven");
        const auto parsed = ai::parse_equation("budget", "AIGoalCategoryType[Offensive]", mod);
        expect(static_cast<bool>(parsed), "PE-01: a goal-category budget equation compiles without a startup refusal");
    }
    expect(evaluate("1 + 2 * 3") == ai::real(7), "PE-03: * binds tighter than +");
    expect(evaluate("(1 + 2) * 3") == ai::real(9), "PE-03: parentheses");
    expect(evaluate("2 > 1") == ai::real(1) && evaluate("2 < 1") == ai::real(0), "PE-04: comparisons give 1 or 0");
    expect(evaluate("0 / 0") == ai::real(0), "PE-04: 0 / x is 0");
    expect(!evaluate("4 / 0").has_value(), "PE-04: a division by 0 fails the equation");
    expect(evaluate("clamp(5, 0, 3)") == ai::real(3), "PE-04: clamp");
    expect(!evaluate("clamp(1, 3, 0)").has_value(), "PE-04: clamp with high < low fails");
    expect(evaluate("Variable_Target.Health * 4") == ai::real(2), "PE-05: a token chain resolves through the host");
    expect(!evaluate("Variable_Target.Unknown").has_value(), "PE-05: an unknown token fails the equation");
    expect(evaluate("3 # 3") == ai::real(3), "PE-11: a # b draws in [a, b]");
    const auto drawn = evaluate("10 # 20");
    expect(drawn && !(*drawn < ai::real(10)) && !(ai::real(20) < *drawn), "PE-11: the draw stays in range");
    expect(evaluate("Game.Age {Parameter_Category = GameObjectCategoryType[Fighter | Bomber]}") == ai::real(30),
        "PE-01: converter constants bind as parameters");
}

void random_and_hash() {
    // PE-11: seed = seed * 0x41C64E6D + 0xBDF (mod 2^32); the draw is bits 10 to 24.
    ai::AiRandom random;
    random.set_seed(12345);
    std::uint32_t seed = 12345;
    bool same = true;
    for (int step = 0; step < 16; ++step) {
        seed = seed * 0x41C64E6DU + 0xBDFU;
        same = same && random.next() == ((seed >> 10) & 0x7FFFU);
    }
    expect(same, "PE-11: the AI random is the engine's linear congruential generator");
    expect(ai::crc32("123456789") == 0xCBF43926U, "PE-12: the target name hash is CRC-32");
}

// Two players on different teams, one AI unit type of power 1000 and reach 500 on a 2000 x 2000 map.
struct World {
    std::shared_ptr<ai::Host> host = ai::make_tactical_host();
    std::shared_ptr<ai::WorldView> view = std::make_shared<ai::WorldView>();
    World() {
        foc::AiType type;
        type.type_id = 1;
        type.name = "SHIP";
        type.unit_ai = true;
        type.category_bits = 2;
        type.combat_power = ai::real(1000);
        type.max_attack_distance = ai::real(500);
        type.locomotor = true;
        host->setup.content.types.push_back(type);
        host->setup.content.categories.emplace("CORVETTE", 2);
        host->setup.content.categories.emplace("FIGHTER", 1);
        host->setup.players = {foc::AiPlayer{1, "REBEL", false, true, ""}, foc::AiPlayer{2, "EMPIRE", false, true, ""}};
        for (const foc::AiType& entry : host->setup.content.types) {
            host->types.emplace(entry.type_id, &entry);
            host->types_by_name.emplace(entry.name, &entry);
        }
        for (const foc::AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
        view->players = {foc::detail::ViewPlayer{1, 1, 0}, foc::detail::ViewPlayer{2, 2, 1}};
        ai::ViewUnit unit;
        unit.id = 10;
        unit.type = 1;
        unit.owner = 2;
        unit.visible_to = 3;
        view->units.push_back(unit);
        host->view = view;
    }
};

// Interleaved plan IDs must retain global event order across disjoint player owners.
void player_runtime_ownership() {
    {
        World world; ai::Engine engine(world.host, {}, {});
        ai::EngineContractAccess::orphan_blocks_fixture(engine);
        ai::EngineContractAccess::movement_service(engine, 2);
        const auto& owner = ai::EngineContractAccess::checked_player(engine, 2);
        const auto& produce = owner.blocks.at(static_cast<std::uint64_t>(ai::Block::Kind::produce) + 1);
        const auto& reinforce = owner.blocks.at(static_cast<std::uint64_t>(ai::Block::Kind::reinforce) + 1);
        const auto& ambush = owner.blocks.at(static_cast<std::uint64_t>(ai::Block::Kind::ambush) + 1);
        const auto& move = owner.blocks.at(static_cast<std::uint64_t>(ai::Block::Kind::move) + 1);
        expect(!produce.finished && !reinforce.finished && ambush.finished && !ambush.ready && move.finished,
            "missing TaskForce keeps produce/reinforce untouched and uses each tactical branch's original failure result");
    }
    std::optional<std::string> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        ai::PlanDef definition; definition.name = "OWNED";
        ai::Engine engine(world.host, {}, {definition});
        auth::ModuleManifest modules;
        expect(modules.add("OWNED.LUA", "Count = 0\nfunction MainForce_Unit_Destroyed(tf) Count = Count + 1 end\n"
            "function MainForce_No_Units_Remaining(tf) Count = Count + 1 end\n").has_value(), "ownership event module loads");
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        if (!scripts) { expect(false, "ownership scheduler creates"); return; }
        for (const auto id : {101U, 102U, 103U})
            expect(scripts.value().create_instance(id, "OWNED.LUA").has_value(), "ownership instances create");
        ai::EngineContractAccess::ownership_fixture(engine, scripts.value());
        ai::EngineContractAccess::stage_movement_queries(engine);
        auto& alias = ai::EngineContractAccess::checked_player(engine, 9).blocks.at(1100);
        alias.waiting_on = 1099;
        expect(engine.block(1100) && engine.block(1100)->id == 1099, "block aliases retain global foreign-owner resolution");
        alias.waiting_on = 1101;
        expect(engine.galactic_movement_requests() == std::vector<std::uint64_t>{300, 200, 100},
            "movement requests retain ascending block order across player owners");
        ai::EngineContractAccess::clear_movement_queries(engine);
        expect(engine.plan(1) && engine.plan(1)->player == 9 && engine.plan(2) && engine.plan(2)->player == 7
            && engine.plan_of_instance(103) == engine.plan(3) && engine.taskforce(2)->plan == 2
            && engine.block(1100) == engine.block(1101), "serial binding index routes interleaved IDs and aliases to their owners");
        auth::ServiceReport foreign;
        auth::ScriptCommand release; release.issuer = 101; release.verb = std::string(ai::verb_ai);
        release.arguments = {auth::Value::text("release_all"), auth::Value{auth::Handle{ai::handle_taskforce, 2}}};
        foreign.commands.push_back(std::move(release));
        expect(engine.after_service(foreign).has_value() && engine.taskforce(2)->units.empty()
            && engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{999},
            "after_service preserves a script's foreign TaskForce handle without redirecting it to the issuer");
        ai::EngineContractAccess::restore_foreign_units(engine);
        ai::EngineContractAccess::foreign_task(engine);
        expect(engine.taskforce(2)->pooled.size() == 1 && engine.taskforce(2)->pooled.front().token == 77,
            "execution tasks preserve the original global TaskForce lookup");
        auto events = ai::EngineContractAccess::run_events(engine);
        expect(events.size() == 6, "each owned force emits death and empty events");
        for (std::size_t i = 0; i < events.size(); ++i) {
            expect(events[i].target == 101 + i / 2 && events[i].key.sequence == i,
                "global ascending plan order retains event targets and sequence numbers across players");
            expect(scripts.value().submit_event(std::move(events[i])).has_value(), "owned event submits");
        }
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(scripts.value().service(executor).has_value(), "owned callbacks execute across worker counts");
        const auto hash = scripts.value().state_hash();
        expect(hash.has_value(), "owned callback state hashes");
        if (hash) { if (!reference) reference = hash.value(); else expect(*reference == hash.value(), "owned callbacks agree on 1/2/4/8 workers"); }
        auto& owner = ai::EngineContractAccess::checked_player(engine, 9);
        ai::EngineContractAccess::abandon_goal(engine, owner, 1, scripts.value());
        expect(!engine.plan(1) && !engine.plan_of_instance(101) && !engine.taskforce(1) && !engine.block(1100)
            && engine.plan(2) && engine.plan(3), "retirement removes all owned lookup rows and preserves other running plans");
    }
}

// S-T3b: what a player's turn writes to engine-wide state stages in its PlayerEffects and commits in ascending
// player order. Two engines run the same turns of the same two players: one writes directly (the sequential
// reference), the other stages. They must end byte-equal, and the staged one must show that a later turn read the
// earlier turn's staged credits and pad claims.
std::string effects_turns(const bool staged, ai::EffectCounts* counts) {
    World world;
    world.host->setup.journal = std::make_shared<foc::AiJournal>();
    ai::PlanDef definition; definition.name = "OWNED";
    ai::Engine engine(world.host, {}, {definition});
    world.host->engine = &engine;
    auth::ModuleManifest modules;
    expect(modules.add("OWNED.LUA", "Count = 0\nfunction MainForce_Unit_Destroyed(tf) Count = Count + 1 end\n"
        "function MainForce_Unit_Damaged(tf) Count = Count + 1 end\n").has_value(), "effects event module loads");
    auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
    if (!scripts) { expect(false, "effects scheduler creates"); return {}; }
    for (const auto id : {101U, 102U, 103U})
        expect(scripts.value().create_instance(id, "OWNED.LUA").has_value(), "effects instances create");
    ai::EngineContractAccess::ownership_fixture(engine, scripts.value());
    ai::EngineContractAccess::backlog(engine, 9, 1);
    const auto& unit = world.view->units.front();
    const auto in_turn = [&](const tactical::PlayerId id, const auto& body) {
        if (staged) ai::EngineContractAccess::turn(engine, id, [&](ai::PlayerAi&) { body(); }); else body();
    };
    in_turn(7, [&] {
        expect(ai::EngineContractAccess::staging(engine) == staged, "a staged turn binds its effects to the running thread");
        ai::EngineContractAccess::emit(engine, 7, 2, 2, "Unit_Destroyed");
        ai::EngineContractAccess::move(engine, 7, 2, unit);
        ai::EngineContractAccess::note(engine, 7, 2, "first");
        ai::EngineContractAccess::wallet(engine, 7, ai::real(-5));
        ai::EngineContractAccess::claim_pad(engine, 50, 7, 4);
        ai::EngineContractAccess::wait_for_attach(engine, ai::EngineContractAccess::checked_player(engine, 7), 4);
        ai::EngineContractAccess::wait_for_attach(engine, ai::EngineContractAccess::checked_player(engine, 7), 4);
    });
    if (staged) {
        expect(ai::EngineContractAccess::waiting_events(engine) == 0 && ai::EngineContractAccess::waiting_orders(engine) == 0
            && ai::EngineContractAccess::waiting_attaches(engine) == 1 && ai::EngineContractAccess::committed_claims(engine) == 0
            && ai::EngineContractAccess::committed_wallets(engine) == 0,
            "a staged turn leaves the engine's queues, wallet and pad claims untouched until the commit");
    }
    in_turn(9, [&] {
        // Stacked reads: the committed state under the earlier turn's staged effects.
        const auto debit = eawr::script::numeric::to_fixed(ai::real(-5)).value().raw();
        expect(ai::EngineContractAccess::stacked(engine, 7) == debit
            && ai::EngineContractAccess::host_stack(engine, 7) == (staged ? debit : 0),
            "credits stack the staged deltas of earlier turns");
        const auto held = ai::EngineContractAccess::claim(engine, 50);
        expect(held && held->player == 7 && held->goal == 4, "a pad claim reads the earlier turn's staged claim");
        ai::EngineContractAccess::free_pads(engine, 7, 4);
        expect(!ai::EngineContractAccess::claim(engine, 50), "a release by predicate sees the staged claim and frees it");
        ai::EngineContractAccess::claim_pad(engine, 51, 9, 6);
        ai::EngineContractAccess::emit(engine, 9, 1, 1, "Unit_Destroyed");
        ai::EngineContractAccess::signal(engine, 9, 1, 1, "Unit_Damaged");
        ai::EngineContractAccess::move(engine, 9, 1, unit);
        ai::EngineContractAccess::wallet(engine, 9, ai::real(3));
        ai::EngineContractAccess::note(engine, 9, 1, "second");
        auto& player = ai::EngineContractAccess::checked_player(engine, 9);
        ai::EngineContractAccess::wait_for_attach(engine, player, 1); // already in the committed backlog
        ai::EngineContractAccess::wait_for_attach(engine, player, 2);
        ai::EngineContractAccess::finish(engine, 9, 3, scripts.value());
    });
    if (staged) {
        const auto committed = ai::EngineContractAccess::commit(engine, scripts.value());
        expect(committed.has_value(), "the ordered commit applies");
        if (counts) *counts = ai::EngineContractAccess::totals(engine);
    }
    return ai::EngineContractAccess::snapshot(engine, scripts.value());
}

void player_effects() {
    ai::EffectCounts counts;
    const auto reference = effects_turns(false, nullptr);
    const auto staged = effects_turns(true, &counts);
    expect(!reference.empty() && reference == staged,
        "staged turns commit to exactly the state of the sequential writes: events, orders, wallet, pads, attaches, records, journal, index");
    expect(counts.turns == 2 && counts.orders == 4 && counts.credit_deltas == 2 && counts.attach_intents == 2
        && counts.events == 3 && counts.removals == 1 && counts.index_effects == 3 && counts.pad_effects == 3,
        "the commit counts the effects each turn staged");
    expect(counts.reads.credits == 2 && counts.reads.pads == 2,
        "later turns count the earlier turn's overlay they read: credits, and pad claims by a lookup and by a release");
    {
        // A plan attachment is a serial boundary: inside a turn it is a logic error, and nothing was allocated.
        World world; ai::PlanDef definition; definition.name = "OWNED";
        ai::Engine engine(world.host, {}, {definition});
        auth::ModuleManifest modules;
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        if (!scripts) { expect(false, "tripwire scheduler creates"); return; }
        ai::EngineContractAccess::ownership_fixture(engine, scripts.value());
        bool threw = false;
        ai::EngineContractAccess::turn(engine, 7, [&](ai::PlayerAi& player) {
            threw = ai::EngineContractAccess::attach_throws(engine, player, scripts.value());
        });
        expect(threw, "plan attachment inside a player turn is a logic error");
        expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value(), "the tripwire turn commits nothing");
    }
    {
        // Typed operations on another player's TaskForce: the turns are sequential, so an operation applies in place at
        // once, in ascending acting-player order, and the owner's later turn sees it. An acting player below the owner
        // is the case a commit-time application would reorder.
        for (const bool acting_below_owner : {false, true}) {
            std::vector<std::vector<std::uint64_t>> tokens;
            ai::EffectCounts operations;
            for (const bool staged_turns : {false, true}) {
                World world;
                ai::PlanDef definition; definition.name = "OWNED";
                ai::Engine engine(world.host, {}, {definition});
                auth::ModuleManifest modules;
                auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
                if (!scripts) { expect(false, "operation scheduler creates"); return; }
                if (acting_below_owner) ai::EngineContractAccess::add_player(engine, 5);
                ai::EngineContractAccess::ownership_fixture(engine, scripts.value());
                ai::EngineContractAccess::add_player(engine, 11);
                ai::EngineContractAccess::reindex(engine);
                // Players run 5 (or 9), 7 (the owner of TaskForce 2), then 9 and 11 (or 11).
                const tactical::PlayerId first = acting_below_owner ? 5 : 9;
                ai::EngineContractAccess::pool_task(engine, first, 2, 77);
                ai::EngineContractAccess::pool_task(engine, 7, 2, 78);
                ai::EngineContractAccess::pool_task(engine, 11, 2, 79);
                const auto run = [&](const tactical::PlayerId id) {
                    if (staged_turns) ai::EngineContractAccess::turn(engine, id, [&](ai::PlayerAi&) { ai::EngineContractAccess::execute(engine, id); });
                    else ai::EngineContractAccess::execute(engine, id);
                };
                const auto order = acting_below_owner ? std::vector<tactical::PlayerId>{5, 7, 9, 11}
                                                      : std::vector<tactical::PlayerId>{7, 9, 11};
                for (const auto id : order) {
                    run(id);
                    if (id == first) expect(ai::EngineContractAccess::forced_units(engine, 2) >= 1,
                        "a typed operation on another player's TaskForce is visible to the next reader at once");
                }
                if (staged_turns) {
                    expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value(), "operation commit applies");
                    operations = ai::EngineContractAccess::totals(engine);
                }
                tokens.push_back(ai::EngineContractAccess::pooled_tokens(engine, 2));
            }
            const std::vector<std::uint64_t> expected = acting_below_owner ? std::vector<std::uint64_t>{77, 78, 79}
                                                                           : std::vector<std::uint64_t>{78, 77, 79};
            expect(tokens.size() == 2 && tokens[0] == tokens[1] && tokens[0] == expected,
                acting_below_owner ? "an acting player below the owner pools before the owner's own turn, as the sequential pass did"
                                   : "an acting player above the owner pools after the owner's own turn, as the sequential pass did");
            expect(operations.operations == 2U && operations.reads.handles >= 2,
                "the commit counts the operations and the foreign handles resolved");
        }
    }
    {
        // NativeRecordWrite invalidations and revision bumps stage and publish at the commit; the scope never publishes in a turn.
        std::vector<std::string> published;
        for (const bool staged_turns : {false, true}) {
            World world;
            ai::PlanDef definition; definition.name = "OWNED";
            ai::Engine engine(world.host, {}, {definition});
            auth::ModuleManifest modules;
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            if (!scripts) { expect(false, "invalidation scheduler creates"); return; }
            ai::EngineContractAccess::ownership_fixture(engine, scripts.value());
            ai::EngineContractAccess::arm_native_records(engine, true);
            std::optional<ai::Engine::NativeRecordWrite> outer; // the barrier's own scope: nothing refreshes below it
            ai::EngineContractAccess::hold_native_scope(outer, engine);
            const auto run = [&](const tactical::PlayerId id, const auto& body) {
                if (staged_turns) ai::EngineContractAccess::turn(engine, id, [&](ai::PlayerAi&) { body(); }); else body();
            };
            const auto before = ai::EngineContractAccess::native_snapshot(engine);
            run(7, [&] { ai::EngineContractAccess::native_changes(engine, 7); });
            run(9, [&] { ai::EngineContractAccess::native_changes(engine, 9); ai::EngineContractAccess::native_shared_change(engine); });
            if (staged_turns) {
                expect(ai::EngineContractAccess::native_snapshot(engine) == before,
                    "a staged turn publishes no invalidation, revision or scope depth before the commit");
                expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value(), "invalidation commit applies");
            }
            published.push_back(ai::EngineContractAccess::native_snapshot(engine));
            expect(ai::EngineContractAccess::native_depth(engine) == 1, "turn scopes leave the barrier's scope depth alone");
            ai::EngineContractAccess::arm_native_records(engine, false); // the outer scope must not refresh a codec-less fixture
            outer.reset();
        }
        expect(published.size() == 2 && published[0] == published[1],
            "staged invalidations publish exactly the dirty groups, keys, header and revision of the direct calls");
    }
    {
        // Attachment intents keep the queue order and the budget: ascending players, then identities from the serial drain.
        std::vector<std::string> outcomes;
        for (const bool staged_turns : {false, true}) {
            World world;
            world.host->setup.schedule.attach_per_tick = 2;
            ai::PlanDef definition; definition.name = "OWNED"; definition.module = "OWNED.LUA";
            ai::Engine engine(world.host, {}, {definition});
            auth::ModuleManifest modules;
            expect(modules.add("OWNED.LUA", "Count = 0\n").has_value(), "attach module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            if (!scripts) { expect(false, "attach scheduler creates"); return; }
            ai::EngineContractAccess::bare_players(engine, scripts.value());
            ai::EngineContractAccess::active_goal(engine, 7, 4, true);
            ai::EngineContractAccess::active_goal(engine, 9, 1, false); // dropped by the drain, not charged to the budget
            for (const std::uint64_t goal : {2U, 3U}) ai::EngineContractAccess::active_goal(engine, 9, goal, true);
            const auto run = [&](const tactical::PlayerId id, const auto& body) {
                if (staged_turns) ai::EngineContractAccess::turn(engine, id, [&](ai::PlayerAi&) { body(); }); else body();
            };
            run(7, [&] { ai::EngineContractAccess::wait_for_attach(engine, ai::EngineContractAccess::checked_player(engine, 7), 4); });
            run(9, [&] {
                for (const std::uint64_t goal : {1U, 2U, 3U})
                    ai::EngineContractAccess::wait_for_attach(engine, ai::EngineContractAccess::checked_player(engine, 9), goal);
            });
            if (staged_turns) {
                expect(ai::EngineContractAccess::waiting_attaches(engine) == 0, "attachment intents wait for the commit");
                expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value(), "attach commit applies");
            }
            expect(ai::EngineContractAccess::waiting_attaches(engine) == 4, "the committed queue holds every intent in player order");
            expect(ai::EngineContractAccess::drain(engine, scripts.value()).has_value(), "the serial drain runs");
            expect(ai::EngineContractAccess::waiting_attaches(engine) == 1, "the budget attaches two goals and leaves the rest queued");
            outcomes.push_back(ai::EngineContractAccess::goal_plans(engine) + ai::EngineContractAccess::snapshot(engine, scripts.value()));
        }
        expect(outcomes.size() == 2 && outcomes[0] == outcomes[1],
            "staged attachment intents drain to the same plan identities, events and queue as the sequential writes");
    }
    {
        // Outside a turn every write is immediate (the serial phases between barriers); a turn's scope ends with the turn.
        World world;
        ai::PlanDef definition; definition.name = "OWNED";
        ai::Engine engine(world.host, {}, {definition});
        auth::ModuleManifest modules;
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        if (!scripts) { expect(false, "serial scheduler creates"); return; }
        ai::EngineContractAccess::ownership_fixture(engine, scripts.value());
        ai::EngineContractAccess::turn(engine, 7, [&](ai::PlayerAi&) {
            ai::EngineContractAccess::claim_pad(engine, 50, 7, 4);
            ai::EngineContractAccess::wallet(engine, 7, ai::real(-5));
        });
        expect(!ai::EngineContractAccess::staging(engine), "a turn's scope ends with the turn");
        expect(ai::EngineContractAccess::committed_claims(engine) == 0 && ai::EngineContractAccess::committed_wallets(engine) == 0
            && ai::EngineContractAccess::committed_orders(engine) == 0 && ai::EngineContractAccess::staged_effects(engine) == 1,
            "the turn's effects wait for the commit");
        ai::EngineContractAccess::claim_pad(engine, 51, 9, 6);
        ai::EngineContractAccess::wallet(engine, 9, ai::real(3));
        expect(ai::EngineContractAccess::committed_claims(engine) == 1 && ai::EngineContractAccess::committed_wallets(engine) == 1
            && ai::EngineContractAccess::committed_orders(engine) == 1,
            "a serial write between turns lands at once, ahead of the staged ones");
        expect(ai::EngineContractAccess::claim(engine, 50) && ai::EngineContractAccess::claim(engine, 51)
            && ai::EngineContractAccess::stacked(engine, 7) == eawr::script::numeric::to_fixed(ai::real(-5)).value().raw(),
            "serial reads see the committed state under the staged effects");
        expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value() && ai::EngineContractAccess::staged_effects(engine) == 0
            && ai::EngineContractAccess::committed_claims(engine) == 2 && ai::EngineContractAccess::committed_wallets(engine) == 2,
            "the commit applies the turn's effects after the serial ones and empties the overlay");
    }
}

// A competing claimant on a shared pad: the second player's producer lookup sees the first turn's staged claim, a
// release by the first turn frees it again, and the answers are those of the sequential writes.
void shared_pad_competition() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    std::vector<std::vector<int>> answers;
    ai::EffectCounts counts;
    for (const bool staged_turns : {false, true}) {
        t::TacticalSetup start;
        start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}, {3, 3, 300, 0}};
        start.units = {{1, 20, 2}};
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20, true}, {2, q(1000), 20}, {3, q(1000), 20}};
        rules.menus = {{20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false},
            {20, 200, {{31, t::BuildKind::structure, t::BuildQueue::units, q(900), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 3;
        rules.pads.capture = {{20, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}, {31, 41, q(100), 1, 1}};
        rules.pads.influence = {{20, false}, {30, false}, {40, false}};
        t::DurabilityTable health;
        for (const t::TypeId type : {20U, 30U, 31U, 40U, 41U}) {
            t::DurabilityProfile profile; profile.type_id = type; profile.max_hull = q(300);
            health.profiles.push_back(profile);
        }
        auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "shared pad fixture starts");
        if (!made) return;
        auto host = ai::make_tactical_host();
        host->world = &made.value(); host->snapshot = made.value().snapshot();
        host->view = foc::detail::build_view(made.value(), *host->snapshot);
        ai::Engine engine(host, {}, {ai::PlanDef{}});
        ai::EngineContractAccess::add_player(engine, 1);
        ai::EngineContractAccess::add_player(engine, 2);
        auth::ModuleManifest modules;
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        if (!scripts) { expect(false, "shared pad scheduler creates"); return; }
        std::vector<int> seen;
        const auto producer = [&] { seen.push_back(engine.producer(1, 30) == 1 ? 1 : 0); };
        const auto in_turn = [&](const tactical::PlayerId id, const auto& body) {
            if (staged_turns) ai::EngineContractAccess::turn(engine, id, [&](ai::PlayerAi&) { body(); }); else body();
        };
        producer(); // outside any turn: the pad is free
        in_turn(2, [&] { ai::EngineContractAccess::claim_pad(engine, 1, 2, 7); });
        in_turn(1, [&] { producer(); }); // the second claimant is refused while the first holds it
        if (staged_turns) expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value(), "pad commit applies");
        producer(); // the committed claim still refuses it
        in_turn(2, [&] { ai::EngineContractAccess::free_pad(engine, 1); });
        in_turn(1, [&] { producer(); }); // released earlier in the barrier: free again
        if (staged_turns) {
            expect(ai::EngineContractAccess::commit(engine, scripts.value()).has_value(), "pad release commit applies");
            counts = ai::EngineContractAccess::totals(engine);
        }
        answers.push_back(seen);
    }
    expect(answers.size() == 2 && answers[0] == answers[1] && answers[0] == std::vector<int>{1, 0, 0, 1},
        "a shared pad refuses the second claimant, in the committed and the staged view alike, and frees on release");
    expect(counts.reads.pads == 2, "each later turn's pad lookup counts the earlier turn's staged claim or release");
}


// This host has no tactical session or snapshot. It supplies a different goal
// domain, target set and equation lookup while the shared engine owns the goals.
class ModeWorld final : public ai::World {
public:
    bool friendly_planet_fixture{};
    bool provide_account{};
    tactical::EconomyView reservation_account;
    std::vector<tactical::Player> reservation_players;
    std::map<eawr::sim::EntityId, tactical::PadState> reservation_pads;
    std::shared_ptr<const ai::WorldView> world_view = std::make_shared<ai::WorldView>();
    std::shared_ptr<const ai::WorldView> view() const override { return world_view; }
    mutable std::uint32_t contexts{};
    mutable std::uint32_t evaluations{};
    bool available() const noexcept override { return true; }
    std::uint64_t frame{45};
    std::uint64_t completed_tick() const noexcept override { return frame; }
    std::vector<ai::Target> new_targets(const ai::Engine&) const override { return {}; }
    bool target_alive(const ai::Target& target) const override { return target.object == 700; }
    std::string mode = "GALACTIC";
    std::string_view goal_mode() const noexcept override { return mode; }
    std::optional<Real> perception(const ai::Engine&, const ai::Equation& equation, const ai::Context& context, ai::AiRandom& random) const override {
        ++evaluations;
        class Resolver final : public ai::LookupResolver {
        public:
            const ai::Context& context;
            explicit Resolver(const ai::Context& value) : context(value) {}
            std::optional<Real> resolve(const ai::Lookup& lookup, const std::vector<ai::Binding>&) override {
                if (lookup.tokens == std::vector<std::string>{"VARIABLE_SELF", "PLANETCOUNT"}
                    && context.enemy == 7U && context.human == 9U) return ai::real(3);
                return std::nullopt;
            }
        } resolver(context);
        return ai::run(equation, resolver, random);
    }
    ai::Context goal_context(const ai::PlayerAi& player, const ai::Target* target) const override {
        ++contexts;
        return ai::Context{player.player, 7, 9, target};
    }
    bool target_matches(tactical::PlayerId, const std::set<std::string>& flags, const ai::Target* target) const override {
        return target && target->object == 700 && (flags.contains("PLANET")
            || (friendly_planet_fixture && flags.contains("FRIENDLY")));
    }
    std::vector<ai::Target> targets(const ai::Engine&, const ai::PlayerAi&) const override {
        ai::Target target; target.object = 700; target.name = "PLANET";
        return {target};
    }
    std::span<const tactical::Player> players() const noexcept override { return reservation_players; }
    const tactical::EconomyRules& economy() const noexcept override { static const tactical::EconomyRules empty; return empty; }
    const tactical::EconomyView* account(tactical::PlayerId) const override { return provide_account ? &reservation_account : nullptr; }
    const std::map<eawr::sim::EntityId, tactical::PadState>& pads() const noexcept override {
        return reservation_pads;
    }
    bool arriving(eawr::sim::EntityId) const override { return false; }
    bool build_allowed(tactical::PlayerId, eawr::sim::EntityId, tactical::TypeId) const override { return false; }
    const tactical::BuildOption* build_option(tactical::PlayerId, eawr::sim::EntityId, tactical::TypeId) const override { return nullptr; }
    tactical::ProductionCounts production_counts(tactical::PlayerId, tactical::TypeId) const override { return {}; }
    bool construction_allowed(tactical::PlayerId, eawr::sim::EntityId) const override { return false; }
    bool constructing_for(eawr::sim::EntityId, tactical::PlayerId) const override { return false; }
    eawr::sim::EntityId next_entity_id() const noexcept override { return 701; }
    std::optional<ai::ReinforcementResult> reinforcement_search_result(tactical::PlayerId, std::uint64_t) const override { return std::nullopt; }
    std::span<const tactical::Event> events() const noexcept override { return {}; }
    std::span<const tactical::CombatEvent> combat_events() const noexcept override { return {}; }
};

// WTA-01/02 (legacy #3048): on a space execution frame the freestore's service tests run inside the
// execution service, after the player's goal service and planning, with one live event counter.
// FD-71/72: scoring owns the early gate; the ordinary scheduler owns late writes.
void player_ai_admission_contract() {
    {
        foc::AiSetup setup;
        const auto baseline = foc::tactical_ai_detail::native_configuration(setup);
        setup.player_ai_start_ticks[2] = 0;
        const auto immediate = foc::tactical_ai_detail::native_configuration(setup);
        setup.player_ai_start_ticks[2] = 5;
        const auto delayed = foc::tactical_ai_detail::native_configuration(setup);
        setup.player_ai_start_ticks[2] = 6;
        const auto other = foc::tactical_ai_detail::native_configuration(setup);
        expect(baseline && immediate && delayed && other
            && baseline.value() == immediate.value() && baseline.value() != delayed.value()
            && delayed.value() != other.value(),
            "AI admission: native continuation identity covers positive boundaries and preserves default/zero");
        ai::Host unbound;
        unbound.setup.player_ai_start_ticks[2] = 0;
        expect(unbound.ai_admitted(2), "AI admission: explicit zero is immediate even before a view is bound");
    }
    {
        const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
        tactical::EconomyRules rules;
        rules.players = {{2, q(100), 5, true}};
        rules.players[0].credit_multiplier = q(2);
        rules.player_ai_start_ticks[2] = 5;
        rules.max_queue = 1;
        const auto human = rules.player_at(2, 4), admitted = rules.player_at(2, 5);
        expect(human && admitted && !human->ai && admitted->ai && !rules.player_at(3, 5),
            "AI admission: economic policy changes at the boundary, preserving absent-player lookup");
        if (!human || !admitted) return;
        tactical::PlayerEconomy ledger; ledger.player = 2;
        expect(tactical::change_credits(ledger, *human, q(10)) && ledger.credits == q(10),
            "AI admission: held income/refunds use human credit changes");
        ledger.credits = {};
        expect(tactical::change_credits(ledger, *admitted, q(10)) && ledger.credits == q(20),
            "AI admission: admitted positive credit changes use the authored AI multiplier");
        const tactical::BuildOption option{10, tactical::BuildKind::unit, tactical::BuildQueue::units, q(1), 30, 3, 0, true};
        ledger.credits = q(100);
        expect(tactical::queue_build(ledger, *human, rules, option, 20, 4) == tactical::RejectReason::none
            && ledger.queues[0].front().frames == 30
            && tactical::queue_build(ledger, *human, rules, option, 20, 4) == tactical::RejectReason::queue_full,
            "AI admission: held purchases use human build time and queue limit");
        ledger.queues = {};
        expect(tactical::queue_build(ledger, *admitted, rules, option, 20, 5) == tactical::RejectReason::none
            && ledger.queues[0].front().frames == 3
            && tactical::queue_build(ledger, *admitted, rules, option, 20, 5) == tactical::RejectReason::none,
            "AI admission: admitted purchases use AI build time and unbounded queue policy");
        rules.income = {{10, q(1), {}}};
        std::vector<std::string> golden;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            tactical::TacticalSetup setup;
            setup.players = {{2, 2, 200, tactical::player_flag_commandable}};
            setup.units = {{20, 10, 2}};
            auto world = tactical::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
            expect(world.has_value(), "AI admission: income boundary world creates");
            if (!world) return;
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            std::vector<std::string> hashes;
            for (unsigned frame = 1; frame <= 8; ++frame) {
                auto tick = world.value().step(executor);
                // A world step services the interval beginning at completed_tick,
                // then publishes the next completed tick to the AI host.
                const auto expected = q(100 + frame + (frame <= 5 ? 0 : frame - 5));
                expect(tick && world.value().ledgers()[0].credits == expected,
                    "AI admission: income uses the pre-step completed tick at the control boundary; frame="
                    + std::to_string(frame) + " actual=" + std::to_string(world.value().ledgers()[0].credits.raw())
                    + " expected=" + std::to_string(expected.raw()));
                if (!tick) return;
                hashes.push_back(tick.value().state_sha256);
            }
            if (golden.empty()) golden = hashes;
            else expect(golden == hashes, "AI admission: economic boundary hashes agree on 1/2/4/8 workers");
        }
    }
    for (const auto schedule : {foc::AiSchedule::Mode::faithful, foc::AiSchedule::Mode::staggered}) {
        std::vector<std::string> default_hashes;
        // Absent and explicit-zero admission must preserve the default hashes.
        for (const unsigned admission : {0U, 1U, 5U}) {
            std::vector<std::string> worker_hashes;
            for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
                tactical::TacticalSetup setup;
                setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
                foc::AiSetup ai_setup;
                ai_setup.players = {{1, "SYNTHETIC", false, true, ""}, {2, "SYNTHETIC", false, true, ""}};
                ai_setup.schedule.mode = schedule;
                if (admission) ai_setup.player_ai_start_ticks[2] = admission == 1 ? 0 : admission;
                ai_setup.freestore_module = "AdmissionProbe.lua";
                const std::map<std::string, std::string> modules{
                    {"AdmissionProbe.lua", "function Base_Definitions() ServiceRate=0; UnitServiceRate=0; pumps=0 end\n"
                        "function main() while true do pumps=pumps+1; coroutine.yield(true) end end"},
                    {"data/scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
                auto world = tactical::TacticalSession::create(setup);
                auto session = foc::create_session(std::move(world).value(), ai_setup, modules);
                expect(session.has_value(), "AI admission: independent player fixture creates");
                if (!session) return;
                const eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> hashes;
                for (unsigned frame = 1; frame <= 8; ++frame) {
                    auto tick = session.value().step(executor);
                    expect(tick.has_value(), "AI admission: fixture steps");
                    if (!tick) return;
                    hashes.push_back(tick.value().state_sha256);
                    auto saved = session.value().save_scripts();
                    auto inspector = foc::prepare_session(ai_setup, modules);
                    expect(saved && inspector && inspector.value().scripts.load(saved.value().scheduler), "AI admission: freestore state inspects");
                    if (!saved || !inspector) return;
                    for (unsigned id = 1; id <= 2; ++id) {
                        auto count = inspector.value().scripts.read_global(foc::freestore_instance(id), "pumps");
                        const unsigned first = id == 2 && admission > 1 ? admission : 2;
                        const unsigned expected = frame < first ? 0 : frame - first + 1;
                        expect(count && count.value() && std::get<Real>(count.value()->data) == ai::real(expected),
                            "AI admission: holding one player preserves the other's work and releases at its boundary");
                    }
                    if (admission > 1 && (frame == 3 || frame == 6)) {
                        auto checkpoint = session.value().world().save_checkpoint_state();
                        auto rebuilt_world = tactical::TacticalSession::create(setup);
                        auto rebuilt_ai = foc::prepare_session(ai_setup, modules);
                        expect(saved.value().native && checkpoint && rebuilt_world && rebuilt_ai
                            && rebuilt_world.value().load_checkpoint_state(checkpoint.value()),
                            "AI admission: native continuation exists before and after handoff");
                        if (!saved.value().native || !checkpoint || !rebuilt_world || !rebuilt_ai) return;
                        auto rebuilt = auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(),
                            std::move(rebuilt_ai.value().scripts), saved.value());
                        expect(rebuilt && rebuilt_ai.value().bind(rebuilt.value()), "AI admission: same-map native continuation binds");
                        if (!rebuilt) return;
                        expect(rebuilt.value().save_scripts().value().native == saved.value().native,
                            "AI admission: same-map native bytes round trip");
                        auto wrong_setup = ai_setup; wrong_setup.player_ai_start_ticks[2] = 6;
                        auto wrong_world = tactical::TacticalSession::create(setup);
                        auto wrong_ai = foc::prepare_session(wrong_setup, modules);
                        expect(wrong_world && wrong_ai && wrong_world.value().load_checkpoint_state(checkpoint.value()),
                            "AI admission: changed-map refusal fixture rebuilds");
                        if (!wrong_world || !wrong_ai) return;
                        auto wrong = auth::ScriptedTacticalSession::restore(std::move(wrong_world).value(),
                            std::move(wrong_ai.value().scripts), saved.value());
                        expect(wrong && !wrong_ai.value().bind(wrong.value()), "AI admission: changed-map native continuation is refused");
                        // Continue the reconstructed side against an uninterrupted next tick.
                        auto direct = session.value().step(executor);
                        auto resumed = rebuilt.value().step(executor);
                        expect(direct && resumed && direct.value().state_sha256 == resumed.value().state_sha256,
                            "AI admission: restored same-map continuation is hash-identical");
                        if (!resumed) return;
                        session = std::move(rebuilt);
                        ++frame;
                        hashes.push_back(resumed.value().state_sha256);
                    }
                }
                if (worker_hashes.empty()) worker_hashes = hashes;
                else expect(worker_hashes == hashes, "AI admission: handoff hashes agree on 1/2/4/8 workers");
            }
            if (!admission) default_hashes = worker_hashes;
            else if (admission == 1) expect(default_hashes == worker_hashes, "AI admission: explicit zero preserves empty-map behavior");
        }
    }
    auto host = std::make_shared<ai::Host>();
    auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE";
    host->ai_world = mode; host->view = mode->view();
    host->setup.schedule.mode = foc::AiSchedule::Mode::faithful;
    host->setup.journal = std::make_shared<foc::AiJournal>();
    host->setup.players = {{1, "SYNTHETIC", false, true, "OPENING"}, {2, "SYNTHETIC", false, true, "OPENING"}};
    for (const auto& entry : host->setup.players) host->players.emplace(entry.player, &entry);
    host->setup.player_ai_start_ticks[2] = 5;
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    ai::AiData data;
    data.equations = std::move(ai::EquationSet::parse({{"Opening.xml", "<Root><Positive>2</Positive></Root>"}}, converters)).value();
    ai::PlayerType player; player.name = "OPENING"; player.function_sets = {"OPENING"}; data.players["OPENING"] = player;
    ai::GoalType goal; goal.mode = "SPACE"; goal.application = {"GLOBAL"};
    data.goals["OPENING"] = goal; data.function_sets["OPENING"] = {{"OPENING", "Positive"}};
    auto native_owner = std::make_shared<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{});
    auto& engine = *native_owner;
    std::optional<ai::GoalStateContributor> native_records;
    auto scripts = auth::ScriptScheduler::create({}, {});
    std::uint64_t sequence{};
    const eawr::sim::InlineExecutor native_executor;
    for (mode->frame = 1; mode->frame <= 8; ++mode->frame) {
        auto view = std::make_shared<ai::WorldView>(); view->tick = mode->frame;
        mode->world_view = std::move(view);
        host->ai_suspended = mode->frame >= 4 && mode->frame <= 6;
        expect(engine.before_service(*mode, scripts.value(), sequence).has_value(), "AI admission: native services step");
        expect(scripts.value().service(native_executor).has_value(), "AI admission: native fixture advances its scheduler barrier");
        // Seed the record oracle after this tactical fixture lazily creates
        // its players; subsequent frames exercise incremental admission writes.
        if (!native_records) native_records.emplace(native_owner, std::string(64, 'a'));
        const auto warm = native_records->record_sync_hash();
        const auto cold = native_records->record_sync_hash(true);
        expect(warm && cold && warm.value() == cold.value(),
            "AI admission: pending refresh and suspension retain equal incremental and cold native records");
        expect(ai::EngineContractAccess::admission_budget(engine) == (mode->frame < 5 ? 20 : 10),
            "AI admission: proposal sharing counts the held side as human until handoff");
        if (mode->frame == 3 || mode->frame == 7) {
            const bool pending = mode->frame < 5;
            expect(ai::EngineContractAccess::player(engine, 2)->admission_pending == pending,
                "AI admission: missed boundary refresh stays pending until the first enabled service");
            const std::string identity(64, 'a');
            auto state = engine.save_tactical_native(identity, scripts.value());
            expect(state && engine.restore_tactical_native(state.value(), identity, scripts.value())
                && ai::EngineContractAccess::player(engine, 2)->admission_pending == pending,
                "AI admission: pending native refresh survives restore across the boundary");
        }
    }
    bool early_active = false, admitted = false;
    for (const auto& service : host->setup.journal->services) {
        if (service.player == 1 && service.frame < 5) early_active = true;
        if (service.player == 2) {
            expect(service.frame >= 7, "AI admission: missed boundary admits at the first enabled turn");
            admitted = true;
        }
    }
    expect(early_active && admitted, "AI admission: existing AI runs before the other side is admitted");
}

void scoring_startup_contract() {
    using Scoring = eawr::platform::BattleScoring;
    const auto scorer = [](std::string definitions, std::string service) {
        auth::ModuleManifest manifest;
        expect(manifest.add("data/scripts/miscellaneous/OpeningProbe.lua",
            "function Base_Definitions() calls=0; " + definitions + " end\n"
            "function main() while true do calls=calls+1; " + service + "; coroutine.yield(true) end end\n"
            "function Get_Game_Stat_For_Control_ID() return calls end\n").has_value(), "FSC-77: scoring module admitted");
        return Scoring::from_scripts(std::move(manifest), "OpeningProbe", "synthetic");
    };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    auto world = tactical::TacticalSession::create(setup);
    expect(world.has_value(), "FSC-77: synthetic world creates");
    if (!world) return;
    // Independent IEEE-754 control values: widened binary32(frame) / binary32(30).
    for (const auto& [frame, expected] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
            {1, 0x3fa1111120000000ULL}, {3, 0x3fb99999a0000000ULL}, {300, 0x4024000000000000ULL},
            {1000, 0x4040aaaaa0000000ULL}, {16777217, 0x4121111120000000ULL}}) {
        expect(foc::scoring_seconds(frame).repr == expected, "FC-76: independent synchronized clock control");
    }
    const std::string exact_first_stamp = "0.033333335071802139";
    std::cout << "FSC-77 synchronized first stamp bits=" << std::hex << foc::scoring_seconds(1).repr << std::dec << '\n';
    for (const auto& [definitions, admitted] : std::vector<std::pair<std::string, bool>>{
            {"ServiceRate=10; LastService=nil", true}, {"ServiceRate=0; LastService=" + exact_first_stamp, false},
            {"ServiceRate=0; LastService=0", true}, {"ServiceRate='invalid'; LastService=nil", false}}) {
        auto scoring = scorer(definitions, "Suspend_AI(1)");
        expect(scoring && scoring.value()->pumps() == 0, "FSC-77: creating main does not pump it");
        if (!scoring) continue;
        auto write = scoring.value()->visit(*world.value().snapshot());
        expect(write && write.value().has_value() == admitted && (!admitted || *write.value()),
            "FSC-77: absent/older stamp admit; equality/nonnumeric rate skip");
    }
    auto exempt = scorer("ServiceRate=10", "calls=calls");
    auto exempt_write = exempt.value()->visit(*world.value().snapshot());
    expect(exempt_write && !exempt_write.value(), "FSC-77: suspension-exempt pump is not a hold");
    auto negative = scorer("ServiceRate=10", "Suspend_AI(-1)");
    auto negative_write = negative.value()->visit(*world.value().snapshot());
    expect(negative_write && negative_write.value() && !*negative_write.value(), "FD-72: only a positive numeric argument suspends");
    auto unsupported = scorer("ServiceRate=10", "Unsupported_Opening_API()");
    expect(!unsupported.value()->visit(*world.value().snapshot()), "FSC-77: unsupported API is not a hold");
    auto dynamic = scorer("ServiceRate=10", "ServiceRate=0; Suspend_AI(1)");
    expect(dynamic.value()->visit(*world.value().snapshot()).has_value(), "FSC-77: rate-changing first pump runs");
    auto dynamic_write = dynamic.value()->visit(*world.value().snapshot());
    expect(dynamic_write && dynamic_write.value() && dynamic.value()->pumps() == 2, "FSC-77: rate is reread each visit");

    std::vector<std::string> golden;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scoring = scorer("ServiceRate=10; LastService=nil", "Suspend_AI(1)");
        if (!scoring) return;
        foc::AiSetup ai_setup;
        ai_setup.scoring = std::move(scoring).value();
        const std::map<std::string, std::string> modules{
            {"StoryProbe.lua", "function Release() Suspend_AI(0) end"},
            {"data/scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
        auto prepared = foc::prepare_session(ai_setup, modules);
        expect(prepared.has_value(), "FSC-78: production tactical host prepares");
        if (!prepared) return;
        expect(prepared.value().scripts.create_instance(99, "StoryProbe.lua").has_value(), "FSC-78: story instance attaches");
        auth::ScriptEvent release;
        release.key = {3, auth::first_simulation_producer, 99, 0};
        release.target = 99; release.kind = auth::ScriptEvent::Kind::call; release.name = "Release";
        expect(prepared.value().scripts.submit_event(release).has_value(), "FSC-78: late story release schedules");
        auto fresh_world = tactical::TacticalSession::create(setup);
        auto session = auth::ScriptedTacticalSession::create(std::move(fresh_world).value(), std::move(prepared.value().scripts));
        expect(session && prepared.value().bind(session.value()), "FSC-77: pre-world scoring binds");
        if (!session) return;
        std::vector<std::string> hashes;
        for (unsigned frame = 1; frame <= 4; ++frame) {
            auto tick = session.value().step(executor);
            expect(tick.has_value(), "FSC-77/78: production session steps");
            if (!tick) return;
            hashes.push_back(tick.value().state_sha256);
            auto saved = session.value().save_scripts();
            expect(saved && saved.value().native, "FD-72: gate and scorer join authoritative state");
            if (!saved || !saved.value().native) return;
            const auto& native = *saved.value().native;
            std::istringstream input{std::string(native.begin(), native.end())};
            std::string tag, identity; unsigned suspended{};
            input >> tag >> identity >> suspended;
            expect(suspended == (frame < 3 ? 1U : 0U), "FSC-77/78: early hold persists; late release commits at r");
        }
        if (golden.empty()) golden = hashes;
        else expect(golden == hashes, "FD-71/72: startup hashes equal on 1/2/4/8 workers");
        auto saved = ai_setup.scoring->save();
        auto restored = scorer("ServiceRate=10; LastService=nil", "Suspend_AI(1)");
        expect(saved && restored && restored.value()->load(saved.value()), "FSC-78: retained scoring loads");
        const auto old_pumps = restored.value()->pumps();
        auto no_write = restored.value()->visit(*world.value().snapshot());
        expect(no_write && !no_write.value() && restored.value()->pumps() == old_pumps,
            "FSC-78: retained clock/stamp skips rather than pumping fresh child time");
    }
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto host = std::make_shared<ai::Host>();
        auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE"; mode->frame = 1;
        host->ai_world = mode; host->view = mode->view();
        host->setup.players = {foc::AiPlayer{2, "EMPIRE", false, true, "OPENING"}};
        host->setup.schedule.mode = foc::AiSchedule::Mode::faithful;
        host->setup.journal = std::make_shared<foc::AiJournal>();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        ai::AiData data;
        data.equations = std::move(ai::EquationSet::parse({{"Opening.xml", "<Root><Positive>2</Positive></Root>"}}, converters)).value();
        ai::PlayerType player; player.function_sets = {"OPENING"}; data.players["OPENING"] = player;
        ai::GoalType goal; goal.mode = "SPACE"; goal.application = {"GLOBAL"};
        data.goals["OPENING"] = goal; data.function_sets["OPENING"] = {{"OPENING", "Positive"}};
        ai::Engine engine(host, std::move(data), {});
        auto scripts = auth::ScriptScheduler::create({}, {});
        std::uint64_t sequence{};
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        host->ai_suspended = true;
        for (mode->frame = 1; mode->frame <= 3; ++mode->frame) {
            expect(engine.before_service(*mode, scripts.value(), sequence, &executor).has_value(), "FSC-77: held native AI initializes");
            const auto* state = ai::EngineContractAccess::player(engine, 2);
            expect(state && state->next_goal == 0 && state->nontrivial == 0 && mode->evaluations == 0
                && host->setup.journal->services.empty(), "FSC-77: no held positive proposal or deadline rearm");
        }
        host->ai_suspended = false;
        expect(engine.before_service(*mode, scripts.value(), sequence, &executor).has_value(), "FSC-78: early release admits overdue AI on r");
        expect(mode->evaluations > 0 && !host->setup.journal->services.empty(), "FSC-78: positive budgeted proposal runs after release");
        host->ai_suspended = true; ++mode->frame;
        const auto count = mode->evaluations;
        expect(engine.before_service(*mode, scripts.value(), sequence, &executor).has_value() && mode->evaluations == count,
            "FSC-78: held AI turn precedes late story release");
        host->ai_suspended = false; ++mode->frame;
        expect(engine.before_service(*mode, scripts.value(), sequence, &executor).has_value(), "FSC-78: story release admits next turn");
    }
    // Cheap production-seam fixture: scoring holds real AI freestores before
    // their first turn, releases early, then holds again. No retail assets.
    std::vector<std::string> freestore_golden;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        foc::AiSetup ai_setup;
        ai_setup.players = {{1, "SYNTHETIC", false, true, ""}, {2, "SYNTHETIC", false, true, ""}};
        ai_setup.freestore_module = "Probe.lua";
        ai_setup.scoring = scorer("ServiceRate=0", "if calls==1 or calls==7 then Suspend_AI(1) elseif calls==4 or calls==10 then Suspend_AI(0) end").value();
        const std::map<std::string, std::string> modules{
            {"Probe.lua", "function Base_Definitions() ServiceRate=0; UnitServiceRate=0; pumps=0 end\nfunction main() while true do pumps=pumps+1; coroutine.yield(true) end end"},
            {"data/scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
        auto new_world = tactical::TacticalSession::create(setup);
        auto session = foc::create_session(std::move(new_world).value(), ai_setup, modules);
        expect(session.has_value(), "FD-73: scoring/freestore fixture binds production seam");
        if (!session) return;
        std::vector<std::string> hashes;
        for (unsigned frame = 1; frame <= 12; ++frame) {
            auto tick = session.value().step(executor);
            expect(tick.has_value(), "FD-73: scoring/freestore production turn completes");
            if (!tick) return;
            hashes.push_back(tick.value().state_sha256);
            auto saved = session.value().save_scripts();
            auto inspector = foc::prepare_session(ai_setup, modules);
            expect(saved && saved.value().native && inspector && inspector.value().scripts.load(saved.value().scheduler), "FD-73: freestore stamps and pending hold are saved");
            if (!saved || !inspector) return;
            for (unsigned id = 1; id <= 2; ++id) {
                const unsigned before_hold = frame < 4 ? 0 : std::min(frame, 6U) - (id == 1 ? 3U : 4U);
                const unsigned after_hold = frame < 10 ? 0 : frame - 9;
                auto count = inspector.value().scripts.read_global(foc::freestore_instance(id), "pumps");
                expect(count && count.value() && std::get<Real>(count.value()->data) == ai::real(before_hold + after_hold),
                    "SCH-03: first enabled freestore phases survive startup and mid-battle holds");
            }
            if (frame == 2 || frame == 8 || frame == 11) {
                auto checkpoint = session.value().world().save_checkpoint_state();
                auto rebuilt_world = tactical::TacticalSession::create(setup);
                auto rebuilt_ai = foc::prepare_session(ai_setup, modules);
                expect(checkpoint && rebuilt_world && rebuilt_ai && rebuilt_world.value().load_checkpoint_state(checkpoint.value()), "FD-73: held world checkpoint rebuilds");
                if (!checkpoint || !rebuilt_world || !rebuilt_ai) return;
                auto rebuilt = auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(), std::move(rebuilt_ai.value().scripts), saved.value());
                expect(rebuilt && rebuilt_ai.value().bind(rebuilt.value()), "FD-73: pending hold/scorer restores into fresh host");
                if (!rebuilt) return;
                expect(rebuilt.value().save_scripts().value().native == saved.value().native, "FD-73: freestore hold native bytes round trip");
                session = std::move(rebuilt);
            }
        }
        if (freestore_golden.empty()) freestore_golden = hashes;
        else expect(freestore_golden == hashes, "FD-73: production scoring/freestore hold hashes equal on 1/2/4/8 workers");
    }
    // SCH-03 / FD-73: positive rates distinguish both stamp translations on
    // each release. Mode time is the completed frame; initial stamps are 0 and 1/30.
    // Seven/four enabled ticks exceed the respective 0.21/0.11 second rates.
    std::vector<std::string> periodic_freestore_golden;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto periodic_setup = setup;
        periodic_setup.units = {{1, 100, 1}, {2, 100, 2}};
        foc::AiSetup ai_setup;
        ai_setup.players = {{1, "SYNTHETIC", false, true, ""}, {2, "SYNTHETIC", false, true, ""}};
        foc::AiType type;
        type.type_id = 100;
        type.locomotor = true;
        ai_setup.content.types = {type};
        ai_setup.freestore_module = "PeriodicProbe.lua";
        ai_setup.scoring = scorer("ServiceRate=0", "if calls==1 or calls==14 then Suspend_AI(1) elseif calls==4 or calls==17 then Suspend_AI(0) end").value();
        const std::map<std::string, std::string> modules{
            {"PeriodicProbe.lua", "function Base_Definitions() ServiceRate=0.21; UnitServiceRate=0.11; pumps=0; unit_calls=0 end\n"
                "function main() while true do pumps=pumps+1; coroutine.yield(true) end end\n"
                "function On_Unit_Service(unit) unit_calls=unit_calls+1 end"},
            {"data/scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
        auto new_world = tactical::TacticalSession::create(periodic_setup);
        expect(new_world.has_value(), "SCH-03: periodic freestore world creates");
        if (!new_world) return;
        auto session = foc::create_session(std::move(new_world).value(), ai_setup, modules);
        expect(session.has_value(), "SCH-03: periodic freestore binds production seam");
        if (!session) return;
        std::vector<std::string> hashes;
        for (unsigned frame = 1; frame <= 29; ++frame) {
            auto tick = session.value().step(executor);
            expect(tick.has_value(), "SCH-03: periodic freestore turn completes");
            if (!tick) return;
            hashes.push_back(tick.value().state_sha256);
            auto saved = session.value().save_scripts();
            auto inspector = foc::prepare_session(ai_setup, modules);
            expect(saved && inspector && inspector.value().scripts.load(saved.value().scheduler),
                "SCH-03: periodic freestore globals inspect");
            if (!saved || !inspector) return;
            for (unsigned id = 1; id <= 2; ++id) {
                const unsigned phase = id - 1;
                unsigned pumps = 0, unit_calls = 0;
                for (const unsigned due_frame : {10U, 20U, 27U})
                    if (frame >= due_frame + phase) ++pumps;
                for (const unsigned due_frame : {7U, 11U, 18U, 22U, 26U})
                    if (frame >= due_frame + phase) ++unit_calls;
                auto pump_count = inspector.value().scripts.read_global(foc::freestore_instance(id), "pumps");
                auto unit_count = inspector.value().scripts.read_global(foc::freestore_instance(id), "unit_calls");
                expect(pump_count && pump_count.value() && std::get<Real>(pump_count.value()->data) == ai::real(pumps),
                    "SCH-03: positive-rate pump frames preserve phase across both holds");
                expect(unit_count && unit_count.value() && std::get<Real>(unit_count.value()->data) == ai::real(unit_calls),
                    "SCH-03: positive-rate unit frames preserve phase across both holds");
            }
        }
        if (periodic_freestore_golden.empty()) periodic_freestore_golden = hashes;
        else expect(periodic_freestore_golden == hashes,
            "SCH-03: periodic freestore hashes equal on 1/2/4/8 workers");
    }
    // FSC-79 / FD-73: independent enabled-turn oracle, including pending
    // initial offsets, periodic rearming, repeated holds and native restore.
    for (const auto schedule : {foc::AiSchedule::Mode::staggered, foc::AiSchedule::Mode::faithful}) {
      for (const bool initial_hold : {false, true}) {
        std::vector<std::vector<std::uint8_t>> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto host = std::make_shared<ai::Host>();
            auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE";
            host->ai_world = mode; host->view = mode->view();
            host->setup.schedule.mode = schedule;
            host->setup.journal = std::make_shared<foc::AiJournal>();
            for (unsigned id = 1; id <= 4; ++id) host->setup.players.push_back({id, "SYNTHETIC", false, true, "OPENING"});
            for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
            const auto make_engine = [&]() {
                const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
                ai::AiData data;
                data.equations = std::move(ai::EquationSet::parse({{"Opening.xml", "<Root><Positive>2</Positive></Root>"}}, converters)).value();
                ai::PlayerType player; player.name = "OPENING"; player.function_sets = {"OPENING"}; data.players["OPENING"] = player;
                ai::GoalType goal; goal.name = "OPENING"; goal.mode = "SPACE"; goal.application = {"GLOBAL"};
                data.goals["OPENING"] = goal; data.function_sets["OPENING"] = {{"OPENING", "Positive"}};
                return std::make_unique<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{});
            };
            auto engine = make_engine();
            auto scripts = auth::ScriptScheduler::create({}, {});
            const std::string identity(64, 'a');
            std::uint64_t sequence{}, enabled_turns{};
            const std::uint64_t first = initial_hold ? 4U : 1U;
            std::vector<std::vector<std::uint8_t>> states;
            for (mode->frame = 1; mode->frame <= first + 9; ++mode->frame) {
                const auto frame = mode->frame;
                host->ai_suspended = frame < first || (frame > first && frame < first + 4);
                if (!host->ai_suspended) ++enabled_turns;
                const auto rows_before = host->setup.journal->services.size();
                expect(engine->before_service(*mode, scripts.value(), sequence, &executor).has_value(), "FD-73: held/released native turn runs");
                std::vector<foc::AiServiceEvent> expected;
                if (!host->ai_suspended) for (unsigned index = 0; index < 4; ++index) {
                    const bool phased = schedule == foc::AiSchedule::Mode::staggered;
                    const bool goals = !phased || enabled_turns >= index + 1;
                    const bool periodic = phased ? (enabled_turns - 1) % 3 == index % 3
                        : frame == first || (frame - (first + 4)) % 3 == 0;
                    if (goals || periodic) expected.push_back({frame, index + 1, goals, periodic, periodic});
                    const auto* player = ai::EngineContractAccess::player(*engine, index + 1);
                    if (phased) expect(player && player->next_learning == static_cast<std::int64_t>(enabled_turns > index % 3 ? 301 + index % 3 : 1 + index % 3),
                        "FD-73: learning's first offset and long-period rearm survive a hold");
                    if (phased) expect(engine->execution_due(index + 1, static_cast<std::int64_t>(frame + 1))
                        == (enabled_turns % 3 == index % 3),
                        "FD-73: next-frame execution query uses the same enabled deadline clock after release");
                }
                const auto& observed = host->setup.journal->services;
                expect(observed.size() == rows_before + expected.size(), "FD-73 / FC-76: exact per-player due row count after release");
                for (std::size_t i = 0; i < expected.size() && rows_before + i < observed.size(); ++i) {
                    const auto& actual = observed[rows_before + i]; const auto& wanted = expected[i];
                    expect(actual.frame == wanted.frame && actual.player == wanted.player && actual.goals == wanted.goals
                        && actual.planning == wanted.planning && actual.execution == wanted.execution,
                        "FD-73 / FC-76: goal/planning/execution due frames match independent oracle");
                }
                auto report = scripts.value().service(executor);
                expect(report && engine->after_service(report.value()), "FD-73: scheduler barrier completes");
                auto state = engine->save_tactical_native(identity, scripts.value());
                expect(state.has_value(), "FD-73: native schedule continuation saves");
                if (!state) { std::cerr << state.error().message << '\n'; return; }
                states.push_back(state.value());
                if (frame == 2 || frame == first + 2 || frame == first + 5) {
                    auto rebuilt = make_engine();
                    auto loaded = rebuilt->restore_tactical_native(state.value(), identity, scripts.value());
                    expect(loaded.has_value(), "FD-73: fresh native engine restores across startup/mid-battle hold and release");
                    if (!loaded) { std::cerr << loaded.error().message << '\n'; return; }
                    expect(rebuilt->save_tactical_native(identity, scripts.value()).value() == state.value(), "FD-73: native continuation canonical round trip");
                    engine = std::move(rebuilt);
                }
            }
            expect(mode->evaluations > 0, "FD-73: released positive proposal fixture actually evaluates");
            if (baseline.empty()) baseline = states;
            else expect(baseline == states, "FD-73: native bytes agree on 1/2/4/8 workers across every hold barrier");
        }
      }
    }
}

// GFS-A24: tactical players use the same per-player stream mechanism as galactic players,
// including the native continuation. IDs and player service order remain unchanged.
void tactical_player_streams() {
    std::vector<std::uint8_t> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto host = std::make_shared<ai::Host>();
        auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE"; mode->frame = 1;
        host->ai_world = mode; host->view = mode->view(); host->setup.seed = 12345;
        // Human and noncontiguous AI IDs exercise seeding by ID rather than dense AI index.
        host->setup.players = {{1, "SYNTHETIC", false, false, ""},
            {9, "SYNTHETIC", false, true, "OPENING"}, {7, "SYNTHETIC", false, true, "OPENING"}};
        for (const auto& p : host->setup.players) host->players.emplace(p.player, &p);
        const auto make_engine = [&]() {
            ai::AiData data; ai::PlayerType type; type.name = "OPENING";
            const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
            auto equations = ai::EquationSet::parse({{"retained.xml", "<Root><Retained>0</Retained></Root>"}}, converters);
            if (!equations) throw std::runtime_error("retained-plan fixture equations failed");
            data.equations = std::move(equations).value();
            ai::GoalType goal; goal.name = "RETAINED"; goal.mode = "SPACE";
            data.goals.emplace(goal.name, goal);
            data.function_sets["OPENING"] = {{"RETAINED", "Retained"}};
            type.function_sets = {"OPENING"};
            data.players.emplace(type.name, type);
            ai::PlanDef plan; plan.name = "RETAINED"; plan.goals = {"RETAINED"}; plan.taskforces.emplace_back();
            return std::make_unique<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{plan});
        };
        auto engine = make_engine();
        auth::ModuleManifest modules;
        expect(modules.add("RETAINED.LUA", "Retained = true").has_value(), "GFS-A24: retained-plan module loads");
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        std::uint64_t sequence{};
        expect(scripts && engine->before_service(*mode, scripts.value(), sequence, &executor),
            "GFS-A24: tactical players initialize through the real barrier");
        if (!scripts) return;
        for (const unsigned id : {7U, 9U}) {
            const auto* player = ai::EngineContractAccess::player(*engine, id);
            expect(player && player->sync.seed() == ai::galactic_stream_seed(12345, id),
                "GFS-A24: tactical stream seed mixes battle seed and player ID");
        }
        ai::AiRandom first, second;
        first.set_seed(ai::galactic_stream_seed(12345, 7));
        second.set_seed(ai::galactic_stream_seed(12345, 9));
        for (unsigned i = 0; i < 7; ++i) {
            expect(ai::EngineContractAccess::draw(*engine, 7) == first.next(), "GFS-A24: first player's stream continues independently");
            if (i % 2 == 0) expect(ai::EngineContractAccess::draw(*engine, 9) == second.next(),
                "GFS-A24: another player's intervening draws do not alter the sequence");
        }
        expect(ai::EngineContractAccess::engine_seed(*engine) == 12345,
            "GFS-A24: player draws leave the engine-wide stream untouched");
        auto report = scripts.value().service(executor);
        expect(report && engine->after_service(report.value()), "GFS-A24: tactical stream barrier completes");
        const std::string identity(64, 'a');
        auto saved = engine->save_tactical_native(identity, scripts.value());
        expect(saved.has_value(), "GFS-A24: advanced tactical streams save");
        if (!saved) { std::cerr << saved.error().message << '\n'; return; }
        if (reference.empty()) reference = saved.value();
        else expect(reference == saved.value(), "GFS-A24: native bytes equal on 1/2/4/8 workers");
        auto restored = make_engine();
        const auto loaded = restored->restore_tactical_native(saved.value(), identity, scripts.value());
        expect(loaded.has_value(), "GFS-A24: fresh tactical engine restores streams");
        if (!loaded) return;
        expect(restored->save_tactical_native(identity, scripts.value()).value() == saved.value(),
            "GFS-A24: advanced stream states round-trip canonically");
        for (unsigned i = 0; i < 8; ++i) for (const unsigned id : {9U, 7U})
            expect(ai::EngineContractAccess::draw(*engine, id) == ai::EngineContractAccess::draw(*restored, id),
                "GFS-A24: every restored player continues at its own next draw");
        auto interleaved = make_engine();
        expect(interleaved->restore_tactical_native(saved.value(), identity, scripts.value()).has_value(), "interleaved codec owner initializes");
        ai::EngineContractAccess::interleaved_native_fixture(*interleaved);
        const auto interleaved_bytes = interleaved->save_tactical_native(identity, scripts.value());
        auto interleaved_restore = make_engine();
        expect(interleaved_bytes && interleaved_restore->restore_tactical_native(interleaved_bytes.value(), identity, scripts.value()),
            "interleaved plans, forces, blocks and instances restore across multiple player owners");
        if (!interleaved_bytes) return;
        expect(interleaved_restore->plan(1) && interleaved_restore->plan(1)->player == 9
            && interleaved_restore->plan(2) && interleaved_restore->plan(2)->player == 7
            && interleaved_restore->taskforce(3) && interleaved_restore->block(ai::block_id(ai::first_plan_instance + 2, 17))
            && interleaved_restore->save_tactical_native(identity, scripts.value()).value() == interleaved_bytes.value(),
            "globally interleaved codec bytes are independent of the decoded positioning shard");
        ai::EngineContractAccess::retain_plan_without_instance(*engine, 7, true);
        const auto exited = engine->save_tactical_native(identity, scripts.value());
        if (!exited) { expect(false, exited.error().message); return; }
        auto completed = make_engine();
        expect(exited && completed->restore_tactical_native(exited.value(), identity, scripts.value()),
            "GFS-A24: exited plan restores after its Lua instance was removed, before owner cleanup");
        expect(exited && completed->save_tactical_native(identity, scripts.value()).value() == exited.value(),
            "GFS-A24: pending exited-plan cleanup and positioning issuer/deadline/target round-trip exactly");
        if (workers == 1) {
            // v9 ends in count + ordered (unit, instance, deadline, target) words.
            // Mutate well-formed bytes to exercise decoding, independently of the writer.
            const auto word = [](std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
                for (std::size_t index = 0; index < 8; ++index)
                    bytes.at(offset + index) = static_cast<std::uint8_t>(value >> (index * 8));
            };
            const auto trailer = exited.value().size() - 40;
            const auto refused = [&](const std::vector<std::uint8_t>& bytes, const std::string& name) {
                auto destination = make_engine();
                const auto before = destination->save_tactical_native(identity, scripts.value());
                expect(!destination->restore_tactical_native(bytes, identity, scripts.value()),
                    "GFS-A24: positioning decode refuses " + name);
                const auto after = destination->save_tactical_native(identity, scripts.value());
                expect(before && after && before.value() == after.value(),
                    "GFS-A24: invalid positioning leaves owner unpublished: " + name);
            };
            const std::array<std::tuple<std::size_t, std::uint64_t, std::string_view>, 8> malformed{{
                {8, 0, "zero unit"}, {8, 701, "unissued unit"}, {32, 701, "unissued target"},
                {16, ai::first_plan_instance - 1, "low instance"},
                {16, ai::first_plan_instance + 1, "unissued instance"},
                {24, std::numeric_limits<std::uint64_t>::max(), "negative deadline"},
                {24, 152, "deadline beyond current frame plus150"},
                {24, std::numeric_limits<std::int64_t>::max(), "extreme deadline"}}};
            for (const auto& [offset, value, name] : malformed) {
                auto bytes = exited.value(); word(bytes, trailer + offset, value);
                refused(bytes, std::string(name));
            }
            for (const auto key : {9U, 10U}) {
                auto bytes = exited.value();
                const std::vector<std::uint8_t> row(bytes.end() - 32, bytes.end());
                word(bytes, trailer, 2);
                const auto second_row_offset = bytes.size(); bytes.insert(bytes.end(), row.begin(), row.end());
                word(bytes, second_row_offset, key);
                refused(bytes, key == 9 ? "descending unit keys" : "duplicate unit keys");
            }
        }
        // Save between positioning deadlines, then exercise the exact future
        // deadline and its next150-frame cycle against the uninterrupted owner.
        auto view = std::make_shared<ai::WorldView>();
        view->players = {{7, 7, 0}, {9, 9, 1}};
        foc::AiType type; type.type_id = 1; type.name = "POSITIONING_SHIP";
        type.locomotor = true; type.max_attack_distance = ai::real(1000);
        type.bounds_half_x = type.bounds_half_y = ai::real(50);
        host->setup.content.types = {type}; host->types[1] = &host->setup.content.types.front();
        ai::ViewUnit actor; actor.id = 10; actor.type = 1; actor.owner = 7;
        actor.position = {eawr::sim::math::Fixed::from_raw(-3000 * eawr::sim::math::Fixed::scale), {}, {}};
        actor.order = tactical::OrderKind::attack_move;
        ai::ViewUnit target; target.id = 11; target.type = 1; target.owner = 9;
        view->units = {actor, target}; mode->world_view = view; host->view = view;
        const auto advance_scripts = [&](std::uint64_t frame) {
            while (scripts.value().completed_tick() < frame)
                if (!scripts.value().service(executor)) return false;
            mode->frame = frame; view->tick = frame;
            return true;
        };
        if (!advance_scripts(50)) { expect(false, "positioning scheduler reaches midpoint"); return; }
        ai::EngineContractAccess::positioning_service(*engine, 50);
        // The row survives real plan removal and a transfer to another AI owner.
        ai::EngineContractAccess::depart_positioning_plan(*engine, 7);
        view->units[0].owner = 9; view->units[1].owner = 7;
        const auto midpoint = engine->save_tactical_native(identity, scripts.value());
        auto resumed = make_engine();
        expect(midpoint && resumed->restore_tactical_native(midpoint.value(), identity, scripts.value()),
            "GFS-A24: positioning restores mid-cycle with a departed issuing plan");
        expect(ai::EngineContractAccess::checked_player(*engine, 7).positioning.contains(10)
            && ai::EngineContractAccess::checked_player(*resumed, 9).positioning.contains(10),
            "positioning shard may follow changed ownership without changing continuation bytes");
        std::size_t expected_orders{};
        for (const auto frame : {75U, 76U, 225U, 226U}) {
            if (!advance_scripts(frame)) { expect(false, "positioning scheduler advances"); return; }
            ai::EngineContractAccess::positioning_service(*engine, frame);
            ai::EngineContractAccess::positioning_service(*resumed, frame);
            if (frame == 76 || frame == 226) ++expected_orders;
            expect(ai::EngineContractAccess::point_defence_orders(*engine) == expected_orders
                && ai::EngineContractAccess::point_defence_orders(*resumed) == expected_orders,
                "GFS-A24: restored positioning preserves exact mid-cycle and repeat deadlines");
            const auto left = engine->save_tactical_native(identity, scripts.value());
            const auto right = resumed->save_tactical_native(identity, scripts.value());
            expect(left && right && left.value() == right.value(),
                "GFS-A24: positioning continuation emits identical orders and native bytes");
        }
        view->units[0].owner = 1;
        const auto transferred = engine->save_tactical_native(identity, scripts.value());
        auto fallback = make_engine();
        const auto fallback_loaded = transferred
            ? fallback->restore_tactical_native(transferred.value(), identity, scripts.value())
            : eawr::core::Result<void>::failure(transferred.error());
        expect(fallback_loaded && ai::EngineContractAccess::checked_player(*fallback, 7).positioning.contains(10)
            && fallback->save_tactical_native(identity, scripts.value()).value() == transferred.value(),
            "departed positioning issuer transferred to a human uses the lowest AI shard and retains exact bytes");
        ai::EngineContractAccess::retain_plan_without_instance(*engine, 7, false);
        const auto live_missing = engine->save_tactical_native(identity, scripts.value());
        auto invalid = make_engine();
        expect(live_missing && !invalid->restore_tactical_native(live_missing.value(), identity, scripts.value()),
            "GFS-A24: live plan still refuses a missing Lua instance");
        expect(scripts.value().create_instance(ai::first_plan_instance, "RETAINED.LUA").has_value(),
            "GFS-A24: retained plan attaches a real Lua instance");
        const auto live_present = engine->save_tactical_native(identity, scripts.value());
        auto live = make_engine();
        expect(live_present && live->restore_tactical_native(live_present.value(), identity, scripts.value()),
            "GFS-A24: live plan accepts its present Lua instance");
        ai::EngineContractAccess::retain_plan_without_instance(*engine, 7, true);
        const auto exited_present = engine->save_tactical_native(identity, scripts.value());
        auto departed = make_engine();
        expect(exited_present && !departed->restore_tactical_native(exited_present.value(), identity, scripts.value()),
            "GFS-A24: exited plan refuses a still-present Lua instance");
    }
}

// The error path. A failing turn ends the player loop where the sequential pass would, the effects staged so far commit
// outside any turn, and nothing leaks into the next barrier. The faithful schedule writes directly and is the reference;
// the staggered schedule gives each player its own phase, so the run steps frames until the failing player's turn.
struct HookRun {
    bool ok{};
    std::string message;
    std::vector<tactical::PlayerId> calls;
    std::uint64_t consumed{};
    std::size_t staged{};
    bool bound{};
};

HookRun effects_hook_run(const foc::AiSchedule::Mode mode, const tactical::PlayerId failing) {
    auto host = std::make_shared<ai::Host>();
    auto world = std::make_shared<ModeWorld>();
    world->mode = "SPACE";
    host->ai_world = world;
    host->view = std::make_shared<ai::WorldView>();
    host->setup.schedule.mode = mode;
    host->setup.players = {foc::AiPlayer{2, "EMPIRE", false, true, "MODE"}, foc::AiPlayer{3, "REBEL", false, true, "MODE"},
        foc::AiPlayer{4, "UNDERWORLD", false, true, "MODE"}};
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto parsed = ai::EquationSet::parse({{"mode.xml", "<Root><Count>2</Count></Root>"}}, converters);
    HookRun run;
    expect(parsed.has_value(), "error path equations parse");
    if (!parsed) return run;
    ai::AiData data; data.equations = std::move(parsed).value();
    ai::PlayerType player; player.function_sets = {"MODE"}; data.players["MODE"] = player;
    ai::GoalType space; space.mode = "SPACE";
    data.goals["SPACE"] = space;
    data.function_sets["MODE"] = {{"SPACE", "Count"}};
    ai::Engine engine(host, std::move(data), {});
    auth::ModuleManifest manifest;
    expect(manifest.add("HOOK.LUA", "LastService = 0").has_value(), "error path module loads");
    auto scripts = auth::ScriptScheduler::create({}, std::move(manifest));
    if (!scripts) { expect(false, "error path scheduler creates"); return run; }
    expect(scripts.value().create_instance(1, "HOOK.LUA").has_value(), "error path instance creates");
    constexpr std::uint64_t first_sequence = 17;
    std::uint64_t sequence = first_sequence;
    const ai::Engine::ExecutionHook hook = [&](tactical::PlayerId id, ai::Engine::ExecutionEffects& effects) -> eawr::core::Result<void> {
        run.calls.push_back(id);
        for (int copy = 0; copy < 2; ++copy) {
            auth::ScriptEvent event;
            event.key = {world->frame + 1, foc::producer_foc_engine, 1, effects.sequence++};
            event.target = 1;
            event.kind = auth::ScriptEvent::Kind::pump;
            event.name = "LastService";
            if (auto submitted = effects.submit(std::move(event)); !submitted) return submitted;
        }
        if (id == failing)
            return eawr::core::Result<void>::failure(foc::detail::make_error(auth::codes::invalid_request, "turn failure"));
        return eawr::core::Result<void>::success();
    };
    const eawr::platform::ThreadWorkerAdapter executor(2);
    for (int frame = 0; frame < 120 && !run.ok && run.message.empty(); ++frame, ++world->frame) {
        const auto serviced = engine.before_service(*world, scripts.value(), sequence, &executor, hook);
        if (!serviced) run.message = serviced.error().message;
    }
    run.ok = run.message.empty();
    run.consumed = ai::EngineContractAccess::event_sequence(engine) - first_sequence;
    run.staged = ai::EngineContractAccess::staged_effects(engine);
    run.bound = ai::EngineContractAccess::staging(engine);
    return run;
}

void effects_error_path() {
    // A failing turn: the barrier fails with the turn's error, the player loop stops there, every hook allocation up to
    // and including the failing turn's is consumed, and nothing stays staged or bound.
    for (const auto mode : {foc::AiSchedule::Mode::faithful, foc::AiSchedule::Mode::staggered}) {
        const auto run = effects_hook_run(mode, 3);
        expect(!run.ok && run.message == "turn failure", "a failing turn fails the barrier with its own error");
        expect(!run.calls.empty() && run.calls.back() == 3 && run.consumed == 2 * run.calls.size(),
            "the loop stops at the failing turn and its hook allocations are consumed");
        expect(run.staged == 0 && !run.bound, "a failed barrier leaves no staged effects and no bound turn");
    }
    // A staged submit the scheduler refuses fails at the commit. The sequential pass stopped there: nothing of the failing
    // turn after the refused event and nothing of a later player applies, and the refused event's number is consumed.
    World world;
    ai::PlanDef definition; definition.name = "OWNED";
    ai::Engine engine(world.host, {}, {definition});
    auth::ModuleManifest modules;
    auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
    if (!scripts) { expect(false, "commit failure scheduler creates"); return; }
    ai::EngineContractAccess::bare_players(engine, scripts.value());
    ai::EngineContractAccess::add_player(engine, 11);
    const auto before = ai::EngineContractAccess::event_sequence(engine);
    ai::EngineContractAccess::turn(engine, 7, [&](ai::PlayerAi&) {
        ai::EngineContractAccess::stage_event(engine, 1); ai::EngineContractAccess::stage_event(engine, 1);
        ai::EngineContractAccess::stage_order(engine);
    });
    ai::EngineContractAccess::turn(engine, 9, [&](ai::PlayerAi&) {
        ai::EngineContractAccess::stage_event(engine, 0); // at or before the completed tick: refused
        ai::EngineContractAccess::stage_event(engine, 1);
        ai::EngineContractAccess::stage_order(engine);
    });
    ai::EngineContractAccess::turn(engine, 11, [&](ai::PlayerAi&) {
        ai::EngineContractAccess::stage_event(engine, 1);
        ai::EngineContractAccess::stage_order(engine);
    });
    const auto committed = ai::EngineContractAccess::commit(engine, scripts.value());
    expect(!committed && committed.error().message.find("future tick") != std::string::npos,
        "a refused staged submit fails the commit with the scheduler's error");
    expect(ai::EngineContractAccess::event_sequence(engine) - before == 3 && ai::EngineContractAccess::committed_orders(engine) == 1,
        "the commit stops at the refused event: later events, orders and players do not apply");
    expect(ai::EngineContractAccess::staged_effects(engine) == 0, "a failed commit leaves no staged effects");
}

void freestore_execution_hook_order() {
  for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
    auto host = std::make_shared<ai::Host>();
    auto mode = std::make_shared<ModeWorld>();
    mode->mode = "SPACE";
    host->ai_world = mode;
    host->view = std::make_shared<ai::WorldView>();
    host->setup.schedule.mode = foc::AiSchedule::Mode::faithful; // both players execute on this frame
    host->setup.players = {foc::AiPlayer{2, "EMPIRE", false, true, "MODE"},
        foc::AiPlayer{3, "REBEL", false, true, "MODE"}};
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto parsed = ai::EquationSet::parse({{"mode.xml", "<Root><Count>2</Count></Root>"}}, converters);
    expect(parsed.has_value(), "WTA-02: hook fixture equation parses");
    if (!parsed) return;
    ai::AiData data; data.equations = std::move(parsed).value();
    ai::PlayerType player; player.function_sets = {"MODE"}; data.players["MODE"] = player;
    ai::GoalType space; space.mode = "SPACE";
    data.goals["SPACE"] = space;
    data.function_sets["MODE"] = {{"SPACE", "Count"}};
    ai::Engine engine(host, std::move(data), {});
    auth::ModuleManifest manifest;
    expect(manifest.add("HOOK.LUA", "LastService = 0; LastUnitService = 0").has_value(),
        "WTA-02: hook fixture module loads");
    auto scripts = auth::ScriptScheduler::create({}, std::move(manifest));
    expect(scripts.has_value(), "WTA-02: hook fixture scheduler creates");
    if (!scripts) return;
    expect(scripts.value().create_instance(1, "HOOK.LUA").has_value(), "WTA-02: hook fixture instance creates");
    std::uint64_t sequence = 17;
    auth::ScriptEvent deferred;
    deferred.key = {1, foc::producer_foc_engine, 1, 0};
    deferred.target = 1;
    deferred.kind = auth::ScriptEvent::Kind::pump;
    ai::EngineContractAccess::defer_event(engine, std::move(deferred));
    std::vector<auth::EventKey> keys;
    std::vector<std::pair<tactical::PlayerId, bool>> calls;
    const ai::Engine::ExecutionHook hook = [&](tactical::PlayerId id, ai::Engine::ExecutionEffects& effects) -> eawr::core::Result<void> {
        if (calls.empty()) {
            for (const auto& event : ai::EngineContractAccess::attachment_events(engine)) keys.push_back(event.key);
        }
        const auto* serviced = ai::EngineContractAccess::player(engine, id);
        calls.emplace_back(id, serviced != nullptr && serviced->next_planning > engine.frame()
            && serviced->next_execution > engine.frame());
        // A due freestore test submits its pump and LastService assignment in this frame.
        for (const auto kind : {auth::ScriptEvent::Kind::pump, auth::ScriptEvent::Kind::assign}) {
            auth::ScriptEvent event;
            event.key = {1, foc::producer_foc_engine, 1, effects.sequence++};
            event.target = 1;
            event.kind = kind;
            event.name = "LastService";
            event.parameter = auth::Value{ai::real(1)};
            keys.push_back(event.key);
            if (auto submitted = effects.submit(std::move(event)); !submitted) return submitted;
        }
        return eawr::core::Result<void>::success();
    };
    const eawr::platform::ThreadWorkerAdapter executor(workers);
    expect(engine.before_service(*mode, scripts.value(), sequence, &executor, hook).has_value(), "WTA-02: hooked service runs");
    expect(calls.size() == 2 && calls[0].first == 2 && calls[1].first == 3 && calls[0].second && calls[1].second,
        "WTA-01/02: each player's freestore tests run once, inside the execution service, after planning");
    auth::ScriptEvent later;
    later.key = {1, foc::producer_foc_engine, 1, sequence++};
    later.target = 1;
    later.kind = auth::ScriptEvent::Kind::pump;
    keys.push_back(later.key);
    expect(scripts.value().submit_event(std::move(later)).has_value(), "WTA-02: later same-frame event submits without a duplicate key");
    expect(keys.size() == 6 && std::set<auth::EventKey>(keys.begin(), keys.end()).size() == keys.size(),
        "WTA-02: all submitted frame event keys are unique when freestore tests fire");
    for (std::size_t index = 1; index < keys.size(); ++index) {
        expect(keys[index - 1] < keys[index] && keys[index - 1].sequence < keys[index].sequence,
            "WTA-02: buffered, freestore and later event keys strictly increase");
    }
    expect(sequence == 23 && ai::EngineContractAccess::event_sequence(engine) == 22,
        "WTA-02: the barrier returns the counter after every hook allocation");
    expect(scripts.value().service(executor).has_value(), "WTA-02: all same-frame events service on 1/2/4/8 workers");
    expect(engine.runs_player(2) && !engine.runs_player(1), "WTA-02: only a player the goal system runs is tested by its execution service");
  }
}

void mode_world_contract() {
    auto host = std::make_shared<ai::Host>();
    auto mode = std::make_shared<ModeWorld>();
    host->ai_world = mode;
    host->view = std::make_shared<ai::WorldView>();
    host->setup.players = {foc::AiPlayer{2, "EMPIRE", false, true, "MODE"}};
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto parsed = ai::EquationSet::parse({{"mode.xml", "<Root><Count>Variable_Self.PlanetCount + 2</Count></Root>"}}, converters);
    expect(parsed.has_value(), "world interface: custom mode equation parses");
    if (!parsed) return;
    ai::AiData data; data.equations = std::move(parsed).value();
    ai::PlayerType player; player.function_sets = {"MODE"}; data.players["MODE"] = player;
    ai::GoalType galactic; galactic.mode = "GALACTIC";
    ai::GoalType space; space.mode = "SPACE";
    data.goals["GALACTIC"] = galactic; data.goals["SPACE"] = space;
    data.function_sets["MODE"] = {{"GALACTIC", "Count"}, {"SPACE", "Count"}};
    ai::Engine engine(host, std::move(data), {});
    auto scripts = auth::ScriptScheduler::create({}, {});
    expect(scripts.has_value(), "world interface: engine scheduler creates");
    if (!scripts) return;
    std::uint64_t sequence = 0;
    expect(engine.before_service(*mode, scripts.value(), sequence).has_value(), "world interface: service without a tactical world");
    const auto* ai_player = ai::EngineContractAccess::player(engine, 2);
    expect(engine.frame() == 45 && ai_player && ai_player->functions.size() == 1
        && ai_player->functions.front().goal_name == "GALACTIC", "world interface: host chooses frame and goal mode");
    const auto* targets = engine.target_list(2);
    expect(targets && targets->size() == 1 && engine.target(targets->front())->object == 700,
        "world interface: host supplies targets with engine-allocated IDs");
    expect(engine.target_matches(2, {"PLANET"}, engine.target_of_object(700)), "world interface: host supplies goal application");
    expect(engine.evaluate("Count", 2, engine.target_of_object(700)) == ai::real(5)
        && mode->contexts > 0 && mode->evaluations > 0, "world interface: host resolves context and perception through shared expressions");
    expect(host->world == nullptr && !host->snapshot, "world interface: no tactical state needed");
}

void game_random_forms() {
    const std::string source = R"(
function Run()
    Record(GameRandom())
    Record(GameRandom(9))
    Record(GameRandom(7, -3))
    Record(GameRandom(-3.9, 7.9))
    Record(GameRandom(6.9, 6.1))
    Record(GameRandom(0))
    Record(GameRandom.Get_Float())
    Record(GameRandom.Get_Float(4))
    Record(GameRandom.Get_Float(4.5, 2.5))
    Record(GameRandom.Get_Float(3.25, 3.25))
    -- The progressive evaluator needs its supplied range, above the unit interval.
    Record(GameRandom.Get_Float(2, 3))
    Record(GameRandom(32766))
    Record(GameRandom(32767))
    Record(GameRandom(-2147483648, 2147483647))
    Record(GameRandom("bad", 5))
    Record(GameRandom.Get_Float(false, 2))
    local cycle = {}; cycle.self = cycle
    Record(GameRandom(cycle, 2))
    Record(GameRandom(function() end, 2))
    Record(GameRandom(nil))
    Record(GameRandom(Find_Player("REBEL"), 2))
    local ok = pcall(function() GameRandom(1, 2, 3) end)
    assert(not ok)
    ok = pcall(function() GameRandom.Get_Float(1, 2, 3) end)
    assert(not ok)
    -- Inferred host behavior: numeric bounds outside int32 convert to INT32_MIN.
    Record(GameRandom(2147483648))
    Record(GameRandom(-2147483649))
    Record(GameRandom(-2147483649, -2147483648))
    Record(GameRandom(2147483648, -2147483649))
    Record(GameRandom(1, 10))
end
)";
    for (const auto seed : {23U, 2392U, 20833U}) {
        std::vector<ai::Real> baseline;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            World world;
            auth::ModuleManifest manifest;
            expect(manifest.add("RANDOM.LUA", source).has_value(), "GR-01: random fixture loads");
            auth::SessionConfig config;
            config.seed = seed;
            auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
            if (!scripts) { expect(false, "GR-01: scheduler starts"); return; }
            std::vector<ai::Real> results;
            std::uint64_t draws = 0;
            expect(scripts.value().register_binding("Record", [&](auth::BindingContext& context, const auth::ValueList& args) {
                results.push_back(std::get<ai::Real>(args.front().data));
                draws = context.random().draws();
                return eawr::core::Result<auth::ValueList>::success({});
            }).has_value(), "GR-01: recorder registers");
            std::vector<eawr::core::Diagnostic> errors;
            foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
            expect(errors.empty() && scripts.value().create_instance(1, "RANDOM.LUA").has_value(),
                "GR-01: production random bindings register");
            auth::ScriptEvent event;
            event.key = {1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Run";
            expect(scripts.value().submit_event(std::move(event)).has_value(), "GR-01: random calls queue");
            eawr::platform::ThreadWorkerAdapter executor(workers);
            const auto report = scripts.value().service(executor);
            expect(report && report.value().diagnostics.size() == 10,
                "L-54: capped ranges and non-numbers diagnose without suppressing the returned draw");
            auth::RandomStream reference(config.seed, 1, 1);
            std::vector<ai::Real> expected;
            const auto integer = [&](std::int64_t low, std::int64_t high) {
                expected.push_back(ai::real(low + static_cast<std::int64_t>(
                    reference.next_below(static_cast<std::uint64_t>(high - low) + 1))));
            };
            const auto floating = [&](ai::Real low, ai::Real high) {
                expected.push_back(low + (high - low) *
                    ai::Real(reference.next_below(32768)) / ai::real(32767));
            };
            integer(0, 65535); integer(0, 9); integer(-3, 7); integer(-3, 7);
            expected.push_back(ai::real(6));
            expected.push_back(ai::real(0));
            floating(ai::real(0), ai::real(1)); floating(ai::real(0), ai::real(4));
            floating(ai::real(5) / ai::real(2), ai::real(9) / ai::real(2));
            expected.push_back(ai::real(13) / ai::real(4));
            floating(ai::real(2), ai::real(3)); integer(0, 32766); integer(0, 32766);
            integer(-2147483648LL, -2147450882LL);
            integer(0, 5); floating(ai::real(0), ai::real(2));
            integer(0, 2); integer(0, 2); integer(0, 0); integer(0, 2);
            integer(-2147483648LL, -2147450882LL);
            integer(-2147483648LL, -2147450882LL);
            expected.push_back(ai::real(-2147483648LL));
            expected.push_back(ai::real(-2147483648LL));
            integer(1, 10);
            expect(results == expected && draws == reference.draws(),
                "GR-01: inclusive/truncated/swapped forms preserve the instance stream and equal bounds draw nothing");
            if (results.size() > 6 && seed != 23)
                expect(results[6] == ai::real(seed == 2392 ? 1 : 0), "L-54: both default float endpoints are attainable");
            if (workers == 1) baseline = results;
            else expect(results == baseline, "GR-01: forms agree across worker counts");
        }
    }
}

void capture_target_application() {
    World world;
    auto nonpad = world.host->setup.content.types.front();
    nonpad.capture_point = true;
    auto pad = nonpad;
    pad.type_id = 2;
    pad.locomotor = false;
    pad.build_pad = true;
    world.host->setup.content.types = {nonpad, pad};
    world.host->types.clear();
    for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
    world.host->setup.players.push_back(foc::AiPlayer{4, "NEUTRAL", true, false, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.view->players.push_back(foc::detail::ViewPlayer{4, 4, 0});
    world.view->units.clear();
    for (const auto& [id, type] : {std::pair{10U, 1U}, std::pair{11U, 2U}, std::pair{12U, 1U}}) {
        ai::ViewUnit unit;
        unit.id = id; unit.type = type; unit.owner = 4;
        unit.visible_to = 3;
        world.view->units.push_back(unit);
    }
    eawr::sim::tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
        {2, 2, 2, eawr::sim::tactical::player_flag_commandable}, {4, 4, 4, 0}};
    setup.units = {{10, 1, 4}, {11, 2, 4}, {12, 1, 4}};
    eawr::sim::tactical::EconomyRules rules;
    rules.pads.neutral = 4;
    const auto radius = eawr::sim::math::Fixed::from_integer(100).value();
    const auto seconds = eawr::sim::math::Fixed::from_integer(1).value();
    rules.pads.capture = {{1, radius, seconds, {1, 2}, false, true, false},
        {2, radius, seconds, {1, 2}, false, true, true}};
    auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(session), "GS-11: capturable non-pad and build-pad world starts");
    if (!session) return;
    world.host->world = &session.value();
    ai::Engine engine(world.host, {}, {});
    ai::Target ordinary; ordinary.object = 10;
    ai::Target build; build.object = 11;
    const std::set<std::string> enemy{"ENEMY_UNIT", "ENEMY_STRUCTURE"};
    const auto matches = [&](const ai::Target& target, const std::set<std::string>& flags) {
        return ai::EngineContractAccess::matches(engine, 2, flags, target);
    };
    expect(matches(ordinary, enemy) && !matches(ordinary, {"ENEMY_BUILD_PAD"}),
        "GS-11: a non-allied capturable non-pad participates in ordinary target evaluation");
    expect(matches(build, {"ENEMY_BUILD_PAD"}) && !matches(build, enemy),
        "GS-11: a real build pad takes the build-pad application flags");
    world.view->units[0].owner = world.view->units[1].owner = 2;
    expect(matches(ordinary, {"FRIENDLY_UNIT"}) && !matches(ordinary, enemy),
        "GS-11: allied capture ownership uses the friendly ordinary target kind");
    expect(matches(build, {"FRIENDLY_BUILD_PAD"}) && !matches(build, {"ENEMY_BUILD_PAD"}),
        "GS-11: allied build pads use friendly pad flags");
    world.view->units[0].owner = world.view->units[1].owner = 4;
    world.host->setup.content.types[0].locomotor = false;
    expect(matches(ordinary, {"ENEMY_STRUCTURE"}) && !matches(ordinary, {"ENEMY_UNIT"}),
        "GS-11: a stationary capturable non-pad is an ordinary structure, not a unit");
    world.view->units[0].owner = 2;
    expect(matches(ordinary, {"FRIENDLY_STRUCTURE"}) && !matches(ordinary, {"FRIENDLY_UNIT"}),
        "GS-11: allied stationary capturable non-pads keep their structure kind");
    world.view->units[0].owner = 4;
    ai::GoalType goal; goal.application = enemy;
    ai::PlayerAi player; player.player = 2; player.per_frame = 3; player.targets = {1, 2, 3};
    for (std::size_t index = 0; index < 7; ++index) {
        ai::GoalFunctionEntry function; function.goal = &goal;
        function.goal_name = "CAPTURE_TARGET_" + std::to_string(index);
        player.functions.push_back(std::move(function));
    }
    ai::EngineContractAccess::target_fixture(engine, 1, 10);
    ai::EngineContractAccess::target_fixture(engine, 2, 12);
    ai::EngineContractAccess::target_fixture(engine, 3, 11);
    std::size_t frames = 0;
    while (!player.maintenance_due && frames < 10) {
        ++frames; ai::EngineContractAccess::propose(engine, player);
    }
    expect(frames == 5 && player.maintenance_due && player.nontrivial == 14
        && ai::EngineContractAccess::evaluated(engine) == 14,
        "GS-04/11: two non-pad targets count against all seven functions even without a desire equation");
    // GS-11: application kind is object behavior, also in combat-only sessions.
    auto combat = eawr::sim::tactical::TacticalSession::create(setup);
    expect(static_cast<bool>(combat), "GS-11: combat-only world starts without pad economy profiles");
    if (!combat) return;
    world.host->world = &combat.value();
    expect(matches(build, {"ENEMY_BUILD_PAD"}) && !matches(build, enemy),
        "GS-11: a neutral real pad never falls through to enemy structure without economy");
    expect(matches(ordinary, {"ENEMY_STRUCTURE"}) && !matches(ordinary, {"ENEMY_BUILD_PAD"}),
        "GS-11: stationary capturable non-pads remain ordinary targets without economy");
    world.view->units[1].owner = 2;
    expect(matches(build, {"FRIENDLY_BUILD_PAD"}) && !matches(build, {"FRIENDLY_STRUCTURE"}),
        "GS-11: an allied real pad keeps its pad kind without economy");
}

void forced_object_fog() {
    const auto handle = [](std::uint32_t kind, std::uint64_t id) { return auth::Value{auth::Handle{kind, id}}; };
    for (const bool requester_ai : {false, true}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        world.host->setup.players[0].ai = requester_ai;
        world.host->setup.players.push_back(foc::AiPlayer{7, "HUTTS", false, false, ""});
        world.host->setup.players.push_back(foc::AiPlayer{9, "NEUTRAL", true, false, ""});
        world.host->players.clear();
        for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
        // Player IDs intentionally differ from the raw visibility bit indexes.
        world.view->players = {{1, 1, 0}, {2, 2, 1}, {7, 7, 2}, {9, 9, 3}};
        auto container = world.host->setup.content.types.front();
        container.name = "CONTAINER"; container.category_bits = 4; container.property_bits = 8;
        container.locomotor = false; container.space_evaluator = false;
        auto station = container;
        station.type_id = 2; station.name = "STATION"; station.property_bits = 0;
        station.star_base = true; station.space_evaluator = true;
        world.host->setup.content.types = {container, station};
        world.host->setup.content.categories.emplace("STRUCTURE", 4);
        world.host->setup.content.properties.emplace("NOTOPPORTUNITYTARGET", 8);
        world.host->types.clear(); world.host->types_by_name.clear();
        for (const auto& type : world.host->setup.content.types) {
            world.host->types.emplace(type.type_id, &type);
            world.host->types_by_name.emplace(type.name, &type);
        }
        world.view->units.clear();
        const auto add = [&](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, int x, int z) {
            ai::ViewUnit unit; unit.id = id; unit.type = type; unit.owner = owner; unit.visible_to = 1;
            unit.position.x = eawr::sim::math::Fixed::from_integer(x).value();
            unit.position.z = eawr::sim::math::Fixed::from_integer(z).value();
            world.view->units.push_back(unit);
        };
        add(10, 1, 1, 0, 0); add(11, 2, 2, 20, 0); add(12, 2, 2, 20, 0);
        add(13, 1, 7, 10, 0); add(14, 2, 9, 1, 0);
        add(15, 2, 2, 1, 0);
        world.view->units.back().planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        auth::ModuleManifest manifest;
        expect(manifest.add("FOG.LUA", "function Check(source, mask, player, ally)\n"
            "if player then Record(Find_Nearest(source, mask, player, ally))\n"
            "else Record(Find_Nearest(source, mask)) end end\n").has_value(), "WNO-11: nearest fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WNO-11: nearest scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), world.host, errors);
        expect(scripts.value().register_binding("Record", [](auth::BindingContext& context, const auth::ValueList& args) {
            context.issue_command("nearest", args);
            return eawr::core::Result<auth::ValueList>::success({});
        }).has_value(), "WNO-11: nearest result recorder registers");
        expect(errors.empty() && scripts.value().create_instance(1, "FOG.LUA").has_value(), "WNO-11: nearest instance starts");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto query = [&](const std::string& mask, std::optional<tactical::PlayerId> player, bool ally,
            sim::EntityId expected, const char* message) {
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {handle(foc::handle_game_object, 10), auth::Value{mask},
                player ? handle(foc::handle_player, *player) : auth::Value{}, auth::Value{ally}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "WNO-11: query submits");
            const auto report = scripts.value().service(executor);
            expect(report && report.value().diagnostics.empty() && report.value().commands.size() == 1,
                "WNO-11: production nearest query runs");
            if (!report || report.value().commands.size() != 1) return;
            const auto& args = report.value().commands.front().arguments;
            const auto* result = args.empty() ? nullptr : std::get_if<auth::Handle>(&args.front().data);
            expect(expected == 0 ? result == nullptr : result != nullptr && result->id == expected, message);
        };
        query("Structure", 1, false, 13, "WNO-11: visible Hutt container beats enemy structures despite property/evaluator");
        world.view->units[3].visible_to = 2;
        query("Structure", 1, false, 11, "WNO-11: Hutt fog uses requester bits and equal-distance retains first enemy");
        query("NotOpportunityTarget", 1, false, 0, "WNO-11: property filter respects raw fog");
        query("CONTAINER", 1, false, 0, "WNO-11: exact type filter respects raw fog");
        query("Structure", std::nullopt, false, 13, "WNO-11: no player filter ignores fog and still skips Neutral");
        query("NotOpportunityTarget", std::nullopt, false, 13, "WNO-11: property query excludes its source");
        world.view->units[1].visible_to = world.view->units[2].visible_to = 2;
        query("Structure", 1, false, 0, "WNO-11: hidden enemy structures excluded for human and AI");
        query("STATION", std::nullopt, false, 11, "WNO-11: no-filter exact type retains first tie");
        world.view->units[3].visible_to = 1;
        query("CONTAINER", 1, false, 13, "WNO-11: visible exact Hutt type admitted");
        query("NotOpportunityTarget", 1, false, 13, "WNO-11: visible Hutt property admitted");
        query("Corvette", 1, false, 0, "WNO-11: category masks stay intact");
        query("Structure", 1, true, 0, "WNO-11: allied mask and source exclusion stay intact");
        world.view->units[1].visible_to = world.view->units[2].visible_to = 1;
        world.view->units[3].position.z = eawr::sim::math::Fixed::from_integer(30).value();
        query("Structure", 1, false, 11, "WNO-11: nearest uses height as well as planar distance");
        world.view->units[3].position.z = {};
        ai::Engine engine(world.host, {}, {});
        ai::Target target; target.object = 11;
        for (unsigned flags = 0; flags != 4; ++flags) {
            auto& type = world.host->setup.content.types[1];
            type.initial_state_visible_under_fow = (flags & 1U) != 0;
            type.last_state_visible_under_fow = (flags & 2U) != 0;
            for (const std::uint64_t visibility : {0U, 1U, 2U}) {
                world.view->units[1].visible_to = visibility;
                expect(ai::EngineContractAccess::matches(engine, 1, {"ENEMY_STRUCTURE"}, target) ==
                    (visibility == 1 || flags != 0), "WNO-13: raw requester fog or either authored flag admits goal");
                expect(!ai::EngineContractAccess::matches(engine, 1, {"ENEMY_UNIT"}, target),
                    "WNO-13: fog exception preserves category classification");
            }
        }
        world.host->setup.content.types[1].initial_state_visible_under_fow = false;
        world.host->setup.content.types[1].last_state_visible_under_fow = false;
        world.host->setup.content.types[1].star_base = false;
        world.host->setup.content.types[1].capture_point = true;
        world.view->units[1].visible_to = 1; world.view->units[1].owner = 9;
        expect(ai::EngineContractAccess::matches(engine, 1, {"ENEMY_STRUCTURE"}, target),
            "WNO-14: visible Neutral merchant remains a nonallied structure goal");
        world.view->units[1].visible_to = 0;
        expect(!ai::EngineContractAccess::matches(engine, 1, {"ENEMY_STRUCTURE"}, target),
            "WNO-13: hidden Neutral merchant without authored exception is rejected");
        target.object = 0;
        expect(ai::EngineContractAccess::matches(engine, 1, {"TACTICAL_LOCATION"}, target),
            "WNO-13: region goals retain their separate admission");
        ai::ThreatGrid grid;
        grid.partition(foc::AiBounds{ai::real(-100), ai::real(100), ai::real(100), ai::real(-100)}, 5, 5, ai::Constants{});
        world.view->units[1].owner = 2;
        for (auto& unit : world.view->units) unit.visible_to = 0;
        grid.service(*world.view, *world.host, 0);
        const auto tracked = grid.objects();
        expect(std::find(tracked.begin(), tracked.end(), 15) == tracked.end(),
            "WSW-01: planetary source stays outside ordinary threat and distance queries");
        ai::PlayerAi player; player.player = 1;
        ai::EngineContractAccess::build_targets(engine, player);
        expect(engine.target_of_object(15) == nullptr,
            "WSW-01: even an evaluator-marked planetary source is not an ordinary goal target");
        auto source = world.view->units.back(); source.id = 16;
        world.view->units.push_back(source);
        ai::EngineContractAccess::update_targets(engine);
        expect(engine.target_of_object(16) == nullptr,
            "WSW-01: a newly observed planetary source stays outside ordinary goal targets");
        const ai::Rect area{ai::real(-100), ai::real(-100), ai::real(200), ai::real(200)};
        const auto force = grid.force(*world.host, *world.view, area, 4, 1, false, Real{}, 0);
        expect(requester_ai ? force > Real{} : force == Real{},
            "SK-45: unforced planning force keeps its independent AI fog preference");
    }
}

// FH-11, AI-08 (#782): the freestore's unit service skips the units a goal has reserved for its
// plan, as well as those a TaskForce holds, until the plan's Produce_Force takes them.
void freestore_unit_service() {
    World world;
    const auto initial = world.view->units.front();
    world.view->units.clear();
    for (eawr::sim::EntityId id = 10; id <= 16; ++id) {
        auto unit = initial;
        unit.id = id;
        world.view->units.push_back(unit);
    }
    ai::Engine engine(world.host, {}, {});
    world.host->engine = &engine;
    auto& player = ai::EngineContractAccess::collect_fixture(engine); // 14 held; 15 and 16 reserved
    const auto& units = world.view->units;
    expect(engine.unit_service(units[0], 2), "FH-11: a free unit gets the unit service");
    expect(engine.in_freestore(units[5], 2) && !engine.unit_service(units[5], 2),
        "FH-11/AI-08: a unit a goal has reserved stays in the freestore but gets no unit service");
    expect(!engine.unit_service(units[6], 2), "FH-11/AI-08: a unit the running plan's goal reserved gets none");
    expect(!engine.unit_service(units[4], 2), "FH-11: a unit a TaskForce holds gets none");
    expect(!engine.unit_service(units[0], 1), "FH-13: another player's unit gets none");
    // WTA-02 (#783): the service tests run with the player's execution service.
    player.next_execution = 33;
    expect(!engine.execution_due(2, 31) && !engine.execution_due(2, 32) && engine.execution_due(2, 33),
        "WTA-02: the freestore's service tests wait for the player's execution service");
    expect(engine.execution_due(1, 31), "WTA-02: a player the goal system does not run is tested every frame");
}

// WTA-30, WTA-32, DT-04 (#733): Get_Rate_Of_Damage_Taken and Get_Time_Till_Dead answer from the
// unit's damage tracking. An AI Acclamator taking 300 hull and shield a second with 5,000 left over
// its destroyable hardpoints plus its shield answers about 16.7 s, below Default_Unit_Damaged's 20.
void time_till_dead_bindings() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto units = [](std::int64_t n) { return math::Fixed::from_integer(n).value(); };
    tactical::DurabilityProfile profile;
    profile.max_hull = units(9000);
    profile.hardpoints.resize(4);
    profile.hardpoints[0].destroyable = profile.hardpoints[1].destroyable = profile.hardpoints[2].destroyable = true;
    tactical::DurabilityState state;
    state.hull = units(9000);
    state.hardpoints = {units(2000), units(1500), {}, {}}; // the third is destroyed; the fourth is not destroyable
    state.shields = units(1500);
    expect(!tactical::time_to_death(profile, state), "WCC-15: no time to death at a zero rate");
    state.damage_rate = units(300);
    const auto time = tactical::time_to_death(profile, state);
    expect(time && *time == math::divide(units(5000), units(300)).value(),
        "WCC-15: destroyable hardpoints plus shield, over the rate; the hull does not count");
    auto hull_only = profile;
    hull_only.hardpoints.clear();
    auto hull_state = state;
    hull_state.hardpoints.clear();
    hull_state.hull = units(600);
    hull_state.shields = {};
    const auto hull_time = tactical::time_to_death(hull_only, hull_state);
    expect(hull_time && *hull_time == units(2), "WCC-15: without destroyable hardpoints the hull counts");

    World world;
    auto& tracked = world.view->units.front();
    tracked.damage_rate = state.damage_rate;
    tracked.time_till_dead = time;
    auto untracked = tracked;
    untracked.id = 11;
    untracked.damage_rate = {};
    untracked.time_till_dead.reset();
    world.view->units.push_back(untracked);
    auth::ModuleManifest manifest;
    expect(manifest.add("DAMAGE.LUA", "function Check(a, b)\n"
        "  tracked_time = a.Get_Time_Till_Dead(); tracked_rate = a.Get_Rate_Of_Damage_Taken()\n"
        "  idle_time = b.Get_Time_Till_Dead(); idle_rate = b.Get_Rate_Of_Damage_Taken()\n"
        "  flees = tracked_time < 20\n"
        "end\n").has_value(), "DT-04: binding fixture loads");
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!scripts) { expect(false, "DT-04: scheduler starts"); return; }
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
    expect(errors.empty() && scripts.value().create_instance(1, "DAMAGE.LUA").has_value(), "DT-04: bindings register");
    auth::ScriptEvent event;
    event.key = {1, auth::first_simulation_producer, 1, 0};
    event.target = 1;
    event.kind = auth::ScriptEvent::Kind::call;
    event.name = "Check";
    event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 11}}};
    expect(scripts.value().submit_event(std::move(event)).has_value(), "DT-04: call queues");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    auto report = scripts.value().service(executor);
    expect(report && report.value().diagnostics.empty(), "DT-04: the queries run");
    const auto number = [&](std::string_view name) -> std::optional<double> {
        const auto value = scripts.value().read_global(1, name);
        if (!value || !value.value()) return std::nullopt;
        const auto* found = std::get_if<Real>(&value.value()->data);
        if (found == nullptr) return std::nullopt;
        return std::bit_cast<double>(found->repr);
    };
    const auto tracked_time = number("tracked_time");
    expect(tracked_time && *tracked_time > 16.6 && *tracked_time < 16.7, "WTA-32: Get_Time_Till_Dead answers about 16.7 s");
    expect(number("tracked_rate") == 300.0, "WTA-32: Get_Rate_Of_Damage_Taken answers the tracked rate");
    expect(number("idle_time") == 1e18 && number("idle_rate") == 0.0, "WTA-32: an untracked or undamaged unit answers 10^18 and 0");
    const auto flees = scripts.value().read_global(1, "flees");
    expect(flees && flees.value() && std::get_if<bool>(&flees.value()->data) != nullptr
        && std::get<bool>(flees.value()->data), "WTA-36: the time test of Default_Unit_Damaged fires");
}

// WTA-36: the damage response's type queries answer the type's authored minimum and maximum
// attack distances and whether it is a hero.
void type_range_and_hero_bindings() {
    for (const bool hero : {false, true}) {
        World world;
        auto& type = world.host->setup.content.types.front();
        type.hero = hero;
        type.min_attack_distance = ai::real(hero ? 150 : 0);
        auth::ModuleManifest manifest;
        expect(manifest.add("TYPE.LUA", "function Check(a)\n"
            "  local t = a.Get_Type()\n"
            "  hero = t.Is_Hero(); min_range = t.Get_Min_Range(); max_range = t.Get_Max_Range()\n"
            "end\n").has_value(), "WTA-36: type fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WTA-36: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "TYPE.LUA").has_value(), "WTA-36: bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WTA-36: call queues");
        const eawr::platform::ThreadWorkerAdapter executor(1);
        auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty(), "WTA-36: the type queries run");
        const auto number = [&](std::string_view name) -> std::optional<double> {
            const auto value = scripts.value().read_global(1, name);
            if (!value || !value.value()) return std::nullopt;
            const auto* found = std::get_if<Real>(&value.value()->data);
            if (found == nullptr) return std::nullopt;
            return std::bit_cast<double>(found->repr);
        };
        expect(number("min_range") == (hero ? 150.0 : 0.0), "WTA-36: Get_Min_Range answers Targeting_Min_Attack_Distance");
        expect(number("max_range") == 500.0, "WTA-36: Get_Max_Range answers Targeting_Max_Attack_Distance");
        const auto answer = scripts.value().read_global(1, "hero");
        expect(answer && answer.value() && std::get_if<bool>(&answer.value()->data) != nullptr
            && std::get<bool>(answer.value()->data) == hero, "WTA-36: Is_Hero answers the type's hero flag");
    }
}

void collect_free_categories() {
    struct Case { std::string arguments; std::vector<eawr::sim::EntityId> members; bool valid; };
    const std::array<Case, 6> cases{{
        {"'Fighter | Corvette'", {10, 12, 16, 19}, true},
        {"", {10, 11, 12, 16, 19}, true},
        {"'None'", {}, true},
        {"123", {}, false},
        {"'Unknown'", {}, false},
        {"'Fighter', 'Bomber'", {}, false},
    }};
    for (const auto& test : cases) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto base = world.host->setup.content.types.front();
        world.host->setup.content.types.clear();
        world.host->setup.content.categories = {{"FIGHTER", std::uint64_t{1} << 63}, {"BOMBER", 2},
            {"CORVETTE", 4}, {"NONE", 0}};
        for (std::uint32_t id = 1; id <= 5; ++id) {
            auto type = base;
            type.type_id = id;
            type.category_bits = id == 2 ? 2 : id == 3 ? 4 : id == 5 ? 0 : std::uint64_t{1} << 63;
            type.locomotor = id != 4;
            world.host->setup.content.types.push_back(type);
        }
        world.host->types.clear();
        for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
        base = world.host->setup.content.types.front();
        const auto initial = world.view->units.front();
        world.view->units.clear();
        for (eawr::sim::EntityId id = 10; id <= 18; ++id) {
            auto unit = initial;
            unit.id = id;
            unit.type = id == 11 ? 2 : id == 12 ? 3 : id == 17 ? 4 : id == 18 ? 5 : 1;
            unit.owner = id == 13 ? 1 : 2;
            world.view->units.push_back(unit);
        }
        auto source = initial; source.id = 19;
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        world.view->units.push_back(source);
        ai::Engine engine(world.host, {}, {});
        world.host->engine = &engine;
        auto& player = ai::EngineContractAccess::collect_fixture(engine);
        auth::ModuleManifest manifest;
        expect(manifest.add("COLLECT.LUA", "function Check(tf) tf.Collect_All_Free_Units(" + test.arguments + ") end\n")
            .has_value(), "EX-12: collect fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "EX-12: collect scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "COLLECT.LUA").has_value(),
            "EX-12: real TaskForce binding registers");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (std::uint64_t repeat = 0; repeat < (test.valid ? 2U : 1U); ++repeat) {
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, repeat};
            event.target = 1;
            event.kind = auth::ScriptEvent::Kind::call;
            event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "EX-12: collection call queues");
            auto report = scripts.value().service(executor);
            expect(report.has_value(), "EX-12: collection binding runs");
            if (!report) return;
            expect(report.value().diagnostics.empty() == test.valid, "EX-12: invalid category arguments fail");
            if (!test.valid) expect(report.value().commands.empty(), "EX-12: a rejected call stages no collection");
            expect(engine.after_service(report.value()).has_value(), "EX-12: staged collection applies");
            expect(engine.taskforce(1)->units == test.members,
                "EX-12/WSW-47: collection matches free objects independently of firing enablement and never duplicates members");
            expect(player.assigned.at(14) == 99 && !player.assigned.contains(13) && !player.assigned.contains(15)
                && !player.assigned.contains(17) && !player.assigned.contains(18),
                "EX-12: ownership, other assignments/reservations, non-movers and category-none stay excluded");
        }
    }
}

void order_destinations() {
    struct Case {
        std::uint32_t receiver;
        std::string method;
        auth::Value target;
        std::vector<eawr::sim::EntityId> members;
        bool valid;
        std::uint64_t object;
        int x, y, z;
        bool command{true};
        bool parent{};
    };
    const auto handle = [](std::uint32_t kind, std::uint64_t id) { return auth::Value{auth::Handle{kind, id}}; };
    const auto point = foc::detail::position_value({eawr::sim::math::Fixed::from_integer(7).value(),
        eawr::sim::math::Fixed::from_integer(8).value(), eawr::sim::math::Fixed::from_integer(9).value()});
    const auto force = handle(ai::handle_taskforce, 1);
    const auto object = handle(foc::handle_game_object, 11);
    const auto target = handle(ai::handle_ai_target, 1);
    const auto region = handle(ai::handle_ai_target, 2);
    const std::vector<Case> cases{
        {foc::handle_game_object, "Attack_Move", force, {10, 11}, true, 0, 20, 30, 40},
        {foc::handle_game_object, "Move_To", force, {10, 11}, true, 0, 20, 30, 40},
        {foc::handle_game_object, "Guard_Target", force, {10, 11}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", force, {10, 11, 12}, true, 12, 0, 0, 0, true, true},
        {foc::handle_game_object, "Guard_Target", object, {10, 11}, true, 0, 30, 40, 50, true, true},
        {foc::handle_game_object, "Guard_Target", force, {10}, true, 0, 10, 20, 30},
        {foc::handle_game_object, "Attack_Move", force, {}, true, 0, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", force, {}, true, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Move", target, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", target, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Move_To", target, {10}, true, 0, 30, 40, 50},
        {foc::handle_game_object, "Attack_Move", region, {10}, true, 0, 0, 0, 0},
        {foc::handle_game_object, "Guard_Target", point, {10}, true, 0, 7, 8, 9},
        {foc::handle_game_object, "Attack_Move", object, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", object, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", target, {10}, true, 11, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", region, {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", force, {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Target", point, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", object, {10}, true, 11, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", target, {10}, true, 11, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", region, {10}, true, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", force, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Target", point, {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Move", handle(foc::handle_game_object, 10), {10}, true, 0, 10, 20, 30},
        {foc::handle_game_object, "Attack_Move", handle(ai::handle_ai_target, 3), {10}, false, 0, 0, 0, 0},
        {foc::handle_game_object, "Attack_Move", auth::Value{}, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Attack_Move", force, {10, 11}, true, 0, 20, 30, 40},
        {ai::handle_taskforce, "Guard_Target", force, {11}, true, 11, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", force, {10, 11}, true, 999, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", point, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", auth::Value{}, {10}, false, 0, 0, 0, 0},
        {ai::handle_taskforce, "Guard_Target", handle(ai::handle_ai_target, 99), {10}, true, 0, 0, 0, 0, false},
        {ai::handle_taskforce, "Guard_Target", handle(ai::handle_ai_target, 3), {10}, true, 0, 0, 0, 0, false},
        {ai::handle_taskforce, "Guard_Target", force, {}, true, 0, 0, 0, 0, false},
    };
    for (const auto& test : cases) {
      std::optional<std::uint64_t> random_member;
      for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto& first = world.view->units.front();
        first.position = {eawr::sim::math::Fixed::from_integer(10).value(), eawr::sim::math::Fixed::from_integer(20).value(),
            eawr::sim::math::Fixed::from_integer(30).value()};
        auto second = first;
        if (test.parent) first.squadron = 11;
        second.id = 11;
        second.position = {eawr::sim::math::Fixed::from_integer(30).value(), eawr::sim::math::Fixed::from_integer(40).value(),
            eawr::sim::math::Fixed::from_integer(50).value()};
        world.view->units.push_back(second);
        second.id = 12;
        world.view->units.push_back(second);
        ai::Engine engine(world.host, {}, {});
        world.host->engine = &engine;
        ai::EngineContractAccess::order_fixture(engine, test.members);
        auth::ModuleManifest manifest;
        expect(manifest.add("ORDER.LUA", "function Check(receiver, target) receiver." + test.method + "(target) end\n")
            .has_value(), "FH-41/EX-32: order module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "FH-41/EX-32: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "ORDER.LUA").has_value(),
            "FH-41/EX-32: production bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call;
        event.name = "Check";
        event.arguments = {handle(test.receiver, test.receiver == ai::handle_taskforce ? 1 : 10), test.target};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "FH-41/EX-32: order queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto report = scripts.value().service(executor);
        if (!report) { expect(false, "FH-41/EX-32: service succeeds"); return; }
        const auto label = test.method + " receiver=" + std::to_string(test.receiver);
        expect(report.value().diagnostics.empty() == test.valid, "FH-41/EX-32: permitted/rejected conversions: " + label);
        expect(report.value().commands.size() == (test.valid && test.command ? 1U : 0U),
            "FH-41/EX-32: rejected/dead destinations stage no orders: " + label);
        if (report.value().commands.empty()) continue;
        const auto& args = report.value().commands.front().arguments;
        if (test.receiver == ai::handle_taskforce) {
            expect(args.size() == 4, "EX-32: TaskForce stages one movement block");
            if (args.size() != 4) continue;
            if (test.object != 0) {
                const auto member = std::get<auth::Handle>(args[3].data).id;
                if (test.object == 999) {
                    if (!random_member) random_member = member;
                    expect((member == 10 || member == 11) && member == *random_member,
                        "EX-32: random member selection is equal on 1/2/4/8 workers");
                } else expect(member == test.object, "EX-32: guard retains member");
            }
            else {
                const auto& coordinates = std::get<std::vector<auth::Value>>(args[3].data);
                expect(coordinates.size() == 3 && std::get<Real>(coordinates[0].data) == ai::real(test.x)
                    && std::get<Real>(coordinates[1].data) == ai::real(test.y)
                    && std::get<Real>(coordinates[2].data) == ai::real(test.z), "EX-32: attack-move takes centroid");
            }
        } else if (test.object != 0) {
            expect(args.size() == 3 && std::get<auth::Handle>(args[2].data).id == test.object, "FH-41: follow object");
        } else {
            expect(args.size() == 5 && std::get<Real>(args[2].data) == ai::real(test.x)
                && std::get<Real>(args[3].data) == ai::real(test.y) && std::get<Real>(args[4].data) == ai::real(test.z),
                "FH-41: movement/attack-move/guard takes point: " + label);
        }
    }
    }
}

void special_weapon_bindings() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
      for (const int state : {0, 1, 2, 3, 4, 5}) {
        World world;
        auto& type = world.host->setup.content.types.front();
        type.space_special_weapon = true;
        type.locomotor = false;
        auto& source = world.view->units.front();
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        source.planetary_weapon->valid_targets = state == 3 ? 1 : 2;
        source.planetary_weapon->readiness = math::Fixed::from_raw(state == 2 ? math::Fixed::scale - 1 : math::Fixed::scale);
        auto enemy = source;
        enemy.id = 11; enemy.owner = state == 4 ? 2 : 1;
        enemy.planetary_weapon.reset(); enemy.visible_to = 0;
        enemy.position.x = math::Fixed::from_integer(100000).value();
        world.view->units.push_back(enemy);
        tactical::EconomyView account; account.player = 2; account.special_weapons_enabled = state != 1;
        world.host->snapshot = std::make_shared<tactical::TacticalSnapshot>(0,
            std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{},
            std::vector<tactical::Event>{}, std::vector<tactical::CombatEvent>{}, std::vector<tactical::Projectile>{},
            std::nullopt, std::vector<tactical::SpinningCraft>{}, std::vector<tactical::SquadronTarget>{},
            std::vector<tactical::Squadron>{}, std::vector<tactical::EconomyView>{account});
        ai::Engine engine(world.host, {}, {});
        world.host->engine = &engine;
        ai::EngineContractAccess::order_fixture(engine, {10});
        auth::ModuleManifest manifest;
        expect(manifest.add("SPECIAL.LUA", "function Check(tf, object, target, player)\n"
            "tf_result = tf.Fire_Special_Weapon('SHIP', target)\n"
            "object_result = object.Fire_Special_Weapon(target, player)\nend\n").has_value(), "WSW: binding fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WSW: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "SPECIAL.LUA").has_value(), "WSW: bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}},
            auth::Value{auth::Handle{foc::handle_game_object, 10}},
            auth::Value{auth::Handle{state == 5 ? foc::handle_game_object : ai::handle_ai_target, state == 5 ? 11U : 1U}},
            auth::Value{auth::Handle{foc::handle_player, 2}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WSW: calls queue");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty(), "WSW: both routes service cleanly");
        if (!report) return;
        const bool accepted = state == 0 || state == 5;
        expect(report.value().commands.size() == (accepted ? 2U : 0U), "WSW-14/16: full readiness, permission, category and relationship gates");
        for (const auto& command : report.value().commands) {
            auto order = foc::tactical_ai_detail::translate_order(command);
            expect(order && std::holds_alternative<tactical::SpecialFirePayload>(order.value().payload), "WSW: staged request translates");
            if (!order) continue;
            const auto& payload = std::get<tactical::SpecialFirePayload>(order.value().payload);
            expect(payload.target == 11 && order.value().units == std::vector<eawr::sim::EntityId>{10}, "WSW: source and resolved target retained");
            expect(payload.admission == (command.sequence == 0 ? tactical::SpecialFireAdmission::ai_taskforce : tactical::SpecialFireAdmission::ai_object),
                "WSW: distinct replay admission retained");
        }
        const auto tf_result = scripts.value().read_global(1, "tf_result");
        const auto object_result = scripts.value().read_global(1, "object_result");
        expect(tf_result && tf_result.value() && (accepted ? std::holds_alternative<auth::Handle>(tf_result.value()->data)
            : std::holds_alternative<bool>(tf_result.value()->data) && !std::get<bool>(tf_result.value()->data)), "WSW-41: handle/false contract");
        expect(object_result && (accepted ? object_result.value().has_value() && std::holds_alternative<auth::Handle>(object_result.value()->data)
            : object_result.value().has_value() && std::holds_alternative<std::monostate>(object_result.value()->data)), "WSW-40: handle/nil contract");
        expect(world.view->units.front().planetary_weapon->state == tactical::SpecialWeaponState{}, "WSW: bindings leave frozen state unchanged");
      }
    }
}

void manual_special_weapon_binding() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    for (const int state : {0, 1, 2, 3, 4, 5, 6, 7, 8}) {
        World world;
        world.host->setup.content.types.front().star_base = true;
        auto enemy = world.view->units.front(); enemy.id = 11; enemy.owner = 1;
        world.view->units.push_back(enemy);
        tactical::TacticalInstance source, target;
        source.entity_id = 10; source.type_id = 1; source.owner = 2; source.team = 2;
        target.entity_id = 11; target.type_id = 1; target.owner = state == 4 ? 2 : 1; target.team = target.owner;
        source.fixed_transform = math::to_matrix(math::identity_quat(), {}).value();
        target.fixed_transform = math::to_matrix(math::identity_quat(), {math::Fixed::from_integer(state == 6 ? 2000 : 500).value(), {}, {}}).value();
        target.category_bits = 2; target.visible_to = state == 5 ? 0 : 3;
        auto profile = std::make_shared<tactical::WeaponProfile>();
        profile->hardpoint = 0; profile->requires_manual_target = true; profile->range = math::Fixed::from_integer(1000).value();
        profile->category_restrictions = state == 7 ? 2 : 0; profile->shot.emplace();
        tactical::ManualWeaponView gun;
        gun.available = state != 3; gun.countdown = state == 1 ? 1U : 0U; gun.target = state == 2 ? 11U : 0U;
        gun.admission_profile = profile;
        if (state != 8) source.manual_weapons.push_back(gun);
        tactical::EconomyView account; account.player = 2; account.special_weapons_enabled = true;
        tactical::ManualPlayerClock clock; clock.last_fired_frame = 599; clock.cooldown_frames = 3600;
        world.host->snapshot = std::make_shared<tactical::TacticalSnapshot>(600,
            std::vector<tactical::SnapshotPlayer>{{1, 1, false}, {2, 2, false}}, std::vector<tactical::TacticalInstance>{source, target},
            std::vector<tactical::Event>{}, std::vector<tactical::CombatEvent>{}, std::vector<tactical::Projectile>{},
            std::nullopt, std::vector<tactical::SpinningCraft>{}, std::vector<tactical::SquadronTarget>{},
            std::vector<tactical::Squadron>{}, std::vector<tactical::EconomyView>{account}, std::vector<tactical::PadView>{},
            std::vector<tactical::PlayerQuit>{}, nullptr, nullptr, std::vector<std::pair<tactical::PlayerId, tactical::ManualPlayerClock>>{{2, clock}});
        auth::ModuleManifest manifest;
        expect(manifest.add("MANUAL.LUA", "function Check(object, target, player) result = object.Fire_Special_Weapon(target, player) end\n").has_value(), "WSW-40: station fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "MANUAL.LUA").has_value(), "WSW-40: object binding registers");
        auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 11}},
            auth::Value{auth::Handle{foc::handle_player, 2}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WSW-40: station call queues");
        const eawr::sim::InlineExecutor executor;
        const auto report = scripts.value().service(executor);
        expect(report && (state == 8 ? report.value().diagnostics.size() == 1 : report.value().diagnostics.empty())
            && report.value().commands.size() == (state == 0 ? 1U : 0U),
            "WSW-17/18/40: station uses frozen gun/target gates while human clock recharges");
        if (state == 8 && report) expect(report.value().diagnostics.front().message == "Station has no manual special gun",
            "WSW-40: missing gun warns and returns nil");
        const auto result = scripts.value().read_global(1, "result");
        expect(result && (state == 0 ? result.value().has_value() && std::holds_alternative<auth::Handle>(result.value()->data)
            : result.value().has_value() && std::holds_alternative<std::monostate>(result.value()->data)), "WSW-40: station returns its handle only for an admitted assignment");
    }
}

void special_weapon_attachment() {
    namespace tactical = eawr::sim::tactical;
    World world;
    auto& type = world.host->setup.content.types.front();
    type.locomotor = false; type.space_special_weapon = true;
    world.view->units.front().planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
    world.view->units.front().planetary_weapon->readiness = eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale);
    const auto plan_globals = [](bool deny) {
        std::vector<auth::Value> taskforce{auth::Value::text("MainForce")};
        if (deny) taskforce.push_back(auth::Value::text("DenySpecialWeaponAttach"));
        taskforce.push_back(auth::Value::text("SHIP = 1"));
        auth::ValueList globals{auth::Value::text("GROUND_TO_SPACE_DAMAGE"),
            auth::Value{std::vector<auth::Value>{auth::Value{std::move(taskforce)}}}};
        globals.resize(10);
        globals[2] = auth::Value{true};
        return globals;
    };
    const auto publish = [&](bool enabled, std::vector<tactical::Event> events = {}) {
        tactical::EconomyView account; account.player = 2; account.special_weapons_enabled = enabled;
        world.host->snapshot = std::make_shared<tactical::TacticalSnapshot>(1,
            std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{}, std::move(events),
            std::vector<tactical::CombatEvent>{}, std::vector<tactical::Projectile>{}, std::nullopt,
            std::vector<tactical::SpinningCraft>{}, std::vector<tactical::SquadronTarget>{},
            std::vector<tactical::Squadron>{}, std::vector<tactical::EconomyView>{account});
    };
    // HA-01/WSW-47: DenySpecialWeaponAttach clears the plan's extra-attach permission, but a source
    // its team lists stays admitted (the stock planetary plans deny and list their source); the
    // match's superweapon admission is the gate.
    for (const auto& [deny, admitted] : {std::pair{true, true}, std::pair{false, true}, std::pair{false, false}}) {
        world.host->setup.allow_special_weapons = admitted;
        std::vector<std::string> notes;
        const auto definition = ai::build_plan(*world.host, "ground", "ground.lua", plan_globals(deny), notes);
        expect(definition && definition.value().allow_special_weapons == !deny, "HA-01: DenySpecialWeaponAttach clears the extra attach");
        const bool allow = admitted;
        if (!definition) return;
        ai::Engine engine(world.host, {}, {definition.value()});
        ai::PlayerAi player; player.player = 2;
        ai::PotentialPlan potential;
        publish(false);
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 7),
            "WSW-47: disabled source stays unavailable to proposal selection");
        publish(true);
        potential = {};
        expect(ai::EngineContractAccess::selection(engine, player, potential, 7) == allow,
            "WSW-34/47: unlock makes the nonmoving source eligible for the first shot in a permitting plan");
        if (allow) expect(potential.freestore == std::vector<eawr::sim::EntityId>{10} && potential.sources == std::vector<std::uint8_t>{0},
            "WSW-47: attachment keeps the live source identity without purchase");
        world.view->units.front().planetary_weapon->readiness = eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale - 1);
        potential = {};
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 7), "WSW-47: charging source cannot enter a new proposal");
        world.view->units.clear();
        potential = {};
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 7), "WSW-47: absent source is never manufactured");
        ai::ViewUnit source; source.id = 10; source.type = 1; source.owner = 2;
        source.planetary_weapon = tactical::TacticalInstance::PlanetaryWeaponView{};
        source.planetary_weapon->readiness = eawr::sim::math::Fixed::from_raw(eawr::sim::math::Fixed::scale);
        world.view->units.push_back(source);
    }
    world.host->setup.allow_special_weapons = true;
    auth::ModuleManifest manifest;
    expect(manifest.add("ONLINE.LUA", "function Default_Special_Weapon_Online(tf, object) online = object end\n").has_value(), "WSW-37: online handler loads");
    auth::SessionConfig config; config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!scripts) return;
    expect(scripts.value().create_instance(1, "ONLINE.LUA").has_value(), "WSW-37: online instance starts");
    const eawr::sim::InlineExecutor executor;
    expect(scripts.value().service(executor).has_value(), "WSW-37: online handler definition executes");
    ai::Engine engine(world.host, {}, {ai::PlanDef{}});
    ai::EngineContractAccess::event_force(engine, scripts.value(), {10});
    publish(false);
    expect(ai::EngineContractAccess::run_events(engine).empty() && engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{10},
        "WSW-34/37: disabled recharge remains attached without a fabricated online signal");
    tactical::Event event;
    event.kind = tactical::EventKind::special_weapon_ready; event.unit = 10;
    publish(true, {event});
    const auto events = ai::EngineContractAccess::run_events(engine);
    expect(events.size() == 1 && events.front().name == "Default_Special_Weapon_Online"
        && engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{10},
        "WSW-34/37: re-enable online event reaches the retained source's plan");
}

// GS-22/31: running allocations are revalidated at the existing maintenance branch.
void running_target_contrast() {
    for (const bool removable : {false, true}) for (const bool proposals : {false, true})
        for (const bool competing : {false, true}) for (int change = 0; change != 13; ++change) {
            World world;
            world.view->units.front().health = ai::real(1);
            auto enemy = world.view->units.front(); enemy.id = 11; enemy.owner = 1;
            world.view->units.push_back(enemy);
            if (change == 12) {
                auto stronger = world.host->setup.content.types.front(); stronger.type_id = 2;
                stronger.name = "STRONGER"; stronger.combat_power = ai::real(2000);
                world.host->setup.content.types.push_back(stronger);
                world.host->types.clear(); world.host->types_by_name.clear();
                for (const auto& type : world.host->setup.content.types) {
                    world.host->types.emplace(type.type_id, &type);
                    world.host->types_by_name.emplace(type.name, &type);
                }
                world.view->units.back().type = 2;
            }
            const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
            auto equations = ai::EquationSet::parse({{"contrast.xml", "<Root><Desire>50</Desire><Positive>1</Positive></Root>"}}, converters);
            expect(static_cast<bool>(equations), "GS-22: maintenance equations load");
            if (!equations) return;
            ai::AiData data; data.equations = std::move(equations).value();
            ai::PlanDef definition; definition.name = "HELD";
            definition.min_contrast = definition.max_contrast = ai::real(1);
            definition.ignore_target = change == 4;
            definition.magic = change == 8;
            ai::Engine engine(world.host, std::move(data), {definition});
            ai::EngineContractAccess::target_fixture(engine, 1, 11);
            ai::GoalType goal; goal.application = {"ENEMY_UNIT"}; goal.category = "HIGH";
            ai::GoalType competitor; competitor.application = {"GLOBAL"}; competitor.category = "HIGH";
            auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, competitor, removable, proposals);
            player.difficulty.space_contrast_multiplier = ai::real(1);
            ai::EngineContractAccess::contrast_fixture(engine, player);
            if (proposals && competing) {
                player.proposed.front().desire = ai::real(200);
                for (std::size_t index = 2; index <= 3; ++index) {
                    auto function = player.functions[1]; function.goal_name += std::to_string(index);
                    player.functions.push_back(std::move(function));
                    ai::Goal candidate; candidate.id = index + 1; candidate.function = index;
                    candidate.desire = ai::real(202 - static_cast<std::int64_t>(index));
                    player.proposed.push_back(candidate);
                }
            }
            auto& selected = *player.active.front().potential;
            selected.target_contrast = {{0, ai::real(1000)}};
            selected.current_contrast = {{0, ai::real(0)}};
            if (change == 1 || change == 4 || change == 5 || change == 6 || change == 8)
                world.view->units.front().health = (ai::real(1) / ai::real(4));
            if (change == 2) player.difficulty.space_contrast_multiplier = ai::real(2);
            if (change == 3) player.reserved[10] = 99;
            if (change == 5) selected.target_contrast_disabled = true;
            if (change == 6) {
                auto spare = world.view->units.front(); spare.id = 12; spare.health = ai::real(1);
                world.view->units.push_back(spare);
            }
            if (change == 7) world.view->units.erase(world.view->units.begin());
            if (change == 9 || change == 10) {
                world.host->contrast_order = {2};
                world.host->contrast[2] = {{2, {}, (ai::real(1) / ai::real(change == 9 ? 1 : 2))}};
                selected.contrast_indices = {1};
            }
            if (change == 11) world.view->units.pop_back();
            auth::ModuleManifest modules;
            expect(modules.add("HELD.LUA", "function Hold() Sleep(120) end\n").has_value(), "GS-22: held module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            if (!scripts) { expect(false, "GS-22: scheduler starts"); return; }
            expect(scripts.value().create_instance(1, "HELD.LUA").has_value(), "GS-22: held instance starts");
            std::uint64_t sequence = 0;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            ai::EngineContractAccess::set_selection_seed(engine, 2, 12345);
            const bool valid = change == 0 || change == 4 || change == 5 || change == 7 || change == 8 || change == 9;
            if (change != 8) {
                expect(ai::EngineContractAccess::contrast(engine, player) == valid,
                    "GS-22: live health, difficulty and reservations revalidate the retained allocation");
                expect(selected.units == std::vector<tactical::TypeId>{1}
                    && selected.freestore == std::vector<eawr::sim::EntityId>{10}
                    && ai::EngineContractAccess::selection_seed(engine, 2) == 12345,
                    "GS-22: recheck neither borrows a spare unit nor draws random numbers");
                // A direct failed test is sticky; reset only to exercise the pass's call.
                selected.valid = true;
            }
            ai::EngineContractAccess::maintain(engine, player);
            const bool replaced = proposals && competing && removable && valid && change != 8;
            const bool tested_magic = proposals && competing && removable && change == 8;
            const bool retained = valid && !replaced && !tested_magic;
            expect((engine.plan(1) != nullptr) == retained && (engine.taskforce(1) != nullptr) == retained,
                "GS-22/31: failed contrast removes a running force even when protected or without proposals");
            expect(static_cast<bool>(scripts.value().thread_slots(1)) == retained,
                "GS-22: abandonment removes the associated script");
            if (!retained) {
                const auto& records = engine.records();
                expect(std::any_of(records.begin(), records.end(), [&](const auto& row) {
                    return row.plan == "HELD" && row.event == "maintenance_end"
                        && row.detail == (replaced ? "replacement" : "target_contrast");
                }), "GS-22/31: cullable failure and replacement have distinct end reasons");
                expect(std::any_of(records.begin(), records.end(), [](const auto& row) {
                    return row.plan == "HELD" && row.event == "abandoned";
                }), "GS-22: maintenance drops report abandonment");
            }
            if (!retained) expect(!player.assigned.contains(10) && (!player.reserved.contains(10) || player.reserved.at(10) == 99),
                "GS-22: abandoned goal releases its units and reservations");
        }
}

void running_reservation_contrast() {
    for (const bool pooled : {false, true}) for (int ownership = 0; ownership != 3; ++ownership) {
        World world; world.view->units.front().health = ai::real(1);
        auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE";
        mode->provide_account = true; mode->reservation_account.pool_tokens = {77};
        mode->reservation_pads.emplace(88, tactical::PadState{});
        world.host->ai_world = mode;
        ai::PlanDef definition; definition.name = "HELD"; definition.ignore_target = true;
        ai::Engine engine(world.host, {}, {definition});
        ai::GoalType goal; goal.category = "HIGH";
        auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, goal, true, false);
        ai::EngineContractAccess::contrast_fixture(engine, player);
        auto& allocation = *player.active.front().potential;
        allocation.freestore = {0}; allocation.sources = {static_cast<std::uint8_t>(pooled ? 1 : 2)};
        allocation.pool_tokens = {77}; allocation.producers = {88};
        if (pooled) {
            if (ownership != 0) player.reserved_pool_tokens[77] = ownership == 1 ? 1 : 99;
        } else {
            ai::EngineContractAccess::retain_pad(engine, 88);
            if (ownership != 0) ai::EngineContractAccess::reserve_pad(engine, 88, ownership == 1 ? 2 : 1, 1);
        }
        expect(ai::EngineContractAccess::contrast(engine, player) == (ownership == 1),
            "GS-22: live pool and pad allocations require their own reservation even with ignored contrast");
        allocation.valid = true;
        if (pooled) mode->reservation_account.pool_tokens.clear();
        else ai::EngineContractAccess::release_retained_pad(engine);
        expect(ai::EngineContractAccess::contrast(engine, player),
            "GS-22: consumed pool tokens and released pad allocations are no longer tested");
    }
}

// Selection reserves reinforcements before Lua necessarily calls Produce_Force.
// Every retirement stage must leave the pool usable and native state restorable.
void retiring_pool_reservations() {
    std::map<std::pair<unsigned, bool>, std::vector<std::uint8_t>> reference;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) for (unsigned stage = 0; stage < 6; ++stage)
        for (const bool abandoned : {false, true}) {
            std::cout << "POOL-RETIREMENT workers=" << workers << " stage=" << stage << " abandoned=" << abandoned << '\n';
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto host = std::make_shared<ai::Host>();
            auto mode = std::make_shared<ModeWorld>(); mode->mode = "SPACE"; mode->frame = 1;
            mode->provide_account = true;
            mode->reservation_account.pool = {1, 1, 1}; mode->reservation_account.pool_tokens = {77, 78, 88};
            tactical::Player account_owner; account_owner.player_id = 2;
            mode->reservation_players.push_back(account_owner);
            host->ai_world = mode; host->view = mode->view();
            host->setup.players = {{2, "SYNTHETIC", false, true, "POOL"}};
            host->players.emplace(2, &host->setup.players.front());
            foc::AiType unit; unit.type_id = 1; unit.name = "POOL_UNIT";
            auto production = unit; production.type_id = 2; production.name = "PRODUCTION_UNIT";
            host->setup.content.types = {unit, production};
            for (const auto& type : host->setup.content.types) host->types.emplace(type.type_id, &type);
            const auto make_engine = [&]() {
                ai::AiData data;
                const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
                auto equations = ai::EquationSet::parse({{"pool.xml", "<Root><Zero>0</Zero></Root>"}}, converters);
                if (!equations) throw std::runtime_error("pool retirement fixture equations failed");
                data.equations = std::move(equations).value();
                ai::GoalType goal; goal.name = "POOL"; goal.mode = "SPACE"; goal.application = {"GLOBAL"};
                data.goals.emplace(goal.name, goal); data.function_sets["POOL"] = {{"POOL", "Zero"}};
                ai::PlayerType type; type.name = "POOL"; type.function_sets = {"POOL"}; data.players.emplace(type.name, type);
                ai::PlanDef plan; plan.name = "POOL"; plan.module = "POOL.LUA"; plan.goals = {"POOL"};
                plan.taskforces.push_back({"Main", {{1, 3, 0, false, {1, 2}}}, false, true, {}, 1});
                return std::make_unique<ai::Engine>(host, std::move(data), std::vector<ai::PlanDef>{plan});
            };
            auth::ModuleManifest modules;
            expect(modules.add("POOL.LUA", stage == 3 ? "function Base_Definitions() end"
                : "function Base_Definitions() end function Main_Thread() coroutine.yield(true) end").has_value(),
                "pool retirement fixture module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            auto engine = make_engine(); std::uint64_t sequence{};
            expect(scripts && engine->before_service(*mode, scripts.value(), sequence, &executor), "pool retirement owner initializes");
            if (!scripts) return;
            auto& player = ai::EngineContractAccess::checked_player(*engine, 2);
            const auto tokens = stage == 5 ? std::vector<std::uint64_t>{77} : std::vector<std::uint64_t>{77, 78};
            auto& goal = ai::EngineContractAccess::pool_goal(*engine, player, tokens, true, stage == 4);
            const auto attached = ai::EngineContractAccess::attach(*engine, player, goal, scripts.value(), sequence);
            expect(attached.has_value(), "selected pool goal attaches through production");
            if (!attached) return;
            const auto* plan = engine->plan(goal.plan);
            expect(plan && plan->taskforces.size() == (stage == 3 ? 0U : 1U), "missing thread drops its selected TaskForce");
            const auto other_goal = ai::EngineContractAccess::pool_goal(*engine, player, {88}, false).id;
            player.reserved_pool[2] = 9;
            if (stage == 4) player.reserved_pool_tokens.at(77) = other_goal;
            if (stage == 1 || stage == 2 || stage == 5) {
                auth::ServiceReport report; report.tick = 1;
                auth::ScriptCommand command; command.issuer = plan->instance; command.sequence = 7;
                command.verb = std::string(ai::verb_ai);
                command.arguments = {auth::Value::text("produce"), auth::Value{auth::Handle{ai::handle_taskforce, plan->taskforces.front()}}};
                report.commands.push_back(std::move(command));
                expect(engine->after_service(report).has_value() && player.tasks.size() == tokens.size(),
                    "Produce_Force transfers selected tokens to pending build tasks");
                if (stage != 1) {
                    ai::EngineContractAccess::service(*engine, player, 1);
                    expect(engine->taskforce(plan->taskforces.front())->pooled.size() == tokens.size(),
                        "execution transfers pending tokens to the TaskForce pool");
                }
            }
            if (stage == 5) {
                auto view = std::make_shared<ai::WorldView>();
                ai::ViewUnit arrived; arrived.id = 10; arrived.type = 1; arrived.owner = 2;
                arrived.purchase_token = 77; arrived.purchase_type = 1; view->units.push_back(arrived);
                mode->world_view = view; host->view = view;
                mode->reservation_account.pool = {1}; mode->reservation_account.pool_tokens = {88};
                ai::EngineContractAccess::await_pool_arrival(*engine, player, goal.plan);
                ai::EngineContractAccess::reinforce_service(*engine, player, 1);
                expect(!player.reserved_pool_tokens.contains(77) && player.reserved_pool.at(1) == 1
                    && engine->taskforce(plan->taskforces.front())->units == std::vector<eawr::sim::EntityId>{10},
                    "reinforcement arrival consumes and releases its token before retirement");
            }
            for (auto event : ai::EngineContractAccess::take_attachment_events(*engine))
                expect(scripts.value().submit_event(std::move(event)).has_value(), "pool retirement attachment events publish");
            auto barrier = scripts.value().service(executor);
            expect(barrier.has_value(), "pool retirement reaches a completed script barrier");
            if (barrier) expect(engine->after_service(barrier.value()).has_value(), "pool retirement script results commit");
            const std::string identity(64, 'a');
            const auto reserved = engine->save_tactical_native(identity, scripts.value());
            expect(reserved.has_value(), "reserved live goal has valid native state");
            if (!reserved) { std::cerr << reserved.error().message << '\n'; continue; }
            auto resumed = make_engine();
            const auto loaded = resumed->restore_tactical_native(reserved.value(), identity, scripts.value());
            expect(loaded.has_value(), "reserved goal restores before retirement");
            if (!loaded) { std::cerr << loaded.error().message << '\n'; continue; }
            const auto checkpoint = resumed->save_tactical_native(identity, scripts.value());
            expect(checkpoint && checkpoint.value() == reserved.value(), "reserved goal round-trips before retirement");
            auto& retiring = ai::EngineContractAccess::checked_player(*resumed, 2);
            ai::EngineContractAccess::retire_pool_goal(*resumed, retiring, scripts.value(), abandoned);
            const bool transferred = stage == 4;
            expect(retiring.reserved_pool_tokens.size() == (transferred ? 2U : 1U)
                && retiring.reserved_pool_tokens.at(88) == other_goal && retiring.reserved_pool.at(1) == (transferred ? 2U : 1U)
                && (!transferred || retiring.reserved_pool_tokens.at(77) == other_goal),
                "retirement releases every owned token once and preserves a reassigned token's new owner");
            expect(retiring.reserved_pool.at(2) == 9, "new production does not release a pool reservation of its type");
            expect(retiring.tasks.empty(), "retirement removes pending and completed build tasks");
            const auto saved = resumed->save_tactical_native(identity, scripts.value());
            expect(saved.has_value(), "retired goal leaves no orphan pool reservation in native state");
            if (!saved) { std::cerr << saved.error().message << '\n'; continue; }
            auto restored = make_engine();
            expect(restored->restore_tactical_native(saved.value(), identity, scripts.value()).has_value(), "pool retirement state restores");
            const auto roundtrip = restored->save_tactical_native(identity, scripts.value());
            expect(roundtrip && roundtrip.value() == saved.value(), "pool retirement native bytes round-trip canonically");
            auto& baseline = reference[{stage, abandoned}];
            if (baseline.empty()) baseline = saved.value();
            else expect(baseline == saved.value(), "pool retirement native bytes agree on 1/2/4/8 workers");
        }
}

void active_tactical_desire() {
    for (const bool positive : {false, true}) for (const bool removable : {false, true})
        for (const bool proposals : {false, true}) {
            World world;
            world.view->units.front().health = ai::real(positive ? 1 : 0);
            const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
                return std::nullopt;
            };
            auto equations = ai::EquationSet::parse({{"maintenance.xml", "<Root>"
                "<Desire>50 * (Variable_Target.Health > 0.5)</Desire><Positive>1</Positive></Root>"}}, converters);
            expect(static_cast<bool>(equations), "GS-30: active desire equations load");
            if (!equations) return;
            ai::AiData data; data.equations = std::move(equations).value();
            ai::PlanDef definition; definition.name = "HELD";
            ai::Engine engine(world.host, std::move(data), {definition});
            ai::EngineContractAccess::target_fixture(engine, 1, 10);
            ai::GoalType goal; goal.application = {"FRIENDLY_UNIT"}; goal.category = "HIGH";
            ai::GoalType competitor; competitor.application = {"GLOBAL"}; competitor.category = "HIGH";
            auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, competitor, removable, proposals);
            auth::ModuleManifest modules;
            expect(modules.add("HELD.LUA", "function Hold() Sleep(120) end\n").has_value(), "GS-31: held module loads");
            auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
            expect(static_cast<bool>(scripts), "GS-31: held scheduler starts");
            if (!scripts) return;
            expect(scripts.value().create_instance(1, "HELD.LUA").has_value(), "GS-31: held instance starts");
            std::uint64_t sequence = 0;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            ai::EngineContractAccess::maintain(engine, player);
            const auto found = std::find_if(player.active.begin(), player.active.end(), [](const auto& entry) { return entry.id == 1; });
            const bool retained = positive || !removable;
            expect((found != player.active.end()) == retained && (engine.plan(1) != nullptr) == retained,
                "GS-31: zero-desire trimming honors removability with and without proposals");
            if (found != player.active.end()) expect(found->desire == ai::real(positive ? 50 : 0) && !found->finished,
                "GS-30: tactical active desire uses current target state without finishing a protected plan");
            expect(static_cast<bool>(scripts.value().thread_slots(1)) == retained,
                "GS-31: abandonment removes the plan script while protected plans keep their instance");
        }
}

void active_tactical_preference() {
    for (const std::string raw : {"0.01", "0.02", "10", "40"}) {
        World world;
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"preference.xml", "<Root><Desire>" + raw
            + "</Desire><Positive>1</Positive></Root>"}}, converters);
        expect(static_cast<bool>(equations), "GS-31: preference equations load");
        if (!equations) return;
        ai::AiData data; data.equations = std::move(equations).value();
        ai::PlanDef definition; definition.name = "HELD";
        ai::Engine engine(world.host, std::move(data), {definition});
        ai::EngineContractAccess::target_fixture(engine, 1, 10);
        ai::GoalType goal; goal.application = {"FRIENDLY_UNIT"}; goal.category = "HIGH";
        ai::GoalType competitor; competitor.application = {"FRIENDLY_UNIT"}; competitor.category = "HIGH";
        competitor.is_like = {"RETAINED"};
        auto& player = ai::EngineContractAccess::active_desire_fixture(engine, goal, competitor, true, true);
        goal.per_failure_desire_adjust = goal.per_activation_failure_desire_adjust = ai::real(-50);
        player.goal_outcomes[{"RETAINED", 1}].entries.emplace_back(false, 10000);
        player.activations[{"RETAINED", 1}].entries.emplace_back(false, 10000);
        player.proposed.front().target = 1;
        player.proposed.front().desire = raw == "40" ? ai::real(41) : raw == "10" ? ai::real(11) : ai::real(1) / ai::real(2);
        auth::ModuleManifest modules;
        expect(modules.add("HELD.LUA", "function Hold() Sleep(120) end\n").has_value(), "GS-31: preference module loads");
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        expect(static_cast<bool>(scripts), "GS-31: preference scheduler starts");
        if (!scripts) return;
        expect(scripts.value().create_instance(1, "HELD.LUA").has_value(), "GS-31: preference instance starts");
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
        ai::EngineContractAccess::maintain(engine, player);
        const bool retained = raw == "0.02" || raw == "10";
        expect(player.active.size() == 1 && player.active.front().id == (retained ? 1U : 2U)
            && (engine.plan(1) != nullptr) == retained,
            "GS-31: active preference prevents nearby like-goal replacement only inside its open bounds");
        if (retained) expect(player.active.front().desire == ai::to_single(ai::real(raw == "10" ? 10 : 2) / ai::real(raw == "10" ? 1 : 100)),
            "GS-30: tactical active reevaluation does not reapply proposal failure adjustments");
    }
}

void initial_goal_budget() {
    World world;
    ai::Engine engine(world.host, ai::AiData{}, {});
    ai::GoalType global;
    global.application = {"GLOBAL"};
    ai::PlayerAi player;
    player.functions.resize(151);
    for (auto& function : player.functions) function.goal = &global;
    ai::EngineContractAccess::initialize_goals(engine, player);
    expect(player.per_frame == 2, "GS-05: an empty target list estimates one target and rounds 151 evaluations up");
    ai::EngineContractAccess::goals(engine, player);
    expect(player.next_function == 2 && player.nontrivial == 2
        && ai::EngineContractAccess::maintenances(engine) == 0,
        "GS-05: the first proposal service consumes the initialized budget");
    for (int frame = 1; frame < 76; ++frame) ai::EngineContractAccess::goals(engine, player);
    expect(ai::EngineContractAccess::maintenances(engine) == 1 && player.nontrivial == 0 && player.per_frame == 2,
        "GS-05: first maintenance uses the actual pass count and resets its accumulator");

    ai::PlayerAi sparse;
    sparse.targets.resize(151);
    sparse.functions.resize(1);
    sparse.functions.front().goal = &global;
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 2, "GS-05: initialization estimates targets times functions before applicability");
    ai::EngineContractAccess::goals(engine, sparse);
    expect(sparse.per_frame == 1 && ai::EngineContractAccess::maintenances(engine) == 2,
        "GS-05: later budgets use the actual one global evaluation, not the initial estimate");
    sparse.targets.resize(150);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 1, "GS-05: an exact five-second evaluation budget does not round up");
    sparse.functions.clear();
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 1, "GS-05: no goal functions still leaves a positive service budget");

    world.host->setup.players.resize(8);
    world.host->setup.players.front() = foc::AiPlayer{1, "REBEL", false, false, "", true};
    for (std::size_t index = 1; index < 8; ++index) {
        world.host->setup.players[index] = foc::AiPlayer{static_cast<eawr::sim::tactical::PlayerId>(index + 1),
            index == 2 ? "UNDERWORLD" : "EMPIRE", index == 7, index <= 2, ""};
    }
    sparse.targets.resize(2000);
    sparse.functions.resize(1);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 3,
        "GS-05: two AI and five non-playable faction players cap at rounded 20/7 with one human");
    world.host->setup.players.resize(5);
    ai::EngineContractAccess::initialize_goals(engine, sparse);
    expect(sparse.per_frame == 5,
        "GS-05: three-faction FFA with Empire/Underworld AI and two faction players counts only the human");
}

void selection_team_starts() {
    World world;
    auto second = world.host->setup.content.types.front();
    second.type_id = 2;
    second.name = "OTHER_SHIP";
    world.host->setup.content.types.push_back(second);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const auto& type : world.host->setup.content.types) {
        world.host->types.emplace(type.type_id, &type);
        world.host->types_by_name.emplace(type.name, &type);
    }
    auto unit = world.view->units.front();
    unit.id = 11;
    unit.type = 2;
    world.view->units.push_back(unit);
    const auto force = [](eawr::sim::tactical::TypeId type) {
        ai::TaskForceDef result;
        ai::TeamDef team;
        team.max_count = 1;
        team.types = {type};
        result.teams.push_back(team);
        return result;
    };
    ai::PlayerAi player;
    player.player = 2;
    ai::PotentialPlan potential;
    ai::PlanDef plan;
    plan.taskforces = {force(1), force(2)};
    ai::Engine singleton(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(singleton, player, potential, 12345),
        "PL-22: types with a single eligible team select both available objects");
    expect(potential.taskforce_of_unit == std::vector<std::size_t>{0, 1}
        && player.sync.seed() == 12345,
        "PL-22: distinct single-team types consume no synchronized draw");
    world.view->units.clear();
    ai::Engine empty(world.host, {}, {plan});
    expect(!ai::EngineContractAccess::selection(empty, player, potential, 12345)
        && player.sync.seed() == 12345,
        "PL-22: a multi-team plan without proposals consumes no synchronized draw");
    world.view->units = {unit};
    world.view->units.front().id = 10;
    world.view->units.front().type = 1;
    world.view->units.push_back(unit);
    plan.taskforces = {force(1), force(2), force(1), force(2)};
    ai::Engine independent(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(independent, player, potential, 12345),
        "PL-22: types with multiple eligible teams select both available objects");
    expect(potential.taskforce_of_unit == std::vector<std::size_t>{0, 3}
        && player.sync.seed() == 0xa4451d33U && ai::EngineContractAccess::engine_seed(independent) == 0,
        "PL-22: each type draws independently among its own teams before allocation");
    plan.taskforces = {force(1), force(2)};
    plan.ignore_target = true;
    potential = {};
    ai::Engine untargeted(world.host, {}, {plan});
    expect(ai::EngineContractAccess::selection(untargeted, player, potential, 12345)
        && player.sync.seed() != 12345 && ai::EngineContractAccess::engine_seed(untargeted) == 0,
        "GFS-A24: ignore-target contrast weights advance only the selecting player's stream");
}

void station_barrier_expiry() {
    for (const auto workers : {1U, 2U, 4U, 6U, 8U}) {
        World world;
        auto& station = world.host->setup.content.types.front();
        station.star_base = true;
        station.base_level = 3;
        auto replacement = station;
        replacement.type_id = 2;
        replacement.name = "REPLACEMENT_STATION";
        replacement.base_level = 5;
        world.host->setup.content.types.push_back(replacement);
        world.host->types.clear();
        for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
        world.host->setup.schedule.mode = foc::AiSchedule::Mode::faithful;
        world.host->setup.players = {foc::AiPlayer{1, "REBEL", false, true, "STATION"}};
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}};
        setup.units = {{10, 1, 1, {}}, {20, 2, 1, {}}};
        tactical::EconomyRules economy;
        economy.players = {{1, {}, 20}};
        auto session = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, {}, economy);
        expect(session.has_value(), "station expiry: tactical session creates");
        if (!session) return;
        world.host->world = &session.value();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"station.xml", "<Root><Level>Variable_Self.BaseLevel</Level></Root>"}}, converters);
        expect(equations.has_value(), "station expiry: equation parses");
        if (!equations) return;
        ai::AiData data;
        data.equations = std::move(equations).value();
        data.players["STATION"] = ai::PlayerType{};
        ai::Engine engine(world.host, std::move(data), {});
        auto scripts = auth::ScriptScheduler::create({}, {});
        expect(scripts.has_value(), "station expiry: scheduler creates");
        if (!scripts) return;
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::uint64_t sequence = 0;
        for (const auto expected : {3, 5}) {
            world.host->snapshot = session.value().snapshot();
            world.host->view = foc::detail::build_view(session.value(), *world.host->snapshot, &executor);
            unsigned calls = 0;
            const ai::Engine::ExecutionHook hook = [&](tactical::PlayerId player, ai::Engine::ExecutionEffects&) {
                ++calls;
                expect(player == 1 && ai::EngineContractAccess::has_prepared_station_levels(engine),
                    "station expiry: real service retains prepared levels inside the barrier");
                expect(engine.input_counts().producer_prepasses == 1 && engine.input_counts().pad_prepasses == 1
                    && engine.input_counts().late_prepasses == 0 && ai::EngineContractAccess::has_prepared_pads(engine),
                    "shared inputs: producer and pad preparation precede the actual player hook");
                expect(engine.evaluate("Level", player, nullptr) == ai::real(expected),
                    "station expiry: barrier query sees the current station level");
                return eawr::core::Result<void>::success();
            };
            expect(engine.before_service(*world.host->ai_world, scripts.value(), sequence, &executor, hook).has_value(),
                "station expiry: real before_service completes");
            expect(calls == 1, "station expiry: execution hook observed the prepared barrier");
            expect(!ai::EngineContractAccess::has_prepared_pads(engine), "shared inputs: barrier pad rows expire before Lua reads");
            expect(!ai::EngineContractAccess::has_prepared_station_levels(engine),
                "station expiry: real before_service expires prepared levels on return");
            expect(session.value().stage_remove(expected == 3 ? 10 : 20).has_value(),
                "station expiry: station removal between services succeeds");
            expect(engine.evaluate("Level", 1, nullptr) == ai::real(expected == 3 ? 5 : 0),
                "station expiry: later query sees removal instead of stale prepared levels");
            expect(scripts.value().service(executor).has_value(), "station expiry: later script service completes");
            // The faithful execution service is due again after three tactical ticks.
            for (unsigned tick = 0; tick < 3; ++tick)
                expect(session.value().step(executor).has_value(), "station expiry: next service frame advances");
        }
    }
}

void shared_station_perception() {
    for (const auto workers : {1U, 2U, 4U, 6U, 8U}) {
        World world;
        auto& station = world.host->setup.content.types.front();
        station.star_base = true;
        station.base_level = 3;
        station.previous_base_levels = {2, 1};
        station.tech_level = 7;
        auto enemy = station;
        enemy.type_id = 2;
        enemy.name = "ENEMY_STATION";
        enemy.base_level = 5;
        world.host->setup.content.types.push_back(enemy);
        world.host->types.clear();
        for (const auto& type : world.host->setup.content.types) world.host->types.emplace(type.type_id, &type);
        world.view->players = {{1, 0, 0}, {2, 1, 1}, {3, 0, 2}, {4, 1, 3}};
        eawr::sim::tactical::TacticalSetup setup;
        setup.players = {{1, 0, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 1, 2, eawr::sim::tactical::player_flag_commandable},
            {3, 0, 1, eawr::sim::tactical::player_flag_commandable},
            {4, 1, 2, eawr::sim::tactical::player_flag_commandable}};
        setup.units = {{10, 1, 1, {}}, {20, 2, 2, {}}, {25, 2, 3, {}}};
        eawr::sim::tactical::EconomyRules economy;
        economy.players = {{1, {}, 20}, {2, {}, 20}, {3, {}, 20}, {4, {}, 20}};
        for (auto& player : economy.players) { player.start_tech = 1; player.max_tech = 5; }
        namespace tactical = eawr::sim::tactical;
        const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
        tactical::DurabilityTable health;
        tactical::DurabilityProfile profile;
        profile.type_id = 1;
        profile.max_hull = q(1000);
        profile.hardpoints.resize(10);
        for (std::size_t index = 0; index < 9; ++index) {
            profile.hardpoints[index].destroyable = true;
            profile.hardpoints[index].max_health = q(100);
        }
        health.profiles.push_back(profile);
        auto session = tactical::TacticalSession::create(setup, {}, health, {}, std::nullopt, {}, {}, {}, economy);
        expect(static_cast<bool>(session), "SAE-02: four ledgers share three live stations");
        if (!session) return;
        world.host->world = &session.value();
        world.host->snapshot = session.value().snapshot();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"team.xml", "<Root><Level>Variable_Self.BaseLevel</Level></Root>"}}, converters);
        expect(static_cast<bool>(equations), "SAE-02: shared station perception equation parses");
        if (!equations) return;
        ai::AiData data;
        data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto level = [&](tactical::PlayerId player) {
            const auto direct = engine.evaluate("Level", player, nullptr);
            const auto prepared = ai::EngineContractAccess::prepared_station_level(engine, player, executor);
            expect(prepared == direct,
                "partitioned station perception matches live queries after damage, content edits and removals");
            expect(engine.evaluate("Level", player, nullptr) == direct,
                "clearing the test preparation restores uncached live station queries");
            return prepared;
        };
        for (const auto player : {1U, 3U})
            expect(level(player) == ai::real(3),
                "SAE-02: both teammates read the first station, excluding higher allied and enemy levels");
        for (const auto player : {2U, 4U})
            expect(level(player) == ai::real(5),
                "SAE-02: the opposing team's second player also sees its shared station");
        auto& first = world.host->setup.content.types.front();
        first.capture_point = true;
        expect(level(1) == ai::real(5),
            "SAE-02: a capture-point station is skipped for the next qualifying ally");
        first.capture_point = false;
        first.unit_ai = false;
        expect(level(3) == ai::real(5),
            "SAE-02: a non-UnitAI station does not enter allied station perception");
        first.unit_ai = true;
        const auto configuration = foc::tactical_ai_detail::native_configuration(world.host->setup);
        auto changed = world.host->setup;
        changed.content.types.front().previous_base_levels = {1};
        const auto changed_configuration = foc::tactical_ai_detail::native_configuration(changed);
        expect(configuration && changed_configuration && configuration.value() != changed_configuration.value(),
            "SAE-02: predecessor levels participate in the native configuration identity");
        first.base_level = 12;
        expect(level(1) == ai::real(12),
            "SAE-02: an undersized hardpoint layout retains its authored level");
        first.base_level = 3;
        for (std::uint32_t index = 0; index < 9; ++index) {
            const auto frame = session.value().completed_tick();
            expect(session.value().submit({{frame, 1, index}, {10}, tactical::DamagePayload{q(100), index}}).has_value(),
                "SAE-02: targeted station hardpoint damage is admitted");
            expect(session.value().step(executor).has_value(), "SAE-02: station damage commits on every worker count");
            world.host->snapshot = session.value().snapshot();
            world.host->view = foc::detail::build_view(session.value(), *world.host->snapshot, &executor);
            const auto expected = index == 8 ? 0 : 3 - (3 * (index + 1)) / 9;
            for (const auto player : {1U, 3U})
                expect(level(player) == ai::real(expected),
                    "SAE-02: only destroyed destroyable hardpoints reduce the first allied station's level");
            if (index == 2) {
                first.previous_base_levels.clear();
                expect(level(1) == Real{},
                    "SAE-02: damage with a missing predecessor returns zero");
                first.previous_base_levels = {1};
                expect(level(3) == Real{},
                    "SAE-02: a predecessor chain that skips the surviving level returns zero");
                first.previous_base_levels = {2, 1};
            }
            expect(session.value().ledgers().front().tech_level == 1,
                "SAE-02: perceived station damage leaves the player's tech ledger independent");
        }
        expect(static_cast<bool>(session.value().stage_remove(10)), "SAE-02: remove shared station");
        expect(level(1) == ai::real(5),
            "SAE-02: station removal advances to the next qualifying ally");
        expect(static_cast<bool>(session.value().stage_remove(25)), "SAE-02: remove remaining allied station");
        for (const auto player : {1U, 3U})
            expect(level(player) == Real{},
                "SAE-02: station loss clears both teammates' level without borrowing the enemy's");
        expect(level(4) == ai::real(5), "SAE-02: other team's station survives independently");
    }
}

void campaign_defender_fallback() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        world.host->setup.perception.campaign_game = true;
        world.host->setup.perception.campaign_defender = 2;
        world.host->setup.perception.base_level = 1;
        auth::ModuleManifest manifest;
        expect(manifest.add("DEFENDER.LUA", "function Check(player) Allowed = EvaluatePerception(\"Allowed_As_Defender_Land\", player) end\n").has_value(),
            "campaign defender fallback fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, world.host, errors);
        expect(errors.empty() && scripts.create_instance(1, "DEFENDER.LUA").has_value(), "perception fallback binds without a goal engine");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const tactical::PlayerId player : {1U, 2U}) {
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, player}}};
            expect(scripts.submit_event(std::move(event)).has_value() && scripts.service(executor).has_value(), "fallback player call services");
            const auto allowed = scripts.read_global(1, "Allowed");
            expect(allowed && allowed.value() && std::get<Real>(allowed.value()->data) == ai::real(player == 1 ? 1 : 0),
                "campaign fallback qualifies each player's defender role without an engine");
        }
    }
}

void campaign_retreat_inputs() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto& setup = world.host->setup;
        setup.perception.campaign_game = true;
        setup.perception.campaign_defender = 2;
        eawr::sim::tactical::TacticalSetup initial;
        initial.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
            {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
        auto child = eawr::sim::tactical::TacticalSession::create(initial).value();
        world.host->snapshot = child.snapshot();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return {}; };
        auto equations = ai::EquationSet::parse({{"campaign.xml", "<Root><Retreat>Variable_Self.CanRetreat</Retreat>"
            "<Defender>Variable_Self.IsDefender</Defender></Root>"}}, converters).value();
        ai::AiData data; data.equations = std::move(equations);
        ai::Engine engine(world.host, std::move(data), {});
        world.host->engine = &engine;
        ai::EngineContractAccess::collect_fixture(engine);
        expect(engine.evaluate("Retreat", 2, nullptr) == Real{}, "WGT-33: absent retreat qualification remains inert");
        setup.campaign_can_retreat = [](const auto& snapshot, auto player) { return player == 2 && snapshot.completed_tick() == 0; };
        expect(engine.evaluate("Retreat", 2, nullptr) == Real{}, "WGT-33: eligibility alone cannot enable an unroutable retreat");
        setup.campaign_retreat_order = [](eawr::sim::tactical::PlayerId player) {
            return eawr::core::Result<auth::TacticalOrder>::success({player, {}, eawr::sim::tactical::QuitPayload{}});
        };
        expect(engine.evaluate("Retreat", 2, nullptr) == ai::real(1) && engine.evaluate("Retreat", 1, nullptr) == Real{},
            "WGT-33: retreat reads the owning campaign's per-player snapshot qualification");
        expect(engine.evaluate("Defender", 2, nullptr) == ai::real(1) && engine.evaluate("Defender", 1, nullptr) == Real{},
            "WGT-33: defender is per player");
        auth::ModuleManifest manifest;
        expect(manifest.add("WITHDRAW.LUA", "function Check(tf) tf.Withdraw_Units() end\n").has_value(), "campaign withdrawal fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest)).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts, world.host, errors);
        expect(errors.empty() && scripts.create_instance(1, "WITHDRAW.LUA").has_value(), "stock withdrawal binding registers");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        for (const bool campaign : {true, false}) {
            setup.perception.campaign_game = campaign;
            auth::ScriptEvent event; event.key = {scripts.completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}}};
            expect(scripts.submit_event(std::move(event)).has_value(), "withdrawal call queues");
            const auto report = scripts.service(executor).value();
            expect(report.diagnostics.empty(), "withdrawal binding has no diagnostic");
            expect(report.commands.size() == (campaign ? 1U : 0U), "only qualified campaign withdrawal stages a command");
            if (campaign) expect(report.commands[0].verb == foc::verb_campaign_retreat
                && report.commands[0].arguments.size() == 1, "withdrawal uses the ordinary deferred replay command route");
            else expect(engine.evaluate("Retreat", 2, nullptr) == Real{}, "standalone still perceives retreat disabled");
        }
    }
}

void economy_inputs() {
    World world;
    foc::AiType other; other.type_id = 2; other.name = "OTHER";
    world.host->types.emplace(2, &other); world.host->types_by_name.emplace(other.name, &other);
    world.host->setup.perception.campaign_game = false;
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto equations = ai::EquationSet::parse({{"economy.xml", "<Root>"
        "<Campaign>Game.IsCampaignGame</Campaign>"
        "<Credits>Variable_Self.CreditsUnnormalized</Credits>"
        "<Room>Variable_Self.UnitSpaceAvailable</Room>"
        "<Pool>Variable_Self.ReinforcementsUnnormalized</Pool>"
        "<Level>Variable_Self.BaseLevel</Level>"
        "<Structures>Variable_Self.TacticalBuiltStructureCount{Parameter_Type = \"SHIP\"}</Structures>"
        "<FilteredPool>Variable_Self.ReinforcementsUnnormalized{Parameter_Category = 1}</FilteredPool>"
        "<OpenPads>Variable_Self.OpenBuildPadCount{Parameter_Type = \"SHIP\"}</OpenPads>"
        "<OtherPads>Variable_Self.OpenBuildPadCount{Parameter_Type = \"OTHER\"}</OtherPads>"
        "<Contestable>Variable_Target.IsContestable</Contestable>"
        "<BuildPad>Variable_Target.IsBuildPad</BuildPad>"
        "</Root>"}}, converters);
    expect(static_cast<bool>(equations), "SAE-02: economy equations parse");
    if (!equations) return;
    ai::AiData data;
    data.equations = std::move(equations).value();
    ai::Engine engine(world.host, std::move(data), {});
    expect(engine.evaluate("Campaign", 1, nullptr) == Real{}, "SAE-01: skirmish campaign input is zero");
    expect(engine.evaluate("Credits", 1, nullptr) == Real{}, "SAE-02: absent economy has no spendable credits");
    const auto snapshot = [&](std::uint32_t population, std::uint32_t cap, std::int64_t credits) {
        eawr::sim::tactical::EconomyView account;
        account.player = 1;
        account.credits = eawr::sim::math::Fixed::from_integer(credits).value();
        account.population = population;
        account.population_cap = cap;
        account.pool = {1, 1};
        world.host->snapshot = std::make_shared<eawr::sim::tactical::TacticalSnapshot>(0,
            std::vector<eawr::sim::tactical::SnapshotPlayer>{}, std::vector<eawr::sim::tactical::TacticalInstance>{},
            std::vector<eawr::sim::tactical::Event>{}, std::vector<eawr::sim::tactical::CombatEvent>{},
            std::vector<eawr::sim::tactical::Projectile>{}, std::nullopt,
            std::vector<eawr::sim::tactical::SpinningCraft>{}, std::vector<eawr::sim::tactical::SquadronTarget>{},
            std::vector<eawr::sim::tactical::Squadron>{}, std::vector<eawr::sim::tactical::EconomyView>{account});
    };
    snapshot(7, 20, 1499);
    expect(engine.evaluate("Credits", 1, nullptr) == ai::real(1499), "SAE-02: credits read the completed account");
    expect(engine.evaluate("Room", 1, nullptr) == ai::real(13), "SAE-02: population room is cap minus population");
    expect(engine.evaluate("Pool", 1, nullptr) == ai::real(2000), "SAE-02: reinforcement power includes both pooled units");
    expect(engine.evaluate("FilteredPool", 1, nullptr) == Real{}, "SAE-02: reinforcement power honors the requested category");
    snapshot(23, 20, 1500);
    expect(engine.evaluate("Room", 1, nullptr) == Real{}, "SAE-02: an over-cap account never underflows room");
    expect(engine.evaluate("Credits", 1, nullptr) == ai::real(1500), "SAE-02: the saving threshold uses current credits");
    eawr::sim::tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, eawr::sim::tactical::player_flag_commandable},
                     {2, 2, 2, eawr::sim::tactical::player_flag_commandable}};
    setup.units = {{10, 1, 1}, {11, 1, 1}, {12, 1, 2}};
    eawr::sim::tactical::EconomyRules rules;
    rules.players = {{1, eawr::sim::math::Fixed::from_integer(1000).value(), 0},
        {2, eawr::sim::math::Fixed::from_integer(1000).value(), 0}};
    rules.pads.neutral = 2;
    rules.pads.capture = {{1, eawr::sim::math::Fixed::from_integer(100).value(),
        eawr::sim::math::Fixed::from_integer(1).value(), {1, 2}, false, true, true}};
    auto session = eawr::sim::tactical::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(session), "SAE-02: the synthetic ownership world starts");
    if (session) {
        world.host->world = &session.value();
        world.host->setup.content.types[0].star_base = true;
        world.host->setup.content.types[0].base_level = 3;
        world.host->setup.content.types[0].tech_level = 7;
        expect(engine.evaluate("Level", 1, nullptr) == ai::real(3), "SAE-02: station level uses Base_Level, independently of Tech_Level");
        expect(engine.evaluate("Structures", 1, nullptr) == Real{}, "WAS-10: unbuilt pads are not completed structures");
        expect(session.value().production_counts(1, 1).owned_player == 2, "SAE-03: ownership cache starts from live units");
        expect(session.value().production_counts(99, 1).current_allies == 0,
            "SAE-02: an undeclared AI service slot has no production ownership");
        expect(!session.value().build_allowed(99, 10, 1), "SAE-03: an undeclared buyer cannot use a live producer");
        world.host->setup.content.types[0].capture_point = true;
        world.view->units.clear();
        for (const auto& placed : setup.units) {
            ai::ViewUnit unit;
            unit.id = placed.entity_id;
            unit.type = placed.type_id;
            unit.owner = placed.owner;
            world.view->units.push_back(unit);
        }
        expect(engine.evaluate("OpenPads", 1, nullptr) == ai::real(2), "SAE-02: open pad count excludes the other player's pads");
        expect(engine.evaluate("OtherPads", 1, nullptr) == Real{}, "SAE-02: open pad count honors the requested type");
        ai::Target target;
        target.object = 10;
        expect(engine.evaluate("Contestable", 1, &target) == ai::real(1), "SAE-02: capture targets expose real contestability");
        expect(engine.evaluate("BuildPad", 1, &target) == ai::real(1), "SAE-02: capture targets expose their build capability");
    }
    ai::GoalType goal;
    goal.time_limit = ai::real(30);
    goal.build_time_delay_tolerance = ai::real(2);
    expect(ai::production_time_limit(goal, std::nullopt, ai::real(5)) == ai::real(35), "SAE-05: no potential uses the authored time limit");
    expect(ai::production_time_limit(goal, ai::real(10), ai::real(5)) == ai::real(30), "SAE-05: potential limit multiplies duration plus adjustment");
    expect(ai::production_time_allowed(goal, ai::real(10), ai::real(20)), "SAE-05: equality is admitted");
    expect(!ai::production_time_allowed(goal, ai::real(10), ai::real(21)), "SAE-05: exceeding a positive limit is rejected");
    goal.build_time_delay_tolerance = Real{};
    expect(ai::production_time_allowed(goal, ai::real(10), ai::real(200)), "SAE-05: a zero limit does not reject");
}

void tactical_activation_estimate() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    for (const int scenario : {0, 1, 2}) {
        t::TacticalSetup start;
        start.players = {{1, 1, 100, t::player_flag_commandable}};
        start.units = {{1, 10, 1}};
        t::EconomyRules rules;
        rules.players = {{1, q(scenario == 2 ? 0 : 1000), 20}};
        rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 900, 900, 1, true}}}};
        auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "SAE-09: activation fixture starts");
        if (!made) continue;
        if (scenario == 1) {
            expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value(),
                "SAE-09: long-running production enters the queue");
            eawr::sim::InlineExecutor executor;
            expect(made.value().step(executor).has_value(), "SAE-09: queued purchase advances");
            expect(!made.value().ledgers().front().queues[0].empty(), "SAE-09: existing delay is present");
        }
        auto host = ai::make_tactical_host();
        host->setup.perception.campaign_game = false;
        foc::AiType factory;
        factory.type_id = 10;
        factory.star_base = true;
        host->types.emplace(10, &factory);
        host->world = &made.value();
        host->snapshot = made.value().snapshot();
        host->view = foc::detail::build_view(made.value(), *host->snapshot);
        ai::PlanDef definition;
        definition.allow_free_store = false;
        definition.ignore_target = true;
        ai::TaskForceDef force;
        ai::TeamDef team;
        team.min_count = team.max_count = 1;
        team.types = {50};
        force.teams = {team};
        definition.taskforces = {force};
        ai::Engine engine(host, {}, {definition});
        expect(engine.producer(1, 50) == 1 && host->economy(1) != nullptr,
            "SAE-09: tactical selection has a producer and a current credit account");
        ai::GoalType type;
        type.time_limit = ai::real(1);
        type.build_time_delay_tolerance = ai::real(1) / ai::real(100);
        ai::PlayerAi player;
        player.player = 1;
        ai::GoalFunctionEntry function;
        function.goal = &type;
        player.functions.push_back(function);
        ai::Goal goal;
        goal.id = 1;
        goal.potential.emplace();
        goal.potential->valid = true;
        const bool allowed = ai::EngineContractAccess::test_valid(engine, player, goal);
        expect(allowed == (scenario != 2),
            "SAE-09/03: production duration and queue delay do not block selection; insufficient credits do");
        if (allowed) expect(goal.potential->units == std::vector<t::TypeId>{50}
            && goal.potential->sources == std::vector<std::uint8_t>{2},
            "SAE-09: selected new production survives a positive delay limit below its duration");
    }
}

void production_lifecycle() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    // Stations use the owner's faction even when an allied buyer has another menu.
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}};
    start.units = {{1, 10, 2}};
    t::EconomyRules rules;
    rules.players = {{1, q(1000), 20}, {2, q(1000), 20}};
    rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(900), 30, 30, 1, true}}},
        {10, 200, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 1, true},
                  {51, t::BuildKind::unit, t::BuildQueue::units, q(200), 30, 30, 1, true}}}};
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-03: allied producer fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    auto view = std::make_shared<ai::WorldView>();
    ai::ViewUnit station;
    station.id = 1; station.type = 10; station.owner = 2;
    view->units.push_back(station);
    for (eawr::sim::EntityId id = 2; id <= 1001; ++id) {
        ai::ViewUnit unit; unit.id = id; unit.type = 99;
        view->units.push_back(unit);
    }
    foc::AiType station_type;
    station_type.type_id = 10; station_type.star_base = true;
    host->types.emplace(10, &station_type);
    host->view = view; host->world = &made.value();
    ai::Engine engine(host, {}, {});
    expect(made.value().build_allowed(1, 1, 51) && engine.producer(1, 51) == 1,
        "SAE-03: AI discovers a type exclusive to its allied station owner's faction");
    const auto* option = engine.build_option(1, 50, 1);
    expect(option != nullptr && option->price == q(100), "SAE-03: AI uses owner-faction station price");
    for (std::uint32_t i = 0; i < 100; ++i) {
        expect(engine.producer(1, 500 + i) == 0, "SAE-03: missing type has no producer");
        expect(engine.producer(1, 50) == 1, "SAE-03: indexed producer remains deterministic");
    }
    expect(engine.producer_work().entities == 1001 && engine.producer_work().candidates == 101,
        "SAE-03: repeated candidates and misses scan world entities only once per service");
    eawr::sim::InlineExecutor executor;
    expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value(), "SAE-03: allied purchase submits");
    expect(made.value().step(executor).has_value() && made.value().ledgers().front().credits == q(900),
        "SAE-03: ordinary purchase debits the same owner-faction price");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    expect(static_cast<bool>(scripts), "SAE-03: lifecycle scheduler starts");
    if (!scripts) return;
    std::uint64_t sequence = 0;
    const auto snapshot = made.value().snapshot();
    expect(engine.before_service(*host->ai_world, scripts.value(), sequence).has_value(),
        "SAE-03: next AI service resets its derived producer index");
    expect(engine.producer(1, 50) == 1 && engine.producer_work().entities == 1001 && engine.producer_work().candidates == 1,
        "SAE-03: producer work counters reset and the next service rebuilds once");

    // Rejection, constructor removal, pad destruction and failed final replacement all
    // finish the block and release its reservation; the ordinary successful build still waits.
    // The allied pad owner remains a combatant; neutral capture ownership is a distinct player.
    for (const int scenario : {0, 1, 2, 3, 4}) {
        start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}, {3, 3, 300, 0}};
        start.units = {{1, 20, 2}};
        rules.players = {{1, q(1000), 20, true}, {2, q(1000), 20}, {3, q(1000), 20}};
        rules.menus = {{20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false},
            {20, 200, {{31, t::BuildKind::structure, t::BuildQueue::units, q(900), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 3;
        rules.pads.capture = {{20, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}, {31, 41, q(100), 1, 1}};
        rules.pads.influence = {{20, false}, {30, false}, {40, false}};
        t::DurabilityTable health;
        for (const t::TypeId type : {20U, 30U, 31U, 40U, 41U}) {
            if (scenario == 3 && type == 40) continue;
            t::DurabilityProfile profile; profile.type_id = type; profile.max_hull = q(300);
            health.profiles.push_back(profile);
        }
        auto pad_world = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(pad_world), "SAE-03: pad lifecycle fixture starts");
        if (!pad_world) continue;
        host = ai::make_tactical_host();
        host->world = &pad_world.value(); host->snapshot = pad_world.value().snapshot();
        host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
        ai::Engine pad_engine(host, {}, {ai::PlanDef{}});
        expect(pad_engine.build_option(1, 30, 1) != nullptr && pad_engine.build_option(1, 31, 1) == nullptr,
            "SAE-03: allied pad preserves buyer-faction menu exception");
        expect(pad_engine.producer(1, 30) == 1, "SAE-03: indexed allied pad is initially available");
        auto& player = ai::EngineContractAccess::add_player(pad_engine, 1);
        ai::EngineContractAccess::seed(pad_engine, player, scripts.value());
        expect(pad_engine.producer(1, 30) == 0, "SAE-03: a reservation made after indexing blocks the next ordered lookup");
        ai::EngineContractAccess::service(pad_engine, player, 1);
        expect(ai::EngineContractAccess::orders(pad_engine) == 1 && !pad_engine.block(1)->finished,
            "SAE-03: pad production emits an ordinary command and waits");
        if (scenario == 0) {
            expect(pad_world.value().submit({{0, 2, 0}, {1}, t::PadBuildPayload{31}}).has_value(), "SAE-03: competing allied build submits");
            expect(pad_world.value().step(executor).has_value(), "SAE-03: competing construction occupies pad");
            expect(pad_world.value().submit({{1, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value(), "SAE-03: stale AI request submits for Core rejection");
        } else {
            expect(pad_world.value().submit({{0, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value(), "SAE-03: pad build submits");
        }
        expect(pad_world.value().step(executor).has_value(), "SAE-03: pad command delivery completes");
        if (scenario == 1 || scenario == 2) {
            const auto victim = scenario == 1 ? pad_world.value().pads().at(1).under_construction : 1;
            expect(victim != 0 && pad_world.value().submit({{1, 1, 1}, {victim}, t::DamagePayload{q(1000), t::attack_hull}}).has_value(),
                "SAE-03: constructor or pad destruction submits");
            expect(pad_world.value().step(executor).has_value(), "SAE-03: destruction commits");
        } else if (scenario >= 3) {
            host->snapshot = pad_world.value().snapshot();
            host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
            ai::EngineContractAccess::service(pad_engine, player, 4);
            expect(!pad_engine.block(1)->finished, "SAE-03: accepted construction remains pending while its child lives");
            for (int frame = 0; frame < 31; ++frame) expect(pad_world.value().step(executor).has_value(), "SAE-03: construction advances");
        }
        host->snapshot = pad_world.value().snapshot();
        host->view = foc::detail::build_view(pad_world.value(), *host->snapshot);
        ai::EngineContractAccess::service(pad_engine, player, 100);
        expect(player.tasks.empty() && pad_engine.block(1)->finished && pad_engine.block(1)->result == (scenario == 4),
            "SAE-03: pad outcome completes the production block with the correct result");
        expect(pad_engine.plan(1)->reserved_pads.empty() && player.reserved_credits.at(1) == Real{},
            "SAE-03: completed or failed pad task releases pad and credit reservations");
        if (scenario == 4) expect(ai::EngineContractAccess::completed_pad_contrast(pad_engine, player),
            "GS-22: real completed pad build remains valid after execution releases its reservation");
    }
}

void spending_pad_states() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        t::TacticalSetup start;
        start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}, {3, 2, 200, 1}, {4, 1, 300, 1}};
        start.units = {{1, 20, 1}, {2, 20, 2}, {3, 20, 3}, {4, 20, 4}, {5, 10, 1}, {6, 20, 2}};
        for (std::size_t i = 0; i < start.units.size(); ++i) start.units[i].position.x = q(static_cast<std::int64_t>(i) * 1000);
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20}, {2, q(1000), 20, true}, {3, q(1000), 20, true}, {4, q(1000), 20}};
        rules.pads.neutral = 4;
        rules.pads.capture = {{20, q(100), q(1), {100, 200, 300}, true, true, true, {}, false, 6}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}, {31, 41, q(100), 1, 1}, {32, 42, q(100), 1, 1}};
        rules.pads.influence = {{10, false}, {20, false}, {30, false}, {31, false}, {32, false}, {40, false}, {41, false}, {42, false}};
        for (const auto& [faction, type] : {std::pair{100U, 30U}, std::pair{200U, 31U}, std::pair{300U, 32U}})
            rules.menus.push_back({20, faction, {{type, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false});
        t::DurabilityTable health;
        for (const auto type : {20U, 30U, 31U, 32U, 40U, 41U, 42U}) {
            t::DurabilityProfile row; row.type_id = type; row.max_hull = q(300); health.profiles.push_back(row);
        }
        auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "WAS-10/11: typed allied pad fixture starts");
        if (!made) continue;
        auto& world = made.value();
        auto host = ai::make_tactical_host(); host->world = &world;
        host->setup.journal = std::make_shared<foc::AiJournal>();
        for (const auto& [type, name] : {std::pair{20U, "PAD"}, {30U, "UC_E"}, {31U, "UC_R"}, {32U, "UC_U"},
                {40U, "MINE_E"}, {41U, "MINE_R"}, {42U, "MINE_U"}}) {
            foc::AiType row; row.type_id = type; row.name = name; host->setup.content.types.push_back(row);
        }
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        host->setup.players = {{1, "EMPIRE", false, false, "", true}, {2, "EMPIRE", false, true, ""},
            {3, "REBEL", false, true, ""}, {4, "NEUTRAL", true, false, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        const auto refresh = [&] {
            host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot, &executor);
            host->credit_changes.clear();
        };
        refresh();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"spending.xml", "<Root>"
            "<Resources>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"MINE_E\", Parameter_Type=\"MINE_R\", Parameter_Type=\"MINE_U\"}</Resources>"
            "<ResourceDesire>Function_Resources.Evaluate > 0</ResourceDesire>"
            "<Constructors>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UC_E\"}</Constructors>"
            "<AllStructures>Variable_Self.TacticalBuiltStructureCount</AllStructures>"
            "<ForeignStructures>Variable_Enemy.TacticalBuiltStructureCount</ForeignStructures>"
            "<Open>Variable_Self.OpenBuildPadCount</Open>"
            "<Menu>Variable_Self.OpenBuildPadCount{Parameter_Type=\"UC_E\"}</Menu>"
            "<Multiple>Variable_Self.OpenBuildPadCount{Parameter_Type=\"PAD\", Parameter_Type=\"UC_E\", Parameter_Type=\"PAD\"}</Multiple>"
            "<WrongMenu>Variable_Self.OpenBuildPadCount{Parameter_Type=\"UC_R\"}</WrongMenu>"
            "<Unknown>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UNKNOWN\"}</Unknown>"
            "<MixedResources>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UNKNOWN\", Parameter_Type=\"MINE_E\", Parameter_Type=\"MINE_R\", Parameter_Type=\"MINE_U\"}</MixedResources>"
            "<Savings>(Function_Menu.Evaluate > 0) * (Variable_Self.CreditsUnnormalized < 1500)</Savings>"
            "</Root>"}}, converters);
        expect(static_cast<bool>(parsed), "WAS-10/12: resource and savings equations parse");
        if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value();
        ai::Engine engine(host, std::move(data), {});
        ai::EngineContractAccess::perception_executor(engine, &executor);
        const auto value = [&](const char* equation) {
            const auto journals = host->setup.journal->plans.size();
            const auto direct = engine.evaluate(equation, 2, nullptr);
            host->setup.journal->plans.resize(journals);
            ai::EngineContractAccess::prepare_pads(engine, executor);
            const auto prepared = engine.evaluate(equation, 2, nullptr);
            ai::EngineContractAccess::reset_pads(engine);
            expect(prepared == direct, "WAS-10/11: prepared pads preserve every live-state and typed-filter result");
            return prepared;
        };
        const auto foreign = [&] {
            ai::AiRandom random;
            return host->ai_world->perception(engine, *engine.data().equations.find("FOREIGNSTRUCTURES"),
                ai::Context{2, 1, {}, nullptr}, random);
        };
        auto& pads = const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(world.pads());
        pads.at(2).cooldown_until = 6;
        pads.at(6).cooldown_until = 1000;
        expect(value("Open") == ai::real(1) && value("Menu") == ai::real(1)
            && value("Multiple") == ai::real(1) && value("WrongMenu") == Real{},
            "WAS-11: eligible human ally matches faction menu once, hostile/neutral/cooldown pads do not");
        expect(value("Savings") == ai::real(1), "WAS-12: allied menu-capable open pad enables refinery savings");
        expect(value("AllStructures") == Real{}, "WAS-10: an unfiltered empty pad census is zero");
        expect(!foreign(), "WAS-10: foreign built-count context rejects an allied player too");
        host->setup.journal->plans.clear();
        expect(value("Unknown") == Real{}, "WAS-10: unknown resource types contribute zero without failing evaluation");
        expect(host->setup.journal->plans.size() == 1
            && host->setup.journal->plans.back().player == 2
            && host->setup.journal->plans.back().event == "error"
            && host->setup.journal->plans.back().detail == "TacticalBuiltStructureCount: unknown type UNKNOWN",
            "WAS-10: unknown type is reported through the existing AI journal");
        expect(value("MixedResources") == Real{}, "WAS-10: missing resource filters retain the known empty counts");
        ai::PlayerAi player; player.player = 2;
        for (const auto type : {30U, 31U, 32U}) {
            ai::Goal goal; goal.id = type; goal.potential = ai::PotentialPlan{};
            goal.potential->valid = true; goal.potential->sources = {2}; goal.potential->units = {type};
            goal.potential->producers = {1}; goal.potential->cost = ai::real(100);
            ai::EngineContractAccess::reserve(engine, player, goal);
            expect(value("AllStructures") == ai::real(1), "WAS-10: unfiltered pending reservation counts once");
            expect(value("Resources") == ai::real(1) && value("ResourceDesire") == ai::real(1),
                "WAS-10: every faction's pending constructor alias enables resource-gated desire before completion");
            expect(value("MixedResources") == ai::real(1), "WAS-10: an unknown filter does not discard a known pending resource");
            expect(value("Open") == Real{}, "WAS-11: pending reservation excludes the last eligible pad");
            ai::EngineContractAccess::release(engine, player, goal);
            expect(value("Resources") == Real{} && value("Menu") == ai::real(1),
                "WAS-10/11: abandonment releases typed and open-pad reservation inputs");
        }
        for (int frame = 0; frame < 5; ++frame) expect(world.step(executor).has_value(), "WAS-11: cooldown advances");
        refresh();
        expect(value("Open") == ai::real(1), "WAS-11: cooldown still excludes the pad before its deadline");
        expect(world.step(executor).has_value(), "WAS-11: cooldown reaches deadline");
        refresh();
        expect(value("Open") == ai::real(2), "WAS-11: cooldown deadline is inclusive");
        expect(world.submit({{6, 1, 0}, {1}, t::PadBuildPayload{30}}).has_value()
            && world.step(executor).has_value(), "WAS-10: human allied construction starts");
        refresh();
        expect(value("Menu") == ai::real(1), "WAS-11: actual construction excludes its pad");
        // WAS-10: the fallback tests the actual child's type when no reservation is stored.
        const auto child = world.pads().at(1).under_construction;
        expect(child != 0, "WAS-10: under-construction child is live");
        expect(value("AllStructures") == ai::real(1), "WAS-10: unfiltered census includes live construction");
        expect(value("Constructors") == ai::real(1) && value("Resources") == Real{},
            "WAS-10: an unreserved allied child matches its actual type, without a completed-type alias");
        for (int frame = 0; frame < 31; ++frame) expect(world.step(executor).has_value(), "WAS-10: mine completes");
        refresh();
        expect(value("Resources") == ai::real(1) && value("Menu") == ai::real(1),
            "WAS-10/11: completed human mine remains visible to allied AI and excludes open-pad count");
        expect(value("MixedResources") == ai::real(1), "WAS-10: an unknown filter does not discard a known completed resource");
        expect(value("AllStructures") == ai::real(1) && !foreign(),
            "WAS-10: unfiltered census includes completion and foreign context still rejects it");
        expect(world.submit({{world.completed_tick(), 2, 0}, {2}, t::PadBuildPayload{30}}).has_value()
            && world.step(executor).has_value(), "WAS-10: a second allied pad starts construction");
        refresh();
        ai::Goal pending; pending.id = 100; pending.potential = ai::PotentialPlan{};
        pending.potential->valid = true; pending.potential->sources = {2}; pending.potential->units = {32};
        pending.potential->producers = {6}; pending.potential->cost = ai::real(100);
        ai::EngineContractAccess::reserve(engine, player, pending);
        expect(world.pads().at(1).constructed != 0 && world.pads().at(2).under_construction != 0
            && value("Resources") == ai::real(2) && value("Constructors") == ai::real(1)
            && value("AllStructures") == ai::real(3)
            && value("AllStructures") == *value("Resources") + *value("Constructors"),
            "WAS-10: unfiltered completed, constructing and reserved pads equal the nonoverlapping typed sum");
        ai::EngineContractAccess::release(engine, player, pending);
        expect(value("AllStructures") == ai::real(2), "WAS-10: releasing one reservation retains both live children");
        expect(world.stage_remove(world.pads().at(2).under_construction).has_value(),
            "WAS-10: second construction destruction stages");
        refresh();
        expect(value("AllStructures") == ai::real(1), "WAS-10: destroying construction retains the completed child");
        expect(world.stage_remove(world.pads().at(1).constructed).has_value(), "WAS-10: completed mine destruction stages");
        refresh();
        expect(value("Resources") == Real{}, "WAS-10: destroyed mine cannot retain the resource gate");
        expect(value("AllStructures") == Real{}, "WAS-10: unfiltered census drops destroyed children");
        const auto hash = world.step(executor);
        expect(static_cast<bool>(hash), "WAS-10/11: post-destruction tick commits");
        if (hash) {
            const auto digest = hash.value().state_sha256;
            if (workers == 1) baseline.push_back(digest);
            else expect(digest == baseline.front(), "WAS-10/11: pad state hashes equal on 1/2/4/8 workers");
        }
    }
}

void spending_pad_refunds() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<std::string> baseline;
    std::vector<std::string> callback_baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
      {
        auto host = ai::make_tactical_host();
        ai::Engine engine(host, ai::AiData{}, {ai::PlanDef{}});
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, auth::ModuleManifest{});
        expect(scripts.has_value(), "WAS-26: station refusal interleaving scheduler starts");
        if (scripts) {
            std::uint64_t sequence = 0;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            auto& player = ai::EngineContractAccess::rejected_station_fixture(engine);
            ai::EngineContractAccess::direct_pad_service(engine, player);
            expect(player.tasks.size() == 1 && !engine.block(1)->finished,
                "WAS-26/WBP-47: pad-only service retains observed station refusal for ordinary execution");
            ai::EngineContractAccess::service(engine, player, 2);
            expect(player.tasks.empty() && engine.block(1)->finished && !engine.block(1)->result,
                "WAS-26: ordinary execution settles the retained station refusal block");
        }
      }
      for (const auto multiplier : {1, 2}) {
       for (const auto scenario : {0, 1, 2, 3, 4}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        t::TacticalSetup start; start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}, {3, 3, 300, 0}};
        start.units = {{1, 20, 1}};
        t::EconomyRules rules; rules.players = {{1, q(1000), 20}, {2, q(1000), 20, true}};
        rules.players[1].credit_multiplier = q(multiplier);
        rules.menus = {{20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(300), 30, 30, 0, true}}, 0, false}};
        rules.pads.capture = {{20, q(100), q(1), {100}, false, true, true}};
        rules.pads.neutral = 3;
        rules.pads.construction = {{30, 40, q(300), 100, 100}};
        rules.pads.influence = {{20, false}, {30, false}, {40, false}};
        t::DurabilityTable health;
        for (const auto type : {20U, 30U, 40U}) {
            if ((scenario == 1 || scenario == 3) && type == 30) continue; // Core refuses creation after AI precheck.
            t::DurabilityProfile row; row.type_id = type; row.max_hull = q(300); health.profiles.push_back(row);
        }
        auto made = t::TacticalSession::create(start, {}, health, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "WBP-47/48: pad refund fixture starts"); if (!made) continue;
        auto& world = made.value(); auto host = ai::make_tactical_host(); host->world = &world;
        foc::AiType constructor; constructor.type_id = 30; constructor.name = "UC"; constructor.tactical_cost = q(100);
        host->setup.content.types = {constructor}; host->types.emplace(30, &host->setup.content.types.front());
        host->types_by_name.emplace("UC", &host->setup.content.types.front());
        const auto refresh = [&] {
            host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot, &executor);
            host->credit_changes.clear();
        };
        expect(world.step(executor).has_value(), "WBP-47: initial barrier completes"); refresh();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"pad.xml", "<Root><Resource>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UC\"}</Resource></Root>"}}, converters);
        expect(static_cast<bool>(parsed), "WAS-10: refund fixture resource equation parses"); if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value();
        ai::Engine engine(host, std::move(data), {ai::PlanDef{}}); auto& player = ai::EngineContractAccess::add_player(engine, 2);
        auth::SessionConfig callback_config; callback_config.tick_duration = {1, 30};
        auto callbacks = auth::ScriptScheduler::create(callback_config, auth::ModuleManifest{});
        expect(static_cast<bool>(callbacks), "WBP-47/48: execution callback scheduler starts"); if (!callbacks) continue;
        ai::Goal goal; goal.id = 1; goal.potential = ai::PotentialPlan{};
        goal.potential->valid = true; goal.potential->cost = ai::real(300);
        goal.potential->sources = {2}; goal.potential->units = {30}; goal.potential->producers = {1};
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::bind_scripts(engine, callbacks.value(), sequence);
        const auto deliver = [&] {
            for (const auto& command : ai::EngineContractAccess::take_orders(engine)) {
                const auto order = foc::tactical_ai_detail::translate_order(command);
                expect(static_cast<bool>(order), "WBP-47/48: pad accounting command translates");
                if (order) expect(world.submit({{world.completed_tick(), order.value().issuer, sequence++},
                    order.value().units, order.value().payload}).has_value(), "WBP-47/48: pad accounting command submits");
            }
            expect(world.step(executor).has_value(), "WBP-47/48: pad accounting barrier commits"); refresh();
        };
        ai::EngineContractAccess::reserve(engine, player, goal); deliver();
        expect(engine.evaluate("Resource", 2, nullptr) == ai::real(1), "WAS-10: pad reservation enables the resource gate");
        if (scenario == 3 || scenario == 4) {
            host->engine = &engine;
            auto& installed = ai::EngineContractAccess::direct_pad_fixture(engine, 30, 1);
            auth::ModuleManifest manifest;
            expect(manifest.add("PAD.LUA", "refused = 0\nfunction Default_No_Units_Remaining(tf) refused = refused + 1 end\n"
                "function Request(tf,pad) tf.Build(\"UC\",pad) rest = tf.Build_All() end\n")
                .has_value(), "WBP-47: direct pad Lua loads");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
            expect(static_cast<bool>(scripts), "WBP-47: direct pad scheduler starts"); if (!scripts) continue;
            std::vector<eawr::core::Diagnostic> errors;
            ai::EngineContractAccess::bind_scripts(engine, scripts.value(), sequence);
            foc::detail::register_plan_bindings(scripts.value(), host, errors);
            expect(errors.empty() && scripts.value().create_instance(1, "PAD.LUA").has_value(), "WBP-47: direct pad binding registers");
            if (scenario == 4) {
                const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(world.pads()).at(1).cooldown_until = 100;
                ai::EngineContractAccess::pause_planning(engine);
                // The caller's event counter expires before after_service executes.
                const auto prepare = [&] {
                    std::uint64_t temporary_sequence = 7;
                    return engine.before_service(*host->ai_world, scripts.value(), temporary_sequence, &executor);
                };
                expect(prepare().has_value(), "WBP-47: direct refusal preparation completes before Lua service");
            }
            auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Request";
            event.arguments = {auth::Value{auth::Handle{ai::handle_taskforce, 1}}, auth::Value{auth::Handle{foc::handle_game_object, 1}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "WBP-47: direct pad call submits");
            auto report = scripts.value().service(executor);
            expect(report && engine.after_service(report.value()).has_value(), "WBP-47: direct pad call applies in its current barrier");
            if (!report) continue;
            if (scenario == 4) {
                expect(report.value().commands.empty(), "WBP-47: cooldown refuses the direct purchase");
                expect(ai::EngineContractAccess::attachment_events(engine).size() == 1,
                    "WBP-47: direct refusal queues its callback after preparation returned");
                const auto next_service = [&] {
                    std::uint64_t temporary_sequence = 7;
                    expect(engine.before_service(*host->ai_world, scripts.value(), temporary_sequence, &executor).has_value(),
                        "WBP-47: deferred callback receives a future tick and unique sequence");
                    expect(scripts.value().service(executor).has_value(), "WBP-47: deferred callback services");
                };
                next_service();
                next_service();
                const auto refused = scripts.value().read_global(1, "refused");
                expect(refused && refused.value() && std::get<Real>(refused.value()->data) == ai::real(1),
                    "WBP-47: direct refusal callback is delivered exactly once across service boundaries");
                const auto hash = scripts.value().state_hash();
                expect(hash.has_value(), "WBP-47: deferred callback state hashes");
                if (hash) {
                    if (workers == 1) callback_baseline.push_back(hash.value());
                    else expect(hash.value() == callback_baseline[static_cast<std::size_t>(multiplier - 1)],
                        "WBP-47: deferred callback hashes equal on 1/2/4/8 workers");
                }
                continue;
            }
            expect(report.value().commands.size() == 1 && report.value().commands.front().verb == foc::verb_pad_build
                && installed.tasks.size() == 1 && installed.tasks.front().issued == world.completed_tick(),
                "WBP-47: direct Lua Build runs precheck now and Build_All cannot duplicate the allocation");
            const auto rest = scripts.value().read_global(1, "rest");
            expect(rest && rest.value() && engine.block(std::get<auth::Handle>(rest.value()->data).id)->finished,
                "WBP-47: Build_All with no remaining allocation returns a completed block");
            ai::EngineContractAccess::return_orders(engine, report.value());
            ai::EngineContractAccess::bind_scripts(engine, callbacks.value(), sequence);
        } else {
            ai::EngineContractAccess::purchase(engine, player, goal, 30, 1);
            if (scenario == 0)
                const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(world.pads()).at(1).cooldown_until = 100;
            ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        }
        expect(player.reserved_credits.at(1) == ai::real(scenario == 0 ? 300 : 200),
            "WBP-47/48: only a passed precheck consumes the generic tactical cost");
        if (scenario == 0) expect(engine.plan(1)->reserved_pads == std::vector<eawr::sim::EntityId>{1},
            "WBP-48: early refusal retains the pad allocation for a later retry or plan release");
        deliver();
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(engine.evaluate("Resource", 2, nullptr) == ai::real(scenario == 1 || scenario == 3 ? 0 : 1),
            "WAS-10: Core no-child refusal clears its typed gate; pending and live child states keep it");
        if (scenario == 2) {
            const auto child = world.pads().at(1).under_construction;
            expect(child != 0 && world.submit({{world.completed_tick(), 2, sequence++}, {child},
                t::DamagePayload{q(1000), t::attack_hull}}).has_value(), "WBP-48: accepted child destruction submits");
            expect(world.step(executor).has_value(), "WBP-48: accepted child destruction commits"); refresh();
            expect(engine.evaluate("Resource", 2, nullptr) == Real{},
                "WAS-10: destroyed construction child clears the gate before the next execution service");
            ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        }
        const auto expected_immediate = 700 + (scenario == 1 || scenario == 3 ? 100 * multiplier : 0);
        expect(host->credits(2) == q(expected_immediate),
            "WBP-48: no-child Core refusal refunds once; early refusal and later child death do not");
        // A second observation of the same rejection must not repeat its direct refund.
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(host->credits(2) == q(expected_immediate), "WBP-48: repeated observation cannot duplicate a refund");
        ai::EngineContractAccess::release(engine, player, goal); deliver();
        expect(world.ledgers()[1].credits == q(700 + (scenario == 2 ? 200 : 300) * multiplier),
            "WBP-47/48: abandonment refunds only the unconsumed pad allocation through the positive-credit path");
        const auto result = world.step(executor);
        expect(static_cast<bool>(result), "WBP-47/48: final pad refund tick commits");
        if (result) {
            const auto index = static_cast<std::size_t>((multiplier - 1) * 4 + scenario);
            if (workers == 1) baseline.push_back(result.value().state_sha256);
            else expect(result.value().state_sha256 == baseline[index], "WBP-47/48: refund hashes equal on 1/2/4/8 workers");
        }
       }
      }
    }
}

void spending_wallet_reservations() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    std::vector<std::string> baseline;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
      for (const auto multiplier : {1, 2}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        t::TacticalSetup start; start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}};
        start.units = {{1, 10, 1}};
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20}, {2, q(1000), 20, true}};
        rules.players[1].credit_multiplier = q(multiplier);
        rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(300), 100, 100, 0, true}}}};
        rules.menus.front().options.front().requirements.current_allies = 1;
        rules.menus.front().options.push_back({51, t::BuildKind::unit, t::BuildQueue::units, q(300), 100, 100, 0, true});
        auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(static_cast<bool>(made), "WAS-25/26: reservation accounting fixture starts");
        if (!made) continue;
        auto& world = made.value();
        auto host = ai::make_tactical_host(); host->world = &world;
        foc::AiType station; station.type_id = 10; station.name = "STATION"; station.star_base = true;
        foc::AiType unit; unit.type_id = 50; unit.name = "SHIP"; unit.tactical_cost = q(200);
        host->setup.content.types = {station, unit};
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        const auto refresh = [&] {
            host->snapshot = world.snapshot(); host->view = foc::detail::build_view(world, *host->snapshot, &executor);
            host->credit_changes.clear();
        };
        expect(world.step(executor).has_value(), "WAS-25: first barrier completes"); refresh();
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"wallet.xml", "<Root><Wallet>Variable_Self.CreditsUnnormalized</Wallet></Root>"}}, converters);
        expect(static_cast<bool>(parsed), "WAS-05: wallet equation parses");
        if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value(); ai::Engine engine(host, std::move(data), {ai::PlanDef{}});
        auth::ModuleManifest manifest;
        expect(manifest.add("WALLET.LUA", "function Read(player) wallet = player.Get_Credits() end\n").has_value(), "WAS-05: wallet Lua loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        expect(static_cast<bool>(scripts), "WAS-05: wallet scheduler starts"); if (!scripts) continue;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "WALLET.LUA").has_value(), "WAS-05: player credit binding registers");
        std::uint64_t event_sequence = 0, command_sequence = 0;
        ai::EngineContractAccess::bind_scripts(engine, scripts.value(), event_sequence);
        const auto wallet = [&](const std::int64_t expected) {
            expect(engine.evaluate("Wallet", 2, nullptr) == ai::real(expected), "WAS-05/25: perception sees reservation-time wallet");
            auth::ScriptEvent event; event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, event_sequence++};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Read";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}}};
            expect(scripts.value().submit_event(std::move(event)).has_value() && scripts.value().service(executor).has_value(), "WAS-05: Lua wallet read runs");
            const auto value = scripts.value().read_global(1, "wallet");
            expect(value && value.value() && std::get<Real>(value.value()->data) == ai::real(expected), "WAS-05: Get_Credits sees the same immediate wallet");
        };
        t::TacticalReplay replay; replay.setup = start;
        const auto deliver = [&] {
            for (const auto& command : ai::EngineContractAccess::take_orders(engine)) {
                const auto order = foc::tactical_ai_detail::translate_order(command);
                expect(static_cast<bool>(order), "WAS-25/26: reservation/refund/prepaid command translates");
                if (!order) continue;
                t::PlayerCommand routed{{world.completed_tick(), order.value().issuer, command_sequence++}, order.value().units, order.value().payload};
                expect(world.submit(routed).has_value(), "WAS-25/26: deterministic accounting command submits"); replay.commands.push_back(routed);
            }
            expect(world.step(executor).has_value(), "WAS-25/26: accounting command commits"); refresh();
        };
        auto& player = ai::EngineContractAccess::add_player(engine, 2);
        ai::Goal goal; goal.id = 1; goal.potential = ai::PotentialPlan{}; goal.potential->valid = true; goal.potential->cost = ai::real(300);
        ai::Goal competing = goal; competing.id = 2; competing.potential->cost = ai::real(100);
        wallet(1000);
        ai::EngineContractAccess::reserve(engine, player, goal); wallet(700);
        ai::EngineContractAccess::reserve(engine, player, goal); wallet(700);
        ai::EngineContractAccess::reserve(engine, player, competing); wallet(600);
        expect(goal.potential->cost == Real{} && player.reserved_credits.at(1) == ai::real(300), "WAS-25: pending cost clears and remainder persists");
        ai::EngineContractAccess::release(engine, player, competing, false); competing.potential->reserved = false;
        ai::EngineContractAccess::reserve(engine, player, competing); wallet(600);
        deliver(); wallet(600);
        ai::EngineContractAccess::release(engine, player, competing); wallet(600 + 100 * multiplier);
        ai::EngineContractAccess::release(engine, player, competing); wallet(600 + 100 * multiplier);
        deliver();
        ai::EngineContractAccess::purchase(engine, player, goal, 50, 1);
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        deliver();
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(player.reserved_credits.at(1) == ai::real(100), "WAS-26: accepted effective-price purchase consumes generic cost only");
        wallet(600 + 100 * multiplier);
        expect(world.ledgers()[1].queues[0].size() == 1 && world.ledgers()[1].queues[0].front().paid == q(300),
            "WAS-26: prepaid queue entry keeps its ordinary cancellation price");
        const t::PlayerCommand cancel{{world.completed_tick(), 2, command_sequence++}, {}, t::CancelPayload{0, 0}};
        replay.commands.push_back(cancel);
        expect(world.submit(cancel).has_value()
            && world.step(executor).has_value(), "WPR-31: started queue cancellation commits"); refresh();
        wallet(600 + 400 * multiplier);
        ai::EngineContractAccess::release(engine, player, goal); wallet(600 + 500 * multiplier); deliver();
        wallet(600 + 500 * multiplier);
        const auto prior = 600 + 500 * multiplier;
        ai::Goal partial; partial.id = 3; partial.potential = ai::PotentialPlan{};
        partial.potential->valid = true; partial.potential->cost = ai::real(600);
        ai::EngineContractAccess::reserve(engine, player, partial); deliver();
        ai::EngineContractAccess::purchase(engine, player, partial, 50, 1);
        ai::EngineContractAccess::purchase(engine, player, partial, 50, 1);
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        deliver();
        ai::EngineContractAccess::service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
        expect(world.ledgers()[1].queues[0].size() == 1 && player.reserved_credits.at(3) == ai::real(400),
            "WAS-26: partial start consumes one generic allocation; refused shared-limit work stays refundable");
        ai::EngineContractAccess::abandon_goal(engine, player, 3, scripts.value());
        wallet(prior - 600 + 400 * multiplier); deliver();
        expect(!player.reserved_credits.contains(3), "WAS-26: plan abandonment returns the unused remainder once");
        const auto human_tick = world.completed_tick();
        for (const auto& payload : {t::CommandPayload{t::AiReservationDebitPayload{q(100)}},
                t::CommandPayload{t::BuyPayload{51, true}}}) {
            const t::PlayerCommand control{{human_tick, 1, command_sequence++},
                std::holds_alternative<t::BuyPayload>(payload) ? std::vector<eawr::sim::EntityId>{1} : std::vector<eawr::sim::EntityId>{}, payload};
            expect(world.submit(control).has_value(), "WAS-25/26: human account control submits"); replay.commands.push_back(control);
        }
        const t::PlayerCommand too_large{{human_tick, 2, command_sequence++}, {}, t::AiReservationDebitPayload{q(5000)}};
        expect(world.submit(too_large).has_value(), "WAS-25: unaffordable reservation debit submits"); replay.commands.push_back(too_large);
        expect(world.step(executor).has_value() && world.ledgers()[0].credits == q(1000)
            && world.ledgers()[0].queues[0].empty() && world.ledgers()[1].credits == q(prior - 600 + 400 * multiplier),
            "WAS-25/26: human accounts cannot debit AI reservations or use prepaid buys"); refresh();
        replay.final_tick_count = world.completed_tick();
        const auto bytes = t::write_replay(replay);
        expect(static_cast<bool>(bytes), "WAS-25/26: new opcodes encode");
        if (bytes) {
            const auto decoded = t::parse_replay(bytes.value());
            expect(decoded && decoded.value() == replay, "WAS-25/26: reservation/prepaid opcodes round-trip exactly");
        }
        const auto result = world.step(executor);
        expect(static_cast<bool>(result), "WAS-25/26: final accounting tick commits");
        if (result) {
            if (workers == 1) baseline.push_back(result.value().state_sha256);
            else expect(result.value().state_sha256 == baseline[static_cast<std::size_t>(multiplier - 1)], "WAS-25/26: wallet/queue hashes equal on 1/2/4/8 workers");
        }
      }
    }
}

void allied_resource_upgrade_selection() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 1, 100, 1}, {3, 1, 100, 1}, {4, 2, 200, 1}};
    start.units = {{1, 10, 1}, {2, 20, 1}, {3, 20, 4}, {6, 21, 1}, {7, 21, 4}};
    t::EconomyRules rules;
    for (const auto player : {1U, 2U, 3U, 4U}) rules.players.push_back({player, q(1000), 20, false, {}, 1, 5});
    t::BuildOption option{50, t::BuildKind::upgrade, t::BuildQueue::upgrades, q(100), 10, 10, 0, true};
    option.requirements.current_allies = 1;
    option.requirements.prerequisites = {20};
    rules.menus = {{10, 100, {option, {20, t::BuildKind::unit, t::BuildQueue::units, q(100), 50, 50, 0, true}}}};
    rules.upgrades = {{50, false, true, 0, {}}};
    rules.pads.capture = {{21, q(100), q(1), {100, 200}, false, true, true}};
    rules.pads.neutral = 4;
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-12: human owns the shared station and resource structure");
    if (!made) return;
    auto& placed_pads = const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(made.value().pads());
    placed_pads.at(6).constructed = 2; placed_pads.at(7).constructed = 3;
    auto host = ai::make_tactical_host();
    host->setup.perception.campaign_game = false;
    foc::AiType station; station.type_id = 10; station.name = "STATION"; station.star_base = true; station.base_level = 1;
    foc::AiType mine; mine.type_id = 20; mine.name = "MINE";
    foc::AiType upgrade; upgrade.type_id = 50; upgrade.name = "RESEARCH"; upgrade.category_bits = 1;
    host->setup.content.types = {station, mine, upgrade};
    for (const auto& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    host->world = &made.value();
    const auto refresh = [&] {
        host->snapshot = made.value().snapshot();
        host->view = foc::detail::build_view(made.value(), *host->snapshot);
    };
    refresh();
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
    auto equations = ai::EquationSet::parse({{"resources.xml", "<Root>"
        "<Resources>Variable_Self.TacticalBuiltStructureCount{Parameter_Type = \"MINE\"}</Resources>"
        "<Desire>Function_Resources.Evaluate > 0</Desire></Root>"}}, converters);
    expect(static_cast<bool>(equations), "SAE-12: resource-gated upgrade equation parses");
    if (!equations) return;
    ai::AiData data; data.equations = std::move(equations).value();
    ai::PlanDef plan; plan.ignore_target = true; plan.allow_free_store = false; plan.required_categories = {1};
    ai::TaskForceDef force;
    ai::TeamDef team; team.types = {50}; team.min_count = team.max_count = 1;
    force.teams = {team}; plan.taskforces = {force};
    ai::Engine engine(host, std::move(data), {plan});
    for (const auto player_id : {2U, 3U}) {
        expect(made.value().production_counts(player_id, 20).owned_player == 0,
            "SAE-12: AI has no own resource structure");
        expect(engine.evaluate("Resources", player_id, nullptr) == ai::real(1)
            && engine.evaluate("Desire", player_id, nullptr) == ai::real(1),
            "SAE-12: human's resource enables both allied AI upgrade desires");
        ai::PlayerAi player; player.player = player_id;
        ai::PotentialPlan potential;
        expect(ai::EngineContractAccess::selection(engine, player, potential, 7)
            && potential.units == std::vector<t::TypeId>{50} && potential.sources == std::vector<std::uint8_t>{2},
            "SAE-12: each AI can select research from the human's shared station");
    }
    eawr::sim::InlineExecutor executor;
    expect(made.value().submit({{0, 2, 0}, {1}, t::BuyPayload{50}}).has_value()
        && made.value().step(executor).has_value(), "SAE-12: AI research starts normally");
    refresh();
    expect(!made.value().build_allowed(3, 1, 50), "WPR-33: teammate reserves the research slot");
    expect(made.value().submit({{1, 2, 1}, {}, t::CancelPayload{1, 0}}).has_value()
        && made.value().step(executor).has_value(), "WPR-31: AI cancels research");
    refresh();
    expect(made.value().build_allowed(3, 1, 50), "WPR-33: cancellation releases the team slot");
    expect(made.value().submit({{2, 3, 0}, {1}, t::BuyPayload{50}}).has_value(), "SAE-12: other AI purchases research");
    for (int tick = 0; tick < 12; ++tick) expect(made.value().step(executor).has_value(), "SAE-12: research completes");
    for (const auto& account : made.value().ledgers())
        expect(account.tech_level == (account.player == 4 ? 1U : 2U), "WPR-22: AI research advances the human and both allies only");
    expect(made.value().stage_remove(2).has_value(), "SAE-12: human's last mine is lost");
    expect(engine.evaluate("Resources", 2, nullptr) == Real{} && engine.evaluate("Desire", 2, nullptr) == Real{},
        "SAE-12: enemy mine cannot keep the allied upgrade desire enabled");
    const auto counted = made.value().production_counts(2, 50);
    expect(counted.owned_allies == 1 && counted.owned_player == 0,
        "SAE-12: held research ownership is queryable independently of the buyer");
    expect(made.value().submit({{14, 2, 2}, {1}, t::BuyPayload{20}}).has_value()
        && made.value().step(executor).has_value(), "SAE-12: queue a type used by the resource filter");
    expect(made.value().production_counts(2, 20).queued_allies == 1
        && engine.evaluate("Resources", 2, nullptr) == Real{}, "SAE-12: queue entries are not completed resource structures");
    for (int tick = 0; tick < 52; ++tick) expect(made.value().step(executor).has_value(), "SAE-12: purchase reaches pool");
    expect(made.value().production_counts(2, 20).current_allies == 1
        && made.value().production_counts(2, 20).owned_allies == 0
        && engine.evaluate("Resources", 2, nullptr) == Real{}, "SAE-12: undeployed pool entries are not completed resource structures");

    auto neutral_start = start;
    neutral_start.players.push_back({5, 1, 300, 0});
    neutral_start.units.push_back({4, 20, 5});
    neutral_start.units.push_back({8, 21, 5});
    std::sort(neutral_start.units.begin(), neutral_start.units.end(), [](const auto& a, const auto& b) {
        return a.entity_id < b.entity_id;
    });
    t::CombatTable neutral_rules;
    neutral_rules.pad_neutral_factions = {300};
    auto neutral_world = t::TacticalSession::create(neutral_start, {}, {}, {}, {}, neutral_rules, {}, {}, rules);
    expect(static_cast<bool>(neutral_world), "SAE-12: neutral owner may share an ally's raw team number");
    if (neutral_world) {
        auto& neutral_pads = const_cast<std::map<eawr::sim::EntityId, t::PadState>&>(neutral_world.value().pads());
        neutral_pads.at(6).constructed = 2; neutral_pads.at(7).constructed = 3; neutral_pads.at(8).constructed = 4;
        const foc::AiPlayer neutral_player{5, "NEUTRAL", true, false, ""};
        host->players.emplace(5, &neutral_player);
        host->world = &neutral_world.value();
        host->snapshot = neutral_world.value().snapshot();
        host->view = foc::detail::build_view(neutral_world.value(), *host->snapshot);
        expect(host->neutral(5), "SAE-12: neutral relationship is also bound in the AI host");
        expect(engine.evaluate("Resources", 2, nullptr) == ai::real(1),
            "SAE-12: bound neutral owner is excluded despite its matching team number");
        expect(neutral_world.value().production_counts(5, 20).owned_allies == 0,
            "SAE-12: neutral buyers have no allied resource view");
        expect(neutral_world.value().stage_remove(2).has_value(), "SAE-12: lose the only non-neutral allied mine");
        expect(engine.evaluate("Resources", 2, nullptr) == Real{} && engine.evaluate("Desire", 2, nullptr) == Real{},
            "SAE-12: a neutral same-team mine cannot enable upgrade desires");
    }
}

void completed_purchases_are_available() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}};
    start.units = {{1, 10, 1}};
    t::EconomyRules rules;
    rules.players = {{1, q(1000), 20}};
    rules.menus = {{10, 100, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 3, 3, 1, true},
                            {51, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 1, true}}}};
    auto made = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
    expect(static_cast<bool>(made), "SAE-03: shared production pool fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    host->setup.perception.campaign_game = false;
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    foc::AiType factory;
    factory.type_id = 10; factory.star_base = true;
    host->types.emplace(10, &factory);
    ai::PlanDef definition;
    definition.allow_free_store = true;
    definition.ignore_target = true;
    ai::TaskForceDef force;
    ai::TeamDef team;
    team.min_count = team.max_count = 1;
    team.types = {50};
    force.teams = {team};
    definition.taskforces = {force};
    ai::Engine engine(host, {}, {definition});
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
    expect(static_cast<bool>(scripts), "SAE-03: shared production scheduler starts");
    if (!scripts) return;
    auto& player = ai::EngineContractAccess::add_player(engine, 1);
    ai::EngineContractAccess::seed_purchases(engine, player, scripts.value());
    ai::EngineContractAccess::service(engine, player, 1);
    expect(ai::EngineContractAccess::orders(engine) == 2, "SAE-03: both build tasks issue purchases");
    expect(made.value().submit({{0, 1, 0}, {1}, t::BuyPayload{50}}).has_value()
        && made.value().submit({{0, 1, 1}, {1}, t::BuyPayload{51}}).has_value(),
        "SAE-03: purchases enter the ordinary queue");
    eawr::sim::InlineExecutor executor;
    for (int frame = 0; frame < 5; ++frame)
        expect(made.value().step(executor).has_value(), "SAE-03: first purchase completes before the second");
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    ai::EngineContractAccess::service(engine, player, 6);
    expect(player.tasks.size() == 1 && player.tasks.front().type == 51 && !engine.block(1)->finished,
        "SAE-03: the producing block still waits for its later purchase");
    expect(engine.taskforce(1)->pooled.empty() && player.reserved_pool_tokens.empty()
        && player.reserved_pool.empty(), "SAE-03: new production does not reserve or attach the completed pool entry");
    ai::PotentialPlan potential;
    expect(ai::EngineContractAccess::selection(engine, player, potential, 67, 2)
        && potential.units == std::vector<t::TypeId>{50} && potential.sources == std::vector<std::uint8_t>{1}
        && potential.pool_tokens.size() == 1 && potential.pool_tokens.front() != 0,
        "SAE-03: another goal selects the completed purchase while production remains blocked");
    if (!potential.pool_tokens.empty()) {
        player.reserved_pool_tokens[potential.pool_tokens.front()] = 3;
        ++player.reserved_pool[50];
        expect(!ai::EngineContractAccess::selection(engine, player, potential, 67, 2),
            "SAE-03/11: a subsequently reserved pool entry remains unavailable to another goal");
    }
}

void reinforcement_work_budget() {
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const m::Vec3 requested{q(-1000), q(50), q(8)};
    expect(ai::reinforcement_candidate(requested, 0) == requested, "SAE-10: radius zero tests the requested point");
    const auto first = ai::reinforcement_candidate(requested, 1);
    const auto second = ai::reinforcement_candidate(requested, 2);
    const auto next = ai::reinforcement_candidate(requested, 11);
    expect(first && first->x == q(-1500) && first->y == q(50), "SAE-10: first angle is reverse arrival facing at radius 500");
    expect(first && second && second->x > first->x && second->y < first->y, "SAE-10: second angle rotates positive 36 degrees");
    expect(next && next->x == q(-2000) && next->y == q(50), "SAE-10: next ring adds 500 and restarts its angle");
    expect(ai::reinforcement_candidate(requested, 211).has_value(), "SAE-10: search continues beyond the former 10000-unit cutoff");
    for (std::uint32_t attempt = 0; attempt < 221; ++attempt) {
        const auto candidate = ai::reinforcement_candidate(requested, attempt);
        expect(candidate && candidate->z == requested.z, "SAE-10: candidate geometry preserves the requested plane");
    }
    const std::array bounds{q(-2000), q(-2000), q(2000), q(2000)};
    const auto clamped = ai::reinforcement_candidate(requested, 81, {}, bounds);
    expect(clamped && clamped->x == q(-2000), "SAE-10: candidate clamps to playable bounds");
    const auto rotated = ai::reinforcement_candidate({}, 11, q(90));
    expect(rotated && rotated->x == q(0) && rotated->y == q(-1000), "SAE-10: ring origin follows authored arrival yaw");
}

void duplicate_reinforcement_calls() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, t::player_flag_commandable}};
    t::EconomyRules economy;
    economy.players = {{1, {}, 20}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(static_cast<bool>(made), "SAE-11: duplicate-call fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    host->view = foc::detail::build_view(made.value(), *host->snapshot);
    ai::Engine engine(host, {}, {ai::PlanDef{}});
    auto& player = ai::EngineContractAccess::duplicate_fixture(engine);
    auth::ServiceReport report;
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        auth::ScriptCommand call;
        call.issuer = 1;
        call.sequence = sequence;
        call.verb = std::string(ai::verb_ai);
        call.arguments = {auth::Value{std::string("reinforce")},
            auth::Value{auth::Handle{ai::handle_taskforce, 1}}, foc::detail::position_value({})};
        report.commands.push_back(std::move(call));
    }
    expect(static_cast<bool>(engine.after_service(report)), "SAE-11: repeated Reinforce calls are taken");
    const auto* first = engine.block(ai::block_id(1, 1));
    const auto* second = engine.block(ai::block_id(1, 2));
    expect(first != nullptr && first == second && !first->finished,
        "SAE-11: both Lua handles wait on the same unfinished operation");
    const auto& orders = ai::EngineContractAccess::reinforce_service(engine, player, 1);
    expect(orders.size() == 1, "SAE-11: repeated calls emit exactly one request for the reserved purchase");
    if (orders.size() == 1) {
        const auto* token = std::get_if<auth::Handle>(&orders[0].arguments[4].data);
        expect(token != nullptr && token->id == 1, "SAE-11: request carries the first reservation's identity");
    }
    expect(player.reserved_pool_tokens.size() == 2 && engine.taskforce(1)->pooled.size() == 2,
        "SAE-11: submission keeps both reservations until authoritative admission");
    auto view = std::make_shared<foc::detail::WorldView>(*host->view);
    foc::detail::ViewUnit converted;
    converted.id = 50;
    converted.owner = 1;
    converted.type = 99;
    converted.purchase_type = 1;
    converted.purchase_token = 1;
    view->units.push_back(converted);
    host->view = view;
    const auto& next = ai::EngineContractAccess::reinforce_service(engine, player, 2);
    expect(engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{50}
        && engine.taskforce(1)->pooled.size() == 1 && !player.reserved_pool_tokens.contains(1),
        "SAE-11/WHE-49: a converted hull joins by logical purchase type and releases its exact reservation");
    expect(next.size() == 1 && engine.block(ai::block_id(1, 2)) == first && !first->finished,
        "SAE-11: both callers continue waiting while the second purchase reinforces");
    converted.id = 51;
    converted.purchase_token = 2;
    view->units.push_back(converted);
    const auto& done = ai::EngineContractAccess::reinforce_service(engine, player, 3);
    expect(done.empty() && first->finished && first->result && player.reserved_pool_tokens.empty()
        && engine.taskforce(1)->pooled.empty() && engine.taskforce(1)->units.size() == 2,
        "SAE-11: both handles finish together after each reserved purchase joins once");
}

void taskforce_attachment_waits_for_arrival() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    std::optional<std::uint64_t> baseline;
    for (const bool abandoned : {false, true}) {
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, t::player_flag_commandable}};
            setup.units = {{1, 10, 1}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
            expect(made.has_value(), "EX-13: arrival attachment fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "EX-13: buy reserved pool entry");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "EX-13: purchase reaches pool");
            auto host = ai::make_tactical_host();
            host->world = &world;
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            foc::AiType type;
            type.type_id = 1;
            type.locomotor = true;
            host->types.emplace(1, &type);
            ai::Engine engine(host, {}, {ai::PlanDef{}});
            auto& player = ai::EngineContractAccess::duplicate_fixture(engine);
            auth::ServiceReport report;
            auth::ScriptCommand call;
            call.issuer = call.sequence = 1;
            call.verb = std::string(ai::verb_ai);
            call.arguments = {auth::Value{std::string("reinforce")},
                auth::Value{auth::Handle{ai::handle_taskforce, 1}}, foc::detail::position_value({})};
            report.commands.push_back(std::move(call));
            expect(engine.after_service(report).has_value(), "EX-13: stage reinforcement block");
            const auto& requests = ai::EngineContractAccess::reinforce_service(engine, player, 2);
            expect(requests.size() == 1, "EX-13: submit one reserved purchase");
            const auto block_id = ai::block_id(1, 1);
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 1}, {}, t::ReinforcePayload{1, {}, 1}},
                {block_id, 0}).has_value(), "EX-13: authoritative reinforcement admission retains the reserved purchase token");
            expect(world.step(executor).has_value() && !world.arrivals().empty(), "EX-13: ship exists during hyperspace");
            const auto arrival_tick = world.completed_tick();
            const auto incoming = world.arrivals().begin()->first;
            if (abandoned) {
                auth::SessionConfig config;
                config.tick_duration = auth::TickDuration{1, 30};
                auto scripts = auth::ScriptScheduler::create(std::move(config), auth::ModuleManifest{});
                expect(scripts.has_value(), "EX-13: abandoned-plan scheduler starts");
                if (!scripts) continue;
                ai::EngineContractAccess::abandon(engine, player, scripts.value());
                expect(engine.taskforce(1) == nullptr && player.reserved_pool_tokens.empty(),
                    "EX-13: plan exit releases its reservation while the ship is still incoming");
            }
            while (!world.arrivals().empty() && world.completed_tick() < arrival_tick + t::arrival_frames + 1) {
                host->snapshot = world.snapshot();
                host->view = foc::detail::build_view(world, *host->snapshot);
                const auto& pending = ai::EngineContractAccess::reinforce_service(engine, player,
                    static_cast<std::int64_t>(world.completed_tick()));
                expect(pending.empty(), "EX-13: no duplicate request during hyperspace");
                if (!abandoned) {
                    expect(engine.taskforce(1)->units.empty() && player.reserved_pool_tokens.contains(1),
                        "EX-13: incoming ship stays unattached and reserved");
                    expect(!engine.block(block_id)->finished, "EX-13: block waits while ship is incoming");
                }
                const auto* unit = host->view->find(incoming);
                expect(unit != nullptr && !ai::EngineContractAccess::in_freestore(engine, *unit),
                    "EX-13: incoming ship is unavailable to freestore even after its plan releases the reservation");
                expect(world.step(executor).has_value(), "EX-13: hyperspace advances");
            }
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            ai::EngineContractAccess::reinforce_service(engine, player, static_cast<std::int64_t>(world.completed_tick()));
            if (abandoned) {
                const auto* unit = host->view->find(incoming);
                expect(world.arrivals().empty() && unit != nullptr && ai::EngineContractAccess::in_freestore(engine, *unit),
                    "EX-13: unload exposes an abandoned plan's incoming ship to freestore");
            } else {
                expect(world.arrivals().empty() && engine.taskforce(1)->units.size() == 1
                    && !player.reserved_pool_tokens.contains(1) && engine.taskforce(1)->pooled.size() == 1,
                    "EX-13: unload attaches the exact ship once and releases only its reservation");
            }
            if (!baseline) baseline = world.completed_tick();
            else expect(world.completed_tick() == *baseline, "EX-13: attachment tick agrees on 1/2/4/8 workers");
        }
    }
}

void concurrent_reserved_arrivals() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, t::player_flag_commandable}};
    t::EconomyRules economy;
    economy.players = {{1, {}, 20}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(static_cast<bool>(made), "SAE-11: concurrent-arrival fixture starts");
    if (!made) return;
    auto host = ai::make_tactical_host();
    host->world = &made.value();
    host->snapshot = made.value().snapshot();
    auto view = std::make_shared<ai::WorldView>();
    host->view = view;
    foc::AiType type;
    type.type_id = 1;
    type.locomotor = true;
    host->types.emplace(1, &type);
    ai::Engine engine(host, {}, {ai::PlanDef{}});
    auto& player = ai::EngineContractAccess::concurrent_fixture(engine);
    const auto& requested = ai::EngineContractAccess::reinforce_service(engine, player, 1);
    expect(requested.size() == 2, "SAE-11: distinct TaskForces may request independent purchases together");
    // Admission order differs from block order; identical type and ascending entity IDs
    // cannot identify the reserved TaskForce. The admitted purchase token can.
    ai::ViewUnit second;
    second.id = 50;
    second.owner = 1;
    second.type = second.purchase_type = 1;
    second.purchase_token = 3;
    auto first = second;
    first.id = 51;
    first.purchase_token = 1;
    view->units = {second, first};
    expect(!ai::EngineContractAccess::in_freestore(engine, first)
        && !ai::EngineContractAccess::in_freestore(engine, second),
        "SAE-11: admitted purchases remain reserved before their TaskForce attachment service");
    auto unreserved = first;
    unreserved.purchase_token = 999;
    expect(ai::EngineContractAccess::in_freestore(engine, unreserved),
        "SAE-11: a purchase token without a live reservation does not exclude a unit from freestore");
    ai::EngineContractAccess::reinforce_service(engine, player, 2);
    expect(engine.taskforce(1)->units == std::vector<eawr::sim::EntityId>{51}
        && engine.taskforce(2)->units == std::vector<eawr::sim::EntityId>{50},
        "SAE-11: inverted same-type arrivals join their exact reserved TaskForce");
    expect(player.reserved_pool_tokens.size() == 2 && player.reserved_pool_tokens.contains(2)
        && player.reserved_pool_tokens.contains(4),
        "SAE-11: each arrival releases only its own reserved purchase");
}

void reinforcement_service_budget() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    // The blocked-ring and second-angle cases exercise prevention and shared static collision.
    for (const auto radius : {750, 1250}) {
        std::vector<std::string> baseline;
        for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
            setup.units = {{1, 10, 1, {q(4000), {}, {}}}, {2, 20, 2},
                {3, 30, 1, {q(-1000), {}, {}}}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}, {2, {}, 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            economy.prevention = {{20, q(radius)}};
            t::MotionTable motion;
            motion.avoidance = t::AvoidanceRules{q(24), q(1), q(100), q(1), q(15), q(1), q(1), q(1), q(1), q(1), q(1),
                3500, 6, 90, 45, q(50)};
            motion.footprints = {{1, t::SpaceLayer::corvette, q(10), q(20), q(20), false},
                {30, t::SpaceLayer::static_object, q(100), q(100), q(100), true}};
            auto made = t::TacticalSession::create(setup, {}, {}, motion, {}, {}, {}, {}, economy);
            expect(static_cast<bool>(made), "SAE-10: ring fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: ring fixture buys pooled unit");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: purchase reaches pool before search");
            const auto before_search = world.record().commands.size();
            const auto wallet = world.ledgers().front().credits;
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 1}, {}, t::ReinforcePayload{1, {}}}, {7, 0}).has_value(),
                "SAE-10: submit initial whole-ring search");
            const auto first = world.step(executor);
            expect(static_cast<bool>(first), "SAE-10: whole ring executes in one tick");
            if (!first) continue;
            const auto result = world.reinforcement_search_result(1, 7);
            expect(result.has_value(), "SAE-10: ring result returns to execution service");
            if (!result) continue;
            expect(result->predictions == 0, "SAE-10: ring queries materialize no private prediction samples");
            std::vector<std::string> trace{first.value().state_sha256};
            if (radius == 1250) {
                expect(!result->valid && result->candidates == 10 && result->next_attempt == 21,
                    "SAE-10: blocked radius-1000 ring tests all ten angles in one service and advances to 1500");
                expect(std::none_of(first.value().snapshot->events().begin(), first.value().snapshot->events().end(),
                    [](const auto& event) { return event.kind == t::EventKind::order_rejected
                        || event.kind == t::EventKind::order_accepted; }),
                    "SAE-10: a blocked search does not submit an ordinary command");
                expect(world.record().commands.size() == before_search,
                    "SAE-10: a blocked search is not replay-recorded");
                expect(world.ledgers().front().credits == wallet
                    && world.ledgers().front().pool == std::vector<t::TypeId>{1},
                    "SAE-10: a blocked search consumes neither credits nor its pooled unit");
                auto blocked = world.reinforcement_point(1, 1, result->position);
                expect(blocked && !blocked.value(), "WR-22: the shared placement predicate rejects the blocked ring point");
                expect(first.value().reinforcement_rejections_by_check[1] >= 10,
                    "SAE-10: every prevention-blocked angle gets a placement verdict");
                expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 2}, {}, t::ReinforcePayload{1, {}}}, {7, result->next_attempt}).has_value(),
                    "SAE-10: next service submits next ring");
                const auto second = world.step(executor);
                expect(second.has_value(), "SAE-10: next radius executes");
                if (!second) continue;
                const auto free = world.reinforcement_search_result(1, 7);
                expect(free && free->valid && free->candidates == 1 && free->position == *ai::reinforcement_candidate({}, 21),
                    "SAE-10: radius 1500 restarts at reverse facing and admits promptly");
                trace.push_back(second.value().state_sha256);
            } else {
                expect(result->valid && result->candidates == 2 && result->position == *ai::reinforcement_candidate({}, 12),
                    "SAE-10: prevention overlap skips requested/500 points and static first-angle blocker selects second angle at 1000");
                expect(first.value().reinforcement_collision_queries >= 2,
                    "SAE-10: shared collision views check the ring and ordinary command admission");
            }
            const auto replay = world.record();
            const auto bytes = t::write_replay(replay);
            expect(bytes.has_value(), "SAE-10: resolved command uses the existing replay format");
            if (bytes) {
                const auto decoded = t::parse_replay(bytes.value());
                expect(decoded && decoded.value() == replay, "SAE-10: resolved ordinary replay round-trips without search metadata");
            }
            expect(std::holds_alternative<t::ReinforcePayload>(replay.commands.back().payload),
                "SAE-10: record contains the ordinary resolved reinforcement");
            auto replayed = t::TacticalSession::create(setup, {}, {}, motion, {}, {}, {}, {}, economy);
            expect(replayed.has_value(), "SAE-10: replay fixture starts");
            if (replayed) {
                for (const auto& command : replay.commands) expect(replayed.value().submit(command).has_value(), "SAE-10: resolved commands replay");
                for (std::uint64_t tick = 0; tick < replay.final_tick_count; ++tick) expect(replayed.value().step(executor).has_value(), "SAE-10: ordinary replay advances");
                expect(replayed.value().snapshot()->canonical_bytes() == world.snapshot()->canonical_bytes(),
                    "SAE-10: ordinary resolved command replay reproduces the searched world");
            }
            if (workers == 1) baseline = trace;
            else expect(trace == baseline, "SAE-10: ring search and world hashes agree on 1/2/4/8 workers");
            expect(world.submit_reinforcement_search({{world.completed_tick(), 1, 10}, {}, t::ReinforcePayload{1, {}}},
                {9, 21}).has_value(), "SAE-10: no-pool search uses ordinary admission");
            const auto empty_pool = world.step(executor);
            const auto held = world.reinforcement_search_result(1, 9);
            expect(empty_pool && held && held->candidates == 0 && held->next_attempt == 21
                && std::none_of(empty_pool.value().snapshot->events().begin(), empty_pool.value().snapshot->events().end(),
                    [](const auto& event) { return event.kind == t::EventKind::order_rejected
                        || event.kind == t::EventKind::order_accepted; }),
                "SAE-03/10: absent pool entry does no ring work and retains its radius");
        }
    }
}

void reinforcement_gravity_well() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const auto decimal = [](std::string_view value) { return m::Fixed::from_decimal(value).value(); };
    // SAE-10: two prevention-blocked points from the lone-squadron economy probe.
    // A synthetic neutral well at the origin uses the authored 2200-unit prevention radius.
    const std::array<m::Vec3, 2> points{{{decimal("-795.585"), decimal("-427.722"), {}},
        {decimal("-600.219"), decimal("-887.974"), {}}}};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        std::vector<std::string> trace;
        for (const auto& requested : points) {
            t::TacticalSetup setup;
            setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
            setup.units = {{1, 10, 1, {q(4000), {}, {}}}, {2, 20, 2}};
            t::EconomyRules economy;
            economy.players = {{1, q(1000), 20}, {2, {}, 20}};
            economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
            economy.prevention = {{20, q(2200)}};
            auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
            expect(made.has_value(), "SAE-10: blocked gravity-well fixture starts");
            if (!made) continue;
            auto& world = made.value();
            eawr::platform::ThreadWorkerAdapter executor(workers);
            expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: gravity fixture purchases its pool entry");
            expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: gravity purchase reaches pool before search");
            auto host = ai::make_tactical_host();
            host->world = &world;
            host->snapshot = world.snapshot();
            host->view = foc::detail::build_view(world, *host->snapshot);
            ai::Engine engine(host, {}, {});
            auto& player = ai::EngineContractAccess::add_player(engine, 1);
            ai::EngineContractAccess::seed_reinforcements(engine, player, 1, true);
            ai::EngineContractAccess::reinforce_at(engine, requested);
            const auto block_token = ai::block_id(ai::first_plan_instance + 1, 17);
            std::uint32_t attempts = 0;
            for (std::int64_t service = 1; service <= 4 && world.arrivals().empty(); ++service) {
                const auto& commands = ai::EngineContractAccess::reinforce_service(engine, player, service);
                expect(commands.size() == 1 && commands.front().arguments.size() == 7,
                    "SAE-10: AI emits one ring request for the blocked pool entry");
                if (commands.empty()) break;
                const auto translated = foc::tactical_ai_detail::translate_order(commands.front());
                expect(translated && translated.value().reinforcement_search
                    && translated.value().reinforcement_search->token == block_token,
                    "SAE-10: command translation preserves the full plan block token");
                if (!translated || !translated.value().reinforcement_search) break;
                const auto tick = world.completed_tick();
                expect(world.submit_reinforcement_search({{tick, 1, static_cast<std::uint64_t>(service)}, {},
                    translated.value().payload}, *translated.value().reinforcement_search).has_value(),
                    "SAE-10: gravity request reaches ordinary command path");
                const auto stepped = world.step(executor);
                expect(stepped.has_value(), "SAE-10: gravity ring advances");
                if (!stepped) break;
                const auto result = world.reinforcement_search_result(1, block_token);
                expect(result && result->candidates <= 10 && result->predictions == 0,
                    "SAE-10: gravity search remains bounded per service with no private predictions");
                if (result) attempts += result->candidates;
                host->snapshot = stepped.value().snapshot;
                host->view = foc::detail::build_view(world, *host->snapshot);
                trace.push_back(stepped.value().state_sha256);
            }
            expect(!world.arrivals().empty() && attempts <= 40,
                "SAE-10: prevention-blocked gravity-well request reinforces within four services");
            for (const auto& command : world.record().commands) {
                if (const auto* reinforce = std::get_if<t::ReinforcePayload>(&command.payload)) {
                    expect(reinforce->position != requested, "SAE-10: blocked gravity request never retries the requested point");
                }
            }
        }
        if (workers == 1) baseline = trace;
        else expect(trace == baseline, "SAE-10: blocked gravity search agrees across 1/2/4/8 workers");
    }
}

void reinforcement_fog_persistence() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}};
    setup.units = {{1, 10, 1}};
    t::EconomyRules economy;
    economy.players = {{1, q(1000), 20}};
    economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
    const t::FogRules fog{q(-12000), q(12000), q(1000), 24, 24};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, fog, {}, {}, {}, economy);
    expect(made.has_value(), "SAE-10: persistent fog fixture starts");
    if (!made) return;
    auto& world = made.value();
    eawr::platform::ThreadWorkerAdapter executor(4);
    expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: fog fixture buys its pool entry");
    expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: fog purchase reaches pool before search");
    std::uint32_t attempt = 0;
    std::uint32_t candidates = 0;
    for (std::uint64_t service = 1; service <= 24; ++service) {
        expect(world.submit_reinforcement_search({{world.completed_tick(), 1, service}, {}, t::ReinforcePayload{1, {}}},
            {8, attempt}).has_value(), "SAE-10: persistent fog submits the next ring");
        expect(world.step(executor).has_value(), "SAE-10: fogged ring executes");
        const auto result = world.reinforcement_search_result(1, 8);
        expect(result && !result->valid && result->candidates == (service == 1 ? 1U : 10U),
            "SAE-10: radius zero checks once; subsequent fogged rings each check ten points");
        if (!result) break;
        attempt = result->next_attempt;
        candidates += result->candidates;
    }
    expect(candidates == 231 && attempt == 231 && world.ledgers().front().pool.size() == 1,
        "SAE-10: failed fog search passes the former distance cutoff and retains the pool entry");
}

// WST-16 (#1905): the AI's queries are stealth-blind, as in the debug build. A real session cloaks
// player 1's frigate; player 2's production nearest query and goal matcher still find it from the
// published view, for an AI and a human requester alike, and only the attack order on it is
// refused (WST-11).
void stealth_blind_queries() {
    namespace t = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto q = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{q(x), {}, {}}; };
    constexpr t::TypeId frigate = 1, corvette = 2;
    for (const bool requester_ai : {true, false}) for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        t::TacticalSetup start;
        start.seed = 1905;
        start.players = {{1, 1, 1, t::player_flag_commandable}, {2, 2, 2, t::player_flag_commandable}};
        start.units = {{1, frigate, 1, at(0), math::identity_quat(), {}}, {2, corvette, 2, at(300), math::identity_quat(), {}}};
        t::DurabilityTable health;
        health.rules = {math::Fixed::from_raw(math::Fixed::scale / 5), math::Fixed::from_raw(math::Fixed::scale * 2 / 5),
            math::Fixed::from_raw(math::Fixed::scale / 3)};
        health.profiles = {{frigate, q(1000), q(1), false, {}}, {corvette, q(1000), q(1), false, {}}};
        t::CombatProfile cloaked; cloaked.type_id = frigate; cloaked.max_attack_distance = q(700);
        cloaked.collision = t::CollisionBox{math::Vec3{q(-10), q(-10), q(-10)}, math::Vec3{q(10), q(10), q(10)}};
        t::CombatProfile hunter = cloaked; hunter.type_id = corvette;
        t::CombatTable combat; combat.profiles = {cloaked, hunter};
        t::AbilityProfile cloak;
        cloak.kind = t::AbilityKind::stealth; cloak.expiration_frames = 300; cloak.recharge_frames = 90;
        cloak.gui_activated_ability_name = "Stealth_Probe";
        t::SpecialAbilityProfile handler;
        handler.name = "Stealth_Probe"; handler.kind = t::SpecialAbilityKind::stealth;
        handler.style = t::SpecialActivationStyle::user_input;
        handler.service_interval = t::special_service_interval(t::SpecialAbilityKind::stealth);
        t::UnitAbilityProfile cloaker{frigate, {cloak}, false};
        cloaker.special = {handler};
        t::AbilityTable abilities; abilities.profiles = {cloaker};
        const std::vector<t::SensorProfile> sensors{{frigate, q(2000)}, {corvette, q(2000)}};
        auto made = t::TacticalSession::create(start, sensors, health, {}, std::nullopt, combat, {}, abilities);
        expect(static_cast<bool>(made), "WST-16: stealth fixture starts");
        if (!made) { std::cerr << made.error().message << '\n'; return; }
        auto& session = made.value();
        expect(session.submit({{0, 1, 0}, {1}, t::AbilityPayload{t::AbilityKind::stealth, t::AbilityAction::activate}}).has_value(),
            "WST-16: the cloak command submits");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        // WST-09: hidden once more than 30 frames passed since the switch-on (detection frame 0).
        for (int frame = 0; frame < 40; ++frame) expect(session.step(executor).has_value(), "WST-16: fixture steps");
        auto host = ai::make_tactical_host();
        foc::AiType ship;
        ship.type_id = frigate; ship.name = "PROBE_FRIGATE"; ship.category_bits = 2;
        ship.combat_power = ai::real(1000); ship.max_attack_distance = ai::real(500); ship.locomotor = true;
        auto gun = ship; gun.type_id = corvette; gun.name = "PROBE_CORVETTE";
        host->setup.content.types = {ship, gun};
        host->setup.content.categories.emplace("CORVETTE", 2);
        host->setup.players = {foc::AiPlayer{1, "REBEL", false, false, ""}, foc::AiPlayer{2, "EMPIRE", false, requester_ai, ""}};
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type);
            host->types_by_name.emplace(type.name, &type);
        }
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        host->world = &session;
        host->snapshot = session.snapshot();
        host->view = foc::detail::build_view(session, *host->snapshot);
        const auto instances = host->snapshot->instances();
        const auto self = std::find_if(instances.begin(), instances.end(), [](const auto& item) { return item.entity_id == 1; });
        const auto* viewed = host->view->find(1);
        expect(self != instances.end() && self->stealth_invisible && viewed != nullptr
            && !foc::detail::object_fogged(*host->view, *viewed, 2),
            "WST-16: the cloaked frigate is stealth-invisible yet unfogged in the AI's published view");
        auth::ModuleManifest manifest;
        expect(manifest.add("NEAR.LUA",
            "function Check(source) Record(Find_Nearest(source, \"Corvette\", source.Get_Owner(), false)) end\n").has_value(),
            "WST-16: nearest fixture loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "WST-16: nearest scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(scripts.value().register_binding("Record", [](auth::BindingContext& context, const auth::ValueList& args) {
            context.issue_command("nearest", args);
            return eawr::core::Result<auth::ValueList>::success({});
        }).has_value(), "WST-16: nearest recorder registers");
        expect(errors.empty() && scripts.value().create_instance(1, "NEAR.LUA").has_value(), "WST-16: nearest instance starts");
        auth::ScriptEvent event;
        event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 2}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "WST-16: nearest query submits");
        const auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty() && report.value().commands.size() == 1,
            "WST-16: production nearest query runs");
        if (report && report.value().commands.size() == 1) {
            const auto& args = report.value().commands.front().arguments;
            const auto* found = args.empty() ? nullptr : std::get_if<auth::Handle>(&args.front().data);
            expect(found != nullptr && found->id == 1, "WST-16: Find_Nearest still returns the cloaked enemy");
        }
        ai::Engine engine(host, {}, {});
        ai::Target target; target.object = 1;
        expect(ai::EngineContractAccess::matches(engine, 2, {"ENEMY_UNIT"}, target),
            "WST-16: the goal matcher still admits the cloaked enemy");
        expect(session.submit({{session.completed_tick(), 2, 0}, {2}, t::AttackPayload{1}}).has_value(),
            "WST-16: the attack order submits");
        const auto stepped = session.step(executor);
        bool hidden = false;
        if (stepped) for (const auto& item : stepped.value().snapshot->events())
            hidden = hidden || (item.kind == t::EventKind::order_rejected && item.reason == t::RejectReason::target_hidden && item.unit == 2);
        expect(hidden, "WST-11/16: only the attack order on the cloaked enemy is refused");
    }
}

// #1891 (AB-44): the AI's ability verb takes every modelled ability kind and every action, so a
// script's MISSILE_SHIELD and SENSOR_JAMMING (and ION_CANNON_SHOT's command) reach the session;
// unmodelled kinds and out-of-range actions stay malformed.
void ai_ability_bound() {
    namespace math = eawr::sim::math;
    const auto command = [](const std::int64_t unit, const std::int64_t kind, const std::int64_t action) {
        auth::ScriptCommand result;
        result.issuer = 1;
        result.verb = std::string(foc::verb_ability);
        result.arguments = {foc::detail::number(Real(1)), foc::detail::handle(foc::handle_game_object, static_cast<std::uint64_t>(unit)),
            foc::detail::number(Real(static_cast<int>(kind))), foc::detail::number(Real(static_cast<int>(action)))};
        return result;
    };
    for (const auto kind : {tactical::AbilityKind::defend, tactical::AbilityKind::spoiler_lock,
             tactical::AbilityKind::ion_cannon_shot, tactical::AbilityKind::hunt, tactical::AbilityKind::missile_shield,
             tactical::AbilityKind::sensor_jamming, tactical::AbilityKind::laser_defense, tactical::AbilityKind::leech_shields}) {
        for (const auto action : {tactical::AbilityAction::activate, tactical::AbilityAction::autofire_off}) {
            const auto order = foc::tactical_ai_detail::translate_order(command(1, static_cast<std::int64_t>(kind),
                static_cast<std::int64_t>(action)));
            const auto* payload = order ? std::get_if<tactical::AbilityPayload>(&order.value().payload) : nullptr;
            expect(payload != nullptr && payload->ability == kind && payload->action == action,
                "#1891: the AI ability verb carries kind " + std::to_string(static_cast<int>(kind)));
        }
    }
    auto targeted = command(1, 23, 1);
    targeted.arguments.push_back(foc::detail::handle(foc::handle_game_object, 2));
    const auto translated = foc::tactical_ai_detail::translate_order(targeted);
    const auto* payload = translated ? std::get_if<tactical::AbilityPayload>(&translated.value().payload) : nullptr;
    expect(payload && payload->ability == tactical::AbilityKind::leech_shields && payload->target == 2,
        "WLS-04: the hosted AI command carries the shield-leech object target");
    targeted.arguments.back() = foc::detail::number(Real(2));
    expect(!foc::tactical_ai_detail::translate_order(targeted), "WLS-04: a numeric target is malformed");
    for (const auto& [kind, action] : std::vector<std::pair<std::int64_t, std::int64_t>>{
             {0, 1}, {255, 1}, {256, 1}, {-1, 1}, {15, 0}, {15, 5}}) {
        expect(!foc::tactical_ai_detail::translate_order(command(1, kind, action)),
            "#1891: an unmodelled kind or an out-of-range action stays malformed");
    }
    const auto q = [](const std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    tactical::AbilityProfile shield;
    shield.kind = tactical::AbilityKind::missile_shield;
    shield.effective_radius = q(100);
    shield.expiration_frames = 300;
    tactical::AbilityProfile jammer = shield;
    jammer.kind = tactical::AbilityKind::sensor_jamming;
    tactical::AbilityTable abilities;
    abilities.profiles = {{11, {shield}}, {12, {jammer}}};
    tactical::CombatTable combat;
    for (const tactical::TypeId type : {11U, 12U}) {
        tactical::CombatProfile profile;
        profile.type_id = type;
        combat.profiles.push_back(profile);
    }
    tactical::TacticalSetup setup;
    setup.seed = 1891;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{1, 11, 1, {}, math::identity_quat(), {}}, {2, 12, 1, {q(500), {}, {}}, math::identity_quat(), {}}};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto made = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, combat, {}, abilities);
        expect(made.has_value(), "#1891: projectile defence fixture starts");
        if (!made) continue;
        auto& world = made.value();
        std::uint64_t sequence = 0;
        for (const auto& [unit, kind] : std::vector<std::pair<std::int64_t, tactical::AbilityKind>>{
                 {1, tactical::AbilityKind::missile_shield}, {2, tactical::AbilityKind::sensor_jamming}}) {
            const auto order = foc::tactical_ai_detail::translate_order(command(unit, static_cast<std::int64_t>(kind),
                static_cast<std::int64_t>(tactical::AbilityAction::activate)));
            expect(static_cast<bool>(order), "#1891: the AI's projectile defence request translates");
            if (order) expect(world.submit({{0, order.value().issuer, sequence++}, order.value().units, order.value().payload}).has_value(),
                "#1891: the AI's projectile defence request submits");
        }
        eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        for (int frame = 0; frame < 3; ++frame) {
            const auto stepped = world.step(executor);
            expect(stepped.has_value(), "#1891: projectile defence fixture steps");
            if (stepped) hashes.push_back(stepped.value().state_sha256);
        }
        const auto shielded = world.ability_state(1);
        const auto jamming = world.ability_state(2);
        expect(shielded && shielded->slots[0].active && jamming && jamming->slots[0].active,
            "#1891: MISSILE_SHIELD and SENSOR_JAMMING switched by the AI are on in the session");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "#1891: AI-switched defences hash alike at 1/2/4/8 workers");
    }
}

void reinforcement_population_wait() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}};
    setup.units = {{1, 10, 1}};
    t::EconomyRules economy;
    economy.players = {{1, q(1000), 1}};
    economy.menus = {{10, 100, {{1, t::BuildKind::unit, t::BuildQueue::units, q(1), 1, 1, 1, true}}}};
    auto made = t::TacticalSession::create(setup, {}, {}, {}, {}, {}, {}, {}, economy);
    expect(made.has_value(), "SAE-10: population wait fixture starts");
    if (!made) return;
    auto& world = made.value();
    eawr::platform::ThreadWorkerAdapter executor(2);
    expect(world.submit({{0, 1, 0}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: buy first pool entry");
    expect(world.submit({{0, 1, 1}, {1}, t::BuyPayload{1}}).has_value(), "SAE-10: buy second pool entry");
    expect(world.step(executor).has_value() && world.step(executor).has_value(), "SAE-10: both pool entries complete");
    expect(world.submit({{2, 1, 2}, {}, t::ReinforcePayload{1, {q(3000), {}, {}}}}).has_value(),
        "SAE-10: reinforce first unit to fill population");
    expect(world.step(executor).has_value(), "SAE-10: population fills");
    expect(world.submit_reinforcement_search({{3, 1, 3}, {}, t::ReinforcePayload{1, {}}}, {10, 21}).has_value(),
        "SAE-10: search waits for population room");
    const auto waiting = world.step(executor);
    const auto result = world.reinforcement_search_result(1, 10);
    expect(waiting && result && result->candidates == 0 && result->next_attempt == 21
        && world.ledgers().front().pool.size() == 1
        && std::none_of(waiting.value().snapshot->events().begin(), waiting.value().snapshot->events().end(),
            [](const auto& event) { return event.kind == t::EventKind::order_rejected
                || event.kind == t::EventKind::order_accepted; }),
        "SAE-03/10: no population room preserves the pool and current ring without placement work");
}

void threat_grid() {
    World world;
    ai::ThreatGrid grid;
    const foc::AiBounds bounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)};
    grid.partition(bounds, 5, 5, ai::Constants{});
    grid.service(*world.view, *world.host, 0);
    const ai::Rect all{ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)};
    // PG-02: radius 500, cell 400 x 400: each of the 3 x 3 covered cells holds 1000 * 160000 / 1000000.
    const Real enemy = grid.force(*world.host, *world.view, all, ~std::uint64_t{0}, 1, false, Real{}, 0);
    expect(enemy == ai::real(1440), "PG-05: the enemy threat over the map is 9 cells of 160");
    expect(grid.force(*world.host, *world.view, all, ~std::uint64_t{0}, 1, true, Real{}, 0) == Real{},
        "PG-05: an enemy unit is no friendly threat");
    expect(grid.force(*world.host, *world.view, all, 1, 1, false, Real{}, 0) == Real{}, "PG-05: the category filters");
    expect(grid.total_force(*world.host, *world.view, ~std::uint64_t{0}, 1, false, Real{}) == ai::real(1000),
        "PG-06: the total enemy force is the unit's power");
    expect(grid.force_visibility(*world.host, *world.view, ~std::uint64_t{0}, 1) == ai::real(1),
        "PG-07: an AI player sees every enemy object");
    const Real none = grid.force_visibility(*world.host, *world.view, 1, 1);
    expect(none == none && none == ai::real(1), "PG-07: no object of the category is fully visible, not NaN");
    const ai::Rect corner{ai::real(-1000), ai::real(-1000), ai::real(100), ai::real(100)};
    expect(grid.force(*world.host, *world.view, corner, ~std::uint64_t{0}, 1, false, Real{}, 0) == Real{},
        "PG-02: a zone covers only the cells within its radius");
}

void unit_ai_grid_admission() {
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto& types = world.host->setup.content.types;
        types.clear();
        const auto type = [](unsigned id, const char* name, unsigned power, bool tracking) {
            foc::AiType result; result.type_id = id; result.name = name;
            result.category_bits = 1; result.combat_power = ai::real(power); result.unit_ai = tracking;
            return result;
        };
        types.push_back(type(1, "SCOUT_GROUP", 420, false));
        types.push_back(type(2, "SCOUT_MEMBER", 140, true));
        types.push_back(type(3, "FIGHTER_GROUP", 300, false));
        types.push_back(type(4, "FIGHTER_MEMBER", 60, true));
        types.push_back(type(5, "TRACKED_SCOUT_GROUP", 420, true));
        const auto identity = foc::tactical_ai_detail::native_configuration(world.host->setup);
        auto changed = world.host->setup;
        changed.content.types.front().unit_ai = true;
        const auto other_identity = foc::tactical_ai_detail::native_configuration(changed);
        expect(identity && other_identity && identity.value() != other_identity.value(),
            "PG-03: UnitAI admission participates in the native configuration identity");
        world.host->types.clear();
        world.host->types_by_name.clear();
        for (const auto& entry : types) world.host->types.emplace(entry.type_id, &entry);
        world.view->units.clear();
        for (const auto root : {10U, 20U, 30U, 40U}) {
            const bool scout = root < 30;
            const auto add = [&](unsigned id, unsigned kind) {
                ai::ViewUnit unit; unit.id = id; unit.type = kind; unit.owner = scout ? 2 : 1;
                world.view->units.push_back(unit);
            };
            add(root, scout ? 1 : 3);
            for (unsigned member = 1; member <= (scout ? 3U : 5U); ++member) add(root + member, scout ? 2 : 4);
        }
        ai::ThreatGrid grid;
        grid.partition({ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)}, 5, 5, ai::Constants{});
        eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto verify = [&](std::int64_t frame, unsigned scout_power, std::size_t objects) {
            grid.service(*world.view, *world.host, frame);
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(grid.objects().size() == objects
                && grid.total_force(*world.host, *world.view, 1, 2, true, Real{}) == ai::real(scout_power)
                && grid.total_force(*world.host, *world.view, 1, 1, true, Real{}) == ai::real(600),
                "PG-03/06: authored UnitAI admission counts craft once across worker counts");
        };
        verify(0, 840, 16);
        for (auto& unit : world.view->units) if (unit.id == 10 || unit.id == 20) unit.type = 5;
        verify(1, 1680, 18);
        for (auto& unit : world.view->units) if (unit.id == 10 || unit.id == 20) unit.type = 1;
        verify(2, 840, 16);
    }
}

struct QueryExecutor final : eawr::sim::PartitionExecutor {
    explicit QueryExecutor(std::size_t workers) : pool(workers) {}
    std::size_t worker_count() const noexcept override { return pool.worker_count(); }
    eawr::core::Result<void> execute(std::size_t partitions,
        const std::function<void(std::size_t)>& partition) const override {
        return pool.execute(partitions, partition);
    }
    eawr::core::Result<void> execute_phase(std::string_view phase, std::size_t partitions,
        const std::function<void(std::size_t)>& partition) const override {
        expect(partitions == eawr::sim::tick_partition_count, "threat preparation uses the fixed 64 partitions");
        expect(!queries_only, "shared inputs: a player query must never dispatch a worker phase");
        ++phases[std::string(phase)];
        if (fail) {
            eawr::core::Diagnostic error;
            error.code = "EAWR-TEST";
            error.message = "query executor refused the phase";
            return eawr::core::Result<void>::failure(std::move(error));
        }
        if (reverse) {
            for (auto index = partitions; index > 0; --index) partition(index - 1);
            return eawr::core::Result<void>::success();
        }
        return pool.execute_phase(phase, partitions, partition);
    }
    eawr::platform::ThreadWorkerAdapter pool;
    mutable std::map<std::string, std::size_t> phases;
    bool fail{}, reverse{}, queries_only{};
};

void shared_view_prepass() {
    namespace t = eawr::sim::tactical;
    t::TacticalSetup start;
    start.players = {{1, 1, 100, 1}, {2, 2, 200, 1}};
    for (eawr::sim::EntityId index = 0; index < 70; ++index) {
        t::Squadron squadron;
        squadron.container = 1000 + index;
        start.units.push_back({squadron.container, 20, 1});
        for (const auto offset : {1U, 101U, 201U, 301U}) {
            const auto id = index + offset;
            squadron.members.push_back(id);
            start.units.push_back({id, 1, 1});
        }
        start.squadrons.push_back(squadron);
    }
    std::sort(start.units.begin(), start.units.end(), [](const auto& left, const auto& right) { return left.entity_id < right.entity_id; });
    for (std::size_t index = 0; index < start.units.size(); ++index) {
        start.units[index].position.x = eawr::sim::math::Fixed::from_integer(static_cast<std::int64_t>(index)).value();
        start.units[index].garrison_enabled = index % 2 != 0;
    }
    auto made = t::TacticalSession::create(start);
    expect(made.has_value(), "shared view: interleaved member identities across 70 containers create");
    if (!made) return;
    const auto original_units = made.value().units();
    expect(!made.value().unit_state(999999), "shared view: lightweight reader rejects absent identities");
    made.value().scramble_storage_for_testing();
    for (const auto& unit : original_units)
        expect(made.value().unit_state(unit.entity_id) == std::optional{unit},
            "shared view: lightweight reader equals the complete UnitState census after storage scrambling");
    const auto published = made.value().snapshot();
    std::vector<t::TacticalInstance> inputs(published->instances().begin(), published->instances().end());
    for (std::size_t index = 0; index < inputs.size(); ++index) inputs[index].visible_to = index % 3 + 1;
    const t::TacticalSnapshot snapshot(published->completed_tick(),
        std::vector<t::SnapshotPlayer>(published->players().begin(), published->players().end()), inputs, {});
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool reverse : {false, true}) {
        QueryExecutor executor(workers); executor.reverse = reverse;
        const auto view = foc::detail::build_view(made.value(), snapshot, &executor);
        expect(executor.phases["ai-world-view"] == 2 && view->units.size() == start.units.size(),
            "shared view: both map replacement and value preparation run on the fixed partitions");
        for (std::size_t index = 0; index < start.units.size(); ++index) {
            const auto& unit = view->units[index];
            expect(unit.id == start.units[index].entity_id && unit.owner == 1 && unit.team == inputs[index].team && unit.visible_to == inputs[index].visible_to,
                "shared view: ordered unit and snapshot joins preserve identity, teams and visibility");
            const auto owner = std::find_if(start.squadrons.begin(), start.squadrons.end(), [&](const auto& squadron) {
                return std::find(squadron.members.begin(), squadron.members.end(), unit.id) != squadron.members.end();
            });
            if (owner != start.squadrons.end()) expect(unit.craft && !unit.container && unit.squadron == owner->container,
                "shared view: ordered member merge joins interleaved craft to the original container");
            else {
                const auto container = std::find_if(start.squadrons.begin(), start.squadrons.end(),
                    [&](const auto& squadron) { return squadron.container == unit.id; });
                expect(container != start.squadrons.end() && unit.container && !unit.craft && unit.members == container->members,
                    "shared view: container membership keeps original member order");
            }
            expect(unit.health == ai::real(1) && unit.shield == Real{} && unit.formation_target == 0 && !unit.formation_moving,
                "shared view: missing durability and formation retain the original defaults");
            const auto& candidate = view->capture_candidates[index];
            expect(candidate.body.entity_id == unit.id && candidate.body.owner == unit.owner && candidate.type == unit.type,
                "shared view: capture inputs retain ascending unit identities");
        }
        auto sparse_inputs = inputs;
        sparse_inputs.erase(sparse_inputs.begin());
        const t::TacticalSnapshot sparse(published->completed_tick(),
            std::vector<t::SnapshotPlayer>(published->players().begin(), published->players().end()), sparse_inputs, {});
        const auto sparse_view = foc::detail::build_view(made.value(), sparse, &executor);
        expect(sparse_view->units.size() == original_units.size() && sparse_view->units.front().id == original_units.front().entity_id
            && sparse_view->units.front().visible_to == 0,
            "shared view: requested snapshot omission retains the current live unit with default snapshot fields");
        executor.fail = true;
        bool refused = false;
        try { static_cast<void>(foc::detail::build_view(made.value(), snapshot, &executor)); }
        catch (const std::runtime_error&) { refused = true; }
        expect(refused, "shared view: failed preparation cannot return a partially populated view");
    }
    t::TacticalSetup small; small.players = start.players; small.units = {{1, 1, 1}};
    auto small_world = t::TacticalSession::create(small);
    expect(small_world.has_value(), "shared view: small inline fixture creates");
    if (small_world) {
        QueryExecutor executor(8); executor.fail = true;
        const auto view = foc::detail::build_view(small_world.value(), *small_world.value().snapshot(), &executor);
        expect(view->units.size() == 1 && executor.phases.empty(), "shared view: fixed row threshold avoids the pool for a small view");
    }
}

void shared_view_formation_join() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto q = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return m::Vec3{q(x), {}, {}}; };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}, {2, 2, 200, t::player_flag_commandable}};
    start.units = {{10, 100, 1, at(0)}, {11, 101, 1, at(0)}, {12, 101, 1, at(10)},
        {20, 100, 1, at(100)}, {21, 101, 1, at(100)}, {22, 101, 1, at(110)},
        {30, 100, 1, at(200)}, {31, 101, 1, at(200)}, {32, 101, 1, at(210)},
        {1000, 102, 2, at(2000)}, {1001, 102, 2, at(3000)}};
    start.squadrons = {{10, {11, 12}}, {20, {21, 22}}, {30, {31, 32}}};
    // Ensure the real worker path runs even with only three formation rows.
    for (eawr::sim::EntityId id = 2000; id < 2260; ++id) start.units.push_back({id, 102, 2, at(100000)});
    t::MotionTable motion;
    t::CraftProfile craft;
    craft.type_id = 101; craft.max_speed = q(5); craft.min_speed = q(1); craft.rate_of_turn = q(6);
    craft.lift = q(6); craft.thrust = q(1); craft.roll_rate = q(6); craft.bank_angle = q(70); craft.strafe_distance = q(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101, 101}, {at(0), at(10)}, q(1000), q(200), q(300), q(20)}};
    t::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = q(450); enemy.type_id = 102;
    t::CombatTable combat; combat.profiles = {fighter, enemy};
    t::DurabilityProfile health; health.type_id = 101; health.max_hull = q(100);
    t::DurabilityTable durability; durability.profiles = {health};
    auto made = t::TacticalSession::create(start, {}, durability, motion, std::nullopt, combat);
    expect(made.has_value(), "formation join: valid moving/attacking squadron fixture creates");
    if (!made) return;
    auto& world = made.value();
    expect(world.submit({{0, 1, 0}, {10}, t::MovePayload{at(5000)}}).has_value()
        && world.submit({{0, 1, 1}, {20}, t::AttackPayload{1000}}).has_value()
        && world.submit({{0, 1, 2}, {30}, t::AttackPayload{1001}}).has_value()
        && world.submit({{0, 1, 3}, {11}, t::DamagePayload{q(25), t::attack_hull}}).has_value(),
        "formation join: distinct real formation orders and non-default damage submit");
    const eawr::sim::InlineExecutor inline_executor;
    expect(world.step(inline_executor).has_value(), "formation join: orders commit into live squadron states");
    const auto moving = world.squadron_state(10), first_attack = world.squadron_state(20), second_attack = world.squadron_state(30);
    expect(moving && moving->mode == t::SquadronMode::move && moving->target == 0
        && first_attack && first_attack->mode == t::SquadronMode::idle && first_attack->target == 1000
        && second_attack && second_attack->mode == t::SquadronMode::idle && second_attack->target == 1001,
        "formation join: fixture establishes distinct non-default modes and targets before testing the join");
    // Deliberately overlap an input member after valid simulation setup. The old
    // first-emplace contract picks the earlier container, whose movement differs.
    auto& duplicate_owner = const_cast<t::Squadron&>(world.squadrons()[1]);
    duplicate_owner.members.push_back(11);
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool reverse : {false, true}) {
        QueryExecutor executor(workers); executor.reverse = reverse;
        const auto view = foc::detail::build_view(world, *world.snapshot(), &executor);
        expect(executor.phases["ai-world-view"] == 2, "formation join: both preparation phases use the worker executor");
        const auto check = [&](const auto& ids, eawr::sim::EntityId target, bool travelling) {
            for (const auto id : ids) {
                const auto* row = view->find(id);
                expect(row && row->formation_target == target && row->formation_moving == travelling,
                    "formation join: each container and craft retains its own non-default target and movement flag");
            }
        };
        check(std::array<eawr::sim::EntityId, 3>{10, 11, 12}, 0, true);
        check(std::array<eawr::sim::EntityId, 3>{20, 21, 22}, 1000, false);
        check(std::array<eawr::sim::EntityId, 3>{30, 31, 32}, 1001, false);
        const auto* duplicate = view->find(11);
        expect(duplicate && duplicate->squadron == 10 && duplicate->formation_target == 0 && duplicate->formation_moving,
            "formation join: duplicate member keeps the first container instead of its attacking neighbour");
        const auto* damaged = view->find(11);
        expect(view->find(10)->can_move && !view->find(11)->can_move && !view->find(1000)->can_move,
            "EX-57: admitted formation containers move independently, their craft and stationary types do not");
        expect(damaged && damaged->hull == q(75) && damaged->max_hull == q(100)
            && damaged->health == ai::real(3) / ai::real(4),
            "formation join: non-default durability row survives the independent snapshot identity join");
    }
    duplicate_owner.members.pop_back();
    expect(world.stage_remove(2259).has_value(), "formation join: stage removal republishes current live identities");
    t::UnitState spawned; spawned.type_id = 102; spawned.owner = 2; spawned.position = at(4000);
    const auto added = world.stage_spawn(spawned);
    expect(added.has_value(), "formation join: stage spawn republishes current live identities");
    if (added) for (const auto workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers); executor.reverse = true;
        const auto view = foc::detail::build_view(world, *world.snapshot(), &executor);
        const auto census = world.units();
        expect(view->units.size() == census.size() && !view->find(2259) && view->find(added.value()),
            "formation join: view immediately sees staged removal and spawn without dropping a live unit");
        for (std::size_t index = 0; index < census.size(); ++index)
            expect(view->units[index].id == census[index].entity_id,
                "formation join: current publication identity domain matches the exact live census after staging");
    }
}

void shared_input_prepasses() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const bool reverse : {false, true}) {
        QueryExecutor executor(workers); executor.reverse = reverse;
        t::TacticalSetup start;
        start.players = {{1, 1, 100, 1}, {2, 1, 200, 1}, {99, 99, 300, 0}};
        start.units = {{1, 10, 2}};
        for (eawr::sim::EntityId id = 1000; id < 1260; ++id) start.units.push_back({id, 20, 2});
        t::EconomyRules rules;
        rules.players = {{1, q(1000), 20, true}, {2, q(1000), 20, true}, {99, {}, 20}};
        rules.menus = {{10, 200, {{50, t::BuildKind::unit, t::BuildQueue::units, q(100), 30, 30, 0, true}}},
            {20, 100, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false},
            {20, 200, {{30, t::BuildKind::structure, t::BuildQueue::units, q(100), 30, 30, 0, true}}, 0, false}};
        rules.pads.neutral = 99;
        rules.pads.capture = {{20, q(100), q(1), {100, 200}, false, true, true}};
        rules.pads.construction = {{30, 40, q(100), 1, 1}};
        rules.pads.influence = {{10, false}, {20, false}, {30, false}, {40, false}};
        auto world = t::TacticalSession::create(start, {}, {}, {}, {}, {}, {}, {}, rules);
        expect(world.has_value(), "shared inputs: large allied producer/pad fixture creates");
        if (!world) continue;
        auto host = ai::make_tactical_host(); host->world = &world.value(); host->snapshot = world.value().snapshot();
        host->view = foc::detail::build_view(world.value(), *host->snapshot, &executor);
        for (const auto& [id, name] : {std::pair{10U, "BASE"}, {20U, "PAD"}, {30U, "UC"}, {40U, "MINE"}}) {
            foc::AiType type; type.type_id = id; type.name = name; type.star_base = id == 10;
            host->setup.content.types.push_back(type);
        }
        for (const auto& type : host->setup.content.types) {
            host->types.emplace(type.type_id, &type); host->types_by_name.emplace(type.name, &type);
        }
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto parsed = ai::EquationSet::parse({{"inputs.xml", "<Root><Open>Variable_Self.OpenBuildPadCount</Open>"
            "<Built>Variable_Self.TacticalBuiltStructureCount{Parameter_Type=\"UC\", Parameter_Type=\"UC\", Parameter_Type=\"MINE\"}</Built></Root>"}}, converters);
        expect(parsed.has_value(), "shared inputs: pad equations parse");
        if (!parsed) continue;
        ai::AiData data; data.equations = std::move(parsed).value();
        ai::Engine engine(host, std::move(data), {});
        ai::EngineContractAccess::perception_executor(engine, &executor);
        const auto uncached = engine.evaluate("Open", 1, nullptr);
        expect(uncached == ai::real(260), "shared inputs: all allied pads are initially open");
        ai::EngineContractAccess::request_open(engine, 1);
        ai::EngineContractAccess::prepare_inputs(engine, executor);
        expect(ai::EngineContractAccess::has_prepared_open(engine, 1) && !ai::EngineContractAccess::has_prepared_open(engine, 2),
            "shared inputs: only preceding actual open-query demand prepares broad-phase admission");
        expect(executor.phases["ai-producers"] == 1 && executor.phases["ai-pad-perception"] == 1,
            "shared inputs: each large census dispatches once before queries");
        std::vector<eawr::sim::EntityId> expected;
        for (eawr::sim::EntityId id = 1000; id < 1260; ++id) expected.push_back(id);
        expect(ai::EngineContractAccess::producers(engine, 1, 30) == expected
            && ai::EngineContractAccess::producers(engine, 1, 50) == std::vector<eawr::sim::EntityId>{1},
            "shared inputs: ordered merge preserves pad candidates and the distinct lower-ID station menu");
        const auto phases = executor.phases;
        executor.queries_only = true;
        expect(engine.evaluate("Open", 1, nullptr) == uncached && engine.producer(1, 30) == 1000,
            "shared inputs: prepared queries retain initial answers");
        ai::EngineContractAccess::reserve_pad(engine, 1000, 2, 30);
        expect(engine.evaluate("Open", 1, nullptr) == ai::real(259)
            && engine.evaluate("Built", 1, nullptr) == ai::real(3) && engine.producer(1, 30) == 1001,
            "shared inputs: later teammate reservation immediately changes open, typed aliases and producer selection");
        ai::EngineContractAccess::release_pad(engine, 1000);
        expect(engine.evaluate("Open", 1, nullptr) == uncached && engine.evaluate("Built", 1, nullptr) == Real{}
            && engine.producer(1, 30) == 1000 && executor.phases == phases,
            "shared inputs: release restores answers without any query-time worker phase");
        executor.queries_only = false;
        ai::EngineContractAccess::prepare_pads(engine, executor);
        expect(!ai::EngineContractAccess::has_prepared_open(engine, 1),
            "shared inputs: open-query demand expires after an unused service");
        executor.queries_only = true;
        expect(engine.evaluate("Open", 2, nullptr) == uncached,
            "shared inputs: a cold player uses the identical inline admission path without dispatch");
        ai::EngineContractAccess::reset_pads(engine);
        expect(world.value().stage_remove(1000).has_value(), "shared inputs: remove pad outside the service barrier");
        expect(engine.evaluate("Open", 1, nullptr) == ai::real(259),
            "shared inputs: unscoped pad read sees removal instead of stale prepared facts");
        executor.queries_only = false; executor.fail = true;
        bool refused = false;
        try { ai::EngineContractAccess::prepare_pads(engine, executor); } catch (const std::runtime_error&) { refused = true; }
        expect(refused && !ai::EngineContractAccess::has_prepared_pads(engine),
            "shared inputs: failed worker phase never publishes partial pad facts");
    }
}

void threat_preparation() {
    World world;
    auto& types = world.host->setup.content.types;
    types.front().combat_power = ai::real(1);
    auto large = types.front();
    large.type_id = 2;
    large.combat_power = ai::real(16777216);
    types.push_back(large);
    world.host->types.clear();
    for (const auto& type : types) world.host->types.emplace(type.type_id, &type);
    world.view->units.clear();
    for (const auto& [id, type, owner] : std::array<std::array<unsigned, 3>, 3>{{{1, 2, 2}, {2, 1, 1}, {3, 1, 1}}}) {
        ai::ViewUnit unit;
        unit.id = id; unit.type = type; unit.owner = owner;
        world.view->units.push_back(unit);
    }
    ai::ThreatGrid grid;
    const auto configure = [&] {
        grid.partition(foc::AiBounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)}, 5, 5, ai::Constants{});
        grid.service(*world.view, *world.host, 0);
    };
    configure();
    const auto all = ~std::uint64_t{};
    const auto original = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
    expect(original.repr == ai::real(16777218).repr, "PG-06: player-first rounding fixture retains the two small contributions");
    const auto entity_fold = ai::to_single(ai::to_single(ai::real(16777216) + ai::real(1)) + ai::real(1));
    expect(original.repr != entity_fold.repr, "PG-06: fixture detects an entity-order or partition-subtotal fold");
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
                "PG-06: staged contributions retain player-first rounding");
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
                "PG-06: repeated query returns identical bits");
            expect(executor.phases.empty(), "PG-06: cold and repeated queries never dispatch a worker phase");
            expect(grid.query_counts().cold == 2 && grid.query_counts().repeats == 1
                && grid.query_counts().scalar_hits == 1, "PG-06: cold, repeated and scalar-hit demand is counted");
        }
        expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr,
            "PG-06: unscoped readers retain the const query path");
        expect(executor.phases.empty(), "PG-06: unscoped reads do not use the borrowed executor");
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            grid.prepare_contributions();
            expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == original.repr
                && grid.query_counts().warm == 1 && executor.phases.empty(),
                "PG-06: small prepass stays inline and retains adversarial player-first rounding");
        }
    }

    types[1].weapons = {{0, ai::real(127), ai::real(431), true, true},
        {1, ai::real(251) / ai::real(7), ai::real(719), true, true}};
    auto other_category = types.front();
    other_category.type_id = 3;
    other_category.category_bits = 1;
    other_category.combat_power = ai::real(73) / ai::real(11);
    types.push_back(other_category);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const auto& type : types) world.host->types.emplace(type.type_id, &type);
    world.host->setup.players.push_back(foc::AiPlayer{3, "NEUTRAL", true, false, ""});
    world.host->setup.players.push_back(foc::AiPlayer{4, "ALLIED", false, true, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.view->players.push_back(foc::detail::ViewPlayer{3, 3, 2});
    world.view->players.push_back(foc::detail::ViewPlayer{4, 1, 3});
    world.view->units.clear();
    for (unsigned index = 0; index < 83; ++index) {
        ai::ViewUnit unit;
        unit.id = index + 1; unit.type = index % 3 + 1; unit.owner = index % 11 == 0 ? 9 : index % 5;
        unit.health = ai::real(1) / ai::real(index % 7 + 1);
        unit.visible_to = index % 16;
        unit.hardpoint_destroyed = {false, index % 2 == 0};
        unit.position.x = eawr::sim::math::Fixed::from_integer(static_cast<int>(index % 5) * 310 - 620).value();
        unit.position.y = eawr::sim::math::Fixed::from_integer(static_cast<int>(index % 7) * 210 - 630).value();
        world.view->units.push_back(unit);
    }
    // A non-AI player uses the authored visibility mask instead of omniscience.
    world.host->setup.players.front().ai = false;
    configure();
    const std::array<ai::Rect, 4> rectangles{{
        {ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)},
        {ai::real(-413), ai::real(-217), ai::real(713), ai::real(507)},
        {ai::real(3000), ai::real(3000), ai::real(1), ai::real(1)},
        {ai::real(-1001), ai::real(99), ai::real(2), ai::real(803)}}};
    struct Query {
        std::uint64_t category;
        eawr::sim::tactical::PlayerId player;
        bool friendly;
        Real attenuator;
        eawr::sim::EntityId excluded;
    };
    const std::array<Query, 10> queries{{
        {all, 0, true, Real{}, 0}, {2, 1, false, ai::real(1) / ai::real(3), 0},
        {2, 2, true, ai::real(1) / ai::real(7), 13}, {1, 1, true, ai::real(1), 0},
        {all, 2, false, ai::real(1), 17}, {all, 9, false, ai::real(1) / ai::real(2), 0},
        {all, 1, false, Real{}, 0}, {all, 1, false, Real::from_repr(0x8000000000000000ULL), 0},
        {all, 3, false, ai::real(1) / ai::real(2), 0}, {2, 4, true, ai::real(1) / ai::real(5), 7}}};
    std::vector<std::uint64_t> expected;
    const auto read = [&](const Query& query, const auto& append) {
        append(grid.total_force(*world.host, *world.view, query.category, query.player, query.friendly, query.attenuator).repr);
        for (const auto& rect : rectangles) {
            append(grid.force(*world.host, *world.view, rect, query.category, query.player, query.friendly, query.attenuator, query.excluded).repr);
        }
    };
    for (const auto& query : queries) read(query, [&](auto value) { expected.push_back(value); });
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        QueryExecutor executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            grid.prepare_contributions();
            const auto prepass_phases = executor.phases;
            for (int repeat = 0; repeat < 2; ++repeat) {
                std::vector<std::uint64_t> actual;
                for (const auto& query : queries) read(query, [&](auto value) { actual.push_back(value); });
                expect(actual == expected, "PG-05/06: cell order, predicates, fractional health and query keys agree across workers");
            }
            expect(executor.phases == prepass_phases, "PG-05/06: all queries stay inline after the prepass");
            expect(executor.phases.size() <= 1 && executor.phases["ai-threat-cells"] == 0,
                "PG-05/06: only one contribution phase can run per prepass");
            expect(grid.query_counts().queries == queries.size() * 10
                && grid.query_counts().repeats == queries.size() * 10 - 8
                && grid.query_counts().cross_player > 0,
                "PG-05/06: repeated keys across query kinds and players are counted");
            if (workers != 1) expect(grid.query_counts().warm == grid.query_counts().queries
                && grid.query_counts().prepared_keys == 8,
                "PG-05/06: prior demand prepares all keys including distinct signed zero");
        }
        std::array<std::vector<std::uint64_t>, eawr::sim::tick_partition_count> concurrent;
        expect(executor.pool.execute(concurrent.size(), [&](const std::size_t partition) {
            for (const auto& query : queries) read(query, [&](auto value) { concurrent[partition].push_back(value); });
        }).has_value(), "PG-05/06: parallel unscoped readers complete");
        for (const auto& values : concurrent) expect(values == expected, "PG-05/06: Lua-style const readers preserve query results");
    }
    QueryExecutor executor(4);
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        const auto first = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
        world.view->units.erase(world.view->units.begin() + 1);
        grid.service(*world.view, *world.host, 300);
        const auto changed = grid.total_force(*world.host, *world.view, all, 0, true, Real{});
        expect(changed.repr != first.repr && executor.phases.empty(),
            "PG-03: service invalidates cached totals after a death without dispatch");
        configure();
        expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr == changed.repr,
            "PG-03: repartition invalidates the query cache");
        expect(executor.phases.empty(), "PG-03: repartition recomputes inline");
    }
    // Seed enough distinct demand for the fixed work-count partition threshold.
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        for (int denominator = 1; denominator <= 12; ++denominator)
            static_cast<void>(grid.total_force(*world.host, *world.view, all, 0, true,
                ai::real(1) / ai::real(denominator)));
    }
    executor.fail = true;
    bool refused = false;
    try {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        grid.prepare_contributions();
    } catch (const std::runtime_error&) {
        refused = true;
    }
    expect(refused, "PG-06: prepass executor failure cannot fabricate a threat result");
    expect(grid.total_force(*world.host, *world.view, all, 0, true, Real{}).repr != 0,
        "PG-06: failure unwinds preparation before later const readers");
    executor.fail = false;
    executor.phases.clear();
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        grid.prepare_contributions();
        expect(grid.query_counts().prepared_keys == 0,
            "PG-06: demand unused for one tick expires instead of accumulating");
        static_cast<void>(grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1)));
    }
    // Change only health between ticks: a warm key must use the CURRENT completed view.
    const auto previous = grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1));
    // Use a large contribution so the final single-precision fold cannot hide the change.
    world.view->units[3].health = ai::real(1) / ai::real(7);
    const auto current = grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1));
    expect(current.repr != previous.repr, "PG-06: current-view fixture detects stale health");
    {
        const auto preparation = grid.prepare(&executor, *world.host, *world.view);
        grid.prepare_contributions();
        expect(grid.total_force(*world.host, *world.view, all, 0, true, ai::real(1)).repr == current.repr
            && grid.query_counts().warm == 1, "PG-06: warm contributions read the current completed view");
    }
}

void plan_definition_paths() {
    World world;
    const std::string path = "Data/Scripts/AI/ai_plan_expansiongeneric_generatemagiccashdrop.lua";
    const std::map<std::string, std::string> modules{{path, R"LUA(
function Base_Definitions()
    Category = "Skirmish_Generate_Magic_Cash_Drop_Space"
    TaskForce = { { "CashForce", "MinimumTotalSize = 0" } }
    MagicPlan = true
    IgnoreTarget = true
    AllowFreeStoreUnits = false
end
)LUA"}};
    std::vector<std::string> notes;
    const auto plans = foc::tactical_ai_detail::load_plans(world.host, modules, notes);
    expect(plans && plans.value().size() == 1,
        "L-03a / PL-10: an admitted plan outside SpaceMode loads from its logical path");
    if (!plans || plans.value().size() != 1) return;
    const auto& plan = plans.value().front();
    expect(plan.module == path && plan.magic && plan.ignore_target && !plan.allow_free_store
        && plan.goals == std::vector<std::string>{"SKIRMISH_GENERATE_MAGIC_CASH_DROP_SPACE"},
        "PL-10: root-level cash definition retains its category and flags");
    expect(std::none_of(notes.begin(), notes.end(), [](const auto& note) {
        return note.starts_with("ai_plan_expansiongeneric_generatemagiccashdrop:");
    }), "L-03a: the cash plan has no module-resolution diagnostic");
    const std::set<std::string> unrelated{"ANOTHER_SPACE_GOAL"};
    notes.clear();
    const auto filtered = foc::tactical_ai_detail::load_plans(world.host, modules, notes, &unrelated);
    expect(filtered && filtered.value().empty() && notes.size() == 1
        && notes.front() == "AI selected space plan definitions loaded=0, outside selection=1",
        "AI-16: unselected definition categories are counted before force validation");
}

void mounted_ai_discovery() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("eawr-ai-discovery-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code error; fs::remove_all(path, error); } } cleanup{root};
    const auto write = [&](const std::string& layer, const std::string& path, const std::string& text) {
        const auto file = root / layer / path;
        fs::create_directories(file.parent_path());
        std::ofstream out(file, std::ios::binary); out << text;
        if (!out) throw std::runtime_error("cannot write AI discovery fixture");
    };
    write("parent", "xml/ai/players/controller.xml", "<AIPlayerType><Name>Parent</Name></AIPlayerType>");
    write("leaf", "xml/ai/players/controller.xml", R"XML(<AIPlayerType><Name>Custom</Name>
        <SpaceFreeStoreScript>CustomFree</SpaceFreeStoreScript><GoalSetExtensionSize>7</GoalSetExtensionSize>
        <GoalProposalFunctionSets>CustomSet</GoalProposalFunctionSets>
        <Templates><Space>CustomTemplate</Space></Templates></AIPlayerType>)XML");
    write("parent", "xml/ai/players/another.xml", R"XML(<AIPlayerType><Name>Other</Name>
        <SpaceFreeStoreScript>OtherFree</SpaceFreeStoreScript></AIPlayerType>)XML");
    write("leaf", "xml/ai/templates/custom.xml", "<AITemplates><CustomTemplate><Budget><Offensive>Score</Offensive></Budget></CustomTemplate></AITemplates>");
    write("leaf", "xml/ai/goalfunctions/customset.xml", "<GoalFunctions><Entry><Goal>CustomGoal</Goal><Function>Score</Function></Entry></GoalFunctions>");
    write("leaf", "xml/ai/goals/custom.xml", "<Goals><CustomGoal><GameMode>Space</GameMode><Category>Offensive</Category></CustomGoal></Goals>");
    write("leaf", "xml/ai/perceptualequations/custom.xml", "<Equations><Score>1</Score></Equations>");
    write("leaf", "xml/difficultyadjustments.xml", "<Root/>");
    write("leaf", "xml/gameconstants.xml", "<Root/>");
    write("leaf", "xml/enum/aigoalcategorytype.xml", "<EnumDefinition><Offensive>1</Offensive></EnumDefinition>");
    for (const auto name : {"PGCommands", "PGBaseDefinitions", "PGBase", "PGDebug", "PGEvents", "PGTaskForce"})
        write("leaf", std::string("scripts/library/") + name + ".lua", "");
    write("leaf", "scripts/library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes = {} FriendlyContrastTypes = {} end");
    write("leaf", "scripts/freestore/CustomFree.lua", "require('LeafHelper')\nrequire('Build-Evaluation/Nested')\nselected = leaf_value + parent_value\n");
    write("parent", "scripts/freestore/OtherFree.lua", "selected = 99\n");
    write("parent", "scripts/library/LeafHelper.lua", "error('shadowed module must not execute')");
    write("leaf", "scripts/library/LeafHelper.lua", "require('ParentHelper') leaf_value = 7");
    write("parent", "scripts/library/ParentHelper.lua", "parent_value = 11");
    write("leaf", "scripts/ai/spacemode/Build-Evaluation/Nested.lua", "nested_value = 19");
    write("leaf", "scripts/ai/spacemode/customplan.lua", R"LUA(function Base_Definitions()
        Category = 'CustomGoal'; TaskForce = {{'CustomForce', 'MinimumTotalSize = 0'}}
        MagicPlan = true
    end)LUA");
    const std::array mounts{eawr::vfs::MountSpec{"leaf", root / "leaf", "data", {}}, eawr::vfs::MountSpec{"parent", root / "parent", "data", {}}};
    auto files = eawr::vfs::Vfs::mount(mounts);
    expect(files.has_value(), "AI-16: layered AI fixture mounts");
    if (!files) return;
    foc::AiSetup setup;
    setup.players = {{1, "FIRST", false, true, "Custom"}, {2, "SECOND", false, true, "Other"}};
    expect(eawr::skirmish::enable_goal_system(files.value(), setup).has_value(), "AI-16: mounted AI directories load");
    const auto data = ai::load_ai_data(setup.xml, {});
    expect(data && data.value().players.contains("CUSTOM") && data.value().players.contains("OTHER")
        && !data.value().players.contains("PARENT") && data.value().templates.contains("CUSTOMTEMPLATE")
        && data.value().function_sets.contains("CUSTOMSET") && data.value().goals.contains("CUSTOMGOAL")
        && data.value().equations.find("Score"), "AI-16: VFS winner and every authored AI category enter the parser");
    expect(data && data.value().players.at("CUSTOM").goal_set_extension == 7
        && data.value().players.at("OTHER").goal_set_extension == 2,
        "GS-30: authored goal extension overrides the omitted-tag engine default");
    expect(foc::freestore_module(setup, setup.players[0]) == "Data/Scripts/FreeStore/CustomFree.lua"
        && foc::freestore_module(setup, setup.players[1]) == "Data/Scripts/FreeStore/OtherFree.lua",
        "AI-16: each controller selects its own authored freestore");
    auto modules = eawr::skirmish::ai_modules(files.value(), setup, true);
    expect(modules.has_value(), "ML-34: selected roots and mounted dependencies enter the bounded manifest");
    if (!modules) return;
    const auto plans = foc::selected_plans(modules.value());
    expect(plans == std::vector<std::string>{"data/scripts/ai/spacemode/customplan.lua"},
        "PL-02: a new plan is discovered while a nested helper stays a dependency");
    auto selected = eawr::skirmish::select_ai_modules(modules.value(), setup, true);
    expect(selected && selected.value() == modules.value(), "ML-34: cached sessions preserve the frozen dependency inventory");
    auto aliases = modules.value();
    aliases["DATA/SCRIPTS/LIBRARY/LEAFHELPER.LUA"] = aliases.at("data/scripts/library/leafhelper.lua");
    const auto same_bytes = eawr::skirmish::select_ai_modules(aliases, setup, true);
    expect(same_bytes && same_bytes.value().size() == modules.value().size(),
        "ML-34: cached optional-root additions collapse identical logical aliases");
    aliases["DATA/SCRIPTS/LIBRARY/LEAFHELPER.LUA"] = "error('conflicting bytes')";
    expect(!eawr::skirmish::select_ai_modules(aliases, setup, true),
        "ML-34: conflicting cached aliases are rejected instead of overriding the mounted winner");
    auto prepared = foc::prepare_session(setup, modules.value());
    expect(prepared.has_value(), "ML-34: new library and space-directory dependencies execute through production setup");
    if (prepared) {
        const auto first = prepared.value().scripts.read_global(foc::freestore_instance(1), "selected");
        const auto second = prepared.value().scripts.read_global(foc::freestore_instance(2), "selected");
        const auto nested = prepared.value().scripts.read_global(foc::freestore_instance(1), "nested_value");
        expect(first && first.value() && std::get<Real>(first.value()->data) == ai::real(18)
            && second && second.value() && std::get<Real>(second.value()->data) == ai::real(99)
            && nested && nested.value() && std::get<Real>(nested.value()->data) == ai::real(19),
            "AI-16 / ML-34: isolated freestores use leaf, parent and space-directory modules");
    }
    write("leaf", "scripts/library/ParentHelper.lua", "this is invalid lua !");
    auto invalid = eawr::vfs::Vfs::mount(mounts);
    auto invalid_modules = invalid ? eawr::skirmish::ai_modules(invalid.value(), setup) : decltype(modules)::failure(invalid.error());
    expect(invalid_modules && !foc::prepare_session(setup, invalid_modules.value()),
        "L-03a: a syntax error in the VFS winner cannot fall through to a valid parent");
}

void authored_goal_extension() {
    for (const auto extension : {0, 2, 7}) {
        World world;
        ai::Engine engine(world.host, {}, {});
        ai::PlayerType controller; controller.goal_set_extension = extension;
        ai::PlayerAi player; player.player = 2; player.type = &controller;
        ai::GoalType kind; kind.category = "HIGH"; kind.application = {"GLOBAL"};
        for (std::size_t index = 0; index < 8; ++index) {
            const auto name = "GOAL_" + std::to_string(index);
            player.functions.push_back({&kind, name, nullptr, {}});
            ai::Goal goal; goal.id = index + 1; goal.function = index; goal.desire = ai::real(1);
            player.proposed.push_back(goal);
        }
        ai::EngineContractAccess::maintain(engine, player);
        expect(player.active.size() == static_cast<std::size_t>(extension),
            "GS-30: actual proposal culling and retention honor the authored extension");
    }
}

void unit_concentration() {
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    const foc::AiBounds bounds{ai::real(-1000), ai::real(1000), ai::real(1000), ai::real(-1000)};
    const ai::Rect all{ai::real(-1000), ai::real(-1000), ai::real(2000), ai::real(2000)};
    std::vector<Real> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        World world;
        world.host->setup.content.types.front().weapons = {{0, ai::real(500), ai::real(500), true, true},
            {1, ai::real(500), ai::real(500), true, true}};
        auto small = world.host->setup.content.types.front(); small.type_id = 2; small.name = "SMALL";
        small.combat_power = ai::real(500); small.weapons.clear();
        world.host->setup.content.types.push_back(small);
        world.host->types.clear(); world.host->types_by_name.clear();
        for (const auto& type : world.host->setup.content.types) {
            world.host->types.emplace(type.type_id, &type); world.host->types_by_name.emplace(type.name, &type);
        }
        auto friendly = world.view->units.front(); friendly.id = 20; friendly.type = 2; friendly.owner = 1;
        friendly.position = {q(600), q(600), {}}; friendly.health = ai::real(1) / ai::real(2);
        world.view->units.push_back(friendly);
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto equations = ai::EquationSet::parse({{"concentration.xml", R"xml(<Root>
          <FriendlyRaw>Variable_Target.FriendlyUnitConcentrationUnnormalized</FriendlyRaw>
          <EnemyRaw>Variable_Target.EnemyUnitConcentrationUnnormalized</EnemyRaw>
          <Friendly>Variable_Target.FriendlyUnitConcentration</Friendly>
          <Enemy>Variable_Target.EnemyUnitConcentration</Enemy>
          <FriendlyNbtd>Variable_Target.FriendlyUnitConcentrationNBTD</FriendlyNbtd>
          <EnemyNbtd>Variable_Target.EnemyUnitConcentrationNBTD</EnemyNbtd>
          <Attenuated>Variable_Target.FriendlyUnitConcentration {Parameter_Attenuator=1}</Attenuated>
          <ObjectLocation>Variable_Target.Location.EnemyUnitConcentrationUnnormalized</ObjectLocation>
        </Root>)xml"}}, converters);
        expect(equations.has_value(), "PE-28: concentration token equations parse"); if (!equations) return;
        ai::AiData data; data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        auto& grid = ai::EngineContractAccess::grid(engine);
        grid.partition(bounds, 5, 5, ai::Constants{}); grid.service(*world.view, *world.host, 0);
        ai::Target region; region.region = all;
        const auto values = [&] {
            std::vector<Real> result;
            for (const auto* name : {"FriendlyRaw", "EnemyRaw", "Friendly", "Enemy", "FriendlyNbtd", "EnemyNbtd", "Attenuated"}) {
                const auto value = engine.evaluate(name, 1, &region);
                expect(value.has_value(), "PE-28: selected concentration token is hosted");
                result.push_back(value.value_or(Real{}));
            }
            return result;
        };
        const auto cold = values();
        expect(cold == std::vector<Real>{ai::real(500), ai::real(2000), ai::real(1) / ai::real(3), ai::real(1),
            ai::real(1), ai::real(1), ai::real(2) / ai::real(5)},
            "PG-09/PE-28: full company power per base entry; game/side normalizers clamp, and only the denominator attenuates");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        {
            const auto preparation = grid.prepare(&executor, *world.host, *world.view);
            expect(values() == cold, "PE-28: cold prepared queries match the const Lua path");
            grid.prepare_contributions();
            expect(values() == cold, "PE-28: warmed force normalizers preserve concentration answers");
        }
        if (reference.empty()) reference = cold;
        else expect(reference == cold, "PE-28: all token bits agree across 1/2/4/8 workers");
        const ai::Rect spread_only{ai::real(400), ai::real(-100), ai::real(100), ai::real(100)};
        expect(grid.concentration(*world.host, *world.view, spread_only, 2, 1, false) == Real{}
            && grid.force(*world.host, *world.view, spread_only, 2, 1, false, Real{}, 0) > Real{},
            "PG-09: spread threat cells do not also count as the entry's base cell");
        const ai::Rect base_cell{ai::real(200), ai::real(-400), ai::real(10), ai::real(10)};
        expect(grid.concentration(*world.host, *world.view, base_cell, 2, 1, false) == ai::real(2000),
            "PG-09: base-cell coordinates round after adding one half, rather than truncate the object position");
        expect(grid.concentration(*world.host, *world.view, all, 1, 1, false) == Real{}, "PG-09: category mask filters entries");
        ai::Target object; object.object = 10;
        expect(engine.evaluate("ObjectLocation", 1, &object) == ai::real(2000), "PE-28: an object-location concentration includes the object itself");
        world.host->setup.players.push_back(foc::AiPlayer{3, "ALLY", false, true, ""});
        world.host->setup.players.push_back(foc::AiPlayer{4, "NEUTRAL", true, false, ""});
        world.host->players.clear();
        for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
        world.view->players.push_back({3, 1, 2}); world.view->players.push_back({4, 4, 3});
        friendly.id = 21; friendly.owner = 3; world.view->units.push_back(friendly);
        friendly.id = 22; friendly.owner = 4; world.view->units.push_back(friendly);
        grid.service(*world.view, *world.host, 1);
        expect(grid.concentration(*world.host, *world.view, all, 2, 1, true) == ai::real(1000)
            && grid.concentration(*world.host, *world.view, all, 2, 1, false) == ai::real(2000), "PG-09: allies count, neutral owners do not");
        world.view->units.front().position.x = q(400); grid.service(*world.view, *world.host, 2);
        expect(grid.concentration(*world.host, *world.view, base_cell, 2, 1, false) == ai::real(2000), "PG-09: base cell retains the existing grid service cadence");
        grid.service(*world.view, *world.host, 300);
        expect(grid.concentration(*world.host, *world.view, base_cell, 2, 1, false) == Real{}, "PG-09: due movement refresh moves concentration to its new base cell");
        for (auto& weapon : world.host->setup.content.types.front().weapons) weapon.range = ai::real(2000);
        auto edge_constants = ai::Constants{}; edge_constants.threat_range_cap = ai::real(2000);
        world.view->units.front().position = {q(999), {}, {}};
        grid.partition(bounds, 5, 5, edge_constants); grid.service(*world.view, *world.host, 0);
        const ai::Rect wrapped_cell{ai::real(-1000), ai::real(-1000), ai::real(10), ai::real(10)};
        const ai::Rect right_cell{ai::real(990), ai::real(-10), ai::real(10), ai::real(10)};
        expect(grid.concentration(*world.host, *world.view, wrapped_cell, 2, 1, false) == ai::real(2000)
            && grid.concentration(*world.host, *world.view, right_cell, 2, 1, false) == Real{},
            "PG-09: the retail right-edge rounded coordinate wraps into the next row when its zone covers that cell");
        world.view->units.front().position = {{}, q(-999), {}};
        grid.partition(bounds, 5, 5, edge_constants); grid.service(*world.view, *world.host, 0);
        expect(grid.concentration(*world.host, *world.view, all, 2, 1, false) == Real{},
            "PG-09: bottom-edge flattened overflow is safely unmatched, never used as a memory index");
    }
}

void coroutine_module_loading() {
    auth::ModuleManifest modules;
    expect(modules.add("Root.lua", R"LUA(
        function Lazy()
            local outer = require('Outer')
            value = outer.answer
            cached = require('Outer') == outer
            restored_name = _REQUIREDNAME == nil
            coroutine.yield(true)
            local ok, message = pcall(require, 'Broken')
            syntax_failed = not ok and string.find(message, 'BROKEN.LUA') ~= nil
            ok, message = pcall(require, 'RuntimeBad')
            runtime_failed = not ok and string.find(message, 'lazy sentinel') ~= nil
            continued = true
        end
    )LUA").has_value(), "L-03a: lazy coroutine root enters manifest");
    expect(modules.add("Outer.lua", "local inner = require('Inner'); outer_name = _REQUIREDNAME == 'Outer'; return {answer = inner.value + 1}").has_value()
        && modules.add("Inner.lua", "inner_name = _REQUIREDNAME == 'Inner'; inner_runs = (inner_runs or 0) + 1; return {value = 41}").has_value()
        && modules.add("Broken.lua", "local = broken").has_value()
        && modules.add("RuntimeBad.lua", "error('lazy sentinel')").has_value(), "L-03a: nested and failing lazy modules enter manifest");
    std::string reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        auto made = auth::ScriptScheduler::create({}, modules);
        expect(made.has_value(), "L-03a: lazy module scheduler creates"); if (!made) return;
        auto scripts = std::move(made).value();
        expect(scripts.create_instance(1, "Root.lua").has_value(), "L-03a: lazy root loads without loading its dependencies");
        auth::ScriptEvent start; start.key = {1, foc::producer_foc_engine, 1, 0}; start.target = 1;
        start.kind = auth::ScriptEvent::Kind::start_thread; start.name = "Lazy";
        expect(scripts.submit_event(start).has_value(), "L-03a: lazy coroutine starts");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto first = scripts.service(executor);
        expect(first && first.value().diagnostics.empty(), "L-03a: first coroutine require uses its own stack and yields");
        const auto saved = scripts.save(executor);
        auto restored = auth::ScriptScheduler::create({}, modules);
        expect(saved && restored && restored.value().load(saved.value()).has_value(), "L-03a: loaded modules and suspended coroutine restore");
        if (!saved || !restored) return;
        auto direct = scripts.service(executor);
        auto resumed = restored.value().service(executor);
        expect(direct && resumed && direct.value().diagnostics.empty() && resumed.value().diagnostics.empty(),
            "L-03a: syntax and runtime require failures reach pcall on the calling coroutine");
        for (const auto* name : {"cached", "restored_name", "outer_name", "inner_name", "syntax_failed", "runtime_failed", "continued"}) {
            const auto value = scripts.read_global(1, name);
            expect(value && value.value() && std::get<bool>(value.value()->data), "L-03a: nested names, cache and failure controls succeed");
        }
        for (const auto& [name, expected] : {std::pair{"value", 42}, {"inner_runs", 1}}) {
            const auto value = scripts.read_global(1, name);
            expect(value && value.value() && std::get<Real>(value.value()->data) == ai::real(expected),
                "L-03a: coroutine receives the returned table and executes a cached dependency once");
        }
        const auto digest = scripts.state_hash(executor), resumed_digest = restored.value().state_hash(executor);
        expect(digest && resumed_digest && digest.value() == resumed_digest.value(), "L-03a: resumed lazy module graph has the uninterrupted bit identity");
        if (!digest) return;
        if (reference.empty()) reference = digest.value();
        else expect(reference == digest.value(), "L-03a: lazy module digest agrees across 1/2/4/8 workers");
    }
}

void movement_query_projection() {
    namespace t = eawr::sim::tactical;
    const auto q = [](std::int64_t value) { return eawr::sim::math::Fixed::from_integer(value).value(); };
    t::TacticalSetup start;
    start.players = {{1, 1, 100, t::player_flag_commandable}};
    start.units = {{1, 100, 1}, {2, 101, 1}};
    t::MotionTable motion;
    motion.rules = {q(30), q(100)};
    motion.profiles = {{100, q(1), q(1), q(1), q(1), q(1)}};
    t::AbilityProfile shield;
    shield.kind = t::AbilityKind::missile_shield; shield.disable_movement = true; shield.effective_radius = q(750);
    auto made = t::TacticalSession::create(start, {}, {}, motion, std::nullopt, {}, {},
        t::AbilityTable{{{100, {shield}}}, {1}});
    expect(made.has_value(), "EX-57: mobile ship and stationary type fixture creates");
    if (!made) { std::cerr << made.error().message << '\n'; return; }
    auto& world = made.value();
    const eawr::sim::InlineExecutor executor;
    for (std::uint64_t tick = 0; tick < 3; ++tick) {
        if (tick != 0) expect(world.submit({{tick - 1, 1, tick}, {1}, t::AbilityPayload{
            t::AbilityKind::missile_shield, tick == 1 ? t::AbilityAction::activate : t::AbilityAction::deactivate}}).has_value(),
            "EX-57: deployment toggle submits");
        if (tick != 0) expect(world.step(executor).has_value(), "EX-57: deployment toggle commits");
        const auto view = foc::detail::build_view(world, *world.snapshot());
        expect(view->find(1)->can_move == (tick != 1) && !view->find(2)->can_move,
            "EX-57: native ship admission respects active deployment and release while stationary type stays excluded");
    }
}

void mod_ai_calls() {
    World world;
    world.view->players.push_back({3, 2, 2});
    world.host->setup.players.push_back(foc::AiPlayer{3, "ALLY", false, true, ""});
    world.host->players.clear();
    for (const auto& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
    world.host->setup.content.types.front().combat_power = ai::real(16777216);
    world.host->setup.content.types.front().property_bits = 2;
    world.host->setup.content.properties = {{"FIRST", 1}, {"SECOND", 2}};
    auto small = world.host->setup.content.types.front(); small.type_id = 2; small.name = "SMALL"; small.combat_power = ai::real(1);
    world.host->setup.content.types.push_back(small);
    world.host->types.clear(); world.host->types_by_name.clear();
    for (const auto& type : world.host->setup.content.types) {
        world.host->types.emplace(type.type_id, &type); world.host->types_by_name.emplace(type.name, &type);
    }
    auto extra = world.view->units.front(); extra.id = 11; extra.type = 2;
    world.view->units.push_back(extra); extra.id = 12; world.view->units.push_back(extra);
    extra.id = 13; extra.owner = 1; world.view->units.push_back(extra);
    ai::Engine engine(world.host, {}, {});
    ai::EngineContractAccess::proposal_fixture(engine); world.host->engine = &engine;
    auth::ModuleManifest manifest;
    expect(manifest.add("CALLS.LUA", R"LUA(
        function Block(tf) tf.Block_Goal_Proposal() end
        function Unblock(tf) tf.Unblock_Goal_Proposal() end
        function SkippedOverride(object)
            skipped_no_result = object.Set_Targeting_Priorities('Custom_Set') == nil
            continued = true
        end
        function SkippedMethods(tf, object)
            skipped_tf_priorities = tf.Set_Targeting_Priorities('Custom_Set') == nil
            skipped_tf_lock = tf.Lock_Current_Orders(true) == nil
            skipped_object_lock = object.Lock_Current_Orders(true) == nil
            methods_continued = true
        end
        function Query(tf, object)
            threat = tf.Get_Self_Threat_Sum()
            online = object.Are_Engines_Online()
            mobile = object.Can_Move()
            ally_self = object.Get_Owner().Is_Ally(object.Get_Owner())
            ally_team = object.Get_Owner().Is_Ally(Find_Player('ALLY'))
            ally_enemy = object.Get_Owner().Is_Ally(Find_Player('REBEL'))
            own_count = table.getn(Find_All_Objects_Of_Type(object.Get_Owner(), 'CORVETTE'))
            total_count = table.getn(Find_All_Objects_Of_Type('CORVETTE'))
            category_union = object.Is_Category('Fighter | Corvette')
            category_absent = object.Is_Category('Fighter')
            property_union = object.Has_Property('First | Second')
        end
    )LUA").has_value(), "EX-53/54/55: call fixture enters manifest");
    auto scripts = auth::ScriptScheduler::create({}, std::move(manifest));
    expect(scripts.has_value(), "EX-53/54/55: call scheduler starts"); if (!scripts) return;
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scripts.value(), world.host, errors);
    foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
    expect(errors.empty(), "EX-53/54/55: production Lua methods register");
    expect(scripts.value().create_instance(1, "CALLS.LUA").has_value()
        && scripts.value().create_instance(2, "CALLS.LUA").has_value(), "EX-55: two independent plan callers");
    const eawr::sim::InlineExecutor executor;
    std::uint64_t sequence{};
    const auto invoke = [&](std::uint64_t instance, const std::string& name, auth::ValueList arguments, std::size_t diagnostics = 0) {
        auth::ScriptEvent event; event.key = {scripts.value().completed_tick() + 1, foc::producer_foc_engine, instance, sequence++};
        event.target = instance; event.kind = auth::ScriptEvent::Kind::call; event.name = name; event.arguments = std::move(arguments);
        expect(scripts.value().submit_event(std::move(event)).has_value(), "EX-55: call event queues");
        auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.size() == diagnostics, "EX-53/54/55: sourced calls report the expected diagnostics");
        if (report && diagnostics != 0)
            expect(std::all_of(report.value().diagnostics.begin(), report.value().diagnostics.end(),
                [](const auto& diagnostic) { return diagnostic.code == auth::codes::unsupported_api; }),
                "EX-59: every skipped override or order lock is explicitly unsupported");
        if (report) expect(engine.after_service(report.value()).has_value(), "EX-55: ordered proposal effect commits");
    };
    const auto force = [](std::uint64_t id) { return auth::Value{auth::Handle{ai::handle_taskforce, id}}; };
    invoke(1, "Block", {force(1)}); invoke(1, "Block", {force(1)}); invoke(2, "Block", {force(2)});
    expect(ai::EngineContractAccess::player(engine, 2)->proposal_blocks == 3, "EX-55: repeated and independent holders are counted");
    invoke(1, "Unblock", {force(1)});
    const auto* player = ai::EngineContractAccess::player(engine, 2);
    expect(player->proposal_blocks == 1 && player->proposal_blockers == std::map<std::uint64_t, std::uint32_t>{{2, 1}},
        "EX-55: unblock removes all of the caller's holders and retains another goal's holder");
    invoke(1, "Unblock", {force(1)});
    expect(player->proposal_blocks == 1, "EX-55: repeated unblock cannot release another goal or underflow");
    invoke(2, "Unblock", {force(2)});
    expect(player->proposal_blocks == 0 && player->proposal_blockers.empty(), "EX-55: final owner releases proposals");
    world.view->units.front().engines_online = false;
    world.view->units.front().can_move = true;
    invoke(1, "Query", {force(1), auth::Value{auth::Handle{foc::handle_game_object, 10}}});
    const auto threat = scripts.value().read_global(1, "threat");
    const auto online = scripts.value().read_global(1, "online");
    const auto own = scripts.value().read_global(1, "own_count");
    const auto total = scripts.value().read_global(1, "total_count");
    const auto category_union = scripts.value().read_global(1, "category_union");
    const auto category_absent = scripts.value().read_global(1, "category_absent");
    const auto property_union = scripts.value().read_global(1, "property_union");
    expect(threat && threat.value() && std::get<Real>(threat.value()->data) == ai::real(16777216),
        "EX-54: threat sum rounds after every member, rather than once after an exact sum");
    expect(online && online.value() && !std::get<bool>(online.value()->data), "EX-53: disabled engines report false independently of combat power");
    for (const auto& [name, expected] : {std::pair{"mobile", true}, {"ally_self", true}, {"ally_team", true}, {"ally_enemy", false}}) {
        const auto value = scripts.value().read_global(1, name);
        expect(value && value.value() && std::get<bool>(value.value()->data) == expected,
            "EX-57/58: completed movement eligibility and player alliances retain independent meanings");
    }
    expect(own && own.value() && std::get<Real>(own.value()->data) == ai::real(3)
        && total && total.value() && std::get<Real>(total.value()->data) == ai::real(4),
        "AI-16: player-first object lookup preserves ownership and category matching");
    expect(category_union && category_union.value() && std::get<bool>(category_union.value()->data)
        && category_absent && category_absent.value() && !std::get<bool>(category_absent.value()->data)
        && property_union && property_union.value() && std::get<bool>(property_union.value()->data),
        "EX-56: authored category and property unions test any matching bit");
    invoke(1, "SkippedOverride", {auth::Value{auth::Handle{foc::handle_game_object, 10}}}, 1);
    for (const auto* name : {"continued", "skipped_no_result"}) {
        const auto value = scripts.value().read_global(1, name);
        expect(value && value.value() && std::get<bool>(value.value()->data),
            "EX-59: counted unsupported setter returns no result and allows the caller to continue");
    }
    invoke(1, "SkippedMethods", {force(1), auth::Value{auth::Handle{foc::handle_game_object, 10}}}, 3);
    for (const auto* name : {"skipped_tf_priorities", "skipped_tf_lock", "skipped_object_lock", "methods_continued"}) {
        const auto value = scripts.value().read_global(1, name);
        expect(value && value.value() && std::get_if<bool>(&value.value()->data) && std::get<bool>(value.value()->data),
            "EX-59: TaskForce targeting and object/TaskForce locks diagnose each skip, return no result and continue");
    }
}

void taskforce_definitions() {
    World world;
    foc::AiType frigate;
    frigate.type_id = 2;
    frigate.name = "FRIGATE_A";
    frigate.category_bits = 4;
    foc::AiType craft;
    craft.type_id = 3;
    craft.name = "CRAFT_A";
    craft.category_bits = 1;
    craft.craft = true;
    world.host->setup.content.types.push_back(frigate);
    world.host->setup.content.types.push_back(craft);
    world.host->setup.content.categories.emplace("FRIGATE", 4);
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const foc::AiType& entry : world.host->setup.content.types) {
        world.host->types.emplace(entry.type_id, &entry);
        world.host->types_by_name.emplace(entry.name, &entry);
    }
    auth::ValueList globals;
    globals.push_back(auth::Value::text("Destroy_Unit | Destroy_Unit_Minimal"));
    std::vector<auth::Value> taskforce{auth::Value::text("MainForce"), auth::Value::text("Corvette | Frigate = 2, 6"),
        auth::Value::text("Fighter = 1, 4"), auth::Value::text("Frigate_A = 1"), auth::Value::text("Craft_A = 1"),
        auth::Value::text("Frigate_Z = 2"), auth::Value::text("MinimumTotalSize = 3"), auth::Value::text("EscortForce")};
    std::vector<auth::Value> list{auth::Value{taskforce}};
    globals.push_back(auth::Value{list});
    for (int index = 0; index < 8; ++index) globals.push_back(auth::Value{});
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "test", "test.lua", globals, notes);
    expect(static_cast<bool>(plan), "PL-10: the plan builds");
    if (!plan) return;
    const ai::PlanDef& definition = plan.value();
    expect(definition.goals == std::vector<std::string>{"DESTROY_UNIT", "DESTROY_UNIT_MINIMAL"}, "PL-10: Category names the goals");
    expect(definition.taskforces.size() == 1, "PL-10: one TaskForce");
    const ai::TaskForceDef& tf = definition.taskforces.front();
    expect(tf.teams.size() == 4, "PL-11: a team naming only a squadron member is dropped");
    if (tf.teams.size() == 4) {
        expect(tf.teams[0].min_count == 2 && tf.teams[0].max_count == 6, "PL-11: a = min, b = max");
        expect(tf.teams[0].types == std::vector<eawr::sim::tactical::TypeId>{1, 2}, "PL-11: a category list takes every type of it");
        expect(tf.teams[1].min_count == 1 && tf.teams[1].max_count == 4 && tf.teams[1].types.empty(),
            "PL-11: a category team no loaded type has stays, and nothing fills it");
        expect(tf.teams[2].min_count == 1 && tf.teams[2].max_count == 1 && tf.teams[2].types.size() == 1,
            "PL-11: a type name with one count");
        expect(tf.teams[3].min_count == 2 && tf.teams[3].types.empty(), "PL-11: a team of an unloaded type stays, and nothing fills it");
    }
    expect(tf.minimum_size == 3 && tf.escort, "PL-11: keywords");
    expect(definition.allow_free_store && definition.allow_engaged && !definition.ignore_target, "PL-12: flag defaults");
}

// PL-20: a named type list proposes its types in the order the plan wrote them, so equal weights
// pick the first name written (the original's enumeration lists the satellite plan's laser first).
void named_team_authored_order() {
    World world;
    world.host->setup.content.types.clear();
    // Ids deliberately descend against the authored order: an id sort would reverse it.
    const std::vector<std::pair<eawr::sim::tactical::TypeId, const char*>> named{{9, "UC_SAT_LASER"}, {7, "UC_SAT_MISSILE"}, {5, "UC_SAT_REPAIR"}};
    for (const auto& [id, name] : named) {
        foc::AiType type;
        type.type_id = id;
        type.name = name;
        world.host->setup.content.types.push_back(type);
    }
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const foc::AiType& entry : world.host->setup.content.types) {
        world.host->types.emplace(entry.type_id, &entry);
        world.host->types_by_name.emplace(entry.name, &entry);
    }
    auth::ValueList globals;
    globals.push_back(auth::Value::text("Build_Structure_Space"));
    std::vector<auth::Value> taskforce{auth::Value::text("MainForce"),
        auth::Value::text("Uc_Sat_Laser | Uc_Sat_Missile | Uc_Sat_Repair = 0, 1")};
    std::vector<auth::Value> list{auth::Value{taskforce}};
    globals.push_back(auth::Value{list});
    for (int index = 0; index < 8; ++index) globals.push_back(auth::Value{});
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "structure", "structure.lua", globals, notes);
    expect(static_cast<bool>(plan), "PL-20: the named-type plan builds");
    if (!plan) return;
    const ai::TaskForceDef& tf = plan.value().taskforces.front();
    expect(tf.teams.size() == 1 && tf.teams.front().types == std::vector<eawr::sim::tactical::TypeId>{9, 7, 5},
        "PL-20: a named type list keeps the authored order, not the id order");
}

// PL-20 (#3717): a category team lists the type table in load order, filtered; ids (name hashes)
// decide nothing. The engine reads content.types as the table; ai_setup builds it from the tables'
// table_order, so a type-id sort of either reverses the list here.
void category_team_table_order() {
    World world;
    world.host->setup.content.types.clear();
    world.host->setup.content.categories = {{"FIGHTER", 1}, {"CORVETTE", 2}};
    // Ids ascend against the table (the first listed type has the highest id).
    const std::vector<std::pair<eawr::sim::tactical::TypeId, const char*>> table{{90, "UC_FIRST"}, {70, "UC_SECOND"}, {50, "UC_OTHER"}, {30, "UC_THIRD"}};
    for (const auto& [id, name] : table) {
        foc::AiType type;
        type.type_id = id;
        type.name = name;
        type.category_bits = std::string_view(name) == "UC_OTHER" ? 2 : 1;
        world.host->setup.content.types.push_back(type);
    }
    world.host->types.clear();
    world.host->types_by_name.clear();
    for (const foc::AiType& entry : world.host->setup.content.types) {
        world.host->types.emplace(entry.type_id, &entry);
        world.host->types_by_name.emplace(entry.name, &entry);
    }
    auth::ValueList globals;
    globals.push_back(auth::Value::text("Hide"));
    std::vector<auth::Value> taskforce{auth::Value::text("MainForce"), auth::Value::text("Fighter = 1, 2")};
    std::vector<auth::Value> list{auth::Value{taskforce}};
    globals.push_back(auth::Value{list});
    for (int index = 0; index < 8; ++index) globals.push_back(auth::Value{});
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "category", "category.lua", globals, notes);
    expect(static_cast<bool>(plan), "PL-20: the category plan builds");
    if (!plan) return;
    expect(plan.value().taskforces.front().teams.front().types == std::vector<eawr::sim::tactical::TypeId>{90, 70, 30},
        "PL-20: a category team keeps the type-table order, not the id order");

    // ai_setup: the table order of the loaded tables decides content.types, whatever the ids are.
    const std::vector<std::string> names{"Uc_Alpha", "Uc_Bravo", "Uc_Charlie", "Uc_Delta", "Uc_Echo"};
    std::vector<std::string> by_id = names;
    std::sort(by_id.begin(), by_id.end(), [](const auto& a, const auto& b) { return skirmish::type_id(a) < skirmish::type_id(b); });
    eawr::units::UnitTables tables;
    std::uint64_t order = 100;
    for (auto name = by_id.rbegin(); name != by_id.rend(); ++name) { // table order = descending id
        eawr::units::UnitType unit;
        unit.id = *name;
        unit.table_order = order++;
        tables.units.push_back(std::move(unit));
    }
    std::reverse(tables.units.begin(), tables.units.end()); // roster order must not matter either
    const auto setup = skirmish::ai_setup(skirmish::SkirmishStart{}, skirmish::StartInputs{}, tables);
    std::vector<std::string> listed;
    for (const auto& type : setup.content.types) listed.push_back(type.name);
    std::vector<std::string> expected;
    for (auto name = by_id.rbegin(); name != by_id.rend(); ++name) {
        std::string upper = *name;
        for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        expected.push_back(std::move(upper));
    }
    expect(listed == expected, "PL-20: ai_setup lists the types in table order, not type-id order");
}

void required_category_unions() {
    World world;
    world.host->setup.content.categories = {{"FIGHTER", 1}, {"CORVETTE", 2}, {"BOMBER", 4},
        {"HIGH", std::uint64_t{1} << 63}, {"NONE", 0}};
    world.host->setup.content.types.front().category_bits = 1;
    const std::vector<auth::Value> main{auth::Value::text("MainForce"), auth::Value::text("Bomber | Corvette = 0, 2")};
    const std::vector<auth::Value> escort{auth::Value::text("EscortForce"), auth::Value::text("Fighter = 1, 2")};
    auth::ValueList globals{auth::Value::text("Hide"), auth::Value{std::vector<auth::Value>{auth::Value{main}, auth::Value{escort}}}};
    globals.resize(10);
    globals[2] = auth::Value{true};
    const std::vector<std::pair<std::string, std::uint64_t>> rows{
        {"Bomber | Corvette", 6}, {"Bomber,Corvette", 6}, {"Bomber Corvette", 6},
        {"Bomber\tCorvette\nBomber", 6}, {"High | Bomber", (std::uint64_t{1} << 63) | 4},
        {"None", 0}, {"", 0}, {" || , \t\n", 0},
    };
    for (const auto& [text, expected] : rows) {
        globals[9] = auth::Value{std::vector<auth::Value>{auth::Value::text(text)}};
        std::vector<std::string> notes;
        auto plan = ai::build_plan(*world.host, "hide", "hide.lua", globals, notes);
        expect(plan && plan.value().required_categories == std::vector<std::uint64_t>{expected} && notes.empty(),
            "PL-14: category union delimiters preserve full unsigned masks");
    }
    globals[9] = auth::Value{std::vector<auth::Value>{auth::Value::text("Fighter"),
        auth::Value::text("Bomber | Corvette"), auth::Value::text("Bomber | Unknown"),
        auth::Value::text("Bomber & Corvette"), auth::Value{true}}};
    std::vector<std::string> notes;
    auto plan = ai::build_plan(*world.host, "hide", "hide.lua", globals, notes);
    expect(plan && plan.value().required_categories == std::vector<std::uint64_t>{1, 6} && notes.size() == 2,
        "PL-14: separate rows stay separate; malformed unions are reported and omitted in full");
    if (!plan) return;
    ai::Engine engine(world.host, {}, {plan.value()});
    world.host->engine = &engine;
    ai::PlayerAi player;
    player.player = 2;
    ai::PotentialPlan potential;
    expect(!ai::EngineContractAccess::selection(engine, player, potential, 67),
        "PL-14/PL-25: hide admission rejects fighter-only escorts before an absent MainForce can be guarded");
    globals[9] = auth::Value{};
    notes.clear();
    auto unguarded = ai::build_plan(*world.host, "hide", "hide.lua", globals, notes);
    if (!unguarded) { expect(false, "PL-14: unrestricted control parses"); return; }
    ai::Engine control(world.host, {}, {unguarded.value()});
    ai::PotentialPlan without_requirement;
    expect(ai::EngineContractAccess::selection(control, player, without_requirement, 67),
        "PL-14: same fighter-only force is otherwise admissible, reproducing the dropped-union defect");
}

void scheduler_reads() {
    auth::ModuleManifest manifest;
    const bool added = static_cast<bool>(manifest.add("TEST.LUA",
        "function Worker() while true do coroutine.yield(true) end end\nfunction Once() end\n"));
    expect(added, "the test module is added");
    auth::SessionConfig config;
    config.seed = 1;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(static_cast<bool>(scripts), "the scheduler starts");
    if (!scripts) return;
    std::vector<std::uint64_t> sequences;
    static_cast<void>(scripts.value().register_binding("Probe", [&sequences](auth::BindingContext& context, const auth::ValueList&) {
        sequences.push_back(context.command_sequence());
        context.issue_command("probe", {});
        sequences.push_back(context.command_sequence());
        return eawr::core::Result<auth::ValueList>::success(auth::ValueList{});
    }));
    expect(static_cast<bool>(scripts.value().create_instance(5, "TEST.LUA")), "the instance is created");
    auto submit = [&](auth::ScriptEvent::Kind kind, std::string name, std::uint64_t sequence) {
        auth::ScriptEvent event;
        event.key = auth::EventKey{scripts.value().completed_tick() + 1, auth::first_simulation_producer, 5, sequence};
        event.target = 5;
        event.kind = kind;
        event.name = std::move(name);
        return static_cast<bool>(scripts.value().submit_event(std::move(event)));
    };
    expect(submit(auth::ScriptEvent::Kind::start_thread, "Worker", 0) && submit(auth::ScriptEvent::Kind::start_thread, "Once", 1) &&
            submit(auth::ScriptEvent::Kind::call, "Probe", 2),
        "the events are accepted");
    const eawr::sim::InlineExecutor executor;
    auto report = scripts.value().service(executor);
    expect(static_cast<bool>(report), "the service runs");
    const auto slots = scripts.value().thread_slots(5);
    expect(slots && slots.value() == std::vector<bool>{true, false}, "PL-42: thread slots report which threads live");
    expect(sequences.size() == 2 && sequences[1] == sequences[0] + 1, "EX-20: the command sequence names the next command");
    auto read = scripts.value().read_global(5, "Worker");
    expect(read && !read.value().has_value(), "EX-40: a function global has no value form");
    expect(static_cast<bool>(scripts.value().remove_instance(5)), "PL-43: the instance is removed");
    expect(scripts.value().instances().empty(), "PL-43: no instance remains");
    expect(ai::block_id(5, 7) == ((std::uint64_t{5} << 32) | 7U), "EX-20: a block is named by instance and sequence");
}

void taskforce_team_order() {
    World world;
    ai::PlanDef definition;
    definition.name = "Ordered";
    definition.module = "ORDERED.LUA";
    ai::TaskForceDef force;
    force.name = "MainForce";
    force.teams.resize(2);
    definition.taskforces = {force};
    ai::Engine engine(world.host, {}, {definition});
    auth::ModuleManifest manifest;
    expect(manifest.add(definition.module,
        "function Base_Definitions() end\nfunction MainForce_Thread() coroutine.yield(true) end\n").has_value(),
        "PL-40: team ordering module loads");
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    expect(scripts.has_value(), "PL-40: team ordering scheduler starts");
    if (!scripts) return;
    auto& player = ai::EngineContractAccess::add_player(engine, 1);
    ai::Goal goal; goal.id = 1; goal.potential.emplace();
    goal.potential->units = {50, 99, 51, 52};
    goal.potential->freestore = {0, 0, 0, 10};
    goal.potential->sources = {2, 2, 1, 0};
    goal.potential->producers = {101, 202, 303, 404};
    goal.potential->pool_tokens = {0, 0, 55, 0};
    goal.potential->taskforce_of_unit = {0, 0, 0, 0};
    goal.potential->team_of_unit = {1, 0, 1, 1};
    std::uint64_t sequence = 0;
    expect(ai::EngineContractAccess::attach(engine, player, goal, scripts.value(), sequence).has_value(),
        "PL-40: out-of-order selected units attach through the plan path");
    const auto* attached = engine.plan(goal.plan);
    expect(attached != nullptr && attached->taskforces.size() == 1, "PL-40: ordered force attaches once");
    if (attached == nullptr || attached->taskforces.size() != 1) return;
    const auto* actual = engine.taskforce(attached->taskforces.front());
    expect(actual != nullptr && actual->types == std::vector<eawr::sim::tactical::TypeId>{99, 50, 51, 52},
        "PL-40: authored teams precede selection order, with stable order within a team");
    if (actual != nullptr) expect(actual->sources == std::vector<std::uint8_t>{2, 2, 1, 0}
        && actual->producers == std::vector<eawr::sim::EntityId>{202, 101, 303, 404}
        && actual->pool_tokens == std::vector<std::uint64_t>{0, 0, 55, 0},
        "PL-40: production and pool identity metadata stay paired with their selected types");
}

void optional_taskforce_startup() {
    // An empty escort's authored exit must not end a selected main force's live plan.
    // Required forces still start when empty, so their authored failure is preserved.
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        for (const int scenario : {0, 1, 2}) {
            for (const auto source : std::array<std::uint8_t, 3>{0, 1, 2}) {
                World world;
                ai::PlanDef definition;
                definition.name = "Startup";
                definition.module = "STARTUP.LUA";
                definition.taskforces = {ai::TaskForceDef{}, ai::TaskForceDef{}};
                definition.taskforces[0].name = "MainForce";
                definition.taskforces[1].name = "EscortForce";
                definition.taskforces[1].required = scenario == 2;
                ai::Engine engine(world.host, {}, {definition});
                auth::ModuleManifest manifest;
                const std::string script = "function Base_Definitions() end\n"
                    "function MainForce_Thread() while true do Probe('main') coroutine.yield(true) end end\n"
                    "function EscortForce_Thread() Probe('escort') "
                    + std::string(scenario == 0 ? "" : "_ScriptExit() coroutine.yield(false) ")
                    + "while true do coroutine.yield(true) end end\n";
                expect(manifest.add(definition.module, script).has_value(), "PL-40: startup fixture module loads");
                auth::SessionConfig config;
                config.tick_duration = {1, 30};
                auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
                expect(scripts.has_value(), "PL-40: startup scheduler starts");
                if (!scripts) continue;
                expect(scripts.value().register_binding("Probe", [](auth::BindingContext& context, const auth::ValueList& args) {
                    context.issue_command("probe", args);
                    return eawr::core::Result<auth::ValueList>::success({});
                }).has_value(), "PL-40: startup observer registers");
                auto& player = ai::EngineContractAccess::add_player(engine, 1);
                ai::Goal goal;
                goal.id = 1;
                goal.potential.emplace();
                const std::size_t selected = scenario == 0 ? 2U : 1U;
                for (std::size_t index = 0; index < selected; ++index) {
                    goal.potential->units.push_back(1);
                    goal.potential->freestore.push_back(10 + index);
                    goal.potential->sources.push_back(source);
                    goal.potential->producers.push_back(0);
                    goal.potential->pool_tokens.push_back(source == 1 ? index + 1 : 0);
                    goal.potential->taskforce_of_unit.push_back(index);
                    goal.potential->team_of_unit.push_back(0);
                }
                std::uint64_t sequence = 0;
                expect(ai::EngineContractAccess::attach(engine, player, goal, scripts.value(), sequence).has_value(),
                    "PL-40: selected plan attaches through the production path");
                for (const auto& event : ai::EngineContractAccess::attachment_events(engine)) {
                    expect(scripts.value().submit_event(event).has_value(), "PL-40: ordinary attachment event submits");
                }
                eawr::platform::ThreadWorkerAdapter executor(workers);
                std::vector<std::string> observed;
                std::vector<std::uint64_t> removed;
                for (int tick = 0; tick < 3; ++tick) {
                    auto report = scripts.value().service(executor);
                    expect(report.has_value(), "PL-40: selected plan's coroutine service succeeds");
                    if (!report) break;
                    for (const auto& command : report.value().commands) {
                        if (command.verb == "probe" && command.arguments.size() == 1) {
                            const auto* text = std::get_if<std::string>(&command.arguments.front().data);
                            if (text != nullptr) observed.push_back(*text);
                        }
                    }
                    removed.insert(removed.end(), report.value().removed_instances.begin(), report.value().removed_instances.end());
                    expect(engine.after_service(report.value()).has_value(), "PL-40: completion reaches the plan engine");
                }
                const auto* plan = engine.plan(goal.plan);
                const auto escort = scripts.value().read_global(ai::first_plan_instance, "EscortForce");
                if (scenario == 1) {
                    expect(observed == std::vector<std::string>{"main", "main", "main"} && removed.empty()
                        && plan != nullptr && !plan->exited && escort && escort.value() && std::holds_alternative<std::monostate>(escort.value()->data),
                        "PL-40: empty optional escort has no global or thread and the main plan keeps running");
                } else if (scenario == 0) {
                    expect(observed == std::vector<std::string>{"main", "escort", "main", "main"} && removed.empty()
                        && plan != nullptr && !plan->exited,
                        "PL-40: selected escort starts in definition order and both forces keep their plan");
                } else {
                    expect(observed == std::vector<std::string>{"main", "escort"}
                        && removed == std::vector<std::uint64_t>{ai::first_plan_instance} && plan != nullptr && plan->exited,
                        "PL-40: empty required escort starts and its authored failure ends the plan");
                }
            }
        }
    }
}

void hazard_plan_events() {
    namespace tactical = eawr::sim::tactical;
    World world;
    ai::ViewUnit unit;
    unit.id = 7;
    unit.type = 1;
    unit.owner = 1;
    world.view->units.insert(world.view->units.begin(), unit);
    world.host->snapshot = std::make_shared<const tactical::TacticalSnapshot>(1,
        std::vector<tactical::SnapshotPlayer>{}, std::vector<tactical::TacticalInstance>{},
        std::vector<tactical::Event>{
            {0, tactical::EventKind::ability_cancelled, 1, static_cast<std::uint64_t>(tactical::AbilityKind::turbo), 7},
            {0, tactical::EventKind::ability_ready, 1, static_cast<std::uint64_t>(tactical::AbilityKind::turbo), 7},
            {0, tactical::EventKind::ability_ready, 1, static_cast<std::uint64_t>(tactical::AbilityKind::turbo), 99}});
    auth::ModuleManifest manifest;
    expect(manifest.add("HAZARDS.LUA", "function MainForce_Unit_Ability_Ready(tf, unit, ability) end\n"
        "function Default_Unit_Ability_Cancelled(tf, unit, ability) end\n").has_value(), "WAB-34: handler fixture loads");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(scripts.has_value(), "WAB-34: handler scheduler starts");
    if (!scripts) return;
    expect(scripts.value().create_instance(1, "HAZARDS.LUA").has_value(), "WAB-34: plan instance loads");
    ai::Engine engine(world.host, {}, {ai::PlanDef{}});
    std::uint64_t sequence = 0;
    const auto events = ai::EngineContractAccess::hazard_events(engine, scripts.value(), sequence);
    expect(events.size() == 2, "WHZ-23/24: only the TaskForce member's signals reach its plan");
    if (events.size() != 2) return;
    expect(events[0].name == "Default_Unit_Ability_Cancelled" && events[1].name == "MainForce_Unit_Ability_Ready",
        "WAB-34: cancellation and ready use the ordinary fallback/direct plan handlers");
    expect(events[0].arguments.size() == 3 && events[1].arguments.size() == 3,
        "WAB-34: handlers receive TaskForce, unit and ability");
    if (events[1].arguments.size() == 3) {
        const auto* name = std::get_if<std::string>(&events[1].arguments[2].data);
        expect(name && *name == "Turbo", "WAB-34: the stock Turbo recovery comparison receives its expected spelling");
    }
}

// EX-46: a squadron member is its team container, which has no weapon state. Target_In_Range fires for
// it when the team leader holds the formation's enemy target within its weapon reach; a ship member
// still fires on its own attack target.
void squadron_target_in_range() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    World world;
    const auto at = [](std::int64_t x) { return math::Vec3{math::Fixed::from_integer(x).value(), {}, {}}; };
    const auto unit = [&](sim::EntityId id, tactical::PlayerId owner, std::int64_t x) {
        ai::ViewUnit entry;
        entry.id = id;
        entry.type = 1;
        entry.owner = owner;
        entry.position = at(x);
        return entry;
    };
    // IDs ascend: container 10 (leader 11, second craft 12), ship 20, enemy 30.
    auto container = unit(10, 1, 0);
    container.container = true;
    container.members = {11, 12};
    container.formation_target = 30;
    auto leader = unit(11, 1, 0);
    leader.craft = true;
    leader.squadron = 10;
    auto second = leader;
    second.id = 12;
    auto ship = unit(20, 1, 0);
    ship.attack_target = 30;
    auto enemy = unit(30, 2, 800);
    const auto publish = [&](std::vector<ai::ViewUnit> units) {
        auto next = std::make_shared<ai::WorldView>(*world.view);
        next->units = std::move(units);
        world.view = next;
        world.host->view = next;
    };
    auth::ModuleManifest manifest;
    expect(manifest.add("RANGE.LUA", "function Default_Target_In_Range(tf, unit, target) end\n").has_value(),
        "EX-46: handler fixture loads");
    auth::SessionConfig config;
    config.tick_duration = auth::TickDuration{1, 30};
    auto scripts = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(scripts.has_value() && scripts.value().create_instance(1, "RANGE.LUA").has_value(), "EX-46: plan instance loads");
    if (!scripts) return;
    ai::Engine engine(world.host, {}, {ai::PlanDef{}});
    ai::EngineContractAccess::event_force(engine, scripts.value(), {10, 20});
    const auto fired = [&](std::vector<ai::ViewUnit> units) {
        publish(std::move(units));
        std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
        for (const auto& event : ai::EngineContractAccess::run_events(engine)) {
            if (event.name != "Default_Target_In_Range" || event.arguments.size() != 3) continue;
            pairs.emplace_back(std::get<auth::Handle>(event.arguments[1].data).id, std::get<auth::Handle>(event.arguments[2].data).id);
        }
        return pairs;
    };
    using Pairs = std::vector<std::pair<std::uint64_t, std::uint64_t>>;
    auto leader_holding = leader;
    leader_holding.attack_target = 30;
    auto second_holding = second;
    second_holding.attack_target = 30;
    // 800 apart, beyond the 500 reach: the leader holds the target but it is not in range yet.
    expect(fired({container, leader_holding, second_holding, ship, enemy}) == Pairs{{20, 30}},
        "EX-46: an out-of-range squadron stays silent while a ship member fires on its own target");
    enemy.position = at(400);
    expect(fired({container, leader_holding, second_holding, ship, enemy}) == Pairs{{10, 30}},
        "EX-46: the container fires once the leader holds the formation target within reach");
    expect(fired({container, leader_holding, second_holding, ship, enemy}).empty(), "EX-46: the same target does not repeat");
    expect(fired({container, leader, second_holding, ship, enemy}).empty(),
        "EX-46: a leader without the target does not fire, whatever the other craft hold");
    expect(fired({container, leader_holding, second_holding, ship, enemy}) == Pairs{{10, 30}},
        "EX-46: reacquiring the target fires again");
    auto friendly = enemy;
    friendly.owner = 1;
    expect(fired({container, leader_holding, second_holding, ship, friendly}).empty(),
        "EX-46: a friendly formation target is not a target in range");
    enemy.position = at(2000);
    expect(fired({container, leader_holding, second_holding, ship, enemy}).empty(), "EX-46: leaving the reach ends the range");
    enemy.position = at(400);
    expect(fired({container, second_holding, ship, enemy}) == Pairs{{10, 30}},
        "EX-46: the next live craft leads when the first dies");
}

// FH-23: the Lua freestore must see formation state even though the container has
// neither a ship locomotor nor weapon state. Exercise the real snapshot and bindings.
// WLD-10: the engine's stand-in for the Crusader's point-defence object script. It reads the deadly enemy's
// projectile types, switches LASER_DEFENSE on once a second per object, and only for an AI-owned,
// ready, inactive Crusader whose deadly enemy fires an affected projectile.
void point_defence_stand_in() {
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    // Unit 1: the AI's Crusader. Unit 2 fires a missile; unit 3 fires a laser-class shot only.
    setup.units = {{1, 100, 2, at(0), math::identity_quat(), {}}, {2, 101, 1, at(900), math::identity_quat(), {}},
        {3, 102, 1, at(-900), math::identity_quat(), {}}};
    tactical::AbilityProfile laser;
    laser.kind = tactical::AbilityKind::laser_defense;
    laser.expiration_frames = 1050;
    laser.recharge_frames = 1800;
    laser.recharge_at_activation = true;
    laser.gui_activated_ability_name = "Point_Defence";
    tactical::SpecialAbilityProfile handler;
    handler.name = "Point_Defence";
    handler.kind = tactical::SpecialAbilityKind::laser_defense;
    handler.style = tactical::SpecialActivationStyle::user_input;
    handler.service_interval = 1;
    handler.defence_duration_frames = 1050;
    handler.defence_look_frames = 15;
    handler.defence_radius = fixed(500);
    handler.defence_min_travel = fixed(100);
    handler.defence_categories = tactical::defence_category_missile;
    tactical::UnitAbilityProfile profile;
    profile.type_id = 100;
    profile.abilities = {laser};
    profile.special = {handler};
    tactical::AbilityTable table;
    table.profiles = {profile};
    tactical::CombatTable combat;
    tactical::CombatProfile crusader, missile_ship, laser_ship;
    crusader.type_id = 100;
    missile_ship.type_id = 101;
    laser_ship.type_id = 102;
    combat.profiles = {crusader, missile_ship, laser_ship};
    for (const bool owned_by_ai : {true, false}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, combat, {}, table);
        expect(created.has_value(), "WLD-10: point defence world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        foc::AiType shooter, lasers, defender;
        defender.type_id = 100; defender.locomotor = true; defender.point_defence_script = true;
        shooter.type_id = 101; shooter.locomotor = true; shooter.hardpoint_projectiles = {0, 300};
        lasers.type_id = 102; lasers.locomotor = true; lasers.hardpoint_projectiles = {301};
        host->setup.content.types = {defender, shooter, lasers};
        host->types.emplace(100, &host->setup.content.types[0]);
        host->types.emplace(101, &host->setup.content.types[1]);
        host->types.emplace(102, &host->setup.content.types[2]);
        host->setup.content.projectile_categories.emplace(300, foc::ai_projectile_missile);
        host->view = foc::detail::build_view(world, *world.snapshot());
        ai::Engine engine(host, {}, {});
        if (owned_by_ai) ai::EngineContractAccess::collect_fixture(engine);
        const auto count = [&](const std::int64_t frame) {
            ai::EngineContractAccess::point_defence(engine, frame);
            return ai::EngineContractAccess::point_defence_orders(engine);
        };
        expect(count(1) == 0, "WLD-10: with no threat nobody switches laser defence on");
        ai::EngineContractAccess::threat(engine, 1, 3, 5);
        expect(count(1) == 0, "WLD-10: a deadly enemy that fires only lasers does not switch it on");
        ai::EngineContractAccess::threat(engine, 1, 2, 9);
        expect(count(2) == 0, "WLD-10: the object's phase in the second decides when it looks");
        const auto issued = count(1);
        if (!owned_by_ai) {
            expect(issued == 0, "WLD-10: a player without an AI never uses the stand-in");
            continue;
        }
        expect(issued == 1, "WLD-10: a deadly enemy with a missile switches laser defence on");
        const auto command = ai::EngineContractAccess::last_order(engine);
        expect(command && command->verb == foc::verb_ability && command->arguments.size() == 4
            && std::get<auth::Handle>(command->arguments[1].data).id == 1
            && std::get<Real>(command->arguments[2].data) == Real(static_cast<int>(tactical::AbilityKind::laser_defense))
            && std::get<Real>(command->arguments[3].data) == Real(static_cast<int>(tactical::AbilityAction::activate)),
            "WLD-10: the request is the ordinary ability activation of the Crusader");
        // The command has to survive the translation into a world order as well: an ability kind beyond the
        // verb's bound would be dropped as malformed, and the stand-in's decision would never reach the world.
        const auto translated = foc::tactical_ai_detail::translate_order(*command);
        const auto* translated_payload = translated ? std::get_if<tactical::AbilityPayload>(&translated.value().payload) : nullptr;
        expect(translated_payload != nullptr && translated_payload->ability == tactical::AbilityKind::laser_defense
            && translated_payload->action == tactical::AbilityAction::activate
            && translated.value().units == std::vector<eawr::sim::EntityId>{1} && translated.value().issuer == 2,
            "WLD-10: the stand-in's request translates to LASER_DEFENSE activate for the Crusader");
        // Once the world has it on, the stand-in leaves it alone.
        tactical::PlayerCommand activate{{world.completed_tick(), 2, 0}, {1},
            tactical::AbilityPayload{tactical::AbilityKind::laser_defense, tactical::AbilityAction::activate}};
        expect(world.submit(activate).has_value(), "WLD-10: the world accepts the activation");
        const eawr::platform::ThreadWorkerAdapter executor(1);
        expect(world.step(executor).has_value(), "WLD-10: the world steps");
        host->view = foc::detail::build_view(world, *world.snapshot());
        expect(count(31) == issued, "WLD-10: an active laser defence is not requested again");
    }
}

void corrupt_systems_stand_in() {
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    for (const bool human : {false, true}) {
        auto host = ai::make_tactical_host();
        host->setup.players = {{1, "REBEL", false, false, ""}, {2, "UNDERWORLD", false, !human, ""},
            {3, "NEUTRAL", true, false, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        foc::AiType hero, enemy_type;
        hero.type_id = 100; hero.corrupt_systems_script = true;
        enemy_type.type_id = 101; enemy_type.combat_power = Real(1000);
        host->setup.content.types = {hero, enemy_type};
        host->types.emplace(100, &host->setup.content.types[0]);
        host->types.emplace(101, &host->setup.content.types[1]);
        auto view = std::make_shared<foc::detail::WorldView>();
        view->players = {{1, 1, 0}, {2, 2, 1}, {3, 3, 2}};
        foc::detail::ViewUnit source, enemy;
        source.id = 1; source.type = 100; source.owner = 2; source.team = 2;
        tactical::AbilityStatus status;
        status.kind = tactical::AbilityKind::corrupt_systems; status.ready = true;
        source.abilities = {status};
        enemy.id = 2; enemy.type = 101; enemy.owner = 1; enemy.team = 1;
        enemy.position.x = fixed(200); enemy.shield = Real(1);
        view->units = {source, enemy};
        auto neutral = enemy; neutral.id = 99; neutral.owner = neutral.team = 3;
        view->units.push_back(neutral);
        host->view = view;
        ai::Engine engine(host, {}, {});
        if (!human) ai::EngineContractAccess::collect_fixture(engine);
        const auto service = [&](std::int64_t frame) {
            std::sort(view->units.begin(), view->units.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
            ai::EngineContractAccess::corrupt_systems(engine, frame);
            return ai::EngineContractAccess::point_defence_orders(engine);
        };
        expect(service(1) == 0, "WHE-67: threat equal to 1000 does not cast");
        enemy.id = 3; enemy.position.x = fixed(300);
        view->units.push_back(enemy);
        expect(service(2) == 0, "WHE-67: service waits for the object's one-second phase");
        expect(service(1) == (human ? 0U : 1U), "WHE-67: only the AI casts above the threshold");
        if (human) continue;
        const auto* command = ai::EngineContractAccess::last_order(engine);
        expect(command && command->arguments.size() == 7
            && std::get<Real>(command->arguments[4].data) == Real(250),
            "WHE-68: a small cluster casts at its planar mean");
        if (command) expect(foc::tactical_ai_detail::translate_order(*command).has_value(),
            "WHE-67: the native point activation reaches the ordinary command path");
        view->units[1].shield = view->units[2].shield = Real{};
        expect(service(31) == 1, "WHE-67: unshielded enemies are ignored without a deadly enemy");
        ai::EngineContractAccess::threat(engine, 1, 2, 10);
        enemy_type.combat_power = Real(1001);
        host->setup.content.types[1].combat_power = enemy_type.combat_power;
        expect(service(31) == 2, "WHE-67: an unshielded deadly enemy still triggers corruption");
        view->units[0].abilities[0].ready = false;
        expect(service(61) == 2, "WHE-67: a cooling-down hero does not issue another request");
        view->units[0].abilities[0].ready = true;
        for (auto& unit : view->units) if (unit.owner == 1) unit.shield = Real(1);
        for (std::uint64_t id = 4; id <= 6; ++id) {
            enemy.id = id; enemy.shield = Real(1); enemy.position.x = fixed(250);
            view->units.push_back(enemy);
        }
        ai::AiRandom draws;
        draws.set_seed(ai::EngineContractAccess::selection_seed(engine, 2));
        for (unsigned draw = 0; draw < 4; ++draw) static_cast<void>(draws.next());
        expect(service(91) == 3 && ai::EngineContractAccess::selection_seed(engine, 2) == draws.seed(),
            "WHE-68: five candidates use exactly two random planar cluster centers");
    }
}

void corrupt_systems_cluster_coordinates() {
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x, std::int64_t y) { return math::Vec3{fixed(x), fixed(y), {}}; };
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        tactical::TacticalSetup setup;
        setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
        setup.units = {{1, 100, 2, at(0, 0), math::identity_quat(), {}}};
        const std::array points{at(100, 0), at(200, 100), at(300, -100), at(500, 150), at(650, -150)};
        for (std::size_t index = 0; index < points.size(); ++index)
            setup.units.push_back({index + 2, 101, 1, points[index], math::identity_quat(), {}});
        tactical::AbilityProfile cast;
        cast.kind = tactical::AbilityKind::corrupt_systems;
        cast.gui_activated_ability_name = "area";
        cast.recharge_frames = 1800; cast.recharge_at_activation = true; cast.effective_radius = fixed(400);
        tactical::SpecialAbilityProfile handler;
        handler.name = "area"; handler.kind = tactical::SpecialAbilityKind::corrupt_systems;
        handler.style = tactical::SpecialActivationStyle::user_input;
        handler.corrupt_min_range = fixed(20); handler.corrupt_max_range = fixed(350); handler.corrupt_duration_frames = 360;
        tactical::AbilityTable abilities;
        abilities.profiles = {{100, {cast}, false, {handler}}};
        tactical::MotionTable motion;
        motion.rules = {fixed(15), fixed(300)};
        const tactical::MotionProfile source_motion{100, fixed(1), fixed(1), fixed(1), fixed(1), fixed(1)};
        const tactical::MotionProfile enemy_motion{101, fixed(1), fixed(1), fixed(1), fixed(1), fixed(1)};
        motion.profiles = {source_motion, enemy_motion};
        tactical::CombatTable combat;
        tactical::CombatProfile source_combat, enemy_combat;
        source_combat.type_id = 100; enemy_combat.type_id = 101;
        combat.profiles = {source_combat, enemy_combat};
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, abilities);
        expect(created.has_value(), "WHE-68: asymmetric cluster world starts");
        if (!created) continue;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        host->setup.players = {{1, "REBEL", false, false, ""}, {2, "UNDERWORLD", false, true, ""}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        foc::AiType hero, enemy_type;
        hero.type_id = 100; hero.corrupt_systems_script = true;
        enemy_type.type_id = 101; enemy_type.combat_power = Real(1000);
        host->setup.content.types = {hero, enemy_type};
        host->types.emplace(100, &host->setup.content.types[0]);
        host->types.emplace(101, &host->setup.content.types[1]);
        auto view = std::make_shared<foc::detail::WorldView>();
        view->players = {{1, 1, 0}, {2, 2, 1}};
        for (const auto& unit : setup.units) {
            foc::detail::ViewUnit shown;
            shown.id = unit.entity_id; shown.type = unit.type_id; shown.owner = shown.team = unit.owner;
            shown.position = unit.position; shown.shield = Real(1);
            if (shown.id == 1) {
                tactical::AbilityStatus status;
                status.kind = tactical::AbilityKind::corrupt_systems; status.ready = true;
                shown.abilities = {status};
            }
            view->units.push_back(shown);
        }
        host->view = view;
        ai::Engine engine(host, {}, {});
        ai::EngineContractAccess::collect_fixture(engine);
        ai::EngineContractAccess::set_selection_seed(engine, 2, 0);
        ai::EngineContractAccess::corrupt_systems(engine, 1);
        const auto* command = ai::EngineContractAccess::last_order(engine);
        expect(command && command->arguments.size() == 7, "WHE-68: asymmetric cluster issues a point request");
        if (!command || command->arguments.size() != 7) continue;
        expect(std::get<Real>(command->arguments[4].data) == Real(150)
            && std::get<Real>(command->arguments[5].data) == Real(50),
            "WHE-68: seed zero draws Y before X and selects the in-range point (150,50)");
        expect(ai::EngineContractAccess::selection_seed(engine, 2) == 2919708916U,
            "WHE-68: the asymmetric two-center search consumes exactly four synchronized draws");
        const auto order = foc::tactical_ai_detail::translate_order(*command);
        expect(order.has_value(), "WHE-68: asymmetric point request translates");
        if (!order) continue;
        expect(world.submit({{0, order.value().issuer, 0}, order.value().units, order.value().payload}).has_value(),
            "WHE-68: asymmetric point request enters the session");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(world.step(executor).has_value(), "WHE-68: asymmetric point request executes");
        const auto& instances = world.snapshot()->instances();
        const auto recipient = std::find_if(instances.begin(), instances.end(), [](const auto& instance) { return instance.entity_id == 2; });
        expect(recipient != instances.end() && recipient->corrupt.has_value(),
            "WHE-68: the selected point passes the authored 350 range and corrupts an enemy");
    }
}

void squadron_ability_context() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(2000), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    tactical::CombatTable combat;
    tactical::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = fixed(450);
    enemy.type_id = 102;
    combat.profiles = {fighter, enemy};
    tactical::AbilityProfile lock;
    lock.kind = tactical::AbilityKind::spoiler_lock;
    tactical::UnitAbilityProfile abilities;
    abilities.type_id = 101;
    abilities.abilities = {lock};
    tactical::AbilityTable table;
    table.profiles = {abilities};
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, table);
        expect(created.has_value(), "FH-23: squadron context world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        foc::AiType type;
        type.type_id = 100; type.locomotor = true; type.category_bits = 1;
        host->setup.content.types = {type};
        host->setup.content.categories.emplace("FIGHTER", 1);
        host->types.emplace(100, &host->setup.content.types[0]);
        auth::ModuleManifest modules;
        expect(modules.add("CONTEXT.LUA",
            "function Check(unit)\n"
            " target = unit.Get_Attack_Target()\n"
            " busy = unit.Has_Active_Orders()\n"
            " if target then unit.Activate_Ability('SPOILER_LOCK', false)\n"
            " elseif not busy then unit.Activate_Ability('SPOILER_LOCK', true) end\n"
            "end\n").has_value(), "FH-23: query fixture loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "FH-23: query scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "CONTEXT.LUA").has_value(),
            "FH-23: real object bindings register");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        std::vector<std::string> hashes;
        const auto query = [&]() {
            host->view = foc::detail::build_view(world, *world.snapshot());
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
            event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, 10}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "FH-23: query queues");
            return scripts.value().service(executor);
        };
        const auto step = [&](tactical::CommandPayload payload, std::uint64_t sequence) {
            tactical::PlayerCommand command{{world.completed_tick(), 1, sequence}, {10}, std::move(payload)};
            expect(world.submit(command).has_value(), "FH-23: world order queues");
            auto result = world.step(executor);
            expect(result.has_value(), "FH-23: world steps");
            if (result) hashes.push_back(result.value().state_sha256);
        };
        const auto idle = query();
        expect(idle && idle.value().commands.size() == 1, "FH-23: idle freestore requests travel lock");
        step(tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, tactical::AbilityAction::activate}, 0);
        step(tactical::MovePayload{at(3000)}, 1);
        const auto travel = query();
        expect(travel && travel.value().commands.empty(), "FH-23: travelling formation is busy, no repeated idle toggle");
        expect(host->view->find(10)->formation_moving, "FH-23: container projects formation travel");
        expect(!host->view->find(10)->moving,
            "FH-23: Lua formation travel does not replace the separate taskforce completion input");
        step(tactical::AttackPayload{20}, 2);
        const auto engaged = query();
        expect(host->view->find(10)->formation_target == 20 && host->view->find(11)->formation_target == 20,
            "FH-23: container and craft project the live formation target");
        expect(host->view->find(10)->attack_target == 0 && host->view->find(11)->attack_target == 0,
            "PL-21: a distant formation target does not mark fighters engaged or fire an in-range event");
        const auto target = scripts.value().read_global(1, "target");
        expect(target && target.value() && std::get<auth::Handle>(target.value()->data).id == 20,
            "FH-23: Lua queries still return the commanded formation target");
        ai::Engine selection(host, {}, {});
        ai::PlayerAi player;
        player.player = 1;
        ai::PlanDef plan;
        plan.allow_engaged = false;
        const ai::Goal goal;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan) == std::vector<eawr::sim::EntityId>{10},
            "PL-21: a fighter approaching its commanded target can still join an unengaged-only plan");
        auto combat_view = std::make_shared<foc::detail::WorldView>(*host->view);
        for (auto& unit : combat_view->units) if (unit.id == 11) unit.attack_target = 20;
        host->view = combat_view;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan).empty(),
            "PL-21: actual craft combat excludes the squadron from an unengaged-only plan");
        plan.allow_engaged = true;
        expect(ai::EngineContractAccess::eligible(selection, player, goal, plan) == std::vector<eawr::sim::EntityId>{10},
            "PL-21: a plan allowing engaged fighters can take the same squadron");
        expect(engaged && engaged.value().commands.size() == 1,
            "FH-23: engaged freestore requests open foils");
        if (engaged && engaged.value().commands.size() == 1) {
            const auto& request = engaged.value().commands.front();
            expect(request.verb == foc::verb_ability && request.arguments.size() == 4
                && std::get<Real>(request.arguments[3].data) == Real(static_cast<int>(tactical::AbilityAction::deactivate)),
                "FH-23: the engaged request deactivates travel lock");
        }
        const auto busy = scripts.value().read_global(1, "busy");
        expect(busy && busy.value() && std::get<bool>(busy.value()->data), "FH-23: engaged squadron reports active orders");
        step(tactical::AbilityPayload{tactical::AbilityKind::spoiler_lock, tactical::AbilityAction::deactivate}, 3);
        const auto state = world.ability_state(11);
        expect(state && !state->slots[0].active, "FH-23: engaged X-wing foils are open");
        if (baseline.empty()) baseline = hashes;
        else expect(hashes == baseline, "FH-23: squadron context hashes equally across workers");
    }
}

// WAB-70/76: the stock AI's Activate_Ability("BUZZ_DROIDS", object) reads the container's own team
// status and becomes a point ability command at the object's position; the unit call answers true.
void buzz_droids_lua_point(const bool corrupt = false) {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(200), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    tactical::CombatTable combat;
    tactical::CombatProfile fighter, enemy;
    fighter.type_id = 101; fighter.max_attack_distance = fixed(450);
    enemy.type_id = 102;
    combat.profiles = {fighter, enemy};
    tactical::AbilityProfile buzz;
    buzz.kind = tactical::AbilityKind::buzz_droids;
    buzz.team = true;
    buzz.recharge_frames = 1800;
    buzz.buzz = tactical::BuzzDroidsProfile{103, fixed(5), fixed(400), fixed(300), fixed(175), fixed(1), fixed(1), fixed(4), 30, 1, 0};
    tactical::UnitAbilityProfile abilities;
    abilities.type_id = 100;
    abilities.abilities = {buzz};
    const auto kind = corrupt ? tactical::AbilityKind::corrupt_systems : tactical::AbilityKind::buzz_droids;
    const std::string ability_name = corrupt ? "CORRUPT_SYSTEMS" : "BUZZ_DROIDS";
    if (corrupt) {
        abilities.type_id = 101;
        auto& cast = abilities.abilities[0];
        cast.kind = kind;
        cast.team = false;
        cast.buzz.reset();
        cast.recharge_at_activation = true;
        cast.gui_activated_ability_name = "area";
        tactical::SpecialAbilityProfile handler;
        handler.kind = tactical::SpecialAbilityKind::corrupt_systems;
        handler.name = "area";
        handler.style = tactical::SpecialActivationStyle::user_input;
        handler.corrupt_duration_frames = 360;
        handler.corrupt_max_range = fixed(350);
        abilities.special = {handler};
    }
    tactical::AbilityTable table;
    table.profiles = {abilities};
    auto created = tactical::TacticalSession::create(setup, {}, {}, motion, std::nullopt, combat, {}, table);
    expect(created.has_value(), "WAB-76: buzz-droid AI world starts");
    if (!created) return;
    auto world = std::move(created).value();
    auto host = ai::make_tactical_host();
    foc::AiType type;
    type.type_id = corrupt ? 101 : 100; type.locomotor = true; type.category_bits = 1;
    type.abilities = {ability_name};
    host->setup.content.types = {type};
    host->types.emplace(type.type_id, &host->setup.content.types[0]);
    auth::ModuleManifest modules;
    expect(modules.add("BUZZ.LUA",
        "function Check(unit, enemy)\n"
        " ready = unit.Is_Ability_Ready('" + ability_name + "')\n"
        " issued = unit.Activate_Ability('" + ability_name + "', enemy)\n"
        "end\n").has_value(), "WAB-76: query fixture loads");
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
    if (!scripts) { expect(false, "WAB-76: scheduler starts"); return; }
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
    expect(errors.empty() && scripts.value().create_instance(1, "BUZZ.LUA").has_value(), "WAB-76: object bindings register");
    const eawr::platform::ThreadWorkerAdapter executor(1);
    expect(world.step(executor).has_value(), "WAB-76: world steps once");
    host->view = foc::detail::build_view(world, *world.snapshot());
    auth::ScriptEvent event;
    event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, 1, 0};
    event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
    event.arguments = {auth::Value{auth::Handle{foc::handle_game_object, corrupt ? 11U : 10U}}, auth::Value{auth::Handle{foc::handle_game_object, 20}}};
    expect(scripts.value().submit_event(std::move(event)).has_value(), "WAB-76: query queues");
    const auto served = scripts.value().service(executor);
    expect(served && served.value().commands.size() == 1, "WAB-76: one ability command for the squadron container");
    if (served && served.value().commands.size() == 1) {
        const auto& request = served.value().commands.front();
        expect(request.verb == foc::verb_ability && request.arguments.size() == 7
            && std::get<Real>(request.arguments[2].data) == Real(static_cast<int>(kind))
            && std::get<Real>(request.arguments[4].data) == Real(200),
            "WAB-76: the request carries the object's position as its point");
    }
    const auto ready = scripts.value().read_global(1, "ready");
    const auto issued = scripts.value().read_global(1, "issued");
    expect(ready && ready.value() && std::get<bool>(ready.value()->data), "WAB-70: the container's own team slot answers ready");
    expect(issued && issued.value() && std::get<bool>(issued.value()->data), "WAB-76: the unit call answers true once issued");
}

// EX-31: pad-capture plans must keep their travelling squadron reserved until its
// formation arrives. A container has no ship Motion; test the real simulation snapshot.
void squadron_movement_blocks() {
    namespace tactical = eawr::sim::tactical;
    namespace math = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return math::Fixed::from_integer(value).value(); };
    const auto at = [&](std::int64_t x) { return math::Vec3{fixed(x), {}, {}}; };
    tactical::TacticalSetup setup;
    setup.players = {{1, 1, 1, tactical::player_flag_commandable}, {2, 2, 2, tactical::player_flag_commandable}};
    setup.units = {{10, 100, 1, at(0), math::identity_quat(), {}},
        {11, 101, 1, at(0), math::identity_quat(), {}}, {20, 102, 2, at(2000), math::identity_quat(), {}}};
    setup.squadrons = {{10, {11}}};
    tactical::MotionTable motion;
    tactical::CraftProfile craft;
    craft.type_id = 101;
    craft.max_speed = fixed(5); craft.min_speed = fixed(1); craft.rate_of_turn = fixed(6);
    craft.lift = fixed(6); craft.thrust = fixed(1); craft.roll_rate = fixed(6);
    craft.bank_angle = fixed(70); craft.strafe_distance = fixed(200);
    motion.squadrons.craft = {craft};
    motion.squadrons.squadrons = {{100, {101}, {at(0)}, fixed(1000), fixed(200), fixed(300), fixed(20)}};
    std::optional<std::uint64_t> baseline_arrival;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auto created = tactical::TacticalSession::create(setup, {}, {}, motion);
        expect(created.has_value(), "EX-31: squadron movement world starts");
        if (!created) return;
        auto world = std::move(created).value();
        auto host = ai::make_tactical_host();
        auth::ModuleManifest modules;
        expect(modules.add("MOVEMENT.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n").has_value(),
            "EX-31: movement handler loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "MOVEMENT.LUA").has_value(),
            "EX-31: movement handler instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "FormationMove";
        ai::Engine engine(host, {}, {definition});
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::movement_fixture(engine, scripts.value(), sequence);
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const tactical::PlayerCommand move{{0, 1, 0}, {10}, tactical::MovePayload{at(300)}};
        expect(world.submit(move).has_value(), "EX-31: ordinary squadron move submits");
        for (int tick = 0; tick < 2; ++tick) expect(world.step(executor).has_value(), "EX-31: squadron moves");
        host->view = foc::detail::build_view(world, *world.snapshot());
        expect(host->view->find(10)->formation_moving && !host->view->find(10)->moving,
            "EX-31: travelling container has formation motion and no ship Motion");
        ai::EngineContractAccess::movement_service(engine, 2);
        expect(!ai::EngineContractAccess::movement_finished(engine, 1) &&
            !ai::EngineContractAccess::movement_finished(engine, 2) && ai::EngineContractAccess::event_sequence(engine) == 0,
            "EX-31: move and ambush blocks do not finish or signal two ticks after departure");
        while (world.completed_tick() < 5000 && world.squadron_state(10)->mode == tactical::SquadronMode::move) {
            if (!world.step(executor)) { expect(false, "EX-31: travelling world steps"); return; }
        }
        expect(world.squadron_state(10)->mode != tactical::SquadronMode::move,
            "EX-31: squadron reaches its destination within the bounded fixture");
        host->view = foc::detail::build_view(world, *world.snapshot());
        ai::EngineContractAccess::movement_service(engine, static_cast<std::int64_t>(world.completed_tick()));
        expect(ai::EngineContractAccess::movement_finished(engine, 1) &&
            ai::EngineContractAccess::movement_finished(engine, 2) && ai::EngineContractAccess::event_sequence(engine) == 2,
            "EX-31: both blocks finish and signal exactly once at actual formation arrival");
        ai::EngineContractAccess::movement_service(engine, static_cast<std::int64_t>(world.completed_tick() + 1));
        expect(ai::EngineContractAccess::event_sequence(engine) == 2, "EX-31: completed formation blocks do not signal again");
        if (!baseline_arrival) baseline_arrival = world.completed_tick();
        else expect(world.completed_tick() == *baseline_arrival, "EX-31: arrival agrees on 1/2/4/8 workers");
    }
}

// EX-36 / WBP-40: arrival is not completion of guarding a live capture point.
void guard_block_lifecycle() {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
    for (const bool lose_force : {false, true}) {
        World world;
        ai::ViewUnit target;
        target.id = 20;
        world.view->units.push_back(target);
        auth::ModuleManifest modules;
        expect(modules.add("GUARD.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n"
            "function MainForce_Current_Target_Destroyed(tf) end\n").has_value(), "EX-36: guard handlers load");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "GUARD.LUA").has_value(), "EX-36: guard instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "GuardCapturePoint";
        ai::Engine engine(world.host, {}, {definition});
        std::uint64_t sequence = 0;
        ai::EngineContractAccess::guard_fixture(engine, scripts.value(), sequence);
        ai::EngineContractAccess::movement_service(engine, 2);
        expect(!ai::EngineContractAccess::movement_finished(engine, 1)
            && ai::EngineContractAccess::movement_finished(engine, 2) && ai::EngineContractAccess::event_sequence(engine) == 2,
            "EX-36: stopped movers signal arrival once; only the ordinary move completes");
        for (const auto owner : {1U, 2U, 0U}) {
            world.view->units.back().owner = owner;
            ai::EngineContractAccess::movement_service(engine, 1802);
            expect(!ai::EngineContractAccess::movement_finished(engine, 1) && ai::EngineContractAccess::event_sequence(engine) == 2,
                "EX-36: guard survives neutral, friendly and hostile ownership after its mover list empties");
        }
        if (lose_force) ai::EngineContractAccess::empty_guard_force(engine);
        else world.view->units.pop_back();
        ai::EngineContractAccess::movement_service(engine, 1803);
        expect(ai::EngineContractAccess::movement_finished(engine, 1)
            && ai::EngineContractAccess::event_sequence(engine) == (lose_force ? 2U : 3U), "EX-36: force loss or target death releases the guard");
        ai::EngineContractAccess::movement_service(engine, 1804);
        expect(ai::EngineContractAccess::event_sequence(engine) == (lose_force ? 2U : 3U), "EX-36: released guard emits no duplicate events");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        expect(scripts.value().service(executor).has_value(), "EX-36: lifecycle callbacks service on every worker count");
    }
    }
}

// EX-31 (#3087): an attack on a non-hostile object refuses its squadron and TARGETING movers (WHZ-51).
// A refused mover gets no order and signals nothing. With every mover refused, the block waits until
// the force empties, the target dies or its new owner is no enemy. Other movers move to the object.
void refused_attack_block() {
    for (const bool targeting : {true, false}) {
    for (const std::uint32_t owner : {4U, 2U}) { // the neutral player's, an enemy's
        World world;
        world.host->setup.players.push_back(foc::AiPlayer{4, "NEUTRAL", true, false, ""});
        world.host->players.clear();
        for (const foc::AiPlayer& player : world.host->setup.players) world.host->players.emplace(player.player, &player);
        world.host->setup.content.types.front().targeting = targeting;
        ai::ViewUnit target;
        target.id = 20;
        target.type = 1;
        target.owner = owner;
        world.view->units.push_back(target);
        auth::ModuleManifest modules;
        expect(modules.add("ATTACK.LUA", "function MainForce_Unit_Move_Finished(tf, unit) end\n").has_value(),
            "EX-31: attack handler loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        expect(scripts && scripts.value().create_instance(1, "ATTACK.LUA").has_value(), "EX-31: attack instance starts");
        if (!scripts) return;
        ai::PlanDef definition;
        definition.name = "BombingRun";
        ai::Engine engine(world.host, {}, {definition});
        std::uint64_t sequence = 0;
        const auto& block = ai::EngineContractAccess::attack_fixture(engine, scripts.value(), sequence, 20);
        const bool refused = targeting && owner == 4;
        expect(block.movers.empty() == refused && (block.held_owner == 4) == refused && !block.finished
            && ai::EngineContractAccess::order_count(engine) == (refused ? 0U : 1U),
            "EX-31: only a TARGETING mover attacking a non-hostile object is refused, without an order");
        if (!refused) continue;
        for (const std::int64_t frame : {2, 1800}) {
            ai::EngineContractAccess::movement_service(engine, frame);
            expect(!block.finished && ai::EngineContractAccess::event_sequence(engine) == 0,
                "EX-31: a fully refused attack waits silently");
        }
        world.view->units.back().owner = 1;
        ai::EngineContractAccess::movement_service(engine, 1801);
        expect(block.finished && ai::EngineContractAccess::event_sequence(engine) == 0,
            "EX-31: the target's new non-enemy owner ends the wait");
    }
    }
}

class FindTargetFixture final : public ai::FindTargetQueries {
public:
    const ai::Engine* engine{};
    bool galactic_available{true};
    bool current_available{true};
    bool map_source{true};
    std::optional<ai::FindTargetPath> path;
    const ai::Engine* manager(ai::FindTargetManager mode) const override {
        return (mode == ai::FindTargetManager::galactic ? galactic_available : current_available) ? engine : nullptr;
    }
    const ai::Target* source_target(const ai::Engine& selected, sim::EntityId object) const override {
        // FT-05: fixture planet/direct-parent/fleet-parent lookup. Production
        // campaign containment and execution routes are supplied by S4.
        if (!map_source) return nullptr;
        if (object == 10 || object == 12 || object == 13) return selected.target(4);
        return selected.target_of_object(object);
    }
    std::optional<ai::FindTargetPath> execution_path(const ai::Engine&, tactical::PlayerId,
        const ai::Target&, const ai::Target& candidate) const override {
        if (candidate.id == 5 && path) { auto other = *path; other.distance = ai::real(1); return other; }
        return path;
    }
};

void find_target_reach_contract() {
    struct Case {
        std::string name, call;
        bool error{}, answer{true};
        std::optional<ai::FindTargetPath> path{};
        bool galactic{true}, current{true}, map_source{true}, enemy{}, competing{};
        int tolerance{1000};
        std::uint64_t picked_id{1};
        bool queries{true};
    };
    const auto route = [](ai::FindTargetService service, int threat = 0) {
        ai::FindTargetPath value; value.service = service; value.distance = ai::real(900000);
        value.threat = ai::real(threat); return value;
    };
    const auto query = [](std::string policy, std::string tail = ", 1, object", std::string flags = "Friendly_Unit") {
        return "FindTarget.Reachable_Target(player, 'Positive', '" + flags + "', '" + policy + "'" + tail + ")";
    };
    std::vector<Case> cases;
    cases.push_back({"FT-04 omitted probability/source", query("Any", "")});
    cases.push_back({"FT-04 explicit best", query("Any", ", 1")});
    cases.push_back({"FT-04 optional probability", query("Any", ", 0.5")});
    for (const auto* value : {"0", "-0.1", "1.1", "'bad'"})
        cases.push_back({"FT-04 invalid probability", query("Any", std::string(", ") + value), true});
    for (const auto* call : {
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Any', 1, object, 0, 0)",
        "FindTarget.Reachable_Target(object, 'Positive', 'Friendly_Unit', 'Any')",
        "FindTarget.Reachable_Target(human, 'Positive', 'Friendly_Unit', 'Any')",
        "FindTarget.Reachable_Target(player, 'Unknown', 'Friendly_Unit', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Global', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Unknown', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', '', 'Any')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Unknown')",
        "FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Any|No_Threat')"})
        cases.push_back({"FT-04/13 invalid arguments", call, true});
    for (const auto* policy : {"Friendly_only", "Friendly_Only", "Friendly_Ignore_Threat", "Enemy_destination",
        "Enemy_undefended", "Enemy_Undefended", "Single_Hop_Disconnected", "Any", "No_Threat", "Low_Threat",
        "Medium_Threat", "High_Threat", "Any_Threat", "  aNy_ThReAt  "})
        cases.push_back({"FT-07/13 accepted policy", query(policy), false, true, route(ai::FindTargetService::galactic)});
    auto absent_galactic = Case{"FT-07 Any requires galactic manager", query("Any", ""), true};
    absent_galactic.galactic = false; cases.push_back(absent_galactic);
    absent_galactic.name = "FT-07 Any_Threat selects current manager"; absent_galactic.call = query("Any_Threat", "");
    absent_galactic.error = false; cases.push_back(absent_galactic);
    auto absent_current = Case{"FT-07 current manager absent", query("Any_Threat", ""), true};
    absent_current.current = false; cases.push_back(absent_current);
    for (const auto* source : {"object", "planet_parent", "fleet_parent", "origin"})
        cases.push_back({"FT-05/14 resolved source", query("Friendly_Only", std::string(", 1, ") + source), false, true,
            route(ai::FindTargetService::galactic)});
    cases.push_back({"FT-05 candidate remains equation target",
        "FindTarget.Reachable_Target(player, 'Candidate', 'Friendly_Unit', 'Any', 1, object)", false, true,
        route(ai::FindTargetService::galactic)});
    for (const auto* source : {"nil", "false", "42", "dead_object", "dead_target", "stale_target"})
        cases.push_back({"FT-05 invalid source", query("Any", std::string(", 1, ") + source), true});
    auto blocked = route(ai::FindTargetService::galactic, 1);
    cases.push_back({"FT-05 no source skips cost", query("Friendly_Only", ", 1"), false, true, blocked});
    auto unmapped = Case{"FT-05 live unmapped source skips cost", query("Friendly_Only"), false, true, blocked};
    unmapped.map_source = false; cases.push_back(unmapped);
    cases.push_back({"FT-06 omitted radius", query("Any"), false, true, route(ai::FindTargetService::galactic)});
    cases.push_back({"FT-06 zero radius", query("Any", ", 1, object, 0")});
    cases.push_back({"FT-06 boundary equality in 3D", query("Any", ", 1, object, 13")});
    cases.push_back({"FT-06 negative radius", query("Any", ", 1, object, -13")});
    cases.push_back({"FT-06 just outside radius", query("Any", ", 1, object, 12.999"), false, false});
    cases.push_back({"FT-06 numeric radius required", query("Any", ", 1, object, 'bad'"), true});
    unmapped.name = "FT-06 nonzero radius needs source"; unmapped.call = query("Any", ", 1, object, 1");
    unmapped.error = true; cases.push_back(unmapped);
    for (const auto* policy : {"Friendly_Only", "Enemy_destination", "Enemy_undefended", "No_Threat"}) {
        cases.push_back({"FT-08 strict threat rejects", query(policy), false, false, blocked});
        auto boundary = route(ai::FindTargetService::galactic);
        boundary.threat = ai::to_single(ai::real(1) / ai::real(10000));
        cases.push_back({"FT-08 strict equality passes", query(policy), false, true, boundary});
    }
    for (const auto* policy : {"Any", "Friendly_Ignore_Threat", "Any_Threat"})
        cases.push_back({"FT-08 permissive threat", query(policy), false, true, blocked});
    auto same = blocked; same.same_target = true;
    cases.push_back({"FT-08 same planet cost zero", query("Friendly_Only"), false, true, same});
    auto prefix = route(ai::FindTargetService::galactic, 100);
    cases.push_back({"FT-08 final hop omitted", query("Single_Hop_Disconnected"), false, true, prefix});
    prefix.prefix_threat = ai::real(1);
    cases.push_back({"FT-08 unsafe prefix rejected", query("Single_Hop_Disconnected"), false, false, prefix});
    auto enemy = Case{"FT-08 policy does not add owner filter", query("Friendly_Only", ", 1, object", "Enemy_Unit"),
        false, true, route(ai::FindTargetService::galactic)};
    enemy.enemy = true; cases.push_back(enemy);
    for (const auto* policy : {"Low_Threat", "Medium_Threat", "High_Threat"}) {
        cases.push_back({"FT-09 authored tolerance boundary", query(policy), false, true, route(ai::FindTargetService::land, 1000)});
        cases.push_back({"FT-09 above tolerance", query(policy), false, false, route(ai::FindTargetService::land, 1001)});
    }
    cases.push_back({"FT-09 land zero safety", query("No_Threat"), false, true, route(ai::FindTargetService::land)});
    cases.push_back({"FT-09 land threat rejected", query("No_Threat"), false, false, route(ai::FindTargetService::land, 1)});
    cases.push_back({"FT-09 galactic policy invalid in land", query("Any"), false, false, route(ai::FindTargetService::land)});
    auto invalid = route(ai::FindTargetService::land); invalid.source_valid = false;
    cases.push_back({"FT-09 source zone/mode required", query("Any_Threat"), false, false, invalid});
    for (const auto* policy : {"No_Threat", "Low_Threat", "Medium_Threat", "High_Threat", "Any_Threat"})
        cases.push_back({"FT-12 space bypass", query(policy)});
    auto empty = route(ai::FindTargetService::galactic); empty.empty = true;
    for (const auto* policy : {"Any", "Any_Threat", "Friendly_Ignore_Threat"})
        cases.push_back({"FT-15 permissive empty path", query(policy), false, true, empty});
    for (const auto* policy : {"Friendly_Only", "Enemy_destination", "Enemy_Undefended", "Single_Hop_Disconnected",
        "No_Threat", "Low_Threat", "Medium_Threat", "High_Threat"})
        cases.push_back({"FT-15 strict empty path", query(policy), false, false, empty});
    auto authored = Case{"FT-15 authored tolerance admits empty path", query("Low_Threat"), false, true, empty};
    authored.tolerance = 6553500; cases.push_back(authored);
    auto sentinel = route(ai::FindTargetService::galactic);
    sentinel.distance = ai::to_single(ai::real(1000000000000000000LL));
    cases.push_back({"FT-05 unreachable cost sentinel", query("Any"), false, false, sentinel});
    auto scored = Case{"FT-08 execution distance does not change score", query("Any"), false, true, route(ai::FindTargetService::galactic)};
    scored.competing = true; cases.push_back(scored);
    for (auto& test : cases) if (test.path && test.path->service == ai::FindTargetService::land) {
        const auto flag = test.call.find("Friendly_Unit");
        if (flag != std::string::npos) test.call.replace(flag, 13, "Tactical_Location");
        test.picked_id = 2;
    }
    auto space_default = Case{"FT-12 default space manager has no cost service", query("Any_Threat")};
    space_default.queries = false; cases.push_back(space_default);
    space_default.name = "FT-07 absent default galactic manager"; space_default.call = query("Any");
    space_default.error = true; cases.push_back(space_default);
    for (const auto workers : {1U, 2U, 4U, 8U}) for (const auto& test : cases) {
        World world;
        world.view->units.front().health = ai::real(0);
        auto destination = world.view->units.front(); destination.id = 11; destination.health = ai::real(1);
        destination.position = {sim::math::Fixed::from_integer(3).value(), sim::math::Fixed::from_integer(4).value(),
            sim::math::Fixed::from_integer(12).value()};
        if (test.enemy) { destination.owner = 1; destination.team = 1; }
        world.view->units.push_back(destination);
        for (const auto id : {12U, 13U}) { auto parent = world.view->units.front(); parent.id = id; world.view->units.push_back(parent); }
        if (test.competing) { auto other = destination; other.id = 14; world.view->units.push_back(other); }
        std::map<std::string, std::string, std::less<>> xml;
        for (const auto& file : ai::ai_xml_files()) xml.emplace(file, "<Root></Root>");
        const auto value = std::to_string(test.tolerance);
        xml["data/xml/gameconstants.xml"] = "<Root><Low_Threat_Reachability_Tolerance>" + value
            + "</Low_Threat_Reachability_Tolerance><Medium_Threat_Reachability_Tolerance>" + value
            + "</Medium_Threat_Reachability_Tolerance><High_Threat_Reachability_Tolerance>" + value
            + "</High_Threat_Reachability_Tolerance></Root>";
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> { return std::nullopt; };
        auto loaded = ai::load_ai_data(xml, converters);
        expect(loaded.has_value(), test.name + ": data loads"); if (!loaded) return;
        auto equations = ai::EquationSet::parse({{"test.xml", "<Root><Positive>1</Positive><Candidate>Variable_Target.Health > 0.5</Candidate></Root>"}}, converters);
        if (!equations) { expect(false, test.name + ": equations load"); return; }
        loaded.value().equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(loaded).value(), {}); world.host->engine = &engine;
        ai::EngineContractAccess::find_target_fixture(engine);
        ai::EngineContractAccess::target_fixture(engine, 4, 10);
        if (test.competing) ai::EngineContractAccess::add_find_target(engine, 5, 14);
        auto provider = std::make_shared<FindTargetFixture>(); provider->engine = &engine;
        provider->path = test.path; provider->galactic_available = test.galactic;
        provider->current_available = test.current; provider->map_source = test.map_source;
        if (test.queries) world.host->find_target_queries = provider;
        auth::ModuleManifest modules;
        expect(modules.add("REACH.LUA", "function Check(player, human, object, planet_parent, fleet_parent, origin, dead_object, dead_target, stale_target)\n result = "
            + test.call + "\nend\n").has_value(), test.name + ": module loads");
        auto scripts = auth::ScriptScheduler::create({}, std::move(modules));
        if (!scripts) { expect(false, test.name + ": scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "REACH.LUA").has_value(), test.name + ": bindings register");
        auth::ScriptEvent event; event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}}, auth::Value{auth::Handle{foc::handle_player, 1}},
            auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 12}},
            auth::Value{auth::Handle{foc::handle_game_object, 13}}, auth::Value{auth::Handle{ai::handle_ai_target, 4}},
            auth::Value{auth::Handle{foc::handle_game_object, 999}}, auth::Value{auth::Handle{ai::handle_ai_target, 999}},
            auth::Value{auth::Handle{ai::handle_ai_target, 3}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), test.name + ": event queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.empty() != test.error, test.name + ": error contract");
        if (report && !test.error && !report.value().diagnostics.empty())
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << test.name << ": " << diagnostic.message << '\n';
        if (!test.error) {
            const auto result = scripts.value().read_global(1, "result");
            const auto* picked = result && result.value() ? std::get_if<auth::Handle>(&result.value()->data) : nullptr;
            expect(test.answer ? picked && picked->kind == ai::handle_ai_target && picked->id == test.picked_id
                : result && result.value() && std::holds_alternative<std::monostate>(result.value()->data), test.name + ": answer contract");
        }
    }
}

// FT-01/02/03, GA-01: query the real Lua bindings, then pass the reachable
// answer straight to the stock callers' consumer APIs without converting it.
void reachable_target_contract() {
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto destination = world.view->units.front();
        destination.id = 11;
        destination.position.x = eawr::sim::math::Fixed::from_integer(25).value();
        world.view->units.push_back(destination);
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"query.xml", "<Root><Positive>1</Positive><Zero>0</Zero>"
            "<Unavailable>Variable_Target.Unavailable_Query</Unavailable></Root>"}}, converters);
        expect(equations.has_value(), "FT-02: query equations parse");
        if (!equations) return;
        ai::AiData data;
        data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        world.host->engine = &engine;
        auto query_provider = std::make_shared<FindTargetFixture>();
        query_provider->engine = &engine;
        world.host->find_target_queries = query_provider;
        ai::EngineContractAccess::find_target_fixture(engine);
        auth::ModuleManifest modules;
        expect(modules.add("QUERY.LUA",
            "function PassTarget(value) return value end\n"
            "function Check(player, tf, object)\n"
            " target = FindTarget.Reachable_Target(player, 'Positive', 'Friendly_Unit', 'Any', 1)\n"
            " valid = target.Is_Valid()\n"
            " converted = target.Get_Game_Object()\n"
            " returned = PassTarget(target)\n"
            " object_move = object.Move_To(returned)\n"
            " force_move = tf.Move_To(returned)\n"
            " produced = tf.Produce_Force(returned)\n"
            " fired = object.Fire_Special_Weapon(returned, player)\n"
            " force_fired = tf.Fire_Special_Weapon('SHIP', returned)\n"
            " plain = FindTarget(tf, 'Positive', 'Friendly_Unit', 1)\n"
            " region = FindTarget.Reachable_Target(player, 'Positive', 'Tactical_Location', 'Any', 1)\n"
            " region_valid = region.Is_Valid()\n"
            " region_object = region.Get_Game_Object()\n"
            " plain_region = FindTarget(tf, 'Positive', 'Tactical_Location', 1)\n"
            " distance_object = object.Get_Distance(converted)\n"
            " distance_wrapped = object.Get_Distance(returned)\n"
            " distance_region = converted.Get_Distance(region)\n"
            " distance_force = converted.Get_Distance(tf)\n"
            " distance_position = object.Get_Distance(converted.Get_Position())\n"
            " empty = FindTarget.Reachable_Target(player, 'Positive', 'Enemy_Unit', 'Any', 1)\n"
            " zero = FindTarget.Reachable_Target(player, 'Zero', 'Friendly_Unit', 'Any', 1)\n"
            " unavailable = FindTarget.Reachable_Target(player, 'Unavailable', 'Friendly_Unit', 'Any', 1)\n"
            " continued = true\n"
            "end\n").has_value(), "FT-02: query module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "FT-02: query scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scripts.value(), world.host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), world.host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "QUERY.LUA").has_value(), "FT-02: query bindings register");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}},
            auth::Value{auth::Handle{ai::handle_taskforce, 1}}, auth::Value{auth::Handle{foc::handle_game_object, 10}}};
        expect(scripts.value().submit_event(std::move(event)).has_value(), "FT-02: query queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto report = scripts.value().service(executor);
        expect(report && report.value().diagnostics.size() == 1
            && report.value().diagnostics.front().code == auth::codes::unsupported_api
            && report.value().diagnostics.front().message.find("skipped 1 candidates") != std::string::npos,
            "FT-02/PE-28: an unavailable selected perception emits one counted diagnostic without aborting consumers");
        const auto check_handle = [&](const char* name, std::uint32_t kind, std::uint64_t id) {
            const auto value = scripts.value().read_global(1, name);
            const auto* handle = value && value.value() ? std::get_if<auth::Handle>(&value.value()->data) : nullptr;
            expect(handle && handle->kind == kind && handle->id == id, std::string("FT-01/02: ") + name);
        };
        check_handle("target", ai::handle_ai_target, 1);
        check_handle("returned", ai::handle_ai_target, 1);
        check_handle("converted", foc::handle_game_object, 11);
        check_handle("plain", foc::handle_game_object, 11);
        check_handle("region", ai::handle_ai_target, 2);
        check_handle("plain_region", ai::handle_ai_target, 2);
        for (const char* name : {"distance_object", "distance_wrapped", "distance_region", "distance_force", "distance_position"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::get_if<Real>(&value.value()->data)
                && std::get<Real>(value.value()->data) == ai::real(25),
                "EX-51: unit distance extracts live objects, wrapped objects, region centres, TaskForces and positions");
        }
        for (const char* name : {"valid", "region_valid"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::get_if<bool>(&value.value()->data)
                && std::get<bool>(value.value()->data), "FT-02: reachable targets support validity tests");
        }
        for (const char* name : {"empty", "zero", "region_object", "unavailable"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::holds_alternative<std::monostate>(value.value()->data),
                "FT-02/03: empty, zero-score and region conversion answer nil");
        }
        const auto continued = scripts.value().read_global(1, "continued");
        expect(continued && continued.value() && std::get<bool>(continued.value()->data), "PE-28: unavailable query leaves the caller running");
        expect(report && report.value().commands.size() == 3, "FT-02: object move, force move and force production stage");
        if (report && report.value().commands.size() == 3) {
            const auto& move = report.value().commands[0];
            expect(move.verb == foc::verb_move && move.arguments.size() == 5
                && std::get<Real>(move.arguments[2].data) == ai::real(25), "FT-02: object move resolves wrapped position");
            const auto& force = report.value().commands[1].arguments;
            expect(force.size() == 4 && std::get<std::string>(force[0].data) == "move"
                && std::get<Real>(std::get<std::vector<auth::Value>>(force[3].data)[0].data) == ai::real(25),
                "FT-02: force move resolves wrapped position");
            const auto& produce = report.value().commands[2].arguments;
            expect(produce.size() == 2 && std::get<std::string>(produce[0].data) == "produce",
                "FT-02: force production accepts the unconverted staging target");
        }
    }
}

// WGA-34/35: run installed, unmodified stock redistribution Lua. The fixture
// supplies planet/freestore/perception services; target search and handle
// conversion use the production bindings. This is not a full campaign host.
void installed_galactic_freestore() {
    std::string game_root;
#ifdef _WIN32
    char* raw = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&raw, &size, "EAWR_EAW_GAME_ROOT") == 0 && raw != nullptr) game_root = raw;
    std::free(raw);
#else
    if (const char* raw = std::getenv("EAWR_EAW_GAME_ROOT")) game_root = raw;
#endif
    if (game_root.empty()) { std::cout << "SKIPPED: EAWR_EAW_GAME_ROOT is not set\n"; return; }
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto& [layer, folder] : {std::pair{"expansion", "corruption"}, std::pair{"base", "GameData"}}) {
        auto mounted = eawr::vfs::resolve_manifest_mount(layer, std::filesystem::path(game_root) / folder / "Data");
        expect(mounted.has_value(), "WGA-34: installed data layer resolves");
        if (!mounted) return;
        mounts.push_back(std::move(mounted).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    expect(filesystem.has_value(), "WGA-34: installed data mounts");
    if (!filesystem) return;
    auto libraries = filesystem.value().enumerate("data/scripts/library", ".lua");
    expect(libraries.has_value(), "WGA-34: installed libraries enumerate");
    if (!libraries) return;
    std::map<std::string, std::string> stock;
    const auto load = [&](const std::string& path) {
        const auto bytes = filesystem.value().open(path);
        expect(bytes.has_value(), "WGA-34: installed Lua opens: " + path);
        if (bytes) stock.emplace(std::filesystem::path(path).filename().string(),
            std::string(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size()));
    };
    for (const auto& library : libraries.value()) load(library.canonical_path);
    load("data/scripts/freestore/galacticfreestore.lua");
    load("data/scripts/freestore/galacticherofreestore.lua");
    if (failures != 0) return;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        World world;
        auto planet = world.view->units.front();
        planet.id = 700;
        world.view->units.push_back(planet);
        planet.id = 12; // a transport for the land redistribution path
        world.view->units.insert(world.view->units.begin() + 1, planet);
        auto mode = std::make_shared<ModeWorld>();
        mode->friendly_planet_fixture = true;
        mode->world_view = world.view;
        world.host->ai_world = mode;
        const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
            return std::nullopt;
        };
        auto equations = ai::EquationSet::parse({{"fixture.xml", "<Root><Space_Priority_Defense_Score>1</Space_Priority_Defense_Score>"
            "<Ground_Priority_Defense_Score>1</Ground_Priority_Defense_Score></Root>"}}, converters);
        if (!equations) { expect(false, "WGA-34: fixture equations parse"); return; }
        ai::AiData data;
        data.equations = std::move(equations).value();
        ai::Engine engine(world.host, std::move(data), {});
        world.host->engine = &engine;
        auto query_provider = std::make_shared<FindTargetFixture>();
        query_provider->engine = &engine;
        world.host->find_target_queries = query_provider;
        auto& player = ai::EngineContractAccess::collect_fixture(engine);
        ai::EngineContractAccess::build_targets(engine, player);
        auth::ModuleManifest modules;
        for (const auto& [path, bytes] : stock) expect(modules.add(path, bytes).has_value(), "WGA-34: installed module registers");
        expect(modules.add("REDISTRIBUTE.LUA", "require('galacticfreestore')\n"
            "function Check(player, store, ship, transport)\n"
            " PlayerObject = player\n FreeStore = store\n"
            " ship_moved = MoveUnit(ship)\n transport_moved = MoveUnit(transport)\n"
            " target_valid = TestValid(FindTarget.Reachable_Target(player, 'Space_Priority_Defense_Score', 'Friendly', 'Friendly_Only', 1))\n"
            "end\n").has_value(), "WGA-34: stock redistribution driver loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(modules));
        if (!scripts) { expect(false, "WGA-34: scheduler starts"); return; }
        auto& scheduler = scripts.value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::detail::register_plan_bindings(scheduler, world.host, errors);
        foc::tactical_ai_detail::register_methods(scheduler, world.host, errors);
        const auto fixed_result = [](auth::Value value) {
            return [value](auth::BindingContext&, const auth::ValueList&) -> eawr::core::Result<auth::ValueList> {
                return eawr::core::Result<auth::ValueList>::success({value});
            };
        };
        expect(scheduler.register_binding("_ScriptMessage", [](auth::BindingContext&, const auth::ValueList&)
            -> eawr::core::Result<auth::ValueList> { return eawr::core::Result<auth::ValueList>::success({}); }).has_value(), "WGA-34: fixture debug sink registers");
        expect(scheduler.register_binding("EvaluatePerception", [](auth::BindingContext&, const auth::ValueList& args)
            -> eawr::core::Result<auth::ValueList> {
                const auto* name = args.empty() ? nullptr : std::get_if<std::string>(&args[0].data);
                const bool global = name && name->find("Global") != std::string::npos;
                return eawr::core::Result<auth::ValueList>::success({auth::Value{ai::real(global ? 1600 : 0)}});
            }).has_value(), "WGA-34: fixture force provider registers");
        // WGA-436/437/439: the production pool-fed callbacks answer the stock destination helpers. Only the
        // native planetary predicate owner (WGA-438) stays a fixture: this contract has no native action owner.
        {
            namespace ga = eawr::script::foc::galactic_ai;
            auto input = std::make_shared<ga::FreestoreInput>();
            auto pool = std::make_shared<ga::FreestorePool>();
            pool->qualified = true;
            ga::FreestoreObject ship; ship.id = 10; ship.type = 1; ship.owner = 2; ship.planet = 500;
            auto transport = ship; transport.id = 12; transport.type = 2; transport.transport = true;
            pool->objects = {ship, transport};
            pool->surfaces = {{700, 0, 10, true}};
            input->pool = pool;
            input->planet_restrictions[700] = {};
            input->type_categories[2] = 0;
            expect(ga::register_freestore_object_methods(scheduler, input).has_value(),
                "WGA-34/35: production object callbacks register");
        }
        expect(scheduler.register_method(foc::handle_game_object, "Get_Is_Planet_AI_Usable",
            fixed_result(auth::Value{true})).has_value(), "WGA-34: fixture planet registers");
        constexpr std::uint32_t fixture_store = 900;
        expect(scheduler.register_method(fixture_store, "Is_Unit_Safe",
            fixed_result(auth::Value{true})).has_value(), "WGA-35: fixture safety provider registers");
        expect(scheduler.register_method(fixture_store, "Move_Object", [](auth::BindingContext& context, const auth::ValueList& args)
            -> eawr::core::Result<auth::ValueList> {
                context.issue_command("fixture.redistribute", {args[1], args[2]});
                return eawr::core::Result<auth::ValueList>::success({});
            }).has_value(), "WGA-34: fixture movement sink registers");
        expect(errors.empty() && scheduler.create_instance(1, "REDISTRIBUTE.LUA").has_value(), "WGA-34: stock instance starts");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0};
        event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        event.arguments = {auth::Value{auth::Handle{foc::handle_player, 2}}, auth::Value{auth::Handle{fixture_store, 1}},
            auth::Value{auth::Handle{foc::handle_game_object, 10}}, auth::Value{auth::Handle{foc::handle_game_object, 12}}};
        expect(scheduler.submit_event(std::move(event)).has_value(), "WGA-34: stock service queues");
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto report = scheduler.service(executor);
        expect(report && report.value().diagnostics.empty(), "WGA-34/35: stock first planet picks have no Lua fault");
        if (report) for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.message << '\n';
        expect(report && report.value().commands.size() == 2, "WGA-34/35: stock ship and transport both redistribute");
        if (report) for (const auto& command : report.value().commands) {
            const auto* target = command.arguments.size() == 2 ? std::get_if<auth::Handle>(&command.arguments[1].data) : nullptr;
            expect(command.verb == "fixture.redistribute" && target && target->kind == foc::handle_game_object && target->id == 700,
                "WGA-34/35: stock conversion sends the picked planet to the freestore");
        }
        for (const char* name : {"ship_moved", "transport_moved", "target_valid"}) {
            const auto value = scheduler.read_global(1, name);
            const auto* result = value && value.value() ? std::get_if<bool>(&value.value()->data) : nullptr;
            expect(result && *result, "WGA-34/35: stock movement and TestValid succeed");
        }
    }
}

// WTA-23, WTA-26 to WTA-28 (G-03, #731): an Acclamator of a destroy plan attacks the Rebel
// station from the quadrant its enemies defend least, then attacks the station.
void attack_positioning() {
    namespace math = eawr::sim::math;
    const auto at = [](std::int64_t x, std::int64_t y) {
        return math::Vec3{math::Fixed::from_integer(x).value(), math::Fixed::from_integer(y).value(), {}};
    };
    auto host = std::make_shared<ai::Host>();
    auto view = std::make_shared<ai::WorldView>();
    const auto type = [&](tactical::TypeId id, const char* name, std::int64_t power, std::int64_t reach,
                          std::int64_t half, bool locomotor) {
        foc::AiType entry;
        entry.type_id = id;
        entry.name = name;
        entry.category_bits = 2;
        entry.combat_power = ai::real(power);
        entry.max_attack_distance = ai::real(reach);
        entry.bounds_half_x = entry.bounds_half_y = ai::real(half);
        entry.locomotor = locomotor;
        host->setup.content.types.push_back(entry);
    };
    type(1, "REBEL_SHIP", 1000, 500, 50, true);
    type(2, "REBEL_STATION", 0, 0, 300, false);
    type(3, "ACCLAMATOR", 0, 1000, 200, true);
    for (const foc::AiType& entry : host->setup.content.types) {
        host->types.emplace(entry.type_id, &entry);
        host->types_by_name.emplace(entry.name, &entry);
    }
    host->setup.players = {foc::AiPlayer{1, "REBEL", false, false, ""}, foc::AiPlayer{2, "EMPIRE", false, true, ""}};
    for (const foc::AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    view->players = {foc::detail::ViewPlayer{1, 1, 0}, foc::detail::ViewPlayer{2, 2, 1}};
    const auto unit = [&](sim::EntityId id, tactical::TypeId kind, tactical::PlayerId owner, math::Vec3 position) {
        ai::ViewUnit entry;
        entry.id = id;
        entry.type = kind;
        entry.owner = owner;
        entry.position = position;
        entry.visible_to = 3;
        view->units.push_back(entry);
    };
    unit(20, 2, 1, at(0, 0));       // the station
    unit(21, 1, 1, at(-800, 0));    // a Rebel ship on its near (-x) side
    unit(30, 3, 2, at(-3000, 0));   // the Acclamator, attacking the station
    view->units.back().order = tactical::OrderKind::attack;
    view->units.back().order_target = 20;
    host->view = view;
    ai::Engine engine(host, {}, {});
    ai::EngineContractAccess::positioning_fixture(engine);
    const auto orders = [&] { return ai::EngineContractAccess::point_defence_orders(engine); };
    const auto number = [](const auth::Value& value) { return std::get<ai::Real>(value.data); };

    ai::EngineContractAccess::positioning_service(engine, 5);
    expect(orders() == 0, "WTA-23: a unit without the flag is not positioned");
    ai::EngineContractAccess::enable_positioning(engine, 10, true);
    expect(ai::EngineContractAccess::positioning_on(engine, 30), "WTA-23: Enable_Attack_Positioning(true) flags the members");
    ai::EngineContractAccess::positioning_service(engine, 10);
    expect(orders() == 0, "WTA-27: the first positioning waits for the next service");
    ai::EngineContractAccess::positioning_service(engine, 11);
    const auth::ScriptCommand* order = ai::EngineContractAccess::last_order(engine);
    expect(orders() == 1 && order != nullptr && order->verb == foc::verb_attack_move && order->arguments.size() == 5,
        "WTA-27: beyond its attack distance of the ideal position the unit attack-moves to a point");
    if (order != nullptr && order->arguments.size() == 5) {
        // WTA-28: s = min(|(300, 300)|, 1000 / 2) = 424.26; the +x boxes hold no Rebel force and
        // the first of them, +x+y, keeps the tie.
        const ai::Real x = number(order->arguments[2]);
        const ai::Real y = number(order->arguments[3]);
        expect(ai::real(424) < x && x < ai::real(425) && ai::real(424) < y && y < ai::real(425)
                && number(order->arguments[4]) == ai::Real{},
            "WTA-28: the ideal position is the centre of the least-defended quadrant box, at height 0");
    }
    ai::EngineContractAccess::positioning_service(engine, 160);
    expect(orders() == 1, "WTA-27: positioning runs every 150 frames");
    // The sim has taken the point attack move; the unit arrived on the quiet side.
    view->units.back().position = at(400, 400);
    view->units.back().order = tactical::OrderKind::attack_move;
    view->units.back().order_target = sim::invalid_entity_id;
    ai::EngineContractAccess::positioning_service(engine, 161);
    order = ai::EngineContractAccess::last_order(engine);
    expect(orders() == 2 && order != nullptr && order->verb == foc::verb_attack && order->arguments.size() == 3
            && std::get<auth::Handle>(order->arguments[2].data).id == 20,
        "WTA-27: within its attack distance it attacks the target");
    view->units.back().order = tactical::OrderKind::attack;
    view->units.back().order_target = 20;
    ai::EngineContractAccess::positioning_service(engine, 311);
    expect(orders() == 2, "an attack the unit already has is not ordered again");
    view->units.back().order = tactical::OrderKind::move;
    view->units.back().order_target = sim::invalid_entity_id;
    ai::EngineContractAccess::positioning_service(engine, 461);
    expect(orders() == 2, "WTA-27: a formation without an attack target is not positioned");
    // U-05/WHZ-51: retaining an object across a point move must not turn an ownership
    // change into an attack on an ally or neutral. The query retains the live destination,
    // so a later ownership change back to hostile can expose it again.
    std::int64_t frame = 611;
    const auto refuse_retained = [&](tactical::PlayerId owner, bool neutral, bool neutral_mover = false) {
        auto& target = view->units.front();
        auto& mover = view->units.back();
        target.owner = 1;
        host->setup.players.front().neutral = false;
        host->setup.players.back().neutral = false;
        mover.position = at(-3000, 0);
        mover.order = tactical::OrderKind::attack_move;
        mover.order_target = 20;
        const auto before = orders();
        ai::EngineContractAccess::positioning_service(engine, frame);
        expect(orders() == before + 1, "U-05: hostile object positioning retains its point-move target");
        target.owner = owner;
        host->setup.players.front().neutral = neutral;
        host->setup.players.back().neutral = neutral_mover;
        mover.position = at(400, 400);
        mover.order_target = sim::invalid_entity_id;
        ai::EngineContractAccess::positioning_service(engine, frame + 150);
        expect(orders() == before + 1, "WHZ-51: positioning silently refuses a retained nonhostile target");
        target.owner = 1;
        host->setup.players.front().neutral = false;
        host->setup.players.back().neutral = false;
        ai::EngineContractAccess::positioning_service(engine, frame + 151);
        const auto* resumed = ai::EngineContractAccess::last_order(engine);
        expect(orders() == before + 2 && resumed != nullptr && resumed->verb == foc::verb_attack
            && resumed->arguments.size() == 3
            && std::get<auth::Handle>(resumed->arguments[2].data).id == 20,
            "U-05/FC-20: hostility returning resumes the due retained destination without another 150-frame wait");
        frame += 450;
    };
    refuse_retained(2, false);
    refuse_retained(0, false);
    refuse_retained(1, true);
    refuse_retained(1, false, true);
    const auto replacement = [&](bool nonhostile, bool scripted) {
        auto& target = view->units.front();
        auto& mover = view->units.back();
        target.owner = 1;
        mover.position = at(-3000, 0);
        mover.order = tactical::OrderKind::attack;
        mover.order_target = 20;
        const auto before = orders();
        ai::EngineContractAccess::positioning_service(engine, frame);
        expect(orders() == before + 1, "U-05: replacement fixture starts with a retained positioning object");
        mover.order = tactical::OrderKind::attack_move;
        mover.order_target = sim::invalid_entity_id;
        if (nonhostile) {
            target.owner = 2;
            ai::EngineContractAccess::positioning_service(engine, frame + 150);
            expect(orders() == before + 1, "WHZ-51: replacement fixture first refuses its nonhostile object");
        }
        const auto point = at(3000, -3000);
        if (scripted) ai::EngineContractAccess::positioning_script_point_order(engine, point);
        else ai::EngineContractAccess::positioning_point_order(engine, point);
        const auto after_replacement = orders();
        if (!scripted) expect(after_replacement == before + 2, "EX-30: the TaskForce emits the replacement point order");
        target.owner = 1;
        ai::EngineContractAccess::positioning_service(engine, frame + (nonhostile ? 151 : 150));
        expect(orders() == after_replacement,
            "U-05: a replacement point attack-move cannot be redirected to the previous positioning object");
        frame += 450;
    };
    replacement(false, false);
    replacement(true, false);
    replacement(true, true);
    ai::EngineContractAccess::enable_positioning(engine, frame, false);
    expect(!ai::EngineContractAccess::positioning_on(engine, 30), "WTA-23: Enable_Attack_Positioning(false) clears the flag");
}

void target_choice() {
    int draws = 0;
    const auto choose = [&draws](std::vector<Real> scores, Real fraction, Real unit) {
        return ai::choose_target(scores, fraction, [&draws, unit] {
            ++draws;
            return unit;
        });
    };
    const Real half = ai::real(1) / ai::real(2);
    expect(!choose({}, half, 0), "FT-03: no candidate answers nil");
    expect(!choose({0, 0, 0}, half, 0) && !choose({0, 0, 0}, 0, 0), "FT-03: every score at 0 answers nil");
    expect(!choose({-1, -3}, half, 0) && !choose({-1, -3}, 0, 0) && !choose({-2, 0}, half, 0),
        "FT-03: every score at or below 0 answers nil");
    expect(draws == 0, "FT-03: no draw without a positive weight");
    expect(choose({0, 3, 0, -1}, half, 0) == 1U && draws == 0,
        "FT-03: a best with only zero or negative others is the answer, without a draw");
    expect(choose({0, 4}, half, 0) == 1U, "FT-03: a later positive score replaces the empty best");
    expect(choose({2, 2, 1}, 0, 0) == 0U && draws == 0, "FT-03: equal scores keep the earlier target as the best");
    // {2, 2, 1} at fraction 1/2: the draw weighs [#1: 2, #2: 1, best #0: 3 / (1/2) = 6], total 9.
    expect(choose({2, 2, 1}, half, 0) == 1U, "FT-01: the draw walks the others in list order first");
    expect(choose({2, 2, 1}, half, ai::real(1) / ai::real(4)) == 2U, "FT-01: 2.25 of 9 falls on the third target");
    expect(choose({2, 2, 1}, half, half) == 0U, "FT-01: the best weighs the others' total over the fraction");
    expect(draws == 3, "FT-01: one draw per weighted choice");
}

// DT-02: the decay step is read under its real tag name, and is 1 when the file has none.
void threat_decay_constant() {
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    const auto load = [&](const std::string& constants) {
        std::map<std::string, std::string, std::less<>> xml;
        for (const std::string& path : ai::ai_xml_files()) xml.emplace(path, "<Root></Root>");
        xml["data/xml/gameconstants.xml"] = constants;
        return ai::load_ai_data(xml, converters);
    };
    const auto authored = load("<Root><AI_SpaceThreatDecayStep>0.05</AI_SpaceThreatDecayStep></Root>");
    expect(authored && Real{} < authored.value().constants.threat_decay_step
            && authored.value().constants.threat_decay_step < ai::real(1) / ai::real(10),
        "DT-02: AI_SpaceThreatDecayStep is read from gameconstants.xml");
    const auto misspelled = load("<Root><AI_SpaceThreatDecay>0.05</AI_SpaceThreatDecay></Root>");
    expect(misspelled && misspelled.value().constants.threat_decay_step == ai::real(1),
        "DT-02: the engine default is 1 when the tag is absent");
}

void proximity_contracts() {
    namespace m = eawr::sim::math;
    const auto fixed = [](int value) { return m::Fixed::from_integer(value).value(); };
    auto host = std::make_shared<foc::detail::Host>();
    auto view = std::make_shared<foc::detail::WorldView>();
    view->players = {{1, 1, 0}, {2, 2, 1}, {3, 2, 2}};
    // IDs deliberately disagree with discovery order. Corner is inside the box
    // despite being outside a sphere. Bounds overlap admits an outside centre.
    for (const auto& [id, owner, type, x, y, z] : {
            std::array<int, 6>{10, 1, 100, 0, 0, 0}, {20, 2, 200, 50, 50, 50},
            {30, 3, 200, 60, 0, 0}, {40, 1, 200, 0, 0, 0}, {50, 2, 300, 0, 0, 0},
            {60, 2, 200, 0, 0, 52}, {70, 2, 400, 0, 0, 0}}) {
        foc::detail::ViewUnit unit;
        unit.id = static_cast<sim::EntityId>(id); unit.owner = static_cast<tactical::PlayerId>(owner);
        unit.type = static_cast<tactical::TypeId>(type); unit.position = {fixed(x), fixed(y), fixed(z)};
        view->units.push_back(unit);
    }
    host->view = view;
    foc::AiType bounded;
    bounded.type_id = 200; bounded.name = "BOUNDED";
    bounded.proximity_bounds = std::array{m::Vec3{fixed(-10), {}, {}}, m::Vec3{fixed(10), {}, {}}};
    foc::AiType decoration; decoration.type_id = 400; decoration.name = "DECORATION"; decoration.decoration = true;
    host->types.emplace(200, &bounded); host->types.emplace(400, &decoration);
    const std::string module = R"lua(
count = 0
order = 0
function Setup(origin, player, kind)
    Object = origin
    enemy = player
    local increment = 1
    handler = function(self, nearby)
        if self ~= Object then error('callback argument order') end
        count = count + increment
        order = order * 10 + nearby.Get_Owner().Get_ID()
    end
    if Object.Event_Object_In_Range(handler, 50, player, kind) ~= true then error('registration return') end
end
function Repeat() Object.Service_Wrapper() end
function Cancel()
    Object.Event_Object_In_Range(handler, 50)
    Object.Cancel_Event_Object_In_Range(handler)
    Object.Service_Wrapper()
end
function Mutation()
    Object.Cancel_Event_Object_In_Range(handler)
    local second
    second = function(self, nearby)
        count = count + 100
        self.Cancel_Event_Object_In_Range(second)
    end
    local first
    first = function(self, nearby)
        count = count + 10
        self.Cancel_Event_Object_In_Range(first)
        self.Event_Object_In_Range(second, 50, player, kind)
    end
    Object.Event_Object_In_Range(first, 50)
    Object.Service_Wrapper()
end
function Marker()
    local callback
    callback = function(self, nearby)
        count = count + 1
        self.Cancel_Event_Object_In_Range(callback)
    end
    Object.Event_Object_In_Range(callback, 50)
    Object.Service_Wrapper()
    Object.Service_Wrapper()
end
function Spawn()
    local callback
    callback = function(self, nearby)
        if nearby.Get_Owner().Get_ID() == 1 then
            spawned = true
            self.Cancel_Event_Object_In_Range(callback)
        end
    end
    Object.Event_Object_In_Range(callback, 250)
    Object.Service_Wrapper()
end
function Artillery()
    local far = function(self, enemy) enemies_in_range = true end
    local near = function(self, enemy) enemies_near = true end
    Object.Event_Object_In_Range(far, 1000, enemy)
    Object.Event_Object_In_Range(near, 50, enemy)
    Object.Service_Wrapper()
end
function Coroutine() Create_Thread('Once') end
function Once() Object.Service_Wrapper() end
function Bad() Object.Event_Object_In_Range(123, 50) end
function Escape() GlobalValue.Set('function', handler) end
function Fault()
    local callback
    callback = function(self, nearby)
        self.Cancel_Event_Object_In_Range(callback)
        nearby.Garrison(self)
    end
    Object.Event_Object_In_Range(callback, 50)
    Object.Service_Wrapper()
end
)lua";
    std::string baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        auth::ModuleManifest manifest;
        expect(manifest.add("PROX.LUA", module).has_value(), "L-32: proximity module loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
        expect(errors.empty(), "L-32: engine methods register");
        for (unsigned id : {1U, 2U}) expect(scripts.value().create_instance(id, "PROX.LUA").has_value(), "L-32: isolated instance starts");
        std::uint64_t sequence = 0;
        const auto call = [&](unsigned id, std::string name, auth::ValueList arguments = {}) {
            auth::ScriptEvent event;
            event.key = {scripts.value().completed_tick() + 1, auth::first_simulation_producer, id, sequence++};
            event.target = id; event.kind = auth::ScriptEvent::Kind::call; event.name = std::move(name);
            event.arguments = std::move(arguments);
            expect(scripts.value().submit_event(std::move(event)).has_value(), "L-32: call queues");
        };
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        const auto service = [&] {
            const auto report = scripts.value().service(executor);
            expect(report && report.value().diagnostics.empty(), "L-32: calls have no diagnostic");
        };
        const auto number = [&](unsigned id, const char* name, int expected) {
            const auto value = scripts.value().read_global(id, name);
            expect(value && value.value() && std::get_if<ai::Real>(&value.value()->data) != nullptr &&
                std::get<ai::Real>(value.value()->data) == ai::Real(expected), "L-32: " + std::string(name));
        };
        call(1, "Setup", {foc::detail::handle(foc::handle_game_object, 10), foc::detail::handle(foc::handle_player, 2),
            foc::detail::handle(foc::handle_type, 200)});
        call(2, "Setup", {foc::detail::handle(foc::handle_game_object, 10), foc::detail::handle(foc::handle_type, 200),
            foc::detail::handle(foc::handle_player, 2)});
        service(); number(1, "count", 0);
        call(1, "Repeat"); service(); number(1, "count", 2); number(1, "order", 23); number(2, "count", 0);
        const auto saved = scripts.value().save(executor);
        expect(saved.has_value(), "L-32: retained closure saves");
        if (saved) {
            expect(scripts.value().load(saved.value()).has_value(), "L-32: retained closure restores");
            if (workers == 1) {
                // Save v3 proximity record: object count, handle, generation, handler count.
                std::string prefix;
                const auto append = [&](std::uint64_t value, unsigned width) {
                    for (unsigned byte = 0; byte < width; ++byte)
                        prefix.push_back(static_cast<char>((value >> (8 * byte)) & 0xffU));
                };
                append(1, 4); append(foc::handle_game_object, 4); append(10, 8);
                append(1, 8); append(1, 4);
                std::string corrupt = saved.value();
                const auto record = corrupt.find(prefix);
                expect(record != std::string::npos, "L-32: saved proximity record found");
                if (record != std::string::npos) {
                    const auto before = scripts.value().state_hash();
                    corrupt.replace(record + prefix.size(), 8, 8, '\0');
                    const auto refused = scripts.value().load(corrupt);
                    expect(!refused && refused.error().code == auth::codes::load_rejected &&
                        refused.error().message.find("proximity handler ID") != std::string::npos,
                        "L-32: zero proximity handler ID is rejected");
                    const auto after = scripts.value().state_hash();
                    expect(before && after && before.value() == after.value(),
                        "L-32: rejected proximity save leaves scheduler unchanged");
                }
            }
        }
        call(1, "Repeat"); call(2, "Repeat"); service(); number(1, "count", 4); number(2, "count", 2);
        call(1, "Cancel"); service(); number(1, "count", 4);
        call(2, "Repeat"); service(); number(2, "count", 4);
        call(2, "Coroutine"); service(); number(2, "count", 6);
        call(1, "Mutation"); service(); number(1, "count", 114);
        call(1, "Marker"); service(); number(1, "count", 115);
        call(1, "Spawn"); call(1, "Artillery"); service();
        for (const char* name : {"spawned", "enemies_in_range", "enemies_near"}) {
            const auto value = scripts.value().read_global(1, name);
            expect(value && value.value() && std::get_if<bool>(&value.value()->data) && std::get<bool>(value.value()->data),
                "L-32: stock consumer " + std::string(name));
        }
        call(1, "Bad"); call(1, "Escape");
        const auto rejected = scripts.value().service(executor);
        expect(rejected && rejected.value().diagnostics.size() == 2, "L-32: wrong handler and function escape diagnose");
        if (rejected && rejected.value().diagnostics.size() == 2)
            expect(rejected.value().diagnostics[0].message.find("proximity handler must be a function") != std::string::npos &&
                rejected.value().diagnostics[0].message.find("cross-instance") == std::string::npos,
                "L-32: non-function handler has its own diagnostic");
        call(1, "Fault");
        const auto fault = scripts.value().service(executor);
        expect(fault && fault.value().diagnostics.size() == 1 && fault.value().diagnostics[0].code == auth::codes::missing_api,
            "L-16: proximity callback preserves missing API classification");
        const auto hash = scripts.value().state_hash(executor);
        expect(hash.has_value(), "L-32: state hashes");
        if (hash) { if (baseline.empty()) baseline = hash.value(); else expect(hash.value() == baseline, "L-32: 1/2/4/8 state equality"); }
    }
}

void reveal_all_contracts() {
    namespace t = eawr::sim::tactical;
    namespace m = eawr::sim::math;
    const auto fixed = [](std::int64_t value) { return m::Fixed::from_integer(value).value(); };
    t::TacticalSetup setup;
    setup.players = {{1, 1, 100, 1}, {2, 2, 200, 1}, {3, 1, 300, 1}, {4, 4, 400, 0}};
    setup.units = {{10, 100, 1, {fixed(50), fixed(-50), {}}},
        {20, 200, 2, {fixed(450), fixed(-450), {}}}};
    t::FogRules rules{{}, {}, fixed(100), 6, 6};
    // Partition failure cannot publish partially assigned rows or the persistent hold.
    struct FailingExecutor final : eawr::sim::PartitionExecutor {
        std::size_t worker_count() const noexcept override { return 1; }
        eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& partition) const override {
            for (std::size_t index = 0; index < count / 2; ++index) partition(index);
            return eawr::core::Result<void>::failure(foc::detail::make_error(t::diagnostic_codes::worker_failure, "fixture failure"));
        }
    };
    t::FogCells fog_fixture(rules, setup.players);
    std::vector<std::uint8_t> before, after;
    fog_fixture.append_state(before);
    expect(!fog_fixture.reveal_all(1, FailingExecutor{}), "V-20: reveal reports partition failure");
    fog_fixture.append_state(after);
    expect(before == after, "V-20: failed reveal leaves canonical cells and future holds unchanged");
    expect(fog_fixture.reveal_all(1, eawr::sim::InlineExecutor{}).has_value(), "V-20: reveal succeeds after failure");
    expect(fog_fixture.copied_grid_bytes() == rules.cells_wide, "V-20: whole-map reveal allocates only one row of cell bytes");
    after.clear(); fog_fixture.append_state(after);
    expect(fog_fixture.reveal_all(1, eawr::sim::InlineExecutor{}).has_value(), "V-20: repeated reveal succeeds");
    before.clear(); fog_fixture.append_state(before);
    expect(before == after, "V-20: repeated reveal is idempotent");
    std::vector<std::string> baseline;
    for (const std::size_t workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto made = t::TacticalSession::create(setup, {}, {}, {}, rules);
        expect(made.has_value(), "V-20: fog world starts");
        if (!made) return;
        auto host = ai::make_tactical_host();
        host->world = &made.value();
        host->view = foc::detail::build_view(made.value(), *made.value().snapshot());
        auth::ModuleManifest manifest;
        expect(manifest.add("BURN.LUA", "function Firesale(player)\n"
            " local result = FogOfWar.Reveal_All(player, 'extra argument')\n"
            " if result ~= nil then error('expected nil') end\n"
            " continued = true\n"
            "end\n").has_value(), "V-20: narrow firesale continuation module loads");
        auth::SessionConfig config;
        config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) { expect(false, "V-20: scheduler starts"); return; }
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "BURN.LUA").has_value(), "V-20: binding registers");
        for (const auto& [tick, recipient] : {std::pair{1U, 2U}, std::pair{2U, 1U}, std::pair{3U, 4U}}) {
            auth::ScriptEvent event;
            event.key = {tick, auth::first_simulation_producer, 1, tick};
            event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Firesale";
            event.arguments = {auth::Value{auth::Handle{foc::handle_player, recipient}}};
            expect(scripts.value().submit_event(std::move(event)).has_value(), "V-20: AI/local/neutral reveal events queue");
        }
        auto linked = auth::ScriptedTacticalSession::create(std::move(made).value(), std::move(scripts).value());
        if (!linked) { expect(false, "V-20: scripted world links"); return; }
        auto& session = linked.value();
        expect(session.register_verb(foc::verb_reveal_all, foc::tactical_ai_detail::translate_order).has_value(),
            "V-20: reveal uses production command router");
        const auto previous_rows = session.world().fog_cells()->value_rows(1);
        std::vector<std::string> hashes;
        for (std::uint64_t tick = 0; tick < 224; ++tick) {
            host->world = &session.world();
            host->view = foc::detail::build_view(session.world(), *session.world().snapshot());
            const auto stepped = session.step(executor);
            expect(stepped.has_value(), "V-20: firesale world steps");
            if (!stepped) return;
            expect(stepped.value().scripts.diagnostics.empty(), "V-20: supported call does not abort its thread");
            for (const auto& input : stepped.value().script_input)
                expect(input.submitted, "V-20: script reveal reaches replay queue");
            hashes.push_back(stepped.value().world.state_sha256);
            const auto* fog = session.world().fog_cells();
            const auto full = [&](std::size_t player, std::uint8_t value) {
                const auto cells = fog->values(player);
                return std::all_of(cells.begin(), cells.end(), [&](std::uint8_t cell) { return cell == value; });
            };
            expect(full(2, 0), "V-20: reveal does not leak to an allied observer");
            if (tick == 0) expect(full(0, 0) && full(1, 0), "V-20: call changes only the next tick");
            if (tick == 1) {
                expect(full(1, 255) && full(0, 0), "V-20: AI reveal leaves the local observer fogged");
                expect((session.world().snapshot()->instances()[0].visible_to & 2U) != 0,
                    "V-20: revealed enemy is present in recipient visibility");
            }
            if (tick >= 2) expect(full(0, 255) && full(1, 255), "V-20: explicit local reveal persists through refresh");
            if (tick >= 3) expect(full(3, 255), "V-20: non-commandable recipient has its own revealed grid");
        }
        expect((*previous_rows[0])[0] == 0, "V-20: previous fog rows remain immutable");
        const auto recorded = session.record();
        expect(recorded.commands.size() == 3, "V-20: AI/local/neutral reveals each record one command");
        const auto encoded = t::write_replay(recorded);
        expect(encoded.has_value(), "V-20: reveal replay encodes");
        if (!encoded) return;
        const auto parsed = t::parse_replay(encoded.value());
        expect(parsed.has_value() && parsed.value().commands == recorded.commands, "V-20: replay preserves recipient and issuer");
        if (!parsed) return;
        auto replayed = t::TacticalSession::from_replay(parsed.value(), {}, {}, {}, rules);
        expect(replayed.has_value(), "V-20: recorded reveals replay without Lua");
        if (!replayed) return;
        for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
            auto stepped = replayed.value().step(executor);
            expect(stepped && stepped.value().state_sha256 == hashes[tick], "V-20: replay reproduces every world hash");
        }
        auto malformed = encoded.value();
        malformed[malformed.size() - 12] = 1; // last command's reserved payload word
        expect(!t::parse_replay(malformed), "V-20: nonzero reveal reserved field is rejected");
        auto invalid = recorded;
        std::get<t::RevealAllPayload>(invalid.commands[0].payload).player = 99;
        expect(!t::write_replay(invalid), "V-20: undeclared recipient is rejected");
        invalid = recorded; invalid.commands[0].units = {10};
        expect(!t::write_replay(invalid), "V-20: reveal with unit orders is rejected");
        if (workers == 1) baseline = hashes;
        else expect(hashes == baseline, "V-20: canonical reveal agrees on 1/2/4/8 workers");
    }
    // Invalid arguments and a non-tactical host retain script errors, never missing-API errors.
    for (const auto& test : {std::pair{true, std::string{}}, std::pair{true, std::string{"nil"}},
            std::pair{true, std::string{"123"}}, std::pair{false, std::string{}}}) {
        auto host = ai::make_tactical_host();
        if (test.first) host->view = std::make_shared<foc::detail::WorldView>();
        auth::ModuleManifest manifest;
        expect(manifest.add("BAD.LUA", "function Check() FogOfWar.Reveal_All(" + test.second + ") end\n").has_value(),
            "V-20: bad-argument module loads");
        auth::SessionConfig config; config.tick_duration = {1, 30};
        auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
        if (!scripts) return;
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
        expect(errors.empty() && scripts.value().create_instance(1, "BAD.LUA").has_value(), "V-20: bad-argument binding registers");
        auth::ScriptEvent event;
        event.key = {1, auth::first_simulation_producer, 1, 0}; event.target = 1;
        event.kind = auth::ScriptEvent::Kind::call; event.name = "Check";
        expect(scripts.value().submit_event(std::move(event)).has_value(), "V-20: invalid call queues");
        const auto report = scripts.value().service(eawr::sim::InlineExecutor{});
        expect(report && report.value().commands.empty() && !report.value().diagnostics.empty(), "V-20: invalid call reports and issues no reveal");
        if (report) for (const auto& error : report.value().diagnostics) {
            expect(error.code != auth::codes::missing_api, "V-20: genuine argument error stays distinct from unsupported API");
            expect(error.message.find(test.first ? (test.second.empty() ? "requires a player" : "expected a player")
                : "only valid in a tactical game") != std::string::npos, "V-20: argument/mode error explains the rejected call");
        }
    }
}

void faction_controller_inventory() {
    // WSS-35 / GS-02: assigning a faction's controller is insufficient unless
    // its definition and space function set also reach the goal engine.
    const ai::ConverterFunction converters = [](std::string_view, std::string_view) -> std::optional<Real> {
        return std::nullopt;
    };
    std::map<std::string, std::string, std::less<>> xml;
    for (const auto& path : ai::ai_xml_files()) xml.emplace(path, "<Root></Root>");
    const std::string player_path = "data/xml/ai/players/ai_player_underworld.xml";
    const std::string set_path = "data/xml/ai/goalfunctions/ai_goalset_underworld_space.xml";
    expect(xml.contains(player_path) && xml.contains(set_path), "WSS-35: the Underworld player and space set are loaded");
    xml[player_path] = "<AIPlayerType><Name>AI_Player_Underworld</Name>"
        "<GoalProposalFunctionSets>AI_GoalSet_Underworld_Space</GoalProposalFunctionSets>"
        "<Templates><Space>Test_Space</Space></Templates></AIPlayerType>";
    xml[set_path] = "<FunctionSet><Probe><Goal>Probe</Goal><Function>ReallyBig</Function></Probe></FunctionSet>";
    const auto controller = ai::load_ai_data(xml, converters);
    expect(controller && controller.value().players.contains("AI_PLAYER_UNDERWORLD")
        && controller.value().players.at("AI_PLAYER_UNDERWORLD").function_sets == std::vector<std::string>{"AI_GOALSET_UNDERWORLD_SPACE"}
        && controller.value().players.at("AI_PLAYER_UNDERWORLD").space_templates == std::vector<std::string>{"TEST_SPACE"}
        && controller.value().function_sets.at("AI_GOALSET_UNDERWORLD_SPACE").size() == 1,
        "WSS-35 / GS-02: the Underworld goal engine has its authored controller and space functions");
}



// The installed bytes of every module the setup's admitted object scripts need.
std::optional<auth::ModuleManifest> stock_manifest(const std::filesystem::path& root, const foc::AiSetup& setup) {
    std::vector<eawr::vfs::MountSpec> mounts;
    for (const auto folder : {"corruption", "GameData"}) {
        auto mounted = eawr::vfs::resolve_manifest_mount(folder, root / folder / "Data");
        if (!mounted) { expect(false, "SOS: installed manifest mount"); return std::nullopt; }
        mounts.push_back(std::move(mounted).value().mount);
    }
    auto files = eawr::vfs::Vfs::mount(mounts);
    if (!files) { expect(false, "SOS: installed VFS"); return std::nullopt; }
    auth::ModuleManifest manifest;
    for (const auto& path : foc::required_modules(setup)) {
        auto bytes = files.value().open(path);
        if (!bytes) { expect(false, "SOS: installed module " + path); return std::nullopt; }
        std::string source(bytes.value().size(), '\0');
        std::transform(bytes.value().begin(), bytes.value().end(), source.begin(), [](std::byte b) { return static_cast<char>(b); });
        expect(manifest.add(path, std::move(source)).has_value(), "SOS: logical stock manifest");
    }
    return manifest;
}

// Installed stock chunks on synthetic immutable inputs: an executable SOS-15/19
// witness, independent of the manually authored fixture scripts below.
void stock_object_scripts(const std::filesystem::path& root) {
    foc::AiSetup setup;
    foc::AiType falcon; falcon.type_id = 100; falcon.lua_script = "ObjectScript_MillenniumFalcon";
    foc::AiType vader; vader.type_id = 101; vader.lua_script = "ObjectScript_VaderTIESquadron";
    setup.content.types = {falcon, vader};
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    const auto& manifest = *stock;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto host = std::make_shared<foc::detail::Host>(); host->setup = setup;
        host->setup.players = {{1, "REBEL", false, true, {}, false}, {2, "REBEL", false, false, {}, true}};
        for (const auto& type : host->setup.content.types) host->types.emplace(type.type_id, &type);
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        auto view = std::make_shared<foc::detail::WorldView>(); host->view = view;
        const auto fixed = [](int n) { return sim::math::Fixed::from_integer(n).value(); };
        for (const auto id : {10U, 11U}) {
            foc::detail::ViewUnit unit; unit.id = id; unit.type = 100; unit.owner = id == 10 ? 1U : 2U;
            unit.hull = fixed(40); unit.max_hull = fixed(100); unit.shield = Real(1);
            unit.abilities = {{tactical::AbilityKind::invulnerability, false, true, true}};
            view->units.push_back(unit);
        }
        foc::detail::ViewUnit enemy; enemy.id = 20; enemy.owner = 3; view->units.push_back(enemy);
        foc::detail::ViewUnit container; container.id = 30; container.type = 101; container.owner = 1;
        container.container = true; container.members = {31}; view->units.push_back(container);
        foc::detail::ViewUnit leader; leader.id = 31; leader.owner = 1;
        leader.abilities = {{tactical::AbilityKind::replenish_wingmen, false, true, true}}; view->units.push_back(leader);
        container.id = 40; container.members = {41}; view->units.push_back(container);
        leader.id = 41; leader.abilities.clear(); view->units.push_back(leader);
        ai::Engine engine(host, {}, {}); host->engine = &engine;
        ai::EngineContractAccess::threat(engine, 10, 20, 1); ai::EngineContractAccess::threat(engine, 11, 20, 1);
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/"};
        auto created = auth::ScriptScheduler::create(config, manifest);
        if (!created) { expect(false, "SOS: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, host, errors); foc::tactical_ai_detail::register_methods(scripts, host, errors);
        expect(errors.empty(), "SOS: real bindings for installed stock");
        std::array<std::uint32_t, 3> uses{};
        for (std::uint64_t tick = 1; tick <= 100; ++tick) {
            view->tick = tick;
            // Human is still above its 0.05 boundary while AI is below 0.2.
            if (tick == 35) for (auto& unit : view->units) if (unit.type == 100) unit.shield = Real(1) / Real(10);
            if (tick == 68) for (auto& unit : view->units) if (unit.type == 100) unit.shield = Real(1) / Real(100);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && active == 4 && pumps <= active, "SOS-42: bounded stock work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS: installed scripts have no diagnostic");
            for (const auto& command : report.value().commands) {
                const auto id = command.issuer - foc::first_object_instance;
                expect(command.verb == foc::verb_ability && id != 40, "SOS-19: inherited script cannot grant absent ability");
                if (id == 10) { ++uses[0]; expect(tick >= 35, "SOS-15: AI above threshold holds"); }
                if (id == 11) { ++uses[1]; expect(tick >= 68, "SOS-15: human stricter threshold holds"); }
                if (id == 30) ++uses[2];
            }
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick-1)], "SOS: stock 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0 && uses[2] > 0, "SOS-15/19: installed AI/human Falcon and Vader visibly fire");
        std::cout << workers << " workers: stock Falcon AI=" << uses[0] << " human=" << uses[1] << " Vader=" << uses[2] << '\n';
    }
}

// SOS-16 scene: Slave_I (102) for AI player 1 at the origin and for human player 2
// far away; enemy player 3 has a squadron container (103, rating 600) whose two
// craft (104, rating 300 each) sit within 200, and ships (105, rating 500).
struct SlaveScene {
    foc::AiSetup setup;
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<foc::detail::WorldView> view;
};

SlaveScene slave_scene(const foc::AiSetup& setup) {
    SlaveScene scene;
    scene.setup = setup;
    scene.host = std::make_shared<foc::detail::Host>();
    scene.host->setup = setup;
    scene.host->setup.players = {{1, "EMPIRE", false, true, {}, false}, {2, "EMPIRE", false, false, {}, true},
        {3, "REBEL", false, true, {}, false}};
    for (const auto& type : scene.host->setup.content.types) scene.host->types.emplace(type.type_id, &type);
    for (const auto& player : scene.host->setup.players) scene.host->players.emplace(player.player, &player);
    scene.view = std::make_shared<foc::detail::WorldView>();
    scene.view->players = {{1, 1, 0}, {2, 1, 1}, {3, 2, 2}};
    scene.host->view = scene.view;
    const auto at = [](int x, int y) {
        return sim::math::Vec3{sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(), {}};
    };
    const auto unit = [&](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, sim::math::Vec3 position) {
        foc::detail::ViewUnit out; out.id = id; out.type = type; out.owner = owner; out.position = position;
        return out;
    };
    auto ai_slave = unit(50, 102, 1, at(0, 0));
    ai_slave.abilities = {{tactical::AbilityKind::harmonic_bomb, false, true, false}};
    auto human_slave = unit(51, 102, 2, at(5000, 0));
    human_slave.abilities = {{tactical::AbilityKind::harmonic_bomb, false, true, true}};
    auto team = unit(60, 103, 3, at(100, 0)); team.container = true; team.members = {61, 62};
    auto first = unit(61, 104, 3, at(100, 10)); first.craft = true; first.squadron = 60;
    auto second = unit(62, 104, 3, at(100, -10)); second.craft = true; second.squadron = 60;
    scene.view->units = {ai_slave, human_slave, team, first, second, unit(80, 105, 3, at(5100, 0)), unit(81, 105, 3, at(5100, 50))};
    return scene;
}

// Tick 40 adds a ship beside the AI Slave I; tick 70 adds the human's third contact.
void slave_scene_arrivals(foc::detail::WorldView& view, std::uint64_t tick) {
    const auto add = [&](sim::EntityId id, int x, int y) {
        foc::detail::ViewUnit out; out.id = id; out.type = 105; out.owner = 3;
        out.position = {sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(), {}};
        view.units.push_back(out);
        std::sort(view.units.begin(), view.units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
    };
    if (tick == 40) add(70, 150, 0);
    if (tick == 70) add(82, 5100, -50);
}

foc::AiSetup slave_setup() {
    foc::AiSetup setup;
    foc::AiType slave; slave.type_id = 102; slave.lua_script = "Slave_I";
    foc::AiType squadron; squadron.type_id = 103; squadron.combat_power = Real(600);
    foc::AiType craft; craft.type_id = 104; craft.combat_power = Real(300);
    foc::AiType ship; ship.type_id = 105; ship.combat_power = Real(500);
    setup.content.types = {slave, squadron, craft, ship};
    return setup;
}

// SOS-16: installed Slave_I promotes craft to their team, counts each parent once
// and sums type combat ratings within 200. AI bombs at threat >= 1000 (team 600
// alone stays below; promotion-free counting would reach 1200 at once); an armed
// human needs more than two counted units.
void stock_slave_i(const std::filesystem::path& root) {
    const auto setup = slave_setup();
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = slave_scene(setup);
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/"};
        auto created = auth::ScriptScheduler::create(config, *stock);
        if (!created) { expect(false, "SOS-16: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
        expect(errors.empty(), "SOS-16: real bindings for installed stock");
        std::array<std::uint32_t, 2> uses{};
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            scene.view->tick = tick;
            slave_scene_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && active == 2 && pumps <= active, "SOS-42: bounded Slave I work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS-16: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS-16: installed Slave_I has no diagnostic");
            for (const auto& command : report.value().commands) {
                const auto id = command.issuer - foc::first_object_instance;
                expect(command.verb == foc::verb_ability && command.arguments.size() == 4
                    && std::get<Real>(command.arguments[2].data) == Real(static_cast<std::int64_t>(tactical::AbilityKind::harmonic_bomb)),
                    "SOS-16: Slave I requests only its harmonic bomb");
                if (id == 50) { ++uses[0]; expect(tick > 40, "SOS-16: one team parent (600) stays below the AI threshold"); }
                if (id == 51) { ++uses[1]; expect(tick > 70, "SOS-16: two contacts do not arm the human bomb"); }
            }
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS-16: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-16: stock 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0, "SOS-16: installed AI and armed human Slave I visibly bomb");
        std::cout << workers << " workers: stock Slave_I AI=" << uses[0] << " human=" << uses[1] << '\n';
    }
}

// SOS-16/43 synthetic contract on the production bindings: parent promotion,
// handle-keyed deduplication, type combat rating, and the sample set across a
// save/restore barrier.
void slave_i_contracts() {
    const std::string module = R"LUA(
function Base_Definitions() ServiceRate = 1; threat = 0; seen = {} end
function Unit_Prox(self_obj, trigger_obj)
    if not trigger_obj.Get_Owner().Is_Enemy(Object.Get_Owner()) then return end
    if trigger_obj.Get_Parent_Object() then trigger_obj = trigger_obj.Get_Parent_Object() end
    if seen[trigger_obj] == nil then
        seen[trigger_obj] = trigger_obj
        threat = threat + trigger_obj.Get_Type().Get_Combat_Rating()
    end
end
function main()
    Object.Event_Object_In_Range(Unit_Prox, 200)
    while true do
        if threat >= 1000 and Object.Is_Ability_Ready('HARMONIC_BOMB') then Object.Activate_Ability('HARMONIC_BOMB', true) end
        sampled = threat; threat = 0; seen = {}
        Object.Service_Wrapper()
        coroutine.yield(true)
    end
end
)LUA";
    const auto setup = slave_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = slave_scene(setup);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/SLAVE_I.lua", module).has_value(), "SOS-16: Slave I module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
            foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
            expect(errors.empty(), "SOS-16: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        const auto sampled = [&]() {
            auto value = scripts.read_global(foc::first_object_instance + 50, "sampled");
            const auto* number = value && value.value() ? std::get_if<Real>(&value.value()->data) : nullptr;
            return number != nullptr ? *number : Real(-1);
        };
        std::uint32_t uses = 0;
        bool restored = false;
        for (std::uint64_t tick = 1; tick <= 120; ++tick) {
            scene.view->tick = tick;
            slave_scene_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                && diagnostics.empty(), "SOS-16: partitioned attachment and admission");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) throw std::runtime_error(report.error().message);
            expect(report.value().diagnostics.empty(), "SOS-16: no binding diagnostics");
            for (const auto& command : report.value().commands) {
                if (command.issuer != foc::first_object_instance + 50) continue;
                ++uses;
                expect(tick > 40, "SOS-16: promoted team counts once below the threshold");
            }
            // Before the ship arrives the sample is the team once (600), never its craft.
            if (tick < 40) expect(sampled() == Real(-1) || sampled() == Real(0) || sampled() == Real(600),
                "SOS-16: craft promote to one team parent with its type rating");
            if (tick > 40 && sampled() == Real(1100) && !restored) {
                // SOS-43: the handle-keyed sample and callback survive a restore.
                auto before = scripts.save(executor);
                if (!before) throw std::runtime_error(before.error().message);
                auto fresh = make_scripts();
                expect(fresh.load(before.value()).has_value(), "SOS-43: Slave I restore");
                auto after = fresh.save();
                expect(after && after.value() == before.value(), "SOS-43: Slave I canonical restore bytes");
                scripts = std::move(fresh);
                restored = true;
            }
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-16: state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-16: 1/2/4/8 equality");
        }
        expect(restored && uses > 0, "SOS-16: team plus ship (600 + 500) bombs after a restore");
    }
}

// SOS-17 scene: Sundered_Heart (106) for AI player 1 at the origin and for human
// player 2 far away; enemy ships (105, rating 500) for player 3. Two enemies sit
// near each Sundered Heart; tick 40 adds a third near each.
struct SunderedScene {
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<foc::detail::WorldView> view;
};

foc::AiSetup sundered_setup() {
    foc::AiSetup setup;
    foc::AiType heart; heart.type_id = 106; heart.lua_script = "Sundered_Heart";
    foc::AiType ship; ship.type_id = 105; ship.combat_power = Real(500);
    setup.content.types = {heart, ship};
    return setup;
}

foc::detail::ViewUnit sundered_unit(sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner, int x, int y) {
    foc::detail::ViewUnit out; out.id = id; out.type = type; out.owner = owner;
    out.position = {sim::math::Fixed::from_integer(x).value(), sim::math::Fixed::from_integer(y).value(), {}};
    return out;
}

SunderedScene sundered_scene(const foc::AiSetup& setup) {
    SunderedScene scene;
    scene.host = std::make_shared<foc::detail::Host>();
    scene.host->setup = setup;
    scene.host->setup.players = {{1, "REBEL", false, true, {}, false}, {2, "REBEL", false, false, {}, true},
        {3, "EMPIRE", false, true, {}, false}};
    for (const auto& type : scene.host->setup.content.types) scene.host->types.emplace(type.type_id, &type);
    for (const auto& player : scene.host->setup.players) scene.host->players.emplace(player.player, &player);
    scene.view = std::make_shared<foc::detail::WorldView>();
    scene.view->players = {{1, 1, 0}, {2, 1, 1}, {3, 2, 2}};
    scene.host->view = scene.view;
    auto ai_heart = sundered_unit(50, 106, 1, 0, 0);
    ai_heart.abilities = {{tactical::AbilityKind::weaken_enemy, false, true, false}};
    auto human_heart = sundered_unit(51, 106, 2, 5000, 0);
    human_heart.abilities = {{tactical::AbilityKind::weaken_enemy, false, true, true}};
    scene.view->units = {ai_heart, human_heart, sundered_unit(70, 105, 3, 400, 0), sundered_unit(71, 105, 3, 450, 50),
        sundered_unit(80, 105, 3, 5400, 0), sundered_unit(81, 105, 3, 5450, 50)};
    return scene;
}

void sundered_arrivals(foc::detail::WorldView& view, std::uint64_t tick) {
    if (tick != 40) return;
    view.units.push_back(sundered_unit(72, 105, 3, 450, -50));
    view.units.push_back(sundered_unit(82, 105, 3, 5450, -50));
    std::sort(view.units.begin(), view.units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
}

// SOS-17: installed Sundered_Heart exits for a human owner; its AI samples enemies
// within 1000 and, from the third distinct contact, weakens the best local threat
// center (radius 300) above threat 1000, at that point.
void stock_sundered_heart(const std::filesystem::path& root) {
    const auto setup = sundered_setup();
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = sundered_scene(setup);
        auth::SessionConfig config; config.tick_duration = {1, 30};
        config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/"};
        auto created = auth::ScriptScheduler::create(config, *stock);
        if (!created) { expect(false, "SOS-17: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
        expect(errors.empty(), "SOS-17: real bindings for installed stock");
        std::uint32_t uses = 0;
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            scene.view->tick = tick;
            sundered_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && pumps <= active, "SOS-42: bounded Sundered Heart work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS-17: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS-17: installed Sundered_Heart has no diagnostic");
            for (const auto& command : report.value().commands) {
                const auto id = command.issuer - foc::first_object_instance;
                expect(id == 50, "SOS-17: a human-owned Sundered Heart exits");
                expect(tick > 40, "SOS-17: two contacts do not weaken");
                expect(command.verb == foc::verb_ability && command.arguments.size() == 7
                    && std::get<Real>(command.arguments[2].data) == Real(static_cast<std::int64_t>(tactical::AbilityKind::weaken_enemy)),
                    "SOS-17: Weaken Enemy at a point");
                // Three ships (<= 4) give their mean planar position.
                expect(command.arguments.size() == 7 && std::get_if<Real>(&command.arguments[4].data) != nullptr
                    && std::get<Real>(command.arguments[4].data) == ai::to_single(Real(1300) / Real(3))
                    && std::get_if<Real>(&command.arguments[5].data) != nullptr
                    && std::get<Real>(command.arguments[5].data) == Real(0), "WHE-68: weakened at the cluster mean");
                auto order = foc::tactical_ai_detail::translate_order(command);
                const auto* payload = order ? std::get_if<tactical::AbilityPayload>(&order.value().payload) : nullptr;
                expect(payload != nullptr && payload->ability == tactical::AbilityKind::weaken_enemy && payload->position,
                    "WHE-61: the order carries the weaken point");
                ++uses;
            }
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS-17: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-17: stock 1/2/4/8 equality");
        }
        expect(uses > 0, "SOS-17: installed AI Sundered Heart visibly weakens");
        std::cout << workers << " workers: stock Sundered_Heart AI=" << uses << '\n';
    }
}

// SOS-17/43, WHE-68 synthetic contract on the production bindings: an object-keyed
// sample crosses the boundary, the position wrapper is valid, answers its point
// and round-trips bit-exactly through save/load, and positions Weaken Enemy.
void sundered_contracts() {
    for (const std::uint32_t bits : {0x00000000U, 0x80000000U, 0x00000001U, 0x80000001U, 0x00400001U, 0x007fffffU,
             0x00800000U, 0x3f800000U, 0x43d8aaabU, 0xc479fff0U, 0x7f7fffffU, 0x7f800000U, 0xff800000U}) {
        expect(ai::binary32_bits(ai::binary32_value(bits)) == bits, "position wrapper: binary32 bits round-trip");
    }
    expect(ai::binary32_value(0x3f800000U) == Real(1), "position wrapper: 1.0 decodes exactly");
    for (const Real value : {Real(1) / Real(3), Real(1300) / Real(3), Real(-7) / Real(10), Real(123456789)}) {
        expect(ai::binary32_value(ai::binary32_bits(value)) == ai::to_single(value), "position wrapper: single rounding");
    }
    const std::string module = R"LUA(
function Base_Definitions() ServiceRate = 1; seen = {}; count = 0; fired = 0 end
function Unit_Prox(self_obj, trigger_obj)
    if not trigger_obj.Get_Owner().Is_Enemy(Object.Get_Owner()) then return end
    if seen[trigger_obj] == nil then
        seen[trigger_obj] = trigger_obj
        count = count + 1
        if count >= 3 and Object.Is_Ability_Ready('WEAKEN_ENEMY') then
            local pos, threat = Find_Best_Local_Threat_Center(seen, 300)
            if pos ~= nil and pos.Is_Valid() and threat > 1000 then
                held = pos; held_threat = threat
                held_x, held_y, held_z = pos.Get_XYZ()
                Object.Activate_Ability('WEAKEN_ENEMY', pos)
                fired = fired + 1
            end
        end
    end
end
function main()
    Object.Event_Object_In_Range(Unit_Prox, 1000)
    while true do
        count = 0; seen = {}
        Object.Service_Wrapper()
        coroutine.yield(true)
    end
end
)LUA";
    const auto setup = sundered_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = sundered_scene(setup);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/SUNDERED_HEART.lua", module).has_value(), "SOS-17: module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
            foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
            expect(errors.empty(), "SOS-17: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        const auto global = [&](std::string_view name) -> std::optional<auth::Value> {
            auto value = scripts.read_global(foc::first_object_instance + 50, name);
            if (!value || !value.value() || std::holds_alternative<std::monostate>(value.value()->data)) return std::nullopt;
            return *value.value();
        };
        std::uint32_t uses = 0;
        bool restored = false;
        for (std::uint64_t tick = 1; tick <= 120; ++tick) {
            scene.view->tick = tick;
            sundered_arrivals(*scene.view, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                && diagnostics.empty(), "SOS-17: partitioned attachment and admission");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) throw std::runtime_error(report.error().message);
            expect(report.value().diagnostics.empty(), "SOS-17: no binding diagnostics");
            for (const auto& command : report.value().commands) {
                if (command.issuer != foc::first_object_instance + 50) continue;
                ++uses;
                expect(tick > 40 && command.arguments.size() == 7, "SOS-17: the third contact weakens at a point");
            }
            if (!restored && global("held")) {
                const auto before_bytes = scripts.save(executor);
                if (!before_bytes) throw std::runtime_error(before_bytes.error().message);
                const auto held = global("held");
                const auto x = global("held_x");
                auto fresh = make_scripts();
                expect(fresh.load(before_bytes.value()).has_value(), "SOS-43: Sundered Heart restore");
                auto after = fresh.save();
                expect(after && after.value() == before_bytes.value(), "SOS-43: canonical restore bytes");
                scripts = std::move(fresh);
                const auto* wrapper = held ? std::get_if<auth::Handle>(&held->data) : nullptr;
                const auto restored_held = global("held");
                const auto* back = restored_held ? std::get_if<auth::Handle>(&restored_held->data) : nullptr;
                expect(wrapper != nullptr && back != nullptr && wrapper->kind == foc::handle_position && *wrapper == *back,
                    "SOS-43: the position wrapper round-trips exactly");
                const auto* x_number = x ? std::get_if<Real>(&x->data) : nullptr;
                expect(x_number != nullptr && *x_number == ai::to_single(Real(1300) / Real(3)), "WHE-68: wrapper answers the cluster mean");
                const auto threat = global("held_threat");
                const auto* threat_number = threat ? std::get_if<Real>(&threat->data) : nullptr;
                expect(threat_number != nullptr && *threat_number == Real(1500), "WHE-68: company power sums within the radius");
                restored = true;
            }
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-17: state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-17: 1/2/4/8 equality");
        }
        expect(restored && uses > 0, "SOS-17: the synthetic Sundered Heart weakens");
    }
}

// SOS-11 scene: Interdictor-like ships (108) with MISSILE_SHIELD for AI player 1
// and human player 2; enemy player 3 has a laser ship (110) and a rocket ship
// (109). The AI's deadly enemy is the laser ship until tick 40, then the rocket
// ship; the human's is the rocket ship throughout and autofire arms at tick 70.
struct ShieldScene {
    std::shared_ptr<foc::detail::Host> host;
    std::shared_ptr<foc::detail::WorldView> view;
    std::unique_ptr<ai::Engine> engine;
};

foc::AiSetup shield_setup() {
    foc::AiSetup setup;
    foc::AiType cruiser; cruiser.type_id = 108; cruiser.lua_script = "ObjectScript_MissileShield";
    foc::AiType rockets; rockets.type_id = 109; rockets.object_projectile = 300;
    foc::AiType lasers; lasers.type_id = 110; lasers.object_projectile = 301;
    setup.content.types = {cruiser, rockets, lasers};
    setup.content.projectile_categories = {{300, foc::ai_projectile_rocket}, {301, 0U}};
    return setup;
}

ShieldScene shield_scene(const foc::AiSetup& setup) {
    ShieldScene scene;
    scene.host = std::make_shared<foc::detail::Host>();
    scene.host->setup = setup;
    scene.host->setup.players = {{1, "EMPIRE", false, true, {}, false}, {2, "EMPIRE", false, false, {}, true},
        {3, "REBEL", false, true, {}, false}};
    for (const auto& type : scene.host->setup.content.types) scene.host->types.emplace(type.type_id, &type);
    for (const auto& player : scene.host->setup.players) scene.host->players.emplace(player.player, &player);
    scene.view = std::make_shared<foc::detail::WorldView>();
    scene.view->players = {{1, 1, 0}, {2, 1, 1}, {3, 2, 2}};
    scene.host->view = scene.view;
    const auto unit = [](sim::EntityId id, tactical::TypeId type, tactical::PlayerId owner) {
        foc::detail::ViewUnit out; out.id = id; out.type = type; out.owner = owner; return out;
    };
    auto ai_cruiser = unit(50, 108, 1);
    ai_cruiser.abilities = {{tactical::AbilityKind::missile_shield, false, true, false}};
    auto human_cruiser = unit(51, 108, 2);
    human_cruiser.abilities = {{tactical::AbilityKind::missile_shield, false, true, false}};
    scene.view->units = {ai_cruiser, human_cruiser, unit(70, 110, 3), unit(71, 109, 3)};
    scene.engine = std::make_unique<ai::Engine>(scene.host, ai::AiData{}, std::vector<ai::PlanDef>{});
    scene.host->engine = scene.engine.get();
    ai::EngineContractAccess::threat(*scene.engine, 50, 70, 1);
    ai::EngineContractAccess::threat(*scene.engine, 51, 71, 1);
    return scene;
}

void shield_events(ShieldScene& scene, std::uint64_t tick) {
    if (tick == 40) ai::EngineContractAccess::threat(*scene.engine, 50, 71, 5);
    if (tick == 70) scene.view->units[1].abilities[0].autofire = true;
}

// Counts missile-shield requests; checks their kinds and the AI/human gates.
void shield_commands(const auth::ServiceReport& report, std::uint64_t tick, std::array<std::uint32_t, 2>& uses) {
    for (const auto& command : report.commands) {
        const auto id = command.issuer - foc::first_object_instance;
        expect(command.verb == foc::verb_ability && command.arguments.size() == 4
            && std::get<Real>(command.arguments[2].data) == Real(static_cast<std::int64_t>(tactical::AbilityKind::missile_shield)),
            "SOS-11: only MISSILE_SHIELD is requested");
        if (id == 50) { ++uses[0]; expect(tick > 40, "SOS-11: a laser-armed deadly enemy does not raise the shield"); }
        if (id == 51) { ++uses[1]; expect(tick > 70, "SOS-11: a human shield waits for autofire"); }
    }
}

// SOS-11: installed ObjectScript_MissileShield raises the shield against a deadly
// enemy that fires an affected projectile; AI owners act at once, human owners
// only with autofire.
void stock_missile_shield(const std::filesystem::path& root) {
    const auto setup = shield_setup();
    auto stock = stock_manifest(root, setup);
    if (!stock) return;
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = shield_scene(setup);
        auth::SessionConfig config; config.tick_duration = {1, 30}; config.script_directories = {"Data/Scripts/Library/"};
        auto created = auth::ScriptScheduler::create(config, *stock);
        if (!created) { expect(false, "SOS-11: stock scheduler starts"); return; }
        auto scripts = std::move(created).value();
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
        foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
        expect(errors.empty(), "SOS-11: real bindings for installed stock");
        std::array<std::uint32_t, 2> uses{};
        for (std::uint64_t tick = 1; tick <= 160; ++tick) {
            scene.view->tick = tick;
            shield_events(scene, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            auto prepared = foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps);
            expect(prepared && diagnostics.empty() && active == 2 && pumps <= active, "SOS-42: bounded missile-shield work counts");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) { expect(false, "SOS-11: stock service"); return; }
            for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
            expect(report.value().diagnostics.empty(), "SOS-11: installed missile shield has no diagnostic");
            shield_commands(report.value(), tick, uses);
            auto hash = scripts.state_hash(executor);
            if (!hash) { expect(false, "SOS-11: stock canonical hash"); return; }
            if (workers == 1) reference.push_back(hash.value());
            else expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-11: stock 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0, "SOS-11: installed AI and armed human shields visibly rise");
        std::cout << workers << " workers: stock missile shield AI=" << uses[0] << " human=" << uses[1] << '\n';
    }
}

// SOS-11 synthetic contract on the production bindings: the deadly enemy's
// projectile types decide, with the AI/human autofire split and a restore.
void missile_shield_contracts() {
    const std::string module = R"LUA(
function Base_Definitions() ServiceRate = 1 end
function main()
    local human = Object.Get_Owner().Is_Human()
    while true do
        if (not human or Object.Is_Ability_Autofire('MISSILE_SHIELD')) and Object.Is_Ability_Ready('MISSILE_SHIELD') then
            local enemy = FindDeadlyEnemy(Object)
            if enemy then
                for _, projectile in pairs(enemy.Get_All_Projectile_Types()) do
                    if projectile.Is_Affected_By_Missile_Shield() then
                        Object.Activate_Ability('MISSILE_SHIELD', true)
                        break
                    end
                end
            end
        end
        coroutine.yield(true)
    end
end
)LUA";
    const auto setup = shield_setup();
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto scene = shield_scene(setup);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/OBJECTSCRIPT_MISSILESHIELD.lua", module).has_value(), "SOS-11: module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
            foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
            expect(errors.empty(), "SOS-11: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        std::array<std::uint32_t, 2> uses{};
        for (std::uint64_t tick = 1; tick <= 120; ++tick) {
            scene.view->tick = tick;
            shield_events(scene, tick);
            std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
            std::vector<auth::ScriptDiagnostic> diagnostics;
            expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                && diagnostics.empty(), "SOS-11: partitioned attachment and admission");
            auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
            auto report = scripts.service(executor, paced);
            if (!report) throw std::runtime_error(report.error().message);
            expect(report.value().diagnostics.empty(), "SOS-11: no binding diagnostics");
            shield_commands(report.value(), tick, uses);
            if (tick == 55) {
                auto before = scripts.save(executor);
                if (!before) throw std::runtime_error(before.error().message);
                auto fresh = make_scripts();
                expect(fresh.load(before.value()).has_value(), "SOS-43: missile-shield restore");
                auto after = fresh.save();
                expect(after && after.value() == before.value(), "SOS-43: canonical restore bytes");
                scripts = std::move(fresh);
            }
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-11: state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)], "SOS-11: 1/2/4/8 equality");
        }
        expect(uses[0] > 0 && uses[1] > 0, "SOS-11: AI and armed human shields rise");
    }
}

// SOS-15/19/40..44: synthetic behavior witnesses use the production attachment,
// pacing, bindings, callback graph and persistence paths.
// SOS-10/12/13/14/18: execute installed authored chunks through the production
// lifecycle on mod-shaped types; no runtime type-name test identifies a ship.
void stock_script_admission(const std::filesystem::path& root) {
    using K = tactical::AbilityKind;
    struct Case { const char* script; K ability; bool renamed{}; };
    for (const auto test : {Case{"ObjectScript_PowerToShields", K::defend},
             Case{"ObjectScript_PointDefense", K::laser_defense}, Case{"ObjectScript_MC30", K::cluster_bomb},
             Case{"ObjectScript_Krayt", K::self_destruct}, Case{"ObjectScript_IG2000", K::corrupt_systems},
             Case{"ObjectScript_Krayt", K::self_destruct, true}}) {
        const bool mc30 = test.ability == K::cluster_bomb;
        const bool krayt = test.ability == K::self_destruct;
        const bool proximity = mc30 || krayt || test.ability == K::corrupt_systems;
        foc::AiSetup setup;
        foc::AiType source; source.type_id = 110; source.lua_script = test.script;
        foc::AiType team; team.type_id = 103; team.combat_power = Real(600);
        foc::AiType craft; craft.type_id = 104; craft.combat_power = Real(300);
        foc::AiType ship; ship.type_id = 105; ship.combat_power = Real(700); ship.locomotor = true;
        ship.hardpoint_projectiles = {300};
        setup.content.types = {source, team, craft, ship};
        setup.content.projectile_categories.emplace(300, foc::ai_projectile_missile);
        auto stock = stock_manifest(root, setup);
        if (!stock) return;
        if (test.renamed) {
            const auto* original = stock->find("Data/Scripts/GameObject/OBJECTSCRIPT_KRAYT.lua");
            expect(original != nullptr, "SOS-01: original authored chunk exists");
            if (!original) return;
            const auto copy = *original;
            expect(stock->add("Data/Scripts/GameObject/MOD_AUTHORED.lua", copy).has_value(),
                "SOS-01: renamed authored copy enters the logical manifest");
            setup.content.types.front().lua_script = "Mod_Authored";
            const auto modules = foc::required_modules(setup);
            expect(std::find(modules.begin(), modules.end(), "Data/Scripts/GameObject/MOD_AUTHORED.lua") != modules.end(),
                "SOS-01: arbitrary authored module name is required from data");
        }
        std::vector<std::string> reference;
        std::vector<std::string> reference_commands;
        for (const auto workers : {1U, 2U, 4U, 8U}) {
            const eawr::platform::ThreadWorkerAdapter executor(workers);
            auto scene = slave_scene(setup);
            for (auto& unit : scene.view->units) {
                if (unit.id == 50 || unit.id == 51) {
                    unit.type = 110;
                    unit.abilities = {{test.ability, false, true, false}};
                    unit.hull = sim::math::Fixed::from_integer(unit.id == 50 ? 40 : 21).value();
                    unit.max_hull = sim::math::Fixed::from_integer(100).value();
                    unit.damage_rate = sim::math::Fixed::from_integer(20).value();
                }
                // The callback query sees the craft at 200; distance exclusion
                // must use their promoted parent's 149 position.
                if (unit.id == 60) unit.position.x = sim::math::Fixed::from_integer(mc30 ? 149 : 200).value();
                if (unit.craft) unit.position.x = sim::math::Fixed::from_integer(200).value();
                if (unit.id == 80 || unit.id == 81) unit.position.x = sim::math::Fixed::from_integer(5200).value();
                if (unit.owner == 3) unit.shield = Real(1);
            }
            // An inherited script cannot grant an ability absent in XML.
            auto absent = scene.view->units.front(); absent.id = 52; absent.abilities.clear();
            scene.view->units.insert(scene.view->units.begin() + 2, absent);
            if (!mc30 && !krayt) {
                std::erase_if(scene.view->units, [](const auto& unit) { return unit.container || unit.craft; });
                scene.view->units.push_back(sundered_unit(70, 105, 3, 200, 0));
                scene.view->units.back().shield = Real(1);
            }
            std::sort(scene.view->units.begin(), scene.view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
            ai::Engine engine(scene.host, {}, {}); scene.host->engine = &engine;
            auth::SessionConfig config; config.tick_duration = {1, 30};
            config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/GameObject/"};
            const auto make_scripts = [&]() {
                auto created = auth::ScriptScheduler::create(config, *stock);
                if (!created) throw std::runtime_error(created.error().message);
                auto scripts = std::move(created).value();
                std::vector<eawr::core::Diagnostic> errors;
                foc::tactical_ai_detail::register_globals(scripts, scene.host, errors);
                foc::tactical_ai_detail::register_methods(scripts, scene.host, errors);
                expect(errors.empty(), "SOS migration: production bindings register");
                return scripts;
            };
            auto scripts = make_scripts();
            const auto number = [&](std::uint64_t object, std::string_view name) -> std::optional<Real> {
                auto value = scripts.read_global(foc::first_object_instance + object, name);
                if (!value || !value.value()) return {};
                const auto* n = std::get_if<Real>(&value.value()->data);
                return n ? std::optional<Real>(*n) : std::nullopt;
            };
            const auto restore = [&]() {
                auto saved = scripts.save(executor);
                if (!saved) throw std::runtime_error(saved.error().message);
                auto restored = make_scripts();
                expect(restored.load(saved.value()).has_value(), "SOS-43: migrated script restores");
                auto bytes = restored.save(executor);
                expect(bytes && bytes.value() == saved.value(), "SOS-43: sample/callback/commands restore byte-exactly");
                scripts = std::move(restored);
            };
            std::array<unsigned, 2> uses{};
            bool removed = false, previous_sample_fired = false, saved_sample = false, saved_command = false;
            unsigned empty_warnings = 0;
            std::uint64_t sample_tick = 0;
            std::vector<std::string> commands;
            for (std::uint64_t tick = 1; tick <= 320; ++tick) {
                scene.view->tick = tick;
                for (auto& unit : scene.view->units) {
                    if (tick == 40 && unit.id == 60) unit.position.x = sim::math::Fixed::from_integer(150).value();
                    if (unit.id == 50 || unit.id == 51) {
                        if (tick == 70) unit.damage_rate = sim::math::Fixed::from_integer(21).value();
                        if (tick == 100 && unit.id == 51) {
                            unit.abilities.front().autofire = true;
                            unit.hull = sim::math::Fixed::from_integer(20).value();
                        }
                        if (tick == 160 && unit.id == 51) unit.abilities.front().autofire = false;
                        if (tick == 220 && unit.id == 51) unit.abilities.front().autofire = true;
                        if (tick == 280) unit.abilities.front().ready = false;
                    }
                }
                if (tick == 70) {
                    scene.view->units.push_back(sundered_unit(71, 105, 3, 200, 50));
                    scene.view->units.back().shield = Real(1);
                    scene.view->units.push_back(sundered_unit(82, 105, 3, 5200, -50));
                    scene.view->units.back().shield = Real(1);
                    // Point defence only reacts to an affected deadly enemy.
                    ai::EngineContractAccess::threat(engine, 50, 71, 1);
                    ai::EngineContractAccess::threat(engine, 51, 82, 1);
                    std::sort(scene.view->units.begin(), scene.view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
                }
                if (proximity && sample_tick != 0 && tick == sample_tick + 1) {
                    // Keep the old sample across save/load, then move all AI
                    // contacts out before the next due update consumes it.
                    for (auto& unit : scene.view->units) if (unit.owner == 3 && unit.position.x < sim::math::Fixed::from_integer(1000).value())
                        unit.position.x = sim::math::Fixed::from_integer(10000).value();
                    removed = true;
                }
                std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
                std::vector<auth::ScriptDiagnostic> diagnostics;
                expect(foc::tactical_ai_detail::service_objects(executor, scene.host, scripts, sequence, diagnostics, active, pumps).has_value()
                    && diagnostics.empty() && pumps <= active, "SOS-42: migrated attachment work is bounded");
                auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
                auto report = scripts.service(executor, paced);
                if (!report) throw std::runtime_error(report.error().message);
                for (const auto& diagnostic : report.value().diagnostics) std::cerr << diagnostic.code << ' ' << diagnostic.message << '\n';
                for (const auto& diagnostic : report.value().diagnostics) {
                    const bool empty_warning = test.ability == K::corrupt_systems
                        && diagnostic.code == auth::codes::script_warning
                        && diagnostic.message == "Find_Best_Local_Threat_Center -- no objects supplied";
                    expect(empty_warning, "SOS migration: only the sourced empty-sample warning is admitted");
                    if (empty_warning) ++empty_warnings;
                }
                for (const auto& command : report.value().commands) {
                    const auto id = command.issuer - foc::first_object_instance;
                    expect((id == 50 || id == 51) && command.verb == foc::verb_ability,
                        "SOS-44: only an authored ready ability can be commanded");
                    expect(tick > 70, "SOS migration: threshold/deadly-enemy gate holds");
                    expect(command.arguments.size() >= 4 && std::get<Real>(command.arguments[2].data) == Real(static_cast<int>(test.ability)),
                        "SOS migration: only the script's ability is requested");
                    expect(foc::tactical_ai_detail::translate_order(command).has_value(), "SOS migration: command translates to native payload");
                    if (id == 50) { ++uses[0]; if (removed) previous_sample_fired = true; }
                    if (id == 51) {
                        ++uses[1];
                        expect(test.ability != K::corrupt_systems && tick > 100 && (tick < 160 || tick > 220),
                            "SOS migration: human exit and autofire transitions retain their authored gates");
                    }
                    expect(tick < 280, "SOS migration: readiness prevents later commands");
                    commands.push_back(std::to_string(tick) + ":" + std::to_string(id));
                    if (!saved_command) { restore(); saved_command = true; }
                }
                if (mc30 || krayt) {
                    const auto threat = number(50, "nearby_unit_threat");
                    if (tick < 40 && threat) expect(*threat == (mc30 ? Real(0) : Real(600)),
                        "SOS-13/14: promote parent before distance exclusion and deduplicate craft");
                    if (mc30 && tick >= 40 && tick < 70 && threat) expect(*threat <= Real(600),
                        "SOS-13: one promoted team stays below the AI threshold");
                    if (tick >= 70 && threat && *threat >= Real(1200) && sample_tick == 0) {
                        sample_tick = tick; restore(); saved_sample = true;
                    }
                } else if (test.ability == K::corrupt_systems && tick >= 70 && sample_tick == 0) {
                    // This update's collected objects are consumed at the next pump.
                    sample_tick = tick + 30; // leave enough time for the due wrapper to collect
                }
                if (tick == 20) restore(); // before the next due service
                auto hash = scripts.state_hash(executor);
                expect(hash.has_value(), "SOS migration: canonical hash is available");
                if (workers == 1 && hash) reference.push_back(hash.value());
                else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(tick - 1)],
                    "SOS migration: 1/2/4/8 worker hashes agree");
            }
            expect(uses[0] > 0 && (test.ability == K::corrupt_systems ? uses[1] == 0 : uses[1] > 0),
                "SOS migration: stock AI and supported human paths visibly act");
            if (test.ability == K::corrupt_systems) expect(empty_warnings > 0 && uses[0] > 0,
                "WHE-68: empty table warns, returns nil and continues to a later IG-2000 activation");
            if (mc30 || krayt) expect(saved_sample && previous_sample_fired,
                "SOS-13/14/43: restored previous sample acts after current contacts leave");
            if (workers == 1) reference_commands = commands;
            else expect(commands == reference_commands, "SOS migration: 1/2/4/8 effects agree");
            std::cout << test.script << ' ' << workers << " workers AI=" << uses[0] << " human=" << uses[1] << '\n';
        }
    }
}
// SOS-44: data admission must not attach a second decision consumer while a
// native payload profile still owns the same object's script service.
void native_object_script_ownership() {
    const std::array names{"ObjectScript_PowerToShields", "ObjectScript_Krayt", "ObjectScript_MC30",
        "ObjectScript_PointDefense", "ObjectScript_IG2000"};
    for (unsigned kind = 0; kind < names.size(); ++kind) {
        auto setup = slave_setup(); setup.content.types.front().lua_script = names[kind];
        setup.content.types.front().point_defence_script = kind == 3;
        setup.content.types.front().corrupt_systems_script = kind == 4;
        auto scene = slave_scene(setup);
        tactical::TacticalSetup initial;
        initial.players = {{1, 1, 1, tactical::player_flag_commandable}};
        initial.units = {{50, 102, 1, {}, sim::math::identity_quat(), {}}};
        tactical::AbilityProfile ability; ability.kind = tactical::AbilityKind::turbo;
        ability.expiration_frames = 30; ability.recharge_frames = 30;
        tactical::UnitAbilityProfile profile; profile.type_id = 102; profile.abilities = {ability};
        profile.defend_script = kind == 0; profile.self_destruct_script = kind == 1; profile.mc30_script = kind == 2;
        tactical::AbilityTable table; table.profiles = {profile};
        auto created = tactical::TacticalSession::create(initial, {}, {}, {}, std::nullopt, {}, {}, table);
        expect(created.has_value(), "SOS-44: native ownership world creates");
        if (!created) continue;
        auto world = std::move(created).value(); scene.host->world = &world;
        auth::ModuleManifest manifest;
        expect(manifest.add(std::string("Data/Scripts/GameObject/") + names[kind] + ".lua", "function main() end").has_value(),
            "SOS-44: authored module is present despite native ownership");
        auto scheduler = auth::ScriptScheduler::create({}, std::move(manifest));
        if (!scheduler) { expect(false, "SOS-44: ownership scheduler creates"); continue; }
        std::uint64_t sequence = 0; std::uint32_t active = 0, pumps = 0;
        std::vector<auth::ScriptDiagnostic> diagnostics;
        expect(foc::tactical_ai_detail::service_objects(sim::InlineExecutor{}, scene.host, scheduler.value(),
            sequence, diagnostics, active, pumps).has_value() && diagnostics.empty() && active == 0
            && scheduler.value().object_scripts().empty(), "SOS-44: all five native owners suppress duplicate Lua attachment");
    }
}

// SOS-42: production provider, independent partition order and a rebuilt host.
class ReverseObjectTurns final : public sim::PartitionExecutor {
public:
    std::size_t worker_count() const noexcept override { return 8; }
    eawr::core::Result<void> execute(std::size_t count, const std::function<void(std::size_t)>& run) const override {
        for (std::size_t at = count; at != 0; --at) run(at - 1);
        return eawr::core::Result<void>::success();
    }
};

// A test-only reference keeps the previous barrier admission, clock and draw key.
// It reuses the original host helper rather than estimating authored Lua cadence.
class LegacyObjectTurnReference final : public auth::ScriptEngine {
public:
    explicit LegacyObjectTurnReference(std::shared_ptr<foc::detail::Host> host) : host_(std::move(host)) {}
    eawr::core::Result<void> initialize_fresh(const tactical::TacticalSession& world, auth::ScriptScheduler& scripts) override {
        const sim::InlineExecutor executor;
        return prepare(executor,world,scripts);
    }
    eawr::core::Result<auth::ServiceOptions> before_service(const tactical::TacticalSession& world,
        const tactical::TacticalTick&, auth::ScriptScheduler& scripts) override {
        const sim::InlineExecutor executor;
        return options(executor,world,scripts);
    }
    eawr::core::Result<auth::ServiceOptions> before_service(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, const tactical::TacticalTick&, auth::ScriptScheduler& scripts) override {
        return options(executor,world,scripts);
    }
private:
    eawr::core::Result<void> prepare(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, auth::ScriptScheduler& scripts) {
        host_->world=&world; host_->snapshot=world.snapshot();
        host_->view=foc::detail::build_view(world,*host_->snapshot,&executor);
        std::uint64_t sequence{};
        std::uint32_t active{},pumps{};
        std::vector<auth::ScriptDiagnostic> diagnostics;
        auto result=foc::tactical_ai_detail::service_objects(executor,host_,scripts,sequence,diagnostics,active,pumps);
        if (!result) return result;
        if (!diagnostics.empty()) return eawr::core::Result<void>::failure(foc::detail::make_error(diagnostics.front().code,diagnostics.front().message));
        return eawr::core::Result<void>::success();
    }
    eawr::core::Result<auth::ServiceOptions> options(const sim::PartitionExecutor& executor,
        const tactical::TacticalSession& world, auth::ScriptScheduler& scripts) {
        if (auto result=prepare(executor,world,scripts);!result) return eawr::core::Result<auth::ServiceOptions>::failure(result.error());
        auth::ServiceOptions result;
        result.host_paced=[](std::uint64_t) { return true; };
        return eawr::core::Result<auth::ServiceOptions>::success(std::move(result));
    }
    std::shared_ptr<foc::detail::Host> host_;
};

void object_turn_cadence_pins() {
    struct Pin { const char* rule; sim::EntityId object; std::vector<std::uint64_t> before,after; };
    const std::vector<Pin> pins{
        {"SOS-15",10,{1,2,62,122,183},{1,14,75,136,197}},
        {"SOS-19",20,{1,2,62,122,183},{1,15,75,136,197}},
        {"SOS-16",30,{1,25,85,146,206},{1,47,107,167}},
        {"SOS-17",40,{1,4,64,124,185},{1,56,117,178}},
        {"SOS-11",50,{1,5,65,125,186},{1,40,100,161}}};
    const std::map<std::string,std::string> modules{
        {"Data/Scripts/GameObject/PHASE_PROBE.lua",
            "function Base_Definitions() ServiceRate=2; calls=0 end\n"
            "function main() sample=GameRandom.Get_Float(); while true do calls=calls+1; Object.Move_To({calls,0,0}); coroutine.yield(true) end end"},
        {"Data/Scripts/Library/PGAICommands.lua","function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
    const sim::InlineExecutor executor;
    const eawr::platform::ThreadWorkerAdapter checkpoint_workers(4);
    for (const auto& pin : pins) {
        tactical::TacticalSetup initial;
        initial.seed=5678; initial.players={{1,1,1,tactical::player_flag_commandable}};
        initial.units={{pin.object,100,1}};
        foc::AiSetup config;
        config.seed=initial.seed;config.players={{1,"REBEL",false,false,""}};
        foc::AiType type;type.type_id=100;type.lua_script="PHASE_PROBE";config.content.types={type};
        for (const bool legacy : {true,false}) {
            auto world=tactical::TacticalSession::create(initial);
            auto session=[&]() -> eawr::core::Result<auth::ScriptedTacticalSession> {
                if (!legacy) return foc::create_session(std::move(world).value(),config,modules);
                auto host=std::make_shared<foc::detail::Host>();host->setup=config;
                for (const auto& entry : host->setup.content.types) host->types.emplace(entry.type_id,&entry);
                for (const auto& player : host->setup.players) host->players.emplace(player.player,&player);
                auth::ModuleManifest manifest;
                for (const auto& [path,bytes] : modules) expect(bool(manifest.add(path,bytes)),"SOS cadence manifest");
                auth::SessionConfig setup;setup.seed=config.seed;setup.tick_duration={1,30};
                setup.script_directories={"Data/Scripts/Library/","Data/Scripts/GameObject/"};
                auto scripts=auth::ScriptScheduler::create(setup,std::move(manifest));
                std::vector<eawr::core::Diagnostic> errors;
                foc::tactical_ai_detail::register_globals(scripts.value(),host,errors);
                foc::tactical_ai_detail::register_methods(scripts.value(),host,errors);
                expect(errors.empty(),"SOS cadence production bindings");
                auto result=auth::ScriptedTacticalSession::create(std::move(world).value(),std::move(scripts).value());
                expect(bool(result.value().register_verb(foc::verb_move,foc::tactical_ai_detail::translate_order)),"SOS cadence move translator");
                expect(bool(result.value().set_engine(std::make_shared<LegacyObjectTurnReference>(host))),"SOS cadence legacy owner");
                return result;
            }();
            expect(bool(session),"SOS cadence session creates");if (!session) return;
            std::vector<std::uint64_t> actual;
            std::optional<auth::ScriptedTacticalSession> uninterrupted;
            for (std::uint64_t frame=0;frame<220;++frame) {
                auto tick=session.value().step(uninterrupted
                    ? static_cast<const sim::PartitionExecutor&>(checkpoint_workers) : executor);
                expect(bool(tick),"SOS cadence step");if (!tick) return;
                expect(tick.value().scripts.diagnostics.empty(),"SOS cadence has no binding fault");
                if (uninterrupted) {
                    auto control_tick=uninterrupted->step(executor);
                    expect(control_tick && control_tick.value().state_sha256==tick.value().state_sha256,
                        "SOS-43: nonzero-rate checkpoint preserves every later hash across worker counts");
                }
                if (!legacy && frame==8) {
                    const auto saved=session.value().save_scripts();
                    const auto checkpoint=session.value().world().save_checkpoint_state();
                    auto prepared=foc::prepare_session(config,modules);
                    auto rebuilt_world=tactical::TacticalSession::from_replay(session.value().record());
                    expect(saved && checkpoint && prepared && rebuilt_world,"SOS-43: checkpoint between nonzero-rate pumps captures");
                    if (!saved || !checkpoint || !prepared || !rebuilt_world) return;
                    auto loaded=rebuilt_world.value().load_checkpoint_state(checkpoint.value());
                    expect(bool(loaded),"SOS-43: between-pump world checkpoint restores");if (!loaded) return;
                    auto rebuilt=auth::ScriptedTacticalSession::restore(std::move(rebuilt_world).value(),std::move(prepared.value().scripts),saved.value());
                    expect(rebuilt && prepared.value().bind(rebuilt.value()),"SOS-43: between-pump Lua clock and due admission restore");
                    if (!rebuilt) { std::cerr<<rebuilt.error().message<<'\n'; return; }
                    expect(rebuilt.value().save_scripts().value().scheduler==saved.value().scheduler,
                        "SOS-43: between-pump rebind preserves canonical Lua bytes");
                    uninterrupted.emplace(std::move(session).value());session=std::move(rebuilt);
                }
                if (!legacy && frame==1) {
                    auto saved=session.value().save_scripts();
                    auto inspect=foc::prepare_session(config,modules);
                    expect(saved && inspect && inspect.value().scripts.load(saved.value().scheduler),"SOS cadence state inspection");
                    if (saved && inspect) {
                        const auto id=foc::first_object_instance+pin.object;
                        const auto last=inspect.value().scripts.read_global(id,"LastService");
                        const auto sample=inspect.value().scripts.read_global(id,"sample");
                        auto phase=foc::tactical_ai_detail::object_phase_random(config.seed,1,id);
                        auth::RandomStream vm(config.seed,2|auth::turn_random_domain,id);
                        const auto offset=phase.next_unit();
                        const auto draw=Real(vm.next_below(std::uint64_t{1}<<15))/Real(32767);
                        expect(last && sample && last.value() && sample.value()
                            && std::get<Real>(last.value()->data)==Real(0)-offset*Real(2)
                            && std::get<Real>(sample.value()->data)==draw
                            && auth::random_word(config.seed,1|auth::object_phase_random_domain,id,0)
                                != auth::random_word(config.seed,1|auth::turn_random_domain,id,0),
                            "SOS-42: host phase and VM draw use independent per-instance keys");
                    }
                }
                for (const auto& command : tick.value().scripts.commands)
                    if (command.issuer==foc::first_object_instance+pin.object) actual.push_back(frame);
            }
            expect(actual==(legacy?pin.before:pin.after),std::string(pin.rule)+": literal old/new phased pump ticks");
            std::cout<<"SOS-CADENCE,"<<pin.rule<<','<<(legacy?"before":"after");
            for (const auto frame : actual) std::cout<<','<<frame;
            std::cout<<'\n';
        }
    }
}

void production_object_turns() {
    const std::string source = R"LUA(
root_ran=0
function Root() assert(Object.Is_Valid()); root_ran=root_ran+1 end
Create_Thread('Root')
function Base_Definitions() assert(Object==nil); ServiceRate=0; LastService=0; calls=0 end
function main()
    while true do
        calls=calls+1
        Object.Activate_Ability('INVULNERABILITY', true)
        Object.Move_To({calls, 0, 0})
        coroutine.yield(true)
    end
end
)LUA";
    tactical::TacticalSetup setup;
    setup.seed = 5678;
    setup.players = {{1,1,1,tactical::player_flag_commandable}, {2,2,2,tactical::player_flag_commandable}};
    setup.units = {{10,100,1}, {20,101,2}};
    tactical::AbilityProfile invulnerable;
    invulnerable.kind = tactical::AbilityKind::invulnerability;
    invulnerable.expiration_frames = 1;
    invulnerable.recharge_frames = 1;
    tactical::AbilityTable abilities;
    abilities.profiles = {{100, {invulnerable}, false}};
    foc::AiSetup config;
    config.seed = setup.seed;
    config.players = {{1,"REBEL",false,false,""}, {2,"EMPIRE",false,false,""}};
    foc::AiType type; type.type_id=100; type.lua_script="RENAMED_TURN_PROBE";
    config.content.types = {type};
    const std::map<std::string,std::string> modules{
        {"Data/Scripts/GameObject/RENAMED_TURN_PROBE.lua", source},
        {"Data/Scripts/Library/PGAICommands.lua", "function Base_Definitions() EnemyContrastTypes={}; FriendlyContrastTypes={} end"}};
    const auto fresh_world = [&] { return tactical::TacticalSession::create(setup, {}, {}, {}, std::nullopt, {}, {}, abilities); };
    std::vector<std::string> reference;
    const ReverseObjectTurns reverse;
    for (const unsigned workers : {1U,2U,4U,8U,0U}) {
        const eawr::platform::ThreadWorkerAdapter pool(workers == 0 ? 1U : workers);
        const sim::PartitionExecutor& executor = workers == 0 ? static_cast<const sim::PartitionExecutor&>(reverse) : pool;
        auto session = foc::create_session(std::move(fresh_world()).value(), config, modules);
        expect(session.has_value(), "SOS-42: production turn provider installs");
        if (!session) return;
        std::vector<std::string> hashes, world_hashes;
        std::optional<auth::ScriptedTacticalSession> uninterrupted;
        const eawr::platform::ThreadWorkerAdapter continuation(workers == 8 ? 2U : 8U);
        for (unsigned frame=0; frame<9; ++frame) {
            auto tick = session.value().step(frame > 4 ? static_cast<const sim::PartitionExecutor&>(continuation) : executor);
            expect(tick.has_value(), "SOS-42: production object turn completes");
            if (!tick) { std::cerr << tick.error().message << '\n'; return; }
            if (uninterrupted) {
                auto control_tick=uninterrupted->step(executor);
                expect(control_tick && control_tick.value().world.state_sha256==tick.value().world.state_sha256,
                    "SOS-43: restored world effects equal uninterrupted continuation on another worker count");
                expect(control_tick && control_tick.value().state_sha256==tick.value().state_sha256,
                    "SOS-43: restored script state equals uninterrupted continuation on another worker count");
            }
            expect(tick.value().scripts.diagnostics.empty(), "SOS-42: production bindings have no stale-read fault");
            hashes.push_back(tick.value().state_sha256);
            world_hashes.push_back(tick.value().world.state_sha256);
            if (frame == 0) expect(tick.value().world.object_turn.empty(), "SOS-30: initial barrier creates threads without pumping");
            if (frame == 1) {
                expect(tick.value().world.object_turn.size() == 1 && tick.value().world.object_turn[0].reason == tactical::RejectReason::none,
                    "SOS-15: first activation lands at tick 1, one tick before the previous bridge route");
                expect(tick.value().script_input.empty() && session.value().pending_script_commands() == 1,
                    "SOS-42: ability uses commit now while Move_To waits for the next tick");
            }
            if (frame == 4) {
                auto saved = session.value().save_scripts();
                auto checkpoint = session.value().world().save_checkpoint_state();
                auto world = tactical::TacticalSession::from_replay(session.value().record(), {}, {}, {}, std::nullopt, {}, {}, abilities);
                auto prepared = foc::prepare_session(config, modules);
                expect(saved && checkpoint && world && prepared, "SOS-43: turn barrier checkpoint captures both owners");
                if (!saved || !checkpoint || !world || !prepared) return;
                auto loaded = world.value().load_checkpoint_state(checkpoint.value());
                expect(loaded.has_value(), "SOS-43: turn world restores");
                if (!loaded) { std::cerr << loaded.error().message << '\n'; return; }
                auto rebuilt = auth::ScriptedTacticalSession::restore(std::move(world).value(), std::move(prepared.value().scripts), saved.value());
                if (!rebuilt) { expect(false,"SOS-43: bridge restore"); return; }
                const auto before_bind=rebuilt.value().save_scripts();
                const auto compare_restore=[](std::string_view stage,const std::string& before,const std::string& after) {
                    if (before==after) return;
                    std::size_t offset{};
                    while (offset<std::min(before.size(),after.size()) && before[offset]==after[offset]) ++offset;
                    std::cout<<"SOS-RESTORE-DELTA,"<<stage<<','<<before.size()<<','<<after.size()<<','<<offset;
                    for (std::size_t index=offset;index<std::min(offset+24,std::min(before.size(),after.size()));++index)
                        std::cout<<','<<static_cast<unsigned>(static_cast<unsigned char>(before[index]))
                            <<':'<<static_cast<unsigned>(static_cast<unsigned char>(after[index]));
                    std::cout<<'\n';
                };
                expect(before_bind.has_value(),"SOS-43: restored scheduler saves before bind");
                if (!before_bind) return;
                compare_restore("load",saved.value().scheduler,before_bind.value().scheduler);
                expect(bool(prepared.value().bind(rebuilt.value())), "SOS-43: fresh host derives due set from restored Lua");
                const auto after_bind=rebuilt.value().save_scripts();
                expect(after_bind.has_value(),"SOS-43: rebound scheduler saves");
                if (!after_bind) return;
                compare_restore("bind",before_bind.value().scheduler,after_bind.value().scheduler);
                expect(after_bind.value().scheduler == saved.value().scheduler, "SOS-43: rebuild never initializes twice");
                uninterrupted.emplace(std::move(session).value());
                session = std::move(rebuilt);
            }
        }
        auto recording = session.value().record();
        expect(recording.object_turns.size() >= 3 && recording.object_turns.front().tick == 1,
            "SOS-15/42: object-turn activations are retained in the world replay");
        auto replayed = tactical::TacticalSession::from_replay(recording, {}, {}, {}, std::nullopt, {}, {}, abilities);
        expect(replayed.has_value(), "SOS-42: world-only replay loads without a provider");
        if (!replayed) return;
        for (std::size_t frame=0; frame<world_hashes.size(); ++frame) {
            auto tick = replayed.value().step(executor);
            expect(tick && tick.value().state_sha256 == world_hashes[frame], "SOS-42: record reproduces every same-turn world effect");
        }
        if (reference.empty()) reference=hashes;
        else expect(reference == hashes, "SOS-42/43: 1/2/4/8 and reverse partitions produce equal turn states");
    }
    {
        foc::detail::Host host;
        auto world=fresh_world();host.world=&world.value();host.snapshot=world.value().snapshot();
        host.view=foc::detail::build_view(world.value(),*host.snapshot);
        foc::ai::TacticalWorld capabilities(host);host.in_object_turn=true;
        const std::vector<std::function<void()>> reads{
            [&] { static_cast<void>(host.economy(1)); },
            [&] { static_cast<void>(capabilities.account(1)); },
            [&] { static_cast<void>(capabilities.pads()); },
            [&] { static_cast<void>(capabilities.arriving(10)); },
            [&] { static_cast<void>(capabilities.build_allowed(1,10,100)); },
            [&] { static_cast<void>(capabilities.build_option(1,10,100)); },
            [&] { static_cast<void>(capabilities.production_counts(1,100)); },
            [&] { static_cast<void>(capabilities.construction_allowed(1,10)); },
            [&] { static_cast<void>(capabilities.constructing_for(10,1)); },
            [&] { static_cast<void>(capabilities.next_entity_id()); },
            [&] { static_cast<void>(capabilities.reinforcement_search_result(1,1)); },
            [&] { static_cast<void>(capabilities.events()); },
            [&] { static_cast<void>(capabilities.combat_events()); }};
        for (const auto& read : reads) {
            host.rejected_turn_read.store(false);read();
            expect(host.rejected_turn_read.load(),"SOS-42: dynamic capability rejects stale reads independently of the Lua API name");
        }
        host.rejected_turn_read.store(false);
        static_cast<void>(capabilities.view());static_cast<void>(capabilities.players());
        static_cast<void>(capabilities.economy());static_cast<void>(capabilities.completed_tick());
        expect(!host.rejected_turn_read.load(),"SOS-42: immutable rules and staged rows stay readable");
    }
    // Dynamic completed-world queries must diagnose rather than silently read t-1.
    const sim::InlineExecutor executor;
    for (const auto* query : {"Object.Get_Owner().Get_Credits()", "Find_Nearest(Object,Object.Get_Owner(),false)"}) {
        auto guarded_modules = modules;
        guarded_modules["Data/Scripts/GameObject/RENAMED_TURN_PROBE.lua"] =
            std::string("function Base_Definitions() ServiceRate=0 end\n")
            + "function main() Object.Activate_Ability('INVULNERABILITY',true); " + query + " end";
        auto guarded = foc::create_session(std::move(fresh_world()).value(), config, guarded_modules);
        expect(guarded && guarded.value().step(executor), "SOS-42: tripwire instance initializes");
        if (!guarded) return;
        auto tick = guarded.value().step(executor);
        expect(tick && !tick.value().scripts.diagnostics.empty() && guarded.value().record().object_turns.empty(),
            "SOS-42: stale-read tripwire faults the instance and drops its entire turn output");
    }
    {
        auto host=std::make_shared<foc::detail::Host>();
        auto view=std::make_shared<foc::detail::WorldView>();
        foc::AiType held;held.type_id=100;held.lua_script="UNADMITTED_TURN";
        host->setup.content.types={held};host->types.emplace(100,&host->setup.content.types.front());
        foc::detail::ViewUnit object;object.id=10;object.type=100;object.owner=1;
        tactical::AbilityStatus ability;ability.kind=tactical::AbilityKind::buzz_droids;ability.ready=true;
        object.abilities={ability};view->units={object};host->view=view;
        auth::ModuleManifest manifest;
        expect(bool(manifest.add("Data/Scripts/GameObject/UNADMITTED_TURN.lua",
            "function Base_Definitions() ServiceRate=0 end\nfunction main() Object.Activate_Ability('BUZZ_DROIDS',{0,0,0}) end")),
            "SOS-45: arbitrary module for an unadmitted turn handler");
        auth::SessionConfig script_config;script_config.tick_duration={1,30};script_config.script_directories={"Data/Scripts/GameObject/"};
        auto scripts=auth::ScriptScheduler::create(script_config,std::move(manifest));
        std::vector<eawr::core::Diagnostic> errors;
        foc::tactical_ai_detail::register_globals(scripts.value(),host,errors);
        foc::tactical_ai_detail::register_methods(scripts.value(),host,errors);
        std::uint64_t sequence{};std::uint32_t active{},pumps{};std::vector<auth::ScriptDiagnostic> diagnostics;
        expect(errors.empty() && foc::tactical_ai_detail::service_objects(executor,host,scripts.value(),sequence,diagnostics,active,pumps),
            "SOS-45: arbitrary turn module attaches");
        expect(bool(scripts.value().service(executor)),"SOS-45: arbitrary turn module initializes");
        host->in_object_turn=true;view->tick=1;sequence=0;
        expect(bool(foc::tactical_ai_detail::service_objects(executor,host,scripts.value(),sequence,diagnostics,active,pumps)),
            "SOS-45: arbitrary turn module becomes due");
        auto report=scripts.value().service_turn(executor,{foc::first_object_instance+10});
        expect(report && report.value().commands.empty() && std::any_of(report.value().diagnostics.begin(),report.value().diagnostics.end(),
            [](const auto& diagnostic) { return diagnostic.code==auth::codes::unsupported_api; }),
            "SOS-45: unadmitted ability handler is exposed for every module instead of silently dropping a ready use");
    }

}

void object_script_contracts() {
    const auto fixed = [](int n) { return sim::math::Fixed::from_integer(n).value(); };
    const std::string falcon = R"LUA(
function Base_Definitions()
    assert(Object == nil)
    definitions = (definitions or 0) + 1
    ServiceRate = 1
    samples = 0
    Register_Event('Sample', function() samples = samples + 1 end)
end
function main()
    assert(Object.Is_Valid())
    while true do
        local human = Object.Get_Owner().Is_Human()
        local threat = FindDeadlyEnemy(Object)
        local low = Object.Get_Shield() < 0.2
        if human then low = Object.Is_Ability_Autofire('INVULNERABILITY') and
            Object.Get_Shield() < 0.05 and Object.Get_Hull() < 0.5 end
        if threat and low and Object.Is_Ability_Ready('INVULNERABILITY') then
            Object.Activate_Ability('INVULNERABILITY', true)
        end
        coroutine.yield(true)
    end
end
)LUA";
    const std::string vader = R"LUA(
function Base_Definitions() assert(Object == nil); ServiceRate = 1 end
function main()
    while true do
        if (not Object.Get_Owner().Is_Human() or Object.Is_Ability_Autofire('REPLENISH_WINGMEN')) and
            Object.Get_Contained_Object_Count() < 2 and Object.Is_Ability_Ready('REPLENISH_WINGMEN') then
            Object.Activate_Ability('REPLENISH_WINGMEN', true)
        end
        coroutine.yield(true)
    end
end
)LUA";
    std::vector<std::string> reference;
    for (const auto workers : {1U, 2U, 4U, 8U}) {
        const eawr::platform::ThreadWorkerAdapter executor(workers);
        auto host = std::make_shared<foc::detail::Host>();
        auto view = std::make_shared<foc::detail::WorldView>();
        host->view = view;
        foc::AiType ship; ship.type_id = 100; ship.lua_script = "ObjectScript_MillenniumFalcon";
        foc::AiType team; team.type_id = 101; team.lua_script = "ObjectScript_VaderTIESquadron";
        host->setup.content.types = {ship, team};
        for (const auto& type : host->setup.content.types) host->types.emplace(type.type_id, &type);
        host->setup.players = {{1, "REBEL", false, true, {}, false}, {2, "EMPIRE", false, true, {}, false}};
        for (const auto& player : host->setup.players) host->players.emplace(player.player, &player);
        foc::detail::ViewUnit object;
        object.id = 10; object.type = 100; object.owner = 1; object.hull = fixed(40); object.max_hull = fixed(100);
        object.shield = Real(1) / Real(10);
        object.abilities = {{tactical::AbilityKind::invulnerability, false, true, true}};
        foc::detail::ViewUnit enemy; enemy.id = 20; enemy.owner = 2;
        foc::detail::ViewUnit container; container.id = 30; container.type = 101; container.owner = 1;
        container.container = true; container.members = {31};
        foc::detail::ViewUnit leader; leader.id = 31; leader.owner = 1;
        leader.abilities = {{tactical::AbilityKind::replenish_wingmen, false, true, true}};
        view->units = {object, enemy, container, leader};
        ai::Engine engine(host, {}, {});
        host->engine = &engine;
        ai::EngineContractAccess::threat(engine, 10, 20, 1);
        const auto make_scripts = [&]() {
            auth::ModuleManifest modules;
            expect(modules.add("Data/Scripts/GameObject/OBJECTSCRIPT_MILLENNIUMFALCON.lua", falcon).has_value(), "SOS: Falcon module");
            expect(modules.add("Data/Scripts/GameObject/OBJECTSCRIPT_VADERTIESQUADRON.lua", vader).has_value(), "SOS: Vader module");
            expect(modules.add("Finished.lua", "function Base_Definitions() ServiceRate=0 end; function main() end").has_value(), "SOS: completion module");
            auth::SessionConfig config; config.tick_duration = {1, 30};
            auto created = auth::ScriptScheduler::create(config, std::move(modules));
            if (!created) throw std::runtime_error(created.error().message);
            auto scripts = std::move(created).value();
            std::vector<eawr::core::Diagnostic> errors;
            foc::tactical_ai_detail::register_globals(scripts, host, errors);
            foc::tactical_ai_detail::register_methods(scripts, host, errors);
            expect(errors.empty(), "SOS: production bindings");
            return scripts;
        };
        auto scripts = make_scripts();
        std::uint32_t active = 0, pumps = 0;
        std::vector<auth::ScriptDiagnostic> diagnostics;
        const auto prepare = [&]() {
            std::uint64_t sequence = 0;
            expect(foc::tactical_ai_detail::service_objects(executor, host, scripts, sequence, diagnostics, active, pumps).has_value(),
                "SOS: partitioned attachment and admission");
        };
        const auto tick = [&]() {
            view->tick = scripts.completed_tick() + 1;
            prepare();
            auth::ServiceOptions options; options.host_paced = [](std::uint64_t) { return true; };
            auto result = scripts.service(executor, options);
            if (!result) throw std::runtime_error(result.error().message);
            expect(result.value().diagnostics.empty() && diagnostics.empty(), "SOS: no script diagnostics");
            return std::move(result).value();
        };
        const auto restore = [&]() {
            auto before = scripts.save(executor);
            if (!before) throw std::runtime_error(before.error().message);
            auto fresh = make_scripts();
            expect(fresh.load(before.value()).has_value(), "SOS-32/43: restore into fresh scheduler");
            auto after = fresh.save();
            expect(after && after.value() == before.value(), "SOS-43: canonical restore bytes and callback identity");
            scripts = std::move(fresh);
        };
        prepare();
        expect(active == 2 && scripts.instances().size() == 2 && pumps == 0, "SOS-40: attach starting ship and real container once");
        restore(); // before definitions or any due service
        auto initialized = tick();
        expect(initialized.commands.empty() && pumps == 0, "SOS-30: definitions/main creation do not pump main");
        expect(scripts.reconcile_objects(scripts.object_scripts()).has_value() && scripts.instances().size() == 2,
            "SOS-40: repeated admission does not attach twice");
        restore();
        auto triggered = tick();
        expect(triggered.commands.size() == 2 && pumps == 2, "SOS-15/19: AI thresholds request Falcon and leader replenishment");
        restore(); // after commands were emitted, before downstream application
        auth::ScriptEvent sample; sample.key = {scripts.completed_tick()+1, auth::first_simulation_producer+2, 10, 0};
        sample.target = foc::first_object_instance + 10; sample.kind = auth::ScriptEvent::Kind::dispatch; sample.name = "Sample";
        expect(scripts.submit_event(sample).has_value(), "SOS-43: retained callback event");
        tick(); restore();
        auto samples = scripts.read_global(sample.target, "samples");
        expect(samples && samples.value() && std::get<Real>(samples.value()->data) == Real(1), "SOS-43: callback runs once across restore");
        auto definitions = scripts.read_global(sample.target, "definitions");
        expect(definitions && definitions.value() && std::get<Real>(definitions.value()->data) == Real(1), "SOS-32: restore never repeats definitions");
        // Exact due equality holds; pause is the absence of a world/script tick.
        auto snapshot = scripts.save();
        expect(snapshot && scripts.save().value() == snapshot.value(), "SOS: paused barriers do not advance state or pump");
        std::uint32_t human_uses = 0;
        for (int frame = 4; frame < 75; ++frame) {
            if (frame == 4) {
                // Human shield 0.1 fails, despite low hull and autofire.
                host->setup.players[0].ai = false;
                host->setup.players[0].human = true;
                view->units[2].members.push_back(32); // stale absent member must not count
            }
            if (frame == 36) view->units[0].shield = Real(1) / Real(100);
            if (frame == 40) view->units[3].abilities.clear();
            auto result = tick();
            if (frame < 36) for (const auto& command : result.commands)
                expect(command.issuer != sample.target, "SOS-15: human threshold is stricter than AI");
            for (const auto& command : result.commands) {
                if (command.issuer == sample.target && frame >= 36) ++human_uses;
                if (frame >= 40) expect(command.issuer != foc::first_object_instance+30,
                    "SOS-19: inherited script without authored readiness does not activate");
            }
            if (frame == 50) restore();
            auto hash = scripts.state_hash(executor);
            expect(hash.has_value(), "SOS-43: attachment state hashes");
            if (workers == 1 && hash) reference.push_back(hash.value());
            else if (hash) expect(hash.value() == reference[static_cast<std::size_t>(frame-4)], "SOS-42: 1/2/4/8 state equality");
        }
        expect(human_uses > 0, "SOS-15: human low shield/hull with autofire fires");
        sample.key.tick = scripts.completed_tick()+5;
        expect(scripts.submit_event(sample).has_value(), "SOS-44: stale future callback queued before destruction");
        // Destruction drops callbacks/events and stale generations cannot restart.
        view->units.erase(view->units.begin()); prepare();
        expect(scripts.instances().size() == 1 && scripts.object_scripts().size() == 1, "SOS-33/44: destruction retires registry and Lua state");
        view->units.insert(view->units.begin(), object); view->units[0].id = 40;
        std::sort(view->units.begin(), view->units.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
        tick();
        expect(scripts.instances().size() == 2 && scripts.instances().back() == foc::first_object_instance+40, "SOS-40/44: replacement/spawn uses fresh allocation generation");
        auto completed = scripts.reconcile_objects({{{1, 50}, 1, foc::first_object_instance+50, "Finished.lua"}});
        expect(completed && completed.value().empty(), "SOS: completion instance admitted");
        auth::ServiceOptions paced; paced.host_paced = [](std::uint64_t) { return true; };
        expect(scripts.service(executor, paced).has_value(), "SOS: completion initialized");
        auth::ScriptEvent pump; pump.key = {scripts.completed_tick()+1, auth::first_simulation_producer, 50, 0};
        pump.target = foc::first_object_instance+50; pump.kind = auth::ScriptEvent::Kind::pump;
        expect(scripts.submit_event(pump).has_value() && scripts.service(executor, paced).has_value(), "SOS: completion pumped");
        expect(scripts.instances().empty(), "SOS-31/44: finished main detaches");
        expect(scripts.reconcile_objects({{{1, 50}, 1, pump.target, "Finished.lua"}}).has_value(), "SOS: ended identity retained");
        expect(scripts.instances().empty(), "SOS-44: finished attachment never restarts");
        restore();
        auto replacement = scripts.reconcile_objects({{{1, 50}, 2, pump.target, "Finished.lua"}});
        expect(replacement && scripts.instances().size() == 1, "SOS-44: a new generation receives a fresh attachment");
        expect(scripts.reconcile_objects({}).has_value() && scripts.instances().empty(), "SOS: explicit retirement");
        auto missing = scripts.reconcile_objects({{{1, 51}, 1, pump.target+1, "Absent.lua"}});
        expect(missing && missing.value().size() == 1 && missing.value()[0].code == auth::codes::module_rejected,
            "SOS-40: unsupported module diagnoses once");
        auto retry = scripts.reconcile_objects({{{1, 51}, 1, pump.target+1, "Absent.lua"}});
        expect(retry && retry.value().empty() && scripts.instances().empty(), "SOS: rejected attachment does not retry each tick");
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--stock-object-scripts") {
        std::string root;
        if (argc > 2) root = argv[2];
        else {
#ifdef _WIN32
            char* value = nullptr;
            std::size_t size = 0;
            if (_dupenv_s(&value, &size, "EAWR_EAW_GAME_ROOT") == 0 && value != nullptr) root = value;
            std::free(value);
#else
            if (const auto* value = std::getenv("EAWR_EAW_GAME_ROOT")) root = value;
#endif
        }
        if (root.empty()) { std::cout << "SKIPPED: installed game root missing\n"; return 0; }
        stock_object_scripts(root);
        stock_slave_i(root);
        stock_sundered_heart(root);
        stock_missile_shield(root);
        stock_script_admission(root);
        return failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--object-scripts") {
        object_turn_cadence_pins();
        production_object_turns();
        object_script_contracts();
        native_object_script_ownership();
        slave_i_contracts();
        sundered_contracts();
        missile_shield_contracts();
        std::cout << "Object script contracts: " << failures << " failures\n";
        return failures == 0 ? 0 : 1;
    }
    // FH-11: retain binary32 rounding at both operations, including large frame counts.
    const auto saved_rounding = std::fegetround();
    std::fesetround(FE_TONEAREST);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> mode_vectors;
    const auto add_mode_vector = [&](std::uint64_t frame) {
        volatile float frames = static_cast<float>(frame);
        volatile float inverse = 1.0F / 30.0F;
        volatile float seconds = frames * inverse;
        mode_vectors.emplace_back(frame, std::bit_cast<std::uint64_t>(static_cast<double>(seconds)));
    };
    for (std::uint64_t frame = 0; frame < 10000; ++frame) add_mode_vector(frame);
    for (unsigned bit = 24; bit < 64; ++bit) {
        const auto pivot = std::uint64_t{1} << bit;
        for (std::uint64_t offset = 0; offset < 32; ++offset) {
            add_mode_vector(pivot - offset);
            add_mode_vector(pivot + offset);
        }
    }
    add_mode_vector(std::numeric_limits<std::uint64_t>::max());
    for (const auto rounding : {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO}) {
        std::fesetround(rounding);
        for (const auto& [frame, expected] : mode_vectors) {
            expect(foc::tactical_ai_detail::mode_seconds(frame).repr == expected,
                "FH-11: integer mode time preserves nearest-even binary32 independently of host rounding");
        }
    }
    std::fesetround(saved_rounding);
    const std::string saved_locale = std::setlocale(LC_CTYPE, nullptr);
    for (const auto* locale : {"C", "", "tr_TR.UTF-8", "Turkish_Turkey.1254"}) {
        if (!std::setlocale(LC_CTYPE, locale)) continue;
        for (unsigned value = 0; value < 256; ++value) {
            const auto byte = static_cast<unsigned char>(value);
            expect(eawr::core::ascii_lower(byte) == (value >= 'A' && value <= 'Z' ? value + 32 : value),
                "LI-4: ASCII case conversion preserves every non-ASCII byte across locales");
        }
    }
    std::setlocale(LC_CTYPE, saved_locale.c_str());
    if (argc == 2) {
        const std::string mode = argv[1];
        if (mode == "--spending-pad-states") spending_pad_states();
        else if (mode == "--spending-wallet") spending_wallet_reservations();
        else if (mode == "--spending-pad-refunds") spending_pad_refunds();
        else if (mode == "--reachable-target") { reachable_target_contract(); find_target_reach_contract(); }
        else if (mode == "--installed-galactic-freestore") installed_galactic_freestore();
        else if (mode == "--player-streams") tactical_player_streams();
        else if (mode == "--pool-retirement") retiring_pool_reservations();
        else return 2;
        return failures == 0 ? 0 : 1;
    }
    mode_world_contract();
    player_ai_admission_contract();
    scoring_startup_contract();
    tactical_player_streams();
    player_runtime_ownership();
    player_effects();
    shared_pad_competition();
    effects_error_path();
    freestore_execution_hook_order();
    equations();
    random_and_hash();
    game_random_forms();
    running_target_contrast();
    running_reservation_contrast();
    retiring_pool_reservations();
    active_tactical_desire();
    active_tactical_preference();
    initial_goal_budget();
    capture_target_application();
    forced_object_fog();
    collect_free_categories();
    freestore_unit_service();
    time_till_dead_bindings();
    type_range_and_hero_bindings();
    order_destinations();
    special_weapon_bindings();
    manual_special_weapon_binding();
    special_weapon_attachment();
    selection_team_starts();
    threat_grid();
    unit_ai_grid_admission();
    shared_view_prepass();
    shared_view_formation_join();
    shared_input_prepasses();
    threat_preparation();
    plan_definition_paths();
    mounted_ai_discovery();
    authored_goal_extension();
    unit_concentration();
    coroutine_module_loading();
    movement_query_projection();
    mod_ai_calls();
    taskforce_definitions();
    named_team_authored_order();
    category_team_table_order();
    required_category_unions();
    scheduler_reads();
    optional_taskforce_startup();
    taskforce_team_order();
    hazard_plan_events();
    squadron_ability_context();
    squadron_target_in_range();
    buzz_droids_lua_point();
    buzz_droids_lua_point(true);
    corrupt_systems_stand_in();
    corrupt_systems_cluster_coordinates();
    point_defence_stand_in();
    squadron_movement_blocks();
    guard_block_lifecycle();
    refused_attack_block();
    attack_positioning();
    target_choice();
    reachable_target_contract();
    find_target_reach_contract();
    threat_decay_constant();
    faction_controller_inventory();
    proximity_contracts();
    reveal_all_contracts();
    economy_inputs();
    campaign_defender_fallback();
    campaign_retreat_inputs();
    station_barrier_expiry();
    shared_station_perception();
    tactical_activation_estimate();
    production_lifecycle();
    spending_pad_states();
    spending_pad_refunds();
    spending_wallet_reservations();
    allied_resource_upgrade_selection();
    completed_purchases_are_available();
    reinforcement_work_budget();
    reinforcement_service_budget();
    duplicate_reinforcement_calls();
    taskforce_attachment_waits_for_arrival();
    concurrent_reserved_arrivals();
    reinforcement_gravity_well();
    reinforcement_fog_persistence();
    reinforcement_population_wait();
    ai_ability_bound();
    stealth_blind_queries();
    if (failures != 0) {
        std::cerr << failures << " FoC AI contract(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
