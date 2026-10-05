include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_presentation STATIC ${EAWR_PRESENTATION_SOURCES} ${EAWR_PRESENTATION_ROOT_ONLY_SOURCES})
add_library(eawr::presentation ALIAS eawr_presentation)
target_include_directories(
    eawr_presentation
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_presentation PUBLIC eawr::core eawr::data eawr::sim PRIVATE pugixml::pugixml)
target_compile_features(eawr_presentation PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_presentation)
