# One switch for the MSVC runtime of every static-CRT target (viewer, godot-cpp, GNS, mods service, relay, probes,
# prototypes).
#
# EAWR_DEBUG_CRT=OFF (default): a Debug build keeps CMAKE_BUILD_TYPE=Debug (asserts on, full PDBs) but links the
#   static RELEASE runtime ("MultiThreaded") with _ITERATOR_DEBUG_LEVEL=0. No Microsoft Debug CRT library
#   (libcmtd, libvcruntimed, libcpmtd, libucrtd) is referenced, so a Debug zip needs none of them.
# EAWR_DEBUG_CRT=ON: the old "MultiThreadedDebug" runtime for Debug configurations (local debugging only).
#
# Release configurations always use "MultiThreaded".
include_guard(GLOBAL)

option(EAWR_DEBUG_CRT "Debug configurations link the Microsoft Debug CRT (off: static release CRT, iterator debug level 0)" OFF)

if(EAWR_DEBUG_CRT)
    set(EAWR_MSVC_RUNTIME "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(EAWR_MSVC_RUNTIME "MultiThreaded")
    if(MSVC)
        # Also for dependencies added after this include: the release STL layout in every object of the link.
        add_compile_definitions($<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
    endif()
endif()
