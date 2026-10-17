include_guard(GLOBAL)

option(EAWR_ENABLE_GNS "Build the optional GameNetworkingSockets transport" OFF)
set(EAWR_GNS_PROVIDER "FETCH" CACHE STRING "GNS dependency provider: FETCH or SYSTEM")
set_property(CACHE EAWR_GNS_PROVIDER PROPERTY STRINGS FETCH SYSTEM)

# An interface target keeps the root and independently configured viewer consistent.
add_library(eawr_gns_dependency INTERFACE)
add_library(eawr::gns_dependency ALIAS eawr_gns_dependency)
if(NOT EAWR_ENABLE_GNS)
    target_compile_definitions(eawr_gns_dependency INTERFACE EAWR_HAS_GNS=0)
    return()
endif()

if(EAWR_GNS_PROVIDER STREQUAL "SYSTEM")
    find_package(GameNetworkingSockets CONFIG REQUIRED)
    message(WARNING "EAWR_GNS_PROVIDER=SYSTEM cannot carry the TURN permission refresh patch: relayed matches "
        "lose their path about five minutes in unless that GameNetworkingSockets refreshes TURN permissions (#3912)")
    target_link_libraries(eawr_gns_dependency INTERFACE GameNetworkingSockets::static)
elseif(EAWR_GNS_PROVIDER STREQUAL "FETCH")
    # Only this explicit opt-in downloads anything. Upstream projects are built
    # separately: their warning flags, CRT settings and output paths cannot leak
    # into EAWR. Native ICE needs none of the upstream WebRTC submodules.
    enable_language(C)
    include(FetchContent)
    include(ExternalProject)
    find_package(Threads REQUIRED)
    if(NOT WIN32)
        find_package(OpenSSL 1.1.1 REQUIRED COMPONENTS Crypto)
    endif()
    set(_eawr_gns_revision d534c19aa760df3fb75fd20db13ba1932b8a5463)
    set(_eawr_protobuf_revision f502b8e9c831bda0bea57d9cbeefca3eb76e4254)
    FetchContent_Declare(eawr_gns_source
        URL "https://codeload.github.com/ValveSoftware/GameNetworkingSockets/tar.gz/${_eawr_gns_revision}"
        URL_HASH SHA256=099d4b0ff1837f0f27c83d4ce05869ec1d32136866f69a3336cc137e951189ab
    )
    FetchContent_Declare(eawr_protobuf_source
        GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
        GIT_TAG "${_eawr_protobuf_revision}"
        GIT_SUBMODULES ""
    )
    # Upstream never refreshes a TURN permission, so a relayed match dies about five minutes in (#3912).
    include("${CMAKE_CURRENT_LIST_DIR}/EawrGnsTurnPermissionRefresh.cmake")
    # A reused build tree may hold a source patched by an older revision of that patch: get a pristine one first.
    if(NOT FETCHCONTENT_SOURCE_DIR_EAWR_GNS_SOURCE)
        eawr_gns_discard_outdated_patched_source(
            "${FETCHCONTENT_BASE_DIR}/eawr_gns_source-src"
            "${FETCHCONTENT_BASE_DIR}/eawr_gns_source-subbuild/eawr_gns_source-populate-prefix/src/eawr_gns_source-populate-stamp")
    endif()
    FetchContent_GetProperties(eawr_gns_source)
    if(NOT eawr_gns_source_POPULATED)
        FetchContent_Populate(eawr_gns_source)
    endif()
    eawr_gns_patch_turn_permission_refresh("${eawr_gns_source_SOURCE_DIR}")
    eawr_gns_patch_turn_diagnostics("${eawr_gns_source_SOURCE_DIR}")
    FetchContent_GetProperties(eawr_protobuf_source)
    if(NOT eawr_protobuf_source_POPULATED)
        FetchContent_Populate(eawr_protobuf_source)
    endif()

    # Source pin changes must invalidate external-project stamps and installed
    # binaries, including reused offload trees. FetchContent replaces sources
    # independently of ExternalProject's otherwise-complete build stamps.
    string(SUBSTRING "${_eawr_gns_revision}" 0 12 _eawr_gns_build_id)
    # The carried patch changes the built library too, so it takes part in the prefix.
    string(APPEND _eawr_gns_build_id "-${EAWR_GNS_TURN_PERMISSION_PATCH_ID}")
    string(SUBSTRING "${_eawr_protobuf_revision}" 0 12 _eawr_protobuf_build_id)
    set(_eawr_gns_prefix "${CMAKE_BINARY_DIR}/gns-deps/${_eawr_gns_build_id}-${_eawr_protobuf_build_id}")
    set(_eawr_gns_config "$<IF:$<CONFIG:Debug>,Debug,Release>")
    # Each configuration has its own prefix, including a matching protoc/runtime.
    set(_eawr_gns_install "${_eawr_gns_prefix}/${_eawr_gns_config}")
    set(_eawr_gns_common_args
        "-DCMAKE_INSTALL_PREFIX:PATH=${_eawr_gns_install}"
        "-DCMAKE_INSTALL_LIBDIR:STRING=lib"
        "-DCMAKE_BUILD_TYPE:STRING=${_eawr_gns_config}"
        "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
        "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
        "-DCMAKE_POSITION_INDEPENDENT_CODE:BOOL=ON"
    )
    if(CMAKE_TOOLCHAIN_FILE)
        list(APPEND _eawr_gns_common_args "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}")
    endif()
    set(_eawr_gns_static_crt OFF)
    if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded" AND NOT CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "DLL")
        set(_eawr_gns_static_crt ON)
    endif()
    if(WIN32)
        # GNS 1.6 uses CMake's runtime selection, replacing MSVC_CRT_STATIC.
        # Pass it explicitly because the separately configured viewer uses MT.
        if(_eawr_gns_static_crt)
            set(_eawr_gns_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        else()
            set(_eawr_gns_runtime "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
        endif()
        # Protobuf's older CMake minimum otherwise leaves CMP0091 unset. Both
        # external projects must honour the caller's runtime selection.
        list(APPEND _eawr_gns_common_args
            "-DCMAKE_POLICY_DEFAULT_CMP0091:STRING=NEW"
            "-DCMAKE_MSVC_RUNTIME_LIBRARY:STRING=${_eawr_gns_runtime}")
        set(_eawr_gns_crypto BCrypt)
        set(_eawr_gns_protobuf_lib "${_eawr_gns_install}/lib/libprotobuf$<$<CONFIG:Debug>:d>.lib")
        set(_eawr_gns_library "${_eawr_gns_install}/lib/GameNetworkingSockets_s.lib")
        set(_eawr_gns_protoc "${_eawr_gns_install}/bin/protoc.exe")
    else()
        set(_eawr_gns_crypto OpenSSL)
        set(_eawr_gns_protobuf_lib "${_eawr_gns_install}/lib/libprotobuf$<$<CONFIG:Debug>:d>.a")
        set(_eawr_gns_library "${_eawr_gns_install}/lib/libGameNetworkingSockets_s.a")
        set(_eawr_gns_protoc "${_eawr_gns_install}/bin/protoc")
    endif()
    ExternalProject_Add(eawr_gns_protobuf_build
        SOURCE_DIR "${eawr_protobuf_source_SOURCE_DIR}/cmake"
        BINARY_DIR "${_eawr_gns_prefix}/protobuf-build"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_ARGS ${_eawr_gns_common_args}
            -Dprotobuf_BUILD_TESTS:BOOL=OFF
            -Dprotobuf_BUILD_SHARED_LIBS:BOOL=OFF
            -Dprotobuf_WITH_ZLIB:BOOL=OFF
            "-Dprotobuf_MSVC_STATIC_RUNTIME:BOOL=${_eawr_gns_static_crt}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config ${_eawr_gns_config} --parallel 2
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config ${_eawr_gns_config}
        BUILD_BYPRODUCTS "${_eawr_gns_protobuf_lib}" "${_eawr_gns_protoc}"
    )
    ExternalProject_Add(eawr_gns_build
        SOURCE_DIR "${eawr_gns_source_SOURCE_DIR}"
        BINARY_DIR "${_eawr_gns_prefix}/gns-build"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        DEPENDS eawr_gns_protobuf_build
        BUILD_ALWAYS TRUE
        CMAKE_ARGS ${_eawr_gns_common_args}
            "-DCMAKE_PREFIX_PATH:PATH=${_eawr_gns_install}"
            "-DProtobuf_PROTOC_EXECUTABLE:FILEPATH=${_eawr_gns_protoc}"
            "-DUSE_CRYPTO:STRING=${_eawr_gns_crypto}"
            -DBUILD_STATIC_LIB:BOOL=ON -DBUILD_SHARED_LIB:BOOL=OFF
            -DBUILD_EXAMPLES:BOOL=OFF -DBUILD_TESTS:BOOL=OFF -DBUILD_TOOLS:BOOL=OFF
            -DENABLE_ICE:BOOL=ON -DUSE_STEAMWEBRTC:BOOL=OFF -DLTO:BOOL=OFF
            -DProtobuf_USE_STATIC_LIBS:BOOL=ON
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config ${_eawr_gns_config} --parallel 2
        INSTALL_COMMAND "${CMAKE_COMMAND}" --install <BINARY_DIR> --config ${_eawr_gns_config}
        BUILD_BYPRODUCTS "${_eawr_gns_library}"
    )
    # Public headers are available during configure, before the external build.
    target_include_directories(eawr_gns_dependency SYSTEM INTERFACE "${eawr_gns_source_SOURCE_DIR}/include")
    target_compile_definitions(eawr_gns_dependency INTERFACE STEAMNETWORKINGSOCKETS_STATIC_LINK)
    target_link_libraries(eawr_gns_dependency INTERFACE
        "${_eawr_gns_library}" "${_eawr_gns_protobuf_lib}" Threads::Threads)
    if(WIN32)
        target_link_libraries(eawr_gns_dependency INTERFACE ws2_32 crypt32 winmm iphlpapi bcrypt)
    else()
        target_link_libraries(eawr_gns_dependency INTERFACE OpenSSL::Crypto)
    endif()
    add_dependencies(eawr_gns_dependency eawr_gns_build)
else()
    message(FATAL_ERROR "EAWR_GNS_PROVIDER must be FETCH or SYSTEM")
endif()
target_compile_definitions(eawr_gns_dependency INTERFACE EAWR_HAS_GNS=1)
