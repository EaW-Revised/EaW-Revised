#include "eawr/sim/galactic/story_goals.hpp"
#include <iostream>
#include <stdexcept>
namespace st = eawr::sim::galactic::story;
namespace {
int failures{};
void expect(bool ok, const char* label) { if (!ok) { ++failures; std::cerr << label << '\n'; } }
void objectives() {
    st::Goals goals(1); st::FakePresentationSink sink;
    const auto apply = [&](st::ObjectiveAction action, std::string key = {}, st::Objective value = {}) {
        return goals.apply(st::ObjectiveReward{action, std::move(key), std::move(value), false}, sink);
    };
    expect(apply(st::ObjectiveAction::add, {}, {"first", "suggestion", 7}), "WST-47 add");
    expect(apply(st::ObjectiveAction::add, {}, {"second", {}, 9}), "WST-47 second add");
    expect(apply(st::ObjectiveAction::add, {}, {"first", "updated", 8}), "policy duplicate update");
    expect(goals.state().objectives.size() == 2 && goals.state().objectives[0].suggestion == "updated", "policy stable duplicate slot");
    const auto count = sink.events.size();
    expect(!apply(st::ObjectiveAction::replace, "first", {"second", {}, 0}), "policy conflicting identity rejected");
    expect(!apply(st::ObjectiveAction::complete, "missing") && sink.events.size() == count, "unknown identity is inert");
    expect(apply(st::ObjectiveAction::replace, "first", {"replacement", "new", 3}), "WST-47 replace");
    expect(apply(st::ObjectiveAction::complete, "replacement") && apply(st::ObjectiveAction::fail, "second"), "WST-47 resolve");
    st::Goals restored(1, goals.state());
    expect(restored.state().objectives == goals.state().objectives, "WST-60 fixture restore text/status/suggestion/number/order");
    expect(apply(st::ObjectiveAction::display) && !goals.state().objectives_visible, "WST-47 hide display");
    expect(apply(st::ObjectiveAction::remove, "replacement") && goals.state().objectives.front().text_id == "second", "WST-47 remove preserves survivors");
    expect(apply(st::ObjectiveAction::remove_all) && goals.state().objectives.empty(), "WST-47 remove all");
    auto invalid = restored.state(); invalid.objectives.push_back(invalid.objectives.front());
    bool rejected{};
    try { st::Goals bad(1, invalid); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "fixture rejects duplicate snapshot identity");
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
int main() { objectives(); dialogs(); adapter_sequence(); return failures == 0 ? 0 : 1; }
