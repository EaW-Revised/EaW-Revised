#include "galactic_support.hpp"
#include "../../src/campaign/galactic_ai_lane_inbox.hpp"
#include "../../src/campaign/galactic_ai_runtime.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
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

// Async step 2: a refused hop or purchase waits in the inbox as a consequence and survives a save.
void refusal_entries_save() {
    using Consequence = ga::refusal::Consequence;
    cp::GalacticAiLaneInbox inbox;
    inbox.push(Consequence{Consequence::Kind::movement, 2, 77, {5, 9}});
    inbox.push(outcome(1, 100, true));
    inbox.push(Consequence{Consequence::Kind::purchase, 1, 5000000001ULL, {}});
    const auto bytes = saved(inbox);
    cp::GalacticAiLaneInbox restored;
    expect(restored.restore(bytes) && restored.entries().size() == 3 && saved(restored) == bytes, "refusal consequences round-trip in arrival order");
    const auto* movement = std::get_if<Consequence>(&restored.entries().front());
    expect(movement && movement->kind == Consequence::Kind::movement && movement->player == 2 && movement->id == 77
        && movement->requests == std::vector<std::uint64_t>{5, 9}, "a movement consequence keeps the block identities it was queued for");
    // Exact layout of a one-entry block: tag length, tag, version, count, then kind, consequence kind, player, id, request count, requests.
    cp::GalacticAiLaneInbox single;
    single.push(Consequence{Consequence::Kind::movement, 2, 77, {5}});
    const auto block = saved(single);
    constexpr std::size_t tag = std::string_view("eawr-gcai-lane-inbox").size();
    const std::size_t version = 8 + tag, count = version + 8, kind = count + 8, refusal_kind = kind + 8, requests = refusal_kind + 8 * 3;
    const auto word = [&](std::vector<std::uint8_t> copy, std::size_t at, std::uint64_t value) {
        for (unsigned i = 0; i < 8; ++i) copy[at + i] = static_cast<std::uint8_t>(value >> (i * 8U));
        return copy;
    };
    const auto refused_block = [&](const std::vector<std::uint8_t>& copy) { cp::GalacticAiLaneInbox probe; return !probe.restore(copy); };
    expect(!refused_block(block), "the one-entry block restores");
    expect(refused_block(word(block, version, 3)), "the previous block version (v3: control switches, no refusal entries) is refused");
    expect(refused_block(word(block, refusal_kind, 2)), "an unknown consequence kind is refused");
    expect(refused_block(word(block, requests, 1ULL << 40)), "an absurd request count is refused");
    expect(refused_block(word(block, requests, 0)), "a movement consequence without its blocks is refused");
    auto truncated = block; truncated.pop_back();
    auto trailing = block; trailing.push_back(0);
    expect(refused_block(truncated) && refused_block(trailing), "a truncated block and trailing bytes are refused");
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
// #6917: the story's AI control switches wait in the inbox like every other world notice, and the types the sim side applied are saved with it.
void type_switch_save() {
    cp::GalacticAiLaneInbox inbox;
    expect(saved(inbox).empty() && inbox.applied_types().empty(), "no switch, no saved bytes");
    inbox.push(outcome(1, 1, true));
    inbox.push(cp::GalacticAiTypeSwitch{2, "EmpireAI"});
    inbox.push(cp::GalacticAiTypeSwitch{3, "None"});
    inbox.note_type(2, "OLDTYPE");
    const auto bytes = saved(inbox);
    cp::GalacticAiLaneInbox reloaded;
    expect(reloaded.restore(bytes) && saved(reloaded) == bytes && reloaded.entries().size() == 3, "a queue with switches reloads to the same bytes");
    expect(std::get<cp::GalacticAiTypeSwitch>(reloaded.entries()[1]) == cp::GalacticAiTypeSwitch{2, "EmpireAI"}
        && std::get<cp::GalacticAiTypeSwitch>(reloaded.entries()[2]) == cp::GalacticAiTypeSwitch{3, "None"}, "the queued switches reload unchanged and in order");
    expect(reloaded.applied_types() == std::map<gc::PlayerId, std::string>{{2, "OLDTYPE"}}, "the applied types reload unchanged");
    auto cut = bytes; cut.pop_back();
    expect(!cp::GalacticAiLaneInbox{}.restore(cut), "a truncated switch block is refused");
    // Taking the queue keeps what was applied: the block stays, with no entries.
    static_cast<void>(inbox.take());
    const auto kept = saved(inbox);
    cp::GalacticAiLaneInbox only_types;
    expect(!kept.empty() && only_types.restore(kept) && only_types.entries().empty()
        && only_types.applied_types() == std::map<gc::PlayerId, std::string>{{2, "OLDTYPE"}}, "applied types outlive the drained queue in the saved block");
    inbox.note_type(2, "NEWTYPE"); inbox.note_type(5, "OTHER");
    cp::GalacticAiLaneInbox updated;
    expect(updated.restore(saved(inbox)) && updated.applied_types().size() == 2 && updated.applied_types().at(2) == "NEWTYPE"
        && updated.applied_types().at(5) == "OTHER", "a later switch of the same player replaces its applied type");
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
    using Digest = cp::GalacticAiBillingView::SchedulerDigest;
    expect(view.scheduler_digest_state() == Digest::none_visible, "before any publication is visible the hash may read the live scheduler (bootstrap)");
    view.capture(first); expect(static_cast<bool>(view.publish_due(0)) && view.active(), "the first publication is visible at once");
    expect(view.scheduler_digest_state() == Digest::published, "a visible publication with its digest is read as published");
    {   // Step 2c: a visible publication without its digest is reported as missing, never as the bootstrap (no live read).
        auto bare = first; bare.scheduler_digest.clear();
        cp::GalacticAiBillingView degraded;
        degraded.capture(bare);
        expect(static_cast<bool>(degraded.publish_due(0)) && degraded.scheduler_digest_state() == Digest::missing
            && !cp::GalacticAiBillingView::missing_scheduler_digest_marker.empty(),
            "a visible publication without its digest is missing, with a fixed marker to hash instead");
    }
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
// #6337: the async stage tells the AI freestore about a loss at once; the world applies it D ticks later. The world's own writes to the
// still-live actor in between, and a loss the world refused, are reconciled with the recorded stage loss; nothing else is relaxed.
void stage_losses_reconcile() {
    using Source = cp::GalacticAiStageLosses::Source;
    using Kind = Change::Kind;
    cp::GalacticAiStageLosses losses;
    std::vector<std::uint8_t> nothing; losses.encode(nothing);
    expect(nothing.empty() && losses.empty(), "an empty set adds no bytes (saves and digests without a pending stage loss do not move)");
    const auto facts = [](eawr::sim::EntityId id) {
        ga::FreestoreFacts row; row.object.id = id; row.object.type = 7; row.object.owner = 1; row.object.planet = 2;
        row.object.kind = ga::FreestoreKind::fleet; row.object.safe = true; row.current_type = 7; row.parent = 2;
        row.parent_is_planet = true; row.object.directly_parented_to_planet = true; row.ai_usable = true; row.qualified = true;
        return row;
    };
    const eawr::sim::InlineExecutor executor;
    const auto reader = [&](eawr::sim::EntityId actor) { return eawr::core::Result<ga::FreestoreFacts>::success(facts(actor)); };
    const auto apply = [&](ga::QualifiedFreestore& store, const std::vector<Change>& changes) {
        return cp::apply_galactic_ai_actor_changes(store, changes, reader, executor);
    };
    const auto change = [](Kind kind, eawr::sim::EntityId actor) { return Change{kind, actor, 0, {}, {}, {}, {}, {}}; };
    ga::QualifiedFreestore store;
    expect(static_cast<bool>(apply(store, {change(Kind::created, 100), change(Kind::created, 101), change(Kind::created, 102)})),
        "the AI freestore holds the actors");
    // The window loses all three and says so at once; the world still holds them.
    expect(static_cast<bool>(apply(store, {change(Kind::lost, 100), change(Kind::lost, 101), change(Kind::lost, 102)})) && !store.facts(100),
        "the stage loss reaches the freestore at once");
    losses.record(100, {900, 3}); losses.record(101, {900, 4}); losses.record(102, {900, 5});
    expect(losses.size() == 3, "each stage loss is filed under the slice that holds it");
    // Negative control: without the set the world's change of a dropped actor is refused, as before.
    const auto refused = apply(store, {change(Kind::changed, 100)});
    expect(!refused && refused.error().message.find("retained changed actor") != std::string::npos,
        "without the set a change of an actor the AI dropped is refused (the #6337 failure)");
    const auto still = [](const Source& source) { return source.virtual_tick != 4; }; // slice 4 of batch 900 has applied
    // (b) the slice still waits: the world's change is ignored, the entry stays, and the world's later `lost` settles it.
    const auto ignored = losses.reconcile(std::vector<Change>{change(Kind::changed, 100)}, still);
    expect(ignored.empty() && losses.size() == 3, "a change while the loss is pending is ignored and the entry stays");
    expect(static_cast<bool>(apply(store, ignored)) && !store.facts(100), "the freestore still lacks it");
    const auto settled = losses.reconcile(std::vector<Change>{change(Kind::lost, 100)}, still);
    expect(settled.size() == 1 && settled.front().kind == Kind::lost && losses.size() == 2 && static_cast<bool>(apply(store, settled)) && !store.facts(100),
        "the world's lost clears the entry and is a no-op for the dropped actor");
    // (c) the slice applied but the world kept the actor: it is born again in the AI, from the delivery's facts.
    const auto reborn = losses.reconcile(std::vector<Change>{change(Kind::changed, 101)}, still);
    expect(reborn.size() == 1 && reborn.front().kind == Kind::created && losses.size() == 1, "a change after a refused loss is a fresh birth and clears the entry");
    expect(static_cast<bool>(apply(store, reborn)) && store.facts(101) && store.facts(101)->object.id == 101, "the AI knows the live actor again");
    expect(static_cast<bool>(apply(store, losses.reconcile(std::vector<Change>{change(Kind::lost, 101)}, still))) && !store.facts(101),
        "a world lost after the re-introduction removes it again (applied later in the same tick, after the relocation)");
    // Not relaxed: an actor without an entry still refuses; an AI-usage write alone is left to the applier; a pure query copy never mutates.
    const auto unknown = apply(store, losses.reconcile(std::vector<Change>{change(Kind::changed, 555)}, still));
    expect(!unknown, "an unknown actor without an entry still refuses");
    Change usage = change(Kind::changed, 102); usage.prevent_ai_usage = true;
    const auto kept = losses.reconcile(std::vector<Change>{usage}, still);
    expect(kept.size() == 1 && kept.front().kind == Kind::changed && losses.size() == 1, "an AI-usage write alone is passed on untouched");
    const auto applied_slice = [](const Source&) { return false; }; // the slice of 102 has applied: the world kept the actor
    const auto pure = losses.reconciled(std::vector<Change>{change(Kind::changed, 102)}, applied_slice);
    expect(pure.size() == 1 && pure.front().kind == Kind::created && losses.size() == 1, "the replay view rewrites a copy and keeps the set");
    // A newer window losing the same actor again replaces the source; an abandoned attempt gives back what it recorded.
    losses.begin_batch(); losses.record(102, {915, 2}); losses.record(103, {915, 2});
    losses.abandon_batch();
    expect(losses.size() == 1 && losses.entries().at(102) == Source{900, 5}, "an abandoned attempt restores the set it began with");
    losses.begin_batch(); losses.record(102, {915, 2}); losses.end_batch(); losses.abandon_batch();
    expect(losses.entries().at(102) == Source{915, 2}, "a finished batch's records stay (the newest source wins)");
    // Saved after the story globals: the block round-trips, stops where it ends, and refuses a damaged frame.
    losses.record(104, {915, 7});
    std::vector<std::uint8_t> bytes; losses.encode(bytes);
    auto framed = bytes; framed.push_back(0);
    cp::GalacticAiStageLosses reloaded;
    const auto used = reloaded.restore_prefix(framed);
    expect(used && used.value() == bytes.size() && reloaded.entries() == losses.entries(), "the block saves and reloads, reading only its own bytes");
    std::vector<std::uint8_t> other = {1, 0, 0, 0, 0, 0, 0, 0, 'x'};
    expect(reloaded.restore_prefix(other) && reloaded.empty(), "bytes of another block (or none) mean an empty set: an older save loads with no pending stage loss");
    auto damaged = bytes; damaged[8 + 22] = 2; // the version word
    expect(!reloaded.restore_prefix(damaged), "an unknown version is refused");
    auto empty_count = bytes; for (unsigned i = 0; i < 8; ++i) empty_count[8 + 22 + 8 + i] = 0;
    expect(!reloaded.restore_prefix(empty_count), "a block with no entries is refused (an empty set writes no block)");

    // A world birth of the same identity clears the entry; the birth applies as sent.
    cp::GalacticAiStageLosses births;
    births.record(200, {900, 3}); births.record(201, {900, 3});
    const auto world_birth = births.reconcile(std::vector<Change>{change(Kind::created, 200)}, still);
    expect(world_birth.size() == 1 && world_birth.front().kind == Kind::created && births.size() == 1 && !births.entries().contains(200),
        "a world birth clears the stage-loss entry of its actor");
    // The refused path keeps the AI-usage writes of the change: the actor is born again, then the writes apply to it.
    // A change that carries usage writes next to another field (a usage-only change alone passes through untouched).
    Change carrying = change(Kind::changed, 201); carrying.dead = false; carrying.prefer_ground = false; carrying.prevent_ai_usage = true;
    const auto reborn_with_usage = births.reconcile(std::vector<Change>{carrying}, applied_slice);
    expect(reborn_with_usage.size() == 2 && reborn_with_usage[0].kind == Kind::created && !reborn_with_usage[0].prefer_ground
            && !reborn_with_usage[0].prevent_ai_usage && reborn_with_usage[1].kind == Kind::changed && reborn_with_usage[1].actor == 201
            && reborn_with_usage[1].prefer_ground == false && reborn_with_usage[1].prevent_ai_usage == true && births.empty(),
        "a re-birth after a refused loss keeps the change's AI-usage writes as a following usage-only change");
    ga::QualifiedFreestore usage_store;
    expect(static_cast<bool>(apply(usage_store, reborn_with_usage)) && usage_store.facts(201),
        "the re-birth and its usage write apply to the freestore");

    // The set is bounded: over its capacity the oldest source is evicted, and an abandoned attempt gives its evictions back.
    cp::GalacticAiStageLosses bounded(3);
    bounded.record(1, {10, 1}); bounded.record(2, {10, 2}); bounded.record(3, {11, 1});
    bounded.begin_batch(); bounded.record(4, {12, 1});
    expect(bounded.size() == 3 && !bounded.entries().contains(1) && bounded.entries().contains(4), "over capacity the oldest source is evicted");
    bounded.abandon_batch();
    expect(bounded.size() == 3 && bounded.entries().contains(1) && !bounded.entries().contains(4),
        "an abandoned attempt restores the evicted entry and drops its own");
    bounded.record(5, {12, 2});
    expect(bounded.size() == 3 && !bounded.entries().contains(1) && bounded.entries().contains(5), "a kept record evicts the oldest again");
}
// #6842 / CGS-19: while the view is the AI's budget, one AI player's refused fiscal cycle (here a tactical credit switch under a
// parent fiscal service, and a negative balance) is that player's: it is reported, skipped for that player (its ledger unchanged),
// the credit still succeeds, a negative income is simply applied, and the logged credit replays and reloads the same.
void billing_view_isolates_fiscal_refusal() {
    using Fixed = eawr::sim::math::Fixed;
    using Kind = gc::GalacticSession::CreditNotification::Kind;
    const auto credits = [](std::int64_t value) { return Fixed::from_raw(value * Fixed::scale); };
    auto switched = budget(credits(100)); switched.tactical.switched = true;
    cp::GalacticAiBillingPublication first{0, 0, {{1, budget(credits(100))}, {2, switched}}, {}, {}, {}, {}, {}, {}, {}};
    cp::GalacticAiBillingView view;
    struct Recorder final : cp::GalacticAiBillingView::FaultObserver {
        void isolated_fault(gc::PlayerId player, const eawr::core::Diagnostic& fault) override { reported.emplace_back(player, fault.message); }
        std::vector<std::pair<gc::PlayerId, std::string>> reported;
    };
    const auto recorder = std::make_shared<Recorder>();
    auto& reported = recorder->reported;
    view.bind_fault_observer(recorder);
    view.capture(first);
    expect(static_cast<bool>(view.publish_due(0)) && view.active(), "the view is the AI's budget");
    const auto player_one = view.ledgers().at(1);
    expect(static_cast<bool>(view.credit({Kind::fiscal, 1, credits(900), credits(-30), credits(10)})) && reported.empty()
        && view.ledgers().at(1) != player_one && view.ledgers().at(1).last_service_income == credits(-3),
        "a negative income is applied to the view's ledger and is not a fault");
    const auto afterwards = view.ledgers();
    expect(static_cast<bool>(view.credit({Kind::fiscal, 2, credits(900), credits(30), credits(10)})) && reported.size() == 1
        && reported.front().first == 2 && view.ledgers() == afterwards,
        "a fiscal credit to a tactically switched player is skipped for that player and does not end the session");
    expect(static_cast<bool>(view.credit({Kind::fiscal, 1, credits(-1), credits(1), credits(10)})) && reported.size() == 2
        && reported.back().first == 1 && view.ledgers() == afterwards,
        "a refused fiscal cycle of another player is skipped for that player too");
    expect(view.log_end() == 3, "every credit stays logged");
    const auto replayed = view.latest_replayed();
    expect(replayed && replayed.value().first == view.ledgers(), "the logged credits replay to the same ledgers, the refused ones skipped again");
    std::vector<std::uint8_t> stored; expect(static_cast<bool>(view.encode(stored)), "the view saves");
    cp::GalacticAiBillingView reloaded;
    expect(static_cast<bool>(reloaded.restore(stored)) && reloaded.ledgers() == view.ledgers(), "and reloads to the same ledgers");
    // A load replaces the view's state, not its wiring: the bound observer still hears the next refusal, exactly once.
    const auto heard = reported.size();
    expect(static_cast<bool>(view.restore(stored)), "the bound view loads its own saved block");
    expect(static_cast<bool>(view.credit({Kind::fiscal, 2, credits(900), credits(30), credits(10)})) && reported.size() == heard + 1
        && reported.back().first == 2, "after a load a refused cycle is still skipped and reported exactly once");
    // The runtime says a refusal once per player and refusal for the session.
    cp::IsolatedFaultOnce once;
    eawr::core::Diagnostic fault; fault.code = "EAWR-GCFUND-0001"; fault.message = "refused";
    auto other = fault; other.message = "refused otherwise";
    expect(once.first(2, fault) && !once.first(2, fault) && once.first(3, fault) && once.first(2, other) && !once.first(2, other),
        "a refusal is said once per player and refusal");
}
} // namespace

int case_galactic_ai_lane_inbox_tests() {
    try {
        refusal_entries_save();
        inbox_order_and_save();
        type_switch_save();
        billing_view_visibility_and_save();
        billing_view_isolates_fiscal_refusal();
        story_globals_split();
        stage_losses_reconcile();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures == 0 ? 0 : 1;
}
