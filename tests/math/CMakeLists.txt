cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(EawrMathTests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(EAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

if(NOT TARGET eawr_sim)
    add_library(
        eawr_math_isolated
        STATIC
            "${EAWR_ROOT}/src/sim/math/fixed.cpp"
            "${EAWR_ROOT}/src/sim/math/geometry.cpp"
            "${EAWR_ROOT}/src/sim/math/trig.cpp"
            "${EAWR_ROOT}/src/sim/math/wide.cpp"
    )
    target_include_directories(eawr_math_isolated PUBLIC "${EAWR_ROOT}/include")
    target_compile_features(eawr_math_isolated PUBLIC cxx_std_20)
    if(MSVC)
        target_compile_options(eawr_math_isolated PRIVATE /W4 /WX /permissive-)
    else()
        target_compile_options(eawr_math_isolated PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    set(EAWR_MATH_TEST_LIBRARY eawr_math_isolated)
else()
    set(EAWR_MATH_TEST_LIBRARY eawr_sim)
endif()

add_executable(math_contract_tests math_tests.cpp
    wide_limb_tests.cpp)
target_link_libraries(math_contract_tests PRIVATE ${EAWR_MATH_TEST_LIBRARY})
target_compile_features(math_contract_tests PRIVATE cxx_std_20)

add_executable(math_million_ops million_ops.cpp)
target_link_libraries(math_million_ops PRIVATE ${EAWR_MATH_TEST_LIBRARY})
target_compile_features(math_million_ops PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(math_contract_tests PRIVATE /W4 /WX /permissive-)
    target_compile_options(math_million_ops PRIVATE /W4 /WX /permissive-)
else()
    target_compile_options(math_contract_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
    target_compile_options(math_million_ops PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

include(CTest)
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME math_contracts
    COMMAND math_contract_tests "${CMAKE_CURRENT_LIST_DIR}/oracle-vectors.csv"
)
add_test(
    NAME math_evidence_comparator_contracts
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/test_compare_math_evidence.py"
)

set(EAWR_MATH_EVIDENCE_DIR "${CMAKE_BINARY_DIR}/math-evidence")
add_custom_target(
    math_evidence
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${EAWR_MATH_EVIDENCE_DIR}"
    COMMAND
        "$<TARGET_FILE:math_contract_tests>"
        "${CMAKE_CURRENT_LIST_DIR}/oracle-vectors.csv"
        --output "${EAWR_MATH_EVIDENCE_DIR}/vectors.csv"
    COMMAND
        "$<TARGET_FILE:math_million_ops>"
        --expected ec63b213f045325974e7bd60d0b075b76fb8c208be6b36f608cc21b1893aa458
        --output "${EAWR_MATH_EVIDENCE_DIR}/digest.txt"
    DEPENDS math_contract_tests math_million_ops
    VERBATIM
)
add_test(
    NAME math_million_operations
    COMMAND math_million_ops --expected ec63b213f045325974e7bd60d0b075b76fb8c208be6b36f608cc21b1893aa458
)

# Tier labels (cmake/EawrTestTiers.cmake).
# Each file runs its CPU contracts; graphical exercises retain their opt-in gates.
eawr_add_python_contract(python_math_range_analysis tests.math.test_range_analysis)

eawr_label_tests()
