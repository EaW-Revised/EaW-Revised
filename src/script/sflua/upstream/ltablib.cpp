// Upstream Lua 5.0.2 src/lib/ltablib.c, unmodified, compiled for the authoritative profile.
#include "sflua_upstream.hpp"

EAWR_SFLUA_BEGIN
#include "ltablib.c"
EAWR_SFLUA_END

// Stable names for persistence (sflua_persist.cpp).
EAWR_SFLUA_BEGIN
const luaL_reg* sflua_table_functions() { return tab_funcs; }
EAWR_SFLUA_END
