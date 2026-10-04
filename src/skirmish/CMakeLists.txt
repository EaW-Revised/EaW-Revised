include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/data/skirmish/roster-gate.json"
    "${PROJECT_SOURCE_DIR}/tools/inventory/roster_gate.py")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/inventory/roster_gate.py"
    --header "${CMAKE_CURRENT_BINARY_DIR}/roster_gate_data.hpp" COMMAND_ERROR_IS_FATAL ANY)
add_library(eawr_skirmish STATIC ${EAWR_SKIRMISH_SOURCES})
target_include_directories(eawr_skirmish PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
add_library(eawr::skirmish ALIAS eawr_skirmish)

target_include_directories(
    eawr_skirmish
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_skirmish PUBLIC eawr::script_foc eawr::units eawr::scene eawr::assets eawr::data eawr::ui_data eawr::sim eawr::vfs eawr::core)
target_compile_features(eawr_skirmish PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_skirmish)
