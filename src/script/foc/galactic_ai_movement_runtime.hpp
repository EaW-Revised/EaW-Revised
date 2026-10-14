#pragma once

#include "galactic_ai_actions.hpp"
#include "ai_engine.hpp"

namespace eawr::script::foc::galactic_ai {
// Rebound before native restore; no initialization, allocation or AI service.
inline void bind_movement_references(NativeActions& actions, std::weak_ptr<ai::Engine> engine) {
    actions.bind_movement_inventory([engine](const std::map<std::uint64_t, NativeActions::Movement>& movements) -> core::Result<void> {
        const auto owner = engine.lock();
        if (!owner) {
            core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
            error.message = "movement inventory requires its reconstructed native engine";
            return core::Result<void>::failure(std::move(error));
        }
        for (const auto request : owner->galactic_movement_requests()) {
            const auto found = movements.find(request);
            if (found == movements.end() || !found->second.registered) {
                core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
                error.message = "native block has no registered saved movement request";
                return core::Result<void>::failure(std::move(error));
            }
            const auto& movement = found->second;
            if (auto valid = owner->validate_galactic_movement(request, movement.plan_id, movement.taskforce_id, movement.block_id, true); !valid) return valid;
        }
        return core::Result<void>::success();
    });
    actions.bind_movement_references([engine](std::uint64_t request, const NativeActions::Movement& movement) {
        const auto owner = engine.lock();
        if (!owner) {
            core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
            error.message = "movement restore requires its reconstructed native engine";
            return core::Result<void>::failure(std::move(error));
        }
        return owner->validate_galactic_movement(request, movement.plan_id, movement.taskforce_id, movement.block_id, movement.registered);
    });
}
inline bool movement_subscribed(const NativeActions& actions, const ai::Engine& engine, std::uint64_t request) {
    const auto found = actions.movements().find(request);
    if (found == actions.movements().end() || !found->second.registered) return false;
    const auto& movement = found->second;
    if (movement.freestore_actor) {
        const auto* facts = actions.qualified_freestore().facts(movement.freestore_actor);
        return facts && facts->qualified && facts->object.owner == movement.player && facts->tracked_movement;
    }
    return static_cast<bool>(engine.validate_galactic_movement(request, movement.plan_id, movement.taskforce_id, movement.block_id, true));
}
} // namespace eawr::script::foc::galactic_ai
