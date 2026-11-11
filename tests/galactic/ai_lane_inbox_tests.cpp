#include "galactic_support.hpp"
#include "../../src/campaign/galactic_ai_lane_inbox.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

// Async AI A2.1: the sim->AI inbox keeps every delivery kind in arrival order, and its saved block reloads to the same queue.
namespace {
namespace cp = eawr::campaign;
namespace ga = eawr::script::foc::galactic_ai;
namespace gc = eawr::sim::galactic;
using galactic_test::expect;
using Change = gc::GalacticSession::NativeActorChange;

cp::GalacticAiLaneInbox::Entry outcome(gc::PlayerId player, std::uint64_t build, bool completed) {
    return ga::NativeActions::Delivery{gc::GalacticSession::NativeProductionOutcome{player, build, completed ? build + 1000 : 0, completed}};
}
cp::GalacticAiLaneInbox::Entry movement(std::uint64_t tick, gc::PlayerId player, eawr::sim::EntityId fleet) {
    gc::GalacticEvent event; event.tick = tick; event.kind = gc::GalacticEventKind::movement_finished;
    event.producer = gc::SeatId{player}; event.subject = fleet; event.sequence = tick * 10 + player; event.value = 3;
    return ga::NativeActions::Delivery{event};
}
cp::GalacticAiLaneInbox::Entry actors(gc::PlayerId player, eawr::sim::EntityId actor) {
    ga::NativeActions::ActorChangesDelivery batch;
    batch.changes = {{Change::Kind::created, actor, 0, {}, {}, {}, {}, {}}, {Change::Kind::changed, actor + 1, 0, true, false, std::nullopt, true, false},
        {Change::Kind::lost, actor + 2, 0, {}, {}, {}, {}, {}}};
    for (const auto id : {actor, actor + 1}) {
        ga::FreestoreFacts facts; facts.object.id = id; facts.object.type = 7; facts.object.owner = player; facts.object.planet = 2;
        facts.object.kind = ga::FreestoreKind::fleet; facts.object.safe = id % 2 == 0; facts.current_type = 7; facts.parent = 2;
        facts.parent_is_planet = true; facts.ai_usable = true; facts.qualified = true; facts.prefer_ground = id % 2 == 1;
        batch.facts.emplace_back(id, facts);
    }
    return ga::NativeActions::Delivery{std::move(batch)};
}
cp::GalacticAiLaneInbox::Entry battle(std::uint64_t tick, gc::PlayerId player, bool returned) {
    cp::GalacticAiBattleFacts facts;
    facts.kind = returned ? cp::GalacticAiBattleKind::returned : cp::GalacticAiBattleKind::entered;
    facts.synchronization_tick = tick; facts.auto_resolve = returned;
    facts.conflict.id = 500 + tick; facts.conflict.planet = 2; facts.conflict.layer = cp::BattleLayer::space;
    cp::ConflictForce force; force.id = 60 + player; force.owner = player; force.parent = 50 + player; facts.conflict.forces = {force};
    if (returned) {
        cp::BattleResult result; result.conflict = facts.conflict.id; result.winner = player; result.retreat_player = player;
        result.forces = {{force.id, cp::ForceDisposition::retreated}};
        facts.result = result;
    }
    facts.fleets = {50 + static_cast<eawr::sim::EntityId>(player)};
    facts.orbiting = {40, 50 + static_cast<eawr::sim::EntityId>(player)};
    return facts;
}
std::vector<std::uint8_t> saved(const cp::GalacticAiLaneInbox& inbox) {
    std::vector<std::uint8_t> bytes; inbox.encode(bytes); return bytes;
}
std::size_t kind_of(const cp::GalacticAiLaneInbox::Entry& entry) {
    if (std::holds_alternative<cp::GalacticAiBattleFacts>(entry)) return 3;
    return std::get<ga::NativeActions::Delivery>(entry).index();
}

void inbox_order_and_save() {
    cp::GalacticAiLaneInbox empty;
    expect(saved(empty).empty(), "an empty inbox adds no saved bytes (the outbox state alone stays the component)");
    expect(empty.restore({}) && empty.empty(), "no block restores an empty inbox");

    // Three ticks, two players, four kinds, pushed in arrival order.
    cp::GalacticAiLaneInbox inbox;
    std::vector<std::size_t> kinds;
    for (std::uint64_t tick = 1; tick <= 3; ++tick)
        for (gc::PlayerId player = 1; player <= 2; ++player) {
            for (auto entry : {outcome(player, tick * 100 + player, tick % 2 == 1), movement(tick, player, 50 + player),
                     actors(player, tick * 1000 + player * 10), battle(tick, player, tick == 3)}) {
                kinds.push_back(kind_of(entry));
                inbox.push(std::move(entry));
            }
        }
    const auto bytes = saved(inbox);
    expect(!bytes.empty() && saved(inbox) == bytes, "the saved block is canonical");

    cp::GalacticAiLaneInbox reloaded;
    expect(reloaded.restore(bytes) && reloaded.entries().size() == inbox.entries().size() && saved(reloaded) == bytes,
        "a save in the middle of the queue reloads to the same entries");
    std::vector<std::size_t> reloaded_kinds;
    for (const auto& entry : reloaded.entries()) reloaded_kinds.push_back(kind_of(entry));
    expect(reloaded_kinds == kinds, "the reloaded queue keeps the arrival order across kinds");
    for (std::size_t i = 0; i < inbox.entries().size(); ++i)
        if (const auto* facts = std::get_if<cp::GalacticAiBattleFacts>(&inbox.entries()[i]))
            expect(std::get<cp::GalacticAiBattleFacts>(reloaded.entries()[i]) == *facts, "battle facts reload unchanged");
    {
        // #6378: a world script's AI-usage writes ride the actor notifications through a save.
        const auto& delivery = std::get<ga::NativeActions::Delivery>(reloaded.entries()[2]);
        const auto& change = std::get<ga::NativeActions::ActorChangesDelivery>(delivery).changes.at(1);
        expect(change.prevent_ai_usage == std::optional<bool>(true) && change.prefer_ground == std::optional<bool>(false)
            && !std::get<ga::NativeActions::ActorChangesDelivery>(delivery).changes.at(0).prevent_ai_usage,
            "the AI-usage writes of an actor notification reload unchanged");
    }

    const auto before = inbox.generation();
    const auto taken = inbox.take();
    expect(taken.size() == kinds.size() && inbox.empty() && inbox.generation() != before && saved(inbox).empty(),
        "taking the queue empties it, changes its generation and leaves no saved block");

    auto trailing = bytes; trailing.push_back(0);
    expect(!cp::GalacticAiLaneInbox{}.restore(trailing), "trailing bytes after the block are refused");
    auto cut = bytes; cut.pop_back();
    expect(!cp::GalacticAiLaneInbox{}.restore(cut), "a truncated block is refused");
    auto foreign = bytes; foreign[8] ^= 0x20;
    expect(!cp::GalacticAiLaneInbox{}.restore(foreign), "a block with another tag is refused");
    cp::GalacticAiLaneInbox untouched; untouched.push(outcome(1, 1, true));
    expect(!untouched.restore(cut) && untouched.entries().size() == 1, "a refused block leaves the queue as it was");
    expect(!untouched.restore(foreign) && untouched.entries().size() == 1, "a foreign block leaves the queue as it was");
}
// W1 S3: the billing view. A bill between a window and its publication tick uses the older published budget; the newer one takes
// over at its tick with the log since its cut replayed; the saved block reloads to the same view.
gc::funding::Ledger budget(eawr::sim::math::Fixed capital) {
    gc::funding::Ledger ledger;
    ledger.categories[1].cash = eawr::sim::math::Fixed::from_raw(1000 * eawr::sim::math::Fixed::scale);
    gc::funding::Goal goal; goal.category = 1; goal.cost = eawr::sim::math::Fixed::from_raw(100 * eawr::sim::math::Fixed::scale);
    goal.capital = capital; ledger.goals[7] = goal;
    return ledger;
}
void billing_view_visibility_and_save() {
    using Fixed = eawr::sim::math::Fixed;
    const auto credits = [](std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); };
    const ga::BillRow row{1, 7, credits(100), {}, {}, credits(50), false};
    cp::GalacticAiBillingPublication first{0, 0, {{1, budget(credits(100))}}, {{900, row}}, {}, {}, {}, {}, {}, {}};
    cp::GalacticAiBillingPublication later{30, 0, {{1, budget(credits(0))}}, {{900, row}}, {}, {}, {}, {}, {}, {}}; // window 0 spent the goal's capital
    // W1 S4: each publication also carries the AI digests the per-tick hashes read from its tick on.
    first.ai_digests.emplace("galactic-goals", std::string(64, 'a')); first.scheduler_digest = std::string(64, 'b');
    later.ai_digests.emplace("galactic-goals", std::string(64, 'c')); later.scheduler_digest = std::string(64, 'd');
    // W1 S6: and the plot edits its window's plan scripts made, handed to the world once when it becomes visible.
    gc::story::PlotCommand plot; plot.op = gc::story::PlotCommand::Op::event_param; plot.subplot = "Story_Sub_Test.xml";
    plot.event = "Trigger_Test"; plot.index = 2; plot.values = {"Rebel", "Coruscant"};
    later.plot_commands.push_back(plot);
    later.diagnostics.push_back("galactic path coefficients: test note");
    // Review #6425: a script-supplied name longer than any fixed bound saves and reloads; the window's load notes ride along too.
    gc::story::PlotCommand long_plot = plot; long_plot.subplot = std::string(5000, 's');
    later.plot_commands.push_back(long_plot);
    eawr::core::LoadNote note; note.code = "EAWR-LOAD-1236"; note.object = "17"; note.tag = "battle_delivery";
    note.phase = eawr::core::LoadPhase::runtime; note.action = eawr::core::LoadAction::runtime_degraded; note.line = 4;
    note.layer.chain_position = 2; note.message = "test delivery note";
    later.load_notes.push_back(note);
    cp::GalacticAiBillingView view;
    view.capture(first); expect(static_cast<bool>(view.publish_due(0)) && view.active(), "the first publication is visible at once");
    view.capture(later);
    const std::vector<gc::ProductionPlayer> players{gc::ProductionPlayer{}};
    gc::GalacticSession::NativeProductionNotice notice{gc::GalacticSession::NativeProductionNotice::Kind::billed, 1, 900, credits(30),
        credits(100), credits(40), 1};
    auto early = view.bill(notice, players);
    expect(early && *early && early->value().payment.accepted && early->value().payment.amount == credits(30),
        "a bill before the newer publication's tick is decided against the older budget");
    expect(view.rows().at(900).billed == credits(30) && view.log_end() == 1, "the bill is logged and applied to the view");
    const auto replayed = view.latest_replayed();
    auto expected = budget(credits(0));
    expected = gc::funding::spend(expected, 7, credits(30)).value();
    expect(replayed && replayed.value().first.at(1) == expected && replayed.value().second.at(900).billed == credits(30),
        "the newest publication with the log replayed is what the AI holds after its drain");
    std::vector<std::uint8_t> stored; expect(static_cast<bool>(view.encode(stored)) && !stored.empty(), "an active view saves a block");
    cp::GalacticAiBillingView reloaded;
    expect(static_cast<bool>(reloaded.restore(stored)) && reloaded.ledgers() == view.ledgers() && reloaded.rows() == view.rows()
        && reloaded.log_end() == view.log_end(), "the saved view reloads to the same budget, rows and log");
    std::vector<std::uint8_t> again; expect(static_cast<bool>(reloaded.encode(again)) && again == stored, "and saves the same bytes");
    expect(view.visible() && view.visible()->scheduler_digest == std::string(64, 'b')
        && view.visible()->ai_digests.at("galactic-goals") == std::string(64, 'a'), "before its tick the older publication's digests are visible");
    expect(reloaded.visible() && *reloaded.visible() == *view.visible(), "the saved view reloads the visible publication with its digests");
    cp::GalacticAiDueOutput handed;
    expect(static_cast<bool>(view.publish_due(30, &handed)), "the newer publication becomes visible at its tick");
    expect(handed.plot_commands.size() == 2 && handed.plot_commands.front() == plot && handed.plot_commands.back() == long_plot
        && view.visible()->plot_commands.empty()
        && handed.diagnostics == std::vector<std::string>{"galactic path coefficients: test note"} && view.visible()->diagnostics.empty()
        && handed.load_notes.size() == 1 && handed.load_notes.front().message == note.message && view.visible()->load_notes.empty(),
        "its plot edits, diagnostics and load notes are handed to the world once, in order, and the view keeps none");
    cp::GalacticAiDueOutput reloaded_handed;
    expect(static_cast<bool>(reloaded.publish_due(30, &reloaded_handed)) && reloaded_handed.plot_commands == handed.plot_commands
        && reloaded_handed.load_notes.size() == 1 && reloaded_handed.load_notes.front().line == note.line
        && reloaded_handed.load_notes.front().layer == note.layer && reloaded_handed.load_notes.front().code == note.code,
        "the reloaded view hands over the same edits and notes");
    cp::GalacticAiDueOutput again_handed;
    expect(static_cast<bool>(view.publish_due(31, &again_handed)) && again_handed.plot_commands.empty() && again_handed.diagnostics.empty()
        && again_handed.load_notes.empty(),
        "a later tick hands nothing again");
    expect(view.visible() && view.visible()->scheduler_digest == std::string(64, 'd')
        && view.visible()->ai_digests.at("galactic-goals") == std::string(64, 'c'), "from its tick the newer publication's digests are visible");
    expect(view.ledgers().at(1) == expected, "from then on the view is the newer budget with the logged bill replayed");
    auto late = view.bill(notice, players);
    expect(late && *late && !late->value().payment.accepted, "the same bill after the publication sees the spent capital");
    std::vector<std::uint8_t> empty; expect(static_cast<bool>(cp::GalacticAiBillingView{}.encode(empty)) && empty.empty(),
        "an inactive view adds no bytes");
    expect(cp::GalacticAiBillingView{}.visible() == nullptr, "an inactive view publishes no digests");
    cp::GalacticAiLaneInbox inbox;
    inbox.push(gc::GalacticSession::CreditNotification{gc::GalacticSession::CreditNotification::Kind::windfall, 1, credits(5), credits(5), {}});
    inbox.push(cp::GalacticAiRecordedBill{900, early->value().effect});
    std::vector<std::uint8_t> both; inbox.encode(both);
    const auto inbox_bytes = both.size();
    expect(static_cast<bool>(view.encode(both)), "the view block follows the inbox block");
    cp::GalacticAiLaneInbox read;
    const auto used = read.restore_prefix(both);
    expect(used && used.value() == inbox_bytes && saved(read) == std::vector<std::uint8_t>(both.begin(), both.begin() + static_cast<std::ptrdiff_t>(inbox_bytes)),
        "the inbox reads its own block and stops there");
}
// W1 S5: the story-owned GlobalValue store. The story writes it at once; an AI write reaches it only when its window's publication
// becomes visible (until then the AI's registry replays it); on the same key the AI's write, merged at that tick, wins.
void story_globals_split() {
    namespace auth = eawr::script::authoritative;
    using Number = eawr::script::numeric::LuaNumber;
    using Scalar = cp::GalacticStoryGlobals::Scalar;
    cp::GalacticStoryGlobals store;
    std::vector<std::uint8_t> nothing; store.encode(nothing);
    expect(nothing.empty() && !store.active(), "an inactive store adds no bytes");
    store.activate({});
    const auto set = [](std::string key, auth::Value value) {
        auth::ServiceReport report;
        report.commands.push_back(auth::ScriptCommand{1, 7, 1, std::string(auth::global_value_command), {auth::Value{std::move(key)}, std::move(value)}});
        return report;
    };
    auto story_write = set("Mission_Flag", auth::Value{Number(3)});
    store.commit(story_write);
    expect(story_write.commands.empty() && story_write.load_notes.empty() && store.values().at("Mission_Flag") == Scalar{Number(3)},
        "a story write reaches the store at once");
    cp::GalacticAiBillingView view;
    cp::GalacticAiBillingPublication now{0, 0, {}, {}, {}, {}, {}, {}, {}, {}};
    cp::GalacticAiBillingPublication window{30, 0, {}, {}, {}, {}, {}, {}, {}, {{"Mission_Flag", Scalar{Number(7)}}, {"AI_Only", Scalar{true}}}};
    view.capture(now);
    expect(static_cast<bool>(view.publish_due(0)), "the first publication is visible");
    view.capture(window);
    const auto pending = view.pending_global_writes();
    expect(pending.size() == 2 && pending.front().first == "Mission_Flag", "the AI's writes wait in the pending publication (its registry replays them)");
    expect(!store.values().contains("AI_Only"), "the story does not see an AI write before its publication's tick");
    auto later_story = set("Mission_Flag", auth::Value{Number(5)});
    store.commit(later_story);
    cp::GalacticAiDueOutput due;
    expect(static_cast<bool>(view.publish_due(30, &due)) && due.global_writes.size() == 2, "at its tick the publication hands its writes over");
    std::vector<eawr::core::LoadNote> notes;
    store.merge(due.global_writes, notes);
    expect(notes.empty() && store.values().at("Mission_Flag") == Scalar{Number(7)} && store.values().at("AI_Only") == Scalar{true},
        "merged in order at that tick: the AI's write wins over the story's earlier one on the same key");
    const std::vector<cp::GalacticStoryGlobals::Write> refused{
        {std::string(eawr::script::foc::galactic_ai::native_state_max_text_bytes + 1, 'k'), Scalar{Number(1)}}};
    store.merge(refused, notes);
    expect(notes.size() == 1 && notes.front().code == "EAWR-LOAD-1400", "an AI write the quotas refuse becomes a note at its tick");
    auto deleted = set("AI_Only", auth::Value{});
    store.commit(deleted);
    expect(!store.values().contains("AI_Only"), "a story delete removes the key");
    std::vector<std::uint8_t> stored; store.encode(stored);
    cp::GalacticStoryGlobals reloaded;
    const auto used = reloaded.restore_prefix(stored);
    expect(used && used.value() == stored.size() && reloaded.active() && reloaded.values() == store.values(), "the store saves and reloads");
}
} // namespace

int case_galactic_ai_lane_inbox_tests() {
    try {
        inbox_order_and_save();
        billing_view_visibility_and_save();
        story_globals_split();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures == 0 ? 0 : 1;
}
