#include "eawr/core/tick_profile.hpp"
#include "ai_engine_internal.hpp"
#include "galactic_ai_actions.hpp"
#include "ai_learning.hpp"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>

namespace eawr::script::foc::ai {

using namespace engine_internal;

// ---- The barrier steps ---------------------------------------------------------------------

core::Result<void> Engine::begin_galactic_barrier(const World& world, galactic_ai::Clocks clocks,
    authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
    std::span<const galactic_ai::PlayerSelection> selected, const sim::PartitionExecutor* executor) {
    mark_native_state_changed();
    if (host_->ai_world.get() != &world || world.goal_mode() != "GALACTIC" || !world.available() ||
        clocks.synchronization > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return core::Result<void>::failure(
            detail::make_error(authoritative::codes::session_abort, "galactic AI barrier requires its available mode world and valid synchronization frame"));
    host_->view = world.view();
    if (!host_->view) return core::Result<void>::failure(detail::make_error(authoritative::codes::session_abort, "galactic AI has no completed view"));
    perception_executor_ = executor;
    frame_ = static_cast<std::int64_t>(clocks.synchronization);
    event_tick_ = scripts.completed_tick() + 1;
    sequence_ = sequence;
    scripts_ = &scripts;
    work_ = {};
    producers_prepared_ = false;
    producer_work_ = {};
    host_->credit_changes.clear();
    for (auto& event : events_) { event.key.tick = event_tick_; event.key.sequence = sequence_++; }
    if (!initialized_) {
        game_seed_ = static_cast<std::uint32_t>(host_->setup.seed);
        sync_.set_seed(game_seed_);
        for (const auto& selection : selected) {
            const auto type = data_.players.find(upper_case(selection.player_type));
            if (type == data_.players.end()) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic AI selected player type is missing"));
            const auto adjustment = type->second.difficulty.find(upper_case(selection.difficulty));
            if (adjustment == type->second.difficulty.end() || !data_.difficulties.contains(adjustment->second)) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic AI selected difficulty is missing"));
            PlayerAi player;
            player.player = selection.player;
            player.type = &type->second;
            player.galactic = true;
            player.difficulty = data_.difficulties.at(adjustment->second);
            for (const auto& set : type->second.function_sets) {
                const auto functions = data_.function_sets.find(set);
                if (functions == data_.function_sets.end()) continue; // other-mode closure
                for (const auto& function : functions->second) {
                    const auto goal = data_.goals.find(function.goal);
                    if (goal == data_.goals.end() || goal->second.mode != "GALACTIC") continue;
                    const auto* equation = data_.equations.find(function.function);
                    if (!equation) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic AI selected goal equation is missing"));
                    GoalFunctionEntry entry;
                    entry.goal = &goal->second; entry.goal_name = function.goal;
                    entry.equation = equation; entry.equation_name = function.function;
                    player.functions.push_back(std::move(entry));
                }
            }
            players_.push_back(std::move(player));
        }
        std::sort(players_.begin(), players_.end(), [](const auto& a, const auto& b) { return a.player < b.player; });
        for (auto& player : players_) { build_targets(player); initialize_goals(player); }
        initialized_ = true;
    }
    update_targets(); // identity/death reconciliation; WGA-79 does not sweep tokens.
    try { service_blocks(); service_taskforce_events(); }
    catch (const std::exception& error) { return core::Result<void>::failure(detail::make_error(authoritative::codes::session_abort, error.what())); }
    sequence = sequence_;
    return core::Result<void>::success();
}

core::Result<void> Engine::apply_galactic_policy(tactical::PlayerId id, const galactic_ai::TemplatePolicy& policy) {
    mark_native_state_changed();
    auto* player = player_ai(id);
    if (!player || !player->galactic) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic template has no initialized native player"));
    player->galactic_policy = policy;
    player->proposal_counts.clear();
    for (const auto& function : player->functions) {
        const auto found = policy.goal_types.find(function.goal_name);
        if (found != policy.goal_types.end() && found->second.value) ++player->proposal_counts[function.goal_name];
    }
    return core::Result<void>::success();
}

void Engine::set_galactic_funding(std::shared_ptr<galactic_ai::Funding> funding) {
    mark_native_state_changed();
    for (auto& player : players_) if (player.galactic) player.galactic_funding = funding;
}

core::Result<void> Engine::service_galactic_system(tactical::PlayerId id, galactic_ai::Service service,
    authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    mark_native_state_changed();
    auto* player = player_ai(id);
    if (!player || !player->galactic) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic service has no native player"));
    switch (service) {
    case galactic_ai::Service::goals:
        if (!galactic_selection_ || !player->galactic_funding || !player->galactic_funding->ledger(id))
            return core::Result<void>::failure(detail::make_error(authoritative::codes::missing_api,
                "galactic goal service requires selector facts and initialized funding"));
        try { service_goals(*player); }
        catch (const std::exception& error) { return core::Result<void>::failure(detail::make_error(
            authoritative::codes::session_abort, error.what())); }
        break;
    case galactic_ai::Service::planning:
        if (auto serviced = service_plans(*player, scripts, sequence_); !serviced) return serviced;
        break;
    case galactic_ai::Service::learning:
        learning::sweep(*player, frame_);
        break;
    case galactic_ai::Service::execution:
        if (!host_->galactic_execution || !galactic_selection_ || !galactic_build_observer_)
            return core::Result<void>::failure(detail::make_error(authoritative::codes::missing_api,
                "galactic execution requires native actions, selector facts and the producer completion owner"));
        try { service_execution(*player); }
        catch (const std::exception& error) { return core::Result<void>::failure(detail::make_error(
            authoritative::codes::session_abort, error.what())); }
        break;
    default: break; // perception only rearms; templates are applied by Manager.
    }
    sequence = sequence_;
    return core::Result<void>::success();
}

core::Result<void> Engine::commit_galactic_events(authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    mark_native_state_changed();
    if (auto attached = drain_attaches(scripts, sequence_); !attached) return attached;
    for (auto& event : events_) if (auto posted = scripts.submit_event(std::move(event)); !posted) return posted;
    events_.clear();
    sequence = sequence_;
    return core::Result<void>::success();
}

core::Result<void> Engine::before_service(const World& world,
    authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
    const sim::PartitionExecutor* executor, const ExecutionHook& execution_hook) {
    core::tick_profile::Scope tail("serial.ai.tail");
    struct StationReset final {
        decltype(station_levels_)& levels;
        decltype(pad_inputs_)& pads;
        ~StationReset() { levels.reset(); pads.reset(); }
    } station_reset{station_levels_, pad_inputs_};
    struct InputReset final {
        Engine& engine;
        ~InputReset() { engine.player_inputs_active_ = false; engine.report_inputs(); }
    } input_reset{*this};
    input_counts_ = {};
    player_inputs_active_ = false;
    const auto timed = [](const std::string_view name, auto&& run) {
        core::tick_profile::Scope scope(name);
        return run();
    };
    mark_native_state_changed();
    if (host_->ai_world.get() != &world) return core::Result<void>::failure(detail::make_error(
        authoritative::codes::session_abort, "AI barrier world does not match its host"));
    host_->view = world.view();
    if (!host_->view) return core::Result<void>::failure(detail::make_error(
        authoritative::codes::session_abort, "AI barrier world has no completed view"));
    input_counts_.view_rows = host_->view->units.size();
    const auto preparation = grid_.prepare(executor, *host_, *host_->view);
    perception_executor_ = executor;
    host_->credit_changes.clear(); // preceding commands are now in the authoritative snapshot
    frame_ = static_cast<std::int64_t>(world.completed_tick());
    event_tick_ = scripts.completed_tick() + 1;
    sequence_ = sequence;
    scripts_ = &scripts;
    // Direct pad builds can emit callbacks after Lua has serviced this tick.
    // Retain them and assign fresh keys alongside this barrier's engine events.
    for (auto& event : events_) {
        event.key.tick = event_tick_;
        event.key.sequence = sequence_++;
    }
    work_ = TickCost{};
    producers_prepared_ = false;
    for (auto& [key, candidates] : producers_) {
        static_cast<void>(key);
        candidates.clear();
    }
    producer_work_ = {};
    if (!initialized_) {
        initialized_ = true;
        game_seed_ = static_cast<std::uint32_t>(host_->setup.seed);
        sync_.set_seed(game_seed_);
        if (host_->setup.bounds) {
            // PG-01: fog cells of DesiredSpaceFOWCellSize, threat cells of
            // AI_FogCellsPerThreatCell fog cells, both rounded up.
            const AiBounds& bounds = *host_->setup.bounds;
            const auto cells = [&](Real extent) {
                const Real fog = to_single(extent / data_.constants.fog_cell_size);
                std::int64_t count = truncate(fog);
                if (real(count) < fog) ++count;
                const std::int64_t per = data_.constants.fog_cells_per_threat_cell;
                return static_cast<std::int32_t>((count + per - 1) / per);
            };
            grid_.partition(bounds, cells(bounds.right - bounds.left), cells(bounds.top - bounds.bottom), data_.constants);
        }
        for (const AiPlayer& setup : host_->setup.players) {
            if (!setup.ai || setup.player_type.empty()) continue;
            const auto type = data_.players.find(upper_case(setup.player_type));
            if (type == data_.players.end()) continue;
            PlayerAi player;
            player.player = setup.player;
            player.type = &type->second;
            if (!type->second.space_templates.empty()) {
                const auto found = data_.templates.find(type->second.space_templates.front());
                if (found != data_.templates.end()) player.space_template = &found->second;
            }
            // AI-G03: Normal difficulty.
            if (const auto name = type->second.difficulty.find("NORMAL"); name != type->second.difficulty.end()) {
                if (const auto found = data_.difficulties.find(name->second); found != data_.difficulties.end()) {
                    player.difficulty = found->second;
                }
            }
            // GS-02: the space goal functions of the player type's function sets, in order.
            for (const std::string& set : type->second.function_sets) {
                const auto functions = data_.function_sets.find(set);
                if (functions == data_.function_sets.end()) continue;
                for (const GoalFunction& function : functions->second) {
                    const auto goal = data_.goals.find(function.goal);
                    if (goal == data_.goals.end() || goal->second.mode != world.goal_mode()) continue;
                    GoalFunctionEntry entry;
                    entry.goal = &goal->second;
                    entry.goal_name = function.goal;
                    entry.equation = data_.equations.find(function.function);
                    entry.equation_name = function.function;
                    player.functions.push_back(std::move(entry));
                }
            }
            players_.push_back(std::move(player));
        }
        std::sort(players_.begin(), players_.end(), [](const PlayerAi& a, const PlayerAi& b) { return a.player < b.player; });
        for (PlayerAi& player : players_) {
            build_targets(player);
            initialize_goals(player);
        }
    }
    // FD-72 / FC-76: initialize under suspension without servicing deadlines.
    // SCH-02 / FD-73: anchor phases at the first enabled turn, and pause their
    // remaining offsets during later holds. Faithful deadlines stay unphased.
    if (staggered()) staggered_clock_.visit(static_cast<std::uint64_t>(frame_), host_->ai_suspended);
    if (host_->ai_suspended) { sequence = sequence_; return core::Result<void>::success(); }
    const auto service_frame = staggered()
        ? static_cast<std::int64_t>(staggered_clock_.enabled_frame(static_cast<std::uint64_t>(frame_))) : frame_;
    if (staggered() && !staggered_seeded_) {
        const std::int64_t period = next_frame(0, to_single(real(1) / real(10)));
        for (std::size_t index = 0; index < players_.size(); ++index) {
            PlayerAi& player = players_[index];
            player.phase = static_cast<std::int64_t>(index);
            player.next_goal = service_frame + player.phase;
            player.next_planning = player.next_execution = player.next_learning = service_frame + player.phase % period;
        }
        staggered_seeded_ = true;
    }
    for (auto& player : players_) timed("serial.ai.purchases", [&] { observe_purchases(player, world); }); // before plan abandonment
    timed("serial.ai.damage", [&] { track_damage(world); });
    timed("serial.ai.point-defence", [&] { service_point_defence(); });
    timed("serial.ai.corrupt-systems", [&] { service_corrupt_systems(); });
    timed("serial.ai.threat-grid", [&] { grid_.service(*host_->view, *host_, frame_); });
    timed("serial.ai.targets", [&] { update_targets(); });
    timed("serial.ai.blocks", [&] { service_blocks(); });
    timed("serial.ai.attack-positioning", [&] { service_attack_positioning(); });
    timed("serial.ai.taskforce-events", [&] { service_taskforce_events(); });
    if (!players_.empty()) timed("serial.ai.station-inputs", [&] { prepare_station_levels(executor); });
    if (!players_.empty()) timed("serial.ai.threat-inputs", [&] { grid_.prepare_contributions(); });
    if (!players_.empty()) {
        timed("serial.ai.producer-inputs", [&] { prepare_producers(executor); });
        timed("serial.ai.pad-inputs", [&] { prepare_pad_inputs(executor); });
    }
    player_inputs_active_ = true;
    for (PlayerAi& player : players_) {
        AiServiceEvent served{static_cast<std::uint64_t>(frame_), player.player, false, false, false};
        if (player.next_goal <= service_frame) {
            // GS-01: the goal system's delay is 0: every frame.
            player.next_goal = next_frame(service_frame, Real{});
            served.goals = true;
            timed("serial.ai.goals", [&] { service_goals(player); });
        }
        if (player.next_planning <= service_frame) {
            player.next_planning = next_frame(service_frame, to_single(real(1) / real(10)));
            served.planning = true;
            if (auto serviced = timed("serial.ai.plans", [&] { return service_plans(player, scripts, sequence_); }); !serviced) return serviced;
        }
        if (player.next_execution <= service_frame) {
            player.next_execution = next_frame(service_frame, to_single(real(1) / real(10)));
            served.execution = true;
            // WTA-01/02: the execution service runs the freestore's service tests, then the build tasks.
            if (execution_hook && !player.galactic) {
                if (auto tested = timed("serial.ai.freestore", [&] { return execution_hook(player.player, sequence_); }); !tested) return tested;
            }
            timed("serial.ai.execution", [&] { service_execution(player); });
        }
        if (host_->setup.journal && (served.goals || served.planning || served.execution)) {
            host_->setup.journal->services.push_back(served);
        }
        if (player.next_learning <= service_frame) {
            // WGA-211/216: one rearm, then the sourced prefix sweep.
            player.next_learning = next_frame(service_frame, real(10));
            timed("serial.ai.learning", [&] { learning::sweep(player, frame_); });
        }
    }
    player_inputs_active_ = false;
    if (auto attached = timed("serial.ai.attaches", [&] { return drain_attaches(scripts, sequence_); }); !attached) return attached;
    work_.plans_deferred = static_cast<std::uint32_t>(pending_attach_.size());
    for (ScriptEvent& event : events_) {
        if (auto submitted = scripts.submit_event(std::move(event)); !submitted) return submitted;
    }
    events_.clear();
    sequence = sequence_;
    return core::Result<void>::success();
}

core::Result<void> Engine::after_service(authoritative::ServiceReport& report) {
    mark_native_state_changed();
    // The Lua cost of the tick (#449 budget).
    TickCost cost = work_;
    cost.tick = report.tick;
    for (const authoritative::InstanceLoad& load : report.loads) {
        if (load.instance >= first_object_instance) continue;
        if (load.instance >= first_plan_instance) {
            cost.plans += load.instructions;
            ++cost.plan_instances;
        } else {
            cost.freestore += load.instructions;
            ++cost.freestore_runs;
        }
    }
    costs_.push_back(cost);
    if (host_->setup.journal) {
        host_->setup.journal->costs.push_back(AiTickCost{cost.tick, cost.freestore, cost.plans, cost.plan_instances,
            cost.goals_evaluated, cost.maintenances, cost.plans_attached, cost.plans_deferred, cost.plans_pumped,
            cost.freestore_runs, cost.reinforcement_candidates, cost.reinforcement_search_ns});
    }
    for (const std::uint64_t removed : report.removed_instances) {
        if (const auto found = plan_of_instance_.find(removed); found != plan_of_instance_.end()) {
            if (auto plan = running_.find(found->second); plan != running_.end()) plan->second.exited = true;
        }
    }
    std::vector<authoritative::ScriptCommand> kept;
    kept.reserve(report.commands.size());
    for (authoritative::ScriptCommand& command : report.commands) {
        if (command.verb != verb_ai) {
            // U-05: direct object-script orders replace the retained positioning destination too.
            if ((command.verb == verb_move || command.verb == verb_attack_move || command.verb == verb_guard
                    || command.verb == verb_attack) && command.arguments.size() > 1) {
                if (const auto unit = handle_id(command.arguments[1], handle_game_object)) {
                    if (auto retained = positioning_.find(*unit); retained != positioning_.end())
                        retained->second.target = sim::invalid_entity_id;
                }
            }
            if (host_->setup.journal && command.verb == verb_special_fire && command.arguments.size() == 4) {
                const auto found = plan_of_instance_.find(command.issuer);
                const Plan* source = found != plan_of_instance_.end() ? this->plan(found->second) : nullptr;
                const auto owner = number_of(command.arguments[0]);
                const auto unit = handle_id(command.arguments[1], handle_game_object);
                const auto target = handle_id(command.arguments[2], handle_game_object);
                const auto admission = number_of(command.arguments[3]);
                if (owner && unit && target && admission) record(source, static_cast<tactical::PlayerId>(truncate(*owner)),
                    "special_fire", "unit " + std::to_string(*unit) + " target " + std::to_string(*target) +
                    " admission " + std::to_string(truncate(*admission)));
            }
            if (host_->setup.journal && command.verb == verb_ability) {
                const auto found = plan_of_instance_.find(command.issuer);
                const Plan* source = found != plan_of_instance_.end() ? this->plan(found->second) : nullptr;
                const auto owner = command.arguments.empty() ? std::nullopt : number_of(command.arguments[0]);
                std::string detail = "issuer " + std::to_string(command.issuer);
                if (command.arguments.size() == 4) {
                    const auto unit = handle_id(command.arguments[1], handle_game_object);
                    const auto kind = number_of(command.arguments[2]);
                    const auto action = number_of(command.arguments[3]);
                    if (unit && kind && action) detail += " unit " + std::to_string(*unit)
                        + " ability " + std::to_string(truncate(*kind)) + " action " + std::to_string(truncate(*action));
                }
                record(source, source != nullptr ? source->player :
                    (owner ? static_cast<tactical::PlayerId>(truncate(*owner)) : 0U), "ability", std::move(detail));
            }
            kept.push_back(std::move(command));
            continue;
        }
        const ValueList& arguments = command.arguments;
        const std::string operation = arguments.empty() ? std::string() : text_of(arguments[0]);
        const auto tf_id = arguments.size() > 1 ? handle_id(arguments[1], handle_taskforce) : std::nullopt;
        auto tf = tf_id ? taskforces_.find(*tf_id) : taskforces_.end();
        Plan* plan = nullptr;
        if (const auto found = plan_of_instance_.find(command.issuer); found != plan_of_instance_.end()) {
            if (auto entry = running_.find(found->second); entry != running_.end()) plan = &entry->second;
        }
        if (plan == nullptr) continue;
        PlayerAi* player = player_ai(plan->player);
        if (player == nullptr) continue;
        const std::uint64_t id = block_id(command.issuer, command.sequence);
        if (operation == "galactic_retire_invasion" && arguments.size() == 2 && player->galactic) {
            const auto requested = handle_id(arguments[1], handle_block);
            auto block = requested ? blocks_.find(*requested) : blocks_.end();
            if (block == blocks_.end() || block->second.instance != plan->instance
                || block->second.kind != Block::Kind::galactic_invade || !block->second.galactic_request) continue;
            auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
            const auto* status = actions ? actions->invasion_status(block->second.galactic_request) : nullptr;
            if (!status || status->state != sim::galactic::GalacticSession::NativeInvasion::State::completed)
                return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "terminal invasion release requires its real completed result"));
            block->second.finished = true; block->second.result = status->success;
            if (auto retired = actions->retire_invasion(block->second.galactic_request); !retired) return retired;
            block->second.galactic_request = 0;
            continue;
        }
        if (operation == "galactic_poll" && arguments.size() == 2 && player->galactic) {
            const auto requested = handle_id(arguments[1], handle_block);
            auto block = requested ? blocks_.find(*requested) : blocks_.end();
            if (block == blocks_.end() || block->second.instance != plan->instance || !block->second.galactic_request) continue;
            auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
            if (!actions) return core::Result<void>::failure(detail::make_error(authoritative::codes::missing_api, "galactic poll requires native movement owner"));
            const auto force = taskforces_.find(block->second.taskforce);
            auto state = actions->poll_movement(block->second.galactic_request, force != taskforces_.end() && !force->second.units.empty());
            if (!state) return core::Result<void>::failure(state.error());
            block->second.finished = state.value().finished; block->second.galactic_actor = state.value().actor;
            continue;
        }
        if ((operation == "galactic_move" || operation == "galactic_land" || operation == "galactic_launch" || operation == "galactic_form" || operation == "galactic_invade")
            && tf != taskforces_.end() && player->galactic) {
            auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
            if (!actions) return core::Result<void>::failure(detail::make_error(authoritative::codes::missing_api, "galactic block requires native world owner"));
            galactic_ai::ActionRequest request;
            request.player = plan->player; request.plan = plan->id; request.taskforce = tf->second.id; request.block = id;
            request.object = tf->second.galactic_fleet; request.members = tf->second.units;
            request.kind = operation == "galactic_move" ? galactic_ai::ActionKind::move
                : operation == "galactic_invade" ? galactic_ai::ActionKind::invade
                : operation == "galactic_land" ? galactic_ai::ActionKind::land
                : operation == "galactic_launch" ? galactic_ai::ActionKind::launch : galactic_ai::ActionKind::regroup;
            if (request.kind == galactic_ai::ActionKind::move) {
                const auto destination = arguments.size() == 3 ? handle_id(arguments[2], handle_game_object) : std::nullopt;
                const auto reach = galactic_movement_reach(plan->id);
                if (!destination || !reach) return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request, "galactic move requires an object destination and retained goal reachability"));
                request.target = *destination; request.reach = *reach;
            } else if (!request.object || !actions->containing_fleet(plan->player, request.object)) {
                request.object = 0;
                for (const auto member : request.members) if (const auto fleet = actions->containing_fleet(plan->player, member)) { request.object = *fleet; break; }
            }
            auto admitted = actions->admit(request);
            if (!admitted) return core::Result<void>::failure(admitted.error());
            Block block; block.id = id; block.instance = command.issuer; block.taskforce = tf->second.id;
            block.kind = request.kind == galactic_ai::ActionKind::move ? Block::Kind::move
                : request.kind == galactic_ai::ActionKind::invade ? Block::Kind::galactic_invade
                : request.kind == galactic_ai::ActionKind::land ? Block::Kind::galactic_land
                : request.kind == galactic_ai::ActionKind::launch ? Block::Kind::galactic_launch : Block::Kind::galactic_form;
            if (request.kind == galactic_ai::ActionKind::invade && admitted.value().admitted) {
                block.galactic_request = admitted.value().request; block.galactic_actor = request.object;
                block.finished = false; block.result = false;
            } else if (request.kind == galactic_ai::ActionKind::move && admitted.value().admitted) {
                block.galactic_request = admitted.value().request;
                const auto& status = actions->movements().at(block.galactic_request);
                block.galactic_actor = status.actor; block.finished = status.finished;
            } else {
                block.finished = true; block.galactic_actor = admitted.value().admitted ? admitted.value().request : 0;
                if (request.kind == galactic_ai::ActionKind::invade) block.result = false;
                if (request.kind == galactic_ai::ActionKind::regroup || request.kind == galactic_ai::ActionKind::launch) {
                    tf->second.galactic_fleet = block.galactic_actor;
                    if (!tf->second.galactic_fleet) for (const auto member : tf->second.units)
                        if (const auto fleet = actions->containing_fleet(plan->player, member)) { tf->second.galactic_fleet = *fleet; break; }
                }
            }
            blocks_.emplace(id, std::move(block));
            continue;
        }
        if (operation == "build_pad" && tf != taskforces_.end() && arguments.size() == 4) {
            const auto pad = handle_id(arguments[2], handle_game_object);
            const auto type = handle_id(arguments[3], handle_type);
            if (!pad || !type) continue;
            for (std::size_t index = 0; index < tf->second.types.size(); ++index) {
                if (tf->second.types[index] != *type || index >= tf->second.producers.size()
                    || tf->second.producers[index] != *pad || tf->second.sources[index] != 2) continue;
                BuildTask task;
                task.taskforce = tf->second.id; task.block = id;
                task.type = *type; task.producer = *pad; task.source = 2;
                task.direct_pad = true;
                player->tasks.push_back(task);
                Block block;
                block.kind = Block::Kind::produce; block.id = id;
                block.instance = command.issuer; block.taskforce = tf->second.id;
                blocks_.emplace(id, std::move(block));
                // WBP-47/48: explicit Lua Build keeps its current-barrier timing and
                // uses the same reservation precheck as ordinary pad production.
                service_execution(*player, true);
                break;
            }
        } else if (operation == "native_magic" && tf != taskforces_.end() && arguments.size() >= 3
            && host_->ai_world && host_->ai_world->goal_mode() == "GALACTIC") {
            const auto* actors = std::get_if<std::vector<Value>>(&arguments[2].data);
            if (!actors) continue;
            Block block; block.kind = Block::Kind::produce; block.id = id;
            block.instance = command.issuer; block.taskforce = tf->second.id;
            const auto requested_stage = arguments.size() > 3 ? handle_id(arguments[3], handle_game_object).value_or(0) : 0;
            const auto* synchronized = arguments.size() > 4 ? std::get_if<bool>(&arguments[4].data) : nullptr;
            if (!requested_stage) if (auto* goal = find_goal(*player, plan->goal); goal && goal->potential)
                goal->potential->target_contrast_disabled = true; // WGA-341, including direct creation
            std::size_t selected_index{};
            for (const auto& value : *actors) if (const auto object = handle_id(value, handle_game_object)) {
                if (std::find(tf->second.units.begin(), tf->second.units.end(), *object) == tf->second.units.end())
                    tf->second.units.push_back(*object);
                player->assigned[*object] = tf->second.id;
                BuildTask task; task.taskforce = tf->second.id; task.block = id;
                task.type = selected_index < tf->second.types.size() ? tf->second.types[selected_index] : 0;
                ++selected_index;
                task.object = *object; task.finished = task.allocated = task.joined = true;
                task.state_entered = static_cast<std::uint64_t>(frame_); task.staging = requested_stage;
                task.synchronized_staging = synchronized && *synchronized;
                player->tasks.push_back(task);
            }
            if (actors->empty()) {
                BuildTask task; task.taskforce = tf->second.id; task.block = id;
                task.finished = task.failed = true; player->tasks.push_back(task);
            }
            mark_galactic_tasks_changed();
            tf->second.had_units |= !tf->second.units.empty();
            tf->second.types.clear(); tf->second.sources.clear(); tf->second.producers.clear(); tf->second.pool_tokens.clear();
            // GFS-A13 / GFD-A08: creation was visible inside Lua; these finished
            // tasks are consumed by the later execution/block completion phase.
            blocks_.emplace(id, std::move(block));
            record(plan, plan->player, "native_magic", tf->second.name);
        } else if (operation == "produce" && tf != taskforces_.end()) {
            if (player->galactic && (arguments.size() <= 2 || !handle_id(arguments[2], handle_game_object).value_or(0))) {
                if (auto* goal = find_goal(*player, plan->goal); goal && goal->potential)
                    goal->potential->target_contrast_disabled = true; // WGA-341
            }
            // EX-10: one build task per type the TaskForce still has to produce.
            Block block;
            block.kind = Block::Kind::produce;
            block.id = id;
            block.instance = command.issuer;
            block.taskforce = tf->second.id;
            for (std::size_t index = 0; index < tf->second.types.size(); ++index) {
                if (tf->second.types[index] == 0) continue;
                BuildTask task;
                task.taskforce = tf->second.id;
                task.block = id;
                task.type = tf->second.types[index];
                task.source = index < tf->second.sources.size() ? tf->second.sources[index] : 0;
                task.producer = index < tf->second.producers.size() ? tf->second.producers[index] : 0;
                task.pool_token = index < tf->second.pool_tokens.size() ? tf->second.pool_tokens[index] : 0;
                if (player->galactic) {
                    task.state_entered = static_cast<std::uint64_t>(frame_);
                    if (arguments.size() > 2) task.staging = handle_id(arguments[2], handle_game_object).value_or(0);
                    if (arguments.size() > 3) if (const auto* sync = std::get_if<bool>(&arguments[3].data)) task.synchronized_staging = *sync;
                    task.allocated = true;
                    mark_galactic_tasks_changed();
                }
                player->tasks.push_back(task);
            }
            if (std::all_of(tf->second.types.begin(), tf->second.types.end(), [](const auto type) { return type == 0; }))
                block.finished = true;
            tf->second.types.clear();
            tf->second.sources.clear();
            tf->second.producers.clear();
            tf->second.pool_tokens.clear();
            blocks_.emplace(id, std::move(block));
            record(plan, plan->player, "produce", tf->second.name);
        } else if (operation == "reinforce" && tf != taskforces_.end() && arguments.size() == 3) {
            const auto position = detail::position_of(*host_, arguments[2]);
            if (!position) continue;
            Block block;
            block.kind = Block::Kind::reinforce;
            block.id = id;
            block.instance = command.issuer;
            block.taskforce = tf->second.id;
            block.destination = *position;
            block.finished = tf->second.pooled.empty();
            // SAE-11: repeat calls wait for the original operation and never issue purchases.
            for (const auto& [active_id, active] : blocks_) {
                if (active.kind == Block::Kind::reinforce && active.taskforce == block.taskforce
                    && !active.finished && active.waiting_on == 0) {
                    block.waiting_on = active_id;
                    block.finished = true;
                    break;
                }
            }
            blocks_.emplace(id, std::move(block));
        } else if (operation == "move" && tf != taskforces_.end() && arguments.size() >= 4) {
            // EX-30: a movement block over the TaskForce's movers.
            Block block;
            block.kind = Block::Kind::move;
            block.id = id;
            block.instance = command.issuer;
            block.taskforce = tf->second.id;
            const std::string kind = text_of(arguments[2]);
            block.order = kind == "attack" ? Block::Order::attack
                : kind == "attack_move"   ? Block::Order::attack_move
                : kind == "guard"         ? Block::Order::guard
                                          : Block::Order::move;
            block.attack = block.order == Block::Order::attack || block.order == Block::Order::attack_move;
            if (const auto object = handle_id(arguments[3], handle_game_object)) {
                block.destination_object = *object;
            } else if (const auto* list = std::get_if<std::vector<Value>>(&arguments[3].data); list != nullptr && list->size() == 3) {
                math::Fixed* axes[] = {&block.destination.x, &block.destination.y, &block.destination.z};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto value = number_of((*list)[axis]);
                    if (!value) continue;
                    if (auto converted = numeric::to_fixed(*value)) *axes[axis] = converted.value();
                }
            }
            order_block(block, tf->second, *plan);
            record(plan, plan->player, "order", tf->second.name + " " + kind +
                (block.destination_object != 0 ? " object " + std::to_string(block.destination_object) : std::string(" point"))
                + members_of(tf->second.units));
            blocks_.emplace(id, std::move(block));
        } else if (operation == "ambush" && tf != taskforces_.end() && arguments.size() >= 6) {
            Block block;
            block.kind = Block::Kind::ambush;
            block.id = id;
            block.instance = command.issuer;
            block.taskforce = tf->second.id;
            if (const auto object = handle_id(arguments[2], handle_game_object)) block.destination_object = *object;
            block.side = truncate(number_of(arguments[3]).value_or(Real{}));
            block.distance = number_of(arguments[4]).value_or(Real{});
            block.tolerance = number_of(arguments[5]).value_or(Real{});
            order_ambush(block, tf->second, *plan, true);
            record(plan, plan->player, "order", tf->second.name + " ambush " + std::to_string(block.side) +
                (block.finished ? " (no point)" : "") + members_of(tf->second.units));
            blocks_.emplace(id, std::move(block));
        } else if (operation == "result" && arguments.size() >= 2) {
            if (const auto* flag = std::get_if<bool>(&arguments[1].data)) plan->result = *flag;
        } else if (operation == "purge") {
            plan->purge = true;
        } else if (operation == "removable" && arguments.size() >= 2) {
            if (const auto* flag = std::get_if<bool>(&arguments[1].data)) plan->removable = *flag;
        } else if (operation == "release" && tf != taskforces_.end() && arguments.size() >= 3) {
            if (const auto unit = handle_id(arguments[2], handle_game_object)) {
                remove_member(tf->second, *unit);
                player->assigned.erase(*unit);
            }
        } else if (operation == "release_all" && tf != taskforces_.end()) {
            for (const sim::EntityId unit : tf->second.units) {
                return_galactic_actor(tf->second, unit);
                player->assigned.erase(unit);
            }
            tf->second.units.clear();
        } else if (operation == "collect" && tf != taskforces_.end()) {
            if (arguments.size() != 3) continue;
            const auto category = text_of(arguments[2]);
            std::uint64_t mask{};
            const auto parsed = std::from_chars(category.data(), category.data() + category.size(), mask);
            if (parsed.ec != std::errc{} || parsed.ptr != category.data() + category.size()) continue;
            // EX-12 / PL-13: only free objects matching the supplied category mask join.
            for (const ViewUnit& unit : host_->view->units) {
                if (!in_freestore(unit, plan->player)) continue;
                if (player->reserved.contains(unit.id) && player->reserved.at(unit.id) != plan->goal) continue;
                const auto* type = host_->type(unit.type);
                if (type == nullptr || (type->category_bits & mask) == 0) continue;
                tf->second.units.push_back(unit.id);
                tf->second.had_units = true;
                player->assigned[unit.id] = tf->second.id;
            }
        } else if (operation == "attack_positioning" && tf != taskforces_.end() && arguments.size() >= 3) {
            // WTA-23: the flag of the TaskForce's current members. Switched on, a unit's first
            // positioning is at the next service (project choice), then every 150 frames.
            const auto* on = std::get_if<bool>(&arguments[2].data);
            if (on == nullptr) continue;
            for (const sim::EntityId unit : tf->second.units) {
                if (!*on) {
                    positioning_.erase(unit);
                    continue;
                }
                const auto [entry, added] = positioning_.try_emplace(unit);
                entry->second.instance = command.issuer;
                if (added) entry->second.next = frame_ + 1;
            }
            record(plan, plan->player, "attack_positioning", tf->second.name + (*on ? " on" : " off") + members_of(tf->second.units));
        } else if (operation == "block_proposal") {
            ++player->proposal_blocks;
        } else if (operation == "global" && arguments.size() >= 3) {
            if (const auto value = number_of(arguments[2])) globals_[text_of(arguments[1])] = *value;
        }
    }
    std::vector<authoritative::ScriptCommand> ordered;
    ordered.reserve(orders_.size() + kept.size());
    for (authoritative::ScriptCommand& order : orders_) {
        order.tick = report.tick;
        ordered.push_back(std::move(order));
    }
    orders_.clear();
    perception_executor_ = nullptr;
    for (auto& command : kept) ordered.push_back(std::move(command));
    report.commands = std::move(ordered);
    return core::Result<void>::success();
}

} // namespace eawr::script::foc::ai
