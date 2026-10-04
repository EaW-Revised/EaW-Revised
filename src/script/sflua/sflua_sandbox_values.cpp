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
using namespace sandbox_internal;

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

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
