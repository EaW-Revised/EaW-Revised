include("${CMAKE_CURRENT_LIST_DIR}/sources.cmake")

add_library(eawr_checkpoint STATIC ${EAWR_CHECKPOINT_SOURCES})
add_library(eawr::checkpoint ALIAS eawr_checkpoint)

target_include_directories(
    eawr_checkpoint
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_checkpoint PUBLIC eawr::core eawr::sim eawr::campaign)
target_compile_features(eawr_checkpoint PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_checkpoint)
