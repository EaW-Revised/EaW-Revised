#pragma once

#include "eawr/campaign/galactic_ai_services.hpp"
#include "galactic_ai_movement_runtime.hpp"
#include <algorithm>

namespace eawr::script::foc::galactic_ai {
// WGA-202/207: this adapter observes the real campaign boundary. Its owner
// supplies retained movement requests and actor/location-qualified listeners.
// WGA-204 seam: delivery may enqueue a handler, never service the parent.
using MovementBattleSubscription = std::function<bool(std::uint64_t request)>;
using MovementBattleDelivery = std::function<core::Result<void>(std::uint64_t request,
    bool begin, const campaign::GalacticAiBattleNotice&)>;

// MovementOwner is NativeActions in production. Keeping its narrow existing
// movements()/movement_conflict() interface lets the adapter be tested before
// the shared TaskForce/action bindings are composed.
template<class MovementOwner>
std::function<core::Result<void>(const campaign::GalacticAiBattleNotice&)> movement_battle_listener(
    std::shared_ptr<MovementOwner> owner, MovementBattleSubscription subscribed_request, MovementBattleDelivery delivery) {
    return [owner = std::move(owner), subscribed_request = std::move(subscribed_request), delivery = std::move(delivery)](const campaign::GalacticAiBattleNotice& notice) -> core::Result<void> {
        const auto refuse = [](const char* message) {
            core::Diagnostic diagnostic; diagnostic.code = "EAWR-GCAI-BATTLE-0001"; diagnostic.message = message;
            return core::Result<void>::failure(std::move(diagnostic));
        };
        if (!owner || !subscribed_request || !delivery) return refuse("battle listener requires retained owner, registered requests and delivery adapter");
        const bool begin = notice.kind == campaign::GalacticAiBattleKind::entered;
        if ((!begin && notice.kind != campaign::GalacticAiBattleKind::returned)
            || !notice.conflict.id || !notice.conflict.planet) return refuse("battle notice has no valid conflict identity");
        // GameSession publishes latest_tactical_result after this callback.
        // Validate and pass this finalized result; never query a previous one.
        if (!begin && (!notice.result || notice.result->conflict != notice.conflict.id))
            return refuse("battle return requires its current finalized result");
        if (notice.conflict.layer != campaign::BattleLayer::space) return core::Result<void>::success();
        for (const auto& [request, movement] : owner->movements()) {
            // WGA-202 clears finished at an active listener. Only the saved
            // listener/block association determines whether it is subscribed.
            if (!movement.actor || !subscribed_request(request)) continue;
            bool subscribed = !begin && movement.conflict_location == notice.conflict.planet;
            if (begin || movement.freestore_actor) {
                // WGA-388: free begin/end attach to the retained fleet actor.
                // A destination or plan target alone is not participation.
                subscribed = std::any_of(notice.conflict.forces.begin(), notice.conflict.forces.end(), [&](const auto& force) {
                    return force.owner == movement.player && (force.id == movement.actor
                        || force.parent == movement.actor || force.carrier == movement.actor);
                });
            }
            if (!subscribed) continue;
            if (auto changed = owner->movement_conflict(request, notice.conflict.planet, begin); !changed) return changed;
            if (auto delivered = delivery(request, begin, notice); !delivered) return delivered;
        }
        // Ground invasion end/result and auto-resolve begin come from their
        // actual world listeners, not a fabricated tactical-entry notice.
        return core::Result<void>::success();
    };
}
// Recreate the exact parent-owner closures before installing/restoring native AI.
inline auto retained_movement_battle_listener(std::shared_ptr<NativeActions> actions,
    std::shared_ptr<ai::Engine> engine, std::function<authoritative::ScriptScheduler*()> scheduler) {
    return movement_battle_listener(actions,
        [actions, engine](std::uint64_t request) {
            if (!actions) return false;
            const auto found = actions->movements().find(request);
            if (found != actions->movements().end() && found->second.registered && found->second.freestore_actor) {
                const auto* facts = actions->qualified_freestore().facts(found->second.freestore_actor);
                return facts && facts->qualified && facts->object.owner == found->second.player && facts->tracked_movement;
            }
            return engine && movement_subscribed(*actions, *engine, request);
        },
        [actions, engine, scheduler = std::move(scheduler)](std::uint64_t request, bool begin, const campaign::GalacticAiBattleNotice& notice) -> core::Result<void> {
            if (!actions || !actions->movements().contains(request)) {
                core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
                error.message = "battle delivery requires its retained native movement owner";
                return core::Result<void>::failure(std::move(error));
            }
            const auto& movement = actions->movements().at(request);
            if (movement.freestore_actor) {
                if (begin) return core::Result<void>::success();
                const auto fleet = movement.actor;
                const bool lost = std::none_of(notice.world.fleets().begin(), notice.world.fleets().end(),
                    [fleet](const auto& row) { return row.id == fleet; });
                const bool retreated = notice.result && notice.result->retreat_player == movement.player
                    && std::any_of(notice.conflict.forces.begin(), notice.conflict.forces.end(), [&](const auto& force) {
                        if (force.owner != movement.player || (force.id != fleet && force.parent != fleet && force.carrier != fleet)) return false;
                        return std::any_of(notice.result->forces.begin(), notice.result->forces.end(), [&](const auto& result) {
                            return result.id == force.id && result.disposition == campaign::ForceDisposition::retreated;
                        });
                    });
                return lost || retreated ? actions->invalidate_freestore_actor(request, fleet) : core::Result<void>::success();
            }
            auto* scripts = scheduler ? scheduler() : nullptr;
            if (!engine || !scripts) {
                core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
                error.message = "TaskForce battle delivery requires its retained engine and script owners";
                return core::Result<void>::failure(std::move(error));
            }
            return engine->deliver_galactic_space_conflict(request, movement.plan_id,
                movement.taskforce_id, movement.block_id, begin, *scripts);
        });
}
inline auto retained_movement_battle_listener(std::shared_ptr<NativeActions> actions,
    std::shared_ptr<ai::Engine> engine, std::shared_ptr<authoritative::ScriptScheduler> scripts) {
    return retained_movement_battle_listener(std::move(actions), std::move(engine),
        [scripts = std::move(scripts)] { return scripts.get(); });
}
} // namespace eawr::script::foc::galactic_ai
