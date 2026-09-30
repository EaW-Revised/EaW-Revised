add_library(
    eawr_core
    STATIC
        diagnostic.cpp
        sha256.cpp
        warning_probe.cpp
)
add_library(eawr::core ALIAS eawr_core)

target_include_directories(
    eawr_core
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
)
target_compile_features(eawr_core PUBLIC cxx_std_20)
if(EAWR_INJECT_PROJECT_WARNING)
    target_compile_definitions(eawr_core PRIVATE EAWR_INJECT_PROJECT_WARNING=1)
endif()
eawr_target_project_warnings(eawr_core)
