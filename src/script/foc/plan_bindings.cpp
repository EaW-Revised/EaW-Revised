// The engine APIs of the FoC space plans (#449, docs/behaviour/foc-tactical-ai.md "Plans and
// TaskForces"): TaskForce objects, blocking objects, AI targets and the global functions the
// plans and their libraries call. Bindings run during the partitioned script service: they
// read the host and the engine and only stage commands; the engine applies them after the
// service (ai::Engine::after_service).

#include "ai_engine.hpp"

#include <algorithm>
#include <limits>

namespace eawr::script::foc::detail {
namespace {

using ai::Engine;
using ai::Real;
using HostPtr = std::shared_ptr<Host>;

std::uint64_t names_to_bits(const Host& host, const std::string& text, bool& ok);

const ai::TaskForce* taskforce_of(const Host& host, const Value& value) {
    const Handle* found = as_handle(value, ai::handle_taskforce);
    if (found == nullptr || host.engine == nullptr) return nullptr;
    return host.engine->taskforce(found->id);
}

const ai::Target* target_of(const Host& host, const Value& value) {
    const Handle* found = as_handle(value, ai::handle_ai_target);
    if (found == nullptr || host.engine == nullptr) return nullptr;
    return host.engine->target(found->id);
}

// EX-50: a TaskForce's position is the average position of its live members, 0 without any.
math::Vec3 taskforce_position(const Host& host, const ai::TaskForce& taskforce) {
    Real x{}, y{}, z{};
    std::int64_t count = 0;
    for (const sim::EntityId member : taskforce.units) {
        const ViewUnit* unit = host.view->find(member);
        if (unit == nullptr) continue;
        x = x + numeric::from_fixed(unit->position.x);
        y = y + numeric::from_fixed(unit->position.y);
        z = z + numeric::from_fixed(unit->position.z);
        ++count;
    }
    math::Vec3 out{};
    if (count == 0) return out;
    const Real n = ai::real(count);
    if (auto value = numeric::to_fixed(ai::to_single(x / n))) out.x = value.value();
    if (auto value = numeric::to_fixed(ai::to_single(y / n))) out.y = value.value();
    if (auto value = numeric::to_fixed(ai::to_single(z / n))) out.z = value.value();
    return out;
}

math::Vec3 point(Real x, Real y) {
    math::Vec3 out{};
    if (auto value = numeric::to_fixed(x)) out.x = value.value();
    if (auto value = numeric::to_fixed(y)) out.y = value.value();
    return out;
}

// EX-51: a position argument is a game object, an AI target (its object, or a region's centre), a
// TaskForce (its position) or a position.
std::optional<math::Vec3> extract_position(const Host& host, const Value& value) {
    if (const ai::TaskForce* taskforce = taskforce_of(host, value)) return taskforce_position(host, *taskforce);
    if (const ai::Target* target = target_of(host, value)) {
        if (target->object == 0) return point(target->x, target->y);
        const ViewUnit* unit = host.view->find(target->object);
        if (unit == nullptr) return std::nullopt;
        return unit->position;
    }
    return position_of(host, value);
}

Value list_of(std::vector<Value> values) { return Value{std::move(values)}; }

void stage(BindingContext& context, std::string operation, std::vector<Value> rest) {
    ValueList arguments;
    arguments.push_back(Value::text(std::move(operation)));
    for (Value& value : rest) arguments.push_back(std::move(value));
    context.issue_command(std::string(ai::verb_ai), std::move(arguments));
}

// The movers of a TaskForce order (EX-30): its live members that move.
bool has_movers(const Host& host, const ai::TaskForce& taskforce) {
    for (const sim::EntityId member : taskforce.units) {
        const ViewUnit* unit = host.view->find(member);
        if (unit == nullptr) continue;
        const AiType* type = host.type(unit->type);
        if (type != nullptr && type->locomotor) return true;
    }
    return false;
}

// EX-30: Move_To, Attack_Move, Attack_Target, Guard_Target and Explore_Area stage a movement
// block; the returned block is nil when the order has nothing to do.
Binding taskforce_move(HostPtr host, std::string kind, bool position_only) {
    return [host, kind, position_only](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        if (arguments.size() < 2) return fail("TaskForce order: no destination");
        Value destination;
        const ViewUnit* object = nullptr;
        if (as_handle(arguments[1], ai::handle_ai_target) != nullptr) {
            const ai::Target* target = target_of(*host, arguments[1]);
            if (target == nullptr) return none(); // EX-32: an expired target returns nil
            if (target->object != 0) {
                object = host->view->find(target->object);
                if (object == nullptr) return none(); // the target is already dead
            } else {
                destination = position_value(point(target->x, target->y));
            }
        } else if (as_handle(arguments[1], handle_game_object) != nullptr) {
            object = live_object(*host, arguments[1]);
            if (object == nullptr) return none();
        } else if (kind == "guard" && taskforce_of(*host, arguments[1]) != nullptr) {
            // EX-32: guard follows a random member, rather than the force's centroid.
            const auto* guarded = taskforce_of(*host, arguments[1]);
            if (guarded->units.empty()) return none();
            const auto index = context.random().next_below(guarded->units.size());
            object = host->view->find(guarded->units[static_cast<std::size_t>(index)]);
            if (object == nullptr) return none();
        } else if (const auto position = (kind == "guard" || kind == "attack") ? std::optional<math::Vec3>{}
                : extract_position(*host, arguments[1])) {
            destination = position_value(*position);
        } else {
            std::string description = "nil";
            if (const auto* value = std::get_if<Handle>(&arguments[1].data)) {
                description = "handle kind=" + std::to_string(value->kind) + " id=" + std::to_string(value->id);
            } else if (std::holds_alternative<std::vector<Value>>(arguments[1].data)) description = "position/table";
            else if (std::holds_alternative<LuaNumber>(arguments[1].data)) description = "number";
            else if (std::holds_alternative<std::string>(arguments[1].data)) description = "string";
            else if (std::holds_alternative<bool>(arguments[1].data)) description = "boolean";
            const auto* plan = host->engine->plan_of_instance(context.instance());
            if (plan != nullptr && plan->definition < host->engine->plans().size())
                description += " plan=" + host->engine->plans()[plan->definition].name;
            return fail("TaskForce order: parameter 1 is not a valid destination (" + description + ")");
        }
        if (object != nullptr) {
            destination = position_only ? position_value(object->position) : handle(handle_game_object, object->id);
        }
        if (!has_movers(*host, *taskforce)) return none();
        const std::uint64_t sequence = context.command_sequence();
        stage(context, "move", {handle(ai::handle_taskforce, taskforce->id), Value::text(kind), std::move(destination)});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    };
}

void register_taskforce(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto method = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(ai::handle_taskforce, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // EX-10 Produce_Force: a block over one build task per selected unit.
    method("Produce_Force", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        const std::uint64_t sequence = context.command_sequence();
        stage(context, "produce", {handle(ai::handle_taskforce, taskforce->id)});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    });
    method("Attack_Move", taskforce_move(host, "attack_move", false));
    method("Build_All", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        const auto sequence = context.command_sequence();
        stage(context, "produce", {handle(ai::handle_taskforce, taskforce->id)});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    });
    method("Get_Reserved_Build_Pads", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* taskforce = taskforce_of(*host, arguments[0]);
        const auto* plan = taskforce != nullptr && host->engine != nullptr ? host->engine->plan(taskforce->plan) : nullptr;
        std::vector<Value> pads;
        if (plan != nullptr) for (const auto pad : plan->reserved_pads) pads.push_back(handle(handle_game_object, pad));
        return one(Value{std::move(pads)});
    });
    method("Build", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* taskforce = taskforce_of(*host, arguments[0]);
        const auto* plan = taskforce != nullptr && host->engine != nullptr ? host->engine->plan(taskforce->plan) : nullptr;
        if (plan == nullptr || arguments.size() != 3 || as_text(arguments[1]) == nullptr) return none();
        const auto type = host->types_by_name.find(upper(*as_text(arguments[1])));
        const auto* pad = as_handle(arguments[2], handle_game_object);
        if (type == host->types_by_name.end() || pad == nullptr
            || std::find(plan->reserved_pads.begin(), plan->reserved_pads.end(), pad->id) == plan->reserved_pads.end()) return none();
        context.issue_command(std::string(verb_pad_build), {number(LuaNumber(plan->player)), arguments[2], handle(handle_type, type->second->type_id)});
        return none();
    });
    method("Attack_Target", taskforce_move(host, "attack", false));
    method("Move_To", taskforce_move(host, "move", true));
    method("Guard_Target", taskforce_move(host, "guard", false));
    // Fidelity list: the exploration block's sweep is not hosted; the TaskForce moves to the area.
    method("Explore_Area", taskforce_move(host, "move", true));
    // EX-35 Prepare_Ambush(target, direction, distance, threat tolerance).
    method("Prepare_Ambush", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        if (arguments.size() != 5) return fail("Prepare_Ambush -- Invalid number of parameters.  Expected 4");
        const ViewUnit* object = nullptr;
        if (const ai::Target* target = target_of(*host, arguments[1])) {
            if (target->object == 0) return fail("Prepare_Ambush -- AI targets must represent game objects");
            object = host->view->find(target->object);
        } else {
            object = live_object(*host, arguments[1]);
        }
        if (object == nullptr) return none();
        const std::string* direction = as_text(arguments[2]);
        if (direction == nullptr) return none();
        const std::string side = upper(*direction);
        std::int64_t offset = 0;
        if (side == "LEFT") {
            offset = 1;
        } else if (side == "RIGHT") {
            offset = 2;
        } else if (side == "FRONT") {
            offset = 0;
        } else if (side == "BACK") {
            offset = 3;
        } else {
            return none();
        }
        const auto* distance = std::get_if<LuaNumber>(&arguments[3].data);
        if (distance == nullptr || !(LuaNumber(0) < *distance)) return fail("Prepare_Ambush -- Parameter 3 is not a positive number.");
        const auto* tolerance = std::get_if<LuaNumber>(&arguments[4].data);
        if (tolerance == nullptr) return fail("Prepare_Ambush -- Parameter 4 is not a valid number.");
        const std::uint64_t sequence = context.command_sequence();
        stage(context, "ambush", {handle(ai::handle_taskforce, taskforce->id), handle(handle_game_object, object->id),
                                     number(LuaNumber(offset)), number(ai::to_single(*distance)), number(ai::to_single(*tolerance))});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    });
    // SAE-03: reserve members stay pooled until their plan asks for reinforcement.
    method("Reinforce", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr || arguments.size() < 2) return none();
        const auto* plan = host->engine->plan(taskforce->plan);
        // SAE-03: a fixed-force session without an economy has no reinforcement pool.
        if (plan == nullptr || host->economy(plan->player) == nullptr) return none();
        const auto position = extract_position(*host, arguments[1]);
        if (!position) return none();
        const auto sequence = context.command_sequence();
        stage(context, "reinforce", {handle(ai::handle_taskforce, taskforce->id), position_value(*position)});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    });
    method("Get_Stage", [](BindingContext&, const ValueList&) { return none(); });
    method("Form_Units", [](BindingContext&, const ValueList&) { return none(); });
    method("Withdraw_Units", [](BindingContext&, const ValueList&) { return none(); });
    method("Launch_Units", [](BindingContext&, const ValueList&) { return none(); });
    // EX-52: the largest combat power metric of the members.
    method("Get_Self_Threat_Max", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        Real top{};
        for (const sim::EntityId member : taskforce->units) {
            const ViewUnit* unit = host->view->find(member);
            const AiType* type = unit == nullptr ? nullptr : host->type(unit->type);
            if (type != nullptr && top < type->combat_power) top = type->combat_power;
        }
        return one(number(top));
    });
    method("Get_Force_Count", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        return one(number(LuaNumber(static_cast<std::int64_t>(taskforce == nullptr ? 0 : taskforce->units.size()))));
    });
    method("Is_Valid", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        return one(boolean(taskforce != nullptr && !taskforce->units.empty()));
    });
    method("Get_Unit_Table", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        std::vector<Value> units;
        if (taskforce != nullptr) {
            for (const sim::EntityId member : taskforce->units) {
                if (host->view->find(member) != nullptr) units.push_back(handle(handle_game_object, member));
            }
        }
        return one(list_of(std::move(units)));
    });
    method("Get_Distance", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        if (arguments.size() != 2) return fail("Get_Distance -- Invalid number of parameters.  Expected 1");
        const auto position = extract_position(*host, arguments[1]);
        if (!position) return fail("Get_Distance -- could not extract a position from parameter 1.");
        const auto length = distance(taskforce_position(*host, *taskforce), *position);
        if (!length) return fail("Get_Distance: distance out of range");
        return one(number(ai::to_single(numeric::from_fixed(*length))));
    });
    method("Set_Plan_Result", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2 || !std::holds_alternative<bool>(arguments[1].data)) {
            return fail("Set_Plan_Result - invalid type for parameter 1.  Expected boolean.");
        }
        stage(context, "result", {arguments[1]});
        return none();
    });
    method("Set_As_Goal_System_Removable", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() < 2 || !std::holds_alternative<bool>(arguments[1].data)) {
            return fail("Set_As_Goal_System_Removable -- Invalid parameter 1, expected boolean");
        }
        stage(context, "removable", {arguments[1]});
        return none();
    });
    method("Release_Unit", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr || arguments.size() < 2 || as_handle(arguments[1], handle_game_object) == nullptr) return none();
        stage(context, "release", {handle(ai::handle_taskforce, taskforce->id), arguments[1]});
        return none();
    });
    method("Release_Forces", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        stage(context, "release_all", {handle(ai::handle_taskforce, taskforce->id)});
        return none();
    });
    method("Collect_All_Free_Units", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 1 && arguments.size() != 2)
            return fail("Collect_All_Free_Units expects zero or one category argument");
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        std::uint64_t mask = std::numeric_limits<std::uint64_t>::max();
        if (arguments.size() == 2) {
            const auto* category = as_text(arguments[1]);
            if (category == nullptr) return fail("Collect_All_Free_Units expects a category string");
            bool valid = false;
            mask = names_to_bits(*host, *category, valid);
            if (!valid || ai::split_names(*category, "|").empty())
                return fail("Collect_All_Free_Units has an unrecognized category");
        }
        // EX-12: preserve all 64 category bits across the staged command.
        stage(context, "collect", {handle(ai::handle_taskforce, taskforce->id), Value::text(std::to_string(mask))});
        return none();
    });
    method("Block_Goal_Proposal", [](BindingContext& context, const ValueList&) -> core::Result<ValueList> {
        stage(context, "block_proposal", {});
        return none();
    });
    method("Are_All_Units_On_Free_Store", [host](BindingContext&, const ValueList& arguments) {
        const auto* taskforce = taskforce_of(*host, arguments[0]);
        const auto* plan = taskforce != nullptr && host->engine != nullptr ? host->engine->plan(taskforce->plan) : nullptr;
        return one(boolean(plan != nullptr && !plan->requires_production));
    });
    // FH-24 (#76): Activate_Ability(name, on) asks each of the TaskForce's units in turn, as the
    // unit call does (AB-44). Retail returns a block that finishes as the units' abilities do;
    // the M2 plans never block on it, so the remake returns nil.
    method("Activate_Ability", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr || arguments.size() < 2 || as_text(arguments[1]) == nullptr) return none();
        const std::string ability = upper(*as_text(arguments[1]));
        for (const sim::EntityId id : taskforce->units) {
            if (const ViewUnit* unit = host->view->find(id)) {
                request_ability(context, *host, *unit, ability, arguments.size() >= 3 ? &arguments[2] : nullptr);
            }
        }
        return none();
    });
    // No targeting priority sets or attack positioning in the M2 simulation.
    for (const char* name : {"Enable_Attack_Positioning", "Set_Targeting_Priorities", "Lock_Current_Orders"}) {
        method(name, [](BindingContext&, const ValueList&) { return none(); });
    }
}

void register_blocks(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto method = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(ai::handle_block, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // EX-20: a block the engine has not taken yet (staged this service) is not finished; a
    // movement block also finishes when its TaskForce has no units left.
    method("IsFinished", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const Handle* found = as_handle(arguments[0], ai::handle_block);
        const ai::Block* block = found == nullptr || host->engine == nullptr ? nullptr : host->engine->block(found->id);
        if (block == nullptr) return one(boolean(false));
        bool finished = block->finished;
        if (block->kind == ai::Block::Kind::move) {
            const ai::TaskForce* taskforce = host->engine->taskforce(block->taskforce);
            finished = finished || taskforce == nullptr || taskforce->units.empty();
        }
        return one(boolean(finished));
    });
    method("Result", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const Handle* found = as_handle(arguments[0], ai::handle_block);
        const ai::Block* block = found == nullptr || host->engine == nullptr ? nullptr : host->engine->block(found->id);
        if (block == nullptr) return none();
        if (block->kind == ai::Block::Kind::move) return none();
        if (block->kind == ai::Block::Kind::ambush) return one(boolean(block->ready));
        return one(boolean(block->result));
    });
}

void register_targets(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto method = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(ai::handle_ai_target, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    method("Get_Game_Object", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::Target* target = target_of(*host, arguments[0]);
        if (target == nullptr || target->object == 0 || host->view->find(target->object) == nullptr) return none();
        return one(handle(handle_game_object, target->object));
    });
    method("Is_Valid", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::Target* target = target_of(*host, arguments[0]);
        return one(boolean(target != nullptr && (target->object == 0 || host->view->find(target->object) != nullptr)));
    });
    method("Get_Position", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto position = extract_position(*host, arguments[0]);
        if (!position) return none();
        return one(position_value(*position));
    });
}

// DT-03 FindDeadlyEnemy(taskforce | object | AI target of an object).
core::Result<ValueList> find_deadly_enemy(const Host& host, BindingContext& context, const ValueList& arguments) {
    if (arguments.size() != 1 && arguments.size() != 3) return fail("FindDeadlyEnemy - invalid number of parameters.");
    if (host.engine == nullptr) {
        context.report(authoritative::codes::unsupported_api, "FindDeadlyEnemy returned nil: the goal system is not hosted");
        return none();
    }
    std::vector<sim::EntityId> objects;
    if (const ai::TaskForce* taskforce = taskforce_of(host, arguments[0])) {
        objects = taskforce->units;
    } else if (const ai::Target* target = target_of(host, arguments[0])) {
        if (target->object != 0) objects.push_back(target->object);
    } else if (const ViewUnit* unit = live_object(host, arguments[0])) {
        objects.push_back(unit->id);
    } else if (arguments.size() == 1) {
        return fail("FindDeadlyEnemy - invalid type for parameter 1. Expected taskforce or AI ship target location.");
    }
    const sim::EntityId enemy = host.engine->deadly_enemy(objects);
    if (enemy == 0) return none();
    return one(handle(handle_game_object, enemy));
}

// Project_By_Unit_Range(object, position): the position moved on, away from the object, by the
// object's reach (its Targeting_Max_Attack_Distance or its longest live weapon hardpoint),
// clamped to the map.
core::Result<ValueList> project_by_unit_range(const Host& host, BindingContext& context, const ValueList& arguments) {
    if (arguments.size() != 2) return fail("Project_By_Unit_Range -- invalid number of parameters.  Expected 2");
    if (as_handle(arguments[0], handle_game_object) == nullptr) {
        return fail("Project_By_Unit_Range -- invalid type for parameter 1.  Expected game object.");
    }
    const ViewUnit* unit = live_object(host, arguments[0]);
    if (unit == nullptr) return none();
    const auto position = extract_position(host, arguments[1]);
    if (!position) return fail("Project_By_Unit_Range -- unable to extract a position from parameter 2.");
    const Real px = numeric::from_fixed(position->x), py = numeric::from_fixed(position->y), pz = numeric::from_fixed(position->z);
    const Real dx = px - numeric::from_fixed(unit->position.x);
    const Real dy = py - numeric::from_fixed(unit->position.y);
    const Real dz = pz - numeric::from_fixed(unit->position.z);
    if (dx == Real{} && dy == Real{} && dz == Real{}) {
        context.report(authoritative::codes::unsupported_api, "Project_By_Unit_Range -- unable to project: positions are identical.");
        return none();
    }
    const Real length = ai::square_root(ai::to_single(dx * dx + dy * dy + dz * dz));
    const ViewUnit* leader = unit;
    if (unit->container) {
        leader = nullptr;
        for (const sim::EntityId member : unit->members) {
            if ((leader = host.view->find(member)) != nullptr) break;
        }
    }
    if (leader == nullptr) return one(position_value(*position));
    const AiType* type = host.type(leader->type);
    Real reach = type == nullptr ? Real{} : type->max_attack_distance;
    if (type != nullptr) {
        for (const AiWeapon& weapon : type->weapons) {
            const bool destroyed = weapon.hardpoint < leader->hardpoint_destroyed.size() && leader->hardpoint_destroyed[weapon.hardpoint];
            if (weapon.destroyable && destroyed) continue;
            if (reach < weapon.range) reach = weapon.range;
        }
    }
    Real x = ai::to_single(px + reach * ai::to_single(dx / length));
    Real y = ai::to_single(py + reach * ai::to_single(dy / length));
    const Real z = ai::to_single(pz + reach * ai::to_single(dz / length));
    if (host.setup.bounds) {
        const AiBounds& bounds = *host.setup.bounds;
        x = std::clamp(x, bounds.left, bounds.right);
        y = std::clamp(y, bounds.bottom, bounds.top);
    }
    std::vector<Value> out{number(x), number(y), number(z)};
    return one(Value{std::move(out)});
}

// FT-01 to FT-03 FindTarget(taskforce, function, application flags, fraction[, max distance]):
// the targets the flags match (within the distance of the TaskForce) are scored by the
// function; the best scoring one is kept apart, the others weigh their score in a draw, and the
// best joins the draw with the others' total over the fraction. Without another entry above 0,
// or with a zero fraction, the best is the answer; no score above 0 answers nil (FT-03,
// ai::choose_target). An object target answers with its game object.
core::Result<ValueList> find_target(const Host& host, BindingContext& context, const ValueList& arguments, bool reachable) {
    if (host.engine == nullptr) {
        context.report(authoritative::codes::unsupported_api, "FindTarget returned nil: the goal system is not hosted");
        return none();
    }
    const std::size_t function_index = 1;
    const std::size_t flags_index = 2;
    const std::size_t fraction_index = reachable ? 4 : 3;
    if (arguments.size() <= fraction_index) return fail("FindTarget -- invalid number of parameters");
    const ai::TaskForce* taskforce = reachable ? nullptr : taskforce_of(host, arguments[0]);
    tactical::PlayerId player{};
    if (taskforce != nullptr) {
        if (const ai::Plan* plan = host.engine->plan(taskforce->plan)) player = plan->player;
    } else if (const Handle* owner = as_handle(arguments[0], handle_player)) {
        player = static_cast<tactical::PlayerId>(owner->id);
    } else {
        return fail("FindTarget -- invalid type for parameter 1.  Expected taskforce.");
    }
    const std::string* function = as_text(arguments[function_index]);
    const std::string* flag_text = as_text(arguments[flags_index]);
    const auto* fraction = std::get_if<LuaNumber>(&arguments[fraction_index].data);
    if (function == nullptr || flag_text == nullptr || fraction == nullptr) return fail("FindTarget -- invalid parameters");
    std::set<std::string> flags;
    for (const std::string& name : ai::split_names(*flag_text, "|")) flags.insert(upper(name));
    Real max_distance{};
    if (!reachable && arguments.size() > 4) {
        if (const auto* value = std::get_if<LuaNumber>(&arguments[4].data)) max_distance = ai::to_single(*value * *value);
    }
    const std::vector<std::uint64_t>* targets = host.engine->target_list(player);
    if (targets == nullptr) return none();
    const math::Vec3 origin = taskforce != nullptr ? taskforce_position(host, *taskforce) : math::Vec3{};
    std::vector<const ai::Target*> candidates;
    std::vector<Real> scores;
    for (const std::uint64_t id : *targets) {
        const ai::Target* target = host.engine->target(id);
        if (!host.engine->target_matches(player, flags, target)) continue;
        // The distance test comes before the scoring (#957): a target out of reach is dropped
        // either way, and scoring is the expensive part (a search over every tactical location
        // took tens of milliseconds); the answer is the same.
        if (taskforce != nullptr && Real{} < max_distance) {
            const auto position = extract_position(host, handle(ai::handle_ai_target, target->id));
            if (!position) continue;
            const Real dx = numeric::from_fixed(origin.x) - numeric::from_fixed(position->x);
            const Real dy = numeric::from_fixed(origin.y) - numeric::from_fixed(position->y);
            const Real dz = numeric::from_fixed(origin.z) - numeric::from_fixed(position->z);
            if (max_distance < ai::to_single(dx * dx + dy * dy + dz * dz)) continue;
        }
        const auto score = host.engine->evaluate(*function, player, target);
        if (!score) continue;
        candidates.push_back(target);
        scores.push_back(*score);
    }
    // Fidelity list: the draw uses the instance's stream, not the game's synchronized one.
    const auto chosen = ai::choose_target(scores, *fraction, [&context] {
        return ai::real(static_cast<std::int64_t>(context.random().next_below(std::uint64_t{1} << 24))) /
            ai::real(std::int64_t{1} << 24);
    });
    if (!chosen) return none();
    const ai::Target* answer = candidates[*chosen];
    if (answer->object != 0) {
        if (host.view->find(answer->object) == nullptr) return none();
        return one(handle(handle_game_object, answer->object));
    }
    return one(handle(ai::handle_ai_target, answer->id));
}

std::uint64_t names_to_bits(const Host& host, const std::string& text, bool& ok) {
    std::uint64_t bits = 0;
    ok = true;
    std::string current;
    const auto flush = [&] {
        std::string name;
        for (const char character : current) {
            if (character != ' ' && character != '\t') name.push_back(character);
        }
        current.clear();
        if (name.empty()) return;
        const auto found = host.setup.content.categories.find(upper(name));
        if (found == host.setup.content.categories.end()) {
            ok = false;
            return;
        }
        bits |= found->second;
    };
    for (const char character : text) {
        if (character == '|') {
            flush();
        } else {
            current.push_back(character);
        }
    }
    flush();
    return bits;
}

void register_functions(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto add = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    add("FindDeadlyEnemy", [host](BindingContext& context, const ValueList& arguments) {
        return find_deadly_enemy(*host, context, arguments);
    });
    add("Project_By_Unit_Range", [host](BindingContext& context, const ValueList& arguments) {
        return project_by_unit_range(*host, context, arguments);
    });
    // GR-01: GameRandom(low, high) and GameRandom.Get_Float() draw on the instance's own
    // seeded stream (fidelity list: the retail synchronized generator is shared by the game).
    add("GameRandom", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2) return fail("GameRandom expects a low and a high value");
        const auto* low = std::get_if<LuaNumber>(&arguments[0].data);
        const auto* high = std::get_if<LuaNumber>(&arguments[1].data);
        if (low == nullptr || high == nullptr) return fail("GameRandom expects numbers");
        const auto a = static_cast<std::int32_t>(*low);
        const auto b = static_cast<std::int32_t>(*high);
        const std::int64_t lo = std::min(a, b), hi = std::max(a, b);
        const std::uint64_t span = static_cast<std::uint64_t>(hi - lo) + 1U;
        return one(number(LuaNumber(lo + static_cast<std::int64_t>(context.random().next_below(span)))));
    });
    add("GameRandom.Get_Float", [](BindingContext& context, const ValueList&) -> core::Result<ValueList> {
        const std::uint64_t draw = context.random().next_below(std::uint64_t{1} << 24);
        return one(number(ai::real(static_cast<std::int64_t>(draw)) / ai::real(std::int64_t{1} << 24)));
    });
    add("Find_All_Objects_Of_Type", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const Handle* player = arguments.empty() ? nullptr : as_handle(arguments[0], handle_player);
        const std::string* name = arguments.size() > 1 ? as_text(arguments[1]) : (arguments.size() == 1 ? as_text(arguments[0]) : nullptr);
        if (name == nullptr) return fail("Find_All_Objects_Of_Type expects a type or category name");
        bool category = false;
        const std::uint64_t bits = names_to_bits(*host, *name, category);
        const auto type = host->types_by_name.find(upper(*name));
        std::vector<Value> out;
        for (const ViewUnit& unit : host->view->units) {
            if (player != nullptr && unit.owner != player->id) continue;
            const AiType* info = host->type(unit.type);
            if (info == nullptr) continue;
            const bool match = category ? (info->category_bits & bits) != 0 : (type != host->types_by_name.end() && type->second == info);
            if (match) out.push_back(handle(handle_game_object, unit.id));
        }
        return one(Value{std::move(out)});
    });
    add("Find_Player", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const std::string* name = arguments.empty() ? nullptr : as_text(arguments[0]);
        if (name == nullptr) return fail("Find_Player expects a faction name");
        for (const AiPlayer& player : host->setup.players) {
            if (player.faction == upper(*name)) return one(handle(handle_player, player.player));
        }
        return none();
    });
    add("FindTarget", [host](BindingContext& context, const ValueList& arguments) {
        return find_target(*host, context, arguments, false);
    });
    add("FindTarget.Reachable_Target", [host](BindingContext& context, const ValueList& arguments) {
        return find_target(*host, context, arguments, true);
    });
    add("Is_Multiplayer_Mode", [](BindingContext&, const ValueList&) { return one(boolean(false)); });
    // No weather fields and no defended-position search in the M2 map (fidelity list).
    add("Find_Nearest_Space_Field", [](BindingContext&, const ValueList&) { return none(); });
    add("Get_Most_Defended_Position", [](BindingContext&, const ValueList&) { return none(); });
    // PL-45: the calling plan's other removable plans are abandoned (staged for the engine).
    add("Purge_Goals", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 1) return fail("Purge_Goals -- invalid number of parameters.  Expected 1");
        if (as_handle(arguments[0], handle_player) == nullptr) return fail("Purge_Goals -- invalid type for parameter 1.  Expected a player.");
        stage(context, "purge", {});
        return none();
    });
    add("GlobalValue.Get", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const std::string* name = arguments.empty() ? nullptr : as_text(arguments[0]);
        if (name == nullptr || host->engine == nullptr) return none();
        const auto value = host->engine->global_value(*name);
        if (!value) return none();
        return one(number(*value));
    });
    add("GlobalValue.Set", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2 || as_text(arguments[0]) == nullptr) return fail("GlobalValue.Set expects a name and a value");
        if (std::holds_alternative<LuaNumber>(arguments[1].data)) stage(context, "global", {arguments[0], arguments[1]});
        return none();
    });
}

} // namespace

const ViewUnit* destination_object(const Host& host, const Value& value) {
    if (const auto* target = target_of(host, value))
        return target->object == 0 ? nullptr : host.view->find(target->object);
    return live_object(host, value);
}

std::optional<UnitDestination> unit_destination(const Host& host, const ViewUnit& mover,
    const Value& value, bool follow_object, bool guard) {
    const auto position = extract_position(host, value);
    if (!position) return std::nullopt;
    UnitDestination destination{0, *position};
    if (!follow_object) return destination;
    const ViewUnit* object = destination_object(host, value);
    // FH-41: unit guard chooses the first member other than itself or its parent.
    if (guard) {
        if (const auto* force = taskforce_of(host, value)) {
            for (const auto member : force->units) {
                if (member == mover.id || (mover.squadron != 0 && member == mover.squadron)) continue;
                object = host.view->find(member);
                break;
            }
        }
    }
    if (object != nullptr && object->id != mover.id && (mover.squadron == 0 || object->id != mover.squadron))
        destination.object = object->id;
    return destination;
}

void register_plan_bindings(ScriptScheduler& scripts, const std::shared_ptr<Host>& host, std::vector<core::Diagnostic>& errors) {
    register_taskforce(scripts, host, errors);
    register_blocks(scripts, host, errors);
    register_targets(scripts, host, errors);
    register_functions(scripts, host, errors);
}

} // namespace eawr::script::foc::detail
