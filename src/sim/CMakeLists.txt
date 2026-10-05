include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_subdirectory(
    "${PROJECT_SOURCE_DIR}/third_party/entt"
    "${PROJECT_BINARY_DIR}/third_party/entt"
    EXCLUDE_FROM_ALL
)

add_library(eawr_sim STATIC ${EAWR_SIM_SOURCES} ${EAWR_SIM_ROOT_ONLY_SOURCES})
add_library(eawr::sim ALIAS eawr_sim)

target_include_directories(
    eawr_sim
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_sim PUBLIC eawr::core PRIVATE EnTT::EnTT)
target_compile_features(eawr_sim PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_sim)
