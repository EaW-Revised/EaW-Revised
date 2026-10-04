#pragma once

// Internals shared by the authoritative sandbox and persistence translation
// units. Include every standard header first: sflua_upstream.hpp
// redefines C library names for the upstream sources.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sflua_metering.hpp"
#include "sflua_persist.hpp"
#include "sflua_sandbox.hpp"
#include "sflua_upstream.hpp"

EAWR_SFLUA_BEGIN
#include "ldebug.h"
#include "lobject.h"
#include "lstate.h"
#include "ltable.h"
EAWR_SFLUA_END

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

// Upstream table writes without the sandbox rules (upstream/ltable.cpp).
TObject* luaH_set_upstream(lua_State* state, Table* table, const TObject* key);
TObject* luaH_setnum_upstream(lua_State* state, Table* table, int key);

// lauxlib.c keeps its table.getn/setn size table at this registry index.
inline constexpr int lauxlib_sizes_reference = 2;

// Logical sizes of the memory quota (docs/lua-sandbox.md): the same on every
// target and independent of table and stack capacities.
namespace logical_size {
inline constexpr ::std::uint64_t string = 32;             // plus the length
inline constexpr ::std::uint64_t table = 64;
inline constexpr ::std::uint64_t table_entry = 32;        // per non-nil value
inline constexpr ::std::uint64_t closure = 32;
inline constexpr ::std::uint64_t upvalue_slot = 8;        // a Lua closure's slot
inline constexpr ::std::uint64_t upvalue = 32;            // an upvalue object
inline constexpr ::std::uint64_t c_upvalue = 16;          // a C closure's value
inline constexpr ::std::uint64_t userdata = 40;           // plus the payload
inline constexpr ::std::uint64_t thread = 8192;           // includes the stack allowance
inline constexpr ::std::ptrdiff_t stack_allowance = 512;  // slots
inline constexpr ::std::uint64_t stack_slot = 16;         // per slot above the allowance
inline constexpr ::std::uint64_t prototype = 128;
inline constexpr ::std::uint64_t prototype_instruction = 8;  // with its line info
inline constexpr ::std::uint64_t prototype_constant = 16;
inline constexpr ::std::uint64_t prototype_child = 8;
inline constexpr ::std::uint64_t prototype_local = 16;
inline constexpr ::std::uint64_t prototype_upvalue_name = 8;
} // namespace logical_size

struct HandlePayload {
    ::std::uint32_t kind;
    ::std::uint32_t reserved;
    ::std::uint64_t id;
};

struct Sandbox::Impl {
    lua_State* state{};
    SandboxLimits limits;
    ::std::int64_t budget{};
    SandboxFault fault{SandboxFault::none};
    int identity_suspended{};
    ::std::unordered_map<const void*, ::std::uint64_t> identities;
    ::std::uint64_t next_identity{1};
    Table* anchors{};
    Table* handles{};
    Table* handle_metatable{};
    Table* sizes{};
    Table* globals{};
    ::std::vector<const Table*> frozen;
    ::std::unordered_set<const void*> protected_names;
    // Scratch tables of the running table.sort calls, innermost last.
    ::std::vector<const Table*> sort_scratch;
    ::std::vector<TObject> scratch_keys;
    // Prototype path (child indices from the chunk's main function) of every
    // prototype the state compiled. Every prototype comes from load_source,
    // which records its tree, so an address reused by a later compile is
    // recorded again; entries of collected prototypes are never looked up.
    ::std::unordered_map<const Proto*, ::std::vector<::std::uint32_t>> prototype_paths;
    // Logical memory: the last measurement and the bytes charged since.
    ::std::uint64_t memory_measured{};
    ::std::uint64_t memory_charged{};
    int memory_suspended{};
    int host_calls{};
    // Stack high-water marks (slots) since the last measurement of the threads
    // that went past the allowance.
    ::std::unordered_map<const lua_State*, ::std::ptrdiff_t> deep_stacks;
};

inline bool is_reference(const TObject* value) noexcept {
    switch (ttype(value)) {
    case LUA_TTABLE:
    case LUA_TFUNCTION:
    case LUA_TUSERDATA:
    case LUA_TTHREAD:
        return true;
    default:
        return false;
    }
}

inline bool is_handle(const Sandbox::Impl& sandbox, const TObject* value) noexcept {
    return ttisuserdata(value) && uvalue(value)->uv.metatable == sandbox.handle_metatable &&
        sandbox.handle_metatable != nullptr;
}

inline const HandlePayload& handle_payload(const TObject* value) noexcept {
    return *reinterpret_cast<const HandlePayload*>(uvalue(value) + 1);
}

inline ::std::uint64_t find_identity(const Sandbox::Impl& sandbox, const TObject* value) noexcept {
    const auto found = sandbox.identities.find(gcvalue(value));
    return found == sandbox.identities.end() ? 0 : found->second;
}

// Canonical key order: false, true, numbers ascending, strings bytewise, host
// handles by (kind, id), then other references by identity.
inline int key_rank(const Sandbox::Impl& sandbox, const TObject* value) noexcept {
    switch (ttype(value)) {
    case LUA_TBOOLEAN:
        return 0;
    case LUA_TNUMBER:
        return 1;
    case LUA_TSTRING:
        return 2;
    default:
        return is_handle(sandbox, value) ? 3 : 4;
    }
}

inline bool key_less(const Sandbox::Impl& sandbox, const TObject* left, const TObject* right) noexcept {
    const int left_rank = key_rank(sandbox, left);
    const int right_rank = key_rank(sandbox, right);
    if (left_rank != right_rank) return left_rank < right_rank;
    switch (left_rank) {
    case 0:
        return bvalue(left) == 0 && bvalue(right) != 0;
    case 1:
        return nvalue(left) < nvalue(right);
    case 2: {
        const TString* a = tsvalue(left);
        const TString* b = tsvalue(right);
        const ::std::size_t common = ::std::min(a->tsv.len, b->tsv.len);
        const int order = ::std::memcmp(getstr(a), getstr(b), common);
        return order != 0 ? order < 0 : a->tsv.len < b->tsv.len;
    }
    case 3: {
        const HandlePayload& a = handle_payload(left);
        const HandlePayload& b = handle_payload(right);
        return a.kind != b.kind ? a.kind < b.kind : a.id < b.id;
    }
    default:
        return find_identity(sandbox, left) < find_identity(sandbox, right);
    }
}

// A key is orderable when it has a place in the canonical order.
inline bool orderable(const Sandbox::Impl& sandbox, const TObject* value) noexcept {
    if (ttisnil(value) || ttislightuserdata(value)) return false;
    if (!is_reference(value) || is_handle(sandbox, value)) return true;
    return find_identity(sandbox, value) != 0;
}

// Collects the keys of `table` into scratch_keys in canonical order; false
// when one has no canonical order.
bool collect_canonical_keys(Sandbox::Impl& sandbox, const Table* table);

// The hook that enforces the instruction budget (count events) and charges
// stack growth past the allowance (call events), and its mask.
lua_Hook sandbox_count_hook() noexcept;
inline constexpr int sandbox_hook_mask = LUA_MASKCOUNT | LUA_MASKCALL;

// Logical size of the graph reachable from the registry and the main thread,
// over the edges the canonical save follows (sflua_persist_graph.cpp).
::std::uint64_t measure_logical_bytes(const Sandbox::Impl& sandbox);

// Records the prototype paths of a freshly compiled chunk.
void record_prototypes(Sandbox::Impl& sandbox, const Proto* root);

// The sandbox's own C functions with their stable names.
void append_sandbox_functions(::std::vector<CFunctionEntry>& out);

namespace sandbox_internal {

extern thread_local Sandbox::Impl* active_sandbox;
Sandbox::Impl* active_for(lua_State* state) noexcept;
Sandbox::Impl& require_active(lua_State* state);
bool string_equals(const TObject* value, ::std::string_view text) noexcept;
void raise_fault(Sandbox::Impl& sandbox, lua_State* state, SandboxFault fault);
::std::uint64_t ensure_identity(Sandbox::Impl& sandbox, lua_State* state, const TObject* value);
void charge_memory(Sandbox::Impl& sandbox, lua_State* state, ::std::uint64_t bytes);
void check_memory(Sandbox::Impl& sandbox, lua_State* state, ::std::uint64_t bytes);
void charge_stack(Sandbox::Impl& sandbox, lua_State* state);
void charge(Sandbox::Impl& sandbox, lua_State* state, sflua_metering::StepUnits units);
sflua_metering::StepUnits sort_units(::std::size_t count) noexcept;
void count_hook(lua_State* state, lua_Debug* activation);
bool is_frozen(const Sandbox::Impl& sandbox, const Table* table) noexcept;
bool is_internal(const Sandbox::Impl& sandbox, const Table* table) noexcept;
void check_table_write(Sandbox::Impl& sandbox, lua_State* state, const Table* table);
void store_keys(Sandbox::Impl& sandbox, lua_State* state, Table* keys);
int sandbox_pairs(lua_State* state);
int sandbox_next(lua_State* state);
int sandbox_foreach(lua_State* state);
int sandbox_sort(lua_State* state);
int sandbox_insert(lua_State* state);
int sandbox_remove(lua_State* state);
int sandbox_concat(lua_State* state);
int sandbox_foreachi(lua_State* state);
int sandbox_rep(lua_State* state);
int sandbox_tostring(lua_State* state);
int sandbox_collectgarbage(lua_State* state);
bool stored_size_on_top(lua_State* state);
void set_field_function(lua_State* state, int table, const char* name, lua_CFunction function);
void set_forbidden(lua_State* state, int table, const char* name, const char* shown_name);
void install_metered_patterns(lua_State* state);
Table* new_registry_table(lua_State* state, const char* name);

} // namespace sandbox_internal

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
