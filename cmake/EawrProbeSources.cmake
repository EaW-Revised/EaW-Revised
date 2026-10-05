# Standalone probes link the viewer's engine-free libraries, which consume the
# shared src/<lib>/sources.cmake lists. Godot adapters remain on the probe target
# so fault-injection compile options cannot affect production targets.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
set(GODOTCPP_SYSTEM_HEADERS ON CACHE BOOL "Expose godot-cpp headers as SYSTEM" FORCE)
if(NOT TARGET eawr_viewer_core)
    add_subdirectory("${EAWR_ROOT}/apps/viewer" "${CMAKE_BINARY_DIR}/viewer-core" EXCLUDE_FROM_ALL)
endif()
set(GODOTCPP_SYMBOL_VISIBILITY hidden)
