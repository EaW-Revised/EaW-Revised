#include "sflua.hpp"

namespace eawr::script::EAWR_SFLUA_NAMESPACE {

lua_State* open_profile_state() noexcept {
    lua_State* state = lua_open();
    if (state == nullptr) {
        return nullptr;
    }
    luaopen_base(state);
    luaopen_string(state);
    luaopen_table(state);
    lua_settop(state, 0);
    return state;
}

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
