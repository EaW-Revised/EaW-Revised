# Works around an MSVC whose optimizer drops the guard in front of a shift (legacy #3930, #1287).
#
# MSVC 19.51 (Visual Studio 18, toolset 14.51.36231) at /O2 folds `n == -1 ? 0 : (1 << n)` into an unconditional
# `1 << n`, so n == -1 yields INT_MIN instead of 0. Upstream Lua 5.0.2 ltable.c computesizes() has exactly that
# line: the first table rehash of every Lua state (luaopen_base) then reads array[INT_MIN] and the process dies
# with an access violation, in the viewer and in every Lua test. The source is well defined (the guard keeps the
# shift count in range), so this is a compiler bug, not undefined behaviour of ours. Visual Studio 2022 (MSVC
# 19.44, what every build host uses) and /O1 or /Od compile it correctly.
#
# Instead of pinning a version number, the module compiles and runs the pattern with the project's /O2. When the
# compiler gets it wrong, the translation units that contain the pattern (eawr_msvc_guard_apply, called where the
# Lua targets are defined) compile with /Od and everything else keeps /O2. A compiler that passes the probe
# changes no flag at all. A probe that cannot be built or run on MSVC fails closed (the workaround applies, with a
# warning; refuse mode stops). Cross builds are not probed.
#
#   EAWR_MSVC_GUARD_MODE=workaround   (default) /Od on the affected Lua units when the probe fails
#   EAWR_MSVC_GUARD_MODE=refuse       stop the configure when the probe fails
#   EAWR_MSVC_GUARD_MODE=off          no probe, no workaround (a miscompiling MSVC then crashes Lua tables)
#   EAWR_MSVC_GUARD_FORCE=ON          run the probe with any compiler (the guard's own test)
#   EAWR_MSVC_GUARD_PROBE_SOURCE      replacement probe source (the guard's own test)
include_guard(GLOBAL)

set(EAWR_MSVC_GUARD_MODE "workaround" CACHE STRING "What to do when the MSVC shift-guard probe fails: workaround, refuse or off")
set_property(CACHE EAWR_MSVC_GUARD_MODE PROPERTY STRINGS workaround refuse off)
option(EAWR_MSVC_GUARD_FORCE "Run the MSVC shift-guard probe with any compiler (test hook)" OFF)
set(EAWR_MSVC_GUARD_PROBE_SOURCE "${CMAKE_CURRENT_LIST_DIR}/msvc_shift_guard_probe.cpp" CACHE FILEPATH
    "Source of the MSVC shift-guard probe (test hook)")
mark_as_advanced(EAWR_MSVC_GUARD_FORCE EAWR_MSVC_GUARD_PROBE_SOURCE)

# Read by eawr_msvc_guard_apply; reset on every configure so a cached value of another compiler never lingers.
set(EAWR_MSVC_SHIFT_MISCOMPILED OFF CACHE INTERNAL "The compiler miscompiles the shift guard; the affected units get /Od" FORCE)

function(eawr_msvc_guard)
    if(NOT EAWR_MSVC_GUARD_MODE MATCHES "^(workaround|refuse|off)$")
        message(FATAL_ERROR "EAWR_MSVC_GUARD_MODE must be workaround, refuse or off, found '${EAWR_MSVC_GUARD_MODE}'")
    endif()
    if(EAWR_MSVC_GUARD_MODE STREQUAL "off")
        return()
    endif()
    if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR EAWR_MSVC_GUARD_FORCE))
        return()
    endif()
    if(CMAKE_CROSSCOMPILING)
        return()
    endif()
    # The result is cached per compiler so a reconfigure does not build the probe again.
    set(_key "${CMAKE_CXX_COMPILER}|${CMAKE_CXX_COMPILER_VERSION}|${EAWR_MSVC_GUARD_PROBE_SOURCE}")
    string(SHA1 _id "${_key}")
    if(NOT DEFINED CACHE{EAWR_MSVC_GUARD_RESULT_${_id}})
        set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            set(_flags "/O2")
        else()
            set(_flags "")
        endif()
        try_run(
            _run_result _compile_result
            "${CMAKE_BINARY_DIR}/eawr_msvc_guard"
            "${EAWR_MSVC_GUARD_PROBE_SOURCE}"
            COMPILE_DEFINITIONS ${_flags}
            COMPILE_OUTPUT_VARIABLE _compile_output
            RUN_OUTPUT_VARIABLE _run_output
        )
        if(NOT _compile_result)
            set(_state "unbuilt")
        elseif("${_run_result}" STREQUAL "0")
            set(_state "ok")
        else()
            set(_state "miscompiled")
        endif()
        string(STRIP "${_run_output}" _run_output)
        string(REPLACE "\r" "" _run_output "${_run_output}")
        string(REPLACE "\n" " " _run_output "${_run_output}")
        set(EAWR_MSVC_GUARD_RESULT_${_id} "${_state}" CACHE INTERNAL "MSVC shift-guard probe result")
        set(EAWR_MSVC_GUARD_OUTPUT_${_id} "${_run_output}" CACHE INTERNAL "MSVC shift-guard probe output")
    endif()
    set(_state "${EAWR_MSVC_GUARD_RESULT_${_id}}")
    if(_state STREQUAL "unbuilt")
        # Fails closed: a compiler that cannot be checked is treated like one that failed the check.
        if(EAWR_MSVC_GUARD_MODE STREQUAL "refuse")
            message(FATAL_ERROR
                "${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_VERSION}: the MSVC shift-guard probe did not build or "
                "run, so the compiler could not be checked for the legacy #3930 miscompile and "
                "-DEAWR_MSVC_GUARD_MODE=refuse stops here. See docs/build.md, \"MSVC 19.51 miscompile\".")
        endif()
        set(EAWR_MSVC_SHIFT_MISCOMPILED ON CACHE INTERNAL
            "The compiler miscompiles the shift guard; the affected units get /Od" FORCE)
        message(WARNING "The MSVC shift-guard probe did not build or run; the compiler could not be checked for the "
            "legacy #3930 miscompile, so the affected Lua units compile with /Od to be safe. See docs/build.md, "
            "\"MSVC 19.51 miscompile\".")
    elseif(_state STREQUAL "miscompiled")
        set(_detail "${EAWR_MSVC_GUARD_OUTPUT_${_id}}")
        if(EAWR_MSVC_GUARD_MODE STREQUAL "refuse")
            message(FATAL_ERROR
                "${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_VERSION} miscompiles `n == -1 ? 0 : (1 << n)` at /O2 "
                "(${_detail}). Upstream Lua's table rehash then reads array[INT_MIN] and the viewer and the Lua "
                "tests crash at the first Lua state (legacy #3930, #1287). Build with Visual Studio 2022 "
                "(MSVC 19.44), use the build pool (tools/offload/windows_build.py), or drop "
                "-DEAWR_MSVC_GUARD_MODE=refuse so the affected Lua units compile with /Od. "
                "See docs/build.md, \"MSVC 19.51 miscompile\".")
        endif()
        set(EAWR_MSVC_SHIFT_MISCOMPILED ON CACHE INTERNAL
            "The compiler miscompiles the shift guard; the affected units get /Od" FORCE)
        message(STATUS "${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_VERSION} miscompiles `n == -1 ? 0 : (1 << n)` at "
            "/O2 (${_detail}; legacy #3930, #1287): compiling the affected Lua units with /Od, everything else keeps "
            "/O2. See docs/build.md, \"MSVC 19.51 miscompile\".")
    endif()
endfunction()

# Compiles the given sources of <target> (the Lua units that contain the guarded shift) with /Od when the probe
# found a miscompiling compiler, or could not be checked. Call it in the directory that defines <target>, after
# add_library; a source that is not one of the target's sources is a configure error. No flag changes for a
# compiler that passed the probe.
function(eawr_msvc_guard_apply target)
    # A path that is not one of the target's sources would make the workaround a silent no-op, so every source
    # is checked against the target whether or not the compiler needs the workaround.
    get_target_property(_target_sources ${target} SOURCES)
    get_target_property(_target_dir ${target} SOURCE_DIR)
    set(_known "")
    foreach(_candidate IN LISTS _target_sources)
        cmake_path(ABSOLUTE_PATH _candidate BASE_DIRECTORY "${_target_dir}" NORMALIZE OUTPUT_VARIABLE _candidate)
        list(APPEND _known "${_candidate}")
    endforeach()
    foreach(_source IN LISTS ARGN)
        cmake_path(ABSOLUTE_PATH _source BASE_DIRECTORY "${_target_dir}" NORMALIZE OUTPUT_VARIABLE _wanted)
        if(CMAKE_HOST_WIN32)
            string(TOLOWER "${_wanted}" _wanted)
            string(TOLOWER "${_known}" _known_compare)
        else()
            set(_known_compare "${_known}")
        endif()
        if(NOT _wanted IN_LIST _known_compare)
            message(FATAL_ERROR "eawr_msvc_guard_apply: ${_source} is not one of the sources of ${target}; the "
                "/Od workaround for the MSVC shift-guard miscompile (legacy #3930) would silently do nothing.")
        endif()
    endforeach()
    if(NOT EAWR_MSVC_SHIFT_MISCOMPILED)
        return()
    endif()
    foreach(_source IN LISTS ARGN)
        set_source_files_properties("${_source}" TARGET_DIRECTORY ${target} PROPERTIES COMPILE_OPTIONS "/Od")
    endforeach()
endfunction()

eawr_msvc_guard()
