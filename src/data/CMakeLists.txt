include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(EAWR_XML_SCHEMA_INC "${CMAKE_CURRENT_BINARY_DIR}/generated/xml_schema_contract.inc")
add_custom_command(
    OUTPUT "${EAWR_XML_SCHEMA_INC}"
    COMMAND
        "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_schema_contract.py"
        --input "${PROJECT_SOURCE_DIR}/plan/inventories/xml-tags.json"
        --output "${EAWR_XML_SCHEMA_INC}"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/generate_schema_contract.py"
        "${PROJECT_SOURCE_DIR}/plan/inventories/xml-tags.json"
    VERBATIM
)

add_library(eawr_data STATIC ${EAWR_DATA_SOURCES} "${EAWR_XML_SCHEMA_INC}")
add_library(eawr::data ALIAS eawr_data)
target_include_directories(
    eawr_data
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)
target_link_libraries(eawr_data PUBLIC eawr::core eawr::vfs eawr::sim PRIVATE pugixml::pugixml)
target_compile_features(eawr_data PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_data)
