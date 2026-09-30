#pragma once

// Internal state of the authoritative scheduler, shared by scheduler.cpp and
// bindings.cpp. Lua raises errors with longjmp: functions called by Lua keep
// no object with a destructor alive across a call that can raise.

#include "eawr/script/authoritative/scheduler.hpp"
#include "sflua_persist.hpp"
#include "sflua_sandbox.hpp"

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace eawr::script::authoritative::detail {

namespace sf = ::eawr::script::sflua;
using sf::lua_State;

struct ThreadSlot {
    bool live{};
    bool started{};
    lua_State* thread{};
    int thread_reference{};
    int function_reference{};
    int parameter_reference{};   // initial parameter, LUA_NOREF when absent
    int values_reference{};      // ThreadValue table, LUA_NOREF until first Set
    std::deque<int> callbacks;   // registry references; LUA_REFNIL for nil
    std::deque<int> parameters;
};

struct Handler {
    std::uint64_t id{};
    int function_reference{};
};

struct Staging {
    std::vector<ScriptCommand> commands;
    std::vector<ScriptEvent> posts;
    std::vector<ScriptEvent> timers;
    std::vector<ScriptDiagnostic> diagnostics;
};

// Host-side state a failed script call must not leave behind.
struct Mark {
    std::size_t commands{};
    std::size_t posts{};
    std::size_t timers{};
    std::uint64_t draws{};
    std::uint64_t command_sequence{};
    std::uint64_t post_sequence{};
    std::uint64_t timer_sequence{};
};

struct Shared;

struct Instance {
    std::uint64_t id{};
    const Shared* shared{};
    std::unique_ptr<sf::Sandbox> sandbox;
    std::vector<ThreadSlot> slots;
    std::int64_t current_thread{-1};
    bool exit_requested{};
    InstanceOutcome outcome{InstanceOutcome::running};
    std::map<std::string, std::vector<Handler>, std::less<>> handlers;
    std::uint64_t next_handler_id{1};
    std::uint64_t mutation_generation{};
    std::uint32_t registrations{};
    std::uint32_t queued_events{};
    std::uint32_t pending_timers{};

    // The tick being executed (completed tick + 1) and its random stream.
    std::uint64_t tick{};
    std::uint64_t time_tick{}; // completed ticks: GetCurrentTime
    RandomStream random{0, 0, 0};
    std::uint64_t command_sequence{};
    std::uint64_t post_sequence{};
    std::uint64_t timer_sequence{};

    std::vector<ScriptEvent> inbox;
    Staging staging;

    [[nodiscard]] Mark mark() const noexcept;
    void rollback(const Mark& mark);
};

struct BindingEntry {
    std::string name;   // "Global", "Global.member", or "#kind.Method" for a handle method
    Binding binding;
};

// Handle methods are bindings named "#<kind>.<Method>"; they install no global.
inline constexpr char method_binding_prefix = '#';

struct Shared {
    SessionConfig config;
    ModuleManifest manifest;
    std::vector<BindingEntry> bindings;
    // Handle kind -> method name -> binding index.
    std::map<std::uint32_t, std::map<std::string, int, std::less<>>> methods;
};

// The instance running on the calling thread (set with the sandbox scope).
Instance* current_instance() noexcept;

class InstanceScope {
public:
    explicit InstanceScope(Instance& instance) noexcept;
    ~InstanceScope();
    InstanceScope(const InstanceScope&) = delete;
    InstanceScope& operator=(const InstanceScope&) = delete;

private:
    Instance* previous_;
    sf::Sandbox::Scope sandbox_scope_;
};

[[nodiscard]] inline core::Diagnostic make_error(std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    return diagnostic;
}

// scheduler.cpp: unprotected Lua errors (allocation failure outside a
// protected call) abort the session instead of exiting the process.
int abort_session(lua_State* state);

// bindings.cpp
[[nodiscard]] bool is_infrastructure_name(std::string_view name);
// The infrastructure's C functions with their stable save names.
[[nodiscard]] std::vector<sf::CFunctionEntry> host_functions();
void install_bindings(Instance& instance);
void push_value(lua_State* state, const Value& value);
void add_diagnostic(Instance& instance, std::string_view code, std::string message);
void end_slot(Instance& instance, std::size_t slot);
[[nodiscard]] bool queue_limit_reached(const Instance& instance) noexcept;

} // namespace eawr::script::authoritative::detail

namespace eawr::script::authoritative {

struct ScriptScheduler::Impl {
    std::unique_ptr<detail::Shared> shared;
    std::map<std::uint64_t, std::unique_ptr<detail::Instance>> instances;
    std::map<EventKey, ScriptEvent> pending;
    std::uint64_t completed_tick{};
    bool aborted{};

    [[nodiscard]] core::Result<void> check_usable() const {
        if (aborted) return core::Result<void>::failure(detail::make_error(codes::session_abort, "the script session was aborted"));
        return core::Result<void>::success();
    }
};

} // namespace eawr::script::authoritative
