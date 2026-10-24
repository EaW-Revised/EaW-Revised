add_library(eawr_project_warnings INTERFACE)
add_library(eawr::project_warnings ALIAS eawr_project_warnings)

if(MSVC)
    # Large translation units can exceed COFF's section limit, especially in Debug.
    # Directory scope also covers targets that do not link the warnings interface.
    add_compile_options(/bigobj)
    target_compile_options(eawr_project_warnings INTERFACE /W4 /permissive-)
    if(EAWR_WARNINGS_AS_ERRORS)
        target_compile_options(eawr_project_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(
        eawr_project_warnings
        INTERFACE
            -Wall
            -Wextra
            -Wpedantic
            # Cross-platform float determinism (presentation and sim): never fuse a*b+c into FMA (GCC on aarch64 does by default).
            -ffp-contract=off
    )
    if(EAWR_WARNINGS_AS_ERRORS)
        target_compile_options(eawr_project_warnings INTERFACE -Werror)
        # GCC's flow-based diagnostics misfire on inlined std::variant and std::vector moves at -O3 and
        # name only library internals; they stay visible as warnings but do not fail the build.
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(eawr_project_warnings INTERFACE
                -Wno-error=maybe-uninitialized -Wno-error=nonnull -Wno-error=stringop-overflow)
        endif()
    endif()
endif()

# Imported dependencies must be exposed as SYSTEM includes and must not link this
# target. This keeps their diagnostics from weakening warnings on project code.
function(eawr_target_project_warnings target_name)
    target_link_libraries(${target_name} PRIVATE eawr::project_warnings)
endfunction()
