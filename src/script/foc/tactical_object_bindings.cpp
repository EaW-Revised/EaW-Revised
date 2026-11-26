// Battle-only object methods of the tactical script host (docs/behaviour/foc-tactical-ai.md, rules TL-04..TL-08).
// Each method validates its arguments like the original and then either applies its effect or, where the battle has
// nothing to apply it to yet, accepts the call and records an unsupported_api note (the rule says which).

#include "tactical_ai_internal.hpp"
#include "discrete_distribution.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
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

// The shared staging of a script creation: validates, then queues one ordered, replayed creation command and answers
// its base reference (the staged objects take base + k; see pending_created_objects). `units` selects what the
// creation may name: false, the original factory's footprint types and ordinary upgrade objects; true, also a squadron
// or lone craft type (the ordered commit accepts these on purpose: it is wider than the original factory pre-check).
core::Result<std::uint64_t> stage_script_creation(const HostPtr& host, BindingContext& context, const std::string& verb,
    const AiType* type, const std::optional<sim::math::Vec3>& position, const Handle* owner, const bool units) {
    const auto refuse = [](std::string message) { return core::Result<std::uint64_t>::failure(fail(std::move(message)).error()); };
    if (!type || !position || !owner || owner->id > std::numeric_limits<tactical::PlayerId>::max()
        || !host->players.contains(static_cast<tactical::PlayerId>(owner->id)))
        return refuse(verb + ": invalid type, position or player");
    const auto* profile = host->world ? host->world->economy().upgrade(type->type_id) : nullptr;
    // An ordinary upgrade object is a hidden economy object; any other data-defined object with a footprint is a
    // unit the ordered commit places at the nearest free spot, as a reinforcement is.
    const bool upgrade = type->upgrade_object && profile != nullptr && !profile->level_up && profile->bounties.empty();
    const bool unit = !type->upgrade_object && host->world != nullptr
        && (host->world->economy().footprint_of(type->type_id) != nullptr
            || (units && (host->world->motion().squadrons.find_squadron(type->type_id) != nullptr
                          || host->world->motion().squadrons.find_craft(type->type_id) != nullptr)));
    if (host->setup.domain != tactical::BattleDomain::space || !(upgrade || unit)
        || host->world->economy().disabled_types.contains(type->type_id))
        return refuse(verb + ": this host supports ordinary space upgrade objects and space units only");
    const auto layout = host->world->motion().squadrons.spawn_layout(type->type_id);
    if (!layout || layout->members == 0 || layout->members > tactical::script_reference_member_limit
        || layout->teams.size() > tactical::script_reference_member_limit - layout->members)
        return refuse(verb + ": creation has an invalid or oversized squadron layout");
    const auto player = static_cast<tactical::PlayerId>(owner->id);
    const auto issuer = host->players.find(player);
    if (issuer == host->players.end() || issuer->second->neutral)
        return refuse(verb + ": no commandable creation owner");
    // The ordered commit needs the owner's economy account (a defeated or account-less seat has none); a missing one
    // would abort the tick after the wrapper is returned, so it is a Lua error here.
    const auto ledgers = host->world->ledgers();
    if (!host->world->economy().player(player)
        || std::none_of(ledgers.begin(), ledgers.end(), [player](const tactical::PlayerEconomy& account) { return account.player == player; }))
        return refuse(verb + ": the creation owner has no economy account");
    // Project reference namespace: bounded, injective (instance, command sequence), retained in replay/state.
    constexpr unsigned sequence_bits = 20;
    constexpr unsigned low_bits = sequence_bits + tactical::script_reference_member_bits;
    if (context.instance() >= (std::uint64_t{1} << (63 - low_bits))
        || context.command_sequence() >= (std::uint64_t{1} << sequence_bits))
        return refuse(verb + ": script creation identity space exhausted");
    const auto identity = tactical::script_creation_namespace | (context.instance() << low_bits)
        | (context.command_sequence() << tactical::script_reference_member_bits);
    if (identity == tactical::script_creation_namespace) return refuse(verb + ": invalid creation identity");
    context.issue_command(std::string(verb_create_object), {number(LuaNumber(static_cast<std::int64_t>(player))),
        handle(handle_type, type->type_id), position_value(*position), handle(handle_player, owner->id), handle(handle_game_object, identity)});
    return core::Result<std::uint64_t>::success(identity);
}

const AiType* named_type(const Host& host, const Value& value) {
    if (const auto* name = as_text(value)) {
        const auto found = host.types_by_name.find(upper(*name));
        return found != host.types_by_name.end() ? found->second : nullptr;
    }
    const auto* value_handle = as_handle(value, handle_type);
    return value_handle != nullptr ? host.type(value_handle->id) : nullptr;
}

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
    const auto pending_scoped = [](Binding binding) -> Binding {
        // Lookups inside the call see the caller's own staged creations.
        return [call = std::move(binding)](BindingContext& context, const ValueList& arguments) {
            const PendingScope scope(context);
            return call(context, arguments);
        };
    };
    const auto method = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_game_object, name, pending_scoped(std::move(binding))); !added) errors.push_back(added.error());
    };
    const auto global = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_binding(name, pending_scoped(std::move(binding))); !added) errors.push_back(added.error());
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

    // Spawn_Unit(type, position, player): the type is an object type handle (a name string is a script error, as in the
    // original), the position anything that gives one, the player a player. The unit, or every member of a squadron or
    // company, is created at the nearest free spot to the position and answered as a list. The objects are created by the
    // ordered, replayed creation command at the next tick; the returned handles answer queries and take commands in this
    // script step (see pending_created_objects).
    global("Spawn_Unit", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 3) return fail("Spawn_Unit expects type, position and player");
        const auto* kind = as_handle(arguments[0], handle_type);
        if (kind == nullptr) return fail("Spawn_Unit: parameter 1 is not an object type");
        auto position = position_of(*host, arguments[1]);
        if (!position) if (const auto pending = pending_created_object(*host, context, arguments[1])) position = pending->position;
        const auto identity = stage_script_creation(host, context, "Spawn_Unit", host->type(kind->id), position, as_handle(arguments[2], handle_player), true);
        if (!identity) return core::Result<ValueList>::failure(identity.error());
        std::vector<Value> made;
        for (const ViewUnit& unit : pending_created_objects(*host, context))
            if (unit.id >= identity.value() && unit.id < identity.value() + tactical::script_reference_member_limit && !unit.container)
                made.push_back(handle(handle_game_object, unit.id));
        return one(Value{std::move(made)});
    });
    // Spawn_Special_Weapon(type, player): exactly these two arguments, the type a name string. The object stands at the
    // battle's special-weapon source (the first source object in map data order; the origin when the map has none) and
    // the call answers that one object.
    global("Spawn_Special_Weapon", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2) return fail("Spawn_Special_Weapon expects type and player");
        if (as_text(arguments[0]) == nullptr) return fail("Spawn_Special_Weapon: parameter 1 is not a type name");
        const sim::math::Vec3 source = host->setup.special_weapon_source.value_or(sim::math::Vec3{});
        const auto identity = stage_script_creation(host, context, "Spawn_Special_Weapon", named_type(*host, arguments[0]), source,
            as_handle(arguments[1], handle_player), true);
        if (!identity) return core::Result<ValueList>::failure(identity.error());
        return one(handle(handle_game_object, identity.value()));
    });
}

} // namespace eawr::script::foc::tactical_ai_detail
