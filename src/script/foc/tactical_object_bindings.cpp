// Battle-only object methods of the tactical script host (docs/behaviour/foc-tactical-ai.md, rules TL-04..TL-08).
// Each method validates its arguments like the original and then either applies its effect or, where the battle has
// nothing to apply it to yet, accepts the call and records an unsupported_api note (the rule says which).

#include "tactical_ai_internal.hpp"
#include "discrete_distribution.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace eawr::script::foc::tactical_ai_detail {

using namespace detail;

namespace {

using authoritative::Binding;
using authoritative::BindingContext;
using authoritative::ValueList;
using numeric::LuaNumber;

core::Result<ValueList> object_call_error(std::string_view method, std::string message) {
    return fail(std::string(method) + ": " + std::move(message));
}

// The shared argument check of the one-Boolean object methods: exactly one argument, a Boolean.
core::Result<bool> one_boolean(std::string_view method, const ValueList& arguments) {
    const std::size_t count = arguments.size() - 1;
    if (count != 1) {
        return core::Result<bool>::failure(make_error(authoritative::codes::invalid_request,
            std::string(method) + ": invalid number of parameters.  Expected 1, got " + std::to_string(count) + "."));
    }
    const auto* flag = std::get_if<bool>(&arguments[1].data);
    if (flag == nullptr) {
        return core::Result<bool>::failure(make_error(authoritative::codes::invalid_request,
            std::string(method) + ": invalid type for parameter 1.  Expected boolean."));
    }
    return core::Result<bool>::success(*flag);
}

} // namespace

bool battle_bindings_enabled(const HostPtr& host) {
    return !host->ai_world || host->ai_world->goal_mode() != "GALACTIC";
}

void register_battle_distribution(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors) {
    if (!battle_bindings_enabled(host)) return;
    if (auto added = distribution::register_discrete_distribution(scripts); !added) errors.push_back(added.error());
}

// TL-04..TL-08. Registered only for a battle scheduler: the galactic scheduler binds Get_Planet_Location and
// Prevent_AI_Usage itself (the galactic free store), and the others do not apply to it.
void register_battle_object_bindings(authoritative::ScriptScheduler& scripts, const HostPtr& host,
    std::vector<core::Diagnostic>& errors) {
    if (!battle_bindings_enabled(host)) return;
    const auto global = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // FindPlanet(name): the original resolves the type by name and answers the first object of that type in the ACTIVE mode, so a
    // battle answers nil unless the battle itself holds an object of that planet type (debug build; static reading, no runtime
    // walk). A missing or non-string argument is a script error. FindPlanet.Get_All_Planets reads the FIRST mode's planet
    // objects: the galaxy in a campaign battle (every planet, ascending, as planet handles), nothing in a skirmish.
    global("FindPlanet", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty()) return fail("FindPlanet -- No planet name set");
        const std::string* name = as_text(arguments[0]);
        if (name == nullptr) return fail("FindPlanet -- Parameter 1 is not a valid string");
        return first_object_of_type(*host, context, *name);
    });
    global("FindPlanet.Get_All_Planets", [host](BindingContext& context, const ValueList&) -> core::Result<ValueList> {
        std::vector<authoritative::Value> planets;
        planets.reserve(host->setup.galactic_planets.size());
        for (const std::uint64_t id : host->setup.galactic_planets) planets.push_back(handle(handle_galactic_planet, id));
        context.charge_host_work(planets.size());
        return one(authoritative::Value{std::move(planets)});
    });
    const auto method = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_game_object, name, std::move(binding)); !added) errors.push_back(added.error());
    };

    // TL-04: a script error in every battle; the call is valid only on an object owned by the galactic mode. A dead
    // object answers nothing (the receiver check comes first).
    method("Get_Planet_Location", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        return object_call_error("Get_Planet_Location", "this function is only valid on galactic mode objects.");
    });

    // TL-05: Play_SFX_Event(name[, number]) starts a sound event on the object (audio presentation only).
    method("Play_SFX_Event", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 1 && count != 2)
            return object_call_error("Play_SFX_Event", "invalid number of parameters.  Expected 1 or 2, got " + std::to_string(count) + ".");
        if (as_text(arguments[1]) == nullptr)
            return object_call_error("Play_SFX_Event", "invalid type for parameter 1.  Expected string.");
        if (count == 2 && !std::holds_alternative<LuaNumber>(arguments[2].data))
            return object_call_error("Play_SFX_Event", "invalid type for parameter 2.  Expected number.");
        return none();
    });

    // TL-06: Disable_Capture(flag) needs the type's capture-point behaviour.
    method("Disable_Capture", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        auto flag = one_boolean("Disable_Capture", arguments);
        if (!flag) return core::Result<ValueList>::failure(flag.error());
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->capture_point) {
            return object_call_error("Disable_Capture", (type != nullptr ? type->name : std::string("object")) +
                " isn't captureable to begin with - no capture point behavior.");
        }
        context.report(authoritative::codes::unsupported_api,
            "Disable_Capture skipped: the capture-disable switch of a capture point is not hosted");
        return none();
    });

    // TL-07: Set_Garrison_Spawn(flag) needs the type's squadron-spawner behaviour.
    method("Set_Garrison_Spawn", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        auto flag = one_boolean("Set_Garrison_Spawn", arguments);
        if (!flag) return core::Result<ValueList>::failure(flag.error());
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->spawns_squadrons) {
            return object_call_error("Set_Garrison_Spawn", (type != nullptr ? type->name : std::string("object")) +
                " has no squadron-spawning behavior, so it cannot spawn garrisons.");
        }
        context.report(authoritative::codes::unsupported_api,
            "Set_Garrison_Spawn skipped: garrison spawning is not hosted");
        return none();
    });

    // Get_Bone_Position(name) (debug build): exactly one string; the case-insensitive exact name of a bone of the
    // object's skeleton model (the root included, the first match winning). Answers a position of the bone's world
    // translation: the object's position and facing (roll included) applied to the bone's rest pose. Our sim has no
    // fixed-step animation pose, so animated bone motion is not modelled (deliberate deviation). A type with no
    // skeleton model and an unknown bone are script errors that answer nothing, never the object's own position.
    method("Get_Bone_Position", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 1)
            return object_call_error("Get_Bone_Position", "invalid number of parameters.  Expected 1, got " + std::to_string(count) + ".");
        const std::string* name = as_text(arguments[1]);
        if (name == nullptr) return object_call_error("Get_Bone_Position", "invalid type for parameter 1.  Expected string.");
        const AiType* type = host->type(unit->type);
        const std::string type_name = type != nullptr ? type->name : std::string("object");
        if (type == nullptr || type->bones.empty()) return object_call_error("Get_Bone_Position", "object " + type_name + " has no model");
        const std::string wanted = upper(*name);
        const auto bone = std::find_if(type->bones.begin(), type->bones.end(),
            [&](const auto& row) { return upper(row.first) == wanted; });
        if (bone == type->bones.end())
            return object_call_error("Get_Bone_Position", "could not find named bone " + *name + " in model for object " + type_name);
        const auto frame = math::to_matrix(unit->rotation, unit->position);
        const auto world = frame ? math::transform_point(frame.value(), bone->second) : core::Result<math::Vec3>::failure(frame.error());
        if (!world) return object_call_error("Get_Bone_Position", "bone position out of range");
        return one(position_object(world.value()));
    });

    // TL-08: Prevent_AI_Usage(flag). Without an AI player on the owner the original warns and does nothing; with one
    // it takes the object out of (true) or back into (false) the owner's free store: a replayed command (opcode 50)
    // sets the unit's hashed flag, and the AI's unit selection skips a flagged unit.
    method("Prevent_AI_Usage", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() < 2 || !std::holds_alternative<bool>(arguments[1].data))
            return object_call_error("Prevent_AI_Usage", "Expected bool parameter.");
        if (host->engine == nullptr || !host->engine->runs_player(unit->owner)) {
            context.report(authoritative::codes::script_warning,
                "Prevent_AI_Usage -- No AI Player active on the owner of this object.");
            return none();
        }
        // The flag is simulation state (the AI's unit selection reads it): one replayed command, applied next tick.
        context.issue_command(std::string(verb_prevent_ai_usage),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
}

} // namespace eawr::script::foc::tactical_ai_detail
