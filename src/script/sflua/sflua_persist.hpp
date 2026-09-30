#pragma once

// Canonical persistence of one authoritative Lua state (#248,
// docs/lua-persistence.md): the reachable object graph in canonical order,
// with prototypes named by module and prototype path and C functions by
// stable names, restored into a fresh inactive state without running any
// script code.

#include "persist_bytes.hpp"
#include "sflua_sandbox.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

// The upvalues a C function expects. A restore checks them, so a corrupted
// save cannot hand a C function values it would misuse.
enum class UpvalueShape : ::std::uint8_t {
    none,
    name,           // one string (forbidden functions, binding objects)
    function,       // one function (table.sort's upstream sort)
    pairs_cursor,   // table, key array, position, count
    gfind_cursor,   // subject, pattern, position
    coroutine,      // one thread (coroutine.wrap)
    binding_index,  // one integer below the session's binding count
    bound_method,   // a binding index and the host handle it is bound to
};

struct CFunctionEntry {
    ::std::string name;
    lua_CFunction function{};
    UpvalueShape shape{UpvalueShape::none};
};

// Stable names of every C function a sandbox state can hold: the libraries,
// the sandbox's replacements and the host's functions.
class CFunctionTable {
public:
    // Fails when a name or a function pointer appears twice (a linker that
    // folds identical functions would make two names indistinguishable).
    [[nodiscard]] static bool create(::std::vector<CFunctionEntry> host, CFunctionTable& out, ::std::string& error);

    [[nodiscard]] const CFunctionEntry* find(lua_CFunction function) const noexcept;
    [[nodiscard]] const CFunctionEntry* find(::std::string_view name) const noexcept;

private:
    ::std::vector<CFunctionEntry> entries_; // sorted by name
};

// Appends the canonical encoding of the sandbox's reachable graph. The state
// must be at a tick barrier: no running call, an idle main thread, and every
// other thread unstarted, finished or suspended in coroutine.yield called from
// a Lua function. Reads only; allocates nothing inside the state.
[[nodiscard]] bool save_graph(Sandbox& sandbox, const CFunctionTable& functions, persist::ByteWriter& out, ::std::string& error);

struct RestoreContext {
    const CFunctionTable* functions{};
    SandboxLimits limits;
    ::std::uint32_t binding_count{};
    // Source bytes of a chunk name ("@PATH"), nullptr when the session has no
    // such module.
    ::std::function<const ::std::string*(::std::string_view chunk_name)> module_source;
};

// Reads one graph into a new inactive sandbox, or returns nullptr with the
// reason. Nothing runs: no chunk, initializer or binding installation.
[[nodiscard]] ::std::unique_ptr<Sandbox> restore_graph(persist::ByteReader& in, const RestoreContext& context, ::std::string& error);

// Registry lookups for the host's references after a restore.
enum class ReferenceKind : ::std::uint8_t { any, thread, function, table };
[[nodiscard]] bool registry_holds(Sandbox& sandbox, int reference, ReferenceKind kind);
[[nodiscard]] lua_State* registry_thread(Sandbox& sandbox, int reference);
// The host's registry references (positive ones) are distinct, off lauxlib's
// free list and below the next new reference, so luaL_ref can never hand one
// out again.
[[nodiscard]] bool registry_references_consistent(Sandbox& sandbox, const ::std::vector<int>& held);

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
