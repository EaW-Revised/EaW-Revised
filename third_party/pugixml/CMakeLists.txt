# pugixml 1.15 sources are vendored verbatim; see LICENSE.md and
# ../../docs/reports/P0-02-dependency.md for provenance and hashes.
add_library(pugixml STATIC src/pugixml.cpp)
add_library(pugixml::pugixml ALIAS pugixml)

target_include_directories(
    pugixml
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_features(pugixml PUBLIC cxx_std_11)
set_target_properties(pugixml PROPERTIES POSITION_INDEPENDENT_CODE ON)
