#include "galactic_ai_intent_refusal.hpp"

#include <variant>

namespace eawr::script::foc::galactic_ai::refusal {
namespace {
using Shadow = sim::galactic::GalacticSession::NativeActionShadow;

Handler none() {
    return [](const IntentOutcome&) { return core::Result<bool>::success(false); };
}
} // namespace

Handler fleet(NativeActions& actions, const IntentRecord& record) {
    const auto* request = std::get_if<Shadow::FleetRequest>(&record.request);
    if (!request) return none();
    // Only a hop carries a movement block; merges and member edits answer Lua directly.
    const auto* move = std::get_if<sim::galactic::MoveFleet>(&request->payload);
    if (!move) return none();
    return [target = &actions, player = request->player, fleet = move->fleet](const IntentOutcome&) {
        return target->refuse_movement(player, fleet);
    };
}
Handler control(NativeActions&, const IntentRecord&) { return none(); }
Handler cash(NativeActions&, const IntentRecord&) { return none(); }
Handler force(NativeActions&, const IntentRecord&) { return none(); }
Handler production(NativeActions& actions, const IntentRecord& record) {
    const auto* request = std::get_if<Shadow::ProductionRequest>(&record.request);
    if (!request || !request->receipt.build) return none();
    return [target = &actions, player = request->request.player, build = request->receipt.build](const IntentOutcome&) {
        return target->refuse_purchase(player, build);
    };
}
Handler finish(NativeActions& actions, const IntentRecord& record) {
    const auto* request = std::get_if<Shadow::FinishRequest>(&record.request);
    if (!request || !request->build) return none();
    // The finish names the purchase's own player; the purchase row carries it.
    return [target = &actions, build = request->build](const IntentOutcome& outcome) {
        return target->refuse_purchase(outcome.player, build);
    };
}
Handler invasion(NativeActions&, const IntentRecord&) { return none(); }
Handler orbit_recheck(NativeActions&, const IntentRecord&) { return none(); }

Handler for_intent(NativeActions& actions, const IntentRecord& record) {
    switch (intent_kind(record)) {
    case IntentKind::fleet: return fleet(actions, record);
    case IntentKind::control: return control(actions, record);
    case IntentKind::cash: return cash(actions, record);
    case IntentKind::force: return force(actions, record);
    case IntentKind::production: return production(actions, record);
    case IntentKind::finish: return finish(actions, record);
    case IntentKind::invasion:
    case IntentKind::invasion_release: return invasion(actions, record);
    case IntentKind::orbit_recheck: return orbit_recheck(actions, record);
    case IntentKind::journal: break;
    }
    return none();
}

IntentHooks standard_hooks(NativeActions& actions, const IntentRecord& record, IntentHooks extra) {
    if (!extra.on_refused) extra.on_refused = for_intent(actions, record);
    return extra;
}

} // namespace eawr::script::foc::galactic_ai::refusal
