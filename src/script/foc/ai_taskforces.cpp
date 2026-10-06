#include "ai_engine_internal.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace eawr::script::foc::ai {

using namespace engine_internal;

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
    out.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, sequence_++};
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
    out.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, sequence_++};
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
            // WHZ-23/24, WAB-34: environmental signals already name a craft's container.
            if (host_->snapshot) {
                for (const auto& signal : host_->snapshot->events()) {
                    if (signal.kind != tactical::EventKind::ability_cancelled && signal.kind != tactical::EventKind::ability_ready) continue;
                    if (view.find(signal.unit) == nullptr || std::find(tf.units.begin(), tf.units.end(), signal.unit) == tf.units.end()) continue;
                    const auto kind = static_cast<tactical::AbilityKind>(signal.sequence);
                    const auto name = tactical::to_string(kind);
                    if (name == "NONE") continue;
                    ValueList arguments;
                    arguments.push_back(Value{authoritative::Handle{handle_game_object, signal.unit}});
                    // WAB-34: the stock plan's recovery branch compares the signal to "Turbo".
                    arguments.push_back(Value{std::string(kind == tactical::AbilityKind::turbo ? "Turbo" : name)});
                    emit_call(plan, tf, signal.kind == tactical::EventKind::ability_ready
                        ? "Unit_Ability_Ready" : "Unit_Ability_Cancelled", std::move(arguments));
                }
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
        if (block.finished || block.kind == Block::Kind::produce || block.kind == Block::Kind::reinforce) continue;
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
                if (static_cast<std::uint64_t>(frame_) < block.ordered_tick[mover] + 2 ||
                    unit->moving || unit->formation_moving) continue;
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
            // EX-31: squadron containers travel through their formation, without ship Motion.
            // FoC's movement-finished signal waits for that travel too.
            if (static_cast<std::uint64_t>(frame_) < ordered + 2 || unit->moving || unit->formation_moving) continue;
            arrived.push_back(mover);
            ValueList arguments;
            arguments.push_back(Value{authoritative::Handle{handle_game_object, mover}});
            emit_call(plan->second, tf->second, "Unit_Move_Finished", std::move(arguments));
        }
        for (const sim::EntityId mover : arrived) {
            block.movers.erase(std::remove(block.movers.begin(), block.movers.end(), mover), block.movers.end());
        }
        // EX-36: escorting a live object remains a blocking command after arrival,
        // including neutral build pads. WBP-40 waits for capture or construction.
        const bool escort_object = destination != nullptr && block.order == Block::Order::guard;
        if (block.movers.empty() && !escort_object && !(enemy_object && block.attack)) block.finished = true;
    }
}

} // namespace eawr::script::foc::ai
