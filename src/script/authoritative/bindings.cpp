// Infrastructure bindings of the authoritative scheduler and the generic host
// binding trampoline (docs/lua-sandbox.md). Thread, event and loading rules
// follow the L-rules in docs/behaviour/lua-script-model.md.
//
// Lua raises errors with longjmp. Each lua_CFunction here does its C++ work in
// a body function whose objects are destroyed before the wrapper raises.

#include "scheduler_internal.hpp"

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
    explicit InstanceContext(Instance& instance) noexcept : instance_(instance) {}

    [[nodiscard]] ::std::uint64_t instance() const noexcept override { return instance_.id; }
    [[nodiscard]] ::std::uint64_t tick() const noexcept override { return instance_.tick; }
    [[nodiscard]] ::std::int64_t thread() const noexcept override { return instance_.current_thread; }
    [[nodiscard]] RandomStream& random() noexcept override { return instance_.random; }
    [[nodiscard]] ::std::uint64_t command_sequence() const noexcept override { return instance_.command_sequence; }

    void issue_command(::std::string verb, ValueList arguments) override {
        instance_.staging.commands.push_back(ScriptCommand{
            instance_.tick, instance_.id, instance_.command_sequence++, ::std::move(verb), ::std::move(arguments)});
    }

    core::Result<void> post_event(ScriptEvent event) override {
        if (event.kind == ScriptEvent::Kind::thread_signal && event.thread < 0) {
            return core::Result<void>::failure(make_error(codes::invalid_request, "thread signal needs a thread slot"));
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
        event.target = instance_.id;
        event.key = EventKey{instance_.tick + *ticks, producer_script_timer, instance_.id, instance_.timer_sequence++};
        instance_.staging.timers.push_back(::std::move(event));
        ++instance_.pending_timers;
        return core::Result<void>::success();
    }

    void report(::std::string_view code, ::std::string message) override {
        add_diagnostic(instance_, code, ::std::move(message));
    }

    bool quota_exhausted{};

private:
    Instance& instance_;
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
    ValueList arguments(static_cast<::std::size_t>(lua_gettop(state)));
    ::std::string error;
    for (int position = 1; position <= lua_gettop(state); ++position) {
        if (!read_value(instance, state, position, 0, arguments[position - 1], error)) {
            const ::std::string message = ::std::string(codes::invalid_request) + ": " + entry.name + " argument " +
                ::std::to_string(position) + ": " + error;
            lua_pushlstring(state, message.data(), message.size());
            return error_raised;
        }
    }
    InstanceContext context(instance);
    auto results = entry.binding(context, arguments);
    if (context.quota_exhausted) return quota_raised;
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
    if (results == 0) lua_pushnil(state);
    return 1;
}

int object_not_callable(lua_State* state) {
    return luaL_error(state, "attempt to call binding object `%s'", lua_tostring(state, lua_upvalueindex(1)));
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
    if (instance.sandbox->load_source(*bytes, chunk_name) != 0) return lua_error(state);
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
    const ::std::vector<ObjectSpec> objects = infrastructure_objects();
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
    for (const ObjectSpec& spec : objects) install_object(instance, state, spec);
    for (const auto& [name, spec] : generic) install_object(instance, state, spec);
    if (!instance.shared->methods.empty()) instance.sandbox->set_handle_index(handle_index);
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

bool queue_limit_reached(const Instance& instance) noexcept {
    return instance.queued_events >= instance.shared->config.quotas.queued_events;
}

} // namespace eawr::script::authoritative::detail
