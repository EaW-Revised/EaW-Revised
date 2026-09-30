cmake_minimum_required(VERSION 3.25)

project(P003IndependentAcceptance LANGUAGES CXX)

set(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../..")

add_library(p003_acceptance_math STATIC
    "${EAWR_ROOT}/src/sim/math/fixed.cpp"
    "${EAWR_ROOT}/src/sim/math/geometry.cpp"
    "${EAWR_ROOT}/src/sim/math/trig.cpp"
    "${EAWR_ROOT}/src/sim/math/wide.cpp"
)
target_include_directories(p003_acceptance_math PUBLIC "${EAWR_ROOT}/include")
target_compile_features(p003_acceptance_math PUBLIC cxx_std_20)

add_executable(p003_public_api_probe api_probe.cpp)
target_link_libraries(p003_public_api_probe PRIVATE p003_acceptance_math)
target_compile_features(p003_public_api_probe PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(p003_acceptance_math PRIVATE /W4 /WX /permissive-)
    target_compile_options(p003_public_api_probe PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(p003_acceptance_math PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(p003_public_api_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()
