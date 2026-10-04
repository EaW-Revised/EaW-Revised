include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_units STATIC ${EAWR_UNITS_SOURCES})
add_library(eawr::units ALIAS eawr_units)

target_include_directories(
    eawr_units
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_units PUBLIC eawr::scene eawr::assets eawr::data eawr::sim eawr::vfs eawr::core)
target_compile_features(eawr_units PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_units)
