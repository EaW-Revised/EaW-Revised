enable_language(C)

add_subdirectory(
    "${PROJECT_SOURCE_DIR}/third_party/lua"
    "${PROJECT_BINARY_DIR}/third_party/lua"
    EXCLUDE_FROM_ALL
)

add_library(
    eawr_script
    STATIC
        pglua.cpp
        script_host.cpp
        script_host_api.cpp
        script_host_context.cpp
        wide_string.cpp
)
add_library(eawr::script ALIAS eawr_script)

target_include_directories(
    eawr_script
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_script PUBLIC eawr::core eawr::vfs PRIVATE eawr::lua_502)
target_compile_features(eawr_script PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_script)

# Authoritative Lua numeric profile (#246, docs/lua-numeric-profile.md):
# integer-only binary64 backend, decimal conversion and the Q24 boundary.
add_library(
    eawr_script_numeric
    STATIC
        numeric/binary64.cpp
        numeric/decimal.cpp
        numeric/q24_boundary.cpp
)
add_library(eawr::script_numeric ALIAS eawr_script_numeric)
target_include_directories(
    eawr_script_numeric
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_script_numeric PUBLIC eawr::core eawr::sim)
target_compile_features(eawr_script_numeric PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_script_numeric)

# C library replacements for the authoritative Lua build.
add_library(eawr_lua_502_sf_hooks STATIC sflua/sflua_hooks.cpp)
target_include_directories(eawr_lua_502_sf_hooks PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/sflua>")
target_link_libraries(eawr_lua_502_sf_hooks PUBLIC eawr::script_numeric)
target_compile_features(eawr_lua_502_sf_hooks PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_lua_502_sf_hooks)

# The unmodified upstream Lua 5.0.2 sources, compiled as C++ inside their own
# namespace through the wrappers in sflua/upstream. The P0 C runtime
# eawr::lua_502 is not affected.
function(eawr_add_sflua_library target_name)
    set(
        _eawr_units
        lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
        lstring ltable ltm lundump lvm lzio lauxlib lbaselib lstrlib ltablib
    )
    set(
        _eawr_sources
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sflua/sflua_state.cpp"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sflua/sflua_sandbox.cpp"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sflua/sflua_persist.cpp"
    )
    foreach(_eawr_unit IN LISTS _eawr_units)
        list(APPEND _eawr_sources "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sflua/upstream/${_eawr_unit}.cpp")
    endforeach()
    add_library(${target_name} STATIC ${_eawr_sources})
    target_include_directories(
        ${target_name}
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_FUNCTION_LIST_DIR}/sflua>"
    )
    target_include_directories(
        ${target_name}
        SYSTEM PUBLIC
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/third_party/lua/include>"
    )
    target_include_directories(
        ${target_name}
        SYSTEM PRIVATE
            "${PROJECT_SOURCE_DIR}/third_party/lua/src"
            "${PROJECT_SOURCE_DIR}/third_party/lua/src/lib"
    )
    target_link_libraries(${target_name} PUBLIC eawr_lua_502_sf_hooks)
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    if(NOT MSVC)
        # Upstream relies on the same aliasing latitude as the P0 C build.
        target_compile_options(${target_name} PRIVATE -fno-strict-aliasing)
    endif()
endfunction()

eawr_add_sflua_library(eawr_lua_502_sf)
add_library(eawr::lua_502_sf ALIAS eawr_lua_502_sf)

# Authoritative script scheduler (#247, docs/lua-sandbox.md): sandboxed
# instances of the soft-float profile, serviced per tick on the sim executor.
add_library(
    eawr_script_authoritative
    STATIC
        authoritative/bindings.cpp
        authoritative/clock_rng.cpp
        authoritative/persistence.cpp
        authoritative/scheduler.cpp
        authoritative/tactical_bridge.cpp
)
add_library(eawr::script_authoritative ALIAS eawr_script_authoritative)
target_include_directories(
    eawr_script_authoritative
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(
    eawr_script_authoritative
    PUBLIC eawr::core eawr::sim eawr::script_numeric
    PRIVATE eawr::lua_502_sf
)
target_compile_features(eawr_script_authoritative PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_script_authoritative)

# The FoC tactical space AI host (#79, docs/behaviour/foc-tactical-ai.md "#79 host"):
# the retail space freestore script driven as the FoC engine drives it.
add_library(eawr_script_foc STATIC foc/tactical_ai.cpp foc/ai_equations.cpp foc/ai_data.cpp foc/ai_perception.cpp
    foc/ai_engine.cpp foc/ai_plans.cpp foc/plan_bindings.cpp)
add_library(eawr::script_foc ALIAS eawr_script_foc)
target_include_directories(
    eawr_script_foc
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_script_foc PUBLIC eawr::script_authoritative eawr::sim eawr::script_numeric)
target_compile_features(eawr_script_foc PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_script_foc)
