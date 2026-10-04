find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

add_executable(tests main.cpp)
target_link_libraries(tests PRIVATE eawr::core eawr::sim)
target_compile_features(tests PRIVATE cxx_std_20)
eawr_target_project_warnings(tests)

add_test(NAME core_contracts COMMAND tests)
add_test(NAME sim_headless_help COMMAND sim_headless --help)
set_tests_properties(sim_headless_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: sim_headless")

option(EAWR_BOUNDARY_RESULT_CACHE
    "Let the compiler-backed boundary checks reuse the result of a file whose every input is unchanged" ON)
set(_eawr_boundary_cache_args "")
set(_eawr_lua_boundary_cache_args "")
if(EAWR_BOUNDARY_RESULT_CACHE)
    # tools/boundary_cache.py: entries are keyed by the content of everything Clang read for a file.
    set(_eawr_boundary_cache_args --cache-dir "${CMAKE_CURRENT_BINARY_DIR}/boundary-cache/sim")
    set(_eawr_lua_boundary_cache_args --cache-dir "${CMAKE_CURRENT_BINARY_DIR}/boundary-cache/lua")
endif()

if(EAWR_ENABLE_SIM_BOUNDARY_CHECK)
    # Clang's automatic VS discovery can select a newer MSVC STL than the
    # toolset used by this CMake build. Probe and test against the same STL.
    unset(_eawr_sim_boundary_clangxx CACHE) # cached by configures before the probe existed
    set(_eawr_clang_candidates)
    if(EAWR_CLANGXX)
        list(APPEND _eawr_clang_candidates "${EAWR_CLANGXX}")
    else()
        if(WIN32)
            # Prefer a standalone LLVM install over Visual Studio's bundled
            # clang, which can lag behind a newer Microsoft STL on the same
            # machine.
            set(_eawr_llvm_dirs)
            foreach(_eawr_program_files IN ITEMS ProgramW6432 ProgramFiles)
                if(DEFINED ENV{${_eawr_program_files}})
                    file(TO_CMAKE_PATH "$ENV{${_eawr_program_files}}/LLVM/bin" _eawr_llvm_dir)
                    list(APPEND _eawr_llvm_dirs "${_eawr_llvm_dir}")
                endif()
            endforeach()
            unset(_eawr_clang_candidate)
            find_program(_eawr_clang_candidate NAMES clang++ clang-cl
                PATHS ${_eawr_llvm_dirs} NO_DEFAULT_PATH NO_CACHE)
            if(_eawr_clang_candidate)
                list(APPEND _eawr_clang_candidates "${_eawr_clang_candidate}")
            endif()
        endif()
        unset(_eawr_clang_candidate)
        find_program(_eawr_clang_candidate NAMES clang++ clang-cl NO_CACHE)
        if(_eawr_clang_candidate)
            list(APPEND _eawr_clang_candidates "${_eawr_clang_candidate}")
        endif()
        list(REMOVE_DUPLICATES _eawr_clang_candidates)
    endif()
    if(NOT _eawr_clang_candidates)
        message(FATAL_ERROR
            "The simulation-boundary checks need clang++ or clang-cl, but none was found "
            "on PATH. Install LLVM or set EAWR_CLANGXX to a clang++ or clang-cl executable."
        )
    endif()

    set(_eawr_clang_toolset_dir "")
    if(WIN32)
        file(TO_CMAKE_PATH "${CMAKE_CXX_COMPILER}" _eawr_cxx_compiler_path)
        if(_eawr_cxx_compiler_path MATCHES "^(.*/VC/Tools/MSVC/[^/]+)/bin/[^/]+/[^/]+/cl\\.exe$")
            set(_eawr_clang_toolset_dir "${CMAKE_MATCH_1}")
        elseif(CMAKE_GENERATOR_INSTANCE)
            # Visual Studio's ClangCL compiler path has no MSVC version in it.
            # Respect an explicit -T version, otherwise use this instance's default.
            if(CMAKE_VS_PLATFORM_TOOLSET_VERSION)
                set(_eawr_toolset_version "${CMAKE_VS_PLATFORM_TOOLSET_VERSION}")
            else()
                set(_eawr_toolset_version_file
                    "${CMAKE_GENERATOR_INSTANCE}/VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt")
                if(EXISTS "${_eawr_toolset_version_file}")
                    file(STRINGS "${_eawr_toolset_version_file}" _eawr_toolset_version LIMIT_COUNT 1)
                    string(STRIP "${_eawr_toolset_version}" _eawr_toolset_version)
                endif()
            endif()
            if(_eawr_toolset_version)
                set(_eawr_clang_toolset_dir
                    "${CMAKE_GENERATOR_INSTANCE}/VC/Tools/MSVC/${_eawr_toolset_version}")
            endif()
        elseif(DEFINED ENV{VCToolsInstallDir})
            # clang-cl under Ninja (windows-clang-ninja) runs in a developer environment.
            file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _eawr_clang_toolset_dir)
            string(REGEX REPLACE "/$" "" _eawr_clang_toolset_dir "${_eawr_clang_toolset_dir}")
        endif()
        if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND NOT _eawr_clang_toolset_dir)
            message(FATAL_ERROR "The simulation-boundary checks could not determine the Visual Studio MSVC toolset used by CMake.")
        endif()
        if(_eawr_clang_toolset_dir AND NOT EXISTS "${_eawr_clang_toolset_dir}/include/yvals_core.h")
            message(FATAL_ERROR "The simulation-boundary checks could not find the CMake MSVC toolset at ${_eawr_clang_toolset_dir}.")
        endif()
        if(_eawr_clang_toolset_dir)
            # Clang's MSVC driver consults VCToolsInstallDir before VS discovery.
            # Use it for both the configure probe and each CTest subprocess.
            set(_eawr_clang_toolset_env "VCToolsInstallDir=${_eawr_clang_toolset_dir}/")
            message(STATUS "Simulation-boundary checks use MSVC toolset: ${_eawr_clang_toolset_dir}")
        endif()
    endif()

    if(NOT WIN32)
        # Keep the existing smoke probe on Linux.
        set(_eawr_clang_probe "${CMAKE_CURRENT_BINARY_DIR}/sim_boundary_clang_probe.cpp")
        file(WRITE "${_eawr_clang_probe}"
            "#include <cstdint>\n#include <string>\n#include <vector>\nint main() { return 0; }\n"
        )
    endif()
    set(_eawr_sim_boundary_clangxx "")
    set(_eawr_clang_rejections "")
    foreach(_eawr_candidate IN LISTS _eawr_clang_candidates)
        if(WIN32)
            # Validate the exact compiler-backed project check, including its
            # AST options, project includes, and every simulation source.
            execute_process(
                COMMAND "${CMAKE_COMMAND}" -E env ${_eawr_clang_toolset_env}
                    "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_sim_boundary.py"
                    --root "${PROJECT_SOURCE_DIR}" --clang "${_eawr_candidate}" --jobs 4
                    ${_eawr_boundary_cache_args}
                RESULT_VARIABLE _eawr_probe_result
                OUTPUT_VARIABLE _eawr_probe_output
                ERROR_VARIABLE _eawr_probe_output
            )
        else()
            execute_process(
                COMMAND "${_eawr_candidate}" -std=c++20 -fsyntax-only -fno-color-diagnostics
                    "${_eawr_clang_probe}"
                RESULT_VARIABLE _eawr_probe_result
                OUTPUT_VARIABLE _eawr_probe_output
                ERROR_VARIABLE _eawr_probe_output
            )
        endif()
        if("${_eawr_probe_result}" STREQUAL "0")
            set(_eawr_sim_boundary_clangxx "${_eawr_candidate}")
            break()
        endif()
        string(REGEX MATCH "[^\n]*error[^\n]*" _eawr_probe_error "${_eawr_probe_output}")
        if(NOT _eawr_probe_error)
            set(_eawr_probe_error "${_eawr_probe_result} ${_eawr_probe_output}")
        endif()
        string(APPEND _eawr_clang_rejections "\n  ${_eawr_candidate}: ${_eawr_probe_error}")
    endforeach()
    if(NOT _eawr_sim_boundary_clangxx)
        if(WIN32)
            message(FATAL_ERROR
                "The simulation-boundary checks found no Clang that parses the project "
                "with CMake's C++ standard library:${_eawr_clang_rejections}\n"
                "Set EAWR_CLANGXX to a newer clang++ or clang-cl executable, such as the "
                "clang++.exe of a current LLVM release."
            )
        else()
            message(FATAL_ERROR
                "The simulation-boundary checks found no Clang that compiles against this "
                "machine's C++ standard library:${_eawr_clang_rejections}\n"
                "Set EAWR_CLANGXX to a newer clang++ or clang-cl executable, such as the "
                "clang++.exe of a current LLVM release."
            )
        endif()
    endif()

    execute_process(
        COMMAND "${_eawr_sim_boundary_clangxx}" --version
        OUTPUT_VARIABLE _eawr_clang_version_output
        ERROR_QUIET
    )
    string(REGEX MATCH "clang version[ \t]+[0-9][0-9.]*" _eawr_clang_version "${_eawr_clang_version_output}")
    message(STATUS "Simulation-boundary checks use ${_eawr_clang_version}: ${_eawr_sim_boundary_clangxx}")

    add_test(
        NAME sim_boundary_project
        COMMAND
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_sim_boundary.py"
            --root "${PROJECT_SOURCE_DIR}"
            --clang "${_eawr_sim_boundary_clangxx}"
            --jobs 4
            ${_eawr_boundary_cache_args}
    )
    set_tests_properties(sim_boundary_project PROPERTIES PROCESSORS 4)
    add_test(
        NAME sim_boundary_fixtures
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_sim_boundary.py"
            --root "${PROJECT_SOURCE_DIR}"
            --clang "${_eawr_sim_boundary_clangxx}"
    )
    add_test(
        NAME sim_boundary_supplemental
        COMMAND
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/acceptance/p0_01/run_boundary_supplemental.py"
            --root "${PROJECT_SOURCE_DIR}"
            --clang "${_eawr_sim_boundary_clangxx}"
    )
    if(_eawr_clang_toolset_env)
        set_tests_properties(
            sim_boundary_project sim_boundary_fixtures sim_boundary_supplemental
            PROPERTIES ENVIRONMENT "${_eawr_clang_toolset_env}")
    endif()
endif()

add_test(NAME presentation_session_boundary
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_presentation_boundary.py"
        --root "${PROJECT_SOURCE_DIR}")
add_test(NAME presentation_session_boundary_fixtures
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_presentation_boundary.py")
add_test(NAME boundary_cache_contracts
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_boundary_cache.py")

if(EAWR_ENABLE_SIM_BOUNDARY_CHECK)
    # The authoritative Lua path (#246) has its own float-free check; it
    # reuses the Clang selected for the simulation boundary above.
    add_test(
        NAME lua_numeric_boundary_project
        COMMAND
            "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_lua_numeric_boundary.py"
            --root "${PROJECT_SOURCE_DIR}"
            --clang "${_eawr_sim_boundary_clangxx}"
            --jobs 4
            ${_eawr_lua_boundary_cache_args}
    )
    add_test(
        NAME lua_numeric_boundary_fixtures
        COMMAND
            "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_lua_numeric_boundary.py"
            --root "${PROJECT_SOURCE_DIR}"
            --clang "${_eawr_sim_boundary_clangxx}"
    )
    set_tests_properties(lua_numeric_boundary_project PROPERTIES PROCESSORS 4)
    if(_eawr_clang_toolset_env)
        set_tests_properties(
            lua_numeric_boundary_project lua_numeric_boundary_fixtures
            PROPERTIES ENVIRONMENT "${_eawr_clang_toolset_env}")
    endif()
endif()

# Tier labels (cmake/EawrTestTiers.cmake).
add_test(NAME headless_missing_replay_contract
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cli.py"
            --program "$<TARGET_FILE:sim_headless>")

eawr_label_tests()
