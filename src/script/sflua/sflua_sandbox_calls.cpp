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

} // namespace sandbox_internal

using namespace sandbox_internal;

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

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
