#include "galactic_support.hpp"
#include "galactic/live_battle_dialog_view.hpp"
#include "galactic/live_battle_bounty.hpp"
#include "galactic/galactic_mode_seats.hpp"
#include "eawr/campaign/conflict_policy.hpp"

#include <algorithm>
#include <utility>
#include <vector>
#include <stdexcept>

namespace {
namespace gc = eawr::sim::galactic;
namespace cp = eawr::campaign;
namespace view = eawr::viewer::galactic;
using galactic_test::expect;
template<class T> T checked(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
void campaign_seats() {
    view::GalaxySnapshotStub authored;
    authored.players = {{1, "Rebel"}, {2, "Empire"}, {3, "Underworld"}};
    authored.seats = {{7, 1, true, true}, {2, 1, true, true}};
    auto single = authored;
    const auto observer = view::configure_campaign_seats(single, 0, false);
    expect(observer && observer->id == 2 && observer->player == 1,
        "ordinary campaign picks the lowest active observer seat independently of player identity");
    expect(single.seats.size() == 3
        && std::count_if(single.seats.begin(), single.seats.end(), [](const auto& seat) { return seat.human; }) == 1,
        "single-player has one human and one seat per faction, with no same-faction AI");
    for (const auto player : {2U, 3U}) expect(std::count_if(single.seats.begin(), single.seats.end(),
        [player](const auto& seat) { return seat.player == player && seat.active && !seat.human; }) == 1,
        "every other faction receives exactly one active AI seat");
    cp::ArbitrationInputs inputs; inputs.queue_tactical = true;
    inputs.local_involved = inputs.invader_human = true;
    inputs.multiplayer = std::count_if(single.seats.begin(), single.seats.end(),
        [](const auto& seat) { return seat.active && seat.human; }) > 1;
    const auto admission = cp::arbitrate_conflict(inputs);
    expect(admission && admission.value().route == cp::ConflictRoute::pending_tactical,
        "ordinary human-versus-AI campaign reaches Fight rather than multiplayer instant auto-resolve");
    auto selected = authored;
    const auto second = view::configure_campaign_seats(selected, 7, false);
    expect(second && second->id == 7 && selected.seats.front().id == 7,
        "explicit local seat selects the only human without changing its player");
    auto coop = authored;
    const auto preview = view::configure_campaign_seats(coop, 7, true);
    expect(preview && std::count_if(coop.seats.begin(), coop.seats.end(),
        [](const auto& seat) { return seat.active && seat.human; }) == 2,
        "explicit co-op preview preserves both human seats");
    inputs.multiplayer = true;
    expect(cp::arbitrate_conflict(inputs).value().route == cp::ConflictRoute::instant_auto,
        "preview keeps the authoritative multiplayer-versus-AI arbitration policy");
    expect(!view::configure_campaign_seats(authored, 99, false), "missing observer never silently selects another seat");
}
void bounty_mapping() {
    gc::GalacticSetup setup;
    setup.players = {{1, 1, 0}, {2, 2, 0}};
    setup.seats = {{{1}, 1, gc::SeatKind::human, 0, true, 1}};
    setup.planets = {{1, {}, 1}, {2, {}, 2}};
    setup.observer = gc::ObserverSetup{};
    setup.observer->local_player = 1;
    setup.observer->players = {{1, false, false}, {2, false, true}};
    for (const auto& planet : setup.planets) {
        gc::ObserverPlanet row;
        row.planet = planet.id;
        row.allegiance = row.target_owner = row.current_owner = row.current_controller = planet.owner;
        setup.observer->planets.push_back(row);
    }
    for (const auto& player : setup.players) for (const auto& planet : setup.planets)
        setup.observer->records.push_back({player.player_id, planet.id});
    auto session = checked(gc::GalacticSession::create(std::move(setup)));
    eawr::platform::LiveGameFrame frame; frame.latest = session.snapshot();
    const auto text = [](std::string_view key) {
        expect(key == "TEXT_SUMMARY_BOUNTY_COLLECTED", "bounty uses the authored summary text key");
        return std::string("Salvage collected: $ %d");
    };
    auto context = std::make_shared<cp::ConflictRecord>();
    context->id = 70; context->planet = 2; context->invader = 1; context->defender = 2;
    frame.result_conflict = context;
    view::LiveBattleDialogNames names;
    names.planet = [](auto) { return std::string("Planet"); };
    names.side = [](auto) { return view::BattleDialogSide{}; };
    names.type = [](auto) { return view::CampaignResultType{}; };
    names.bounty = view::live_campaign_bounty_namer(frame, text);
    const auto summary = [&](gc::PlayerId local) {
        const auto dialog = view::live_battle_dialog_view(frame, {1}, local, 0, names);
        expect(dialog.phase == view::BattleDialogPhase::summary, "bounty contract exercises the published summary");
        return dialog;
    };
    cp::BattleResult result; result.conflict = 70;
    frame.result = result;
    expect(!view::live_campaign_bounty(frame, 70, 2, text), "unknown tally stays absent even for corruption faction");
    expect(!summary(2).bounty, "summary preserves an absent published tally");
    frame.result->bounty = eawr::sim::math::Fixed::from_raw(42 * eawr::sim::math::Fixed::scale);
    const auto saved = *frame.result;
    auto bounty = view::live_campaign_bounty(frame, 70, 1, text);
    expect(bounty && bounty->positive && !bounty->benefits_from_corruption
        && bounty->caption == "Salvage collected: $ 42" && *frame.result == saved,
        "returned tally maps to localized whole credits without changing the result");
    expect(summary(1).bounty == bounty && *frame.result == saved,
        "production namer binds the returned tally into the native summary without changing the result");
    expect(!view::live_campaign_bounty(frame, 71, 1, text), "another conflict cannot reuse the tally");
    frame.result->bounty = eawr::sim::math::Fixed{};
    expect(!view::campaign_bounty_visible(view::live_campaign_bounty(frame, 70, 1, text)),
        "ordinary zero bounty remains hidden");
    expect(summary(1).bounty && !view::campaign_bounty_visible(summary(1).bounty),
        "summary retains the ordinary zero tally and hides its caption");
    bounty = view::live_campaign_bounty(frame, 70, 2, text);
    expect(bounty && !bounty->positive && bounty->benefits_from_corruption
        && bounty->caption == "Salvage collected: $ 0" && view::campaign_bounty_visible(bounty),
        "published observer faction permits a zero tally");
    expect(summary(2).bounty == bounty && view::campaign_bounty_visible(summary(2).bounty),
        "summary shows a known zero tally for the published corruption-benefiting observer");
    auto outcome = std::make_shared<eawr::platform::LiveAutoResolveOutcome>();
    outcome->result = *frame.result;
    outcome->planet = 2; outcome->invader = 1; outcome->defender = 2;
    frame.result.reset(); frame.auto_resolve_outcome = outcome;
    expect(view::live_campaign_bounty(frame, 70, 2, text) == bounty,
        "auto-resolve reads only its own published tally through the same mapping");
    expect(summary(2).auto_resolved && summary(2).bounty == bounty,
        "production namer binds a published auto-resolve tally into the native summary");
    outcome->result.bounty.reset();
    expect(!view::live_campaign_bounty(frame, 70, 2, text), "auto-resolve cannot invent an absent tally");
    expect(!summary(2).bounty, "auto-resolve summary preserves an absent published tally");
}
void publication() {
    auto session = checked(gc::GalacticSession::create(gc::synthetic_galaxy()));
    eawr::platform::LiveGameFrame frame; frame.latest = session.snapshot();
    auto pending = std::make_shared<cp::PendingConflictView>();
    pending->conflict.id = 70; pending->conflict.planet = 2;
    pending->conflict.defender = 2; pending->conflict.invader = 1;
    pending->conflict.forces = {{30, 1000, 1, 10}, {31, 1001, 2, 11}};
    pending->arbitration.route = cp::ConflictRoute::pending_tactical;
    frame.pending_conflict = pending;
    view::LiveBattleDialogNames names;
    names.planet = [](auto) { return std::string("Kamino"); };
    names.side = [](auto owner) { view::BattleDialogSide side; side.name = std::to_string(owner); return side; };
    names.force = [](auto type) { return std::to_string(type); };
    names.original_type = [](auto type) { return type == 1000 ? std::string("hero") : std::string("ship"); };
    names.tactical_type = [](auto) { return std::string("ship"); };
    names.space_picture = [](auto owner, bool won) { return std::to_string(owner) + (won ? "win" : "loss"); };
    names.type = [](std::string_view name) {
        return view::CampaignResultType{std::string(name), name != "unscored", name == "hero", 2, 5};
    };
    auto dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(dialog.phase == view::BattleDialogPhase::choice && !dialog.choice_ready && !dialog.auto_allowed,
        "announcement is visible without enabling input");
    pending->choice_ready = true;
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(dialog.choice_ready && dialog.auto_allowed && !dialog.tactical_available && !dialog.retreat_allowed
        && dialog.planet == "Kamino" && dialog.sides[0].combatants == std::vector<std::string>{"1001"}
        && dialog.sides[1].combatants == std::vector<std::string>{"1000"},
        "ready publication exposes only supported choice and defender/invader forces");
    pending->arbitration.forced_tactical = true;
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(!dialog.auto_allowed && !dialog.refusal.empty(), "forced tactical conflict keeps its explicit unavailable-provider refusal");
    pending->arbitration.forced_tactical = false;
    frame.tactical_refusal_code = "fixture.space.capability";
    frame.tactical_refusal = "reachable ship has an unsupported rocket curve";
    pending->arbitration.forced_tactical = true;
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    const auto diagnostic_report = view::battle_tactical_report_json(dialog,
        [](std::string_view value) { return '"' + std::string(value) + '"'; });
    expect(dialog.refusal == frame.tactical_refusal
        && diagnostic_report.find("\"code\":\"fixture.space.capability\"") != std::string::npos
        && diagnostic_report.find("\"reason\":\"reachable ship has an unsupported rocket curve\"") != std::string::npos,
        "a refused forced conflict reports its actual provider code and reason");
    const auto observer = view::live_battle_dialog_view(frame, {3}, 3, 0, names);
    expect(observer.tactical_refusal.empty() && observer.tactical_refusal_code.empty(),
        "an uninvolved observer receives no local capability diagnostic");
    auto active_refusal = frame;
    active_refusal.pending_conflict.reset();
    active_refusal.conflict = std::make_shared<const cp::ConflictRecord>(pending->conflict);
    expect(view::live_battle_dialog_view(active_refusal, {1}, 1, 0, names).tactical_refusal_code
        == frame.tactical_refusal_code,
        "child presentation preparation refusal remains reportable after pending choice closes");
    pending->arbitration.forced_tactical = false;
    frame.battle_choice_responses = {{70, 3, {1}, "unavailable"}, {70, 4, {2}, "other seat"}};
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(dialog.response_revision == 3 && dialog.refusal == "unavailable" && dialog.choice_ready && dialog.auto_allowed,
        "the local authoritative refusal keeps a ready choice and carries its response revision");
    eawr::platform::LiveAutoResolveProgress progress; progress.conflict = 70;
    progress.elapsed_seconds = 2;
    progress.display_seconds = 10;
    progress.combatants = {{{2, "original", "ship"}, {1, "hero", "hero"}}};
    progress.initiated = progress.withdraw_admitted = true;
    frame.auto_resolve_progress = std::make_shared<const eawr::platform::LiveAutoResolveProgress>(progress);
    auto blank = std::make_shared<eawr::platform::LiveAutoResolveProgress>();
    blank->conflict = progress.conflict; blank->elapsed_seconds = progress.elapsed_seconds; blank->display_seconds = progress.display_seconds;
    frame.auto_resolve_observer_progress = blank;
    expect(eawr::platform::auto_resolve_progress_for_observer(frame, {1}) == frame.auto_resolve_progress
        && eawr::platform::auto_resolve_progress_for_observer(frame, {3}) == blank
        && eawr::platform::auto_resolve_progress_for_observer(frame, {99}) == blank
        && !blank->roster && blank->combatants[0].current_type.empty() && !blank->withdraw_admitted,
        "platform observer boundary shares participant facts and returns clock-only publication for other or missing seats");
    expect(eawr::platform::auto_resolve_withdraw_order(frame, {1}, 70).has_value()
        && !eawr::platform::auto_resolve_withdraw_order(frame, {3}, 70)
        && !eawr::platform::auto_resolve_withdraw_order(frame, {1}, 71),
        "withdraw publication admits exact participant and conflict; uninvolved and stale callbacks fail");
    progress.withdraw_admitted = false;
    frame.auto_resolve_progress = std::make_shared<const eawr::platform::LiveAutoResolveProgress>(progress);
    expect(!eawr::platform::auto_resolve_withdraw_order(frame, {1}, 70), "recorded first withdrawal refuses another command");
    progress.withdraw_admitted = true;
    cp::PendingBattleAlertInputs alert;
    alert.local = 1; alert.active_mode = alert.queue_tactical = alert.multiplayer = alert.authorized_issuer = true;
    alert.timeout_ms = 15000; alert.elapsed_wall_ms = 15000;
    pending->alert = cp::pending_battle_alert_view(pending->conflict, alert, true);
    expect(!eawr::platform::pending_battle_timeout_order(frame, {1}), "deadline equality cannot inject a command");
    alert.elapsed_wall_ms = 15001;
    pending->alert = cp::pending_battle_alert_view(pending->conflict, alert, true);
    const auto timeout = eawr::platform::pending_battle_timeout_order(frame, {1});
    expect(timeout && std::get<gc::BattleChoice>(timeout->payload).tactical
        && !eawr::platform::pending_battle_timeout_order(frame, {2}),
        "timeout injects Begin preference for only the owner-admitted local seat");
    frame.auto_resolve_progress.reset();
    alert.tutorial_forced = true;
    pending->alert = cp::pending_battle_alert_view(pending->conflict, alert, true);
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names, true);
    expect(dialog.single_button && !dialog.retreat_allowed,
        "owner-published Begin-only selection uses the pending shell without retreat");
    alert.tutorial_forced = false;
    pending->alert = cp::pending_battle_alert_view(pending->conflict, alert, true, true);
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names, true);
    expect(!dialog.single_button && dialog.waiting_for_opponent,
        "waiting-for-opponent stays in the ordinary two-button alert");
    view::LivePendingBattlePublication pending_publication;
    expect(!pending_publication.reuse(frame, {1}, 1, 0), "first pending publication needs projection");
    for (unsigned count = 0; count < 1000; ++count)
        expect(pending_publication.reuse(frame, {1}, 1, 0), "unchanged pending publication reuses its projection");
    ++frame.session_tick;
    expect(!pending_publication.reuse(frame, {1}, 1, 0), "new logical publication refreshes pending projection");
    ++frame.battle_choice_responses.front().revision;
    expect(!pending_publication.reuse(frame, {1}, 1, 0), "local refusal revision refreshes pending projection");
    frame.auto_resolve_progress = std::make_shared<const eawr::platform::LiveAutoResolveProgress>(progress);
    pending->alert.reset();
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(dialog.phase == view::BattleDialogPhase::progress && !dialog.auto_allowed && !dialog.fight_allowed
        && dialog.elapsed_seconds == 2 && dialog.display_seconds == 10
        && dialog.sides[0].combatants == std::vector<std::string>{"ship"},
        "resolver publication supplies progress without driving its time");
    auto full_progress = std::make_shared<eawr::platform::LiveAutoResolveProgress>(*frame.auto_resolve_progress);
    full_progress->roster.emplace(std::array<eawr::platform::LiveAutoResolveProgress::SideRoster, 2>{});
    frame.auto_resolve_progress = full_progress;
    auto& roster = *full_progress->roster;
    roster[0].owner = 1; roster[1].owner = 2; // reverse source order
    roster[1].current = eawr::platform::LiveAutoResolveProgress::Combatant{2, "history", "current"};
    roster[1].escort = eawr::platform::LiveAutoResolveProgress::Combatant{2, "history", "escort"};
    roster[1].queue = {{2, "history", "one"}, {2, "history", ""}, {2, "history", "two"},
        {2, "history", "two"}, {2, "history", "four"}};
    roster[0].queue = {{1, "history", "attacker"}};
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(dialog.roster_complete && dialog.retreat_allowed
        && dialog.sides[0].combatants == std::vector<std::string>{"current", "escort", "one", "two", "two", "four"}
        && dialog.sides[1].combatants == std::vector<std::string>{"attacker"},
        "WAR-114/115/121 maps owners then current/escort/full queue, skips absent types and preserves duplicates");
    unsigned observer_names{};
    auto observed_names = names;
    observed_names.planet = [&](auto) { ++observer_names; return std::string{}; };
    observed_names.side = [&](auto) { ++observer_names; return view::BattleDialogSide{}; };
    observed_names.type = [&](auto) { ++observer_names; return view::CampaignResultType{}; };
    const auto solo_observer = view::live_battle_dialog_view(frame, {3}, 3, 0, observed_names);
    expect(solo_observer.phase == view::BattleDialogPhase::hidden && observer_names == 0,
        "WAR-02/WGM-173 a solo conflict the local seat is not part of opens no progress dialog");
    full_progress->withdraw_admitted = false;
    expect(!view::live_battle_dialog_view(frame, {1}, 1, 0, names).retreat_allowed,
        "first recorded withdrawal disables subsequent local admission");
    const auto solo_snapshot = frame.latest;
    auto multiplayer_setup = gc::synthetic_galaxy(); multiplayer_setup.victory.emplace();
    multiplayer_setup.victory->context.multiplayer = true;
    for (const auto& player : multiplayer_setup.players)
        multiplayer_setup.victory->context.players.push_back({player.player_id, {}});
    for (const auto& planet : multiplayer_setup.planets)
        multiplayer_setup.victory->context.planets.push_back({planet.id, 1000, planet.owner, planet.owner, planet.owner});
    auto multiplayer_session = checked(gc::GalacticSession::create(multiplayer_setup));
    frame.latest = multiplayer_session.snapshot();
    expect(view::live_battle_dialog_view(frame, {1}, 1, 0, names).phase == view::BattleDialogPhase::hidden,
        "WGM-178 pinned multiplayer space initializer bypasses both progress resources without a seat-count guess");
    pending->conflict.layer = cp::BattleLayer::land;
    expect(view::live_battle_dialog_view(frame, {1}, 1, 0, names).phase == view::BattleDialogPhase::progress,
        "space multiplayer bypass does not invent a universal land-dialog bypass");
    const auto progress_observer = view::live_battle_dialog_view(frame, {3}, 3, 0, observed_names);
    expect(progress_observer.phase == view::BattleDialogPhase::progress && !progress_observer.involved
        && progress_observer.planet.empty() && !progress_observer.retreat_allowed
        && progress_observer.sides[0].combatants.empty() && progress_observer.sides[1].combatants.empty()
        && observer_names == 0 && progress_observer.elapsed_seconds == 2 && progress_observer.display_seconds == 10,
        "WAR-66/67 multiplayer observer progress retains identical clocks without name lookup, body/list or withdraw permission");
    pending->conflict.layer = cp::BattleLayer::space; frame.latest = solo_snapshot;
    frame.pending_conflict.reset(); frame.auto_resolve_progress.reset();
    auto outcome = std::make_shared<eawr::platform::LiveAutoResolveOutcome>();
    outcome->planet = 2; outcome->invader = 1; outcome->defender = 2;
    outcome->result.conflict = 70; outcome->result.winner = 1;
    outcome->record = {{"ship", 2}, {"ship", 2}, {"hero", 1}, {"unscored", 2}, {"ship", 3}};
    frame.auto_resolve_outcome = outcome;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> bounty_queries;
    names.bounty = [&](auto conflict, auto local) -> std::optional<view::CampaignBounty> {
        bounty_queries.emplace_back(static_cast<std::uint64_t>(conflict), static_cast<std::uint64_t>(local));
        return view::CampaignBounty{false,true,"Collected: 0"};
    };
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names);
    expect(bounty_queries.size() == 1 && bounty_queries.front() == std::pair<std::uint64_t, std::uint64_t>{70, 1},
        "bounty query is scoped to result and local observer");
    expect(dialog.auto_resolved && dialog.bounty && view::campaign_bounty_visible(dialog.bounty),
        "AR shared builder can show a zero tally for a corruption-benefiting observer");
    expect(dialog.phase == view::BattleDialogPhase::summary && dialog.won && dialog.results.totals[0] == 1
        && dialog.results.totals[1] == 2 && dialog.results.heroes[0].size() == 1
        && dialog.results.losses[1].front().count == 2 && dialog.results.elapsed_milliseconds == 0
        && dialog.results.score_cost == std::array<double, 2>{2, 4}
        && dialog.results.combat_power == std::array<double, 2>{5, 10},
        "summary groups confirmed original types, filters unscored/unrelated losses and separates heroes");
    expect(view::live_battle_dialog_view(frame, {1}, 1, 70, names).phase == view::BattleDialogPhase::hidden,
        "Continue acknowledgement hides the summary without a simulation command");
    expect(view::live_battle_dialog_view(frame, {3}, 3, 0, names).phase == view::BattleDialogPhase::hidden,
        "uninvolved observer never receives a local loss summary");
    const auto returned = pending->conflict;
    frame.auto_resolve_outcome.reset();
    cp::BattleResult result;
    result.conflict = returned.id; result.winner = 1; result.tactical_frames = 1845;
    result.forces = {{30, cp::ForceDisposition::survived, 0, true},
        {31, cp::ForceDisposition::destroyed}, {99, cp::ForceDisposition::destroyed}};
    result.casualties = {{30, 2000, 1000, 1}, {31, 2001, 1001, 2}, {99, 2001, 0, 2}};
    result.invasion_offers = {{2, 80, 1, {300}}, {2, 81, 2, {301}}};
    frame.result = result;
    frame.result_conflict = std::make_shared<const cp::ConflictRecord>(returned);
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names, true, &returned);
    expect(!dialog.invasion_fleet, "prepared candidates never stand in for admitted visible permission");
    auto offer = std::make_shared<cp::ObservedInvasionOffer>();
    offer->conflict = returned.id; offer->observer = 1; offer->candidates = {result.invasion_offers.front()};
    offer->visible_admitted = offer->accept_permitted = true;
    frame.invasion_offer = offer;
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names, true, &returned);
    expect(dialog.phase == view::BattleDialogPhase::summary && dialog.won && dialog.planet == "Kamino"
        && dialog.results.elapsed_milliseconds == 61500 && dialog.results.totals == std::array<std::uint64_t, 2>{1, 2}
        && dialog.results.heroes[0].front().type == "hero" && dialog.invasion_fleet == 80 && dialog.picture == "1win",
        "released child summary retains confirmed persistent and battle-only casualties, time and admitted invasion");
    expect(!dialog.auto_resolved && dialog.bounty && dialog.bounty->caption == "Collected: 0",
        "tactical summary consumes the same copied local bounty without credit mutation");
    offer->observer = 2;
    expect(!view::live_battle_dialog_view(frame, {1}, 1, 0, names).invasion_fleet,
        "another observer's invasion permission is never projected");
    offer->observer = 1; offer->accept_permitted = false;
    expect(!view::live_battle_dialog_view(frame, {1}, 1, 0, names).invasion_fleet,
        "revoked acceptance suppresses a previously visible action");
    offer->accept_permitted = true;
    view::LiveBattleSummaryPublication publication;
    unsigned type_reads = 0;
    const auto original_type = names.type;
    names.type = [&](auto type) { ++type_reads; return original_type(type); };
    const auto update = [&](unsigned metadata = 1, unsigned local = 1) {
        if (!publication.reuse(frame, {1}, local, 0, metadata, &returned))
            dialog = view::live_battle_dialog_view(frame, {1}, local, 0, names, true, &returned);
    };
    update();
    const auto initial_reads = type_reads;
    for (unsigned count = 0; count < 1000; ++count) update();
    expect(publication.builds() == 1 && type_reads == initial_reads,
        "unchanged summary frames do no type/name/grouping work");
    update(2);
    expect(publication.builds() == 2 && type_reads > initial_reads, "changed metadata rebuilds summary");
    const auto previous_reads = type_reads;
    frame.result->casualties.push_back({100, 2001, 1001, 2}); update(2);
    expect(publication.builds() == 3 && type_reads > previous_reads && dialog.results.totals[1] == 3,
        "changed confirmed scoring entries rebuild summary even without a new snapshot pointer");
    frame.result->casualties.pop_back(); update(2);
    update(2, 2);
    expect(publication.builds() == 5, "observer identity participates in summary publication");
    update(2, 1);
    auto revoked_offer = std::make_shared<cp::ObservedInvasionOffer>(*offer);
    revoked_offer->accept_permitted = false; frame.invasion_offer = revoked_offer;
    update(2, 1);
    expect(publication.builds() == 7 && !dialog.invasion_fleet,
        "changed immutable invasion permission invalidates the cached summary");
    frame.invasion_offer = offer; update(2, 1);
    expect(view::live_battle_dialog_view(frame, {1}, 1, 70, names, true, &returned).phase == view::BattleDialogPhase::hidden,
        "tactical acknowledgement is presentation-only and once per conflict");
    expect(view::live_battle_dialog_view(frame, {1}, 1, 0, names).results.totals == dialog.results.totals,
        "restored publication describes first-frame summary without presentation-retained child context");
    frame.result->winner = 2;
    dialog = view::live_battle_dialog_view(frame, {1}, 1, 0, names, true, &returned);
    expect(!dialog.won && !dialog.invasion_fleet && dialog.picture == "1loss",
        "loss picture stays local and an opponent's victory never grants an invasion action");
    frame.result->conflict = 71;
    expect(view::live_battle_dialog_view(frame, {1}, 1, 0, names, true, &returned).phase == view::BattleDialogPhase::hidden,
        "stale retained context cannot describe another battle result");
    expect(std::all_of(bounty_queries.begin(), bounty_queries.end(), [](const auto& query) {
        return query.first == 70 && (query.second == 1 || query.second == 2); }),
        "every later bounty query stays on the returned conflict and an involved observer");
}
// GC #4030: an AI-vs-AI conflict (pending announcement, resolver progress, outcome) never opens or closes any
// battle UI for the local seat that is not part of it, and the party seats see the same publications unchanged.
void uninvolved_conflict_lifecycle() {
    auto session = checked(gc::GalacticSession::create(gc::synthetic_galaxy()));
    eawr::platform::LiveGameFrame frame; frame.latest = session.snapshot();
    auto pending = std::make_shared<cp::PendingConflictView>();
    pending->conflict.id = 90; pending->conflict.planet = 2;
    pending->conflict.defender = 2; pending->conflict.invader = 3;
    pending->conflict.forces = {{40, 1000, 3, 10}, {41, 1001, 2, 11}};
    pending->arbitration.route = cp::ConflictRoute::instant_auto;
    view::LiveBattleDialogNames names;
    names.planet = [](auto) { return std::string("Bestine"); };
    names.side = [](auto owner) { view::BattleDialogSide side; side.name = std::to_string(owner); return side; };
    names.force = [](auto type) { return std::to_string(type); };
    names.type = [](std::string_view name) { return view::CampaignResultType{std::string(name), true, false, 2, 5}; };
    const auto outcome = [] {
        auto value = std::make_shared<eawr::platform::LiveAutoResolveOutcome>();
        value->planet = 2; value->invader = 3; value->defender = 2;
        value->result.conflict = 90; value->result.winner = 3;
        value->record = {{"ship", 2}};
        return value;
    }();
    eawr::platform::LiveAutoResolveProgress progress; progress.conflict = 90;
    progress.elapsed_seconds = 0; progress.display_seconds = 0.01; progress.initiated = true;
    progress.combatants = {{{3, "ship", "ship"}, {2, "ship", "ship"}}};
    unsigned opened = 0, changes = 0;
    auto previous = view::BattleDialogPhase::hidden;
    const auto sample = [&](gc::PlayerId local) {
        const auto dialog = view::live_battle_dialog_view(frame, {local}, local, 0, names, true);
        if (dialog.phase != view::BattleDialogPhase::hidden) ++opened;
        if (dialog.phase != previous) ++changes;
        previous = dialog.phase;
        return dialog.phase;
    };
    // Local player 1 is neither side: the conflict is announced, resolved over several publications, and returned.
    for (unsigned step = 0; step < 4; ++step) {
        frame.pending_conflict = pending; frame.auto_resolve_progress.reset();
        sample(1);
        progress.elapsed_seconds = 0.01 * step;
        frame.auto_resolve_progress = std::make_shared<const eawr::platform::LiveAutoResolveProgress>(progress);
        sample(1);
    }
    frame.pending_conflict.reset(); frame.auto_resolve_progress.reset(); frame.auto_resolve_outcome = outcome;
    sample(1);
    expect(opened == 0 && changes == 0, "an AI-vs-AI conflict opens no battle UI for an uninvolved local player");
    // The same publications still reach the defender: announcement, progress, then summary.
    frame.auto_resolve_outcome.reset(); frame.pending_conflict = pending;
    pending->arbitration.route = cp::ConflictRoute::dialog_auto;
    previous = view::BattleDialogPhase::hidden; opened = changes = 0;
    expect(sample(2) == view::BattleDialogPhase::choice, "a party seat still gets its announcement");
    frame.auto_resolve_progress = std::make_shared<const eawr::platform::LiveAutoResolveProgress>(progress);
    expect(sample(2) == view::BattleDialogPhase::progress, "a party seat still gets the resolver progress");
    frame.pending_conflict.reset(); frame.auto_resolve_progress.reset(); frame.auto_resolve_outcome = outcome;
    expect(sample(2) == view::BattleDialogPhase::summary && changes == 3, "a party seat still gets one summary");
}
}
int main() {
    try { campaign_seats(); bounty_mapping(); publication(); uninvolved_conflict_lifecycle(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
