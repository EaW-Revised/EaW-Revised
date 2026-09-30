// Upstream Lua 5.0.2 src/lvm.c, unmodified, compiled for the authoritative profile.
// luaV_concat's call to luaZ_openspace goes through the sandbox, which checks
// the result string's memory charge before the buffer for it is allocated
// and filled (sflua_sandbox.cpp); the string's own charge follows unchanged.
#include "sflua_upstream.hpp"

EAWR_SFLUA_BEGIN
#include "lzio.h"
char* sandbox_concat_openspace(lua_State* L, Mbuffer* buff, size_t n);
EAWR_SFLUA_END

#define luaZ_openspace sandbox_concat_openspace
EAWR_SFLUA_BEGIN
#include "lvm.c"
EAWR_SFLUA_END
#undef luaZ_openspace
