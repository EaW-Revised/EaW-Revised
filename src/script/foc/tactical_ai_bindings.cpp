// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/authoritative/letter_box.hpp"
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
    if (host.in_object_turn && player_filter)
        return fail("object turn dynamic-read tripwire: Find_Nearest raw fog");
    const ViewUnit* nearest = nullptr;
    std::optional<math::Fixed> best;
    for (const ViewPlayer& player : host.view->players) {
        if (host.neutral(player.id)) continue;
        if (player_filter && host.allied(player.id, *player_filter) != ally) continue;
        for (const ViewUnit& unit : host.view->units) {
            // WSW-01: persistent planetary sources are attachment handles, not combat targets.
            if (unit.owner != player.id || unit.id == excluded || unit.planetary_weapon) continue;
            const AiType* type = host.type(unit.type);
            if (type_filter != nullptr && type != type_filter) continue;
            const std::uint64_t properties = type != nullptr ? type->property_bits : 0;
            const std::uint64_t categories = type != nullptr ? type->category_bits : 0;
            if (property_mask != 0 && (property_mask & properties) == 0) continue;
            if ((category_mask & categories) == 0) continue;
            // WNO-11: this query forces raw object fog; SK-45 only relaxes unforced planning.
            if (player_filter && object_fogged(*host.view, unit, *player_filter)) continue;
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
    const Handle* player = as_handle(arguments[1], handle_player);
    if (player == nullptr) return fail("EvaluatePerception: parameter 2 is not a player");
    // A galactic player's starting free-store units reach the added hook before its AI is initialized: no value then, which the
    // stock free-store helpers read as 'no destination'. Only a galactic engine arms this window, and only for the player's own
    // free-store instance: tactical calls, plans and other scripts never see it.
    if (host.engine != nullptr && host.engine->startup_perception_pending(static_cast<tactical::PlayerId>(player->id), context.instance()))
        return none();
    if (host.ai_world && host.ai_world->goal_mode() == "GALACTIC") {
        const auto immediate = context.query_immediate("galactic.markup_perception", arguments);
        if (immediate && !immediate.value().empty()) {
            if (std::get_if<LuaNumber>(&immediate.value().front().data)) return immediate;
            context.report(authoritative::codes::unsupported_api, "EvaluatePerception(" + *as_text(arguments[0]) + ") failed; 0");
            return one(number(LuaNumber(0)));
        }
    }
    if (host.engine != nullptr) {
        // PE-30: the equation for the player, with the object or AI target as Target.
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
        const bool defender = perception.campaign_defender
            ? player->id == *perception.campaign_defender : perception.defender;
        if (!perception.campaign_game || !defender || perception.base_level == 0) {
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

// legacy #1713 (RGC-119): the first live object whose current type is the named one, any owner. The view lists objects in
// ascending identity (creation order; the retail active-object order is unverified), so the first match is the lowest. nil
// when nothing matches or the name is unknown. The scan stops at the first match, so it is charged by the units it visited.
core::Result<ValueList> first_object_of_type(Host& host, BindingContext& context, const std::string& name) {
    const auto key = upper(name);
    std::optional<tactical::TypeId> type;
    if (const auto found = host.types_by_name.find(key); found != host.types_by_name.end()) {
        type = found->second->type_id;
    } else if (const auto& names = host.setup.content.catalog_types) {
        const auto registered = std::lower_bound(names->begin(), names->end(), key,
            [](const auto& entry, const std::string& value) { return entry.first < value; });
        if (registered != names->end() && registered->first == key) type = registered->second;
    }
    if (!type) return none();
    std::uint64_t visited = 0;
    for (const ViewUnit& unit : host.view->units) {
        ++visited;
        if (unit.type != *type) continue;
        context.charge_host_work(visited);
        return one(handle(handle_game_object, unit.id));
    }
    for (const ViewUnit& unit : pending_created_objects(host, context)) {
        ++visited;
        if (unit.type != *type) continue;
        context.charge_host_work(visited);
        return one(handle(handle_game_object, unit.id));
    }
    context.charge_host_work(visited);
    return none();
}

void register_globals(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors, bool install_type_lists,
    bool tactical_world_queries) {
    const auto add = [&](std::string_view name, Binding binding) {
        // Dynamic manager/parent queries still depend on a completed barrier.
        // Fail before entering their transitive world/snapshot readers in a turn.
        if (name == "EvaluatePerception" || name == "Evaluate_In_Galactic_Context") {
            binding = [host, name = std::string(name), call = std::move(binding)](BindingContext& context, const ValueList& arguments) {
                if (host->in_object_turn) return fail("object turn dynamic-read tripwire: " + name);
                return call(context, arguments);
            };
        }
        if (auto added = scripts.register_binding(name, std::move(binding)); !added) errors.push_back(added.error());
    };
    add("Suspend_AI", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* value = arguments.size() == 1 ? std::get_if<LuaNumber>(&arguments[0].data) : nullptr;
        if (value == nullptr) return fail("Suspend_AI expects one number");
        context.issue_command("tactical-suspend-ai", {number(*value)});
        return none();
    });
    // legacy #6634 (HCB-75): the cinematic bands of the battle HUD; presentation only, see verb_letter_box.
    add("Letter_Box_In", authoritative::letter_box_binding("Letter_Box_In", "in", std::string(verb_letter_box)));
    add("Letter_Box_Out", authoritative::letter_box_binding("Letter_Box_Out", "out", std::string(verb_letter_box)));
    // #7977 family 5 (story flags and events), battle hosts. A battle hosts no story plot, no flag store, no advisor text and
    // no objective list (the galactic session owns them), so each call validates its arguments like the galactic bindings
    // (galactic_story_scripts.cpp) where the original agrees; the effect a battle has no state for reports unsupported_api.
    // TL-S1/S2: the original sends the event to the active mode's own story mode, which hands it to that battle plot's active
    // sub-plots. This host has no battle plot, so the event reaches no sub-plot; the call is validated and noted, never quiet.
    add("Story_Event", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty() || arguments.size() > 2 || as_text(arguments[0]) == nullptr)
            return fail("Story_Event expects a name and an optional object");
        if (arguments.size() == 2 && !std::holds_alternative<std::monostate>(arguments[1].data) && as_handle(arguments[1], handle_game_object) == nullptr)
            return fail("Story_Event: the second argument must be a game object");
        context.report(authoritative::codes::unsupported_api, "Story_Event(" + *as_text(arguments[0]) + ") reaches no sub-plot: this battle hosts no story plot");
        return none();
    });
    // TL-S3: the original takes exactly (player, flag, object or nil, reset Boolean) and reads the battle's own AI flag store,
    // never the galactic story state. No battle plot sets a flag here, so the answer is false, reported with a note.
    add("Check_Story_Flag", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 4) return fail("Check_AI_Story_Flag -- invalid number of parameters.  Expected between 4, got " + std::to_string(arguments.size()) + ".");
        if (as_handle(arguments[0], handle_player) == nullptr) return fail("Check_AI_Story_Flag -- invalid type for parameter 1.  Expected player");
        if (as_text(arguments[1]) == nullptr) return fail("Check_AI_Story_Flag -- invalid type for parameter 2.  Expected string");
        if (!std::holds_alternative<std::monostate>(arguments[2].data) && as_handle(arguments[2], handle_game_object) == nullptr)
            return fail("Check_AI_Story_Flag -- target is not a game object; unable to check story flag.");
        if (!std::holds_alternative<bool>(arguments[3].data)) return fail("Check_AI_Story_Flag -- invalid type for parameter 4; expected boolean.");
        context.report(authoritative::codes::unsupported_api, "Check_Story_Flag(" + *as_text(arguments[1]) + ") answers false: this battle hosts no story plot to set it");
        return one(boolean(false));
    });
    add("Game_Message", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 1 || as_text(arguments[0]) == nullptr) return fail("Game_Message requires exactly one text key");
        context.report(authoritative::codes::unsupported_api, "Game_Message presentation is unavailable; script continues");
        return none();
    });
    add("Add_Objective", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.empty() || as_text(arguments[0]) == nullptr) return fail("Add_Objective expects a text key");
        context.report(authoritative::codes::unsupported_api, "Add_Objective is unavailable: a battle hosts no objective list");
        return none();
    });
    for (const std::string_view name : {"Add_Planet_Highlight", "Remove_Planet_Highlight"})
        add(name, [name = std::string(name)](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            if (arguments.size() != 1 || as_handle(arguments[0], handle_game_object) == nullptr) return fail(name + " expects a planet");
            context.report(authoritative::codes::unsupported_api, name + " is unavailable: a battle has no galactic map");
            return none();
        });
    add("Remove_All_Text", [](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (!arguments.empty()) return fail("Remove_All_Text takes no arguments");
        context.report(authoritative::codes::unsupported_api, "Remove_All_Text: no script text is hosted; script continues");
        return none();
    });
    add("Get_Game_Mode", [host](BindingContext&, const ValueList&) {
        const auto domain = host->world != nullptr ? host->world->domain() : tactical::BattleDomain::space;
        return one(Value::text(domain == tactical::BattleDomain::land ? "Land" : "Space"));
    });
    add("Find_Nearest", [host](BindingContext&, const ValueList& arguments) { return find_nearest(*host, arguments); });
    // legacy #1713 (RGC-119): the first live object whose current type is the named one, any owner. The view lists
    // objects in ascending identity (creation order; the retail active-object order is unverified), so the first match
    // is the lowest. nil when nothing matches or the name is unknown; a missing or non-string first argument is an
    // error and extra arguments are ignored.
    if (tactical_world_queries) add("Find_First_Object", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const std::string* name = arguments.empty() ? nullptr : as_text(arguments[0]);
        if (name == nullptr) return fail("Find_First_Object expects an original type name");
        return first_object_of_type(*host, context, *name);
    });
    add("EvaluatePerception", [host](BindingContext& context, const ValueList& arguments) {
        return evaluate_perception(*host, context, arguments);
    });
    // WGA-80/81: preserve skirmish bindings when no real parent was supplied.
    if (host->setup.evaluate_in_galactic_context) {
        add("Evaluate_In_Galactic_Context", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            if (arguments.size() != 2 || as_text(arguments[0]) == nullptr)
                return fail("Evaluate_In_Galactic_Context expects an equation name and player");
            const auto* player = as_handle(arguments[1], handle_player);
            if (!player) return fail("Evaluate_In_Galactic_Context: parameter 2 is not a player");
            const auto value = host->setup.perception.campaign_game
                ? host->setup.evaluate_in_galactic_context(*as_text(arguments[0]), static_cast<tactical::PlayerId>(player->id))
                : std::nullopt;
            if (!value) {
                context.report(authoritative::codes::unsupported_api,
                    "Evaluate_In_Galactic_Context requires its retained campaign parent, manager and conflict planet");
                return none();
            }
            return one(number(*value));
        });
    }
    add("Find_Object_Type", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 1 || as_text(arguments[0]) == nullptr) return fail("Find_Object_Type expects a name");
        const auto found = host->types_by_name.find(upper(*as_text(arguments[0])));
        if (found == host->types_by_name.end()) return none();
        return one(handle(handle_type, found->second->type_id));
    });
    if (tactical_world_queries) add("Create_Generic_Object", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 3) return fail("Create_Generic_Object expects type, position and player");
        const AiType* type = nullptr;
        if (const auto* name = as_text(arguments[0])) {
            const auto found = host->types_by_name.find(upper(*name));
            if (found != host->types_by_name.end()) type = found->second;
        } else if (const auto* value = as_handle(arguments[0], handle_type)) type = host->type(value->id);
        auto position = position_of(*host, arguments[1]);
        if (!position) if (const auto pending = pending_created_object(*host, context, arguments[1])) position = pending->position;
        const auto* owner = as_handle(arguments[2], handle_player);
        if (!type || !position || !owner || owner->id > std::numeric_limits<tactical::PlayerId>::max()
            || !host->players.contains(static_cast<tactical::PlayerId>(owner->id)))
            return fail("Create_Generic_Object: invalid type, position or player");
        const auto* profile = host->world ? host->world->economy().upgrade(type->type_id) : nullptr;
        // An ordinary upgrade object is a hidden economy object; any other data-defined object with a footprint is a
        // unit the ordered commit places at the nearest free spot, as a reinforcement is.
        const bool upgrade = type->upgrade_object && profile != nullptr && !profile->level_up && profile->bounties.empty();
        const bool unit = !type->upgrade_object && host->world != nullptr && host->world->economy().footprint_of(type->type_id) != nullptr;
        if (host->setup.domain != tactical::BattleDomain::space || !(upgrade || unit)
            || host->world->economy().disabled_types.contains(type->type_id))
            return fail("Create_Generic_Object: this host supports ordinary space upgrade objects and space units only");
        const auto player = static_cast<tactical::PlayerId>(owner->id);
        const auto issuer = host->players.find(player);
        if (issuer == host->players.end() || issuer->second->neutral)
            return fail("Create_Generic_Object: no commandable creation owner");
        // The ordered commit needs the owner's economy account (a defeated or account-less seat has none); a missing one
        // would abort the tick after the wrapper is returned, so it is a Lua error here.
        const auto ledgers = host->world->ledgers();
        if (!host->world->economy().player(player)
            || std::none_of(ledgers.begin(), ledgers.end(), [player](const tactical::PlayerEconomy& account) { return account.player == player; }))
            return fail("Create_Generic_Object: the creation owner has no economy account");
        // Project reference namespace: bounded, injective (instance, command sequence), retained in replay/state.
        constexpr unsigned sequence_bits = 20;
        constexpr unsigned low_bits = sequence_bits + tactical::script_reference_member_bits;
        if (context.instance() >= (std::uint64_t{1} << (63 - low_bits))
            || context.command_sequence() >= (std::uint64_t{1} << sequence_bits))
            return fail("Create_Generic_Object: script creation identity space exhausted");
        const auto identity = tactical::script_creation_namespace | (context.instance() << low_bits)
            | (context.command_sequence() << tactical::script_reference_member_bits);
        if (identity == tactical::script_creation_namespace) return fail("Create_Generic_Object: invalid creation identity");
        context.issue_command(std::string(verb_create_object), {number(LuaNumber(static_cast<std::int64_t>(player))),
            handle(handle_type, type->type_id), position_value(*position), arguments[2], handle(handle_game_object, identity)});
        return one(handle(handle_game_object, identity));
    });
    add("Script.Debug_Should_Issue_Event_Alert", [](BindingContext&, const ValueList&) { return one(boolean(false)); });
    // Debug output only (PGDebug): no effect on the game.
    for (const char* name : {"_OuputDebug", "_ScriptMessage", "_CustomScriptMessage", "_MessagePopup", "DumpCallStack"}) add(name, noop());
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
    // WHE-68 (debug build): parameter 1 is a table of objects, parameter 2 the radius.
    // Entries that are not objects warn; dead objects are skipped; the rest are
    // taken in ID order. Answers a position wrapper and its threat, or nil when
    // no center has positive threat. The initial centers draw from the
    // instance stream, the existing per-instance RNG seam.
    add("Find_Best_Local_Threat_Center", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2) return fail("Find_Best_Local_Threat_Center -- invalid number of parameters. Expected 2");
        const auto* list = std::get_if<std::vector<Value>>(&arguments[0].data);
        if (list == nullptr) return fail("Find_Best_Local_Threat_Center -- invalid type for parameter 1. Expected table.");
        const auto* radius = std::get_if<LuaNumber>(&arguments[1].data);
        if (radius == nullptr) return fail("Find_Best_Local_Threat_Center -- invalid type for parameter 2. Expected number.");
        std::vector<const ViewUnit*> objects;
        for (const Value& entry : *list) {
            if (as_handle(entry, handle_game_object) == nullptr) {
                context.report(authoritative::codes::script_error, "Find_Best_Local_Threat_Center -- invalid type for entry in object table");
                continue;
            }
            if (const ViewUnit* unit = live_object(*host, entry)) objects.push_back(unit);
        }
        std::stable_sort(objects.begin(), objects.end(), [](const ViewUnit* a, const ViewUnit* b) { return a->id < b->id; });
        if (objects.empty()) {
            // WHE-68: an empty sample warns and returns nil; the next update
            // must still run (IG-2000 reaches this before its first sample).
            context.report(authoritative::codes::script_warning, "Find_Best_Local_Threat_Center -- no objects supplied");
            return none();
        }
        auto& random = context.random();
        const auto center = ai::local_threat_center(objects, *radius * *radius,
            [&](const ViewUnit& unit) {
                // The company type's power: a team's own (squadron) type, else the object's.
                const AiType* type = host->type(unit.type);
                return type != nullptr ? type->combat_power : LuaNumber{};
            },
            [&](LuaNumber low, LuaNumber high) {
                if (high < low) std::swap(low, high);
                const auto fraction = ai::to_single(LuaNumber(random.next_in_range(0, 32767)) / LuaNumber(32767));
                return ai::to_single(low + ai::to_single(fraction * ai::to_single(high - low)));
            });
        if (!(LuaNumber(0) < center.threat)) return none();
        return core::Result<ValueList>::success(ValueList{position_handle(center.x, center.y), number(center.threat)});
    });
    register_plan_bindings(scripts, host, errors);
    // A plan's definition load (PGAICommands Set_Contrast_Values) builds its contrast lists. The
    // lists feed the goal system's contrast sizing, which is not hosted: the handle is opaque
    // and Parse only checks its arguments (the host's own contrast weights, FH-30, come from
    // the same function through load_contrast).
    if (install_type_lists) add("WeightedTypeList.Create", [](BindingContext&, const ValueList&) { return one(handle(handle_type_list, 1)); });
}

void register_methods(ScriptScheduler& scripts, const HostPtr& host, std::vector<core::Diagnostic>& errors, bool install_type_lists) {
    const auto guard_dynamic_read = [host](std::string_view name, Binding binding) -> Binding {
        return [host, name = std::string(name), call = std::move(binding)](BindingContext& context, const ValueList& arguments) {
            if (host->in_object_turn) return fail("object turn dynamic-read tripwire: " + name);
            return call(context, arguments);
        };
    };
    const auto object = [&](std::string_view name, Binding binding) {
        if (name == "Fire_Special_Weapon" || name == "Get_Build_Pad_Contents" || name == "Can_Move" || name == "Build")
            binding = guard_dynamic_read(name, std::move(binding));
        if (auto added = scripts.register_method(handle_game_object, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    const auto player = [&](std::string_view name, Binding binding) {
        if (name == "Get_Tech_Level") binding = guard_dynamic_read(name, std::move(binding));
        if (auto added = scripts.register_method(handle_player, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    const auto type = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_type, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    // Arguments start with the handle itself. A method on a dead object gives nothing (the
    // wrappers assert and return no value).
    object("Is_Valid", [host](BindingContext& context, const ValueList& arguments) {
        if (pending_created_object(*host, context, arguments[0])) return one(boolean(true));
        auto immediate = galactic_immediate_query(context, *host, "galactic.created_valid", {arguments[0]});
        if (!immediate || !immediate.value().empty()) return immediate;
        return one(boolean(live_object(*host, arguments[0]) != nullptr));
    });
    object("Get_Type", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (const auto unit = pending_created_object(*host, context, arguments[0])) return one(handle(handle_type, unit->type));
        auto immediate = galactic_immediate_query(context, *host, "galactic.created_type", {arguments[0]});
        if (!immediate || !immediate.value().empty()) return immediate;
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(handle(handle_type, unit->type));
    });
    object("Get_Owner", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (const auto unit = pending_created_object(*host, context, arguments[0])) return one(handle(handle_player, unit->owner));
        auto immediate = galactic_immediate_query(context, *host, "galactic.created_owner", {arguments[0]});
        if (!immediate || !immediate.value().empty()) return immediate;
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(handle(handle_player, unit->owner));
    });
    object("Get_Position", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (const auto unit = pending_created_object(*host, context, arguments[0])) return one(position_object(unit->position));
        auto immediate = galactic_immediate_query(context, *host, "galactic.created_position", {arguments[0]});
        if (!immediate || !immediate.value().empty()) return immediate;
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(position_object(unit->position));
    });
    object("Get_Distance", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || arguments.size() < 2) return none();
        // EX-51: use the same position extraction as movement and TaskForce
        // distance, including an AI target's live object or region centre.
        const auto other = unit_destination(*host, *unit, arguments[1], false, false);
        if (!other) return fail("Get_Distance: parameter 1 is not a live object or a position");
        const auto length = distance(unit->position, other->position);
        if (!length) return fail("Get_Distance: distance out of range");
        return one(number(numeric::from_fixed(*length)));
    });
    const auto has_bits = [host](bool categories) {
        return [host, categories](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
            const ViewUnit* unit = live_object(*host, arguments[0]);
            if (unit == nullptr) return none();
            if (arguments.size() != 2 || as_text(arguments[1]) == nullptr) return fail("expects one name");
            const auto& names = categories ? host->setup.content.categories : host->setup.content.properties;
            // EX-56: both APIs parse an authored bitmask and test any overlap.
            std::uint64_t requested{};
            const auto requested_names = ai::split_names(*as_text(arguments[1]), "|");
            if (requested_names.empty()) return fail("unknown name " + *as_text(arguments[1]));
            for (const auto& name : requested_names) {
                const auto bit = names.find(upper(name));
                if (bit == names.end()) return fail("unknown name " + name);
                requested |= bit->second;
            }
            const AiType* type = host->type(unit->type);
            const std::uint64_t bits = type == nullptr ? 0 : (categories ? type->category_bits : type->property_bits);
            return one(boolean((bits & requested) != 0));
        };
    };
    // #7977 family 5: Highlight(on[, ...]) / Highlight_Small(on[, ...]) mark an object for the story's tutorial pointer; no
    // pointer is hosted, so a valid call reports and continues. The first argument is the Boolean switch (stock scripts also
    // pass a trailing colour or size number or string, accepted untouched).
    for (const std::string_view name : {"Highlight", "Highlight_Small"})
        object(name, [host, name = std::string(name)](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
            if (live_object(*host, arguments[0]) == nullptr) return none();
            if (arguments.size() < 2 || arguments.size() > 4 || !std::holds_alternative<bool>(arguments[1].data))
                return fail(name + " expects a Boolean and optional options");
            context.report(authoritative::codes::unsupported_api, name + " is unavailable: no tutorial pointer is hosted");
            return none();
        });
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
    // TL-03 (debug build): Make_Invulnerable(enabled) takes exactly one boolean (a script error otherwise).
    // True sets the object's script-invulnerable behaviour unless it is already set (idempotent, no
    // stacking, no expiry); false clears it; a squadron applies it to each member. The flag is simulation
    // state, so a valid call queues a replay command for the next tick and answers nothing; a dead object
    // answers nothing like its siblings. The retail "active mode flag" gate is unverified (fidelity list).
    object("Make_Invulnerable", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 1)
            return fail("Make_Invulnerable: invalid number of parameters.  Expected 1, got " + std::to_string(count) + ".");
        if (!std::holds_alternative<bool>(arguments[1].data))
            return fail("Make_Invulnerable: invalid type for parameter 1.  Expected boolean.");
        const ViewUnit* unit = live_object(*host, arguments[0]);
        context.issue_command(std::string(verb_make_invulnerable),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // HD-30 (debug build): Take_Damage(amount[, hardpoint name]) takes 1 or 2 arguments (a number, then a string).
    // The name must be a hardpoint some type defines (a script error otherwise); the retail call then passes the
    // hardpoint's collision mesh, and the object damages the hardpoint that owns that mesh when it has one and it
    // can be destroyed, else its hull. The damage is the debug-cheat kind: scripted damage that the sim applies
    // through the replay command queue (DamagePayload, shields first under the damage rules). A team applies it to
    // each member, each resolving the hardpoint from its own type. A dead object warns in retail and answers nothing
    // here. An amount outside the fixed-point range is a script error; a negative amount is not queued (the sim's
    // scripted damage takes amounts >= 0; retail's float path for a negative amount is unverified).
    object("Take_Damage", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const std::size_t count = arguments.size() - 1;
        if (count != 1 && count != 2)
            return fail("Take_Damage: invalid number of parameters.  Expected 1 or 2, got " + std::to_string(count) + ".");
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (!std::holds_alternative<LuaNumber>(arguments[1].data))
            return fail("Take_Damage: invalid type for parameter 1.  Expected number.");
        std::string mesh;
        if (count == 2) {
            const std::string* name = as_text(arguments[2]);
            if (name == nullptr) return fail("Take_Damage: invalid type for parameter 2.  Expected string.");
            const auto upper = [](std::string text) { for (char& c : text) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A')); return text; };
            const std::string wanted = upper(*name);
            bool known = false;
            for (const auto& entry : host->types) {
                const AiType& candidate = *entry.second;
                for (std::size_t i = 0; i < candidate.hardpoint_ids.size() && !known; ++i)
                    if (candidate.hardpoint_ids[i] == wanted) { known = true; mesh = candidate.hardpoint_meshes[i]; }
                if (known) break;
            }
            if (!known) return fail("Take_Damage: unknown hard point " + *name + ".");
        }
        const auto fixed = numeric::to_fixed(std::get<LuaNumber>(arguments[1].data));
        if (!fixed) return fail("Take_Damage: invalid value for parameter 1.  The amount is out of range.");
        if (fixed.value().raw() < 0) return none();
        // The retail team loop damages each member by the mesh name, so every member resolves the hardpoint from its own
        // type: the first hardpoint with that collision mesh, and only when it can be destroyed (else the hull).
        const auto send = [&](const ViewUnit& target) {
            ValueList command{number(LuaNumber(static_cast<std::int64_t>(target.owner))),
                handle(handle_game_object, target.id), arguments[1]};
            if (const AiType* type = host->type(target.type); type != nullptr && !mesh.empty()) {
                for (std::size_t i = 0; i < type->hardpoint_meshes.size(); ++i) {
                    if (type->hardpoint_meshes[i] != mesh) continue;
                    if (i < type->hardpoint_destroyable.size() && type->hardpoint_destroyable[i])
                        command.push_back(number(LuaNumber(static_cast<std::int64_t>(i))));
                    break;
                }
            }
            context.issue_command(std::string(verb_take_damage), std::move(command));
        };
        if (unit->container) {
            for (const sim::EntityId member : unit->members)
                if (const ViewUnit* craft = host->view->find(member)) send(*craft);
        } else {
            send(*unit);
        }
        return none();
    });
    // Prevent_All_Fire(enabled) (debug build): exactly one boolean (a script error otherwise). True disables every
    // weapon hardpoint, the targeting and fire behaviours of the object, and each member of a team; false enables them
    // again. The flag is simulation state: a valid call queues a replay command for the next tick and answers nothing;
    // a dead object answers nothing like its siblings.
    object("Prevent_All_Fire", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 1)
            return fail("Prevent_All_Fire: invalid number of parameters.  Expected 1, got " + std::to_string(count) + ".");
        if (!std::holds_alternative<bool>(arguments[1].data))
            return fail("Prevent_All_Fire: invalid type for parameter 1.  Expected boolean.");
        context.issue_command(std::string(verb_prevent_all_fire),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // Teleport_And_Face(object) (debug build): exactly one game object (a script error for a position, another type or
    // a dead object). The caller leaves its garrison, moves to the object's position and takes its facing. The sim
    // applies it through the replay command queue and stops the unit where it lands; a squadron or one of its craft
    // is not moved (they fly by their squadron). A dead caller answers nothing.
    object("Teleport_And_Face", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 1)
            return fail("Teleport_And_Face: invalid number of parameters.  Expected 1, got " + std::to_string(count) + ".");
        if (as_handle(arguments[1], handle_position) != nullptr)
            return fail("Teleport_And_Face: Expected game object for parameter 1, got position instead.");
        if (as_handle(arguments[1], handle_game_object) == nullptr)
            return fail("Teleport_And_Face: Expected game object for parameter 1.");
        const ViewUnit* target = live_object(*host, arguments[1]);
        if (target == nullptr) return fail("Teleport_And_Face: Invalid game object given for parameter 1.");
        context.issue_command(std::string(verb_teleport_and_face),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // Teleport(position) (debug build): exactly one argument, resolved as a position (a position wrapper, an object or a
    // plain list; a script error otherwise), then the caller moves to the point (z included) keeping its facing and stops
    // there. Only in a tactical mode (a script error in the galactic host). The sim applies it through the replay command
    // queue; a squadron container takes its members along with their offsets, a craft alone is not moved. Garrisons are a
    // land-battle concept, so there is none to leave here. A dead caller answers nothing.
    object("Teleport", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC")
            return fail("Teleport: this function may only be used in tactical modes.");
        const std::size_t count = arguments.size() - 1;
        if (count != 1)
            return fail("Teleport: invalid number of parameters.  Expected 1, got " + std::to_string(count) + ".");
        const auto point = position_of(*host, arguments[1]);
        if (!point) return fail("Teleport: unable to deduce a position from parameter 1.");
        context.issue_command(std::string(verb_teleport_position),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], number(numeric::from_fixed(point->x)),
             number(numeric::from_fixed(point->y)), number(numeric::from_fixed(point->z))});
        return none();
    });
    // Hide(flag) (debug build): the flag is required and must be a boolean (a script error otherwise); the object's model
    // and attached sound are hidden or shown. A presentation flag the sim keeps and checkpoints; no combat effect. The
    // retail "object has no model" error is not hosted (the host has no model table); a dead caller answers nothing.
    object("Hide", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() < 2) return fail("Hide: Missing required boolean parameter.");
        if (!std::holds_alternative<bool>(arguments[1].data)) return fail("Hide: Expected bool parameter.");
        context.issue_command(std::string(verb_hide_sound), {arguments[0], arguments[1]});
        context.issue_command(std::string(verb_hide), {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // Set_Selectable(flag) (debug build): exactly one argument (a script error naming
    // the count otherwise), a dead object warns and answers nothing, a non-boolean is a script error, and an object
    // without the SELECTABLE behaviour warns ("not selectable") and changes nothing. Otherwise the object is deselected
    // (its team members too) and its selection data takes the flag: a false flag makes the unit refuse player selection.
    object("Set_Selectable", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() != 2)
            return fail("Set_Selectable: invalid number of parameters.  Expected 1, got " + std::to_string(arguments.size() - 1) + ".");
        if (!std::holds_alternative<bool>(arguments[1].data)) return fail("Set_Selectable: invalid type for parameter 1.  Expected boolean.");
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->selectable) {
            context.report(authoritative::codes::script_warning, "Set_Selectable: this object is not selectable.");
            return none();
        }
        context.issue_command(std::string(verb_selectable), {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // Cancel_Hyperspace() (TL-17, debug build): reads no argument and answers nothing. A dead object does nothing; outside a
    // space battle, or on an object without a fighter or simple space locomotor, the call does nothing. Otherwise the sim
    // ends the object's hyperspace arrival sequence at once (an object that is not arriving is left alone there).
    object("Cancel_Hyperspace", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if ((host->ai_world && host->ai_world->goal_mode() == "GALACTIC") || (host->world != nullptr ? host->world->domain() : tactical::BattleDomain::space) != tactical::BattleDomain::space)
            return none();
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->cancels_hyperspace) return none();
        context.issue_command(std::string(verb_cancel_hyperspace), {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], number(LuaNumber(0))});
        return none();
    });
    // Cinematic_Hyperspace_In(frames) (debug build): one number (a script error otherwise: none given, or not a number),
    // an object that cannot move is a script error, and the sim starts the hyperspace arrival sequence at the object's
    // position. The "type is hyperspace capable" gate is not hosted (no per-type flag here): every mobile type starts it.
    object("Cinematic_Hyperspace_In", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if ((host->ai_world && host->ai_world->goal_mode() == "GALACTIC") || (host->world != nullptr ? host->world->domain() : tactical::BattleDomain::space) != tactical::BattleDomain::space)
            return fail("Cinematic_Hyperspace_In: this function may only be used in space mode.");
        if (arguments.size() < 2) return fail("Cinematic_Hyperspace_In: invalid number of parameters.  Expected interger value for delay frames");
        if (!std::holds_alternative<LuaNumber>(arguments[1].data)) return fail("Cinematic_Hyperspace_In: invalid type for parameter 1.  Expected number.");
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->locomotor)
            return fail("Cinematic_Hyperspace_In: object " + (type != nullptr ? type->name : std::string("object")) + " can't move, so it certainly can't hyperspace.");
        context.issue_command(std::string(verb_hyperspace_in), {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // Change_Owner(player) (debug build): exactly one argument (a script error otherwise), then a dead object warns and
    // answers nothing, then a non-player argument is a script error. A valid call queues one replay command for the next
    // tick and answers nothing; the same owner is a no-op in the sim. Planets (a faction change on the planetary
    // behaviour) are galactic and not hosted here.
    object("Change_Owner", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const std::size_t count = arguments.size() - 1;
        if (count != 1)
            return fail("Change_Owner: invalid number of parameters.  Expected 1, got " + std::to_string(count) + ".");
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const Handle* player = as_handle(arguments[1], handle_player);
        if (player == nullptr || player->id > std::numeric_limits<tactical::PlayerId>::max())
            return fail("Change_Owner: invalid type for parameter 1.  Expected player.");
        context.issue_command(std::string(verb_change_owner),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // #7886: Object.Despawn() takes no arguments (a script error otherwise) and removes the object from the battle without any
    // death effect; the simulation state changes, so a valid call queues a replay command for the next tick and answers
    // nothing. A dead object answers nothing like its siblings. The effect on a carried or squadron object and on objects
    // that other scripts still hold is unverified (fidelity list).
    object("Despawn", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 0)
            return fail("Despawn: invalid number of parameters.  Expected 0, got " + std::to_string(count) + ".");
        const ViewUnit* unit = live_object(*host, arguments[0]);
        context.issue_command(std::string(verb_despawn),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0]});
        return none();
    });
    // WSU-21: stock object scripts call Is_Selectable() with no arguments to ask whether the
    // object carries the SELECTABLE behaviour; a dead object answers nothing like its siblings.
    // Dynamic Set_Selectable overrides are not hosted (fidelity list).
    object("Is_Selectable", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        const AiType* type = host->type(unit->type);
        return one(boolean(type != nullptr && type->selectable));
    });
    // TL-02 (debug build): Play_Animation(name, loop[, index]) validates its 2 or 3 arguments
    // (string, boolean, number) with a script error per mismatch, then only sets the object's
    // target animation: a presentation state with no simulation effect. The AI script host has
    // no animation state, so a valid call is a silent no-op that answers nothing; a dead object
    // answers nothing like its siblings. The animation name is not checked against the model
    // animation names (no verified name table; fidelity list).
    object("Play_Animation", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        const std::size_t count = arguments.size() - 1;
        if (count != 2 && count != 3)
            return fail("Play_Animation: invalid number of parameters.  Expected 2 or 3, got " + std::to_string(count) + ".");
        if (as_text(arguments[1]) == nullptr)
            return fail("Play_Animation: invalid type for parameter 1.  Expected string.");
        if (!std::holds_alternative<bool>(arguments[2].data))
            return fail("Play_Animation: invalid type for parameter 2.  Expected boolean.");
        if (count == 3 && !std::holds_alternative<LuaNumber>(arguments[3].data))
            return fail("Play_Animation: invalid type for parameter 3.  Expected number.");
        return none();
    });
    // EX-53: the completed object's engine-enabled state, not its speed.
    object("Are_Engines_Online", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) {
            context.report(authoritative::codes::script_error, "Are_Engines_Online -- this object is already dead");
            return none();
        }
        return one(boolean(unit->engines_online));
    });
    object("Can_Move", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) {
            context.report(authoritative::codes::script_error, "Can_Move -- this object is already dead");
            return none();
        }
        if (host->ai_world && host->ai_world->goal_mode() != "SPACE") return fail("Can_Move requires the SPACE movement projection");
        return one(boolean(unit->can_move));
    });
    // EX-59 / #3844: this state-changing operation is explicitly skipped until
    // the simulation hosts per-unit targeting sets. Keep the caller running.
    object("Set_Targeting_Priorities", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) {
            context.report(authoritative::codes::script_error, "Set_Targeting_Priorities -- this object is already dead");
            return none();
        }
        const auto* name = arguments.size() == 2 ? as_text(arguments[1]) : nullptr;
        if (name == nullptr) return fail("Set_Targeting_Priorities expects one targeting set name");
        context.report(authoritative::codes::unsupported_api,
            "Set_Targeting_Priorities skipped override " + *name + ": per-unit targeting sets are not hosted");
        return none();
    });
    // SOS-19: the immutable container view holds actual live members, including
    // its leader. Inherited script names never supply an absent ability.
    object("Get_Contained_Object_Count", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        std::int64_t count = 0;
        for (const auto member : unit->members) if (host->view->find(member) != nullptr) ++count;
        return one(number(LuaNumber(count)));
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
        if (request_ability(context, *host, *unit, upper(*as_text(arguments[1])), arguments.size() >= 3 ? &arguments[2] : nullptr))
            return one(boolean(true));
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
    object("Suspend_Locomotor", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() != 2 || !std::holds_alternative<bool>(arguments[1].data))
            return fail("Suspend_Locomotor expects one boolean");
        const AiType* type = host->type(unit->type);
        if (type == nullptr || !type->locomotor) return fail("Suspend_Locomotor: can't obtain locomotor");
        context.issue_command(std::string(verb_suspend_locomotor),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0], arguments[1]});
        return none();
    });
    // MV-20: face orders use the same ordered and replayed movement path as player face orders.
    object("Turn_To_Face", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() < 2) return fail("Turn_To_Face expects a target");
        const auto target = position_of(*host, arguments[1]);
        if (!target) return fail("Turn_To_Face cannot deduce a position");
        context.issue_command(std::string(verb_face),
            {number(LuaNumber(static_cast<std::int64_t>(unit->owner))), arguments[0],
             number(numeric::from_fixed(target->x)), number(numeric::from_fixed(target->y)), number(numeric::from_fixed(target->z))});
        return none();
    });
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
    // (an unsupported kind) exists but is never ready or active.
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
    object("Event_Object_In_Range", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        if (arguments.size() < 3 || std::get_if<LuaNumber>(&arguments[2].data) == nullptr)
            return fail("Event_Object_In_Range expects a function and numeric range");
        authoritative::ProximityQuery query;
        query.object = *as_handle(arguments[0], handle_game_object);
        query.extent = ai::to_single(*std::get_if<LuaNumber>(&arguments[2].data));
        for (std::size_t index = 3; index < std::min(arguments.size(), std::size_t{5}); ++index) {
            if (!query.player) if (const auto* player = as_handle(arguments[index], handle_player)) query.player = *player;
            if (!query.type) if (const auto* type = as_handle(arguments[index], handle_type)) query.type = *type;
        }
        const auto registered = context.register_proximity(std::move(query));
        if (!registered) return core::Result<ValueList>::failure(registered.error());
        return one(boolean(true));
    });
    object("Cancel_Event_Object_In_Range", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (live_object(*host, arguments[0]) == nullptr) return none();
        const auto cancelled = context.cancel_proximity(*as_handle(arguments[0], handle_game_object));
        if (!cancelled) return core::Result<ValueList>::failure(cancelled.error());
        return none();
    });
    object("Service_Wrapper", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* object = as_handle(arguments[0], handle_game_object);
        if (object == nullptr) return none();
        // L-33 project policy: stable entity order over the immutable completed
        // world view. FC-58/UFS-27 names its post-movement sampling seam.
        const auto serviced = context.service_proximities(*object, [host](const authoritative::ProximityQuery& query) {
            std::vector<Handle> out;
            const auto* origin = host->view->find(query.object.id);
            if (origin == nullptr) return out;
            const std::array center_axes{numeric::from_fixed(origin->position.x), numeric::from_fixed(origin->position.y),
                numeric::from_fixed(origin->position.z)};
            for (const auto& unit : host->view->units) {
                if (unit.id == origin->id) continue;
                const auto* type = host->type(unit.type);
                if (type != nullptr && type->decoration) continue;
                if (query.player && !host->allied(unit.owner, static_cast<tactical::PlayerId>(query.player->id))) continue;
                if (query.type && unit.type != query.type->id) continue;
                auto low = unit.position;
                auto high = unit.position;
                if (unit.proximity_bounds) {
                    low = (*unit.proximity_bounds)[0];
                    high = (*unit.proximity_bounds)[1];
                } else if (type != nullptr && type->proximity_bounds) {
                    const auto matrix = math::to_matrix(unit.rotation, unit.position);
                    if (!matrix) continue;
                    bool first = true;
                    for (unsigned corner = 0; corner < 8; ++corner) {
                        const auto& bounds = *type->proximity_bounds;
                        const auto point = math::transform_point(matrix.value(), {bounds[corner & 1U].x,
                            bounds[(corner >> 1U) & 1U].y, bounds[(corner >> 2U) & 1U].z});
                        if (!point) continue;
                        if (first) { low = high = point.value(); first = false; }
                        else {
                            low = {std::min(low.x, point.value().x), std::min(low.y, point.value().y), std::min(low.z, point.value().z)};
                            high = {std::max(high.x, point.value().x), std::max(high.y, point.value().y), std::max(high.z, point.value().z)};
                        }
                    }
                }
                const std::array low_axes{numeric::from_fixed(low.x), numeric::from_fixed(low.y), numeric::from_fixed(low.z)};
                const std::array high_axes{numeric::from_fixed(high.x), numeric::from_fixed(high.y), numeric::from_fixed(high.z)};
                bool inside = true;
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    const auto middle = center_axes[axis];
                    inside = inside && low_axes[axis] <= middle + query.extent && high_axes[axis] >= middle - query.extent;
                }
                if (inside) out.push_back({handle_game_object, unit.id});
            }
            return out;
        });
        if (!serviced) return core::Result<ValueList>::failure(serviced.error());
        return none();
    });
    for (const char* name : {"Lock_Current_Orders"}) {
        object(name, [name = std::string(name)](BindingContext& context, const ValueList&) {
            context.report(authoritative::codes::unsupported_api, name + " skipped: per-unit order locks are not hosted");
            return none();
        });
    }
    // WTA-34: a team answers for its leader. Get_Current_Projectile_Type is the object weapon's
    // projectile (index 0 of Projectile_Types, the only one the tables keep); a unit without an
    // object weapon has no current projectile and answers nil.
    // Get_All_Projectile_Types lists the projectile of every standing hardpoint in HardPoints order,
    // then the object weapon's.
    object("Get_Current_Projectile_Type", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || (unit = projectile_leader(*host, *unit)) == nullptr) return none();
        const AiType* type = host->type(unit->type);
        if (type == nullptr || type->object_projectile == 0) return none();
        return one(handle(handle_type, type->object_projectile));
    });
    object("Get_All_Projectile_Types", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || projectile_leader(*host, *unit) == nullptr) return none();
        std::vector<Value> list;
        for (const auto projectile : projectile_types_of(*host, *unit)) list.push_back(handle(handle_type, projectile));
        return one(Value{std::move(list)});
    });
    // DT-04, WTA-32: the unit's remaining health (its destroyable hardpoints', else its hull, plus
    // its shield) over its rate of damage taken; 10^18 at a zero rate or without damage tracking.
    object("Get_Time_Till_Dead", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (!unit->time_till_dead) return one(number(LuaNumber(std::int64_t{1000000000000000000})));
        return one(number(numeric::from_fixed(*unit->time_till_dead)));
    });
    // DT-04, WTA-30: the hull and shield taken in the last 30-frame window, per second.
    object("Get_Rate_Of_Damage_Taken", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        return one(number(numeric::from_fixed(unit->damage_rate)));
    });
    // WSW-37/40: the Empire online handler retries its attached planetary source here.
    object("Fire_Special_Weapon", [host](
                                      BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (arguments.size() != 3) return fail("Fire_Special_Weapon expects an object target and a player");
        const ViewUnit* target = destination_object(*host, arguments[1]);
        const Handle* player = as_handle(arguments[2], handle_player);
        if (player == nullptr || player->id > std::numeric_limits<tactical::PlayerId>::max() ||
            host->view->player(static_cast<tactical::PlayerId>(player->id)) == nullptr)
            return fail("Fire_Special_Weapon: expected a player");
        if (as_handle(arguments[1], handle_game_object) == nullptr && as_handle(arguments[1], ai::handle_ai_target) == nullptr)
            return fail("Fire_Special_Weapon: expected an object or AI target");
        if (target == nullptr) return none();
        if (unit->planetary_weapon) {
            const auto issuer = static_cast<tactical::PlayerId>(player->id);
            if (!planetary_fire_ready(*host, *unit, *target, issuer)) return none();
            issue_special_fire(context, *unit, *target, issuer, tactical::SpecialFireAdmission::ai_object);
            return one(handle(handle_game_object, unit->id));
        }
        const AiType* type = unit != nullptr ? host->type(unit->type) : nullptr;
        if (type != nullptr && type->star_base) {
            if (!host->snapshot) return none();
            const auto instances = host->snapshot->instances();
            const auto source = std::lower_bound(instances.begin(), instances.end(), unit->id,
                [](const auto& instance, sim::EntityId id) { return instance.entity_id < id; });
            if (source == instances.end() || source->entity_id != unit->id || source->manual_weapons.empty()) {
                // WSW-40: a station lacking the authored gun warns without staging an order.
                context.report(authoritative::codes::unsupported_api, "Station has no manual special gun");
                return none();
            }
            const auto issuer = static_cast<tactical::PlayerId>(player->id);
            const auto* permission = host->economy(issuer);
            if (permission == nullptr || !permission->special_weapons_enabled || !host->snapshot) return none();
            // WSW-40: frozen gun readiness/admission, independent of the human player clock.
            const auto admitted = tactical::manual_special_fire_admission(*host->snapshot, unit->id, target->id);
            if (!admitted) return core::Result<ValueList>::failure(admitted.error());
            if (!admitted.value()) return none();
            issue_special_fire(context, *unit, *target, issuer, tactical::SpecialFireAdmission::ai_object);
            return one(handle(handle_game_object, unit->id));
        }
        return none();
    });
    // legacy #1713 (TSW-07): the readiness is the object's phase ready; it does not look at the gate or the owner. An
    // object whose type lacks the behaviour is a script error; an inert behaviour (a battle without the setup) is
    // never ready.
    object("Is_Tactical_Superweapon_Ready", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (!unit->tsw) return fail("Is_Tactical_Superweapon_Ready: not a tactical superweapon");
        return one(boolean(unit->tsw->active && unit->tsw->phase == tactical::TswPhase::ready));
    });
    // legacy #1713 (TSW-07): the power-up request. Accepted (true) when the object is ready and the gate is open: the
    // shared switch command is staged with the AI admission and the sequence starts at the next tick boundary, where
    // the command validator checks the same facts again. False when not ready, the gate is shut, or this instance
    // already asked for this object in this tick.
    object("Fire_Tactical_Superweapon", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr) return none();
        if (!unit->tsw) return fail("Fire_Tactical_Superweapon: not a tactical superweapon");
        if (!unit->tsw->active || unit->tsw->phase != tactical::TswPhase::ready || !unit->tsw->gate_open)
            return one(boolean(false));
        if (!host->stage_tsw_once(context.instance(), context.tick(), unit->id)) return one(boolean(false));
        issue_tsw_request(context, *unit, tactical::TswStage::start_power_up);
        return one(boolean(true));
    });
    object("Get_Build_Pad_Contents", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || host->world == nullptr) return none();
        const auto pad = host->world->pads().find(unit->id);
        if (pad == host->world->pads().end()) return none();
        const auto child = pad->second.constructed != 0 ? pad->second.constructed : pad->second.under_construction;
        return child != 0 ? one(handle(handle_game_object, child)) : none();
    });
    // EXP-E01 GameObject.Build(type[, check]): the object form of Build, on a build pad. One or two
    // arguments; the type is a name or a type handle, the optional Boolean adds the owner's faction-menu
    // and allow-construction checks (refusal answers false). The receiver needs build-pad behaviour and
    // neither an under-construction nor a completed child. It starts construction for the receiver's
    // owner with the local-debit and start-feedback flags false (WBP-09): no debit here. The mod's own
    // money adjustment (Give_Money after Build) and the TaskForce reservation (WBP-47/48) are the only
    // charges. Checks use the completed view; duplicate same-barrier starts answer true and are dropped
    // in ordered command resolution. A malformed call or an occupied or cooling pad reports an error and nil.
    object("Build", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto refuse = [&](const std::string& message) -> core::Result<ValueList> {
            context.report(authoritative::codes::script_error, "GameObject.Build -- " + message);
            return none();
        };
        if (arguments.size() < 2 || arguments.size() > 3) return refuse("invalid number of parameters. Expected 1 or 2");
        const ViewUnit* pad = live_object(*host, arguments[0]);
        if (pad == nullptr) return refuse("the receiver is not a live object");
        const AiType* type = nullptr;
        if (const std::string* name = as_text(arguments[1])) {
            const auto found = host->types_by_name.find(upper(*name));
            if (found != host->types_by_name.end()) type = found->second;
        } else if (const Handle* typed = as_handle(arguments[1], handle_type)) {
            type = host->type(static_cast<tactical::TypeId>(typed->id));
        }
        if (type == nullptr) return refuse("parameter 1 is not a valid game object type");
        bool checked = false;
        if (arguments.size() == 3) {
            const bool* flag = std::get_if<bool>(&arguments[2].data);
            if (flag == nullptr) return refuse("parameter 2 is not a boolean");
            checked = *flag;
        }
        if (!host->ai_world || !host->view) return refuse("no tactical world is hosted");
        const auto& world = *host->ai_world;
        const auto* profile = world.economy().pads.point(pad->type);
        if (profile == nullptr || !profile->build_pad) return refuse("the receiver has no build behavior");
        const auto state = world.pads().find(pad->id);
        if (state == world.pads().end()) return refuse("the receiver has no build-pad state");
        if (state->second.under_construction != 0 || state->second.constructed != 0)
            return refuse("the pad already holds, or has just been given, an under-construction or completed structure");
        const tactical::PlayerId owner = pad->owner;
        const auto* option = world.build_option(owner, pad->id, type->type_id);
        if (checked) {
            // The wrapper's refusal is an answer, not an error.
            if (option == nullptr || !option->available || !world.construction_allowed(owner, pad->id)) return one(boolean(false));
        }
        // The completed-view acceptance matches the pad order's checks in the simulation
        // (menu entry and availability even for the unchecked form, under-construction behavior, the roster
        // gate, the cooldown, the owner's fog). Anything the order would reject is an error and nil here.
        if (option == nullptr || !option->available) return refuse("the type is not available on the owner's faction menu");
        if (world.economy().pads.child(type->type_id) == nullptr) return refuse("the type has no under-construction behavior");
        if (world.economy().disabled_types.contains(type->type_id)) return refuse("the type is disabled in this match");
        if (host->view->tick < state->second.cooldown_until) return refuse("the pad is not ready for new construction");
        if (object_fogged(*host->view, *pad, owner)) return refuse("the pad is not revealed to its owner");
        if (!host->ai(owner)) return refuse("GameObject.Build is hosted for AI-owned pads; the owner has no AI");
        context.issue_command(std::string(verb_pad_build), {number(LuaNumber(static_cast<std::int64_t>(owner))),
            arguments[0], handle(handle_type, type->type_id)});
        return one(boolean(true));
    });
    // SOS-16, WSD-10: a squadron craft's parent is its live team container; any
    // other space object has none (debug build: the parent-container query).
    object("Get_Parent_Object", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewUnit* unit = live_object(*host, arguments[0]);
        if (unit == nullptr || !unit->craft || host->view->find(unit->squadron) == nullptr) return none();
        return one(handle(handle_game_object, unit->squadron));
    });
    // Type.Get_Combat_Rating is the type method below, not this object namesake.
    for (const char* name : {"Garrison", "Leave_Garrison", "Get_Combat_Rating"}) {
        object(name, missing(std::string("GameObject.") + name));
    }

    player("Get_ID", [](BindingContext&, const ValueList& arguments) {
        return one(number(LuaNumber(static_cast<std::int64_t>(std::get<Handle>(arguments[0].data).id))));
    });
    // RGC-126: the stored player name is a string, including when it is empty.
    player("Get_Name", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto found = host->players.find(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        if (found == host->players.end()) return none();
        return one(Value::text(found->second->name));
    });
    player("Get_Faction_Name", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto found = host->players.find(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        if (found == host->players.end()) return none();
        return one(Value::text(found->second->faction));
    });
    // RGC-210: Player.Get_Team answers the player's team number, or -1 for a player the match does not know
    // (debug build: the player wrapper returns the player's team, and -1 without a player). Allies share it (Is_Ally).
    player("Get_Team", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const ViewPlayer* found = host->view->player(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        return one(number(LuaNumber(found == nullptr ? std::int64_t{-1} : static_cast<std::int64_t>(found->team))));
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
    // WTA-47: the AI player's level; a player that is not a lobby AI answers Normal.
    player("Get_Difficulty", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        const auto found = host->players.find(self);
        const auto level = found == host->players.end() ? sim::tactical::AiLevel::normal : found->second->difficulty;
        return one(Value::text(std::string(sim::tactical::to_string(level))));
    });
    player("Is_Human", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        const auto found = host->players.find(self);
        return one(boolean(found != host->players.end() && !host->ai(self) && !found->second->neutral));
    });
    player("Is_Ally", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        const auto self = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
        const Handle* other = arguments.size() == 2 ? as_handle(arguments[1], handle_player) : nullptr;
        if (other == nullptr) {
            context.report(authoritative::codes::script_error, "Is_Ally expects one player");
            return one(boolean(false));
        }
        return one(boolean(host->allied(self, static_cast<tactical::PlayerId>(other->id))));
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
    player("Get_Credits", [host](BindingContext& context, const ValueList& arguments) {
        if (host->in_object_turn) {
            // SOS-T1: only this getter admits the immutable staged wallet; other economy reads stay guarded.
            const auto player = static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id);
            const auto* row = host->view->player(player);
            return one(number(numeric::from_fixed(row != nullptr ? row->turn_credits.value_or(math::Fixed{}) : math::Fixed{})));
        }
        auto immediate = galactic_immediate_query(context, *host, "galactic.credits", {arguments[0]});
        if (!immediate || !immediate.value().empty()) return immediate;
        return one(number(numeric::from_fixed(host->credits(
            static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id)))));
    });
    player("Get_Tech_Level", [host](BindingContext&, const ValueList& arguments) {
        const auto* account = host->economy(static_cast<tactical::PlayerId>(std::get<Handle>(arguments[0].data).id));
        return one(number(LuaNumber(account != nullptr ? account->tech_level : 0)));
    });
    player("Give_Money", [host](BindingContext& context, const ValueList& arguments) -> core::Result<ValueList> {
        if (arguments.size() != 2 || std::get_if<LuaNumber>(&arguments[1].data) == nullptr) return fail("Give_Money expects a credit amount");
        if (host->ai_world && host->ai_world->goal_mode() == "GALACTIC")
            return context.apply_immediate("galactic.give_money", arguments);
        const auto amount = numeric::to_fixed(std::get<LuaNumber>(arguments[1].data));
        if (!amount) return fail("Give_Money expects a credit amount in range");
        const auto player = std::get<Handle>(arguments[0].data).id;
        if (amount.value().raw() < 0) {
            // WGA-100: a negative amount is a debit with no affordability test, floored at zero credits.
            // Ordered command resolution clamps this request against earlier barrier wallet changes.
            if (host->in_object_turn) return fail("object turn dynamic-read tripwire: Give_Money");
            context.issue_command(std::string(pending_debit), {number(LuaNumber(static_cast<std::int64_t>(player))),
                handle(handle_game_object, 0), arguments[1]});
            return none();
        }
        if (amount.value().raw() == 0) return fail("Give_Money expects positive credits");
        context.issue_command(std::string(host->in_object_turn ? turn_grant : verb_credit_grant), {number(LuaNumber(static_cast<std::int64_t>(player))),
            handle(handle_game_object, 0), arguments[1]});
        return none();
    });
    type("Get_Name", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        return one(Value::text(found->name));
    });
    type("Get_Base_Level", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        // SSU-01: authored unsigned Base_Level; the method ignores extra arguments.
        const auto* found = host->type(std::get<Handle>(arguments[0].data).id);
        return found == nullptr ? none() : one(number(LuaNumber(static_cast<std::int64_t>(found->base_level))));
    });
    type("Get_Build_Cost", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        // RGC-125: generic tactical cost (WAS-26), without a producer's effective price.
        if (!found->tactical_cost) return fail("Get_Build_Cost: generic tactical cost metadata unavailable");
        return one(number(numeric::from_fixed(*found->tactical_cost)));
    });
    type("Get_Tactical_Build_Cost", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        // SSU-04: integer authored price; campaign uses a positive override, otherwise multiplayer price.
        const auto* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (!found) return none();
        auto cost = found->tactical_cost;
        if (host->setup.perception.campaign_game && found->tactical_campaign_cost
            && found->tactical_campaign_cost->raw() >= math::Fixed::scale) cost = found->tactical_campaign_cost;
        if (!cost) return fail("Get_Tactical_Build_Cost: tactical cost metadata unavailable");
        return one(number(LuaNumber(cost->raw() / math::Fixed::scale)));
    });
    type("Get_Max_Range", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        return one(number(found->max_attack_distance));
    });
    // SOS-16, WSD-10: the type's AI_Combat_Power metric in single precision; a
    // squadron type's is its craft sum (PL-13).
    type("Get_Combat_Rating", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        return one(number(ai::to_single(found->combat_power)));
    });
    type("Get_Min_Range", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        if (found == nullptr) return none();
        return one(number(found->min_attack_distance));
    });
    type("Is_Hero", [host](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const AiType* found = host->type(std::get<Handle>(arguments[0].data).id);
        return one(boolean(found != nullptr && found->hero));
    });
    // WTA-34: the defensive abilities test the projectile type's category. LASER_DEFENSE affects
    // MISSILE, ROCKET and MPTL_ROCKET; MISSILE_SHIELD only ROCKET and MPTL_ROCKET. A type that is not
    // a projectile answers false.
    const auto affected = [host](std::uint32_t mask) {
        return [host, mask](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
            return one(boolean(projectile_in_categories(*host, std::get<Handle>(arguments[0].data).id, mask)));
        };
    };
    type("Is_Affected_By_Laser_Defense", affected(laser_defense_categories));
    type("Is_Affected_By_Missile_Shield", affected(missile_shield_categories));
    // A position wrapper is valid unless it holds the unset value (1e18 on every
    // axis); a planar wrapper's z is 0, so it always is.
    const auto position = [&](std::string_view name, Binding binding) {
        if (auto added = scripts.register_method(handle_position, name, std::move(binding)); !added) errors.push_back(added.error());
    };
    position("Is_Valid", [](BindingContext&, const ValueList&) { return one(boolean(true)); });
    position("Get_XYZ", [](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
        const auto& wrapper = std::get<Handle>(arguments[0].data);
        return core::Result<ValueList>::success(ValueList{number(ai::binary32_value(static_cast<std::uint32_t>(wrapper.id >> 32U))),
            number(ai::binary32_value(static_cast<std::uint32_t>(wrapper.id & 0xffffffffU))), number(ai::binary32_value(wrapper.aux))});
    });
    if (install_type_lists) if (auto added = scripts.register_method(handle_type_list, "Parse", [](BindingContext&, const ValueList& arguments) -> core::Result<ValueList> {
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
