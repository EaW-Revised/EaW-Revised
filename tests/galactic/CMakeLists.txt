# M3 galactic tests (docs/galactic-architecture.md, testing).
include(EawrTests)

# Numeric prefixes preserve registration order, the tier boundary and late
# cross-target properties. New independent cases may use any unused prefix.
# include(), rather than add_subdirectory(), keeps source/binary paths unchanged.
# In fragments, use CMAKE_CURRENT_SOURCE_DIR/BINARY_DIR; LIST_DIR/FILE refer to cases/.
file(GLOB galactic_test_cases CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cases/*.cmake")
list(SORT galactic_test_cases)
foreach(galactic_test_case IN LISTS galactic_test_cases)
    include("${galactic_test_case}")
endforeach()
