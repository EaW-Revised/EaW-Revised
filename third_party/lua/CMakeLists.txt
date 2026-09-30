cmake_minimum_required(VERSION 3.25)

# Lua 5.0.2, vendored from the upstream archive whose SHA-256 is
# a6c85d85f912e1c321723084389d63dee7660b81b8292452b190ea7190dd73bc.
# `src/llimits.h` has the documented LP64 instruction-width portability patch;
# all other upstream C sources are unmodified.
# Only the VM, auxiliary library, base/coroutine, string, and table libraries
# are built.  The I/O, OS, math, debug, and dynamic-loader libraries are not
# linked into the bounded script host.
add_library(
    eawr_lua_502
    STATIC
        src/lapi.c
        src/lcode.c
        src/ldebug.c
        src/ldo.c
        src/ldump.c
        src/lfunc.c
        src/lgc.c
        src/llex.c
        src/lmem.c
        src/lobject.c
        src/lopcodes.c
        src/lparser.c
        src/lstate.c
        src/lstring.c
        src/ltable.c
        src/ltm.c
        src/lundump.c
        src/lvm.c
        src/lzio.c
        src/lib/lauxlib.c
        src/lib/lbaselib.c
        src/lib/lstrlib.c
        src/lib/ltablib.c
)
add_library(eawr::lua_502 ALIAS eawr_lua_502)

target_include_directories(
    eawr_lua_502
    SYSTEM PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
set_target_properties(
    eawr_lua_502
    PROPERTIES
        C_STANDARD 90
        C_STANDARD_REQUIRED YES
        C_EXTENSIONS YES
)

# Phase-0 qualification deliberately keeps the upstream C runtime
# unoptimized, strict-binary64, and with bytecode validation enabled.
if(MSVC)
    target_compile_options(eawr_lua_502 PRIVATE /Od /fp:strict)
else()
    target_compile_options(
        eawr_lua_502
        PRIVATE
            -O0
            -fno-strict-aliasing
            -ffp-contract=off
            -fno-fast-math
            # llimits.h IntPoint() narrows a pointer to lu_hash on purpose
            # ("this is for hashing only; there is no problem if the integer
            # cannot hold the whole pointer value"). The upstream bytes are
            # kept, so the LP64 truncation diagnostic is silenced here only.
            -Wno-pointer-to-int-cast
    )
endif()
