// The engine side of the authoritative scheduler (#79): handle methods, engine
// calls, global assignments and reads, host-paced thread pumps, engine thread
// creation, the missing-API diagnostic and per-instance load.
//
//   lua_engine_tests

#include "harness.hpp"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace eawr::script::authoritative::test;

constexpr std::uint32_t object_kind = 7;

constexpr char engine_script[] = R"LUA(
function Base_Definitions()
    Count = 0
    Name = nil
end

function main()
    while true do
        Count = Count + 1
        coroutine.yield(true)
    end
end

function On_Service(object)
    Name = object.Name()
    Bound = object.Name
    Absent = object.Nope
    object.Order(3)
end

function Use_Missing()
    Missing_Engine_Call()
    object_after_missing = true
end
)LUA";

auth::ScriptEvent engine_event(std::uint64_t tick, std::uint64_t sequence, auth::ScriptEvent::Kind kind, std::string name) {
    auth::ScriptEvent event;
    event.key = auth::EventKey{tick, auth::first_simulation_producer, 1, sequence};
    event.target = 1;
    event.kind = kind;
    event.name = std::move(name);
    return event;
}

std::optional<auth::Value> global(Session& session, std::string_view name) {
    auto value = session.scheduler->read_global(1, name);
    expect(value.has_value(), "read_global " + std::string(name));
    return value ? value.value() : std::nullopt;
}

std::string global_text(Session& session, std::string_view name) {
    const auto value = global(session, name);
    return value ? text(*value) : "(unreadable)";
}

void engine() {
    Session session = make_session({{"Main.lua", engine_script}});
    auto& scheduler = *session.scheduler;
    expect(scheduler.register_method(object_kind, "Name", [](auth::BindingContext&, const auth::ValueList& arguments) {
        const auto& handle = std::get<auth::Handle>(arguments[0].data);
        return eawr::core::Result<auth::ValueList>::success(auth::ValueList{auth::Value::text("object " + std::to_string(handle.id))});
    }).has_value(), "register method Name");
    expect(scheduler.register_method(object_kind, "Order", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        context.issue_command("order", arguments);
        return eawr::core::Result<auth::ValueList>::success(auth::ValueList{});
    }).has_value(), "register method Order");
    expect(!scheduler.register_method(object_kind, "Name", [](auth::BindingContext&, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success(auth::ValueList{});
    }).has_value(), "a method name is registered once per kind");
    expect(scheduler.register_binding("Missing_Engine_Call", [](auth::BindingContext&, const auth::ValueList&) {
        eawr::core::Diagnostic diagnostic;
        diagnostic.code = std::string(auth::codes::missing_api);
        diagnostic.message = "Missing_Engine_Call is not implemented";
        return eawr::core::Result<auth::ValueList>::failure(diagnostic);
    }).has_value(), "register missing binding");
    expect(scheduler.create_instance(1, "Main.lua").has_value(), "instance created");

    const eawr::sim::InlineExecutor executor;
    auth::ServiceOptions paced;
    paced.host_paced = [](std::uint64_t) { return true; };
    const auto service = [&](const std::vector<auth::ScriptEvent>& events) {
        for (const auto& event : events) expect(scheduler.submit_event(event).has_value(), "submit " + event.name);
        auto report = scheduler.service(executor, paced);
        expect(report.has_value(), "service");
        if (!report) return auth::ServiceReport{};
        for (const auto& command : report.value().commands) session.commands.push_back(text(command));
        for (const auto& diagnostic : report.value().diagnostics) session.diagnostics.push_back(diagnostic.code);
        return std::move(report).value();
    };

    // Attach: Base_Definitions, then the main thread; a paced instance is not pumped.
    auto attach = engine_event(1, 0, auth::ScriptEvent::Kind::assign, "Owner");
    attach.parameter = auth::Value{auth::Handle{object_kind, 42}};
    const auto first = service({engine_event(1, 1, auth::ScriptEvent::Kind::call, "Base_Definitions"),
        engine_event(1, 2, auth::ScriptEvent::Kind::start_thread, "main"), attach,
        engine_event(1, 3, auth::ScriptEvent::Kind::call, "Not_A_Function")});
    expect(global_text(session, "Count") == "0", "a paced instance is not pumped without a pump event");
    expect(global_text(session, "Owner") == "handle(7,42)", "assign sets a global to a handle");
    expect(first.loads.size() == 1 && first.loads[0].instance == 1 && first.loads[0].instructions >= 0,
           "the report carries the instance's instruction load (metered per 128 instructions)");

    service({engine_event(2, 0, auth::ScriptEvent::Kind::pump, {})});
    expect(global_text(session, "Count") == "1", "a pump event pumps the threads once");
    service({});
    expect(global_text(session, "Count") == "1", "no pump event, no pump");

    // A call with a handle argument; methods bind to the handle.
    auto call = engine_event(4, 0, auth::ScriptEvent::Kind::call, "On_Service");
    call.arguments.push_back(auth::Value{auth::Handle{object_kind, 9}});
    service({call});
    expect(global_text(session, "Name") == "object 9", "a handle method receives its handle");
    expect(global_text(session, "Absent") == "nil", "a name no method has reads as nil");
    expect(!session.commands.empty() && contains(session.commands.back(), "order handle(7,9) 3"),
           "a method's command carries the bound handle: " + (session.commands.empty() ? "" : session.commands.back()));

    // Missing engine API: the call stops with EAWR-SCRIPT-0215 and leaves nothing behind.
    const std::size_t commands = session.commands.size();
    service({engine_event(5, 0, auth::ScriptEvent::Kind::call, "Use_Missing")});
    expect(!session.diagnostics.empty() && session.diagnostics.back() == auth::codes::missing_api,
           "a missing engine API is diagnosed with its own code");
    expect(global_text(session, "object_after_missing") == "nil" && session.commands.size() == commands,
           "the missing call stops the script call");

    // A bound method closure kept in a global survives a save and load.
    auto saved = scheduler.save();
    expect(saved.has_value(), "save with a bound method: " + (saved ? std::string() : saved.error().message));
    if (saved) {
        auto hash = scheduler.state_hash();
        expect(scheduler.load(saved.value()).has_value(), "load with a bound method");
        auto reloaded = scheduler.state_hash();
        expect(hash && reloaded && hash.value() == reloaded.value(), "the state hash survives the round trip");
    }
}

} // namespace

int main() {
    engine();
    if (failures != 0) {
        std::cerr << failures << " engine check(s) failed\n";
        return 1;
    }
    std::cout << "lua engine contracts passed\n";
    return 0;
}
