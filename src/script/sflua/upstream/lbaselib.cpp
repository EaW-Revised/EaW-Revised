// Upstream Lua 5.0.2 src/lib/lbaselib.c, unmodified, compiled for the authoritative profile,
// with accessors that name its C functions for persistence.
#include "sflua_upstream.hpp"

EAWR_SFLUA_BEGIN
#include "lbaselib.c"
EAWR_SFLUA_END

// Stable names for persistence (sflua_persist.cpp).
EAWR_SFLUA_BEGIN
const luaL_reg* sflua_base_functions() { return base_funcs; }
const luaL_reg* sflua_coroutine_functions() { return co_funcs; }
lua_CFunction sflua_coroutine_wrapper() { return luaB_auxwrap; }
EAWR_SFLUA_END
