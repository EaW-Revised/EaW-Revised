// Upstream Lua 5.0.2 src/lstate.c, unmodified, compiled for the authoritative profile.
// luaE_freethread is renamed here so that the sandbox forgets a collected
// thread's stack high-water mark before its address can be reused
// (sflua_sandbox.cpp).
#include "sflua_upstream.hpp"

#define luaE_freethread luaE_freethread_upstream
EAWR_SFLUA_BEGIN
#include "lstate.c"
EAWR_SFLUA_END
#undef luaE_freethread

EAWR_SFLUA_BEGIN
void sandbox_before_free_thread(lua_State* L, lua_State* thread);

void luaE_freethread(lua_State* L, lua_State* L1) {
    sandbox_before_free_thread(L, L1);
    luaE_freethread_upstream(L, L1);
}
EAWR_SFLUA_END
