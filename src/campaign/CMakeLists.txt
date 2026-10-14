include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_campaign STATIC ${EAWR_CAMPAIGN_SOURCES})
add_library(eawr::campaign ALIAS eawr_campaign)

target_include_directories(
    eawr_campaign
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_campaign PUBLIC eawr::sim_galactic eawr::sim eawr::core eawr::script_authoritative eawr::skirmish eawr::script_galactic eawr::data eawr::units)
target_compile_features(eawr_campaign PUBLIC cxx_std_20)
target_link_libraries(eawr_campaign PUBLIC eawr::script_foc)
eawr_target_project_warnings(eawr_campaign)
