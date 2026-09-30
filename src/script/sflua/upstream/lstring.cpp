// Upstream Lua 5.0.2 src/lstring.c, unmodified, compiled for the authoritative profile.
// luaS_newlstr and luaS_newudata are renamed here so that every other
// translation unit reaches them through the sandbox's memory charge
// (sflua_sandbox.cpp), made before the object exists.
#include "sflua_upstream.hpp"

#define luaS_newlstr luaS_newlstr_upstream
#define luaS_newudata luaS_newudata_upstream
EAWR_SFLUA_BEGIN
#include "lstring.c"
EAWR_SFLUA_END
#undef luaS_newlstr
#undef luaS_newudata

EAWR_SFLUA_BEGIN
void sandbox_before_new_string(lua_State* L, size_t length);
void sandbox_before_new_userdata(lua_State* L, size_t size);

TString* luaS_newlstr(lua_State* L, const char* str, size_t l) {
    sandbox_before_new_string(L, l);
    return luaS_newlstr_upstream(L, str, l);
}

Udata* luaS_newudata(lua_State* L, size_t s) {
    sandbox_before_new_userdata(L, s);
    return luaS_newudata_upstream(L, s);
}
EAWR_SFLUA_END
