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

// ---- Command translation (FH-40) --------------------------------------------------------

core::Result<authoritative::TacticalOrder> translate_order(const authoritative::ScriptCommand& command) {
    using OrderResult = core::Result<authoritative::TacticalOrder>;
    const auto bad = [&](std::string message) {
        return OrderResult::failure(make_error(authoritative::codes::command_unroutable, command.verb + ": " + std::move(message)));
    };
    const auto& arguments = command.arguments;
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
    if (command.verb == verb_credit_grant) {
        const auto* value = std::get_if<LuaNumber>(&arguments[2].data);
        if (arguments.size() != 3 || value == nullptr) return bad("malformed credit grant");
        const auto amount = numeric::to_fixed(*value);
        if (!amount || amount.value().raw() <= 0) return bad("invalid credit grant");
        return OrderResult::success(make_order({}, tactical::CreditGrantPayload{amount.value()}));
    }
    if (command.verb == verb_buy || command.verb == verb_pad_build || command.verb == verb_reinforce) {
        const Handle* type = as_handle(arguments[2], handle_type);
        if (type == nullptr) return bad("malformed production type");
        if (command.verb == verb_buy || command.verb == verb_pad_build) {
            if (arguments.size() != 3) return bad("buy expects producer and type");
            if (command.verb == verb_pad_build) {
                return OrderResult::success(make_order({unit->id}, tactical::PadBuildPayload{type->id}));
            }
            return OrderResult::success(make_order({unit->id}, tactical::BuyPayload{type->id}));
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
        if (arguments.size() != 4) return bad("expects an ability and an action");
        std::int64_t values[2]{};
        for (std::size_t index = 0; index < 2; ++index) {
            const auto* value = std::get_if<LuaNumber>(&arguments[2 + index].data);
            if (value == nullptr) return bad("malformed ability or action");
            auto exact = numeric::to_exact_integer(*value);
            if (!exact || exact.value() < 1 || exact.value() > 4) return bad("malformed ability or action");
            values[index] = exact.value();
        }
        return OrderResult::success(make_order({unit->id},
            tactical::AbilityPayload{static_cast<tactical::AbilityKind>(values[0]),
                static_cast<tactical::AbilityAction>(values[1])}));
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
