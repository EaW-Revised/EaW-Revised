if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(eawr_terrain_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_executable(terrain_tests
        terrain_blend_tests.cpp
        terrain_descriptor_tests.cpp
        terrain_geometry_tests.cpp
        terrain_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/terrain/terrain.cpp"
    )
    target_include_directories(terrain_tests PRIVATE "${EAWR_ROOT}/include")
    if(MSVC)
        target_compile_options(terrain_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(terrain_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    add_executable(terrain_tests
        terrain_blend_tests.cpp
        terrain_descriptor_tests.cpp
        terrain_geometry_tests.cpp
        terrain_tests.cpp
    )
    target_link_libraries(terrain_tests PRIVATE eawr::presentation)
    target_compile_features(terrain_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(terrain_tests)
endif()

enable_testing()
add_test(NAME terrain_contracts COMMAND terrain_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
