cmake_minimum_required(VERSION 3.25)
project(eawr_map_effect_plan_tests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
get_filename_component(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
add_executable(map_effect_plan_tests
    main.cpp
    "${EAWR_ROOT}/src/presentation/particles/map_effect_plan.cpp"
    "${EAWR_ROOT}/src/sim/math/geometry.cpp"
    "${EAWR_ROOT}/src/sim/math/fixed.cpp"
    "${EAWR_ROOT}/src/sim/math/wide.cpp")
target_include_directories(map_effect_plan_tests PRIVATE "${EAWR_ROOT}/include")
if(MSVC)
    target_compile_options(map_effect_plan_tests PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(map_effect_plan_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
enable_testing()
add_test(NAME map_effect_plan_contracts COMMAND map_effect_plan_tests)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
