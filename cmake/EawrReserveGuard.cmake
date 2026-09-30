# The owner's reserve switch (tools/common/eawr_reserve.py, docs/worker-offload.md).
#
# While the workstation is reserved, nothing configures, builds or tests on it. This
# file is used three ways, and each reads the switch anew:
#   include()d by a CMakeLists.txt   configure fails, and every target of the project
#                                    (third-party ones too) checks it at build time
#                                    before anything of it compiles
#   cmake -P EawrReserveGuard.cmake  the build-time check
#   a TEST_INCLUDE_FILES entry       ctest reads it before it runs any test
# The switch: EAWR_OFFLOAD_ONLY=1 or 0 forces it; otherwise it is on while the reserve
# file exists (EAWR_RESERVE_FILE, else ~/.eawr/reserve.json). The build hosts have no
# such file, so this never stops a remote build.

function(eawr_reserve_state out_reserved out_why)
    set(reserved OFF)
    if(NOT "$ENV{EAWR_OFFLOAD_ONLY}" STREQUAL "")
        if("$ENV{EAWR_OFFLOAD_ONLY}" STREQUAL "1")
            set(reserved ON)
        endif()
        set(why "EAWR_OFFLOAD_ONLY=$ENV{EAWR_OFFLOAD_ONLY}")
    else()
        if(NOT "$ENV{EAWR_RESERVE_FILE}" STREQUAL "")
            set(file "$ENV{EAWR_RESERVE_FILE}")
        elseif(NOT "$ENV{USERPROFILE}" STREQUAL "")
            set(file "$ENV{USERPROFILE}/.eawr/reserve.json")
        else()
            set(file "$ENV{HOME}/.eawr/reserve.json")
        endif()
        if(EXISTS "${file}")
            set(reserved ON)
        endif()
        set(why "reserve file ${file}")
    endif()
    set(${out_reserved} ${reserved} PARENT_SCOPE)
    set(${out_why} "${why}" PARENT_SCOPE)
endfunction()

function(eawr_reserve_check what)
    eawr_reserve_state(reserved why)
    if(reserved)
        message(FATAL_ERROR
            "This machine is reserved by the owner (${why}): no local ${what}. Build and test on a build "
            "host instead: python tools/offload/windows_build.py (add --dirty for uncommitted edits, --viewer "
            "for the viewer extension), python tools/offload/linux_build.py, and pwsh tools/rig/Invoke-RigSuite.ps1 "
            "(it builds the viewer remotely while reserved). See docs/worker-offload.md.")
    endif()
endfunction()

set(EAWR_RESERVE_GUARD_FILE "${CMAKE_CURRENT_LIST_FILE}")

if(CMAKE_SCRIPT_MODE_FILE AND CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    eawr_reserve_check("build")
elseif(NOT CMAKE_PROJECT_NAME AND NOT CMAKE_SCRIPT_MODE_FILE)
    # Read by ctest through TEST_INCLUDE_FILES.
    eawr_reserve_check("CTest run")
else()
    eawr_reserve_check("configure")

    # The build-time check: every build (an up-to-date one as well) runs it first, since every
    # target of the build tree depends on it, so a parallel build starts no compiler before it
    # passes and no single target (cmake --build --target <any>) skips it.
    if(NOT TARGET eawr_reserve_guard)
        add_custom_target(eawr_reserve_guard
            COMMAND "${CMAKE_COMMAND}" -P "${EAWR_RESERVE_GUARD_FILE}"
            COMMENT "Checking the owner's reserve switch"
            VERBATIM)
    endif()

    function(_eawr_reserve_collect_targets dir out)
        get_property(targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
        get_property(subdirs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
        foreach(sub IN LISTS subdirs)
            _eawr_reserve_collect_targets("${sub}" more)
            list(APPEND targets ${more})
        endforeach()
        set(${out} ${targets} PARENT_SCOPE)
    endfunction()

    # Runs once the top-level directory is done, when every target of the tree exists.
    function(_eawr_reserve_guard_all_targets)
        _eawr_reserve_collect_targets("${CMAKE_SOURCE_DIR}" targets)
        list(REMOVE_ITEM targets eawr_reserve_guard)
        foreach(target IN LISTS targets)
            add_dependencies(${target} eawr_reserve_guard)
        endforeach()
        set_property(GLOBAL PROPERTY EAWR_RESERVE_GUARDED_TARGETS "${targets}")
    endfunction()

    get_property(deferred GLOBAL PROPERTY EAWR_RESERVE_GUARD_DEFERRED)
    if(NOT deferred)
        set_property(GLOBAL PROPERTY EAWR_RESERVE_GUARD_DEFERRED TRUE)
        cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _eawr_reserve_guard_all_targets)
    endif()

    # CTest reads this file before it runs any test of this directory tree.
    set_property(DIRECTORY APPEND PROPERTY TEST_INCLUDE_FILES "${EAWR_RESERVE_GUARD_FILE}")
endif()
