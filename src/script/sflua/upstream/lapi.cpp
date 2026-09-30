// Upstream Lua 5.0.2 src/lapi.c, unmodified, compiled for the authoritative profile.
// lua_newthread is renamed here so that a new thread is charged to the
// sandbox's memory quota and inherits the creating state's hook (the
// instruction budget, sflua_sandbox.cpp); upstream starts every thread without
// a hook.
#include "sflua_upstream.hpp"

#define lua_newthread lua_newthread_upstream
EAWR_SFLUA_BEGIN
#include "lapi.c"
EAWR_SFLUA_END
#undef lua_newthread

EAWR_SFLUA_BEGIN
void sandbox_before_new_thread(lua_State* L);
void sandbox_after_new_thread(lua_State* L, lua_State* thread);

LUA_API lua_State* lua_newthread(lua_State* L) {
    sandbox_before_new_thread(L);
    lua_State* thread = lua_newthread_upstream(L);
    sandbox_after_new_thread(L, thread);
    return thread;
}
EAWR_SFLUA_END
