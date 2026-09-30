// Upstream Lua 5.0.2 src/lfunc.c, unmodified, compiled for the authoritative profile.
// luaF_newproto, luaF_newCclosure and luaF_newLclosure are renamed here so
// that every other translation unit reaches them through the sandbox's memory
// charge (sflua_sandbox.cpp), made before the object exists.
#include "sflua_upstream.hpp"

#define luaF_newproto luaF_newproto_upstream
#define luaF_newCclosure luaF_newCclosure_upstream
#define luaF_newLclosure luaF_newLclosure_upstream
EAWR_SFLUA_BEGIN
#include "lfunc.c"
EAWR_SFLUA_END
#undef luaF_newproto
#undef luaF_newCclosure
#undef luaF_newLclosure

EAWR_SFLUA_BEGIN
void sandbox_before_new_prototype(lua_State* L);
void sandbox_before_new_closure(lua_State* L, int upvalues, bool lua);

Proto* luaF_newproto(lua_State* L) {
    sandbox_before_new_prototype(L);
    return luaF_newproto_upstream(L);
}

Closure* luaF_newCclosure(lua_State* L, int nelems) {
    sandbox_before_new_closure(L, nelems, false);
    return luaF_newCclosure_upstream(L, nelems);
}

Closure* luaF_newLclosure(lua_State* L, int nelems, TObject* e) {
    sandbox_before_new_closure(L, nelems, true);
    return luaF_newLclosure_upstream(L, nelems, e);
}
EAWR_SFLUA_END
