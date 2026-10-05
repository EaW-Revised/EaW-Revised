if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(eawr_space_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
    add_executable(space_tests
        space_effect_tests.cpp
    space_meshgloss_tests.cpp
    space_meshadditive_tests.cpp
    space_environment_effect_tests.cpp
        space_geometry_tests.cpp
        space_surface_tests.cpp
        space_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/space/space.cpp"
        "${EAWR_ROOT}/src/presentation/space/space_effects.cpp"
        "${EAWR_ROOT}/src/presentation/space/space_geometry.cpp"
        "${EAWR_ROOT}/src/presentation/space/space_surfaces.cpp"
    )
    target_include_directories(space_tests PRIVATE "${EAWR_ROOT}/include")
    add_executable(sun_retail_tests
        sun_retail_geometry_tests.cpp
        sun_retail_routing_tests.cpp
        sun_retail_tests.cpp
        "${EAWR_ROOT}/src/core/diagnostic.cpp"
        "${EAWR_ROOT}/src/presentation/space/space.cpp"
        "${EAWR_ROOT}/src/presentation/space/space_effects.cpp"
        "${EAWR_ROOT}/src/presentation/space/space_geometry.cpp"
        "${EAWR_ROOT}/src/presentation/space/space_surfaces.cpp"
        "${EAWR_ROOT}/src/presentation/space/sun_retail.cpp"
    )
    target_include_directories(sun_retail_tests PRIVATE "${EAWR_ROOT}/include")
    if(MSVC)
        target_compile_options(space_tests PRIVATE /W4 /WX /permissive-)
        target_compile_options(sun_retail_tests PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(space_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
        target_compile_options(sun_retail_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
else()
    add_executable(space_tests
        space_effect_tests.cpp
    space_meshgloss_tests.cpp
    space_meshadditive_tests.cpp
    space_environment_effect_tests.cpp
        space_geometry_tests.cpp
        space_surface_tests.cpp
        space_tests.cpp
    )
    target_link_libraries(space_tests PRIVATE eawr::presentation)
    target_compile_features(space_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(space_tests)
    set(EAWR_ROOT "${PROJECT_SOURCE_DIR}")
    add_executable(sun_retail_tests
        sun_retail_geometry_tests.cpp
        sun_retail_routing_tests.cpp
        sun_retail_tests.cpp
    )
    target_link_libraries(sun_retail_tests PRIVATE eawr::presentation)
    target_compile_features(sun_retail_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(sun_retail_tests)
    add_executable(environment_scene_tests environment_scene_tests.cpp)
    target_link_libraries(environment_scene_tests PRIVATE eawr::presentation)
    target_compile_features(environment_scene_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(environment_scene_tests)
    add_test(NAME environment_scene_contracts COMMAND environment_scene_tests)
    add_executable(live_units_tests live_units_tests.cpp)
    find_package(Threads REQUIRED)
    target_link_libraries(live_units_tests PRIVATE eawr::presentation Threads::Threads)
    target_compile_features(live_units_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(live_units_tests)
    add_test(NAME live_units_contracts COMMAND live_units_tests)
    add_executable(population_index_tests population_index_tests.cpp)
    target_link_libraries(population_index_tests PRIVATE eawr::presentation Threads::Threads)
    target_compile_features(population_index_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(population_index_tests)
    add_test(NAME population_index_contracts COMMAND population_index_tests)
    add_executable(fighter_heading_tests fighter_heading_tests.cpp)
    target_link_libraries(fighter_heading_tests PRIVATE eawr::presentation eawr::sim eawr::platform)
    target_compile_features(fighter_heading_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(fighter_heading_tests)
    add_test(NAME fighter_heading_contracts COMMAND fighter_heading_tests)
    add_executable(shield_hits_tests shield_hits_tests.cpp)
    target_link_libraries(shield_hits_tests PRIVATE eawr::presentation)
    target_compile_features(shield_hits_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(shield_hits_tests)
    add_test(NAME shield_hits_contracts COMMAND shield_hits_tests)
    add_executable(debris_tests debris_tests.cpp)
    target_link_libraries(debris_tests PRIVATE eawr::presentation)
    target_compile_features(debris_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(debris_tests)
    add_test(NAME debris_contracts COMMAND debris_tests)
    add_executable(fog_field_tests fog_field_tests.cpp)
    target_link_libraries(fog_field_tests PRIVATE eawr::presentation)
    target_compile_features(fog_field_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(fog_field_tests)
    add_test(NAME fog_field_contracts COMMAND fog_field_tests)
    add_executable(projectiles_tests projectiles_tests.cpp)
    target_link_libraries(projectiles_tests PRIVATE eawr::presentation)
    target_compile_features(projectiles_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(projectiles_tests)
    add_test(NAME projectiles_contracts COMMAND projectiles_tests)
    add_executable(unit_fade_tests unit_fade_tests.cpp)
    target_link_libraries(unit_fade_tests PRIVATE eawr::presentation)
    target_compile_features(unit_fade_tests PRIVATE cxx_std_20)
    eawr_target_project_warnings(unit_fade_tests)
    add_test(NAME unit_fade_contracts COMMAND unit_fade_tests)
endif()
target_compile_definitions(sun_retail_tests PRIVATE EAWR_SUN_RETAIL_SOURCE_ROOT="${EAWR_ROOT}")

enable_testing()
add_test(NAME space_contracts COMMAND space_tests)
add_test(NAME sun_retail_contracts COMMAND sun_retail_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
eawr_add_python_contract(python_presentation_space_environment_effect_graphical tests.presentation.space.test_environment_effect_graphical)

eawr_label_tests()
