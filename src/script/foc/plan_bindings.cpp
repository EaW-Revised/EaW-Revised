// The engine APIs of the FoC space plans (#449, docs/behaviour/foc-tactical-ai.md "Plans and
// TaskForces"): TaskForce objects, blocking objects, AI targets and the global functions the
// plans and their libraries call. Default bindings stage commands for after_service.
// Qualified GC magic uses the bounded native shadow for request-time creation;
// block completion remains owned by the later execution service (GFD-A08).

#include "ai_engine.hpp"
#include "galactic_ai_actions.hpp"

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
        if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC") {
            if (kind != "move" || taskforce->units.empty() || !taskforce->galactic_fleet) return none();
            const auto* plan = host->engine->plan(taskforce->plan);
            if (!plan || !host->engine->galactic_movement_reach(plan->id)) return fail("galactic Move_To requires retained goal reachability");
            const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host->galactic_execution.get());
            if (!actions || !actions->supports(galactic_ai::ActionKind::move)) return fail("galactic Move_To requires its rebuilt movement owner");
            std::optional<sim::EntityId> destination;
            if (const auto* target = target_of(*host, arguments[1])) destination = target->object;
            else if (const auto* object = as_handle(arguments[1], handle_game_object)) destination = object->id;
            if (!destination || !*destination || !host->view || !host->view->find(*destination)) return none();
            const auto sequence = context.command_sequence();
            stage(context, "galactic_move", {arguments[0], handle(handle_game_object, *destination)});
            return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
        }
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

Binding taskforce_produce(HostPtr host) {
    return [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* taskforce = taskforce_of(*host, arguments[0]);
        if (!taskforce) return none();
        const auto sequence = context.command_sequence();
        if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC") {
            const auto* plan = host->engine->plan(taskforce->plan);
            if (!plan || plan->definition >= host->engine->plans().size()) return none();
            if (arguments.size() > 3) return fail("Produce_Force accepts only staging and synchronized staging");
            sim::EntityId requested_stage{};
            if (arguments.size() > 1 && !std::holds_alternative<std::monostate>(arguments[1].data)) {
                if (const auto* target = target_of(*host, arguments[1])) requested_stage = target->object;
                else if (const auto* object = as_handle(arguments[1], handle_game_object)) requested_stage = object->id;
                const auto* world = dynamic_cast<const ai::GalacticWorld*>(host->ai_world.get());
                if (!requested_stage || !world || world->containing_planet(requested_stage) != requested_stage)
                    return fail("Produce_Force staging must resolve to a live planet");
            }
            bool synchronized{};
            if (arguments.size() > 2) {
                const auto* supplied = std::get_if<bool>(&arguments[2].data);
                if (!supplied) return fail("Produce_Force synchronized staging must be Boolean");
                synchronized = *supplied;
            }
            if (!host->engine->plans()[plan->definition].magic) {
                if (!host->galactic_execution || !host->engine->galactic_goal_for_plan(plan->player, plan->id)
                    || taskforce->types.size() != taskforce->sources.size()
                    || taskforce->types.size() != taskforce->producers.size()
                    || std::find(taskforce->sources.begin(), taskforce->sources.end(), 1) != taskforce->sources.end())
                    return core::Result<ValueList>::failure(make_error(authoritative::codes::missing_api,
                        "galactic paid production requires selected retained sources and the native goal owner"));
                stage(context, "produce", {arguments[0], handle(handle_game_object, requested_stage), Value{synchronized}});
                return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
            }
            sim::EntityId staging = plan->target_object;
            if (arguments.size() > 1 && !std::holds_alternative<std::monostate>(arguments[1].data)) {
                if (const auto* target = target_of(*host, arguments[1])) staging = target->object;
                else if (const auto* object = as_handle(arguments[1], handle_game_object)) staging = object->id;
                else return fail("magic Produce_Force requires a live staging planet");
            }
            std::vector<Value> types;
            for (const auto type : taskforce->types) if (type) types.push_back(handle(handle_type, type));
            auto made = context.apply_immediate("galactic.magic_force", {handle(handle_player, plan->player),
                handle(handle_game_object, staging), Value{std::move(types)}, arguments[0]});
            if (!made) return made;
            if (made.value().size() != 1 || !std::holds_alternative<std::vector<Value>>(made.value()[0].data))
                return fail("native magic provider returned no actor list");
            // This command adopts already-created identities. It cannot spawn
            // again, and is rolled back alongside the private native writes.
            stage(context, "native_magic", {arguments[0], std::move(made).value()[0],
                handle(handle_game_object, requested_stage), Value{synchronized}});
        } else stage(context, "produce", {handle(ai::handle_taskforce, taskforce->id)});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    };
}

void register_taskforce(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto method = [&](std::string_view name, Binding binding) {
        if (name == "Withdraw_Units") binding = [host, call = std::move(binding)](BindingContext& context, const ValueList& arguments) {
            if (host->in_object_turn) return fail("object turn dynamic-read tripwire: Withdraw_Units");
            return call(context, arguments);
        };
        if (auto added = scripts.register_method(ai::handle_taskforce, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // EX-10 Produce_Force: a block over one build task per selected unit.
    method("Produce_Force", taskforce_produce(host));
    method("Attack_Move", taskforce_move(host, "attack_move", false));
    method("Build_All", taskforce_produce(host));
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
        stage(context, "build_pad", {handle(ai::handle_taskforce, taskforce->id), arguments[2],
            handle(handle_type, type->second->type_id)});
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
    method("Get_Stage", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty()) return none();
        const auto* force = taskforce_of(*host, arguments[0]);
        if (!force || !force->galactic_stage || !host->ai_world || host->ai_world->goal_mode() != "GALACTIC") return none();
        if (const auto* target = host->engine->target_of_object(force->galactic_stage)) return one(handle(ai::handle_ai_target, target->id));
        return host->view && host->view->find(force->galactic_stage) ? one(handle(handle_game_object, force->galactic_stage)) : none();
    });
    const auto containment = [host](std::string operation, galactic_ai::ActionKind kind) -> Binding {
        return [host, operation, kind](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            if (arguments.empty() || !host->ai_world || host->ai_world->goal_mode() != "GALACTIC") return none();
            const auto* force = taskforce_of(*host, arguments[0]);
            if (!force || force->units.empty()) return none();
            if (!host->galactic_execution || !host->galactic_execution->supports(kind)) return fail("galactic containment requires its live world owner");
            if (kind == galactic_ai::ActionKind::land && !force->galactic_fleet) return none();
            const auto sequence = context.command_sequence(); stage(context, operation, {arguments[0]});
            return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
        };
    };
    method("Form_Units", containment("galactic_form", galactic_ai::ActionKind::regroup));
    method("Land_Units", containment("galactic_land", galactic_ai::ActionKind::land));
    method("Invade", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty() || !host->ai_world || host->ai_world->goal_mode() != "GALACTIC") return none();
        const auto* force = taskforce_of(*host, arguments[0]);
        if (!force || !force->galactic_fleet) return none();
        if (!host->galactic_execution || !host->galactic_execution->supports(galactic_ai::ActionKind::invade)) return fail("Invade requires its retained campaign queue owner");
        const auto sequence = context.command_sequence(); stage(context, "galactic_invade", {arguments[0]});
        return one(handle(ai::handle_block, ai::block_id(context.instance(), sequence)));
    });
    method("Withdraw_Units", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty()) return none();
        const auto* force = taskforce_of(*host, arguments[0]);
        const auto* plan = force ? host->engine->plan(force->plan) : nullptr;
        const auto& setup = host->setup;
        if (!plan || !setup.perception.campaign_game || !setup.campaign_can_retreat
            || !setup.campaign_retreat_order || !host->snapshot || !setup.campaign_can_retreat(*host->snapshot, plan->player))
            return none();
        context.issue_command(std::string(verb_campaign_retreat), {number(LuaNumber(plan->player))});
        return one(boolean(true));
    });
    method("Launch_Units", containment("galactic_launch", galactic_ai::ActionKind::launch));
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
    // EX-54: preserve the single-precision fold in force membership order.
    method("Get_Self_Threat_Sum", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        Real total{};
        for (const sim::EntityId member : taskforce->units) {
            const auto* unit = host->view->find(member);
            const auto* type = unit == nullptr ? nullptr : host->type(unit->type);
            if (type != nullptr) total = ai::to_single(total + type->combat_power);
        }
        return one(number(total));
    });
    method("Get_Force_Count", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        auto immediate = galactic_immediate_query(context, *host, "galactic.taskforce_units", {arguments[0]});
        if (!immediate) return immediate;
        const auto extra = immediate.value().empty() ? 0 : std::get<std::vector<Value>>(immediate.value()[0].data).size();
        return one(number(LuaNumber(static_cast<std::int64_t>((taskforce ? taskforce->units.size() : 0) + extra))));
    });
    method("Is_Valid", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        auto immediate = galactic_immediate_query(context, *host, "galactic.taskforce_units", {arguments[0]});
        if (!immediate) return immediate;
        const bool created = !immediate.value().empty() && !std::get<std::vector<Value>>(immediate.value()[0].data).empty();
        return one(boolean(taskforce != nullptr && (!taskforce->units.empty() || created)));
    });
    method("Get_Unit_Table", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        std::vector<Value> units;
        if (taskforce != nullptr) {
            for (const sim::EntityId member : taskforce->units) {
                if (host->view->find(member) != nullptr) units.push_back(handle(handle_game_object, member));
            }
        }
        auto immediate = galactic_immediate_query(context, *host, "galactic.taskforce_units", {arguments[0]});
        if (!immediate) return immediate;
        if (!immediate.value().empty()) {
            auto& created = std::get<std::vector<Value>>(immediate.value()[0].data);
            units.insert(units.end(), created.begin(), created.end());
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
    // EX-55: remove the calling goal's holders, leaving other goals blocked.
    method("Unblock_Goal_Proposal", [](BindingContext& context, const ValueList&) -> core::Result<ValueList> {
        stage(context, "unblock_proposal", {});
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
                static_cast<void>(request_ability(context, *host, *unit, ability, arguments.size() >= 3 ? &arguments[2] : nullptr));
            }
        }
        return none();
    });
    method("Fire_Special_Weapon", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 3 || as_text(arguments[1]) == nullptr)
            return fail("Fire_Special_Weapon expects a type name and an object target");
        const auto found = host->types_by_name.find(upper(*as_text(arguments[1])));
        if (found == host->types_by_name.end()) return fail("Fire_Special_Weapon: unknown type");
        const ViewUnit* target = destination_object(*host, arguments[2]);
        if (as_handle(arguments[2], handle_game_object) == nullptr &&
            as_handle(arguments[2], ai::handle_ai_target) == nullptr)
            return fail("Fire_Special_Weapon: expected an object or AI target");
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        const ai::Plan* plan = taskforce != nullptr ? host->engine->plan(taskforce->plan) : nullptr;
        // WSW-41: unavailable weapon/permission/category answers false, successful fire its handle.
        if (plan == nullptr || target == nullptr || !found->second->space_special_weapon) return one(boolean(false));
        for (const sim::EntityId member : taskforce->units) {
            const ViewUnit* source = host->view->find(member);
            if (source == nullptr || source->type != found->second->type_id ||
                !planetary_fire_ready(*host, *source, *target, plan->player)) continue;
            issue_special_fire(context, *source, *target, plan->player, tactical::SpecialFireAdmission::ai_taskforce);
            return one(handle(handle_game_object, source->id));
        }
        return one(boolean(false));
    });
    // WTA-23: Enable_Attack_Positioning(on) sets the members' attack-positioning flag (Lua truth).
    method("Enable_Attack_Positioning", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ai::TaskForce* taskforce = taskforce_of(*host, arguments[0]);
        if (taskforce == nullptr) return none();
        const Value* flag = arguments.size() >= 2 ? &arguments[1] : nullptr;
        const bool on = flag != nullptr && !std::holds_alternative<std::monostate>(flag->data)
            && !(std::holds_alternative<bool>(flag->data) && !std::get<bool>(flag->data));
        stage(context, "attack_positioning", {handle(ai::handle_taskforce, taskforce->id), boolean(on)});
        return none();
    });
    // EX-59: unhosted targeting overrides and order locks must remain visible.
    for (const char* name : {"Set_Targeting_Priorities", "Lock_Current_Orders"}) {
        method(name, [name = std::string(name)](BindingContext& context, const ValueList&) {
            context.report(authoritative::codes::unsupported_api, "TaskForce." + name + " skipped: per-unit targeting sets and order locks are not hosted");
            return none();
        });
    }
}

void register_blocks(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto method = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(ai::handle_block, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // EX-20: a block the engine has not taken yet (staged this service) is not finished; a
    // movement block also finishes when its TaskForce has no units left.
    method("IsFinished", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const Handle* found = as_handle(arguments[0], ai::handle_block);
        const ai::Block* block = found == nullptr || host->engine == nullptr ? nullptr : host->engine->block(found->id);
        if (block == nullptr) return one(boolean(false));
        if (block->kind == ai::Block::Kind::galactic_invade && block->galactic_request) {
            const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host->galactic_execution.get());
            const auto* invasion = actions ? actions->invasion_status(block->galactic_request) : nullptr;
            if (!invasion) return fail("invasion block lost its retained queue owner");
            if (invasion->state == sim::galactic::GalacticSession::NativeInvasion::State::completed)
                stage(context, "galactic_retire_invasion", {arguments[0]});
            return one(boolean(invasion->state == sim::galactic::GalacticSession::NativeInvasion::State::completed));
        }
        if (block->galactic_request) {
            const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host->galactic_execution.get());
            if (!actions || !actions->movements().contains(block->galactic_request)) return fail("galactic block lost its native movement owner");
            const auto& movement = actions->movements().at(block->galactic_request);
            if (!movement.finished) stage(context, "galactic_poll", {arguments[0]});
            return one(boolean(movement.finished || !movement.actor || !actions->movement_actor_exists(movement.actor)));
        }
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
        if (block->kind == ai::Block::Kind::galactic_invade && !block->galactic_request) return one(boolean(block->result));
        if (block->kind == ai::Block::Kind::galactic_invade && block->galactic_request) {
            const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host->galactic_execution.get());
            const auto* invasion = actions ? actions->invasion_status(block->galactic_request) : nullptr;
            if (!invasion) return fail("invasion result lost its retained queue owner");
            return one(boolean(invasion->success));
        }
        const bool galactic = host->ai_world && host->ai_world->goal_mode() == "GALACTIC";
        if (galactic && (block->kind == ai::Block::Kind::move || block->kind == ai::Block::Kind::galactic_land
            || block->kind == ai::Block::Kind::galactic_launch || block->kind == ai::Block::Kind::galactic_form)) {
            auto actor = block->galactic_actor;
            if (block->galactic_request) {
                const auto* actions = dynamic_cast<const galactic_ai::NativeActions*>(host->galactic_execution.get());
                if (!actions || !actions->movements().contains(block->galactic_request)) return fail("galactic result lost its native movement owner");
                actor = actions->movements().at(block->galactic_request).actor;
            }
            return actor ? one(handle(handle_game_object, actor)) : none();
        }
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
    // The stock evaluators measure from their Target (GetDistanceToNearestSpaceField, EX-51): the
    // distance from the target's object or region centre to an object, AI target, TaskForce or position.
    method("Get_Distance", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() < 2) return none();
        const auto origin = extract_position(*host, arguments[0]);
        if (!origin) return none();
        const auto other = extract_position(*host, arguments[1]);
        if (!other) return fail("Get_Distance: parameter 1 is not a live object or a position");
        const auto length = distance(*origin, *other);
        if (!length) return fail("Get_Distance: distance out of range");
        return one(number(numeric::from_fixed(*length)));
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

// FT-07/13: a single policy; manager selection is independent of source.
enum class TargetReach { friendly, ignore_threat, enemy_destination, enemy_undefended, single_hop,
    any, no_threat, low, medium, high, any_threat };
std::optional<TargetReach> target_reach(std::string_view text) {
    const std::string name = ai::upper_case(ai::trimmed(text));
    static const std::map<std::string, TargetReach, std::less<>> names{
        {"FRIENDLY_ONLY", TargetReach::friendly}, {"FRIENDLY_IGNORE_THREAT", TargetReach::ignore_threat},
        {"ENEMY_DESTINATION", TargetReach::enemy_destination}, {"ENEMY_UNDEFENDED", TargetReach::enemy_undefended},
        {"SINGLE_HOP_DISCONNECTED", TargetReach::single_hop}, {"ANY", TargetReach::any},
        {"NO_THREAT", TargetReach::no_threat}, {"LOW_THREAT", TargetReach::low},
        {"MEDIUM_THREAT", TargetReach::medium}, {"HIGH_THREAT", TargetReach::high},
        {"ANY_THREAT", TargetReach::any_threat}};
    const auto found = names.find(name);
    return found == names.end() ? std::nullopt : std::optional{found->second};
}
bool galactic_reach(TargetReach reach) { return reach <= TargetReach::any; }

// FT-08/09/15: decoded execution cost is only a reachability gate; never a score.
Real target_path_cost(const ai::FindTargetPath& path, TargetReach reach, const ai::Constants& constants) {
    const Real unreachable = ai::to_single(ai::real(1000000000000000000LL));
    if (!path.source_valid || (path.service == ai::FindTargetService::land && galactic_reach(reach))) return unreachable;
    if (path.same_target && path.service == ai::FindTargetService::galactic) return Real{};
    const Real strict = path.service == ai::FindTargetService::galactic
        ? ai::to_single(ai::real(1) / ai::real(10000)) : Real{};
    Real tolerance = strict;
    switch (reach) {
    case TargetReach::ignore_threat: case TargetReach::any: case TargetReach::any_threat: tolerance = unreachable; break;
    case TargetReach::low: tolerance = ai::to_single(constants.low_threat_tolerance); break;
    case TargetReach::medium: tolerance = ai::to_single(constants.medium_threat_tolerance); break;
    case TargetReach::high: tolerance = ai::to_single(constants.high_threat_tolerance); break;
    default: break;
    }
    const Real threat = path.empty ? ai::real(6553500)
        : ai::to_single(reach == TargetReach::single_hop ? path.prefix_threat : path.threat);
    if (!(threat <= tolerance)) return unreachable;
    return path.empty ? ai::to_single(ai::real(65535) / ai::real(10)) : ai::to_single(path.distance);
}

// FT-01..09/12..15: selected-manager goal targets, optional resolved source,
// execution-path safety, perception evaluation, source radius, then score draw.
core::Result<ValueList> find_target(const Host& host, BindingContext& context, const ValueList& arguments, bool reachable) {
    if (host.engine == nullptr && !(reachable && host.find_target_queries)) {
        if (reachable) return fail("FindTarget -- selected manager is unavailable");
        context.report(authoritative::codes::unsupported_api, "FindTarget returned nil: the goal system is not hosted");
        return none();
    }
    if ((reachable && (arguments.size() < 4 || arguments.size() > 7))
        || (!reachable && (arguments.size() < 4 || arguments.size() > 5)))
        return fail("FindTarget -- invalid number of parameters");
    const ai::TaskForce* taskforce = reachable ? nullptr : taskforce_of(host, arguments[0]);
    tactical::PlayerId player{};
    if (taskforce != nullptr) {
        if (const ai::Plan* plan = host.engine->plan(taskforce->plan)) player = plan->player;
    } else if (reachable) {
        const Handle* owner = as_handle(arguments[0], handle_player);
        if (!owner || owner->id > std::numeric_limits<tactical::PlayerId>::max()) return fail("FindTarget -- invalid player");
        player = static_cast<tactical::PlayerId>(owner->id);
    } else return fail("FindTarget -- invalid taskforce");
    const std::string* function = as_text(arguments[1]);
    const std::string* flag_text = as_text(arguments[2]);
    if (!function || !flag_text) return fail("FindTarget -- invalid function or application");
    std::set<std::string> flags;
    for (const std::string& name : ai::split_names(*flag_text, "|")) flags.insert(upper(name));
    const ai::Engine* engine = host.engine;
    std::optional<TargetReach> reach;
    if (reachable) {
        const auto* text = as_text(arguments[3]);
        if (!text || !(reach = target_reach(*text))) return fail("FindTarget -- unknown reach policy");
        const auto selected = galactic_reach(*reach) ? ai::FindTargetManager::galactic : ai::FindTargetManager::current;
        engine = host.find_target_queries ? host.find_target_queries->manager(selected)
            : (selected == ai::FindTargetManager::current ? host.engine : nullptr);
        if (!engine) return fail("FindTarget -- selected manager is unavailable");
        static const std::set<std::string> applications{"ENEMY", "FRIENDLY", "NEUTRAL", "TACTICAL_LOCATION",
            "ENEMY_UNIT", "FRIENDLY_UNIT", "ENEMY_STRUCTURE", "FRIENDLY_STRUCTURE",
            "ENEMY_BUILD_PAD", "FRIENDLY_BUILD_PAD", "ENEMY_REINFORCEMENT_POINT", "FRIENDLY_REINFORCEMENT_POINT"};
        if (flags.empty() || !std::all_of(flags.begin(), flags.end(), [](const auto& flag) { return applications.contains(flag); }))
            return fail("FindTarget -- unknown or global application");
        if (!engine->target_list(player)) return fail("FindTarget -- player has no AI manager");
        if (!engine->data().equations.find(*function)) return fail("FindTarget -- unknown perception function");
    }
    Real ratio{};
    const std::size_t probability_index = reachable ? 4 : 3;
    if (arguments.size() > probability_index) {
        const auto* probability = std::get_if<LuaNumber>(&arguments[probability_index].data);
        if (!probability || !(Real{} < *probability && *probability <= ai::real(1)))
            return fail("FindTarget -- probability must be in (0, 1]");
        ratio = ai::to_single(ai::to_single(ai::real(1) / ai::to_single(*probability)) - ai::real(1));
    }
    const auto* selected_view = engine->completed_view();
    const ai::Target* source = nullptr;
    if (reachable && arguments.size() > 5) {
        if (const Handle* value = as_handle(arguments[5], ai::handle_ai_target)) {
            source = engine->target(value->id);
            if (!source || (source->object && (!selected_view || !selected_view->find(source->object))))
                return fail("FindTarget -- invalid source target");
        } else if (const Handle* object_source = as_handle(arguments[5], handle_game_object)) {
            if (!selected_view || !selected_view->find(object_source->id)) return fail("FindTarget -- invalid source object");
            source = host.find_target_queries ? host.find_target_queries->source_target(*engine, object_source->id)
                : engine->target_of_object(object_source->id);
        } else return fail("FindTarget -- source must be a live object or AI target");
    }
    Real max_distance{};
    const std::size_t distance_index = reachable ? 6 : 4;
    if (arguments.size() > distance_index) {
        const auto* distance = std::get_if<LuaNumber>(&arguments[distance_index].data);
        if (!distance) return fail("FindTarget -- distance must be numeric");
        const Real single = ai::to_single(*distance);
        max_distance = ai::to_single(single * single);
    }
    if (reachable && max_distance != Real{} && !source) return fail("FindTarget -- nonzero distance requires a source target");
    const auto* targets = engine->target_list(player);
    if (!targets) return none();
    const auto target_position = [selected_view](const ai::Target& target) -> std::optional<math::Vec3> {
        if (!target.object) return point(target.x, target.y);
        const auto* unit = selected_view ? selected_view->find(target.object) : nullptr;
        return unit ? std::optional{unit->position} : std::nullopt;
    };
    const auto source_position = source ? target_position(*source) : std::optional<math::Vec3>{};
    if (reachable && max_distance != Real{} && !source_position) return fail("FindTarget -- source has no position");
    const math::Vec3 origin = taskforce ? taskforce_position(host, *taskforce) : source_position.value_or(math::Vec3{});
    const Real unreachable = ai::to_single(ai::real(1000000000000000000LL));
    const auto in_radius = [&](const ai::Target& target) {
        if (!(taskforce || source) || max_distance == Real{}) return true;
        const auto position = target_position(target);
        if (!position) return false;
        Real dx = numeric::from_fixed(origin.x) - numeric::from_fixed(position->x);
        Real dy = numeric::from_fixed(origin.y) - numeric::from_fixed(position->y);
        Real dz = numeric::from_fixed(origin.z) - numeric::from_fixed(position->z);
        if (!reachable) return !(max_distance < ai::to_single(dx * dx + dy * dy + dz * dz));
        dx = ai::to_single(dx); dy = ai::to_single(dy); dz = ai::to_single(dz);
        const Real square = ai::to_single(ai::to_single(ai::to_single(dx * dx) + ai::to_single(dy * dy)) + ai::to_single(dz * dz));
        return !(max_distance < square);
    };
    std::vector<const ai::Target*> candidates;
    std::vector<Real> scores;
    std::size_t unavailable{};
    for (const auto id : *targets) {
        const ai::Target* target = engine->target(id);
        if (!engine->target_matches(player, flags, target)) continue;
        if (reachable && source && host.find_target_queries) {
            const auto path = host.find_target_queries->execution_path(*engine, player, *source, *target);
            if (path && target_path_cost(*path, *reach, engine->data().constants) == unreachable) continue;
        }
        // Preserve plain FindTarget's early radius optimization; FT-06 places
        // the reachable form's source-radius test after perception evaluation.
        if (!reachable && !in_radius(*target)) continue;
        const auto score = engine->evaluate(*function, player, target);
        if (!score) { ++unavailable; continue; }
        if (reachable && !in_radius(*target)) continue;
        candidates.push_back(target);
        scores.push_back(*score);
    }
    if (unavailable != 0) context.report(authoritative::codes::unsupported_api,
        "FindTarget(" + *function + ") skipped " + std::to_string(unavailable) + " candidates: perception unavailable");
    const auto chosen = ai::choose_target(scores, ratio, [&context] {
        return ai::real(static_cast<std::int64_t>(context.random().next_below(std::uint64_t{1} << 24))) /
            ai::real(std::int64_t{1} << 24);
    });
    if (!chosen) return none();
    const ai::Target* answer = candidates[*chosen];
    if (answer->object != 0) {
        if (!selected_view || selected_view->find(answer->object) == nullptr) return none();
        if (!reachable) return one(handle(handle_game_object, answer->object));
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
        if (name == "FindTarget" || name == "FindTarget.Reachable_Target") {
            binding = [host, name = std::string(name), call = std::move(binding)](BindingContext& context, const ValueList& arguments) {
                if (host->in_object_turn) return fail("object turn dynamic-read tripwire: " + name);
                return call(context, arguments);
            };
        }
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    add("FindDeadlyEnemy", [host](BindingContext& context, const ValueList& arguments) {
        return find_deadly_enemy(*host, context, arguments);
    });
    add("Project_By_Unit_Range", [host](BindingContext& context, const ValueList& arguments) {
        return project_by_unit_range(*host, context, arguments);
    });
    // L-54 forms/ranges; GR-01: all GameRandom forms draw on the instance's own
    // seeded stream (fidelity list: the retail synchronized generator is shared by the game).
    add("GameRandom", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() > 2) return fail("GameRandom expects zero, one or two numbers");
        std::int32_t a = 0, b = 65535;
        bool invalid = false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto* value = std::get_if<LuaNumber>(&arguments[index].data);
            if (value == nullptr) {
                context.report(authoritative::codes::invalid_request, "GameRandom expects numbers");
                invalid = true;
            }
            // Inferred/unverified: a non-number falls back to zero after diagnosing.
            // Out-of-int32 numeric bounds follow LuaNumber's INT32_MIN conversion;
            // its use for this API is inferred, not an independently observed L-54 rule.
            const auto bound = value == nullptr ? 0 : static_cast<std::int32_t>(*value);
            if (arguments.size() == 2 && index == 0) a = bound;
            else b = bound;
        }
        const std::int64_t lo = std::min(a, b), hi = std::max(a, b);
        if (lo == hi && !invalid) return one(number(LuaNumber(lo)));
        // L-54: supplied ranges have at most 32766 steps; the default draw is 16-bit.
        std::int64_t steps = hi - lo;
        if (!arguments.empty() && steps > 32766) {
            context.report(authoritative::codes::invalid_request, "GameRandom range exceeds 32766 steps; clamped");
            steps = 32766;
        }
        const std::uint64_t span = static_cast<std::uint64_t>(steps) + 1U;
        return one(number(LuaNumber(lo + static_cast<std::int64_t>(context.random().next_below(span)))));
    });
    add("GameRandom.Get_Float", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() > 2) return fail("GameRandom.Get_Float expects zero, one or two numbers");
        LuaNumber low(0), high(1);
        bool invalid = false;
        for (std::size_t index = 0; index < arguments.size(); ++index) {
            const auto* value = std::get_if<LuaNumber>(&arguments[index].data);
            if (value == nullptr) {
                context.report(authoritative::codes::invalid_request, "GameRandom.Get_Float expects numbers");
                invalid = true;
            }
            // Inferred/unverified: use zero for a non-number after diagnosing.
            const LuaNumber bound = value == nullptr ? LuaNumber(0) : *value;
            if (arguments.size() == 2 && index == 0) low = bound;
            else high = bound;
        }
        // Inferred/unverified from the shared range path: floats swap reversed
        // bounds and return a valid equal bound without consuming a draw.
        if (high < low) std::swap(low, high);
        if (low == high && !invalid) return one(number(low));
        const std::uint64_t draw = context.random().next_below(std::uint64_t{1} << 15);
        return one(number(low + (high - low) * LuaNumber(draw) / LuaNumber(32767)));
    });
    add("Find_All_Objects_Of_Type", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        // RGC-112/113: filters are recognized in order and intersect; a player
        // may appear alone or on either side of the name/type filter.
        const Handle* player = nullptr;
        std::optional<sim::tactical::TypeId> type;
        std::optional<std::uint64_t> categories, properties;
        const auto unrestricted = [](const std::optional<std::uint64_t> bits) {
            return !bits || *bits == std::numeric_limits<std::uint64_t>::max();
        };
        const auto mask = [](const auto& names, const std::string& text) -> std::optional<std::uint64_t> {
            std::uint64_t bits = 0;
            std::string name;
            bool valid = true;
            const auto flush = [&] {
                const auto found = names.find(upper(name));
                if (name.empty() || found == names.end()) valid = false;
                else bits |= found->second;
                name.clear();
            };
            for (const char ch : text) {
                if (ch == '|') flush();
                else if (ch != ' ' && ch != '\t') name.push_back(ch);
            }
            flush();
            return valid ? std::optional<std::uint64_t>{bits} : std::nullopt;
        };
        std::size_t filter_index = 0;
        for (const auto& argument : arguments) {
            ++filter_index;
            if (const auto* owner = as_handle(argument, handle_player)) { player = owner; continue; }
            if (const auto* wrapped = as_handle(argument, handle_type); wrapped != nullptr && !type) {
                if (const auto* info = host->type(wrapped->id)) { type = info->type_id; continue; }
            }
            if (const auto* name = as_text(argument)) {
                if (unrestricted(properties)) {
                    properties = mask(host->setup.content.properties, *name);
                    if (properties) continue;
                }
                if (unrestricted(categories)) {
                    categories = mask(host->setup.content.categories, *name);
                    if (categories) continue;
                }
                if (!type) {
                    const auto key = upper(*name);
                    const auto found = host->types_by_name.find(key);
                    if (found != host->types_by_name.end()) { type = found->second->type_id; continue; }
                    if (const auto& names = host->setup.content.catalog_types) {
                        const auto registered = std::lower_bound(names->begin(), names->end(), key,
                            [](const auto& entry, const std::string& value) { return entry.first < value; });
                        if (registered != names->end() && registered->first == key) { type = registered->second; continue; }
                    }
                }
            }
            const auto* rejected = as_text(argument);
            return fail("Find_All_Objects_Of_Type expects a known type, property, category or player; argument "
                + std::to_string(filter_index) + (rejected ? " '" + *rejected + "'" : " (non-text value)"));
        }
        if (player == nullptr && !type && unrestricted(categories) && unrestricted(properties))
            return fail("Find_All_Objects_Of_Type expects at least one filter");
        std::vector<Value> out;
        for (const ViewUnit& unit : host->view->units) {
            if (player != nullptr && unit.owner != player->id) continue;
            const AiType* info = host->type(unit.type);
            if (type && unit.type != *type) continue;
            if (!unrestricted(categories) && (info == nullptr || (info->category_bits & *categories) == 0)) continue;
            if (!unrestricted(properties) && (info == nullptr || (info->property_bits & *properties) == 0)) continue;
            out.push_back(handle(handle_game_object, unit.id));
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
