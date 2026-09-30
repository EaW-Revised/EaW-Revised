#include "eawr/script/script_host.hpp"

#include "script_host_internal.hpp"

#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

namespace eawr::script {

ScriptHost::ScriptHost(const vfs::Vfs& files) : impl_(std::make_unique<Impl>(files)) {}
ScriptHost::~ScriptHost() = default;
ScriptHost::ScriptHost(ScriptHost&&) noexcept = default;
ScriptHost& ScriptHost::operator=(ScriptHost&&) noexcept = default;

core::Result<void> ScriptHost::register_api(
    std::string name,
    std::string signature,
    ApiCallback callback
) {
    if (name.empty() || impl_->binding_by_name.contains(name)) {
        return core::Result<void>::failure(make_core_diagnostic(
            diagnostic_codes::invalid_value,
            "API name must be nonempty and registered exactly once"
        ));
    }
    const auto index = impl_->bindings.size();
    impl_->binding_by_name.emplace(name, index);
    impl_->bindings.push_back({std::move(name), std::move(signature), std::move(callback)});
    for (auto& [id, instance] : impl_->instances) {
        (void)id;
        if (instance->state != nullptr && !instance->closing) impl_->install_api(*instance, index);
    }
    return core::Result<void>::success();
}

core::Result<InstanceId> ScriptHost::load(
    std::string script_path,
    std::vector<std::string> script_directories,
    std::optional<RetailIdentity> retail_identity
) {
    auto canonical = vfs::canonicalize(script_path);
    if (!canonical) return core::Result<InstanceId>::failure(canonical.error());
    auto instance = std::make_unique<Impl::Instance>();
    instance->owner = impl_.get();
    instance->id = impl_->next_instance++;
    instance->logical_path = canonical.value();
    instance->retail_identity = std::move(retail_identity);
    instance->state = lua_open();
    if (instance->state == nullptr) {
        return core::Result<InstanceId>::failure(make_core_diagnostic(
            diagnostic_codes::resource_limit,
            "Lua 5.0.2 state allocation failed",
            instance->logical_path
        ));
    }
    Impl::set_context(instance->state, instance.get());
    luaopen_base(instance->state);
    luaopen_string(instance->state);
    luaopen_table(instance->state);
    impl_->install_infrastructure(*instance);
    for (std::size_t index = 0; index < impl_->bindings.size(); ++index) {
        impl_->install_api(*instance, index);
    }
    std::string lua_path = "./?.lua;./?.lc";
    std::unordered_set<std::string> seen;
    for (auto& directory : script_directories) {
        if (!seen.insert(directory).second) continue;
        if (!directory.empty() && directory.back() != '/' && directory.back() != '\\') directory.push_back('/');
        lua_path += ';' + directory + "?.lua;" + directory + "?.lc";
    }
    lua_pushlstring(instance->state, lua_path.data(), lua_path.size());
    lua_setglobal(instance->state, "LUA_PATH");

    auto loaded = impl_->push_chunk(*instance, instance->state, instance->logical_path);
    if (!loaded) {
        impl_->close_instance(*instance);
        return core::Result<InstanceId>::failure(loaded.error());
    }
    Impl::begin_operation(*instance, instance->state);
    const int status = Impl::protected_call(*instance, instance->state, 0, 1, "load");
    Impl::end_operation(instance->state);
    if (status != 0) {
        auto diagnostic = impl_->operation_error(*instance, instance->state, "load").diagnostic;
        lua_settop(instance->state, 0);
        impl_->close_instance(*instance);
        return core::Result<InstanceId>::failure(std::move(diagnostic));
    }
    lua_pop(instance->state, 1);
    const auto id = instance->id;
    impl_->instances.emplace(id, std::move(instance));
    return core::Result<InstanceId>::success(id);
}

CallOutcome ScriptHost::start(
    const InstanceId instance_id,
    const std::string_view function,
    const ValueList& arguments
) {
    const auto found = impl_->instances.find(instance_id);
    if (found == impl_->instances.end() || found->second->state == nullptr || found->second->closing) {
        ScriptDiagnostic diagnostic;
        diagnostic.diagnostic = make_core_diagnostic(diagnostic_codes::invalid_instance, "invalid script instance");
        diagnostic.instance = instance_id;
        diagnostic.operation = "start";
        return {std::nullopt, std::move(diagnostic)};
    }
    auto& instance = *found->second;
    lua_getglobal(instance.state, std::string(function).c_str());
    if (!lua_isfunction(instance.state, -1)) {
        lua_pop(instance.state, 1);
        return {};
    }
    for (const auto& argument : arguments) {
        auto pushed = impl_->push_value(instance, instance.state, argument);
        if (!pushed) {
            lua_settop(instance.state, 0);
            auto diagnostic = Impl::make_script_diagnostic(
                instance, diagnostic_codes::invalid_value, "start", pushed.error().message, instance.state
            );
            return {std::nullopt, std::move(diagnostic)};
        }
    }
    Impl::begin_operation(instance, instance.state);
    const int status = Impl::protected_call(
        instance,
        instance.state,
        static_cast<int>(arguments.size()),
        1,
        "start"
    );
    Impl::end_operation(instance.state);
    if (status != 0) {
        auto diagnostic = impl_->operation_error(instance, instance.state, "start");
        lua_settop(instance.state, 0);
        if (instance.destroy_requested) impl_->close_instance(instance);
        return {std::nullopt, std::move(diagnostic)};
    }
    std::optional<ScriptValue> result;
    if (!lua_isnil(instance.state, -1)) {
        auto value = impl_->read_value(instance, instance.state, -1);
        if (!value) {
            lua_pop(instance.state, 1);
            auto diagnostic = Impl::make_script_diagnostic(
                instance, diagnostic_codes::invalid_value, "start-result", value.error().message, instance.state
            );
            return {std::nullopt, std::move(diagnostic)};
        }
        result = std::move(value).value();
    }
    lua_pop(instance.state, 1);
    if (instance.destroy_requested) impl_->close_instance(instance);
    return {std::move(result), std::nullopt};
}

ResumeOutcome ScriptHost::resume(const ThreadReference thread_reference) {
    const auto found = impl_->instances.find(thread_reference.instance);
    if (found == impl_->instances.end() || found->second->closing ||
        thread_reference.slot >= found->second->threads.size()) {
        ScriptDiagnostic diagnostic;
        diagnostic.diagnostic = make_core_diagnostic(diagnostic_codes::invalid_instance, "invalid coroutine instance or slot");
        diagnostic.instance = thread_reference.instance;
        diagnostic.coroutine = thread_reference.slot;
        diagnostic.operation = "resume";
        return {ResumeState::ended, std::move(diagnostic)};
    }
    auto& instance = *found->second;
    auto& slot = instance.threads[thread_reference.slot];
    if (!slot.live || slot.registry_reference == LUA_NOREF) return {ResumeState::ended, std::nullopt};
    lua_rawgeti(instance.state, LUA_REGISTRYINDEX, slot.registry_reference);
    lua_State* thread = lua_tothread(instance.state, -1);
    lua_pop(instance.state, 1);
    if (slot.restart_on_next_resume) {
        lua_rawgeti(thread, LUA_REGISTRYINDEX, slot.function_reference);
        slot.restart_on_next_resume = false;
    }
    int argument_count = 0;
    if (slot.first_resume && slot.initial) {
        auto pushed = impl_->push_value(instance, thread, *slot.initial);
        if (!pushed) {
            auto diagnostic = Impl::make_script_diagnostic(
                instance, diagnostic_codes::invalid_value, "resume", pushed.error().message, thread
            );
            impl_->release_thread(instance, slot);
            return {ResumeState::ended, std::move(diagnostic)};
        }
        argument_count = 1;
    }
    slot.first_resume = false;
    slot.initial.reset();
    instance.current_coroutine = thread_reference.slot;
    Impl::begin_operation(instance, thread);
    const int status = lua_resume(thread, argument_count);
    Impl::end_operation(thread);
    instance.current_coroutine.reset();
    if (status != 0) {
        auto diagnostic = impl_->operation_error(instance, thread, "resume");
        lua_settop(thread, 0);
        impl_->release_thread(instance, slot);
        if (instance.destroy_requested) impl_->close_instance(instance);
        return {ResumeState::ended, std::move(diagnostic)};
    }
    // Lua 5.0.2 reports zero for a yield and a return. Retail keeps the slot
    // whenever the topmost result is boolean true, regardless of result count.
    const bool stays_live = lua_gettop(thread) > 0 && lua_isboolean(thread, -1) &&
        lua_toboolean(thread, -1) != 0;
    lua_Debug activation{};
    const bool returned = lua_getstack(thread, 0, &activation) == 0;
    lua_settop(thread, 0);
    if (stays_live) {
        slot.restart_on_next_resume = returned;
        return {ResumeState::live, std::nullopt};
    }
    impl_->release_thread(instance, slot);
    return {ResumeState::ended, std::nullopt};
}

DispatchOutcome ScriptHost::dispatch(
    const InstanceId instance_id,
    const std::string_view event,
    const ValueList& arguments
) {
    const auto found = impl_->instances.find(instance_id);
    if (found == impl_->instances.end() || found->second->closing) {
        ScriptDiagnostic diagnostic;
        diagnostic.diagnostic = make_core_diagnostic(diagnostic_codes::invalid_instance, "invalid script instance");
        diagnostic.instance = instance_id;
        diagnostic.operation = "dispatch";
        return {0, std::move(diagnostic)};
    }
    auto& instance = *found->second;
    auto handler_set = instance.handlers.find(std::string(event));
    if (handler_set == instance.handlers.end()) return {};
    std::unordered_set<std::uint64_t> completed;
    std::size_t invoked = 0;
    while (true) {
        handler_set = instance.handlers.find(std::string(event));
        if (handler_set == instance.handlers.end()) break;
        const auto candidate = std::find_if(
            handler_set->second.begin(), handler_set->second.end(),
            [&](const Impl::EventHandler& handler) { return !completed.contains(handler.id); }
        );
        if (candidate == handler_set->second.end()) break;
        const auto handler_id = candidate->id;
        const auto reference = candidate->function_reference;
        const auto generation = instance.mutation_generation;
        lua_rawgeti(instance.state, LUA_REGISTRYINDEX, reference);
        for (const auto& argument : arguments) {
            auto pushed = impl_->push_value(instance, instance.state, argument);
            if (!pushed) {
                lua_settop(instance.state, 0);
                auto diagnostic = Impl::make_script_diagnostic(
                    instance, diagnostic_codes::invalid_value, "dispatch", pushed.error().message, instance.state
                );
                return {invoked, std::move(diagnostic)};
            }
        }
        Impl::begin_operation(instance, instance.state);
        const int status = Impl::protected_call(
            instance,
            instance.state,
            static_cast<int>(arguments.size()),
            0,
            "dispatch"
        );
        Impl::end_operation(instance.state);
        ++invoked;
        if (status != 0) {
            auto diagnostic = impl_->operation_error(instance, instance.state, "dispatch");
            lua_settop(instance.state, 0);
            if (instance.destroy_requested) impl_->close_instance(instance);
            return {invoked, std::move(diagnostic)};
        }
        if (generation == instance.mutation_generation) completed.insert(handler_id);
        if (invoked >= max_queue_entries) {
            auto diagnostic = Impl::make_script_diagnostic(
                instance, diagnostic_codes::resource_limit, "dispatch",
                "event mutation service limit exceeded", instance.state
            );
            return {invoked, std::move(diagnostic)};
        }
    }
    return {invoked, std::nullopt};
}

core::Result<void> ScriptHost::destroy(const InstanceId instance_id) {
    const auto found = impl_->instances.find(instance_id);
    if (found == impl_->instances.end()) return core::Result<void>::success();
    if (found->second->active_operations != 0) {
        found->second->closing = true;
        found->second->destroy_requested = true;
        return core::Result<void>::success();
    }
    impl_->close_instance(*found->second);
    impl_->instances.erase(found);
    return core::Result<void>::success();
}

const std::vector<ApiInvocation>& ScriptHost::invocations() const noexcept {
    return impl_->invocation_log;
}

core::Result<std::vector<ModuleRequest>> ScriptHost::module_trace(const InstanceId instance_id) const {
    const auto found = impl_->instances.find(instance_id);
    if (found == impl_->instances.end() || found->second->closing) {
        return core::Result<std::vector<ModuleRequest>>::failure(make_core_diagnostic(
            diagnostic_codes::invalid_instance,
            "invalid script instance"
        ));
    }
    return core::Result<std::vector<ModuleRequest>>::success(found->second->module_trace);
}

} // namespace eawr::script
