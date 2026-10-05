#include "ai_engine_internal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>

namespace eawr::script::foc::ai {

using namespace engine_internal;

// ---- The barrier steps ---------------------------------------------------------------------

core::Result<void> Engine::before_service(const tactical::TacticalSession& world,
    const tactical::TacticalSnapshot& snapshot, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence,
    const sim::PartitionExecutor* executor) {
    const auto preparation = grid_.prepare(executor, *host_, *host_->view);
    frame_ = static_cast<std::int64_t>(world.completed_tick());
    event_tick_ = scripts.completed_tick() + 1;
    sequence_ = &sequence;
    scripts_ = &scripts;
    events_.clear();
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
                    if (goal == data_.goals.end() || goal->second.mode != "SPACE") continue;
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
        // SCH-02: under the staggered schedule the player at position k of the AI players starts its
        // goal service k frames late and runs its planning, execution and learning services k (mod
        // the 0.1 s period) frames after the first service, instead of all on it; faithful, every
        // system starts at the first service. The phases count from the first serviced frame, not
        // from frame 0: the world has completed a tick before the first service, so a deadline
        // counted from 0 would already be due for the players at positions 0 and 1.
        if (staggered()) {
            const std::int64_t period = next_frame(0, to_single(real(1) / real(10)));
            for (std::size_t index = 0; index < players_.size(); ++index) {
                PlayerAi& player = players_[index];
                player.phase = static_cast<std::int64_t>(index);
                player.next_goal = frame_ + player.phase;
                player.next_planning = player.next_execution = player.next_learning = frame_ + player.phase % period;
            }
        }
        for (PlayerAi& player : players_) {
            build_targets(player);
            initialize_goals(player);
        }
    }
    track_damage(snapshot);
    grid_.service(*host_->view, *host_, frame_);
    update_targets();
    service_blocks();
    service_taskforce_events();
    for (PlayerAi& player : players_) {
        AiServiceEvent served{static_cast<std::uint64_t>(frame_), player.player, false, false, false};
        if (player.next_goal <= frame_) {
            // GS-01: the goal system's delay is 0: every frame.
            player.next_goal = next_frame(frame_, Real{});
            served.goals = true;
            service_goals(player);
        }
        if (player.next_planning <= frame_) {
            player.next_planning = next_frame(frame_, to_single(real(1) / real(10)));
            served.planning = true;
            if (auto serviced = service_plans(player, scripts, sequence); !serviced) return serviced;
        }
        if (player.next_execution <= frame_) {
            player.next_execution = next_frame(frame_, to_single(real(1) / real(10)));
            served.execution = true;
            service_execution(player);
        }
        if (host_->setup.journal && (served.goals || served.planning || served.execution)) {
            host_->setup.journal->services.push_back(served);
        }
        if (player.next_learning <= frame_) {
            // LS-02: every 10 s the expired records leave the front of each history.
            player.next_learning = next_frame(frame_, real(10));
            const auto prune = [&](auto& histories) {
                for (auto& [key, history] : histories) {
                    auto& entries = history.entries;
                    while (!entries.empty() && entries.front().second != -1 && entries.front().second <= frame_) {
                        entries.erase(entries.begin());
                    }
                }
            };
            prune(player.activations);
            prune(player.goal_outcomes);
            prune(player.plan_outcomes);
        }
    }
    if (auto attached = drain_attaches(scripts, sequence); !attached) return attached;
    work_.plans_deferred = static_cast<std::uint32_t>(pending_attach_.size());
    for (ScriptEvent& event : events_) {
        if (auto submitted = scripts.submit_event(std::move(event)); !submitted) return submitted;
    }
    events_.clear();
    return core::Result<void>::success();
}

core::Result<void> Engine::after_service(authoritative::ServiceReport& report) {
    // The Lua cost of the tick (#449 budget).
    TickCost cost = work_;
    cost.tick = report.tick;
    for (const authoritative::InstanceLoad& load : report.loads) {
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
        if (operation == "produce" && tf != taskforces_.end()) {
            // EX-10: one build task per type the TaskForce still has to produce.
            Block block;
            block.kind = Block::Kind::produce;
            block.id = id;
            block.instance = command.issuer;
            block.taskforce = tf->second.id;
            for (std::size_t index = 0; index < tf->second.types.size(); ++index) {
                BuildTask task;
                task.taskforce = tf->second.id;
                task.block = id;
                task.type = tf->second.types[index];
                task.source = index < tf->second.sources.size() ? tf->second.sources[index] : 0;
                task.producer = index < tf->second.producers.size() ? tf->second.producers[index] : 0;
                task.pool_token = index < tf->second.pool_tokens.size() ? tf->second.pool_tokens[index] : 0;
                player->tasks.push_back(task);
            }
            if (tf->second.types.empty()) block.finished = true;
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
            for (const sim::EntityId unit : tf->second.units) player->assigned.erase(unit);
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
        } else if (operation == "block_proposal") {
            ++player->proposal_blocks;
        } else if (operation == "global" && arguments.size() >= 3) {
            if (const auto value = number_of(arguments[2])) globals_[text_of(arguments[1])] = *value;
        }
    }
    for (authoritative::ScriptCommand& order : orders_) {
        order.tick = report.tick;
        kept.push_back(std::move(order));
    }
    orders_.clear();
    report.commands = std::move(kept);
    return core::Result<void>::success();
}

} // namespace eawr::script::foc::ai
