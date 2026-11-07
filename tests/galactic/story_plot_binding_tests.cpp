#include "../support/result.hpp"
#include "galactic_support.hpp"
#include "story_plot_bindings.hpp"
#include "tactical_ai_internal.hpp"
#include "host.hpp"
#include "eawr/platform/sim_workers.hpp"

#include <iostream>
#include <string>
#include <vector>

// #5495: Get_Story_Plot and the plot and event objects, from an AI-plan style script, down to the plot engine.
namespace {
namespace st = eawr::sim::galactic::story;
namespace auth = eawr::script::authoritative;
namespace foc = eawr::script::foc;
using eawr::testing::take;
using galactic_test::expect;

st::SubPlotDef sub(std::string name, std::vector<std::string> events) {
    st::SubPlotDef out;
    out.name = std::move(name); out.owner = 1; out.active = false;
    for (auto& event : events) { st::EventDef e; e.name = st::upper(event); e.authored_name = event; e.type = "STORY_ELAPSED"; e.params[0] = {"0"}; out.events.push_back(e); }
    return out;
}

constexpr const char* plan = R"LUA(
function main()
    plot = Get_Story_Plot("Intervention_Test.xml")
    event = plot.Get_Event("Test_00")
    event.Set_Reward_Parameter(0, 3000)
    event.Set_Event_Parameter(0.6, "ABC", 7)
    event.Clear_Dialog_Text()
    event.Add_Dialog_Text("TEXT_REWARD", "TEXT_CREDITS", 3000)
    event.Set_Dialog("DIALOG_X")
    event.Set_Reward_Type("CREDITS")
    plot.Activate()
    plot.Suspend()
    plot.Reset()
    Valid = event.Is_Valid()
    Missing = Get_Story_Plot("Nope.xml")
end
function Bad()
    Get_Story_Plot("Intervention_Test.xml").Get_Event("Missing_Event")
end
function Huge()
    Get_Story_Plot("Intervention_Test.xml").Get_Event("Test_00").Set_Reward_Parameter(0, 1e300)
end
function Infinite()
    Get_Story_Plot("Intervention_Test.xml").Get_Event("Test_00").Set_Event_Parameter(1e300 * 1e300, "A")
end
function HugeDialog()
    Get_Story_Plot("Intervention_Test.xml").Get_Event("Test_00").Add_Dialog_Text("TEXT_X", 1e300)
end
)LUA";

auth::ServiceReport run(const std::shared_ptr<foc::detail::Host>& host, std::string_view script, std::string_view function) {
    auth::ModuleManifest manifest;
    take(manifest.add("test/plan.lua", std::string(script)));
    auth::SessionConfig config;
    config.seed = 5; config.galactic_clock = true; config.tick_duration = {1, 30}; config.script_directories = {"test/"};
    auto scheduler = take(auth::ScriptScheduler::create(config, std::move(manifest)));
    std::vector<eawr::core::Diagnostic> errors;
    foc::detail::register_story_plot_bindings(scheduler, host, errors);
    expect(errors.empty(), "the bindings register");
    take(scheduler.create_instance(1, "test/plan.lua"));
    auth::ScriptEvent event;
    event.key = {1, auth::first_simulation_producer, 1, 0};
    event.target = 1; event.kind = auth::ScriptEvent::Kind::start_thread; event.name = std::string(function);
    take(scheduler.submit_event(std::move(event)));
    const eawr::sim::InlineExecutor executor;
    auth::ServiceOptions options; options.time_tick = 1;
    return take(scheduler.service(executor, options));
}

void bindings() {
    std::vector<st::SubPlotDef> subs{sub("Intervention_Test.xml", {"Test_00", "Test_01"})};
    auto engine = take(st::Engine::create({}, {{1, "plot.xml"}}, subs));
    auto host = std::make_shared<foc::detail::Host>();
    host->story_plots = foc::StoryPlotLink::from(engine.definitions(), [&](const st::PlotCommand& c) { return engine.apply(c); });
    const auto pristine = engine.digest();
    auto report = run(host, plan, "main");
    std::size_t errors = 0;
    for (const auto& row : report.diagnostics) { if (row.code == auth::codes::script_error) ++errors; std::cerr << row.code << " " << row.message << "\n"; }
    expect(errors == 0, "the plan script runs without a script error");
    expect(report.commands.size() == 9, "nine ordered requests are staged");
    for (const auto& command : report.commands) {
        expect(command.verb == foc::verb_story_plot, "requests use the story plot verb");
        expect(foc::detail::commit_story_plot(*host, command).empty(), "each request commits");
    }
    const auto& state = engine.state();
    expect(state.edits.size() == 1 && state.edits[0].event == "TEST_00" && state.edits[0].reward_type == std::string("CREDITS")
        && state.edits[0].dialog == std::string("DIALOG_X"), "the event edit is recorded");
    expect(state.edits[0].reward_params.at(0) == std::vector<std::string>{"3000"}
        && state.edits[0].event_params.at(1) == (std::vector<std::string>{"ABC", "7"}), "numbers become tokens and a rounded 0-based index is used");
    expect(state.edits[0].dialog_text == (std::vector<std::string>{"text:TEXT_REWARD", "text:TEXT_CREDITS", "int:3000"}), "dialog text arguments keep their kind");
    expect(!state.subplots[0].active && engine.digest() != pristine, "Activate then Suspend leaves the plot off");
    // A missing event is a script error; a missing plot only warns.
    auto bad = run(host, std::string(plan) + "\nplot = Get_Story_Plot('Intervention_Test.xml')\n", "Bad");
    const auto script_errors = [](const auth::ServiceReport& r) {
        std::size_t n = 0;
        for (const auto& row : r.diagnostics) if (row.code == auth::codes::script_error) ++n;
        return n;
    };
    expect(bad.commands.empty(), "a refused call stages nothing");
    expect(script_errors(bad) == 1, "the refused call raises a script error");
    // Out-of-range numbers are script errors, never an undefined narrowing cast.
    for (const char* name : {"Huge", "Infinite", "HugeDialog"}) {
        auto wild = run(host, plan, name);
        expect(wild.commands.empty() && script_errors(wild) == 1, std::string(name) + " is refused with a script error");
    }
    // No plot engine hosted: the plot is absent with a warning.
    auto bare = std::make_shared<foc::detail::Host>();
    auto none = run(bare, "function main() Result = Get_Story_Plot('x.xml') end", "main");
    bool warned = false;
    for (const auto& row : none.diagnostics) warned = warned || row.code == auth::codes::script_warning;
    expect(warned && none.commands.empty(), "without hosted plots Get_Story_Plot warns and returns nothing");
}
} // namespace

int case_galactic_story_plot_binding_tests(int, char**) {
    try { bindings(); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
