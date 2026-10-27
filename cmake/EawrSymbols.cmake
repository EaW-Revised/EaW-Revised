# Embedded compile information keeps object caching effective. Only explicitly
# retained/shipped targets link an external PDB; keep optimization flags identical.
option(EAWR_DEBUG_SYMBOLS "Emit PDBs for shipped Windows binaries" OFF)
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
