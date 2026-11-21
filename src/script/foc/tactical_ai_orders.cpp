// FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host").
// Engine rules cite the host rules FH-xx of that note; each names its evidence.

#include "eawr/script/foc/tactical_ai.hpp"

#include "tactical_ai_internal.hpp"
#include "eawr/sim/tactical/types.hpp"

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

// ---- Command translation (FH-40) --------------------------------------------------------

core::Result<authoritative::TacticalOrder> translate_order(const authoritative::ScriptCommand& command) {
    using OrderResult = core::Result<authoritative::TacticalOrder>;
    const auto bad = [&](std::string message) {
        return OrderResult::failure(make_error(authoritative::codes::command_unroutable, command.verb + ": " + std::move(message)));
    };
    const auto& arguments = command.arguments;
    if (command.verb == verb_create_object) {
        if (arguments.size() != 5) return bad("expects issuer, type, position, owner and identity");
        const auto* issuer = std::get_if<LuaNumber>(&arguments[0].data);
        const auto* type = as_handle(arguments[1], handle_type);
        const auto* owner = as_handle(arguments[3], handle_player);
        const auto* identity = as_handle(arguments[4], handle_game_object);
        const auto* point = std::get_if<std::vector<Value>>(&arguments[2].data);
        if (!issuer || !type || !owner || !identity || !point || point->size() != 3)
            return bad("malformed creation arguments");
        const auto player = numeric::to_exact_integer(*issuer);
        if (!player || player.value() < 0 || player.value() > std::numeric_limits<tactical::PlayerId>::max()
            || owner->id > std::numeric_limits<tactical::PlayerId>::max()) return bad("invalid player");
        math::Vec3 position;
        math::Fixed* axes[] = {&position.x, &position.y, &position.z};
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const auto* coordinate = std::get_if<LuaNumber>(&(*point)[axis].data);
            if (!coordinate) return bad("malformed coordinate");
            const auto fixed = numeric::to_fixed(*coordinate);
            if (!fixed) return bad("invalid coordinate");
            *axes[axis] = fixed.value();
        }
        return OrderResult::success({static_cast<tactical::PlayerId>(player.value()), {},
            tactical::ScriptCreateObjectPayload{type->id, static_cast<tactical::PlayerId>(owner->id), position, identity->id}});
    }
    if (command.verb == verb_reveal_all) {
        if (arguments.size() != 2) return bad("expects issuer and player");
        const auto* issuer = std::get_if<LuaNumber>(&arguments[0].data);
        const Handle* recipient = as_handle(arguments[1], handle_player);
        if (issuer == nullptr) return bad("malformed issuer");
        const auto player = numeric::to_exact_integer(*issuer);
        if (!player || player.value() < 0 || player.value() > std::numeric_limits<tactical::PlayerId>::max()
            || recipient == nullptr || recipient->id > std::numeric_limits<tactical::PlayerId>::max())
            return bad("malformed issuer or player");
        return OrderResult::success({static_cast<tactical::PlayerId>(player.value()), {},
            tactical::RevealAllPayload{static_cast<tactical::PlayerId>(recipient->id)}});
    }
    if (command.verb == verb_despawn) {
        if (arguments.size() != 2) return bad("expects issuer and unit");
        const auto* issuer = std::get_if<LuaNumber>(&arguments[0].data);
        const Handle* unit = as_handle(arguments[1], handle_game_object);
        if (issuer == nullptr || unit == nullptr) return bad("malformed issuer or unit");
        const auto player = numeric::to_exact_integer(*issuer);
        if (!player || player.value() < 0 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("malformed issuer");
        return OrderResult::success({static_cast<tactical::PlayerId>(player.value()), {unit->id}, tactical::ScriptDespawnPayload{}});
    }
    if (arguments.size() < 3) return bad("too few arguments");
    const auto* issuer = std::get_if<LuaNumber>(&arguments[0].data);
    const Handle* unit = as_handle(arguments[1], handle_game_object);
    if (issuer == nullptr || unit == nullptr) return bad("malformed issuer or unit");
    auto player = numeric::to_exact_integer(*issuer);
    if (!player || player.value() < 0) return bad("malformed issuer");
    const auto make_order = [&](std::vector<sim::EntityId> units, auto payload) {
        return authoritative::TacticalOrder{static_cast<tactical::PlayerId>(player.value()),
            std::move(units), std::move(payload)};
    };
    if (command.verb == verb_make_invulnerable) {
        if (arguments.size() != 3 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, unit and enabled");
        const auto* enabled = std::get_if<bool>(&arguments[2].data);
        if (enabled == nullptr) return bad("malformed enabled flag");
        return OrderResult::success(make_order({unit->id}, tactical::ScriptInvulnerablePayload{*enabled}));
    }
    if (command.verb == verb_prevent_all_fire) {
        if (arguments.size() != 3 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, unit and enabled");
        const auto* enabled = std::get_if<bool>(&arguments[2].data);
        if (enabled == nullptr) return bad("malformed enabled flag");
        return OrderResult::success(make_order({unit->id}, tactical::ScriptHoldFirePayload{*enabled}));
    }
    if (command.verb == verb_change_owner) {
        if (arguments.size() != 3 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, unit and new owner");
        const Handle* owner = as_handle(arguments[2], handle_player);
        if (owner == nullptr || owner->id > std::numeric_limits<tactical::PlayerId>::max()) return bad("malformed new owner");
        return OrderResult::success(make_order({unit->id}, tactical::ScriptChangeOwnerPayload{static_cast<tactical::PlayerId>(owner->id)}));
    }
    if (command.verb == verb_teleport_and_face) {
        if (arguments.size() != 3 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, unit and target");
        const Handle* target = as_handle(arguments[2], handle_game_object);
        if (target == nullptr || target->id == 0) return bad("malformed target");
        return OrderResult::success(make_order({unit->id}, tactical::ScriptTeleportPayload{target->id}));
    }
    if (command.verb == verb_take_damage) {
        // HD-30: the retail wrapper's debug-cheat damage. A hardpoint index of 0xffffffff is the hull.
        if ((arguments.size() != 3 && arguments.size() != 4) || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, unit, amount and an optional hardpoint");
        const auto* amount = std::get_if<LuaNumber>(&arguments[2].data);
        if (amount == nullptr) return bad("malformed amount");
        const auto fixed = numeric::to_fixed(*amount);
        if (!fixed || fixed.value().raw() < 0) return bad("invalid amount");
        std::uint32_t hardpoint = tactical::hull_target;
        if (arguments.size() == 4) {
            const auto* index = std::get_if<LuaNumber>(&arguments[3].data);
            if (index == nullptr) return bad("malformed hardpoint");
            const auto value = numeric::to_exact_integer(*index);
            if (!value || value.value() < 0 || value.value() >= std::numeric_limits<std::uint32_t>::max())
                return bad("malformed hardpoint");
            hardpoint = static_cast<std::uint32_t>(value.value());
        }
        return OrderResult::success(make_order({unit->id}, tactical::DamagePayload{fixed.value(), hardpoint}));
    }
    if (command.verb == verb_tactical_superweapon) {
        // TSW-07: the script call stages the shared switch command with the AI admission, never a human one.
        if (arguments.size() != 3 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, unit and stage");
        const auto* stage = std::get_if<LuaNumber>(&arguments[2].data);
        if (stage == nullptr) return bad("malformed stage");
        const auto value = numeric::to_exact_integer(*stage);
        if (!value || (value.value() != static_cast<std::int64_t>(tactical::TswStage::start_approach)
                && value.value() != static_cast<std::int64_t>(tactical::TswStage::start_power_up)))
            return bad("invalid super weapon stage");
        return OrderResult::success(make_order({unit->id}, tactical::TacticalSuperweaponPayload{
            static_cast<tactical::TswStage>(value.value()), tactical::TswAdmission::ai_object}));
    }
    if (command.verb == verb_special_fire) {
        if (arguments.size() != 4 || player.value() > std::numeric_limits<tactical::PlayerId>::max())
            return bad("expects issuer, source, target and AI admission");
        const auto* target = as_handle(arguments[2], handle_game_object);
        const auto* admission = std::get_if<LuaNumber>(&arguments[3].data);
        if (target == nullptr || admission == nullptr) return bad("malformed target or admission");
        const auto value = numeric::to_exact_integer(*admission);
        if (!value || (value.value() != static_cast<std::int64_t>(tactical::SpecialFireAdmission::ai_object) &&
            value.value() != static_cast<std::int64_t>(tactical::SpecialFireAdmission::ai_taskforce)))
            return bad("invalid AI admission");
        return OrderResult::success(make_order({unit->id}, tactical::SpecialFirePayload{target->id,
            static_cast<tactical::SpecialFireAdmission>(value.value())}));
    }
    if (command.verb == verb_credit_grant || command.verb == verb_reservation_debit) {
        const auto* value = std::get_if<LuaNumber>(&arguments[2].data);
        if (arguments.size() != 3 || value == nullptr) return bad("malformed credit grant");
        const auto amount = numeric::to_fixed(*value);
        if (!amount || amount.value().raw() <= 0) return bad("invalid credit grant");
        if (command.verb == verb_reservation_debit)
            return OrderResult::success(make_order({}, tactical::AiReservationDebitPayload{amount.value()}));
        return OrderResult::success(make_order({}, tactical::CreditGrantPayload{amount.value()}));
    }
    if (command.verb == verb_buy || command.verb == verb_pad_build || command.verb == verb_reinforce) {
        const Handle* type = as_handle(arguments[2], handle_type);
        if (type == nullptr) return bad("malformed production type");
        if (command.verb == verb_buy || command.verb == verb_pad_build) {
            if (arguments.size() != 3 && !(command.verb == verb_buy && arguments.size() == 4
                && std::get_if<bool>(&arguments[3].data) != nullptr)) return bad("buy expects producer and type");
            if (command.verb == verb_pad_build) {
                return OrderResult::success(make_order({unit->id}, tactical::PadBuildPayload{type->id}));
            }
            return OrderResult::success(make_order({unit->id}, tactical::BuyPayload{type->id,
                arguments.size() == 4 && std::get<bool>(arguments[3].data)}));
        } else {
            if (arguments.size() != 4 && arguments.size() != 5 && arguments.size() != 7) return bad("reinforce expects type, position and optional purchase token and search metadata");
            const auto* token = arguments.size() >= 5 ? std::get_if<Handle>(&arguments[4].data) : nullptr;
            if (arguments.size() >= 5 && (token == nullptr || token->kind != handle_type || token->id == 0))
                return bad("malformed reinforcement purchase token");
            const auto* position = std::get_if<std::vector<Value>>(&arguments[3].data);
            if (position == nullptr || position->size() != 3) return bad("malformed reinforcement position");
            math::Vec3 point;
            math::Fixed* axes[] = {&point.x, &point.y, &point.z};
            for (std::size_t i = 0; i < 3; ++i) {
                const auto* value = std::get_if<LuaNumber>(&(*position)[i].data);
                if (value == nullptr) return bad("malformed reinforcement coordinate");
                const auto fixed = numeric::to_fixed(*value);
                if (!fixed) return bad("invalid reinforcement coordinate");
                *axes[i] = fixed.value();
            }
            auto order = make_order({}, tactical::ReinforcePayload{type->id, point, token != nullptr ? token->id : 0});
            if (arguments.size() == 7) {
                const auto* search_token = as_handle(arguments[5], ai::handle_block);
                const auto* ring = std::get_if<LuaNumber>(&arguments[6].data);
                if (search_token == nullptr || ring == nullptr) return bad("malformed reinforcement search");
                const auto ring_value = numeric::to_exact_integer(*ring);
                if (search_token->id == 0 || !ring_value || ring_value.value() < 0
                    || ring_value.value() > std::numeric_limits<std::uint32_t>::max()) return bad("invalid reinforcement search");
                order.reinforcement_search = tactical::TacticalSession::ReinforcementSearch{
                    search_token->id, static_cast<std::uint32_t>(ring_value.value())};
            }
            return OrderResult::success(std::move(order));
        }
    }
    if (command.verb == verb_attack) {
        const Handle* target = as_handle(arguments[2], handle_game_object);
        if (target == nullptr) return bad("malformed target");
        return OrderResult::success(make_order({unit->id}, tactical::AttackPayload{target->id}));
    }
    if (command.verb == verb_ability) {
        if (arguments.size() != 4 && arguments.size() != 5 && arguments.size() != 7) return bad("expects an ability and an action");
        std::int64_t values[2]{};
        for (std::size_t index = 0; index < 2; ++index) {
            const auto* value = std::get_if<LuaNumber>(&arguments[2 + index].data);
            if (value == nullptr) return bad("malformed ability or action");
            auto exact = numeric::to_exact_integer(*value);
            // AB-44: any modelled ability kind (the AbilityKind enum's named kinds) and any action
            // (switch on, off, autofire on, off); the session itself refuses what the unit cannot do.
            const bool valid = exact && exact.value() >= 1 && (index == 0
                ? exact.value() <= std::numeric_limits<std::uint8_t>::max()
                    && tactical::to_string(static_cast<tactical::AbilityKind>(exact.value())) != "NONE"
                : exact.value() <= static_cast<std::int64_t>(tactical::AbilityAction::autofire_off));
            if (!valid) return bad("malformed ability or action");
            values[index] = exact.value();
        }
        if (arguments.size() == 4) {
            return OrderResult::success(make_order({unit->id},
                tactical::AbilityPayload{static_cast<tactical::AbilityKind>(values[0]),
                    static_cast<tactical::AbilityAction>(values[1])}));
        }
        if (arguments.size() == 5) {
            const Handle* target = as_handle(arguments[4], handle_game_object);
            if (!target || values[0] != static_cast<std::int64_t>(tactical::AbilityKind::leech_shields)
                || values[1] != static_cast<std::int64_t>(tactical::AbilityAction::activate))
                return bad("shield leech activation expects an object target");
            return OrderResult::success(make_order({unit->id}, tactical::AbilityPayload{
                tactical::AbilityKind::leech_shields, tactical::AbilityAction::activate, target->id}));
        }
        // WAB-76, WHE-61: BUZZ_DROIDS and WEAKEN_ENEMY from Lua switch on at a point.
        if ((values[0] != static_cast<std::int64_t>(tactical::AbilityKind::buzz_droids)
                && values[0] != static_cast<std::int64_t>(tactical::AbilityKind::corrupt_systems)
                && values[0] != static_cast<std::int64_t>(tactical::AbilityKind::weaken_enemy))
            || values[1] != static_cast<std::int64_t>(tactical::AbilityAction::activate))
            return bad("only an area ability activation takes a point");
        math::Vec3 point;
        math::Fixed* axes[] = {&point.x, &point.y, &point.z};
        for (std::size_t i = 0; i < 3; ++i) {
            const auto* coordinate = std::get_if<LuaNumber>(&arguments[4 + i].data);
            if (coordinate == nullptr) return bad("malformed ability point");
            auto fixed = numeric::to_fixed(*coordinate);
            if (!fixed) return bad("invalid ability point");
            *axes[i] = fixed.value();
        }
        tactical::AbilityPayload payload{static_cast<tactical::AbilityKind>(values[0]), tactical::AbilityAction::activate};
        payload.position = point;
        return OrderResult::success(make_order({unit->id}, payload));
    }
    const bool attack_move = command.verb == verb_attack_move;
    if ((attack_move || command.verb == verb_guard) && arguments.size() == 3) {
        const Handle* target = as_handle(arguments[2], handle_game_object);
        if (target == nullptr) return bad("malformed target");
        if (attack_move) {
            return OrderResult::success(make_order({unit->id}, tactical::AttackMovePayload{math::Vec3{}, target->id}));
        } else {
            return OrderResult::success(make_order({unit->id}, tactical::GuardPayload{math::Vec3{}, target->id}));
        }
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
        return OrderResult::success(make_order({unit->id}, tactical::AttackMovePayload{destination, sim::invalid_entity_id}));
    } else if (command.verb == verb_guard) {
        return OrderResult::success(make_order({unit->id}, tactical::GuardPayload{destination, sim::invalid_entity_id}));
    } else {
        return OrderResult::success(make_order({unit->id}, tactical::MovePayload{destination}));
    }
}

} // namespace tactical_ai_detail

} // namespace eawr::script::foc
