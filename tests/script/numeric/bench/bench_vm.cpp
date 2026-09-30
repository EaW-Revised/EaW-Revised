// Compiled twice: against the soft-float VM, and with
// EAWR_SFLUA_HARDWARE_TWIN against the benchmark-only hardware-double twin.

#include "bench_vm.hpp"

#include "bench_workload.hpp"
#include "sflua.hpp"

#include <chrono>
#include <stdexcept>
#include <string>

// Code inside the Lua namespace spells ::std: clang-cl 19's <stddef.h>
// declares a nested std::nullptr_t wherever the Lua headers include it.
namespace eawr::script::EAWR_SFLUA_NAMESPACE {

::std::vector<::std::int64_t> run_tick_workload(int ticks, int steps_per_tick, ::std::string& last_message) {
    lua_State* state = open_profile_state();
    const ::std::string_view source = numeric::bench::workload_source;
    if (luaL_loadbuffer(state, source.data(), source.size(), "=workload") != 0 || lua_pcall(state, 0, 0, 0) != 0) {
        const ::std::string message = lua_tostring(state, -1);
        lua_close(state);
        throw ::std::runtime_error(message);
    }
    ::std::vector<::std::int64_t> samples;
    samples.reserve(static_cast<::std::size_t>(ticks));
    for (int tick = 0; tick < ticks; ++tick) {
        const auto start = ::std::chrono::steady_clock::now();
        lua_getglobal(state, "Tick");
        lua_pushnumber(state, tick);
        lua_pushnumber(state, steps_per_tick);
        if (lua_pcall(state, 2, 1, 0) != 0) {
            const ::std::string message = lua_tostring(state, -1);
            lua_close(state);
            throw ::std::runtime_error(message);
        }
        const auto stop = ::std::chrono::steady_clock::now();
        samples.push_back(::std::chrono::duration_cast<::std::chrono::nanoseconds>(stop - start).count());
        if (tick + 1 == ticks) {
            last_message = lua_tostring(state, -1);
        }
        lua_settop(state, 0);
    }
    lua_close(state);
    return samples;
}

} // namespace eawr::script::EAWR_SFLUA_NAMESPACE
