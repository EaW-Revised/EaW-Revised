#include "eawr/script/script_host.hpp"

#include "script_host_internal.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace eawr::script {

[[nodiscard]] bool ScriptHost::Impl::is_api_value(lua_State* state, const int index) {
    if (lua_type(state, index) != LUA_TUSERDATA || lua_getmetatable(state, index) == 0) {
        return false;
    }
    luaL_getmetatable(state, api_metatable);
    const bool equal = lua_rawequal(state, -1, -2) != 0;
    lua_pop(state, 2);
    return equal;
}

[[nodiscard]] ScriptHost::Impl::ApiUserdata* ScriptHost::Impl::api_value(lua_State* state, const int index) {
    return is_api_value(state, index)
        ? static_cast<ApiUserdata*>(lua_touserdata(state, index))
        : nullptr;
}

[[nodiscard]] core::Result<ScriptValue> ScriptHost::Impl::read_value(
    Instance& instance,
    lua_State* state,
    const int index,
    std::unordered_set<const void*>& table_stack
) {
    switch (lua_type(state, index)) {
    case LUA_TNIL:
        return core::Result<ScriptValue>::success(ScriptValue{});
    case LUA_TBOOLEAN:
        return core::Result<ScriptValue>::success(ScriptValue(lua_toboolean(state, index) != 0));
    case LUA_TNUMBER: {
        const auto number = lua_tonumber(state, index);
        if (!std::isfinite(number) || number > std::numeric_limits<float>::max() ||
            number < -std::numeric_limits<float>::max()) {
            return core::Result<ScriptValue>::failure(make_core_diagnostic(
                diagnostic_codes::invalid_value,
                "nonfinite or out-of-binary32-range number crossed the host boundary"
            ));
        }
        return core::Result<ScriptValue>::success(ScriptValue(static_cast<float>(number)));
    }
    case LUA_TSTRING: {
        const char* data = lua_tostring(state, index);
        const auto size = lua_strlen(state, index);
        return core::Result<ScriptValue>::success(ScriptValue(std::string(data, size)));
    }
    case LUA_TFUNCTION: {
        lua_pushvalue(state, index);
        const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
        const auto token = instance.next_function_token++;
        instance.function_references.emplace(token, reference);
        return core::Result<ScriptValue>::success(
            ScriptValue(FunctionReference{instance.id, token})
        );
    }
    case LUA_TTHREAD:
        for (std::size_t slot = 0; slot < instance.threads.size(); ++slot) {
            lua_rawgeti(instance.state, LUA_REGISTRYINDEX, instance.threads[slot].registry_reference);
            auto* candidate = lua_tothread(instance.state, -1);
            lua_pop(instance.state, 1);
            if (candidate == lua_tothread(state, index)) {
                return core::Result<ScriptValue>::success(ScriptValue(ThreadReference{
                    instance.id,
                    static_cast<std::uint32_t>(slot),
                }));
            }
        }
        break;
    case LUA_TUSERDATA: {
        auto* api = api_value(state, index);
        if (api != nullptr && api->instance == instance.id) {
            return core::Result<ScriptValue>::success(
                ScriptValue(HostReference{instance.id, api->binding + 1U})
            );
        }
        break;
    }
    case LUA_TTABLE: {
        const void* identity = lua_topointer(state, index);
        if (!table_stack.insert(identity).second) {
            return core::Result<ScriptValue>::failure(make_core_diagnostic(
                diagnostic_codes::invalid_value,
                "cyclic table conversion is unsupported"
            ));
        }
        const int absolute = index > 0 ? index : lua_gettop(state) + index + 1;
        const int length = luaL_getn(state, absolute);
        ScriptValue::Sequence sequence;
        sequence.reserve(static_cast<std::size_t>(std::max(length, 0)));
        for (int position = 1; position <= length; ++position) {
            lua_rawgeti(state, absolute, position);
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                table_stack.erase(identity);
                return core::Result<ScriptValue>::failure(make_core_diagnostic(
                    diagnostic_codes::invalid_value,
                    "holey table conversion is unsupported"
                ));
            }
            auto item = read_value(instance, state, -1, table_stack);
            lua_pop(state, 1);
            if (!item) {
                table_stack.erase(identity);
                return core::Result<ScriptValue>::failure(item.error());
            }
            sequence.push_back(std::move(item).value());
        }
        std::size_t key_count = 0;
        lua_pushnil(state);
        while (lua_next(state, absolute) != 0) {
            ++key_count;
            lua_pop(state, 1);
        }
        table_stack.erase(identity);
        if (key_count != sequence.size()) {
            return core::Result<ScriptValue>::failure(make_core_diagnostic(
                diagnostic_codes::invalid_value,
                "associative table conversion is unsupported"
            ));
        }
        return core::Result<ScriptValue>::success(ScriptValue(std::move(sequence)));
    }
    default:
        break;
    }
    return core::Result<ScriptValue>::failure(make_core_diagnostic(
        diagnostic_codes::invalid_value,
        "unsupported or cross-instance Lua value"
    ));
}

[[nodiscard]] core::Result<ScriptValue> ScriptHost::Impl::read_value(
    Instance& instance,
    lua_State* state,
    const int index
) {
    std::unordered_set<const void*> table_stack;
    return read_value(instance, state, index, table_stack);
}

[[nodiscard]] core::Result<void> ScriptHost::Impl::push_value(
    Instance& instance,
    lua_State* state,
    const ScriptValue& value
) {
    return std::visit([&](const auto& stored) -> core::Result<void> {
        using T = std::decay_t<decltype(stored)>;
        if constexpr (std::is_same_v<T, ScriptValue::Nil>) {
            lua_pushnil(state);
        } else if constexpr (std::is_same_v<T, bool>) {
            lua_pushboolean(state, stored ? 1 : 0);
        } else if constexpr (std::is_same_v<T, float>) {
            if (!std::isfinite(stored)) {
                return core::Result<void>::failure(make_core_diagnostic(
                    diagnostic_codes::invalid_value,
                    "nonfinite host number cannot enter Lua"
                ));
            }
            lua_pushnumber(state, static_cast<lua_Number>(stored));
        } else if constexpr (std::is_same_v<T, std::string>) {
            lua_pushlstring(state, stored.data(), stored.size());
        } else if constexpr (std::is_same_v<T, ScriptValue::SequenceStorage>) {
            lua_newtable(state);
            if (stored) {
                int position = 1;
                for (const auto& item : *stored) {
                    auto pushed = push_value(instance, state, item);
                    if (!pushed) {
                        lua_pop(state, 1);
                        return pushed;
                    }
                    lua_rawseti(state, -2, position++);
                }
            }
        } else if constexpr (std::is_same_v<T, FunctionReference>) {
            if (stored.instance != instance.id) {
                return core::Result<void>::failure(make_core_diagnostic(
                    diagnostic_codes::invalid_value,
                    "cross-instance function reference"
                ));
            }
            const auto found = instance.function_references.find(stored.token);
            if (found == instance.function_references.end()) {
                return core::Result<void>::failure(make_core_diagnostic(
                    diagnostic_codes::invalid_value,
                    "expired function reference"
                ));
            }
            lua_rawgeti(state, LUA_REGISTRYINDEX, found->second);
        } else if constexpr (std::is_same_v<T, HostReference>) {
            if (stored.instance != instance.id || stored.token == 0 ||
                stored.token > bindings.size()) {
                return core::Result<void>::failure(make_core_diagnostic(
                    diagnostic_codes::invalid_value,
                    "invalid or cross-instance host reference"
                ));
            }
            push_api_value(instance, stored.token - 1U);
            if (state != instance.state) lua_xmove(instance.state, state, 1);
        } else if constexpr (std::is_same_v<T, ThreadReference>) {
            if (stored.instance != instance.id || stored.slot >= instance.threads.size()) {
                return core::Result<void>::failure(make_core_diagnostic(
                    diagnostic_codes::invalid_value,
                    "invalid or cross-instance coroutine reference"
                ));
            }
            lua_rawgeti(state, LUA_REGISTRYINDEX, instance.threads[stored.slot].registry_reference);
        }
        return core::Result<void>::success();
    }, value.storage());
}

int ScriptHost::Impl::api_call(lua_State* state, const int userdata_index, const int first_argument) {
    auto* instance = context(state);
    auto* value = api_value(state, userdata_index);
    if (instance == nullptr || value == nullptr || value->host != instance->owner ||
        value->instance != instance->id || instance->closing ||
        value->binding >= instance->owner->bindings.size()) {
        if (instance == nullptr) {
            lua_pushliteral(state, "invalid script instance");
            return lua_error(state);
        }
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance,
            diagnostic_codes::invalid_instance,
            "Function_Call",
            "invalid or cross-instance host callable",
            state
        ));
    }
    auto& binding = instance->owner->bindings[value->binding];
    ValueList arguments;
    for (int index = first_argument; index <= lua_gettop(state); ++index) {
        auto argument = instance->owner->read_value(*instance, state, index);
        if (!argument) {
            return raise_pending(state, *instance, make_script_diagnostic(
                *instance,
                diagnostic_codes::invalid_value,
                "engine-api",
                argument.error().message,
                state,
                binding.name
            ));
        }
        arguments.push_back(std::move(argument).value());
    }
    instance->owner->invocation_log.push_back({
        instance->id,
        instance->current_coroutine,
        binding.name,
        arguments,
    });
    if (!binding.callback) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance,
            diagnostic_codes::missing_engine_api,
            "engine-api",
            "missing engine API: " + binding.name,
            state,
            binding.name
        ));
    }
    const ApiCallContext call_context{instance->id, instance->current_coroutine, binding.name};
    auto returned = binding.callback(call_context, arguments);
    if (instance->closing) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance,
            diagnostic_codes::invalid_instance,
            "engine-api-reentry",
            "script instance was destroyed during a host callback",
            state,
            binding.name
        ));
    }
    if (!returned) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance,
            returned.error().code.empty() ? diagnostic_codes::lua_execution : returned.error().code,
            "engine-api",
            returned.error().message,
            state,
            binding.name
        ));
    }
    int count = 0;
    for (const auto& item : returned.value()) {
        auto pushed = instance->owner->push_value(*instance, state, item);
        if (!pushed) {
            return raise_pending(state, *instance, make_script_diagnostic(
                *instance,
                diagnostic_codes::invalid_value,
                "engine-api-result",
                pushed.error().message,
                state,
                binding.name
            ));
        }
        ++count;
    }
    return count;
}

int ScriptHost::Impl::api_trampoline(lua_State* state) { return api_call(state, 1, 2); }

int ScriptHost::Impl::function_call(lua_State* state) { return api_call(state, 1, 2); }

void ScriptHost::Impl::push_api_value(Instance& instance, const std::size_t binding) {
    auto* value = static_cast<ApiUserdata*>(lua_newuserdata(instance.state, sizeof(ApiUserdata)));
    *value = ApiUserdata{this, instance.id, binding};
    luaL_getmetatable(instance.state, api_metatable);
    lua_setmetatable(instance.state, -2);
}

void ScriptHost::Impl::install_api(Instance& instance, const std::size_t binding) {
    push_api_value(instance, binding);
    lua_setglobal(instance.state, bindings[binding].name.c_str());
}

int ScriptHost::Impl::vfs_loadfile(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || instance->closing) {
        lua_pushnil(state);
        lua_pushliteral(state, "invalid script instance");
        return 2;
    }
    if (lua_type(state, 1) != LUA_TSTRING) {
        lua_pushnil(state);
        lua_pushliteral(state, "loadfile requires a logical path; stdin is unavailable");
        return 2;
    }
    const std::string path(lua_tostring(state, 1), lua_strlen(state, 1));
    auto loaded = instance->owner->push_chunk(*instance, state, path);
    if (!loaded) {
        lua_pushnil(state);
        lua_pushlstring(state, loaded.error().message.data(), loaded.error().message.size());
        return 2;
    }
    return 1;
}

int ScriptHost::Impl::vfs_dofile(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || instance->closing || lua_type(state, 1) != LUA_TSTRING) {
        lua_pushliteral(state, "dofile requires a logical VFS path");
        return lua_error(state);
    }
    const std::string path(lua_tostring(state, 1), lua_strlen(state, 1));
    lua_settop(state, 0);
    auto loaded = instance->owner->push_chunk(*instance, state, path);
    if (!loaded) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance,
            diagnostic_codes::load_parse,
            "dofile",
            loaded.error().message,
            state,
            std::nullopt,
            &loaded.error()
        ));
    }
    const int status = protected_call(*instance, state, 0, LUA_MULTRET, "dofile");
    if (status != 0) return lua_error(state);
    return lua_gettop(state);
}

std::string ScriptHost::Impl::substitute_module(std::string pattern, const std::string_view name) {
    std::size_t position = 0;
    while ((position = pattern.find('?', position)) != std::string::npos) {
        pattern.replace(position, 1, name);
        position += name.size();
    }
    return pattern;
}

int ScriptHost::Impl::vfs_require(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || instance->closing || lua_type(state, 1) != LUA_TSTRING) {
        lua_pushliteral(state, "require expects an exact module-name string");
        return lua_error(state);
    }
    const std::string name(lua_tostring(state, 1), lua_strlen(state, 1));
    lua_getglobal(state, "_LOADED");
    lua_pushlstring(state, name.data(), name.size());
    lua_rawget(state, -2);
    if (lua_toboolean(state, -1) != 0) {
        instance->module_trace.push_back({name, std::nullopt, true});
        lua_remove(state, -2);
        return 1;
    }
    lua_pop(state, 1);
    lua_getglobal(state, "LUA_PATH");
    const std::string path = lua_type(state, -1) == LUA_TSTRING
        ? std::string(lua_tostring(state, -1), lua_strlen(state, -1))
        : std::string("./?.lua;./?.lc");
    lua_pop(state, 1);

    std::optional<std::string> resolved;
    std::string last_error = "module was not found in LUA_PATH";
    std::size_t begin = 0;
    while (begin <= path.size()) {
        const auto end = path.find(';', begin);
        const auto component = path.substr(begin, end == std::string::npos ? path.size() - begin : end - begin);
        const auto candidate = substitute_module(component, name);
        if (!candidate.empty()) {
            auto loaded = instance->owner->push_chunk(*instance, state, candidate);
            if (loaded) {
                resolved = candidate;
                break;
            }
            last_error = loaded.error().message;
            if (loaded.error().code != vfs::diagnostic_codes::not_found) {
                instance->module_trace.push_back({name, candidate, false});
                return raise_pending(state, *instance, make_script_diagnostic(
                    *instance,
                    diagnostic_codes::load_parse,
                    "require",
                    last_error,
                    state,
                    std::nullopt,
                    &loaded.error()
                ));
            }
        }
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    instance->module_trace.push_back({name, resolved, false});
    if (!resolved) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance,
            diagnostic_codes::load_parse,
            "require",
            "module '" + name + "' was not found through logical VFS paths: " + last_error,
            state
        ));
    }
    const int status = protected_call(*instance, state, 0, 1, "require");
    if (status != 0) return lua_error(state);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        lua_pushboolean(state, 1);
    }
    lua_pushlstring(state, name.data(), name.size());
    lua_pushvalue(state, -2);
    lua_rawset(state, -4);
    lua_remove(state, -2);
    return 1;
}

int ScriptHost::Impl::create_thread_lua(lua_State* state) {
    auto* instance = context(state);
    // Create_Thread is a callable table, so its __call metamethod receives
    // the table itself before the explicit function name.
    constexpr int name_index = 2;
    constexpr int initial_index = 3;
    if (instance == nullptr || instance->closing || lua_type(state, name_index) != LUA_TSTRING) {
        lua_pushliteral(state, "Create_Thread expects a global function name");
        return lua_error(state);
    }
    const std::string name(
        lua_tostring(state, name_index),
        lua_strlen(state, name_index)
    );
    std::optional<ScriptValue> initial;
    if (lua_gettop(state) >= initial_index) {
        auto value = instance->owner->read_value(*instance, state, initial_index);
        if (!value) {
            return raise_pending(state, *instance, make_script_diagnostic(
                *instance, diagnostic_codes::invalid_value, "Create_Thread",
                value.error().message, state
            ));
        }
        initial = std::move(value).value();
    }
    auto slot = instance->owner->create_thread(*instance, name, std::move(initial));
    if (!slot) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance, slot.error().code, "Create_Thread", slot.error().message, state
        ));
    }
    lua_pushnumber(state, static_cast<lua_Number>(slot.value()));
    return 1;
}

int ScriptHost::Impl::kill_thread(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || lua_type(state, 1) != LUA_TNUMBER) return 0;
    const auto raw = lua_tonumber(state, 1);
    if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max()) return 0;
    const auto slot = static_cast<std::uint32_t>(raw);
    if (slot >= instance->threads.size() || instance->current_coroutine == slot) return 0;
    instance->owner->release_thread(*instance, instance->threads[slot]);
    return 0;
}

int ScriptHost::Impl::kill_all_threads(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr) return 0;
    for (std::size_t slot = 0; slot < instance->threads.size(); ++slot) {
        if (!instance->current_coroutine || *instance->current_coroutine != slot) {
            instance->owner->release_thread(*instance, instance->threads[slot]);
        }
    }
    return 0;
}

int ScriptHost::Impl::register_event(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || lua_type(state, 1) != LUA_TSTRING || !lua_isfunction(state, 2)) {
        lua_pushliteral(state, "Register_Event expects event name and function");
        return lua_error(state);
    }
    const std::string name(lua_tostring(state, 1), lua_strlen(state, 1));
    lua_pushvalue(state, 2);
    const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
    instance->handlers[name].push_back({instance->next_handler_id++, reference});
    ++instance->mutation_generation;
    return 0;
}

int ScriptHost::Impl::cancel_event(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || lua_type(state, 1) != LUA_TSTRING || !lua_isfunction(state, 2)) return 0;
    const std::string name(lua_tostring(state, 1), lua_strlen(state, 1));
    auto found = instance->handlers.find(name);
    if (found == instance->handlers.end()) return 0;
    bool changed = false;
    auto& handlers = found->second;
    for (auto iterator = handlers.begin(); iterator != handlers.end();) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, iterator->function_reference);
        const bool equal = lua_rawequal(state, -1, 2) != 0;
        lua_pop(state, 1);
        if (equal) {
            luaL_unref(state, LUA_REGISTRYINDEX, iterator->function_reference);
            iterator = handlers.erase(iterator);
            changed = true;
        } else {
            ++iterator;
        }
    }
    if (changed) ++instance->mutation_generation;
    return 0;
}

int ScriptHost::Impl::get_event(lua_State* state) { return pop_event_queue(state, true); }

int ScriptHost::Impl::get_event_params(lua_State* state) { return pop_event_queue(state, false); }

int ScriptHost::Impl::pop_event_queue(lua_State* state, const bool callback) {
    auto* instance = context(state);
    if (instance == nullptr || !instance->current_coroutine ||
        *instance->current_coroutine >= instance->threads.size()) return 0;
    auto& queue = callback
        ? instance->threads[*instance->current_coroutine].callbacks
        : instance->threads[*instance->current_coroutine].parameters;
    if (queue.empty()) return 0;
    const int reference = queue.front();
    queue.erase(queue.begin());
    if (reference == LUA_REFNIL || reference == LUA_NOREF) return 0;
    lua_rawgeti(state, LUA_REGISTRYINDEX, reference);
    luaL_unref(state, LUA_REGISTRYINDEX, reference);
    return 1;
}

int ScriptHost::Impl::reset_events(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr) return 0;
    for (auto& slot : instance->threads) {
        instance->owner->clear_queue(*instance, slot.callbacks);
        instance->owner->clear_queue(*instance, slot.parameters);
    }
    return 0;
}

int ScriptHost::Impl::queue_event(lua_State* state) {
    auto* instance = context(state);
    if (instance == nullptr || lua_type(state, 1) != LUA_TNUMBER || !lua_isfunction(state, 2)) {
        lua_pushliteral(state, "Queue_Event expects coroutine slot, callback, and optional parameter");
        return lua_error(state);
    }
    const auto raw = lua_tonumber(state, 1);
    if (raw < 0 || raw > std::numeric_limits<std::uint32_t>::max()) return 0;
    const auto slot_index = static_cast<std::uint32_t>(raw);
    if (slot_index >= instance->threads.size() || !instance->threads[slot_index].live) return 0;
    auto& slot = instance->threads[slot_index];
    if (slot.callbacks.size() >= max_queue_entries || slot.parameters.size() >= max_queue_entries) {
        return raise_pending(state, *instance, make_script_diagnostic(
            *instance, diagnostic_codes::resource_limit, "Queue_Event",
            "event queue limit exceeded", state
        ));
    }
    lua_pushvalue(state, 2);
    slot.callbacks.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
    if (lua_gettop(state) >= 3 && !lua_isnil(state, 3)) {
        lua_pushvalue(state, 3);
        slot.parameters.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
    } else {
        slot.parameters.push_back(LUA_REFNIL);
    }
    return 0;
}

void ScriptHost::Impl::install_infrastructure(Instance& instance) {
    luaL_newmetatable(instance.state, api_metatable);
    lua_pushliteral(instance.state, "__call");
    lua_pushcfunction(instance.state, api_trampoline);
    lua_rawset(instance.state, -3);
    lua_pop(instance.state, 1);

    lua_register(instance.state, "Function_Call", function_call);
    lua_register(instance.state, "loadfile", vfs_loadfile);
    lua_register(instance.state, "dofile", vfs_dofile);
    lua_register(instance.state, "require", vfs_require);
    lua_register(instance.state, "Register_Event", register_event);
    lua_register(instance.state, "Cancel_Event", cancel_event);
    lua_register(instance.state, "Queue_Event", queue_event);

    lua_newtable(instance.state);
    lua_pushliteral(instance.state, "Kill");
    lua_pushcfunction(instance.state, kill_thread);
    lua_rawset(instance.state, -3);
    lua_pushliteral(instance.state, "Kill_All");
    lua_pushcfunction(instance.state, kill_all_threads);
    lua_rawset(instance.state, -3);
    lua_newtable(instance.state);
    lua_pushliteral(instance.state, "__call");
    lua_pushcfunction(instance.state, create_thread_lua);
    lua_rawset(instance.state, -3);
    lua_setmetatable(instance.state, -2);
    lua_setglobal(instance.state, "Create_Thread");

    lua_newtable(instance.state);
    lua_pushliteral(instance.state, "Params");
    lua_pushcfunction(instance.state, get_event_params);
    lua_rawset(instance.state, -3);
    lua_pushliteral(instance.state, "Reset");
    lua_pushcfunction(instance.state, reset_events);
    lua_rawset(instance.state, -3);
    lua_newtable(instance.state);
    lua_pushliteral(instance.state, "__call");
    lua_pushcfunction(instance.state, get_event);
    lua_rawset(instance.state, -3);
    lua_setmetatable(instance.state, -2);
    lua_setglobal(instance.state, "GetEvent");

    lua_newtable(instance.state);
    lua_setglobal(instance.state, "Script");
}

} // namespace eawr::script
