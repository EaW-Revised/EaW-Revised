include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

find_package(Threads REQUIRED)

add_library(eawr_platform STATIC ${EAWR_PLATFORM_SOURCES} ${EAWR_PLATFORM_ROOT_ONLY_SOURCES})
add_library(eawr::platform ALIAS eawr_platform)
target_include_directories(
    eawr_platform
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_platform PUBLIC eawr::core eawr::sim eawr::script_authoritative PRIVATE Threads::Threads)
target_compile_features(eawr_platform PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_platform)

# The FoC tactical AI of a live M2 battle (#79): the skirmish's AI host as LiveSession scripts.
add_library(eawr_platform_live_ai STATIC ${EAWR_PLATFORM_LIVE_AI_SOURCES})
add_library(eawr::platform_live_ai ALIAS eawr_platform_live_ai)
target_include_directories(
    eawr_platform_live_ai
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_platform_live_ai PUBLIC eawr::platform eawr::skirmish)
target_compile_features(eawr_platform_live_ai PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_platform_live_ai)
