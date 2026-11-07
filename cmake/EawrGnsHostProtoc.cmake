# Override the imported target from the Windows protobuf package with the native generator.
function(eawr_gns_patch_host_protoc source_dir)
    set(path "${source_dir}/src/CMakeLists.txt")
    file(READ "${path}" contents)
    set(marker "# EAWR cross host protoc")
    string(FIND "${contents}" "${marker}" found)
    if(NOT found EQUAL -1)
        return()
    endif()
    # Escape the upstream variable so replacement targets its literal source text.
    set(anchor "\tmessage(STATUS \"Using host protoc: \${GNS_HOST_PROTOC}\")\nendif()")
    string(FIND "${contents}" "${anchor}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "GNS host protoc patch anchor missing; check the dependency pin")
    endif()
    set(patch [=[
# EAWR cross host protoc
if(CMAKE_CROSSCOMPILING AND GNS_HOST_PROTOC)
    if(NOT EXISTS "${GNS_HOST_PROTOC}")
        message(FATAL_ERROR "Missing host protoc: ${GNS_HOST_PROTOC}")
    endif()
    set_target_properties(protobuf::protoc PROPERTIES
        IMPORTED_LOCATION "${GNS_HOST_PROTOC}"
        IMPORTED_LOCATION_RELEASE "${GNS_HOST_PROTOC}"
        IMPORTED_LOCATION_DEBUG "${GNS_HOST_PROTOC}")
    set(Protobuf_PROTOC_EXECUTABLE "${GNS_HOST_PROTOC}" CACHE FILEPATH "" FORCE)
endif()
]=])
    string(REPLACE "${anchor}" "${anchor}\n${patch}" contents "${contents}")
    file(WRITE "${path}" "${contents}")
endfunction()
