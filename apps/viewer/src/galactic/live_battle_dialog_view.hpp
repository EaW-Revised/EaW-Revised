#pragma once

#include "battle_dialog_model.hpp"
#include "eawr/platform/live_game_session.hpp"

namespace eawr::viewer::galactic {
// Only acknowledge an actually published result. A result can occupy both
// slots after auto-resolve; closing its one summary dismisses both copies.
inline bool acknowledge_battle_summary(BattleSummaryAcknowledgements& acknowledged,
    const platform::LiveGameFrame& frame, std::uint64_t conflict) {
    if (!conflict) return false;
    bool found = false;
    if (frame.result && frame.result->conflict == conflict) {
        acknowledged.returned = conflict; found = true;
    }
    if (frame.auto_resolve_outcome && frame.auto_resolve_outcome->result.conflict == conflict) {
        acknowledged.automatic = conflict; found = true;
    }
    return found;
}
struct LiveBattleDialogNames {
    std::function<std::string(std::uint64_t)> planet;
    std::function<BattleDialogSide(sim::galactic::PlayerId)> side;
    std::function<std::string(sim::galactic::TypeId)> force;
    std::function<std::string(sim::galactic::TypeId)> original_type;
    // WGUI-176: Icon_Name and Tactical_Health of a force's type, for the pending shell's centre graphs.
    std::function<std::string(sim::galactic::TypeId)> force_icon;
    std::function<std::optional<float>(sim::galactic::TypeId)> force_health;
    std::function<std::string(sim::tactical::TypeId)> tactical_type;
    std::function<CampaignResultType(std::string_view)> type;
    std::function<std::string(sim::galactic::PlayerId, bool)> space_picture;
    std::function<std::optional<CampaignBounty>(std::uint64_t, sim::galactic::PlayerId)> bounty;
};

// A result is immutable until the live owner publishes another result or the
// metadata/local observer changes. Comparing the copied publication does not
// allocate; only a changed publication retains a new copy.
class LiveBattleSummaryPublication final {
public:
    bool reuse(const platform::LiveGameFrame& frame, sim::galactic::SeatId seat,
        sim::galactic::PlayerId local, BattleSummaryAcknowledgements acknowledged, std::uint64_t metadata,
        const campaign::ConflictRecord* fallback = nullptr) {
        const auto* context = frame.result_conflict ? frame.result_conflict.get() : fallback;
        const bool tactical = frame.result && context && frame.result->conflict == context->id
            && !acknowledged.contains(frame.result->conflict);
        const bool automatic = frame.auto_resolve_outcome && !acknowledged.contains(frame.auto_resolve_outcome->result.conflict);
        if (frame.pending_conflict || (!tactical && !automatic)) { valid_ = false; return false; }
        if (valid_ && result_ == frame.result && outcome_ == frame.auto_resolve_outcome && invasion_ == frame.invasion_offer
            && ((!context && !context_) || (context && context_ && *context_ == *context))
            && seat_ == seat && local_ == local && acknowledged_ == acknowledged
            && metadata_ == metadata && active_ == frame.conflict && available_ == frame.tactical_available
            && refusal_ == frame.tactical_refusal && refusal_code_ == frame.tactical_refusal_code) return true;
        valid_ = true; result_ = frame.result; outcome_ = frame.auto_resolve_outcome; invasion_ = frame.invasion_offer;
        if (context) context_ = *context; else context_.reset();
        seat_ = seat; local_ = local; acknowledged_ = acknowledged; metadata_ = metadata;
        active_ = frame.conflict; available_ = frame.tactical_available;
        refusal_ = frame.tactical_refusal; refusal_code_ = frame.tactical_refusal_code;
        ++builds_; return false;
    }
    [[nodiscard]] std::uint64_t builds() const noexcept { return builds_; }
private:
    std::optional<campaign::BattleResult> result_;
    std::shared_ptr<const platform::LiveAutoResolveOutcome> outcome_;
    std::shared_ptr<const campaign::ObservedInvasionOffer> invasion_;
    std::optional<campaign::ConflictRecord> context_;
    std::shared_ptr<const campaign::ConflictRecord> active_;
    std::string refusal_, refusal_code_;
    bool available_{};
    sim::galactic::SeatId seat_{};
    sim::galactic::PlayerId local_{};
    BattleSummaryAcknowledgements acknowledged_;
    std::uint64_t metadata_{}, builds_{};
    bool valid_{};
};
// Pending/progress content is retained between immutable owner publications.
class LivePendingBattlePublication final {
public:
    bool reuse(const platform::LiveGameFrame& frame, sim::galactic::SeatId seat,
        sim::galactic::PlayerId local, std::uint64_t metadata) {
        if (!frame.pending_conflict) { pending_.reset(); return false; }
        std::uint64_t response{};
        for (const auto& row : frame.battle_choice_responses)
            if (row.seat == seat && row.conflict == frame.pending_conflict->conflict.id) response = row.revision;
        if (pending_ == frame.pending_conflict && tick_ == frame.session_tick && response_ == response
            && seat_ == seat && local_ == local && metadata_ == metadata && available_ == frame.tactical_available
            && refusal_ == frame.tactical_refusal && code_ == frame.tactical_refusal_code) return true;
        pending_ = frame.pending_conflict; tick_ = frame.session_tick; response_ = response;
        seat_ = seat; local_ = local; metadata_ = metadata; available_ = frame.tactical_available;
        refusal_ = frame.tactical_refusal; code_ = frame.tactical_refusal_code;
        return false;
    }
private:
    std::shared_ptr<const campaign::PendingConflictView> pending_;
    sim::galactic::SeatId seat_{};
    sim::galactic::PlayerId local_{};
    std::uint64_t tick_{}, response_{}, metadata_{};
    std::string refusal_, code_;
    bool available_{};
};

// Pure projection of the same publication used by command validation. Summary
// acknowledgement is presentation state, never a parent ResumeGalaxy command.
[[nodiscard]] inline BattleDialogView live_battle_dialog_view(const platform::LiveGameFrame& frame,
    sim::galactic::SeatId seat, sim::galactic::PlayerId local, BattleSummaryAcknowledgements acknowledged,
    const LiveBattleDialogNames& names, bool tactical_available = false,
    const campaign::ConflictRecord* returned_conflict = nullptr) {
    if (frame.result_conflict) returned_conflict = frame.result_conflict.get();
    BattleDialogView view;
    const auto* admission = frame.pending_conflict ? &frame.pending_conflict->conflict : frame.conflict.get();
    if (admission && (local == admission->invader || local == admission->defender)) {
        view.tactical_refusal = frame.tactical_refusal;
        view.tactical_refusal_code = frame.tactical_refusal_code;
    }
    const auto describe = [&](std::uint64_t planet, sim::galactic::PlayerId defender, sim::galactic::PlayerId invader) {
        view.involved = local == defender || local == invader;
        view.local_invader = local == invader;
        const bool progress_observer = !view.involved && frame.auto_resolve_progress
            && admission && frame.auto_resolve_progress->conflict == admission->id;
        if (!progress_observer) view.planet = names.planet(planet);
        if (view.involved) view.sides = {names.side(defender), names.side(invader)};
    };
    if (frame.pending_conflict) {
        const auto& pending = *frame.pending_conflict;
        const auto& entry = pending.conflict;
        // WGM-178: only the space initializer has the multiplayer bypass.
        // Read the pinned campaign mode; a live human-seat count is not that mode.
        const bool multiplayer = frame.latest && frame.latest->victory() && frame.latest->victory()->context.multiplayer;
        if (entry.layer == campaign::BattleLayer::space && frame.auto_resolve_progress
            && frame.auto_resolve_progress->conflict == entry.id && multiplayer) return {};
        // WAR-02/WGM-173: a solo conflict the local player is not part of resolves instantly and bypasses the
        // pending alert and the progress dialog; nothing opens for it. (Whether a multiplayer
        // uninvolved seat gets the alert stays unverified, WGM-BP-U02, and keeps its earlier projection.)
        if (!multiplayer && local != entry.defender && local != entry.invader) return {};
        view.conflict = entry.id;
        view.pending_land = entry.layer == campaign::BattleLayer::land;
        view.pending_multiplayer = multiplayer;
        describe(entry.planet, entry.defender, entry.invader);
        view.phase = BattleDialogPhase::choice;
        view.choice_ready = pending.choice_ready;
        if (view.involved) view.refusal = frame.tactical_refusal;
        for (const auto& response : frame.battle_choice_responses)
            if (response.conflict == entry.id && response.seat == seat) {
                view.response_revision = response.revision;
                view.refusal = response.refusal;
            }
        view.tactical_available = tactical_available;
        view.fight_allowed = platform::battle_choice_order(frame, seat, entry.id, true).has_value();
        view.auto_allowed = platform::battle_choice_order(frame, seat, entry.id, false).has_value();
        view.single_button = pending.arbitration.forced_tactical;
        if (pending.alert && pending.alert->local == local) {
            view.pending_visible = pending.alert->visible;
            view.single_button = pending.alert->single_button;
            view.waiting_for_opponent = pending.alert->waiting_for_opponent;
            if (view.pending_multiplayer) {
                const auto timeout = pending.alert->timeout_ms / 1000;
                const auto elapsed = pending.alert->elapsed_wall_ms / 1000;
                view.pending_countdown = timeout > elapsed ? timeout - elapsed : 0;
            }
        }
        // H073/H074 remain WGM-BP-U01 seams; no extra modal or pending retreat.
        if (pending.arbitration.forced_tactical && !tactical_available && view.refusal.empty())
            view.refusal = "This conflict requires a tactical battle.";
        if (view.involved && !frame.auto_resolve_progress) {
            // WGUI-176: per side, the unit count and the best ship (greatest Tactical_Health, first on a tie).
            std::array<std::optional<float>, 2> best_health;
            for (const auto& force : entry.forces) {
                const auto index = force.owner == entry.defender ? 0U : 1U;
                view.sides[index].combatants.push_back(names.force(force.type));
                ++view.sides[index].units;
                const auto icon = names.force_icon ? names.force_icon(force.type) : std::string{};
                if (icon.empty()) continue;
                const auto health = names.force_health ? names.force_health(force.type) : std::nullopt;
                const bool better = view.sides[index].best_icon.empty()
                    || (health && (!best_health[index] || *health > *best_health[index]));
                if (better) { view.sides[index].best_icon = icon; if (health) best_health[index] = health; }
            }
        }
        if (frame.auto_resolve_progress && frame.auto_resolve_progress->conflict == entry.id) {
            const auto& progress = *frame.auto_resolve_progress;
            view.phase = BattleDialogPhase::progress;
            view.choice_ready = view.fight_allowed = view.auto_allowed = false;
            view.elapsed_seconds = progress.elapsed_seconds;
            view.display_seconds = progress.display_seconds;
            view.retreat_allowed = platform::auto_resolve_withdraw_order(frame, seat, entry.id).has_value();
            for (auto& side : view.sides) side.combatants.clear();
            view.roster_complete = progress.roster.has_value();
            const auto append = [&](const auto& combatant) {
                if (!view.involved || combatant.current_type.empty()
                    || (combatant.owner != entry.defender && combatant.owner != entry.invader)) return;
                const auto side = combatant.owner == entry.defender ? 0U : 1U;
                view.sides[side].combatants.push_back(names.type(combatant.current_type).name);
            };
            if (progress.roster) {
                // WAR-114/121: map by owner; current, escort, then every queue entry.
                for (const auto& roster : *progress.roster) {
                    if (roster.current) append(*roster.current);
                    if (roster.escort) append(*roster.escort);
                    for (const auto& combatant : roster.queue) append(combatant);
                }
            } else {
                // WGM-181 provider seam: legacy current-slot facts are not a full queue.
                for (const auto& combatant : progress.combatants) append(combatant);
            }
        }
    } else if (frame.result && returned_conflict && frame.result->conflict == returned_conflict->id
        && !acknowledged.contains(frame.result->conflict)) {
        const auto& entry = *returned_conflict;
        const auto& result = *frame.result;
        view.conflict = entry.id;
        describe(entry.planet, entry.defender, entry.invader);
        if (!view.involved) return {};
        view.phase = BattleDialogPhase::summary;
        view.won = result.winner == local;
        if (names.bounty) view.bounty = names.bounty(view.conflict,local);
        if (names.space_picture && entry.layer == campaign::BattleLayer::space)
            view.picture = names.space_picture(local, view.won);
        std::vector<CampaignRecordEntry> record;
        for (const auto& row : result.casualties) {
            const auto type = row.original_type && names.original_type ? names.original_type(row.original_type)
                : names.tactical_type ? names.tactical_type(row.tactical_type) : std::string{};
            if (!type.empty()) record.push_back({type, row.owner});
        }
        view.results = campaign_record_results(record, local,
            [&](auto owner) { return owner == (local == entry.invader ? entry.defender : entry.invader); }, names.type);
        // WGP-34: logical tactical time, independent of rendering and loading.
        view.results.elapsed_milliseconds = result.tactical_frames / 30 * 1000
            + result.tactical_frames % 30 * 1000 / 30;
        if (view.won && entry.layer == campaign::BattleLayer::space && frame.invasion_offer
            && frame.invasion_offer->conflict == entry.id && frame.invasion_offer->observer == local
            && frame.invasion_offer->visible_admitted && frame.invasion_offer->accept_permitted)
            for (const auto& offer : frame.invasion_offer->candidates) if (!offer.transports.empty()) {
                view.invasion_fleet = offer.fleet; view.invasion_planet = offer.planet;
                break;
            }
    } else if (frame.auto_resolve_outcome && !acknowledged.contains(frame.auto_resolve_outcome->result.conflict)) {
        const auto& outcome = *frame.auto_resolve_outcome;
        view.conflict = outcome.result.conflict;
        describe(outcome.planet, outcome.defender, outcome.invader);
        if (!view.involved) return {};
        view.phase = BattleDialogPhase::summary;
        view.auto_resolved = true;
        view.won = outcome.result.winner == local;
        if (names.bounty) view.bounty = names.bounty(view.conflict,local);
        if (names.space_picture && !outcome.land) view.picture = names.space_picture(local, view.won);
        std::vector<CampaignRecordEntry> record;
        record.reserve(outcome.record.size());
        for (const auto& row : outcome.record) record.push_back({row.original_type, row.owner});
        view.results = campaign_record_results(record, local,
            [&](auto owner) { return owner == (local == outcome.invader ? outcome.defender : outcome.invader); }, names.type);
    }
    return view;
}
} // namespace eawr::viewer::galactic
