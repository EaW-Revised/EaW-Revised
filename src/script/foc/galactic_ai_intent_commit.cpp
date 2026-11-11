#include "galactic_ai_intent_commit.hpp"
#include "galactic_ai_economic_replay.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace eawr::script::foc::galactic_ai {
namespace {
using Shadow = sim::galactic::GalacticSession::NativeActionShadow;

template<class T> core::Result<T> invalid(std::string message) {
    core::Diagnostic error; error.code = std::string(authoritative::codes::invalid_request);
    error.message = std::move(message); return core::Result<T>::failure(std::move(error));
}

// What the derived journal rows that follow an intent must do. The player's turn journals
// through the same shadow as its effects, so a row can only follow the intent it belongs to.
struct Gate {
    bool present{};
    bool admitted{};
    bool world_journaled{};  // the readmission appended the actual row itself
    bool cash{};             // the commit appended the actual cash row itself
};


void tally(IntentCommitReport& report, const IntentOutcome& outcome) {
    switch (outcome.fate) {
    case IntentFate::admitted: ++report.admitted; break;
    case IntentFate::adjusted: ++report.adjusted; break;
    case IntentFate::refused: ++report.refused; break;
    case IntentFate::journaled: ++report.journaled; break;
    case IntentFate::dropped: ++report.dropped; break;
    }
    if (outcome.consequence) ++report.consequences;
    if (outcome.stale_identity) ++report.stale;
}

// An allocating intent whose first identity the world has already passed. Non-allocating kinds
// (cash, finish, journal, orbit recheck, release) carry equal first and next identities.
bool stale_identities(const sim::galactic::GalacticSession& session, const IntentRecord& record, std::uint64_t identity_floor,
    std::uint64_t invasion_floor) {
    return (record.next_id != record.first_id && record.first_id < (identity_floor ? identity_floor : session.next_entity_id()))
        || (record.next_invasion != record.first_invasion
            && record.first_invasion < (invasion_floor ? invasion_floor : session.next_native_invasion_id()));
}

core::Result<void> settle(IntentCommitReport& report, const IntentHooks& hooks, IntentOutcome outcome) {
    if (outcome.fate == IntentFate::refused) {
        if (hooks.on_refused) {
            auto applied = hooks.on_refused(outcome);
            if (!applied) return core::Result<void>::failure(applied.error());
            outcome.consequence = applied.value();
        }
    } else if (outcome.fate == IntentFate::admitted || outcome.fate == IntentFate::adjusted) {
        if (hooks.on_admitted) if (auto done = hooks.on_admitted(outcome); !done) return done;
    }
    tally(report, outcome);
    report.outcomes.push_back(std::move(outcome));
    return core::Result<void>::success();
}
} // namespace

IntentKind intent_kind(const IntentRecord& record) noexcept {
    const auto& request = record.request;
    if (std::holds_alternative<Shadow::FleetRequest>(request)) return IntentKind::fleet;
    if (std::holds_alternative<Shadow::ControlRequest>(request)) return IntentKind::control;
    if (std::holds_alternative<Shadow::CashRequest>(request)) return IntentKind::cash;
    if (std::holds_alternative<Shadow::ForceRequest>(request)) return IntentKind::force;
    if (std::holds_alternative<Shadow::ProductionRequest>(request)) return IntentKind::production;
    if (std::holds_alternative<Shadow::FinishRequest>(request)) return IntentKind::finish;
    if (const auto* invasion = std::get_if<Shadow::InvasionRequest>(&request))
        return invasion->retire ? IntentKind::invasion_release : IntentKind::invasion;
    if (std::holds_alternative<Shadow::JournalRequest>(request)) return IntentKind::journal;
    return IntentKind::orbit_recheck;
}

const char* intent_kind_name(IntentKind kind) noexcept {
    switch (kind) {
    case IntentKind::fleet: return "fleet";
    case IntentKind::control: return "control";
    case IntentKind::cash: return "cash";
    case IntentKind::force: return "force";
    case IntentKind::production: return "production";
    case IntentKind::finish: return "finish";
    case IntentKind::invasion: return "invasion";
    case IntentKind::invasion_release: return "invasion-release";
    case IntentKind::journal: return "journal";
    case IntentKind::orbit_recheck: return "orbit-recheck";
    }
    return "unknown";
}

IntentLedger::IntentLedger(std::span<const sim::galactic::PlayerId> players) {
    slots_.reserve(players.size());
    for (const auto player : players)
        if (!find(player)) slots_.push_back({player, {}, {}});
}
IntentLedger::Slot* IntentLedger::find(sim::galactic::PlayerId player) noexcept {
    for (auto& slot : slots_) if (slot.player == player) return &slot;
    return nullptr;
}
void IntentLedger::bind_hooks(sim::galactic::PlayerId player, HookFactory factory) {
    if (auto* slot = find(player)) slot->factory = std::move(factory);
    else ++unknown_;
}
void IntentLedger::submit(sim::galactic::PlayerId player, IntentRecord record, IntentHooks hooks) {
    auto* slot = find(player);
    if (!slot) { ++unknown_; return; }
    if (!hooks.on_refused && !hooks.on_admitted && !hooks.force_builder && !hooks.production_preflight && slot->factory)
        hooks = slot->factory(record, slot->intents.size());
    slot->intents.push_back({player, std::move(record), std::move(hooks)});
}
IntentSink IntentLedger::sink() {
    return [this](sim::galactic::PlayerId player, IntentRecord record) { submit(player, std::move(record)); };
}
std::size_t IntentLedger::size() const noexcept {
    std::size_t total{};
    for (const auto& slot : slots_) total += slot.intents.size();
    return total;
}
std::vector<PlayerIntent> IntentLedger::take_ordered() {
    std::vector<Slot*> order;
    order.reserve(slots_.size());
    for (auto& slot : slots_) order.push_back(&slot);
    std::sort(order.begin(), order.end(), [](const Slot* a, const Slot* b) { return a->player < b->player; });
    std::vector<PlayerIntent> ordered;
    ordered.reserve(size());
    for (auto* slot : order) {
        for (auto& intent : slot->intents) ordered.push_back(std::move(intent));
        slot->intents.clear();
    }
    return ordered;
}

core::Result<IntentCommitReport> commit_ordered_intents(sim::galactic::GalacticSession& session,
    IntentLedger& ledger, const sim::PartitionExecutor& executor, const IntentCommitOptions& options) {
    if (ledger.unknown_players())
        return invalid<IntentCommitReport>("an AI intent came from a player that is not part of this barrier");
    return commit_ordered_intents(session, ledger.take_ordered(), executor, options);
}

void order_slice_intents(std::vector<PlayerIntent>& intents) {
    // Ascending player ID wins every tie; stable, so each player's own order is untouched.
    std::stable_sort(intents.begin(), intents.end(),
        [](const PlayerIntent& a, const PlayerIntent& b) { return a.player < b.player; });
}

bool slice_step_stale(const sim::galactic::GalacticSession& session, const IntentRecord& record, const IntentCommitOptions& options) {
    return options.refuse_stale_identities && stale_identities(session, record, options.identity_floor, options.invasion_floor);
}

std::vector<PlannedIntent> plan_slice(const std::vector<PlayerIntent>& intents) {
    // The order of order_slice_intents (ascending player, stable), as a view: the overlay plans pending slices it must not move.
    std::vector<const PlayerIntent*> view;
    view.reserve(intents.size());
    for (const auto& intent : intents) view.push_back(&intent);
    std::stable_sort(view.begin(), view.end(), [](const PlayerIntent* a, const PlayerIntent* b) { return a->player < b->player; });
    std::vector<PlannedIntent> plan;
    plan.reserve(view.size());
    sim::galactic::PlayerId current{};
    std::size_t index{};
    for (const auto* intent : view) {
        const bool first = plan.empty() || intent->player != current;
        if (first) { current = intent->player; index = 0; }
        plan.push_back({intent, index++, first});
    }
    return plan;
}

core::Result<IntentCommitReport> commit_ordered_intents(sim::galactic::GalacticSession& session,
    std::vector<PlayerIntent> intents, const sim::PartitionExecutor& executor, const IntentCommitOptions& options) {
    using Report = core::Result<IntentCommitReport>;
    const auto plan = plan_slice(intents);
    IntentCommitReport report;
    report.outcomes.reserve(intents.size());
    Gate gate;
    for (const auto& step : plan) {
        const auto& intent = *step.intent;
        if (step.first_of_player) gate = {};
        IntentOutcome outcome;
        outcome.player = intent.player; outcome.index = step.index;
        outcome.kind = intent_kind(intent.record);
        const auto& record = intent.record;
        const auto failed = [](const auto& error) { return Report::failure(error); };
        const auto done = [&](IntentOutcome&& result) -> core::Result<void> {
            return settle(report, intent.hooks, std::move(result));
        };
        if (slice_step_stale(session, record, options)) {
            outcome.fate = IntentFate::refused; outcome.stale_identity = true;
            gate = {true, false, true, false};
        } else switch (outcome.kind) {
        case IntentKind::fleet: {
            auto admitted = session.readmit_native_fleet(record);
            if (!admitted) return failed(admitted.error());
            outcome.fate = admitted.value().admitted ? IntentFate::admitted : IntentFate::refused;
            outcome.reason = admitted.value().reason; outcome.object = admitted.value().object;
            gate = {true, admitted.value().admitted, true, false};
            break;
        }
        case IntentKind::control: {
            auto admitted = session.readmit_native_control(record);
            if (!admitted) return failed(admitted.error());
            const auto& event = admitted.value().event;
            outcome.fate = event.admitted ? IntentFate::admitted : IntentFate::refused;
            outcome.reason = event.reason; outcome.object = event.object;
            gate = {true, event.admitted, true, false};
            break;
        }
        case IntentKind::invasion: {
            auto admitted = session.readmit_native_invasion(record, options.invasion_floor != 0);
            if (!admitted) return failed(admitted.error());
            outcome.fate = admitted.value().admitted ? IntentFate::admitted : IntentFate::refused;
            outcome.reason = admitted.value().reason; outcome.request = admitted.value().request;
            gate = {true, admitted.value().admitted, true, false};
            break;
        }
        case IntentKind::invasion_release: {
            // A speculative reader that was refused (or already released) is a retained refusal.
            auto released = session.readmit_native_invasion_release(record);
            if (!released) return failed(released.error());
            outcome.fate = released.value() ? IntentFate::admitted : IntentFate::refused;
            outcome.request = std::get<Shadow::InvasionRequest>(record.request).number;
            gate = {true, released.value(), true, false};
            break;
        }
        case IntentKind::orbit_recheck: {
            auto admitted = session.readmit_native_orbit_recheck(record);
            if (!admitted) return failed(admitted.error());
            outcome.fate = admitted.value() ? IntentFate::admitted : IntentFate::refused;
            gate = {true, admitted.value(), true, false};
            break;
        }
        case IntentKind::cash: {
            const auto& cash = std::get<Shadow::CashRequest>(record.request);
            auto actual = session.readmit_native_cash(record);
            if (!actual) return failed(actual.error());
            outcome.actual = actual.value();
            outcome.fate = actual.value() == cash.actual ? IntentFate::admitted : IntentFate::adjusted;
            if (options.journal_cash) {
                if (auto recorded = session.append_native_effect(sim::galactic::NativeEffectKind::cash,
                    economic_replay::cash(cash.player, cash.delta, cash.multiplier, actual.value())); !recorded)
                    return failed(recorded.error());
            }
            gate = {true, true, false, options.journal_cash};
            break;
        }
        case IntentKind::force: {
            if (!intent.hooks.force_builder)
                return invalid<IntentCommitReport>("a force intent needs the player's placement builder to be readmitted");
            auto made = session.readmit_native_force(record, intent.hooks.force_builder, executor);
            if (!made) return failed(made.error());
            outcome.fate = made.value().admitted ? IntentFate::admitted : IntentFate::refused;
            outcome.actors = std::move(made.value().actors);
            gate = {true, made.value().admitted, false, false};
            break;
        }
        case IntentKind::production: {
            auto made = session.readmit_native_production(record, executor, intent.hooks.production_preflight);
            if (!made) return failed(made.error());
            const bool allowed = made.value().admission == sim::galactic::ProductionAdmission::allowed;
            outcome.fate = allowed ? IntentFate::admitted : IntentFate::refused;
            outcome.admission = made.value().admission; outcome.object = made.value().build;
            outcome.actual = made.value().price;
            gate = {true, allowed, false, false};
            break;
        }
        case IntentKind::finish: {
            // A refused speculative build never had a queue entry: no fake finish is journaled.
            auto finished = session.readmit_native_finish(record);
            if (!finished) return failed(finished.error());
            outcome.fate = finished.value() ? IntentFate::admitted : IntentFate::refused;
            outcome.object = std::get<Shadow::FinishRequest>(record.request).build;
            gate = {true, finished.value(), false, false};
            break;
        }
        case IntentKind::journal: {
            const auto& row = std::get<Shadow::JournalRequest>(record.request);
            // The journal records actual admissions only, in the player's own order.
            const bool covered = gate.present && (gate.world_journaled
                || (gate.cash && row.kind == sim::galactic::NativeEffectKind::cash));
            const bool refused = gate.present && !gate.admitted;
            if (covered || refused) {
                outcome.fate = IntentFate::dropped;
            } else {
                if (auto recorded = session.append_native_effect(row.kind, row.payload, row.phase); !recorded)
                    return failed(recorded.error());
                outcome.fate = IntentFate::journaled;
            }
            break;
        }
        }
        if (auto settled = done(std::move(outcome)); !settled) return failed(settled.error());
    }
    return Report::success(std::move(report));
}

} // namespace eawr::script::foc::galactic_ai
