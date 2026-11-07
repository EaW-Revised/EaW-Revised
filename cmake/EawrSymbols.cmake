# Embedded compile information keeps object caching effective. Only explicitly
# retained/shipped targets link an external PDB; keep optimization flags identical.
option(EAWR_DEBUG_SYMBOLS "Emit PDBs for shipped Windows binaries" OFF)

# Normalise paths in shipped configurations, including the standalone viewer.
# Its CMake source directory is apps/viewer, so map the repository root instead.
get_filename_component(_eawr_source_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(_eawr_release_config "$<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>>")
if(MSVC)
    file(TO_NATIVE_PATH "${_eawr_source_root}" _eawr_source_native)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _eawr_binary_native)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # Dash spelling on purpose: sccache 0.18 reads a compile argument that starts with '/pathmap:' as a second
        # input file and refuses to cache the compile ("multiple input files"); '-pathmap:' is parsed and keyed.
        add_compile_options("$<${_eawr_release_config}:/experimental:deterministic>"
            "$<${_eawr_release_config}:-pathmap:${_eawr_source_native}=.>"
            "$<${_eawr_release_config}:-pathmap:${_eawr_binary_native}=.>")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        include(CheckCXXCompilerFlag)
        check_cxx_compiler_flag("/clang:-ffile-prefix-map=source=." EAWR_HAS_CLANG_CL_FILE_PREFIX_MAP)
        if(EAWR_HAS_CLANG_CL_FILE_PREFIX_MAP)
            add_compile_options("$<${_eawr_release_config}:/clang:-ffile-prefix-map=${_eawr_source_native}=.>"
                "$<${_eawr_release_config}:/clang:-ffile-prefix-map=${_eawr_binary_native}=.>")
        endif()
    endif()
    # lld-link accepts PDBALTPATH but not link.exe's pathmap options.
    get_filename_component(_eawr_linker_name "${CMAKE_LINKER}" NAME)
    string(TOLOWER "${_eawr_linker_name}" _eawr_linker_name)
    if(_eawr_linker_name STREQUAL "link.exe" OR _eawr_linker_name STREQUAL "link")
        add_link_options("$<${_eawr_release_config}:/experimental:deterministic>"
            "$<${_eawr_release_config}:/pathmap:${_eawr_source_native}=.>"
            "$<${_eawr_release_config}:/pathmap:${_eawr_binary_native}=.>")
    elseif(NOT _eawr_linker_name STREQUAL "lld-link.exe" AND NOT _eawr_linker_name STREQUAL "lld-link")
        message(WARNING "Release linker path mapping is unavailable for ${CMAKE_LINKER}")
    endif()
    add_link_options("$<${_eawr_release_config}:/PDBALTPATH:%_PDB%>")
endif()

if(MSVC)
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT Embedded)
    add_link_options(/DEBUG:NONE $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
        $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF> /INCREMENTAL:NO)
endif()

function(eawr_target_symbols target)
    if(MSVC AND EAWR_DEBUG_SYMBOLS)
        target_link_options(${target} PRIVATE /DEBUG:FULL)
    endif()
endfunction()
