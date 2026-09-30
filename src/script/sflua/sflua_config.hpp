#pragma once

// Configuration of the upstream Lua 5.0.2 headers for the authoritative
// numeric profile. Upstream sources and headers are compiled as C++ inside
// namespace eawr::script::sflua, so this build and the P0 C runtime
// (eawr::lua_502, hardware binary64) coexist in one program.
//
// EAWR_SFLUA_HARDWARE_TWIN selects a benchmark-only twin with plain `double`
// numbers in namespace eawr::script::hwlua_bench; it measures the soft-float
// cost and is never linked into authoritative code.

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(EAWR_SFLUA_HARDWARE_TWIN)

#define EAWR_SFLUA_NAMESPACE hwlua_bench

#else

#include "eawr/script/numeric/lua_number.hpp"

#define EAWR_SFLUA_NAMESPACE sflua
#define LUA_NUMBER ::eawr::script::numeric::LuaNumber
#define LUA_UACNUMBER ::eawr::script::numeric::LuaNumber
// Upstream aligns allocations with a union containing `double`; an integer of
// the same size keeps the alignment without a floating type.
#define LUSER_ALIGNMENT_T \
    union {               \
        unsigned long long u; \
        void* s;          \
        long l;           \
    }

#endif

#define EAWR_SFLUA_BEGIN    \
    namespace eawr::script { \
    namespace EAWR_SFLUA_NAMESPACE {
#define EAWR_SFLUA_END \
    }                  \
    }
