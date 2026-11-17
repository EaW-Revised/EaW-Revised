// Infrastructure bindings of the authoritative scheduler and the generic host
// binding trampoline (docs/lua-sandbox.md). Thread, event and loading rules
// follow the L-rules in docs/behaviour/lua-script-model.md.
//
// Lua raises errors with longjmp. Each lua_CFunction here does its C++ work in
// a body function whose objects are destroyed before the wrapper raises.

#include "scheduler_internal.hpp"

#include "eawr/script/numeric/decimal.hpp"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::script::authoritative::detail {
namespace {

using namespace ::eawr::script::sflua;

constexpr char binding_metatable_guard[] = "EAWR binding";
constexpr int max_value_depth = 16;
constexpr int error_raised = -1;
constexpr int quota_raised = -2;

thread_local Instance* running_instance = nullptr;

Instance& instance_of(lua_State* state) {
    Instance* instance = running_instance;
    if (instance == nullptr) luaL_error(state, "EAWR-SCRIPT-0203: binding called outside its script instance");
    return *instance;
}

int finish(lua_State* state, Instance& instance, int result) {
    if (result == quota_raised) instance.sandbox->raise_fault(state, sf::SandboxFault::logical_quota);
    // A memory quota fault the body recorded without raising.
    if (instance.sandbox->fault() != sf::SandboxFault::none) instance.sandbox->raise_fault(state, instance.sandbox->fault());
    if (result == error_raised) return lua_error(state);
    return result;
}

// Host-side work of a value that crosses the binding boundary, in deterministic units charged to the instance's
// budget: one per value plus one per byte of a string, lists adding their entries'. The same on every target.
::std::uint64_t list_units(const ValueList& values) noexcept;
::std::uint64_t value_units(const Value& value) noexcept {
    if (const auto* text = ::std::get_if<::std::string>(&value.data)) return 1 + text->size();
    if (const auto* list = ::std::get_if<ValueList>(&value.data)) return 1 + list_units(*list);
    return 1;
}
::std::uint64_t list_units(const ValueList& values) noexcept {
    ::std::uint64_t total = 0;
    for (const Value& value : values) total += value_units(value);
    return total;
}

bool read_value(Instance& instance, lua_State* state, int index, int depth, Value& out, ::std::string& error) {
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        out.data = ::std::monostate{};
        return true;
    case LUA_TBOOLEAN:
        out.data = lua_toboolean(state, index) != 0;
        return true;
    case LUA_TNUMBER:
        out.data = lua_tonumber(state, index);
        return true;
    case LUA_TSTRING:
        out.data = ::std::string(lua_tostring(state, index), lua_strlen(state, index));
        return true;
    case LUA_TUSERDATA: {
        sf::HostHandle handle;
        if (!instance.sandbox->to_handle(state, index, handle)) break;
        out.data = Handle{handle.kind, handle.id};
        return true;
    }
    case LUA_TTABLE: {
        if (depth >= max_value_depth) {
            error = "table nesting too deep";
            return false;
        }
        if (index < 0) index = lua_gettop(state) + index + 1;
        // A list is exactly the keys 1..n; counting is independent of traversal order.
        int count = 0;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            ++count;
            lua_pop(state, 1);
        }
        bool sequential = true;
        for (int position = 1; position <= count && sequential; ++position) {
            lua_rawgeti(state, index, position);
            sequential = !lua_isnil(state, -1);
            lua_pop(state, 1);
        }
        if (!sequential) {
            // SOS-17/WHE-68: an engine call receives a table's values; a set of host
            // handles (keyed by anything, e.g. an object-keyed sample) crosses as its
            // handle values in ascending (kind, id) order, independent of traversal.
            ::std::vector<Handle> handles;
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                sf::HostHandle handle;
                const bool read = instance.sandbox->to_handle(state, -1, handle);
                lua_pop(state, 1);
                if (!read) {
                    lua_pop(state, 1);
                    error = "only sequential tables or sets of handles cross the binding boundary";
                    return false;
                }
                handles.push_back(Handle{handle.kind, handle.id});
            }
            ::std::sort(handles.begin(), handles.end(), [](const Handle& left, const Handle& right) {
                return ::std::pair{left.kind, left.id} < ::std::pair{right.kind, right.id};
            });
            ::std::vector<Value> list;
            for (const Handle& handle : handles) list.push_back(Value{handle});
            out.data = ::std::move(list);
            return true;
        }
        ::std::vector<Value> list(static_cast<::std::size_t>(count));
        for (int position = 1; position <= count; ++position) {
            lua_rawgeti(state, index, position);
            const bool present = !lua_isnil(state, -1);
            const bool read = present && read_value(instance, state, -1, depth + 1, list[position - 1], error);
            lua_pop(state, 1);
            if (!present) {
                error = "only sequential tables cross the binding boundary";
                return false;
            }
            if (!read) return false;
        }
        out.data = ::std::move(list);
        return true;
    }
    default:
        break;
    }
    error = ::std::string(lua_typename(state, lua_type(state, index))) + " values do not cross the binding boundary";
    return false;
}

class InstanceContext final : public BindingContext {
public:
    explicit InstanceContext(Instance& instance, lua_State* state, bool local_handler) noexcept
        : instance_(instance), state_(state), local_handler_(local_handler) {}

    [[nodiscard]] ::std::uint64_t instance() const noexcept override { return instance_.id; }
    [[nodiscard]] ::std::uint64_t tick() const noexcept override { return instance_.tick; }
    [[nodiscard]] ::std::int64_t thread() const noexcept override { return instance_.current_thread; }
    [[nodiscard]] RandomStream& random() noexcept override { return instance_.random; }
    void request_exit() noexcept override { instance_.exit_requested = true; }
    [[nodiscard]] ::std::uint64_t command_sequence() const noexcept override { return instance_.command_sequence; }

    void charge_host_work(const ::std::uint64_t units) noexcept override { instance_.sandbox->charge_host_work(units); }

    void issue_command(::std::string verb, ValueList arguments) override {
        if (instance_.staging.commands.size() >= instance_.shared->config.quotas.staged_commands) {
            quota_exhausted = true;
            return;
        }
        instance_.sandbox->charge_host_work(1 + verb.size() + list_units(arguments));
        instance_.staging.commands.push_back(ScriptCommand{
            instance_.tick, instance_.id, instance_.command_sequence++, ::std::move(verb), ::std::move(arguments)});
    }

    [[nodiscard]] ::std::span<const ScriptCommand> staged_commands() const noexcept override {
        return instance_.staging.commands;
    }

    [[nodiscard]] ::std::optional<::std::optional<Value>> staged_global_value(const ::std::string_view name) const override {
        for (auto at = instance_.staging.commands.rbegin(); at != instance_.staging.commands.rend(); ++at) {
            if (at->verb != global_value_command || at->arguments.empty()) continue;
            const auto* key = ::std::get_if<::std::string>(&at->arguments[0].data);
            if (!key || *key != name) continue;
            if (at->arguments.size() != 2) continue; // a refused table write stages the name alone: the registry stays as it was
            if (::std::holds_alternative<::std::monostate>(at->arguments[1].data)) return ::std::optional<Value>{};
            return ::std::optional<Value>{at->arguments[1]};
        }
        return ::std::nullopt;
    }

    core::Result<ValueList> apply_immediate(::std::string_view action, const ValueList& arguments) override {
        if (!instance_.immediate) return core::Result<ValueList>::failure(
            make_error(codes::missing_api, "immediate effects require an ordered native shadow"));
        // The first action takes this service's one private copy of the shadow; later actions apply in place. A
        // refused action may have changed several native dependencies before it failed (and a pcall can catch it),
        // so the working state is then rebuilt from the logged actions before it; each copy is charged by what it
        // copies and each replay by one unit (Instance::rebuild_immediate).
        if (instance_.immediate == instance_.immediate_base) {
            instance_.sandbox->charge_host_work(instance_.immediate->clone_units());
            auto own = instance_.immediate->clone();
            if (!own) throw ::std::runtime_error("immediate effect clone is null");
            instance_.immediate = ::std::move(own);
        }
        auto result = instance_.immediate->apply(action, arguments);
        if (result) {
            instance_.immediate_log.push_back({::std::string(action), arguments});
            instance_.immediate_changed = true;
        } else {
            instance_.rebuild_immediate(instance_.immediate_log.size());
        }
        return result;
    }

    core::Result<ValueList> query_immediate(::std::string_view name, const ValueList& arguments) const override {
        if (!instance_.immediate) return core::Result<ValueList>::failure(
            make_error(codes::missing_api, "immediate queries require an ordered native shadow"));
        auto result = instance_.immediate->query(name, arguments);
        // A query that scans host state pays for what it scanned, beside what it returns.
        if (const auto scanned = instance_.immediate->take_query_work(); scanned != 0) instance_.sandbox->charge_host_work(scanned);
        return result;
    }

    core::Result<void> post_event(ScriptEvent event) override {
        if (event.kind == ScriptEvent::Kind::thread_signal && event.thread < 0) {
            return core::Result<void>::failure(make_error(codes::invalid_request, "thread signal needs a thread slot"));
        }
        if (instance_.staging.posts.size() >= instance_.shared->config.quotas.staged_posts) {
            quota_exhausted = true;
            return core::Result<void>::success();
        }
        {
            ::std::uint64_t units = 1 + event.name.size() + list_units(event.arguments);
            if (event.parameter) units += value_units(*event.parameter);
            instance_.sandbox->charge_host_work(units);
        }
        event.key = EventKey{instance_.tick + 1, producer_script_post, instance_.id, instance_.post_sequence++};
        instance_.staging.posts.push_back(::std::move(event));
        return core::Result<void>::success();
    }

    core::Result<void> start_timer(const numeric::LuaNumber seconds, ScriptEvent event) override {
        const auto ticks = ticks_until(seconds, instance_.shared->config.tick_duration);
        if (!ticks) {
            return core::Result<void>::failure(
                make_error(codes::invalid_request, "timer duration must be finite and not negative"));
        }
        if (instance_.pending_timers >= instance_.shared->config.quotas.pending_timers) {
            quota_exhausted = true;
            return core::Result<void>::success();
        }
        const auto query_clock = instance_.shared->config.query_clock;
        const auto origin = query_clock ? instance_.time_tick : instance_.tick;
        if (query_clock && *ticks >= max_script_tick - origin) {
            return core::Result<void>::failure(make_error(codes::invalid_request, "query timer deadline exceeds clock format"));
        }
        event.target = instance_.id;
        event.key = EventKey{origin + *ticks, producer_script_timer, instance_.id, instance_.timer_sequence++};
        instance_.staging.timers.push_back(::std::move(event));
        ++instance_.pending_timers;
        return core::Result<void>::success();
    }

    void report(::std::string_view code, ::std::string message) override {
        if (instance_.staging.diagnostics.size() >= instance_.shared->config.quotas.staged_diagnostics) {
            quota_exhausted = true;
            return;
        }
        instance_.sandbox->charge_host_work(1 + code.size() + message.size());
        add_diagnostic(instance_, code, ::std::move(message));
    }

    core::Result<void> register_proximity(ProximityQuery query) override {
        if (!local_handler_) return bad_handler();
        if (!lua_isfunction(state_, 2)) return core::Result<void>::failure(
            make_error(codes::invalid_request, "proximity handler must be a function"));
        if (instance_.registrations >= instance_.shared->config.quotas.registrations) {
            quota_exhausted = true;
            return core::Result<void>::success();
        }
        lua_pushvalue(state_, 2);
        const int reference = luaL_ref(state_, LUA_REGISTRYINDEX);
        auto& list = instance_.proximities[{query.object.kind, query.object.id}];
        list.handlers.push_back({{instance_.next_handler_id++, reference}, ::std::move(query)});
        ++list.generation;
        ++instance_.registrations;
        return core::Result<void>::success();
    }

    core::Result<void> cancel_proximity(Handle object) override {
        if (!local_handler_) return bad_handler();
        if (!lua_isfunction(state_, 2)) return core::Result<void>::failure(
            make_error(codes::invalid_request, "proximity handler must be a function"));
        const auto found = instance_.proximities.find({object.kind, object.id});
        if (found == instance_.proximities.end()) return core::Result<void>::success();
        auto& list = found->second;
        for (auto it = list.handlers.begin(); it != list.handlers.end();) {
            lua_rawgeti(state_, LUA_REGISTRYINDEX, it->handler.function_reference);
            const bool matches = lua_rawequal(state_, 2, -1) != 0;
            lua_pop(state_, 1);
            if (matches) {
                luaL_unref(state_, LUA_REGISTRYINDEX, it->handler.function_reference);
                it = list.handlers.erase(it);
                --instance_.registrations;
                ++list.generation;
            } else ++it;
        }
        return core::Result<void>::success();
    }

    core::Result<void> service_proximities(Handle object, const ProximityCandidates& candidates) override {
        const auto found = instance_.proximities.find({object.kind, object.id});
        if (found == instance_.proximities.end()) return core::Result<void>::success();
        auto& list = found->second;
        ::std::vector<::std::uint64_t> completed;
        ::std::uint64_t invoked = 0;
        while (!instance_.exit_requested) {
            const auto it = ::std::find_if(list.handlers.begin(), list.handlers.end(), [&](const ProximityHandler& row) {
                return ::std::find(completed.begin(), completed.end(), row.handler.id) == completed.end();
            });
            if (it == list.handlers.end()) break;
            // Copy before calling Lua: cancellation/registration may invalidate the row.
            const ProximityHandler row = *it;
            const auto generation = list.generation;
            for (const Handle candidate : candidates(row.query)) {
                if (++invoked > instance_.shared->config.quotas.queued_events) {
                    quota_exhausted = true;
                    return core::Result<void>::success();
                }
                const Mark mark = instance_.mark();
                const int top = lua_gettop(state_);
                lua_rawgeti(state_, LUA_REGISTRYINDEX, row.handler.function_reference);
                push_value(state_, Value{object});
                push_value(state_, Value{candidate});
                // L-32: direct call at Service_Wrapper, including from a coroutine.
                const int status = lua_pcall(state_, 2, 0, 0);
                if (status != 0) {
                    const char* message = lua_tostring(state_, -1);
                    const ::std::string text = message != nullptr ? message : "proximity callback failed";
                    add_lua_error(instance_, text.find(codes::missing_api) != ::std::string::npos ? codes::missing_api : codes::script_error, text);
                    instance_.rollback(mark);
                }
                lua_settop(state_, top);
                if (instance_.outcome == InstanceOutcome::faulted) return core::Result<void>::success();
                if (instance_.sandbox->fault() != sf::SandboxFault::none) return core::Result<void>::success();
                if (generation != list.generation || instance_.exit_requested) break;
            }
            if (generation == list.generation) completed.push_back(row.handler.id);
        }
        return core::Result<void>::success();
    }

    bool quota_exhausted{};

private:
    static core::Result<void> bad_handler() {
        return core::Result<void>::failure(make_error(codes::invalid_request,
            "proximity handler must be a function in the same script instance; cross-instance functions are unsupported"));
    }
    Instance& instance_;
    lua_State* state_;
    bool local_handler_;
};

int generic_body(lua_State* state, bool drop_self, bool bound = false) {
    Instance& instance = *running_instance;
    const auto index = static_cast<::std::size_t>(static_cast<int>(lua_tonumber(state, lua_upvalueindex(1))));
    if (drop_self) lua_remove(state, 1);
    // A bound method receives its handle as the first argument.
    if (bound) {
        lua_pushvalue(state, lua_upvalueindex(2));
        lua_insert(state, 1);
    }
    const BindingEntry& entry = instance.shared->bindings[index];
    const bool local_handler = bound && (entry.name.ends_with(".Event_Object_In_Range") ||
        entry.name.ends_with(".Cancel_Event_Object_In_Range"));
    ValueList arguments(static_cast<::std::size_t>(lua_gettop(state)));
    ::std::string error;
    for (int position = 1; position <= lua_gettop(state); ++position) {
        // The handler never enters Value: the context retains/compares it in
        // this Lua state only. Every other binding still rejects functions.
        if (local_handler && position == 2 && lua_isfunction(state, position)) continue;
        // L-54: these numeric APIs diagnose non-numbers but still draw. Do not
        // reject or traverse an arbitrary Lua graph before the binding sees it.
        if ((entry.name == "GameRandom" || entry.name == "GameRandom.Get_Float" || entry.name == "GameRandom.Free_Random") &&
            lua_type(state, position) != LUA_TNUMBER) continue;
        if (!read_value(instance, state, position, 0, arguments[position - 1], error)) {
            const ::std::string message = ::std::string(codes::invalid_request) + ": " + entry.name + " argument " +
                ::std::to_string(position) + ": " + error;
            lua_pushlstring(state, message.data(), message.size());
            return error_raised;
        }
    }
    // The copy just made is host work too: charged before the binding runs, so a stream of large calls ends at the
    // budget, and so do the results a binding hands back.
    instance.sandbox->charge_host_work(list_units(arguments));
    InstanceContext context(instance, state, local_handler);
    auto results = entry.binding(context, arguments);
    if (context.quota_exhausted) return quota_raised;
    if (results) instance.sandbox->charge_host_work(list_units(results.value()));
    if (!results) {
        const ::std::string message = results.error().code + ": " + entry.name + ": " + results.error().message;
        lua_pushlstring(state, message.data(), message.size());
        return error_raised;
    }
    lua_settop(state, 0);
    for (const Value& value : results.value()) push_value(state, value);
    return static_cast<int>(results.value().size());
}

// The body keeps C++ objects alive across Lua API calls, so a memory quota
// fault inside it is only recorded; finish raises it.
int generic_call_with(lua_State* state, bool drop_self, bool bound = false) {
    Instance& instance = instance_of(state);
    instance.sandbox->begin_host_call();
    const int result = generic_body(state, drop_self, bound);
    instance.sandbox->end_host_call();
    return finish(state, instance, result);
}

int generic_call(lua_State* state) { return generic_call_with(state, false); }

int generic_call_object(lua_State* state) { return generic_call_with(state, true); }

// A handle method: upvalues are the binding index and the handle.
int bound_method(lua_State* state) { return generic_call_with(state, false, true); }

// An audit lookup returns a callable, but records only actual calls, not probes.
int audit_missing_call(lua_State* state) {
    Instance& instance = instance_of(state);
    instance.shared->config.missing_call_sink->called(instance.id,
        ::std::string_view(lua_tostring(state, lua_upvalueindex(1)), lua_strlen(state, lua_upvalueindex(1))));
    lua_pushnil(state);
    return 1;
}

int audit_missing_index(lua_State* state) {
    if (lua_type(state, 2) != LUA_TSTRING) { lua_pushnil(state); return 1; }
    const auto name = ::std::string_view(lua_tostring(state, 2), lua_strlen(state, 2));
    if (!instance_of(state).shared->config.missing_call_sink->stub_global(name)) {
        lua_pushnil(state); return 1;
    }
    lua_pushvalue(state, 2);
    lua_pushcclosure(state, audit_missing_call, 1);
    return 1;
}

// `__index` of every handle: a method of the handle's kind, bound to it, or nil.
int handle_index_body(lua_State* state) {
    Instance& instance = *running_instance;
    sf::HostHandle handle;
    if (!instance.sandbox->to_handle(state, 1, handle) || lua_type(state, 2) != LUA_TSTRING) return 0;
    const auto kind = instance.shared->methods.find(handle.kind);
    if (kind == instance.shared->methods.end()) return 0;
    const auto method = kind->second.find(::std::string_view(lua_tostring(state, 2), lua_strlen(state, 2)));
    if (method == kind->second.end()) return 0;
    lua_pushnumber(state, lua_Number(method->second));
    lua_pushvalue(state, 1);
    lua_pushcclosure(state, bound_method, 2);
    return 1;
}

int handle_index(lua_State* state) {
    instance_of(state);
    const int results = handle_index_body(state);
    if (results == 0) {
        if (running_instance->shared->config.missing_call_sink && lua_type(state, 2) == LUA_TSTRING) {
            sf::HostHandle handle;
            if (running_instance->sandbox->to_handle(state, 1, handle)) {
                lua_pushfstring(state, "handle[%d].%s", static_cast<int>(handle.kind), lua_tostring(state, 2));
                lua_pushcclosure(state, audit_missing_call, 1);
                return 1;
            }
        }
        lua_pushnil(state);
    }
    return 1;
}

int object_not_callable(lua_State* state) {
    return luaL_error(state, "attempt to call binding object `%s'", lua_tostring(state, lua_upvalueindex(1)));
}

int audit_object_index(lua_State* state) {
    lua_pushvalue(state, 2);
    lua_rawget(state, lua_upvalueindex(1));
    if (!lua_isnil(state, -1)) return 1;
    lua_pop(state, 1);
    if (lua_type(state, 2) != LUA_TSTRING) { lua_pushnil(state); return 1; }
    lua_pushfstring(state, "%s.%s", lua_tostring(state, lua_upvalueindex(2)), lua_tostring(state, 2));
    lua_pushcclosure(state, audit_missing_call, 1);
    return 1;
}

// ---- Threads (L-20 to L-26) ----

int create_thread(lua_State* state) {
    Instance& instance = instance_of(state);
    lua_remove(state, 1); // the Create_Thread object
    if (lua_type(state, 1) != LUA_TSTRING) return luaL_error(state, "Create_Thread expects a global function name");
    if (instance.slots.size() >= instance.shared->config.quotas.thread_slots) {
        instance.sandbox->raise_fault(state, sf::SandboxFault::logical_quota);
    }
    const bool has_parameter = lua_gettop(state) >= 2;
    lua_settop(state, 2);
    lua_pushvalue(state, 1);
    lua_gettable(state, LUA_GLOBALSINDEX);
    const int function_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    int parameter_reference = LUA_NOREF;
    if (has_parameter) {
        lua_pushvalue(state, 2);
        parameter_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    }
    lua_State* thread = lua_newthread(state);
    const int thread_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    ThreadSlot slot;
    slot.live = true;
    slot.thread = thread;
    slot.thread_reference = thread_reference;
    slot.function_reference = function_reference;
    slot.parameter_reference = parameter_reference;
    slot.values_reference = LUA_NOREF;
    instance.slots.push_back(::std::move(slot));
    lua_pushnumber(state, static_cast<lua_Number>(static_cast<int>(instance.slots.size() - 1)));
    return 1;
}

bool slot_argument(lua_State* state, const Instance& instance, ::std::size_t& slot) {
    if (lua_type(state, 1) != LUA_TNUMBER) return false;
    const lua_Number raw = lua_tonumber(state, 1);
    if (raw < lua_Number(0) || !(raw < lua_Number(static_cast<int>(instance.slots.size())))) return false;
    const int whole = static_cast<int>(raw);
    if (!(lua_Number(whole) == raw)) return false;
    slot = static_cast<::std::size_t>(whole);
    return true;
}

int kill_thread(lua_State* state) {
    Instance& instance = instance_of(state);
    ::std::size_t slot = 0;
    if (!slot_argument(state, instance, slot)) return 0;
    if (instance.current_thread == static_cast<::std::int64_t>(slot) || !instance.slots[slot].live) return 0;
    end_slot(instance, slot);
    return 0;
}

int kill_all_threads(lua_State* state) {
    Instance& instance = instance_of(state);
    for (::std::size_t slot = 0; slot < instance.slots.size(); ++slot) {
        if (instance.current_thread != static_cast<::std::int64_t>(slot) && instance.slots[slot].live) end_slot(instance, slot);
    }
    return 0;
}

int get_thread_id(lua_State* state) {
    Instance& instance = instance_of(state);
    lua_pushnumber(state, static_cast<lua_Number>(static_cast<int>(instance.current_thread)));
    return 1;
}

int script_exit(lua_State* state) {
    instance_of(state).exit_requested = true;
    return 0;
}

// ---- Events (L-30 to L-32) ----

ThreadSlot* current_slot(Instance& instance) {
    if (instance.current_thread < 0) return nullptr;
    ThreadSlot& slot = instance.slots[static_cast<::std::size_t>(instance.current_thread)];
    return slot.live ? &slot : nullptr;
}

int pop_event(lua_State* state, bool callback) {
    Instance& instance = instance_of(state);
    ThreadSlot* slot = current_slot(instance);
    if (slot == nullptr) return 0;
    ::std::deque<int>& queue = callback ? slot->callbacks : slot->parameters;
    if (queue.empty()) return 0;
    const int reference = queue.front();
    queue.pop_front();
    --instance.queued_events;
    if (reference == LUA_REFNIL) return 0;
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    if (lua_isnil(state, -1)) return 0;
    return 1;
}

int get_event(lua_State* state) { return pop_event(state, true); }
int get_event_params(lua_State* state) { return pop_event(state, false); }

int get_event_reset(lua_State* state) {
    Instance& instance = instance_of(state);
    for (ThreadSlot& slot : instance.slots) {
        for (::std::deque<int>* queue : {&slot.callbacks, &slot.parameters}) {
            for (const int reference : *queue) luaL_unref(state, LUA_REGISTRYINDEX, reference);
            instance.queued_events -= static_cast<::std::uint32_t>(queue->size());
            queue->clear();
        }
    }
    return 0;
}

int register_event(lua_State* state) {
    Instance& instance = instance_of(state);
    if (lua_type(state, 1) != LUA_TSTRING || !lua_isfunction(state, 2)) {
        return luaL_error(state, "Register_Event expects event name and function");
    }
    if (instance.registrations >= instance.shared->config.quotas.registrations) {
        instance.sandbox->raise_fault(state, sf::SandboxFault::logical_quota);
    }
    lua_pushvalue(state, 2);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    const ::std::string name(lua_tostring(state, 1), lua_strlen(state, 1));
    instance.handlers[name].push_back(Handler{instance.next_handler_id++, reference});
    ++instance.registrations;
    ++instance.mutation_generation;
    return 0;
}

int cancel_event(lua_State* state) {
    Instance& instance = instance_of(state);
    if (lua_type(state, 1) != LUA_TSTRING || !lua_isfunction(state, 2)) return 0;
    const auto found = instance.handlers.find(::std::string_view(lua_tostring(state, 1), lua_strlen(state, 1)));
    if (found == instance.handlers.end()) return 0;
    bool changed = false;
    auto& handlers = found->second;
    for (auto iterator = handlers.begin(); iterator != handlers.end();) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, iterator->function_reference);
        const bool equal = lua_rawequal(state, -1, 2) != 0;
        lua_pop(state, 1);
        if (equal) {
            luaL_unref(state, LUA_REGISTRYINDEX, iterator->function_reference);
            iterator = handlers.erase(iterator);
            --instance.registrations;
            changed = true;
        } else {
            ++iterator;
        }
    }
    if (changed) ++instance.mutation_generation;
    return 0;
}

// ---- Thread values ----

int thread_value_body(lua_State* state, Instance& instance) {
    lua_remove(state, 1); // the ThreadValue object
    if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TSTRING) {
        add_diagnostic(instance, codes::invalid_request, "ThreadValue expects one string");
        return 0;
    }
    ThreadSlot* slot = current_slot(instance);
    if (slot == nullptr || slot->values_reference == LUA_NOREF) return 0;
    lua_rawgeti(state, LUA_REGISTRYINDEX, slot->values_reference);
    lua_pushvalue(state, 1);
    lua_rawget(state, -2);
    return lua_isnil(state, -1) ? 0 : 1;
}

int thread_value(lua_State* state) { return thread_value_body(state, instance_of(state)); }

int thread_value_set_body(lua_State* state, Instance& instance) {
    if (lua_gettop(state) != 2 || lua_type(state, 1) != LUA_TSTRING) {
        add_diagnostic(instance, codes::invalid_request, "ThreadValue.Set expects a string and a value");
        return 0;
    }
    ThreadSlot* slot = current_slot(instance);
    if (slot == nullptr) return 0;
    if (slot->values_reference == LUA_NOREF) {
        lua_newtable(state);
        slot->values_reference = luaL_ref(state, LUA_REGISTRYINDEX);
    }
    lua_rawgeti(state, LUA_REGISTRYINDEX, slot->values_reference);
    lua_pushvalue(state, 1);
    lua_pushvalue(state, 2);
    lua_rawset(state, -3);
    return 0;
}

int thread_value_set(lua_State* state) { return thread_value_set_body(state, instance_of(state)); }

int thread_value_reset(lua_State* state) {
    Instance& instance = instance_of(state);
    for (ThreadSlot& slot : instance.slots) {
        if (slot.values_reference != LUA_NOREF) luaL_unref(state, LUA_REGISTRYINDEX, slot.values_reference);
        slot.values_reference = LUA_NOREF;
    }
    return 0;
}

// ---- print (policy v4) ----

// print(...): the arguments' text goes to the script log (ServiceReport::log) and nowhere else. It never calls
// `tostring' (that would assign object identities), never creates a Lua value (so it moves neither the logical memory
// account nor any saved state), and never faults: past the per-service cap or the flood guard the line is dropped.
// References print as their type name. The text costs one budget unit per byte, and a call is staged with the
// service, so a rolled-back call drops its output.
constexpr ::std::size_t max_print_line = 512;
constexpr ::std::size_t max_print_sites = 256;

void append_print_text(::std::string& line, ::std::uint64_t& bytes, const char* text, ::std::size_t length) {
    bytes += length;
    if (line.size() < max_print_line) line.append(text, ::std::min(length, max_print_line - line.size()));
}

void log_print(Instance& instance, ::std::string line) {
    if (instance.staging.log.size() >= instance.shared->config.quotas.staged_diagnostics) return;
    ::std::string key = line;
    const bool overflow = instance.print_sites.size() >= max_print_sites && !instance.print_sites.contains(key);
    if (overflow) key = "\x01overflow"; // a control-character prefix: no printed line can take this key
    const auto count = ++instance.print_sites[key];
    if (count > 3 && (count & (count - 1)) != 0) return;
    // The overflow bucket counts distinct lines together, so its count says nothing about this line.
    if (count > 1 && !overflow) line += " (occurrence " + ::std::to_string(count) + ")";
    instance.staging.log.push_back(
        ScriptDiagnostic{::std::string(codes::script_log), ::std::move(line), instance.tick, instance.id, instance.current_thread});
}

// Builds the line; no Lua call that can raise runs while its objects are alive.
void print_body(Instance& instance, lua_State* state) {
    const int count = lua_gettop(state);
    ::std::string line;
    ::std::uint64_t bytes = 1;
    for (int index = 1; index <= count; ++index) {
        if (index > 1) append_print_text(line, bytes, "\t", 1);
        switch (lua_type(state, index)) {
        case LUA_TNIL:
            append_print_text(line, bytes, "nil", 3);
            break;
        case LUA_TBOOLEAN:
            if (lua_toboolean(state, index) != 0) append_print_text(line, bytes, "true", 4);
            else append_print_text(line, bytes, "false", 5);
            break;
        case LUA_TNUMBER: {
            const ::std::string number = numeric::decimal::format_lua_number(lua_tonumber(state, index).repr);
            append_print_text(line, bytes, number.data(), number.size());
            break;
        }
        case LUA_TSTRING:
            append_print_text(line, bytes, lua_tostring(state, index), lua_strlen(state, index));
            break;
        default: {
            const char* type = lua_typename(state, lua_type(state, index));
            append_print_text(line, bytes, type, ::std::char_traits<char>::length(type));
            break;
        }
        }
    }
    if (bytes - 1 > line.size()) line += "...";
    instance.sandbox->charge_host_work(bytes);
    log_print(instance, ::std::move(line));
}

int print_to_log(lua_State* state) {
    Instance& instance = instance_of(state);
    print_body(instance, state);
    // Only the budget can fault here: raised once the line's objects are gone.
    return finish(state, instance, 0);
}

// ---- Time ----

int get_current_time(lua_State* state) {
    Instance& instance = instance_of(state);
    lua_pushnumber(state, time_at_tick(instance.time_tick, instance.shared->config.tick_duration));
    return 1;
}

int get_current_frame(lua_State* state) {
    Instance& instance = instance_of(state);
    lua_pushnumber(state, lua_Number(instance.time_tick));
    return 1;
}

// ---- Modules (L-03, L-03a) ----

// Finds the first manifest module for the request; fills the chunk name.
const ::std::string* find_module(const Instance& instance, ::std::string_view request, char* chunk_name, ::std::size_t capacity) {
    ::std::vector<::std::string> patterns{"./?.lua", "./?.lc"};
    for (const ::std::string& directory : instance.shared->config.script_directories) {
        patterns.push_back(directory + "?.lua");
        patterns.push_back(directory + "?.lc");
    }
    for (const ::std::string& pattern : patterns) {
        ::std::string candidate;
        for (const char character : pattern) {
            if (character == '?') {
                candidate.append(request);
            } else {
                candidate.push_back(character);
            }
        }
        const ::std::string* bytes = instance.shared->manifest.find(candidate);
        if (bytes == nullptr) continue;
        const ::std::string name = "@" + *ModuleManifest::normalise(candidate);
        const ::std::size_t length = ::std::min(name.size(), capacity - 1);
        name.copy(chunk_name, length);
        chunk_name[length] = '\0';
        return bytes;
    }
    return nullptr;
}

int require_module(lua_State* state) {
    Instance& instance = instance_of(state);
    luaL_checktype(state, 1, LUA_TSTRING);
    lua_settop(state, 1);
    lua_pushliteral(state, "_LOADED");
    lua_rawget(state, LUA_GLOBALSINDEX);
    if (!lua_istable(state, 2)) return luaL_error(state, "`_LOADED' is not a table");
    lua_pushvalue(state, 1);
    lua_rawget(state, 2);
    if (lua_toboolean(state, -1)) return 1;
    lua_pop(state, 1);
    char chunk_name[512];
    const ::std::string* bytes = find_module(
        instance, ::std::string_view(lua_tostring(state, 1), lua_strlen(state, 1)), chunk_name, sizeof(chunk_name));
    if (bytes == nullptr) {
        return luaL_error(state, "EAWR-SCRIPT-0207: module `%s' is not in the session manifest", lua_tostring(state, 1));
    }
    const int status = instance.sandbox->load_source(*bytes, chunk_name);
    if (status == LUA_ERRSYNTAX && instance.tracking_initial_syntax && (bytes->empty() || bytes->front() != '\x1b')) {
        const char* message = lua_tostring(instance.sandbox->state(), -1);
        if (message) instance.initial_syntax_message = message;
    }
    // Compilation records prototypes on the sandbox's main stack. A lazy
    // require may run inside a coroutine: transfer its chunk (or error) before
    // using the caller's stack, which currently ends with the module cache.
    lua_State* main = instance.sandbox->state();
    if (state != main) lua_xmove(main, state, 1);
    if (status != 0) return lua_error(state);
    // _REQUIREDNAME holds the module name while it runs (L-03a), as upstream.
    lua_pushliteral(state, "_REQUIREDNAME");
    lua_gettable(state, LUA_GLOBALSINDEX);
    lua_insert(state, -2);
    lua_pushliteral(state, "_REQUIREDNAME");
    lua_pushvalue(state, 1);
    lua_settable(state, LUA_GLOBALSINDEX);
    lua_call(state, 0, 1);
    lua_pushliteral(state, "_REQUIREDNAME");
    lua_pushvalue(state, -3);
    lua_settable(state, LUA_GLOBALSINDEX);
    lua_remove(state, -2);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushboolean(state, 1);
    }
    lua_pushvalue(state, 1);
    lua_pushvalue(state, -2);
    lua_rawset(state, 2);
    return 1;
}

// ---- Installation ----

struct Member {
    ::std::string name;
    lua_CFunction function{};
    int generic{-1};
};

struct ObjectSpec {
    ::std::string name;
    lua_CFunction call{};   // receives the object first
    int generic_call{-1};
    ::std::vector<Member> members;
};

void push_member_function(lua_State* state, lua_CFunction function, int generic, bool drop_self) {
    if (generic >= 0) {
        lua_pushnumber(state, lua_Number(generic));
        lua_pushcclosure(state, drop_self ? generic_call_object : generic_call, 1);
    } else {
        lua_pushcfunction(state, function);
    }
}

void install_object(Instance& instance, lua_State* state, const ObjectSpec& spec) {
    lua_pushlstring(state, spec.name.data(), spec.name.size());
    if (spec.members.empty()) {
        push_member_function(state, spec.call, spec.generic_call, false);
        lua_rawset(state, LUA_GLOBALSINDEX);
        return;
    }
    static_cast<void>(lua_newuserdata(state, 0));
    lua_newtable(state); // metatable
    lua_pushliteral(state, "__call");
    if (spec.call != nullptr || spec.generic_call >= 0) {
        push_member_function(state, spec.call, spec.generic_call, true);
    } else {
        lua_pushlstring(state, spec.name.data(), spec.name.size());
        lua_pushcclosure(state, object_not_callable, 1);
    }
    lua_rawset(state, -3);
    lua_pushliteral(state, "__metatable");
    lua_pushstring(state, binding_metatable_guard);
    lua_rawset(state, -3);
    lua_pushliteral(state, "__index");
    lua_newtable(state);
    for (const Member& member : spec.members) {
        lua_pushlstring(state, member.name.data(), member.name.size());
        push_member_function(state, member.function, member.generic, false);
        lua_rawset(state, -3);
    }
    instance.sandbox->freeze_table(-1);
    if (instance.shared->config.missing_call_sink) {
        lua_pushlstring(state, spec.name.data(), spec.name.size());
        lua_pushcclosure(state, audit_object_index, 2);
    }
    lua_rawset(state, -3);
    instance.sandbox->freeze_table(-1);
    lua_setmetatable(state, -2);
    lua_rawset(state, LUA_GLOBALSINDEX);
}

int thread_value_call(lua_State* state) { return thread_value(state); }

int get_event_call(lua_State* state) {
    lua_remove(state, 1);
    return get_event(state);
}

int get_current_time_call(lua_State* state) {
    lua_remove(state, 1);
    return get_current_time(state);
}

::std::vector<ObjectSpec> infrastructure_objects() {
    return {
        ObjectSpec{"Create_Thread", create_thread, -1, {{"Kill", kill_thread}, {"Kill_All", kill_all_threads}}},
        ObjectSpec{"GetThreadID", get_thread_id, -1, {}},
        ObjectSpec{"GetEvent", get_event_call, -1, {{"Params", get_event_params}, {"Reset", get_event_reset}}},
        ObjectSpec{"ThreadValue", thread_value_call, -1, {{"Set", thread_value_set}, {"Reset", thread_value_reset}}},
        ObjectSpec{"_ScriptExit", script_exit, -1, {}},
        ObjectSpec{"GetCurrentTime", get_current_time_call, -1, {{"Frame", get_current_frame}}},
        ObjectSpec{"Register_Event", register_event, -1, {}},
        ObjectSpec{"Cancel_Event", cancel_event, -1, {}},
        ObjectSpec{"require", require_module, -1, {}},
        ObjectSpec{"print", print_to_log, -1, {}},
    };
}

} // namespace

bool is_infrastructure_name(::std::string_view name) {
    const ::std::string_view base = name.substr(0, name.find('.'));
    for (const ObjectSpec& spec : infrastructure_objects()) {
        if (spec.name == base) return true;
    }
    return false;
}

bool can_extend_infrastructure(::std::string_view name) {
    const auto dot = name.find('.');
    if (dot == ::std::string_view::npos) return false;
    for (const ObjectSpec& spec : infrastructure_objects()) {
        if (spec.name != name.substr(0, dot)) continue;
        return ::std::none_of(spec.members.begin(), spec.members.end(), [&](const Member& member) {
            return member.name == name.substr(dot + 1);
        });
    }
    return false;
}

::std::vector<sf::CFunctionEntry> host_functions() {
    using sf::UpvalueShape;
    return {
        {"host.Create_Thread", create_thread, UpvalueShape::none},
        {"host.Create_Thread.Kill", kill_thread, UpvalueShape::none},
        {"host.Create_Thread.Kill_All", kill_all_threads, UpvalueShape::none},
        {"host.GetThreadID", get_thread_id, UpvalueShape::none},
        {"host.GetEvent", get_event_call, UpvalueShape::none},
        {"host.GetEvent.Params", get_event_params, UpvalueShape::none},
        {"host.GetEvent.Reset", get_event_reset, UpvalueShape::none},
        {"host.ThreadValue", thread_value_call, UpvalueShape::none},
        {"host.ThreadValue.Set", thread_value_set, UpvalueShape::none},
        {"host.ThreadValue.Reset", thread_value_reset, UpvalueShape::none},
        {"host._ScriptExit", script_exit, UpvalueShape::none},
        {"host.GetCurrentTime", get_current_time_call, UpvalueShape::none},
        {"host.GetCurrentTime.Frame", get_current_frame, UpvalueShape::none},
        {"host.Register_Event", register_event, UpvalueShape::none},
        {"host.Cancel_Event", cancel_event, UpvalueShape::none},
        {"host.require", require_module, UpvalueShape::none},
        {"host.print", print_to_log, UpvalueShape::none},
        {"host.binding", generic_call, UpvalueShape::binding_index},
        {"host.binding_object", generic_call_object, UpvalueShape::binding_index},
        {"host.object_not_callable", object_not_callable, UpvalueShape::name},
        {"host.handle_index", handle_index, UpvalueShape::none},
        {"host.bound_method", bound_method, UpvalueShape::bound_method},
    };
}

Instance* current_instance() noexcept { return running_instance; }

InstanceScope::InstanceScope(Instance& instance) noexcept
    : previous_(running_instance), sandbox_scope_(*instance.sandbox) {
    running_instance = &instance;
}

InstanceScope::~InstanceScope() { running_instance = previous_; }

void install_bindings(Instance& instance) {
    lua_State* state = instance.sandbox->state();
    ::std::vector<ObjectSpec> objects = infrastructure_objects();
    const auto& bindings = instance.shared->bindings;
    ::std::map<::std::string, ObjectSpec> generic;
    for (::std::size_t index = 0; index < bindings.size(); ++index) {
        const ::std::string& name = bindings[index].name;
        if (!name.empty() && name.front() == method_binding_prefix) continue;
        const auto dot = name.find('.');
        ObjectSpec& spec = generic[name.substr(0, dot)];
        spec.name = name.substr(0, dot);
        if (dot == ::std::string::npos) {
            spec.generic_call = static_cast<int>(index);
        } else {
            spec.members.push_back(Member{name.substr(dot + 1), nullptr, static_cast<int>(index)});
        }
    }
    // GC-only registered extensions retain the protected object's call and
    // existing members. Registration rejects replacing any built-in member.
    if (instance.shared->config.galactic_clock) {
        for (ObjectSpec& spec : objects) {
            const auto found = generic.find(spec.name);
            if (found == generic.end()) continue;
            spec.members.insert(spec.members.end(), found->second.members.begin(), found->second.members.end());
            generic.erase(found);
        }
    }
    for (const ObjectSpec& spec : objects) install_object(instance, state, spec);
    for (const auto& [name, spec] : generic) install_object(instance, state, spec);
    if (!instance.shared->methods.empty() || instance.shared->config.missing_call_sink)
        instance.sandbox->set_handle_index(handle_index);
    if (instance.shared->config.missing_call_sink) {
        lua_newtable(state);
        lua_pushliteral(state, "__index");
        lua_pushcfunction(state, audit_missing_index);
        lua_rawset(state, -3);
        instance.sandbox->freeze_table(-1);
        lua_setmetatable(state, LUA_GLOBALSINDEX);
    }
    lua_settop(state, 0);
}

void push_value(lua_State* state, const Value& value) {
    ::std::visit(
        [&](const auto& data) {
            using T = ::std::decay_t<decltype(data)>;
            if constexpr (::std::is_same_v<T, ::std::monostate>) {
                lua_pushnil(state);
            } else if constexpr (::std::is_same_v<T, bool>) {
                lua_pushboolean(state, data ? 1 : 0);
            } else if constexpr (::std::is_same_v<T, numeric::LuaNumber>) {
                lua_pushnumber(state, data);
            } else if constexpr (::std::is_same_v<T, ::std::string>) {
                lua_pushlstring(state, data.data(), data.size());
            } else if constexpr (::std::is_same_v<T, Handle>) {
                running_instance->sandbox->push_handle(state, sf::HostHandle{data.kind, data.id});
            } else {
                lua_newtable(state);
                for (::std::size_t index = 0; index < data.size(); ++index) {
                    push_value(state, data[index]);
                    lua_rawseti(state, -2, static_cast<int>(index + 1));
                }
            }
        },
        value.data
    );
}

void add_diagnostic(Instance& instance, ::std::string_view code, ::std::string message) {
    instance.staging.diagnostics.push_back(
        ScriptDiagnostic{::std::string(code), ::std::move(message), instance.tick, instance.id, instance.current_thread});
    if (instance.outcome == InstanceOutcome::faulted && !instance.fault_diagnostic)
        instance.fault_diagnostic = instance.staging.diagnostics.size() - 1;
}

void add_lua_error(Instance& instance, ::std::string_view code, ::std::string message) {
    // Owner policy: only an uncaught Lua failure retires the GC provider.
    // BindingContext::report can diagnose a sourced fallback and continue.
    // Story and object scripts do not retire: the failing thread or call has ended and rolled back.
    const auto& config = instance.shared->config;
    const bool keeps_instance = config.galactic_clock && !config.retire_on_lua_error;
    if (config.galactic_clock && config.retire_on_lua_error) instance.outcome = InstanceOutcome::faulted;
    if (instance.current_thread >= 0) instance.thread_failed = true;
    if (keeps_instance) {
        // Flood guard: a failing call that repeats (an event or timer every tick) reports its first three occurrences,
        // then each power-of-two occurrence with its count, instead of one diagnostic per call.
        constexpr ::std::size_t max_sites = 256;
        auto key = ::std::string(code) + '\n' + message;
        if (instance.failure_sites.size() >= max_sites && !instance.failure_sites.contains(key)) key = "other";
        const auto count = ++instance.failure_sites[key];
        if (count > 3 && (count & (count - 1)) != 0) return;
        if (count > 1) message += " (occurrence " + ::std::to_string(count) + ")";
    }
    add_diagnostic(instance, code, ::std::move(message));
}

void end_slot(Instance& instance, ::std::size_t index) {
    lua_State* state = instance.sandbox->state();
    ThreadSlot& slot = instance.slots[index];
    slot.live = false;
    slot.thread = nullptr;
    for (int* reference : {&slot.thread_reference, &slot.function_reference, &slot.parameter_reference, &slot.values_reference}) {
        if (*reference != LUA_NOREF && *reference != LUA_REFNIL) luaL_unref(state, LUA_REGISTRYINDEX, *reference);
        *reference = LUA_NOREF;
    }
    for (::std::deque<int>* queue : {&slot.callbacks, &slot.parameters}) {
        for (const int reference : *queue) luaL_unref(state, LUA_REGISTRYINDEX, reference);
        instance.queued_events -= static_cast<::std::uint32_t>(queue->size());
        queue->clear();
    }
}

} // namespace eawr::script::authoritative::detail

namespace eawr::script::authoritative {
namespace {
core::Diagnostic global_value_error(::std::string message) {
    core::Diagnostic out; out.code = ::std::string(codes::invalid_request); out.message = ::std::move(message); return out;
}
}
core::Result<void> register_shared_global_values(ScriptScheduler& scripts, GlobalValueReader reader) {
    if (auto added = scripts.register_binding("GlobalValue.Get", [reader = ::std::move(reader)](
            BindingContext& context, const ValueList& args) {
        const auto* key = args.empty() ? nullptr : ::std::get_if<::std::string>(&args[0].data);
        // The instance's own staged write wins over the committed registry (#7188: the stock attack-continuation check
        // sets its baseline and reads it back in one call).
        if (key) if (const auto own = context.staged_global_value(*key))
            return core::Result<ValueList>::success(*own ? ValueList{**own} : ValueList{});
        const auto found = key && reader ? reader(*key) : ::std::nullopt;
        return core::Result<ValueList>::success(found ? ValueList{*found} : ValueList{});
    }); !added) return added;
    return scripts.register_binding("GlobalValue.Set", [](BindingContext& context, const ValueList& args) {
        if (args.size() != 2 || !::std::holds_alternative<::std::string>(args[0].data))
            return core::Result<ValueList>::failure(global_value_error("GlobalValue.Set expects a name and a value"));
        if (::std::holds_alternative<Handle>(args[1].data))
            return core::Result<ValueList>::failure(global_value_error("GlobalValue.Set rejects engine objects (L-55)"));
        if (::std::holds_alternative<::std::vector<Value>>(args[1].data)) {
            context.report(codes::unsupported_api, "GlobalValue.Set: table identity is unsupported; registry unchanged (L-55)");
            context.issue_command(::std::string(global_value_command), {args[0]});
        } else context.issue_command(::std::string(global_value_command), args);
        return core::Result<ValueList>::success({});
    });
}

} // namespace eawr::script::authoritative
