// Upstream Lua 5.0.2 src/lundump.c, unmodified, compiled for the authoritative profile.
#include "sflua_upstream.hpp"

EAWR_SFLUA_BEGIN
#include "lundump.h"
EAWR_SFLUA_END

#if !defined(EAWR_SFLUA_HARDWARE_TWIN)
// The chunk header's test number, 3.14159265358979323846E7, as the exact
// binary64 bits a hardware build stores; no floating literal is compiled.
#undef TEST_NUMBER
#define TEST_NUMBER (::eawr::script::numeric::LuaNumber::from_repr(0x417DF5E7689309B6ULL))
#endif

EAWR_SFLUA_BEGIN
#include "lundump.c"
EAWR_SFLUA_END
