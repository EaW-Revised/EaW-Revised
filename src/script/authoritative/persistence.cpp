// Save, load and state hash of the authoritative script scheduler (#248); the
// format and its rules are in docs/lua-persistence.md. Everything here runs
// serially at the tick barrier.

#include "scheduler_internal.hpp"

#include "eawr/core/sha256.hpp"
#include "eawr/script/numeric/backend.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace eawr::script::authoritative {
namespace detail {
namespace {

using persist::ByteReader;
using persist::ByteWriter;

// Smallest encodings, used to bound counts by the remaining input.
constexpr std::size_t min_event_bytes = 54;
constexpr std::size_t min_instance_bytes = 90;

enum ValueTag : std::uint8_t {
    value_nil = 0,
    value_false = 1,
    value_true = 2,
    value_number = 3,
    value_string = 4,
    value_handle = 5,
    value_list = 6,
};

core::Result<void> reject(std::string message) {
    return core::Result<void>::failure(make_error(codes::load_rejected, std::move(message)));
}

const sf::CFunctionTable* function_table(std::string& error) {
    struct Built {
        sf::CFunctionTable table;
        std::string error;
        bool ok{};
    };
    static const Built built = [] {
        Built result;
        result.ok = sf::CFunctionTable::create(host_functions(), result.table, result.error);
        return result;
    }();
    if (!built.ok) error = built.error;
    return built.ok ? &built.table : nullptr;
}

// ---- Session identity ----

void write_identity(ByteWriter& out) {
    out.text(save_format_identity);
    out.text(sandbox_policy_identity);
    out.text(rng_identity);
    out.text(numeric::numeric_abi().name);
    out.u32(numeric::numeric_abi().version);
}

void write_config(ByteWriter& out, const SessionConfig& config) {
    out.u64(config.seed);
    out.u32(config.tick_duration.numerator);
    out.u32(config.tick_duration.denominator);
    out.i64(config.quotas.instructions_per_service);
    out.u64(config.quotas.identities);
    out.u32(config.quotas.thread_slots);
    out.u32(config.quotas.queued_events);
    out.u32(config.quotas.pending_timers);
    out.u32(config.quotas.registrations);
    out.u64(config.quotas.memory_bytes);
    out.u32(static_cast<std::uint32_t>(config.script_directories.size()));
    for (const std::string& directory : config.script_directories) out.text(directory);
}

void write_bindings(ByteWriter& out, const std::vector<BindingEntry>& bindings) {
    out.u32(static_cast<std::uint32_t>(bindings.size()));
    for (const BindingEntry& binding : bindings) out.text(binding.name);
}

// ---- Host values and events ----

// A list nested deeper than max_value_list_depth fails the writer: the reader
// would reject it.
void write_value(ByteWriter& out, const Value& value, int depth = 0) {
    std::visit(
        [&](const auto& data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                out.u8(value_nil);
            } else if constexpr (std::is_same_v<T, bool>) {
                out.u8(data ? value_true : value_false);
            } else if constexpr (std::is_same_v<T, numeric::LuaNumber>) {
                out.u8(value_number);
                out.u64(data.repr);
            } else if constexpr (std::is_same_v<T, std::string>) {
                out.u8(value_string);
                out.text(data);
            } else if constexpr (std::is_same_v<T, Handle>) {
                out.u8(value_handle);
                out.u32(data.kind);
                out.u64(data.id);
            } else {
                if (depth >= max_value_list_depth) {
                    out.fail();
                    return;
                }
                out.u8(value_list);
                out.u32(static_cast<std::uint32_t>(data.size()));
                for (const Value& item : data) write_value(out, item, depth + 1);
            }
        },
        value.data
    );
}

bool read_value(ByteReader& in, Value& value, int depth) {
    const std::uint8_t tag = in.u8();
    switch (tag) {
    case value_nil:
        value.data = std::monostate{};
        break;
    case value_false:
    case value_true:
        value.data = tag == value_true;
        break;
    case value_number:
        value.data = numeric::LuaNumber::from_repr(in.u64());
        break;
    case value_string:
        value.data = in.text();
        break;
    case value_handle: {
        Handle handle;
        handle.kind = in.u32();
        handle.id = in.u64();
        value.data = handle;
        break;
    }
    case value_list: {
        if (depth >= max_value_list_depth) return false;
        std::vector<Value> items;
        const std::uint32_t count = in.count(1);
        for (std::uint32_t index = 0; index < count; ++index) {
            Value* item = in.append(items);
            if (item == nullptr || !read_value(in, *item, depth + 1)) return false;
        }
        value.data = std::move(items);
        break;
    }
    default:
        return false;
    }
    return in.ok();
}

void write_values(ByteWriter& out, const ValueList& values) {
    out.u32(static_cast<std::uint32_t>(values.size()));
    for (const Value& value : values) write_value(out, value);
}

bool read_values(ByteReader& in, ValueList& values) {
    const std::uint32_t count = in.count(1);
    for (std::uint32_t index = 0; index < count; ++index) {
        Value* value = in.append(values);
        if (value == nullptr || !read_value(in, *value, 0)) return false;
    }
    return in.ok();
}

void write_event(ByteWriter& out, const ScriptEvent& event) {
    out.u64(event.key.tick);
    out.u32(event.key.producer);
    out.u64(event.key.entity);
    out.u64(event.key.sequence);
    out.u64(event.target);
    out.u8(static_cast<std::uint8_t>(event.kind));
    out.text(event.name);
    out.i64(event.thread);
    write_values(out, event.arguments);
    out.u8(event.parameter ? 1 : 0);
    if (event.parameter) write_value(out, *event.parameter);
}

bool read_event(ByteReader& in, ScriptEvent& event) {
    event.key.tick = in.u64();
    event.key.producer = in.u32();
    event.key.entity = in.u64();
    event.key.sequence = in.u64();
    event.target = in.u64();
    const std::uint8_t kind = in.u8();
    if (kind > static_cast<std::uint8_t>(ScriptEvent::Kind::start_thread)) return false;
    event.kind = static_cast<ScriptEvent::Kind>(kind);
    event.name = in.text();
    event.thread = in.i64();
    if (!read_values(in, event.arguments)) return false;
    const std::uint8_t has_parameter = in.u8();
    if (has_parameter > 1) return false;
    if (has_parameter == 1) {
        Value parameter;
        if (!read_value(in, parameter, 0)) return false;
        event.parameter = std::move(parameter);
    }
    return in.ok();
}

bool valid_producer(std::uint32_t producer) noexcept {
    return producer == producer_script_post || producer == producer_script_timer || producer >= first_simulation_producer;
}

// ---- Instances ----

void write_instance(ByteWriter& out, const Instance& instance) {
    out.u64(instance.id);
    out.u64(instance.tick);
    out.u64(instance.time_tick);
    out.u64(instance.random.draws());
    out.u64(instance.command_sequence);
    out.u64(instance.post_sequence);
    out.u64(instance.timer_sequence);
    out.u64(instance.next_handler_id);
    out.u64(instance.mutation_generation);
    out.u32(instance.registrations);
    out.u32(instance.queued_events);
    out.u32(instance.pending_timers);
    out.u32(static_cast<std::uint32_t>(instance.slots.size()));
    for (const ThreadSlot& slot : instance.slots) {
        out.u8(slot.live ? 1 : 0);
        out.u8(slot.started ? 1 : 0);
        out.i32(slot.thread_reference);
        out.i32(slot.function_reference);
        out.i32(slot.parameter_reference);
        out.i32(slot.values_reference);
        for (const std::deque<int>* queue : {&slot.callbacks, &slot.parameters}) {
            out.u32(static_cast<std::uint32_t>(queue->size()));
            for (const int reference : *queue) out.i32(reference);
        }
    }
    out.u32(static_cast<std::uint32_t>(instance.handlers.size()));
    for (const auto& [name, handlers] : instance.handlers) {
        out.text(name);
        out.u32(static_cast<std::uint32_t>(handlers.size()));
        for (const Handler& handler : handlers) {
            out.u64(handler.id);
            out.i32(handler.function_reference);
        }
    }
    const Staging& staging = instance.staging;
    out.u32(static_cast<std::uint32_t>(staging.commands.size()));
    for (const ScriptCommand& command : staging.commands) {
        out.u64(command.tick);
        out.u64(command.issuer);
        out.u64(command.sequence);
        out.text(command.verb);
        write_values(out, command.arguments);
    }
    for (const std::vector<ScriptEvent>* events : {&staging.posts, &staging.timers}) {
        out.u32(static_cast<std::uint32_t>(events->size()));
        for (const ScriptEvent& event : *events) write_event(out, event);
    }
    out.u32(static_cast<std::uint32_t>(staging.diagnostics.size()));
    for (const ScriptDiagnostic& diagnostic : staging.diagnostics) {
        out.text(diagnostic.code);
        out.text(diagnostic.message);
        out.u64(diagnostic.tick);
        out.u64(diagnostic.instance);
        out.i64(diagnostic.thread);
    }
}

// A registry reference the host holds: LUA_NOREF, LUA_REFNIL or a live entry.
bool reference_ok(sf::Sandbox& sandbox, int reference, sf::ReferenceKind kind, bool allow_none, bool allow_nil) {
    if (reference == LUA_NOREF) return allow_none;
    if (reference == LUA_REFNIL) return allow_nil;
    return sf::registry_holds(sandbox, reference, kind);
}

class InstanceReader {
public:
    InstanceReader(ByteReader& in, const Shared& shared, const sf::CFunctionTable& functions, std::uint64_t completed_tick) noexcept
        : in_(in), shared_(shared), functions_(functions), completed_tick_(completed_tick) {}

    core::Result<std::unique_ptr<Instance>> read() {
        auto instance = std::make_unique<Instance>();
        instance->shared = &shared_;
        Instance& current = *instance;
        current.id = in_.u64();
        current.tick = in_.u64();
        current.time_tick = in_.u64();
        const std::uint64_t draws = in_.u64();
        current.command_sequence = in_.u64();
        current.post_sequence = in_.u64();
        current.timer_sequence = in_.u64();
        current.next_handler_id = in_.u64();
        current.mutation_generation = in_.u64();
        current.registrations = in_.u32();
        current.queued_events = in_.u32();
        current.pending_timers = in_.u32();
        const std::string prefix = "instance " + std::to_string(current.id) + ": ";
        if (!in_.ok()) return failure("truncated instance");
        if (current.id == 0) return failure("instance ID 0");
        if (current.tick < 1 || (current.tick != completed_tick_ && current.tick != completed_tick_ + 1) ||
            current.time_tick + 1 != current.tick) {
            return failure(prefix + "instance tick does not match the completed tick");
        }
        current.random = RandomStream(shared_.config.seed, current.tick, current.id, draws);

        // Every count is bounded by its quota and the input before anything is
        // decoded for it; items are appended as they decode.
        const Quotas& quotas = shared_.config.quotas;
        const std::uint32_t slot_count = in_.count(22);
        if (slot_count > quotas.thread_slots) return failure(prefix + "thread slot quota exceeded");
        std::uint64_t queued = 0;
        for (std::uint32_t slot_index = 0; slot_index < slot_count; ++slot_index) {
            ThreadSlot* appended = in_.append(current.slots);
            if (appended == nullptr) return failure(prefix + "truncated instance");
            ThreadSlot& slot = *appended;
            const std::uint8_t live = in_.u8();
            const std::uint8_t started = in_.u8();
            if (live > 1 || started > 1) return failure(prefix + "thread slot flags");
            slot.live = live == 1;
            slot.started = started == 1;
            slot.thread_reference = in_.i32();
            slot.function_reference = in_.i32();
            slot.parameter_reference = in_.i32();
            slot.values_reference = in_.i32();
            for (std::deque<int>* queue : {&slot.callbacks, &slot.parameters}) {
                const std::uint32_t size = in_.count(4);
                if (size > quotas.queued_events - queued) return failure(prefix + "event, registration or timer quota exceeded");
                for (std::uint32_t index = 0; index < size && in_.charge(sizeof(int)); ++index) queue->push_back(in_.i32());
                queued += size;
            }
        }
        const std::uint32_t name_count = in_.count(8);
        std::uint64_t registered = 0;
        std::set<std::uint64_t> handler_ids;
        for (std::uint32_t index = 0; index < name_count && in_.charge(sizeof(std::string) + sizeof(std::vector<Handler>)); ++index) {
            std::string name = in_.text();
            const std::uint32_t count = in_.count(12);
            if (count > quotas.registrations - registered) return failure(prefix + "event, registration or timer quota exceeded");
            std::vector<Handler> handlers;
            for (std::uint32_t handler_index = 0; handler_index < count; ++handler_index) {
                Handler* handler = in_.append(handlers);
                if (handler == nullptr) return failure(prefix + "truncated instance");
                handler->id = in_.u64();
                handler->function_reference = in_.i32();
                if (handler->id == 0 || handler->id >= current.next_handler_id || !handler_ids.insert(handler->id).second) {
                    return failure(prefix + "handler ID");
                }
            }
            registered += count;
            if (!current.handlers.emplace(std::move(name), std::move(handlers)).second) return failure(prefix + "duplicate event name");
        }
        Staging& staging = current.staging;
        const std::uint32_t command_count = in_.count(32);
        for (std::uint32_t index = 0; index < command_count; ++index) {
            ScriptCommand* command = in_.append(staging.commands);
            if (command == nullptr) return failure(prefix + "truncated instance");
            command->tick = in_.u64();
            command->issuer = in_.u64();
            command->sequence = in_.u64();
            command->verb = in_.text();
            if (!read_values(in_, command->arguments)) return failure(prefix + "staged command");
        }
        // Staged timers count towards the pending timer quota.
        for (std::vector<ScriptEvent>* events : {&staging.posts, &staging.timers}) {
            const std::uint32_t limit = events == &staging.timers ? quotas.pending_timers : UINT32_MAX;
            const std::uint32_t event_count = in_.count(min_event_bytes);
            if (event_count > limit) return failure(prefix + "event, registration or timer quota exceeded");
            for (std::uint32_t index = 0; index < event_count; ++index) {
                ScriptEvent* event = in_.append(*events);
                if (event == nullptr) return failure(prefix + "truncated instance");
                if (!read_event(in_, *event) || !valid_producer(event->key.producer)) return failure(prefix + "staged event");
            }
        }
        const std::uint32_t diagnostic_count = in_.count(32);
        for (std::uint32_t index = 0; index < diagnostic_count; ++index) {
            ScriptDiagnostic* diagnostic = in_.append(staging.diagnostics);
            if (diagnostic == nullptr) return failure(prefix + "truncated instance");
            diagnostic->code = in_.text();
            diagnostic->message = in_.text();
            diagnostic->tick = in_.u64();
            diagnostic->instance = in_.u64();
            diagnostic->thread = in_.i64();
        }
        if (!in_.ok()) return failure(prefix + "truncated instance");
        if (queued != current.queued_events || registered != current.registrations) {
            return failure(prefix + "event queue or registration counts do not match");
        }
        if (current.queued_events > quotas.queued_events || current.registrations > quotas.registrations ||
            current.pending_timers > quotas.pending_timers) {
            return failure(prefix + "event, registration or timer quota exceeded");
        }

        std::string error;
        sf::RestoreContext context;
        context.functions = &functions_;
        context.limits.identities = quotas.identities;
        context.limits.memory_bytes = quotas.memory_bytes;
        context.binding_count = static_cast<std::uint32_t>(shared_.bindings.size());
        context.module_source = [&](std::string_view chunk_name) -> const std::string* {
            return chunk_name.size() > 1 && chunk_name.front() == '@' ? shared_.manifest.find(chunk_name.substr(1)) : nullptr;
        };
        current.sandbox = sf::restore_graph(in_, context, error);
        if (current.sandbox == nullptr) return failure(prefix + error);
        sf::lua_atpanic(current.sandbox->state(), abort_session);
        if (!check_references(current)) return failure(prefix + "a host reference does not match the Lua registry");
        return core::Result<std::unique_ptr<Instance>>::success(std::move(instance));
    }

private:
    static core::Result<std::unique_ptr<Instance>> failure(std::string message) {
        return core::Result<std::unique_ptr<Instance>>::failure(make_error(codes::load_rejected, std::move(message)));
    }

    static bool check_references(Instance& instance) {
        using sf::ReferenceKind;
        sf::Sandbox& sandbox = *instance.sandbox;
        for (ThreadSlot& slot : instance.slots) {
            if (!slot.live) {
                if (slot.thread_reference != LUA_NOREF || slot.function_reference != LUA_NOREF ||
                    slot.parameter_reference != LUA_NOREF || slot.values_reference != LUA_NOREF || !slot.callbacks.empty() ||
                    !slot.parameters.empty()) {
                    return false;
                }
                continue;
            }
            if (!reference_ok(sandbox, slot.thread_reference, ReferenceKind::thread, false, false) ||
                !reference_ok(sandbox, slot.function_reference, ReferenceKind::any, false, true) ||
                !reference_ok(sandbox, slot.parameter_reference, ReferenceKind::any, true, true) ||
                !reference_ok(sandbox, slot.values_reference, ReferenceKind::table, true, false)) {
                return false;
            }
            for (const std::deque<int>* queue : {&slot.callbacks, &slot.parameters}) {
                for (const int reference : *queue) {
                    if (!reference_ok(sandbox, reference, ReferenceKind::any, false, true)) return false;
                }
            }
            slot.thread = sf::registry_thread(sandbox, slot.thread_reference);
        }
        std::vector<int> held;
        const auto hold = [&](int reference) {
            if (reference > 0) held.push_back(reference);
        };
        for (const ThreadSlot& slot : instance.slots) {
            for (const int reference : {slot.thread_reference, slot.function_reference, slot.parameter_reference, slot.values_reference}) {
                hold(reference);
            }
            for (const int reference : slot.callbacks) hold(reference);
            for (const int reference : slot.parameters) hold(reference);
        }
        for (const auto& [name, handlers] : instance.handlers) {
            for (const Handler& handler : handlers) {
                if (!reference_ok(sandbox, handler.function_reference, ReferenceKind::function, false, false)) return false;
                hold(handler.function_reference);
            }
        }
        return sf::registry_references_consistent(sandbox, held);
    }

    ByteReader& in_;
    const Shared& shared_;
    const sf::CFunctionTable& functions_;
    std::uint64_t completed_tick_;
};

} // namespace
} // namespace detail

core::Result<std::string> ScriptScheduler::save() {
    if (auto usable = impl_->check_usable(); !usable) return core::Result<std::string>::failure(usable.error());
    const auto refuse = [](std::string message) {
        return core::Result<std::string>::failure(detail::make_error(codes::save_refused, std::move(message)));
    };
    std::string error;
    const detail::sf::CFunctionTable* functions = detail::function_table(error);
    if (functions == nullptr) return refuse(error);
    detail::ByteWriter out;
    detail::write_identity(out);
    detail::write_config(out, impl_->shared->config);
    out.text(impl_->shared->manifest.digest());
    detail::write_bindings(out, impl_->shared->bindings);
    out.u64(impl_->completed_tick);
    out.u32(static_cast<std::uint32_t>(impl_->pending.size()));
    for (const auto& [key, event] : impl_->pending) detail::write_event(out, event);
    out.u32(static_cast<std::uint32_t>(impl_->instances.size()));
    for (const auto& [id, instance] : impl_->instances) {
        if (!instance->inbox.empty() || instance->current_thread != -1 || instance->exit_requested ||
            instance->outcome != InstanceOutcome::running) {
            return refuse("instance " + std::to_string(id) + " is not at a tick barrier");
        }
        detail::write_instance(out, *instance);
        if (!detail::sf::save_graph(*instance->sandbox, *functions, out, error)) {
            return refuse("instance " + std::to_string(id) + ": " + error);
        }
    }
    // submit_event admits no deeper value; this guards the other writers.
    if (!out.ok()) return refuse("a host value nests lists deeper than " + std::to_string(max_value_list_depth));
    return core::Result<std::string>::success(std::move(out.bytes()));
}

// An allocation failure while decoding rejects the save like any other defect;
// the scheduler is only touched after the whole save decoded.
core::Result<void> ScriptScheduler::load(std::string_view bytes) try {
    std::string error;
    const detail::sf::CFunctionTable* functions = detail::function_table(error);
    if (functions == nullptr) return detail::reject(error);
    detail::ByteReader in(bytes);
    const auto expect = [&](const auto& write, std::string_view what) {
        detail::ByteWriter expected;
        write(expected);
        in.expect(expected.bytes());
        return in.ok() ? core::Result<void>::success() : detail::reject(std::string(what));
    };
    if (auto checked = expect([](detail::ByteWriter& out) { detail::write_identity(out); },
                              "not a script save of this format, policy, random stream or numeric ABI");
        !checked) {
        return checked;
    }
    const detail::Shared& shared = *impl_->shared;
    if (auto checked = expect([&](detail::ByteWriter& out) { detail::write_config(out, shared.config); },
                              "the save belongs to a session with other settings or quotas");
        !checked) {
        return checked;
    }
    if (auto checked = expect([&](detail::ByteWriter& out) { out.text(shared.manifest.digest()); },
                              "the save belongs to a session with other modules");
        !checked) {
        return checked;
    }
    if (auto checked = expect([&](detail::ByteWriter& out) { detail::write_bindings(out, shared.bindings); },
                              "the save belongs to a session with other bindings");
        !checked) {
        return checked;
    }

    const std::uint64_t completed_tick = in.u64();
    if (!in.ok() || completed_tick >= max_script_tick) return detail::reject("completed tick");
    std::map<EventKey, ScriptEvent> pending;
    const std::uint32_t pending_count = in.count(detail::min_event_bytes);
    for (std::uint32_t index = 0; index < pending_count; ++index) {
        if (!in.charge(sizeof(ScriptEvent) + sizeof(EventKey))) return detail::reject("pending event");
        ScriptEvent event;
        if (!detail::read_event(in, event) || !detail::valid_producer(event.key.producer) || event.key.tick <= completed_tick) {
            return detail::reject("pending event");
        }
        const EventKey key = event.key;
        if (!pending.emplace(key, std::move(event)).second) return detail::reject("duplicate pending event key");
    }
    std::map<std::uint64_t, std::unique_ptr<detail::Instance>> instances;
    const std::uint32_t instance_count = in.count(detail::min_instance_bytes);
    for (std::uint32_t index = 0; index < instance_count; ++index) {
        if (!in.charge(sizeof(detail::Instance))) return detail::reject("truncated save or trailing bytes");
        detail::InstanceReader reader(in, shared, *functions, completed_tick);
        auto instance = reader.read();
        if (!instance) return core::Result<void>::failure(instance.error());
        const std::uint64_t id = instance.value()->id;
        if (!instances.empty() && id <= instances.rbegin()->first) return detail::reject("instances out of order");
        instances.emplace(id, std::move(instance).value());
    }
    if (!in.ok() || !in.at_end()) return detail::reject("truncated save or trailing bytes");
    // Timer accounting: staged timers plus pending timer events per instance.
    for (const auto& [id, instance] : instances) {
        std::uint64_t timers = instance->staging.timers.size();
        for (const auto& [key, event] : pending) {
            if (key.producer == producer_script_timer && event.target == id) ++timers;
        }
        if (timers != instance->pending_timers) return detail::reject("instance " + std::to_string(id) + ": timer count");
    }

    // Publish: nothing above touched the scheduler.
    impl_->instances = std::move(instances);
    impl_->pending = std::move(pending);
    impl_->completed_tick = completed_tick;
    impl_->aborted = false;
    return core::Result<void>::success();
} catch (const std::bad_alloc&) {
    return detail::reject("not enough memory to decode the save");
}

core::Result<std::string> ScriptScheduler::state_hash() {
    auto bytes = save();
    if (!bytes) return core::Result<std::string>::failure(bytes.error());
    std::string framed(state_hash_identity);
    framed.push_back('\0');
    framed += bytes.value();
    return core::Result<std::string>::success(
        core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(framed.data()), framed.size())));
}

std::string authoritative_state_sha256(
    const std::uint64_t completed_tick,
    const std::string_view world_state_sha256,
    const std::string_view script_state_sha256
) {
    detail::ByteWriter out;
    out.text(authoritative_state_identity);
    out.u64(completed_tick);
    out.text(world_state_sha256);
    out.text(script_state_sha256);
    const std::string& bytes = out.bytes();
    return core::sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
}

} // namespace eawr::script::authoritative
