#pragma once

// Lua 5.0.2 API of the authoritative numeric profile (or of the benchmark
// twin when EAWR_SFLUA_HARDWARE_TWIN is defined), in its own namespace.
// Code written inside that namespace must spell ::std: clang-cl 19's
// <stddef.h> declares a nested std::nullptr_t wherever Lua includes it.

#include "sflua_config.hpp"

EAWR_SFLUA_BEGIN
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
EAWR_SFLUA_END

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

// A state with the bounded P0 library set: base (with coroutine), string and
// table. Returns nullptr when allocation fails.
lua_State* open_profile_state() noexcept;

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
