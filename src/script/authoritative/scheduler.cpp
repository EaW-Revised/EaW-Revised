#include "scheduler_internal.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/sim/world.hpp"

#include <algorithm>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace eawr::script::authoritative {
namespace detail {
namespace {

using namespace ::eawr::script::sflua;

::std::string error_text(lua_State* state) {
    const char* message = lua_tostring(state, -1);
    return message != nullptr ? ::std::string(message, lua_strlen(state, -1)) : ::std::string("(error object is not a string)");
}

::std::string_view fault_code(sf::SandboxFault fault) noexcept {
    switch (fault) {
    case sf::SandboxFault::identity_quota:
        return codes::identity_quota;
    case sf::SandboxFault::logical_quota:
    case sf::SandboxFault::memory_quota:
        return codes::logical_quota;
    default:
        return codes::instruction_budget;
    }
}

// A sandbox fault ends the instance: its tick output is dropped at commit.
bool record_fault(Instance& instance) {
    const sf::SandboxFault fault = instance.sandbox->fault();
    if (fault == sf::SandboxFault::none) return false;
    instance.outcome = InstanceOutcome::faulted;
    const char* message = fault == sf::SandboxFault::identity_quota ? "object identity quota exhausted"
        : fault == sf::SandboxFault::logical_quota                   ? "logical quota exhausted"
        : fault == sf::SandboxFault::memory_quota                    ? "memory quota exhausted"
                                                                      : "instruction budget exhausted";
    add_diagnostic(instance, fault_code(fault), message);
    return true;
}

// A script error raised by a missing engine API keeps that code (L-16).
::std::string_view error_code(::std::string_view text) noexcept {
    return text.find(codes::missing_api) != ::std::string_view::npos ? codes::missing_api : codes::script_error;
}

void logical_fault(Instance& instance, ::std::string message) {
    instance.outcome = InstanceOutcome::faulted;
    add_diagnostic(instance, codes::logical_quota, ::std::move(message));
}

// Register_Event dispatch with the L-32 mutation restart: handlers that finished
// without mutating the list are skipped when the scan restarts at the first one.
void dispatch(Instance& instance, const ScriptEvent& event) {
    lua_State* state = instance.sandbox->state();
    ::std::vector<::std::uint64_t> completed;
    ::std::uint64_t invoked = 0;
    while (true) {
        const auto found = instance.handlers.find(event.name);
        if (found == instance.handlers.end()) return;
        const auto candidate = ::std::find_if(found->second.begin(), found->second.end(), [&](const Handler& handler) {
            return ::std::find(completed.begin(), completed.end(), handler.id) == completed.end();
        });
        if (candidate == found->second.end()) return;
        const ::std::uint64_t handler_id = candidate->id;
        const ::std::uint64_t generation = instance.mutation_generation;
        if (++invoked > instance.shared->config.quotas.queued_events) {
            logical_fault(instance, "event dispatch restart limit exceeded");
            return;
        }
        const Mark mark = instance.mark();
        lua_rawgeti(state, LUA_REGISTRYINDEX, candidate->function_reference);
        for (const Value& argument : event.arguments) push_value(state, argument);
        const int status = lua_pcall(state, static_cast<int>(event.arguments.size()), 0, 0);
        if (record_fault(instance)) return;
        if (status != 0) {
            const ::std::string text = error_text(state);
            add_diagnostic(instance, error_code(text), "dispatch " + event.name + ": " + text);
            lua_settop(state, 0);
            instance.rollback(mark);
            return;
        }
        if (generation == instance.mutation_generation) completed.push_back(handler_id);
    }
}

void signal_thread(Instance& instance, const ScriptEvent& event) {
    if (event.thread < 0 || static_cast<::std::size_t>(event.thread) >= instance.slots.size()) return;
    ThreadSlot& slot = instance.slots[static_cast<::std::size_t>(event.thread)];
    if (!slot.live) return;
    if (instance.queued_events + 2 > instance.shared->config.quotas.queued_events) {
        logical_fault(instance, "event queue quota exhausted");
        return;
    }
    lua_State* state = instance.sandbox->state();
    lua_pushlstring(state, event.name.data(), event.name.size());
    lua_rawget(state, LUA_GLOBALSINDEX);
    slot.callbacks.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
    if (event.parameter) {
        push_value(state, *event.parameter);
        slot.parameters.push_back(luaL_ref(state, LUA_REGISTRYINDEX));
    } else {
        slot.parameters.push_back(LUA_REFNIL);
    }
    instance.queued_events += 2;
}

// The global function with the event's arguments (#79); nothing when the
// global is absent or not a function (L-10).
void call_global(Instance& instance, const ScriptEvent& event) {
    lua_State* state = instance.sandbox->state();
    lua_pushlstring(state, event.name.data(), event.name.size());
    lua_rawget(state, LUA_GLOBALSINDEX);
    if (!lua_isfunction(state, -1)) {
        lua_settop(state, 0);
        return;
    }
    const Mark mark = instance.mark();
    for (const Value& argument : event.arguments) push_value(state, argument);
    const int status = lua_pcall(state, static_cast<int>(event.arguments.size()), 0, 0);
    if (record_fault(instance)) return;
    if (status != 0) {
        const ::std::string text = error_text(state);
        add_diagnostic(instance, error_code(text), "call " + event.name + ": " + text);
        instance.rollback(mark);
    }
    lua_settop(state, 0);
}

// The Create_Thread binding (L-20) called with the function name, outside
// any thread.
void start_thread(Instance& instance, const ScriptEvent& event) {
    lua_State* state = instance.sandbox->state();
    lua_pushliteral(state, "Create_Thread");
    lua_rawget(state, LUA_GLOBALSINDEX);
    lua_pushlstring(state, event.name.data(), event.name.size());
    const int status = lua_pcall(state, 1, 0, 0);
    if (record_fault(instance)) return;
    if (status != 0) add_diagnostic(instance, codes::script_error, "start thread " + event.name + ": " + error_text(state));
    lua_settop(state, 0);
}

// Sets the global to the event's parameter, nil when absent (#79).
void assign_global(Instance& instance, const ScriptEvent& event) {
    lua_State* state = instance.sandbox->state();
    lua_pushlstring(state, event.name.data(), event.name.size());
    if (event.parameter) {
        push_value(state, *event.parameter);
    } else {
        lua_pushnil(state);
    }
    lua_rawset(state, LUA_GLOBALSINDEX);
    lua_settop(state, 0);
}

// L-23 policy point. Retail FoC (debug build, #287) reads only the topmost value
// a resume leaves: boolean true keeps the slot whether the coroutine yielded or
// returned, and a slot that returned true starts its function again on the next
// pump. RO-4 (#296) confirmed the restart at runtime.

// Pumps slots in ascending order (L-22), re-reading the slot count so a
// thread created during the pump runs in the same pump; the topmost value
// decides whether a slot stays live (L-23); the exit flag stops the pump
// after the current thread and ends the script.
void pump_threads(Instance& instance) {
    if (instance.exit_requested) return;
    for (::std::size_t index = 0; index < instance.slots.size(); ++index) {
        if (!instance.slots[index].live) continue;
        instance.current_thread = static_cast<::std::int64_t>(index);
        lua_State* thread = instance.slots[index].thread;
        int arguments = 0;
        if (!instance.slots[index].started) {
            instance.slots[index].started = true;
            lua_rawgeti(thread, LUA_REGISTRYINDEX, instance.slots[index].function_reference);
            const int parameter = instance.slots[index].parameter_reference;
            if (parameter != LUA_NOREF) {
                lua_rawgeti(thread, LUA_REGISTRYINDEX, parameter);
                luaL_unref(thread, LUA_REGISTRYINDEX, parameter);
                instance.slots[index].parameter_reference = LUA_NOREF;
                arguments = 1;
            }
        }
        const Mark mark = instance.mark();
        const int status = lua_resume(thread, arguments);
        if (record_fault(instance)) {
            instance.current_thread = -1;
            return;
        }
        bool live = false;
        if (status != 0) {
            const ::std::string text = error_text(thread);
            add_diagnostic(instance, error_code(text), text);
            instance.rollback(mark);
        } else {
            lua_Debug activation;
            const bool returned = lua_getstack(thread, 0, &activation) == 0;
            live = lua_gettop(thread) > 0 && lua_isboolean(thread, -1) && lua_toboolean(thread, -1) != 0;
            // A returned coroutine is called from its start on the next pump.
            if (live && returned) instance.slots[index].started = false;
        }
        lua_settop(thread, 0);
        instance.current_thread = -1;
        if (!live) end_slot(instance, index);
        if (instance.exit_requested) return;
    }
}

void service_instance(Instance& instance, ::std::uint64_t tick, bool host_paced) {
    InstanceScope scope(instance);
    instance.sandbox->set_budget(instance.shared->config.quotas.instructions_per_service);
    if (instance.tick != tick) {
        instance.tick = tick;
        instance.random = RandomStream(instance.shared->config.seed, tick, instance.id);
    }
    instance.time_tick = tick - 1;
    for (const ScriptEvent& event : instance.inbox) {
        if (event.key.producer == producer_script_timer) --instance.pending_timers;
        switch (event.kind) {
        case ScriptEvent::Kind::dispatch:
            dispatch(instance, event);
            break;
        case ScriptEvent::Kind::thread_signal:
            signal_thread(instance, event);
            break;
        case ScriptEvent::Kind::call:
            call_global(instance, event);
            break;
        case ScriptEvent::Kind::assign:
            assign_global(instance, event);
            break;
        case ScriptEvent::Kind::pump:
            pump_threads(instance);
            break;
        case ScriptEvent::Kind::start_thread:
            start_thread(instance, event);
            break;
        }
        if (instance.outcome == InstanceOutcome::faulted) break;
    }
    instance.inbox.clear();
    if (instance.outcome == InstanceOutcome::faulted) return;
    if (!host_paced) pump_threads(instance);
    if (instance.outcome == InstanceOutcome::running && instance.exit_requested) instance.outcome = InstanceOutcome::exited;
}

// At the end of an instance's service: the memory measurement when due, and
// a quota fault recorded without raising (host code outside a protected call,
// or the measurement itself) ends the instance like any fault.
void settle_memory(Instance& instance) {
    if (instance.outcome == InstanceOutcome::faulted) return;
    instance.sandbox->settle_memory();
    record_fault(instance);
}

void append_value(::std::string& out, lua_State* state, int index, int depth, ::std::vector<const void*>& seen);

void append_u64(::std::string& out, ::std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) out.push_back(static_cast<char>((value >> shift) & 0xFF));
}

void append_bytes(::std::string& out, const char* data, ::std::size_t size) {
    append_u64(out, size);
    out.append(data, size);
}

// Canonical walk of plain data: tables by key order, other references by the
// order in which the walk first meets them. Only for digests.
void append_value(::std::string& out, lua_State* state, int index, int depth, ::std::vector<const void*>& seen) {
    if (index < 0) index = lua_gettop(state) + index + 1;
    const int type = lua_type(state, index);
    out.push_back(static_cast<char>('0' + type));
    switch (type) {
    case LUA_TNIL:
        return;
    case LUA_TBOOLEAN:
        out.push_back(lua_toboolean(state, index) != 0 ? 't' : 'f');
        return;
    case LUA_TNUMBER:
        append_u64(out, lua_tonumber(state, index).repr);
        return;
    case LUA_TSTRING:
        append_bytes(out, lua_tostring(state, index), lua_strlen(state, index));
        return;
    default:
        break;
    }
    const void* address = lua_topointer(state, index);
    const auto found = ::std::find(seen.begin(), seen.end(), address);
    if (found != seen.end()) {
        out.push_back('@');
        append_u64(out, static_cast<::std::uint64_t>(found - seen.begin()));
        return;
    }
    seen.push_back(address);
    Instance& instance = *current_instance();
    sf::HostHandle handle;
    if (type == LUA_TUSERDATA && instance.sandbox->to_handle(state, index, handle)) {
        append_u64(out, handle.kind);
        append_u64(out, handle.id);
        return;
    }
    if (type != LUA_TTABLE || depth > 64) return;
    if (!instance.sandbox->push_canonical_keys(index)) {
        out.push_back('?');
        return;
    }
    const int keys = lua_gettop(state);
    for (int position = 1;; ++position) {
        lua_rawgeti(state, keys, position);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 1);
            break;
        }
        append_value(out, state, -1, depth + 1, seen);
        lua_rawget(state, index);
        append_value(out, state, -1, depth + 1, seen);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
    out.push_back('.');
}

} // namespace

int abort_session(lua_State* state) {
    const char* message = lua_tostring(state, -1);
    throw ::std::runtime_error(
        ::std::string(codes::session_abort) + ": unprotected Lua error: " + (message != nullptr ? message : "(no message)"));
}

Mark Instance::mark() const noexcept {
    return Mark{staging.commands.size(), staging.posts.size(), staging.timers.size(), random.draws(),
                command_sequence,        post_sequence,        timer_sequence};
}

void Instance::rollback(const Mark& mark) {
    pending_timers -= static_cast<::std::uint32_t>(staging.timers.size() - mark.timers);
    staging.commands.resize(mark.commands);
    staging.posts.resize(mark.posts);
    staging.timers.resize(mark.timers);
    random = RandomStream(shared->config.seed, tick, id, mark.draws);
    command_sequence = mark.command_sequence;
    post_sequence = mark.post_sequence;
    timer_sequence = mark.timer_sequence;
}

} // namespace detail

// ---- ModuleManifest ----

::std::optional<::std::string> ModuleManifest::normalise(::std::string_view logical_path) {
    ::std::string out;
    out.reserve(logical_path.size());
    for (const char character : logical_path) {
        if (character == '\\') {
            out.push_back('/');
        } else if (character >= 'a' && character <= 'z') {
            out.push_back(static_cast<char>(character - ('a' - 'A')));
        } else {
            out.push_back(character);
        }
    }
    while (out.rfind("./", 0) == 0) out.erase(0, 2);
    if (out.empty() || out.front() == '/' || out.find(':') != ::std::string::npos) return ::std::nullopt;
    ::std::size_t start = 0;
    while (start <= out.size()) {
        const ::std::size_t end = ::std::min(out.find('/', start), out.size());
        const ::std::string_view part(out.data() + start, end - start);
        if (part.empty() || part == "." || part == "..") return ::std::nullopt;
        start = end + 1;
    }
    return out;
}

core::Result<void> ModuleManifest::add(::std::string_view logical_path, ::std::string bytes) {
    auto path = normalise(logical_path);
    if (!path) {
        return core::Result<void>::failure(
            detail::make_error(codes::module_rejected, "invalid logical path: " + ::std::string(logical_path)));
    }
    if (modules_.contains(*path)) {
        return core::Result<void>::failure(detail::make_error(codes::module_rejected, "duplicate module: " + *path));
    }
    modules_.emplace(::std::move(*path), ::std::move(bytes));
    return core::Result<void>::success();
}

const ::std::string* ModuleManifest::find(::std::string_view logical_path) const {
    const auto path = normalise(logical_path);
    if (!path) return nullptr;
    const auto found = modules_.find(*path);
    return found == modules_.end() ? nullptr : &found->second;
}

::std::string ModuleManifest::digest() const {
    ::std::string listing(sandbox_policy_identity);
    for (const auto& [path, bytes] : modules_) {
        detail::append_bytes(listing, path.data(), path.size());
        const auto hash = core::sha256(::std::span(reinterpret_cast<const ::std::uint8_t*>(bytes.data()), bytes.size()));
        listing.append(reinterpret_cast<const char*>(hash.data()), hash.size());
    }
    return core::sha256_hex(::std::span(reinterpret_cast<const ::std::uint8_t*>(listing.data()), listing.size()));
}

// ---- ScriptScheduler ----

ScriptScheduler::ScriptScheduler(::std::unique_ptr<Impl> impl) noexcept : impl_(::std::move(impl)) {}
ScriptScheduler::ScriptScheduler(ScriptScheduler&&) noexcept = default;
ScriptScheduler& ScriptScheduler::operator=(ScriptScheduler&&) noexcept = default;
ScriptScheduler::~ScriptScheduler() = default;

core::Result<ScriptScheduler> ScriptScheduler::create(SessionConfig config, ModuleManifest manifest) {
    if (!valid_tick_duration(config.tick_duration)) {
        return core::Result<ScriptScheduler>::failure(
            detail::make_error(codes::invalid_request, "tick duration terms must be in 1..2048"));
    }
    if (config.quotas.identities == 0 || config.quotas.identities > (::std::uint64_t{1} << 30) ||
        config.quotas.instructions_per_service <= 0 || config.quotas.memory_bytes == 0) {
        return core::Result<ScriptScheduler>::failure(detail::make_error(codes::invalid_request, "invalid quotas"));
    }
    auto impl = ::std::make_unique<Impl>();
    impl->shared = ::std::make_unique<detail::Shared>();
    impl->shared->config = ::std::move(config);
    impl->shared->manifest = ::std::move(manifest);
    return core::Result<ScriptScheduler>::success(ScriptScheduler(::std::move(impl)));
}

core::Result<void> ScriptScheduler::register_binding(::std::string_view name, Binding binding) {
    if (!impl_->instances.empty()) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "bindings are registered before the first instance"));
    }
    const auto dot = name.find('.');
    const bool valid = !name.empty() && dot != 0 && dot + 1 != name.size() &&
        name.find('.', dot == ::std::string_view::npos ? name.size() : dot + 1) == ::std::string_view::npos;
    if (!valid || !binding) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "invalid binding name: " + ::std::string(name)));
    }
    // Free_Random draws outside the synchronized stream; it has no authoritative form.
    if (name == "GameRandom.Free_Random" || name == "Free_Random") {
        return core::Result<void>::failure(
            detail::make_error(codes::forbidden_capability, ::std::string(name) + " is forbidden in authoritative scripts"));
    }
    for (const detail::BindingEntry& entry : impl_->shared->bindings) {
        if (entry.name == name) {
            return core::Result<void>::failure(detail::make_error(codes::invalid_request, "duplicate binding: " + entry.name));
        }
    }
    if (detail::is_infrastructure_name(name)) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "name belongs to an infrastructure binding: " + ::std::string(name)));
    }
    // install_bindings runs before seal() and would silently replace the global.
    if (const ::std::string_view base = name.substr(0, dot); detail::sf::Sandbox::installs_global(base)) {
        return core::Result<void>::failure(detail::make_error(
            codes::invalid_request, "name belongs to the sandbox global `" + ::std::string(base) + "': " + ::std::string(name)));
    }
    impl_->shared->bindings.push_back(detail::BindingEntry{::std::string(name), ::std::move(binding)});
    return core::Result<void>::success();
}

core::Result<void> ScriptScheduler::register_method(
    const ::std::uint32_t handle_kind, ::std::string_view method, Binding binding) {
    if (!impl_->instances.empty()) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "bindings are registered before the first instance"));
    }
    if (method.empty() || !binding) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "invalid method binding"));
    }
    auto& methods = impl_->shared->methods[handle_kind];
    if (methods.contains(method)) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "duplicate method: " + ::std::string(method)));
    }
    ::std::string name(1, detail::method_binding_prefix);
    name += ::std::to_string(handle_kind) + "." + ::std::string(method);
    methods.emplace(::std::string(method), static_cast<int>(impl_->shared->bindings.size()));
    impl_->shared->bindings.push_back(detail::BindingEntry{::std::move(name), ::std::move(binding)});
    return core::Result<void>::success();
}

core::Result<void> ScriptScheduler::create_instance(const ::std::uint64_t id, ::std::string_view root_module) {
    using namespace ::eawr::script::sflua;
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (id == 0 || impl_->instances.contains(id)) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "instance IDs are unique and nonzero"));
    }
    const ::std::string* bytes = impl_->shared->manifest.find(root_module);
    if (bytes == nullptr) {
        return core::Result<void>::failure(
            detail::make_error(codes::module_rejected, "root module is not in the session manifest: " + ::std::string(root_module)));
    }
    auto instance = ::std::make_unique<detail::Instance>();
    instance->id = id;
    instance->shared = impl_->shared.get();
    detail::sf::SandboxLimits limits;
    limits.identities = impl_->shared->config.quotas.identities;
    limits.memory_bytes = impl_->shared->config.quotas.memory_bytes;
    instance->sandbox = detail::sf::Sandbox::open(limits);
    if (instance->sandbox == nullptr) {
        return core::Result<void>::failure(detail::make_error(codes::session_abort, "cannot allocate a Lua state"));
    }
    lua_State* state = instance->sandbox->state();
    lua_atpanic(state, detail::abort_session);
    const ::std::uint64_t tick = impl_->completed_tick + 1;
    instance->tick = tick;
    instance->time_tick = impl_->completed_tick;
    instance->random = RandomStream(impl_->shared->config.seed, tick, id);
    try {
        detail::InstanceScope scope(*instance);
        detail::install_bindings(*instance);
        instance->sandbox->seal();
        instance->sandbox->set_budget(impl_->shared->config.quotas.instructions_per_service);
        const ::std::string chunk_name = "@" + *ModuleManifest::normalise(root_module);
        if (instance->sandbox->load_source(*bytes, chunk_name) != 0) {
            return core::Result<void>::failure(detail::make_error(codes::module_rejected, detail::error_text(state)));
        }
        const int status = lua_pcall(state, 0, 0, 0);
        if (instance->sandbox->fault() != detail::sf::SandboxFault::none) {
            return core::Result<void>::failure(detail::make_error(
                detail::fault_code(instance->sandbox->fault()), "instance chunk faulted: " + detail::error_text(state)));
        }
        if (status != 0) return core::Result<void>::failure(detail::make_error(codes::script_error, detail::error_text(state)));
        lua_settop(state, 0);
        instance->sandbox->settle_memory();
        if (instance->sandbox->fault() != detail::sf::SandboxFault::none) {
            return core::Result<void>::failure(
                detail::make_error(detail::fault_code(instance->sandbox->fault()), "instance chunk faulted: memory quota exhausted"));
        }
    } catch (const ::std::exception& error) {
        impl_->aborted = true;
        return core::Result<void>::failure(detail::make_error(codes::session_abort, error.what()));
    }
    impl_->instances.emplace(id, ::std::move(instance));
    return core::Result<void>::success();
}

namespace {

// Whether no list in `value` sits deeper than max_value_list_depth.
bool within_list_depth(const Value& value, int depth = 0) {
    const auto* list = ::std::get_if<::std::vector<Value>>(&value.data);
    if (list == nullptr) return true;
    if (depth >= max_value_list_depth) return false;
    return ::std::all_of(list->begin(), list->end(), [&](const Value& item) { return within_list_depth(item, depth + 1); });
}

} // namespace

core::Result<void> ScriptScheduler::submit_event(ScriptEvent event) {
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (event.key.tick <= impl_->completed_tick || event.key.producer < first_simulation_producer) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "events are for a future tick from a simulation producer"));
    }
    const bool arguments_ok =
        ::std::all_of(event.arguments.begin(), event.arguments.end(), [](const Value& value) { return within_list_depth(value); });
    if (!arguments_ok || (event.parameter && !within_list_depth(*event.parameter))) {
        return core::Result<void>::failure(detail::make_error(
            codes::invalid_request,
            "event values nest lists deeper than " + ::std::to_string(max_value_list_depth) + " (the save format's bound)"));
    }
    if (impl_->pending.contains(event.key)) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "duplicate event key"));
    }
    impl_->pending.emplace(event.key, ::std::move(event));
    return core::Result<void>::success();
}

core::Result<ServiceReport> ScriptScheduler::service(
    const sim::PartitionExecutor& executor, const ServiceOptions& options) {
    if (auto usable = impl_->check_usable(); !usable) return core::Result<ServiceReport>::failure(usable.error());
    const ::std::uint64_t tick = impl_->completed_tick + 1;

    // Serial: route due events and timers to their instances in key order.
    while (!impl_->pending.empty() && impl_->pending.begin()->first.tick <= tick) {
        auto node = impl_->pending.extract(impl_->pending.begin());
        const auto found = impl_->instances.find(node.mapped().target);
        if (found != impl_->instances.end()) found->second->inbox.push_back(::std::move(node.mapped()));
    }
    ::std::vector<detail::Instance*> ordered;
    ordered.reserve(impl_->instances.size());
    for (auto& [id, instance] : impl_->instances) ordered.push_back(instance.get());
    // Serial: the host's pacing choice is read before the partitioned phase.
    ::std::vector<char> paced(ordered.size());
    for (::std::size_t index = 0; index < ordered.size(); ++index) {
        paced[index] = options.host_paced && options.host_paced(ordered[index]->id) ? 1 : 0;
    }
    ::std::vector<::std::int64_t> used(ordered.size());

    // Partitioned: each instance reads its own state and writes its own staging.
    ::std::vector<::std::string> failures(sim::tick_partition_count);
    const auto run = executor.execute_phase(script_phase_name, sim::tick_partition_count, [&](const ::std::size_t partition) {
        const sim::PartitionRange range = sim::partition_range(partition, ordered.size());
        try {
            for (::std::size_t index = range.begin; index < range.end; ++index) {
                detail::service_instance(*ordered[index], tick, paced[index] != 0);
                used[index] = impl_->shared->config.quotas.instructions_per_service - ordered[index]->sandbox->budget();
                detail::settle_memory(*ordered[index]);
            }
        } catch (const ::std::exception& error) {
            failures[partition] = error.what();
        }
    });
    for (const ::std::string& failure : failures) {
        if (!failure.empty()) {
            impl_->aborted = true;
            return core::Result<ServiceReport>::failure(detail::make_error(codes::session_abort, failure));
        }
    }
    if (!run) {
        impl_->aborted = true;
        return core::Result<ServiceReport>::failure(run.error());
    }

    // Serial commit in ascending instance ID.
    ServiceReport report;
    report.tick = tick;
    for (::std::size_t index = 0; index < ordered.size(); ++index) {
        report.loads.push_back(InstanceLoad{ordered[index]->id, ::std::max<::std::int64_t>(used[index], 0)});
    }
    for (detail::Instance* instance : ordered) {
        detail::Staging& staging = instance->staging;
        for (ScriptDiagnostic& diagnostic : staging.diagnostics) report.diagnostics.push_back(::std::move(diagnostic));
        if (instance->outcome != InstanceOutcome::faulted) {
            for (ScriptCommand& command : staging.commands) report.commands.push_back(::std::move(command));
            for (ScriptEvent& event : staging.posts) impl_->pending.emplace(event.key, ::std::move(event));
            for (ScriptEvent& event : staging.timers) impl_->pending.emplace(event.key, ::std::move(event));
        }
        staging = detail::Staging{};
    }
    for (detail::Instance* instance : ordered) {
        if (instance->outcome == InstanceOutcome::running) continue;
        report.removed_instances.push_back(instance->id);
        impl_->instances.erase(instance->id);
    }
    // Events and timers addressed to removed instances are dropped.
    for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end();) {
        iterator = impl_->instances.contains(iterator->second.target) ? ::std::next(iterator) : impl_->pending.erase(iterator);
    }
    impl_->completed_tick = tick;
    return core::Result<ServiceReport>::success(::std::move(report));
}

core::Result<::std::optional<Value>> ScriptScheduler::read_global(
    const ::std::uint64_t instance, ::std::string_view name) {
    using namespace ::eawr::script::sflua;
    using ReadResult = core::Result<::std::optional<Value>>;
    if (auto usable = impl_->check_usable(); !usable) return ReadResult::failure(usable.error());
    const auto found = impl_->instances.find(instance);
    if (found == impl_->instances.end()) {
        return ReadResult::failure(detail::make_error(codes::invalid_request, "no such instance"));
    }
    detail::Instance& target = *found->second;
    detail::InstanceScope scope(target);
    lua_State* state = target.sandbox->state();
    lua_pushlstring(state, name.data(), name.size());
    lua_rawget(state, LUA_GLOBALSINDEX);
    ::std::optional<Value> value;
    detail::sf::HostHandle handle;
    switch (lua_type(state, -1)) {
    case LUA_TNIL:
        value = Value{};
        break;
    case LUA_TBOOLEAN:
        value = Value{lua_toboolean(state, -1) != 0};
        break;
    case LUA_TNUMBER:
        value = Value::number(lua_tonumber(state, -1));
        break;
    case LUA_TSTRING:
        value = Value::text(::std::string(lua_tostring(state, -1), lua_strlen(state, -1)));
        break;
    case LUA_TUSERDATA:
        if (target.sandbox->to_handle(state, -1, handle)) value = Value{Handle{handle.kind, handle.id}};
        break;
    default:
        break;
    }
    lua_settop(state, 0);
    return ReadResult::success(::std::move(value));
}

::std::uint64_t ScriptScheduler::completed_tick() const noexcept { return impl_->completed_tick; }

::std::vector<::std::uint64_t> ScriptScheduler::instances() const {
    ::std::vector<::std::uint64_t> ids;
    for (const auto& [id, instance] : impl_->instances) ids.push_back(id);
    return ids;
}

core::Result<::std::vector<bool>> ScriptScheduler::thread_slots(const ::std::uint64_t instance) const {
    using SlotsResult = core::Result<::std::vector<bool>>;
    const auto found = impl_->instances.find(instance);
    if (found == impl_->instances.end()) {
        return SlotsResult::failure(detail::make_error(codes::invalid_request, "no such instance"));
    }
    ::std::vector<bool> slots;
    slots.reserve(found->second->slots.size());
    for (const detail::ThreadSlot& slot : found->second->slots) slots.push_back(slot.live);
    return SlotsResult::success(::std::move(slots));
}

core::Result<void> ScriptScheduler::remove_instance(const ::std::uint64_t instance) {
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (impl_->instances.erase(instance) == 0) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "no such instance"));
    }
    for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end();) {
        iterator = iterator->second.target == instance ? impl_->pending.erase(iterator) : ::std::next(iterator);
    }
    return core::Result<void>::success();
}

::std::string ScriptScheduler::state_digest() {
    using namespace ::eawr::script::sflua;
    ::std::string out(sandbox_policy_identity);
    out.append(rng_identity);
    detail::append_u64(out, impl_->completed_tick);
    for (const auto& [key, event] : impl_->pending) {
        detail::append_u64(out, key.tick);
        detail::append_u64(out, key.producer);
        detail::append_u64(out, key.entity);
        detail::append_u64(out, key.sequence);
        detail::append_u64(out, event.target);
        detail::append_bytes(out, event.name.data(), event.name.size());
    }
    for (auto& [id, instance] : impl_->instances) {
        detail::append_u64(out, id);
        detail::append_u64(out, instance->command_sequence);
        detail::append_u64(out, instance->post_sequence);
        detail::append_u64(out, instance->timer_sequence);
        detail::append_u64(out, instance->random.draws());
        detail::append_u64(out, instance->sandbox->identity_count());
        detail::append_u64(out, instance->sandbox->memory_measured());
        detail::append_u64(out, instance->sandbox->memory_charged());
        for (const detail::ThreadSlot& slot : instance->slots) {
            out.push_back(slot.live ? 'L' : 'D');
            detail::append_u64(out, slot.callbacks.size());
            detail::append_u64(out, slot.parameters.size());
        }
        for (const auto& [name, handlers] : instance->handlers) {
            detail::append_bytes(out, name.data(), name.size());
            for (const detail::Handler& handler : handlers) detail::append_u64(out, handler.id);
        }
        detail::InstanceScope scope(*instance);
        lua_State* state = instance->sandbox->state();
        ::std::vector<const void*> seen;
        lua_pushvalue(state, LUA_GLOBALSINDEX);
        detail::append_value(out, state, -1, 0, seen);
        lua_settop(state, 0);
    }
    return core::sha256_hex(::std::span(reinterpret_cast<const ::std::uint8_t*>(out.data()), out.size()));
}

} // namespace eawr::script::authoritative
