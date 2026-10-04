// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "tactical_ai_internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <utility>

namespace eawr::script::foc {
namespace tactical_ai_detail {

namespace tactical = sim::tactical;
namespace math = sim::math;
using authoritative::Binding;
using authoritative::BindingContext;
using authoritative::Handle;
using authoritative::ScriptEvent;
using authoritative::ScriptScheduler;
using authoritative::Value;
using authoritative::ValueList;
using numeric::LuaNumber;

using namespace detail;

namespace {

Binding missing(std::string name) {
    return [name = std::move(name)](BindingContext&, const ValueList&) -> core::Result<ValueList> {
        return core::Result<ValueList>::failure(make_error(authoritative::codes::missing_api,
            name + " is a FoC engine API this host does not implement (#79, the #78 fallback trigger)"));
    };
}

} // namespace

Binding noop() {
    return [](BindingContext&, const ValueList&) { return none(); };
}

namespace {

void issue_move(BindingContext& context, const ViewUnit& unit, const math::Vec3& target) {
    ValueList arguments;
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(unit.owner))));
    arguments.push_back(handle(handle_game_object, unit.id));
    arguments.push_back(number(numeric::from_fixed(target.x)));
    arguments.push_back(number(numeric::from_fixed(target.y)));
    arguments.push_back(number(numeric::from_fixed(target.z)));
    context.issue_command(std::string(verb_move), std::move(arguments));
}

// FH-20: a properties or category mask names one or more values separated by '|', spaces,
// commas, tabs or newlines ("Structure | Capital"), each case-insensitive; the mask is their
// union, and one unknown name fails the whole mask.
std::optional<std::uint64_t> mask_of(const std::map<std::string, std::uint64_t, std::less<>>& values, std::string_view text) {
    const std::vector<std::string> names = ai::split_names(text, "| ,\t\n");
    if (names.empty()) return std::nullopt;
    std::uint64_t mask = 0;
    for (const std::string& name : names) {
        const auto found = values.find(upper(name));
        if (found == values.end()) return std::nullopt;
        mask |= found->second;
    }
    return mask;
}

// FH-20: Find_Nearest.
core::Result<ValueList> find_nearest(const Host& host, const ValueList& arguments) {
    if (arguments.empty() || arguments.size() > 4) return fail("Find_Nearest expects 1 to 4 arguments");
    // Parameter 1: a game object, an AI target or a TaskForce (#449), the source position; an
    // object source is left out of the search.
    std::optional<math::Vec3> origin;
    sim::EntityId excluded{};
    if (const ViewUnit* object = live_object(host, arguments[0])) {
        origin = object->position;
        excluded = object->id;
    } else if (const Handle* target = as_handle(arguments[0], ai::handle_ai_target); target != nullptr && host.engine != nullptr) {
        if (const ai::Target* entry = host.engine->target(target->id)) {
            if (entry->object != 0) {
                if (const ViewUnit* unit = host.view->find(entry->object)) {
                    origin = unit->position;
                    excluded = unit->id;
                }
            } else {
                origin = math::Vec3{};
                if (auto x = numeric::to_fixed(entry->x)) origin->x = x.value();
                if (auto y = numeric::to_fixed(entry->y)) origin->y = y.value();
            }
        }
    } else if (const Handle* taskforce = as_handle(arguments[0], ai::handle_taskforce); taskforce != nullptr && host.engine != nullptr) {
        if (const ai::TaskForce* entry = host.engine->taskforce(taskforce->id)) {
            ai::Real x{}, y{}, z{};
            std::int64_t count = 0;
            for (const sim::EntityId member : entry->units) {
                const ViewUnit* unit = host.view->find(member);
                if (unit == nullptr) continue;
                x = x + numeric::from_fixed(unit->position.x);
                y = y + numeric::from_fixed(unit->position.y);
                z = z + numeric::from_fixed(unit->position.z);
                ++count;
            }
            origin = math::Vec3{};
            if (count > 0) {
                if (auto value = numeric::to_fixed(ai::to_single(x / ai::real(count)))) origin->x = value.value();
                if (auto value = numeric::to_fixed(ai::to_single(y / ai::real(count)))) origin->y = value.value();
                if (auto value = numeric::to_fixed(ai::to_single(z / ai::real(count)))) origin->z = value.value();
            }
        }
    }
    if (!origin) return fail("Find_Nearest - parameter 1 is already dead; cannot extract a position from it.");
    const AiType* type_filter = nullptr;
    std::uint64_t property_mask = 0;
    std::uint64_t category_mask = ~std::uint64_t{0};
    std::optional<tactical::PlayerId> player_filter;
    bool ally = true;
    std::size_t next = 1;
    if (arguments.size() > 1) {
        if (const std::string* name = as_text(arguments[1])) {
            const std::string key = upper(*name);
            if (const auto properties = mask_of(host.setup.content.properties, *name)) {
                property_mask = *properties;
            } else if (const auto categories = mask_of(host.setup.content.categories, *name)) {
                category_mask = *categories;
            } else if (const auto type = host.types_by_name.find(key); type != host.types_by_name.end()) {
                type_filter = type->second;
            } else {
                return fail("Find_Nearest: unknown game object type, properties mask or category " + *name);
            }
            next = 2;
        }
        if (next + 1 < arguments.size()) {
            if (const Handle* player = as_handle(arguments[next], handle_player)) player_filter = static_cast<tactical::PlayerId>(player->id);
            if (const bool* flag = std::get_if<bool>(&arguments[next + 1].data)) ally = *flag;
        }
    }
    const ViewUnit* nearest = nullptr;
    std::optional<math::Fixed> best;
    for (const ViewPlayer& player : host.view->players) {
        if (host.neutral(player.id)) continue;
        if (player_filter && host.allied(player.id, *player_filter) != ally) continue;
        for (const ViewUnit& unit : host.view->units) {
            if (unit.owner != player.id || unit.id == excluded) continue;
            const AiType* type = host.type(unit.type);
            if (type_filter != nullptr && type != type_filter) continue;
            const std::uint64_t properties = type != nullptr ? type->property_bits : 0;
            const std::uint64_t categories = type != nullptr ? type->category_bits : 0;
            if (property_mask != 0 && (property_mask & properties) == 0) continue;
            if ((category_mask & categories) == 0) continue;
            // Is_Fogged for the filter player: an AI player sees everything (SK-45).
            if (player_filter && !host.ai(*player_filter)) {
                const ViewPlayer* viewer = host.view->player(*player_filter);
                if (viewer == nullptr || (unit.visible_to & (std::uint64_t{1} << viewer->snapshot_index)) == 0) continue;
            }
            const auto length = distance(*origin, unit.position);
            if (!length) continue;
            if (!best || *length < *best) {
                best = length;
                nearest = &unit;
            }
        }
    }
    if (nearest == nullptr) return none();
    return one(handle(handle_game_object, nearest->id));
}

// FH-22 EvaluatePerception: the equations the freestore evaluates.
core::Result<ValueList> evaluate_perception(const Host& host, BindingContext& context, const ValueList& arguments) {
    if (arguments.size() < 2 || as_text(arguments[0]) == nullptr) return fail("EvaluatePerception expects a name and a player");
    if (host.engine != nullptr) {
        // PE-30: the equation for the player, with the object or AI target as Target.
        const Handle* player = as_handle(arguments[1], handle_player);
        if (player == nullptr) return fail("EvaluatePerception: parameter 2 is not a player");
        const ai::Target* target = nullptr;
        if (arguments.size() > 2) {
            if (const Handle* object = as_handle(arguments[2], handle_game_object)) {
                target = host.engine->target_of_object(object->id);
            } else if (const Handle* location = as_handle(arguments[2], ai::handle_ai_target)) {
                target = host.engine->target(location->id);
            }
        }
        const auto value = host.engine->evaluate(*as_text(arguments[0]), static_cast<tactical::PlayerId>(player->id), target);
        if (!value) {
            context.report(authoritative::codes::unsupported_api, "EvaluatePerception(" + *as_text(arguments[0]) + ") failed; 0");
            return one(number(LuaNumber(0)));
        }
        return one(number(*value));
    }
    const std::string name = upper(*as_text(arguments[0]));
    const AiPerception& perception = host.setup.perception;
    if (name == "ALLOWED_AS_DEFENDER_LAND") {
        // basiclandequations.xml: (IsCampaignGame == 0) + (IsCampaignGame == 1) * ((1 - IsDefender)
        // + IsDefender * (BaseLevel == 0) + IsDefender * (BaseLevel > 0) * (force test)).
        if (!perception.campaign_game || !perception.defender || perception.base_level == 0) {
            return one(number(LuaNumber(1)));
        }
        context.report(authoritative::codes::unsupported_api,
            "EvaluatePerception(Allowed_As_Defender_Land) needs Variable_Self.FriendlyForceUnnormalized; the force term is 0");
        return one(number(LuaNumber(0)));
    }
    context.report(authoritative::codes::unsupported_api, "EvaluatePerception(" + *as_text(arguments[0]) + ") returned 0: no perception equation evaluator");
    return one(number(LuaNumber(0)));
}

} // namespace

void register_globals(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto add = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    add("Get_Game_Mode", [](BindingContext&, const ValueList&) { return one(Value::text("Space")); });
    add("Find_Nearest", [host](BindingContext&, const ValueList& arguments) { return find_nearest(*host, arguments); });
    add("EvaluatePerception", [host](BindingContext& context, const ValueList& arguments) {
        return evaluate_perception(*host, context, arguments);
    });
    add("Find_Object_Type", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 1 || as_text(arguments[0]) == nullptr) return fail("Find_Object_Type expects a name");
        const auto found = host->types_by_name.find(upper(*as_text(arguments[0])));
        if (found == host->types_by_name.end()) return none();
        return one(handle(handle_type, found->second->type_id));
    });
    add("Script.Debug_Should_Issue_Event_Alert", [](BindingContext&, const ValueList&) { return one(boolean(false)); });
    // Debug output only (PGDebug): no effect on the game.
    for (const char* name : {"_OuputDebug", "_ScriptMessage", "_MessagePopup", "DumpCallStack"}) add(name, noop());
    add("FogOfWar.Reveal_All", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        // V-20: tactical only, at least one argument, a player, and no returned value.
        if (!host->view) return fail("FogOfWar.Reveal_All: command only valid in a tactical game");
        if (arguments.empty()) return fail("FogOfWar.Reveal_All: requires a player argument");
        const Handle* player = as_handle(arguments[0], handle_player);
        if (player == nullptr || player->id > std::numeric_limits<tactical::PlayerId>::max()
            || host->view->player(static_cast<tactical::PlayerId>(player->id)) == nullptr)
            return fail("FogOfWar.Reveal_All: expected a player object as first argument");
        auto issuer = static_cast<tactical::PlayerId>(player->id);
        // Project routing: retain the plan owner as issuer even when it reveals for the local player.
        if (host->engine != nullptr) {
            if (const ai::Plan* plan = host->engine->plan_of_instance(context.instance())) issuer = plan->player;
        }
        if (host->world != nullptr) {
            const auto players = host->world->players();
            const auto found = std::find_if(players.begin(), players.end(),
                [&](const tactical::Player& entry) { return entry.player_id == issuer && entry.commandable(); });
            if (found == players.end()) {
                const auto commandable = std::find_if(players.begin(), players.end(),
                    [](const tactical::Player& entry) { return entry.commandable(); });
                if (commandable == players.end()) return fail("FogOfWar.Reveal_All: tactical game has no command issuer");
                issuer = commandable->player_id;
            }
        }
        context.issue_command(std::string(verb_reveal_all),
            {number(LuaNumber(static_cast<std::int64_t>(issuer))), arguments[0]});
        return none();
    });
    register_plan_bindings(scripts, host, errors);
    // A plan's definition load (PGAICommands Set_Contrast_Values) builds its contrast lists. The
    // lists feed the goal system's contrast sizing, which is not hosted: the handle is opaque
    // and Parse only checks its arguments (the host's own contrast weights, FH-30, come from
    // the same function through load_contrast).
    add("WeightedTypeList.Create", [](BindingContext&, const ValueList&) { return one(handle(handle_type_list, 1)); });
}

void register_methods(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors) {
    const auto object = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_game_object, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    const auto player = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_player, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    const auto type = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_type, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // Arguments start with the handle itself. A method on a dead object gives nothing (the
    // wrappers assert and return no value).
    object("Is_Valid", [host](BindingContext&, const ValueList& arguments) {
        return one(boolean(live_object(*host, arguments[0]) != nullptr));
    });
    object("Get_Type", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(handle(handle_type, unit->type));
    });
    object("Get_Owner", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(handle(handle_player, unit->owner));
    });
    object("Get_Position", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(position_value(unit->position));
    });
    object("Get_Distance", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || arguments.size() < 2) return none();
        const auto other = position_of(*host, arguments[1]);
        if (!other) return fail("Get_Distance: parameter 1 is not a live object or a position");
        const auto length = distance(unit->position, *other);
        if (!length) return fail("Get_Distance: distance out of range");
        return one(number(numeric::from_fixed(*length)));
    });
    const auto has_bits = [host](bool categories) {
        return [host, categories](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
            const ViewUnit* unit = live_object(*host, arguments[0]);
            if (unit == nullptr) return none();
            if (arguments.size() != 2 || as_text(arguments[1]) == nullptr) return fail("expects one name");
            const auto& names = categories ? host->setup.content.categories : host->setup.content.properties;
            const auto bit = names.find(upper(*as_text(arguments[1])));
            if (bit == names.end()) return fail("unknown name " + *as_text(arguments[1]));
            const AiType* type = host->type(unit->type);
            const std::uint64_t bits = type == nullptr ? 0 : (categories ? type->category_bits : type->property_bits);
            return one(boolean((bits & bit->second) != 0));
        };
    };
    object("Is_Category", has_bits(true));
    object("Has_Property", has_bits(false));
    object("Get_Attack_Target", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const auto target = unit->formation_target != 0 ? unit->formation_target : unit->attack_target;
        if (target == 0 || host->view->find(target) == nullptr) return none();
        return one(handle(handle_game_object, target));
    });
    object("Get_Hull", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (!unit->hull || !unit->max_hull || unit->max_hull->raw() <= 0) return one(number(LuaNumber(1)));
        auto fraction = math::divide(*unit->hull, *unit->max_hull);
        if (!fraction) return one(number(LuaNumber(1)));
        return one(number(numeric::from_fixed(fraction.value())));
    });
    object("Has_Active_Orders", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const auto target = unit->formation_target != 0 ? unit->formation_target : unit->attack_target;
        if (target != 0 && host->view->find(target) != nullptr) return one(boolean(true));
        if (unit->container) {
            return one(boolean(unit->formation_moving ||
                (unit->order == tactical::OrderKind::attack && host->view->find(unit->order_target) != nullptr)));
        }
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->locomotor) return one(boolean(false));
        return one(boolean(unit->moving || unit->formation_moving));
    });
    object("Is_Good_Against", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const ViewUnit* target = arguments.size() == 2 ? live_object(*host, arguments[1]) : nullptr;
        if (target == nullptr) return fail("Is_Good_Against: target is already dead");
        const AiType* self = host->type(unit->type);
        const AiType* other = host->type(target->type);
        LuaNumber best(0);
        if (self != nullptr && other != nullptr) {
            for (int bit = 0; bit < 64; ++bit) {
                const std::uint64_t category = std::uint64_t{1} << bit;
                if ((other->category_bits & category) == 0) continue;
                const LuaNumber factor = average_contrast(*host, *self, category);
                if (best < factor) best = factor;
            }
        }
        return one(boolean(LuaNumber(1) < best));
    });
    object("Activate_Ability", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() < 2 || as_text(arguments[1]) == nullptr) return fail("Activate_Ability expects an ability name");
        request_ability(context, *host, *unit, upper(*as_text(arguments[1])), arguments.size() >= 3 ? &arguments[2] : nullptr);
        return none();
    });
    object("Should_Switch_Weapons", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const AiType* type = host->type(unit->type);
        // FH-25: fewer than two projectile types never switch.
        if (type == nullptr || type->projectile_types < 2) return one(boolean(false));
        context.report(authoritative::codes::unsupported_api, "Should_Switch_Weapons returned false: armour tests are not modelled");
        return one(boolean(false));
    });
    // FH-26: no M2 unit has a garrison (m2-skirmish.md, Q1 notes).
    object("Get_Garrisoned_Units", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        return one(Value{std::vector<Value>{}});
    });
    // Orders (FH-40): the next tick's replay commands.
    const auto move_like = [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() < 2) return fail("expects a target");
        const auto target = unit_destination(*host, *unit, arguments[1], false, false);
        if (!target) return fail("the target is not a live object, AI target, TaskForce or position");
        issue_move(context, *unit, target->position);
        return none();
    };
    object("Move_To", move_like);
    // FH-40, space-orders OR-12 and OR-14: an attack-move or guard of a live object keeps the
    // object as its target; any other position is an attack-move or guard of that point.
    const auto ordered = [host](std::string_view verb) {
        return [host, verb](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            const ViewUnit* unit = live_object(*host, arguments[0]);
            if (unit == nullptr) return none();
            if (arguments.size() < 2) return fail("expects a target");
            ValueList command;
            command.push_back(number(LuaNumber(static_cast<std::int64_t>(unit->owner))));
            command.push_back(handle(handle_game_object, unit->id));
            const auto target = unit_destination(*host, *unit, arguments[1], true, verb == verb_guard);
            if (!target) return fail("the target is not a live object, AI target, TaskForce or position");
            if (target->object != 0) {
                command.push_back(handle(handle_game_object, target->object));
            } else {
                command.push_back(number(numeric::from_fixed(target->position.x)));
                command.push_back(number(numeric::from_fixed(target->position.y)));
                command.push_back(number(numeric::from_fixed(target->position.z)));
            }
            context.issue_command(std::string(verb), std::move(command));
            return none();
        };
    };
    object("Attack_Move", ordered(verb_attack_move));
    object("Guard_Target", ordered(verb_guard));
    object("Attack_Target", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const ViewUnit* target = arguments.size() >= 2 ? destination_object(*host, arguments[1]) : nullptr;
        if (target == nullptr) return fail("Attack_Target: the target is not a live object");
        ValueList command;
        command.push_back(number(LuaNumber(static_cast<std::int64_t>(unit->owner))));
        command.push_back(handle(handle_game_object, unit->id));
        command.push_back(handle(handle_game_object, target->id));
        context.issue_command(std::string(verb_attack), std::move(command));
        return none();
    });
    // #449 plan needs. GameObject.Get_Shield: the shield fraction (0 without shields).
    object("Get_Shield", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(number(unit->shield));
    });
    // FH-24 (#76): the ability calls read the snapshot's ability status (AB-44); a cut ability
    // (HUNT; space-abilities.md AB-03) exists but is never ready or active.
    const auto ability_query = [host](std::string name, int field) {
        return [host, name = std::move(name), field](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            const ViewUnit* unit = live_object(*host, arguments[0]);
            if (unit == nullptr) return none();
            if (arguments.size() < 2 || as_text(arguments[1]) == nullptr) return fail(name + " expects an ability name");
            const std::string ability = upper(*as_text(arguments[1]));
            const auto kind = tactical::ability_kind(ability);
            if (kind == tactical::AbilityKind::none) {
                const bool has = authored_ability(*host, *unit, ability);
                if (has && field != 0) {
                    context.report(authoritative::codes::unsupported_api, name + "(" + ability + ") returned false: the ability is cut (#76)");
                }
                return one(boolean(field == 0 && has));
            }
            const auto view = ability_view(*host, *unit, kind);
            const bool values[] = {view.has, view.has && view.ready, view.has && view.active, view.has && view.autofire};
            return one(boolean(values[field]));
        };
    };
    object("Has_Ability", ability_query("Has_Ability", 0));
    object("Is_Ability_Ready", ability_query("Is_Ability_Ready", 1));
    object("Is_Ability_Active", ability_query("Is_Ability_Active", 2));
    object("Is_Ability_Autofire", ability_query("Is_Ability_Autofire", 3));
    object("Has_Attack_Target", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(boolean(unit->attack_target != 0 && host->view->find(unit->attack_target) != nullptr));
    });
    object("Divert", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || arguments.size() < 2) return none();
        const auto target = position_of(*host, arguments[1]);
        if (!target) return one(boolean(false));
        issue_move(context, *unit, *target);
        return one(boolean(true));
    });
    // Diversions and weather are not simulated in M2 (fidelity list): nothing is in a field.
    for (const char* name : {"Is_On_Diversion", "Is_In_Asteroid_Field", "Is_In_Ion_Storm", "Is_In_Nebula", "Can_Garrison"}) {
        object(name, [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
            if (live_object(*host, arguments[0]) == nullptr) return none();
            return one(boolean(false));
        });
    }
    for (const char* name : {"Lock_Current_Orders", "Service_Wrapper", "Event_Object_In_Range", "Get_Current_Projectile_Type"}) {
        object(name, [](BindingContext&, const ValueList&) { return none(); });
    }
    // DT-04: the time-to-death estimate is not tracked here; a unit is never about to die.
    object("Get_Time_Till_Dead", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        return one(number(LuaNumber(1000000)));
    });
    object("Get_Rate_Of_Damage_Taken", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        return one(number(LuaNumber(0)));
    });
    // FH-27: a star base fires its special weapon only through a hardpoint that takes a manual
    // target; without one the call is a script warning and answers nil. No M2 station has one
    // (fidelity list), so the station answers nil; any other object is still the missing API.
    object("Fire_Special_Weapon", [host, fallback = missing("GameObject.Fire_Special_Weapon")](
                                      BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        const AiType* type = unit != nullptr ? host->type(unit->type) : nullptr;
        if (type != nullptr && type->star_base) return none();
        return fallback(context, arguments);
    });
    object("Get_Build_Pad_Contents", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || host->world == nullptr) return none();
        const auto pad = host->world->pads().find(unit->id);
        if (pad == host->world->pads().end()) return none();
        const auto child = pad->second.constructed != 0 ? pad->second.constructed : pad->second.under_construction;
        return child != 0 ? one(handle(handle_game_object, child)) : none();
    });
    for (const char* name : {"Garrison", "Leave_Garrison", "Get_Parent_Object",
             "Get_Combat_Rating", "Get_All_Projectile_Types"}) {
        object(name, missing(std::string("GameObject.") + name));
    }

    player("Get_ID", [](BindingContext&, const ValueList& arguments) {
        return one(number(LuaNumber(static_cast<std::int64_t>(std::get<Handle>(arguments[0].data).id))));
    });
    player("Get_Faction_Name", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto found = host->players.find(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        if (found == host->players.end()) return none();
        return one(Value::text(found->second->faction));
    });
    // FH-21 Get_Space_Station: the first star base of an allied player.
    player("Get_Space_Station", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        for (const ViewPlayer& entry : host->view->players) {
            if (!host->allied(entry.id, self)) continue;
            for (const ViewUnit& unit : host->view->units) {
                const AiType* type = host->type(unit.type);
                if (unit.owner == entry.id && type != nullptr && type->star_base) return one(handle(handle_game_object, unit.id));
            }
        }
        return none();
    });
    // AI-G03: Normal difficulty.
    player("Get_Difficulty", [](BindingContext&, const ValueList&) { return one(Value::text("Normal")); });
    player("Is_Human", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        const auto found = host->players.find(self);
        return one(boolean(found != host->players.end() && !found->second->ai && !found->second->neutral));
    });
    player("Is_Enemy", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        const Handle* other = arguments.size() > 1 ? as_handle(arguments[1], handle_player) : nullptr;
        if (other == nullptr) return fail("Is_Enemy expects a player");
        const auto id = static_cast<tactical::PlayerId>(other->id);
        return one(boolean(!host->neutral(id) && !host->neutral(self) && !host->allied(self, id)));
    });
    player("Get_Enemy", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        for (const AiPlayer& other : host->setup.players) {
            if (!other.neutral && !host->allied(self, other.player)) return one(handle(handle_player, other.player));
        }
        return none();
    });
    player("Get_Credits", [host](BindingContext&, const ValueList& arguments) {
        const auto* account = host->economy(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        return one(number(account != nullptr ? numeric::from_fixed(account->credits) : LuaNumber{}));
    });
    player("Get_Tech_Level", [host](BindingContext&, const ValueList& arguments) {
        const auto* account = host->economy(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        return one(number(LuaNumber(account != nullptr ? account->tech_level : 0)));
    });
    player("Give_Money", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2 || std::get_if<LuaNumber>(&arguments[1].data) == nullptr) return fail("Give_Money expects a credit amount");
        const auto amount = numeric::to_fixed(std::get<LuaNumber>(arguments[1].data));
        if (!amount || amount.value().raw() <= 0) return fail("Give_Money expects positive credits");
        const auto player = std::get<Handle>(arguments[0].data).id;
        context.issue_command(std::string(verb_credit_grant), {number(LuaNumber(static_cast<std::int64_t>(player))),
            handle(handle_game_object, 0), arguments[1]});
        return none();
    });
    type("Get_Name", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        return one(Value::text(found->name));
    });
    type("Get_Max_Range", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        return one(number(found->max_attack_distance));
    });
    type("Get_Min_Range", [](BindingContext&, const ValueList&) { return one(number(LuaNumber(0))); });
    // No M2 type is a hero, and no projectile is affected by the defensive abilities.
    for (const char* name : {"Is_Hero", "Is_Affected_By_Laser_Defense", "Is_Affected_By_Missile_Shield"}) {
        type(name, [](BindingContext&, const ValueList&) { return one(boolean(false)); });
    }
    if (auto added = scripts.register_method(handle_type_list, "Parse", [](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
            const auto* names = arguments.size() == 3 ? std::get_if<std::vector<Value>>(&arguments[1].data) : nullptr;
            const auto* weights = arguments.size() == 3 ? std::get_if<std::vector<Value>>(&arguments[2].data) : nullptr;
            if (names == nullptr || weights == nullptr || names->size() != weights->size()) {
                return fail("WeightedTypeList.Parse expects two lists of one length");
            }
            return none();
        });
        !added) {
        errors.push_back(added.error());
    }
}

} // namespace tactical_ai_detail

} // namespace eawr::script::foc
