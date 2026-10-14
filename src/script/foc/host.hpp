#pragma once

// Host state of the FoC tactical AI shared by the bindings and the engine side (#79, #449):
// the completed tick as the scripts read it, and the host's static tables. Private to
// eawr_script_foc.

#include "eawr/script/foc/tactical_ai.hpp"
#include "ai_equations.hpp"
#include "eawr/script/foc/galactic_ai.hpp"
#include "ai_world.hpp"
#include "ai_find_target.hpp"

#include "eawr/script/numeric/q24_boundary.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/tactical/abilities.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <bit>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::script::foc {
namespace ai {
class Engine;
}
namespace detail {

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

inline core::Diagnostic make_error(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return diagnostic;
}

inline core::Result<ValueList> fail(std::string message) {
    return core::Result<ValueList>::failure(make_error(authoritative::codes::invalid_request, std::move(message)));
}

inline core::Result<ValueList> none() { return core::Result<ValueList>::success(ValueList{}); }

inline core::Result<ValueList> one(Value value) {
    ValueList values;
    values.push_back(std::move(value));
    return core::Result<ValueList>::success(std::move(values));
}

inline Value boolean(bool value) { return Value{value}; }
inline Value number(LuaNumber value) { return Value::number(value); }
inline Value handle(std::uint32_t kind, std::uint64_t id) { return Value{Handle{kind, id}}; }

inline std::string upper(std::string_view text) {
    std::string out(text);
    for (char& character : out) {
        if (character >= 'a' && character <= 'z') character = static_cast<char>(character - ('a' - 'A'));
    }
    return out;
}

inline const Handle* as_handle(const Value& value, std::uint32_t kind) {
    const auto* found = std::get_if<Handle>(&value.data);
    return found != nullptr && found->kind == kind ? found : nullptr;
}

inline const std::string* as_text(const Value& value) { return std::get_if<std::string>(&value.data); }

// ---- Host state shared by the bindings and the engine -------------------------------------

// One friendly entry of a contrast list: a category mask or an exact type, and its weight.
struct ContrastEntry {
    std::uint64_t category_bits{};
    std::optional<tactical::TypeId> type;
    LuaNumber weight{};
};

struct Host {
    AiSetup setup;
    bool ai_suspended{}; // FD-72: ordered host gate, never written from a Lua worker.
    bool suspension_used{};
    std::shared_ptr<const ai::World> ai_world;
    // FT-05/07: immutable selected-manager/source/execution queries during Lua.
    std::shared_ptr<const ai::FindTargetQueries> find_target_queries;
    // Ordered native owner only. Partitioned Lua bindings must retain the
    // immediate-action scheduler seam rather than mutate this capability.
    std::shared_ptr<galactic_ai::Actions> galactic_execution;
    // SAE-02/03: borrowed immutable world during the barrier's script service.
    const tactical::TacticalSession* world{};
    std::shared_ptr<const tactical::TacticalSnapshot> snapshot;
    std::map<tactical::TypeId, const AiType*> types;
    std::map<std::string, const AiType*, std::less<>> types_by_name;
    std::map<tactical::PlayerId, const AiPlayer*> players;
    // Enemy category bit -> friendly weights (FH-30).
    std::map<std::uint64_t, std::vector<ContrastEntry>> contrast;
    // The enemy categories in EnemyContrastTypes order (PL-31).
    std::vector<std::uint64_t> contrast_order;
    // Written serially by the engine before each service; the bindings only read it.
    std::shared_ptr<const WorldView> view;
    // The goal system engine (#449); null when only the freestore runs. The bindings only read it.
    const ai::Engine* engine{};
    std::map<tactical::PlayerId, math::Fixed> credit_changes; // current barrier's ordered wallet changes

    [[nodiscard]] const AiType* type(tactical::TypeId id) const {
        const auto found = types.find(id);
        return found == types.end() ? nullptr : found->second;
    }
    [[nodiscard]] const tactical::EconomyView* economy(tactical::PlayerId id) const {
        if (ai_world) return ai_world->account(id);
        if (!snapshot) return nullptr;
        for (const auto& account : snapshot->economy()) if (account.player == id) return &account;
        return nullptr;
    }
    [[nodiscard]] math::Fixed credits(tactical::PlayerId id) const {
        const auto* account = economy(id);
        if (account == nullptr) return {};
        const auto change = credit_changes.find(id);
        return math::Fixed::from_raw(account->credits.raw()
            + (change != credit_changes.end() ? change->second.raw() : 0));
    }
    [[nodiscard]] bool neutral(tactical::PlayerId id) const {
        const auto found = players.find(id);
        return found != players.end() && found->second->neutral;
    }
    [[nodiscard]] bool ai(tactical::PlayerId id) const {
        const auto found = players.find(id);
        return found != players.end() && found->second->ai;
    }
    [[nodiscard]] bool allied(tactical::PlayerId a, tactical::PlayerId b) const {
        const ViewPlayer* left = view->player(a);
        const ViewPlayer* right = view->player(b);
        return left != nullptr && right != nullptr && left->team == right->team;
    }
};

// Ordinary completed-view reads remain available without the opt-in provider.
// Once installed, private GC writes are consulted before that view.
inline core::Result<ValueList> galactic_immediate_query(BindingContext& context, const Host& host,
    std::string_view name, const ValueList& arguments) {
    if (!host.ai_world || host.ai_world->goal_mode() != "GALACTIC") return none();
    auto result = context.query_immediate(name, arguments);
    if (!result && result.error().code == authoritative::codes::missing_api) return none();
    return result;
}

inline const ViewUnit* live_object(const Host& host, const Value& value) {
    const Handle* found = as_handle(value, handle_game_object);
    return found == nullptr ? nullptr : host.view->find(found->id);
}

inline Value position_value(const math::Vec3& position) {
    std::vector<Value> list;
    list.push_back(number(numeric::from_fixed(position.x)));
    list.push_back(number(numeric::from_fixed(position.y)));
    list.push_back(number(numeric::from_fixed(position.z)));
    return Value{std::move(list)};
}

// A position wrapper (Find_Best_Local_Threat_Center): planar x and y as
// binary32 bits in the handle id, so it round-trips exactly; z is 0.
inline Value position_handle(LuaNumber x, LuaNumber y) {
    return handle(handle_position, (std::uint64_t{ai::binary32_bits(x)} << 32U) | ai::binary32_bits(y));
}

inline std::optional<math::Vec3> position_of_handle(const Handle& wrapper) {
    auto x = numeric::to_fixed(ai::binary32_value(static_cast<std::uint32_t>(wrapper.id >> 32U)));
    auto y = numeric::to_fixed(ai::binary32_value(static_cast<std::uint32_t>(wrapper.id & 0xffffffffU)));
    if (!x || !y) return std::nullopt;
    return math::Vec3{x.value(), y.value(), {}};
}

// A position argument: a game object (its position), a position wrapper or a
// position list {x, y, z}.
inline std::optional<math::Vec3> position_of(const Host& host, const Value& value) {
    if (const ViewUnit* unit = live_object(host, value)) return unit->position;
    if (const Handle* wrapper = as_handle(value, handle_position)) return position_of_handle(*wrapper);
    const auto* list = std::get_if<std::vector<Value>>(&value.data);
    if (list == nullptr || list->size() != 3) return std::nullopt;
    math::Vec3 out;
    math::Fixed* axes[] = {&out.x, &out.y, &out.z};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const auto* coordinate = std::get_if<LuaNumber>(&(*list)[axis].data);
        if (coordinate == nullptr) return std::nullopt;
        auto fixed = numeric::to_fixed(*coordinate);
        if (!fixed) return std::nullopt;
        *axes[axis] = fixed.value();
    }
    return out;
}

struct UnitDestination {
    sim::EntityId object{};
    math::Vec3 position{};
};

// FH-42: attack-target accepts objects directly or through an AI target.
const ViewUnit* destination_object(const Host& host, const Value& value);

// WSW-13/14/16: planetary admission is category-only, independent of fog and range.
inline bool planetary_fire_ready(const Host& host, const ViewUnit& source, const ViewUnit& target,
    tactical::PlayerId player) {
    const auto* permission = host.economy(player);
    const auto* target_type = host.type(target.type);
    return source.planetary_weapon && permission != nullptr && permission->special_weapons_enabled &&
        source.planetary_weapon->readiness.raw() >= math::Fixed::scale && target_type != nullptr &&
        (!target.hull || target.hull->raw() > 0) &&
        !host.allied(source.owner, target.owner) &&
        (source.planetary_weapon->valid_targets & target_type->category_bits) != 0;
}

inline void issue_special_fire(BindingContext& context, const ViewUnit& source, const ViewUnit& target,
    tactical::PlayerId player, tactical::SpecialFireAdmission admission) {
    context.issue_command(std::string(verb_special_fire), {
        number(LuaNumber(static_cast<std::int64_t>(player))), handle(handle_game_object, source.id),
        handle(handle_game_object, target.id), number(LuaNumber(static_cast<std::int64_t>(admission)))});
}

// FH-41: movement takes a point; attack-move and guard may retain an object.
std::optional<UnitDestination> unit_destination(const Host& host, const ViewUnit& mover,
    const Value& value, bool follow_object, bool guard);

inline std::optional<math::Fixed> distance(const math::Vec3& a, const math::Vec3& b) {
    auto x = math::subtract(b.x, a.x);
    auto y = math::subtract(b.y, a.y);
    auto z = math::subtract(b.z, a.z);
    if (!x || !y || !z) return std::nullopt;
    auto length = math::length(math::Vec3{x.value(), y.value(), z.value()});
    if (!length) return std::nullopt;
    return length.value();
}

// FH-30: the average contrast factor of a type against an enemy category (no plan).
void register_plan_bindings(ScriptScheduler& scripts, const std::shared_ptr<Host>& host, std::vector<core::Diagnostic>& errors);

inline LuaNumber average_contrast(const Host& host, const AiType& self, std::uint64_t category) {
    const auto list = host.contrast.find(category);
    if (list == host.contrast.end()) return LuaNumber(0);
    LuaNumber sum(0);
    int counted = 0;
    bool matched = false;
    for (const ContrastEntry& entry : list->second) {
        if (entry.type) {
            if (*entry.type == self.type_id) return entry.weight;
            continue;
        }
        if ((entry.category_bits & self.category_bits) == 0) continue;
        matched = true;
        if (!(entry.weight == LuaNumber(1))) {
            sum += entry.weight;
            ++counted;
        }
    }
    if (counted == 0) return matched ? LuaNumber(1) : LuaNumber(0);
    return sum / LuaNumber(counted);
}

// ---- Unit abilities (#76, docs/behaviour/space-abilities.md AB-44) ------------------------

// One modelled ability of a unit as the Lua ability calls read it. A squadron container stands
// for its live craft that have it (AB-15): it has the ability when one does, is ready when all
// are, and is on or on autofire when all are.
struct AbilityView {
    bool has{};
    bool ready{true};
    bool active{true};
    bool autofire{true};
};

inline AbilityView ability_view(const Host& host, const ViewUnit& unit, tactical::AbilityKind kind) {
    AbilityView result;
    const auto add = [&](const ViewUnit& holder) {
        for (const tactical::AbilityStatus& status : holder.abilities) {
            if (status.kind != kind) continue;
            result.has = true;
            result.ready = result.ready && status.ready;
            result.active = result.active && status.active;
            result.autofire = result.autofire && status.autofire;
        }
    };
    if (unit.container && kind == tactical::AbilityKind::buzz_droids) {
        add(unit); // WAB-70: a team ability the container holds itself (AB-60)
    } else if (unit.container) {
        for (const sim::EntityId member : unit.members) {
            if (const ViewUnit* craft = host.view->find(member)) add(*craft);
        }
    } else {
        add(unit);
    }
    if (!result.has) return AbilityView{false, false, false, false};
    return result;
}

// WTA-34: a team answers the projectile queries for its leader, its first live craft.
inline const ViewUnit* projectile_leader(const Host& host, const ViewUnit& unit) {
    if (!unit.container) return &unit;
    return unit.members.empty() ? nullptr : host.view->find(unit.members.front());
}

// WTA-34, Get_All_Projectile_Types: the projectile of every standing hardpoint in HardPoints
// order, then the object weapon's.
inline std::vector<tactical::TypeId> projectile_types_of(const Host& host, const ViewUnit& unit) {
    std::vector<tactical::TypeId> types;
    const ViewUnit* leader = projectile_leader(host, unit);
    const AiType* type = leader != nullptr ? host.type(leader->type) : nullptr;
    if (type == nullptr) return types;
    for (std::size_t index = 0; index < type->hardpoint_projectiles.size(); ++index) {
        const bool destroyed = index < leader->hardpoint_destroyed.size() && leader->hardpoint_destroyed[index];
        if (!destroyed && type->hardpoint_projectiles[index] != 0) types.push_back(type->hardpoint_projectiles[index]);
    }
    if (type->object_projectile != 0) types.push_back(type->object_projectile);
    return types;
}

// WTA-34: whether a projectile type is in one of the categories an ability's query tests.
inline bool projectile_in_categories(const Host& host, tactical::TypeId projectile, std::uint32_t mask) {
    const auto& categories = host.setup.content.projectile_categories;
    const auto found = categories.find(projectile);
    return found != categories.end() && (found->second & mask) != 0;
}
inline constexpr std::uint32_t laser_defense_categories = ai_projectile_missile | ai_projectile_rocket | ai_projectile_mptl_rocket;
inline constexpr std::uint32_t missile_shield_categories = ai_projectile_rocket | ai_projectile_mptl_rocket;

// Whether the unit's type (or a container's craft's) authors the ability, cut ones included.
inline bool authored_ability(const Host& host, const ViewUnit& unit, const std::string& ability) {
    const auto has_type = [&](const ViewUnit& holder) {
        const AiType* type = host.type(holder.type);
        return type != nullptr && std::find(type->abilities.begin(), type->abilities.end(), ability) != type->abilities.end();
    };
    if (has_type(unit)) return true;
    for (const sim::EntityId member : unit.members) {
        if (const ViewUnit* craft = host.view->find(member); craft != nullptr && has_type(*craft)) return true;
    }
    return false;
}

// Activate_Ability(name, on) on one unit (FH-24, AB-13, AB-44): an ability the unit lacks draws
// only a script warning, a cut one (AB-03) is reported, and a modelled one that is ready is
// switched on or off by an ability command. The four power modes take no target; the targeted
// ION_CANNON_SHOT (#561, AB-69) is not switched from Lua: its autofire serves the engine's players.
// Answers whether a BUZZ_DROIDS activation was issued (WAB-76: the unit call reports it to its script).
inline bool request_ability(BindingContext& context, const Host& host, const ViewUnit& unit, const std::string& ability,
    const Value* on) {
    const auto kind = tactical::ability_kind(ability);
    if (kind == tactical::AbilityKind::none) {
        if (authored_ability(host, unit, ability)) {
            context.report(authoritative::codes::unsupported_api, "Activate_Ability(" + ability + ") ignored: the ability is cut (#76)");
        }
        return false;
    }
    const auto view = ability_view(host, unit, kind);
    if (!view.has || !view.ready) return false;
    if (kind == tactical::AbilityKind::ion_cannon_shot) {
        context.report(authoritative::codes::unsupported_api,
            "Activate_Ability(" + ability + ") ignored: a targeted ability is not switched from Lua (#561)");
        return false;
    }
    if (kind == tactical::AbilityKind::buzz_droids || kind == tactical::AbilityKind::corrupt_systems
        || (kind == tactical::AbilityKind::weaken_enemy && on != nullptr && std::get_if<bool>(&on->data) == nullptr)) {
        // WAB-76: FoC turns the second argument into an area marker at its position (an object's
        // position or a point) and activates the ability there. WHE-61: Weaken Enemy
        // switches on at a supplied position; a boolean keeps the owner-position path.
        std::optional<math::Vec3> point;
        if (on != nullptr) {
            if (const ViewUnit* object = destination_object(host, *on)) point = object->position;
            else point = position_of(host, *on);
        }
        if (!point) {
            context.report(authoritative::codes::unsupported_api, "Activate_Ability(" + ability + ") ignored: expects a target position");
            return false;
        }
        ValueList arguments;
        arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(unit.owner))));
        arguments.push_back(handle(handle_game_object, unit.id));
        arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(kind))));
        arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(tactical::AbilityAction::activate))));
        arguments.push_back(number(numeric::from_fixed(point->x)));
        arguments.push_back(number(numeric::from_fixed(point->y)));
        arguments.push_back(number(numeric::from_fixed(point->z)));
        context.issue_command(std::string(verb_ability), std::move(arguments));
        return true;
    }
    if (kind == tactical::AbilityKind::leech_shields) {
        // WLS-04: targeted Lua activation uses a live object; stock skirmish scripts do not call it.
        const bool* flag = on != nullptr ? std::get_if<bool>(&on->data) : nullptr;
        const auto* target = on != nullptr ? destination_object(host, *on) : nullptr;
        if (!target && (flag == nullptr || *flag)) return false;
        ValueList arguments;
        arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(unit.owner))));
        arguments.push_back(handle(handle_game_object, unit.id));
        arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(kind))));
        arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(target
            ? tactical::AbilityAction::activate : tactical::AbilityAction::deactivate))));
        if (target) arguments.push_back(handle(handle_game_object, target->id));
        context.issue_command(std::string(verb_ability), std::move(arguments));
        return true;
    }
    const bool* flag = on != nullptr ? std::get_if<bool>(&on->data) : nullptr;
    if (flag == nullptr) {
        context.report(authoritative::codes::unsupported_api, "Activate_Ability(" + ability + ") ignored: expects true or false");
        return false;
    }
    ValueList arguments;
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(unit.owner))));
    arguments.push_back(handle(handle_game_object, unit.id));
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(kind))));
    const auto action = *flag ? tactical::AbilityAction::activate : tactical::AbilityAction::deactivate;
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(action))));
    context.issue_command(std::string(verb_ability), std::move(arguments));
    return false;
}

} // namespace detail
} // namespace eawr::script::foc
