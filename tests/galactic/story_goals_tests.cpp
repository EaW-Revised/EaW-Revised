#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "../support/check.hpp"
#include "eawr/sim/galactic/story_goals.hpp"
#include <iostream>
#include <stdexcept>
namespace st = eawr::sim::galactic::story;
namespace {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool ok, const char* label) {
    if (!ok) local_checker.check_before_print(ok, label, std::cerr, "");
}
void objectives() {
    st::Goals goals(1); st::FakePresentationSink sink;
    const auto apply = [&](st::ObjectiveAction action, std::string key = {}, st::Objective value = {}, std::int64_t index = -1) {
        st::ObjectiveReward reward{action, std::move(key), std::move(value), false};
        reward.index = index;
        return goals.apply(reward, sink);
    };
    const auto ids = [&] { std::vector<std::string> out; for (const auto& o : goals.state().objectives) out.push_back(o.text_id); return out; };
    using Ids = std::vector<std::string>;
    // WST-65: an empty identity does nothing; an add takes the counter's number and increments it, status incomplete.
    expect(!apply(st::ObjectiveAction::add, {}, {}) && goals.state().objectives.empty() && sink.events.empty(), "WST-65 empty identity does nothing");
    expect(apply(st::ObjectiveAction::add, {}, {"first", true, 99}) && apply(st::ObjectiveAction::add, {}, {"second", false, 99}), "WST-65 add");
    expect(goals.state().objectives[0].number == 1 && goals.state().objectives[1].number == 2 && goals.state().objectives[0].suggestion
        && goals.state().next_number == 3, "WST-65 numbers come from the counter, the suggestion flag is stored");
    // Duplicates are admitted; an index from 0 to the size inserts there, anything else appends.
    expect(apply(st::ObjectiveAction::add, {}, {"first"}) && ids() == Ids{"first", "second", "first"}, "WST-65 duplicate identities are admitted");
    expect(apply(st::ObjectiveAction::add, {}, {"front"}, 0) && ids() == Ids{"front", "first", "second", "first"}, "WST-65 index 0 inserts at the head");
    expect(apply(st::ObjectiveAction::add, {}, {"tail"}, 99) && apply(st::ObjectiveAction::add, {}, {"neg"}, -3)
        && ids() == Ids{"front", "first", "second", "first", "tail", "neg"}, "WST-65 an out-of-range index appends");
    // WST-66: first exact match; a missing match changes nothing.
    const auto count = sink.events.size();
    expect(!apply(st::ObjectiveAction::complete, "missing") && !apply(st::ObjectiveAction::complete, "First") && sink.events.size() == count, "WST-66 no match is inert, case is exact");
    expect(apply(st::ObjectiveAction::complete, "first") && goals.state().objectives[1].status == st::ObjectiveStatus::complete
        && goals.state().objectives[3].status == st::ObjectiveStatus::incomplete, "WST-66 complete hits the first match only");
    expect(apply(st::ObjectiveAction::complete, "first") && sink.events.size() == count + 2, "WST-66 completing again still gives feedback");
    expect(apply(st::ObjectiveAction::fail, "second") && goals.state().objectives[2].status == st::ObjectiveStatus::failed, "WST-66 fail");
    // Replace: both identities nonempty; keeps number, suggestion and position, resets status, admits a duplicate identity.
    const auto before = goals.state().objectives[1];
    expect(!apply(st::ObjectiveAction::replace, "first", {}) && !apply(st::ObjectiveAction::replace, {}, {"x"}) && !apply(st::ObjectiveAction::replace, "gone", {"x"}), "WST-66 replace needs both identities and a match");
    expect(apply(st::ObjectiveAction::replace, "first", {"second"}) && goals.state().objectives[1].text_id == "second"
        && goals.state().objectives[1].number == before.number && goals.state().objectives[1].suggestion == before.suggestion
        && goals.state().objectives[1].status == st::ObjectiveStatus::incomplete, "WST-66 replace keeps number/suggestion/position, resets status, admits a duplicate");
    // Remove deletes the first match and never rewinds the counter.
    const auto next = goals.state().next_number;
    expect(apply(st::ObjectiveAction::remove, "tail") && apply(st::ObjectiveAction::remove, "neg") && goals.state().next_number == next, "WST-66 removal does not rewind the counter");
    expect(apply(st::ObjectiveAction::add, {}, {"after"}) && goals.state().objectives.back().number == next, "the counter goes on after a removal");
    st::Goals restored(1, goals.state());
    expect(restored.state().objectives == goals.state().objectives && restored.state().next_number == goals.state().next_number, "WST-67 restore keeps order, duplicates, numbers and the counter");
    expect(apply(st::ObjectiveAction::display) && !goals.state().objectives_visible, "WST-66 hide display");
    expect(apply(st::ObjectiveAction::remove_all) && goals.state().objectives.empty() && goals.state().next_number > next, "WST-66 remove all keeps the counter");
    auto invalid = restored.state(); invalid.objectives.front().number = invalid.next_number;
    bool rejected{};
    try { st::Goals bad(1, invalid); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "fixture rejects a number beyond the counter");
}
void dialogs() {
    st::Goals goals(1); st::FakePresentationSink sink;
    st::Dialog d{"fixture_intro", "variable", {}, 2, false, true};
    st::LocalPresentation local{false, "transmission", "added"};
    expect(goals.dialog(d, local, sink), "WST-58 intro");
    expect(goals.state().goals[0].tag == "fixture_intro_2", "WST-58 default tag");
    expect(sink.events.size() == 4 && std::get<st::SummaryRequest>(sink.events[1].payload).action == st::SummaryAction::flash_button
        && std::get<st::AdvisorRequest>(sink.events[2].payload).text_id == "transmission"
        && std::get<st::SoundRequest>(sink.events[3].payload).sound_id == "added", "WST-58 flash/log/sound order");
    sink.events.clear(); local.summary_open = true; d.popup = true;
    expect(goals.dialog(d, local, sink) && sink.events.size() == 2
        && std::get<st::SummaryRequest>(sink.events[1].payload).action == st::SummaryAction::refresh, "WST-58 open summary refresh takes priority");
    expect(goals.state().goals.size() == 1, "policy intro key remains unique");
    sink.events.clear(); local.summary_open = false;
    expect(goals.dialog(d, local, sink) && sink.events.size() == 2
        && std::get<st::SummaryRequest>(sink.events[1].payload).action == st::SummaryAction::open, "WST-58 popup avoids flash/log/sound");
    expect(goals.apply(st::GoalReward{st::GoalAction::complete, "fixture_intro_2"}, sink), "WST-58 complete goal");
    expect(goals.apply(st::GoalReward{st::GoalAction::hide, "fixture_intro_2"}, sink), "fixture hide seam");
    d.variable = "updated variable";
    expect(goals.dialog(d, local, sink) && goals.state().goals.size() == 1
        && goals.state().goals[0].completed && goals.state().goals[0].hidden
        && goals.state().goals[0].variable == "updated variable"
        && std::get<st::GoalChanged>(sink.events[sink.events.size() - 2].payload).value->completed,
        "policy repeat after completion preserves outcome/visibility and updates content");
    st::Goals restored(1, goals.state());
    expect(restored.state().goals == goals.state().goals && restored.state().goals[0].completed && restored.state().goals[0].hidden, "goal fixture snapshot");
    expect(goals.apply(st::GoalReward{st::GoalAction::remove, "fixture_intro_2"}, sink) && goals.state().goals.empty(), "WST-58 remove goal");
    expect(!goals.dialog({}, local, sink), "empty dialog inert");
    d.tag = "first"; d.popup = false; local.defer_summary_state = true; sink.events.clear();
    expect(goals.dialog(d, local, sink) && sink.events.size() == 2, "WST-70 live quiet requests defer advisor and sound admission");
    const auto& deferred = std::get<st::SummaryRequest>(sink.events[1].payload);
    expect(deferred.action == st::SummaryAction::refresh_or_flash && deferred.transmission_text == "transmission"
        && deferred.mission_added_sound == "added", "WST-70 the local consumer receives the closed-branch outputs together");
    auto second = d; second.tag = "incoming"; second.remove_after_viewed = true;
    expect(goals.dialog(second, local, sink) && goals.state().goals.size() == 2
        && goals.state().goals[0].tag == "incoming" && goals.state().goals[0].remove_after_viewed
        && goals.state().goals[1].tag == "first" && !goals.state().goals[1].remove_after_viewed,
        "WST-58 new goals prepend and an incoming override preserves the tagless removal policy");
    d.variable = "first updated";
    expect(goals.dialog(d, local, sink) && goals.state().goals[0].tag == "incoming"
        && goals.state().goals[1].variable == "first updated", "a repeated goal updates its existing slot");
}
void adapter_sequence() {
    // Stock GC shape (WST-30/58): elapsed start -> reward -> human intro dialog.
    // Synthetic identifiers; no retail story text/assets. Engine owns elapsed threshold.
    st::FakeFeed feed; st::DispatchGate gate; gate.active_mode = false;
    std::vector<st::Recipient> recipients{{"sandbox", 1, true}};
    expect(feed.submit({1, 1, st::ElapsedNotice{}}, gate, recipients).empty(), "WST-13 intro queues while parent inactive");
    gate.active_mode = true;
    const auto deliveries = feed.service(gate, recipients);
    expect(deliveries.size() == 1 && feed.service(gate, recipients).empty(), "WST-13 intro delivered once");
    st::Goals goals(1); st::FakePresentationSink sink;
    st::GoalTrigger trigger{st::ObjectiveReward{st::ObjectiveAction::add, {}, {"intro", {}, 1}, true}, st::Dialog{"fixture_intro", {}, {}, 1, false, false}};
    bool callback{}, dependants{};
    expect(st::trigger_goals(goals, deliveries.front(), "sandbox", trigger, {}, sink,
        [&] { callback = goals.state().objectives.empty() && sink.events.empty(); },
        [&] { dependants = goals.state().objectives.size() == 1 && goals.state().goals.size() == 1 && sink.events.size() == 4; }), "WST-20 delivery reaches goals");
    expect(callback && dependants && std::holds_alternative<st::ObjectiveChanged>(sink.events[0].payload)
        && std::holds_alternative<st::GoalChanged>(sink.events[1].payload), "WST-20 callback/reward/dialog/dependants order");
    expect(!st::trigger_goals(goals, deliveries.front(), "other", trigger, {}, sink, {}, {}), "delivery plot isolation");
    expect(goals.apply(st::ConsumerRequest{st::Consumer::camera, "POSITION_CAMERA", {"authored", "raw"}}, sink), "WST-49 request sink");
    const auto& request = std::get<st::ConsumerRequest>(sink.events.back().payload);
    expect(request.parameters.size() == 2 && request.parameters[1] == "raw" && sink.events.back().owner == 1, "WST-U09 preserves raw parameters/owner");
}
}
int case_galactic_story_goals_tests() { objectives(); dialogs(); adapter_sequence(); return failures == 0 ? 0 : 1; }
