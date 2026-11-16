#include "galactic_ai_intent_refusal.hpp"

#include <variant>

namespace eawr::script::foc::galactic_ai::refusal {
namespace {
using Shadow = sim::galactic::GalacticSession::NativeActionShadow;

Handler none() {
    return [](const IntentOutcome&) { return core::Result<bool>::success(false); };
}
} // namespace

core::Result<bool> apply(NativeActions& actions, const Consequence& consequence) {
    return consequence.kind == Consequence::Kind::movement ? actions.refuse_movements(consequence.player, consequence.id, consequence.requests)
                                                           : actions.refuse_purchase(consequence.player, consequence.id);
}

Handler fleet(NativeActions& actions, const IntentRecord& record, const Sink& sink) {
    const auto* request = std::get_if<Shadow::FleetRequest>(&record.request);
    if (!request) return none();
    // Only a hop carries a movement block; merges and member edits answer Lua directly.
    const auto* move = std::get_if<sim::galactic::MoveFleet>(&request->payload);
    if (!move) return none();
    return [target = &actions, player = request->player, fleet = move->fleet, sink](const IntentOutcome&) {
        if (!sink) return target->refuse_movement(player, fleet);
        // The blocks this refusal finishes are fixed now (what the direct write would finish); a block made before the AI's next
        // window start is a later movement and stays.
        Consequence consequence{Consequence::Kind::movement, player, fleet, target->unfinished_movements(player, fleet)};
        if (consequence.requests.empty()) return core::Result<bool>::success(false);
        if (sink(consequence)) return core::Result<bool>::success(true); // a persisted consequence is queued: counted as one
        return target->refuse_movement(player, fleet);
    };
}
Handler control(NativeActions&, const IntentRecord&) { return none(); }
Handler cash(NativeActions&, const IntentRecord&) { return none(); }
Handler force(NativeActions&, const IntentRecord&) { return none(); }
Handler production(NativeActions& actions, const IntentRecord& record, const Sink& sink) {
    const auto* request = std::get_if<Shadow::ProductionRequest>(&record.request);
    if (!request || !request->receipt.build) return none();
    return [target = &actions, player = request->request.player, build = request->receipt.build, sink](const IntentOutcome&) {
        if (!sink) return target->refuse_purchase(player, build);
        if (!target->purchase_refusable(player, build)) return core::Result<bool>::success(false);
        if (sink(Consequence{Consequence::Kind::purchase, player, build, {}})) return core::Result<bool>::success(true);
        return target->refuse_purchase(player, build);
    };
}
Handler finish(NativeActions& actions, const IntentRecord& record, const Sink& sink) {
    const auto* request = std::get_if<Shadow::FinishRequest>(&record.request);
    if (!request || !request->build) return none();
    // The finish names the purchase's own player; the purchase row carries it.
    return [target = &actions, build = request->build, sink](const IntentOutcome& outcome) {
        if (!sink) return target->refuse_purchase(outcome.player, build);
        if (!target->purchase_refusable(outcome.player, build)) return core::Result<bool>::success(false);
        if (sink(Consequence{Consequence::Kind::purchase, outcome.player, build, {}})) return core::Result<bool>::success(true);
        return target->refuse_purchase(outcome.player, build);
    };
}
// A refused invasion writes nothing here: its record lives in the world, and a refused one is simply never created. Once the
// lane's slice has applied, the request is neither in the world nor held by the lane, and the AI's readers (block service, the
// plan's IsFinished/Result and the release) finish its block as a failed invasion at the AI's next service: the timing of a
// queued movement consequence, with no AI-side record to save. A save in between keeps the waiting block as it is (the goal-state
// validator accepts an invasion block whose request is absent from the world).
Handler invasion(NativeActions&, const IntentRecord&) { return none(); }
Handler orbit_recheck(NativeActions&, const IntentRecord&) { return none(); }

Handler for_intent(NativeActions& actions, const IntentRecord& record, const Sink& sink) {
    switch (intent_kind(record)) {
    case IntentKind::fleet: return fleet(actions, record, sink);
    case IntentKind::control: return control(actions, record);
    case IntentKind::cash: return cash(actions, record);
    case IntentKind::force: return force(actions, record);
    case IntentKind::production: return production(actions, record, sink);
    case IntentKind::finish: return finish(actions, record, sink);
    case IntentKind::invasion:
    case IntentKind::invasion_release: return invasion(actions, record);
    case IntentKind::orbit_recheck: return orbit_recheck(actions, record);
    case IntentKind::journal: break;
    }
    return none();
}

IntentHooks standard_hooks(NativeActions& actions, const IntentRecord& record, IntentHooks extra, const Sink& sink) {
    if (!extra.on_refused) extra.on_refused = for_intent(actions, record, sink);
    if (!extra.on_admitted) if (const auto* request = std::get_if<Shadow::ProductionRequest>(&record.request); request && request->receipt.build) {
        // An admitted purchase bills by the canonical queue entry's price, which the commit reports as the outcome's actual price.
        extra.on_admitted = [target = &actions, player = request->request.player, build = request->receipt.build](const IntentOutcome& outcome) {
            return outcome.kind == IntentKind::production ? target->adopt_purchase_price(player, build, outcome.actual) : core::Result<void>::success();
        };
    }
    return extra;
}

} // namespace eawr::script::foc::galactic_ai::refusal
