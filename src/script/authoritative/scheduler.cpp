#include "scheduler_internal.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/core/tick_profile.hpp"
#include "eawr/sim/world.hpp"
#include "../../sim/tactical/diagnostic_timer.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <set>
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
            add_lua_error(instance, error_code(text), "dispatch " + event.name + ": " + text);
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
        add_lua_error(instance, error_code(text), "call " + event.name + ": " + text);
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
    if (status != 0) add_lua_error(instance, codes::script_error, "start thread " + event.name + ": " + error_text(state));
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
            add_lua_error(instance, error_code(text), "thread " + ::std::to_string(index) + ": " + text);
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
        if (instance.exit_requested || instance.outcome == InstanceOutcome::faulted) return;
    }
}

void service_instance(Instance& instance, ::std::uint64_t tick, bool host_paced, ::std::uint64_t time_tick) {
    if (!instance.inbox.empty() || !host_paced) instance.sync_graph_dirty = true;
    instance.rolled_back = false;
    instance.fault_diagnostic.reset();
    InstanceScope scope(instance);
    instance.sandbox->set_budget(instance.shared->config.quotas.instructions_per_service);
    if (instance.tick != tick) {
        instance.tick = tick;
        instance.random = RandomStream(instance.shared->config.seed, tick, instance.id);
    }
    instance.time_tick = time_tick;
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

// A C++ exception out of a binding (or anything else the instance's service
// runs on the host) fails that instance with a diagnostic, like an uncaught
// Lua error would; it never reaches the session. The instance's Lua state may
// be mid-call after the unwind, so it is not run again.
void service_instance_guarded(Instance& instance, ::std::uint64_t tick, bool host_paced, ::std::uint64_t time_tick) {
    ::std::string failure;
    try {
        service_instance(instance, tick, host_paced, time_tick);
        return;
    } catch (const ::std::exception& error) {
        failure = error.what();
    } catch (...) {
        failure = "unknown exception";
    }
    instance.outcome = InstanceOutcome::faulted;
    if (!instance.fault_diagnostic) instance.fault_diagnostic = instance.staging.diagnostics.size();
    instance.staging.diagnostics.push_back(ScriptDiagnostic{::std::string(codes::script_error),
        "a binding failed on the host: " + (failure.empty() ? ::std::string("exception") : failure),
        instance.tick, instance.id, instance.current_thread});
}

// At the end of an instance's service: the memory measurement when due, and
// a quota fault recorded without raising (host code outside a protected call,
// or the measurement itself) ends the instance like any fault.
void settle_memory(Instance& instance) {
    if (instance.outcome == InstanceOutcome::faulted) return;
    core::tick_profile::AuxiliaryScope memory_scope("memory-settle");
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
        append_u64(out, handle.aux);
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

namespace {
::std::mutex& peaks_mutex() { static ::std::mutex mutex; return mutex; }
::std::map<::std::string, HostWorkPeaks>& peaks_table() { static ::std::map<::std::string, HostWorkPeaks> table; return table; }
} // namespace

void note_host_work(const Instance& instance, const ::std::int64_t instructions) {
    const auto& label = instance.shared->config.label;
    if (label.empty()) return;
    const ::std::lock_guard lock(peaks_mutex());
    auto& peak = peaks_table()[label];
    peak.commands = ::std::max<::std::uint64_t>(peak.commands, instance.staging.commands.size());
    peak.posts = ::std::max<::std::uint64_t>(peak.posts, instance.staging.posts.size());
    peak.diagnostics = ::std::max<::std::uint64_t>(peak.diagnostics, instance.staging.diagnostics.size());
    peak.host_units = ::std::max<::std::uint64_t>(peak.host_units, instance.sandbox->host_work());
    peak.instructions = ::std::max<::std::int64_t>(peak.instructions, instructions);
    ++peak.services;
}

Mark Instance::mark() const noexcept {
    return Mark{staging.commands.size(), staging.posts.size(), staging.timers.size(), random.draws(),
                command_sequence,        post_sequence,        timer_sequence, immediate_log.size(), immediate_changed,
                staging.log.size()};
}

void Instance::begin_immediate(::std::shared_ptr<ImmediateEffectState> shadow) {
    immediate = shadow;
    immediate_base = ::std::move(shadow);
    immediate_changed = false;
    immediate_log.clear();
}

void Instance::end_immediate() noexcept {
    immediate.reset();
    immediate_base.reset();
    immediate_changed = false;
    immediate_log.clear();
}

// The working state after the first `actions` logged actions: the base itself for none, else a private copy of it
// with those actions replayed (they were admitted once, so they apply again to the same result). The rebuild is host
// work of this instance, charged like any other: the copy by what it copies, each replay by `host_replay_units`, none for
// the base itself.
void Instance::rebuild_immediate(const ::std::size_t actions) {
    if (actions == 0) {
        immediate = immediate_base;
        immediate_log.clear();
        return;
    }
    sandbox->charge_host_work(immediate_base->clone_units() + actions * host_replay_units);
    auto rebuilt = immediate_base->clone();
    if (!rebuilt) throw ::std::runtime_error("immediate effect clone is null");
    for (::std::size_t index = 0; index < actions; ++index) {
        auto replayed = rebuilt->apply(immediate_log[index].action, immediate_log[index].arguments);
        if (!replayed) throw ::std::runtime_error("replay of an admitted immediate action failed: " + replayed.error().message);
    }
    immediate_log.resize(actions);
    immediate = ::std::move(rebuilt);
}

void Instance::rollback(const Mark& mark) {
    rolled_back = true;
    pending_timers -= static_cast<::std::uint32_t>(staging.timers.size() - mark.timers);
    staging.commands.resize(mark.commands);
    staging.posts.resize(mark.posts);
    staging.timers.resize(mark.timers);
    staging.log.resize(mark.log);
    random = RandomStream(shared->config.seed, tick | random_domain, id, mark.draws);
    command_sequence = mark.command_sequence;
    post_sequence = mark.post_sequence;
    timer_sequence = mark.timer_sequence;
    if (immediate_log.size() > mark.immediate_actions) rebuild_immediate(mark.immediate_actions);
    immediate_changed = mark.immediate_changed;
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

::std::vector<::std::pair<::std::string, HostWorkPeaks>> host_work_peaks() {
    const ::std::lock_guard lock(detail::peaks_mutex());
    return {detail::peaks_table().begin(), detail::peaks_table().end()};
}

// ---- ScriptScheduler ----

ScriptScheduler::ScriptScheduler(::std::unique_ptr<Impl> impl) noexcept : impl_(::std::move(impl)) {}
ScriptScheduler::ScriptScheduler(ScriptScheduler&&) noexcept = default;
ScriptScheduler& ScriptScheduler::operator=(ScriptScheduler&& other) noexcept {
    if (this != &other) {
        impl_ = ::std::move(other.impl_);
        if (impl_) impl_->state_revision = ::std::numeric_limits<::std::uint64_t>::max();
    }
    return *this;
}
ScriptScheduler::~ScriptScheduler() = default;

core::Result<ScriptScheduler> ScriptScheduler::create(SessionConfig config, ModuleManifest manifest) {
    if (config.galactic_clock && config.query_clock) {
        return core::Result<ScriptScheduler>::failure(detail::make_error(codes::invalid_request,
            "galactic and query clock profiles are mutually exclusive"));
    }
    if (!valid_tick_duration(config.tick_duration)) {
        return core::Result<ScriptScheduler>::failure(
            detail::make_error(codes::invalid_request, "tick duration terms must be in 1..2048"));
    }
    // ADR-013: GC mode time uses the logical 30-frame second.
    if ((config.galactic_clock || config.query_clock) && config.tick_duration.numerator * 30U != config.tick_duration.denominator) {
        return core::Result<ScriptScheduler>::failure(detail::make_error(codes::invalid_request,
            config.query_clock ? "the query clock profile requires one frame per 1/30 second"
                : "the GC clock profile requires one frame per 1/30 second"));
    }
    if (config.quotas.identities == 0 || config.quotas.identities > (::std::uint64_t{1} << 30) ||
        config.quotas.instructions_per_service <= 0 || config.quotas.memory_bytes == 0) {
        return core::Result<ScriptScheduler>::failure(detail::make_error(codes::invalid_request, "invalid quotas"));
    }
    auto impl = ::std::make_unique<Impl>();
    impl->shared = ::std::make_unique<detail::Shared>();
    impl->shared->config = ::std::move(config);
    impl->shared->manifest = ::std::move(manifest);
    impl->shared->manifest_digest = impl->shared->manifest.digest();
    return core::Result<ScriptScheduler>::success(ScriptScheduler(::std::move(impl)));
}

core::Result<void> ScriptScheduler::register_binding(::std::string_view name, Binding binding) {
    return install_binding(name, ::std::move(binding), false);
}

core::Result<void> ScriptScheduler::register_free_random(Binding binding) {
    return install_binding("GameRandom.Free_Random", ::std::move(binding), true);
}

core::Result<void> ScriptScheduler::install_binding(::std::string_view name, Binding binding, const bool free_random) {
    impl_->invalidate_state();
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
    // Free_Random draws outside the synchronized stream; a host binds it only through register_free_random, which
    // takes responsibility for the sourced policy (RGC-195..199).
    if (!free_random && (name == "GameRandom.Free_Random" || name == "Free_Random")) {
        return core::Result<void>::failure(
            detail::make_error(codes::forbidden_capability, ::std::string(name) + " is forbidden in authoritative scripts"));
    }
    for (const detail::BindingEntry& entry : impl_->shared->bindings) {
        if (entry.name == name) {
            return core::Result<void>::failure(detail::make_error(codes::invalid_request, "duplicate binding: " + entry.name));
        }
    }
    if (detail::is_infrastructure_name(name) &&
        !(impl_->shared->config.galactic_clock && detail::can_extend_infrastructure(name))) {
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

bool ScriptScheduler::has_module(::std::string_view logical_path) const {
    return impl_->shared->manifest.find(logical_path) != nullptr;
}

core::Result<void> ScriptScheduler::register_prelude(::std::string_view module) {
    impl_->invalidate_state();
    if (!impl_->instances.empty()) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "preludes are registered before the first instance"));
    }
    const auto normal = ModuleManifest::normalise(module);
    if (!normal || impl_->shared->manifest.find(module) == nullptr) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "prelude is not in the session manifest: " + ::std::string(module)));
    }
    auto& preludes = impl_->shared->preludes;
    if (::std::find(preludes.begin(), preludes.end(), *normal) != preludes.end()) {
        return core::Result<void>::failure(
            detail::make_error(codes::invalid_request, "duplicate prelude: " + ::std::string(module)));
    }
    preludes.push_back(*normal);
    return core::Result<void>::success();
}

core::Result<void> ScriptScheduler::register_method(
    const ::std::uint32_t handle_kind, ::std::string_view method, Binding binding) {
    impl_->invalidate_state();
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

core::Result<void> ScriptScheduler::set_immediate_effect_host(
    ::std::shared_ptr<ImmediateEffectHost> host, const ::std::size_t max_instances) {
    impl_->invalidate_state();
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (!host || max_instances == 0 || impl_->instances.size() > max_instances)
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "immediate effects need a host and sufficient instance bound"));
    impl_->immediate_host = ::std::move(host);
    impl_->immediate_instance_limit = max_instances;
    return core::Result<void>::success();
}

core::Result<void> ScriptScheduler::register_managed_handles(ManagedHandlePolicy policy) {
    impl_->invalidate_state();
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (!impl_->instances.empty() || impl_->managed_handles || !policy.kind || !policy.interval
        || policy.inventory_query.empty() || policy.release_action.empty())
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "managed handles need one pre-instance policy"));
    impl_->managed_handles = ::std::move(policy);
    return core::Result<void>::success();
}

void ScriptScheduler::set_managed_handle_roots(::std::function<ValueList()> roots) { impl_->invalidate_state(); impl_->managed_roots = ::std::move(roots); }
ManagedHandleWork ScriptScheduler::managed_handle_work() const noexcept { return impl_->managed_work; }

core::Result<void> ScriptScheduler::create_instance(const ::std::uint64_t id, ::std::string_view root_module,
    bool* initial_syntax_error) {
    impl_->invalidate_state();
    if (initial_syntax_error) *initial_syntax_error = false;
    using namespace ::eawr::script::sflua;
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (id == 0 || impl_->instances.contains(id) || impl_->retired.contains(id)) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "instance IDs are unique, nonzero and not retired"));
    }
    if (impl_->immediate_host && impl_->instances.size() >= impl_->immediate_instance_limit)
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "ordered native instance bound exceeded"));
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
    instance->time_tick = (impl_->shared->config.galactic_clock || impl_->shared->config.query_clock)
        ? impl_->galactic_frame : impl_->completed_tick;
    instance->random = RandomStream(impl_->shared->config.seed, tick, id);
    try {
        if (impl_->immediate_host) {
            auto shadow = impl_->immediate_host->capture();
            if (!shadow) return core::Result<void>::failure(shadow.error());
            if (!shadow.value()) throw ::std::runtime_error("immediate effect capture is null");
            instance->begin_immediate(::std::move(shadow).value());
        }
        detail::InstanceScope scope(*instance);
        detail::install_bindings(*instance);
        instance->sandbox->seal();
        instance->sandbox->set_budget(impl_->shared->config.quotas.instructions_per_service);
        for (const ::std::string& prelude : impl_->shared->preludes) {
            const ::std::string* source = impl_->shared->manifest.find(prelude);
            if (instance->sandbox->load_source(*source, "@" + prelude) != 0) {
                return core::Result<void>::failure(detail::make_error(codes::module_rejected, detail::error_text(state)));
            }
            const int ran = lua_pcall(state, 0, 0, 0);
            if (instance->sandbox->fault() != detail::sf::SandboxFault::none) {
                return core::Result<void>::failure(detail::make_error(
                    detail::fault_code(instance->sandbox->fault()), "prelude faulted: " + detail::error_text(state)));
            }
            if (ran != 0) return core::Result<void>::failure(detail::make_error(codes::script_error, detail::error_text(state)));
            lua_settop(state, 0);
        }
        const ::std::string chunk_name = "@" + *ModuleManifest::normalise(root_module);
        const int loaded = instance->sandbox->load_source(*bytes, chunk_name);
        if (loaded != 0) {
            if (initial_syntax_error && loaded == LUA_ERRSYNTAX && (bytes->empty() || bytes->front() != '\x1b'))
                *initial_syntax_error = true;
            return core::Result<void>::failure(detail::make_error(codes::module_rejected, detail::error_text(state)));
        }
        instance->tracking_initial_syntax = initial_syntax_error != nullptr;
        const int status = lua_pcall(state, 0, 0, 0);
        instance->tracking_initial_syntax = false;
        if (instance->sandbox->fault() != detail::sf::SandboxFault::none) {
            return core::Result<void>::failure(detail::make_error(
                detail::fault_code(instance->sandbox->fault()), "instance chunk faulted: " + detail::error_text(state)));
        }
        if (status != 0) {
            const auto message = detail::error_text(state);
            if (initial_syntax_error && !instance->initial_syntax_message.empty() && message == instance->initial_syntax_message)
                *initial_syntax_error = true;
            return core::Result<void>::failure(detail::make_error(codes::script_error, message));
        }
        lua_settop(state, 0);
        instance->sandbox->settle_memory();
        if (instance->sandbox->fault() != detail::sf::SandboxFault::none) {
            return core::Result<void>::failure(
                detail::make_error(detail::fault_code(instance->sandbox->fault()), "instance chunk faulted: memory quota exhausted"));
        }
        if (instance->immediate_changed) {
            auto publication = instance->immediate->clone();
            if (!publication) throw ::std::runtime_error("immediate effect clone is null");
            // The catch below is for what happens before the world is touched (the clone): the creation then fails and
            // nothing was published. An exception while the host publishes may follow world changes, so it is terminal.
            auto published = [&]() -> core::Result<void> {
                try {
                    return impl_->immediate_host->publish(::std::move(publication));
                } catch (const ::std::exception& error) {
                    impl_->aborted = true;
                    return core::Result<void>::failure(detail::make_error(codes::session_abort, error.what()));
                }
            }();
            if (!published) return core::Result<void>::failure(published.error());
        }
        instance->end_immediate();
    } catch (const ::std::exception& error) {
        // The instance was never registered and its effects were never published: the creation fails, the session goes on.
        return core::Result<void>::failure(detail::make_error(codes::script_error,
            ::std::string("the instance could not be created on the host: ") + error.what()));
    }
    impl_->instances.emplace(id, ::std::move(instance));
    ++impl_->instance_revision;
    return core::Result<void>::success();
}

core::Result<::std::vector<ScriptDiagnostic>> ScriptScheduler::reconcile_objects(::std::vector<ObjectScript> objects) {
    impl_->invalidate_state();
    using Result = core::Result<::std::vector<ScriptDiagnostic>>;
    if (auto usable = impl_->check_usable(); !usable) return Result::failure(usable.error());
    ::std::sort(objects.begin(), objects.end(), [](const auto& a, const auto& b) { return a.instance < b.instance; });
    ::std::set<::std::pair<::std::uint32_t, ::std::uint64_t>> identities;
    ::std::uint64_t previous = 0;
    for (const auto& object : objects) {
        // SSU-05: object handles are full-width; instance and generation have independent identities.
        if (object.instance == 0 || object.instance == previous || !detail::valid_object_handle(object.object.id) ||
            object.generation == 0 || object.module.empty() ||
            !ModuleManifest::normalise(object.module) ||
            !identities.emplace(object.object.kind, object.object.id).second ||
            (impl_->instances.contains(object.instance) && !impl_->objects.contains(object.instance))) {
            return Result::failure(detail::make_error(codes::invalid_request, "invalid object-script identities"));
        }
        previous = object.instance;
    }
    // Lifecycle changes are structural barrier work. Lua definitions and pumps
    // remain in the partitioned service, never in this ordered commit.
    for (auto iterator = impl_->objects.begin(); iterator != impl_->objects.end();) {
        const auto desired = ::std::lower_bound(objects.begin(), objects.end(), iterator->first,
            [](const auto& object, ::std::uint64_t id) { return object.instance < id; });
        if (desired != objects.end() && desired->instance == iterator->second.instance
            && desired->object == iterator->second.object && desired->generation == iterator->second.generation
            && desired->module == iterator->second.module) { ++iterator; continue; }
        if (impl_->instances.contains(iterator->first)) {
            if (auto removed = remove_instance(iterator->first); !removed) return Result::failure(removed.error());
        }
        iterator = impl_->objects.erase(iterator);
    }
    ::std::vector<ScriptDiagnostic> diagnostics;
    ::std::uint64_t registration = 0;
    if (impl_->shared->config.galactic_clock)
        for (const auto& [id, object] : impl_->objects) { (void)id; registration = ::std::max(registration, object.registration); }
    for (auto object : objects) {
        if (impl_->objects.contains(object.instance)) continue;
        if (impl_->shared->config.galactic_clock) {
            if (registration == ::std::numeric_limits<::std::uint64_t>::max())
                return Result::failure(detail::make_error(codes::invalid_request, "object registration sequence exhausted"));
            object.registration = ++registration;
        }
        impl_->objects.emplace(object.instance, object);
        if (auto created = create_instance(object.instance, object.module); !created) {
            diagnostics.push_back({created.error().code, object.module + ": " + created.error().message,
                impl_->completed_tick + 1, object.instance, -1});
            continue;
        }
        for (::std::uint64_t step = 0; step < 3; ++step) {
            ScriptEvent event;
            event.key = {impl_->completed_tick + 1, first_simulation_producer, object.instance, step};
            event.target = object.instance;
            event.kind = step == 0 ? ScriptEvent::Kind::call : step == 1 ? ScriptEvent::Kind::start_thread : ScriptEvent::Kind::assign;
            event.name = step == 0 ? "Base_Definitions" : step == 1 ? "main" : "Object";
            if (step == 2) event.parameter = Value{object.object};
            if (auto submitted = submit_event(::std::move(event)); !submitted) return Result::failure(submitted.error());
        }
    }
    return Result::success(::std::move(diagnostics));
}

::std::vector<ObjectScript> ScriptScheduler::object_scripts() const {
    ::std::vector<ObjectScript> objects;
    for (const auto& [id, object] : impl_->objects) { static_cast<void>(id); objects.push_back(object); }
    return objects;
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
    impl_->invalidate_state();
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
    // Notifications to disabled providers are harmless. Do not retain events
    // that could revive a retired identity after a checkpoint round trip.
    if (impl_->retired.contains(event.target)) return core::Result<void>::success();
    impl_->pending.emplace(event.key, ::std::move(event));
    return core::Result<void>::success();
}

core::Result<ServiceReport> ScriptScheduler::service(
    const sim::PartitionExecutor& executor, const ServiceOptions& options) {
    impl_->invalidate_state();
    ::std::optional<sim::tactical::DiagnosticTimer> service_timer;
    if (impl_->managed_handles) service_timer.emplace();
    if (impl_->managed_handles && !impl_->immediate_host)
        return core::Result<ServiceReport>::failure(detail::make_error(codes::invalid_request, "managed handles require atomic native effects"));
    if (auto usable = impl_->check_usable(); !usable) return core::Result<ServiceReport>::failure(usable.error());
    if (impl_->immediate_host && impl_->instances.size() > impl_->immediate_instance_limit)
        return core::Result<ServiceReport>::failure(detail::make_error(codes::invalid_request, "ordered native instance bound exceeded"));
    const ::std::uint64_t tick = impl_->completed_tick + 1;
    const auto& config = impl_->shared->config;
    if (config.query_clock && (!options.time_tick || *options.time_tick >= max_script_tick
        || *options.time_tick < impl_->galactic_frame)) {
        return core::Result<ServiceReport>::failure(detail::make_error(codes::invalid_request,
            "query clock service requires a valid nondecreasing upstream time tick"));
    }
    if (options.time_tick && !config.query_clock) {
        if (auto supplied = set_galactic_frame(*options.time_tick); !supplied)
            return core::Result<ServiceReport>::failure(supplied.error());
    }
    const auto time_tick = config.query_clock ? *options.time_tick
        : config.galactic_clock ? impl_->galactic_frame : tick - 1;

    ::std::vector<detail::Instance*> ordered, deferred;
    ::std::vector<::std::uint64_t> deferred_ids;
    ordered.reserve(impl_->instances.size());
    for (auto& [id, instance] : impl_->instances) {
        if (!options.defer_instance || !options.defer_instance(id)) {
            ordered.push_back(instance.get());
        } else {
            if (!impl_->objects.contains(id) || config.galactic_clock || config.query_clock || impl_->immediate_host)
                return core::Result<ServiceReport>::failure(detail::make_error(codes::invalid_request,
                    "deferral requires a default-profile object instance without immediate effects"));
            deferred.push_back(instance.get());
            deferred_ids.push_back(id);
        }
    }
    const auto is_deferred = [&](::std::uint64_t id) {
        return ::std::binary_search(deferred_ids.begin(), deferred_ids.end(), id);
    };

    // Query timers use upstream frame deadlines; ordinary event keys retain
    // their invocation domain. Deliver elapsed timers before this invocation's
    // host events, with each domain retaining canonical key order.
    if (config.query_clock) {
        for (const bool timers : {true, false}) {
            for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end();) {
                const auto& key = iterator->first;
                if ((key.producer == producer_script_timer) != timers || key.tick > (timers ? time_tick : tick)
                    || is_deferred(iterator->second.target)) { ++iterator; continue; }
                auto node = impl_->pending.extract(iterator++);
                const auto found = impl_->instances.find(node.mapped().target);
                if (found != impl_->instances.end()) found->second->inbox.push_back(::std::move(node.mapped()));
            }
        }
    } else {
        // The normal and GC event ordering and tick domain are unchanged.
        for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end() && iterator->first.tick <= tick;) {
            if (is_deferred(iterator->second.target)) { ++iterator; continue; }
            auto node = impl_->pending.extract(iterator++);
            const auto found = impl_->instances.find(node.mapped().target);
            if (found != impl_->instances.end()) found->second->inbox.push_back(::std::move(node.mapped()));
        }
    }
    // Serial: the host's pacing choice is read before the partitioned phase.
    ::std::vector<char> paced(ordered.size());
    for (::std::size_t index = 0; index < ordered.size(); ++index) {
        paced[index] = impl_->objects.contains(ordered[index]->id) ||
            (options.host_paced && options.host_paced(ordered[index]->id)) ? 1 : 0;
    }
    ::std::vector<::std::int64_t> used(ordered.size());
    ManagedHandleWork managed_work;
    ::std::vector<Handle> release_order;

    // Partitioned: each instance reads its own state and writes its own staging.
    ::std::vector<::std::string> failures(sim::tick_partition_count);
    auto partition_service = [&](const ::std::size_t partition) {
        const sim::PartitionRange range = sim::partition_range(partition, ordered.size());
        try {
            // A deferred VM does no Lua work, but its serialized clock still
            // reaches this barrier. Its callbacks retain their original keys
            // in pending until the next subset service, rather than inbox.
            const auto deferred_range = sim::partition_range(partition, deferred.size());
            for (auto index = deferred_range.begin; index < deferred_range.end; ++index) {
                auto& instance = *deferred[index];
                const auto draws = instance.tick == tick ? instance.random.draws() : 0;
                instance.tick = tick;
                instance.time_tick = time_tick;
                instance.random_domain = 0;
                instance.random = RandomStream(config.seed, tick, instance.id, draws);
            }
            for (::std::size_t index = range.begin; index < range.end; ++index) {
                core::tick_profile::JobScope instance_job_scope("job.script-instance", ordered[index]->id);
                detail::service_instance_guarded(*ordered[index], tick, paced[index] != 0, time_tick);
                // GC policy: report() diagnoses and continues, irrespective of
                // code; only an uncaught Lua failure retires the provider.
                if (!config.galactic_clock && impl_->objects.contains(ordered[index]->id) && ::std::any_of(
                        ordered[index]->staging.diagnostics.begin(), ordered[index]->staging.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code != codes::unsupported_api
                            && diagnostic.code != codes::script_warning; })) {
                    ordered[index]->outcome = InstanceOutcome::faulted;
                }
                used[index] = impl_->shared->config.quotas.instructions_per_service - ordered[index]->sandbox->budget()
                    - static_cast<::std::int64_t>(ordered[index]->sandbox->host_work());
                detail::settle_memory(*ordered[index]);
            }
        } catch (const ::std::exception& error) {
            failures[partition] = error.what();
        }
    };
    core::Result<void> run = core::Result<void>::success();
    if (impl_->immediate_host) {
        // Native identity allocation and notifications share one campaign
        // shadow. Only this explicitly bounded script set runs in order; no
        // world entity loop is added and default/tactical service is unchanged.
        try {
            // S9a: the instances of one owner run as a chain of their own over a shadow of their own. The chains
            // keep the ID order inside and run in ascending owner order; the last chain (key 0: no owner) also
            // carries the census, which is why it exists whenever handles are managed. The (key, index) rows sit in
            // a scratch member, so a steady tick allocates nothing here.
            constexpr ::std::uint64_t unowned_key = ::std::numeric_limits<::std::uint64_t>::max();
            auto& order = impl_->chain_order;
            order.clear();
            for (::std::size_t index = 0; index < ordered.size(); ++index) {
                const auto key = options.chain_of ? options.chain_of(ordered[index]->id) : 0;
                order.emplace_back(key == 0 ? unowned_key : key, index);
            }
            const bool object_order = impl_->shared->config.galactic_clock && !options.chain_of && !order.empty()
                && ::std::all_of(ordered.begin(), ordered.end(), [&](const auto* instance) { return impl_->objects.contains(instance->id); });
            if (object_order) {
                ::std::sort(order.begin(), order.end(), [&](const auto& a, const auto& b) {
                    const auto& left = impl_->objects.at(ordered[a.second]->id);
                    const auto& right = impl_->objects.at(ordered[b.second]->id);
                    return left.registration != right.registration ? left.registration > right.registration : left.instance < right.instance;
                });
            } else ::std::sort(order.begin(), order.end());
            const auto owners_end = static_cast<::std::size_t>(::std::find_if(order.begin(), order.end(),
                [](const auto& row) { return row.first == unowned_key; }) - order.begin());
            const bool tail = owners_end < order.size() || impl_->managed_handles || order.empty();
            ::std::shared_ptr<ImmediateEffectState> shadow;
            ::std::shared_ptr<void> chain_scope;
            bool changed = false;
            bool recapture = true;
            // A publication the host refuses, or any exception while publishing, fails the instances of that chain
            // (the batch is dropped, each instance is retired with a diagnostic) and the session goes on. The host's
            // own limits are admitted per action, so a refusal here is the exception, never the way a limit ends.
            // A host that says its publication may have changed the world (the session-abort code) or throws while
            // publishing stays terminal, as before: the world is no longer the one the replay journal describes.
            const auto publish_shadow = [&](ImmediateEffectState& state) -> ::std::string {
                ::std::unique_ptr<ImmediateEffectState> publication;
                try {
                    publication = state.clone();
                } catch (const ::std::exception& error) {
                    return error.what()[0] ? ::std::string(error.what()) : ::std::string("exception");
                }
                if (!publication) return "immediate effect clone is null";
                auto published = impl_->immediate_host->publish(::std::move(publication));
                if (published) return {};
                if (published.error().code == codes::session_abort) throw ::std::runtime_error(published.error().message);
                return published.error().message.empty() ? ::std::string("publication refused") : published.error().message;
            };
            // The chain shares one shadow: every instance after a contributor ran against its effects, so its
            // output and query answers describe a world that is never published. When a batch with contributors is
            // dropped the whole chain is retired (each instance with a diagnostic), not only the contributors. A
            // refused publication with no contributor (the census release alone) is not any instance's doing and
            // retires nobody.
            ::std::vector<char> contributed(order.size(), 0);
            const auto fault_chain = [&](::std::size_t first, ::std::size_t past, const ::std::string& why) {
                bool any = false;
                for (auto row = first; row < past; ++row) any = any || contributed[row] != 0;
                for (auto row = first; row < past; ++row) {
                    auto& instance = *ordered[order[row].second];
                    instance.end_immediate();
                    if (!any || instance.outcome == InstanceOutcome::faulted) continue;
                    instance.outcome = InstanceOutcome::faulted;
                    instance.fault_diagnostic = instance.staging.diagnostics.size();
                    instance.staging.diagnostics.push_back(ScriptDiagnostic{::std::string(codes::script_error),
                        contributed[row] ? "immediate effects could not be published: " + why
                                         : "its chain's immediate effects could not be published (" + why + "); the chain is retired",
                        instance.tick, instance.id, -1});
                }
            };
            ::std::size_t last_begin = 0, last_end = 0;
            for (::std::size_t begin = 0;;) {
                const bool owned = begin < owners_end;
                ::std::size_t end = begin;
                if (owned) {
                    while (end < owners_end && order[end].first == order[begin].first) ++end;
                } else {
                    end = order.size();
                }
                const bool last = owned ? end == owners_end && !tail : true;
                if (options.chain_of && last && begin == end && !impl_->managed_handles) break;
                if (options.begin_chain) {
                    chain_scope.reset();
                    auto selected = options.begin_chain(owned ? order[begin].first : 0);
                    if (!selected) throw ::std::runtime_error(selected.error().message);
                    chain_scope = ::std::move(selected).value();
                    recapture = true;
                }
                // A shadow is copy-on-write, so one that no chain changed still equals the host: no new capture.
                if (recapture || !shadow) {
                    auto captured = impl_->immediate_host->capture();
                    if (!captured) throw ::std::runtime_error(captured.error().message);
                    shadow = ::std::move(captured).value();
                    if (!shadow) throw ::std::runtime_error("immediate effect capture is null");
                }
                changed = false;
                for (auto row = begin; row < end; ++row) {
                    const auto index = order[row].second;
                    auto& instance = *ordered[index];
                    instance.begin_immediate(shadow);
                    detail::service_instance_guarded(instance, tick, paced[index] != 0, time_tick);
                    used[index] = impl_->shared->config.quotas.instructions_per_service - instance.sandbox->budget()
                        - static_cast<::std::int64_t>(instance.sandbox->host_work());
                    detail::settle_memory(instance);
                    if (instance.outcome != InstanceOutcome::faulted) {
                        shadow = instance.immediate;
                        changed = changed || instance.immediate_changed;
                        if (instance.immediate_changed) contributed[row] = 1;
                    }
                    instance.end_immediate();
                }
                last_begin = begin; last_end = end;
                if (last) break;
                // A finished chain publishes before the next one captures; the last chain publishes after the census.
                recapture = changed;
                if (changed) {
                    if (const auto failure = publish_shadow(*shadow); !failure.empty()) {
                        fault_chain(begin, end, failure);
                        shadow.reset();
                        recapture = true;
                    }
                    changed = false;
                }
                begin = end;
            }
            // WGA-471/472: census the prospective completed state, then release
            // within this same shadow. No callback runs during VM destruction.
            // A failure of the census (it only reads and edits this chain's own shadow) costs the last chain's
            // instances, never the session; nothing of that shadow is published.
            const bool clean = impl_->managed_handles && ::std::all_of(ordered.begin(), ordered.end(), [](const auto* instance) {
                return instance->outcome != InstanceOutcome::faulted && !instance->rolled_back;
            });
            if (impl_->managed_handles && clean && tick % impl_->managed_handles->interval == 0) try {
                const sim::tactical::DiagnosticTimer census_timer;
                const auto& policy = *impl_->managed_handles;
                auto inventory = shadow->query(policy.inventory_query, {});
                if (!inventory) throw ::std::runtime_error(inventory.error().message);
                if (!inventory.value().empty()) {
                    const auto surviving = [&](const detail::Instance& instance) {
                        return instance.outcome == InstanceOutcome::running && (!impl_->objects.contains(instance.id)
                            || ::std::any_of(instance.slots.begin(), instance.slots.end(), [](const auto& slot) { return slot.live; }));
                    };
                    ::std::vector<detail::sf::HandleCensus> census(ordered.size());
                    auto walked = executor.execute_phase("script-handle-census", sim::tick_partition_count,
                        [&](::std::size_t partition) {
                            const auto range = sim::partition_range(partition, ordered.size());
                            for (::std::size_t index = range.begin; index < range.end; ++index)
                                if (surviving(*ordered[index])) census[index] = ordered[index]->sandbox->handle_census(policy.kind);
                        });
                    if (!walked) throw ::std::runtime_error(walked.error().message);
                    ::std::set<::std::uint64_t> live;
                    for (const auto& row : census) {
                        managed_work.objects += row.objects; managed_work.edges += row.edges; managed_work.entries += row.entries;
                        if (row.objects) ++managed_work.censuses;
                        for (const auto& handle : row.handles) live.insert(handle.id);
                    }
                    const auto visit = [&](const auto& self, const Value& value) -> void {
                        ++managed_work.host_values;
                        if (const auto* handle = ::std::get_if<Handle>(&value.data)) {
                            if (handle->kind == policy.kind) live.insert(handle->id);
                        } else if (const auto* list = ::std::get_if<ValueList>(&value.data)) {
                            for (const auto& item : *list) self(self, item);
                        }
                    };
                    const auto values = [&](const ValueList& list) { for (const auto& value : list) visit(visit, value); };
                    const auto event_values = [&](const ScriptEvent& event) {
                        const auto found = impl_->instances.find(event.target);
                        if (found == impl_->instances.end() || !surviving(*found->second)) return;
                        values(event.arguments); if (event.parameter) visit(visit, *event.parameter);
                    };
                    for (const auto& [key, event] : impl_->pending) { static_cast<void>(key); event_values(event); }
                    for (const auto* instance : ordered) {
                        for (const auto& command : instance->staging.commands) values(command.arguments);
                        for (const auto& event : instance->staging.posts) event_values(event);
                        for (const auto& event : instance->staging.timers) event_values(event);
                    }
                    if (impl_->managed_roots) values(impl_->managed_roots());
                    ::std::set<::std::uint64_t> ids;
                    for (const auto& value : inventory.value()) {
                        const auto* handle = ::std::get_if<Handle>(&value.data);
                        if (!handle || handle->kind != policy.kind || !handle->id)
                            throw ::std::runtime_error("invalid managed handle inventory");
                        ids.insert(handle->id);
                    }
                    for (const auto id : ids) if (!live.contains(id)) {
                        auto released = shadow->apply(policy.release_action, {Value{Handle{policy.kind, id}}});
                        if (!released) throw ::std::runtime_error(released.error().message);
                        ++managed_work.released; changed = true;
                        release_order.push_back({policy.kind, id});
                    }
                }
                managed_work.nanoseconds = census_timer.elapsed_nanoseconds();
            } catch (const ::std::exception& error) {
                fault_chain(last_begin, last_end, error.what());
                release_order.clear();
                managed_work.released = 0;
                changed = false;
            }
            if (changed) {
                if (const auto failure = publish_shadow(*shadow); !failure.empty()) {
                    fault_chain(last_begin, last_end, failure);
                    release_order.clear();
                    managed_work.released = 0;
                }
            }
        } catch (const ::std::exception& error) {
            impl_->aborted = true;
            for (auto* instance : ordered) instance->end_immediate();
            return core::Result<ServiceReport>::failure(detail::make_error(codes::session_abort, error.what()));
        }
    } else {
        core::tick_profile::prepare_jobs(ordered.size());
        run = executor.execute_phase(script_phase_name, sim::tick_partition_count, partition_service);
    }
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
    report.managed_handles = managed_work;
    report.managed_releases = ::std::move(release_order);
    report.tick = tick;
    for (::std::size_t index = 0; index < ordered.size(); ++index) {
        report.loads.push_back(InstanceLoad{ordered[index]->id, ::std::max<::std::int64_t>(used[index], 0)});
        detail::note_host_work(*ordered[index], used[index]);
    }
    for (detail::Instance* instance : ordered) {
        detail::Staging& staging = instance->staging;
        if (instance->thread_failed) report.thread_failures.push_back(instance->id);
        instance->thread_failed = false;
        if (config.galactic_clock && instance->outcome == InstanceOutcome::faulted) {
            impl_->retired.insert(instance->id);
            core::LoadNote note;
            note.code = "EAWR-LOAD-1400";
            note.object = ::std::to_string(instance->id);
            note.action = core::LoadAction::runtime_degraded;
            note.phase = core::LoadPhase::runtime;
            note.effective_value = "Lua instance retired; uncommitted effects discarded";
            if (!staging.diagnostics.empty()) {
                const auto index = instance->fault_diagnostic.value_or(staging.diagnostics.size() - 1);
                note.diagnostic_code = staging.diagnostics[index].code;
                note.message = staging.diagnostics[index].message;
            }
            report.load_notes.push_back(::std::move(note));
        }
        for (ScriptDiagnostic& diagnostic : staging.diagnostics) report.diagnostics.push_back(::std::move(diagnostic));
        if (instance->outcome != InstanceOutcome::faulted) {
            for (ScriptDiagnostic& line : staging.log) report.log.push_back(::std::move(line));
            for (ScriptCommand& command : staging.commands) report.commands.push_back(::std::move(command));
            for (ScriptEvent& event : staging.posts) impl_->pending.emplace(event.key, ::std::move(event));
            for (ScriptEvent& event : staging.timers) impl_->pending.emplace(event.key, ::std::move(event));
        }
        staging = detail::Staging{};
    }
    for (detail::Instance* instance : ordered) {
        const bool object_finished = impl_->objects.contains(instance->id) &&
            ::std::none_of(instance->slots.begin(), instance->slots.end(), [](const auto& slot) { return slot.live; });
        if (instance->outcome == InstanceOutcome::running && !object_finished) continue;
        report.removed_instances.push_back(instance->id);
        impl_->instances.erase(instance->id);
        ++impl_->instance_revision;
    }
    // Events and timers addressed to removed instances are dropped.
    for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end();) {
        iterator = impl_->instances.contains(iterator->second.target) ? ::std::next(iterator) : impl_->pending.erase(iterator);
    }
    impl_->completed_tick = tick;
    if (config.query_clock) impl_->galactic_frame = time_tick;
    if (service_timer) report.managed_handles.service_nanoseconds = service_timer->elapsed_nanoseconds();
    impl_->managed_work = report.managed_handles;
    return core::Result<ServiceReport>::success(::std::move(report));
}

core::Result<ServiceReport> ScriptScheduler::service_turn(
    const sim::PartitionExecutor& executor, const ::std::vector<::std::uint64_t>& requested) {
    impl_->invalidate_state();
    using Result = core::Result<ServiceReport>;
    if (auto usable = impl_->check_usable(); !usable) return Result::failure(usable.error());
    const auto& config = impl_->shared->config;
    if (config.galactic_clock || config.query_clock || impl_->immediate_host) {
        return Result::failure(detail::make_error(codes::invalid_request,
            "object turns need the default clock profile and no immediate-effect host"));
    }
    // Validate everything before any state changes: ascending unique object instances.
    ::std::vector<::std::uint64_t> subset = requested;
    ::std::sort(subset.begin(), subset.end());
    if (::std::adjacent_find(subset.begin(), subset.end()) != subset.end()) {
        return Result::failure(detail::make_error(codes::invalid_request, "object turn lists an instance twice"));
    }
    ::std::vector<detail::Instance*> ordered;
    ordered.reserve(subset.size());
    for (const ::std::uint64_t id : subset) {
        const auto found = impl_->instances.find(id);
        if (found == impl_->instances.end() || !impl_->objects.contains(id)) {
            return Result::failure(detail::make_error(codes::invalid_request,
                "object turn instance " + ::std::to_string(id) + " is not a live object script"));
        }
        ordered.push_back(found->second.get());
    }
    const ::std::uint64_t tick = impl_->completed_tick + 1;
    const ::std::uint64_t time_tick = tick - 1;

    // Due events and timers of the subset, in key order. Everything the turn posts
    // is keyed for a later tick, so nothing is delivered twice.
    for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end() && iterator->first.tick <= tick;) {
        if (!::std::binary_search(subset.begin(), subset.end(), iterator->second.target)) { ++iterator; continue; }
        auto node = impl_->pending.extract(iterator++);
        impl_->instances.find(node.mapped().target)->second->inbox.push_back(::std::move(node.mapped()));
    }
    // Serial: move each instance onto the turn's key domain, keeping its draw counter,
    // so a draw of the turn never repeats a key of the barrier service of this tick.
    for (detail::Instance* instance : ordered) {
        if (instance->tick != tick) {
            instance->tick = tick;
            instance->random = RandomStream(config.seed, tick, instance->id);
        }
        instance->random_domain = turn_random_domain;
        instance->random = RandomStream(config.seed, tick | turn_random_domain, instance->id, instance->random.draws());
    }
    ::std::vector<::std::int64_t> used(ordered.size());
    ::std::vector<::std::string> failures(sim::tick_partition_count);
    auto partition_service = [&](const ::std::size_t partition) {
        const sim::PartitionRange range = sim::partition_range(partition, ordered.size());
        try {
            for (::std::size_t index = range.begin; index < range.end; ++index) {
                core::tick_profile::JobScope instance_job_scope("job.object-instance", ordered[index]->id);
                detail::service_instance_guarded(*ordered[index], tick, true, time_tick);
                if (::std::any_of(ordered[index]->staging.diagnostics.begin(), ordered[index]->staging.diagnostics.end(),
                        [](const auto& diagnostic) { return diagnostic.code != codes::unsupported_api
                            && diagnostic.code != codes::script_warning; })) {
                    ordered[index]->outcome = InstanceOutcome::faulted;
                }
                used[index] = config.quotas.instructions_per_service - ordered[index]->sandbox->budget()
                    - static_cast<::std::int64_t>(ordered[index]->sandbox->host_work());
                detail::settle_memory(*ordered[index]);
            }
        } catch (const ::std::exception& error) {
            failures[partition] = error.what();
        }
    };
    core::tick_profile::prepare_jobs(ordered.size());
    const core::Result<void> run = executor.execute_phase(script_phase_name, sim::tick_partition_count, partition_service);
    for (const ::std::string& failure : failures) {
        if (!failure.empty()) {
            impl_->aborted = true;
            return Result::failure(detail::make_error(codes::session_abort, failure));
        }
    }
    if (!run) {
        impl_->aborted = true;
        return Result::failure(run.error());
    }

    // Serial commit in ascending instance ID, as service() does.
    ServiceReport report;
    report.tick = tick;
    for (::std::size_t index = 0; index < ordered.size(); ++index) {
        report.loads.push_back(InstanceLoad{ordered[index]->id, ::std::max<::std::int64_t>(used[index], 0)});
        detail::note_host_work(*ordered[index], used[index]);
    }
    for (detail::Instance* instance : ordered) {
        detail::Staging& staging = instance->staging;
        if (instance->thread_failed) report.thread_failures.push_back(instance->id);
        instance->thread_failed = false;
        for (ScriptDiagnostic& diagnostic : staging.diagnostics) report.diagnostics.push_back(::std::move(diagnostic));
        if (instance->outcome != InstanceOutcome::faulted) {
            for (ScriptDiagnostic& line : staging.log) report.log.push_back(::std::move(line));
            for (ScriptCommand& command : staging.commands) report.commands.push_back(::std::move(command));
            for (ScriptEvent& event : staging.posts) impl_->pending.emplace(event.key, ::std::move(event));
            for (ScriptEvent& event : staging.timers) impl_->pending.emplace(event.key, ::std::move(event));
        }
        staging = detail::Staging{};
        // Back to the barrier key domain on the same draw counter.
        instance->random_domain = 0;
        instance->random = RandomStream(config.seed, tick, instance->id, instance->random.draws());
    }
    for (detail::Instance* instance : ordered) {
        const bool object_finished = ::std::none_of(
            instance->slots.begin(), instance->slots.end(), [](const auto& slot) { return slot.live; });
        if (instance->outcome == InstanceOutcome::running && !object_finished) continue;
        report.removed_instances.push_back(instance->id);
        impl_->instances.erase(instance->id);
        ++impl_->instance_revision;
    }
    // Events and timers addressed to removed instances are dropped; completed_tick stays.
    for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end();) {
        iterator = impl_->instances.contains(iterator->second.target) ? ::std::next(iterator) : impl_->pending.erase(iterator);
    }
    return Result::success(::std::move(report));
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
    const int top = lua_gettop(state);
    target.sandbox->push_global(name);
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
        if (target.sandbox->to_handle(state, -1, handle)) value = Value{Handle{handle.kind, handle.id, handle.aux}};
        break;
    default:
        break;
    }
    lua_settop(state, top);
    return ReadResult::success(::std::move(value));
}

core::Result<void> ScriptScheduler::stamp_galactic_object_service(::std::uint64_t instance, bool first,
    numeric::LuaNumber rate, numeric::LuaNumber now) {
    impl_->invalidate_state();
    if (auto usable = impl_->check_usable(); !usable) return usable;
    const auto found = impl_->instances.find(instance);
    if (!impl_->shared->config.galactic_clock || found == impl_->instances.end() || !impl_->objects.contains(instance))
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "object service stamp requires a live galactic attachment"));
    auto& target = *found->second;
    detail::InstanceScope scope(target);
    auto stamp = now;
    if (first) {
        const auto draw = static_cast<::std::uint32_t>(target.random.next_word() >> 49U);
        stamp = -(rate * (numeric::LuaNumber(draw) / numeric::LuaNumber(32767)));
    }
    ScriptEvent event; event.name = "LastService"; event.parameter = Value::number(stamp);
    detail::assign_global(target, event);
    target.sync_graph_dirty = true;
    return core::Result<void>::success();
}

namespace {
struct LuaMemoryObservation {
    ::std::uint64_t bytes{}, states{};
    bool complete{true};
};

template<class Instances>
LuaMemoryObservation observe_lua_memory(const Instances& instances) {
    LuaMemoryObservation result;
    for (const auto& entry : instances) {
        if (!entry.second->sandbox) continue;
        // Lua 5.0 reports KiB rounded down; coroutines share their root's allocator.
        // Query exactly one root per Sandbox, without a collection or stack change.
        const auto kib = detail::sf::lua_getgccount(entry.second->sandbox->state());
        if (kib < 0) { result.complete = false; continue; }
        result.bytes += static_cast<::std::uint64_t>(kib) * 1024U;
        ++result.states;
    }
    return result;
}
} // namespace

diagnostics::MemoryCapacity ScriptScheduler::memory_capacity() const {
    const auto observed = observe_lua_memory(impl_->instances);
    diagnostics::MemoryCapacity result;
    result.complete = observed.complete;
    result.add("lua", "vm_gc_kib_bytes_lower_bound", observed.bytes, observed.states);
    return result;
}

::std::uint64_t ScriptScheduler::diagnostic_lua_bytes() const {
    return observe_lua_memory(impl_->instances).bytes;
}

::std::uint64_t ScriptScheduler::completed_tick() const noexcept { return impl_->completed_tick; }

bool ScriptScheduler::uses_galactic_clock() const noexcept { return impl_->shared->config.galactic_clock; }
TickDuration ScriptScheduler::tick_duration() const noexcept { return impl_->shared->config.tick_duration; }
::std::uint64_t ScriptScheduler::galactic_frame() const noexcept { return impl_->galactic_frame; }
core::Result<void> ScriptScheduler::set_galactic_frame(const ::std::uint64_t frame) {
    impl_->invalidate_state();
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (!uses_galactic_clock() || frame >= max_script_tick || frame > impl_->completed_tick + 1) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request,
            "a mode frame requires the GC clock profile and cannot exceed the next service tick"));
    }
    impl_->galactic_frame = frame;
    return core::Result<void>::success();
}

::std::vector<::std::uint64_t> ScriptScheduler::instances() const {
    ::std::vector<::std::uint64_t> ids;
    for (const auto& [id, instance] : impl_->instances) ids.push_back(id);
    return ids;
}
::std::uint64_t ScriptScheduler::instance_revision() const noexcept { return impl_->instance_revision; }
::std::uint64_t ScriptScheduler::state_revision() const noexcept {
    return impl_->aborted ? ::std::numeric_limits<::std::uint64_t>::max() : impl_->state_revision;
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

::std::vector<::std::uint64_t> ScriptScheduler::retired_instances() const {
    return {impl_->retired.begin(), impl_->retired.end()};
}

core::Result<void> ScriptScheduler::remove_instance(const ::std::uint64_t instance) {
    impl_->invalidate_state();
    if (auto usable = impl_->check_usable(); !usable) return usable;
    if (impl_->instances.erase(instance) == 0) {
        return core::Result<void>::failure(detail::make_error(codes::invalid_request, "no such instance"));
    }
    ++impl_->instance_revision;
    for (auto iterator = impl_->pending.begin(); iterator != impl_->pending.end();) {
        iterator = iterator->second.target == instance ? impl_->pending.erase(iterator) : ::std::next(iterator);
    }
    return core::Result<void>::success();
}

::std::string ScriptScheduler::state_digest() {
    using namespace ::eawr::script::sflua;
    ::std::string out(sandbox_policy_identity);
    out.append(rng_identity);
    if (impl_->managed_handles) {
        const auto& policy = *impl_->managed_handles;
        out.append("managed-handles-wga-471"); detail::append_u64(out, policy.kind); detail::append_u64(out, policy.interval);
        detail::append_bytes(out, policy.inventory_query.data(), policy.inventory_query.size());
        detail::append_bytes(out, policy.release_action.data(), policy.release_action.size());
    }
    detail::append_u64(out, impl_->completed_tick);
    if (uses_galactic_clock()) {
        out.append(galactic_save_format_identity);
        detail::append_u64(out, impl_->galactic_frame);
        detail::append_u64(out, impl_->retired.size());
        for (const auto id : impl_->retired) detail::append_u64(out, id);
    } else if (impl_->shared->config.query_clock) {
        out.append(query_save_format_identity);
        detail::append_u64(out, impl_->galactic_frame);
    }
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
