#pragma once

// Host state of the FoC tactical AI shared by the bindings and the engine side (#79, #449):
// the completed tick as the scripts read it, and the host's static tables. Private to
// eawr_script_foc.

#include "eawr/script/foc/tactical_ai.hpp"

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

inline LuaNumber lua_double(double value) { return LuaNumber::from_repr(std::bit_cast<std::uint64_t>(value)); }

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

// ---- The completed tick as the bindings read it -------------------------------------------

struct ViewUnit {
    sim::EntityId id{};
    tactical::TypeId type{};
    tactical::PlayerId owner{};
    tactical::TeamId team{};
    math::Vec3 position{};
    math::Quat rotation{math::identity_quat()};
    std::optional<math::Fixed> hull;     // durability hull and maximum, when it has any
    std::optional<math::Fixed> max_hull;
    std::optional<math::Fixed> max_shields;
    std::uint64_t visible_to{};          // snapshot player bits
    sim::EntityId attack_target{};       // weapon target: combat eligibility and in-range events
    sim::EntityId formation_target{};    // commanded formation target: Lua object queries
    bool moving{};                       // a planned path or turn
    bool formation_moving{};             // formation point travel for Lua order queries
    tactical::OrderKind order{tactical::OrderKind::none};
    sim::EntityId order_target{};
    bool container{};                    // a squadron's team container
    bool craft{};                        // a squadron member
    // #449 perception inputs.
    LuaNumber health{LuaNumber(1)};      // Get_Health_Percent: hull over maximum, 1 without durability
    LuaNumber shield{};                  // Get_Shield_Percent: shield over maximum, 0 without shields
    bool engines_online{true};
    std::vector<bool> hardpoint_destroyed; // HardPoints order
    sim::EntityId squadron{};            // a craft's team container
    std::vector<sim::EntityId> members;  // a container's live craft
    std::vector<tactical::AbilityStatus> abilities; // #76: the snapshot's (none on a container)
    tactical::TypeId purchase_type{}; // SAE-11/WHE-49: logical identity; combat still reads type
    std::uint64_t purchase_token{}; // SAE-11: distinguish concurrent arrivals of the same type
};

struct ViewPlayer {
    tactical::PlayerId id{};
    tactical::TeamId team{};
    std::size_t snapshot_index{};
};

struct WorldView {
    std::uint64_t tick{};
    std::vector<ViewUnit> units; // ascending ID
    std::vector<ViewPlayer> players; // ascending ID

    [[nodiscard]] const ViewUnit* find(sim::EntityId id) const {
        const auto found = std::lower_bound(units.begin(), units.end(), id,
            [](const ViewUnit& unit, sim::EntityId key) { return unit.id < key; });
        return found != units.end() && found->id == id ? &*found : nullptr;
    }
    [[nodiscard]] const ViewPlayer* player(tactical::PlayerId id) const {
        for (const ViewPlayer& entry : players) {
            if (entry.id == id) return &entry;
        }
        return nullptr;
    }
};

inline std::shared_ptr<const WorldView> build_view(const tactical::TacticalSession& world, const tactical::TacticalSnapshot& snapshot) {
    auto view = std::make_shared<WorldView>();
    view->tick = world.completed_tick();
    const auto snapshot_players = snapshot.players();
    for (std::size_t index = 0; index < snapshot_players.size(); ++index) {
        view->players.push_back(ViewPlayer{snapshot_players[index].player_id, snapshot_players[index].team_id, index});
    }
    std::map<sim::EntityId, const tactical::TacticalInstance*> instances;
    for (const auto& instance : snapshot.instances()) instances.emplace(instance.entity_id, &instance);
    std::map<sim::EntityId, const tactical::Squadron*> containers;
    std::map<sim::EntityId, sim::EntityId> craft;
    std::map<sim::EntityId, std::pair<sim::EntityId, bool>> formations;
    for (const auto& squadron : world.squadrons()) {
        containers.emplace(squadron.container, &squadron);
        if (const auto state = world.squadron_state(squadron.container)) {
            formations.emplace(squadron.container,
                std::pair{state->target, state->mode == tactical::SquadronMode::move});
        }
        for (const sim::EntityId member : squadron.members) craft.emplace(member, squadron.container);
    }
    const auto fraction = [](math::Fixed part, math::Fixed whole) {
        if (whole.raw() <= 0) return LuaNumber(0);
        return numeric::from_fixed(part) / numeric::from_fixed(whole);
    };
    for (const tactical::UnitState& unit : world.units()) {
        ViewUnit entry;
        entry.id = unit.entity_id;
        entry.type = unit.type_id;
        entry.purchase_type = tactical::purchase_identity(unit);
        entry.purchase_token = unit.purchase_token;
        entry.owner = unit.owner;
        entry.position = unit.position;
        entry.rotation = unit.rotation;
        entry.order = unit.order.kind;
        entry.order_target = unit.order.target;
        if (const auto squadron = containers.find(unit.entity_id); squadron != containers.end()) {
            entry.container = true;
            entry.members = squadron->second->members;
        }
        if (const auto member = craft.find(unit.entity_id); member != craft.end()) {
            entry.craft = true;
            entry.squadron = member->second;
        }
        if (const auto instance = instances.find(unit.entity_id); instance != instances.end()) {
            entry.team = instance->second->team;
            entry.visible_to = instance->second->visible_to;
            entry.abilities = instance->second->abilities;
            if (const auto& durability = instance->second->durability) {
                entry.hull = durability->hull;
                entry.max_hull = durability->max_hull;
                entry.health = fraction(durability->hull, durability->max_hull);
                entry.max_shields = durability->max_shields;
                if (durability->shields && durability->max_shields) {
                    entry.shield = fraction(*durability->shields, *durability->max_shields);
                }
                entry.engines_online = durability->engines_online;
                for (const auto& hardpoint : durability->hardpoints) {
                    entry.hardpoint_destroyed.push_back(hardpoint.state == tactical::HardpointState::destroyed);
                }
            }
        }
        if (const auto combat = world.combat_state(unit.entity_id)) entry.attack_target = combat->attack_target;
        if (const auto motion = world.motion_state(unit.entity_id)) entry.moving = motion->kind != tactical::MotionKind::none;
        // FH-23: a squadron (and its craft) reports the formation target before a weapon
        // target. Its container travels through the formation, without a ship motion state.
        const auto formation = formations.find(entry.craft ? entry.squadron : entry.id);
        if (formation != formations.end()) {
            entry.formation_target = formation->second.first;
            entry.formation_moving = formation->second.second;
        }
        view->units.push_back(entry);
    }
    return view;
}

// ---- Host state shared by the bindings and the engine -------------------------------------

// One friendly entry of a contrast list: a category mask or an exact type, and its weight.
struct ContrastEntry {
    std::uint64_t category_bits{};
    std::optional<tactical::TypeId> type;
    LuaNumber weight{};
};

struct Host {
    AiSetup setup;
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

    [[nodiscard]] const AiType* type(tactical::TypeId id) const {
        const auto found = types.find(id);
        return found == types.end() ? nullptr : found->second;
    }
    [[nodiscard]] const tactical::EconomyView* economy(tactical::PlayerId id) const {
        if (!snapshot) return nullptr;
        for (const auto& account : snapshot->economy()) if (account.player == id) return &account;
        return nullptr;
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

// A position argument: a game object (its position) or a position list {x, y, z}.
inline std::optional<math::Vec3> position_of(const Host& host, const Value& value) {
    if (const ViewUnit* unit = live_object(host, value)) return unit->position;
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
    if (unit.container) {
        for (const sim::EntityId member : unit.members) {
            if (const ViewUnit* craft = host.view->find(member)) add(*craft);
        }
    } else {
        add(unit);
    }
    if (!result.has) return AbilityView{false, false, false, false};
    return result;
}

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
inline void request_ability(BindingContext& context, const Host& host, const ViewUnit& unit, const std::string& ability,
    const Value* on) {
    const auto kind = tactical::ability_kind(ability);
    if (kind == tactical::AbilityKind::none) {
        if (authored_ability(host, unit, ability)) {
            context.report(authoritative::codes::unsupported_api, "Activate_Ability(" + ability + ") ignored: the ability is cut (#76)");
        }
        return;
    }
    const auto view = ability_view(host, unit, kind);
    if (!view.has || !view.ready) return;
    if (kind == tactical::AbilityKind::ion_cannon_shot) {
        context.report(authoritative::codes::unsupported_api,
            "Activate_Ability(" + ability + ") ignored: a targeted ability is not switched from Lua (#561)");
        return;
    }
    const bool* flag = on != nullptr ? std::get_if<bool>(&on->data) : nullptr;
    if (flag == nullptr) {
        context.report(authoritative::codes::unsupported_api, "Activate_Ability(" + ability + ") ignored: expects true or false");
        return;
    }
    ValueList arguments;
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(unit.owner))));
    arguments.push_back(handle(handle_game_object, unit.id));
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(kind))));
    const auto action = *flag ? tactical::AbilityAction::activate : tactical::AbilityAction::deactivate;
    arguments.push_back(number(LuaNumber(static_cast<std::int64_t>(action))));
    context.issue_command(std::string(verb_ability), std::move(arguments));
}

} // namespace detail
} // namespace eawr::script::foc
