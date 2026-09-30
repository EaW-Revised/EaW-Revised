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

namespace {

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

const Table* check_table(lua_State* state, int index) {
    luaL_checktype(state, index, LUA_TTABLE);
    return static_cast<const Table*>(lua_topointer(state, index));
}

// Pushes the canonical keys of the table at index; returns their count.
// Scripts: charges the sort to the budget and raises for an unorderable key.
::std::size_t push_keys(Sandbox::Impl& sandbox, lua_State* state, int index) {
    const Table* table = static_cast<const Table*>(lua_topointer(state, index));
    lua_newtable(state);
    Table* keys = hvalue(state->top - 1);
    if (!collect_canonical_keys(sandbox, table)) {
        luaG_runerror(state, "EAWR-SCRIPT-0203: table key has no canonical order");
    }
    charge(sandbox, state, sort_units(sandbox.scratch_keys.size()));
    store_keys(sandbox, state, keys);
    return sandbox.scratch_keys.size();
}

int pairs_iterator(lua_State* state) {
    const int count = static_cast<int>(lua_tonumber(state, lua_upvalueindex(4)));
    int position = static_cast<int>(lua_tonumber(state, lua_upvalueindex(3)));
    while (position < count) {
        ++position;
        lua_rawgeti(state, lua_upvalueindex(2), position);
        lua_pushvalue(state, -1);
        lua_rawget(state, lua_upvalueindex(1));
        if (!lua_isnil(state, -1)) {
            lua_pushnumber(state, static_cast<lua_Number>(position));
            lua_replace(state, lua_upvalueindex(3));
            return 2;
        }
        lua_pop(state, 2);
    }
    lua_pushnumber(state, static_cast<lua_Number>(position));
    lua_replace(state, lua_upvalueindex(3));
    lua_pushnil(state);
    return 1;
}

// pairs(t): iterates a snapshot of t's keys in canonical order, skipping keys
// deleted since and reading current values; keys added since are not visited.
int sandbox_pairs(lua_State* state) {
    Sandbox::Impl& sandbox = require_active(state);
    check_table(state, 1);
    lua_settop(state, 1);
    lua_pushvalue(state, 1);
    const ::std::size_t count = push_keys(sandbox, state, 1);
    lua_pushnumber(state, static_cast<lua_Number>(0));
    lua_pushnumber(state, static_cast<lua_Number>(static_cast<int>(count)));
    lua_pushcclosure(state, pairs_iterator, 4);
    lua_pushvalue(state, 1);
    lua_pushnil(state);
    return 3;
}

// next(t, k): the least current key greater than k (the least key for nil),
// whether or not k is still present.
int sandbox_next(lua_State* state) {
    Sandbox::Impl& sandbox = require_active(state);
    const Table* table = check_table(state, 1);
    lua_settop(state, 2);
    const TObject* after = state->base + 1;
    if (!ttisnil(after) && !orderable(sandbox, after)) luaG_runerror(state, "invalid key to `next'");
    const TObject* best = nullptr;
    sflua_metering::StepUnits visited = 0;
    TObject array_key;
    TObject best_array_key;
    for (int index = 0; index < table->sizearray; ++index) {
        if (ttisnil(&table->array[index])) continue;
        ++visited;
        setnvalue(&array_key, static_cast<lua_Number>(index + 1));
        if (!ttisnil(after) && !key_less(sandbox, after, &array_key)) continue;
        if (best == nullptr || key_less(sandbox, &array_key, best)) {
            best_array_key = array_key;
            best = &best_array_key;
        }
    }
    const int node_count = sizenode(table);
    for (int index = 0; index < node_count; ++index) {
        const Node* node = gnode(table, index);
        if (ttisnil(gval(node))) continue;
        ++visited;
        const TObject* key = gkey(node);
        if (!ttisnil(after) && !key_less(sandbox, after, key)) continue;
        if (best == nullptr || key_less(sandbox, key, best)) best = key;
    }
    charge(sandbox, state, visited);
    if (best == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    setobj2s(state->top, best);
    ++state->top;
    lua_pushvalue(state, -1);
    lua_rawget(state, 1);
    return 2;
}

// table.foreach(t, f) over the same snapshot as pairs.
int sandbox_foreach(lua_State* state) {
    Sandbox::Impl& sandbox = require_active(state);
    check_table(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    lua_settop(state, 2);
    const int count = static_cast<int>(push_keys(sandbox, state, 1));
    for (int position = 1; position <= count; ++position) {
        lua_rawgeti(state, 3, position);
        lua_pushvalue(state, -1);
        lua_rawget(state, 1);
        if (lua_isnil(state, -1)) {
            lua_pop(state, 2);
            continue;
        }
        lua_pushvalue(state, 2);
        lua_insert(state, -3);
        lua_call(state, 2, 1);
        if (!lua_isnil(state, -1)) return 1;
        lua_pop(state, 1);
    }
    return 0;
}

// table.sort: the upstream algorithm on a private copy, so a comparator can
// neither see nor mutate a half-sorted table. Comparators may not write tables.
int sandbox_sort(lua_State* state) {
    Sandbox::Impl& sandbox = require_active(state);
    check_table(state, 1);
    const int count = luaL_getn(state, 1);
    if (!lua_isnoneornil(state, 2)) luaL_checktype(state, 2, LUA_TFUNCTION);
    lua_settop(state, 2);
    // Charged before the copy: a table.setn size can far exceed the entries.
    charge(sandbox, state, sort_units(static_cast<::std::size_t>(count)));
    lua_newtable(state);
    Table* scratch = hvalue(state->top - 1);
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(state, 1, index);
        lua_rawseti(state, 3, index);
    }
    lua_pushliteral(state, "n");
    lua_pushnumber(state, static_cast<lua_Number>(count));
    lua_rawset(state, 3);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_pushvalue(state, 3);
    lua_pushvalue(state, 2);
    sandbox.sort_scratch.push_back(scratch);
    const int status = lua_pcall(state, 2, 0, 0);
    sandbox.sort_scratch.pop_back();
    if (status != 0) return lua_error(state);
    for (int index = 1; index <= count; ++index) {
        lua_rawgeti(state, 3, index);
        lua_rawseti(state, 1, index);
    }
    return 0;
}

// table.insert, table.remove, table.concat and string.rep: the upstream
// function (upvalue 1), called on this frame after charging the steps its C
// loop will take.
// A table.setn size or a far position makes that loop much longer than the
// table's entries (FoC's table library is upstream's). Arguments the upstream
// function rejects charge nothing; it raises its own error.
int call_metered(lua_State* state, sflua_metering::StepUnits units) {
    Sandbox::Impl& sandbox = require_active(state);
    charge(sandbox, state, units);
    const lua_CFunction upstream = lua_tocfunction(state, lua_upvalueindex(1));
    return upstream(state);
}

// Upstream luaL_checkint of a valid argument.
int argument_int(lua_State* state, int index) { return static_cast<int>(lua_tonumber(state, index)); }

// luaB_tinsert: n = getn + 1, grown to a later position; elements pos..n-1
// move up one.
int sandbox_insert(lua_State* state) {
    sflua_metering::StepUnits moves = 0;
    if (lua_istable(state, 1) && lua_gettop(state) != 2 && lua_isnumber(state, 2)) {
        const ::std::int64_t position = argument_int(state, 2);
        const ::std::int64_t size = ::std::max<::std::int64_t>(::std::int64_t{luaL_getn(state, 1)} + 1, position);
        moves = static_cast<sflua_metering::StepUnits>(size - position);
    }
    return call_metered(state, moves);
}

// luaB_tremove: when n >= 1, elements pos+1..n move down one.
int sandbox_remove(lua_State* state) {
    sflua_metering::StepUnits moves = 0;
    if (lua_istable(state, 1) && lua_isnumber(state, 2)) {
        const ::std::int64_t size = luaL_getn(state, 1);
        const ::std::int64_t position = argument_int(state, 2);
        if (size >= 1 && position < size) moves = static_cast<sflua_metering::StepUnits>(size - position);
    }
    return call_metered(state, moves);
}

// str_concat appends t[i..n] (n = getn for 0) and raises at the first value
// that is not a string or number, so its loop never runs past the table's
// entries plus one. Charges the elements it will visit.
int sandbox_concat(lua_State* state) {
    sflua_metering::StepUnits visited = 0;
    const bool valid_range = (lua_isnoneornil(state, 3) || lua_isnumber(state, 3)) &&
        (lua_isnoneornil(state, 4) || lua_isnumber(state, 4));
    if (lua_istable(state, 1) && valid_range) {
        const int first = lua_isnoneornil(state, 3) ? 1 : argument_int(state, 3);
        int last = lua_isnoneornil(state, 4) ? 0 : argument_int(state, 4);
        if (last == 0) last = luaL_getn(state, 1);
        for (::std::int64_t index = first; index <= last; ++index) {
            ++visited;
            lua_rawgeti(state, 1, static_cast<int>(index));
            const bool text = lua_isstring(state, -1) != 0;
            lua_pop(state, 1);
            if (!text) break;
        }
    }
    return call_metered(state, visited);
}

// table.foreachi(t, f): luaB_foreachi, charging each call before making it,
// so a C function f cannot run a table.setn-sized loop unmetered.
int sandbox_foreachi(lua_State* state) {
    Sandbox::Impl& sandbox = require_active(state);
    luaL_checktype(state, 1, LUA_TTABLE);
    const int count = luaL_getn(state, 1);
    luaL_checktype(state, 2, LUA_TFUNCTION);
    for (int index = 1; index <= count; ++index) {
        charge(sandbox, state, 1);
        lua_pushvalue(state, 2);
        lua_pushnumber(state, static_cast<lua_Number>(index));
        lua_rawgeti(state, 1, index);
        lua_call(state, 2, 1);
        if (!lua_isnil(state, -1)) return 1;
        lua_pop(state, 1);
    }
    return 0;
}

// string.rep(s, n): str_rep appends s n times, so an empty s loops n times
// without using memory. Charges the n appends.
int sandbox_rep(lua_State* state) {
    sflua_metering::StepUnits appends = 0;
    if (lua_isstring(state, 1) && lua_isnumber(state, 2)) {
        appends = static_cast<sflua_metering::StepUnits>(::std::max(argument_int(state, 2), 0));
    }
    return call_metered(state, appends);
}

// Upstream lauxlib checkint: whether the value on top is a size >= 0.
bool stored_size_on_top(lua_State* state) {
    const int size = static_cast<int>(lua_tonumber(state, -1));
    return size > 0 || (size == 0 && lua_isnumber(state, -1) != 0);
}

void append_hex(char*& out, ::std::uint64_t value, int digit_count = 16) noexcept {
    constexpr char digits[] = "0123456789ABCDEF";
    for (int shift = (digit_count - 1) * 4; shift >= 0; shift -= 4) *out++ = digits[(value >> shift) & 0xF];
}

// tostring: upstream for plain values; tables, functions, threads and
// userdata print their identity in the retail "%p" shape (16 upper-case hex
// digits), never an address. Host handles print (kind, id) as 8 and 16 hex
// digits and take no identity, so their strings sort in canonical key order.
int sandbox_tostring(lua_State* state) {
    luaL_checkany(state, 1);
    if (luaL_callmeta(state, 1, "__tostring")) return 1;
    switch (lua_type(state, 1)) {
    case LUA_TNUMBER:
        lua_pushstring(state, lua_tostring(state, 1));
        return 1;
    case LUA_TSTRING:
        lua_pushvalue(state, 1);
        return 1;
    case LUA_TBOOLEAN:
        lua_pushstring(state, lua_toboolean(state, 1) ? "true" : "false");
        return 1;
    case LUA_TNIL:
        lua_pushliteral(state, "nil");
        return 1;
    case LUA_TLIGHTUSERDATA:
        luaG_runerror(state, "EAWR-SCRIPT-0203: light userdata has no identity");
        return 0;
    default:
        break;
    }
    Sandbox::Impl& sandbox = require_active(state);
    char buffer[48];
    char* out = buffer;
    const char* name = lua_typename(state, lua_type(state, 1));
    while (*name != '\0') *out++ = *name++;
    *out++ = ':';
    *out++ = ' ';
    if (is_handle(sandbox, state->base)) {
        const HandlePayload& handle = handle_payload(state->base);
        append_hex(out, handle.kind, 8);
        *out++ = ':';
        append_hex(out, handle.id);
    } else {
        append_hex(out, ensure_identity(sandbox, state, state->base));
    }
    lua_pushlstring(state, buffer, static_cast<::std::size_t>(out - buffer));
    return 1;
}

// collectgarbage(limit): accepted and ignored. FoC's library calls
// collectgarbage(256) when it loads; collection is unobservable here.
int sandbox_collectgarbage(lua_State* state) {
    static_cast<void>(luaL_optint(state, 1, 0));
    return 0;
}

int forbidden(lua_State* state) {
    return luaL_error(
        state, "EAWR-SCRIPT-0203: `%s' is not available in the authoritative sandbox",
        lua_tostring(state, lua_upvalueindex(1))
    );
}

void set_field_function(lua_State* state, int table, const char* name, lua_CFunction function) {
    lua_pushstring(state, name);
    lua_pushcfunction(state, function);
    lua_rawset(state, table);
}

void set_forbidden(lua_State* state, int table, const char* name, const char* shown_name) {
    lua_pushstring(state, name);
    lua_pushstring(state, shown_name);
    lua_pushcclosure(state, forbidden, 1);
    lua_rawset(state, table);
}

// Replaces string.find, gfind and gsub with the metered matcher.
void install_metered_patterns(lua_State* state) {
    lua_pushliteral(state, "string");
    lua_rawget(state, LUA_GLOBALSINDEX);
    const int string_library = lua_gettop(state);
    set_field_function(state, string_library, "find", sflua_metered_find());
    set_field_function(state, string_library, "gfind", sflua_metered_gfind());
    set_field_function(state, string_library, "gsub", sflua_metered_gsub());
    lua_pop(state, 1);
}

Table* new_registry_table(lua_State* state, const char* name) {
    lua_pushstring(state, name);
    lua_newtable(state);
    Table* table = hvalue(state->top - 1);
    lua_rawset(state, LUA_REGISTRYINDEX);
    return table;
}

} // namespace

bool collect_canonical_keys(Sandbox::Impl& sandbox, const Table* table) {
    sandbox.scratch_keys.clear();
    for (int index = 0; index < table->sizearray; ++index) {
        if (ttisnil(&table->array[index])) continue;
        TObject key;
        setnvalue(&key, static_cast<lua_Number>(index + 1));
        sandbox.scratch_keys.push_back(key);
    }
    const int node_count = sizenode(table);
    for (int index = 0; index < node_count; ++index) {
        const Node* node = gnode(table, index);
        if (!ttisnil(gval(node))) sandbox.scratch_keys.push_back(*gkey(node));
    }
    for (const TObject& key : sandbox.scratch_keys) {
        if (!orderable(sandbox, &key)) return false;
    }
    ::std::sort(sandbox.scratch_keys.begin(), sandbox.scratch_keys.end(), [&](const TObject& left, const TObject& right) {
        return key_less(sandbox, &left, &right);
    });
    return true;
}


lua_Hook sandbox_count_hook() noexcept { return count_hook; }

void record_prototypes(Sandbox::Impl& sandbox, const Proto* root) {
    ::std::vector<::std::pair<const Proto*, ::std::vector<::std::uint32_t>>> pending{{root, {}}};
    while (!pending.empty()) {
        auto [prototype, path] = ::std::move(pending.back());
        pending.pop_back();
        for (int index = 0; index < prototype->sizep; ++index) {
            ::std::vector<::std::uint32_t> child = path;
            child.push_back(static_cast<::std::uint32_t>(index));
            pending.emplace_back(prototype->p[index], ::std::move(child));
        }
        sandbox.prototype_paths[prototype] = ::std::move(path);
    }
}

void append_sandbox_functions(::std::vector<CFunctionEntry>& out) {
    out.push_back({"sandbox.pairs", sandbox_pairs, UpvalueShape::none});
    out.push_back({"sandbox.pairs_iterator", pairs_iterator, UpvalueShape::pairs_cursor});
    out.push_back({"sandbox.next", sandbox_next, UpvalueShape::none});
    out.push_back({"sandbox.foreach", sandbox_foreach, UpvalueShape::none});
    out.push_back({"sandbox.sort", sandbox_sort, UpvalueShape::function});
    out.push_back({"sandbox.insert", sandbox_insert, UpvalueShape::function});
    out.push_back({"sandbox.remove", sandbox_remove, UpvalueShape::function});
    out.push_back({"sandbox.concat", sandbox_concat, UpvalueShape::function});
    out.push_back({"sandbox.foreachi", sandbox_foreachi, UpvalueShape::none});
    out.push_back({"sandbox.rep", sandbox_rep, UpvalueShape::function});
    out.push_back({"sandbox.tostring", sandbox_tostring, UpvalueShape::none});
    out.push_back({"sandbox.collectgarbage", sandbox_collectgarbage, UpvalueShape::none});
    out.push_back({"sandbox.forbidden", forbidden, UpvalueShape::name});
}

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

bool Sandbox::push_canonical_keys(int index) {
    lua_State* state = impl_->state;
    if (index < 0 && index > LUA_REGISTRYINDEX) index = lua_gettop(state) + index + 1;
    const Table* table = static_cast<const Table*>(lua_topointer(state, index));
    if (!collect_canonical_keys(*impl_, table)) return false;
    ++impl_->memory_suspended;
    lua_newtable(state);
    store_keys(*impl_, state, hvalue(state->top - 1));
    --impl_->memory_suspended;
    return true;
}

void Sandbox::freeze_table(int index) {
    impl_->frozen.push_back(static_cast<const Table*>(lua_topointer(impl_->state, index)));
}

void Sandbox::raise_fault(lua_State* thread, SandboxFault fault) {
    EAWR_SFLUA_NAMESPACE::raise_fault(*impl_, thread, fault);
}

void Sandbox::push_handle(lua_State* state, HostHandle handle) {
    const HandlePayload payload{handle.kind, 0, handle.id};
    lua_pushstring(state, "eawr.handles");
    lua_rawget(state, LUA_REGISTRYINDEX);
    lua_pushlstring(state, reinterpret_cast<const char*>(&payload), sizeof(payload));
    lua_rawget(state, -2);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        auto* data = static_cast<HandlePayload*>(lua_newuserdata(state, sizeof(HandlePayload)));
        *data = payload;
        lua_pushstring(state, "eawr.handle_metatable");
        lua_rawget(state, LUA_REGISTRYINDEX);
        lua_setmetatable(state, -2);
        lua_pushlstring(state, reinterpret_cast<const char*>(&payload), sizeof(payload));
        lua_pushvalue(state, -2);
        lua_rawset(state, -4);
    }
    lua_remove(state, -2);
}

void Sandbox::set_handle_index(const lua_CFunction index) {
    lua_State* state = impl_->state;
    lua_pushstring(state, "eawr.handle_metatable");
    lua_rawget(state, LUA_REGISTRYINDEX);
    lua_pushliteral(state, "__index");
    lua_pushcfunction(state, index);
    lua_rawset(state, -3);
    lua_pop(state, 1);
}

bool Sandbox::to_handle(lua_State* state, int index, HostHandle& handle) const {
    if (lua_type(state, index) != LUA_TUSERDATA) return false;
    if (!lua_getmetatable(state, index)) return false;
    const bool match = lua_topointer(state, -1) == impl_->handle_metatable;
    lua_pop(state, 1);
    if (!match) return false;
    const auto* payload = static_cast<const HandlePayload*>(lua_touserdata(state, index));
    handle = HostHandle{payload->kind, payload->id};
    return true;
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
