// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "ai_engine.hpp"
#include "host.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <optional>
#include <set>
#include <utility>

namespace eawr::script::foc {
namespace {

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

// ---- Bindings ---------------------------------------------------------------------------

using HostPtr = std::shared_ptr<Host>;

Binding missing(std::string name) {
    return [name = std::move(name)](BindingContext&, const ValueList&) -> core::Result<ValueList> {
        return core::Result<ValueList>::failure(make_error(authoritative::codes::missing_api,
            name + " is a FoC engine API this host does not implement (#79, the #78 fallback trigger)"));
    };
}

Binding noop() {
    return [](BindingContext&, const ValueList&) { return none(); };
}

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
    add("FogOfWar.Reveal_All", missing("FogOfWar.Reveal_All"));
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
        if (unit == nullptr || unit->attack_target == 0 || host->view->find(unit->attack_target) == nullptr) return none();
        return one(handle(handle_game_object, unit->attack_target));
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
        if (unit->attack_target != 0 && host->view->find(unit->attack_target) != nullptr) return one(boolean(true));
        if (unit->container) {
            return one(boolean(unit->order == tactical::OrderKind::attack && host->view->find(unit->order_target) != nullptr));
        }
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->locomotor) return one(boolean(false));
        return one(boolean(unit->moving));
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
        const auto target = position_of(*host, arguments[1]);
        if (!target) return fail("the target is not a live object or a position");
        issue_move(context, *unit, *target);
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
            if (const ViewUnit* target = live_object(*host, arguments[1])) {
                command.push_back(handle(handle_game_object, target->id));
            } else {
                const auto point = position_of(*host, arguments[1]);
                if (!point) return fail("the target is not a live object or a position");
                command.push_back(number(numeric::from_fixed(point->x)));
                command.push_back(number(numeric::from_fixed(point->y)));
                command.push_back(number(numeric::from_fixed(point->z)));
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
        const ViewUnit* target = arguments.size() >= 2 ? live_object(*host, arguments[1]) : nullptr;
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
    for (const char* name : {"Garrison", "Get_Build_Pad_Contents", "Leave_Garrison", "Get_Parent_Object",
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
    player("Get_Credits", [](BindingContext&, const ValueList&) { return one(number(LuaNumber(0))); });
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

// ---- Command translation (FH-40) --------------------------------------------------------

core::Result<authoritative::TacticalOrder> translate_order(const authoritative::ScriptCommand& command) {
    using OrderResult = core::Result<authoritative::TacticalOrder>;
    const auto bad = [&](std::string message) {
        return OrderResult::failure(make_error(authoritative::codes::command_unroutable, command.verb + ": " + std::move(message)));
    };
    const auto& arguments = command.arguments;
    if (arguments.size() < 3) return bad("too few arguments");
    const auto* issuer = std::get_if<LuaNumber>(&arguments[0].data);
    const Handle* unit = as_handle(arguments[1], handle_game_object);
    if (issuer == nullptr || unit == nullptr) return bad("malformed issuer or unit");
    auto player = numeric::to_exact_integer(*issuer);
    if (!player || player.value() < 0) return bad("malformed issuer");
    authoritative::TacticalOrder order;
    order.issuer = static_cast<tactical::PlayerId>(player.value());
    order.units.push_back(unit->id);
    if (command.verb == verb_attack) {
        const Handle* target = as_handle(arguments[2], handle_game_object);
        if (target == nullptr) return bad("malformed target");
        order.payload = tactical::AttackPayload{target->id};
        return OrderResult::success(std::move(order));
    }
    if (command.verb == verb_ability) {
        if (arguments.size() != 4) return bad("expects an ability and an action");
        std::int64_t values[2]{};
        for (std::size_t index = 0; index < 2; ++index) {
            const auto* value = std::get_if<LuaNumber>(&arguments[2 + index].data);
            if (value == nullptr) return bad("malformed ability or action");
            auto exact = numeric::to_exact_integer(*value);
            if (!exact || exact.value() < 1 || exact.value() > 4) return bad("malformed ability or action");
            values[index] = exact.value();
        }
        order.payload = tactical::AbilityPayload{static_cast<tactical::AbilityKind>(values[0]),
            static_cast<tactical::AbilityAction>(values[1])};
        return OrderResult::success(std::move(order));
    }
    const bool attack_move = command.verb == verb_attack_move;
    if ((attack_move || command.verb == verb_guard) && arguments.size() == 3) {
        const Handle* target = as_handle(arguments[2], handle_game_object);
        if (target == nullptr) return bad("malformed target");
        if (attack_move) {
            order.payload = tactical::AttackMovePayload{math::Vec3{}, target->id};
        } else {
            order.payload = tactical::GuardPayload{math::Vec3{}, target->id};
        }
        return OrderResult::success(std::move(order));
    }
    if (arguments.size() != 5) return bad("expects x, y and z");
    math::Vec3 destination;
    math::Fixed* axes[] = {&destination.x, &destination.y, &destination.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto* value = std::get_if<LuaNumber>(&arguments[2 + axis].data);
        if (value == nullptr) return bad("malformed coordinate");
        auto fixed = numeric::to_fixed(*value);
        if (!fixed) return bad(fixed.error().message);
        *axes[axis] = fixed.value();
    }
    if (command.verb == verb_attack_move) {
        order.payload = tactical::AttackMovePayload{destination, sim::invalid_entity_id};
    } else if (command.verb == verb_guard) {
        order.payload = tactical::GuardPayload{destination, sim::invalid_entity_id};
    } else {
        order.payload = tactical::MovePayload{destination};
    }
    return OrderResult::success(std::move(order));
}

// ---- Engine side (FH-10 to FH-13) -------------------------------------------------------

// FH-11: the frame count times the single-precision inverse FPS, then
// widened to the Lua double.
LuaNumber mode_seconds(std::uint64_t frame) {
    const float inverse_fps = 1.0F / static_cast<float>(tactical::logical_frames_per_second);
    const float seconds = static_cast<float>(frame) * inverse_fps;
    return lua_double(static_cast<double>(seconds));
}

const LuaNumber* number_of(const std::optional<Value>& value) {
    return value ? std::get_if<LuaNumber>(&value->data) : nullptr;
}

class FreestoreEngine final : public authoritative::ScriptEngine {
public:
    FreestoreEngine(HostPtr host, std::shared_ptr<ai::Engine> goals) : host_(std::move(host)), goals_(std::move(goals)) {}

    core::Result<void> after_service(authoritative::ServiceReport& report) override {
        if (!goals_) return core::Result<void>::success();
        return goals_->after_service(report);
    }

    core::Result<authoritative::ServiceOptions> before_service(
        const tactical::TacticalSession& world, const tactical::TacticalTick& tick, ScriptScheduler& scripts) override {
        using OptionsResult = core::Result<authoritative::ServiceOptions>;
        host_->view = build_view(world, *tick.snapshot);
        const std::uint64_t next = scripts.completed_tick() + 1;
        const auto instances = scripts.instances();
        std::uint64_t sequence = 0;
        for (const AiPlayer& player : host_->setup.players) {
            if (!player.ai) continue;
            const std::uint64_t instance = freestore_instance(player.player);
            if (!std::binary_search(instances.begin(), instances.end(), instance)) continue;
            std::vector<ScriptEvent> events;
            const auto event = [&](ScriptEvent::Kind kind, std::string name) {
                ScriptEvent out;
                out.key = authoritative::EventKey{next, producer_foc_engine, instance, sequence++};
                out.target = instance;
                out.kind = kind;
                out.name = std::move(name);
                return out;
            };
            const auto assign = [&](std::string name, Value value) {
                ScriptEvent out = event(ScriptEvent::Kind::assign, std::move(name));
                out.parameter = std::move(value);
                events.push_back(std::move(out));
            };
            if (scripts.completed_tick() == 0) {
                // FH-10, on a fresh load, before the first service.
                events.push_back(event(ScriptEvent::Kind::call, "Base_Definitions"));
                events.push_back(event(ScriptEvent::Kind::start_thread, "main"));
                assign("PlayerObject", handle(handle_player, player.player));
                assign("LastService", number(LuaNumber(0)));
                assign("LastUnitService", number(LuaNumber(0)));
            } else {
                // FH-11.
                const LuaNumber now = mode_seconds(world.completed_tick());
                auto read = [&](std::string_view name) { return scripts.read_global(instance, name); };
                auto rate = read("ServiceRate");
                auto last = read("LastService");
                auto unit_rate = read("UnitServiceRate");
                auto last_unit = read("LastUnitService");
                if (!rate || !last || !unit_rate || !last_unit) return OptionsResult::failure(make_error(
                    authoritative::codes::session_abort, "cannot read the freestore service globals"));
                const auto due = [&](const std::optional<Value>& period, const std::optional<Value>& previous) {
                    const LuaNumber* seconds = number_of(period);
                    if (seconds == nullptr) return false;
                    const LuaNumber* since = number_of(previous);
                    return since == nullptr || *seconds < now - *since;
                };
                if (due(rate.value(), last.value())) {
                    events.push_back(event(ScriptEvent::Kind::pump, {}));
                    assign("LastService", number(now));
                }
                if (due(unit_rate.value(), last_unit.value())) {
                    // FH-12: every freestore unit in entity order (retail: hash order).
                    for (const ViewUnit& unit : host_->view->units) {
                        if (!in_freestore(unit, player.player)) continue;
                        ScriptEvent call = event(ScriptEvent::Kind::call, "On_Unit_Service");
                        call.arguments.push_back(handle(handle_game_object, unit.id));
                        events.push_back(std::move(call));
                    }
                    assign("LastUnitService", number(now));
                }
            }
            for (ScriptEvent& out : events) {
                if (auto submitted = scripts.submit_event(std::move(out)); !submitted) return OptionsResult::failure(submitted.error());
            }
        }
        if (goals_) {
            if (auto serviced = goals_->before_service(world, *tick.snapshot, scripts, sequence); !serviced) {
                return OptionsResult::failure(serviced.error());
            }
        }
        authoritative::ServiceOptions options;
        options.host_paced = [](std::uint64_t instance) { return instance >= 1000U; };
        return OptionsResult::success(std::move(options));
    }

private:
    // FH-13: the AI player's own objects that move (BEHAVIOR_LOCO), except
    // squadron members.
    [[nodiscard]] bool in_freestore(const ViewUnit& unit, tactical::PlayerId player) const {
        if (goals_) return goals_->in_freestore(unit, player);
        if (unit.owner != player || unit.craft) return false;
        const AiType* type = host_->type(unit.type);
        return type != nullptr && type->locomotor && !type->star_base;
    }

    HostPtr host_;
    std::shared_ptr<ai::Engine> goals_;
};

// ---- Contrast weights (FH-30) -----------------------------------------------------------

// Runs PGAICommands' Base_Definitions as a plan definition load (PlanDefinitionLoad, which
// calls Set_Contrast_Values) and hands each EnemyContrastTypes entry and its
// FriendlyContrastTypes list to the host. Host glue, not a retail script.
constexpr char contrast_loader_path[] = "EAWR/FOC_CONTRAST_LOADER.LUA";
constexpr char contrast_loader[] = R"LUA(
require("PGAICommands")
PlanDefinitionLoad = true
Base_Definitions()
for index, enemy in pairs(EnemyContrastTypes) do
    _EAWR_Contrast(enemy, FriendlyContrastTypes[index])
end
)LUA";

core::Result<void> load_contrast(Host& host, const std::map<std::string, std::string>& modules) {
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return added;
    }
    if (auto added = manifest.add(contrast_loader_path, contrast_loader); !added) return added;
    authoritative::SessionConfig config;
    config.seed = host.setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/"};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return core::Result<void>::failure(scripts.error());
    struct Lists {
        std::vector<std::pair<std::vector<std::string>, std::vector<LuaNumber>>> lists;
        std::vector<std::pair<std::string, std::uint64_t>> enemies;
    };
    auto lists = std::make_shared<Lists>();
    std::vector<core::Diagnostic> errors;
    const auto check = [&](core::Result<void> added) {
        if (!added) errors.push_back(added.error());
    };
    check(scripts.value().register_binding("WeightedTypeList.Create", [lists](BindingContext&, const ValueList&) {
        lists->lists.emplace_back();
        return one(handle(handle_type_list, lists->lists.size()));
    }));
    check(scripts.value().register_method(handle_type_list, "Parse", [lists](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 3) return fail("Parse expects names and weights");
        const auto* names = std::get_if<std::vector<Value>>(&arguments[1].data);
        const auto* weights = std::get_if<std::vector<Value>>(&arguments[2].data);
        if (names == nullptr || weights == nullptr || names->size() != weights->size()) return fail("Parse expects two lists of one length");
        auto& list = lists->lists.at(std::get<Handle>(arguments[0].data).id - 1);
        list = {};
        for (std::size_t index = 0; index < names->size(); ++index) {
            const std::string* name = as_text((*names)[index]);
            const auto* weight = std::get_if<LuaNumber>(&(*weights)[index].data);
            if (name == nullptr || weight == nullptr) return fail("Parse expects names and numbers");
            list.first.push_back(upper(*name));
            list.second.push_back(*weight);
        }
        return none();
    }));
    check(scripts.value().register_binding("_EAWR_Contrast", [lists](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const Handle* list = arguments.size() == 2 ? as_handle(arguments[1], handle_type_list) : nullptr;
        if (list == nullptr || as_text(arguments[0]) == nullptr) return fail("expects an enemy type and a list");
        lists->enemies.emplace_back(upper(*as_text(arguments[0])), list->id);
        return none();
    }));
    for (const char* name : {"_OuputDebug", "_ScriptMessage", "_MessagePopup", "DumpCallStack"}) {
        check(scripts.value().register_binding(name, noop()));
    }
    if (!errors.empty()) return core::Result<void>::failure(errors.front());
    if (auto created = scripts.value().create_instance(1, contrast_loader_path); !created) return created;
    for (const auto& [enemy, list_id] : lists->enemies) {
        const auto category = host.setup.content.categories.find(enemy);
        if (category == host.setup.content.categories.end() || list_id == 0 || list_id > lists->lists.size()) continue;
        if (std::find(host.contrast_order.begin(), host.contrast_order.end(), category->second) == host.contrast_order.end()) {
            host.contrast_order.push_back(category->second);
        }
        auto& entries = host.contrast[category->second];
        const auto& [names, weights] = lists->lists[list_id - 1];
        for (std::size_t index = 0; index < names.size(); ++index) {
            // FH-30: a name is a category, else an object type.
            ContrastEntry entry;
            entry.weight = weights[index];
            if (const auto bit = host.setup.content.categories.find(names[index]); bit != host.setup.content.categories.end()) {
                entry.category_bits = bit->second;
            } else if (const auto type = host.types_by_name.find(names[index]); type != host.types_by_name.end()) {
                entry.type = type->second->type_id;
            } else {
                continue;
            }
            entries.push_back(entry);
        }
    }
    return core::Result<void>::success();
}

// ---- The goal system (#449) --------------------------------------------------------------

std::string plan_stem(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    std::string name = path.substr(slash == std::string::npos ? 0 : slash + 1);
    if (name.size() > 4 && name.ends_with(".lua")) name.resize(name.size() - 4);
    return name;
}

// PL-10: each selected plan's definition load (PlanDefinitionLoad, Base_Definitions) runs in its
// own instance; host glue hands the globals it leaves to the engine.
core::Result<std::vector<ai::PlanDef>> load_plans(const HostPtr& host, const std::map<std::string, std::string>& modules,
    std::vector<std::string>& notes) {
    using PlansResult = core::Result<std::vector<ai::PlanDef>>;
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return PlansResult::failure(added.error());
    }
    const std::vector<std::string> paths = selected_plans();
    for (const std::string& path : paths) {
        const std::string loader = "require(\"" + plan_stem(path) + "\")\n"
            "PlanDefinitionLoad = true\n"
            "Base_Definitions()\n"
            "_EAWR_Plan(Category, TaskForce, IgnoreTarget, MagicPlan, AllowFreeStoreUnits, AllowEngagedUnits,\n"
            "    PerFailureContrastAdjust, MinContrastScale, MaxContrastScale, RequiredCategories)\n";
        if (auto added = manifest.add("EAWR/PLAN_LOADER/" + upper(plan_stem(path)) + ".LUA", loader); !added) {
            return PlansResult::failure(added.error());
        }
    }
    authoritative::SessionConfig config;
    config.seed = host->setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/", "Data/Scripts/AI/SpaceMode/"};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return PlansResult::failure(scripts.error());
    auto loaded = std::make_shared<std::map<std::uint64_t, ValueList>>();
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (auto added = scripts.value().register_binding("_EAWR_Plan", [loaded](BindingContext& context, const ValueList& arguments) {
            (*loaded)[context.instance()] = arguments;
            return none();
        });
        !added) {
        errors.push_back(added.error());
    }
    if (!errors.empty()) return PlansResult::failure(errors.front());
    std::vector<ai::PlanDef> plans;
    std::uint64_t instance = 1;
    for (const std::string& path : paths) {
        const std::string name = plan_stem(path);
        if (auto created = scripts.value().create_instance(instance, "EAWR/PLAN_LOADER/" + upper(name) + ".LUA"); !created) {
            notes.push_back(name + ": " + created.error().code + " " + created.error().message);
            ++instance;
            continue;
        }
        const auto found = loaded->find(instance);
        ++instance;
        if (found == loaded->end()) {
            notes.push_back(name + ": the definition load left nothing");
            continue;
        }
        auto plan = ai::build_plan(*host, name, path, found->second, notes);
        if (!plan) {
            notes.push_back(plan.error().code + " " + plan.error().message);
            continue;
        }
        plans.push_back(std::move(plan).value());
    }
    return PlansResult::success(std::move(plans));
}

// PE-01 converter constants: category masks, the difficulty levels and hardpoint types.
ai::ConverterFunction converters(const HostPtr& host) {
    return [host](std::string_view converter, std::string_view value) -> std::optional<ai::Real> {
        const std::string name = upper(converter);
        if (name == "GAMEOBJECTCATEGORYTYPE") {
            std::uint64_t bits = 0;
            for (const std::string& part : ai::split_names(value, "|")) {
                const auto found = host->setup.content.categories.find(upper(part));
                if (found == host->setup.content.categories.end()) return std::nullopt;
                bits |= found->second;
            }
            return LuaNumber(static_cast<std::int64_t>(bits));
        }
        if (name == "DIFFICULTYLEVELTYPE") {
            const std::string level = upper(ai::trimmed(value));
            if (level == "EASY") return LuaNumber(0);
            if (level == "NORMAL") return LuaNumber(1);
            if (level == "HARD") return LuaNumber(2);
            return std::nullopt;
        }
        if (name == "HARDPOINTTYPE") return LuaNumber(0); // only HardPointHealth reads it (fidelity list)
        return std::nullopt;
    };
}

} // namespace

std::vector<std::string> required_xml() { return ai::ai_xml_files(); }

std::vector<std::string> required_modules(const AiSetup& setup) {
    std::vector<std::string> paths{setup.freestore_module,
        "Data/Scripts/Library/PGCommands.lua",
        "Data/Scripts/Library/PGBaseDefinitions.lua",
        "Data/Scripts/Library/PGBase.lua",
        "Data/Scripts/Library/PGDebug.lua",
        "Data/Scripts/Library/PGAICommands.lua"};
    if (setup.bounds && !setup.xml.empty()) {
        for (auto list : {plan_library_modules(), selected_plans()}) paths.insert(paths.end(), list.begin(), list.end());
    }
    return paths;
}

core::Result<authoritative::ScriptedTacticalSession> create_session(
    tactical::TacticalSession world, const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    using SessionResult = core::Result<authoritative::ScriptedTacticalSession>;
    auto host = std::make_shared<Host>();
    host->setup = setup;
    for (const AiType& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    for (const AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    if (auto loaded = load_contrast(*host, modules); !loaded) return SessionResult::failure(loaded.error());
    std::shared_ptr<ai::Engine> goals;
    if (setup.bounds && !setup.xml.empty()) {
        auto data = ai::load_ai_data(setup.xml, converters(host));
        if (!data) return SessionResult::failure(data.error());
        std::vector<std::string> notes;
        auto plans = load_plans(host, modules, notes);
        if (!plans) return SessionResult::failure(plans.error());
        goals = std::make_shared<ai::Engine>(host, std::move(data).value(), std::move(plans).value());
        host->engine = goals.get();
    }

    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return SessionResult::failure(added.error());
    }
    authoritative::SessionConfig config;
    config.seed = setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/"};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return SessionResult::failure(scripts.error());
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (!errors.empty()) return SessionResult::failure(errors.front());
    for (const AiPlayer& player : host->setup.players) {
        if (!player.ai) continue;
        if (auto created = scripts.value().create_instance(freestore_instance(player.player), setup.freestore_module); !created) {
            return SessionResult::failure(created.error());
        }
    }
    auto session = authoritative::ScriptedTacticalSession::create(std::move(world), std::move(scripts).value());
    if (!session) return session;
    for (const std::string_view verb : {verb_move, verb_attack, verb_attack_move, verb_guard, verb_ability}) {
        if (auto added = session.value().register_verb(verb, translate_order); !added) return SessionResult::failure(added.error());
    }
    if (auto set = session.value().set_engine(std::make_shared<FreestoreEngine>(host, goals)); !set) return SessionResult::failure(set.error());
    return session;
}

std::vector<std::string> selected_plans() {
    std::vector<std::string> paths;
    for (const char* name : {"destroyunit", "flankplan", "destroyunitminimal", "areasweep", "bombingrun", "escortplan",
             "spacescout", "hidesurpriseunits", "hidetransports", "turboattack", "turboattacklocation", "spaceartillery",
             "movetolocation", "movetolocationrush", "retreatplan", "burnunits", "ai_plan_expansiongeneric_defendspacestation"}) {
        paths.push_back(std::string("Data/Scripts/AI/SpaceMode/") + name + ".lua");
    }
    return paths;
}

std::vector<std::string> plan_library_modules() {
    return {"Data/Scripts/Library/PGEvents.lua", "Data/Scripts/Library/PGTaskForce.lua"};
}

core::Result<std::vector<PlanInspection>> inspect_plans(const AiSetup& setup, const std::map<std::string, std::string>& modules) {
    using InspectResult = core::Result<std::vector<PlanInspection>>;
    auto host = std::make_shared<Host>();
    host->setup = setup;
    for (const AiType& type : host->setup.content.types) {
        host->types.emplace(type.type_id, &type);
        host->types_by_name.emplace(type.name, &type);
    }
    for (const AiPlayer& player : host->setup.players) host->players.emplace(player.player, &player);
    host->view = std::make_shared<WorldView>();
    authoritative::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) {
        if (auto added = manifest.add(path, bytes); !added) return InspectResult::failure(added.error());
    }
    authoritative::SessionConfig config;
    config.seed = setup.seed;
    config.tick_duration = authoritative::TickDuration{tactical::tick_numerator, tactical::tick_denominator};
    config.script_directories = {"Data/Scripts/Library/"};
    auto scripts = ScriptScheduler::create(std::move(config), std::move(manifest));
    if (!scripts) return InspectResult::failure(scripts.error());
    std::vector<core::Diagnostic> errors;
    register_globals(scripts.value(), host, errors);
    register_methods(scripts.value(), host, errors);
    if (!errors.empty()) return InspectResult::failure(errors.front());
    std::vector<PlanInspection> plans;
    std::uint64_t instance = 1;
    for (const std::string& path : selected_plans()) {
        PlanInspection plan;
        plan.path = path;
        if (auto created = scripts.value().create_instance(instance, path); !created) {
            plan.diagnostics.push_back(created.error().code + " " + created.error().message);
            plans.push_back(std::move(plan));
            continue;
        }
        const std::uint64_t tick = scripts.value().completed_tick() + 1;
        ScriptEvent flag;
        flag.key = authoritative::EventKey{tick, producer_foc_engine, instance, 0};
        flag.target = instance;
        flag.kind = ScriptEvent::Kind::assign;
        flag.name = "PlanDefinitionLoad";
        flag.parameter = boolean(true);
        ScriptEvent load = flag;
        load.key.sequence = 1;
        load.kind = ScriptEvent::Kind::call;
        load.name = "Base_Definitions";
        load.parameter.reset();
        for (ScriptEvent* event : {&flag, &load}) {
            if (auto submitted = scripts.value().submit_event(*event); !submitted) return InspectResult::failure(submitted.error());
        }
        authoritative::ServiceOptions options;
        options.host_paced = [](std::uint64_t) { return true; };
        const sim::InlineExecutor executor;
        auto report = scripts.value().service(executor, options);
        if (!report) return InspectResult::failure(report.error());
        for (const auto& diagnostic : report.value().diagnostics) plan.diagnostics.push_back(diagnostic.code + " " + diagnostic.message);
        auto category = scripts.value().read_global(instance, "Category");
        if (category && category.value()) {
            if (const std::string* text = as_text(*category.value())) plan.category = *text;
        }
        plan.loaded = plan.diagnostics.empty();
        plans.push_back(std::move(plan));
        ++instance;
    }
    return InspectResult::success(std::move(plans));
}

std::vector<std::string> unsupported_plan_calls() {
    return {
        "targeted and behaviour abilities (ION_CANNON_SHOT, HUNT): cut in #76; no Unit_Ability_Ready plan event",
        "exploration sweep (Explore_Area): the TaskForce moves to the area's centre",
        "reinforcement blocks (Reinforce): every M2 unit starts on the map, nothing to bring in",
        "star base Fire_Special_Weapon answers nil (FH-27); missing: GameObject.Fire_Special_Weapon on other objects, Garrison, Leave_Garrison, Get_Build_Pad_Contents, Get_Parent_Object, "
        "Get_Combat_Rating, Get_All_Projectile_Types; FogOfWar.Reveal_All",
        "GameRandom and FindTarget draws use the plan instance's seeded stream, not the game's shared one",
    };
}

} // namespace eawr::script::foc
