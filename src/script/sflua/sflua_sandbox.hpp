#pragma once

// Authoritative Lua sandbox (#247, docs/lua-sandbox.md): the library allow-list,
// canonical iteration, protected bindings, per-state object identities and the
// instruction budget of one Lua state of the authoritative profile.
//
// The sandbox enforces its table-write rules from inside the VM (the upstream
// table writes and thread creation are interposed in upstream/ltable.cpp and
// upstream/lapi.cpp). The rules apply only while a Sandbox is active on the
// calling thread, which the owner arranges with Sandbox::Scope around every call
// into the state; a state is used by one thread at a time.

#include "sflua.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

struct SandboxLimits {
    // Tables, functions, threads and userdata that received an identity (as a
    // table key or through tostring) stay alive for the instance lifetime.
    ::std::uint64_t identities = 65'536; // at most 2^30
    // Instructions between two budget checks of the count hook.
    int hook_granularity = 128;
    // Logical memory quota: bytes of Lua objects by the fixed sizes of
    // docs/lua-sandbox.md, not the host allocator's usage.
    ::std::uint64_t memory_bytes = ::std::uint64_t{64} << 20;
};

enum class SandboxFault : ::std::uint8_t {
    none,
    instruction_budget,
    identity_quota,
    logical_quota,
    memory_quota,
};

// Opaque host object handle (for example a game object): ordered as a table key
// by (kind, id), interned per state so equal handles are one Lua value.
struct HostHandle {
    ::std::uint32_t kind{};
    ::std::uint64_t id{};
};

class Sandbox {
public:
    struct Impl;

    // Opens a state with the allow-listed libraries. Returns nullptr when
    // allocation fails.
    [[nodiscard]] static ::std::unique_ptr<Sandbox> open(const SandboxLimits& limits);
    // True when open() installs a global of this name (the base, coroutine,
    // string and table libraries and the sandbox's replacements). Hosts must
    // not bind over one.
    [[nodiscard]] static bool installs_global(::std::string_view name);
    ~Sandbox();

    Sandbox(const Sandbox&) = delete;
    Sandbox& operator=(const Sandbox&) = delete;

    [[nodiscard]] lua_State* state() const noexcept;

    // Makes this sandbox the active one of the calling thread for its lifetime.
    class Scope {
    public:
        explicit Scope(Sandbox& sandbox) noexcept;
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        Impl* previous_;
    };

    // Instruction budget of the current operation. Pairs, next and sort charge
    // their C-side work to it too (docs/lua-sandbox.md, costs).
    void set_budget(::std::int64_t instructions) noexcept;
    [[nodiscard]] ::std::int64_t budget() const noexcept;
    [[nodiscard]] SandboxFault fault() const noexcept;

    // Protects every current global of the state and freezes the library tables.
    // Call after the host has registered its bindings, before any script runs.
    void seal();
    // Makes the table at index read-only for scripts.
    void freeze_table(int index);

    // Records a sticky fault and raises it as a Lua error in `thread`: every
    // later instruction of the state raises again, so pcall cannot absorb it.
    void raise_fault(lua_State* thread, SandboxFault fault);

    // Loads source text as a function onto the stack (0) or leaves an error
    // message (nonzero). Precompiled chunks are rejected.
    [[nodiscard]] int load_source(::std::string_view bytes, ::std::string_view chunk_name);

    // Pushes an array (1..n) of the keys of the table at index, in canonical
    // order, and returns true; returns false (pushing nothing) when a key has no
    // canonical order. Host traversal: charges nothing and never raises.
    [[nodiscard]] bool push_canonical_keys(int index);

    // Pushes the interned userdata of a handle onto `thread` (the state or one
    // of its coroutines), or reads one from its stack.
    void push_handle(lua_State* thread, HostHandle handle);
    // Before seal(): makes `index` the `__index` of every handle, so a host can
    // resolve methods on handles. The function must be a registered C function.
    void set_handle_index(lua_CFunction index);
    [[nodiscard]] bool to_handle(lua_State* thread, int index, HostHandle& handle) const;

    [[nodiscard]] ::std::uint64_t identity_count() const noexcept;

    // Logical memory (docs/lua-sandbox.md): the reachable graph's size at the
    // last measurement plus what scripts created since. Exceeding the quota
    // is a sticky fault.
    [[nodiscard]] ::std::uint64_t memory_measured() const noexcept;
    [[nodiscard]] ::std::uint64_t memory_charged() const noexcept;
    // At a tick barrier: measures the reachable graph when enough was charged
    // since the last measurement (or a thread's stack grew past the
    // allowance) and records the quota fault when the measurement exceeds it.
    void settle_memory();
    // Around host code that keeps C++ objects alive across Lua API calls
    // (binding bodies): a quota fault is recorded but not raised inside, so
    // the caller raises it once those objects are gone.
    void begin_host_call() noexcept;
    void end_host_call() noexcept;

private:
    friend struct PersistAccess; // sflua_persist.cpp
    explicit Sandbox(::std::unique_ptr<Impl> impl) noexcept;
    ::std::unique_ptr<Impl> impl_;
};

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
