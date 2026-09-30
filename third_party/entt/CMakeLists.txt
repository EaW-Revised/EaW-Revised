# EnTT v3.15.0 single-include header vendored verbatim; see README.md and LICENSE.
add_library(entt INTERFACE)
add_library(EnTT::EnTT ALIAS entt)
target_include_directories(
    entt
    SYSTEM INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/single_include>"
)
target_compile_features(entt INTERFACE cxx_std_17)
