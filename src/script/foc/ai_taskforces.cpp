#include "ai_engine_internal.hpp"
#include "galactic_ai_actions.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace eawr::script::foc::ai {

using namespace engine_internal;

std::optional<galactic_ai::MovementReach> Engine::galactic_movement_reach(std::uint64_t plan_id) const {
    const auto* retained = plan(plan_id); if (!retained) return {};
    const auto* player = player_ai(retained->player); if (!player || !player->galactic) return {};
    const Goal* goal{};
    for (const auto& row : player->active) if (row.id == retained->goal) goal = &row;
    if (!goal || goal->function >= player->functions.size()) return {};
    const auto& reach = player->functions[goal->function].goal->reachability;
    using Reach = galactic_ai::MovementReach;
    for (const auto& [name, value] : {std::pair{"FRIENDLY_ONLY", Reach::friendly_only},
        {"FRIENDLY_IGNORE_THREAT", Reach::friendly_ignore_threat}, {"ENEMY_DESTINATION", Reach::enemy_destination},
        {"ENEMY_UNDEFENDED", Reach::enemy_undefended}, {"SINGLE_HOP_DISCONNECTED", Reach::single_hop_disconnected}, {"ANY", Reach::any}})
        if (reach == name) return value;
    return {};
}

std::vector<std::uint64_t> Engine::galactic_movement_requests() const {
    std::vector<std::uint64_t> requests;
    for (const auto& [id, row] : ordered_runtime(&PlayerAi::blocks)) { const auto& block = *row; (void)id; if (block.galactic_request && block.kind == Block::Kind::move) requests.push_back(block.galactic_request); }
    for (const auto& player : players_) if (player.galactic)
        for (const auto& task : player.tasks) if (task.galactic_assembly_request) requests.push_back(task.galactic_assembly_request);
    return requests;
}
core::Result<void> Engine::validate_galactic_movement(std::uint64_t request,
    std::uint64_t plan_id, std::uint64_t taskforce_id, std::uint64_t block_id, bool registered) const {
    const auto invalid = [&] { return core::Result<void>::failure(detail::make_error(authoritative::codes::invalid_request,
        "native movement plan/taskforce/block association is invalid")); };
    // S9-early: each identity must lie inside its owner's allocated range.
    if (!request || !galactic_plan_issued(plan_id) || !galactic_taskforce_issued(taskforce_id)
        || !galactic_instance_issued(block_id >> 32)) return invalid();
    if (!registered) return core::Result<void>::success(); // retired wrappers keep historical actor IDs
    const auto* retained_plan = plan(plan_id); const auto* retained_force = taskforce(taskforce_id); const auto* retained_block = block(block_id);
    if (!retained_plan || !retained_force || !retained_block || retained_plan->exited
        || retained_force->plan != plan_id || retained_block->instance != retained_plan->instance
        || retained_block->taskforce != taskforce_id) return invalid();
    if (retained_block->kind == Block::Kind::produce) {
        const auto* player = player_ai(retained_plan->player);
        if (!player || !player->galactic || std::count_if(player->tasks.begin(), player->tasks.end(), [&](const auto& task) {
            return task.source == 0 && task.taskforce == taskforce_id && task.block == block_id && task.staging
                && task.galactic_assembly == BuildTask::GalacticAssembly::moving && !task.finished
                && task.galactic_assembly_request == request;
        }) != 1) return invalid();
    } else if (retained_block->kind != Block::Kind::move || retained_block->galactic_request != request) return invalid();
    return core::Result<void>::success();
}

// WGA-202/204; EGA-BX02 qualifies the stock override/default event vocabulary.
// Handler delivery stays explicitly deferred until the scheduler has a
// synchronous handler-only API. Do not service the frozen parent here.
core::Result<void> Engine::deliver_galactic_space_conflict(std::uint64_t request,
    std::uint64_t plan_id, std::uint64_t taskforce_id, std::uint64_t block_id, bool begin,
    authoritative::ScriptScheduler& scripts) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {29});
    if (auto valid = validate_galactic_movement(request, plan_id, taskforce_id, block_id, true); !valid) return valid;
    const auto& retained_plan = *plan(plan_id);
    const auto& retained_force = *taskforce(taskforce_id);
    const auto* player = player_ai(retained_plan.player);
    if (!player || !player->galactic) return core::Result<void>::failure(detail::make_error(
        authoritative::codes::invalid_request, "space conflict listener requires a retained galactic plan"));
    const auto slots = scripts.thread_slots(retained_plan.instance);
    if (!slots || retained_force.thread < 0 || static_cast<std::size_t>(retained_force.thread) >= slots.value().size()
        || !slots.value()[static_cast<std::size_t>(retained_force.thread)]) return core::Result<void>::success();
    const std::string_view event = begin ? "Space_Conflict_Begin" : "Space_Conflict_End";
    const auto defined = [&](const std::string& name) {
        const auto value = scripts.read_global(retained_plan.instance, name);
        return value && !value.value().has_value(); // same authored-handler convention as emit_call
    };
    std::string name = retained_force.name + "_" + std::string(event);
    if (!defined(name)) {
        name = "Default_" + std::string(event);
        if (!defined(name)) return core::Result<void>::success();
    }
    ScriptEvent call; call.key = {scripts.completed_tick() + 1, producer_foc_engine, retained_plan.instance, sequence_};
    call.target = retained_plan.instance; call.kind = ScriptEvent::Kind::call; call.name = std::move(name);
    call.arguments.push_back(Value{authoritative::Handle{handle_taskforce, retained_force.id}});
    // Retain in the native archive while the parent is frozen. Its next real
    // barrier assigns the admitted scheduler tick/global sequence before commit.
    events_.push_back(std::move(call));
    mark_native_state_changed(); ++sequence_;
    record(&retained_plan, retained_plan.player, "event", retained_force.name + "_" + std::string(event));
    return core::Result<void>::success();
}

// ---- Unit orders (EX-30 stand-ins: the tactical orders the M2 simulation has) -----------------

void Engine::issue_move(const Plan& plan, const ViewUnit& unit, const math::Vec3& destination) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {30});
    mark_native_state_changed();
    // U-05: a new destination replaces the positioning point move's retained object.
    if (auto* retained = positioning(unit.id))
        retained->target = sim::invalid_entity_id;
    authoritative::ScriptCommand command;
    command.issuer = plan.instance;
    command.verb = std::string(verb_move);
    command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(unit.owner))));
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, unit.id}});
    command.arguments.push_back(Value::number(fixed(destination.x)));
    command.arguments.push_back(Value::number(fixed(destination.y)));
    command.arguments.push_back(Value::number(fixed(destination.z)));
    queue_order(std::move(command));
}

void Engine::issue_order(const Plan& plan, const ViewUnit& unit, const std::string_view verb, const sim::EntityId target,
    const math::Vec3& destination) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {30});
    mark_native_state_changed();
    if (auto* retained = positioning(unit.id))
        retained->target = sim::invalid_entity_id;
    authoritative::ScriptCommand command;
    command.issuer = plan.instance;
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
    queue_order(std::move(command));
}

void Engine::issue_attack(const Plan& plan, const ViewUnit& unit, sim::EntityId target) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {30});
    mark_native_state_changed();
    if (auto* retained = positioning(unit.id))
        retained->target = sim::invalid_entity_id;
    authoritative::ScriptCommand command;
    command.issuer = plan.instance;
    command.verb = std::string(verb_attack);
    command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(unit.owner))));
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, unit.id}});
    command.arguments.push_back(Value{authoritative::Handle{handle_game_object, target}});
    queue_order(std::move(command));
}

// EX-30: the move list is the TaskForce's live members that move; with an object destination
// and an attack, each squadron or TARGETING member attacks it (EX-31: unless refused) and any
// other member moves to it; an attack-move or guard orders each to the object or point
// (FH-40, space-orders OR-12, OR-14); otherwise each moves to the destination point (an object
// destination's position at the order).
void Engine::order_block(Block& block, const TaskForce& taskforce, const Plan& plan) {
    const auto native_record_write = native_record_scope();
    native_record_changed(native_all_players, 23, block.id);
    mark_native_state_changed();
    const WorldView& view = *host_->view;
    const ViewUnit* destination = block.destination_object != 0 ? view.find(block.destination_object) : nullptr;
    if (block.destination_object != 0 && destination == nullptr) {
        block.finished = true;
        return;
    }
    // EX-31 (#3087): an attack on an object hands squadrons and TARGETING movers the attack order.
    // Attack admission refuses a non-hostile target (WHZ-51): a refused mover gets no order and
    // leaves the block silently. The other movers move to the object.
    const bool attack = destination != nullptr && block.order == Block::Order::attack;
    const bool hostile = destination != nullptr && destination->owner != 0 && !host_->neutral(destination->owner)
        && !host_->allied(destination->owner, plan.player);
    bool refused = false;
    for (const sim::EntityId member : taskforce.units) {
        const ViewUnit* unit = view.find(member);
        if (unit == nullptr) continue;
        const AiType* type = host_->type(unit->type);
        if (type == nullptr || !type->locomotor) continue;
        const bool attacker = type->squadron || type->targeting;
        if (attack && attacker && !hostile) {
            refused = true;
            continue;
        }
        block.movers.push_back(member);
        block.ordered_tick[member] = static_cast<std::uint64_t>(frame_);
        if (attack && attacker) {
            issue_attack(plan, *unit, destination->id);
        } else if (block.order == Block::Order::attack_move || block.order == Block::Order::guard) {
            issue_order(plan, *unit, block.order == Block::Order::attack_move ? verb_attack_move : verb_guard,
                destination != nullptr ? destination->id : sim::invalid_entity_id, block.destination);
        } else {
            issue_move(plan, *unit, destination != nullptr ? destination->position : block.destination);
        }
    }
    // With every mover refused no movement ever ends, so the block waits (EX-31).
    if (block.movers.empty() && refused) block.held_owner = static_cast<std::int64_t>(destination->owner);
    else if (block.movers.empty()) block.finished = true;
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
    const auto native_record_write = native_record_scope();
    native_record_changed(native_all_players, 23, block.id);
    mark_native_state_changed();
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
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {29});
    mark_native_state_changed();
    std::string name = taskforce.name + "_" + std::string(event);
    if (!defines_function(plan.instance, name)) {
        name = "Default_" + std::string(event);
        if (!defines_function(plan.instance, name)) return;
    }
    ScriptEvent out;
    out.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, 0};
    out.target = plan.instance;
    out.kind = ScriptEvent::Kind::call;
    out.name = std::move(name);
    out.arguments.push_back(Value{authoritative::Handle{handle_taskforce, taskforce.id}});
    for (Value& argument : arguments) out.arguments.push_back(std::move(argument));
    queue_event(std::move(out));
    record(&plan, plan.player, "event", taskforce.name + "_" + std::string(event));
}

// EX-41: events queued on the TaskForce's thread (Unit_Damaged, Unit_Diversion_Finished).
void Engine::emit_thread_event(const Plan& plan, const TaskForce& taskforce, std::string_view event, ValueList arguments) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {29});
    mark_native_state_changed();
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
    out.key = authoritative::EventKey{event_tick_, producer_foc_engine, plan.instance, 0};
    out.target = plan.instance;
    out.kind = ScriptEvent::Kind::thread_signal;
    out.name = std::move(name);
    out.thread = taskforce.thread;
    out.parameter = Value{std::move(parameters)};
    queue_event(std::move(out));
}

// A global the engine cannot read as a value (read_global gives none) is a function
// or a table; plans define their handlers as functions.
bool Engine::defines_function(std::uint64_t instance, const std::string& name) const {
    auto value = scripts_->read_global(instance, name);
    return value && !value.value().has_value();
}

// WGA-163/314: task return is distinct from an owner-change or creation offer.
void Engine::return_galactic_actor(const TaskForce& force, sim::EntityId actor) {
    const auto* running = plan(force.plan);
    auto* player = running ? player_ai(running->player) : nullptr;
    auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
    if (!player || !player->galactic || !actions || !actions->qualified_freestore_available()) return;
    auto& store = actions->qualified_freestore();
    if (!store.facts(actor)) return; // Actual loss already retired its actor facts.
    const Goal* goal{};
    for (const auto* rows : {&player->active, &player->proposed})
        for (const auto& row : *rows) if (row.id == running->goal) goal = &row;
    if (goal && goal->potential) store.release_object(player->player, actor, goal->potential->id);
    else {
        const auto offered = store.offer(player->player, actor);
        if (!offered) throw std::runtime_error(offered.error().message);
    }
}

void Engine::remove_member(TaskForce& taskforce, sim::EntityId unit) {
    const auto native_record_write = native_record_scope();
    native_record_changed(native_all_players, 21, taskforce.id);
    if (std::find(taskforce.units.begin(), taskforce.units.end(), unit) != taskforce.units.end())
        return_galactic_actor(taskforce, unit);
    mark_native_state_changed();
    taskforce.units.erase(std::remove(taskforce.units.begin(), taskforce.units.end(), unit), taskforce.units.end());
    taskforce.last_target.erase(unit);
}

// DT-01, DT-02: every 30 frames each damaged object's attackers lose (maximum hull + maximum
// shield) x AI_SpaceThreatDecayStep (engine default 1, FoC data 0.05) of threat and leave at 0; each hit
// adds its projectile's damage to the attacker's threat (a craft's hit counts for its squadron).
void Engine::track_damage(const World& world) {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {25, 26});
    mark_native_state_changed();
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
    for (const tactical::CombatEvent& event : world.combat_events()) {
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

namespace {

// EX-46: the target a member's Target_In_Range reports. A squadron container has no weapon state;
// the debug build raises the signal from the container when the team leader (the first live craft)
// reaches its firing state, and answers with the formation's enemy attack target. Here the leader
// must hold that target and have it within its weapon reach in the plane.
sim::EntityId in_range_target(const Host& host, const ViewUnit& unit) {
    if (!unit.container) return unit.attack_target;
    const WorldView& view = *host.view;
    const ViewUnit* leader = nullptr;
    for (const sim::EntityId member : unit.members) {
        if ((leader = view.find(member)) != nullptr) break;
    }
    const ViewUnit* target = view.find(unit.formation_target);
    if (leader == nullptr || target == nullptr || leader->attack_target != target->id) return sim::invalid_entity_id;
    if (host.allied(unit.owner, target->owner)) return sim::invalid_entity_id;
    const AiType* type = host.type(leader->type);
    if (type == nullptr) return sim::invalid_entity_id;
    Real reach = type->max_attack_distance;
    for (const AiWeapon& weapon : type->weapons) {
        const bool destroyed = weapon.hardpoint < leader->hardpoint_destroyed.size() && leader->hardpoint_destroyed[weapon.hardpoint];
        if (weapon.destroyable && destroyed) continue;
        if (reach < weapon.range) reach = weapon.range;
    }
    const Real distance_squared = engine_internal::xy_distance_squared(leader->position,
        engine_internal::fixed(target->position.x), engine_internal::fixed(target->position.y));
    return distance_squared <= to_single(reach * reach) ? target->id : sim::invalid_entity_id;
}

} // namespace

// ObjectScript_PointDefense (WLD-10, AI-32). The script updates once a second (its ServiceRate); the
// remake spreads the objects over the second by entity ID (the phase is unverified). For a non-human
// owner it switches LASER_DEFENSE on when the ability is ready and the deadly enemy of the object
// fires a projectile type that laser defence affects. A human owner's autofire path never runs: the
// ability does not support autofire.
void Engine::service_point_defence() {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {29, 30});
    mark_native_state_changed();
    const WorldView& view = *host_->view;
    for (const ViewUnit& unit : view.units) {
        const AiType* type = host_->type(unit.type);
        if (type == nullptr || !type->point_defence_script || host_->object_script_omitted(*type)
            || static_cast<std::uint64_t>(frame_) % 30 != unit.id % 30) continue;
        if (player_ai(unit.owner) == nullptr) continue;
        const auto ability = detail::ability_view(*host_, unit, tactical::AbilityKind::laser_defense);
        if (!ability.has || !ability.ready || ability.active) continue;
        const sim::EntityId deadly = deadly_enemy({unit.id});
        const ViewUnit* enemy = deadly != 0 ? view.find(deadly) : nullptr;
        if (enemy == nullptr) continue;
        const auto projectiles = detail::projectile_types_of(*host_, *enemy);
        if (std::none_of(projectiles.begin(), projectiles.end(), [&](const tactical::TypeId projectile) {
                return detail::projectile_in_categories(*host_, projectile, detail::laser_defense_categories);
            })) continue;
        authoritative::ScriptCommand command;
        command.verb = std::string(verb_ability);
        command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(unit.owner))));
        command.arguments.push_back(Value{authoritative::Handle{handle_game_object, unit.id}});
        command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(tactical::AbilityKind::laser_defense))));
        command.arguments.push_back(Value::number(numeric::LuaNumber(static_cast<std::int64_t>(tactical::AbilityAction::activate))));
        queue_order(std::move(command));
        record(nullptr, unit.owner, "ability", "point defence unit " + std::to_string(unit.id));
    }
}

ThreatCenter local_threat_center(const std::vector<const ViewUnit*>& objects, Real radius_squared,
    const std::function<Real(const ViewUnit&)>& power, const std::function<Real(Real, Real)>& draw) {
    struct Center { Real x{}, y{}; };
    if (objects.empty()) return {};
    const std::size_t count = std::min<std::size_t>((objects.size() + 3) / 4, 10);
    std::vector<Center> centers(count);
    if (count == 1) {
        for (const ViewUnit* object : objects) {
            centers[0].x = to_single(centers[0].x + fixed(object->position.x));
            centers[0].y = to_single(centers[0].y + fixed(object->position.y));
        }
        centers[0].x = to_single(centers[0].x / real(static_cast<std::int64_t>(objects.size())));
        centers[0].y = to_single(centers[0].y / real(static_cast<std::int64_t>(objects.size())));
    } else {
        Real min_x = fixed(objects.front()->position.x), max_x = min_x;
        Real min_y = fixed(objects.front()->position.y), max_y = min_y;
        for (const ViewUnit* object : objects) {
            min_x = std::min(min_x, fixed(object->position.x)); max_x = std::max(max_x, fixed(object->position.x));
            min_y = std::min(min_y, fixed(object->position.y)); max_y = std::max(max_y, fixed(object->position.y));
        }
        for (Center& center : centers) {
            const auto y = draw(min_y, max_y);
            const auto x = draw(min_x, max_x);
            center = {x, y};
        }
        for (std::size_t round = 0; round < 5; ++round) {
            std::vector<Center> sums(count);
            std::vector<std::int64_t> members(count);
            for (const ViewUnit* object : objects) {
                std::size_t nearest = 0;
                Real distance = xy_distance_squared(object->position, centers[0].x, centers[0].y);
                for (std::size_t index = 1; index < count; ++index) {
                    const Real candidate = xy_distance_squared(object->position, centers[index].x, centers[index].y);
                    if (candidate < distance) { nearest = index; distance = candidate; }
                }
                sums[nearest].x = to_single(sums[nearest].x + fixed(object->position.x));
                sums[nearest].y = to_single(sums[nearest].y + fixed(object->position.y));
                ++members[nearest];
            }
            for (std::size_t index = 0; index < count; ++index) if (members[index] != 0) {
                centers[index] = {to_single(sums[index].x / real(members[index])), to_single(sums[index].y / real(members[index]))};
            }
        }
    }
    ThreatCenter best;
    for (const Center& center : centers) {
        Real threat{};
        for (const ViewUnit* object : objects) {
            if (xy_distance_squared(object->position, center.x, center.y) <= radius_squared)
                threat = to_single(threat + power(*object));
        }
        if (threat >= best.threat) best = {center.x, center.y, threat};
    }
    return best;
}

// WHE-67/68: the IG-2000 object script collects nearby shielded enemies (or its deadly enemy),
// then casts at the strongest local threat center. Like AI-32, entity IDs spread the one-second
// service over frames; the original object's relative service phase remains unverified.
void Engine::service_corrupt_systems() {
    const auto native_record_write = native_record_scope();
    native_records_changed(0, {29, 30});
    mark_native_state_changed();
    const WorldView& view = *host_->view;
    for (const ViewUnit& source : view.units) {
        const AiType* type = host_->type(source.type);
        if (type == nullptr || !type->corrupt_systems_script || host_->object_script_omitted(*type)) continue;
        auto* player = player_ai(source.owner);
        if (player == nullptr || host_->neutral(source.owner)
            || static_cast<std::uint64_t>(frame_) % 30 != source.id % 30) continue;
        const auto ability = detail::ability_view(*host_, source, tactical::AbilityKind::corrupt_systems);
        if (!ability.has || !ability.ready) continue;
        const sim::EntityId deadly = deadly_enemy({source.id});
        std::vector<const ViewUnit*> nearby;
        for (const ViewUnit& enemy : view.units) {
            if (host_->allied(source.owner, enemy.owner) || host_->neutral(enemy.owner) || enemy.id == source.id
                || (enemy.hull && *enemy.hull <= math::Fixed{})
                || (enemy.shield <= Real{} && enemy.id != deadly)) continue;
            if (xy_distance_squared(enemy.position, fixed(source.position.x), fixed(source.position.y)) > real(700 * 700)) continue;
            nearby.push_back(&enemy);
        }
        if (nearby.empty()) continue;
        // WHE-68 / GFS-A24: coordinate draws are ordered Y then X in the owner's stream.
        const ThreatCenter destination = local_threat_center(nearby, real(300 * 300),
            [&](const ViewUnit& enemy) { return power(enemy); },
            [&](Real low, Real high) { return stream(*player).uniform(low, high); });
        if (destination.threat <= real(1000)) continue;
        authoritative::ScriptCommand command;
        command.verb = std::string(verb_ability);
        command.arguments = {Value::number(real(static_cast<std::int64_t>(source.owner))),
            Value{authoritative::Handle{handle_game_object, source.id}},
            Value::number(real(static_cast<std::int64_t>(tactical::AbilityKind::corrupt_systems))),
            Value::number(real(static_cast<std::int64_t>(tactical::AbilityAction::activate))),
            Value::number(destination.x), Value::number(destination.y), Value::number(Real{})};
        queue_order(std::move(command));
        record(nullptr, source.owner, "ability", "corrupt systems unit " + std::to_string(source.id));
    }
}

// EX-42 to EX-45: member deaths, No_Units_Remaining, Target_In_Range and Unit_Damaged, and the
// plan target's destruction, in plan then TaskForce order.
void Engine::service_taskforce_events() {
    const auto native_record_write = native_record_scope();
    mark_native_state_changed();
    const WorldView& view = *host_->view;
    for (auto& [plan_id, row] : ordered_runtime(&PlayerAi::running)) {
        auto& plan = *row;
        auto* owner = player_ai(plan.player);
        if (!owner) continue;
        auto& taskforces_ = owner->taskforces;
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
                if (PlayerAi* player = player_ai(plan.player)) {
                    native_records_changed(player->player, {14}); player->assigned.erase(unit);
                }
                emit_call(plan, tf, "Unit_Destroyed", {});
            }
            if (!dead.empty() && tf.units.empty()) {
                bool pending = false;
                if (const PlayerAi* player = player_ai(plan.player)) {
                    for (const BuildTask& task : player->tasks) pending = pending || (task.taskforce == tf.id && !task.finished);
                }
                if (!pending) emit_call(plan, tf, "No_Units_Remaining", {});
            }
            // Galactic roster deaths/no-units above use the completed persistent
            // identities. Tactical target-range and ability signals have no parent analogue.
            if (host_->ai_world && host_->ai_world->goal_mode() == "GALACTIC") continue;
            for (const sim::EntityId member : tf.units) {
                const ViewUnit* unit = view.find(member);
                if (unit == nullptr) continue;
                const sim::EntityId target = in_range_target(*host_, *unit);
                sim::EntityId& last = tf.last_target[member];
                if (target != 0 && target != last && view.find(target) != nullptr) {
                    ValueList arguments;
                    arguments.push_back(Value{authoritative::Handle{handle_game_object, member}});
                    arguments.push_back(Value{authoritative::Handle{handle_game_object, target}});
                    emit_call(plan, tf, "Target_In_Range", std::move(arguments));
                }
                native_record_changed(plan.player, 21, tf_id);
                last = target;
            }
            // WHZ-23/24, WAB-34: environmental signals already name a craft's container.
            if (host_->ai_world) {
                for (const auto& signal : host_->ai_world->events()) {
                    // WSW-34/37: the retained source's online signal drives Empire retries.
                    if (signal.kind == tactical::EventKind::special_weapon_ready) {
                        if (view.find(signal.unit) != nullptr && std::find(tf.units.begin(), tf.units.end(), signal.unit) != tf.units.end())
                            emit_call(plan, tf, "Special_Weapon_Online", {Value{authoritative::Handle{handle_game_object, signal.unit}}});
                        continue;
                    }
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
            native_record_changed(plan.player, 22, plan_id);
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
    for (auto& [plan_id, row] : ordered_runtime(&PlayerAi::running)) {
        auto& plan = *row;
        auto* owner = player_ai(plan.player);
        if (!owner) continue;
        auto& taskforces_ = owner->taskforces;
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
                native_record_changed(plan.player, 21, tf_id);
                tf.damaged_pending = true;
                break;
            }
        }
    }
}

// WTA-28: of the four boxes of half-size `half` around the target point, one per quadrant, the one
// with the least enemy force for `player` (PG-05, unnormalised); a tie keeps the first. The ideal
// position is its centre at height 0. Project choices (walk tactical-ai, G-03): the quadrants are
// tried in the order +x+y, -x+y, -x-y, +x-y, and the target point is the target's position (the
// AI's view has no hardpoint positions, so an aimed hardpoint is not used).
math::Vec3 Engine::ideal_attack_position(const ViewUnit& target, const Real half, const tactical::PlayerId player) const {
    static constexpr std::array<std::array<std::int64_t, 2>, 4> quadrants{{{1, 1}, {-1, 1}, {-1, -1}, {1, -1}}};
    const Real x = fixed(target.position.x);
    const Real y = fixed(target.position.y);
    const Real size = to_single(half * real(2));
    std::optional<Real> least;
    Real best_x = x;
    Real best_y = y;
    for (const auto& [sx, sy] : quadrants) {
        const Real cx = to_single(x + real(sx) * half);
        const Real cy = to_single(y + real(sy) * half);
        const Rect box{to_single(cx - half), to_single(cy - half), size, size};
        const Real force = grid_.force(*host_, *host_->view, box, ~std::uint64_t{0}, player, false, Real{}, 0);
        if (!least || force < *least) {
            least = force;
            best_x = cx;
            best_y = cy;
        }
    }
    math::Vec3 ideal{};
    if (auto value = numeric::to_fixed(best_x)) ideal.x = value.value();
    if (auto value = numeric::to_fixed(best_y)) ideal.y = value.value();
    return ideal;
}

// WTA-26, WTA-27: every 150 frames a flagged unit with a locomotor whose formation has an attack
// target, other than a team attacking a team, is positioned. Farther than its attack distance
// from the ideal position (WTA-28) it moves there as an attack move; otherwise it attacks the
// target. The remake orders each unit on its own, so the split from the formation is implicit.
// Project choices (walk tactical-ai, G-03): the distance compared is the unit's planar distance
// to the ideal position; a team's attack distance is its first craft type's (M2 squadrons have
// one craft type); a type without an attack distance is not positioned; while a unit holds the
// point attack move of its last positioning, that positioning's target stays its formation's
// attack target (the remake's point order keeps no object); an attack the unit already has is
// not ordered again.
void Engine::service_attack_positioning() {
    const WorldView& view = *host_->view;
    for (const auto& [id, row] : ordered_runtime(&PlayerAi::positioning)) {
        const ViewUnit* unit = view.find(id);
        if (unit == nullptr) {
            for (auto& owner : players_) owner.positioning.erase(id);
            continue;
        }
        AttackPositioning& state = *row;
        if (frame_ < state.next) continue;
        const AiType* type = host_->type(unit->type);
        if (type == nullptr || !type->locomotor) continue;
        // The formation's attack target: an attack, or an attack move on an object (U-05); a
        // squadron's formation target; else the target of its last positioning move.
        sim::EntityId target_id{};
        const bool attacking = unit->order == tactical::OrderKind::attack || unit->order == tactical::OrderKind::attack_move;
        if (attacking && unit->order_target != sim::invalid_entity_id) {
            target_id = unit->order_target;
        } else if (unit->container && unit->formation_target != sim::invalid_entity_id) {
            target_id = unit->formation_target;
        } else if (unit->order == tactical::OrderKind::attack_move) {
            target_id = state.target;
        }
        state.target = sim::invalid_entity_id;
        const ViewUnit* target = target_id != sim::invalid_entity_id && target_id != unit->id ? view.find(target_id) : nullptr;
        if (target == nullptr || (unit->container && (target->container || target->craft))) continue;
        // U-05/WHZ-51: expose an object only while hostile, without erasing the retained
        // destination. A later ownership change can expose the same live object again.
        if (target->owner == 0 || host_->neutral(unit->owner) || host_->neutral(target->owner)
            || host_->allied(unit->owner, target->owner)) {
            state.target = target_id;
            continue;
        }
        // FC-20: no formation attack target leaves the expired deadline untouched.
        state.next = frame_ + attack_positioning_frames;
        Real distance = type->max_attack_distance;
        if (type->squadron) {
            if (const AiType* craft = host_->type(type->squadron_unit)) distance = craft->max_attack_distance;
        }
        if (!(Real{} < distance)) continue;
        Real extent{};
        if (const AiType* target_type = host_->type(target->type)) {
            extent = square_root(to_single(target_type->bounds_half_x * target_type->bounds_half_x
                + target_type->bounds_half_y * target_type->bounds_half_y));
        }
        const Real reach = to_single(distance / real(2));
        const Real half = extent < reach ? extent : reach;
        const math::Vec3 ideal = ideal_attack_position(*target, half, unit->owner);
        Plan issuer;
        issuer.instance = state.instance;
        issuer.player = unit->owner;
        const Plan* source = plan_of_instance(state.instance);
        if (to_single(distance * distance) < xy_distance_squared(unit->position, fixed(ideal.x), fixed(ideal.y))) {
            issue_order(issuer, *unit, verb_attack_move, sim::invalid_entity_id, ideal);
            state.target = target->id;
            record(source, unit->owner, "attack_position", "unit " + std::to_string(unit->id) + " target "
                + std::to_string(target->id) + " at " + show(fixed(ideal.x)) + " " + show(fixed(ideal.y)));
        } else if (unit->order != tactical::OrderKind::attack || unit->order_target != target->id) {
            issue_attack(issuer, *unit, target->id);
            record(source, unit->owner, "attack_position", "unit " + std::to_string(unit->id) + " attacks "
                + std::to_string(target->id));
        }
    }
}

// EX-31 to EX-34: movement blocks finish when every mover has stopped (a point, or an object
// that is not a visible enemy), when an attacked object dies, or when the TaskForce is empty.
void Engine::service_blocks() {
    const auto native_record_write = native_record_scope();
    mark_native_state_changed();
    const WorldView& view = *host_->view;
    auto taskforces_ = all_taskforces();
    auto running_ = all_plans();
    for (auto& [id, row] : ordered_runtime(&PlayerAi::blocks)) {
        auto& block = *row;
        if (block.kind == Block::Kind::galactic_invade && block.galactic_request) {
            const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host_->galactic_execution.get());
            const auto* invasion = actions ? actions->invasion_status(block.galactic_request) : nullptr;
            if (!invasion) throw std::runtime_error("invasion block lost its retained explicit queue owner");
            native_record_changed(native_all_players, 23, id);
            block.finished = invasion->state == sim::galactic::GalacticSession::NativeInvasion::State::completed;
            block.result = invasion->success; continue;
        }
        if ((host_->ai_world && host_->ai_world->goal_mode() == "GALACTIC" && block.kind == Block::Kind::move)
            || block.galactic_request || block.kind == Block::Kind::galactic_land
            || block.kind == Block::Kind::galactic_launch || block.kind == Block::Kind::galactic_form) {
            // Native movement advances only when Lua stages its completion poll.
            // Tactical Motion flags and two-frame delays cannot finish it.
            if (block.galactic_request) {
                auto* actions = dynamic_cast<galactic_ai::NativeActions*>(host_->galactic_execution.get());
                if (!actions || !actions->movements().contains(block.galactic_request))
                    throw std::runtime_error("galactic block requires its retained native request owner");
                const auto& status = actions->movements().at(block.galactic_request);
                if (block.finished != status.finished || block.galactic_actor != status.actor) {
                    native_record_changed(native_all_players, 23, id); mark_native_state_changed();
                }
                block.finished = status.finished; block.galactic_actor = status.actor;
            }
            continue;
        }
        if (block.finished || block.kind == Block::Kind::produce || block.kind == Block::Kind::reinforce) continue;
        native_record_changed(native_all_players, 23, id);
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
        if (block.held_owner >= 0) {
            // EX-31 (#3087): every mover was refused; an ownership change to a non-enemy ends the wait.
            if (static_cast<std::int64_t>(destination->owner) != block.held_owner && !enemy_object) block.finished = true;
            continue;
        }
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
