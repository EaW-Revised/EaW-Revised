// The P0 C runtime (hardware binary64, upstream CRT conversions) as a
// differential oracle. It is not part of the authoritative path.

#include "p0_oracle.hpp"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace eawr::script::numeric::test {

std::string p0_run(std::string_view code) {
    lua_State* state = lua_open();
    luaopen_base(state);
    luaopen_string(state);
    luaopen_table(state);
    lua_settop(state, 0);
    std::string result;
    int status = luaL_loadbuffer(state, code.data(), code.size(), "=test");
    if (status == 0) {
        status = lua_pcall(state, 0, 1, 0);
    }
    std::size_t length = 0;
    const char* text = lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : nullptr;
    length = text != nullptr ? lua_strlen(state, -1) : 0;
    result = status != 0 ? "error: " + std::string(text != nullptr ? text : "?")
                         : (text != nullptr ? std::string(text, length) : std::string("<not a string>"));
    lua_close(state);
    return result;
}

} // namespace eawr::script::numeric::test
