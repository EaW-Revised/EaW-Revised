// Upstream Lua 5.0.2 src/ltable.c, unmodified, compiled for the authoritative profile.
// luaH_new, luaH_set and luaH_setnum are renamed here so that every other
// translation unit reaches them through the sandbox rules and memory charges
// (sflua_sandbox.cpp); ltable.c's own calls (rehash) keep using the upstream
// functions.
#include "sflua_upstream.hpp"

#include <cstdint>

EAWR_SFLUA_BEGIN
#include "llimits.h"
EAWR_SFLUA_END

// IntPoint narrows a pointer to lu_hash for hashing ("there is no problem if
// the integer cannot hold the whole pointer value"). C accepts the direct
// cast; C++ rejects it on LP64, so keep the same low bits via uintptr_t.
#undef IntPoint
#define IntPoint(p) (static_cast<lu_hash>(reinterpret_cast<::std::uintptr_t>(p)))

#define luaH_new luaH_new_upstream
#define luaH_set luaH_set_upstream
#define luaH_setnum luaH_setnum_upstream
EAWR_SFLUA_BEGIN
#include "ltable.c"
EAWR_SFLUA_END
#undef luaH_new
#undef luaH_set
#undef luaH_setnum

EAWR_SFLUA_BEGIN
void sandbox_before_table_set(lua_State* L, Table* t, const TObject* key);
void sandbox_before_table_setnum(lua_State* L, Table* t, int key);
void sandbox_before_new_table(lua_State* L);

Table* luaH_new(lua_State* L, int narray, int lnhash) {
    sandbox_before_new_table(L);
    return luaH_new_upstream(L, narray, lnhash);
}

TObject* luaH_set(lua_State* L, Table* t, const TObject* key) {
    sandbox_before_table_set(L, t, key);
    return luaH_set_upstream(L, t, key);
}

TObject* luaH_setnum(lua_State* L, Table* t, int key) {
    sandbox_before_table_setnum(L, t, key);
    return luaH_setnum_upstream(L, t, key);
}
EAWR_SFLUA_END
