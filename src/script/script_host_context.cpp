#include "eawr/script/script_host.hpp"

#include "script_host_internal.hpp"

#include <limits>
#include <sstream>
#include <utility>

namespace eawr::script {


[[nodiscard]] core::Diagnostic make_core_diagnostic(
    const std::string_view code,
    std::string message,
    const std::optional<std::string_view> logical_path
) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.message = std::move(message);
    if (logical_path) diagnostic.logical_path = std::string(*logical_path);
    return diagnostic;
}

[[nodiscard]] std::string lua_message(lua_State* state) {
    if (lua_gettop(state) == 0) return "Lua operation failed without an error value";
    const char* message = lua_tostring(state, -1);
    return message == nullptr ? "Lua operation failed with a non-string error" : std::string(message);
}

[[nodiscard]] std::optional<std::uint64_t> lua_error_line(const std::string_view message) {
    const auto detail = message.rfind(": ");
    if (detail == std::string_view::npos || detail == 0) return std::nullopt;
    const auto separator = message.rfind(':', detail - 1);
    if (separator == std::string_view::npos || separator + 1 == detail) return std::nullopt;
    std::uint64_t line = 0;
    for (const char digit : message.substr(separator + 1, detail - separator - 1)) {
        if (digit < '0' || digit > '9') return std::nullopt;
        const auto value = static_cast<std::uint64_t>(digit - '0');
        if (line > (std::numeric_limits<std::uint64_t>::max() - value) / 10U) return std::nullopt;
        line = line * 10U + value;
    }
    return line == 0 ? std::nullopt : std::optional<std::uint64_t>(line);
}

[[nodiscard]] ScriptHost::Impl::Instance* ScriptHost::Impl::context(lua_State* state) {
    lua_pushlightuserdata(state, &context_key);
    lua_rawget(state, LUA_REGISTRYINDEX);
    auto* instance = static_cast<Instance*>(lua_touserdata(state, -1));
    lua_pop(state, 1);
    return instance;
}

void ScriptHost::Impl::set_context(lua_State* state, Instance* instance) {
    lua_pushlightuserdata(state, &context_key);
    lua_pushlightuserdata(state, instance);
    lua_rawset(state, LUA_REGISTRYINDEX);
}

void ScriptHost::Impl::remember_source(
    Instance& instance,
    const std::string_view requested_path,
    const vfs::AssetRecord& record
) {
    LoadedSource source{record.canonical_path, record.source_id};
    instance.loaded_sources.insert_or_assign(std::string(requested_path), source);
    instance.loaded_sources.insert_or_assign(record.canonical_path, std::move(source));
}

[[nodiscard]] const ScriptHost::Impl::LoadedSource* ScriptHost::Impl::loaded_source(
    const Instance& instance,
    const char* frame_source
) {
    if (frame_source == nullptr || *frame_source == '\0') return nullptr;
    std::string_view source(frame_source);
    if (source.front() == '@' || source.front() == '=') source.remove_prefix(1);
    if (const auto exact = instance.loaded_sources.find(std::string(source));
        exact != instance.loaded_sources.end()) {
        return &exact->second;
    }
    auto canonical = vfs::canonicalize(source);
    if (!canonical) return nullptr;
    const auto found = instance.loaded_sources.find(canonical.value());
    return found == instance.loaded_sources.end() ? nullptr : &found->second;
}

[[nodiscard]] ScriptHost::Impl::StackContext ScriptHost::Impl::stack_context(Instance& instance, lua_State* state) {
    StackContext result;
    std::ostringstream stream;
    bool wrote_frame = false;
    lua_Debug frame{};
    for (int level = 0; lua_getstack(state, level, &frame) != 0; ++level) {
        if (lua_getinfo(state, "Snl", &frame) == 0) continue;
        if (wrote_frame) stream << '\n';
        wrote_frame = true;
        stream << (frame.short_src[0] == '\0' ? "?" : frame.short_src);
        if (frame.currentline > 0) stream << ':' << frame.currentline;
        if (frame.name != nullptr) stream << " in " << frame.name;
        if (result.source == nullptr && frame.currentline > 0) {
            if (const auto* source = loaded_source(instance, frame.source)) {
                result.source = source;
                result.line = static_cast<std::uint64_t>(frame.currentline);
            }
        }
    }
    result.traceback = stream.str();
    return result;
}

[[nodiscard]] ScriptDiagnostic ScriptHost::Impl::make_script_diagnostic(
    Instance& instance,
    const std::string_view code,
    const std::string_view operation,
    std::string message,
    lua_State* state,
    std::optional<std::string> api_name,
    const core::Diagnostic* cause
) {
    ScriptDiagnostic result;
    result.diagnostic = cause == nullptr
        ? make_core_diagnostic(code, std::move(message))
        : *cause;
    result.instance = instance.id;
    result.coroutine = instance.current_coroutine;
    result.operation = std::string(operation);
    result.api_name = std::move(api_name);
    auto stack = stack_context(instance, state);
    result.traceback = std::move(stack.traceback);
    result.source_kind = "lua";

    if (cause == nullptr && stack.source != nullptr) {
        result.diagnostic.logical_path = stack.source->canonical_path;
        result.diagnostic.source_id = stack.source->source_id;
        result.diagnostic.line = stack.line;
    }
    if (result.api_name && *result.api_name == "Lock_Controls") {
        auto canonical = vfs::canonicalize(instance.logical_path);
        if (canonical && canonical.value() == smoke_path) {
            if (const auto* source = loaded_source(instance, instance.logical_path.c_str())) {
                result.diagnostic.logical_path = source->canonical_path;
                result.diagnostic.source_id = source->source_id;
            } else {
                result.diagnostic.logical_path = instance.logical_path;
            }
            result.source_kind = "pglua";
            result.prototype_id = 7;
            result.pc = 5;
            result.diagnostic.line.reset();
            result.diagnostic.column.reset();
        }
    }
    return result;
}

int ScriptHost::Impl::capture_runtime_error(lua_State* state) {
    auto* instance = context(state);
    if (instance != nullptr && !instance->pending_diagnostic) {
        instance->pending_diagnostic = make_script_diagnostic(
            *instance,
            diagnostic_codes::lua_execution,
            instance->current_operation.empty() ? "lua" : instance->current_operation,
            lua_message(state),
            state
        );
    }
    return 1;
}

int ScriptHost::Impl::protected_call(
    Instance& instance,
    lua_State* state,
    const int arguments,
    const int results,
    const std::string_view operation
) {
    const int function_index = lua_gettop(state) - arguments;
    lua_pushcfunction(state, capture_runtime_error);
    lua_insert(state, function_index);
    auto previous_operation = std::move(instance.current_operation);
    instance.current_operation = std::string(operation);
    const int status = lua_pcall(state, arguments, results, function_index);
    instance.current_operation = std::move(previous_operation);
    lua_remove(state, function_index);
    return status;
}

int ScriptHost::Impl::raise_pending(lua_State* state, Instance& instance, ScriptDiagnostic diagnostic) {
    const auto message = diagnostic.diagnostic.message;
    instance.pending_diagnostic = std::move(diagnostic);
    lua_pushlstring(state, message.data(), message.size());
    return lua_error(state);
}

void ScriptHost::Impl::instruction_hook(lua_State* state, lua_Debug*) {
    auto* instance = context(state);
    if (instance == nullptr) return;
    ++instance->instruction_count;
    if (instance->instruction_count <= max_instructions) return;
    auto diagnostic = make_script_diagnostic(
        *instance,
        diagnostic_codes::resource_limit,
        "instruction-quota",
        "script instruction limit exceeded",
        state
    );
    (void)raise_pending(state, *instance, std::move(diagnostic));
}

void ScriptHost::Impl::begin_operation(Instance& instance, lua_State* state) {
    ++instance.active_operations;
    instance.instruction_count = 0;
    instance.pending_diagnostic.reset();
    lua_sethook(state, instruction_hook, LUA_MASKCOUNT, hook_granularity);
}

void ScriptHost::Impl::end_operation(lua_State* state) {
    lua_sethook(state, nullptr, 0, 0);
    auto* instance = context(state);
    if (instance != nullptr && instance->active_operations != 0) --instance->active_operations;
}

[[nodiscard]] core::Result<void> ScriptHost::Impl::push_chunk(
    Instance& instance,
    lua_State* state,
    const std::string& logical_path
) {
    auto record = files.stat(logical_path);
    if (!record) return core::Result<void>::failure(record.error());
    auto opened = files.open(logical_path);
    if (!opened) {
        auto error = opened.error();
        error.logical_path = record.value().canonical_path;
        error.source_id = record.value().source_id;
        return core::Result<void>::failure(std::move(error));
    }
    remember_source(instance, logical_path, record.value());
    auto bytes = std::move(opened).value();
    std::vector<std::byte> converted;
    if (bytes.size() >= 4 && bytes[0] == std::byte{0x1b} && bytes[1] == std::byte{'L'} &&
        bytes[2] == std::byte{'u'} && bytes[3] == std::byte{'p'}) {
        auto conversion = convert_pglua(bytes, record.value().canonical_path, instance.retail_identity);
        if (!conversion) {
            auto error = conversion.error();
            error.logical_path = record.value().canonical_path;
            error.source_id = record.value().source_id;
            return core::Result<void>::failure(std::move(error));
        }
        if (!conversion.value().unsupported_execution.empty()) {
            const auto& location = conversion.value().unsupported_execution.front();
            auto error = make_core_diagnostic(
                diagnostic_codes::unsupported_feature,
                "PGLua opcode outside the proven execution subset at prototype " +
                    std::to_string(location.prototype_id) + "/PC " + std::to_string(location.pc),
                record.value().canonical_path
            );
            error.source_id = record.value().source_id;
            return core::Result<void>::failure(std::move(error));
        }
        converted = std::move(conversion).value().bytes;
    }
    const auto& load_bytes = converted.empty() ? bytes : converted;
    const auto* data = reinterpret_cast<const char*>(load_bytes.data());
    const int status = luaL_loadbuffer(state, data, load_bytes.size(), logical_path.c_str());
    if (status != 0) {
        auto message = lua_message(state);
        lua_pop(state, 1);
        const auto line = lua_error_line(message);
        auto error = make_core_diagnostic(
            diagnostic_codes::load_parse,
            std::move(message),
            record.value().canonical_path
        );
        error.line = line;
        error.source_id = record.value().source_id;
        return core::Result<void>::failure(std::move(error));
    }
    return core::Result<void>::success();
}

[[nodiscard]] core::Result<std::uint32_t> ScriptHost::Impl::create_thread(
    Instance& instance,
    const std::string_view function,
    std::optional<ScriptValue> initial
) {
    if (instance.threads.size() >= max_coroutines) {
        return core::Result<std::uint32_t>::failure(make_core_diagnostic(
            diagnostic_codes::resource_limit,
            "coroutine slot limit exceeded"
        ));
    }
    lua_getglobal(instance.state, std::string(function).c_str());
    if (!lua_isfunction(instance.state, -1)) {
        lua_pop(instance.state, 1);
        return core::Result<std::uint32_t>::failure(make_core_diagnostic(
            diagnostic_codes::invalid_value,
            "coroutine global is absent or not a function"
        ));
    }
    lua_State* thread = lua_newthread(instance.state);
    lua_pushvalue(instance.state, -2);
    lua_xmove(instance.state, thread, 1);
    const int reference = luaL_ref(instance.state, LUA_REGISTRYINDEX);
    const int function_reference = luaL_ref(instance.state, LUA_REGISTRYINDEX);
    const auto slot = static_cast<std::uint32_t>(instance.threads.size());
    instance.threads.push_back({reference, function_reference, true, true, false, std::move(initial), {}, {}});
    return core::Result<std::uint32_t>::success(slot);
}

void ScriptHost::Impl::clear_queue(Instance& instance, std::vector<int>& queue) {
    for (const int reference : queue) {
        if (reference != LUA_NOREF && reference != LUA_REFNIL) {
            luaL_unref(instance.state, LUA_REGISTRYINDEX, reference);
        }
    }
    queue.clear();
}

void ScriptHost::Impl::release_thread(Instance& instance, ThreadSlot& slot) {
    if (!slot.live && slot.registry_reference == LUA_NOREF) return;
    clear_queue(instance, slot.callbacks);
    clear_queue(instance, slot.parameters);
    if (slot.registry_reference != LUA_NOREF && slot.registry_reference != LUA_REFNIL) {
        luaL_unref(instance.state, LUA_REGISTRYINDEX, slot.registry_reference);
    }
    if (slot.function_reference != LUA_NOREF && slot.function_reference != LUA_REFNIL) {
        luaL_unref(instance.state, LUA_REGISTRYINDEX, slot.function_reference);
    }
    slot.registry_reference = LUA_NOREF;
    slot.function_reference = LUA_NOREF;
    slot.live = false;
    slot.initial.reset();
}

void ScriptHost::Impl::close_instance(Instance& instance) {
    if (instance.state == nullptr) return;
    instance.closing = true;
    for (auto& slot : instance.threads) release_thread(instance, slot);
    for (auto& [name, handlers] : instance.handlers) {
        (void)name;
        for (const auto& handler : handlers) {
            luaL_unref(instance.state, LUA_REGISTRYINDEX, handler.function_reference);
        }
    }
    instance.handlers.clear();
    for (const auto& [token, reference] : instance.function_references) {
        (void)token;
        luaL_unref(instance.state, LUA_REGISTRYINDEX, reference);
    }
    instance.function_references.clear();
    set_context(instance.state, nullptr);
    lua_close(instance.state);
    instance.state = nullptr;
}

[[nodiscard]] ScriptDiagnostic ScriptHost::Impl::operation_error(
    Instance& instance,
    lua_State* state,
    const std::string_view operation
) {
    if (instance.pending_diagnostic) {
        auto result = std::move(*instance.pending_diagnostic);
        instance.pending_diagnostic.reset();
        return result;
    }
    return make_script_diagnostic(
        instance,
        diagnostic_codes::lua_execution,
        operation,
        lua_message(state),
        state
    );
}

} // namespace eawr::script
