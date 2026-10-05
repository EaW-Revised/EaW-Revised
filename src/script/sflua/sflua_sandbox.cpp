// Authoritative Lua sandbox (#247); rules in docs/lua-sandbox.md.
//
// Lua 5.0.2 raises errors with longjmp. Functions below that can raise keep no
// object with a destructor alive across the raising call; their scratch
// storage lives in the Sandbox.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sflua_metering.hpp"
#include "sflua_sandbox_internal.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

// Metered string.find, gfind and gsub (upstream/lstrlib.cpp).
lua_CFunction sflua_metered_find();
lua_CFunction sflua_metered_gfind();
lua_CFunction sflua_metered_gsub();

namespace sandbox_internal {

constexpr char handle_metatable_guard[] = "EAWR host handle";

thread_local Sandbox::Impl* active_sandbox = nullptr;

Sandbox::Impl* active_for(lua_State* state) noexcept {
    Sandbox::Impl* sandbox = active_sandbox;
    return sandbox != nullptr && G(sandbox->state) == G(state) ? sandbox : nullptr;
}

Sandbox::Impl& require_active(lua_State* state) {
    Sandbox::Impl* sandbox = active_for(state);
    if (sandbox == nullptr) luaL_error(state, "EAWR-SCRIPT-0203: sandbox function called outside its sandbox");
    return *sandbox;
}

bool string_equals(const TObject* value, ::std::string_view text) noexcept {
    const TString* string = tsvalue(value);
    return string->tsv.len == text.size() && ::std::memcmp(getstr(string), text.data(), text.size()) == 0;
}

void raise_fault(Sandbox::Impl& sandbox, lua_State* state, SandboxFault fault);

// Identity of a reference value, assigned on first use in creation order of
// uses. Anchored so the object (and therefore its address) outlives the use.
::std::uint64_t ensure_identity(Sandbox::Impl& sandbox, lua_State* state, const TObject* value) {
    const ::std::uint64_t existing = find_identity(sandbox, value);
    if (existing != 0 || sandbox.identity_suspended != 0) return existing;
    if (sandbox.next_identity > sandbox.limits.identities) raise_fault(sandbox, state, SandboxFault::identity_quota);
    const ::std::uint64_t identity = sandbox.next_identity++;
    sandbox.identities.emplace(gcvalue(value), identity);
    setobj2t(luaH_setnum_upstream(state, sandbox.anchors, static_cast<int>(identity)), value);
    return identity;
}

// Logical memory: scripts create objects through the interposed upstream
// entry points below, each charged before the object exists. Outside a
// protected call, or inside a binding body, the fault is recorded without
// raising; the sticky hook or the binding raises it at the next safe point.
void charge_memory(Sandbox::Impl& sandbox, lua_State* state, ::std::uint64_t bytes) {
    if (sandbox.fault != SandboxFault::none || sandbox.memory_suspended != 0) return;
    sandbox.memory_charged += bytes;
    if (sandbox.memory_measured + sandbox.memory_charged <= sandbox.limits.memory_bytes) return;
    if (state->errorJmp == nullptr || sandbox.host_calls != 0) {
        sandbox.fault = SandboxFault::memory_quota;
        lua_sethook(state, sandbox_count_hook(), LUA_MASKCOUNT, 1);
        return;
    }
    raise_fault(sandbox, state, SandboxFault::memory_quota);
}

// Checks a charge that follows without making it: when it would exceed the
// quota, it is made here instead (raising or recording the fault exactly as
// it would), before the work it pays for.
void check_memory(Sandbox::Impl& sandbox, lua_State* state, ::std::uint64_t bytes) {
    if (sandbox.fault != SandboxFault::none || sandbox.memory_suspended != 0) return;
    if (sandbox.memory_measured + sandbox.memory_charged + bytes <= sandbox.limits.memory_bytes) return;
    charge_memory(sandbox, state, bytes);
}

// A call event: charges the running thread's stack past the allowance, from
// its high-water mark since the last measurement.
void charge_stack(Sandbox::Impl& sandbox, lua_State* state) {
    const ::std::ptrdiff_t used = state->ci->top - state->stack;
    if (used <= logical_size::stack_allowance) return;
    const auto [mark, inserted] = sandbox.deep_stacks.try_emplace(state, logical_size::stack_allowance);
    if (used <= mark->second) return;
    const auto grown = static_cast<::std::uint64_t>(used - mark->second);
    mark->second = used;
    charge_memory(sandbox, state, grown * logical_size::stack_slot);
}

void charge(Sandbox::Impl& sandbox, lua_State* state, sflua_metering::StepUnits units) {
    if (sandbox.fault != SandboxFault::none) raise_fault(sandbox, state, sandbox.fault);
    sandbox.budget = sflua_metering::charged_budget(sandbox.budget, units);
    if (sandbox.budget < 0) raise_fault(sandbox, state, SandboxFault::instruction_budget);
}

sflua_metering::StepUnits sort_units(::std::size_t count) noexcept {
    sflua_metering::StepUnits log2 = 0;
    for (::std::size_t remaining = count; remaining != 0; remaining >>= 1) ++log2;
    return sflua_metering::saturating_multiply(static_cast<sflua_metering::StepUnits>(count), log2 + 1);
}

void count_hook(lua_State* state, lua_Debug* activation) {
    Sandbox::Impl* sandbox = active_for(state);
    if (sandbox == nullptr) return;
    if (activation->event == LUA_HOOKCALL) {
        if (sandbox->fault == SandboxFault::none) {
            charge_stack(*sandbox, state);
            return;
        }
    } else if (sandbox->fault == SandboxFault::none) {
        sandbox->budget = sflua_metering::charged_budget(
            sandbox->budget, static_cast<sflua_metering::StepUnits>(sandbox->limits.hook_granularity));
        if (sandbox->budget >= 0) return;
    }
    raise_fault(*sandbox, state, sandbox->fault == SandboxFault::none ? SandboxFault::instruction_budget : sandbox->fault);
}

// A fault is sticky: the thread's hook fires on every instruction from now on,
// so a script cannot catch it with pcall and continue.
void raise_fault(Sandbox::Impl& sandbox, lua_State* state, SandboxFault fault) {
    sandbox.fault = fault;
    lua_sethook(state, count_hook, LUA_MASKCOUNT, 1);
    if (fault == SandboxFault::identity_quota) {
        luaG_runerror(state, "EAWR-SCRIPT-0204: object identity quota exhausted");
    }
    if (fault == SandboxFault::logical_quota) luaG_runerror(state, "EAWR-SCRIPT-0208: logical quota exhausted");
    if (fault == SandboxFault::memory_quota) luaG_runerror(state, "EAWR-SCRIPT-0208: memory quota exhausted");
    luaG_runerror(state, "EAWR-SCRIPT-0205: instruction budget exhausted");
}

bool is_frozen(const Sandbox::Impl& sandbox, const Table* table) noexcept {
    return ::std::find(sandbox.frozen.begin(), sandbox.frozen.end(), table) != sandbox.frozen.end();
}

bool is_internal(const Sandbox::Impl& sandbox, const Table* table) noexcept {
    return table == sandbox.sizes || table == sandbox.anchors || table == sandbox.handles;
}

void check_table_write(Sandbox::Impl& sandbox, lua_State* state, const Table* table) {
    if (!sandbox.sort_scratch.empty() && table != sandbox.sort_scratch.back()) {
        luaG_runerror(state, "EAWR-SCRIPT-0206: table write inside a table.sort comparator");
    }
    if (is_frozen(sandbox, table)) luaG_runerror(state, "EAWR-SCRIPT-0201: library table is read-only");
}

void store_keys(Sandbox::Impl& sandbox, lua_State* state, Table* keys) {
    const ::std::size_t count = sandbox.scratch_keys.size();
    for (::std::size_t index = 0; index < count; ++index) {
        setobj2t(luaH_setnum_upstream(state, keys, static_cast<int>(index + 1)), &sandbox.scratch_keys[index]);
    }
}

} // namespace sandbox_internal

using namespace sandbox_internal;

lua_Hook sandbox_count_hook() noexcept { return count_hook; }

// Interposed upstream entry points (upstream/ltable.cpp, upstream/lapi.cpp).
void sandbox_before_table_set(lua_State* state, Table* table, const TObject* key) {
    Sandbox::Impl* sandbox = active_for(state);
    if (sandbox == nullptr || is_internal(*sandbox, table)) return;
    check_table_write(*sandbox, state, table);
    if (ttisstring(key)) {
        if (table == sandbox->globals && sandbox->protected_names.contains(gcvalue(key))) {
            luaG_runerror(state, "EAWR-SCRIPT-0201: protected binding `%s' is read-only", getstr(tsvalue(key)));
        }
        if (string_equals(key, "__mode") || string_equals(key, "__gc")) {
            luaG_runerror(state, "EAWR-SCRIPT-0202: weak tables and finalizers are not available");
        }
        if (ttisnil(luaH_getstr(table, tsvalue(key)))) charge_memory(*sandbox, state, logical_size::table_entry);
        return;
    }
    if (ttislightuserdata(key)) luaG_runerror(state, "EAWR-SCRIPT-0203: light userdata cannot be a table key");
    if (is_reference(key) && !is_handle(*sandbox, key)) ensure_identity(*sandbox, state, key);
    if (ttisnil(luaH_get(table, key))) charge_memory(*sandbox, state, logical_size::table_entry);
}

void sandbox_before_table_setnum(lua_State* state, Table* table, int key) {
    Sandbox::Impl* sandbox = active_for(state);
    if (sandbox == nullptr || is_internal(*sandbox, table)) return;
    check_table_write(*sandbox, state, table);
    if (ttisnil(luaH_getnum(table, key))) charge_memory(*sandbox, state, logical_size::table_entry);
}

void sandbox_before_new_string(lua_State* state, ::std::size_t length) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::string + length);
    }
}

void sandbox_before_new_userdata(lua_State* state, ::std::size_t size) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::userdata + size);
    }
}

void sandbox_before_new_table(lua_State* state) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) charge_memory(*sandbox, state, logical_size::table);
}

// A Lua closure's slots may each open a new upvalue; the charge assumes they do.
void sandbox_before_new_closure(lua_State* state, int upvalues, bool lua) {
    Sandbox::Impl* sandbox = active_for(state);
    if (sandbox == nullptr) return;
    const ::std::uint64_t each = lua ? logical_size::upvalue_slot + logical_size::upvalue : logical_size::c_upvalue;
    charge_memory(*sandbox, state, logical_size::closure + static_cast<::std::uint64_t>(upvalues) * each);
}

void sandbox_before_new_prototype(lua_State* state) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) charge_memory(*sandbox, state, logical_size::prototype);
}

// The compiler's items (upstream/prototype_charges.hpp), before its array grows.
void sandbox_before_prototype_item(lua_State* state, const Instruction*) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::prototype_instruction);
    }
}

void sandbox_before_prototype_item(lua_State*, const int*) {} // with its instruction

void sandbox_before_prototype_item(lua_State* state, const TObject*) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::prototype_constant);
    }
}

void sandbox_before_prototype_item(lua_State* state, Proto* const*) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::prototype_child);
    }
}

void sandbox_before_prototype_item(lua_State* state, const LocVar*) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::prototype_local);
    }
}

void sandbox_before_prototype_item(lua_State* state, TString* const*) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        charge_memory(*sandbox, state, logical_size::prototype_upvalue_name);
    }
}

// luaV_concat (upstream/lvm.cpp) sizes its buffer for the whole result and
// copies the operands into it before it creates the result string.
char* sandbox_concat_openspace(lua_State* state, Mbuffer* buffer, ::std::size_t length) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) {
        check_memory(*sandbox, state, logical_size::string + length);
    }
    return luaZ_openspace(state, buffer, length);
}

void sandbox_before_new_thread(lua_State* state) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) charge_memory(*sandbox, state, logical_size::thread);
}

// A collected thread's address can be reused by a later thread.
void sandbox_before_free_thread(lua_State* state, lua_State* thread) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) sandbox->deep_stacks.erase(thread);
}

// Pattern matching steps (upstream/lstrlib.cpp), charged before they run.
void sandbox_charge_steps(lua_State* state, sflua_metering::StepUnits units) {
    if (Sandbox::Impl* sandbox = active_for(state); sandbox != nullptr) charge(*sandbox, state, units);
}

// Public luaL_getn (upstream/lauxlib.cpp). When neither an `n' field nor a
// table.setn size holds the size, upstream counts the entries up to the first
// nil. That count runs here instead, charging each element before the next is
// read, so an exhausted budget stops it at once; the result and the total
// charge (the size) are upstream's. Returns -1 when upstream answers: no
// active sandbox, an internal table, or a stored size, which costs nothing
// here (the loops it drives charge themselves).
int sandbox_getn(lua_State* state, int table) {
    Sandbox::Impl* sandbox = active_for(state);
    if (sandbox == nullptr) return -1;
    if (table <= 0 && table > LUA_REGISTRYINDEX) table = lua_gettop(state) + table + 1;
    if (is_internal(*sandbox, static_cast<const Table*>(lua_topointer(state, table)))) return -1;
    lua_pushliteral(state, "n");
    lua_rawget(state, table);
    bool stored = stored_size_on_top(state);
    lua_pop(state, 1);
    if (!stored) {
        lua_rawgeti(state, LUA_REGISTRYINDEX, lauxlib_sizes_reference);
        if (lua_istable(state, -1)) {
            lua_pushvalue(state, table);
            lua_rawget(state, -2);
            stored = stored_size_on_top(state);
            lua_pop(state, 1);
        }
        lua_pop(state, 1);
    }
    if (stored) return -1;
    int size = 0;
    for (;;) {
        lua_rawgeti(state, table, size + 1);
        const bool present = !lua_isnil(state, -1);
        lua_pop(state, 1);
        if (!present) return size;
        charge(*sandbox, state, 1);
        ++size;
    }
}

void sandbox_after_new_thread(lua_State* state, lua_State* thread) {
    if (lua_gethook(state) != nullptr) {
        lua_sethook(thread, lua_gethook(state), lua_gethookmask(state), lua_gethookcount(state));
    }
}

Sandbox::Sandbox(::std::unique_ptr<Impl> impl) noexcept : impl_(::std::move(impl)) {}

Sandbox::~Sandbox() {
    if (impl_ != nullptr && impl_->state != nullptr) {
        if (active_sandbox == impl_.get()) active_sandbox = nullptr;
        lua_close(impl_->state);
    }
}

::std::unique_ptr<Sandbox> Sandbox::open(const SandboxLimits& limits) {
    lua_State* state = lua_open();
    if (state == nullptr) return nullptr;
    auto impl = ::std::make_unique<Impl>();
    impl->state = state;
    impl->limits = limits;
    luaopen_base(state);
    luaopen_string(state);
    luaopen_table(state);
    lua_settop(state, 0);

    // Create lauxlib's size table now; it is internal and weak-keyed by design.
    lua_newtable(state);
    static_cast<void>(luaL_getn(state, -1));
    lua_pop(state, 1);
    lua_rawgeti(state, LUA_REGISTRYINDEX, lauxlib_sizes_reference);
    impl->sizes = ttistable(state->top - 1) ? hvalue(state->top - 1) : nullptr;
    lua_pop(state, 1);

    impl->anchors = new_registry_table(state, "eawr.identity_anchors");
    impl->handles = new_registry_table(state, "eawr.handles");
    impl->handle_metatable = new_registry_table(state, "eawr.handle_metatable");
    lua_pushstring(state, "eawr.handle_metatable");
    lua_rawget(state, LUA_REGISTRYINDEX);
    lua_pushliteral(state, "__metatable");
    lua_pushstring(state, handle_metatable_guard);
    lua_rawset(state, -3);
    lua_pop(state, 1);

    lua_pushvalue(state, LUA_GLOBALSINDEX);
    const int globals = lua_gettop(state);
    impl->globals = hvalue(state->top - 1);
    set_field_function(state, globals, "pairs", sandbox_pairs);
    set_field_function(state, globals, "next", sandbox_next);
    set_field_function(state, globals, "tostring", sandbox_tostring);
    set_field_function(state, globals, "collectgarbage", sandbox_collectgarbage);
    for (const char* name : {"print", "gcinfo", "loadstring", "loadfile", "dofile", "newproxy", "setfenv", "require"}) {
        set_forbidden(state, globals, name, name);
    }
    lua_pushliteral(state, "string");
    lua_rawget(state, globals);
    set_forbidden(state, lua_gettop(state), "dump", "string.dump");
    lua_pushliteral(state, "rep");
    lua_pushliteral(state, "rep");
    lua_rawget(state, -3);
    lua_pushcclosure(state, sandbox_rep, 1);
    lua_rawset(state, -3);
    lua_pop(state, 1);
    lua_pushliteral(state, "table");
    lua_rawget(state, globals);
    const int table_library = lua_gettop(state);
    set_field_function(state, table_library, "foreach", sandbox_foreach);
    set_field_function(state, table_library, "foreachi", sandbox_foreachi);
    const ::std::pair<const char*, lua_CFunction> wrapped[] = {
        {"sort", sandbox_sort}, {"insert", sandbox_insert}, {"remove", sandbox_remove}, {"concat", sandbox_concat}};
    for (const auto& [name, function] : wrapped) {
        lua_pushstring(state, name);
        lua_pushstring(state, name);
        lua_rawget(state, table_library);
        lua_pushcclosure(state, function, 1);
        lua_rawset(state, table_library);
    }
    lua_settop(state, 0);
    install_metered_patterns(state);

    lua_sethook(state, count_hook, sandbox_hook_mask, limits.hook_granularity);
    return ::std::unique_ptr<Sandbox>(new Sandbox(::std::move(impl)));
}

bool Sandbox::installs_global(::std::string_view name) {
    // Read off a freshly opened sandbox so the list cannot drift from open().
    static const ::std::set<::std::string, ::std::less<>> names = [] {
        const ::std::unique_ptr<Sandbox> sandbox = open(SandboxLimits{});
        if (sandbox == nullptr) throw ::std::bad_alloc();
        lua_State* state = sandbox->state();
        ::std::set<::std::string, ::std::less<>> result;
        lua_pushnil(state);
        while (lua_next(state, LUA_GLOBALSINDEX) != 0) {
            if (lua_type(state, -2) == LUA_TSTRING) result.emplace(lua_tostring(state, -2), lua_strlen(state, -2));
            lua_pop(state, 1);
        }
        return result;
    }();
    return names.contains(name);
}

lua_State* Sandbox::state() const noexcept { return impl_->state; }

Sandbox::Scope::Scope(Sandbox& sandbox) noexcept : previous_(active_sandbox) { active_sandbox = sandbox.impl_.get(); }

Sandbox::Scope::~Scope() { active_sandbox = previous_; }

void Sandbox::set_budget(::std::int64_t instructions) noexcept { impl_->budget = instructions; }

::std::int64_t Sandbox::budget() const noexcept { return impl_->budget; }

SandboxFault Sandbox::fault() const noexcept { return impl_->fault; }

void Sandbox::seal() {
    lua_State* state = impl_->state;
    Table* globals = impl_->globals;
    const int node_count = sizenode(globals);
    for (int index = 0; index < node_count; ++index) {
        const Node* node = gnode(globals, index);
        if (!ttisnil(gval(node)) && ttisstring(gkey(node))) impl_->protected_names.insert(gcvalue(gkey(node)));
    }
    for (const char* name : {"string", "table", "coroutine"}) {
        lua_pushstring(state, name);
        lua_rawget(state, LUA_GLOBALSINDEX);
        if (lua_istable(state, -1)) impl_->frozen.push_back(hvalue(state->top - 1));
        lua_pop(state, 1);
    }
    impl_->frozen.push_back(impl_->handle_metatable);
}

int Sandbox::load_source(::std::string_view bytes, ::std::string_view chunk_name) {
    lua_State* state = impl_->state;
    if (!bytes.empty() && bytes.front() == '\x1b') {
        lua_pushliteral(state, "EAWR-SCRIPT-0207: precompiled chunks are not accepted");
        return LUA_ERRSYNTAX;
    }
    const ::std::string name(chunk_name);
    ++impl_->identity_suspended;
    const int status = luaL_loadbuffer(state, bytes.data(), bytes.size(), name.c_str());
    --impl_->identity_suspended;
    if (status == 0) record_prototypes(*impl_, clvalue(state->top - 1)->l.p);
    return status;
}

void Sandbox::freeze_table(int index) {
    impl_->frozen.push_back(static_cast<const Table*>(lua_topointer(impl_->state, index)));
}

void Sandbox::raise_fault(lua_State* thread, SandboxFault fault) {
    EAWR_SFLUA_NAMESPACE::raise_fault(*impl_, thread, fault);
}

::std::uint64_t Sandbox::identity_count() const noexcept { return impl_->next_identity - 1; }

::std::uint64_t Sandbox::memory_measured() const noexcept { return impl_->memory_measured; }

::std::uint64_t Sandbox::memory_charged() const noexcept { return impl_->memory_charged; }

void Sandbox::settle_memory() {
    Impl& sandbox = *impl_;
    if (sandbox.fault != SandboxFault::none) return;
    // A measurement walks the reachable graph, so it waits until half of the
    // last one (at least 256 KiB) was charged: the walks cost a fixed share of
    // the charges.
    const ::std::uint64_t due = ::std::max<::std::uint64_t>(sandbox.memory_measured / 2, ::std::uint64_t{256} << 10);
    if (sandbox.deep_stacks.empty() && sandbox.memory_charged < due) return;
    sandbox.deep_stacks.clear();
    sandbox.memory_measured = measure_logical_bytes(sandbox);
    sandbox.memory_charged = 0;
    if (sandbox.memory_measured > sandbox.limits.memory_bytes) sandbox.fault = SandboxFault::memory_quota;
}

void Sandbox::begin_host_call() noexcept { ++impl_->host_calls; }

void Sandbox::end_host_call() noexcept { --impl_->host_calls; }

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
