// #7977 family 5 (story flags and events) in the battle script hosts: argument validation like the galactic bindings, an
// unsupported_api note for the effect a battle has no state for, and a script fault (never missing_api) for a bad call.

#include "foc_ai_contract_fixtures.hpp"
#include "tactical_ai_internal.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace foc_contract {

namespace {

struct StoryRun {
    bool loaded{};
    std::vector<std::string> codes;
    std::vector<std::string> messages;
    std::vector<int> marks;   // Mark(n) calls, in order
    std::vector<bool> flags;  // the Boolean arguments of Flag(b)
    std::size_t commands{};
};

StoryRun story_run(const std::string& body) {
    StoryRun out;
    auto host = std::make_shared<foc::detail::Host>();
    auto view = std::make_shared<foc::detail::WorldView>();
    foc::detail::ViewUnit unit;
    unit.id = 10; unit.owner = 1; unit.type = 100;
    view->units.push_back(unit);
    view->players = {{1, 1, 0}};
    host->view = view;
    auth::ModuleManifest manifest;
    if (!manifest.add("STORY.LUA", "function Run()\n" + body + "\nend\n").has_value()) return out;
    auth::SessionConfig config;
    config.tick_duration = {1, 30};
    auto scripts = auth::ScriptScheduler::create(config, std::move(manifest));
    if (!scripts) return out;
    std::vector<eawr::core::Diagnostic> errors;
    foc::tactical_ai_detail::register_globals(scripts.value(), host, errors);
    foc::tactical_ai_detail::register_methods(scripts.value(), host, errors);
    using Handle = auth::Handle;
    const auto test_binding = [&](const char* name, auth::Binding binding) {
        if (!scripts.value().register_binding(name, std::move(binding)).has_value()) errors.push_back({});
    };
    test_binding("Unit", [](auth::BindingContext&, const auth::ValueList&) -> eawr::core::Result<auth::ValueList> {
        return eawr::core::Result<auth::ValueList>::success({auth::Value{Handle{foc::handle_game_object, 10}}});
    });
    test_binding("Dead", [](auth::BindingContext&, const auth::ValueList&) -> eawr::core::Result<auth::ValueList> {
        return eawr::core::Result<auth::ValueList>::success({auth::Value{Handle{foc::handle_game_object, 99}}});
    });
    test_binding("Me", [](auth::BindingContext&, const auth::ValueList&) -> eawr::core::Result<auth::ValueList> {
        return eawr::core::Result<auth::ValueList>::success({auth::Value{Handle{foc::handle_player, 1}}});
    });
    test_binding("Mark", [&out](auth::BindingContext&, const auth::ValueList& args) -> eawr::core::Result<auth::ValueList> {
        const auto* number = args.size() == 1 ? std::get_if<eawr::script::numeric::LuaNumber>(&args[0].data) : nullptr;
        if (number != nullptr) out.marks.push_back(static_cast<int>(*number));
        return eawr::core::Result<auth::ValueList>::success({});
    });
    test_binding("Flag", [&out](auth::BindingContext&, const auth::ValueList& args) -> eawr::core::Result<auth::ValueList> {
        const auto* value = args.size() == 1 ? std::get_if<bool>(&args[0].data) : nullptr;
        if (value != nullptr) out.flags.push_back(*value);
        return eawr::core::Result<auth::ValueList>::success({});
    });
    if (!errors.empty() || !scripts.value().create_instance(1, "STORY.LUA").has_value()) return out;
    auth::ScriptEvent event;
    event.key = {1, auth::first_simulation_producer, 1, 0};
    event.target = 1; event.kind = auth::ScriptEvent::Kind::call; event.name = "Run";
    if (!scripts.value().submit_event(std::move(event)).has_value()) return out;
    const auto report = scripts.value().service(eawr::sim::InlineExecutor{});
    if (!report) return out;
    out.loaded = true;
    out.commands = report.value().commands.size();
    for (const auto& diagnostic : report.value().diagnostics) {
        out.codes.push_back(diagnostic.code);
        out.messages.push_back(diagnostic.message);
    }
    return out;
}

bool only_code(const StoryRun& run, const std::string_view code) {
    if (run.codes.empty()) return false;
    for (const auto& found : run.codes) if (found != code) return false;
    return true;
}

void valid_call(const std::string& name, const std::string& call) {
    const auto run = story_run(call + " Mark(1)");
    expect(run.loaded, "#7977 f5: " + name + " script runs");
    expect(run.marks == std::vector<int>{1}, "#7977 f5: " + name + " valid call lets the script go on");
    expect(only_code(run, std::string(auth::codes::unsupported_api)),
        "#7977 f5: " + name + " valid call reports unsupported_api only");
    expect(run.commands == 0, "#7977 f5: " + name + " issues no simulation command");
}

void bad_call(const std::string& name, const std::string& call) {
    const auto run = story_run(call + " Mark(1)");
    expect(run.loaded, "#7977 f5: " + name + " bad call script runs");
    expect(run.marks.empty(), "#7977 f5: " + name + " bad call ends the thread");
    expect(!run.codes.empty(), "#7977 f5: " + name + " bad call reports a fault");
    for (const auto& code : run.codes)
        expect(code != std::string(auth::codes::missing_api), "#7977 f5: " + name + " bad call is not missing_api");
}

} // namespace

void battle_story_contracts() {
    valid_call("Story_Event(name)", "Story_Event('Foo')");
    valid_call("Story_Event(name, nil)", "Story_Event('Foo', nil)");
    valid_call("Story_Event(name, object)", "Story_Event('Foo', Unit())");
    bad_call("Story_Event()", "Story_Event()");
    bad_call("Story_Event(number)", "Story_Event(3)");
    bad_call("Story_Event(name, string)", "Story_Event('Foo', 'Bar')");
    bad_call("Story_Event(name, object, extra)", "Story_Event('Foo', Unit(), 1)");

    // TL-S3: exactly four arguments; no battle plot sets a flag, so the answer is false WITH an unsupported_api note.
    for (const char* call : {"Check_Story_Flag(Me(), 'F', nil, true)", "Check_Story_Flag(Me(), 'F', Unit(), false)"}) {
        const auto run = story_run(std::string("Flag(") + call + ") Mark(1)");
        expect(run.loaded && run.marks == std::vector<int>{1} && run.flags == std::vector<bool>{false} && only_code(run, std::string(auth::codes::unsupported_api)) && run.commands == 0,
            std::string("#7977 f5: ") + call + " answers false with a note");
    }
    bad_call("Check_Story_Flag(2 args)", "Check_Story_Flag(Me(), 'F')");
    bad_call("Check_Story_Flag(3 args)", "Check_Story_Flag(Me(), 'F', Unit())");
    bad_call("Check_Story_Flag(non-player)", "Check_Story_Flag(5, 'F', nil, true)");
    bad_call("Check_Story_Flag(non-string)", "Check_Story_Flag(Me(), 7, nil, true)");
    bad_call("Check_Story_Flag(bad object)", "Check_Story_Flag(Me(), 'F', 'X', true)");
    bad_call("Check_Story_Flag(bad reset)", "Check_Story_Flag(Me(), 'F', nil, 'yes')");
    bad_call("Check_Story_Flag(5 args)", "Check_Story_Flag(Me(), 'F', nil, true, 1)");

    valid_call("Game_Message", "Game_Message('TEXT_KEY')");
    bad_call("Game_Message()", "Game_Message()");
    bad_call("Game_Message(number)", "Game_Message(4)");
    bad_call("Game_Message(2 args)", "Game_Message('A', 'B')");

    valid_call("Add_Objective", "Add_Objective('TEXT_OBJ')");
    valid_call("Add_Objective(extra)", "Add_Objective('TEXT_OBJ', 1, 2)");
    bad_call("Add_Objective()", "Add_Objective()");
    bad_call("Add_Objective(number)", "Add_Objective(1)");

    for (const char* name : {"Add_Planet_Highlight", "Remove_Planet_Highlight"}) {
        valid_call(name, std::string(name) + "(Unit())");
        bad_call(std::string(name) + "()", std::string(name) + "()");
        bad_call(std::string(name) + "(string)", std::string(name) + "('Kuat')");
    }
    valid_call("Remove_All_Text", "Remove_All_Text()");
    bad_call("Remove_All_Text(arg)", "Remove_All_Text('x')");

    for (const char* name : {"Highlight", "Highlight_Small"}) {
        valid_call(name, std::string("Unit().") + name + "(true)");
        valid_call(std::string(name) + "(on, option)", std::string("Unit().") + name + "(true, 1)");
        bad_call(std::string(name) + "()", std::string("Unit().") + name + "()");
        bad_call(std::string(name) + "(number)", std::string("Unit().") + name + "(1)");
        const auto dead = story_run(std::string("Dead().") + name + "(true) Mark(1)");
        expect(dead.loaded && dead.marks == std::vector<int>{1} && dead.codes.empty(),
            std::string("#7977 f5: ") + name + " on a dead object answers nothing, silently");
    }
}

} // namespace foc_contract
