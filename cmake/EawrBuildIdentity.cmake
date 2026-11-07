set(EAWR_BUILD_IDENTITY "" CACHE STRING "Explicit portable build identity for package builds and tests")
get_filename_component(EAWR_BUILD_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
# The kit a release is cut as (for example 0.3.2g): replaces PROJECT_VERSION in the git-derived identity. Only the
# offload build scripts (windows_remote_build.ps1, windows_viewer_build.ps1) set it: they pass the first line of
# tools/release/kit-label.txt, a file the release cut commits. Empty (no file) = a development build.
# package_release.py builds nothing; it checks the packaged binaries against that file.
set(EAWR_KIT_LABEL "" CACHE STRING "Kit label shown instead of the project version in the build identity")
# One rule, the same text in the offload scripts and package_release.py.
string(LENGTH "${EAWR_KIT_LABEL}" EAWR_KIT_LABEL_LENGTH)
if(NOT EAWR_KIT_LABEL STREQUAL "" AND (NOT EAWR_KIT_LABEL MATCHES "^[A-Za-z0-9._-]+$" OR EAWR_KIT_LABEL_LENGTH GREATER 32))
    message(FATAL_ERROR "EAWR_KIT_LABEL is invalid: the kit label must be 1-32 characters from A-Z a-z 0-9 . _ - (no spaces, paths or quotes)")
endif()
set(EAWR_BUILD_VALUE "${EAWR_BUILD_IDENTITY}")
if(EAWR_BUILD_VALUE STREQUAL "")
    find_package(Git QUIET)
    if(NOT GIT_FOUND)
        message(FATAL_ERROR "Set EAWR_BUILD_IDENTITY when building without Git")
    endif()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${EAWR_BUILD_ROOT}"
        OUTPUT_VARIABLE EAWR_BUILD_HEAD OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE EAWR_BUILD_RESULT)
    if(NOT EAWR_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "Set EAWR_BUILD_IDENTITY when building without Git metadata")
    endif()
    # A warm tree skips configure (the pool), which would keep the previous commit's identity: make the build
    # system re-run configure when HEAD moves (a detached checkout rewrites HEAD, a commit appends to its reflog).
    # `--git-path` resolves a worktree's own HEAD; without a git directory (a source export) nothing is added.
    foreach(EAWR_GIT_NAME HEAD logs/HEAD)
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path "${EAWR_GIT_NAME}"
            WORKING_DIRECTORY "${EAWR_BUILD_ROOT}" OUTPUT_VARIABLE EAWR_GIT_FILE OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE EAWR_GIT_FILE_RESULT ERROR_QUIET)
        if(EAWR_GIT_FILE_RESULT EQUAL 0 AND NOT EAWR_GIT_FILE STREQUAL "")
            get_filename_component(EAWR_GIT_FILE "${EAWR_GIT_FILE}" ABSOLUTE BASE_DIR "${EAWR_BUILD_ROOT}")
            if(EXISTS "${EAWR_GIT_FILE}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_GIT_FILE}")
            endif()
        endif()
    endforeach()
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
        WORKING_DIRECTORY "${EAWR_BUILD_ROOT}" OUTPUT_VARIABLE EAWR_BUILD_DIRTY RESULT_VARIABLE EAWR_BUILD_RESULT)
    if(NOT EAWR_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "Could not inspect build worktree status")
    endif()
    set(EAWR_BUILD_LABEL "${PROJECT_VERSION}")
    if(NOT EAWR_KIT_LABEL STREQUAL "")
        set(EAWR_BUILD_LABEL "${EAWR_KIT_LABEL}")
    endif()
    set(EAWR_BUILD_VALUE "${EAWR_BUILD_LABEL}+${EAWR_BUILD_HEAD}")
    if(NOT EAWR_BUILD_DIRTY STREQUAL "")
        string(APPEND EAWR_BUILD_VALUE "+dirty")
    endif()
endif()
if(NOT EAWR_BUILD_VALUE MATCHES "^[a-zA-Z0-9_.+-]+$")
    message(FATAL_ERROR "EAWR_BUILD_IDENTITY must contain only portable identifier characters")
endif()
# The admission identity carries the same native sync format as tick hashing,
# including package builds that provide an explicit identity override.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EAWR_BUILD_ROOT}/include/eawr/script/foc/galactic_ai_state.hpp")
file(STRINGS "${EAWR_BUILD_ROOT}/include/eawr/script/foc/galactic_ai_state.hpp" EAWR_NATIVE_SYNC_VERSION_LINE
    REGEX "^inline constexpr std::uint32_t native_sync_digest_version = [0-9]+;")
string(REGEX MATCH "= ([0-9]+)" EAWR_NATIVE_SYNC_VERSION_MATCH "${EAWR_NATIVE_SYNC_VERSION_LINE}")
if(NOT EAWR_NATIVE_SYNC_VERSION_MATCH)
    message(FATAL_ERROR "Missing native sync digest version")
endif()
string(APPEND EAWR_BUILD_VALUE "+gcsync${CMAKE_MATCH_1}")
set(EAWR_BUILD_IDENTITY_CPP "${CMAKE_CURRENT_BINARY_DIR}/generated/build_identity.cpp")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(CONFIGURE OUTPUT "${EAWR_BUILD_IDENTITY_CPP}" CONTENT
    "#include \"eawr/core/build_identity.hpp\"\nnamespace eawr::core { std::string_view build_identity() noexcept { return \"${EAWR_BUILD_VALUE}\"; } }\n"
    @ONLY)
