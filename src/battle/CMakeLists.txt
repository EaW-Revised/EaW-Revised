include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_battle STATIC ${EAWR_BATTLE_SOURCES})
add_library(eawr::battle ALIAS eawr_battle)

target_include_directories(eawr_battle PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>")
target_link_libraries(eawr_battle PUBLIC eawr::units eawr::assets eawr::sim eawr::core)
target_compile_features(eawr_battle PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_battle)
