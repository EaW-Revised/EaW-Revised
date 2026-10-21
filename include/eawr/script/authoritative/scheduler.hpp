#pragma once

// Authoritative script scheduler (#247, docs/lua-sandbox.md): sandboxed script
// instances serviced once per simulation tick. Each instance owns one Lua
// state of the authoritative numeric profile; its service reads only its own
// state and the tick's inputs and writes only its own staging buffer, so the
// default instance service runs partitioned over the executor (ADR-009) and an
// ordered serial commit publishes commands and events in stable instance order.
// A bounded host can opt into the transactional native action phase below.

#include "eawr/core/result.hpp"
#include "eawr/script/authoritative/clock_rng.hpp"
#include "eawr/script/numeric/lua_number.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace eawr::sim {
class PartitionExecutor;
}

namespace eawr::script::authoritative {

inline constexpr std::string_view sandbox_policy_identity = "eawr-lua-sandbox-v3";

// The instance service's executor phase (docs/simulation.md, phase map).
inline constexpr std::string_view script_phase_name = "script-instances";

namespace codes {
inline constexpr std::string_view protected_binding = "EAWR-SCRIPT-0201";
inline constexpr std::string_view weak_or_finalizer = "EAWR-SCRIPT-0202";
inline constexpr std::string_view forbidden_capability = "EAWR-SCRIPT-0203";
inline constexpr std::string_view identity_quota = "EAWR-SCRIPT-0204";
inline constexpr std::string_view instruction_budget = "EAWR-SCRIPT-0205";
inline constexpr std::string_view impure_comparator = "EAWR-SCRIPT-0206";
inline constexpr std::string_view module_rejected = "EAWR-SCRIPT-0207";
inline constexpr std::string_view logical_quota = "EAWR-SCRIPT-0208";
inline constexpr std::string_view session_abort = "EAWR-SCRIPT-0209";
inline constexpr std::string_view invalid_request = "EAWR-SCRIPT-0210";
inline constexpr std::string_view script_error = "EAWR-SCRIPT-0211";
inline constexpr std::string_view save_refused = "EAWR-SCRIPT-0212";
inline constexpr std::string_view load_rejected = "EAWR-SCRIPT-0213";
inline constexpr std::string_view command_unroutable = "EAWR-SCRIPT-0214";
// L-16: a known engine API the host does not implement was called (#79). A binding
// fails with this code; the call stops and the diagnostic carries it.
inline constexpr std::string_view missing_api = "EAWR-SCRIPT-0215";
// A known engine API ran with a documented stand-in result because the host
// does not model what it reads (#79, the #78 fallback record). The call goes on.
inline constexpr std::string_view unsupported_api = "EAWR-SCRIPT-0216";
// A sourced nonfatal Lua API warning. The call and object service continue.
inline constexpr std::string_view script_warning = "EAWR-SCRIPT-0217";
} // namespace codes

// Persistence and state hashing (#248, docs/lua-persistence.md). A changed
// encoding rule needs a new identity.
inline constexpr std::string_view save_format_identity = "eawr-script-save-v4";
// Galactic clock profile plus the shared proximity and object attachment state.
inline constexpr std::string_view galactic_save_format_identity = "eawr-script-galactic-save-v2";
inline constexpr std::string_view query_save_format_identity = "eawr-script-query-save-v1";
inline constexpr std::string_view state_hash_identity = "eawr-script-state-v1";
inline constexpr std::string_view authoritative_state_identity = "eawr-authoritative-state-v1";

struct Handle {
    std::uint32_t kind{};
    std::uint64_t id{}; // full-width host identity; object attachment registries impose their own limits
    friend bool operator==(const Handle&, const Handle&) = default;
};

// Host value crossing the binding boundary. Numbers are binary64 bits; lists
// become sequential Lua tables (1..n) and back.
struct Value {
    std::variant<std::monostate, bool, numeric::LuaNumber, std::string, Handle, std::vector<Value>> data;

    [[nodiscard]] static Value number(numeric::LuaNumber value) { return Value{value}; }
    [[nodiscard]] static Value text(std::string value) { return Value{std::move(value)}; }
};
using ValueList = std::vector<Value>;

// WGA-471/472: opt-in semantic release, through the atomic native-effect host.
struct ManagedHandlePolicy {
    std::uint32_t kind{};
    std::string inventory_query{};
    std::string release_action{};
    std::uint64_t interval{1}; // completed invocation domain, independent of wall time
};
struct ManagedHandleWork {
    std::uint64_t objects{}, edges{}, entries{}, host_values{}, censuses{}, released{};
    std::uint64_t nanoseconds{}, service_nanoseconds{}; // observation only, never saved
};

// Lists nest at most this deep in a host value (a flat list is depth 1): the
// save format's bound, so submit_event rejects deeper event values.
inline constexpr int max_value_list_depth = 64;

// A gameplay command a script issued. It is published in (issuer, sequence)
// order after the tick's script service; it never touches world storage during
// the script call.
struct ScriptCommand {
    std::uint64_t tick{};
    std::uint64_t issuer{};
    std::uint64_t sequence{};
    std::string verb;
    ValueList arguments;
};

// Canonical event ordering key: (tick, producer system, entity, producer sequence).
struct EventKey {
    std::uint64_t tick{};
    std::uint32_t producer{};
    std::uint64_t entity{};
    std::uint64_t sequence{};
    friend auto operator<=>(const EventKey&, const EventKey&) = default;
};

// Producer systems below 16 belong to the scheduler.
inline constexpr std::uint32_t producer_script_post = 1;
inline constexpr std::uint32_t producer_script_timer = 2;
inline constexpr std::uint32_t first_simulation_producer = 16;

struct ScriptEvent {
    enum class Kind : std::uint8_t {
        // Calls the handlers registered with Register_Event(name, f), with the
        // parameters as arguments (L-32 mutation restart).
        dispatch,
        // Appends the global function `name` and the parameter to the event
        // queues of thread slot `thread` (L-30/L-31).
        thread_signal,
        // The engine calls the global function `name` with the arguments
        // (#79). Nothing happens when the global is absent or not a function
        // (L-10); results are dropped.
        call,
        // The engine sets the global `name` to the parameter, nil when it has
        // none (#79).
        assign,
        // The engine pumps the instance's threads at this point of the
        // delivery (L-22). Only host-paced instances (ServiceOptions) need
        // it; the others are pumped every service.
        pump,
        // The engine creates a thread of the global function `name`
        // (L-20), as Create_Thread would.
        start_thread,
    };
    EventKey key;
    std::uint64_t target{};
    Kind kind{Kind::dispatch};
    std::string name;
    std::int64_t thread{-1};
    ValueList arguments;              // dispatch
    std::optional<Value> parameter;   // thread_signal; nullopt is a null parameter
};

struct Quotas {
    std::int64_t instructions_per_service = 4'000'000;
    std::uint64_t identities = 65'536;
    std::uint32_t thread_slots = 4'096;
    std::uint32_t queued_events = 65'536;
    std::uint32_t pending_timers = 65'536;
    std::uint32_t registrations = 65'536;
    // Logical bytes of Lua objects per instance (docs/lua-sandbox.md).
    std::uint64_t memory_bytes = std::uint64_t{64} << 20;
};

struct SessionConfig {
    std::uint64_t seed{};
    TickDuration tick_duration{};
    Quotas quotas{};
    // Module search directories after "./" (L-03), in order, e.g. "Data/Scripts/Library/".
    std::vector<std::string> script_directories;
    // Mode time is supplied at a barrier, independently of service/RNG ticks.
    // The GC save identity persists that mode frame; default saves are unchanged.
    bool galactic_clock{};
    // Synchronous query sessions receive nondecreasing 1/30-second mode time
    // independently of invocation count. Roots read the last supplied time (0
    // before service); timers use this time. Saves have a distinct identity.
    bool query_clock{};
};

// The session's pinned effective modules. Logical paths are compared after
// normalisation (ASCII upper case, '/' separators); ".." and absolute paths are
// rejected. require() and instance roots can load nothing else.
class ModuleManifest {
public:
    [[nodiscard]] core::Result<void> add(std::string_view logical_path, std::string bytes);
    [[nodiscard]] const std::string* find(std::string_view logical_path) const;
    // SHA-256 over the sorted (path, SHA-256 of bytes) list.
    [[nodiscard]] std::string digest() const;
    [[nodiscard]] static std::optional<std::string> normalise(std::string_view logical_path);

private:
    std::map<std::string, std::string, std::less<>> modules_;
};

class BindingContext;
// L-32: the callable stays in the calling instance's Lua registry. Only
// object/filter identities and the query extent reach the engine binding.
struct ProximityQuery {
    Handle object;
    numeric::LuaNumber extent{};
    std::optional<Handle> player;
    std::optional<Handle> type;
};
using ProximityCandidates = std::function<std::vector<Handle>(const ProximityQuery&)>;
// A host binding: arguments in, results out. A failure raises a Lua error with
// its message; the resume or handler that called it is rolled back.
using Binding = std::function<core::Result<ValueList>(BindingContext&, const ValueList&)>;

// Opt-in native effects (docs/lua-sandbox.md, immediate effects). A shadow owns
// every mutable dependency of its actions, including notifications and identity
// allocation. Clones share only immutable inputs; queries never mutate state.
class ImmediateEffectState {
public:
    virtual ~ImmediateEffectState() = default;
    [[nodiscard]] virtual std::unique_ptr<ImmediateEffectState> clone() const = 0;
    [[nodiscard]] virtual core::Result<ValueList> apply(std::string_view action, const ValueList&) = 0;
    [[nodiscard]] virtual core::Result<ValueList> query(std::string_view name, const ValueList&) const = 0;
};

class ImmediateEffectHost {
public:
    virtual ~ImmediateEffectHost() = default;
    [[nodiscard]] virtual core::Result<std::unique_ptr<ImmediateEffectState>> capture() const = 0;
    // Atomic publication: failure leaves the host unchanged. No Lua runs here.
    [[nodiscard]] virtual core::Result<void> publish(std::unique_ptr<ImmediateEffectState>) = 0;
};

// What a binding may do during a script call. Everything it stages belongs to
// the calling instance's current transaction.
class BindingContext {
public:
    virtual ~BindingContext() = default;
    [[nodiscard]] virtual std::uint64_t instance() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t tick() const noexcept = 0;
    // Current thread slot, -1 outside a thread.
    [[nodiscard]] virtual std::int64_t thread() const noexcept = 0;
    [[nodiscard]] virtual RandomStream& random() noexcept = 0;
    // The sequence the next issue_command gets. Sequences never repeat within an
    // instance, so a command's sequence can name what the command creates (#449).
    [[nodiscard]] virtual std::uint64_t command_sequence() const noexcept = 0;
    virtual void issue_command(std::string verb, ValueList arguments) = 0;
    // Runs against the ordered service's private shadow. A failed action also
    // discards its partial writes, even if Lua catches the binding error.
    [[nodiscard]] virtual core::Result<ValueList> apply_immediate(std::string_view action, const ValueList& arguments) = 0;
    [[nodiscard]] virtual core::Result<ValueList> query_immediate(std::string_view name, const ValueList& arguments) const = 0;
    // Posts an event to an instance (possibly this one), delivered from the next tick.
    virtual core::Result<void> post_event(ScriptEvent event) = 0;
    // Delivers `event` to this instance once `seconds` have elapsed
    // (ticks_until); its key is (deadline, timer producer, instance, registration).
    virtual core::Result<void> start_timer(numeric::LuaNumber seconds, ScriptEvent event) = 0;
    // Adds a diagnostic to this instance's service report; the call goes on.
    virtual void report(std::string_view code, std::string message) = 0;
    virtual core::Result<void> register_proximity(ProximityQuery query) = 0;
    virtual core::Result<void> cancel_proximity(Handle object) = 0;
    virtual core::Result<void> service_proximities(Handle object, const ProximityCandidates& candidates) = 0;
};

enum class InstanceOutcome : std::uint8_t {
    running,
    exited,   // _ScriptExit
    faulted,  // quota or budget: the instance is removed and its tick's output dropped
};

struct ScriptDiagnostic {
    std::string code;
    std::string message;
    std::uint64_t tick{};
    std::uint64_t instance{};
    std::int64_t thread{-1};
};

// Lua instructions one instance executed in one service: its script load.
struct InstanceLoad {
    std::uint64_t instance{};
    std::int64_t instructions{};
};

struct ServiceReport {
    std::uint64_t tick{};
    std::vector<ScriptCommand> commands;
    std::vector<ScriptDiagnostic> diagnostics; // ordered by instance, then occurrence
    std::vector<std::uint64_t> removed_instances;
    std::vector<InstanceLoad> loads; // every serviced instance, ascending ID
    ManagedHandleWork managed_handles{};
    std::vector<Handle> managed_releases{}; // ordered observation, never saved
};

// Per-service choices of the host. They are inputs of the tick, like its events.
struct ServiceOptions {
    // An in-tick owner defers these instances, including their callbacks,
    // until its next subset service. Fresh definitions can still run at a barrier.
    std::function<bool(std::uint64_t instance)> defer_instance{};
    // Instances whose threads the host paces: they are pumped only by pump
    // events, never after their events. Empty: every instance is pumped.
    std::function<bool(std::uint64_t instance)> host_paced;
    // Immediate-effect hosts only. Names the chain an instance runs in: its owner's. Instances of one chain run
    // in ascending ID over a shadow of their own, one chain after another in ascending key with key 0 (no owner)
    // last, and each chain publishes its changes before the next chain captures. Empty: one chain in ID order.
    std::function<std::uint64_t(std::uint64_t instance)> chain_of;
    // Select an owning host frontier before capturing each chain (zero selects the unowned tail).
    // A selected frontier always captures afresh, even when the preceding chain made no writes.
    // The returned token owns that selection through publication (and the last chain's census).
    // It is released before selecting another chain and on every failure path.
    std::function<core::Result<std::shared_ptr<void>>(std::uint64_t owner)> begin_chain;
    // GC profile: GetCurrentTime reads this mode frame at 1/30 s.
    // Absent, it reads the last set_galactic_frame value. Native timers and
    // random keys retain the scheduler's invocation tick domain.
    // The query profile requires time_tick on every invocation; its upstream
    // clock may advance while this scheduler receives no calls. Query timers
    // use upstream deadlines; elapsed timers precede the invocation's events.
    std::optional<std::uint64_t> time_tick{};
};

// SOS-40/44: lifecycle metadata is distinct from the Lua graph. An ended or
// rejected attachment stays recorded while its object lives, preventing restart.
struct ObjectScript {
    Handle object;
    std::uint64_t generation{};
    std::uint64_t instance{};
    std::string module;
    friend bool operator==(const ObjectScript&, const ObjectScript&) = default;
};

class ScriptScheduler {
public:
    [[nodiscard]] static core::Result<ScriptScheduler> create(SessionConfig config, ModuleManifest manifest);
    ScriptScheduler(ScriptScheduler&&) noexcept;
    ScriptScheduler& operator=(ScriptScheduler&&) noexcept;
    ~ScriptScheduler();

    // Registers a global binding (or `Table.member` on a binding object) for
    // every instance created afterwards.
    [[nodiscard]] core::Result<void> register_binding(std::string_view name, Binding binding);
    // Registers a method of the host handles of one kind: `handle.Method(...)`
    // calls the binding with the handle as its first argument (the retail
    // wrapper objects are called with '.', not ':'). Reading a name no
    // method has gives nil. Before the first instance, like bindings.
    [[nodiscard]] core::Result<void> register_method(std::uint32_t handle_kind, std::string_view method, Binding binding);

    // Opt in at a barrier, before root chunks that use immediate bindings.
    // Services native script instances in ascending ID order on the owner
    // thread. The host supplies a small explicit instance bound; world/entity
    // phases remain partitioned, as does the default scheduler profile.
    [[nodiscard]] core::Result<void> set_immediate_effect_host(
        std::shared_ptr<ImmediateEffectHost> host, std::size_t max_instances);
    [[nodiscard]] core::Result<void> register_managed_handles(ManagedHandlePolicy policy);
    // Retained host aliases (e.g. bridge commands), supplied at the barrier.
    void set_managed_handle_roots(std::function<ValueList()> roots);
    [[nodiscard]] ManagedHandleWork managed_handle_work() const noexcept;

    // Creates an instance from a manifest module, runs its chunk with the
    // service budget at the current tick, then seals its globals. Instance IDs
    // are stable and unique; a failing chunk leaves no instance.
    [[nodiscard]] core::Result<void> create_instance(std::uint64_t instance, std::string_view root_module);

    // Ordered structural commit of the host's partitioned attachment proposals.
    // Fresh initialization events run in the normal partitioned service, with
    // definitions before main creation and Object assignment (SOS-30/41).
    [[nodiscard]] core::Result<std::vector<ScriptDiagnostic>> reconcile_objects(std::vector<ObjectScript> objects);
    [[nodiscard]] std::vector<ObjectScript> object_scripts() const;

    // Queues an event from the simulation. Its key tick must be later than the
    // completed tick.
    [[nodiscard]] core::Result<void> submit_event(ScriptEvent event);

    // Services every instance for the next tick: delivers due events and timers
    // in key order, pumps threads, then commits in instance order. The instance
    // default phase runs partitioned on the executor; an immediate-effect host
    // instead runs its bounded script set in the ordered native action phase.
    [[nodiscard]] core::Result<ServiceReport> service(
        const sim::PartitionExecutor& executor, const ServiceOptions& options = {});

    // Object turn (#3644 A2a, #3702): services only the given object-script
    // instances inside the next tick, before that tick's barrier service(). Due
    // events (key tick up to the next tick) addressed to them are delivered, the
    // threads are pumped on the partitioned script-instances phase, and the
    // staged output is committed in ascending instance ID (a faulted instance
    // drops only its own output and is removed, as in service()). completed_tick
    // does not advance; a later service() of the same tick still runs. The turn
    // draws random numbers from its own key domain (turn_random_domain) on the
    // instance's one draw counter, so no key of the barrier service repeats, also
    // across save() and load() between the turn and the barrier. Commands and
    // posts continue the instance's sequences. Fails with EAWR-SCRIPT-0210,
    // before any state changes, for an unknown or non-object instance, a
    // duplicate ID, the GC or query clock profile, or an immediate-effect host.
    [[nodiscard]] core::Result<ServiceReport> service_turn(
        const sim::PartitionExecutor& executor, const std::vector<std::uint64_t>& instances);

    // The engine reads a global of an instance at the tick barrier (#79):
    // its value when it is nil, a boolean, a number, a string or a handle;
    // nullopt otherwise. Reads only.
    [[nodiscard]] core::Result<std::optional<Value>> read_global(std::uint64_t instance, std::string_view name);

    [[nodiscard]] std::uint64_t completed_tick() const noexcept;
    [[nodiscard]] bool uses_galactic_clock() const noexcept;
    [[nodiscard]] TickDuration tick_duration() const noexcept;
    [[nodiscard]] std::uint64_t galactic_frame() const noexcept;
    // Before engine preparation/attachment so root chunks see mode time too.
    [[nodiscard]] core::Result<void> set_galactic_frame(std::uint64_t frame);
    [[nodiscard]] std::vector<std::uint64_t> instances() const;
    // The thread slots of an instance at the tick barrier, in slot order: true for a live
    // thread (PL-42 reads this, #449). Reads only.
    [[nodiscard]] core::Result<std::vector<bool>> thread_slots(std::uint64_t instance) const;
    // Ends an instance at the tick barrier (PL-43, #449): it is removed
    // with its pending events and timers; no script code runs.
    [[nodiscard]] core::Result<void> remove_instance(std::uint64_t instance);
    // SHA-256 over the scheduler state and every instance's reachable global
    // data in canonical order (tables by key order, functions and threads by
    // identity): a test and diagnostics digest; state_hash() is authoritative.
    [[nodiscard]] std::string state_digest();

    // Canonical save of the whole script state at the tick barrier (between
    // services): session identity, pending events and timers, and every
    // instance's host state and reachable Lua graph. Reads only; runs
    // serially. Fails with EAWR-SCRIPT-0212 for a state it cannot represent.
    [[nodiscard]] core::Result<std::string> save();
    // Independent sandbox graphs are encoded on disjoint worker slots, then
    // appended in instance order; bytes and refusal order match save().
    [[nodiscard]] core::Result<std::string> save(const sim::PartitionExecutor& executor);
    // Replaces the script state with a save of this session (same config,
    // manifest and bindings). Validates everything before publishing; on any
    // failure (EAWR-SCRIPT-0213) the scheduler is unchanged. No script code
    // runs: no chunk, initializer or binding installation.
    [[nodiscard]] core::Result<void> load(std::string_view bytes);
    // Lowercase hex SHA-256 of the versioned script state (state_hash_identity
    // over the canonical save): every value that can affect future ticks.
    [[nodiscard]] core::Result<std::string> state_hash();
    [[nodiscard]] core::Result<std::string> state_hash(const sim::PartitionExecutor& executor);
    // Per-tick actual state digest: cache unchanged sandbox graphs, compose
    // current scheduler metadata. The full archive/state_hash stays available.
    [[nodiscard]] core::Result<std::string> sync_hash(bool verify_graphs = false);
    // Dirty graphs use disjoint fixed partitions, joined before returning.
    // No Lua mutation, save or hash consumer can overlap this capture.
    [[nodiscard]] core::Result<std::string> sync_hash(const sim::PartitionExecutor& executor, bool verify_graphs = false);
    [[nodiscard]] std::uint64_t sync_graph_encodes() const noexcept;

private:
    struct Impl;
    explicit ScriptScheduler(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

// The authoritative state hash of one completed tick: world and script state
// under one versioned, domain-separated encoding (lowercase hex SHA-256 in and
// out). Existing replay hashes are unchanged; this is the combined form for
// sessions that run scripts.
[[nodiscard]] std::string authoritative_state_sha256(
    std::uint64_t completed_tick,
    std::string_view world_state_sha256,
    std::string_view script_state_sha256
);

} // namespace eawr::script::authoritative
