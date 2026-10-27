#include "../../support/env.hpp"
#include "../../support/check.hpp"
#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_world.hpp"
#include "tactical_ai_internal.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "mode_time.hpp"
#include "galactic_ai_goal_state.hpp"
#include "eawr/core/ascii.hpp"
#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/platform/battle_scoring.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cfenv>
#include <clocale>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ios>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::script::foc::ai {

// Fixture setup only; execution and admission use the production paths.
std::shared_ptr<Host> make_tactical_host() {
    auto host = std::make_shared<Host>();
    host->ai_world = std::make_shared<TacticalWorld>(*host);
    return host;
}

    PlayerAi& EngineContractAccess::add_player(Engine& engine, tactical::PlayerId id) {
        PlayerAi row; row.player = id; engine.players_.push_back(std::move(row));

        return engine.players_.back();
    }
    PlayerAi& EngineContractAccess::fixture_owner(Engine& engine, tactical::PlayerId id) {
        if (auto* owner = engine.player_ai(id)) return *owner;
        return add_player(engine, id);
    }
    const PlayerAi& EngineContractAccess::fixture_owner(const Engine& engine, tactical::PlayerId id) {
        const auto* owner = engine.player_ai(id);
        if (!owner) throw std::runtime_error("synthetic owner missing");
        return *owner;
    }

    ThreatGrid& EngineContractAccess::grid(Engine& engine) { return engine.grid_; }
    void EngineContractAccess::prepare_inputs(Engine& engine, const sim::PartitionExecutor& executor) {
        engine.prepare_producers(&executor); engine.prepare_pad_inputs(&executor);
    }
    void EngineContractAccess::prepare_pads(Engine& engine, const sim::PartitionExecutor& executor) { engine.prepare_pad_inputs(&executor); }
    void EngineContractAccess::request_open(Engine& engine, tactical::PlayerId player) { engine.pad_open_requested_.insert(player); }
    bool EngineContractAccess::has_prepared_open(const Engine& engine, tactical::PlayerId player) {
        if (!engine.pad_inputs_) return false;
        for (const auto& row : *engine.pad_inputs_) for (const auto& buyer : row.buyers)
            if (buyer.player == player && buyer.can_build && buyer.open.has_value()) return true;
        return false;
    }
    void EngineContractAccess::reset_pads(Engine& engine) { engine.pad_inputs_.reset(); }
    bool EngineContractAccess::has_prepared_pads(const Engine& engine) { return engine.pad_inputs_.has_value(); }
    const std::vector<sim::EntityId>& EngineContractAccess::producers(const Engine& engine, tactical::PlayerId player, tactical::TypeId type) {
        return engine.producers_.at({player, type});
    }
    void EngineContractAccess::reserve_pad(Engine& engine, sim::EntityId pad, tactical::PlayerId player, tactical::TypeId type) {
        engine.pad_reservations_[pad] = {player, 1, type, false, 0};
    }
    void EngineContractAccess::release_pad(Engine& engine, sim::EntityId pad) { engine.pad_reservations_.erase(pad); }
    bool EngineContractAccess::has_prepared_station_levels(const Engine& engine) { return engine.station_levels_.has_value(); }
    std::optional<Real> EngineContractAccess::prepared_station_level(Engine& engine, tactical::PlayerId player,
        const sim::PartitionExecutor& executor) {
        engine.host_->view = detail::build_view(*engine.host_->world, *engine.host_->snapshot, &executor);
        engine.prepare_station_levels(&executor);
        const auto result = engine.evaluate("Level", player, nullptr);
        engine.station_levels_.reset();
        return result;
    }
    PlayerAi& EngineContractAccess::collect_fixture(Engine& engine) {
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
    const PlayerAi* EngineContractAccess::player(const Engine& engine, tactical::PlayerId id) { return engine.player_ai(id); }
    void EngineContractAccess::initialize_goals(Engine& engine, PlayerAi& player) { engine.initialize_goals(player); }
    void EngineContractAccess::build_targets(Engine& engine, PlayerAi& player) { engine.build_targets(player); }
    void EngineContractAccess::update_targets(Engine& engine) { engine.update_targets(); }
    bool EngineContractAccess::matches(const Engine& engine, tactical::PlayerId player, const std::set<std::string>& flags,
        const Target& target) { return engine.target_matches(player, flags, &target); }
    void EngineContractAccess::target_fixture(Engine& engine, std::uint64_t id, sim::EntityId object) {
        Target target;
        target.id = id;
        target.object = object;
        engine.targets_.emplace(id, target);
    }
    void EngineContractAccess::order_fixture(Engine& engine, std::vector<sim::EntityId> members) {
        collect_fixture(engine);
        fixture_owner(engine).taskforces.at(1).units = std::move(members);
        target_fixture(engine, 1, 11);
        target_fixture(engine, 2, 0);
        target_fixture(engine, 3, 999);
    }
    void EngineContractAccess::proposal_fixture(Engine& engine) {
        order_fixture(engine, {10, 11, 12});
        Plan plan; plan.id = plan.instance = plan.goal = 2; plan.player = 2; plan.taskforces = {2};
        fixture_owner(engine).running.emplace(2, plan); fixture_owner(engine).plan_of_instance[2] = 2;
        TaskForce force; force.id = force.plan = 2; force.units = {10};
        fixture_owner(engine).taskforces.emplace(2, force);

        engine.rebuild_runtime_index();
    }
    void EngineContractAccess::add_find_target(Engine& engine, std::uint64_t id, sim::EntityId object) {
        target_fixture(engine, id, object); engine.players_.back().targets.push_back(id);
    }
    void EngineContractAccess::find_target_fixture(Engine& engine) {
        order_fixture(engine, {10});
        engine.players_.back().targets = {1, 2, 3};
    }
    void EngineContractAccess::propose(Engine& engine, PlayerAi& player) { engine.propose(player); }
    std::uint32_t EngineContractAccess::evaluated(const Engine& engine) { return engine.work_.goals_evaluated; }
    void EngineContractAccess::goals(Engine& engine, PlayerAi& player) { engine.service_goals(player); }
    std::uint32_t EngineContractAccess::maintenances(const Engine& engine) { return engine.work_.maintenances; }
    std::int64_t EngineContractAccess::admission_budget(const Engine& engine) { return engine.proposal_budget(100000); }
    void EngineContractAccess::maintain(Engine& engine, PlayerAi& player) { engine.maintain(player); }
    PlayerAi& EngineContractAccess::active_desire_fixture(Engine& engine, const GoalType& goal,
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
    void EngineContractAccess::contrast_fixture(Engine& engine, PlayerAi& player) {
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
    bool EngineContractAccess::contrast(Engine& engine, PlayerAi& player) {
        return engine.test_target_contrast(player, player.active.front());
    }
    void EngineContractAccess::retain_pad(Engine& engine, std::uint64_t pad) { fixture_owner(engine).running.at(1).reserved_pads = {pad}; }
    void EngineContractAccess::release_retained_pad(Engine& engine) { fixture_owner(engine).running.at(1).reserved_pads.clear(); }
    bool EngineContractAccess::completed_pad_contrast(Engine& engine, PlayerAi& player) {
        Goal goal; goal.id = goal.plan = 1; goal.potential.emplace();
        auto& selection = *goal.potential; selection.valid = selection.reserved = selection.funded = true;
        selection.units = {30}; selection.freestore = {0}; selection.sources = {2};
        selection.producers = {1}; selection.pool_tokens = {0}; selection.contrast_indices = {0};
        return engine.test_target_contrast(player, goal);
    }
    bool EngineContractAccess::selection(Engine& engine, PlayerAi& player, PotentialPlan& potential, std::uint32_t seed,
        std::uint64_t goal_id) {
        player.sync.set_seed(seed);
        Goal goal;
        goal.id = goal_id;
        return engine.select_units(player, goal, potential);
    }
    const PlayerAi& EngineContractAccess::checked_player(const Engine& engine, tactical::PlayerId id) {
        const auto* player = engine.player_ai(id);
        if (!player) throw std::runtime_error("synthetic AI player missing");
        return *player;
    }
    PlayerAi& EngineContractAccess::checked_player(Engine& engine, tactical::PlayerId id) {
        auto* player = engine.player_ai(id);
        if (!player) throw std::runtime_error("synthetic AI player missing");
        return *player;
    }
    std::uint32_t EngineContractAccess::selection_seed(const Engine& engine, tactical::PlayerId player) { return checked_player(engine, player).sync.seed(); }
    void EngineContractAccess::set_selection_seed(Engine& engine, tactical::PlayerId player, std::uint32_t seed) { checked_player(engine, player).sync.set_seed(seed); }
    std::uint32_t EngineContractAccess::engine_seed(const Engine& engine) { return engine.sync_.seed(); }
    std::uint32_t EngineContractAccess::draw(Engine& engine, tactical::PlayerId player) { return engine.stream(checked_player(engine, player)).next(); }
    void EngineContractAccess::retain_plan_without_instance(Engine& engine, tactical::PlayerId id, bool exited) {
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
    void EngineContractAccess::ownership_fixture(Engine& engine, authoritative::ScriptScheduler& scripts) {
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
    void EngineContractAccess::foreign_task(Engine& engine) {
        BuildTask task; task.taskforce = 2; task.source = 1; task.type = 1; task.pool_token = 77;
        auto& owner = checked_player(engine, 9); owner.tasks.push_back(task); engine.service_execution(owner);
    }
    void EngineContractAccess::restore_foreign_units(Engine& engine) { checked_player(engine, 7).taskforces.at(2).units = {999}; }
    core::Result<void> EngineContractAccess::commit(Engine& engine, authoritative::ScriptScheduler& scripts) { return engine.commit_effects(scripts); }
    bool EngineContractAccess::staging(const Engine& engine) { return engine.staged() != nullptr; }
    const EffectCounts& EngineContractAccess::totals(const Engine& engine) { return engine.effect_totals_; }
    const PlayerEffects& EngineContractAccess::effects_of(const Engine& engine, tactical::PlayerId id) { return checked_player(engine, id).effects; }
    void EngineContractAccess::emit(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::uint64_t force_id, std::string_view event) {
        auto& player = checked_player(engine, owner);
        engine.emit_call(player.running.at(plan_id), player.taskforces.at(force_id), event, {});
    }
    void EngineContractAccess::signal(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::uint64_t force_id, std::string_view event) {
        auto& player = checked_player(engine, owner);
        player.taskforces.at(force_id).thread = 0;
        engine.emit_thread_event(player.running.at(plan_id), player.taskforces.at(force_id), event, {});
    }
    void EngineContractAccess::move(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, const ViewUnit& unit) {
        engine.issue_move(checked_player(engine, owner).running.at(plan_id), unit, math::Vec3{});
    }
    void EngineContractAccess::note(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, std::string text) {
        engine.record(&checked_player(engine, owner).running.at(plan_id), owner, "effects", std::move(text));
    }
    void EngineContractAccess::wallet(Engine& engine, tactical::PlayerId player, Real amount) { engine.change_wallet(player, amount); }
    std::int64_t EngineContractAccess::stacked(const Engine& engine, tactical::PlayerId player) { return engine.stacked_credits(player).raw(); }
    std::int64_t EngineContractAccess::host_stack(const Engine& engine, tactical::PlayerId player) { return engine.host_->uncommitted_credits(player); }
    void EngineContractAccess::claim_pad(Engine& engine, sim::EntityId pad, tactical::PlayerId player, std::uint64_t goal) {
        engine.set_pad_claim(pad, PadReservation{player, goal, 30});
    }
    std::optional<PadReservation> EngineContractAccess::claim(const Engine& engine, sim::EntityId pad) {
        const auto* held = engine.pad_claim(pad);
        return held ? std::optional<PadReservation>(*held) : std::nullopt;
    }
    void EngineContractAccess::free_pads(Engine& engine, tactical::PlayerId player, std::uint64_t goal) { engine.release_pad_claims(player, goal); }
    void EngineContractAccess::free_pad(Engine& engine, sim::EntityId pad) { engine.release_pad_claim(pad); }
    void EngineContractAccess::wait_for_attach(Engine& engine, PlayerAi& player, std::uint64_t goal_id) {
        Goal goal; goal.id = goal_id; engine.queue_attach(player, goal);
    }
    void EngineContractAccess::reindex(Engine& engine) { engine.rebuild_runtime_index(); }
    void EngineContractAccess::backlog(Engine& engine, tactical::PlayerId player, std::uint64_t goal) { engine.pending_attach_.push_back({player, goal}); }
    void EngineContractAccess::finish(Engine& engine, tactical::PlayerId owner, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(checked_player(engine, owner), plan_id, scripts, true);
    }
    void EngineContractAccess::execute(Engine& engine, tactical::PlayerId owner) { engine.service_execution(checked_player(engine, owner)); }
    void EngineContractAccess::pool_task(Engine& engine, tactical::PlayerId owner, std::uint64_t force, std::uint64_t token) {
        BuildTask task; task.taskforce = force; task.source = 1; task.type = 1; task.pool_token = token;
        checked_player(engine, owner).tasks.push_back(task);
    }
    // A goal the player's active list holds; an attachable one carries a potential plan of the first PlanDef.
    void EngineContractAccess::active_goal(Engine& engine, tactical::PlayerId id, std::uint64_t goal_id, bool attachable) {
        Goal goal; goal.id = goal_id;
        if (attachable) goal.potential = PotentialPlan{};
        checked_player(engine, id).active.push_back(std::move(goal));
    }
    // Inside a turn: a scheduler event (submitted at the commit) of the given tick, and an engine order.
    void EngineContractAccess::stage_event(Engine& engine, std::uint64_t tick) {
        authoritative::ScriptEvent event;
        event.key = {tick, producer_foc_engine, 1, 0};
        event.target = 1;
        event.kind = authoritative::ScriptEvent::Kind::pump;
        engine.staged()->scheduler.push_back(PlayerEffects::SchedulerEvent{std::move(event), true});
    }
    void EngineContractAccess::stage_order(Engine& engine) { engine.queue_order(authoritative::ScriptCommand{}); }
    void EngineContractAccess::bare_players(Engine& engine, authoritative::ScriptScheduler& scripts) {
        for (const tactical::PlayerId id : {7U, 9U}) {
            add_player(engine, id).functions.push_back(GoalFunctionEntry{nullptr, "ATTACH_GOAL", nullptr, "Attach"});
        }
        engine.scripts_ = &scripts; engine.event_tick_ = 1;
    }
    core::Result<void> EngineContractAccess::drain(Engine& engine, authoritative::ScriptScheduler& scripts) {
        std::uint64_t sequence = 0;
        return engine.drain_attaches(scripts, sequence);
    }
    std::string EngineContractAccess::goal_plans(const Engine& engine) {
        std::ostringstream out;
        for (const auto& player : engine.players_) for (const auto& goal : player.active)
            out << 'G' << player.player << ',' << goal.id << ':' << goal.plan << ';';
        return out.str();
    }
    bool EngineContractAccess::attach_throws(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        Goal goal; std::uint64_t sequence = 0;
        try { static_cast<void>(engine.attach_plan(player, goal, scripts, sequence)); } catch (const std::logic_error&) { return true; }
        return false;
    }
    std::size_t EngineContractAccess::waiting_events(const Engine& engine) { return engine.events_.size(); }
    std::size_t EngineContractAccess::waiting_orders(const Engine& engine) { return engine.orders_.size(); }
    std::size_t EngineContractAccess::waiting_attaches(const Engine& engine) { return engine.pending_attach_.size(); }
    std::size_t EngineContractAccess::forced_units(const Engine& engine, std::uint64_t force) {
        const auto* row = engine.taskforce(force); return row ? row->pooled.size() : 0;
    }
    std::vector<std::uint64_t> EngineContractAccess::pooled_tokens(const Engine& engine, std::uint64_t force) {
        std::vector<std::uint64_t> tokens;
        if (const auto* row = engine.taskforce(force)) for (const auto& purchase : row->pooled) tokens.push_back(purchase.token);
        return tokens;
    }
    void EngineContractAccess::arm_native_records(Engine& engine, bool on) { engine.native_records_enabled_ = on; }
    void EngineContractAccess::hold_native_scope(std::optional<Engine::NativeRecordWrite>& slot, Engine& engine) { slot.emplace(engine); }
    std::size_t EngineContractAccess::committed_claims(const Engine& engine) { return engine.pad_reservations_.size(); }
    std::size_t EngineContractAccess::committed_orders(const Engine& engine) { return engine.orders_.size(); }
    std::size_t EngineContractAccess::staged_effects(const Engine& engine) {
        std::size_t staged = 0;
        for (const auto& player : engine.players_) staged += player.effects.empty() ? 0 : 1;
        return staged;
    }
    std::size_t EngineContractAccess::committed_wallets(const Engine& engine) { return engine.host_->credit_changes.size(); }
    void EngineContractAccess::native_changes(Engine& engine, std::uint32_t owner) {
        const auto write = engine.native_record_scope();
        engine.native_records_changed(owner, {1, 13});
        engine.native_record_changed(owner, 2, 5 + owner);
        engine.mark_native_state_changed();
        engine.mark_native_state_changed();
    }
    void EngineContractAccess::native_shared_change(Engine& engine) {
        engine.native_records_changed(Engine::native_all_players, {32});
        engine.native_record_changed(0, 20, 9);
        engine.mark_native_state_changed();
    }
    std::size_t EngineContractAccess::native_depth(const Engine& engine) { return engine.native_record_depth_; }
    std::string EngineContractAccess::native_snapshot(const Engine& engine) {
        std::ostringstream out;
        for (const auto& [owner, bits] : engine.native_record_dirty_groups_) out << 'G' << owner << ':' << bits << ';';
        for (const auto& key : engine.native_record_dirty_keys_) out << 'K' << std::get<0>(key) << ',' << std::get<1>(key) << ',' << std::get<2>(key).size() << ';';
        out << "R" << engine.native_revision_ << ',' << engine.native_record_header_dirty_ << ',' << engine.native_record_depth_;
        return out.str();
    }
    std::string EngineContractAccess::snapshot(Engine& engine, authoritative::ScriptScheduler& scripts) {
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
    void EngineContractAccess::orphan_blocks_fixture(Engine& engine) {
        auto& owner = fixture_owner(engine);
        for (const auto kind : {Block::Kind::produce, Block::Kind::reinforce, Block::Kind::ambush, Block::Kind::move}) {
            Block row; row.id = static_cast<std::uint64_t>(kind) + 1; row.taskforce = 999; row.kind = kind;
            row.ready = true; owner.blocks.emplace(row.id, row);
        }
        engine.rebuild_runtime_index();
    }
    void EngineContractAccess::interleaved_native_fixture(Engine& engine) {
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
    void EngineContractAccess::stage_movement_queries(Engine& engine) {
        auto& first = checked_player(engine, 9).blocks.at(1100);
        auto& second = checked_player(engine, 9).blocks.at(1101);
        first.galactic_request = 200; second.galactic_request = 100;
        Block row; row.id = 1099; row.galactic_request = 300;
        checked_player(engine, 7).blocks.emplace(row.id, row);
        engine.rebuild_runtime_index();
    }
    void EngineContractAccess::clear_movement_queries(Engine& engine) {
        checked_player(engine, 9).blocks.at(1100).galactic_request = 0;
        checked_player(engine, 9).blocks.at(1101).galactic_request = 0;
        checked_player(engine, 7).blocks.erase(1099);
        engine.rebuild_runtime_index();
    }
    void EngineContractAccess::depart_positioning_plan(Engine& engine, tactical::PlayerId id) {
        auto& owner = checked_player(engine, id);
        owner.running.clear(); owner.plan_of_instance.clear(); owner.active.clear();

        engine.rebuild_runtime_index();
    }
    core::Result<void> EngineContractAccess::attach(Engine& engine, PlayerAi& player, Goal& goal,
        authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.event_tick_ = 1;
        engine.sequence_ = sequence;
        engine.scripts_ = &scripts;
        return engine.attach_plan(player, goal, scripts, sequence);
    }
    const std::vector<authoritative::ScriptEvent>& EngineContractAccess::attachment_events(const Engine& engine) {
        return engine.events_;
    }
    std::vector<authoritative::ScriptEvent> EngineContractAccess::take_attachment_events(Engine& engine) {
        auto events = std::move(engine.events_); engine.events_.clear(); return events;
    }
    void EngineContractAccess::defer_event(Engine& engine, authoritative::ScriptEvent event) {
        engine.events_.push_back(std::move(event));
    }
    std::vector<sim::EntityId> EngineContractAccess::eligible(Engine& engine, const PlayerAi& player,
        const Goal& goal, const PlanDef& plan) {
        std::vector<sim::EntityId> units;
        for (const ViewUnit* unit : engine.freestore_list(player, goal, plan)) units.push_back(unit->id);
        return units;
    }
    std::vector<authoritative::ScriptEvent> EngineContractAccess::hazard_events(Engine& engine,
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
    void EngineContractAccess::event_force(Engine& engine, authoritative::ScriptScheduler& scripts, std::vector<sim::EntityId> members) {
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
    std::vector<authoritative::ScriptEvent> EngineContractAccess::run_events(Engine& engine) {
        engine.events_.clear();
        engine.service_taskforce_events();
        return engine.events_;
    }
    void EngineContractAccess::seed(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
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
    void EngineContractAccess::service(Engine& engine, PlayerAi& player, std::int64_t frame) {
        engine.frame_ = frame;
        if (engine.host_->ai_world) engine.observe_purchases(player, *engine.host_->ai_world);
        engine.service_execution(player);
    }
    PlayerAi& EngineContractAccess::rejected_station_fixture(Engine& engine) {
        auto& player = collect_fixture(engine);
        Block block; block.id = block.taskforce = 1; block.kind = Block::Kind::produce;
        fixture_owner(engine).blocks.emplace(1, block);
        BuildTask task; task.taskforce = task.block = 1; task.type = 50; task.source = 2;
        task.issued = 1; task.finished = task.failed = true;
        player.tasks.push_back(task);

        engine.rebuild_runtime_index();
        return player;
    }
    void EngineContractAccess::direct_pad_service(Engine& engine, PlayerAi& player) { engine.service_execution(player, true); }
    void EngineContractAccess::reserve(Engine& engine, PlayerAi& player, Goal& goal) { engine.reserve(player, goal); }
    Goal& EngineContractAccess::pool_goal(Engine& engine, PlayerAi& player, std::vector<std::uint64_t> tokens, bool active, bool non_pool) {
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
    void EngineContractAccess::retire_pool_goal(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts, bool abandoned) {
        engine.finish_plan(player, player.active.front().plan, scripts, abandoned);
        player.active.clear();
    }
    void EngineContractAccess::await_pool_arrival(Engine& engine, PlayerAi& player, std::uint64_t plan_id) {
        const auto& plan = player.running.at(plan_id);
        Block block; block.kind = Block::Kind::reinforce; block.instance = plan.instance;
        block.id = block_id(plan.instance, 19); block.taskforce = plan.taskforces.front();
        block.reinforcing = 1; block.pool_token = 77;
        player.blocks.emplace(block.id, block);
        engine.rebuild_runtime_index();
    }
    void EngineContractAccess::bind_scripts(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        engine.scripts_ = &scripts; engine.sequence_ = sequence; engine.event_tick_ = scripts.completed_tick() + 1;
    }
    void EngineContractAccess::pause_planning(Engine& engine) {
        engine.initialized_ = true;
        // This fixture installs its own already-seeded deadlines. Production
        // startup intentionally leaves them unseeded until its first enabled turn.
        engine.staggered_seeded_ = true;
        for (auto& player : engine.players_) {
            player.next_goal = player.next_planning = player.next_execution = player.next_learning = 1000000;
        }
    }
    void EngineContractAccess::release(Engine& engine, PlayerAi& player, Goal& goal, bool refund) {
        engine.release(player, goal, refund);
    }
    void EngineContractAccess::perception_executor(Engine& engine, const sim::PartitionExecutor* executor) {
        engine.perception_executor_ = executor;
    }
    std::vector<authoritative::ScriptCommand> EngineContractAccess::take_orders(Engine& engine) {
        auto orders = std::move(engine.orders_); engine.orders_.clear(); return orders;
    }
    void EngineContractAccess::return_orders(Engine& engine, authoritative::ServiceReport& report) {
        engine.orders_ = std::move(report.commands);
    }
    void EngineContractAccess::purchase(Engine& engine, PlayerAi& player, const Goal& goal,
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
    void EngineContractAccess::abandon_goal(Engine& engine, PlayerAi& player, std::uint64_t id, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(player, id, scripts, true);
    }
    PlayerAi& EngineContractAccess::direct_pad_fixture(Engine& engine, tactical::TypeId type, sim::EntityId pad) {
        auto& owner = fixture_owner(engine);
        Goal goal; goal.id = 1; purchase(engine, owner, goal, type, pad);
        owner.tasks.clear();
        owner.plan_of_instance[1] = 1; owner.running.at(1).reserved_pads = {pad};
        auto& force = owner.taskforces.at(1); force.types = {type}; force.sources = {2}; force.producers = {pad};
        engine.frame_ = static_cast<std::int64_t>(engine.host_->world->completed_tick());

        engine.rebuild_runtime_index();
        return owner;
    }
    void EngineContractAccess::seed_purchases(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        seed(engine, player, scripts);
        player.running.at(1).reserved_pads.clear();
        player.tasks.front().type = 50;
        auto later = player.tasks.front();
        later.type = 51;
        player.tasks.push_back(later);
    }
    std::size_t EngineContractAccess::orders(const Engine& engine) { return engine.orders_.size(); }
    void EngineContractAccess::seed_reinforcements(Engine& engine, PlayerAi& player, const std::uint64_t count, const bool large_block) {
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
    const std::vector<authoritative::ScriptCommand>& EngineContractAccess::reinforce_service(
        Engine& engine, PlayerAi& player, const std::int64_t frame) {
        engine.frame_ = frame;
        engine.work_ = {};
        engine.orders_.clear();
        engine.service_reinforcements(player);
        return engine.orders_;
    }
    std::uint32_t EngineContractAccess::reinforcement_candidates(const Engine& engine) {
        return engine.work_.reinforcement_candidates;
    }
    PlayerAi& EngineContractAccess::duplicate_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 1;
        seed_reinforcements(engine, player, 1);
        player.blocks.clear();
        player.plan_of_instance[1] = 1;
        engine.players_.push_back(std::move(player));

        engine.rebuild_runtime_index();
        return engine.players_.back();
    }
    PlayerAi& EngineContractAccess::concurrent_fixture(Engine& engine) {
        PlayerAi player;
        player.player = 1;
        seed_reinforcements(engine, player, 2);
        engine.players_.push_back(std::move(player));

        engine.rebuild_runtime_index();
        return engine.players_.back();
    }
    bool EngineContractAccess::in_freestore(const Engine& engine, const ViewUnit& unit) {
        return engine.in_freestore(unit, 1);
    }
    void EngineContractAccess::abandon(Engine& engine, PlayerAi& player, authoritative::ScriptScheduler& scripts) {
        engine.finish_plan(player, 1, scripts, true);
    }
    bool EngineContractAccess::test_valid(Engine& engine, PlayerAi& player, Goal& goal) {
        engine.frame_ = 1;
        return engine.test_valid(player, goal);
    }
    void EngineContractAccess::reinforce_at(Engine& engine, const math::Vec3& position) {
        fixture_owner(engine, 1).blocks.at(block_id(first_plan_instance + 1, 17)).destination = position;
    }
    void EngineContractAccess::movement_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
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
    void EngineContractAccess::movement_service(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_blocks();
    }
    std::uint64_t EngineContractAccess::event_sequence(const Engine& engine) { return engine.sequence_; }
    bool EngineContractAccess::movement_finished(const Engine& engine, std::uint64_t id) {
        return fixture_owner(engine, 1).blocks.at(id).finished;
    }
    void EngineContractAccess::guard_fixture(Engine& engine, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
        movement_fixture(engine, scripts, sequence);
        auto& guard = fixture_owner(engine, 1).blocks.at(1);
        guard.order = Block::Order::guard;
        guard.destination_object = 20;
        auto& move = fixture_owner(engine, 1).blocks.at(2);
        move.kind = Block::Kind::move;
        move.order = Block::Order::move;
    }
    void EngineContractAccess::empty_guard_force(Engine& engine) { fixture_owner(engine, 1).taskforces.at(1).units.clear(); }
    // EX-31 (#3087): one attack block of player 1's force {10} on `target`, ordered at frame 0.
    const Block& EngineContractAccess::attack_fixture(Engine& engine, authoritative::ScriptScheduler& scripts,
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
    std::size_t EngineContractAccess::order_count(const Engine& engine) { return engine.orders_.size(); }
    void EngineContractAccess::point_defence(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_point_defence();
    }
    void EngineContractAccess::corrupt_systems(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_corrupt_systems();
    }
    std::size_t EngineContractAccess::point_defence_orders(const Engine& engine) { return engine.orders_.size(); }
    const authoritative::ScriptCommand* EngineContractAccess::last_order(const Engine& engine) {
        return engine.orders_.empty() ? nullptr : &engine.orders_.back();
    }
    // WTA-23 to WTA-28: plan instance first_plan_instance of player 2 runs TaskForce 1 with unit 30;
    // a perception grid of 20 x 20 cells of 400 over -4000..4000.
    void EngineContractAccess::positioning_fixture(Engine& engine) {
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
    void EngineContractAccess::enable_positioning(Engine& engine, std::int64_t frame, bool on) {
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
    void EngineContractAccess::positioning_service(Engine& engine, std::int64_t frame) {
        engine.frame_ = frame;
        engine.service_attack_positioning();
    }
    void EngineContractAccess::positioning_point_order(Engine& engine, const math::Vec3& point) {
        Block block;
        block.order = Block::Order::attack_move;
        block.destination = point;
        engine.order_block(block, fixture_owner(engine).taskforces.at(1), fixture_owner(engine).running.at(1));
    }
    void EngineContractAccess::positioning_script_point_order(Engine& engine, const math::Vec3& point) {
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
    bool EngineContractAccess::positioning_on(const Engine& engine, sim::EntityId unit) { return fixture_owner(engine).positioning.contains(unit); }
    void EngineContractAccess::threat(Engine& engine, sim::EntityId victim, sim::EntityId attacker, std::int64_t amount) {
        engine.threats_[victim][attacker] = Real(static_cast<int>(amount));
    }
}

namespace foc_contract {

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void stock_object_scripts(const std::filesystem::path& root);
void stock_slave_i(const std::filesystem::path& root);
void stock_sundered_heart(const std::filesystem::path& root);
void stock_missile_shield(const std::filesystem::path& root);
void stock_script_admission(const std::filesystem::path& root);
void script_evaluator_contracts();
void object_build_contracts();


void expect(bool condition, const std::string& message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
}


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
    {
        // PR-C1/C2 (#4992): AaW authors <Build_Cost_Credits />; an empty or TBD scoring value skips
        // the key (the caller keeps Max_Squad_Size 10 and build cost 0), other text must parse.
        const auto empty = eawr::platform::scoring_decimal("");
        const auto spaces = eawr::platform::scoring_decimal("  ");
        const auto tbd = eawr::platform::scoring_decimal("TBD");
        const auto value = eawr::platform::scoring_decimal(" 425 ");
        const auto garbage = eawr::platform::scoring_decimal("ten");
        expect(empty && !empty.value() && spaces && !spaces.value() && tbd && !tbd.value(), "PR-C1/C2: empty or TBD scoring values are skipped");
        expect(value && value.value() && value.value()->raw() == 425 * eawr::sim::math::Fixed::scale, "an authored scoring value parses");
        expect(!garbage, "a non-numeric scoring value still fails");
    }
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

} // namespace foc_contract

using namespace foc_contract;

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--stock-object-scripts") {
        std::string root;
        if (argc > 2) root = argv[2];
        else {
            root = eawr::testing::game_root().value_or("");
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
        if (mode == "--producer-census") production_lifecycle();
        else if (mode == "--spending-pad-states") spending_pad_states();
        else if (mode == "--spending-wallet") spending_wallet_reservations();
        else if (mode == "--spending-pad-refunds") spending_pad_refunds();
        else if (mode == "--reachable-target") { reachable_target_contract(); find_target_reach_contract(); }
        else if (mode == "--installed-galactic-freestore") installed_galactic_freestore();
        else if (mode == "--script-evaluators") script_evaluator_contracts();
        else if (mode == "--object-build") object_build_contracts();
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
    threat_flat_storage();
    plan_definition_paths();
    mounted_ai_discovery();
    mode_selected_closure();
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
    ai_difficulty_levels();
    if (failures != 0) {
        std::cerr << failures << " FoC AI contract(s) failed\n";
        return 1;
    }
    std::cout << "FoC AI contracts passed\n";
    return 0;
}
