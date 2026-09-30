add_library(eawr_skirmish STATIC ai.cpp census.cpp content.cpp inputs.cpp melee.cpp placement.cpp start.cpp)
add_library(eawr::skirmish ALIAS eawr_skirmish)

target_include_directories(
    eawr_skirmish
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_link_libraries(eawr_skirmish PUBLIC eawr::script_foc eawr::units eawr::scene eawr::assets eawr::data eawr::sim eawr::vfs eawr::core)
target_compile_features(eawr_skirmish PUBLIC cxx_std_20)
eawr_target_project_warnings(eawr_skirmish)
