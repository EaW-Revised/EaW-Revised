// Upstream Lua 5.0.2 src/lib/lauxlib.c, unmodified, compiled for the authoritative profile.
// luaL_getn is renamed here so that every other translation unit (table.getn,
// the table library, unpack) reaches it through the sandbox, which counts a
// table without a stored size itself and charges each element as it counts
// (sflua_sandbox.cpp); lauxlib.c's own calls (luaL_ref) keep the upstream
// function.
#include "sflua_upstream.hpp"

#define luaL_getn luaL_getn_upstream
EAWR_SFLUA_BEGIN
#include "lauxlib.c"
EAWR_SFLUA_END
#undef luaL_getn

EAWR_SFLUA_BEGIN
int sandbox_getn(lua_State* L, int t);

int luaL_getn(lua_State* L, int t) {
    const int n = sandbox_getn(L, t);
    return n >= 0 ? n : luaL_getn_upstream(L, t);
}
EAWR_SFLUA_END
