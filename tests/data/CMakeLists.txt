add_executable(data_tests main.cpp tag_trace_tests.cpp xml_catalog_tests.cpp xml_inheritance_tests.cpp xml_override_tests.cpp xml_schema_tests.cpp)
find_package(Threads REQUIRED)
target_link_libraries(data_tests PRIVATE eawr::data eawr::vfs Threads::Threads)
target_compile_features(data_tests PRIVATE cxx_std_20)
eawr_target_project_warnings(data_tests)

add_test(NAME data_contracts COMMAND data_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
    NAME data_schema_contract_generator
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_schema_contract_generator.py"
        --generator "${PROJECT_SOURCE_DIR}/src/data/generate_schema_contract.py"
        --inventory "${PROJECT_SOURCE_DIR}/plan/inventories/xml-tags.json"
)

# Tier labels (cmake/EawrTestTiers.cmake).
eawr_label_tests()
