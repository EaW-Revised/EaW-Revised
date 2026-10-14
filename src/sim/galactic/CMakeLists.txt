include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_sim_galactic STATIC ${EAWR_SIM_GALACTIC_SOURCES})
add_library(eawr::sim_galactic ALIAS eawr_sim_galactic)

target_include_directories(
    eawr_sim_galactic
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_sim_galactic PUBLIC eawr::sim eawr::core PRIVATE EnTT::EnTT)
target_compile_features(eawr_sim_galactic PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_sim_galactic)
