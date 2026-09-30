// FoC tactical AI goal, planning, execution and learning systems (#449,
// docs/behaviour/foc-tactical-ai.md "Goal system" GS-xx, "Plans and TaskForces" PL-xx and EX-xx,
// "Learning" LS-xx, "Damage tracking" DT-xx). Engine logic runs serially on the tick barrier: one
// per-player goal loop over shared learning, reservation and random state, whose order is part of
// the rules (GS-01), so it is not partitioned; the per-entity work it reads (the tick's
// simulation) is.

#include "ai_engine.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>

namespace eawr::script::foc::ai {
namespace {

using authoritative::ScriptEvent;
using authoritative::Value;
using authoritative::ValueList;

constexpr std::int64_t fps = tactical::logical_frames_per_second;
const Real huge = real(1000000000000000000LL); // 1e18, the marshallers' reject weight

Real fixed(math::Fixed value) { return numeric::from_fixed(value); }

// GS-01: a serviced system's next frame, frame + max(1, (int)(delay * fps)).
std::int64_t next_frame(std::int64_t frame, Real delay) {
    return frame + std::max<std::int64_t>(1, truncate(to_single(delay * real(fps))));
}

std::string text_of(const Value& value) {
    const std::string* text = std::get_if<std::string>(&value.data);
    return text == nullptr ? std::string() : *text;
}

std::optional<std::uint64_t> handle_id(const Value& value, std::uint32_t kind) {
    const auto* found = std::get_if<authoritative::Handle>(&value.data);
    if (found == nullptr || found->kind != kind) return std::nullopt;
    return found->id;
}

std::optional<Real> number_of(const Value& value) {
    const auto* found = std::get_if<numeric::LuaNumber>(&value.data);
    if (found == nullptr) return std::nullopt;
    return *found;
}

std::string show(Real value) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.3f", std::bit_cast<double>(value.repr));
    return buffer;
}

Real xy_distance_squared(const math::Vec3& a, Real x, Real y) {
    const Real dx = fixed(a.x) - x;
    const Real dy = fixed(a.y) - y;
    return to_single(dx * dx + dy * dy);
}

} // namespace

Engine::Engine(std::shared_ptr<Host> host, AiData data, std::vector<PlanDef> plans)
    : host_(std::move(host)), data_(std::move(data)), plans_(std::move(plans)) {}

// ---- Reads ------------------------------------------------------------------------------------

const TaskForce* Engine::taskforce(std::uint64_t id) const {
    const auto found = taskforces_.find(id);
    return found == taskforces_.end() ? nullptr : &found->second;
}

const Plan* Engine::plan_of_instance(std::uint64_t instance) const {
    const auto found = plan_of_instance_.find(instance);
    return found == plan_of_instance_.end() ? nullptr : plan(found->second);
}

const Plan* Engine::plan(std::uint64_t id) const {
    const auto found = running_.find(id);
    return found == running_.end() ? nullptr : &found->second;
}

const Block* Engine::block(std::uint64_t id) const {
    const auto found = blocks_.find(id);
    return found == blocks_.end() ? nullptr : &found->second;
}

bool Engine::reserved(sim::EntityId object) const {
    for (const PlayerAi& player : players_) {
        if (player.reserved.contains(object) || player.assigned.contains(object)) return true;
    }
    return false;
}

std::optional<Real> Engine::global_value(const std::string& name) const {
    const auto found = globals_.find(name);
    if (found == globals_.end()) return std::nullopt;
    return found->second;
}

PlayerAi* Engine::player_ai(tactical::PlayerId player) {
    for (PlayerAi& entry : players_) {
        if (entry.player == player) return &entry;
    }
    return nullptr;
}

const PlayerAi* Engine::player_ai(tactical::PlayerId player) const {
    for (const PlayerAi& entry : players_) {
        if (entry.player == player) return &entry;
    }
    return nullptr;
}

Goal* Engine::find_goal(PlayerAi& player, std::uint64_t id) {
    for (Goal& goal : player.active) {
        if (goal.id == id) return &goal;
    }
    return nullptr;
}

// FH-13 with EX-02: the AI player's own moving objects, except squadron members, star bases and
// the units a TaskForce holds.
bool Engine::in_freestore(const ViewUnit& unit, tactical::PlayerId player) const {
    if (unit.owner != player || unit.craft) return false;
    const AiType* type = host_->type(unit.type);
    if (type == nullptr || !type->locomotor || type->star_base) return false;
    const PlayerAi* ai = player_ai(player);
    return ai == nullptr || !ai->assigned.contains(unit.id);
}

// PG-08: the combat power metric of an object's company type.
Real Engine::power(const ViewUnit& unit) const {
    const AiType* type = host_->type(unit.type);
    return type == nullptr ? Real{} : type->combat_power;
}

// DT-03: the attacker with the most accumulated threat on the objects (a squadron counts its
// craft's); ties go to the lowest attacker ID. Only live attackers count.
sim::EntityId Engine::deadly_enemy(const std::vector<sim::EntityId>& objects) const {
    std::map<sim::EntityId, Real> merged;
    const auto add = [&](sim::EntityId object) {
        const auto found = threats_.find(object);
        if (found == threats_.end()) return;
        for (const auto& [attacker, threat] : found->second) merged[attacker] = merged[attacker] + threat;
    };
    for (const sim::EntityId object : objects) {
        const ViewUnit* unit = host_->view->find(object);
        if (unit == nullptr) continue;
        if (unit->container) {
            for (const sim::EntityId member : unit->members) add(member);
        } else {
            add(object);
        }
    }
    sim::EntityId best{};
    Real most{};
    for (const auto& [attacker, threat] : merged) {
        if (host_->view->find(attacker) == nullptr) continue;
        if (best == 0 || most < threat) {
            best = attacker;
            most = threat;
        }
    }
    return best;
}

namespace {
// The journal names a TaskForce's members, so a viewer run can follow them (eye checks).
std::string members_of(const std::vector<sim::EntityId>& units) {
    std::string text = " [";
    for (std::size_t index = 0; index < units.size(); ++index) {
        if (index > 0) text += ',';
        text += std::to_string(units[index]);
    }
    return text + "]";
}
}  // namespace

void Engine::record(const Plan* plan, tactical::PlayerId player, std::string event, std::string detail) {
    PlanRecord line;
    line.tick = static_cast<std::uint64_t>(frame_);
    line.player = player;
    if (plan != nullptr) {
        line.plan = plans_[plan->definition].name;
        if (const PlayerAi* ai = player_ai(plan->player)) {
            for (const Goal& goal : ai->active) {
                if (goal.id == plan->goal) line.goal = ai->functions[goal.function].goal_name;
            }
        }
        line.target = target_name(target(plan->target));
    }
    line.event = std::move(event);
    line.detail = std::move(detail);
    if (host_->setup.journal) {
        host_->setup.journal->plans.push_back(
            PlanEvent{line.tick, line.player, line.plan, line.goal, line.target, line.event, line.detail});
    }
    records_.push_back(std::move(line));
}

// ---- The barrier steps ---------------------------------------------------------------------

core::Result<void> Engine::before_service(const tactical::TacticalSession& world,
    const tactical::TacticalSnapshot& snapshot, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    frame_ = static_cast<std::int64_t>(world.completed_tick());
    event_tick_ = scripts.completed_tick() + 1;
    sequence_ = &sequence;
    scripts_ = &scripts;
    events_.clear();
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
        for (PlayerAi& player : players_) build_targets(player);
    }
    track_damage(snapshot);
    grid_.service(*host_->view, *host_, frame_);
    update_targets();
    service_blocks();
    service_taskforce_events();
    for (PlayerAi& player : players_) {
        if (player.next_goal <= frame_) {
            // GS-01: the goal system's delay is 0: every frame.
            player.next_goal = next_frame(frame_, Real{});
            service_goals(player);
        }
        if (player.next_planning <= frame_) {
            player.next_planning = next_frame(frame_, to_single(real(1) / real(10)));
            if (auto serviced = service_plans(player, scripts, sequence); !serviced) return serviced;
        }
        if (player.next_execution <= frame_) {
            player.next_execution = next_frame(frame_, to_single(real(1) / real(10)));
            service_execution(player);
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
    for (ScriptEvent& event : events_) {
        if (auto submitted = scripts.submit_event(std::move(event)); !submitted) return submitted;
    }
    events_.clear();
    return core::Result<void>::success();
}

core::Result<void> Engine::after_service(authoritative::ServiceReport& report) {
    // The Lua cost of the tick (#449 budget).
    TickCost cost;
    cost.tick = report.tick;
    for (const authoritative::InstanceLoad& load : report.loads) {
        if (load.instance >= first_plan_instance) {
            cost.plans += load.instructions;
            ++cost.plan_instances;
        } else {
            cost.freestore += load.instructions;
        }
    }
    costs_.push_back(cost);
    if (host_->setup.journal) {
        host_->setup.journal->costs.push_back(AiTickCost{cost.tick, cost.freestore, cost.plans, cost.plan_instances});
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
            for (const tactical::TypeId type : tf->second.types) {
                BuildTask task;
                task.taskforce = tf->second.id;
                task.block = id;
                task.type = type;
                player->tasks.push_back(task);
            }
            if (tf->second.types.empty()) block.finished = true;
            tf->second.types.clear();
            blocks_.emplace(id, std::move(block));
            record(plan, plan->player, "produce", tf->second.name);
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
            // EX-12: every free store object of the player joins the TaskForce.
            for (const ViewUnit& unit : host_->view->units) {
                if (!in_freestore(unit, plan->player)) continue;
                if (player->reserved.contains(unit.id) && player->reserved.at(unit.id) != plan->goal) continue;
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

// ---- Unit orders (EX-30 stand-ins: the tactical orders the M2 simulation has) -----------------

void Engine::issue_move(const Plan& plan, const ViewUnit& unit, const math::Vec3& destination) {
    authoritative::ScriptCommand command;
    command.issuer = plan.instance;
    command.sequence = order_sequence_++;
    command.verb = std::string(verb_move);
    command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(unit.owner))));
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, unit.id}});
    command.arguments.push_back(Value::number(fixed(destination.x)));
    command.arguments.push_back(Value::number(fixed(destination.y)));
    command.arguments.push_back(Value::number(fixed(destination.z)));
    orders_.push_back(std::move(command));
}

void Engine::issue_order(const Plan& plan, const ViewUnit& unit, const std::string_view verb, const sim::EntityId target,
    const math::Vec3& destination) {
    authoritative::ScriptCommand command;
    command.issuer = plan.instance;
    command.sequence = order_sequence_++;
    command.verb = std::string(verb);
    command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(unit.owner))));
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, unit.id}});
    if (target != sim::invalid_entity_id) {
        command.arguments.push_back(Value{authoritative::Handle{handle_game_object, target}});
    } else {
        command.arguments.push_back(Value::number(fixed(destination.x)));
        command.arguments.push_back(Value::number(fixed(destination.y)));
        command.arguments.push_back(Value::number(fixed(destination.z)));
    }
    orders_.push_back(std::move(command));
}

void Engine::issue_attack(const Plan& plan, const ViewUnit& unit, sim::EntityId target) {
    authoritative::ScriptCommand command;
    command.issuer = plan.instance;
    command.sequence = order_sequence_++;
    command.verb = std::string(verb_attack);
    command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(unit.owner))));
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, unit.id}});
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, target}});
    orders_.push_back(std::move(command));
}

// EX-30: the move list is the TaskForce's live members that move; with an object destination
// and an attack, each attacks it; an attack-move or guard orders each to the object or point
// (FH-40, space-orders OR-12, OR-14); otherwise each moves to the destination point (an object
// destination's position at the order).
void Engine::order_block(Block& block, const TaskForce& taskforce, const Plan& plan) {
    const WorldView& view = *host_->view;
    const ViewUnit* destination = block.destination_object != 0 ? view.find(block.destination_object) : nullptr;
    if (block.destination_object != 0 && destination == nullptr) {
        block.finished = true;
        return;
    }
    for (const sim::EntityId member : taskforce.units) {
        const ViewUnit* unit = view.find(member);
        if (unit == nullptr) continue;
        const AiType* type = host_->type(unit->type);
        if (type == nullptr || !type->locomotor) continue;
        block.movers.push_back(member);
        block.ordered_tick[member] = static_cast<std::uint64_t>(frame_);
        if (destination != nullptr && block.order == Block::Order::attack) {
            issue_attack(plan, *unit, destination->id);
        } else if (block.order == Block::Order::attack_move || block.order == Block::Order::guard) {
            issue_order(plan, *unit, block.order == Block::Order::attack_move ? verb_attack_move : verb_guard,
                destination != nullptr ? destination->id : sim::invalid_entity_id, block.destination);
        } else {
            issue_move(plan, *unit, destination != nullptr ? destination->position : block.destination);
        }
    }
    if (block.movers.empty()) block.finished = true;
}

// EX-35: the ambush point lies on the target's side (its facing, the left or right normal, or
// behind it) at the given distance, then steps outward by the target's reveal range (the
// region size without one) until a square of that size around it holds no more enemy threat
// than the tolerance; none inside the map means no ambush.
std::optional<math::Vec3> Engine::ambush_point(const Block& block, const Plan& plan) const {
    const ViewUnit* target = host_->view->find(block.destination_object);
    if (target == nullptr || !host_->setup.bounds) return std::nullopt;
    const math::Quat& q = target->rotation;
    const Real w = fixed(q.w), qx = fixed(q.x), qy = fixed(q.y), qz = fixed(q.z);
    Real fx = to_single(real(1) - real(2) * (qy * qy + qz * qz));
    Real fy = to_single(real(2) * (qx * qy + w * qz));
    const Real length = square_root(to_single(fx * fx + fy * fy));
    if (length == Real{}) {
        fx = Real{};
        fy = real(1);
    } else {
        fx = to_single(fx / length);
        fy = to_single(fy / length);
    }
    Real dx = fx, dy = fy;
    switch (block.side) {
    case 0: break;
    case 1: dx = -fy; dy = fx; break;
    case 2: dx = fy; dy = -fx; break;
    case 3: dx = -fx; dy = -fy; break;
    default: return std::nullopt;
    }
    const AiType* type = host_->type(target->type);
    Real size = type == nullptr ? Real{} : type->reveal_range;
    if (!(Real{} < size)) size = data_.constants.region_size;
    Real px = to_single(fixed(target->position.x) + block.distance * dx);
    Real py = to_single(fixed(target->position.y) + block.distance * dy);
    const AiBounds& bounds = *host_->setup.bounds;
    const Real half = to_single(size / real(2));
    while (!(px < bounds.left) && !(bounds.right < px) && !(py < bounds.bottom) && !(bounds.top < py)) {
        Rect rect{to_single(px - half), to_single(py - half), size, size};
        const Real threat = grid_.force(*host_, *host_->view, rect, ~std::uint64_t{0}, plan.player, false, Real{}, 0);
        if (!(block.tolerance < threat)) {
            math::Vec3 out = target->position;
            if (auto value = numeric::to_fixed(px)) out.x = value.value();
            if (auto value = numeric::to_fixed(py)) out.y = value.value();
            return out;
        }
        px = to_single(px + size * dx);
        py = to_single(py + size * dy);
    }
    return std::nullopt;
}

void Engine::order_ambush(Block& block, const TaskForce& taskforce, const Plan& plan, bool fresh) {
    block.last_update = frame_;
    const auto destination = ambush_point(block, plan);
    if (!destination) {
        block.finished = true;
        block.ready = false;
        return;
    }
    if (fresh) {
        for (const sim::EntityId member : taskforce.units) {
            const ViewUnit* unit = host_->view->find(member);
            const AiType* type = unit == nullptr ? nullptr : host_->type(unit->type);
            if (type != nullptr && type->locomotor) block.movers.push_back(member);
        }
    }
    for (const sim::EntityId mover : block.movers) {
        const ViewUnit* unit = host_->view->find(mover);
        if (unit == nullptr) continue;
        block.ordered_tick[mover] = static_cast<std::uint64_t>(frame_);
        issue_move(plan, *unit, *destination);
    }
    if (block.movers.empty()) block.finished = true;
}

// ---- Script events -------------------------------------------------------------------------

// EX-40: an event handler is <TaskForce>_<Event> when the plan defines it, else Default_<Event>.
void Engine::emit_call(const Plan& plan, const TaskForce& taskforce, std::string_view event, ValueList arguments) {
    std::string name = taskforce.name + "_" + std::string(event);
    if (!defines_function(plan.instance, name)) {
        name = "Default_" + std::string(event);
        if (!defines_function(plan.instance, name)) return;
    }
    ScriptEvent out;
    out.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, (*sequence_)++};
    out.target = plan.instance;
    out.kind = ScriptEvent::Kind::call;
    out.name = std::move(name);
    out.arguments.push_back(Value{authoritative::Handle{handle_taskforce, taskforce.id}});
    for (Value& argument : arguments) out.arguments.push_back(std::move(argument));
    events_.push_back(std::move(out));
    record(&plan, plan.player, "event", taskforce.name + "_" + std::string(event));
}

// EX-41: events queued on the TaskForce's thread (Unit_Damaged, Unit_Diversion_Finished).
void Engine::emit_thread_event(const Plan& plan, const TaskForce& taskforce, std::string_view event, ValueList arguments) {
    if (taskforce.thread < 0) return;
    std::string name = taskforce.name + "_" + std::string(event);
    if (!defines_function(plan.instance, name)) {
        name = "Default_" + std::string(event);
        if (!defines_function(plan.instance, name)) return;
    }
    ValueList parameters;
    parameters.push_back(Value{authoritative::Handle{handle_taskforce, taskforce.id}});
    for (Value& argument : arguments) parameters.push_back(std::move(argument));
    ScriptEvent out;
    out.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, (*sequence_)++};
    out.target = plan.instance;
    out.kind = ScriptEvent::Kind::thread_signal;
    out.name = std::move(name);
    out.thread = taskforce.thread;
    out.parameter = Value{std::move(parameters)};
    events_.push_back(std::move(out));
}

// A global the engine cannot read as a value (read_global gives none) is a function
// or a table; plans define their handlers as functions.
bool Engine::defines_function(std::uint64_t instance, const std::string& name) const {
    auto value = scripts_->read_global(instance, name);
    return value && !value.value().has_value();
}

void Engine::remove_member(TaskForce& taskforce, sim::EntityId unit) {
    taskforce.units.erase(std::remove(taskforce.units.begin(), taskforce.units.end(), unit), taskforce.units.end());
    taskforce.last_target.erase(unit);
}

// DT-01, DT-02: every 30 frames each damaged object's attackers lose (maximum hull + maximum
// shield) x AI_SpaceThreatDecayStep (engine default 1, FoC data 0.05) of threat and leave at 0; each hit
// adds its projectile's damage to the attacker's threat (a craft's hit counts for its squadron).
void Engine::track_damage(const tactical::TacticalSnapshot& snapshot) {
    const WorldView& view = *host_->view;
    if (frame_ % 30 == 0) {
        for (auto iterator = threats_.begin(); iterator != threats_.end();) {
            const ViewUnit* victim = view.find(iterator->first);
            if (victim == nullptr) {
                iterator = threats_.erase(iterator);
                continue;
            }
            Real step{};
            if (victim->max_hull) step = step + fixed(*victim->max_hull);
            if (victim->max_shields) step = step + fixed(*victim->max_shields);
            step = to_single(step * data_.constants.threat_decay_step);
            for (auto entry = iterator->second.begin(); entry != iterator->second.end();) {
                entry->second = to_single(entry->second - step);
                if (entry->second <= Real{}) {
                    entry = iterator->second.erase(entry);
                } else {
                    ++entry;
                }
            }
            ++iterator;
        }
    }
    last_hits_.clear();
    for (const tactical::CombatEvent& event : snapshot.combat_events()) {
        if (event.kind != tactical::CombatEventKind::projectile_hit) continue;
        const ViewUnit* shooter = view.find(event.shooter);
        const ViewUnit* victim = view.find(event.target);
        if (shooter == nullptr || victim == nullptr) continue;
        const sim::EntityId attacker = shooter->craft && shooter->squadron != 0 ? shooter->squadron : shooter->id;
        // EX-44: a hit is deliberate when the victim (or its squadron) is the shooter's target.
        const ViewUnit* attacking = view.find(attacker);
        const sim::EntityId aimed = attacking != nullptr && attacking->attack_target != 0 ? attacking->attack_target : shooter->attack_target;
        const bool deliberate = aimed == victim->id || (victim->craft && aimed == victim->squadron);
        last_hits_.try_emplace(victim->id, attacker, deliberate);
        Real damage = real(1);
        if (const AiType* type = host_->type(shooter->type)) {
            if (const auto found = type->weapon_damage.find(event.weapon); found != type->weapon_damage.end()) damage = found->second;
        }
        Real& threat = threats_[event.target][attacker];
        threat = to_single(threat + damage);
    }
}

// EX-42 to EX-45: member deaths, No_Units_Remaining, Target_In_Range and Unit_Damaged, and the
// plan target's destruction, in plan then TaskForce order.
void Engine::service_taskforce_events() {
    const WorldView& view = *host_->view;
    for (auto& [plan_id, plan] : running_) {
        if (plan.exited) continue;
        for (const std::uint64_t tf_id : plan.taskforces) {
            auto found = taskforces_.find(tf_id);
            if (found == taskforces_.end()) continue;
            TaskForce& tf = found->second;
            std::vector<sim::EntityId> dead;
            for (const sim::EntityId unit : tf.units) {
                if (view.find(unit) == nullptr) dead.push_back(unit);
            }
            for (const sim::EntityId unit : dead) {
                remove_member(tf, unit);
                if (PlayerAi* player = player_ai(plan.player)) player->assigned.erase(unit);
                emit_call(plan, tf, "Unit_Destroyed", {});
            }
            if (!dead.empty() && tf.units.empty()) {
                bool pending = false;
                if (const PlayerAi* player = player_ai(plan.player)) {
                    for (const BuildTask& task : player->tasks) pending = pending || (task.taskforce == tf.id && !task.finished);
                }
                if (!pending) emit_call(plan, tf, "No_Units_Remaining", {});
            }
            for (const sim::EntityId member : tf.units) {
                const ViewUnit* unit = view.find(member);
                if (unit == nullptr) continue;
                const sim::EntityId target = unit->attack_target;
                sim::EntityId& last = tf.last_target[member];
                if (target != 0 && target != last && view.find(target) != nullptr) {
                    ValueList arguments;
                    arguments.push_back(Value{authoritative::Handle{handle_game_object, member}});
                    arguments.push_back(Value{authoritative::Handle{handle_game_object, target}});
                    emit_call(plan, tf, "Target_In_Range", std::move(arguments));
                }
                last = target;
            }
        }
        if (plan.target_object != 0 && !plan.target_destroyed_signalled && view.find(plan.target_object) == nullptr) {
            plan.target_destroyed_signalled = true;
            for (const std::uint64_t tf_id : plan.taskforces) {
                if (const auto found = taskforces_.find(tf_id); found != taskforces_.end()) {
                    emit_call(plan, found->second, "Original_Target_Destroyed", {});
                }
            }
        }
    }
    // Unit_Damaged: this tick's hits on a TaskForce member, at most one queued per TaskForce
    // between two pumps of its thread.
    if (host_->view == nullptr) return;
    for (auto& [plan_id, plan] : running_) {
        if (plan.exited) continue;
        for (const std::uint64_t tf_id : plan.taskforces) {
            auto found = taskforces_.find(tf_id);
            if (found == taskforces_.end() || found->second.damaged_pending) continue;
            TaskForce& tf = found->second;
            for (const auto& [victim, hit] : last_hits_) {
                const ViewUnit* unit = view.find(victim);
                if (unit == nullptr) continue;
                const sim::EntityId member = unit->craft && unit->squadron != 0 ? unit->squadron : unit->id;
                if (std::find(tf.units.begin(), tf.units.end(), member) == tf.units.end()) continue;
                const ViewUnit* attacker = view.find(hit.first);
                if (attacker == nullptr) continue;
                ValueList arguments;
                arguments.push_back(Value{authoritative::Handle{handle_game_object, member}});
                arguments.push_back(Value{authoritative::Handle{handle_game_object, attacker->id}});
                arguments.push_back(Value{hit.second});
                emit_thread_event(plan, tf, "Unit_Damaged", std::move(arguments));
                tf.damaged_pending = true;
                break;
            }
        }
    }
}

// EX-31 to EX-34: movement blocks finish when every mover has stopped (a point, or an object
// that is not a visible enemy), when an attacked object dies, or when the TaskForce is empty.
void Engine::service_blocks() {
    const WorldView& view = *host_->view;
    for (auto& [id, block] : blocks_) {
        if (block.finished || block.kind == Block::Kind::produce) continue;
        if (block.kind == Block::Kind::ambush) {
            const auto tf = taskforces_.find(block.taskforce);
            const auto plan = tf == taskforces_.end() ? running_.end() : running_.find(tf->second.plan);
            if (tf == taskforces_.end() || plan == running_.end() || view.find(block.destination_object) == nullptr) {
                block.finished = true;
                block.ready = false;
                continue;
            }
            std::vector<sim::EntityId> arrived;
            for (const sim::EntityId mover : block.movers) {
                const ViewUnit* unit = view.find(mover);
                if (unit == nullptr || std::find(tf->second.units.begin(), tf->second.units.end(), mover) == tf->second.units.end()) {
                    arrived.push_back(mover);
                    continue;
                }
                if (static_cast<std::uint64_t>(frame_) < block.ordered_tick[mover] + 2 || unit->moving) continue;
                arrived.push_back(mover);
                ValueList arguments;
                arguments.push_back(Value{authoritative::Handle{handle_game_object, mover}});
                emit_call(plan->second, tf->second, "Unit_Move_Finished", std::move(arguments));
            }
            for (const sim::EntityId mover : arrived) {
                block.movers.erase(std::remove(block.movers.begin(), block.movers.end(), mover), block.movers.end());
            }
            if (block.movers.empty()) {
                block.finished = true;
                block.ready = true;
            } else if (frame_ - block.last_update > 150) {
                order_ambush(block, tf->second, plan->second, false);
            }
            continue;
        }
        const auto tf = taskforces_.find(block.taskforce);
        const auto plan = tf == taskforces_.end() ? running_.end() : running_.find(tf->second.plan);
        if (tf == taskforces_.end() || plan == running_.end() || tf->second.units.empty()) {
            block.finished = true;
            continue;
        }
        const ViewUnit* destination = block.destination_object != 0 ? view.find(block.destination_object) : nullptr;
        if (block.destination_object != 0 && destination == nullptr) {
            block.finished = true;
            for (const std::uint64_t other : plan->second.taskforces) {
                if (const auto found = taskforces_.find(other); found != taskforces_.end()) {
                    emit_call(plan->second, found->second, "Current_Target_Destroyed", {});
                }
            }
            continue;
        }
        const bool enemy_object = destination != nullptr && destination->owner != 0 &&
            !host_->neutral(destination->owner) && !host_->allied(destination->owner, plan->second.player);
        std::vector<sim::EntityId> arrived;
        for (const sim::EntityId mover : block.movers) {
            const ViewUnit* unit = view.find(mover);
            if (unit == nullptr || std::find(tf->second.units.begin(), tf->second.units.end(), mover) == tf->second.units.end()) {
                arrived.push_back(mover);
                continue;
            }
            // An attacker keeps its target; it reports no end of movement.
            if (block.attack && enemy_object) continue;
            const std::uint64_t ordered = block.ordered_tick[mover];
            if (static_cast<std::uint64_t>(frame_) < ordered + 2 || unit->moving) continue;
            arrived.push_back(mover);
            ValueList arguments;
            arguments.push_back(Value{authoritative::Handle{handle_game_object, mover}});
            emit_call(plan->second, tf->second, "Unit_Move_Finished", std::move(arguments));
        }
        for (const sim::EntityId mover : arrived) {
            block.movers.erase(std::remove(block.movers.begin(), block.movers.end(), mover), block.movers.end());
        }
        if (block.movers.empty() && !(enemy_object && block.attack)) block.finished = true;
    }
}

// ---- Goal system ---------------------------------------------------------------------------

// GS-03: a goal type is proposable while no plan blocks proposal and the space template turns
// its category on (and not off).
bool Engine::proposable(const PlayerAi& player, const GoalFunctionEntry& function) const {
    if (player.proposal_blocks > 0) return false;
    if (player.space_template == nullptr) return true;
    const std::string& category = function.goal->category;
    return player.space_template->goals_on.contains(category) && !player.space_template->goals_off.contains(category);
}

// FT-01/FT-03: the best starts as none at score 0 and is replaced only by a strictly higher
// score, so ties keep the earlier candidate and nothing above 0 means no best. A replaced best
// and every other candidate enter the draw, but a weight of 0 or less does not; with nothing in
// the draw, or a zero fraction, the best (or nil) is the answer.
std::optional<std::size_t> choose_target(const std::vector<Real>& scores, Real fraction, const std::function<Real()>& unit_draw) {
    std::optional<std::size_t> best;
    Real best_score{};
    std::vector<std::pair<std::size_t, Real>> weighed;
    const auto weigh = [&weighed](std::optional<std::size_t> candidate, Real weight) {
        if (candidate && Real{} < weight) weighed.emplace_back(*candidate, weight);
    };
    for (std::size_t index = 0; index < scores.size(); ++index) {
        if (best_score < scores[index]) {
            weigh(best, to_single(best_score));
            best = index;
            best_score = scores[index];
        } else {
            weigh(index, to_single(scores[index]));
        }
    }
    if (weighed.empty() || fraction == Real{}) return best;
    Real total{};
    for (const auto& entry : weighed) total = to_single(total + entry.second);
    weigh(best, to_single(total / to_single(fraction)));
    Real sum{};
    for (const auto& entry : weighed) sum = to_single(sum + entry.second);
    const Real draw = unit_draw() * sum;
    Real running{};
    std::size_t answer = weighed.back().first;
    for (const auto& [index, weight] : weighed) {
        running = to_single(running + weight);
        if (draw < running) {
            answer = index;
            break;
        }
    }
    return answer;
}

// GS-11: which targets a goal applies to. A region is a TACTICAL_LOCATION; an object is a
// friendly (allied owner) or enemy unit, or a structure (a star base).
bool Engine::target_matches(tactical::PlayerId player, const std::set<std::string>& flags, const Target* target) const {
    if (target == nullptr) return false;
    if (target->object == 0) return flags.contains("TACTICAL_LOCATION");
    const ViewUnit* unit = host_->view->find(target->object);
    if (unit == nullptr) return false;
    const bool allied = unit->owner != 0 && host_->players.contains(unit->owner) && !host_->neutral(unit->owner) &&
        host_->allied(unit->owner, player);
    const AiType* type = host_->type(unit->type);
    const bool structure = type != nullptr && type->star_base;
    const std::string flag = std::string(allied ? "FRIENDLY_" : "ENEMY_") + (structure ? "STRUCTURE" : "UNIT");
    return flags.contains(flag);
}

bool Engine::applies(const PlayerAi& player, const GoalType& goal, const Target* target) const {
    return target_matches(player.player, goal.application, target);
}

// GS-12: goals are alike when they share a target and a goal type, or one names the other's
// type in Is_Like.
bool Engine::like(const PlayerAi& player, const Goal& a, const Goal& b) const {
    if (a.target != b.target) return false;
    const GoalFunctionEntry& left = player.functions[a.function];
    const GoalFunctionEntry& right = player.functions[b.function];
    if (left.goal_name == right.goal_name) return true;
    const auto& a_like = left.goal->is_like;
    const auto& b_like = right.goal->is_like;
    return std::find(a_like.begin(), a_like.end(), right.goal_name) != a_like.end() ||
        std::find(b_like.begin(), b_like.end(), left.goal_name) != b_like.end();
}

std::int64_t Engine::recent_failures(const History* history) const {
    if (history == nullptr) return 0;
    std::int64_t count = 0;
    for (const auto& [success, expiry] : history->entries) count += success ? 0 : 1;
    return count;
}

void Engine::register_activation(PlayerAi& player, const Goal& goal, bool success) {
    const GoalFunctionEntry& function = player.functions[goal.function];
    const Real duration = function.goal->activation_tracking_duration;
    const std::int64_t expiry = duration < Real{} ? -1 : frame_ + truncate(to_single(duration * real(fps)));
    player.activations[{function.goal_name, goal.target}].entries.emplace_back(success, expiry);
}

Real Engine::category_budget(const PlayerAi& player, const std::string& category) const {
    if (player.space_template == nullptr) return Real{};
    const auto found = player.space_template->budget.find(category);
    if (found == player.space_template->budget.end()) return Real{};
    const Equation* equation = data_.equations.find(found->second);
    if (equation == nullptr) return Real{};
    return run_equation(*equation, context_of(player, nullptr)).value_or(Real{});
}

// GS-02 to GS-05.
void Engine::service_goals(PlayerAi& player) {
    if (player.sleep_frames >= 1) {
        --player.sleep_frames;
        return;
    }
    propose(player);
    if (!player.maintenance_due) return;
    maintain(player);
    player.maintenance_due = false;
    // GS-05: the next pass's per-frame budget.
    std::int64_t humans = 0;
    for (const AiPlayer& entry : host_->setup.players) humans += (!entry.ai && !entry.neutral) ? 1 : 0;
    const std::int64_t others = std::max<std::int64_t>(1, static_cast<std::int64_t>(host_->setup.players.size()) - humans);
    const Real share = to_single(real(player.nontrivial) / to_single(real(fps) * real(5)));
    std::int64_t per_frame = truncate(share);
    if (real(per_frame) < share) ++per_frame;
    const std::int64_t cap = truncate(to_single(to_single(real(20) / real(others)) + to_single(real(1) / real(2))));
    per_frame = std::max<std::int64_t>(1, std::min(per_frame, cap));
    player.per_frame = per_frame;
    player.nontrivial = 0;
    player.sleep_frames = truncate(to_single(real(fps) * player.difficulty.space_goal_cycle_sleep));
}

// GS-04: the proposal pass, a per-frame budget of (function, target) evaluations.
void Engine::propose(PlayerAi& player) {
    if (player.maintenance_due) return;
    for (std::int64_t step = 0; step < player.per_frame; ++step) {
        if (player.next_function >= player.functions.size()) {
            player.next_function = 0;
            player.next_target = 0;
            player.maintenance_due = true;
            return;
        }
        const GoalFunctionEntry& function = player.functions[player.next_function];
        if (!proposable(player, function)) {
            ++player.next_function;
            player.next_target = 0;
            --step;
            continue;
        }
        const bool global = function.goal->application.contains("GLOBAL");
        const Target* target = nullptr;
        if (!global) {
            if (player.next_target >= player.targets.size()) {
                ++player.next_function;
                player.next_target = 0;
                --step;
                continue;
            }
            target = this->target(player.targets[player.next_target]);
            if (!applies(player, *function.goal, target)) {
                ++player.next_target;
                --step;
                continue;
            }
        }
        Goal goal;
        goal.id = player.next_goal_id++;
        goal.function = player.next_function;
        goal.target = target == nullptr ? 0 : target->id;
        const auto advance = [&] {
            if (global) {
                ++player.next_function;
                player.next_target = 0;
            } else {
                ++player.next_target;
            }
        };
        bool culled = false;
        for (const Goal& active : player.active) culled = culled || like(player, active, goal);
        if (culled) {
            advance();
            --step;
            continue;
        }
        ++player.nontrivial;
        advance();
        if (function.equation == nullptr) continue;
        const auto value = run_equation(*function.equation, context_of(player, target));
        if (!value) continue;
        const auto goal_key = std::make_pair(function.goal_name, goal.target);
        const auto outcomes = player.goal_outcomes.find(goal_key);
        const auto activations = player.activations.find(goal_key);
        Real desire = *value;
        desire = desire + to_single(function.goal->per_failure_desire_adjust *
            real(recent_failures(outcomes == player.goal_outcomes.end() ? nullptr : &outcomes->second)));
        desire = desire + to_single(function.goal->per_activation_failure_desire_adjust *
            real(recent_failures(activations == player.activations.end() ? nullptr : &activations->second)));
        if (!(Real{} < desire)) continue;
        goal.desire = to_single(desire);
        if (!plan_goal(player, goal)) continue;
        player.proposed.push_back(std::move(goal));
    }
}

// PL-01: a plan is drawn for the goal among the plans of its type whose goal category the
// template turns on, weighted by the plan's success rate plus 1.
bool Engine::plan_goal(PlayerAi& player, Goal& goal) {
    const GoalFunctionEntry& function = player.functions[goal.function];
    std::vector<std::pair<std::size_t, Real>> usable;
    Real total{};
    for (std::size_t index = 0; index < plans_.size(); ++index) {
        const PlanDef& plan = plans_[index];
        if (std::find(plan.goals.begin(), plan.goals.end(), function.goal_name) == plan.goals.end()) continue;
        if (player.space_template != nullptr) {
            const std::string& category = function.goal->category;
            if (!player.space_template->plans_on.contains(category) || player.space_template->plans_off.contains(category)) {
                continue;
            }
        }
        Real rate = real(1);
        if (const auto found = player.plan_success.find(index); found != player.plan_success.end() && found->second.second > 0) {
            rate = to_single(real(found->second.first) / real(found->second.second));
        }
        const Real weight = to_single(rate + real(1));
        usable.emplace_back(index, weight);
        total = to_single(total + weight);
    }
    if (usable.empty()) return false;
    const Real draw = sync_.uniform(Real{}, total);
    std::size_t chosen = usable.back().first;
    Real sum{};
    for (const auto& [index, weight] : usable) {
        sum = to_single(sum + weight);
        if (draw < sum) {
            chosen = index;
            break;
        }
    }
    PotentialPlan potential;
    potential.plan = chosen;
    const bool valid = select_units(player, goal, potential);
    goal.potential = std::move(potential);
    return valid;
}

// EX-01: the free store objects a potential plan may take, with their cost (the XY distance
// squared to the goal target, 0 without one).
std::vector<const ViewUnit*> Engine::freestore_list(const PlayerAi& player, const Goal& goal, const PlanDef& plan) const {
    std::vector<const ViewUnit*> out;
    for (const ViewUnit& unit : host_->view->units) {
        if (!in_freestore(unit, player.player)) continue;
        if (const auto found = player.reserved.find(unit.id); found != player.reserved.end() && found->second != goal.id) continue;
        if (unit.health < data_.constants.health_low_threshold) continue;
        if (!plan.allow_engaged) {
            const AiType* type = host_->type(unit.type);
            const auto fighter = host_->setup.content.categories.find("FIGHTER");
            if (type != nullptr && unit.container && fighter != host_->setup.content.categories.end() &&
                (type->category_bits & fighter->second) != 0) {
                bool engaged = unit.attack_target != 0;
                for (const sim::EntityId member : unit.members) {
                    if (const ViewUnit* craft = host_->view->find(member)) engaged = engaged || craft->attack_target != 0;
                }
                if (engaged) continue;
            }
        }
        out.push_back(&unit);
    }
    return out;
}

// PL-31: the target's contrast list: entry 0 the target force, then one entry per enemy
// contrast category, each times the plan's maximum contrast factor.
std::vector<Contrast> Engine::contrast_list(const PlayerAi& player, const Goal& goal, const PlanDef& plan, Real max_factor) const {
    static_cast<void>(plan);
    std::vector<Contrast> out;
    const Target* target = this->target(goal.target);
    if (target == nullptr) return out;
    const auto force = [&](std::uint64_t category) -> Real {
        if (target->object != 0) {
            const ViewUnit* unit = host_->view->find(target->object);
            if (unit == nullptr) return Real{};
            const AiType* type = host_->type(unit->type);
            if (type == nullptr || (type->category_bits & category) == 0) return Real{};
            return type->combat_power;
        }
        return grid_.force(*host_, *host_->view, target->region, category, player.player, false, Real{}, 0);
    };
    const Real total = force(~std::uint64_t{0});
    out.push_back(Contrast{0, to_single(total * max_factor)});
    for (const std::uint64_t category : host_->contrast_order) {
        out.push_back(Contrast{category, total <= Real{} ? Real{} : to_single(force(category) * max_factor)});
    }
    if (total <= Real{}) {
        for (Contrast& entry : out) entry.force = Real{};
    }
    return out;
}

namespace {

// PL-33: the share of the target force the selection leaves, overall and by category.
bool contrast_met(const std::vector<Contrast>& target, const std::vector<Contrast>& current, Real threshold) {
    if (target.empty() || target.front().force <= Real{}) return true;
    const Real total = target.front().force;
    const Real left = std::max(current.front().force, Real{});
    if (threshold < left / total) return false;
    Real breakdown{};
    for (std::size_t index = 1; index < target.size(); ++index) {
        if (target[index].force <= Real{}) continue;
        const Real share = std::max(current[index].force, Real{}) / target[index].force;
        breakdown = breakdown + share * (target[index].force / total);
    }
    return !(threshold < breakdown);
}

} // namespace

// PL-20 to PL-35: the marshallers select units for the goal's plan one at a time.
bool Engine::select_units(PlayerAi& player, const Goal& goal, PotentialPlan& potential) {
    const PlanDef& plan = plans_[potential.plan];
    potential.units.clear();
    potential.freestore.clear();
    potential.taskforce_of_unit.clear();
    potential.valid = false;
    const Target* target = this->target(goal.target);
    Real target_x{}, target_y{};
    bool has_target = false;
    if (target != nullptr) {
        if (target->object != 0) {
            if (const ViewUnit* unit = host_->view->find(target->object)) {
                target_x = fixed(unit->position.x);
                target_y = fixed(unit->position.y);
                has_target = true;
            }
        } else {
            target_x = target->x;
            target_y = target->y;
            has_target = true;
        }
    }
    // Free store marshaller (PL-21).
    const std::vector<const ViewUnit*> store = plan.allow_free_store ? freestore_list(player, goal, plan) : std::vector<const ViewUnit*>{};
    std::vector<Real> cost(store.size());
    Real max_cost{};
    for (std::size_t index = 0; index < store.size(); ++index) {
        cost[index] = has_target ? xy_distance_squared(store[index]->position, target_x, target_y) : Real{};
        max_cost = std::max(max_cost, cost[index]);
    }
    std::vector<bool> allocated(store.size());
    // Plan marshaller (PL-22): the team layout and the proposal types.
    struct Slot {
        std::size_t taskforce{};
        std::size_t team{};
        std::int32_t count{};
    };
    std::vector<Slot> layout;
    std::vector<tactical::TypeId> proposals;
    for (std::size_t tf = 0; tf < plan.taskforces.size(); ++tf) {
        for (std::size_t team = 0; team < plan.taskforces[tf].teams.size(); ++team) {
            layout.push_back(Slot{tf, team, 0});
            for (const tactical::TypeId type : plan.taskforces[tf].teams[team].types) {
                if (std::find(proposals.begin(), proposals.end(), type) != proposals.end()) continue;
                bool present = false;
                for (const ViewUnit* unit : store) present = present || unit->type == type;
                if (present) proposals.push_back(type);
            }
        }
    }
    const std::size_t start = layout.empty() ? 0 : static_cast<std::size_t>(sync_.range(0, static_cast<std::int32_t>(layout.size()) - 1));
    std::vector<std::int32_t> tf_size(plan.taskforces.size());
    std::vector<Real> tf_force(plan.taskforces.size());
    std::vector<std::uint64_t> tf_categories(plan.taskforces.size());
    // Contrast marshaller (PL-30).
    const auto plan_history = player.plan_outcomes.find({potential.plan, goal.target});
    const Real failures = real(recent_failures(plan_history == player.plan_outcomes.end() ? nullptr : &plan_history->second));
    const Real adjust = to_single(failures * plan.per_failure_contrast_adjust);
    const Real max_scale = to_single(plan.max_contrast + adjust);
    const Real min_scale = to_single(plan.min_contrast + adjust);
    const bool contrast = target != nullptr && !plan.ignore_target;
    potential.threshold = contrast && Real{} < max_scale ? to_single(real(1) - min_scale / max_scale) : Real{};
    potential.target_contrast = contrast
        ? contrast_list(player, goal, plan, to_single(max_scale * player.difficulty.space_contrast_multiplier))
        : std::vector<Contrast>{};
    potential.current_contrast = potential.target_contrast;
    const auto still_needs = [&] {
        for (const Slot& slot : layout) {
            if (slot.count < plan.taskforces[slot.taskforce].teams[slot.team].min_count) return true;
        }
        for (std::size_t tf = 0; tf < plan.taskforces.size(); ++tf) {
            if (tf_force[tf] < plan.taskforces[tf].minimum_force || tf_size[tf] < plan.taskforces[tf].minimum_size) return true;
        }
        for (const std::uint64_t category : plan.required_categories) {
            bool matched = false;
            for (const std::uint64_t bits : tf_categories) matched = matched || (bits & category) != 0;
            if (!matched) return true;
        }
        return false;
    };
    const auto type_power = [&](tactical::TypeId id) {
        const AiType* type = host_->type(id);
        return type == nullptr ? Real{} : type->combat_power;
    };
    std::vector<std::size_t> contrast_index(proposals.size());
    std::vector<bool> erased(proposals.size());
    for (;;) {
        std::vector<Real> weights(proposals.size(), -huge);
        std::vector<std::size_t> allocation(proposals.size(), store.size());
        for (std::size_t index = 0; index < proposals.size(); ++index) {
            if (erased[index]) continue;
            const tactical::TypeId type = proposals[index];
            const AiType* info = host_->type(type);
            Real weight{};
            bool reject = false;
            // Free store: the nearest unallocated object of the type.
            std::size_t best = store.size();
            for (std::size_t object = 0; object < store.size(); ++object) {
                if (allocated[object] || store[object]->type != type) continue;
                if (best == store.size() || cost[object] < cost[best]) best = object;
            }
            allocation[index] = best;
            if (best != store.size() && Real{} < max_cost) weight = weight + to_single(real(1) - cost[best] / max_cost);
            // Tech tree (tactical): Tech_Level / 5 * 2.
            if (info != nullptr) weight = weight + to_single(to_single(real(info->tech_level) / real(5)) * real(2));
            // Plan: the first team of the type with room.
            Real plan_weight = huge;
            for (const Slot& slot : layout) {
                const TeamDef& team = plan.taskforces[slot.taskforce].teams[slot.team];
                if (std::find(team.types.begin(), team.types.end(), type) == team.types.end()) continue;
                if (team.percentage_based) {
                    plan_weight = real(1);
                    break;
                }
                if (slot.count < team.max_count) {
                    plan_weight = to_single(real(1) - real(slot.count) / real(team.max_count));
                    break;
                }
            }
            if (plan_weight == huge) reject = true;
            weight = weight + (reject ? Real{} : plan_weight);
            // Reinforcement: a type the free store does not hold has no reinforcement pool.
            if (!reject && best == store.size() && !plan.magic) {
                erased[index] = true;
                continue;
            }
            // Contrast.
            if (!reject) {
                Real contrast_weight{};
                if (plan.ignore_target) {
                    contrast_weight = sync_.uniform(Real{}, to_single(type_power(type) / real(500)));
                } else if (contrast && info != nullptr) {
                    Real top{};
                    std::size_t top_index = 0;
                    for (std::size_t entry = 1; entry < potential.current_contrast.size(); ++entry) {
                        const Real force = potential.current_contrast[entry].force;
                        if (!(Real{} < force)) continue;
                        const Real factor = average_contrast(*host_, *info, potential.current_contrast[entry].category);
                        const Real applied = to_single(type_power(type) * factor);
                        const Real ratio = std::max(to_single(force - applied), Real{}) / std::max(force, applied);
                        const Real value = to_single(std::max(to_single(real(1) - ratio * ratio), Real{}) * factor);
                        if (top < value) {
                            top = value;
                            top_index = entry;
                        }
                    }
                    contrast_index[index] = top_index;
                    if (Real{} < top) top = to_single(top * real(2));
                    if (contrast_met(potential.target_contrast, potential.current_contrast, potential.threshold)) {
                        if (still_needs()) {
                            contrast_weight = Real{};
                        } else {
                            reject = true;
                        }
                    } else {
                        contrast_weight = top;
                    }
                }
                weight = weight + contrast_weight;
            }
            weights[index] = reject ? -huge : to_single(weight);
        }
        // PL-24: unit variety. A type holding half the selection yields while another can go in.
        if (!potential.units.empty()) {
            std::vector<Real> varied = weights;
            for (std::size_t index = 0; index < proposals.size(); ++index) {
                if (!(-huge < weights[index])) continue;
                bool other = false;
                for (std::size_t alternative = 0; alternative < proposals.size(); ++alternative) {
                    other = other || (alternative != index && -huge < weights[alternative]);
                }
                const auto count = std::count(potential.units.begin(), potential.units.end(), proposals[index]);
                if (other && static_cast<std::size_t>(count) * 2 >= potential.units.size()) varied[index] = -huge;
            }
            weights = std::move(varied);
        }
        std::size_t pick = proposals.size();
        for (std::size_t index = 0; index < proposals.size(); ++index) {
            if (!(-huge < weights[index])) continue;
            if (pick == proposals.size() || weights[pick] < weights[index]) pick = index;
        }
        if (pick == proposals.size()) break;
        const tactical::TypeId type = proposals[pick];
        const std::size_t object = allocation[pick];
        potential.units.push_back(type);
        potential.freestore.push_back(object == store.size() ? 0 : store[object]->id);
        if (object != store.size()) allocated[object] = true;
        // The team: rotating from the drawn start, the first with room, else any of the type.
        std::size_t chosen = layout.size();
        for (std::size_t step = 0; step < layout.size(); ++step) {
            const std::size_t slot = (start + step) % layout.size();
            const TeamDef& team = plan.taskforces[layout[slot].taskforce].teams[layout[slot].team];
            if (std::find(team.types.begin(), team.types.end(), type) == team.types.end()) continue;
            if (team.percentage_based || layout[slot].count < team.max_count) {
                chosen = slot;
                break;
            }
        }
        if (chosen == layout.size()) {
            for (std::size_t slot = 0; slot < layout.size(); ++slot) {
                const TeamDef& team = plan.taskforces[layout[slot].taskforce].teams[layout[slot].team];
                if (std::find(team.types.begin(), team.types.end(), type) != team.types.end()) {
                    chosen = slot;
                    break;
                }
            }
        }
        const std::size_t tf = chosen == layout.size() ? 0 : layout[chosen].taskforce;
        if (chosen != layout.size()) ++layout[chosen].count;
        potential.taskforce_of_unit.push_back(tf);
        ++tf_size[tf];
        tf_force[tf] = to_single(tf_force[tf] + type_power(type));
        if (const AiType* info = host_->type(type)) tf_categories[tf] |= info->category_bits;
        // PL-32: the unit's contrast leaves the target's.
        if (!potential.current_contrast.empty()) {
            const Real unit_power = type_power(type);
            const std::size_t entry = contrast_index[pick];
            if (entry < 1) {
                potential.current_contrast.front().force = to_single(potential.current_contrast.front().force - unit_power);
            } else if (const AiType* info = host_->type(type)) {
                const Real factor = average_contrast(*host_, *info, potential.current_contrast[entry].category);
                potential.current_contrast[entry].force = to_single(potential.current_contrast[entry].force - unit_power * factor);
                potential.current_contrast.front().force = to_single(potential.current_contrast.front().force - unit_power);
            }
        }
    }
    // PL-25: the plan's validity.
    bool valid = true;
    if (potential.units.empty()) {
        valid = false;
        for (const TaskForceDef& tf : plan.taskforces) valid = valid || tf.required;
    }
    if (still_needs()) valid = false;
    if (contrast && !contrast_met(potential.target_contrast, potential.current_contrast, potential.threshold)) valid = false;
    potential.valid = valid;
    return valid;
}

// GS-21: a potential plan is still valid when its units are still there for it.
bool Engine::test_valid(PlayerAi& player, Goal& goal) {
    if (!goal.potential || !goal.potential->valid) return false;
    if (goal.plan != 0) return running_.contains(goal.plan);
    if (!goal.potential->reserved) return select_units(player, goal, *goal.potential);
    for (const sim::EntityId object : goal.potential->freestore) {
        if (object == 0) continue;
        const ViewUnit* unit = host_->view->find(object);
        if (unit == nullptr || !in_freestore(*unit, player.player)) return false;
        if (const auto found = player.reserved.find(object); found != player.reserved.end() && found->second != goal.id) return false;
    }
    return true;
}

// GS-22: the contrast test of an active goal.
bool Engine::test_target_contrast(PlayerAi& player, Goal& goal) {
    if (!goal.potential || !goal.potential->valid) return false;
    if (goal.plan != 0) {
        // A running plan keeps its goal while its target lives (fidelity list: the retail test
        // recomputes the contrast of the TaskForce units).
        const Target* target = this->target(goal.target);
        return target == nullptr || target->object == 0 || host_->view->find(target->object) != nullptr;
    }
    if (!goal.potential->reserved) return test_valid(player, goal);
    if (!test_valid(player, goal)) return false;
    const PlanDef& plan = plans_[goal.potential->plan];
    if (plan.ignore_target || goal.target == 0) return true;
    return select_units(player, goal, *goal.potential);
}

void Engine::reserve(PlayerAi& player, Goal& goal) {
    if (!goal.potential) return;
    for (const sim::EntityId object : goal.potential->freestore) {
        if (object != 0) player.reserved[object] = goal.id;
    }
    goal.potential->reserved = true;
}

void Engine::release(PlayerAi& player, const Goal& goal) {
    for (auto iterator = player.reserved.begin(); iterator != player.reserved.end();) {
        if (iterator->second == goal.id) {
            iterator = player.reserved.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

// GS-30: goal set maintenance, category by category in budget order.
void Engine::maintain(PlayerAi& player) {
    const auto by_desire = [](const Goal& a, const Goal& b) { return b.desire < a.desire; };
    std::stable_sort(player.active.begin(), player.active.end(), by_desire);
    std::vector<std::string> categories;
    for (const auto* set : {&player.active, &player.proposed}) {
        for (const Goal& goal : *set) {
            const std::string& category = player.functions[goal.function].goal->category;
            if (std::find(categories.begin(), categories.end(), category) == categories.end()) categories.push_back(category);
        }
    }
    std::sort(categories.begin(), categories.end());
    std::vector<std::pair<Real, std::string>> ordered;
    for (const std::string& category : categories) ordered.emplace_back(category_budget(player, category), category);
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return b.first < a.first; });
    // Reservations are rebuilt by the pass.
    for (Goal& goal : player.active) {
        if (goal.potential && goal.plan == 0) {
            release(player, goal);
            goal.potential->reserved = false;
        }
    }
    // GS-31: at most the active count plus the extension of proposals per category.
    std::stable_sort(player.proposed.begin(), player.proposed.end(), by_desire);
    const std::size_t extension = 2; // GS-31: the player type's goal set extension (engine default)
    std::map<std::string, std::size_t> seen;
    std::vector<Goal> culled;
    for (Goal& goal : player.proposed) {
        const std::string& category = player.functions[goal.function].goal->category;
        std::size_t active_count = 0;
        for (const Goal& active : player.active) {
            active_count += player.functions[active.function].goal->category == category ? 1 : 0;
        }
        if (seen[category]++ < active_count + extension) culled.push_back(std::move(goal));
    }
    player.proposed = std::move(culled);
    std::vector<Goal> kept;
    for (const auto& [budget, category] : ordered) maintain_category(player, category, kept);
    player.proposed.clear();
    // Kept goals become the active set; each gets its plan.
    player.active = std::move(kept);
    for (Goal& goal : player.active) {
        if (goal.plan != 0 || !goal.potential) continue;
        if (auto attached = attach_plan(player, goal, *scripts_, *sequence_); !attached) goal.finished = true;
    }
}

void Engine::maintain_category(PlayerAi& player, const std::string& category, std::vector<Goal>& kept_all) {
    const auto in_category = [&](const Goal& goal) { return player.functions[goal.function].goal->category == category; };
    std::vector<Goal> active;
    for (Goal& goal : player.active) {
        if (in_category(goal)) active.push_back(goal);
    }
    std::vector<Goal> candidates;
    std::size_t proposed_count = 0;
    for (Goal& goal : player.proposed) {
        if (!in_category(goal)) continue;
        ++proposed_count;
        const bool global = player.functions[goal.function].goal->application.contains("GLOBAL");
        if (global || applies(player, *player.functions[goal.function].goal, target(goal.target))) candidates.push_back(goal);
    }
    const auto finish = [&](Goal& goal) {
        release(player, goal);
        if (goal.plan != 0) finish_plan(player, goal.plan, *scripts_);
    };
    if (proposed_count == 0) {
        for (Goal& goal : active) {
            if (goal.finished || (goal.plan != 0 && !running_.contains(goal.plan))) continue;
            if (goal.potential && goal.plan == 0) reserve(player, goal);
            kept_all.push_back(goal);
        }
        return;
    }
    const std::size_t limit = active.size() + 2;
    Real threshold = real(-1);
    for (const Goal& goal : candidates) threshold = std::max(threshold, goal.desire);
    std::vector<Goal> keep;
    std::vector<Goal> abandon;
    for (Goal& goal : active) {
        if (goal.finished || (goal.plan != 0 && !running_.contains(goal.plan))) {
            finish(goal);
            continue;
        }
        const Plan* running = goal.plan != 0 ? plan(goal.plan) : nullptr;
        const bool cullable = running == nullptr || running->removable;
        if (!(goal.desire < threshold) || !cullable) {
            if (!goal.potential) {
                keep.push_back(goal);
            } else if (!plans_[goal.potential->plan].magic && !test_target_contrast(player, goal)) {
                finish(goal);
            } else {
                if (goal.plan == 0) reserve(player, goal);
                keep.push_back(goal);
            }
        } else if (goal.potential && !test_target_contrast(player, goal)) {
            finish(goal);
        } else {
            abandon.push_back(goal);
        }
    }
    const auto by_desire = [](const Goal& a, const Goal& b) { return b.desire < a.desire; };
    std::stable_sort(keep.begin(), keep.end(), by_desire);
    std::stable_sort(abandon.begin(), abandon.end(), by_desire);
    std::optional<Goal> first_abandon;
    if (!abandon.empty()) first_abandon = abandon.front();
    std::vector<Goal> pool = abandon;
    pool.insert(pool.end(), candidates.begin(), candidates.end());
    std::stable_sort(pool.begin(), pool.end(), by_desire);
    const auto like_kept = [&](const Goal& goal) {
        for (const Goal& other : keep) {
            if (other.id != goal.id && like(player, other, goal)) return true;
        }
        for (const Goal& other : kept_all) {
            if (other.id != goal.id && like(player, other, goal)) return true;
        }
        return false;
    };
    const auto activate = [&](Goal& goal) {
        if (goal.plan == 0) reserve(player, goal);
        if (!goal.active) {
            goal.active = true;
            register_activation(player, goal, true);
        }
        keep.push_back(goal);
    };
    // Pass 1: in desire order until a goal cannot go in.
    while (!pool.empty() && keep.size() < limit) {
        Goal goal = pool.front();
        if (!proposable(player, player.functions[goal.function]) || like_kept(goal)) break;
        if (goal.potential && !test_valid(player, goal)) {
            register_activation(player, goal, false);
            break;
        }
        pool.erase(pool.begin());
        activate(goal);
        if (first_abandon && like(player, goal, *first_abandon)) break;
    }
    // Pass 2: desire-weighted draws among the rest.
    while (keep.size() < limit && !pool.empty()) {
        std::vector<std::size_t> eligible;
        Real total{};
        for (std::size_t index = 0; index < pool.size(); ++index) {
            if (!proposable(player, player.functions[pool[index].function]) || like_kept(pool[index])) continue;
            eligible.push_back(index);
            total = to_single(total + pool[index].desire);
        }
        if (eligible.empty()) break;
        const Real draw = sync_.uniform(Real{}, total);
        std::size_t chosen = eligible.back();
        Real sum{};
        for (const std::size_t index : eligible) {
            sum = to_single(sum + pool[index].desire);
            if (draw < sum) {
                chosen = index;
                break;
            }
        }
        Goal goal = pool[chosen];
        pool.erase(pool.begin() + static_cast<std::ptrdiff_t>(chosen));
        if (goal.potential && !test_valid(player, goal)) {
            if (goal.active) finish(goal);
            register_activation(player, goal, false);
            continue;
        }
        activate(goal);
    }
    // What stays in the pool and was active is dropped.
    for (Goal& goal : pool) {
        if (goal.active) finish(goal);
    }
    for (Goal& goal : keep) kept_all.push_back(goal);
}

// ---- Planning (PL-40 to PL-44) --------------------------------------------------------------

core::Result<void> Engine::attach_plan(PlayerAi& player, Goal& goal, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    static_cast<void>(sequence);
    const PlanDef& definition = plans_[goal.potential->plan];
    const std::uint64_t instance = next_instance_++;
    if (auto created = scripts.create_instance(instance, definition.module); !created) {
        record(nullptr, player.player, "failed", definition.name + ": " + created.error().message);
        return created;
    }
    Plan plan;
    plan.id = next_plan_id_++;
    plan.instance = instance;
    plan.definition = goal.potential->plan;
    plan.goal = goal.id;
    plan.player = player.player;
    plan.target = goal.target;
    if (const Target* target = this->target(goal.target)) plan.target_object = target->object;
    plan.start_tick = static_cast<std::uint64_t>(frame_);
    const auto event = [&](ScriptEvent::Kind kind, std::string name) {
        ScriptEvent out;
        out.key = authoritative::EventKey{event_tick_, producer_foc_engine, instance, (*sequence_)++};
        out.target = instance;
        out.kind = kind;
        out.name = std::move(name);
        return out;
    };
    const auto assign = [&](std::string name, std::optional<Value> value) {
        ScriptEvent out = event(ScriptEvent::Kind::assign, std::move(name));
        out.parameter = std::move(value);
        events_.push_back(std::move(out));
    };
    events_.push_back(event(ScriptEvent::Kind::call, "Base_Definitions"));
    const Target* target = this->target(goal.target);
    assign("Target", target != nullptr && target->object != 0
            ? std::optional<Value>(Value{authoritative::Handle{handle_game_object, target->object}})
            : std::nullopt);
    assign("AITarget", target != nullptr ? std::optional<Value>(Value{authoritative::Handle{handle_ai_target, target->id}})
                                         : std::nullopt);
    std::int64_t thread = 0;
    for (std::size_t index = 0; index < definition.taskforces.size(); ++index) {
        const TaskForceDef& tf_def = definition.taskforces[index];
        auto thread_function = scripts.read_global(instance, tf_def.name + "_Thread");
        const bool has_thread = thread_function && !thread_function.value();
        if (!has_thread) continue; // PL-41: a TaskForce without its thread function is dropped
        TaskForce tf;
        tf.id = next_taskforce_id_++;
        tf.plan = plan.id;
        tf.definition = index;
        tf.name = tf_def.name;
        for (std::size_t unit = 0; unit < goal.potential->units.size(); ++unit) {
            if (goal.potential->taskforce_of_unit[unit] == index) tf.types.push_back(goal.potential->units[unit]);
        }
        tf.thread = thread++;
        assign(tf_def.name, Value{authoritative::Handle{handle_taskforce, tf.id}});
        plan.taskforces.push_back(tf.id);
        taskforces_.emplace(tf.id, std::move(tf));
    }
    assign("PlayerObject", Value{authoritative::Handle{handle_player, player.player}});
    for (const std::uint64_t tf_id : plan.taskforces) {
        events_.push_back(event(ScriptEvent::Kind::start_thread, taskforces_.at(tf_id).name + "_Thread"));
    }
    goal.plan = plan.id;
    plan_of_instance_.emplace(instance, plan.id);
    const std::uint64_t id = plan.id;
    running_.emplace(id, std::move(plan));
    record(&running_.at(id), player.player, "started",
        std::to_string(goal.potential->units.size()) + " units, desire " + show(goal.desire));
    return core::Result<void>::success();
}

// PL-42: every 0.1 s each plan's threads are pumped; a plan whose script exited or has no live
// thread is finished.
core::Result<void> Engine::service_plans(PlayerAi& player, authoritative::ScriptScheduler& scripts, std::uint64_t& sequence) {
    static_cast<void>(sequence);
    // PL-45: Purge_Goals abandons every other running plan of the player that the goal system may
    // remove, in plan order.
    std::vector<std::uint64_t> purged;
    for (auto& [id, plan] : running_) {
        if (plan.player != player.player || !plan.purge) continue;
        plan.purge = false;
        for (const auto& [other_id, other] : running_) {
            if (other.player == player.player && other_id != id && other.removable && !other.exited &&
                std::find(purged.begin(), purged.end(), other_id) == purged.end()) {
                purged.push_back(other_id);
            }
        }
    }
    for (const std::uint64_t id : purged) {
        const std::uint64_t goal_id = running_.at(id).goal;
        finish_plan(player, id, scripts, true);
        for (Goal& goal : player.active) {
            if (goal.id == goal_id) goal.finished = true;
        }
    }
    std::vector<std::uint64_t> finished;
    for (auto& [id, plan] : running_) {
        if (plan.player != player.player) continue;
        if (plan.exited) {
            finished.push_back(id);
            continue;
        }
        if (plan.start_tick < static_cast<std::uint64_t>(frame_)) {
            auto slots = scripts.thread_slots(plan.instance);
            bool live = false;
            if (slots) {
                for (const bool slot : slots.value()) live = live || slot;
            }
            if (!live) {
                finished.push_back(id);
                continue;
            }
        }
        for (const std::uint64_t tf_id : plan.taskforces) {
            if (auto found = taskforces_.find(tf_id); found != taskforces_.end()) found->second.damaged_pending = false;
        }
        ScriptEvent pump;
        pump.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, (*sequence_)++};
        pump.target = plan.instance;
        pump.kind = ScriptEvent::Kind::pump;
        events_.push_back(std::move(pump));
    }
    for (const std::uint64_t id : finished) {
        finish_plan(player, id, scripts);
        for (Goal& goal : player.active) {
            if (goal.plan == id) goal.finished = true;
        }
    }
    return core::Result<void>::success();
}

// PL-43: a plan ends: its outcome is learned, its units go back to the free store, its build
// tasks and blocks go and its script instance is removed. PL-45: an abandoned plan learns only its
// goal's outcome, with the plan's result so far.
void Engine::finish_plan(PlayerAi& player, std::uint64_t plan_id, authoritative::ScriptScheduler& scripts, const bool abandoned) {
    const auto found = running_.find(plan_id);
    if (found == running_.end()) return;
    Plan& plan = found->second;
    const PlanDef& definition = plans_[plan.definition];
    const Goal* goal = nullptr;
    for (const Goal& entry : player.active) {
        if (entry.id == plan.goal) goal = &entry;
    }
    if (goal != nullptr) {
        // LS-01: goal and plan outcomes.
        const GoalFunctionEntry& function = player.functions[goal->function];
        const Real duration = function.goal->tracking_duration;
        const std::int64_t expiry = duration < Real{} ? -1 : frame_ + truncate(to_single(duration * real(fps)));
        player.goal_outcomes[{function.goal_name, goal->target}].entries.emplace_back(plan.result, expiry);
        if (!abandoned) player.plan_outcomes[{plan.definition, goal->target}].entries.emplace_back(plan.result, expiry);
    }
    if (!abandoned) {
        auto& success = player.plan_success[plan.definition];
        success.first += plan.result ? 1 : 0;
        success.second += 1;
    }
    record(&plan, player.player, abandoned ? "abandoned" : "finished", plan.result ? "success" : "failure");
    for (const std::uint64_t tf_id : plan.taskforces) {
        const auto tf = taskforces_.find(tf_id);
        if (tf == taskforces_.end()) continue;
        for (const sim::EntityId unit : tf->second.units) player.assigned.erase(unit);
        taskforces_.erase(tf);
    }
    player.tasks.erase(std::remove_if(player.tasks.begin(), player.tasks.end(),
                           [&](const BuildTask& task) {
                               return std::find(plan.taskforces.begin(), plan.taskforces.end(), task.taskforce) != plan.taskforces.end();
                           }),
        player.tasks.end());
    for (auto iterator = blocks_.begin(); iterator != blocks_.end();) {
        if (iterator->second.instance == plan.instance) {
            iterator = blocks_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = player.reserved.begin(); iterator != player.reserved.end();) {
        if (iterator->second == plan.goal) {
            iterator = player.reserved.erase(iterator);
        } else {
            ++iterator;
        }
    }
    static_cast<void>(scripts.remove_instance(plan.instance));
    static_cast<void>(definition);
    plan_of_instance_.erase(plan.instance);
    running_.erase(found);
}

// EX-11: every 0.1 s each unfinished build task takes a free store object of its type, the one
// the plan's potential plan reserved first; a type the free store no longer holds fails.
void Engine::service_execution(PlayerAi& player) {
    std::set<std::uint64_t> touched;
    for (BuildTask& task : player.tasks) {
        if (task.finished) continue;
        auto tf = taskforces_.find(task.taskforce);
        if (tf == taskforces_.end()) {
            task.finished = task.failed = true;
            continue;
        }
        const Plan* plan = this->plan(tf->second.plan);
        const ViewUnit* chosen = nullptr;
        for (const ViewUnit& unit : host_->view->units) {
            if (unit.type != task.type || !in_freestore(unit, player.player)) continue;
            const auto reservation = player.reserved.find(unit.id);
            if (plan != nullptr && reservation != player.reserved.end() && reservation->second == plan->goal) {
                chosen = &unit;
                break;
            }
        }
        if (chosen == nullptr) {
            for (const ViewUnit& unit : host_->view->units) {
                if (unit.type != task.type || !in_freestore(unit, player.player) || player.reserved.contains(unit.id)) continue;
                chosen = &unit;
                break;
            }
        }
        touched.insert(task.block);
        if (chosen == nullptr) {
            task.finished = task.failed = true;
            continue;
        }
        task.object = chosen->id;
        task.finished = true;
        tf->second.units.push_back(chosen->id);
        tf->second.had_units = true;
        player.assigned[chosen->id] = tf->second.id;
    }
    for (const std::uint64_t id : touched) {
        auto block = blocks_.find(id);
        if (block == blocks_.end()) continue;
        bool done = true;
        bool any = false;
        for (const BuildTask& task : player.tasks) {
            if (task.block != id) continue;
            done = done && task.finished;
            any = any || (task.finished && !task.failed);
        }
        if (!done) continue;
        block->second.finished = true;
        const auto tf = taskforces_.find(block->second.taskforce);
        block->second.result = tf != taskforces_.end() && !tf->second.units.empty();
        if (tf != taskforces_.end()) {
            if (const Plan* plan = this->plan(tf->second.plan)) {
                record(plan, player.player, "produced", tf->second.name + " " + std::to_string(tf->second.units.size()) + " units"
                    + members_of(tf->second.units));
                if (!any && tf->second.units.empty()) emit_call(*plan, tf->second, "No_Units_Remaining", {});
            }
        }
    }
    std::erase_if(player.tasks, [](const BuildTask& task) { return task.finished; });
}

} // namespace eawr::script::foc::ai
