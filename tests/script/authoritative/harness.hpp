#pragma once

// Shared harness of the authoritative Lua tests (#247, #248): value text,
// the test bindings and a scheduler session.

#include "eawr/script/authoritative/scheduler.hpp"
#include "eawr/script/numeric/binary64.hpp"
#include "eawr/sim/world.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace eawr::script::authoritative::test {

namespace auth = eawr::script::authoritative;
namespace b64 = eawr::script::numeric::binary64;
using eawr::script::numeric::LuaNumber;

inline int failures = 0;

inline void expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

inline std::string hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::uppercase;
    stream.width(16);
    stream.fill('0');
    stream << value;
    return stream.str();
}

inline LuaNumber number(std::uint64_t bits) { return LuaNumber::from_repr(bits); }
inline LuaNumber integer(std::int64_t value) { return LuaNumber::from_repr(b64::from_int64(value)); }

// ---- Text form of host values and commands ----

inline std::string text(const auth::Value& value);

inline std::string text(const auth::ValueList& values) {
    std::string out;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) out += ' ';
        out += text(values[index]);
    }
    return out;
}

inline std::string text(const auth::Value& value) {
    return std::visit(
        [](const auto& data) -> std::string {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return "nil";
            } else if constexpr (std::is_same_v<T, bool>) {
                return data ? "true" : "false";
            } else if constexpr (std::is_same_v<T, LuaNumber>) {
                const std::int64_t whole = b64::to_int64_truncate(data.repr);
                if (b64::equal(b64::from_int64(whole), data.repr)) return std::to_string(whole);
                return "0x" + hex(data.repr);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return data;
            } else if constexpr (std::is_same_v<T, auth::Handle>) {
                return "handle(" + std::to_string(data.kind) + "," + std::to_string(data.id) + ")";
            } else {
                return "{" + text(data) + "}";
            }
        },
        value.data
    );
}

inline std::string text(const auth::ScriptCommand& command) {
    const std::string arguments = text(command.arguments);
    return std::to_string(command.issuer) + "#" + std::to_string(command.sequence) + " " + command.verb +
        (arguments.empty() ? "" : " " + arguments);
}

// ---- Harness ----

inline std::int64_t whole(const auth::Value& value) {
    const auto* data = std::get_if<LuaNumber>(&value.data);
    return data == nullptr ? 0 : b64::to_int64_truncate(data->repr);
}

inline auth::ScriptEvent dispatch_event(auth::EventKey key, std::uint64_t target, std::string name, auth::ValueList arguments) {
    auth::ScriptEvent event;
    event.key = key;
    event.target = target;
    event.kind = auth::ScriptEvent::Kind::dispatch;
    event.name = std::move(name);
    event.arguments = std::move(arguments);
    return event;
}

inline void register_test_bindings(auth::ScriptScheduler& scheduler) {
    auto check = [](eawr::core::Result<void> result) { expect(result.has_value(), "register test binding"); };
    check(scheduler.register_binding("Test_Report", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        context.issue_command("report", arguments);
        return eawr::core::Result<auth::ValueList>::success({});
    }));
    check(scheduler.register_binding("Test_Random", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        const std::int64_t bound = arguments.empty() ? 0 : whole(arguments.front());
        const std::uint64_t word = context.random().next_below(static_cast<std::uint64_t>(bound));
        return eawr::core::Result<auth::ValueList>::success(
            {auth::Value::number(integer(static_cast<std::int64_t>(bound == 0 ? (word >> 12) : word)))});
    }));
    check(scheduler.register_binding("Test_Unit", [](auth::BindingContext& context, const auth::ValueList&) {
        return eawr::core::Result<auth::ValueList>::success({auth::Value::number(context.random().next_unit())});
    }));
    check(scheduler.register_binding("Test_Handle", [](auth::BindingContext&, const auth::ValueList& arguments) {
        auth::Handle handle{static_cast<std::uint32_t>(whole(arguments.at(0))), static_cast<std::uint64_t>(whole(arguments.at(1)))};
        return eawr::core::Result<auth::ValueList>::success({auth::Value{handle}});
    }));
    check(scheduler.register_binding("Test_Timer", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        auth::ScriptEvent event = dispatch_event({}, context.instance(), std::get<std::string>(arguments.at(1).data),
                                                 {arguments.size() > 2 ? arguments[2] : auth::Value{}});
        auto started = context.start_timer(std::get<LuaNumber>(arguments.at(0).data), std::move(event));
        if (!started) return eawr::core::Result<auth::ValueList>::failure(started.error());
        return eawr::core::Result<auth::ValueList>::success({});
    }));
    check(scheduler.register_binding("Test_Signal", [](auth::BindingContext& context, const auth::ValueList& arguments) {
        auth::ScriptEvent event;
        event.kind = auth::ScriptEvent::Kind::thread_signal;
        event.target = static_cast<std::uint64_t>(whole(arguments.at(0)));
        event.thread = whole(arguments.at(1));
        event.name = std::get<std::string>(arguments.at(2).data);
        if (arguments.size() > 3) event.parameter = arguments[3];
        auto posted = context.post_event(std::move(event));
        if (!posted) return eawr::core::Result<auth::ValueList>::failure(posted.error());
        return eawr::core::Result<auth::ValueList>::success({});
    }));
}

struct Session {
    std::unique_ptr<auth::ScriptScheduler> scheduler;
    std::vector<std::string> commands;
    std::vector<std::string> diagnostics;
    std::vector<std::uint64_t> removed;
};

inline auth::SessionConfig default_config() {
    auth::SessionConfig config;
    config.seed = 0x5EED;
    config.tick_duration = auth::TickDuration{1, 30};
    config.script_directories = {"Library/"};
    return config;
}

inline Session make_session(const std::map<std::string, std::string>& modules, auth::SessionConfig config = default_config()) {
    auth::ModuleManifest manifest;
    for (const auto& [path, bytes] : modules) expect(manifest.add(path, bytes).has_value(), "manifest add " + path);
    auto created = auth::ScriptScheduler::create(std::move(config), std::move(manifest));
    expect(created.has_value(), "scheduler create");
    Session session;
    session.scheduler = std::make_unique<auth::ScriptScheduler>(std::move(created).value());
    register_test_bindings(*session.scheduler);
    return session;
}

inline void step(Session& session, const eawr::sim::PartitionExecutor& executor, int ticks = 1) {
    for (int index = 0; index < ticks; ++index) {
        auto report = session.scheduler->service(executor);
        expect(report.has_value(), "service: " + (report ? std::string() : report.error().code + " " + report.error().message));
        if (!report) return;
        for (const auto& command : report.value().commands) session.commands.push_back(text(command));
        for (const auto& diagnostic : report.value().diagnostics) {
            session.diagnostics.push_back(diagnostic.code + " " + diagnostic.message);
        }
        for (const std::uint64_t id : report.value().removed_instances) session.removed.push_back(id);
    }
}

// Runs `script` as instance 1 for `ticks` ticks; returns the create error or "".
inline std::string run_script(Session& session, int ticks = 1) {
    auto created = session.scheduler->create_instance(1, "Main.lua");
    if (!created) return created.error().code + " " + created.error().message;
    const eawr::sim::InlineExecutor inline_executor;
    step(session, inline_executor, ticks);
    return "";
}

inline Session script_session(const std::string& script, auth::SessionConfig config = default_config()) {
    return make_session({{"Main.lua", script}}, std::move(config));
}

inline bool contains(const std::string& haystack, std::string_view needle) { return haystack.find(needle) != std::string::npos; }

inline bool any_contains(const std::vector<std::string>& lines, std::string_view needle) {
    for (const std::string& line : lines) {
        if (contains(line, needle)) return true;
    }
    return false;
}

inline std::string joined(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) out += line + "\n";
    return out;
}

} // namespace eawr::script::authoritative::test
